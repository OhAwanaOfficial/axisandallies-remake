// simcli scenario (M4 part 2): six regiments of mixed types cross the test map, exercising
// divisions, loose groups, Run, SetFormation, Stop, a blocker dropped on the route, and arrival.
// Records the whole thing as a replay so `simcli determinism-check` can verify it.

#include <cstdio>
#include <fstream>
#include <map>
#include <memory>

#include "core/ascii.hpp"
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

namespace {

std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

std::uint64_t u64(const SimArgs& a, const char* name, std::uint64_t fallback) {
    const std::string s = a.get(name);
    if (s.empty()) return fallback;
    const auto v = tgi::parseInt(s);
    return v && *v >= 0 ? static_cast<std::uint64_t>(*v) : fallback;
}

// Centre of an open square (all cells passable for wheels and feet) near `want`, in the same
// wheeled component as `label` when given. Nearest by distance², then cell index.
bool findOpen(const nav::NavGrid& g, std::int32_t wx, std::int32_t wy, std::int32_t half, std::uint32_t label, FVec2& out,
              std::uint32_t& outLabel) {
    std::int64_t best = INT64_MAX;
    std::uint32_t bestCell = 0;
    for (std::int32_t y = half; y < g.height() - half; ++y) {
        for (std::int32_t x = half; x < g.width() - half; ++x) {
            const std::int64_t d2 = static_cast<std::int64_t>(x - wx) * (x - wx) + static_cast<std::int64_t>(y - wy) * (y - wy);
            if (d2 >= best) continue;
            const std::uint32_t c = g.index(x, y);
            if (label != 0 && g.label(nav::MoveClass::Wheeled, c) != label) continue;
            bool open = true;
            for (std::int32_t dy = -half; dy <= half && open; ++dy) {
                for (std::int32_t dx = -half; dx <= half && open; ++dx) {
                    const std::uint32_t k = g.index(x + dx, y + dy);
                    open = g.cost(nav::MoveClass::Wheeled, k) != 0 && g.cost(nav::MoveClass::Foot, k) != 0;
                }
            }
            if (!open) continue;
            best = d2;
            bestCell = c;
        }
    }
    if (best == INT64_MAX) return false;
    out = {Fixed::fromInt(g.xOf(bestCell)) + 0.5_fx, Fixed::fromInt(g.yOf(bestCell)) + 0.5_fx};
    outLabel = g.label(nav::MoveClass::Wheeled, bestCell);
    return true;
}

struct GameData {
    vfs::Vfs vfs;
    std::unique_ptr<tgi::TgiDatabase> db;
    DefSet defs;
};

}  // namespace

