#include "session/replay.hpp"

#include <algorithm>

#include "session/bytes.hpp"
#include "session/state_io.hpp"

#ifndef CORPS_SIM_VERSION
#define CORPS_SIM_VERSION "unknown"
#endif

namespace corps {

using namespace fixed_literals;

const char* simVersion() {
    return CORPS_SIM_VERSION;
}

std::vector<std::uint8_t> encodeReplay(const Replay& r) {
    ByteWriter w;
    w.tag("CRPL");
    w.u16(kReplayFormatVersion);
    w.str(r.header.simVersion);
    w.u16(r.header.protocolVersion);
    w.u64(r.header.dataHash);
    w.u64(r.header.matchSeed);
    w.u32(r.header.tickCount);
    w.u16(r.header.hashInterval);
    w.u32(static_cast<std::uint32_t>(r.header.players.size()));
    for (const auto& p : r.header.players) w.str(p);
    w.tag("CMDS");
    w.u32(static_cast<std::uint32_t>(r.commands.size()));
    for (const Command& c : r.commands) {
        w.u32(c.tick);
        w.u16(c.seq);
        std::vector<std::uint8_t> enc;
        encodeCommand(c, enc);
        w.bytes(enc);
    }
    w.tag("HASH");
    w.u32(static_cast<std::uint32_t>(r.hashes.size()));
    for (const TickHash& h : r.hashes) {
        w.u32(h.tick);
        w.u64(h.hash);
    }
    w.u64(hashBytes(w.buf));
    return std::move(w.buf);
}

bool decodeReplay(const std::vector<std::uint8_t>& bytes, Replay& out, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (bytes.size() < 12) return fail("file too short");
    {
        ByteReader tail(bytes, bytes.size() - 8);
        if (tail.u64() != hashBytes(bytes.data(), bytes.size() - 8)) return fail("integrity hash mismatch (corrupt file)");
    }
    const std::vector<std::uint8_t> body(bytes.begin(), bytes.end() - 8);
    ByteReader r(body);
    Replay rep;
    if (!r.expectTag("CRPL")) return fail("not a replay (missing CRPL)");
    if (r.u16() != kReplayFormatVersion) return fail("unsupported replay format version");
    rep.header.simVersion = r.str();
    rep.header.protocolVersion = r.u16();
    rep.header.dataHash = r.u64();
    rep.header.matchSeed = r.u64();
    rep.header.tickCount = r.u32();
    rep.header.hashInterval = r.u16();
    const std::uint32_t np = r.count(4);
    for (std::uint32_t i = 0; i < np && r.ok; ++i) rep.header.players.push_back(r.str());
    if (!r.expectTag("CMDS")) return fail("missing CMDS");
    const std::uint32_t nc = r.count(10);
    for (std::uint32_t i = 0; i < nc && r.ok; ++i) {
        Command c;
        c.tick = r.u32();
        c.seq = r.u16();
        const std::vector<std::uint8_t> enc = r.bytes();
        std::size_t pos = 0;
        if (!r.ok || !decodeCommand(enc, pos, c) || pos != enc.size()) return fail("malformed command");
        rep.commands.push_back(std::move(c));
    }
    if (!r.expectTag("HASH")) return fail("missing HASH");
    const std::uint32_t nh = r.count(12);
    for (std::uint32_t i = 0; i < nh && r.ok; ++i) {
        TickHash h;
        h.tick = r.u32();
        h.hash = r.u64();
        rep.hashes.push_back(h);
    }
    if (!r.ok || r.remaining() != 0) return fail("truncated or trailing bytes");
    out = std::move(rep);
    return true;
}

RunResult runReplay(const Replay& replay, const DefSet* defs, const RunOptions& options) {
    RunResult res;
    Sim sim({replay.header.matchSeed, defs});
    CommandQueue queue;  // session side: delivers each tick's bundle, like the lockstep scheduler
    for (const Command& c : replay.commands) {
        if (!queue.schedule(c, 0)) ++res.rejectedSchedules;
    }
    std::array<std::uint64_t, kStageCount> stages{};
    if (options.perStage || options.afterStage) {
        sim.afterStage = [&](Sim& s, Stage st) {
            if (options.afterStage) options.afterStage(s, st);
            if (options.perStage) stages[static_cast<std::size_t>(st)] = stateHash(s);
        };
    }
    const std::uint32_t interval = std::max<std::uint16_t>(replay.header.hashInterval, 1);
    for (std::uint32_t t = 0; t < replay.header.tickCount; ++t) {
        sim.step(queue.take(t));
        if (options.perStage) res.stageHashes.push_back(stages);
        const bool last = t + 1 == replay.header.tickCount;
        if (options.everyTick || t % interval == 0 || last) res.hashes.push_back({t, stateHash(sim)});
    }
    res.finalHash = stateHash(sim);
    return res;
}

Replay recordSynthetic(std::uint64_t matchSeed, std::uint64_t commandSeed, std::uint32_t ticks,
                       std::uint32_t commandsPerTick, const DefSet* defs, const std::vector<Command>& extra) {
    constexpr std::uint32_t kInputDelay = 3;  // NETCODE §2 default
    Replay rep;
    rep.header.simVersion = simVersion();
    rep.header.dataHash = defs != nullptr ? defs->dataHash : 0;
    rep.header.matchSeed = matchSeed;
    rep.header.tickCount = ticks;
    rep.header.players = {"P1", "P2"};

    Sim sim({matchSeed, defs});
    CommandQueue queue;
    Rng host(commandSeed, 99);  // host-side: never touches sim state
    std::uint16_t seq[2] = {0, 0};
    for (const Command& c : extra) {
        if (queue.schedule(c, 0)) rep.commands.push_back(c);
    }
    const auto randomPos = [&host]() {
        return FVec2{Fixed::fromInt(host.range(-200, 200)) + host.fixed01(), Fixed::fromInt(host.range(-200, 200)) + host.fixed01()};
    };
    for (std::uint32_t t = 0; t < ticks; ++t) {
        const std::uint32_t due = t + kInputDelay;
        for (std::uint32_t k = 0; k < commandsPerTick; ++k) {
            const auto player = static_cast<std::uint8_t>(host.below(2));
            const std::uint16_t s = seq[player]++;
            const auto& alive = sim.world().transforms.ids();
            const EntityId target = alive.empty() ? EntityId::make(host.below(64), 1) : alive[host.below(static_cast<std::uint32_t>(alive.size()))];
            Command c;
            switch (host.below(10)) {
            case 0: case 1: case 2: case 3:
                c = makeDebugSpawn(due, player, s, randomPos(), Angle::fromUnits(host.nextU32()));
                break;
            case 4: case 5:
                c = makeDebugDestroy(due, player, s, target);
                break;
            case 6: case 7: case 8:
                c = makeDebugSetTransform(due, player, s, target, randomPos(), Angle::fromUnits(host.nextU32()));
                break;
            default:
                c = makeDebugRngDraw(due, player, s, static_cast<std::uint16_t>(host.below(5)));
                break;
            }
            if (queue.schedule(c, t)) rep.commands.push_back(c);
        }
        sim.step(queue.take(t));
        const bool last = t + 1 == ticks;
        if (t % rep.header.hashInterval == 0 || last) rep.hashes.push_back({t, stateHash(sim)});
    }
    // Commands issued for ticks past the end are never applied; drop them from the film.
    rep.commands.erase(std::remove_if(rep.commands.begin(), rep.commands.end(), [ticks](const Command& c) { return c.tick >= ticks; }),
                       rep.commands.end());
    std::stable_sort(rep.commands.begin(), rep.commands.end(), [](const Command& a, const Command& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.player != b.player) return a.player < b.player;
        return a.seq < b.seq;
    });
    return rep;
}

