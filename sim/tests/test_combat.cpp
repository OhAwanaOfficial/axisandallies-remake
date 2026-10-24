// M5: combat. Part 1: damage formula and tuning, resistances from properties, terrain and
// entrenchment, target filters, ZOC auto-engage and the engagement states, Attack / AttackMove,
// turrets, projectiles, area damage with max_targets and attenuation, target priority, deaths,
// buildings, commands and determinism. Part 2: morale (losses, gains, breaking point, rout,
// exhaustion, rally), experience levels and army XP, combat efficiency blending, the entrenchment
// timer, ordered Retreat, and a committed combat replay fixture.
//
// Uses a small hand-built DefSet (no game data needed) shaped like the original's records.

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>

#include "combat/rules.hpp"
#include "combat/tuning.hpp"
#include "core/sim.hpp"
#include "data/defs.hpp"
#include "nav/nav_world.hpp"
#include "session/bytes.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"
#include "systems/morale.hpp"
#include "test_env.hpp"
#include "vfs/path.hpp"

using namespace corps;
using namespace corps::fixed_literals;

namespace {

AbilityDef attack(std::string ids, const char* type, Fixed amount, Fixed range, Fixed recharge, Fixed area = {},
                  std::vector<std::string> targets = {"+enemy", "-air", "-mine", "-water"}) {
    AbilityDef a;
    a.ids = std::move(ids);
    a.abilityType = "attack";
    a.range = range;
    a.rechargeTime = recharge;
    a.area = area;
    a.targets = std::move(targets);
    EffectDef e;
    e.type = "MAIN_DAMAGE";
    e.damageType = type;
    e.amount = amount;
    a.effects.push_back(e);
    return a;
}

UnitDef unit(std::string ids, UnitMoveClass cls, Fixed health, Fixed defense, std::vector<std::string> props, Fixed radius = 0.35_fx,
             bool still = false) {
    UnitDef u;
    u.ids = std::move(ids);
    u.moveClass = cls;
    u.speedMax = still ? 0_fx : cls == UnitMoveClass::Foot ? 2.5_fx : 3.5_fx;
    u.acceleration = cls == UnitMoveClass::Foot ? 0_fx : 3_fx;
    u.turnRadius = cls == UnitMoveClass::Foot ? 0_fx : 0.9_fx;
    u.turnType = cls == UnitMoveClass::Foot ? TurnType::Instant : TurnType::InPlace;
    u.blockingRadius = radius;
    u.health = health;
    u.defense = defense;
    u.properties = std::move(props);
    return u;
}

PropertyDef property(std::string ids, std::vector<std::string> flags, std::vector<std::pair<std::string, Fixed>> resist = {},
                     Fixed dv = {}, std::vector<std::string> requires_ = {}) {
    PropertyDef p;
    p.ids = std::move(ids);
    p.targetFlags = std::move(flags);
    p.resist = std::move(resist);
    p.defenseAdd = dv;
    p.requiredProperties = std::move(requires_);
    return p;
}

LayoutFormationDef layoutFormation(std::string ids, std::vector<FVec2> offsets) {
    LayoutFormationDef f;
    f.formationIds = std::move(ids);
    f.offsets = std::move(offsets);
    for (const FVec2 o : f.offsets) f.halfWidth = max(f.halfWidth, abs(o.y));
    return f;
}

// Units: rifle (gunfire 10, range 12, recharge 3, local gun), tank (AP 70 shell, range 20, turret
// 36°/s), mg (gunfire 14, area 1.5, at most 2 targets, attenuation 0.5 / 0.5), unarmed, flak
// (+enemy +air: can't hit ground), and two immobile targets: post (infantry, defense 0) and hulk
// (tank, defense 7). Building: bunker (gunfire 20, range 15, turret, ZOC 20). Regiments of four
// (layout lay: front x3 + captain): reg_rifle, reg_tank, reg_mg, reg_unarmed, reg_flak, reg_post,
// reg_hulk; of one (layout solo): reg_solo_tank, reg_solo_post. Terrain "woods" gives
// terrain_forest_infantry.
DefSet makeDefs() {
    DefSet d;
    d.damageTypes = {{"armor_piercing", 2}, {"explosive", 3}, {"gunfire", 0}};
    d.targetTypes = {{"air"}, {"building"}, {"infantry"}, {"routing"}, {"tank"}, {"vehicle"}};
    d.properties = {property("building", {"building"}),
                    property("infantry", {"infantry"}, {{"armor_piercing", 0.2_fx}}),
                    property("infantry_entrench", {}, {{"armor_piercing", 0.4_fx}, {"explosive", 0.4_fx}}, 4_fx, {"infantry"}),
                    property("routing", {"routing"}),
                    property("tank", {"tank"}, {{"gunfire", 0.2_fx}, {"explosive", 0.2_fx}}),
                    property("terrain_forest_infantry", {}, {{"armor_piercing", 0.2_fx}}, 4_fx, {"infantry"}),
                    property("vehicle", {"vehicle"}, {{"gunfire", 0.4_fx}, {"explosive", 0.2_fx}})};
    TerrainDef woods;
    woods.ids = "woods";
    woods.properties = {"terrain_forest_infantry"};
    woods.variations = {"woods01"};
    d.terrains = {woods};
    ProjectileDef shell;
    shell.ids = "shell";
    shell.speed = 50_fx;
    d.projectiles = {shell};

    AbilityDef mg = attack("mg#0", "gunfire", 14_fx, 12_fx, 2_fx, 1.5_fx);
    mg.maxTargets = 2;
    mg.attenuationRangeMin = 0.5_fx;
    mg.attenuationFactorMax = 0.5_fx;
    AbilityDef tank = attack("tank#0", "armor_piercing", 70_fx, 20_fx, 6_fx);
    tank.projectileIds = "shell";
    d.abilities = {attack("bunker#0", "gunfire", 20_fx, 15_fx, 2_fx), attack("flak#0", "gunfire", 30_fx, 15_fx, 2_fx, {}, {"+enemy", "+air"}),
                   mg, attack("rifle#0", "gunfire", 10_fx, 12_fx, 3_fx), tank};  // sorted by IDS
    for (AbilityDef& a : d.abilities) a.projectileIndex = a.projectileIds.empty() ? kNoDef : d.projectileIndex(a.projectileIds);

    const auto weapon = [&](const char* ids, std::uint32_t turret = WeaponDef::kNoTurret, Fixed fuzzy = 3_fx) {
        WeaponDef w;
        w.ability = static_cast<std::uint32_t>(d.ability(ids) - d.abilities.data());
        w.turret = turret;
        w.fuzzyRange = fuzzy;
        return w;
    };
    TurretDef turret;
    turret.ids = "turret";
    turret.yawRate = 36_fx;
    turret.yawRange = 360_fx;
    turret.resetTime = 10_fx;
    UnitDef flak = unit("flak", UnitMoveClass::Foot, 200_fx, 1_fx, {"infantry"});
    flak.abilities = {static_cast<std::uint32_t>(d.ability("flak#0") - d.abilities.data())};
    flak.weapons = {weapon("flak#0")};
    UnitDef mgu = unit("mg", UnitMoveClass::Foot, 150_fx, 1_fx, {"infantry"});
    mgu.weapons = {weapon("mg#0")};
    UnitDef rifle = unit("rifle", UnitMoveClass::Foot, 200_fx, 1_fx, {"infantry"});
    rifle.weapons = {weapon("rifle#0", WeaponDef::kNoTurret, 0_fx)};
    UnitDef tk = unit("tank", UnitMoveClass::Tracked, 400_fx, 7_fx, {"vehicle", "tank"}, 0.8_fx);
    tk.turrets = {turret};
    tk.weapons = {weapon("tank#0", 0)};
    tk.checkDamageResistance = true;
    UnitDef unarmed = unit("unarmed", UnitMoveClass::Foot, 100_fx, 0_fx, {"infantry"});
    UnitDef post = unit("post", UnitMoveClass::Foot, 100_fx, 0_fx, {"infantry"}, 0.35_fx, true);
    UnitDef hulk = unit("hulk", UnitMoveClass::Tracked, 400_fx, 7_fx, {"vehicle", "tank"}, 0.8_fx, true);
    d.units = {flak, hulk, mgu, post, rifle, tk, unarmed};  // sorted
    for (UnitDef& u : d.units) {
        for (const WeaponDef& w : u.weapons) u.abilities.push_back(w.ability);
    }
    BuildingDef bunker;
    bunker.ids = "bunker";
    bunker.health = 1000_fx;
    bunker.defense = 10_fx;
    bunker.blockingRadius = 1.5_fx;
    bunker.controlRange = 20_fx;
    bunker.properties = {"building"};
    TurretDef bt = turret;
    bt.yawRate = 90_fx;
    bunker.turrets = {bt};
    bunker.weapons = {weapon("bunker#0", 0, 0_fx)};
    bunker.abilities = {bunker.weapons[0].ability};
    d.buildings = {bunker};

    FormationDef agg, col, def;
    agg.ids = "regiment_aggressive";
    agg.aggressive = true;
    col.ids = "regiment_column";
    col.attackMultiplier = 0.5_fx;
    def.ids = "regiment_defensive";
    def.aggressive = true;
    def.attackMultiplier = 0.75_fx;
    d.formations = {agg, col, def};
    LayoutDef lay;
    lay.ids = "lay";
    lay.defaultFormationIds = "regiment_aggressive";
    lay.slots = {{"front", 3, false, true, true}, {"captain", 1, true, true, false}};
    lay.elementSlot = {0, 0, 0, 1};
    lay.formations = {layoutFormation("regiment_aggressive", {{1_fx, 3_fx}, {1_fx, 0_fx}, {1_fx, -3_fx}, {-1_fx, 0_fx}}),
                      layoutFormation("regiment_column", {{3_fx, 0_fx}, {1_fx, 0_fx}, {-1_fx, 0_fx}, {-3_fx, 0_fx}}),
                      layoutFormation("regiment_defensive", {{1_fx, 3_fx}, {1_fx, 0_fx}, {1_fx, -3_fx}, {-1_fx, 0_fx}})};
    LayoutDef solo;
    solo.ids = "solo";
    solo.defaultFormationIds = "regiment_aggressive";
    solo.slots = {{"front", 1, true, true, true}};
    solo.elementSlot = {0};
    solo.formations = {layoutFormation("regiment_aggressive", {{0_fx, 0_fx}}), layoutFormation("regiment_column", {{0_fx, 0_fx}}),
                       layoutFormation("regiment_defensive", {{0_fx, 0_fx}})};
    d.layouts = {lay, solo};
    for (LayoutDef& l : d.layouts) {
        for (LayoutFormationDef& f : l.formations) f.formationIndex = d.formationIndex(f.formationIds);
    }
    const auto regiment = [&](std::string ids, const char* u, const char* layout = "lay") {
        RegimentDef g;
        g.ids = std::move(ids);
        g.layoutIds = layout;
        g.layoutIndex = d.layoutIndex(layout);
        g.controlRange = 18_fx;
        g.slotIds = std::string(layout) == "solo" ? std::vector<std::string>{u} : std::vector<std::string>{u, u};
        for (const std::string& s : g.slotIds) g.slots.push_back({ElementKind::Unit, d.unitIndex(s)});
        return g;
    };
    d.regiments = {regiment("reg_flak", "flak"),      regiment("reg_hulk", "hulk"),
                   regiment("reg_mg", "mg"),          regiment("reg_post", "post"),
                   regiment("reg_rifle", "rifle"),    regiment("reg_solo_post", "post", "solo"),
                   regiment("reg_solo_tank", "tank", "solo"), regiment("reg_tank", "tank"),
                   regiment("reg_unarmed", "unarmed")};
    // Part 2: morale values, the entrenchment property's MORALE_LOSS_RATE (× 1.5 as in the data),
    // and reg_brittle: rifles that break at 15 of 20 morale, entrench after 30 s, and have the
    // manual's 0 / 100 / 250 / 500 levels.
    for (UnitDef& u : d.units) u.moraleValue = u.moveClass == UnitMoveClass::Tracked ? 20_fx : 5_fx;
    d.properties[d.propertyIndex("infantry_entrench")].modifiers = {{"MORALE_LOSS_RATE", ModifierDef::Op::Multiply, 0.5_fx}};
    d.layouts[d.layoutIndex("lay")].retreatFormation = d.layouts[d.layoutIndex("lay")].formation("regiment_column");
    RegimentDef brittle = regiment("reg_brittle", "rifle");
    brittle.moraleBreakpoint = 15_fx;
    brittle.entrenchmentTime = 30_fx;
    const auto level = [](Fixed req, Fixed av, Fixed dv, Fixed health, Fixed morale, Fixed recovery) {
        ExperienceLevelDef l;
        l.required = req;
        l.attack = av;
        l.defense = dv;
        l.healthMultiplier = health;
        l.morale = morale;
        l.moraleRecovery = recovery;
        return l;
    };
    brittle.experienceLevels = {level(0_fx, 0_fx, 0_fx, 1_fx, 0_fx, 1_fx), level(100_fx, 4_fx, 1_fx, 1.05_fx, 0_fx, 1.05_fx),
                                level(250_fx, 8_fx, 2_fx, 1.1_fx, 0.5_fx, 1.1_fx), level(500_fx, 16_fx, 3_fx, 1.2_fx, 1_fx, 1.15_fx)};
    d.regiments.insert(d.regiments.begin(), brittle);  // "reg_brittle" sorts first
    d.dataHash = hashBytes(serializeDefs(d));
    return d;
}

const DefSet& defs() {
    static const DefSet d = makeDefs();
    return d;
}

nav::MapDef openMap(std::uint32_t size = 96, const char* terrain = "open") {
    nav::MapDef m;
    m.name = "combat-test";
    m.width = m.height = size;
    m.kind.assign(m.cellCount(), nav::TerrainKind::Ground);
    m.terrainId.assign(m.cellCount(), 0);
    m.terrainNames = {terrain};
    REQUIRE(m.finalize());
    return m;
}

struct Run {
    Sim sim;
    std::uint16_t seqs[2] = {0, 0};
    std::vector<SimEvent> log;  // every event, with the tick in `command`'s place unused
    std::vector<std::uint32_t> logTick;
    Run(const nav::MapDef& m, const CombatTuning* t = nullptr) : sim({5, &defs(), &m, t}) {}

