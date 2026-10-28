// M8: lockstep scheduler over an in-memory loopback transport (NETCODE §2–3, §6).

#include <doctest/doctest.h>

#include <algorithm>
#include <deque>
#include <memory>

#include "core/command.hpp"
#include "core/sim.hpp"
#include "math/rng.hpp"
#include "net/lockstep.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"

using namespace corps;
using namespace corps::net;
using namespace corps::fixed_literals;

namespace {

// Host + peers + one Sim per peer, connected by in-memory queues the test can reorder or hold.
struct Loopback {
    LockstepHost host;
    std::vector<LockstepPeer> peers;
    std::vector<std::unique_ptr<Sim>> sims;
    std::vector<std::deque<TickBundle>> toPeer;
    std::deque<CmdPacket> toHost;
    std::vector<TickBundle> broadcast;  // everything the host sent (the replay stream)
    std::vector<bool> connected;
    Rng commandRng{7, 7};
    std::size_t issued = 0;
    std::uint32_t now = 0;
    bool reorderToHost = false;
    bool reverseToPeer1 = false;

    explicit Loopback(std::uint8_t n, std::uint32_t delay = kDefaultInputDelay)
        : host(static_cast<std::uint8_t>((1u << n) - 1), delay), toPeer(n), connected(n, true) {
        for (std::uint8_t p = 0; p < n; ++p) {
            peers.emplace_back(p, delay);
            SimConfig cfg;
            cfg.matchSeed = 99;
            sims.push_back(std::make_unique<Sim>(cfg));
        }
    }

    void issueRandom(std::uint8_t p) {
        const std::uint32_t r = commandRng.nextU32() % 3;
        if (r == 0) {
            const FVec2 pos{Fixed::fromInt(static_cast<std::int32_t>(commandRng.nextU32() % 100)), 5_fx};
            peers[p].issue(makeDebugSpawn(0, p, 0, pos, Angle{}));
            ++issued;
        } else if (r == 1) {
            peers[p].issue(makeDebugRngDraw(0, p, 0, static_cast<std::uint16_t>(1 + commandRng.nextU32() % 4)));
            ++issued;
        }
    }

    // One 100 ms frame: send, relay, deliver, step at most one tick per peer (more when behind).
    void frame(bool issue = true) {
        now += kTickMs;
        for (std::uint8_t p = 0; p < peers.size(); ++p) {
            if (!connected[p]) continue;
            if (issue) issueRandom(p);
            for (CmdPacket& pk : peers[p].takeOutgoing()) toHost.push_back(std::move(pk));
        }
        if (reorderToHost) std::reverse(toHost.begin(), toHost.end());
        while (!toHost.empty()) {
            host.onPacket(std::move(toHost.front()), now);
            toHost.pop_front();
        }
        for (TickBundle& b : host.poll(now)) {
            broadcast.push_back(b);
            for (std::uint8_t p = 0; p < peers.size(); ++p) toPeer[p].push_back(b);
        }
        for (std::uint8_t p = 0; p < peers.size(); ++p) {
            if (p == 1 && reverseToPeer1) std::reverse(toPeer[p].begin(), toPeer[p].end());
            // Peer 1 with reversed delivery gets its bundles only every 4th frame (burst, out of order).
            if (p == 1 && reverseToPeer1 && (now / kTickMs) % 4 != 0) continue;
            while (!toPeer[p].empty()) {
                peers[p].onBundle(std::move(toPeer[p].front()));
                toPeer[p].pop_front();
            }
        }
        for (std::uint8_t p = 0; p < peers.size(); ++p) {
            if (!connected[p]) continue;
            const int budget = peers[p].queuedBundles() > 2 ? 3 : 1;  // catch-up ticks
            for (int i = 0; i < budget && peers[p].canStep(); ++i) sims[p]->step(peers[p].takeTick().commands);
        }
    }

