#include "net_session.hpp"

#include <cstring>
#include <type_traits>
#include <vector>

#include <godot_cpp/core/class_db.hpp>

#include "net/packets.hpp"

namespace godot {

using namespace corps::net;

namespace {

PackedByteArray toPacked(const std::vector<uint8_t>& b) {
    PackedByteArray out;
    out.resize(static_cast<int64_t>(b.size()));
    if (!b.empty()) std::memcpy(out.ptrw(), b.data(), b.size());
    return out;
}

std::vector<uint8_t> fromPacked(const PackedByteArray& p) {
    std::vector<uint8_t> out(static_cast<std::size_t>(p.size()));
    if (!out.empty()) std::memcpy(out.data(), p.ptr(), out.size());
    return out;
}

}  // namespace

void NetSession::_bind_methods() {
    ClassDB::bind_method(D_METHOD("start_host", "player_mask", "local_mask", "view_player", "input_delay"), &NetSession::start_host);
    ClassDB::bind_method(D_METHOD("start_client", "player", "input_delay"), &NetSession::start_client);
    ClassDB::bind_method(D_METHOD("is_host"), &NetSession::is_host);
    ClassDB::bind_method(D_METHOD("issue", "commands"), &NetSession::issue);
    ClassDB::bind_method(D_METHOD("outgoing"), &NetSession::outgoing);
    ClassDB::bind_method(D_METHOD("host_poll", "now_ms"), &NetSession::host_poll);
    ClassDB::bind_method(D_METHOD("receive", "message", "from", "now_ms"), &NetSession::receive);
    ClassDB::bind_method(D_METHOD("can_step"), &NetSession::can_step);
    ClassDB::bind_method(D_METHOD("take_tick"), &NetSession::take_tick);
    ClassDB::bind_method(D_METHOD("report_hash", "tick", "hash"), &NetSession::report_hash);
    ClassDB::bind_method(D_METHOD("set_input_delay", "d"), &NetSession::set_input_delay);
    ClassDB::bind_method(D_METHOD("set_speed", "percent"), &NetSession::set_speed);
    ClassDB::bind_method(D_METHOD("set_paused", "paused"), &NetSession::set_paused);
    ClassDB::bind_method(D_METHOD("drop_player", "player", "stand_in"), &NetSession::drop_player);
    ClassDB::bind_static_method("NetSession", D_METHOD("adaptive_delay", "max_rtt_ms", "jitter_ms"), &NetSession::adaptive_delay);
    ClassDB::bind_static_method("NetSession", D_METHOD("encode_ping", "id", "now_ms"), &NetSession::encode_ping);
    ClassDB::bind_static_method("NetSession", D_METHOD("encode_chat", "player", "text"), &NetSession::encode_chat);
    ClassDB::bind_method(D_METHOD("status", "now_ms"), &NetSession::status);
}

void NetSession::start_host(int64_t playerMask, int64_t localMask, int64_t viewPlayer, int64_t inputDelay) {
    host = std::make_unique<LockstepHost>(static_cast<uint8_t>(playerMask), static_cast<uint32_t>(inputDelay));
    peers.clear();
    for (uint8_t p = 0; p < kMaxNetPlayers; ++p) {
        if ((localMask & playerMask & (1 << p)) != 0) peers.emplace(p, LockstepPeer(p, static_cast<uint32_t>(inputDelay)));
    }
    view = static_cast<uint8_t>(viewPlayer);
}

void NetSession::start_client(int64_t player, int64_t inputDelay) {
    host.reset();
    peers.clear();
    view = static_cast<uint8_t>(player);
    peers.emplace(view, LockstepPeer(view, static_cast<uint32_t>(inputDelay)));
}

LockstepPeer* NetSession::viewPeer() {
    const auto it = peers.find(view);
    return it == peers.end() ? nullptr : &it->second;
}
const LockstepPeer* NetSession::viewPeer() const {
    const auto it = peers.find(view);
    return it == peers.end() ? nullptr : &it->second;
}

void NetSession::issue(const PackedByteArray& commands) {
    std::vector<corps::Command> cmds;
    if (!decodeCommandList(fromPacked(commands), 0, cmds, 4096)) return;
    for (corps::Command& c : cmds) {
        const auto it = peers.find(c.player);
        if (it != peers.end()) it->second.issue(std::move(c));  // other players' commands: not ours to send
    }
}

Array NetSession::outgoing() {
    Array out;
    if (host) return out;
    for (auto& [slot, peer] : peers) {
        for (CmdPacket& p : peer.takeOutgoing()) out.push_back(toPacked(encodeMessage(Message{std::move(p)})));
    }
    return out;
}

Array NetSession::host_poll(int64_t nowMs) {
    Array out;
    if (!host) return out;
    const auto now = static_cast<uint32_t>(nowMs);
    for (auto& [slot, peer] : peers) {
        if ((host->activeMask() & (1u << slot)) == 0) continue;
        for (CmdPacket& p : peer.takeOutgoing()) host->onPacket(std::move(p), now);
    }
    for (TickBundle& b : host->poll(now)) {
        for (auto& [slot, peer] : peers) peer.onBundle(b);
        out.push_back(toPacked(encodeMessage(Message{std::move(b)})));
    }
    return out;
}

Dictionary NetSession::receive(const PackedByteArray& message, int64_t from, int64_t nowMs) {
    Dictionary d;
    Message m;
    if (!decodeMessage(fromPacked(message), m)) {
        d["kind"] = "bad";
        return d;
    }
    std::visit(
        [&](auto& body) {
            using T = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<T, CmdPacket>) {
                d["kind"] = "packet";
                if (host && body.player == from && (droppedRemote & (1u << body.player)) == 0) host->onPacket(std::move(body), static_cast<uint32_t>(nowMs));
            } else if constexpr (std::is_same_v<T, TickBundle>) {
                d["kind"] = "bundle";
                d["tick"] = static_cast<int64_t>(body.tick);
                if (!host) {
                    for (auto& [slot, peer] : peers) peer.onBundle(body);
                }
            } else if constexpr (std::is_same_v<T, HashReport>) {
                d["kind"] = "hash";
                if (host && body.player == from) host->onHashReport(body);
            } else if constexpr (std::is_same_v<T, Ping>) {
                d["kind"] = "ping";
                d["reply"] = toPacked(encodeMessage(Message{Pong{body.id, body.sentMs}}));
            } else if constexpr (std::is_same_v<T, Pong>) {
                d["kind"] = "pong";
                d["id"] = static_cast<int64_t>(body.id);
                d["sent_ms"] = static_cast<int64_t>(body.sentMs);
            } else {
                d["kind"] = "chat";
                d["player"] = static_cast<int64_t>(body.player);
                d["text"] = String::utf8(body.text.c_str(), static_cast<int64_t>(body.text.size()));
            }
        },
        m);
    return d;
}