    std::uint16_t seq(std::uint8_t p) { return seqs[p]++; }
    void step(std::vector<Command> cmds = {}) {
        for (Command& c : cmds) c.tick = sim.tick();
        sim.step(cmds);
        for (const SimEvent& e : sim.events()) {
            log.push_back(e);
            logTick.push_back(sim.tick() - 1);
        }
    }
    void steps(std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) step();
    }
    EntityId spawn(const char* ids, FVec2 at, Angle facing, std::uint8_t player) {
        step({makeSpawnRegiment(0, player, seq(player), ids, at, facing)});
        for (const SimEvent& e : sim.events()) {
            if (e.type == SimEvent::Type::Spawned && sim.world().regiments.has(e.entity)) return e.entity;
        }
        FAIL("no regiment");
        return {};
    }
    EntityId building(const char* ids, FVec2 at, std::uint8_t player) {
        step({makeDebugSpawnBuilding(0, player, seq(player), ids, at, Angle{})});
        for (const SimEvent& e : sim.events()) {
            if (e.type == SimEvent::Type::Spawned) return e.entity;
        }
        FAIL("no building");
        return {};
    }
    std::vector<EntityId> units(EntityId reg) const {
        std::vector<EntityId> out;
        for (std::size_t i = 0; i < sim.world().members.size(); ++i) {
            if (sim.world().members.data()[i].organization == reg) out.push_back(sim.world().members.ids()[i]);
        }
        return out;
    }
    const Engagement& eng(EntityId reg) const { return *sim.world().engagements.get(reg); }
    const Order& order(EntityId reg) const { return *sim.world().orders.get(reg); }
    Fixed hp(EntityId id) const {
        const Health* h = sim.world().registry.alive(id) ? sim.world().health.get(id) : nullptr;
        return h != nullptr ? h->current : Fixed{};
    }
    std::size_t count(SimEvent::Type t) const {
        return static_cast<std::size_t>(std::count_if(log.begin(), log.end(), [&](const SimEvent& e) { return e.type == t; }));
    }
    // Tick of the first event of type t from `entity` (any entity if null), or UINT32_MAX.
    std::uint32_t first(SimEvent::Type t, EntityId entity = {}) const {
        for (std::size_t i = 0; i < log.size(); ++i) {
            if (log[i].type == t && (entity.isNull() || log[i].entity == entity)) return logTick[i];
        }
        return UINT32_MAX;
    }
    // Teleports units in one tick (tests place immobile targets exactly).
    void place(const std::vector<std::pair<EntityId, FVec2>>& where, std::uint8_t player) {
        std::vector<Command> cmds;
        for (const auto& [id, at] : where) cmds.push_back(makeDebugSetTransform(0, player, seq(player), id, at, Angle{}));
        step(cmds);
    }
};

