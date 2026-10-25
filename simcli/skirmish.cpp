// simcli skirmish (M6 part 2): a 1v1 on a real map (tools/rmp.py map.json, e.g. Golden Fields).
// Each kingdom starts at its map placeholder with its nation's Corps HQ (NationDef placeholders)
// and N infantry regiments; the first regiment of each side goes to take the nearest neutral city,
// the others attack-move on the enemy HQ. Prints a line per minute (money, net income, cities
// held, regiments, regiments in supply) and the supply / capture / victory events, until the
// match is won or time runs out. Records the run as a replay so `simcli determinism-check
// --map F.json` can verify it.
//
//   simcli skirmish --map F.json [NATION_A [NATION_B]] [--minutes M] [--regiments N] [--regiments-b N]
//                   [--city-ratio R] [--seed S] [--out F.rpl] [--game DIR]
//
// NATION: gbr (default A), ger (default B), usa, rus, jpn, per, sau; regiments are
// regiment_<nation>_infantry (--regiments each, or --regiments-b for B). Every 30 s, attackers
// left idle are ordered to Attack the enemy Corps HQ. R: the city victory ratio (default: the map's CityVictoryRatio rule).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "core/ascii.hpp"
#include "core/sim.hpp"
#include "data/def_compiler.hpp"
#include "data/tgi/pipeline.hpp"
#include "data/tgi/value_parse.hpp"
#include "nav/map_def.hpp"
#include "nav/nav_world.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"
#include "sim_commands.hpp"
#include "systems/economy.hpp"
#include "systems/supply.hpp"
#include "vfs/path.hpp"
#include "vfs/vfs.hpp"

namespace simcli {

using namespace corps;
using namespace corps::fixed_literals;

namespace {

std::string money(Fixed v) {
    char b[32];
    const std::int64_t hundredths = (v * 100).roundInt();
    std::snprintf(b, sizeof b, "%lld.%02lld", static_cast<long long>(hundredths / 100), static_cast<long long>(std::llabs(hundredths % 100)));
    return b;
}

FVec2 toward(FVec2 from, FVec2 to, Fixed by) {
    const FVec2 d = to - from;
    const Fixed len = distance(from, to);
    return len > Fixed{} ? from + d * (by / len) : from;
}

bool walkable(const nav::NavGrid& g, FVec2 p) {
    const auto x = static_cast<std::int32_t>(p.x.floorInt());
    const auto y = static_cast<std::int32_t>(p.y.floorInt());
    if (!g.inside(x, y)) return false;
    // The cell and its neighbours: a regiment needs some room.
    for (std::int32_t dy = -2; dy <= 2; ++dy) {
        for (std::int32_t dx = -2; dx <= 2; ++dx) {
            if (!g.passable(nav::MoveClass::Foot, x + dx, y + dy) || g.stampCount(g.index(x + dx, y + dy)) != 0) return false;
        }
    }
    return true;
}

// The walkable point nearest `p` on rings of 2 m (p itself if none within 30 m).
FVec2 nearestWalkable(const nav::NavGrid& g, FVec2 p) {
    if (walkable(g, p)) return p;
    for (std::int32_t r = 2; r <= 30; r += 2) {
        for (std::uint32_t k = 0; k < 16; ++k) {
            const FVec2 c = p + FVec2{Fixed::fromInt(r), 0_fx}.rotated(Angle::fromUnits(k * 4096));
            if (walkable(g, c)) return c;
        }
    }
    return p;
}

}  // namespace

int cmdSkirmish(const SimArgs& a) {
    constexpr int kSkipped = 77;
    if (!a.has("map")) {
        std::fprintf(stderr, "skirmish needs --map F.json (tools/rmp.py output)\n");
        return 2;
    }
    std::optional<nav::MapDef> mapOpt;
    if (!mapFromArgs(a, mapOpt) || !mapOpt) return kSkipped;  // no map: the data isn't there (CI)
    const nav::MapDef& map = *mapOpt;
    if (!a.gameDir) {
        std::fprintf(stderr, "no game install: pass --game DIR or create config/game_path.cfg\n");
        return kSkipped;
    }
    vfs::Vfs vfs;
    std::vector<std::string> log;
    if (!vfs::mountGameInstall(vfs, *a.gameDir, {}, log)) {
        std::fprintf(stderr, "error: nothing could be mounted from %s\n", vfs::pathToUtf8(*a.gameDir).c_str());
        return kSkipped;
    }
    const auto db = tgi::loadTgiDatabase(vfs);
    tgi::Diagnostics diags;
    const DefSet defs = compileDefs(*db, diags);

    const std::vector<MapStart> starts = mapStarts(map);
    if (starts.size() < 2) {
        std::fprintf(stderr, "the map has %zu start placeholders; a skirmish needs 2\n", starts.size());
        return 1;
    }
    const std::string nations[2] = {asciiLowerCopy(a.positional.size() > 0 ? a.positional[0] : "gbr"),
                                    asciiLowerCopy(a.positional.size() > 1 ? a.positional[1] : "ger")};
    std::string corpsIds[2], regIds[2];
    for (int s = 0; s < 2; ++s) {
        corpsIds[s] = placeholderThing(defs, nations[s], starts[s].placeholder);
        if (corpsIds[s].empty()) corpsIds[s] = nations[s] + "_corps";
        regIds[s] = "regiment_" + nations[s] + "_infantry";
        if (defs.building(corpsIds[s]) == nullptr || defs.regiment(regIds[s]) == nullptr) {
            std::fprintf(stderr, "nation %s: no '%s' or '%s'\n", nations[s].c_str(), corpsIds[s].c_str(), regIds[s].c_str());
            return 1;
        }
    }
    const tgi::FixedParse minutesArg = tgi::parseFixed(a.get("minutes", "10"));
    const std::uint32_t ticks = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(((minutesArg.ok ? minutesArg.value : 10_fx) * Fixed::fromInt(60 * kTicksPerSecond)).floorInt(), 1, 1000000));
    const auto regsOpt = tgi::parseInt(a.get("regiments", "3"));
    const int perA = static_cast<int>(std::clamp<std::int64_t>(regsOpt ? *regsOpt : 3, 1, 12));
    const auto regsBOpt = tgi::parseInt(a.get("regiments-b", std::to_string(perA)));
    const int perSide[2] = {perA, static_cast<int>(std::clamp<std::int64_t>(regsBOpt ? *regsBOpt : perA, 1, 12))};
    const auto seedOpt = tgi::parseInt(a.get("seed", "1"));
    const std::uint64_t seed = seedOpt && *seedOpt >= 0 ? static_cast<std::uint64_t>(*seedOpt) : 1;
    Fixed ratio = map.rule("CityVictoryRatio", defs.economy.cityVictoryRatio);
    if (a.has("city-ratio")) {
        const tgi::FixedParse r = tgi::parseFixed(a.get("city-ratio"));
        if (!r.ok || r.value < Fixed{} || r.value > 1_fx) {
            std::fprintf(stderr, "bad --city-ratio (0..1)\n");
            return 2;
        }
        ratio = r.value;
    }

