// simcli duel (M5): two regiments (or a regiment and a building) fight on a flat arena of one
// terrain until one side routs or is wiped out (--until-wiped: wiped out). Prints the time to
// rout / kill, survivors, morale, experience and army XP; the calibration harness for
// docs/CALIBRATION.md. Records the fight as a replay so `simcli determinism-check` can verify it.
//
//   simcli duel <regA> <regB|building> [terrain] [--distance D] [--formation-a F] [--formation-b F]
//               [--entrench-a] [--entrench-b] [--xp-a N] [--xp-b N] [--retreat-a S] [--retreat-b S]
//               [--until-wiped] [--max-seconds S] [--size N] [--seed S] [--tune name=value,...]
//               [--out F.rpl] [--verbose] [--game DIR]
//
// terrain: open (default), forest, jungle, city, sand, snow (the first Terrain record whose
// properties include terrain_<word>_infantry), or any Terrain / variation IDS.

#include <cstdio>
#include <fstream>
#include <memory>

#include "combat/tuning.hpp"
#include "core/ascii.hpp"
#include "core/sim.hpp"
#include "data/def_compiler.hpp"
#include "data/tgi/pipeline.hpp"
#include "data/tgi/value_parse.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"
#include "sim_commands.hpp"
#include "vfs/path.hpp"
#include "vfs/vfs.hpp"

namespace simcli {

using namespace corps;
using namespace corps::fixed_literals;

namespace {

struct GameData {
    vfs::Vfs vfs;
    std::unique_ptr<tgi::TgiDatabase> db;
    DefSet defs;
};

std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

std::string seconds(std::uint32_t ticks) {
    char b[32];
    std::snprintf(b, sizeof b, "%u.%u s", ticks / kTicksPerSecond, ticks % kTicksPerSecond);
    return b;
}

Fixed fixedArg(const SimArgs& a, const char* name, Fixed fallback) {
    const std::string s = a.get(name);
    if (s.empty()) return fallback;
    const tgi::FixedParse p = tgi::parseFixed(s);
    return p.ok ? p.value : fallback;
}

// The arena terrain: a keyword picks the first Terrain whose properties include
// terrain_<word>_infantry; anything else is taken as a Terrain / variation IDS.
bool terrainFor(const DefSet& d, const std::string& want, std::string& out) {
    const std::string w = asciiLowerCopy(want);
    if (w.empty() || w == "open") {
        out = "open";
        return true;
    }
    if (d.terrainOfName(w) != kNoDef) {
        out = w;
        return true;
    }
    const std::string prop = "terrain_" + w + "_infantry";
    for (const TerrainDef& t : d.terrains) {
        for (const std::string& p : t.properties) {
            if (p == prop) {
                out = t.ids;
                return true;
            }
        }
    }
    return false;
}

struct Side {
    std::string ids;
    bool building = false;
    EntityId root;                   // the regiment, or the building
    std::vector<EntityId> units;     // its fighting entities
    Fixed startHp;
    std::uint32_t shots = 0;
    Fixed damageDealt;
    std::uint32_t firstLoss = 0;     // tick + 1 of its first death (0: none)
    std::uint32_t wipedAt = 0;       // tick + 1 when its last unit died (0: not wiped)
    std::uint32_t routedAt = 0;      // tick + 1 when it first routed (0: never)
    std::uint32_t levelUps = 0;
    Fixed startMorale;
};

}  // namespace

int cmdDuel(const SimArgs& a) {
    constexpr int kSkipped = 77;
    if (a.positional.size() < 2) {
        std::fprintf(stderr, "usage: simcli duel <regA> <regB|building> [terrain] [--distance D] [--formation-a F] [--formation-b F]\n"
                             "                   [--entrench-a] [--entrench-b] [--xp-a N] [--xp-b N] [--retreat-a S] [--retreat-b S]\n"
                             "                   [--until-wiped] [--max-seconds S] [--size N] [--seed S] [--tune name=value,...]\n"
                             "                   [--out F.rpl] [--verbose]\n");
        return 2;
    }
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
    const DefSet& defs = game.defs;

    Side sides[2];
    sides[0].ids = asciiLowerCopy(a.positional[0]);
    sides[1].ids = asciiLowerCopy(a.positional[1]);
    if (defs.regiment(sides[0].ids) == nullptr) {
        std::fprintf(stderr, "'%s' is not a regiment\n", sides[0].ids.c_str());
        return 1;
    }
    if (defs.regiment(sides[1].ids) == nullptr) {
        if (defs.building(sides[1].ids) == nullptr) {
            std::fprintf(stderr, "'%s' is neither a regiment nor a building\n", sides[1].ids.c_str());
            return 1;
        }
        sides[1].building = true;
    }
    std::string terrain;
    const std::string terrainArg = a.positional.size() > 2 ? a.positional[2] : "open";
    if (!terrainFor(defs, terrainArg, terrain)) {
        std::fprintf(stderr, "unknown terrain '%s' (open, forest, jungle, city, sand, snow, or a Terrain IDS)\n", terrainArg.c_str());
        return 1;
    }
    CombatTuning tuning;
    if (a.has("tune")) {
        std::string err;
        if (!parseCombatTuning(tuning, a.get("tune"), err)) {
            std::fprintf(stderr, "--tune: %s\n", err.c_str());
            return 1;
        }
    }
    const auto size = static_cast<std::uint32_t>(std::clamp<std::int64_t>(fixedArg(a, "size", 96_fx).floorInt(), 32, 1024));
    const nav::MapDef map = nav::generateArenaMap(terrain, size);
    const Fixed dist = fixedArg(a, "distance", 30_fx);
    const std::uint32_t maxTicks = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>((fixedArg(a, "max-seconds", 600_fx) * Fixed::fromInt(kTicksPerSecond)).floorInt(), 1, 1000000));
    const auto seedOpt = tgi::parseInt(a.get("seed", "1"));
    const std::uint64_t seed = seedOpt && *seedOpt >= 0 ? static_cast<std::uint64_t>(*seedOpt) : 1;