Divergence firstDivergence(const RunResult& a, const RunResult& b) {
    Divergence d;
    const std::size_t n = std::min(a.hashes.size(), b.hashes.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (a.hashes[i].tick != b.hashes[i].tick || a.hashes[i].hash != b.hashes[i].hash) {
            d.found = true;
            d.tick = a.hashes[i].tick;
            d.a = a.hashes[i].hash;
            d.b = b.hashes[i].hash;
            break;
        }
    }
    if (!d.found && a.hashes.size() != b.hashes.size()) {
        d.found = true;
        d.tick = static_cast<std::uint32_t>(n);
    }
    // Refine with per-stage hashes: first (tick, stage) that differs.
    const std::size_t ns = std::min(a.stageHashes.size(), b.stageHashes.size());
    for (std::size_t t = 0; t < ns; ++t) {
        for (std::size_t s = 0; s < kStageCount; ++s) {
            if (a.stageHashes[t][s] != b.stageHashes[t][s]) {
                d.found = true;
                d.tick = static_cast<std::uint32_t>(t);
                d.stage = static_cast<int>(s);
                d.a = a.stageHashes[t][s];
                d.b = b.stageHashes[t][s];
                return d;
            }
        }
    }
    return d;
}

Divergence divergenceFromRecorded(const Replay& replay, const RunResult& run) {
    Divergence d;
    std::size_t j = 0;
    for (const TickHash& rec : replay.hashes) {
        while (j < run.hashes.size() && run.hashes[j].tick < rec.tick) ++j;
        if (j < run.hashes.size() && run.hashes[j].tick == rec.tick && run.hashes[j].hash != rec.hash) {
            d.found = true;
            d.tick = rec.tick;
            d.a = rec.hash;
            d.b = run.hashes[j].hash;
            return d;
        }
    }
    return d;
}

}  // namespace corps
