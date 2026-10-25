// M4 part 2: regiment movement. Anchors and layout slots, slowest-unit speed, turning and
// wheeling, chokepoint compression, arrival, groups and divisions, avoidance, stuck → repath,
// the movement commands, and determinism (save/load mid-move, golden hash).
//
// Uses a small hand-built DefSet (no game data needed) shaped like the original's layouts.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>

#include "core/sim.hpp"
#include "data/defs.hpp"
#include "nav/nav_world.hpp"
#include "session/bytes.hpp"
#include "session/state_io.hpp"
#include "systems/movement.hpp"
#include "test_env.hpp"

using namespace corps;
using namespace corps::fixed_literals;

namespace {

UnitDef unitDef(std::string ids, UnitMoveClass cls, Fixed speed, Fixed accel, Fixed turnRadius, TurnType turn, Fixed radius,
                Fixed health) {
    UnitDef u;
    u.ids = std::move(ids);
    u.moveClass = cls;
    u.speedMax = speed;
    u.acceleration = accel;
    u.turnRadius = turnRadius;
    u.turnType = turn;
    u.blockingRadius = radius;
    u.health = health;
    return u;
}

LayoutFormationDef layoutFormation(std::string ids, std::vector<FVec2> offsets) {
    LayoutFormationDef f;
    f.formationIds = std::move(ids);
    f.offsets = std::move(offsets);
    for (const FVec2 o : f.offsets) f.halfWidth = max(f.halfWidth, abs(o.y));
    return f;
}

// Units: inf (foot), tank (tracked, pivots), car (wheeled, turns while moving), slow (foot, 1.0).
// Layouts: lay_inf (front x3, support x2, captain) and lay_veh (front x3, captain), each with the
// three regiment formations; column is narrower than the others. Regiments: reg_inf, reg_tank,
// reg_car, reg_mixed (two inf + a slow captain).
DefSet makeDefs() {
    DefSet d;
    d.units = {unitDef("car", UnitMoveClass::Wheeled, 4.5_fx, 3.5_fx, 1.5_fx, TurnType::WhileMoving, 0.6_fx, 250_fx),
               unitDef("inf", UnitMoveClass::Foot, 2.5_fx, 0_fx, 0_fx, TurnType::Instant, 0.35_fx, 200_fx),
               unitDef("slow", UnitMoveClass::Foot, 1_fx, 0_fx, 0_fx, TurnType::Instant, 0.35_fx, 100_fx),
               unitDef("tank", UnitMoveClass::Tracked, 3.5_fx, 3_fx, 0.9_fx, TurnType::InPlace, 0.8_fx, 400_fx)};
    FormationDef agg, def, col;
    agg.ids = "regiment_aggressive";
    agg.aggressive = true;
    agg.speedMultiplier = 0.75_fx;
    agg.groupFormationIds = "group_aggressive";
    def.ids = "regiment_defensive";
    def.aggressive = true;
    def.groupFormationIds = "group_defensive";
    col.ids = "regiment_column";
    col.speedMultiplier = 1.25_fx;
    col.groupFormationIds = "group_column";
    d.formations = {agg, col, def};  // sorted by IDS
    GroupFormationDef ga, gc, gd;
    ga.ids = "group_aggressive";
    ga.offsets = {{6_fx, 0_fx}, {6_fx, 13.5_fx}, {6_fx, -13.5_fx}, {-6_fx, 0_fx}, {-6_fx, 13.5_fx}, {-6_fx, -13.5_fx}};
    gc.ids = "group_column";
    gc.offsets = {{8_fx, 0_fx}, {8_fx, 9_fx}, {8_fx, -9_fx}, {-8_fx, 0_fx}, {-8_fx, 9_fx}, {-8_fx, -9_fx}};
    gd.ids = "group_defensive";
    gd.offsets = {{10_fx, -6.5_fx}, {10_fx, 6.5_fx}, {0_fx, -6.5_fx}, {0_fx, 6.5_fx}, {-10_fx, -6.5_fx}, {-10_fx, 6.5_fx}};
    d.groupFormations = {ga, gc, gd};

    LayoutDef inf;
    inf.ids = "lay_inf";
    inf.defaultFormationIds = "regiment_defensive";
    inf.combatFormationIds = "regiment_aggressive";
    inf.slots = {{"front", 3, false, true, true}, {"support", 2, false, false, false}, {"captain", 1, true, true, true}};
    inf.elementSlot = {0, 0, 0, 1, 1, 2};
    inf.formations = {
        layoutFormation("regiment_aggressive", {{4_fx, 3_fx}, {4.5_fx, 0_fx}, {4_fx, -3_fx}, {-1_fx, 2_fx}, {-1_fx, -2_fx}, {0_fx, 0_fx}}),
        layoutFormation("regiment_defensive", {{2.5_fx, 3_fx}, {3_fx, 0_fx}, {2.5_fx, -3_fx}, {-2_fx, 2.5_fx}, {-2_fx, -2.5_fx}, {0_fx, 0_fx}}),
        layoutFormation("regiment_column", {{3_fx, 1_fx}, {3_fx, -1_fx}, {1.5_fx, 1_fx}, {-1.5_fx, 1_fx}, {-1.5_fx, -1_fx}, {0_fx, 0_fx}})};
    LayoutDef veh;
    veh.ids = "lay_veh";
    veh.defaultFormationIds = "regiment_defensive";
    veh.slots = {{"front", 3, false, true, true}, {"captain", 1, true, true, false}};
    veh.elementSlot = {0, 0, 0, 1};
    veh.formations = {layoutFormation("regiment_aggressive", {{3_fx, 5_fx}, {4_fx, 0_fx}, {3_fx, -5_fx}, {0_fx, 0_fx}}),
                      layoutFormation("regiment_defensive", {{2_fx, 4_fx}, {4_fx, 0_fx}, {2_fx, -4_fx}, {0_fx, 0_fx}}),
                      layoutFormation("regiment_column", {{4_fx, 1.5_fx}, {4_fx, -1.5_fx}, {-4_fx, 0_fx}, {0_fx, 0_fx}})};
    d.layouts = {inf, veh};
    for (LayoutDef& l : d.layouts) {
        for (LayoutFormationDef& f : l.formations) f.formationIndex = d.formationIndex(f.formationIds);
    }
    for (FormationDef& f : d.formations) f.groupFormationIndex = d.groupFormationIndex(f.groupFormationIds);

    const auto regiment = [&](std::string ids, std::string layout, std::vector<std::string> slots) {
        RegimentDef g;
        g.ids = std::move(ids);
        g.layoutIds = std::move(layout);
        g.layoutIndex = d.layoutIndex(g.layoutIds);
        g.slotIds = std::move(slots);
        for (const std::string& s : g.slotIds) {
            const std::uint32_t ui = d.unitIndex(s);
            g.slots.push_back(ui == kNoDef ? ElementRef{} : ElementRef{ElementKind::Unit, ui});
        }
        return g;
    };
    d.regiments = {regiment("reg_car", "lay_veh", {"car", "car"}), regiment("reg_inf", "lay_inf", {"inf", "inf", "inf"}),
                   regiment("reg_mixed", "lay_inf", {"inf", "", "slow"}), regiment("reg_tank", "lay_veh", {"tank", "tank"})};
    d.dataHash = hashBytes(serializeDefs(d));
    return d;
}

const DefSet& defs() {
    static const DefSet d = makeDefs();
    return d;
}

nav::MapDef openMap(std::uint32_t w, std::uint32_t h, const std::vector<std::pair<std::int32_t, std::int32_t>>& rocks = {}) {
    nav::MapDef m;
    m.name = "open";
    m.width = w;
    m.height = h;
    m.kind.assign(m.cellCount(), nav::TerrainKind::Ground);
    for (const auto& [x, y] : rocks) m.kind[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] = nav::TerrainKind::Mountain;
    m.terrainId.assign(m.cellCount(), 0);
    m.terrainNames = {"t"};
    REQUIRE(m.finalize());
    return m;
}

// A vertical rock wall at x with a gap of `gap` cells centred on gapY.
nav::MapDef wallMap(std::uint32_t w, std::uint32_t h, std::int32_t x, std::int32_t gapY, std::int32_t gap) {
    std::vector<std::pair<std::int32_t, std::int32_t>> rocks;
    for (std::int32_t y = 0; y < static_cast<std::int32_t>(h); ++y) {
        if (y < gapY - gap / 2 || y > gapY + (gap - 1) / 2) {
            rocks.emplace_back(x, y);
            rocks.emplace_back(x + 1, y);
        }
    }
    return openMap(w, h, rocks);
}

struct Run {
    Sim sim;
    std::uint32_t nextSeq = 0;
    explicit Run(const nav::MapDef& m) : sim({7, &defs(), &m}) {}

