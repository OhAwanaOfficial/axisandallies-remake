// simcli economy (M6): one kingdom's base on a flat arena, run for some minutes with a script of
// recruits and research; prints a ledger every minute (money, net income, oil / ammo balance and
// shortfall, unit and building limits) and the bases events. Trucks that come out are unpacked at
// the first free spot around the Corps HQ. Records the run as a replay so `simcli
// determinism-check` can verify it.
//
//   simcli economy [NATION] [--minutes M] [--recruit IDS@S,...] [--research PROP@S,...]
//                  [--size N] [--seed S] [--out F.rpl] [--game DIR]
//
// NATION: gbr (default), ger, usa, rus, jpn, per, sau; its Corps HQ is <nation>_corps. IDS@S: the
// regiment (or bare truck) recruited S seconds in, at the first deployed building that recruits it.
// PROP@S: the property researched S seconds in, at the first building that offers it.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>

#include "core/ascii.hpp"
#include "core/sim.hpp"
#include "data/def_compiler.hpp"
#include "data/tgi/pipeline.hpp"
#include "data/tgi/value_parse.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"
#include "sim_commands.hpp"
#include "systems/bases.hpp"
#include "systems/economy.hpp"
#include "vfs/path.hpp"
#include "vfs/vfs.hpp"

namespace simcli {

using namespace corps;
using namespace corps::fixed_literals;

namespace {

struct Step {
    std::string ids;
    std::uint32_t tick = 0;
    bool research = false;
    bool done = false;
};

// "a@10,b@60" -> steps (seconds -> ticks).
bool parseSteps(const std::string& s, bool research, std::vector<Step>& out) {
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(pos, end - pos);
        const std::size_t at = item.find('@');
        Step st;
        st.research = research;
        st.ids = asciiLowerCopy(item.substr(0, at));
        if (at != std::string::npos) {
            const tgi::FixedParse p = tgi::parseFixed(item.substr(at + 1));
            if (!p.ok || p.value < Fixed{}) return false;
            st.tick = static_cast<std::uint32_t>((p.value * static_cast<std::int32_t>(kTicksPerSecond)).floorInt());
        }
        if (st.ids.empty()) return false;
        out.push_back(st);
        pos = end + 1;
    }
    return true;
}

std::string money(Fixed v) {
    char b[32];
    const std::int64_t hundredths = (v * 100).roundInt();
    std::snprintf(b, sizeof b, "%lld.%02lld", static_cast<long long>(hundredths / 100), static_cast<long long>(std::llabs(hundredths % 100)));
    return b;
}

std::string whole(Fixed v) {
    return std::to_string(v.roundInt());
}

const char* stateName(Structure::State s) {
    switch (s) {
    case Structure::State::Deployed: return "deployed";
    case Structure::State::Unpacking: return "unpacking";
    case Structure::State::Packing: return "packing";
    }
    return "?";
}

}  // namespace

