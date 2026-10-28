// simcli slice (M6 close-out): the vertical slice played headless. A scripted player (ai::
// makeSlicePlayer) against the dummy opponent (ai::makeDummyAi) on a real map, from the
// skirmish setup (map, nations, starting money) to the end of the match. Prints the match's
// timeline (both controllers' decisions, captures, eliminations, the result) and a line a minute,
// then checks the slice path for the scripted player:
//   Infantry and Armour Division HQs deployed; rifle infantry, AT infantry, light and medium tanks
//   recruited; both cities taken; the enemy eliminated; victory.
// Records the whole match as a replay (--out) for `simcli determinism-check --map F.json`.
//
//   simcli slice --map F.json [NATION [ENEMY]] [--money M] [--minutes M] [--seed S] [--out F.rpl] [--trace] [--view]
//
// NATION: gbr (default), ENEMY: ger (default). --money: starting money (default: the map's).
// Exit 0 when the slice path was played through, 1 when not, 77 without the data or the map.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>

#include "ai/controller.hpp"
#include "ai/view.hpp"
#include "core/ascii.hpp"
#include "core/sim.hpp"
#include "data/def_compiler.hpp"
#include "data/tgi/pipeline.hpp"
#include "data/tgi/value_parse.hpp"
#include "nav/map_def.hpp"
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

std::string clock(std::uint32_t tick) {
    char b[16];
    std::snprintf(b, sizeof b, "%u:%02u", tick / 600, (tick / 10) % 60);
    return b;
}

}  // namespace