    EntityId spawn(const char* ids, FVec2 at, Angle facing = Angle{}, std::uint8_t player = 0) {
        const std::size_t before = sim.world().regiments.size();
        step({makeSpawnRegiment(sim.tick(), player, seq(), ids, at, facing)});
        REQUIRE(sim.world().regiments.size() == before + 1);
        for (const SimEvent& e : sim.events()) {
            if (e.type == SimEvent::Type::Spawned && sim.world().regiments.has(e.entity)) return e.entity;
        }
        FAIL("no regiment spawned");
        return EntityId{};
    }
    std::uint16_t seq() { return static_cast<std::uint16_t>(nextSeq++); }
    void step(std::vector<Command> cmds = {}) {
        for (Command& c : cmds) c.tick = sim.tick();
        sim.step(cmds);
        for (const SimEvent& e : sim.events()) {
            if (e.type == SimEvent::Type::Arrived) arrived.emplace_back(e.entity, sim.tick() - 1);
            if (e.type == SimEvent::Type::Blocked) blocked.emplace_back(e.entity, sim.tick() - 1);
            if (e.type == SimEvent::Type::CommandRejected) ++rejected;
        }
    }
    // Steps until `pred` or `limit` ticks; returns ticks run.
    template <class P>
    std::uint32_t until(P pred, std::uint32_t limit) {
        for (std::uint32_t i = 0; i < limit; ++i) {
            if (pred()) return i;
            step();
        }
        return limit;
    }
    std::uint32_t arrivedAt(EntityId id) const {
        for (const auto& [e, t] : arrived) {
            if (e == id) return t;
        }
        return UINT32_MAX;
    }
    bool isBlocked(EntityId id) const {
        return std::any_of(blocked.begin(), blocked.end(), [&](const auto& p) { return p.first == id; });
    }
    const Order& order(EntityId id) { return *sim.world().orders.get(id); }
    const Transform& anchor(EntityId id) { return *sim.world().transforms.get(id); }
    std::vector<EntityId> units(EntityId reg) {
        std::vector<EntityId> out;
        for (std::size_t i = 0; i < sim.world().members.size(); ++i) {
            if (sim.world().members.data()[i].organization == reg) out.push_back(sim.world().members.ids()[i]);
        }
        return out;
    }
    // Max distance of a regiment's units from their slots in the regiment's chosen formation.
    Fixed maxSlotError(EntityId reg) {
        const Regiment& r = *sim.world().regiments.get(reg);
        const Transform& a = anchor(reg);
        Fixed worst;
        for (const EntityId u : units(reg)) {
            const FVec2 slot = a.position + layoutOffset(defs(), r, r.formation, sim.world().members.get(u)->slot).rotated(a.facing);
            worst = max(worst, distance(sim.world().transforms.get(u)->position, slot));
        }
        return worst;
    }
    std::vector<std::pair<EntityId, std::uint32_t>> arrived, blocked;
    std::size_t rejected = 0;
};

bool idle(Run& r, EntityId id) {
    return r.order(id).state == Order::State::Idle;
}

}  // namespace