    std::uint64_t referenceHash(std::uint32_t ticks) const {
        SimConfig cfg;
        cfg.matchSeed = 99;
        Sim ref(cfg);
        for (std::uint32_t t = 0; t < ticks; ++t) ref.step(broadcast.at(t).commands);
        return stateHash(ref);
    }
};

std::size_t commandCount(const std::vector<TickBundle>& bundles) {
    std::size_t n = 0;
    for (const TickBundle& b : bundles) n += b.commands.size();
    return n;
}

}  // namespace

TEST_CASE("Lockstep: peers step identical bundles and agree with a direct run") {
    Loopback net(3);
    for (int i = 0; i < 120; ++i) net.frame();
    const std::uint32_t t = net.peers[0].nextTick();
    CHECK(t >= 115);
    for (const auto& peer : net.peers) CHECK(peer.nextTick() == t);
    CHECK(commandCount(net.broadcast) > 100);
    const std::uint64_t h = stateHash(*net.sims[0]);
    CHECK(stateHash(*net.sims[1]) == h);
    CHECK(stateHash(*net.sims[2]) == h);
    CHECK(net.referenceHash(t) == h);
    // Every bundle's commands carry its tick and come sorted by (player, seq).
    for (const TickBundle& b : net.broadcast) {
        for (std::size_t i = 0; i < b.commands.size(); ++i) {
            CHECK(b.commands[i].tick == b.tick);
            if (i > 0) {
                const Command& a = b.commands[i - 1];
                const Command& c = b.commands[i];
                CHECK((a.player < c.player || (a.player == c.player && a.seq < c.seq)));
            }
        }
    }
}

TEST_CASE("Lockstep: out-of-order packets and bundles change nothing") {
    Loopback net(2);
    net.reorderToHost = true;
    net.reverseToPeer1 = true;
    for (int i = 0; i < 160; ++i) net.frame();
    for (int i = 0; i < 8; ++i) net.frame(false);  // let the bursty peer catch up
    const std::uint32_t t = std::min(net.peers[0].nextTick(), net.peers[1].nextTick());
    REQUIRE(t > 100);
    // Peer 1 ran catch-up ticks and is level with peer 0.
    CHECK(net.peers[1].nextTick() + 1 >= net.peers[0].nextTick());
    Loopback ref(2);
    ref.broadcast = net.broadcast;
    SimConfig cfg;
    cfg.matchSeed = 99;
    Sim a(cfg);
    for (std::uint32_t i = 0; i < net.peers[1].nextTick(); ++i) a.step(net.broadcast[i].commands);
    CHECK(stateHash(*net.sims[1]) == stateHash(a));
    // A bundle that arrives twice, or for a tick already run, is ignored.
    CHECK_FALSE(net.peers[0].onBundle(net.broadcast[0]));
}

TEST_CASE("Lockstep: a stalled peer freezes everyone until it sends again") {
    Loopback net(2);
    for (int i = 0; i < 20; ++i) net.frame();
    const std::uint32_t before = net.peers[0].nextTick();
    net.connected[1] = false;  // peer 1 stops sending (and stepping)
    for (int i = 0; i < 40; ++i) net.frame();
    // Peer 0 runs out the bundles already in flight (up to D ahead), then waits.
    CHECK(net.peers[0].nextTick() <= before + kDefaultInputDelay + 1);
    CHECK_FALSE(net.peers[0].canStep());
    const auto waiting = net.host.waitingOn();
    REQUIRE(waiting.size() == 1);
    CHECK(waiting[0] == 1);
    CHECK(net.host.stalledForMs(net.now) >= 3000);  // "Waiting for player..." after 3 s (NETCODE §6)
    net.connected[1] = true;
    for (int i = 0; i < 40; ++i) net.frame();
    CHECK(net.host.waitingOn().empty() == false);  // always waiting on the newest tick...
    CHECK(net.host.stalledForMs(net.now) < 3000);  // ...but not stalled
    CHECK(net.peers[0].nextTick() > before + 20);
    const std::uint32_t t = std::min(net.peers[0].nextTick(), net.peers[1].nextTick());
    SimConfig cfg;
    cfg.matchSeed = 99;
    Sim a(cfg);
    Sim b(cfg);
    for (std::uint32_t i = 0; i < net.peers[0].nextTick(); ++i) a.step(net.broadcast[i].commands);
    for (std::uint32_t i = 0; i < net.peers[1].nextTick(); ++i) b.step(net.broadcast[i].commands);
    CHECK(stateHash(*net.sims[0]) == stateHash(a));
    CHECK(stateHash(*net.sims[1]) == stateHash(b));
    CHECK(t > 0);
}