int cmdEconomy(const SimArgs& a) {
    constexpr int kSkipped = 77;
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

    const std::string nation = asciiLowerCopy(a.positional.empty() ? "gbr" : a.positional[0]);
    const std::string corpsIds = nation + "_corps";
    if (defs.building(corpsIds) == nullptr) {
        std::fprintf(stderr, "no building '%s'\n", corpsIds.c_str());
        return 1;
    }
    std::vector<Step> script;
    if (!parseSteps(a.get("recruit"), false, script) || !parseSteps(a.get("research"), true, script)) {
        std::fprintf(stderr, "bad --recruit / --research (IDS@SECONDS,...)\n");
        return 2;
    }
    const tgi::FixedParse minutesArg = tgi::parseFixed(a.get("minutes", "5"));
    const std::uint32_t ticks = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(((minutesArg.ok ? minutesArg.value : 5_fx) * Fixed::fromInt(60 * kTicksPerSecond)).floorInt(), 1, 1000000));
    const auto sizeOpt = tgi::parseInt(a.get("size", "192"));
    const auto size = static_cast<std::uint32_t>(std::clamp<std::int64_t>(sizeOpt ? *sizeOpt : 192, 64, 1024));
    const auto seedOpt = tgi::parseInt(a.get("seed", "1"));
    const std::uint64_t seed = seedOpt && *seedOpt >= 0 ? static_cast<std::uint64_t>(*seedOpt) : 1;
    const nav::MapDef map = nav::generateArenaMap("open", size);

    Sim sim({seed, &defs, &map});
    std::vector<Command> cmds;
    std::uint16_t seq = 0;
    const Fixed mid = Fixed::fromInt(static_cast<std::int32_t>(size)) / 2_fx;
    const auto issue = [&](Command c) {
        c.tick = sim.tick();
        c.seq = seq++;
        cmds.push_back(c);
        return c;
    };
    std::vector<Command> now = {issue(makeStartPlayer(0, 0, 0, corpsIds, {mid, mid}, Angle{}))};
    // Free spots for unpacking: a ring of 18-unit steps around the Corps HQ, nearest first.
    std::vector<FVec2> spots;
    for (std::int32_t ring = 1; ring <= 4; ++ring) {
        for (std::int32_t dy = -ring; dy <= ring; ++dy) {
            for (std::int32_t dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != ring) continue;
                spots.push_back({mid + Fixed::fromInt(dx * 18), mid + Fixed::fromInt(dy * 18)});
            }
        }
    }
    std::printf("economy: %s on a %u arena, %u ticks (%s min), seed %llu\n", corpsIds.c_str(), size, ticks,
                money(Fixed::fromInt(static_cast<std::int32_t>(ticks)) / Fixed::fromInt(60 * kTicksPerSecond)).c_str(),
                static_cast<unsigned long long>(seed));
    std::printf("%6s %10s %9s %8s %8s %9s %7s %9s  %s\n", "time", "money", "net/min", "oil", "ammo", "shortfall", "units",
                "buildings", "events");
    std::string events;
    std::size_t rejected = 0;
    for (std::uint32_t t = 0; t < ticks; ++t) {
        // Script steps due now: recruit at the first deployed building that lists it; research
        // at the first building that offers it.
        for (Step& st : script) {
            if (st.done || st.tick > t) continue;
            const World& w = sim.world();
            for (std::size_t i = 0; i < w.structures.size() && !st.done; ++i) {
                const EntityId b = w.structures.ids()[i];
                const Structure& s = w.structures.data()[i];
                const BuildingDef& bd = defs.buildings[w.defRefs.get(b)->index];
                if (s.state != Structure::State::Deployed) continue;
                bool offers = false;
                if (st.research) {
                    for (const ResearchItemDef& r : bd.research) offers = offers || r.propertyIds == st.ids;
                } else {
                    for (const RecruitRef& r : bd.recruitRefs) offers = offers || r.ids == st.ids;
                }
                if (!offers) continue;
                now.push_back(issue(st.research ? makeResearch(0, 0, 0, b, st.ids) : makeRecruit(0, 0, 0, b, st.ids)));
                st.done = true;
            }
        }
        sim.step(now);
        now.clear();
        for (const SimEvent& e : sim.events()) {
            const World& w = sim.world();
            const auto defOf = [&](EntityId id) -> std::string {
                const DefRef* r = w.defRefs.get(id);
                if (r == nullptr) return "?";
                if (r->kind == DefRef::Kind::Regiment) return defs.regiments[r->index].ids;
                if (r->kind == DefRef::Kind::Building) return defs.buildings[r->index].ids;
                return defs.units[r->index].ids;
            };
            switch (e.type) {
            case SimEvent::Type::Recruited: {
                events += " recruited:" + defOf(e.entity);
                // A truck regiment: unpack it at the first free spot.
                for (std::size_t i = 0; i < w.members.size(); ++i) {
                    if (w.members.data()[i].organization != e.entity) continue;
                    const DefRef* r = w.defRefs.get(w.members.ids()[i]);
                    if (r == nullptr || r->kind != DefRef::Kind::Unit || defs.units[r->index].unpackTo == kNoDef) continue;
                    for (const FVec2 spot : spots) {
                        if (!placeBuilding(sim, defs.units[r->index].unpackTo, spot, Angle{}, 0).ok) continue;
                        bool taken = false;  // by a truck already on its way
                        for (const Deploy& d : w.deploys.data()) taken = taken || distance(d.at, spot) < 12_fx;
                        if (taken) continue;
                        now.push_back(issue(makeUnpack(0, 0, 0, e.entity, spot, Angle{})));
                        break;
                    }
                    break;
                }
                break;
            }
            case SimEvent::Type::Unpacked: events += " unpacked:" + defOf(e.entity); break;
            case SimEvent::Type::ResearchDone: events += " researched:" + defs.properties[e.detail].ids; break;
            case SimEvent::Type::Bankrupt: events += " BANKRUPT"; break;
            case SimEvent::Type::Solvent: events += " solvent"; break;
            case SimEvent::Type::DeployFailed: events += " deploy-failed:" + defOf(e.entity); break;
            case SimEvent::Type::CommandRejected:
                ++rejected;
                events += " rejected:" + std::to_string(static_cast<int>(e.command));
                break;
            default: break;
            }
        }
        if ((t + 1) % (60 * kTicksPerSecond) == 0 || t + 1 == ticks) {
            const Ledger l = ledgerOf(sim, 0);
            const Kingdom& k = sim.world().kingdoms[0];
            std::printf("%3u:%02u %10s %9s %8s %8s %9s %3s/%-3s %4s/%-4s %s\n", (t + 1) / 600, ((t + 1) / 10) % 60, money(k.money).c_str(),
                        money(l.moneyNet()).c_str(), money(l.oilNet()).c_str(), money(l.ammoNet()).c_str(), money(l.shortfallCost).c_str(),
                        whole(l.upkeep.unitLimitConsumed).c_str(), whole(l.unitLimit).c_str(), whole(l.upkeep.buildingLimitConsumed).c_str(),
                        whole(l.buildingLimit).c_str(), events.empty() ? "" : events.c_str() + 1);
            events.clear();
        }
    }
    const World& w = sim.world();
    std::printf("buildings:");
    for (std::size_t i = 0; i < w.structures.size(); ++i) {
        const EntityId b = w.structures.ids()[i];
        std::printf(" %s (%s)", defs.buildings[w.defRefs.get(b)->index].ids.c_str(), stateName(w.structures.data()[i].state));
    }
    std::printf("\nregiments: %zu, rejected commands: %zu\n", w.regiments.size(), rejected);
    for (const Step& st : script) {
        if (!st.done) std::printf("never issued (no building offered it): %s\n", st.ids.c_str());
    }

    const std::uint64_t direct = stateHash(sim);
    Replay rep;
    rep.header.simVersion = simVersion();
    rep.header.dataHash = defs.dataHash;
    rep.header.mapSpec = map.name;
    rep.header.mapHash = map.mapHash;
    rep.header.matchSeed = seed;
    rep.header.tickCount = ticks;
    rep.header.players = {"A"};
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