const Angle kEast{};
const Angle kWest = Angle::fromUnits(Angle::kHalf);
const Angle kNorth = Angle::fromUnits(Angle::kQuarter);

}  // namespace

// ------------------------------------------------------------------------------------- formula

TEST_CASE("damage formula: resistances, defense, minimum damage, attack multiplier; every constant tunable") {
    const nav::MapDef m = openMap();
    const CombatRules rules(&defs(), &m, CombatTuning{});
    const auto& d = defs();
    const std::uint32_t gun = d.damageTypeIndex("gunfire"), ap = d.damageTypeIndex("armor_piercing");
    const TargetStats inf = rules.statsOf(*rules.unit(d.unitIndex("rifle")), 0, false);
    const TargetStats tank = rules.statsOf(*rules.unit(d.unitIndex("tank")), 0, false);
    CHECK(inf.defense == 1_fx);
    CHECK(inf.resist[ap] == 0.2_fx);
    CHECK(tank.defense == 7_fx);
    CHECK(tank.resist[gun] == 0.4_fx + 0.2_fx);  // vehicle + tank add up
    // gunfire 10 on infantry: 10 × 10 / (10 + 1)
    CHECK(rules.damage({gun, 10_fx}, inf, 1_fx) == 10_fx * 10_fx / 11_fx);
    // AP 70 on infantry: 70 × 0.8 × 10 / 11; on the tank: 70 × 10 / 17
    CHECK(rules.damage({ap, 70_fx}, inf, 1_fx) == 70_fx * (1_fx - 0.2_fx) * 10_fx / 11_fx);
    CHECK(rules.damage({ap, 70_fx}, tank, 1_fx) == 70_fx * 10_fx / 17_fx);
    // Small arms on a tank: 10 × 0.4 × 10 / 17 = 2.35; a tiny hit still does minDamage.
    CHECK(rules.damage({gun, 10_fx}, tank, 1_fx) == 10_fx * (1_fx - (0.4_fx + 0.2_fx)) * 10_fx / 17_fx);
    CHECK(rules.damage({gun, 1_fx}, tank, 1_fx) == 1_fx);
    // The attack multiplier scales after the minimum; zero multiplier, zero damage.
    CHECK(rules.damage({gun, 10_fx}, inf, 0.5_fx) == 10_fx * 10_fx / 11_fx * 0.5_fx);
    CHECK(rules.damage({gun, 10_fx}, inf, 0_fx) == 0_fx);

    CombatTuning t;
    std::string err;
    REQUIRE(parseCombatTuning(t, "defenseK=20, defenseD=2,resistMax=0.5", err));
    CHECK(t.defenseK == 20_fx);
    CHECK(formatCombatTuning(t) == "defenseK=20,defenseD=2,resistMax=0.5");
    const CombatRules tuned(&defs(), &m, t);
    CHECK(tuned.damage({gun, 10_fx}, inf, 1_fx) == 10_fx * 20_fx / (20_fx + 2_fx));
    CHECK(tuned.damage({gun, 10_fx}, tank, 1_fx) == 10_fx * (1_fx - 0.5_fx) * 20_fx / (20_fx + 14_fx));  // resist clamped to 0.5
    CHECK_FALSE(parseCombatTuning(t, "nonsense=1", err));
    CHECK_FALSE(parseCombatTuning(t, "defenseK", err));
    CHECK(combatTuningHash(t) != combatTuningHash(CombatTuning{}));
    // Every field is in the table (so --tune, the hash and the docs see it).
    CHECK(combatTuningFields().size() == 16);
}

TEST_CASE("terrain and entrenchment properties apply only to units that have their required properties") {
    const nav::MapDef woods = openMap(64, "woods01");
    const CombatRules rules(&defs(), &woods, CombatTuning{});
    const auto& d = defs();
    const std::uint32_t ap = d.damageTypeIndex("armor_piercing"), ex = d.damageTypeIndex("explosive");
    const TargetStats inf = rules.statsOf(*rules.unit(d.unitIndex("rifle")), 0, false);
    CHECK(inf.defense == 5_fx);              // 1 + forest DV 4
    CHECK(inf.resist[ap] == 0.4_fx);         // infantry 0.2 + forest 0.2
    const TargetStats dug = rules.statsOf(*rules.unit(d.unitIndex("rifle")), 0, true);
    CHECK(dug.defense == 9_fx);
    CHECK(dug.resist[ap] == 0.2_fx + 0.2_fx + 0.4_fx);
    CHECK(dug.resist[ex] == 0.4_fx);
    const TargetStats tank = rules.statsOf(*rules.unit(d.unitIndex("tank")), 0, true);
    CHECK(tank.defense == 7_fx);             // neither forest nor infantry_entrench applies
    CHECK(rules.statsOf(*rules.unit(d.unitIndex("rifle")), 0xFFFFFFFFu, false).defense == 1_fx);  // no terrain
}