    // Tick 0: spawn A west facing east, B east facing west.
    const Fixed mid = Fixed::fromInt(static_cast<std::int32_t>(size)) / 2_fx;
    const FVec2 posA{mid - dist / 2_fx, mid}, posB{mid + dist / 2_fx, mid};
    const Angle east{}, west = Angle::fromUnits(Angle::kHalf);
    std::vector<Command> cmds;
    std::uint16_t seq[2] = {0, 0};
    cmds.push_back(makeSpawnRegiment(0, 0, seq[0]++, sides[0].ids, posA, east));
    cmds.push_back(sides[1].building ? makeDebugSpawnBuilding(0, 1, seq[1]++, sides[1].ids, posB, west)
                                     : makeSpawnRegiment(0, 1, seq[1]++, sides[1].ids, posB, west));
    {
        Sim scratch({seed, &defs, &map, &tuning});
        scratch.step(cmds);
        const World& w = scratch.world();
        for (const EntityId id : w.regiments.ids()) (w.regiments.get(id)->player == 0 ? sides[0] : sides[1]).root = id;
        if (sides[1].building) {
            for (const EntityId id : w.combats.ids()) {
                if (!w.members.has(id)) sides[1].root = id;
            }
        }
        for (int s = 0; s < 2; ++s) {
            if (sides[s].root.isNull()) {
                std::fprintf(stderr, "could not spawn %s\n", sides[s].ids.c_str());
                return 1;
            }
            for (const EntityId id : w.combats.ids()) {
                const Member* m = w.members.get(id);
                if ((m != nullptr && m->organization == sides[s].root) || id == sides[s].root) {
                    sides[s].units.push_back(id);
                    sides[s].startHp = sides[s].startHp + w.health.get(id)->current;
                }
            }
            if (const Engagement* e = w.engagements.get(sides[s].root)) sides[s].startMorale = e->morale;
        }
    }
    // Tick 1: formations, entrenchment, experience, and the attack orders (an entrenched side holds
    // its ground instead). --retreat-x S: that side retreats toward its own edge S seconds in.
    const char* formation[2] = {"formation-a", "formation-b"};
    const char* entrench[2] = {"entrench-a", "entrench-b"};
    const char* xpArg[2] = {"xp-a", "xp-b"};
    const char* retreatArg[2] = {"retreat-a", "retreat-b"};
    for (std::uint8_t s = 0; s < 2; ++s) {
        if (sides[s].building) continue;
        if (a.has(formation[s])) cmds.push_back(makeSetFormation(1, s, seq[s]++, {sides[s].root}, asciiLowerCopy(a.get(formation[s]))));
        if (a.has(entrench[s])) cmds.push_back(makeDebugSetEntrenched(1, s, seq[s]++, {sides[s].root}, true));
        if (a.has(xpArg[s])) cmds.push_back(makeDebugSetExperience(1, s, seq[s]++, {sides[s].root}, fixedArg(a, xpArg[s], {})));
        // An entrenched side holds its ground (it auto-engages whatever comes into its zone).
        if (!a.has(entrench[s])) cmds.push_back(makeAttack(1, s, seq[s]++, {sides[s].root}, sides[1 - s].root));
        if (a.has(retreatArg[s])) {
            const auto at = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>((fixedArg(a, retreatArg[s], {}) * Fixed::fromInt(kTicksPerSecond)).floorInt(), 2, 1000000));
            const FVec2 edge{s == 0 ? 6_fx : Fixed::fromInt(static_cast<std::int32_t>(size)) - 6_fx, mid};
            cmds.push_back(makeRetreat(at, s, seq[s]++, {sides[s].root}, edge));
        }
    }
    const bool untilWiped = a.has("until-wiped");

    Sim sim({seed, &defs, &map, &tuning});
    CommandQueue q;
    for (const Command& c : cmds) q.schedule(c, 0);
    const auto sideOf = [&](EntityId id) -> int {
        for (int s = 0; s < 2; ++s) {
            if (std::find(sides[s].units.begin(), sides[s].units.end(), id) != sides[s].units.end()) return s;
        }
        return -1;
    };
    std::uint32_t t = 0;
    for (; t < maxTicks; ++t) {
        // Health before the tick, to measure damage dealt.
        Fixed before[2];
        for (int s = 0; s < 2; ++s) {
            for (const EntityId id : sides[s].units) {
                if (const Health* h = sim.world().registry.alive(id) ? sim.world().health.get(id) : nullptr) before[s] = before[s] + h->current;
            }
        }
        sim.step(q.take(t));
        Fixed after[2];
        std::size_t alive[2] = {0, 0};
        for (int s = 0; s < 2; ++s) {
            for (const EntityId id : sides[s].units) {
                const Health* h = sim.world().registry.alive(id) ? sim.world().health.get(id) : nullptr;
                if (h != nullptr && h->current > Fixed{}) {
                    after[s] = after[s] + h->current;
                    ++alive[s];
                }
            }
        }
        if (t > 0) {  // tick 0 spawns them
            for (int s = 0; s < 2; ++s) sides[1 - s].damageDealt = sides[1 - s].damageDealt + (before[s] - after[s]);
        }
        for (const SimEvent& e : sim.events()) {
            const int s = sideOf(e.entity);
            if (s < 0) continue;
            if (e.type == SimEvent::Type::Fired) ++sides[s].shots;
            if (e.type == SimEvent::Type::Died && sides[s].firstLoss == 0) sides[s].firstLoss = t + 1;
        }
        for (const SimEvent& e : sim.events()) {
            for (int s = 0; s < 2; ++s) {
                if (e.entity != sides[s].root) continue;
                if (e.type == SimEvent::Type::Routed && sides[s].routedAt == 0) sides[s].routedAt = t + 1;
                if (e.type == SimEvent::Type::LevelUp) ++sides[s].levelUps;
            }
        }
        bool over = false;
        for (int s = 0; s < 2; ++s) {
            if (alive[s] == 0 && sides[s].wipedAt == 0) {
                sides[s].wipedAt = t + 1;
                over = true;
            }
            if (!untilWiped && sides[s].routedAt == t + 1) over = true;  // a rout ends the fight (CALIBRATION §3)
        }
        if (over) {
            ++t;
            break;
        }
    }
    const std::uint32_t ticks = std::min(t, maxTicks);

    std::printf("duel %s vs %s on %s, %s units apart, seed %llu%s%s\n", sides[0].ids.c_str(), sides[1].ids.c_str(), map.name.c_str(),
                formatFixed(dist).c_str(), static_cast<unsigned long long>(seed), a.has("tune") ? ", tuning " : "",
                a.has("tune") ? formatCombatTuning(tuning).c_str() : "");
    const char* names[2] = {"A", "B"};
    // A side is beaten when it was wiped out or (unless --until-wiped) routed; the earlier one loses.
    const auto beatenAt = [&](const Side& sd) -> std::uint32_t {
        std::uint32_t at = sd.wipedAt;
        if (!untilWiped && sd.routedAt != 0) at = at == 0 ? sd.routedAt : std::min(at, sd.routedAt);
        return at;
    };
    const std::uint32_t beaten[2] = {beatenAt(sides[0]), beatenAt(sides[1])};
    int winner = -1;
    if (beaten[0] != 0 && (beaten[1] == 0 || beaten[0] < beaten[1])) winner = 1;
    if (beaten[1] != 0 && (beaten[0] == 0 || beaten[1] < beaten[0])) winner = 0;
    if (winner >= 0) {
        const Side& loser = sides[1 - winner];
        const bool routed = !untilWiped && loser.routedAt != 0 && (loser.wipedAt == 0 || loser.routedAt <= loser.wipedAt);
        std::printf("  result: %s wins; %s %s at %s (time to %s)\n", names[winner], names[1 - winner], routed ? "routed" : "wiped out",
                    seconds(beaten[1 - winner]).c_str(), routed ? "rout" : "kill");
    } else if (beaten[0] != 0) {
        std::printf("  result: both beaten at %s\n", seconds(beaten[0]).c_str());
    } else {
        std::printf("  result: no winner after %s\n", seconds(ticks).c_str());
    }
    for (int s = 0; s < 2; ++s) {
        const Side& sd = sides[s];
        std::size_t alive = 0;
        Fixed hp;
        std::string each;
        for (const EntityId id : sd.units) {
            const Health* h = sim.world().registry.alive(id) ? sim.world().health.get(id) : nullptr;
            if (h == nullptr || h->current <= Fixed{}) continue;
            ++alive;
            hp = hp + h->current;
            each += " " + std::to_string(h->current.floorInt());
        }
        const std::int64_t pct = sd.startHp > Fixed{} ? (hp * Fixed::fromInt(100) / sd.startHp).floorInt() : 0;
        std::printf("  %s %-28s %zu of %zu survive, %lld of %lld hp (%lld%%)%s%s; %u shots, %lld damage dealt; first loss %s\n", names[s],
                    sd.ids.c_str(), alive, sd.units.size(), static_cast<long long>(hp.floorInt()), static_cast<long long>(sd.startHp.floorInt()),
                    static_cast<long long>(pct), alive != 0 ? ":" : "", each.c_str(), sd.shots,
                    static_cast<long long>(sd.damageDealt.floorInt()), sd.firstLoss != 0 ? seconds(sd.firstLoss).c_str() : "-");
    }
    {
        static const char* kStateName[] = {"idle", "engaging", "stalemate", "reform", "regroup", "retreating", "routing", "exhausted"};
        for (int s = 0; s < 2; ++s) {
            const Engagement* e = sim.world().registry.alive(sides[s].root) ? sim.world().engagements.get(sides[s].root) : nullptr;
            if (e == nullptr) continue;
            std::printf("  %s morale %lld (started %lld, breaks at %lld), %s; XP %lld, level %u (%u level-ups); efficiency %s%%%s; routed %s\n",
                        names[s], static_cast<long long>(e->morale.floorInt()), static_cast<long long>(sides[s].startMorale.floorInt()),
                        static_cast<long long>(defs.regiment(sides[s].ids)->moraleBreakpoint.floorInt()), kStateName[static_cast<int>(e->state)],
                        static_cast<long long>(e->xp.floorInt()), e->level, sides[s].levelUps,
                        formatFixed(e->efficiency * Fixed::fromInt(100)).substr(0, 5).c_str(), e->entrenched ? ", entrenched" : "",
                        sides[s].routedAt != 0 ? seconds(sides[s].routedAt).c_str() : "-");
        }
        std::printf("  army XP: player 0 %lld, player 1 %lld\n", static_cast<long long>(sim.world().kingdoms[0].armyXp.floorInt()),
                    static_cast<long long>(sim.world().kingdoms[1].armyXp.floorInt()));
    }
    if (a.has("verbose")) {  // why a fight ended where it did
        static const char* kState[] = {"idle", "engaging", "stalemate", "reform", "regroup", "retreating", "routing", "exhausted"};
        static const char* kMode[] = {"none", "attack", "attack-move"};
        static const char* kOrder[] = {"idle", "wait-path", "moving", "wheeling", "arriving", "following", "engaged"};
        const World& w = sim.world();
        for (int s = 0; s < 2; ++s) {
            const Engagement* e = w.engagements.get(sides[s].root);
            const Order* o = w.orders.get(sides[s].root);
            const Transform* at = w.transforms.get(sides[s].root);
            if (e != nullptr && o != nullptr && at != nullptr) {
                std::printf("  %s engagement %s mode %s%s%s no-target %u, order %s, anchor (%s, %s)\n", names[s],
                            kState[static_cast<int>(e->state)], kMode[static_cast<int>(e->mode)], e->suspended ? " suspended" : "",
                            e->closing ? " closing" : "", e->noTarget, kOrder[static_cast<int>(o->state)],
                            formatFixed(at->position.x).c_str(), formatFixed(at->position.y).c_str());
            }
            for (const EntityId id : sides[s].units) {
                const Combat* cb = w.registry.alive(id) ? w.combats.get(id) : nullptr;
                const Transform* ut = w.transforms.get(id);
                if (cb == nullptr || ut == nullptr) continue;
                std::printf("    unit %u at (%s, %s) facing %u:", id.index(), formatFixed(ut->position.x).c_str(),
                            formatFixed(ut->position.y).c_str(), ut->facing.units);
                for (std::size_t j = 0; j < cb->targets.size(); ++j) {
                    const Transform* tt = cb->targets[j].isNull() ? nullptr : w.transforms.get(cb->targets[j]);
                    std::printf(" [w%zu cd %u target %s", j, cb->cooldown[j], tt != nullptr ? std::to_string(cb->targets[j].index()).c_str() : "-");
                    if (tt != nullptr) {
                        std::printf(" at %s bearing-off %d", formatFixed(distance(ut->position, tt->position)).c_str(),
                                    Angle::delta(ut->facing, (tt->position - ut->position).angle()));
                    }
                    std::printf("]");
                }
                std::printf("\n");
            }
        }
    }
    const std::uint64_t direct = stateHash(sim);

    Replay rep;
    rep.header.simVersion = simVersion();
    rep.header.dataHash = defs.dataHash;
    rep.header.mapSpec = map.name;
    rep.header.mapHash = map.mapHash;
    rep.header.matchSeed = seed;
    rep.header.tickCount = ticks;
    rep.header.players = {"A", "B"};
    rep.header.tuning = formatCombatTuning(tuning);
    rep.commands = cmds;
    RunOptions opt;
    opt.map = &map;
    const RunResult run = runReplay(rep, &defs, opt);
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
    return 0;
}

}  // namespace simcli