TEST_CASE("Lockstep: input delay changes take effect on one tick without a stall or a lost command") {
    Loopback net(2);
    for (int i = 0; i < 30; ++i) net.frame();
    net.host.queueControl({ControlType::SetInputDelay, 0, 7});
    for (int i = 0; i < 30; ++i) net.frame();
    CHECK(net.peers[0].inputDelay() == 7);
    CHECK(net.peers[1].inputDelay() == 7);
    net.host.queueControl({ControlType::SetInputDelay, 0, 2});
    for (int i = 0; i < 30; ++i) net.frame();
    CHECK(net.peers[0].inputDelay() == 2);
    net.host.queueControl({ControlType::SetInputDelay, 0, 50});  // clamped
    for (int i = 0; i < 10; ++i) net.frame();
    CHECK(net.peers[1].inputDelay() == kMaxInputDelay);
    // Both peers switched on the same tick (the bundle that carried the control).
    // No stall: the peers kept stepping every frame (a few ticks behind at most while D grew).
    CHECK(net.peers[0].nextTick() >= 90);
    // Every issued command reached a bundle: count them against what the peers issued.
    for (int i = 0; i < 12; ++i) net.frame(false);
    CHECK(commandCount(net.broadcast) == net.issued);
    const std::uint32_t t = net.peers[0].nextTick();
    CHECK(net.peers[1].nextTick() == t);
    CHECK(stateHash(*net.sims[0]) == stateHash(*net.sims[1]));
    CHECK(net.referenceHash(t) == stateHash(*net.sims[0]));
}

TEST_CASE("Lockstep: a player leaving is announced in a bundle and no longer waited for") {
    Loopback net(3);
    for (int i = 0; i < 20; ++i) net.frame();
    net.connected[2] = false;
    for (int i = 0; i < 10; ++i) net.frame();
    CHECK(net.host.stalledForMs(net.now) >= 500);
    net.host.dropPlayer(2);
    for (int i = 0; i < 30; ++i) net.frame();
    CHECK(net.peers[0].leftMask() == 0b100);
    CHECK(net.peers[1].leftMask() == 0b100);
    CHECK(net.host.stalledForMs(net.now) < 200);
    std::size_t leftControls = 0;
    std::uint32_t leftTick = 0;
    for (const TickBundle& b : net.broadcast) {
        for (const Control& c : b.controls) {
            if (c.type == ControlType::PlayerLeft) {
                ++leftControls;
                leftTick = b.tick;
                CHECK(c.player == 2);
            }
        }
    }
    CHECK(leftControls == 1);
    // After the drop, player 2 has no commands in any bundle; late packets from it are refused.
    for (const TickBundle& b : net.broadcast) {
        if (b.tick < leftTick) continue;
        for (const Command& c : b.commands) CHECK(c.player != 2);
    }
    CmdPacket late;
    late.player = 2;
    late.tick = net.host.nextBundleTick() + 1;
    CHECK(net.host.onPacket(late, net.now) == LockstepHost::PacketResult::UnknownPlayer);
    CHECK(stateHash(*net.sims[0]) == stateHash(*net.sims[1]));
}