// simcli scenario [--gen SEED --size N | --map F.json] [--ticks N] [--seed S] [--out F.rpl] [--game DIR]
int cmdScenario(const SimArgs& a) {
    constexpr int kSkipped = 77;  // CTest SKIP_RETURN_CODE: no game data on this machine (CI)
    if (!a.gameDir) {
        std::fprintf(stderr, "no game install: pass --game DIR or create config/game_path.cfg\n");
        return kSkipped;
    }
    GameData game;
    std::vector<std::string> log;
    if (!vfs::mountGameInstall(game.vfs, *a.gameDir, {}, log)) {
        std::fprintf(stderr, "error: nothing could be mounted from %s\n", vfs::pathToUtf8(*a.gameDir).c_str());
        return kSkipped;
    }
    game.db = tgi::loadTgiDatabase(game.vfs);
    tgi::Diagnostics diags;
    game.defs = compileDefs(*game.db, diags);
    std::optional<nav::MapDef> map;
    if (!mapFromArgs(a, map)) return 1;
    if (!map) map = nav::generateTestMap(7, 256);
    const auto maxTicks = static_cast<std::uint32_t>(u64(a, "ticks", 3000));
    const std::uint64_t seed = u64(a, "seed", 1);

    // Spawn area in the west, goal in the east, both open and connected for wheels.
    nav::NavWorld probe(*map);
    const nav::NavGrid& g = probe.grid;
    FVec2 spawn, goal;
    std::uint32_t label = 0, goalLabel = 0;
    if (!findOpen(g, 28, g.height() / 2, 20, 0, spawn, label) ||
        !findOpen(g, g.width() - 28, g.height() / 2, 12, label, goal, goalLabel)) {
        std::fprintf(stderr, "map %s has no open, connected spawn and goal areas\n", map->name.c_str());
        return 1;
    }
    const Angle east = (goal - spawn).angle();

    static const char* kRegiments[6] = {"regiment_per_infantry",   "regiment_usa_infantry", "regiment_ger_tank_medium",
                                        "regiment_gbr_tank_heavy", "regiment_usa_halftrack", "regiment_gbr_infantry_at"};
    const FVec2 spots[6] = {{-8_fx, -14_fx}, {8_fx, -14_fx}, {-8_fx, 0_fx}, {8_fx, 0_fx}, {-8_fx, 14_fx}, {8_fx, 14_fx}};
    std::vector<Command> cmds;
    std::uint16_t seq = 0;
    for (int i = 0; i < 6; ++i) cmds.push_back(makeSpawnRegiment(0, 0, seq++, kRegiments[i], spawn + spots[i].rotated(east), east));

    // Learn the regiment ids (deterministic: same as in every replay of this film).
    std::vector<EntityId> regs;
    {
        Sim scratch({seed, &game.defs, &*map});
        scratch.step(cmds);
        regs = scratch.world().regiments.ids();
        if (regs.size() != 6) {
            std::fprintf(stderr, "expected 6 regiments, spawned %zu (missing data?)\n", regs.size());
            return 1;
        }
    }
    // Division: PER infantry + GER medium tank + USA halftrack. Loose group: USA infantry + GBR
    // heavy tank. GBR AT infantry runs alone. Mid-way: a formation change, a blocker on the
    // route, and a Stop / re-order that takes the heavy tanks out of their group.
    const std::vector<EntityId> division = {regs[0], regs[2], regs[4]};
    const std::vector<EntityId> loose = {regs[1], regs[3]};
    const FVec2 runGoal = goal + FVec2{0_fx, -16_fx}.rotated(east);
    cmds.push_back(makeSetGroupBehaviour(5, 0, seq++, division, true));
    cmds.push_back(makeMoveGroup(5, 0, seq++, division, goal));
    cmds.push_back(makeMoveGroup(5, 0, seq++, loose, goal + FVec2{0_fx, 18_fx}.rotated(east)));
    cmds.push_back(makeRun(5, 0, seq++, {regs[5]}, runGoal));
    cmds.push_back(makeSetFormation(40, 0, seq++, {regs[1]}, "regiment_column"));
    const FVec2 mid = (spawn + goal) / 2_fx;
    cmds.push_back(makeDebugPlaceBlocker(120, 0, seq++, mid + FVec2{0_fx, 18_fx}.rotated(east), {3_fx, 12_fx}));
    cmds.push_back(makeStop(200, 0, seq++, {regs[3]}));
    cmds.push_back(makeMoveGroupFacing(230, 0, seq++, {regs[3]}, goal + FVec2{-10_fx, 30_fx}.rotated(east), east));
    constexpr std::uint32_t kLastCommandTick = 230;

    // Run until every regiment is idle after the last command (or the tick limit).
    Sim sim({seed, &game.defs, &*map});
    CommandQueue q;
    for (const Command& c : cmds) q.schedule(c, 0);
    std::map<std::uint32_t, std::pair<std::uint32_t, bool>> done;  // regiment bits -> (tick, arrived)
    std::size_t rejected = 0;
    std::uint32_t t = 0;
    for (; t < maxTicks; ++t) {
        sim.step(q.take(t));
        for (const SimEvent& e : sim.events()) {
            if (e.type == SimEvent::Type::CommandRejected) ++rejected;
            if ((e.type == SimEvent::Type::Arrived || e.type == SimEvent::Type::Blocked) && sim.world().regiments.has(e.entity)) {
                done[e.entity.bits] = {t, e.type == SimEvent::Type::Arrived};
            }
        }
        if (t <= kLastCommandTick) continue;
        bool allIdle = true;
        for (const EntityId r : regs) allIdle = allIdle && sim.world().orders.get(r)->state == Order::State::Idle;
        if (allIdle) break;
    }
    const std::uint32_t ticks = std::min(t + 1, maxTicks);

    std::printf("map %s (%ux%u), spawn (%s, %s) -> goal (%s, %s), %u ticks, %zu rejected commands\n", map->name.c_str(), map->width,
                map->height, formatFixed(spawn.x).c_str(), formatFixed(spawn.y).c_str(), formatFixed(goal.x).c_str(),
                formatFixed(goal.y).c_str(), ticks, rejected);
    static const char* kRole[6] = {"division", "group/column", "division", "group->alone", "division", "run"};
    static const char* kClass[] = {"foot", "wheeled", "tracked", "naval", "amphibious", "air"};
    for (int i = 0; i < 6; ++i) {
        const EntityId r = regs[static_cast<std::size_t>(i)];
        const Regiment& rg = *sim.world().regiments.get(r);
        const Transform& anchor = *sim.world().transforms.get(r);
        const Order& o = *sim.world().orders.get(r);
        std::size_t units = 0;
        for (const Member& m : sim.world().members.data()) units += m.organization == r;
        const auto it = done.find(r.bits);
        const Group* grp = rg.group.isNull() ? nullptr : sim.world().groups.get(rg.group);
        const FVec2 aim = grp != nullptr && grp->division ? sim.world().orders.get(rg.group)->goal : o.goal;
        std::printf("  %-26s %-13s %-8s %zu units  %s at tick %-5s pos (%s, %s)  %s from its goal\n", kRegiments[i], kRole[i],
                    kClass[rg.moveClass], units, it == done.end() ? "still moving" : (it->second.second ? "arrived" : "BLOCKED"),
                    it == done.end() ? "-" : std::to_string(it->second.first).c_str(), formatFixed(anchor.position.x).c_str(),
                    formatFixed(anchor.position.y).c_str(), formatFixed(distance(anchor.position, aim)).c_str());
    }
    const std::uint64_t direct = stateHash(sim);

    Replay rep;
    rep.header.simVersion = simVersion();
    rep.header.dataHash = game.defs.dataHash;
    rep.header.mapSpec = map->name;
    rep.header.mapHash = map->mapHash;
    rep.header.matchSeed = seed;
    rep.header.tickCount = ticks;
    rep.header.players = {"P1"};
    rep.commands = cmds;
    RunOptions opt;
    opt.map = &*map;
    const RunResult run = runReplay(rep, &game.defs, opt);
    rep.hashes = run.hashes;
    std::printf("STATE HASH %s\n", hex(direct).c_str());
    if (run.finalHash != direct) {
        std::printf("ERROR: replay runner disagrees (%s)\n", hex(run.finalHash).c_str());
        return 1;
    }
    if (a.has("out")) {
        const auto bytes = encodeReplay(rep);
        std::ofstream out(vfs::pathFromUtf8(a.get("out")), std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", a.get("out").c_str());
            return 1;
        }
        std::printf("wrote %s (%zu commands, %zu hashes)\n", a.get("out").c_str(), rep.commands.size(), rep.hashes.size());
    }
    std::size_t arrived = 0;
    for (const auto& [bits, info] : done) arrived += info.second;
    return arrived == 6 ? 0 : 1;
}

}  // namespace simcli