int cmdSlice(const SimArgs& a) {
    constexpr int kSkipped = 77;
    if (!a.has("map")) {
        std::fprintf(stderr, "slice needs --map F.json (tools/rmp.py output)\n");
        return 2;
    }
    std::optional<nav::MapDef> mapOpt;
    if (!mapFromArgs(a, mapOpt) || !mapOpt) return kSkipped;
    const nav::MapDef& map = *mapOpt;
    if (!a.gameDir) {
        std::fprintf(stderr, "no game install: pass --game DIR or create config/game_path.cfg\n");
        return kSkipped;
    }
    vfs::Vfs vfs;
    std::vector<std::string> log;
    if (!vfs::mountGameInstall(vfs, *a.gameDir, {}, log)) return kSkipped;
    const auto db = tgi::loadTgiDatabase(vfs);
    tgi::Diagnostics diags;
    const DefSet defs = compileDefs(*db, diags);

    const std::vector<MapStart> starts = mapStarts(map);
    if (starts.size() < 2) {
        std::fprintf(stderr, "the map has %zu start placeholders; the slice needs 2\n", starts.size());
        return 1;
    }
    const std::string nations[2] = {asciiLowerCopy(a.positional.size() > 0 ? a.positional[0] : "gbr"),
                                    asciiLowerCopy(a.positional.size() > 1 ? a.positional[1] : "ger")};
    std::string corpsIds[2];
    for (int s = 0; s < 2; ++s) {
        corpsIds[s] = placeholderThing(defs, nations[s], starts[s].placeholder);
        if (corpsIds[s].empty()) corpsIds[s] = nations[s] + "_corps";
        if (defs.building(corpsIds[s]) == nullptr) {
            std::fprintf(stderr, "nation %s: no '%s'\n", nations[s].c_str(), corpsIds[s].c_str());
            return 1;
        }
    }
    const tgi::FixedParse minutesArg = tgi::parseFixed(a.get("minutes", "45"));
    const std::uint32_t ticks = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(((minutesArg.ok ? minutesArg.value : 45_fx) * Fixed::fromInt(60 * kTicksPerSecond)).floorInt(), 1, 1000000));
    Fixed money;
    if (a.has("money")) {
        const tgi::FixedParse m = tgi::parseFixed(a.get("money"));
        if (!m.ok || m.value < Fixed{}) {
            std::fprintf(stderr, "bad --money\n");
            return 2;
        }
        money = m.value;
    }
    const bool trace = a.has("trace");  // every minute: the enemy buildings' health and your regiments; Blocked events
    const bool showView = a.has("view");  // every minute: what each side's AI read API (ai/view) shows it
    const auto seedOpt = tgi::parseInt(a.get("seed", "1"));
    const std::uint64_t seed = seedOpt && *seedOpt >= 0 ? static_cast<std::uint64_t>(*seedOpt) : 1;

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
    const std::uint8_t you = starts[0].player;
    const std::uint8_t enemy = starts[1].player;
    // The setup screen's choices: rules, then both kingdoms at their start positions.
    issue(makeSetMatchRules(0, you, 0, {}, map.rule("CityVictoryRatio", defs.economy.cityVictoryRatio), true, money));
    for (int s = 0; s < 2; ++s) issue(makeStartPlayer(0, starts[s].player, 0, corpsIds[s], starts[s].position, starts[s].facing));
    std::unique_ptr<ai::Controller> controllers[2] = {ai::makeSlicePlayer(you, nations[0]), ai::makeDummyAi(enemy, nations[1])};

    std::printf("slice: you %s (%s, scripted) vs %s (%s, dummy AI) on %s, money %s, up to %s, seed %llu\n", nations[0].c_str(),
                corpsIds[0].c_str(), nations[1].c_str(), corpsIds[1].c_str(), map.name.c_str(),
                money > Fixed{} ? std::to_string(money.roundInt()).c_str() : "the map's", clock(ticks).c_str(),
                static_cast<unsigned long long>(seed));
    std::set<std::string> unpacked, recruited;
    std::set<std::uint32_t> citiesTaken;
    bool enemyOut = false, youOut = false;
    std::uint32_t endAt = ticks;
    std::size_t rejected = 0;
    std::map<int, std::size_t> rejectedBy;  // command type * 16 + player
    std::vector<ai::View> views;  // --view: updated every second, so last-seen memory works
    if (showView) views = {ai::View(you), ai::View(enemy)};
    for (std::uint32_t t = 0; t < endAt; ++t) {
        for (auto& c : controllers) {
            std::vector<Command> out;
            c->think(sim, out);
            for (Command& cmd : out) issue(cmd);
            for (const std::string& n : c->notes) std::printf("  %s\n", n.c_str());
            c->notes.clear();
        }
        sim.step(now);
        now.clear();
        const World& w = sim.world();
        for (const SimEvent& e : sim.events()) {
            const auto defIds = [&](EntityId id) -> std::string {
                const DefRef* r = w.defRefs.get(id);
                if (r == nullptr) return "?";
                if (r->kind == DefRef::Kind::Regiment) return defs.regiments[r->index].ids;
                if (r->kind == DefRef::Kind::Building) return defs.buildings[r->index].ids;
                return defs.units[r->index].ids;
            };
            switch (e.type) {
            case SimEvent::Type::Unpacked:
                if (e.player == you) unpacked.insert(defIds(e.entity));
                std::printf("  %s p%u deployed %s\n", clock(t).c_str(), e.player, defIds(e.entity).c_str());
                break;
            case SimEvent::Type::Recruited:
                if (e.player == you) recruited.insert(defIds(e.entity));
                break;
            case SimEvent::Type::CityCaptured:
                if (e.player == you) citiesTaken.insert(e.entity.bits);
                std::printf("  %s p%u took the city at %lld,%lld\n", clock(t).c_str(), e.player,
                            static_cast<long long>(e.at.x.roundInt()), static_cast<long long>(e.at.y.roundInt()));
                break;
            case SimEvent::Type::KingdomDefeated:
                (e.player == enemy ? enemyOut : youOut) = true;
                std::printf("  %s p%u DEFEATED\n", clock(t).c_str(), e.player);
                break;
            case SimEvent::Type::TeamVictory:
            case SimEvent::Type::CityRatioVictory:
                std::printf("  %s VICTORY for team %u\n", clock(t).c_str(), e.detail);
                break;
            case SimEvent::Type::Blocked:
                if (trace) {
                    const Path* p = w.paths.get(e.entity);
                    const Transform* tr = w.transforms.get(e.entity);
                    std::printf("  %s TRACE blocked %u at %lld,%lld path status %d\n", clock(t).c_str(), e.entity.bits,
                                tr ? static_cast<long long>(tr->position.x.roundInt()) : -1, tr ? static_cast<long long>(tr->position.y.roundInt()) : -1,
                                p ? static_cast<int>(p->status) : -1);
                }
                break;
            case SimEvent::Type::CommandRejected:
                ++rejected;
                ++rejectedBy[static_cast<int>(e.command) * 16 + e.player];
                break;
            default:
                break;
            }
        }
        if (w.match.over && endAt == ticks) endAt = std::min(ticks, t + 1 + 10 * kTicksPerSecond);
        if ((t + 1) % kTicksPerSecond == 0) {
            for (ai::View& v : views) v.update(sim);
        }
        if ((t + 1) % (60 * kTicksPerSecond) == 0 || t + 1 == endAt) {
            for (const ai::View& v : views) {
                std::size_t structures = 0, supplied = 0, cities = 0;
                for (const ai::Seen& s : v.visible()) structures += s.structure;
                for (const ai::OwnRegiment& r : v.regiments) supplied += r.inSupply;
                for (const ai::City& c : v.cities()) cities += c.owner == v.player();
                std::printf("  VIEW %s p%u sees %zu regiments %zu buildings, remembers %zu; own %zu regiments (%zu in supply) %zu buildings; "
                            "cities %zu/%zu; money %lld\n",
                            clock(t + 1).c_str(), v.player(), v.visible().size() - structures, structures, v.remembered().size(),
                            v.regiments.size(), supplied, v.buildings.size(), cities, v.cities().size(),
                            static_cast<long long>(v.money.floorInt()));
            }
            std::printf("%6s", clock(t + 1).c_str());
            for (const std::uint8_t p : {you, enemy}) {
                std::size_t regs = 0, cities = 0;
                for (std::size_t i = 0; i < w.regiments.size(); ++i) regs += w.regiments.data()[i].player == p;
                for (std::size_t i = 0; i < w.structures.size(); ++i) {
                    const DefRef* r = w.defRefs.get(w.structures.ids()[i]);
                    cities += w.structures.data()[i].player == p && r != nullptr && isCity(defs, r->index);
                }
                const Ledger l = ledgerOf(sim, p);
                std::printf("   p%u money %5lld (%+4lld/min) regiments %2zu cities %zu", p, static_cast<long long>(w.kingdoms[p].money.floorInt()),
                            static_cast<long long>(l.moneyNet().roundInt()), regs, cities);
            }
            std::printf("\n");
            if (trace) {
                for (std::size_t i = 0; i < w.structures.size(); ++i) {
                    const EntityId id = w.structures.ids()[i];
                    const DefRef* r = w.defRefs.get(id);
                    const Health* h = w.health.get(id);
                    if (w.structures.data()[i].player != enemy || r == nullptr || h == nullptr) continue;
                    std::printf("  TRACE %s %lld/%lld\n", defs.buildings[r->index].ids.c_str(), static_cast<long long>(h->current.roundInt()),
                                static_cast<long long>(h->max.roundInt()));
                }
                for (std::size_t i = 0; i < w.regiments.size(); ++i) {
                    const EntityId id = w.regiments.ids()[i];
                    const Engagement* e = w.engagements.get(id);
                    const Transform* tr = w.transforms.get(id);
                    if (w.regiments.data()[i].player == enemy) {
                        std::size_t units = 0;
                        for (const Member& mb : w.members.data()) units += mb.organization == id;
                        std::printf("  TRACE enemy reg %u %s at %lld,%lld units %zu\n", id.bits, defs.regiments[w.defRefs.get(id)->index].ids.c_str(),
                                    static_cast<long long>(tr->position.x.roundInt()), static_cast<long long>(tr->position.y.roundInt()), units);
                    }
                    if (w.regiments.data()[i].player != you || e == nullptr) continue;
                    std::printf("  TRACE reg %u %s at %lld,%lld state %d mode %d target %u\n", id.bits, defs.regiments[w.defRefs.get(id)->index].ids.c_str(),
                                static_cast<long long>(tr->position.x.roundInt()), static_cast<long long>(tr->position.y.roundInt()),
                                static_cast<int>(e->state), static_cast<int>(e->mode), e->target.bits);
                }
            }
        }
    }
    const Match& m = sim.world().match;
    const bool won = m.over && m.winner == sim.world().teamOf(you);
    std::string why;
    for (const auto& [k, n] : rejectedBy) why += " type " + std::to_string(k / 16) + " p" + std::to_string(k % 16) + " x" + std::to_string(n);
    std::printf("result: %s at %s; commands %zu (%zu rejected%s)\n",
                m.over ? (won ? "VICTORY for you" : "DEFEAT") : "no winner", clock(m.over ? m.endTick : endAt).c_str(), cmds.size(), rejected,
                why.empty() ? "" : (":" + why).c_str());

    // The slice path.
    std::vector<std::string> missing;
    for (const char* hq : {"_hq_infantry", "_hq_armor"}) {
        if (!unpacked.count(nations[0] + hq)) missing.push_back("deploy " + nations[0] + hq);
    }
    for (const char* r : {"_infantry", "_infantry_at", "_tank_light", "_tank_medium"}) {
        if (!recruited.count("regiment_" + nations[0] + r)) missing.push_back("recruit regiment_" + nations[0] + r);
    }
    std::size_t cities = 0;
    for (std::size_t i = 0; i < sim.world().structures.size(); ++i) {
        const DefRef* r = sim.world().defRefs.get(sim.world().structures.ids()[i]);
        cities += r != nullptr && isCity(defs, r->index);
    }
    if (citiesTaken.size() < std::min<std::size_t>(cities, 2)) missing.push_back("take both cities (" + std::to_string(citiesTaken.size()) + ")");
    if (!enemyOut) missing.push_back("eliminate the enemy");
    if (!won) missing.push_back("win");
    if (missing.empty()) {
        std::printf("SLICE PASSED: both division HQs, the four regiment types, %zu cities, the enemy eliminated, victory\n", citiesTaken.size());
    } else {
        std::string all;
        for (const std::string& s : missing) all += (all.empty() ? "" : "; ") + s;
        std::printf("SLICE FAILED: %s\n", all.c_str());
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
    return missing.empty() ? 0 : 1;
}

}  // namespace simcli