TEST_CASE("target filters: +enemy -air hits ground enemies; +enemy +air only air; friends never") {
    const nav::MapDef m = openMap();
    const CombatRules rules(&defs(), &m, CombatTuning{});
    const auto& d = defs();
    const TargetFilter ground = rules.ability(static_cast<std::uint32_t>(d.ability("rifle#0") - d.abilities.data()))->filter;
    const TargetFilter flak = rules.ability(static_cast<std::uint32_t>(d.ability("flak#0") - d.abilities.data()))->filter;
    const std::uint64_t infantry = rules.unit(d.unitIndex("rifle"))->targetTypes;
    const std::uint64_t air = std::uint64_t{1} << d.targetTypeIndex("air");
    CHECK(CombatRules::passes(ground, true, infantry));
    CHECK_FALSE(CombatRules::passes(ground, false, infantry));
    CHECK_FALSE(CombatRules::passes(ground, true, air));
    CHECK(CombatRules::passes(flak, true, air));
    CHECK_FALSE(CombatRules::passes(flak, true, infantry));
}

// ------------------------------------------------------------------------------------- engagement

TEST_CASE("aggressive regiments auto-engage in their ZOC and hold their move; Column passes by") {
    for (const bool column : {false, true}) {
        CAPTURE(column);
        const nav::MapDef m = openMap(128);
        Run r(m);
        const EntityId mine = r.spawn("reg_rifle", {20_fx, 40_fx}, kEast, 0);
        const EntityId theirs = r.spawn("reg_unarmed", {60_fx, 52_fx}, kWest, 1);
        if (column) r.step({makeSetFormation(0, 0, r.seq(0), {mine}, "regiment_column")});
        r.step({makeMoveGroup(0, 0, r.seq(0), {mine}, {110_fx, 40_fx})});
        // The move grace (5 s) passes, then the next ZOC sample sees the enemy 12 to the side.
        for (int i = 0; i < 800 && r.eng(mine).state == Engagement::State::Idle && r.order(mine).state != Order::State::Idle; ++i) r.step();
        if (column) {
            CHECK(r.eng(mine).state == Engagement::State::Idle);
            CHECK(r.order(mine).state == Order::State::Idle);  // arrived
            CHECK(r.count(SimEvent::Type::Fired) == 0);
            CHECK(r.hp(r.units(theirs)[0]) == 100_fx);
        } else {
            REQUIRE(r.eng(mine).state == Engagement::State::Engaging);
            CHECK(r.order(mine).state == Order::State::Engaged);  // the move is on hold, its goal kept
            CHECK(r.eng(mine).suspended);
            CHECK(r.eng(mine).resumeGoal == FVec2{110_fx, 40_fx});
            // It wins, reforms, and goes on to its goal.
            for (int i = 0; i < 3000 && (r.sim.world().registry.alive(theirs) || r.eng(mine).state != Engagement::State::Idle ||
                                         r.order(mine).state != Order::State::Idle);
                 ++i) {
                r.step();
            }
            CHECK_FALSE(r.sim.world().registry.alive(theirs));
            CHECK(r.order(mine).state == Order::State::Idle);
            CHECK(distance(r.sim.world().transforms.get(mine)->position, {110_fx, 40_fx}) < 2_fx);
            CHECK(r.count(SimEvent::Type::Died) == 4);
        }
    }
}

TEST_CASE("regiments that can't hurt anything don't engage; unarmed units are still targets") {
    const nav::MapDef m = openMap(128);
    Run r(m);
    const EntityId a = r.spawn("reg_unarmed", {20_fx, 40_fx}, kEast, 0);
    const EntityId flak = r.spawn("reg_flak", {40_fx, 40_fx}, kWest, 1);  // only hits air
    r.steps(120);
    CHECK(r.eng(a).state == Engagement::State::Idle);
    CHECK(r.eng(flak).state == Engagement::State::Idle);
    CHECK(r.count(SimEvent::Type::Fired) == 0);
}

TEST_CASE("Attack chases to CAITargetInRangeZOCMultiplier x ZOC, engages, and kills; deaths remove units, then the regiment") {
    const nav::MapDef m = openMap(128);
    Run r(m);
    const EntityId mine = r.spawn("reg_rifle", {10_fx, 40_fx}, kEast, 0);
    const EntityId theirs = r.spawn("reg_unarmed", {80_fx, 40_fx}, kWest, 1);
    r.step({makeAttack(0, 0, r.seq(0), {mine}, theirs)});
    CHECK(r.eng(mine).mode == Engagement::Mode::Attack);
    for (int i = 0; i < 600 && r.eng(mine).state != Engagement::State::Engaging; ++i) r.step();
    REQUIRE(r.eng(mine).state == Engagement::State::Engaging);
    const Fixed gap = distance(r.sim.world().transforms.get(mine)->position, r.sim.world().transforms.get(theirs)->position);
    CHECK(gap <= 18_fx * 0.6_fx + 0.5_fx);  // stopped at 0.6 x ZOC (SVars default)
    CHECK(gap > 18_fx * 0.6_fx - 2_fx);
    const std::vector<EntityId> targets = r.units(theirs);
    for (int i = 0; i < 1500 && r.sim.world().registry.alive(theirs); ++i) r.step();
    CHECK_FALSE(r.sim.world().registry.alive(theirs));
    CHECK(r.count(SimEvent::Type::Died) == targets.size());
    for (const EntityId u : targets) {
        CHECK_FALSE(r.sim.world().registry.alive(u));
        CHECK(r.first(SimEvent::Type::Died, u) != UINT32_MAX);
    }
    // Killed by one of ours.
    for (std::size_t i = 0; i < r.log.size(); ++i) {
        if (r.log[i].type != SimEvent::Type::Died) continue;
        const Member* killer = r.sim.world().members.get(r.log[i].other);
        REQUIRE(killer != nullptr);
        CHECK(killer->organization == mine);
    }
    r.steps(200);
    CHECK(r.eng(mine).mode == Engagement::Mode::None);  // the target is gone
    CHECK(r.eng(mine).state == Engagement::State::Idle);  // reformed
}

TEST_CASE("AttackMove engages on the way whatever the formation, then carries on") {
    const nav::MapDef m = openMap(128);
    Run r(m);
    const EntityId mine = r.spawn("reg_rifle", {10_fx, 40_fx}, kEast, 0);
    const EntityId theirs = r.spawn("reg_unarmed", {60_fx, 48_fx}, kWest, 1);
    r.step({makeSetFormation(0, 0, r.seq(0), {mine}, "regiment_column")});
    r.step({makeAttackMove(0, 0, r.seq(0), {mine}, {110_fx, 40_fx})});
    CHECK(r.eng(mine).mode == Engagement::Mode::AttackMove);
    for (int i = 0; i < 3000 && (r.order(mine).state != Order::State::Idle || r.eng(mine).state != Engagement::State::Idle); ++i) r.step();
    CHECK_FALSE(r.sim.world().registry.alive(theirs));
    CHECK(distance(r.sim.world().transforms.get(mine)->position, {110_fx, 40_fx}) < 2_fx);
    CHECK(r.eng(mine).mode == Engagement::Mode::None);
}

TEST_CASE("Move and Stop end a fight; the grace keeps a moved regiment from re-engaging at once") {
    const nav::MapDef m = openMap(128);
    Run r(m);
    const EntityId mine = r.spawn("reg_rifle", {20_fx, 40_fx}, kEast, 0);
    const EntityId theirs = r.spawn("reg_unarmed", {30_fx, 40_fx}, kWest, 1);
    r.step({makeAttack(0, 0, r.seq(0), {mine}, theirs)});
    r.steps(5);
    REQUIRE(r.eng(mine).state == Engagement::State::Engaging);
    r.step({makeMoveGroup(0, 0, r.seq(0), {mine}, {20_fx, 100_fx})});
    CHECK(r.eng(mine).state == Engagement::State::Idle);
    CHECK(r.eng(mine).mode == Engagement::Mode::None);
    r.steps(30);
    CHECK(r.eng(mine).state == Engagement::State::Idle);  // still within the 5 s grace
    CHECK(r.order(mine).state != Order::State::Idle);
}