// ------------------------------------------------------------------------------------- data

TEST_CASE("test defs compile into consistent layouts, formations and group formations") {
    const DefSet& d = defs();
    const LayoutDef& inf = d.layouts[d.layoutIndex("lay_inf")];
    CHECK(inf.elementSlot.size() == 6);
    CHECK(inf.formation("regiment_column") != kNoDef);
    CHECK(d.formations[d.formationIndex("regiment_aggressive")].speedMultiplier == 0.75_fx);
    CHECK(d.formations[d.formationIndex("regiment_column")].groupFormationIndex == d.groupFormationIndex("group_column"));
    CHECK(restrictiveClass(static_cast<std::uint8_t>(UnitMoveClass::Foot), static_cast<std::uint8_t>(UnitMoveClass::Wheeled)) ==
          static_cast<std::uint8_t>(UnitMoveClass::Wheeled));
    CHECK(restrictiveClass(static_cast<std::uint8_t>(UnitMoveClass::Tracked), static_cast<std::uint8_t>(UnitMoveClass::Foot)) ==
          static_cast<std::uint8_t>(UnitMoveClass::Tracked));
}

TEST_CASE("spawning expands layout slots by number and places units on their default-formation slots") {
    const nav::MapDef m = openMap(64, 64);
    Run r(m);
    const Angle north = Angle::fromUnits(Angle::kQuarter);
    const EntityId inf = r.spawn("reg_inf", {30_fx, 30_fx}, north);
    // reg_inf: slot 0 (front x3) inf, slot 1 (support x2) inf, slot 2 (captain) inf → 6 units.
    CHECK(r.units(inf).size() == 6);
    const Regiment& g = *r.sim.world().regiments.get(inf);
    CHECK(g.layout == defs().layoutIndex("lay_inf"));
    CHECK(g.formation == defs().layouts[g.layout].formation("regiment_defensive"));
    CHECK(g.moveClass == static_cast<std::uint8_t>(UnitMoveClass::Foot));
    CHECK(r.maxSlotError(inf) == 0_fx);
    const EntityId mixed = r.spawn("reg_mixed", {10_fx, 10_fx});
    CHECK(r.units(mixed).size() == 4);  // 3 inf + empty support + slow captain
    const EntityId car = r.spawn("reg_car", {50_fx, 50_fx});
    CHECK(r.sim.world().regiments.get(car)->moveClass == static_cast<std::uint8_t>(UnitMoveClass::Wheeled));
    for (const EntityId u : r.units(car)) CHECK(r.sim.world().movers.has(u));
}

