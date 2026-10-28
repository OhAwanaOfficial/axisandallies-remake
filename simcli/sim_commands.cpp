#include "sim_commands.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "core/ascii.hpp"
#include "core/sim.hpp"
#include "data/def_compiler.hpp"
#include "data/tgi/pipeline.hpp"
#include "data/tgi/value_parse.hpp"
#include "session/bytes.hpp"
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

// Map for a replay: regenerated for "gen:" specs, else --map F.json; the hash must match.
bool mapForReplay(const SimArgs& a, const Replay& rep, std::optional<nav::MapDef>& map) {
    std::optional<nav::MapDef> supplied;
    if (!mapFromArgs(a, supplied)) return false;
    std::string err;
    if (!resolveReplayMap(rep.header, supplied ? &*supplied : nullptr, map, err)) {
        std::fprintf(stderr, "map: %s\n", err.c_str());
        return false;
    }
    if (map) std::fprintf(stderr, "map: %s (%ux%u cells)\n", map->name.c_str(), map->width, map->height);
    return true;
}

}  // namespace

std::string SimArgs::get(const std::string& name, const std::string& fallback) const {
    const auto it = options.find(name);
    return it == options.end() ? fallback : it->second;
}

// simcli record --out F.rpl [--ticks N] [--seed S] [--command-seed C] [--commands-per-tick K]
//                [--spawn REGIMENT_IDS [--at X,Y]]   (with --spawn: uses the game data)
//                [--map F.json | --gen SEED [--size N]]   (adds blockers and path requests)
int cmdRecord(const SimArgs& a) {
    const std::string out = a.get("out");
    if (out.empty()) {
        std::fprintf(stderr, "usage: simcli record --out F.rpl [--ticks N] [--seed S] [--command-seed C] "
                     "[--commands-per-tick K] [--spawn REGIMENT_IDS [--at X,Y]] [--map F.json | --gen SEED [--size N]]\n");
        return kUsage;
    }
    std::optional<nav::MapDef> map;
    if (!mapFromArgs(a, map)) return kFail;
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
                                       game ? &game->defs : nullptr, extra, map ? &*map : nullptr);
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
    std::optional<nav::MapDef> map;
    if (!mapForReplay(a, rep, map)) return kFail;
    if (rep.header.simVersion != simVersion()) {
        std::fprintf(stderr, "note: recorded with sim %s, this is sim %s\n", rep.header.simVersion.c_str(), simVersion());
    }
    RunOptions opt;
    opt.map = map ? &*map : nullptr;
    const RunResult run = runReplay(rep, defs, opt);
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
    std::optional<nav::MapDef> map;
    if (!mapForReplay(a, rep, map)) return kFail;
    RunOptions opt;
    opt.map = map ? &*map : nullptr;
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