// ------------------------------------------------------------------------------------- weapons

TEST_CASE("turrets turn at their yaw rate before firing; shells fly distance / speed") {
    const nav::MapDef m = openMap(128);
    Run r(m);
    const EntityId tank = r.spawn("reg_solo_tank", {40_fx, 40_fx}, kEast, 0);
    const EntityId post = r.spawn("reg_solo_post", {40_fx, 50_fx}, kWest, 1);  // due north, 10 away
    const std::uint32_t start = r.sim.tick();
    r.step({makeAttack(0, 0, r.seq(0), {tank}, post)});
    for (int i = 0; i < 200 && r.count(SimEvent::Type::Fired) == 0; ++i) r.step();
    REQUIRE(r.count(SimEvent::Type::Fired) == 1);
    const std::uint32_t fired = r.first(SimEvent::Type::Fired);
    // The turret starts facing east (the body's way) and turns 90° at 36°/s (655 units a tick):
    // within 3° after 25 ticks, so it fires on the 25th tick of the attack.
    CHECK(fired - start == 24);
    const SimEvent* shot = nullptr;
    for (const SimEvent& e : r.log) {
        if (e.type == SimEvent::Type::Fired) {
            shot = &e;
            break;
        }
    }
    REQUIRE(shot != nullptr);
    const Combat& cb = *r.sim.world().combats.get(shot->entity);
    const Transform& body = *r.sim.world().transforms.get(shot->entity);
    const Transform& target = *r.sim.world().transforms.get(shot->other);
    CHECK(std::abs(Angle::delta(body.facing + cb.turretYaw[0], (target.position - body.position).angle())) <= 3 * 182 + 1);
    // The body didn't turn (turreted weapon).
    CHECK(std::abs(Angle::delta(body.facing, kEast)) < 600);
    // A projectile is in flight, then hits ceil(distance / 50 × 10) ticks later.
    CHECK(r.sim.world().projectiles.size() >= 1);
    const Fixed hpBefore = r.hp(shot->other);
    const Fixed dist = distance(body.position, target.position);
    const auto flight = static_cast<std::uint32_t>((dist / 50_fx * 10_fx).ceilInt());
    const EntityId victim = shot->other;
    for (std::uint32_t i = 0; i + 1 < flight; ++i) {
        r.step();
        CHECK(r.hp(victim) == hpBefore);
    }
    r.step();
    CHECK(r.hp(victim) == hpBefore - 70_fx * (1_fx - 0.2_fx));  // AP 70 on infantry (resist 0.2), defense 0
}

TEST_CASE("area damage: nearest first, at most max_targets, attenuated past attenuation_range_min") {
    const nav::MapDef m = openMap(128);
    Run r(m);
    r.spawn("reg_mg", {30_fx, 40_fx}, kEast, 0);
    const EntityId foes = r.spawn("reg_post", {80_fx, 80_fx}, kWest, 1);
    const std::vector<EntityId> f = r.units(foes);
    REQUIRE(f.size() == 4);
    // Immobile posts in a line east of the MGs: the primary at 40, others 1.0 and 1.2 behind it.
    r.place({{f[0], {40_fx, 40_fx}}, {f[1], {41_fx, 40_fx}}, {f[2], {41.2_fx, 40_fx}}, {f[3], {80_fx, 80_fx}}}, 1);
    // The MGs (aggressive, idle) engage at their next ZOC sample and all aim at the nearest post.
    for (int i = 0; i < 120 && r.count(SimEvent::Type::Fired) == 0; ++i) r.step();
    const std::size_t shots = r.count(SimEvent::Type::Fired);
    REQUIRE(shots >= 1);
    const Fixed full = 14_fx * 10_fx / 10_fx;  // post: defense 0, no gunfire resistance
    const Fixed lost0 = 100_fx - r.hp(f[0]), lost1 = 100_fx - r.hp(f[1]), lost2 = 100_fx - r.hp(f[2]);
    CHECK(lost0 > Fixed{});
    // Each MG shot hits the primary and the nearest one other (max_targets 2), the latter at
    // 1 - (1 - 0.5) × (1 - 0.5) / (1.5 - 0.5) = 0.75.
    CHECK(lost0 == full * Fixed::fromInt(static_cast<std::int32_t>(shots)));
    CHECK(lost1 == full * 0.75_fx * Fixed::fromInt(static_cast<std::int32_t>(shots)));
    CHECK(lost2 == Fixed{});
    CHECK(r.hp(f[3]) == 100_fx);
}

TEST_CASE("target choice: nearest by default; check_damage_resistance picks the most damage; kill targets are kept") {
    const nav::MapDef m = openMap(128);
    Run r(m);
    const EntityId tanks = r.spawn("reg_tank", {30_fx, 40_fx}, kEast, 0);
    const EntityId armour = r.spawn("reg_hulk", {80_fx, 10_fx}, kWest, 1);
    const EntityId men = r.spawn("reg_post", {90_fx, 90_fx}, kWest, 1);
    const std::vector<EntityId> hulks = r.units(armour), posts = r.units(men);
    // From the first tank (31, 43): a hulk 9.5 away, a post 14.3 away. AP 70 does 41.2 to the hulk
    // (defense 7) and 56 to the post (defense 0, AP resist 0.2): check_damage_resistance picks it.
    r.place({{hulks[0], {40_fx, 40_fx}}, {posts[0], {45_fx, 40_fx}}}, 1);
    const EntityId first = r.units(tanks)[0];
    for (int i = 0; i < 120 && r.sim.world().combats.get(first)->targets[0].isNull(); ++i) r.step();
    CHECK(r.sim.world().combats.get(first)->targets[0] == posts[0]);
    // KillTargetIsHighestPriority: a nearer, better target appearing doesn't steal it.
    r.place({{posts[1], {35_fx, 43_fx}}}, 1);
    CHECK(r.sim.world().combats.get(first)->targets[0] == posts[0]);
    // A unit without check_damage_resistance takes the nearest: the rifles pick the hulk.
    Run q(m);
    const EntityId rifles = q.spawn("reg_rifle", {30_fx, 40_fx}, kEast, 0);
    const EntityId hulks2 = q.spawn("reg_hulk", {80_fx, 10_fx}, kWest, 1);
    const EntityId posts2 = q.spawn("reg_post", {90_fx, 90_fx}, kWest, 1);
    q.place({{q.units(hulks2)[0], {38_fx, 43_fx}}, {q.units(posts2)[0], {41_fx, 43_fx}}}, 1);
    const EntityId rifle = q.units(rifles)[0];
    for (int i = 0; i < 120 && q.sim.world().combats.get(rifle)->targets[0].isNull(); ++i) q.step();
    CHECK(q.sim.world().combats.get(rifle)->targets[0] == q.units(hulks2)[0]);
}