// ------------------------------------------------------------------------------------- one regiment

TEST_CASE("a regiment moves along its path, its units hold their slots, and it arrives") {
    const nav::MapDef m = openMap(96, 64);
    Run r(m);
    const EntityId inf = r.spawn("reg_inf", {10_fx, 30_fx});
    r.step({makeMoveGroupFacing(0, 0, r.seq(), {inf}, {80_fx, 34_fx}, Angle::fromUnits(Angle::kQuarter))});
    Fixed worstWhileMoving;
    const std::uint32_t ticks = r.until(
        [&] {
            if (r.order(inf).state == Order::State::Moving) worstWhileMoving = max(worstWhileMoving, r.maxSlotError(inf));
            return idle(r, inf);
        },
        800);
    REQUIRE(ticks < 800);
    CHECK(r.arrivedAt(inf) != UINT32_MAX);
    CHECK_FALSE(r.order(inf).blocked);
    CHECK(distance(r.anchor(inf).position, {80_fx, 34_fx}) < 0.01_fx);
    CHECK(r.anchor(inf).facing == Angle::fromUnits(Angle::kQuarter));  // explicit facing on arrival
    CHECK(worstWhileMoving < 4_fx);   // formation holds while marching (one slot length + slack)
    // 70 units at 2.5 u/s (defensive formation, ×1.0) ≈ 28 s; plus start-up and settling.
    CHECK(ticks > 270);
    CHECK(ticks < 420);
    r.until([] { return false; }, 40);  // settle
    CHECK(r.maxSlotError(inf) < move::kSlotTolerance);
}

TEST_CASE("the anchor moves at the slowest unit's speed times the formation multiplier") {
    const nav::MapDef m = openMap(128, 32);
    Run r(m);
    const EntityId mixed = r.spawn("reg_mixed", {6_fx, 16_fx});
    const Angle east{};
    r.step({makeSetFormation(0, 0, r.seq(), {mixed}, "regiment_aggressive")});  // slow captain: 1.0 × 0.75
    r.step({makeMoveGroup(0, 0, r.seq(), {mixed}, {120_fx, 16_fx})});
    r.until([&] { return r.order(mixed).state == Order::State::Moving; }, 10);
    Fixed fastest;
    for (int i = 0; i < 100; ++i) {
        const FVec2 before = r.anchor(mixed).position;
        r.step();
        fastest = max(fastest, distance(before, r.anchor(mixed).position));
    }
    CHECK(fastest <= 0.075_fx + 0.0001_fx);  // 1.0 × 0.75 × 0.1 s
    CHECK(fastest > 0.07_fx);
    (void)east;
}

TEST_CASE("Column is faster than Assault (data speed multipliers)") {
    const nav::MapDef m = openMap(128, 32);
    std::uint32_t arrive[2] = {0, 0};
    const char* formations[2] = {"regiment_aggressive", "regiment_column"};
    for (int k = 0; k < 2; ++k) {
        Run r(m);
        const EntityId inf = r.spawn("reg_inf", {8_fx, 16_fx});
        r.step({makeSetFormation(0, 0, r.seq(), {inf}, formations[k]), makeMoveGroup(0, 0, r.seq(), {inf}, {110_fx, 16_fx})});
        arrive[k] = r.until([&] { return idle(r, inf); }, 1200);
    }
    CHECK(arrive[1] < arrive[0]);
    CHECK(arrive[0] < 1200);
}