// simcli show-def IDS   - the compiled (typed) definition: unit, regiment, building, ability,
//                         property, terrain, projectile, damage type, ...
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
        if (ab.maxTargets != 0 || ab.attenuationRangeMin != Fixed::fromInt(1) || ab.attenuationFactorMax != Fixed::fromInt(1) ||
            !ab.projectileIds.empty()) {
            std::printf("    max targets %u attenuation range min %s factor max %s projectile %s%s\n", ab.maxTargets,
                        formatFixed(ab.attenuationRangeMin).c_str(), formatFixed(ab.attenuationFactorMax).c_str(),
                        ab.projectileIds.empty() ? "-" : ab.projectileIds.c_str(),
                        !ab.projectileIds.empty() && ab.projectileIndex == kNoDef ? " (UNDEFINED)" : "");
        }
        for (const EffectDef& e : ab.effects) {
            std::printf("    effect %s %s amount %s\n", e.type.c_str(), e.damageType.c_str(), formatFixed(e.amount).c_str());
        }
    };
    const auto combat = [&](const std::vector<WeaponDef>& weapons, const std::vector<TurretDef>& turrets, bool checkResist) {
        for (const TurretDef& t : turrets) {
            std::printf("  turret %s: yaw %s deg/s range %s start %s reset %s s%s\n", t.ids.c_str(), formatFixed(t.yawRate).c_str(),
                        formatFixed(t.yawRange).c_str(), formatFixed(t.yawStart).c_str(), formatFixed(t.resetTime).c_str(),
                        t.canReset ? "" : " (no reset)");
        }
        for (const WeaponDef& w : weapons) {
            std::printf("  weapon %s: priority %s fuzzy %s reload %s %s\n", w.ability != kNoDef ? d.abilities[w.ability].ids.c_str() : "?",
                        formatFixed(w.priority).c_str(), formatFixed(w.fuzzyRange).c_str(), formatFixed(w.reloadTime).c_str(),
                        w.turret != WeaponDef::kNoTurret ? ("turret " + turrets[w.turret].ids).c_str() : "local");
        }
        if (checkResist) std::printf("  check_damage_resistance\n");
    };
    bool found = false;
    if (const UnitDef* u = d.unit(ids)) {
        found = true;
        static const char* kClass[] = {"foot", "wheeled", "tracked", "naval", "amphibious", "air"};
        static const char* kTurn[] = {"instant", "in_place", "while_moving"};
        std::printf("UnitDef %s (template %s)\n  health %s defense %s speed %s accel %s turn radius %s (%s) class %s radius %s "
                    "detection %s\n",
                    u->ids.c_str(), u->templateName.c_str(), formatFixed(u->health).c_str(), formatFixed(u->defense).c_str(),
                    formatFixed(u->speedMax).c_str(), formatFixed(u->acceleration).c_str(), formatFixed(u->turnRadius).c_str(),
                    kTurn[static_cast<int>(u->turnType)], kClass[static_cast<int>(u->moveClass)], formatFixed(u->blockingRadius).c_str(),
                    formatFixed(u->detectionRange).c_str());
        for (const std::uint32_t i : u->abilities) ability(i);
        combat(u->weapons, u->turrets, u->checkDamageResistance);
        std::printf("  morale value %s\n", formatFixed(u->moraleValue).c_str());
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
        for (std::size_t k = 0; k < g->experienceLevels.size(); ++k) {
            const ExperienceLevelDef& l = g->experienceLevels[k];
            std::printf("  level %zu at %s XP: AV +%s DV +%s health x%s morale +%s recovery x%s\n", k, formatFixed(l.required).c_str(),
                        formatFixed(l.attack).c_str(), formatFixed(l.defense).c_str(), formatFixed(l.healthMultiplier).c_str(),
                        formatFixed(l.morale).c_str(), formatFixed(l.moraleRecovery).c_str());
        }
    }
    if (const BuildingDef* b = d.building(ids)) {
        found = true;
        std::printf("BuildingDef %s (template %s) health %s defense %s\n", b->ids.c_str(), b->templateName.c_str(),
                    formatFixed(b->health).c_str(), formatFixed(b->defense).c_str());
        for (const RecruitRef& r : b->recruitRefs) std::printf("  recruits %s (regiment %s)\n", r.ids.c_str(), d.regiments[r.regiment].ids.c_str());
        for (const std::uint32_t i : b->abilities) ability(i);
        combat(b->weapons, b->turrets, b->checkDamageResistance);
        if (b->controlRange > Fixed{}) std::printf("  control range %s\n", formatFixed(b->controlRange).c_str());
        res("cost", b->cost);
        res("upkeep", b->upkeep);
        res("production", b->production);
        static const char* kShapes[] = {"square", "rect", "circle"};
        std::printf("  footprint %s %s x %s (square %s), padding %s%s%s%s\n", kShapes[static_cast<int>(b->shape)],
                    formatFixed(b->rectWidth).c_str(), formatFixed(b->rectHeight).c_str(), formatFixed(b->squareSize).c_str(),
                    formatFixed(b->padding).c_str(), b->tileLocked ? " tile_locked" : "", b->axisLocked ? " axis_locked" : "",
                    b->providesCorps ? " corps" : "");
        if (b->mobile) {
            std::printf("  packs in %s s (health >= %s), unpacks in %s s; truck %s, packed regiment %s, unpacked regiment %s\n",
                        formatFixed(b->packingLength).c_str(), formatFixed(b->packHealthMin).c_str(), formatFixed(b->unpackingLength).c_str(),
                        b->truck != kNoDef ? d.units[b->truck].ids.c_str() : "-",
                        b->packedRegiment != kNoDef ? d.regiments[b->packedRegiment].ids.c_str() : "-",
                        b->unpackedRegiment != kNoDef ? d.regiments[b->unpackedRegiment].ids.c_str() : "-");
        }
        std::printf("  recruits appear at (%s, %s), form up at (%s, %s)\n", formatFixed(b->creationOffset.x).c_str(),
                    formatFixed(b->creationOffset.y).c_str(), formatFixed(b->deploymentOffset.x).c_str(),
                    formatFixed(b->deploymentOffset.y).c_str());
        static const char* kScopes[] = {"GLOBAL", "LOCAL", "SUPPORT"};
        for (const ResearchItemDef& r : b->research) {
            std::printf("  research %s%s %s %s s, %s money\n", r.propertyIds.c_str(), r.property == kNoDef ? " (UNDEFINED)" : "",
                        kScopes[static_cast<int>(r.scope)], formatFixed(r.time).c_str(), formatFixed(r.cost.money).c_str());
        }
    }
    if (const AbilityDef* ab = d.ability(ids)) {
        found = true;
        ability(static_cast<std::uint32_t>(ab - d.abilities.data()));
    }
    const auto vec = [](FVec2 v) { return "(" + formatFixed(v.x) + ", " + formatFixed(v.y) + ")"; };
    if (const std::uint32_t li = d.layoutIndex(ids); li != kNoDef) {
        found = true;
        const LayoutDef& l = d.layouts[li];
        std::printf("LayoutDef %s default %s combat %s retreat %s (uses %s), %zu elements\n", l.ids.c_str(), l.defaultFormationIds.c_str(),
                    l.combatFormationIds.c_str(), l.retreatFormationIds.c_str(),
                    l.retreatFormation != kNoDef ? l.formations[l.retreatFormation].formationIds.c_str() : "-", l.elementSlot.size());
        for (std::size_t s = 0; s < l.slots.size(); ++s) {
            const LayoutSlotDef& sd = l.slots[s];
            std::printf("  slot %zu: %s x%u%s%s%s\n", s, sd.property.c_str(), sd.number, sd.primary ? " primary" : "",
                        sd.required ? " required" : "", sd.locked ? " locked" : "");
        }
        for (const LayoutFormationDef& f : l.formations) {
            std::printf("  formation %s%s half width %s:", f.formationIds.c_str(), f.formationIndex == kNoDef ? " (UNDEFINED)" : "",
                        formatFixed(f.halfWidth).c_str());
            for (const FVec2 o : f.offsets) std::printf(" %s", vec(o).c_str());
            std::printf("\n");
        }
    }
    if (const std::uint32_t fi = d.formationIndex(ids); fi != kNoDef) {
        found = true;
        const FormationDef& f = d.formations[fi];
        std::printf("FormationDef %s organized %d aggressive %d herding %d speed x%s attack x%s group %s%s\n", f.ids.c_str(),
                    f.organized, f.aggressive, f.herding, formatFixed(f.speedMultiplier).c_str(), formatFixed(f.attackMultiplier).c_str(),
                    f.groupFormationIds.c_str(), f.groupFormationIndex == kNoDef && !f.groupFormationIds.empty() ? " (UNDEFINED)" : "");
    }
    if (const std::uint32_t gi = d.groupFormationIndex(ids); gi != kNoDef) {
        found = true;
        std::printf("GroupFormationDef %s:", d.groupFormations[gi].ids.c_str());
        for (const FVec2 o : d.groupFormations[gi].offsets) std::printf(" %s", vec(o).c_str());
        std::printf("\n");
    }
    if (const NationDef* n = d.nation(ids)) {
        found = true;
        std::printf("NationDef %s name %s factions:", n->ids.c_str(), n->name.c_str());
        for (const auto& f : n->factions) std::printf(" %s", f.c_str());
        std::printf("\n");
        for (const auto& [p, t] : n->placeholders) std::printf("  %s -> %s\n", p.c_str(), t.c_str());
    }
    if (const std::uint32_t pi = d.propertyIndex(ids); pi != kNoDef) {
        found = true;
        const PropertyDef& p = d.properties[pi];
        std::printf("PropertyDef %s DV %s", p.ids.c_str(), formatFixed(p.defenseAdd).c_str());
        for (const auto& [t, amount] : p.resist) std::printf(" resist %s %s", t.c_str(), formatFixed(amount).c_str());
        for (const auto& t : p.targetFlags) std::printf(" target-type %s", t.c_str());
        for (const auto& r : p.requiredProperties) std::printf(" requires %s", r.c_str());
        for (const ModifierDef& m : p.modifiers) {
            std::printf(" %s %s %s", m.attribute.c_str(), m.op == ModifierDef::Op::Add ? "+" : "x(1+)", formatFixed(m.amount).c_str());
        }
        std::printf("\n");
    }
    if (const std::uint32_t ti = d.terrainOfName(ids); ti != kNoDef) {
        found = true;
        const TerrainDef& t = d.terrains[ti];
        std::printf("TerrainDef %s group %s properties:", t.ids.c_str(), t.group.c_str());
        for (const auto& p : t.properties) std::printf(" %s", p.c_str());
        std::printf(" (%zu variations)\n", t.variations.size());
    }
    if (const std::uint32_t pj = d.projectileIndex(ids); pj != kNoDef) {
        found = true;
        static const char* kType[] = {"antigrav", "lob", "bomb"};
        const ProjectileDef& p = d.projectiles[pj];
        std::printf("ProjectileDef %s speed %s type %s life %s\n", p.ids.c_str(), formatFixed(p.speed).c_str(),
                    kType[static_cast<int>(p.type)], formatFixed(p.life).c_str());
    }
    if (const std::uint32_t di = d.damageTypeIndex(ids); di != kNoDef) {
        found = true;
        std::printf("DamageTypeDef %s filter %d\n", d.damageTypes[di].ids.c_str(), d.damageTypes[di].damageFilter);
    }
    if (const std::uint32_t tt = d.targetTypeIndex(ids); tt != kNoDef) {
        found = true;
        std::printf("TargetTypeDef %s\n", d.targetTypes[tt].ids.c_str());
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

// --- M8 desync tools (NETCODE §5) ---

// simcli diff a.state b.state: the first differing item (entity and field) of two state dumps.
int cmdDiff(const SimArgs& a) {
    if (a.positional.size() != 2) {
        std::fprintf(stderr, "usage: simcli diff a.state b.state\n");
        return kUsage;
    }
    std::vector<std::uint8_t> sa;
    std::vector<std::uint8_t> sb;
    if (!readFile(a.positional[0], sa) || !readFile(a.positional[1], sb)) {
        std::fprintf(stderr, "cannot read %s or %s\n", a.positional[0].c_str(), a.positional[1].c_str());
        return kFail;
    }
    const StateDiff d = diffStates(sa, sb);
    if (d.equal) {
        std::printf("identical states (%zu bytes, hash %s)\n", sa.size(), hex(hashBytes(sa)).c_str());
        return kOk;
    }
    if (d.malformed) {
        std::printf("states differ and at least one doesn't parse (%zu vs %zu bytes)\n", sa.size(), sb.size());
        return kFail;
    }
    std::printf("FIRST DIFFERENCE: %s\n  a: %s\n  b: %s\n%zu item(s) differ in total\n", d.where.c_str(), d.a.c_str(), d.b.c_str(),
                d.differingItems);
    return kFail;
}

namespace {

// "tick stage hash" lines, one per stage of every tick.
bool writeStageHashes(const std::string& path, const RunResult& run) {
    std::ofstream out(vfs::pathFromUtf8(path), std::ios::binary);
    for (std::size_t t = 0; t < run.stageHashes.size(); ++t) {
        for (std::size_t s = 0; s < kStageCount; ++s) out << t << ' ' << s << ' ' << hex(run.stageHashes[t][s]) << '\n';
    }
    return static_cast<bool>(out);
}

bool readStageHashes(const std::string& path, RunResult& run) {
    std::ifstream in(vfs::pathFromUtf8(path), std::ios::binary);
    if (!in) return false;
    std::size_t t = 0;
    std::size_t s = 0;
    std::string h;
    while (in >> t >> s >> h) {
        if (s >= kStageCount) return false;
        if (run.stageHashes.size() <= t) run.stageHashes.resize(t + 1);
        run.stageHashes[t][s] = std::stoull(h, nullptr, 16);
    }
    return true;
}

}  // namespace

// simcli bisect R.rpl [--map F.json] [--write-stages F.txt] [--against F.txt] [--perturb TICK:STAGE]
// Re-runs a replay hashing every stage of every tick. Finds the first divergent tick and stage
// against the replay's recorded hashes (the recording machine's; a 10-tick window), a second local
// run, and --against: another machine's --write-stages output. --perturb (debug) draws from the sim
// RNG after that tick's stage, to stand in for a nondeterministic machine.
int cmdBisect(const SimArgs& a) {
    if (a.positional.empty()) {
        std::fprintf(stderr, "usage: simcli bisect R.rpl [--map F.json] [--write-stages F] [--against F] [--perturb TICK:STAGE]\n");
        return kUsage;
    }
    Replay rep;
    if (!loadReplayFile(a.positional[0], rep)) return kFail;
    std::unique_ptr<GameData> game;
    const DefSet* defs = nullptr;
    if (!dataForReplay(a, rep, game, defs)) return kFail;
    std::optional<nav::MapDef> map;
    if (!mapForReplay(a, rep, map)) return kFail;
    RunOptions opt;
    opt.map = map ? &*map : nullptr;
    opt.everyTick = true;
    opt.perStage = true;
    if (a.has("perturb")) {
        const std::string p = a.get("perturb");
        const std::size_t colon = p.find(':');
        const auto tick = static_cast<std::uint32_t>(parseU64(p.substr(0, colon), 0));
        int stage = static_cast<int>(Stage::Cleanup);
        if (colon != std::string::npos) {
            const std::string name = asciiLowerCopy(p.substr(colon + 1));
            for (std::size_t s = 0; s < kStageCount; ++s) {
                if (asciiLowerCopy(stageName(static_cast<Stage>(s))) == name) stage = static_cast<int>(s);
            }
        }
        opt.afterStage = [tick, stage](Sim& sim, Stage s) {
            if (sim.tick() == tick && static_cast<int>(s) == stage) sim.rng().nextU32();
        };
        std::fprintf(stderr, "debug: perturbing after tick %u stage %s\n", tick, stageName(static_cast<Stage>(stage)));
    }
    const RunResult run = runReplay(rep, defs, opt);
    std::printf("%u ticks re-run, %zu stage hashes, final %s\n", rep.header.tickCount, run.stageHashes.size() * kStageCount,
                hex(run.finalHash).c_str());
    if (a.has("write-stages")) {
        if (!writeStageHashes(a.get("write-stages"), run)) {
            std::fprintf(stderr, "cannot write %s\n", a.get("write-stages").c_str());
            return kFail;
        }
        std::printf("stage hashes written to %s\n", a.get("write-stages").c_str());
    }
    int rc = kOk;
    const Divergence rec = divergenceFromRecorded(rep, run);
    if (rec.found) {
        printDivergence("run vs recording", rec);
        std::printf("  (recorded hashes are every %u ticks: the divergence began after tick %u)\n", rep.header.hashInterval,
                    rec.tick >= rep.header.hashInterval ? rec.tick - rep.header.hashInterval : 0);
        rc = kFail;
    }
    if (a.has("against")) {
        RunResult other;
        if (!readStageHashes(a.get("against"), other)) {
            std::fprintf(stderr, "cannot read %s\n", a.get("against").c_str());
            return kFail;
        }
        RunResult mine;  // stage hashes only: the other file has no per-tick list
        mine.stageHashes = run.stageHashes;
        const Divergence d = firstDivergence(mine, other);
        if (d.found) {
            printDivergence("this run vs --against", d);
            rc = kFail;
        } else {
            std::printf("this run matches --against stage by stage\n");
        }
    } else {
        RunOptions again = opt;
        again.afterStage = nullptr;
        const RunResult second = runReplay(rep, defs, again);
        const Divergence d = firstDivergence(run, second);
        if (d.found) {
            printDivergence("run 1 vs run 2", d);
            rc = kFail;
        }
    }
    if (rc == kOk) std::printf("no divergence\n");
    return rc;
}

}  // namespace simcli