TEST_CASE("Lockstep host: rate limit, re-stamping, late and duplicate packets") {
    LockstepHost host(0b11);
    CmdPacket a;
    a.tick = 0;
    a.player = 0;
    for (int i = 0; i < 40; ++i) a.commands.push_back(makeDebugRngDraw(55, 1, 9, 1));  // claims player 1, tick 55
    CHECK(host.onPacket(a, 0) == LockstepHost::PacketResult::Trimmed);
    CHECK(host.onPacket(a, 0) == LockstepHost::PacketResult::Duplicate);
    CmdPacket b;
    b.player = 1;
    CHECK(host.onPacket(b, 0) == LockstepHost::PacketResult::Accepted);
    CmdPacket c;
    c.player = 5;
    CHECK(host.onPacket(c, 0) == LockstepHost::PacketResult::UnknownPlayer);
    const auto bundles = host.poll(0);
    REQUIRE(bundles.size() == 1);
    REQUIRE(bundles[0].commands.size() == kMaxCommandsPerPlayerTick);
    for (std::size_t i = 0; i < bundles[0].commands.size(); ++i) {
        CHECK(bundles[0].commands[i].player == 0);
        CHECK(bundles[0].commands[i].tick == 0);
        CHECK(bundles[0].commands[i].seq == i);
    }
    CHECK(host.onPacket(b, 0) == LockstepHost::PacketResult::Late);
}

TEST_CASE("Lockstep host: adaptive input delay and desync detection") {
    CHECK(LockstepHost::adaptiveDelay(0, 0) == 2);
    CHECK(LockstepHost::adaptiveDelay(300, 50) == 4);   // 150 + 100 + 10 = 260 ms -> 3 ticks + 1
    CHECK(LockstepHost::adaptiveDelay(5000, 500) == 8);
    LockstepHost host(0b11);
    host.onHashReport({10, 0, 0xAA});
    host.onHashReport({10, 1, 0xAA});
    CHECK_FALSE(host.desyncTick());
    host.onHashReport({20, 1, 0xBB});
    host.onHashReport({20, 0, 0xBC});
    REQUIRE(host.desyncTick());
    CHECK(*host.desyncTick() == 20);
    CmdPacket a;
    a.player = 0;
    CmdPacket b;
    b.player = 1;
    host.onPacket(a, 0);
    host.onPacket(b, 0);
    const auto bundles = host.poll(0);
    REQUIRE(bundles.size() == 1);
    REQUIRE(bundles[0].controls.size() == 1);
    CHECK(bundles[0].controls[0].type == ControlType::DesyncDetected);
    CHECK(bundles[0].controls[0].value == 20);
    LockstepPeer peer(0);
    peer.onBundle(bundles[0]);
    peer.takeTick();
    REQUIRE(peer.desyncTick());
    CHECK(*peer.desyncTick() == 20);
}

TEST_CASE("Lockstep: the host's stand-in takes over a dropped player's slot without a stall") {
    Loopback net(2);
    for (int i = 0; i < 20; ++i) net.frame();
    net.connected[1] = false;
    for (int i = 0; i < 5; ++i) net.frame();
    // Drop player 1 and let a host-side stand-in (a LockstepPeer the host feeds directly) play it.
    net.host.dropPlayer(1);
    net.host.addPlayer(1);
    LockstepPeer standIn(1);
    standIn.startAt(net.host.nextBundleTick());
    std::size_t standInCommands = 0;
    for (int i = 0; i < 40; ++i) {
        standIn.issue(makeDebugRngDraw(0, 1, 0, 2));
        for (CmdPacket& p : standIn.takeOutgoing()) net.host.onPacket(std::move(p), net.now);
        net.frame();
        // The stand-in follows the bundles (on the host they arrive at once).
        for (const TickBundle& b : net.broadcast) {
            if (b.tick == standIn.nextTick()) standIn.onBundle(b);
        }
        while (standIn.canStep()) standIn.takeTick();
    }
    for (const TickBundle& b : net.broadcast) {
        for (const Command& c : b.commands) standInCommands += c.player == 1 ? 1 : 0;
    }
    CHECK(net.peers[0].nextTick() > 50);
    CHECK(net.host.stalledForMs(net.now) < 200);
    CHECK(standInCommands >= 30);
    CHECK(net.peers[0].leftMask() == 0b10);
    CHECK(net.referenceHash(net.peers[0].nextTick()) == stateHash(*net.sims[0]));
}