    Sim sim({seed, &defs, &map});
    std::vector<Command> cmds;
    std::uint16_t seqs[kMaxPlayers] = {};
    std::vector<Command> now;
    const auto issue = [&](Command c) {
        c.tick = sim.tick();
        c.seq = seqs[c.player]++;
        cmds.push_back(c);
        now.push_back(c);
    };
    const FVec2 centre{Fixed::fromInt(static_cast<std::int32_t>(map.width)) / 2_fx, Fixed::fromInt(static_cast<std::int32_t>(map.height)) / 2_fx};
    issue(makeSetMatchRules(0, starts[0].player, 0, {}, ratio, true));  // each kingdom its own team
    for (int s = 0; s < 2; ++s) {
        const MapStart& st = starts[s];
        issue(makeStartPlayer(0, st.player, 0, corpsIds[s], st.position, st.facing));
        // Infantry in a line 22 m toward the map centre from the HQ.
        const FVec2 front = toward(st.position, centre, 22_fx);
        const FVec2 side = FVec2{-(front - st.position).y, (front - st.position).x} * (1_fx / 22_fx);
        for (int k = 0; k < perSide[s]; ++k) {
            const Fixed off = Fixed::fromInt(10 * k - 5 * (perSide[s] - 1));
            issue(makeSpawnRegiment(0, st.player, 0, regIds[s], nearestWalkable(sim.nav()->grid, front + side * off), (centre - st.position).angle()));
        }
    }
    std::printf("skirmish: %s (%s at %lld,%lld) vs %s (%s at %lld,%lld) on %s, %u ticks, %d vs %d regiments, city ratio %s, seed %llu\n",
                nations[0].c_str(), corpsIds[0].c_str(), static_cast<long long>(starts[0].position.x.roundInt()),
                static_cast<long long>(starts[0].position.y.roundInt()), nations[1].c_str(), corpsIds[1].c_str(),
                static_cast<long long>(starts[1].position.x.roundInt()), static_cast<long long>(starts[1].position.y.roundInt()),
                map.name.c_str(), ticks, perSide[0], perSide[1], money(ratio).c_str(), static_cast<unsigned long long>(seed));
    std::size_t cities = 0;
    for (std::size_t i = 0; i < sim.world().structures.size(); ++i) cities += sim.world().structures.data()[i].player == kNeutral;
    std::printf("cities on the map: %zu\n", cities);

