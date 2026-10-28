#include "net/lockstep.hpp"

#include <algorithm>

namespace corps::net {

namespace {

std::uint32_t clampDelay(std::uint32_t d) { return std::clamp(d, kMinInputDelay, kMaxInputDelay); }

}  // namespace

// --- host ---

LockstepHost::LockstepHost(std::uint8_t playerMask, std::uint32_t inputDelay)
    : active_(playerMask), delay_(clampDelay(inputDelay)) {}

LockstepHost::PacketResult LockstepHost::onPacket(CmdPacket packet, std::uint32_t nowMs) {
    if (packet.player >= kMaxNetPlayers || (active_ & (1u << packet.player)) == 0) return PacketResult::UnknownPlayer;
    if (packet.tick < next_) return PacketResult::Late;
    auto& slot = pending_[packet.tick];
    if (slot.count(packet.player) != 0) return PacketResult::Duplicate;
    PacketResult result = PacketResult::Accepted;
    if (packet.commands.size() > kMaxCommandsPerPlayerTick) {
        packet.commands.resize(kMaxCommandsPerPlayerTick);
        result = PacketResult::Trimmed;
    }
    for (std::size_t i = 0; i < packet.commands.size(); ++i) {
        Command& c = packet.commands[i];
        c.tick = packet.tick;
        c.player = packet.player;
        c.seq = static_cast<std::uint16_t>(i);
    }
    slot.emplace(packet.player, std::move(packet));
    if (!waitStartMs_) waitStartMs_ = nowMs;
    return result;
}

std::vector<TickBundle> LockstepHost::poll(std::uint32_t nowMs) {
    std::vector<TickBundle> out;
    for (;;) {
        const auto it = pending_.find(next_);
        bool complete = true;
        for (std::uint8_t p = 0; p < kMaxNetPlayers; ++p) {
            if ((active_ & (1u << p)) == 0) continue;
            if (it == pending_.end() || it->second.count(p) == 0) {
                complete = false;
                break;
            }
        }
        // A bundle with nobody left to wait for still goes out only when something asks for it.
        if (!complete || (active_ == 0 && controls_.empty())) break;
        TickBundle b;
        b.tick = next_;
        if (it != pending_.end()) {
            for (auto& [player, packet] : it->second) {  // std::map: ascending player
                for (Command& c : packet.commands) b.commands.push_back(std::move(c));
            }
            pending_.erase(it);
        }
        b.controls = std::move(controls_);
        controls_.clear();
        out.push_back(std::move(b));
        ++next_;
    }
    if (out.empty()) {
        if (!waitStartMs_) waitStartMs_ = nowMs;
    } else {
        waitStartMs_ = nowMs;  // waiting for the next tick starts now
    }
    return out;
}

void LockstepHost::dropPlayer(std::uint8_t player) {
    if (player >= kMaxNetPlayers || (active_ & (1u << player)) == 0) return;
    active_ = static_cast<std::uint8_t>(active_ & ~(1u << player));
    for (auto& [tick, slot] : pending_) slot.erase(player);  // its future packets don't count
    controls_.push_back({ControlType::PlayerLeft, player, 0});
}

void LockstepHost::addPlayer(std::uint8_t player) {
    if (player >= kMaxNetPlayers) return;
    active_ = static_cast<std::uint8_t>(active_ | (1u << player));
    for (auto& [tick, slot] : pending_) slot.erase(player);
}

void LockstepHost::queueControl(Control control) {
    if (control.type == ControlType::SetInputDelay) {
        control.value = clampDelay(control.value);
        delay_ = control.value;
    }
    controls_.push_back(control);
}

std::uint32_t LockstepHost::adaptiveDelay(std::uint32_t maxRttMs, std::uint32_t jitterMs, std::uint32_t processingMs) {
    const std::uint64_t ms = maxRttMs / 2 + 2ull * jitterMs + processingMs;
    const std::uint64_t ticks = (ms + kTickMs - 1) / kTickMs + 1;
    return clampDelay(static_cast<std::uint32_t>(std::min<std::uint64_t>(ticks, kMaxInputDelay)));
}

void LockstepHost::onHashReport(const HashReport& report) {
    if (report.player >= kMaxNetPlayers) return;
    auto& slot = hashes_[report.tick];
    slot[report.player] = report.hash;
    if (!desync_) {
        for (const auto& [player, hash] : slot) {
            if (hash != report.hash) {
                desync_ = report.tick;
                controls_.push_back({ControlType::DesyncDetected, 0, report.tick});
                break;
            }
        }
    }
    // Keep only the last few reported ticks.
    while (hashes_.size() > 64) hashes_.erase(hashes_.begin());
}

std::vector<std::uint8_t> LockstepHost::waitingOn() const {
    std::vector<std::uint8_t> out;
    const auto it = pending_.find(next_);
    for (std::uint8_t p = 0; p < kMaxNetPlayers; ++p) {
        if ((active_ & (1u << p)) == 0) continue;
        if (it == pending_.end() || it->second.count(p) == 0) out.push_back(p);
    }
    return out;
}

std::uint32_t LockstepHost::stalledForMs(std::uint32_t nowMs) const noexcept {
    if (!waitStartMs_ || waitingOn().empty()) return 0;
    return nowMs >= *waitStartMs_ ? nowMs - *waitStartMs_ : 0;
}

// --- peer ---

LockstepPeer::LockstepPeer(std::uint8_t player, std::uint32_t inputDelay) : player_(player), delay_(clampDelay(inputDelay)) {}

void LockstepPeer::startAt(std::uint32_t tick) {
    next_ = tick;
    lastSent_.reset();
    if (tick > 0) lastSent_ = tick - 1;
    bundles_.clear();
}

void LockstepPeer::issue(Command command) { outbox_.push_back(std::move(command)); }

std::vector<CmdPacket> LockstepPeer::takeOutgoing() {
    std::vector<CmdPacket> out;
    const std::uint32_t target = next_ + delay_;
    const std::uint32_t first = lastSent_ ? *lastSent_ + 1 : 0;
    for (std::uint32_t t = first; t <= target; ++t) {
        CmdPacket p;
        p.tick = t;
        p.player = player_;
        p.seq = packetSeq_++;
        out.push_back(std::move(p));
    }
    if (out.empty()) return out;  // D shrank: everything up to lastSent is out already; commands wait
    lastSent_ = target;
    CmdPacket& last = out.back();
    // At most kMaxCommandsPerPlayerTick per packet (the host's rate limit); the rest wait.
    const std::size_t n = std::min(outbox_.size(), kMaxCommandsPerPlayerTick);
    for (std::size_t i = 0; i < n; ++i) {
        Command c = std::move(outbox_[i]);
        c.tick = last.tick;
        c.player = player_;
        c.seq = static_cast<std::uint16_t>(i);
        last.commands.push_back(std::move(c));
    }
    outbox_.erase(outbox_.begin(), outbox_.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
}

bool LockstepPeer::onBundle(TickBundle bundle) {
    if (bundle.tick < next_ || bundles_.count(bundle.tick) != 0) return false;
    const std::uint32_t tick = bundle.tick;
    bundles_.emplace(tick, std::move(bundle));
    return true;
}

bool LockstepPeer::canStep() const { return bundles_.count(next_) != 0; }

TickBundle LockstepPeer::takeTick() {
    const auto it = bundles_.find(next_);
    TickBundle b = std::move(it->second);
    bundles_.erase(it);
    for (const Control& c : b.controls) {
        switch (c.type) {
        case ControlType::SetInputDelay: delay_ = clampDelay(c.value); break;
        case ControlType::PlayerLeft:
            if (c.player < kMaxNetPlayers) left_ = static_cast<std::uint8_t>(left_ | (1u << c.player));
            break;
        case ControlType::SetSpeed: speed_ = c.value; break;
        case ControlType::Pause: paused_ = c.value != 0; break;
        case ControlType::DesyncDetected:
            if (!desync_) desync_ = c.value;
            break;
        }
    }
    ++next_;
    return b;
}

}  // namespace corps::net