TEST_CASE("an order behind the regiment makes it wheel before it moves") {
    const nav::MapDef m = openMap(96, 64);
    Run r(m);
    const EntityId tank = r.spawn("reg_tank", {60_fx, 30_fx});  // facing east
    r.step({makeMoveGroup(0, 0, r.seq(), {tank}, {10_fx, 31_fx})});
    bool wheeled = false;
    r.until(
        [&] {
            wheeled |= r.order(tank).state == Order::State::Wheeling;
            return idle(r, tank);
        },
        600);
    CHECK(wheeled);
    CHECK(r.arrivedAt(tank) != UINT32_MAX);
    r.until([] { return false; }, 30);  // let the last pivots finish
    // In-place turners end facing the direction of travel (west).
    for (const EntityId u : r.units(tank)) CHECK(std::abs(Angle::delta(r.sim.world().transforms.get(u)->facing, Angle::fromUnits(Angle::kHalf))) < 3000);
}

TEST_CASE("chokepoints compress the formation (Column, then single file) and it widens again after") {
    const nav::MapDef m = wallMap(96, 64, 48, 32, 2);  // a 2-cell gap
    Run r(m);
    const EntityId inf = r.spawn("reg_inf", {10_fx, 32_fx});
    r.step({makeMoveGroup(0, 0, r.seq(), {inf}, {86_fx, 32_fx})});
    std::uint8_t most = 0;
    r.until(
        [&] {
            most = std::max(most, r.sim.world().regiments.get(inf)->compression);
            return idle(r, inf);
        },
        1500);
    CHECK(r.arrivedAt(inf) != UINT32_MAX);
    CHECK(most == 2);  // 2 cells is narrower than even the column
    r.until([] { return false; }, 30);
    CHECK(r.sim.world().regiments.get(inf)->compression == 0);
    // Nobody ended up inside the rock.
    for (const EntityId u : r.units(inf)) {
        const FVec2 p = r.sim.world().transforms.get(u)->position;
        CHECK(r.sim.nav()->grid.cost(nav::MoveClass::Foot, r.sim.cellAt(p)) != 0);
    }
}

TEST_CASE("Run moves in Column and restores the formation on arrival; Stop halts") {
    const nav::MapDef m = openMap(96, 32);
    Run r(m);
    const EntityId inf = r.spawn("reg_inf", {8_fx, 16_fx});
    const std::uint16_t before = r.sim.world().regiments.get(inf)->formation;
    r.step({makeRun(0, 0, r.seq(), {inf}, {80_fx, 16_fx})});
    CHECK(r.sim.world().regiments.get(inf)->formation == defs().layouts[0].formation("regiment_column"));
    r.until([&] { return idle(r, inf); }, 800);
    CHECK(r.sim.world().regiments.get(inf)->formation == before);

    r.step({makeMoveGroup(0, 0, r.seq(), {inf}, {10_fx, 16_fx})});
    r.until([] { return false; }, 60);
    r.step({makeStop(0, 0, r.seq(), {inf})});
    CHECK(idle(r, inf));
    const FVec2 stopped = r.anchor(inf).position;
    r.until([] { return false; }, 50);
    CHECK(r.anchor(inf).position == stopped);
    CHECK(r.maxSlotError(inf) < move::kSlotTolerance);  // units settle around the halted anchor
}

TEST_CASE("commands validate ownership and arguments") {
    const nav::MapDef m = openMap(64, 64);
    Run r(m);
    const EntityId mine = r.spawn("reg_inf", {10_fx, 10_fx}, Angle{}, 0);
    const EntityId theirs = r.spawn("reg_inf", {50_fx, 50_fx}, Angle{}, 1);
    r.rejected = 0;
    r.step({makeMoveGroup(0, 0, r.seq(), {theirs}, {20_fx, 20_fx})});                    // not player 0's
    r.step({makeSetFormation(0, 0, r.seq(), {mine}, "regiment_flying_wedge")});          // no such formation
    r.step({makeSetGroupBehaviour(0, 0, r.seq(), {mine}, true)});                        // no group to switch
    r.step({makeStop(0, 0, r.seq(), {EntityId::make(999, 1)})});                         // dead id
    CHECK(r.rejected == 4);
    CHECK(idle(r, theirs));
    r.step({makeMoveGroup(0, 0, r.seq(), {theirs, mine}, {20_fx, 20_fx})});  // only `mine` moves
    CHECK(r.order(mine).state != Order::State::Idle);
    CHECK(idle(r, theirs));
    Sim noMap({1, &defs()});
    noMap.step({makeSpawnRegiment(0, 0, 0, "reg_inf", {}, Angle{})});
    noMap.step({makeMoveGroup(1, 0, 0, noMap.world().regiments.ids(), {5_fx, 5_fx})});
    CHECK(noMap.events().size() == 1);
}