TEST_CASE("buildings defend themselves without a regiment") {
    const nav::MapDef m = openMap(128);
    Run r(m);
    const EntityId bunker = r.building("bunker", {60_fx, 40_fx}, 1);
    REQUIRE(r.sim.world().footprints.has(bunker));
    const EntityId men = r.spawn("reg_unarmed", {50_fx, 40_fx}, kEast, 0);
    const std::vector<EntityId> targets = r.units(men);
    r.steps(100);
    CHECK(r.count(SimEvent::Type::Fired) >= 4);  // turret turns 180° at 90°/s, then a shot every 2 s
    Fixed lost;
    for (const EntityId u : targets) lost = lost + (100_fx - r.hp(u));  // dead ones count as 100
    CHECK(lost == 20_fx * Fixed::fromInt(static_cast<std::int32_t>(r.count(SimEvent::Type::Fired))));  // gunfire 20, defense 0
    // Rifles attack the bunker: the regiment attacks a building.
    const EntityId rifles = r.spawn("reg_rifle", {30_fx, 40_fx}, kEast, 0);
    r.step({makeAttack(0, 0, r.seq(0), {rifles}, bunker)});
    CHECK(r.eng(rifles).mode == Engagement::Mode::Attack);
    r.steps(400);
    CHECK(r.hp(bunker) < 1000_fx);
}

TEST_CASE("combat efficiency blends: down at 0.02/s, up at 0.01/s; a settled Column hits for half") {
    const nav::MapDef m = openMap(128);
    for (const bool column : {false, true}) {
        Run r(m);
        const EntityId rifles = r.spawn("reg_rifle", {30_fx, 40_fx}, kEast, 0);
        CHECK(r.eng(rifles).efficiency == 1_fx);  // starts at its formation's
        if (column) {
            r.step({makeSetFormation(0, 0, r.seq(0), {rifles}, "regiment_column")});
            Fixed want = 1_fx;
            for (int i = 0; i < 100; ++i) want = want - 0.02_fx * 0.1_fx;  // the stage's own arithmetic
            r.steps(99);
            CHECK(r.eng(rifles).efficiency == want);  // 10 s later: 0.8
            r.steps(200);
            CHECK(r.eng(rifles).efficiency == 0.5_fx);  // settled
        }
        const EntityId foes = r.spawn("reg_unarmed", {38_fx, 40_fx}, kWest, 1);  // in range, after the blend
        const std::size_t before = r.count(SimEvent::Type::Fired);
        r.step({makeAttack(0, 0, r.seq(0), {rifles}, foes)});  // in range: it may fire this very tick
        for (int i = 0; i < 100 && r.count(SimEvent::Type::Fired) == before; ++i) r.step();
        Fixed lost;
        for (const EntityId u : r.units(foes)) lost = lost + (100_fx - r.hp(u));
        CHECK_MESSAGE(lost == Fixed::fromInt(static_cast<std::int32_t>(r.count(SimEvent::Type::Fired) - before)) * (column ? 5_fx : 10_fx),
                      "lost " << formatFixed(lost) << " shots " << (r.count(SimEvent::Type::Fired) - before) << " eff "
                              << formatFixed(r.eng(rifles).efficiency));
        if (column) {  // back to Assault: climbs at half the speed it fell
            r.step({makeSetFormation(0, 0, r.seq(0), {rifles}, "regiment_aggressive")});
            r.steps(9);
            CHECK(r.eng(rifles).efficiency == 0.5_fx + (0.01_fx * 0.1_fx) * 10_fx);
        }
    }
}

TEST_CASE("entrenched infantry takes less (DebugSetEntrenched)") {
    const nav::MapDef m = openMap(128);
    Fixed lost[2];
    for (const int dug : {0, 1}) {
        Run r(m);
        const EntityId tanks = r.spawn("reg_tank", {30_fx, 40_fx}, kEast, 0);
        const EntityId men = r.spawn("reg_rifle", {45_fx, 40_fx}, kWest, 1);
        r.step({makeSetFormation(0, 1, r.seq(1), {men}, "regiment_column"), makeDebugSetEntrenched(0, 1, r.seq(1), {men}, dug == 1)});
        CHECK(r.eng(men).entrenched == (dug == 1));
        r.step({makeAttack(0, 0, r.seq(0), {tanks}, men)});
        r.steps(80);
        for (const EntityId u : r.units(men)) lost[dug] = lost[dug] + (200_fx - r.hp(u));
    }
    CHECK(lost[0] > Fixed{});
    CHECK(lost[1] < lost[0]);
}

// ------------------------------------------------------------------------------------- determinism

TEST_CASE("combat commands round-trip through the replay codec") {
    const std::vector<Command> cmds = {makeAttack(3, 1, 2, {EntityId{5}, EntityId{9}}, EntityId{77}),
                                       makeAttackMove(4, 0, 1, {EntityId{5}}, {12.5_fx, 3_fx}),
                                       makeDebugSpawnBuilding(0, 1, 0, "bunker", {60_fx, 40_fx}, kNorth),
                                       makeDebugSetEntrenched(7, 0, 3, {EntityId{5}}, true),
                                       makeRetreat(8, 1, 4, {EntityId{5}, EntityId{6}}, {1_fx, 2.5_fx}),
                                       makeDebugSetExperience(9, 0, 5, {EntityId{5}}, 250.5_fx)};
    for (const Command& c : cmds) {
        std::vector<std::uint8_t> bytes;
        encodeCommand(c, bytes);
        std::size_t pos = 0;
        Command back;
        back.tick = c.tick;
        back.seq = c.seq;
        REQUIRE(decodeCommand(bytes, pos, back));
        CHECK(pos == bytes.size());
        CHECK(back == c);
    }
}

TEST_CASE("save/load mid-fight continues identically; a replay with tuning reproduces the fight") {
    const nav::MapDef m = openMap(128);
    CombatTuning tuning;
    tuning.defenseK = 12_fx;
    const auto script = [](Run& r) {
        const EntityId a = r.spawn("reg_rifle", {20_fx, 40_fx}, kEast, 0);
        const EntityId b = r.spawn("reg_mg", {50_fx, 42_fx}, kWest, 1);
        const EntityId c = r.spawn("reg_tank", {25_fx, 60_fx}, kEast, 0);
        const EntityId d = r.spawn("reg_brittle", {60_fx, 70_fx}, kWest, 1);  // breaks: a rout mid-film
        r.step({makeAttack(0, 0, r.seq(0), {a, c}, b), makeAttackMove(0, 1, r.seq(1), {b, d}, {10_fx, 40_fx}),
                makeDebugSetExperience(0, 1, r.seq(1), {d}, 260_fx)});
    };
    Run ref(m, &tuning);
    script(ref);
    ref.steps(400);
    const std::uint64_t want = stateHash(ref.sim);
    CHECK(ref.count(SimEvent::Type::Died) > 0);
    CHECK(ref.count(SimEvent::Type::LevelUp) > 0);

    Run a(m, &tuning);
    script(a);
    for (std::uint32_t k = 0; k < 400; ++k) {
        a.step();
        if (k % 97 != 41) continue;
        Sim b({5, &defs(), &m, &tuning});
        std::string err;
        REQUIRE_MESSAGE(loadState(b, saveState(a.sim), &err), err);
        CHECK(saveState(b) == saveState(a.sim));
        for (std::uint32_t j = k + 1; j < 400; ++j) b.step();
        CHECK(stateHash(b) == want);
        // A Sim with other tuning refuses the state.
        Sim other({5, &defs(), &m});
        CHECK_FALSE(loadState(other, saveState(a.sim), &err));
    }
    CHECK(stateHash(a.sim) == want);
}

// ------------------------------------------------------------------------------------- part 2: morale and experience

