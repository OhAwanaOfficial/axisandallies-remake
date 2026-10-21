// simcli perf (M4 close-out): many regiments crossing a big map; measures the sim tick time
// (target ≤ 15 ms for 160 regiments, ARCHITECTURE §7) per tick and per stage.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>

#include "core/sim.hpp"
#include "data/def_compiler.hpp"
#include "data/tgi/pipeline.hpp"
#include "data/tgi/value_parse.hpp"
#include "nav/nav_world.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"
#include "sim_commands.hpp"
#include "vfs/path.hpp"
#include "vfs/vfs.hpp"

namespace simcli {

using namespace corps;
using namespace corps::fixed_literals;
using Clock = std::chrono::steady_clock;

namespace {

std::uint64_t u64(const SimArgs& a, const char* name, std::uint64_t fallback) {
    const std::string s = a.get(name);
    if (s.empty()) return fallback;
    const auto v = tgi::parseInt(s);
    return v && *v >= 0 ? static_cast<std::uint64_t>(*v) : fallback;
}

double ms(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// Open squares (passable for feet and wheels, `half` cells around) on a lattice, west and east
// thirds of the map, nearest to the lattice points first.
bool open(const nav::NavGrid& g, std::int32_t x, std::int32_t y, std::int32_t half) {
    if (x - half < 0 || y - half < 0 || x + half >= g.width() || y + half >= g.height()) return false;
    for (std::int32_t dy = -half; dy <= half; ++dy) {
        for (std::int32_t dx = -half; dx <= half; ++dx) {
            const std::uint32_t c = g.index(x + dx, y + dy);
            if (g.cost(nav::MoveClass::Wheeled, c) == 0 || g.cost(nav::MoveClass::Foot, c) == 0) return false;
        }
    }
    return true;
}

}  // namespace

// simcli perf [--gen SEED --size N | --map F.json] [--regiments N] [--ticks N] [--seed S] [--out F.rpl]
int cmdPerf(const SimArgs& a) {
    if (!a.gameDir) {
        std::fprintf(stderr, "no game install: pass --game DIR or create config/game_path.cfg\n");
        return 77;
    }
    vfs::Vfs vfs;
    std::vector<std::string> log;
    if (!vfs::mountGameInstall(vfs, *a.gameDir, {}, log)) return 77;
    const auto db = tgi::loadTgiDatabase(vfs);
    tgi::Diagnostics diags;
    const DefSet defs = compileDefs(*db, diags);
    std::optional<nav::MapDef> map;
    if (!mapFromArgs(a, map)) return 1;
    if (!map) map = nav::generateTestMap(7, 1024);
    const auto want = static_cast<std::uint32_t>(u64(a, "regiments", 160));
    const auto ticks = static_cast<std::uint32_t>(u64(a, "ticks", 600));
    const std::uint64_t seed = u64(a, "seed", 1);

    // Spawn spots: a lattice of open squares, half the regiments in the west, half in the east.
    const auto tBuild = Clock::now();
    nav::NavWorld probe(*map);
    const double buildMs = ms(Clock::now() - tBuild);
    const nav::NavGrid& g = probe.grid;
    const std::int32_t step = 22;
    std::vector<FVec2> west, east;
    for (std::int32_t y = step; y < g.height() - step; y += step) {
        for (std::int32_t x = step; x < g.width() - step; x += step) {
            if (!open(g, x, y, 7)) continue;
            const FVec2 p{Fixed::fromInt(x) + 0.5_fx, Fixed::fromInt(y) + 0.5_fx};
            if (x < g.width() / 3) west.push_back(p);
            if (x > g.width() * 2 / 3) east.push_back(p);
        }
    }
    const std::uint32_t perSide = want / 2;
    if (west.size() < perSide || east.size() < want - perSide) {
        std::fprintf(stderr, "map %s has room for %zu + %zu regiments, need %u\n", map->name.c_str(), west.size(), east.size(), want);
        return 1;
    }
    static const char* kTypes[] = {"regiment_ger_infantry",    "regiment_ger_tank_medium", "regiment_usa_infantry",
                                   "regiment_usa_halftrack",   "regiment_gbr_tank_heavy",  "regiment_gbr_infantry_at",
                                   "regiment_per_infantry",    "regiment_usa_tank_light"};
    std::vector<Command> cmds;
    std::uint16_t seq[2] = {0, 0};
    for (std::uint32_t i = 0; i < want; ++i) {
        const std::uint8_t player = i < perSide ? 0 : 1;
        const FVec2 at = player == 0 ? west[i * west.size() / perSide] : east[(i - perSide) * east.size() / (want - perSide)];
        const Angle facing = player == 0 ? Angle{} : Angle::fromUnits(Angle::kHalf);
        cmds.push_back(makeSpawnRegiment(0, player, seq[player]++, kTypes[i % 8], at, facing));
    }
    std::vector<EntityId> regs;
    std::vector<std::uint8_t> owner;
    {
        Sim scratch({seed, &defs, &*map});
        scratch.step(cmds);
        for (std::size_t i = 0; i < scratch.world().regiments.size(); ++i) {
            regs.push_back(scratch.world().regiments.ids()[i]);
            owner.push_back(scratch.world().regiments.data()[i].player);
        }
    }
    // Orders at tick 2: groups of six cross to the other side (mirrored x), every third group as
    // a division; head-on traffic in the middle exercises avoidance.
    std::size_t groups = 0;
    for (const std::uint8_t p : {std::uint8_t{0}, std::uint8_t{1}}) {
        std::vector<EntityId> mine;
        for (std::size_t i = 0; i < regs.size(); ++i) {
            if (owner[i] == p) mine.push_back(regs[i]);
        }
        for (std::size_t s = 0; s < mine.size(); s += 6, ++groups) {
            std::vector<EntityId> grp(mine.begin() + static_cast<std::ptrdiff_t>(s),
                                      mine.begin() + static_cast<std::ptrdiff_t>(std::min(s + 6, mine.size())));
            const FVec2 from = (p == 0 ? west : east)[std::min(s, (p == 0 ? west : east).size() - 1)];
            const FVec2 goal{Fixed::fromInt(g.width()) - from.x, from.y};
            if (groups % 3 == 0 && grp.size() > 1) cmds.push_back(makeSetGroupBehaviour(2, p, seq[p]++, grp, true));
            cmds.push_back(makeMoveGroup(2, p, seq[p]++, grp, goal));
        }
    }

    // Buildings: every 100 ticks a 6x6 blocker drops on an open lattice spot in the middle third
    // (stamps change the nav grid: dirty clusters, labels and clearance are rebuilt).
    std::vector<FVec2> middle;
    for (std::int32_t y = step; y < g.height() - step; y += step) {
        for (std::int32_t x = g.width() / 3; x <= g.width() * 2 / 3; x += step) {
            if (open(g, x, y, 4)) middle.push_back({Fixed::fromInt(x) + 0.5_fx, Fixed::fromInt(y) + 0.5_fx});
        }
    }
    for (std::uint32_t t = 100, k = 0; t < ticks && !middle.empty(); t += 100, ++k) {
        cmds.push_back(makeDebugPlaceBlocker(t, 0, seq[0]++, middle[(k * 7919) % middle.size()], {3_fx, 3_fx}));
    }

    Sim sim({seed, &defs, &*map});
    std::array<double, kStageCount> stageMs{};
    std::array<double, kStageCount> stageMax{};
    Clock::time_point stageStart;
    sim.afterStage = [&](Sim&, Stage st) {
        const auto now = Clock::now();
        const double d = ms(now - stageStart);
        stageMs[static_cast<std::size_t>(st)] += d;
        stageMax[static_cast<std::size_t>(st)] = std::max(stageMax[static_cast<std::size_t>(st)], d);
        stageStart = now;
    };
    CommandQueue q;
    for (const Command& c : cmds) q.schedule(c, 0);
    std::vector<double> tickMs;
    std::vector<double> stampTickMs;  // ticks right after a blocker drops
    std::size_t arrived = 0, blocked = 0;
    for (std::uint32_t t = 0; t < ticks; ++t) {
        const auto t0 = Clock::now();
        stageStart = t0;
        sim.step(q.take(t));
        tickMs.push_back(ms(Clock::now() - t0));
        if (t >= 100 && t % 100 == 0) stampTickMs.push_back(tickMs.back());
        for (const SimEvent& e : sim.events()) {
            if (!sim.world().regiments.has(e.entity)) continue;
            arrived += e.type == SimEvent::Type::Arrived;
            blocked += e.type == SimEvent::Type::Blocked;
            if (e.type == SimEvent::Type::Blocked && a.has("why")) {
                const Regiment& r = *sim.world().regiments.get(e.entity);
                const EntityId who = !r.group.isNull() && sim.world().groups.get(r.group) && sim.world().groups.get(r.group)->division ? r.group : e.entity;
                const Path* p = sim.world().paths.get(who);
                const Order* o = sim.world().orders.get(who);
                std::printf("  blocked t%u reg %u%s path status %d (%zu wp) repaths %d goal (%.0f,%.0f)\n", t, e.entity.index(),
                            who == e.entity ? "" : " (division)",
                            p ? int(p->status) : -1, p ? p->waypoints.size() : 0, o ? int(o->repaths) : -1,
                            o ? o->goal.x.raw / 4294967296.0 : 0.0, o ? o->goal.y.raw / 4294967296.0 : 0.0);
            }
        }
    }
    std::size_t units = 0;
    for (std::size_t i = 0; i < sim.world().movers.size(); ++i) ++units;
    std::vector<double> sorted(tickMs.begin() + 1, tickMs.end());  // tick 0 spawns everything
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (const double d : sorted) sum += d;
    const auto pct = [&](double p) { return sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(p * static_cast<double>(sorted.size())))]; };
    std::printf("map %s (%ux%u), nav build %.0f ms, %zu regiments, %zu units, %zu groups, %u ticks\n", map->name.c_str(), map->width,
                map->height, buildMs, regs.size(), units, groups, ticks);
    std::printf("tick ms: avg %.2f  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f   (spawn tick %.1f)\n", sum / static_cast<double>(sorted.size()),
                pct(0.5), pct(0.95), pct(0.99), sorted.back(), tickMs.front());
    if (!stampTickMs.empty()) {
        double worst = 0, total = 0;
        for (const double d : stampTickMs) {
            worst = std::max(worst, d);
            total += d;
        }
        std::printf("ticks with a new building: %zu, avg %.2f ms, max %.2f ms\n", stampTickMs.size(),
                    total / static_cast<double>(stampTickMs.size()), worst);
    }
    std::printf("per stage (avg / max ms):");
    for (std::size_t s = 0; s < kStageCount; ++s) {
        if (stageMs[s] / ticks < 0.005 && stageMax[s] < 0.05) continue;
        std::printf("  %s %.2f/%.1f", stageName(static_cast<Stage>(s)), stageMs[s] / ticks, stageMax[s]);
    }
    std::printf("\norders finished: %zu arrived, %zu blocked; state hash %016llx\n", arrived, blocked,
                static_cast<unsigned long long>(stateHash(sim)));
    if (a.has("out")) {
        Replay rep;
        rep.header.simVersion = simVersion();
        rep.header.dataHash = defs.dataHash;
        rep.header.mapSpec = map->name;
        rep.header.mapHash = map->mapHash;
        rep.header.matchSeed = seed;
        rep.header.tickCount = ticks;
        rep.header.players = {"P1", "P2"};
        rep.commands = cmds;
        std::stable_sort(rep.commands.begin(), rep.commands.end(), [](const Command& x, const Command& y) {
            if (x.tick != y.tick) return x.tick < y.tick;
            if (x.player != y.player) return x.player < y.player;
            return x.seq < y.seq;
        });
        RunOptions opt;
        opt.map = &*map;
        rep.hashes = runReplay(rep, &defs, opt).hashes;
        if (rep.hashes.back().hash != stateHash(sim)) {
            std::fprintf(stderr, "replay runner disagrees with the timed run\n");
            return 1;
        }
        const auto bytes = encodeReplay(rep);
        std::ofstream out(vfs::pathFromUtf8(a.get("out")), std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::printf("wrote %s\n", a.get("out").c_str());
    }
    return 0;
}

}  // namespace simcli