TEST_CASE("State diff names the first differing entity and field (simcli diff)") {
    SimConfig cfg;
    cfg.matchSeed = 5;
    Sim a(cfg);
    a.step({makeDebugSpawn(0, 0, 0, {3_fx, 4_fx}, Angle{}), makeDebugSpawn(0, 0, 1, {7_fx, 8_fx}, Angle{})});
    Sim b(cfg);
    b.step({makeDebugSpawn(0, 0, 0, {3_fx, 4_fx}, Angle{}), makeDebugSpawn(0, 0, 1, {7_fx, 9_fx}, Angle{})});
    CHECK(diffStates(saveState(a), saveState(a)).equal);
    const StateDiff d = diffStates(saveState(a), saveState(b));
    REQUIRE_FALSE(d.equal);
    CHECK(d.where == "XFRM #1.1 position.y");  // index 1, generation 1
    CHECK(d.differingItems == 1);
    Sim c(cfg);
    c.step({makeDebugSpawn(0, 0, 0, {3_fx, 4_fx}, Angle{}), makeDebugSpawn(0, 0, 1, {7_fx, 8_fx}, Angle{})});
    c.rng().nextU32();  // a deliberate desync
    const StateDiff r = diffStates(saveState(a), saveState(c));
    CHECK(r.where == "header rng.state");
    Sim e(cfg);
    e.step({makeDebugSpawn(0, 0, 0, {3_fx, 4_fx}, Angle{})});
    const StateDiff m = diffStates(saveState(a), saveState(e));
    REQUIRE_FALSE(m.equal);
    CHECK(m.where.find("only in a") != std::string::npos);
    std::vector<std::uint8_t> junk = {1, 2, 3};
    CHECK(diffStates(saveState(a), junk).malformed);
}

TEST_CASE("Films: keyframes round-trip and seeking lands on the same state as playing through") {
    const Replay base = recordSynthetic(61, 62, 400, 3, nullptr);
    RunOptions opt;
    opt.keyframeEvery = 100;
    const RunResult run = runReplay(base, nullptr, opt);
    REQUIRE(run.keyframes.size() == 4);
    CHECK(run.keyframes[0].tick == 99);
    Replay film = base;
    film.keyframes = run.keyframes;
    Replay back;
    std::string err;
    REQUIRE(decodeReplay(encodeReplay(film), back, &err));
    CHECK(back.keyframes == film.keyframes);
    CHECK(encodeReplay(base) != encodeReplay(film));
    REQUIRE(decodeReplay(encodeReplay(base), back, &err));  // no keyframes: the old layout
    CHECK(back.keyframes.empty());

    RunOptions every;  // reference: the hash after every tick
    every.everyTick = true;
    const RunResult ref = runReplay(base, nullptr, every);
    ReplayPlayer player(film, nullptr, nullptr);
    CHECK(player.length() == 400);
    for (int i = 0; i < 50; ++i) player.step();
    CHECK(stateHash(player.sim()) == ref.hashes[49].hash);
    player.seek(250);  // forward past two keyframes: from the one after tick 199
    CHECK(player.tick() == 250);
    CHECK(player.lastSeekTicks() == 50);
    CHECK(stateHash(player.sim()) == ref.hashes[249].hash);
    player.seek(120);  // backwards
    CHECK(player.lastSeekTicks() == 20);
    CHECK(stateHash(player.sim()) == ref.hashes[119].hash);
    player.seek(30);  // before the first keyframe: from the start
    CHECK(stateHash(player.sim()) == ref.hashes[29].hash);
    player.seek(1000);  // clamped to the end
    CHECK(player.tick() == 400);
    CHECK(stateHash(player.sim()) == ref.finalHash);
    CHECK_FALSE(player.step());
}