namespace {

// Steps until the regiment's next morale sample is the coming tick ((tick + index) % 50 == 0).
void untilSample(Run& r, EntityId reg) {
    while ((r.sim.tick() + reg.index()) % 50 != 0) r.step();
}

}  // namespace

TEST_CASE("morale: starts full; deaths cost morale_value x MoraleDeathFactor; kills give the slayer bonus; idle regiments replenish") {
    const nav::MapDef m = openMap(128);
    Run r(m);
    const EntityId a = r.spawn("reg_brittle", {20_fx, 40_fx}, kEast, 0);
    const EntityId b = r.spawn("reg_rifle", {100_fx, 40_fx}, kWest, 1);
    CHECK(moraleMaxOf(r.sim, a) == 20_fx);  // four riflemen, morale_value 5
    CHECK(r.eng(a).morale == 20_fx);
    Engagement& ea = *r.sim.world().engagements.get(a);
    ea.morale = 12_fx;
    // One of B's men killed by one of A's: B loses 5 x 0.25; A gains 0.5 x clamp(5 / 5, 0.5, 3).
    moraleOnKill(r.sim, r.units(b)[0], r.units(a)[0]);
    CHECK(r.eng(b).morale == 20_fx - 5_fx * 0.25_fx);
    CHECK(ea.morale == 12_fx + 0.5_fx);
    // Out of combat, every sample: + MoraleReplenish (2).
    untilSample(r, a);
    const Fixed before = ea.morale;
    r.step();
    CHECK(r.eng(a).morale == before + 2_fx);
    // Never above the ceiling.
    for (int i = 0; i < 400; ++i) r.step();
    CHECK(r.eng(a).morale == moraleMaxOf(r.sim, a));
}

TEST_CASE("morale: engaged regiments lose per sample, scaled by enemy strength (x1..x3); entrenched lose x1.5") {
    const nav::MapDef m = openMap(128);
    for (const bool dug : {false, true}) {
        CAPTURE(dug);
        Run r(m);
        const EntityId a = r.spawn("reg_brittle", {40_fx, 40_fx}, kEast, 0);
        const EntityId hulks = r.spawn("reg_hulk", {50_fx, 40_fx}, kWest, 1);  // 4 x 400 health against 4 x 200
        if (dug) r.step({makeDebugSetEntrenched(0, 0, r.seq(0), {a}, true)});
        r.step({makeAttack(0, 0, r.seq(0), {a}, hulks)});
        REQUIRE(r.eng(a).state == Engagement::State::Engaging);
        untilSample(r, a);
        const Fixed before = r.eng(a).morale;
        r.step();
        // factor = clamp(enemy health in its zone / its own, 1, 3): about 1600 / 800, measured after
        // this tick's hits (Abilities runs before Morale).
        Fixed enemy, own;
        for (const EntityId u : r.units(hulks)) enemy = enemy + r.hp(u);
        for (const EntityId u : r.units(a)) own = own + r.hp(u);
        const Fixed factor = std::clamp(enemy / own, 1_fx, 3_fx);
        CHECK(factor > 1.9_fx);
        const Fixed loss = 0.5_fx * factor;
        CHECK_MESSAGE(r.eng(a).morale == before - (dug ? loss * 1.5_fx : loss),
                      "before " << formatFixed(before) << " after " << formatFixed(r.eng(a).morale));
    }
}

TEST_CASE("rout: a fighting regiment at its breaking point runs from its attacker, ignores orders, then rallies; the attacker earns rout XP") {
    const nav::MapDef m = openMap(128);
    Run r(m);
    const EntityId a = r.spawn("reg_brittle", {64_fx, 64_fx}, kEast, 0);
    const EntityId b = r.spawn("reg_rifle", {75_fx, 64_fx}, kWest, 1);
    r.step({makeAttack(0, 0, r.seq(0), {a}, b), makeAttack(0, 1, r.seq(1), {b}, a)});
    r.steps(3);
    REQUIRE(r.eng(a).state == Engagement::State::Engaging);
    Engagement& ea = *r.sim.world().engagements.get(a);
    ea.morale = 15_fx;  // at the breaking point
    ea.lastAttacker = b;
    const Fixed xpBefore = r.eng(b).xp;
    r.step();
    REQUIRE(r.eng(a).state == Engagement::State::Routing);
    CHECK(r.first(SimEvent::Type::Routed, a) != UINT32_MAX);
    // It runs away from B (to its west), 40 units, within ±45° of straight away.
    const FVec2 from = r.sim.world().transforms.get(a)->position;
    const FVec2 to = r.order(a).goal - from;
    CHECK(distance(to, FVec2{}) > 30_fx);
    CHECK(to.x < Fixed{});
    CHECK(abs(to.y) <= abs(to.x) + 0.5_fx);
    // B earns the rout: clamp(4 x 200 x 0.01, 0, 10) x 1.
    CHECK(r.eng(b).xp >= xpBefore + 8_fx);
    CHECK(r.eng(a).mode == Engagement::Mode::None);
    // Deaf to orders while it routs; it doesn't fire.
    const std::size_t rejected = r.count(SimEvent::Type::CommandRejected);
    const std::size_t fired = r.log.size();
    std::size_t firedByA = 0;
    r.step({makeMoveGroup(0, 0, r.seq(0), {a}, {10_fx, 10_fx})});
    CHECK(r.count(SimEvent::Type::CommandRejected) == rejected + 1);
    for (int i = 0; i < 90; ++i) r.step();
    for (std::size_t i = fired; i < r.log.size(); ++i) {
        const Member* mm = r.log[i].type == SimEvent::Type::Fired ? r.sim.world().members.get(r.log[i].entity) : nullptr;
        if (mm != nullptr && mm->organization == a) ++firedByA;
    }
    CHECK(firedByA == 0);
    // After RoutingTime (10 s) it is exhausted; replenishing to the rally point it reforms, then is idle.
    for (int i = 0; i < 30 && r.eng(a).state == Engagement::State::Routing; ++i) r.step();
    if (!r.sim.world().registry.alive(a)) return;  // B may have caught it: fine, nothing more to see
    CHECK(r.eng(a).state == Engagement::State::Exhausted);
    r.step({makeStop(0, 1, r.seq(1), {b})});  // B leaves it alone
    for (int i = 0; i < 2000 && r.eng(a).state != Engagement::State::Idle && r.sim.world().registry.alive(a); ++i) r.step();
    if (r.sim.world().registry.alive(a)) {
        CHECK(r.eng(a).state == Engagement::State::Idle);
        CHECK(r.eng(a).morale >= 15_fx + (moraleMaxOf(r.sim, a) - 15_fx) * 0.5_fx);
    }
}