    std::size_t rejected = 0, replaced = 0, died = 0;
    std::string events;
    std::uint32_t endAt = ticks;
    for (std::uint32_t t = 0; t < endAt; ++t) {
        if (t == 1) {
            // Orders: regiment 0 of each side to the nearest neutral city (8 m from its centre, on
            // its side); the others attack-move on the enemy HQ.
            const World& w = sim.world();
            for (int s = 0; s < 2; ++s) {
                const std::uint8_t p = starts[s].player;
                std::vector<EntityId> regs;
                for (std::size_t i = 0; i < w.regiments.size(); ++i) {
                    if (w.regiments.data()[i].player == p) regs.push_back(w.regiments.ids()[i]);
                }
                if (regs.empty()) continue;
                const FVec2 at = w.transforms.get(regs[0])->position;
                const FVec2* best = nullptr;
                for (std::size_t i = 0; i < w.structures.size(); ++i) {
                    const FVec2& c = w.transforms.get(w.structures.ids()[i])->position;
                    if (w.structures.data()[i].player == kNeutral && (best == nullptr || distance(at, c) < distance(at, *best))) best = &c;
                }
                if (best != nullptr) {
                    // 8 m from the centre: on the regiment's side, else the first walkable spot round it.
                    const nav::NavGrid& g = sim.nav()->grid;
                    const Angle side = (at - *best).angle();
                    FVec2 goal = toward(*best, at, 8_fx);
                    for (std::uint32_t k = 0; k < 8; ++k) {
                        const FVec2 c = *best + FVec2{8_fx, 0_fx}.rotated(side + Angle::fromUnits(k * 8192));
                        const auto x = static_cast<std::int32_t>(c.x.floorInt());
                        const auto y = static_cast<std::int32_t>(c.y.floorInt());
                        if (g.passable(nav::MoveClass::Foot, x, y) && g.stampCount(g.index(x, y)) == 0) {
                            goal = c;
                            break;
                        }
                    }
                    issue(makeMoveGroup(0, p, 0, {regs[0]}, goal));
                }
                const FVec2 enemyHq = starts[1 - s].position;
                for (std::size_t k = 1; k < regs.size(); ++k) issue(makeAttackMove(0, p, 0, {regs[k]}, nearestWalkable(sim.nav()->grid, toward(enemyHq, at, 14_fx))));
            }
        }
        if (t > 1 && t % (30 * kTicksPerSecond) == 0 && !sim.world().match.over) {
            // Every 30 s: attackers left idle (after their attack-move, or a fight) attack the enemy
            // Corps HQ itself.
            const World& w = sim.world();
            for (int s = 0; s < 2; ++s) {
                const std::uint8_t p = starts[s].player;
                EntityId hq;
                for (std::size_t i = 0; i < w.structures.size() && hq.isNull(); ++i) {
                    const DefRef* r = w.defRefs.get(w.structures.ids()[i]);
                    if (w.structures.data()[i].player == starts[1 - s].player && r != nullptr && defs.buildings[r->index].providesCorps) {
                        hq = w.structures.ids()[i];
                    }
                }
                if (hq.isNull()) continue;
                bool first = true;
                for (std::size_t i = 0; i < w.regiments.size(); ++i) {
                    if (w.regiments.data()[i].player != p) continue;
                    const EntityId id = w.regiments.ids()[i];
                    if (first) {  // the city holder stays
                        first = false;
                        continue;
                    }
                    const Order* o = w.orders.get(id);
                    const Engagement* e = w.engagements.get(id);
                    if (o != nullptr && o->state == Order::State::Idle && e != nullptr && e->mode != Engagement::Mode::Attack &&
                        e->state != Engagement::State::Routing && e->state != Engagement::State::Exhausted) {
                        issue(makeAttack(0, p, 0, {id}, hq));
                    }
                }
            }
        }
        sim.step(now);
        now.clear();
        for (const SimEvent& e : sim.events()) {
            switch (e.type) {
            case SimEvent::Type::CityCaptured:
                events += " city-captured:p" + std::to_string(e.player) + "@" + std::to_string(sim.world().transforms.get(e.entity)->position.x.roundInt()) +
                          "," + std::to_string(sim.world().transforms.get(e.entity)->position.y.roundInt());
                break;
            case SimEvent::Type::KingdomDefeated: events += " DEFEATED:p" + std::to_string(e.player); break;
            case SimEvent::Type::TeamVictory: events += " VICTORY:team" + std::to_string(e.detail); break;
            case SimEvent::Type::CityRatioVictory: events += " CITY-VICTORY:team" + std::to_string(e.detail); break;
            case SimEvent::Type::Bankrupt: events += " bankrupt:p" + std::to_string(e.player); break;
            case SimEvent::Type::Blocked: events += " blocked:p" + std::to_string(e.player) + "#" + std::to_string(e.entity.index()); break;
            case SimEvent::Type::Replaced: ++replaced; break;
            case SimEvent::Type::Died: ++died; break;
            case SimEvent::Type::CommandRejected:
                ++rejected;
                events += " rejected:" + std::to_string(static_cast<int>(e.command));
                break;
            default: break;
            }
        }
        if (sim.world().match.over && endAt == ticks) endAt = std::min(ticks, t + 1 + 10 * kTicksPerSecond);  // 10 s more, then stop
        if ((t + 1) % (60 * kTicksPerSecond) == 0 || t + 1 == endAt) {
            const World& w = sim.world();
            std::printf("%3u:%02u", (t + 1) / 600, ((t + 1) / 10) % 60);
            for (int s = 0; s < 2; ++s) {
                const std::uint8_t p = starts[s].player;
                std::size_t held = 0, regs = 0, inSupply = 0;
                for (std::size_t i = 0; i < w.structures.size(); ++i) {
                    const DefRef* r = w.defRefs.get(w.structures.ids()[i]);
                    held += w.structures.data()[i].player == p && r != nullptr && isCity(defs, r->index);
                }
                for (std::size_t i = 0; i < w.regiments.size(); ++i) {
                    if (w.regiments.data()[i].player != p) continue;
                    ++regs;
                    const Supply* sp = w.supplies.get(w.regiments.ids()[i]);
                    inSupply += sp != nullptr && sp->inSupply;
                }
                const Ledger l = ledgerOf(sim, p);
                std::printf("  p%u %9s (%+8s/min) cities %zu regiments %zu (%zu in supply)", p, money(w.kingdoms[p].money).c_str(),
                            money(l.moneyNet()).c_str(), held, regs, inSupply);
            }
            std::printf(" %s\n", events.empty() ? "" : events.c_str() + 1);
            events.clear();
        }
    }
    const Match& m = sim.world().match;
    if (m.over) {
        std::printf("result: team %u wins at %u:%02u\n", m.winner, m.endTick / 600, (m.endTick / 10) % 60);
    } else {
        std::printf("result: no winner after %u:%02u\n", endAt / 600, (endAt / 10) % 60);
    }
    std::printf("units died: %zu, replaced in supply: %zu, rejected commands: %zu\n", died, replaced, rejected);
    {
        const World& w = sim.world();
        for (std::size_t i = 0; i < w.regiments.size(); ++i) {
            const EntityId id = w.regiments.ids()[i];
            const Transform* t = w.transforms.get(id);
            const Order* o = w.orders.get(id);
            const Supply* sp = w.supplies.get(id);
            std::size_t units = 0;
            for (const Member& mb : w.members.data()) units += mb.organization == id;
            std::printf("  p%u %-28s at %4lld,%-4lld units %zu order %d%s\n", w.regiments.data()[i].player,
                        defs.regiments[w.defRefs.get(id)->index].ids.c_str(), static_cast<long long>(t->position.x.roundInt()),
                        static_cast<long long>(t->position.y.roundInt()), units, o != nullptr ? static_cast<int>(o->state) : -1,
                        sp != nullptr && sp->inSupply ? " in supply" : "");
        }
    }

    const std::uint64_t direct = stateHash(sim);
    Replay rep;
    rep.header.simVersion = simVersion();
    rep.header.dataHash = defs.dataHash;
    rep.header.mapSpec = map.name;
    rep.header.mapHash = map.mapHash;
    rep.header.matchSeed = seed;
    rep.header.tickCount = endAt;
    rep.header.players = {nations[0], nations[1]};
    rep.commands = cmds;
    RunOptions opt;
    opt.map = &map;
    const RunResult run = runReplay(rep, &defs, opt);
    rep.hashes = run.hashes;
    char hash[19];
    std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(direct));
    std::printf("STATE HASH %s\n", hash);
    if (run.finalHash != direct) {
        std::printf("ERROR: replay runner disagrees\n");
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