// ------------------------------------------------------------------------------------- groups

TEST_CASE("a loose group sends each regiment to its own group-formation slot") {
    const nav::MapDef m = openMap(128, 96);
    Run r(m);
    const EntityId a = r.spawn("reg_inf", {10_fx, 30_fx});
    const EntityId b = r.spawn("reg_tank", {10_fx, 60_fx});
    r.step({makeMoveGroup(0, 0, r.seq(), {a, b}, {100_fx, 45_fx})});
    const EntityId gid = r.sim.world().regiments.get(a)->group;
    REQUIRE_FALSE(gid.isNull());
    CHECK(r.sim.world().regiments.get(b)->group == gid);
    CHECK_FALSE(r.sim.world().groups.get(gid)->division);
    CHECK(distance(r.order(a).goal, r.order(b).goal) > 5_fx);  // different slots
    r.until([&] { return idle(r, a) && idle(r, b); }, 1200);
    CHECK(r.arrivedAt(a) != UINT32_MAX);
    CHECK(r.arrivedAt(b) != UINT32_MAX);
    // Ordering one of them alone takes it out of the group, which dissolves.
    r.step({makeMoveGroup(0, 0, r.seq(), {a}, {20_fx, 20_fx})});
    CHECK(r.sim.world().regiments.get(a)->group.isNull());
    CHECK(r.sim.world().regiments.get(b)->group.isNull());
    r.step();
    CHECK_FALSE(r.sim.world().registry.alive(gid));
}

TEST_CASE("a division moves as one: shared anchor, strongest in front, slowest speed, arrives together") {
    const nav::MapDef m = openMap(160, 96);
    Run r(m);
    const EntityId inf = r.spawn("reg_inf", {12_fx, 30_fx});
    const EntityId tank = r.spawn("reg_tank", {12_fx, 48_fx});
    const EntityId car = r.spawn("reg_car", {12_fx, 66_fx});
    r.step({makeSetGroupBehaviour(0, 0, r.seq(), {inf, tank, car}, true), makeMoveGroup(0, 0, r.seq(), {inf, tank, car}, {140_fx, 48_fx})});
    const EntityId gid = r.sim.world().regiments.get(inf)->group;
    REQUIRE_FALSE(gid.isNull());
    const Group& g = *r.sim.world().groups.get(gid);
    CHECK(g.division);
    // Combat value = total health: tank 4×400 = 1600 > inf 6×200 = 1200 > car 4×250 = 1000.
    REQUIRE(g.slotOrder.size() == 3);
    CHECK(g.slotOrder[0] == tank);
    CHECK(g.slotOrder[1] == inf);
    CHECK(g.slotOrder[2] == car);
    CHECK(r.order(inf).state == Order::State::Following);
    Fixed fastest;
    std::uint32_t ticks = r.until(
        [&] {
            const FVec2 before = r.anchor(gid).position;
            (void)before;
            return idle(r, gid);
        },
        2000);
    (void)fastest;
    REQUIRE(ticks < 2000);
    CHECK(r.arrivedAt(inf) == r.arrivedAt(tank));  // together
    CHECK(r.arrivedAt(car) == r.arrivedAt(tank));
    CHECK(r.arrivedAt(gid) == r.arrivedAt(inf));
    // Members end in their slots around the division anchor at the goal.
    for (std::size_t k = 0; k < g.slotOrder.size(); ++k) {
        const FVec2 slot = r.anchor(gid).position + groupSlotOffset(r.sim, g, k).rotated(r.anchor(gid).facing);
        CHECK(distance(r.anchor(g.slotOrder[k]).position, slot) <= move::kSlotTolerance);
    }
    CHECK(distance(r.anchor(gid).position, {140_fx, 48_fx}) < 0.01_fx);
}

TEST_CASE("the division anchor never runs faster than its slowest regiment") {
    const nav::MapDef m = openMap(160, 64);
    Run r(m);
    const EntityId inf = r.spawn("reg_inf", {12_fx, 24_fx});
    const EntityId car = r.spawn("reg_car", {12_fx, 40_fx});
    r.step({makeSetGroupBehaviour(0, 0, r.seq(), {inf, car}, true), makeMoveGroup(0, 0, r.seq(), {inf, car}, {150_fx, 32_fx})});
    const EntityId gid = r.sim.world().regiments.get(inf)->group;
    Fixed fastest;
    for (int i = 0; i < 300 && !idle(r, gid); ++i) {
        const FVec2 before = r.anchor(gid).position;
        r.step();
        fastest = max(fastest, distance(before, r.anchor(gid).position));
    }
    CHECK(fastest <= 0.25_fx + 0.0001_fx);  // infantry 2.5 u/s × 0.1 s (defensive ×1.0)
    CHECK(fastest > 0.2_fx);
}