bool NetSession::can_step() const {
    const LockstepPeer* p = viewPeer();
    return p != nullptr && p->canStep();
}

PackedByteArray NetSession::take_tick() {
    LockstepPeer* p = viewPeer();
    if (p == nullptr || !p->canStep()) return {};
    const TickBundle b = p->takeTick();
    // The other local slots (AI) follow the same bundles so their packets stay D ahead.
    for (auto& [slot, peer] : peers) {
        while (slot != view && peer.canStep() && peer.nextTick() <= b.tick) peer.takeTick();
    }
    return toPacked(encodeCommandList(b.commands));
}

PackedByteArray NetSession::report_hash(int64_t tick, int64_t hash) {
    if ((tick + 1) % 10 != 0) return {};  // after ticks 9, 19, 29... (10 ticks done)
    const HashReport r{static_cast<uint32_t>(tick), view, static_cast<uint64_t>(hash)};
    if (host) {
        host->onHashReport(r);
        return {};
    }
    return toPacked(encodeMessage(Message{r}));
}

void NetSession::set_input_delay(int64_t d) {
    if (host) host->queueControl({ControlType::SetInputDelay, 0, static_cast<uint32_t>(d)});
}
void NetSession::set_speed(int64_t percent) {
    if (host) host->queueControl({ControlType::SetSpeed, 0, static_cast<uint32_t>(percent)});
}
void NetSession::set_paused(bool paused) {
    if (host) host->queueControl({ControlType::Pause, 0, paused ? 1u : 0u});
}
void NetSession::drop_player(int64_t player, bool standIn) {
    if (!host || player < 0 || player >= kMaxNetPlayers || peers.count(static_cast<uint8_t>(player)) != 0) return;
    const auto p = static_cast<uint8_t>(player);
    host->dropPlayer(p);
    droppedRemote = static_cast<uint8_t>(droppedRemote | (1u << p));
    if (!standIn) return;
    host->addPlayer(p);
    LockstepPeer peer(p, host->inputDelay());
    peer.startAt(host->nextBundleTick());
    peers.emplace(p, std::move(peer));
}

int64_t NetSession::adaptive_delay(int64_t maxRttMs, int64_t jitterMs) {
    return LockstepHost::adaptiveDelay(static_cast<uint32_t>(std::max<int64_t>(0, maxRttMs)), static_cast<uint32_t>(std::max<int64_t>(0, jitterMs)));
}

PackedByteArray NetSession::encode_ping(int64_t id, int64_t nowMs) {
    return toPacked(encodeMessage(Message{Ping{static_cast<uint32_t>(id), static_cast<uint32_t>(nowMs)}}));
}

PackedByteArray NetSession::encode_chat(int64_t player, const String& text) {
    const CharString u = text.utf8();
    return toPacked(encodeMessage(Message{Chat{static_cast<uint8_t>(player), std::string(u.get_data(), static_cast<std::size_t>(u.length()))}}));
}

Dictionary NetSession::status(int64_t nowMs) const {
    Dictionary d;
    const LockstepPeer* p = viewPeer();
    if (p != nullptr) {
        d["next_tick"] = static_cast<int64_t>(p->nextTick());
        d["input_delay"] = static_cast<int64_t>(p->inputDelay());
        d["queued"] = static_cast<int64_t>(p->queuedBundles());
        d["speed"] = static_cast<int64_t>(p->speedPercent());
        d["paused"] = p->paused();
        d["left_mask"] = static_cast<int64_t>(p->leftMask());
        d["desync_tick"] = p->desyncTick() ? static_cast<int64_t>(*p->desyncTick()) : int64_t{-1};
    }
    if (host) {
        Array waiting;
        for (uint8_t w : host->waitingOn()) waiting.push_back(static_cast<int64_t>(w));
        d["waiting_on"] = waiting;
        d["stalled_ms"] = static_cast<int64_t>(host->stalledForMs(static_cast<uint32_t>(nowMs)));
        d["next_bundle"] = static_cast<int64_t>(host->nextBundleTick());
        d["host_desync_tick"] = host->desyncTick() ? static_cast<int64_t>(*host->desyncTick()) : int64_t{-1};
    }
    return d;
}

}  // namespace godot
