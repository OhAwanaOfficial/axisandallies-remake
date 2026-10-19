#include "sim_commands.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>

#include "core/ascii.hpp"
#include "core/sim.hpp"
#include "data/def_compiler.hpp"
#include "data/tgi/pipeline.hpp"
#include "data/tgi/value_parse.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"
#include "vfs/path.hpp"
#include "vfs/vfs.hpp"

namespace simcli {

using namespace corps;

namespace {

constexpr int kOk = 0;
constexpr int kFail = 1;
constexpr int kUsage = 2;

std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

bool readFile(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(vfs::pathFromUtf8(path), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

bool writeFile(const std::string& path, const std::vector<std::uint8_t>& data) {
    std::ofstream out(vfs::pathFromUtf8(path), std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

std::uint64_t parseU64(const std::string& s, std::uint64_t fallback) {
    if (s.empty()) return fallback;
    const auto v = tgi::parseInt(s);
    return v && *v >= 0 ? static_cast<std::uint64_t>(*v) : fallback;
}

// Game data: mount, load TGI, compile defs. Kept alive for the whole command.
struct GameData {
    vfs::Vfs vfs;
    std::unique_ptr<tgi::TgiDatabase> db;
    DefSet defs;
};

std::unique_ptr<GameData> loadGame(const SimArgs& a) {
    if (!a.gameDir) {
        std::fprintf(stderr, "no game install: pass --game DIR or create config/game_path.cfg\n");
        return nullptr;
    }
    auto g = std::make_unique<GameData>();
    std::vector<std::string> log;
    if (!vfs::mountGameInstall(g->vfs, *a.gameDir, {}, log)) {
        std::fprintf(stderr, "error: nothing could be mounted from %s\n", vfs::pathToUtf8(*a.gameDir).c_str());
        return nullptr;
    }
    g->db = tgi::loadTgiDatabase(g->vfs);
    tgi::Diagnostics diags;
    g->defs = compileDefs(*g->db, diags);
    const DefSet& d = g->defs;
    std::fprintf(stderr, "defs: %zu units, %zu regiments, %zu buildings, %zu abilities, %zu nations, %zu svars; "
                 "data hash %s\n", d.units.size(), d.regiments.size(), d.buildings.size(), d.abilities.size(),
                 d.nations.size(), d.svars.vars.size(), hex(d.dataHash).c_str());
    std::fprintf(stderr, "defs: Things by category:");
    for (const auto& [cat, n] : d.stats.categories) std::fprintf(stderr, " %s %zu", cat.empty() ? "(none)" : cat.c_str(), n);
    std::fprintf(stderr, "\n");
    if (d.stats.missingSlotUnits != 0 || d.stats.badValues != 0) {
        std::fprintf(stderr, "defs: %zu regiment slots name no unit, %zu bad values\n", d.stats.missingSlotUnits, d.stats.badValues);
        int shown = 0;
        for (const auto& diag : diags.all()) {
            if (diag.code == tgi::DiagCode::BadValue && shown++ < 8) std::fprintf(stderr, "  %s\n", tgi::formatDiagnostic(diag).c_str());
        }
    }
    return g;
}

void printDivergence(const char* what, const Divergence& d) {
    if (d.stage >= 0) {
        std::printf("%s: FIRST DIVERGENCE at tick %u, stage %s (%s vs %s)\n", what, d.tick,
                    stageName(static_cast<Stage>(d.stage)), hex(d.a).c_str(), hex(d.b).c_str());
    } else {
        std::printf("%s: FIRST DIVERGENCE at tick %u (%s vs %s)\n", what, d.tick, hex(d.a).c_str(), hex(d.b).c_str());
    }
}

bool loadReplayFile(const std::string& path, Replay& rep) {
    std::vector<std::uint8_t> bytes;
    if (!readFile(path, bytes)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return false;
    }
    std::string err;
    if (!decodeReplay(bytes, rep, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return false;
    }
    return true;
}

// Data for a replay: none when it was recorded without data, else the install (hash must match).
bool dataForReplay(const SimArgs& a, const Replay& rep, std::unique_ptr<GameData>& game, const DefSet*& defs) {
    defs = nullptr;
    if (rep.header.dataHash == 0) return true;
    game = loadGame(a);
    if (!game) return false;
    if (game->defs.dataHash != rep.header.dataHash) {
        std::fprintf(stderr, "data mismatch: replay recorded with data %s, this install compiles to %s\n",
                     hex(rep.header.dataHash).c_str(), hex(game->defs.dataHash).c_str());
        return false;
    }
    defs = &game->defs;
    return true;
}

}  // namespace

std::string SimArgs::get(const std::string& name, const std::string& fallback) const {
    const auto it = options.find(name);
    return it == options.end() ? fallback : it->second;
}

// simcli record --out F.rpl [--ticks N] [--seed S] [--command-seed C] [--commands-per-tick K]
//                [--spawn REGIMENT_IDS [--at X,Y]]   (with --spawn: uses the game data)
int cmdRecord(const SimArgs& a) {
    const std::string out = a.get("out");
    if (out.empty()) {
        std::fprintf(stderr, "usage: simcli record --out F.rpl [--ticks N] [--seed S] [--command-seed C] "
                     "[--commands-per-tick K] [--spawn REGIMENT_IDS [--at X,Y]]\n");
        return kUsage;
    }
    const auto ticks = static_cast<std::uint32_t>(parseU64(a.get("ticks"), 300));
    const std::uint64_t seed = parseU64(a.get("seed"), 1);
    std::unique_ptr<GameData> game;
    std::vector<Command> extra;
    if (a.has("spawn")) {
        game = loadGame(a);
        if (!game) return kFail;
        const auto p = tgi::parsePoint(a.get("at", "0,0"), 2);
        extra.push_back(makeSpawnRegiment(0, 0, 0, asciiLowerCopy(a.get("spawn")), {p.v[0], p.v[1]}, Angle{}));
    }
    const Replay rep = recordSynthetic(seed, parseU64(a.get("command-seed"), seed + 1), ticks,
                                       static_cast<std::uint32_t>(parseU64(a.get("commands-per-tick"), 2)),
                                       game ? &game->defs : nullptr, extra);
    if (!writeFile(out, encodeReplay(rep))) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return kFail;
    }
    std::printf("recorded %s: %u ticks, %zu commands, final hash %s (sim %s)\n", out.c_str(), rep.header.tickCount,
                rep.commands.size(), hex(rep.hashes.back().hash).c_str(), rep.header.simVersion.c_str());
    return kOk;
}

// simcli play F.rpl   - replays and verifies the recorded hashes
int cmdPlay(const SimArgs& a) {
    if (a.positional.empty()) {
        std::fprintf(stderr, "usage: simcli play F.rpl\n");
        return kUsage;
    }
    Replay rep;
    if (!loadReplayFile(a.positional[0], rep)) return kFail;
    std::unique_ptr<GameData> game;
    const DefSet* defs = nullptr;
    if (!dataForReplay(a, rep, game, defs)) return kFail;
    if (rep.header.simVersion != simVersion()) {
        std::fprintf(stderr, "note: recorded with sim %s, this is sim %s\n", rep.header.simVersion.c_str(), simVersion());
    }
    const RunResult run = runReplay(rep, defs);
    const Divergence d = divergenceFromRecorded(rep, run);
    if (d.found) {
        printDivergence("replay vs recording", d);
        return kFail;
    }
    std::printf("played %u ticks: %zu recorded hashes match, final hash %s\n", rep.header.tickCount, rep.hashes.size(),
                hex(run.finalHash).c_str());
    return kOk;
}

// simcli determinism-check F.rpl [--every-tick] [--per-stage]
//   Runs the replay twice from fresh Sims and compares per-tick hashes with each other and with the
//   hashes stored in the file (which may come from another build or OS).
int cmdDeterminismCheck(const SimArgs& a) {
    if (a.positional.empty()) {
        std::fprintf(stderr, "usage: simcli determinism-check F.rpl [--every-tick] [--per-stage]\n");
        return kUsage;
    }
    Replay rep;
    if (!loadReplayFile(a.positional[0], rep)) return kFail;
    std::unique_ptr<GameData> game;
    const DefSet* defs = nullptr;
    if (!dataForReplay(a, rep, game, defs)) return kFail;
    RunOptions opt;
    opt.everyTick = a.has("every-tick") || a.has("per-stage");
    opt.perStage = a.has("per-stage");
    const RunResult first = runReplay(rep, defs, opt);
    const RunResult second = runReplay(rep, defs, opt);
    int rc = kOk;
    const Divergence d = firstDivergence(first, second);
    if (d.found) {
        printDivergence("run 1 vs run 2", d);
        rc = kFail;
    }
    const Divergence r = divergenceFromRecorded(rep, first);
    if (r.found) {
        printDivergence("run vs recording", r);
        rc = kFail;
    }
    if (rc == kOk) {
        std::printf("deterministic: %u ticks x 2 runs, %zu hashes compared%s, %zu recorded hashes match, final %s\n",
                    rep.header.tickCount, first.hashes.size(), opt.perStage ? " (+ every stage)" : "",
                    rep.hashes.size(), hex(first.finalHash).c_str());
    }
    return rc;
}

// simcli spawn-test [--regiment IDS] [--at X,Y] [--facing DEG] [--ticks N] [--seed S] [--out F.rpl]
int cmdSpawnTest(const SimArgs& a) {
    const auto game = loadGame(a);
    if (!game) return kFail;
    const std::string regIds = asciiLowerCopy(a.get("regiment", "regiment_per_infantry"));
    const RegimentDef* reg = game->defs.regiment(regIds);
    if (reg == nullptr) {
        std::fprintf(stderr, "no regiment '%s' in the data\n", regIds.c_str());
        return kFail;
    }
    const auto at = tgi::parsePoint(a.get("at", "100,50"), 2);
    const auto facing = tgi::parseFixed(a.get("facing", "0"));
    const auto ticks = static_cast<std::uint32_t>(parseU64(a.get("ticks"), 100));
    const std::uint64_t seed = parseU64(a.get("seed"), 1);

    Replay rep;
    rep.header.simVersion = simVersion();
    rep.header.dataHash = game->defs.dataHash;
    rep.header.matchSeed = seed;
    rep.header.tickCount = ticks;
    rep.header.players = {"P1"};
    rep.commands.push_back(makeSpawnRegiment(0, 0, 0, regIds, {at.v[0], at.v[1]}, Angle::fromDegrees(facing.value)));

    // Run once by hand to print what was spawned, then through the replay runner for the hashes.
    Sim sim({seed, &game->defs});
    CommandQueue q;
    for (const Command& c : rep.commands) q.schedule(c, 0);
    for (std::uint32_t t = 0; t < ticks; ++t) {
        sim.step(q.take(t));
        if (t == 0) {
            std::printf("tick 0: spawned %s at (%s, %s)\n", regIds.c_str(), formatFixed(at.v[0]).c_str(), formatFixed(at.v[1]).c_str());
            for (const SimEvent& e : sim.events()) {
                if (e.type == SimEvent::Type::CommandRejected) std::printf("  command REJECTED\n");
            }
            const World& w = sim.world();
            for (std::size_t i = 0; i < w.defRefs.size(); ++i) {
                const EntityId id = w.defRefs.ids()[i];
                const DefRef& ref = w.defRefs.data()[i];
                const Transform* tr = w.transforms.get(id);
                if (ref.kind == DefRef::Kind::Regiment) {
                    std::printf("  entity %u  regiment %s (%zu slots)\n", id.index(), game->defs.regiments[ref.index].ids.c_str(),
                                game->defs.regiments[ref.index].slotIds.size());
                } else {
                    const std::string& elIds =
                        ref.kind == DefRef::Kind::Unit ? game->defs.units[ref.index].ids : game->defs.buildings[ref.index].ids;
                    const Health* h = w.health.get(id);
                    const Member* m = w.members.get(id);
                    std::printf("  entity %u  %-8s %-18s slot %u  health %s  pos (%s, %s)\n", id.index(),
                                ref.kind == DefRef::Kind::Unit ? "unit" : "building", elIds.c_str(),
                                m != nullptr ? m->slot : 0, h != nullptr ? formatFixed(h->current).c_str() : "-",
                                tr != nullptr ? formatFixed(tr->position.x).c_str() : "-",
                                tr != nullptr ? formatFixed(tr->position.y).c_str() : "-");
                }
            }
        }
    }
    const std::uint64_t direct = stateHash(sim);
    const RunResult run = runReplay(rep, &game->defs);
    for (const TickHash& h : run.hashes) rep.hashes.push_back(h);
    std::printf("ran %u ticks (seed %llu): %zu entities alive\n", ticks, static_cast<unsigned long long>(seed),
                sim.world().registry.aliveCount());
    for (const TickHash& h : run.hashes) {
        if (h.tick % 50 == 0 || h.tick + 1 == ticks) std::printf("  hash after tick %3u: %s\n", h.tick, hex(h.hash).c_str());
    }
    std::printf("STATE HASH %s\n", hex(direct).c_str());
    if (direct != run.finalHash) {
        std::printf("ERROR: replay runner disagrees (%s)\n", hex(run.finalHash).c_str());
        return kFail;
    }
    if (a.has("out")) {
        if (!writeFile(a.get("out"), encodeReplay(rep))) {
            std::fprintf(stderr, "cannot write %s\n", a.get("out").c_str());
            return kFail;
        }
        std::printf("wrote %s\n", a.get("out").c_str());
    }
    return kOk;
}

// simcli show-def IDS   - the compiled (typed) definition: unit, regiment, building or ability
int cmdShowDef(const SimArgs& a) {
    if (a.positional.empty()) {
        std::fprintf(stderr, "usage: simcli show-def IDS\n");
        return kUsage;
    }
    const auto game = loadGame(a);
    if (!game) return kFail;
    const DefSet& d = game->defs;
    const std::string& ids = a.positional[0];
    const auto res = [](const char* what, const ResourceAmounts& r) {
        std::printf("  %-10s money %s oil %s ammo %s unitLimit +%s/-%s\n", what, formatFixed(r.money).c_str(), formatFixed(r.oil).c_str(),
                    formatFixed(r.ammo).c_str(), formatFixed(r.unitLimitProvided).c_str(), formatFixed(r.unitLimitConsumed).c_str());
    };
    const auto ability = [&](std::uint32_t i) {
        if (i == kNoDef) return;
        const AbilityDef& ab = d.abilities[i];
        std::printf("  ability %s: type %s range %s recharge %s area %s targets", ab.ids.c_str(), ab.abilityType.c_str(),
                    formatFixed(ab.range).c_str(), formatFixed(ab.rechargeTime).c_str(), formatFixed(ab.area).c_str());
        for (const auto& t : ab.targets) std::printf(" %s", t.c_str());
        std::printf("\n");
        for (const EffectDef& e : ab.effects) {
            std::printf("    effect %s %s amount %s\n", e.type.c_str(), e.damageType.c_str(), formatFixed(e.amount).c_str());
        }
    };
    bool found = false;
    if (const UnitDef* u = d.unit(ids)) {
        found = true;
        std::printf("UnitDef %s (template %s)\n  health %s defense %s speed %s accel %s turn radius %s detection %s\n",
                    u->ids.c_str(), u->templateName.c_str(), formatFixed(u->health).c_str(), formatFixed(u->defense).c_str(),
                    formatFixed(u->speedMax).c_str(), formatFixed(u->acceleration).c_str(), formatFixed(u->turnRadius).c_str(),
                    formatFixed(u->detectionRange).c_str());
        for (const std::uint32_t i : u->abilities) ability(i);
        std::printf("  properties:");
        for (const auto& p : u->properties) std::printf(" %s", p.c_str());
        std::printf("\n");
        res("cost", u->cost);
    }
    if (const RegimentDef* g = d.regiment(ids)) {
        found = true;
        std::printf("RegimentDef %s (template %s) layout %s\n  control %s operational %s morale breakpoint %s recruit %s s\n",
                    g->ids.c_str(), g->templateName.c_str(), g->layoutIds.c_str(), formatFixed(g->controlRange).c_str(),
                    formatFixed(g->operationalRange).c_str(), formatFixed(g->moraleBreakpoint).c_str(), formatFixed(g->recruitTime).c_str());
        for (std::size_t s = 0; s < g->slotIds.size(); ++s) {
            const ElementKind k = g->slots[s].kind;
            std::printf("  slot %zu: %s%s\n", s, g->slotIds[s].c_str(),
                        k == ElementKind::Unit ? "" : (k == ElementKind::Building ? "  (building)" : (g->slotIds[s].empty() ? "(empty)" : "  (UNDEFINED)")));
        }
        res("cost", g->cost);
        res("upkeep", g->upkeep);
    }
    if (const BuildingDef* b = d.building(ids)) {
        found = true;
        std::printf("BuildingDef %s (template %s) health %s defense %s\n", b->ids.c_str(), b->templateName.c_str(),
                    formatFixed(b->health).c_str(), formatFixed(b->defense).c_str());
        for (const auto& r : b->recruits) std::printf("  recruits %s\n", r.c_str());
        for (const std::uint32_t i : b->abilities) ability(i);
        res("cost", b->cost);
        res("production", b->production);
    }
    if (const AbilityDef* ab = d.ability(ids)) {
        found = true;
        ability(static_cast<std::uint32_t>(ab - d.abilities.data()));
    }
    if (const NationDef* n = d.nation(ids)) {
        found = true;
        std::printf("NationDef %s name %s factions:", n->ids.c_str(), n->name.c_str());
        for (const auto& f : n->factions) std::printf(" %s", f.c_str());
        std::printf("\n");
        for (const auto& [p, t] : n->placeholders) std::printf("  %s -> %s\n", p.c_str(), t.c_str());
    }
    if (const SVar* v = d.svars.find(ids)) {
        found = true;
        std::printf("SVar %s = %s (fixed %s)\n", v->name.c_str(), v->text.c_str(), formatFixed(v->fixedValue).c_str());
    }
    if (!found) {
        std::fprintf(stderr, "no compiled def named '%s'\n", ids.c_str());
        return kFail;
    }
    return kOk;
}

}  // namespace simcli