TEST_CASE("experience: kills earn clamp(health_max x xpPerHealth); levels scale health and add DV, AV and morale; army XP") {
    const nav::MapDef m = openMap(128);
    Run r(m);
    const EntityId a = r.spawn("reg_brittle", {20_fx, 40_fx}, kEast, 0);
    const EntityId b = r.spawn("reg_rifle", {100_fx, 40_fx}, kWest, 1);
    const Fixed army0 = r.sim.world().kingdoms[0].armyXp;
    moraleOnKill(r.sim, r.units(b)[0], r.units(a)[0]);
    CHECK(r.eng(a).xp == 200_fx * 0.01_fx);  // health_max x xpPerHealth (≈ 2)
    CHECK(r.sim.world().kingdoms[0].armyXp == army0 + 200_fx * 0.01_fx);  // x ExperienceFactionGainMultiplier 1
    CHECK(r.eng(a).level == 0);
    const Fixed hp = r.hp(r.units(a)[0]);
    r.step({makeDebugSetExperience(0, 0, r.seq(0), {a}, 260_fx)});  // Veteran (250)
    CHECK(r.eng(a).level == 2);
    CHECK(r.first(SimEvent::Type::LevelUp, a) != UINT32_MAX);
    CHECK(r.hp(r.units(a)[0]) == hp * 1.1_fx / 1_fx);
    CHECK(levelDefenseOf(r.sim, a) == 2_fx);
    CHECK(attackMultiplierOf(r.sim, a) == r.eng(a).efficiency * (1_fx + 8_fx * 0.01_fx));
    CHECK(moraleMaxOf(r.sim, a) == 4_fx * (5_fx + 0.5_fx));
    // Army XP also grows by ExperienceFactionGainPeriodic (1/s) for every player with something on the map.
    const Fixed p1 = r.sim.world().kingdoms[1].armyXp;
    r.steps(10);
    CHECK(r.sim.world().kingdoms[1].armyXp == p1 + (1_fx * 0.1_fx) * 10_fx);
    CHECK(r.sim.world().kingdoms[5].armyXp == Fixed{});
}

TEST_CASE("entrenchment: halted for entrenchment_time entrenches; a move resets it") {
    const nav::MapDef m = openMap(128);
    Run r(m);
    const EntityId a = r.spawn("reg_brittle", {20_fx, 40_fx}, kEast, 0);
    r.steps(290);
    CHECK_FALSE(r.eng(a).entrenched);
    r.steps(20);
    CHECK(r.eng(a).entrenched);  // 30 s halted
    r.step({makeMoveGroup(0, 0, r.seq(0), {a}, {60_fx, 40_fx})});
    r.steps(2);
    CHECK_FALSE(r.eng(a).entrenched);
    CHECK(r.eng(a).stillTicks == 0);
}

TEST_CASE("Retreat: the retreat formation, passive, MoraleRetreatPenalty; on arrival it reforms and gets its formation back") {
    const nav::MapDef m = openMap(128);
    Run r(m);
    const EntityId a = r.spawn("reg_brittle", {60_fx, 40_fx}, kEast, 0);
    const EntityId b = r.spawn("reg_post", {72_fx, 40_fx}, kWest, 1);
    r.step({makeAttack(0, 0, r.seq(0), {a}, b)});
    r.steps(5);
    REQUIRE(r.eng(a).state == Engagement::State::Engaging);
    const std::uint16_t formation = r.sim.world().regiments.get(a)->formation;
    r.step({makeRetreat(0, 0, r.seq(0), {a}, {10_fx, 40_fx})});
    CHECK(r.eng(a).state == Engagement::State::Retreating);
    CHECK(r.eng(a).mode == Engagement::Mode::None);
    const LayoutDef& lay = defs().layouts[defs().layoutIndex("lay")];
    CHECK(r.sim.world().regiments.get(a)->formation == lay.retreatFormation);
    const std::size_t fired = r.count(SimEvent::Type::Fired);
    for (int i = 0; i < 2000 && r.eng(a).state != Engagement::State::Idle; ++i) r.step();
    CHECK(r.eng(a).state == Engagement::State::Idle);
    CHECK(r.count(SimEvent::Type::Fired) == fired);  // never fired while retreating, nor after (posts are far)
    CHECK(distance(r.sim.world().transforms.get(a)->position, {10_fx, 40_fx}) < 2_fx);
    CHECK(r.sim.world().regiments.get(a)->formation == formation);
}

TEST_CASE("committed combat replay plays identically on this compiler (fixture with morale, routs, XP, retreat)") {
    // sim/tests/fixtures/combat_m5.rpl: these test defs, a 128 open map, 900 ticks of the scenario
    // below, recorded with MSVC x64. CORPS_WRITE_FIXTURES=1 re-records it.
    const nav::MapDef m = openMap(128);
    Replay rep;
    rep.header.simVersion = simVersion();
    rep.header.dataHash = defs().dataHash;
    rep.header.mapSpec = m.name;
    rep.header.mapHash = m.mapHash;
    rep.header.matchSeed = 9;
    rep.header.tickCount = 900;
    rep.header.players = {"A", "B"};
    CombatTuning tuning;
    tuning.defenseK = 11_fx;
    rep.header.tuning = formatCombatTuning(tuning);
    rep.commands = {makeSpawnRegiment(0, 0, 0, "reg_brittle", {30_fx, 40_fx}, kEast), makeSpawnRegiment(0, 0, 1, "reg_tank", {30_fx, 70_fx}, kEast),
                    makeSpawnRegiment(0, 1, 0, "reg_rifle", {90_fx, 40_fx}, kWest), makeSpawnRegiment(0, 1, 1, "reg_mg", {95_fx, 70_fx}, kWest),
                    makeDebugSpawnBuilding(0, 1, 2, "bunker", {110_fx, 55_fx}, kWest)};
    {
        Sim scratch({9, &defs(), &m, &tuning});
        scratch.step(rep.commands);
        const std::vector<EntityId> regs = scratch.world().regiments.ids();
        REQUIRE(regs.size() == 4);
        rep.commands.push_back(makeDebugSetExperience(1, 0, 2, {regs[0]}, 120_fx));  // reg_brittle: Regular
        rep.commands.push_back(makeAttackMove(1, 0, 3, {regs[0], regs[1]}, {100_fx, 55_fx}));
        rep.commands.push_back(makeAttackMove(1, 1, 3, {regs[2]}, {20_fx, 40_fx}));
        rep.commands.push_back(makeAttack(1, 1, 4, {regs[3]}, regs[1]));
        rep.commands.push_back(makeRetreat(400, 0, 4, {regs[0]}, {8_fx, 40_fx}));
    }
    RunOptions opt;
    opt.map = &m;
    const std::string path = CORPS_SOURCE_DIR "/sim/tests/fixtures/combat_m5.rpl";
    std::ifstream in(vfs::pathFromUtf8(path), std::ios::binary);
    if (testEnvSet("CORPS_WRITE_FIXTURES") || !in) {
        rep.hashes = runReplay(rep, &defs(), opt).hashes;
        std::ofstream out(vfs::pathFromUtf8(path), std::ios::binary);
        const auto b = encodeReplay(rep);
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        MESSAGE("wrote " << path);
        return;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Replay recorded;
    std::string err;
    REQUIRE_MESSAGE(decodeReplay(bytes, recorded, &err), err);
    CHECK(recorded.commands == rep.commands);  // the scenario above is what's on disk
    const RunResult run = runReplay(recorded, &defs(), opt);
    CHECK_FALSE(divergenceFromRecorded(recorded, run).found);
    CHECK(run.finalHash == recorded.hashes.back().hash);
    // The film has what part 2 is about.
    Sim sim({9, &defs(), &m, &tuning});
    CommandQueue q;
    for (const Command& c : recorded.commands) q.schedule(c, 0);
    std::size_t routs = 0, levels = 0, deaths = 0;
    for (std::uint32_t t = 0; t < recorded.header.tickCount; ++t) {
        sim.step(q.take(t));
        for (const SimEvent& e : sim.events()) {
            routs += e.type == SimEvent::Type::Routed;
            levels += e.type == SimEvent::Type::LevelUp;
            deaths += e.type == SimEvent::Type::Died;
        }
    }
    CHECK(levels >= 1);
    CHECK(deaths >= 1);
    MESSAGE("combat fixture: " << routs << " routs, " << levels << " level-ups, " << deaths << " deaths");
}