// ------------------------------------------------------------------------------------- avoidance

TEST_CASE("local avoidance keeps crossing regiments apart (friends yield, enemies push)") {
    for (const std::uint8_t otherPlayer : {std::uint8_t{0}, std::uint8_t{1}}) {
        CAPTURE(int(otherPlayer));
        const nav::MapDef m = openMap(96, 64);
        Run r(m);
        const EntityId a = r.spawn("reg_inf", {10_fx, 32_fx}, Angle{}, 0);
        const EntityId b = r.spawn("reg_inf", {86_fx, 32.5_fx}, Angle::fromUnits(Angle::kHalf), otherPlayer);
        r.step({makeMoveGroup(0, 0, r.seq(), {a}, {86_fx, 32_fx}), makeMoveGroup(0, otherPlayer, r.seq(), {b}, {10_fx, 32.5_fx})});
        Fixed closest = 100_fx;
        const auto ua = r.units(a), ub = r.units(b);
        r.until(
            [&] {
                for (const EntityId x : ua) {
                    for (const EntityId y : ub) {
                        closest = min(closest, distance(r.sim.world().transforms.get(x)->position, r.sim.world().transforms.get(y)->position));
                    }
                }
                return idle(r, a) && idle(r, b);
            },
            1500);
        CHECK(r.arrivedAt(a) != UINT32_MAX);
        CHECK(r.arrivedAt(b) != UINT32_MAX);
        CHECK(closest > 0.35_fx);  // radius sum is 0.7; separation keeps overlaps small
    }
}

// ------------------------------------------------------------------------------------- stuck / blocked

TEST_CASE("a building dropped on the route makes the regiment re-path around it") {
    const nav::MapDef m = openMap(128, 64);
    Run r(m);
    const EntityId tank = r.spawn("reg_tank", {8_fx, 32_fx});
    r.step({makeMoveGroup(0, 0, r.seq(), {tank}, {120_fx, 32_fx})});
    r.until([] { return false; }, 40);
    r.step({makeDebugPlaceBlocker(0, 0, r.seq(), {64_fx, 32_fx}, {2_fx, 20_fx})});
    bool sawRepath = false;
    r.until(
        [&] {
            sawRepath |= r.order(tank).state == Order::State::WaitPath;
            return idle(r, tank);
        },
        1500);
    CHECK(sawRepath);
    CHECK(r.arrivedAt(tank) != UINT32_MAX);
    for (const EntityId u : r.units(tank)) {
        CHECK(r.sim.nav()->grid.stampCount(r.sim.cellAt(r.sim.world().transforms.get(u)->position)) == 0);
    }
}

TEST_CASE("a regiment that can't get anywhere ends its order as blocked") {
    // Boxed in by buildings on all sides; the goal is outside.
    const nav::MapDef m = openMap(64, 64);
    Run r(m);
    const EntityId inf = r.spawn("reg_inf", {32_fx, 32_fx});
    r.step({makeDebugPlaceBlocker(0, 0, r.seq(), {32_fx, 22_fx}, {12_fx, 1_fx}), makeDebugPlaceBlocker(0, 0, r.seq(), {32_fx, 42_fx}, {12_fx, 1_fx}),
            makeDebugPlaceBlocker(0, 0, r.seq(), {22_fx, 32_fx}, {1_fx, 12_fx}), makeDebugPlaceBlocker(0, 0, r.seq(), {42_fx, 32_fx}, {1_fx, 12_fx})});
    r.step({makeMoveGroup(0, 0, r.seq(), {inf}, {60_fx, 60_fx})});
    r.until([&] { return idle(r, inf); }, 600);
    // The nearest reachable cell is inside the box: the regiment goes there and arrives (no
    // path out exists), it never walks through the walls.
    CHECK((r.arrivedAt(inf) != UINT32_MAX || r.isBlocked(inf)));
    const FVec2 p = r.anchor(inf).position;
    CHECK(p.x < 42_fx);
    CHECK(p.y < 42_fx);
}

// ------------------------------------------------------------------------------------- determinism

namespace {
std::vector<std::vector<Command>> script(const std::vector<EntityId>& regs) {
    std::vector<std::vector<Command>> t(400);
    std::uint16_t seq = 100;
    t[1] = {makeSetGroupBehaviour(1, 0, seq++, {regs[0], regs[1]}, true), makeMoveGroup(1, 0, seq++, {regs[0], regs[1]}, {100_fx, 70_fx})};
    t[2] = {makeRun(2, 0, seq++, {regs[2]}, {20_fx, 80_fx}), makeMoveGroup(2, 1, seq++, {regs[3]}, {12_fx, 12_fx})};
    t[40] = {makeDebugPlaceBlocker(40, 0, seq++, {60_fx, 50_fx}, {2_fx, 10_fx})};
    t[90] = {makeSetFormation(90, 0, seq++, {regs[2]}, "regiment_aggressive"), makeStop(90, 1, seq++, {regs[3]})};
    t[120] = {makeMoveGroup(120, 1, seq++, {regs[3]}, {110_fx, 20_fx})};
    return t;
}

void spawnFour(Sim& s) {
    s.step({makeSpawnRegiment(0, 0, 0, "reg_inf", {10_fx, 20_fx}, Angle{}), makeSpawnRegiment(0, 0, 1, "reg_tank", {10_fx, 40_fx}, Angle{}),
            makeSpawnRegiment(0, 0, 2, "reg_car", {100_fx, 20_fx}, Angle::fromUnits(Angle::kHalf)),
            makeSpawnRegiment(0, 1, 3, "reg_mixed", {60_fx, 80_fx}, Angle{})});
}
}  // namespace

TEST_CASE("save/load mid-move continues identically; the golden movement hash holds") {
    const nav::MapDef m = nav::generateTestMap(11, 128);
    Sim ref({3, &defs(), &m});
    spawnFour(ref);
    const std::vector<EntityId> regs = ref.world().regiments.ids();
    REQUIRE(regs.size() == 4);
    const auto t = script(regs);
    for (std::uint32_t k = 1; k < 400; ++k) ref.step(t[k]);
    const std::uint64_t want = stateHash(ref);

    Sim a({3, &defs(), &m});
    spawnFour(a);
    for (std::uint32_t k = 1; k < 400; ++k) {
        a.step(t[k]);
        if (k % 57 != 3) continue;
        Sim b({3, &defs(), &m});
        std::string err;
        REQUIRE_MESSAGE(loadState(b, saveState(a), &err), err);
        CHECK(saveState(b) == saveState(a));
        for (std::uint32_t j = k + 1; j < 400; ++j) b.step(t[j]);
        CHECK(stateHash(b) == want);
    }
    CHECK(stateHash(a) == want);

    // Cross-compiler / cross-platform golden value (CORPS_PRINT_GOLDEN=1 prints it).
    constexpr std::uint64_t kGolden = 0xd4377411e6e28a49ULL;
    if (testEnvSet("CORPS_PRINT_GOLDEN")) {
        std::printf("movement golden: 0x%016llxULL\n", static_cast<unsigned long long>(want));
    } else {
        CHECK(want == kGolden);
    }
}

TEST_CASE("movement commands round-trip through the byte encoding") {
    const std::vector<EntityId> ids = {EntityId::make(3, 1), EntityId::make(70000 % 1048575, 2)};
    const Command cmds[] = {makeMoveGroup(4, 1, 2, ids, {-3_fx, 7.5_fx}), makeMoveGroupFacing(4, 1, 3, ids, {1_fx, 2_fx}, Angle::fromUnits(1234)),
                            makeRun(5, 0, 1, {ids[0]}, {9_fx, 9_fx}), makeStop(6, 2, 9, ids),
                            makeSetFormation(7, 0, 4, ids, "regiment_column"), makeSetGroupBehaviour(8, 3, 5, ids, true)};
    for (const Command& c : cmds) {
        std::vector<std::uint8_t> bytes;
        encodeCommand(c, bytes);
        Command back;
        back.tick = c.tick;
        back.seq = c.seq;
        std::size_t pos = 0;
        REQUIRE(decodeCommand(bytes, pos, back));
        CHECK(pos == bytes.size());
        CHECK(back == c);
        for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
            std::vector<std::uint8_t> shorter(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
            std::size_t p2 = 0;
            Command x;
            CHECK_FALSE(decodeCommand(shorter, p2, x));
        }
    }
    // More than six regiments, or none, is malformed.
    Command big = makeStop(1, 0, 0, std::vector<EntityId>(7, EntityId::make(1, 1)));
    std::vector<std::uint8_t> bytes;
    encodeCommand(big, bytes);
    std::size_t pos = 0;
    Command x;
    CHECK_FALSE(decodeCommand(bytes, pos, x));
}

