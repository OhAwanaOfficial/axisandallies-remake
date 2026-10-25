// M6 part 1: economy and bases. Income per tick, oil / ammo shortfall bought with money,
// bankruptcy, unit and building limits, placement rules, the Corps HQ, pack / unpack via trucks,
// recruiting unit by unit with required structures and properties, research granting properties
// (and their AV in combat), save / load, command encoding, and a committed replay fixture.
//
// Uses a small hand-built DefSet shaped like the GBR records (no game data needed).

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>

#include "core/sim.hpp"
#include "data/def_compiler.hpp"
#include "data/defs.hpp"
#include "nav/nav_world.hpp"
#include "session/bytes.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"
#include "systems/bases.hpp"
#include "systems/economy.hpp"
#include "systems/morale.hpp"
#include "test_env.hpp"
#include "vfs/path.hpp"

using namespace corps;
using namespace corps::fixed_literals;

namespace {

PropertyDef property(std::string ids, std::vector<std::string> requires_ = {}, std::vector<ModifierDef> mods = {}) {
    PropertyDef p;
    p.ids = std::move(ids);
    p.requiredProperties = std::move(requires_);
    p.modifiers = std::move(mods);
    return p;
}

UnitDef unit(std::string ids, UnitMoveClass cls, std::vector<std::string> props) {
    UnitDef u;
    u.ids = std::move(ids);
    u.moveClass = cls;
    u.speedMax = 3_fx;
    u.turnType = TurnType::Instant;
    u.blockingRadius = 0.4_fx;
    u.health = 200_fx;
    u.properties = std::move(props);
    return u;
}

UnitDef truck(std::string ids, std::string unpackTo, Fixed cost) {
    UnitDef u = unit(std::move(ids), UnitMoveClass::Wheeled, {"vehicle"});
    u.category = "TRUCK";
    u.health = 500_fx;
    u.blockingRadius = 0.65_fx;
    u.unpackToIds = std::move(unpackTo);
    u.unpackingLength = 7_fx;
    u.cost.money = cost;
    u.upkeep.buildingLimitConsumed = 1_fx;
    return u;
}

BuildingDef building(std::string ids, Fixed size, Fixed packing, std::vector<std::string> props) {
    BuildingDef b;
    b.ids = std::move(ids);
    b.health = 2000_fx;
    b.shape = FootprintShape::Square;
    b.squareSize = size;
    b.padding = 1_fx;
    b.tileLocked = true;
    b.axisLocked = true;
    b.mobile = true;
    b.packHealthMin = 0.5_fx;
    b.packingLength = packing;
    b.unpackingLength = packing;
    b.creationOffset = {7_fx, 0_fx};
    b.deploymentOffset = {14_fx, 0_fx};
    b.properties = std::move(props);
    b.upkeep.buildingLimitConsumed = 1_fx;
    return b;
}

RegimentDef regiment(std::string ids, std::vector<std::string> slots, Fixed cost, Fixed recruit) {
    RegimentDef g;
    g.ids = std::move(ids);
    g.slotIds = std::move(slots);
    g.cost.money = cost;
    g.recruitTime = recruit;
    return g;
}

// Buildings: corps (Corps HQ: 10x10, packs in 10 s; +25 money, +10 oil, +20 ammo; recruits the
// HQ regiment, the motor pool and depot trucks; researches tech_mg), hq (division HQ: 10x10,
// 8 s; +25 money, +5 oil, +10 ammo, unit limit 3; recruits infantry and heavy tanks; researches
// tech_heavy), pool (motor pool, 8x8), depot (oil depot, 8x8, +25 oil). Trucks: corps_truck,
// hq_truck (in reg_hq), pool_truck, depot_truck. Regiments: reg_inf (3 rifle + mgun; 50 money,
// ammo 10, unit limit 1, 4 s), reg_heavy (one heavy tank: 200 money, oil 18, ammo 20, unit limit
// 1; needs the motor pool and tech_heavy), reg_hq (the HQ truck; 2 s). Start location: building
// limit 5.
const DefSet& defs() {
    static const DefSet set = [] {
        DefSet d;
        ModifierDef av4;
        av4.attribute = "AV";
        av4.amount = 4_fx;
        d.properties = {property("armorhq"), property("building"), property("corpshq"), property("infantry"), property("mg"),
                        property("tech_heavy", {"armorhq"}), property("tech_mg", {"mg"}, {av4}), property("vehicle")};
        UnitDef heavy = unit("heavy", UnitMoveClass::Tracked, {"vehicle", "tank"});
        heavy.requiredStructureIds = {"pool"};
        heavy.requiredProperties = {"tech_heavy"};
        UnitDef corpsTruck = truck("corps_truck", "corps", 400_fx);
        corpsTruck.production.money = 12_fx;
        UnitDef hqTruck = truck("hq_truck", "hq", 225_fx);
        hqTruck.production.money = 12_fx;
        hqTruck.production.unitLimitProvided = 3_fx;
        d.units = {corpsTruck, truck("depot_truck", "depot", 112_fx), heavy, hqTruck,
                   unit("mgun", UnitMoveClass::Foot, {"infantry", "mg"}), truck("pool_truck", "pool", 100_fx),
                   unit("rifle", UnitMoveClass::Foot, {"infantry"})};
        RegimentDef heavyReg = regiment("reg_heavy", {"heavy"}, 200_fx, 2_fx);
        heavyReg.upkeep.oil = 18_fx;
        heavyReg.upkeep.ammo = 20_fx;
        heavyReg.upkeep.unitLimitConsumed = 1_fx;
        RegimentDef hqReg = regiment("reg_hq", {"hq_truck"}, 0_fx, 2_fx);
        RegimentDef inf = regiment("reg_inf", {"rifle", "rifle", "rifle", "mgun"}, 50_fx, 4_fx);
        inf.upkeep.ammo = 10_fx;
        inf.upkeep.unitLimitConsumed = 1_fx;
        d.regiments = {heavyReg, hqReg, inf};
        BuildingDef corps = building("corps", 10_fx, 10_fx, {"building", "corpshq"});
        corps.providesCorps = true;
        corps.health = 3500_fx;
        corps.production.money = 25_fx;
        corps.production.oil = 10_fx;
        corps.production.ammo = 20_fx;
        corps.recruits = {"reg_hq", "pool_truck", "depot_truck"};
        ResearchItemDef mg;
        mg.propertyIds = "tech_mg";
        mg.time = 6_fx;
        mg.cost.money = 100_fx;
        corps.research = {mg};
        BuildingDef depot = building("depot", 8_fx, 7_fx, {"building"});
        depot.production.oil = 25_fx;
        BuildingDef hq = building("hq", 10_fx, 8_fx, {"building", "armorhq"});
        hq.production.money = 25_fx;
        hq.production.oil = 5_fx;
        hq.production.ammo = 10_fx;
        hq.production.unitLimitProvided = 3_fx;
        hq.recruits = {"reg_inf", "reg_heavy"};
        ResearchItemDef heavyTech;
        heavyTech.propertyIds = "tech_heavy";
        heavyTech.time = 3_fx;
        heavyTech.cost.money = 150_fx;
        hq.research = {heavyTech};
        BuildingDef pool = building("pool", 8_fx, 7_fx, {"building"});
        d.buildings = {corps, depot, hq, pool};
        d.economy.startLocation.buildingLimitProvided = 5_fx;
        for (RegimentDef& g : d.regiments) {
            for (const std::string& s : g.slotIds) g.slots.push_back({ElementKind::Unit, d.unitIndex(s)});
        }
        tgi::Diagnostics diags;
        linkBases(d, diags);
        d.dataHash = hashBytes(serializeDefs(d));
        return d;
    }();
    return set;
}

nav::MapDef openMap(std::uint32_t size = 128) {
    nav::MapDef m;
    m.name = "economy-test";
    m.width = m.height = size;
    m.kind.assign(m.cellCount(), nav::TerrainKind::Ground);
    for (std::uint32_t y = 100; y < 106; ++y) {  // a pond
        for (std::uint32_t x = 100; x < 106; ++x) m.kind[y * size + x] = nav::TerrainKind::Water;
    }
    m.terrainId.assign(m.cellCount(), 0);
    m.terrainNames = {"open"};
    REQUIRE(m.finalize());
    return m;
}

const Angle kEast{};

struct Run {
    Sim sim;
    std::uint16_t seqs[2] = {0, 0};
    std::vector<SimEvent> log;
    std::vector<std::uint32_t> logTick;
    explicit Run(const nav::MapDef& m) : sim({7, &defs(), &m}) {}

    std::uint16_t seq(std::uint8_t p) { return seqs[p]++; }
    void step(std::vector<Command> cmds = {}) {
        for (Command& c : cmds) c.tick = sim.tick();
        sim.step(cmds);
        for (const SimEvent& e : sim.events()) {
            log.push_back(e);
            logTick.push_back(sim.tick() - 1);
        }
    }
    void steps(int n) {
        for (int i = 0; i < n; ++i) step();
    }
    std::size_t count(SimEvent::Type t) const {
        return static_cast<std::size_t>(std::count_if(log.begin(), log.end(), [t](const SimEvent& e) { return e.type == t; }));
    }
    bool rejected() const { return !log.empty() && std::any_of(log.end() - static_cast<std::ptrdiff_t>(sim.events().size()), log.end(),
                                                                  [](const SimEvent& e) { return e.type == SimEvent::Type::CommandRejected; }); }
    // Steps until an event of type `t` (at most `limit` ticks); the event, or null.
    const SimEvent* until(SimEvent::Type t, int limit = 3000) {
        const std::size_t from = log.size();
        for (int i = 0; i < limit; ++i) {
            step();
            for (std::size_t k = from; k < log.size(); ++k) {
                if (log[k].type == t) return &log[k];
            }
        }
        return nullptr;
    }
    Kingdom& kingdom(std::uint8_t p = 0) { return sim.world().kingdoms[p]; }
    std::vector<EntityId> structures(const char* ids) {
        std::vector<EntityId> out;
        const World& w = sim.world();
        for (std::size_t i = 0; i < w.structures.size(); ++i) {
            const EntityId id = w.structures.ids()[i];
            if (defs().buildings[w.defRefs.get(id)->index].ids == ids) out.push_back(id);
        }
        return out;
    }
    std::vector<EntityId> regiments(const char* ids) {
        std::vector<EntityId> out;
        const World& w = sim.world();
        for (std::size_t i = 0; i < w.regiments.size(); ++i) {
            const EntityId id = w.regiments.ids()[i];
            if (w.registry.alive(id) && defs().regiments[w.defRefs.get(id)->index].ids == ids) out.push_back(id);
        }
        return out;
    }
    std::size_t membersOf(EntityId org) const {
        std::size_t n = 0;
        for (const Member& m : sim.world().members.data()) n += m.organization == org;
        return n;
    }
    // A started player 0 with its Corps HQ at (40, 40).
    EntityId start() {
        step({makeStartPlayer(0, 0, seq(0), "corps", {40_fx, 40_fx}, kEast)});
        REQUIRE(structures("corps").size() == 1);
        return structures("corps")[0];
    }
    // Recruits regiment `ids` at `building` and waits until it is complete.
    EntityId recruit(EntityId building, const char* ids) {
        step({makeRecruit(0, 0, seq(0), building, ids)});
        REQUIRE_FALSE(rejected());
        const SimEvent* e = until(SimEvent::Type::Recruited);
        REQUIRE(e != nullptr);
        return e->entity;
    }
    // Unpacks truck regiment `org` at `at` and waits until the building is deployed.
    EntityId unpack(EntityId org, FVec2 at) {
        step({makeUnpack(0, 0, seq(0), org, at, kEast)});
        REQUIRE_FALSE(rejected());
        const SimEvent* e = until(SimEvent::Type::Unpacked);
        REQUIRE(e != nullptr);
        return e->entity;
    }
};

}  // namespace

// ------------------------------------------------------------------------------------------ data

TEST_CASE("bases data: trucks link to their buildings, HQ regiments and convoys; recruit lists resolve") {
    const DefSet& d = defs();
    const BuildingDef& hq = *d.building("hq");
    CHECK(hq.truck == d.unitIndex("hq_truck"));
    CHECK(hq.packedRegiment == d.regimentIndex("reg_hq"));
    const BuildingDef& pool = *d.building("pool");
    REQUIRE(pool.packedRegiment != kNoDef);
    CHECK(d.regiments[pool.packedRegiment].ids == "convoy_pool_truck");  // no regiment holds it: a convoy
    CHECK(d.regiments[pool.packedRegiment].convoy);
    CHECK(d.regiments[pool.packedRegiment].recruitTime == 7_fx);
    const BuildingDef& corps = *d.building("corps");
    REQUIRE(corps.recruitRefs.size() == 3);
    CHECK(d.regiments[corps.recruitRefs[1].regiment].ids == "convoy_pool_truck");
    CHECK(recruitCost(d, d.regimentIndex("reg_hq")) == 225_fx);  // the truck's cost
    CHECK(recruitCost(d, d.regimentIndex("reg_inf")) == 50_fx);
    CHECK(recruitPlan(d, d.regimentIndex("reg_inf")).size() == 4);
}

// --------------------------------------------------------------------------------------- economy

TEST_CASE("economy: StartPlayer gives a deployed Corps HQ and 500 money; income accrues per tick at the per-minute rate") {
    const nav::MapDef m = openMap();
    Run r(m);
    const EntityId corps = r.start();
    CHECK(r.kingdom().started);
    const Fixed start = r.kingdom().money;
    CHECK(abs(start - 500_fx - 25_fx / 600_fx) < 0.001_fx);  // 500, plus that tick's income
    const Structure* s = r.sim.world().structures.get(corps);
    CHECK(s->state == Structure::State::Deployed);
    const Ledger l = ledgerOf(r.sim, 0);
    CHECK(l.production.money == 25_fx);
    CHECK(l.oilNet() == 10_fx);
    CHECK(l.ammoNet() == 20_fx);
    CHECK(l.buildingLimit == 5_fx);
    CHECK(l.upkeep.buildingLimitConsumed == 1_fx);
    CHECK(l.moneyNet() == 25_fx);
    r.steps(600);  // one minute
    CHECK(abs(r.kingdom().money - start - 25_fx) < 0.001_fx);
    // A second start, or a start on top of a building, is refused.
    r.step({makeStartPlayer(0, 0, r.seq(0), "corps", {90_fx, 40_fx}, kEast)});
    CHECK(r.rejected());
    r.step({makeStartPlayer(0, 1, r.seq(1), "corps", {44_fx, 44_fx}, kEast)});
    CHECK(r.rejected());
}

TEST_CASE("economy: an oil deficit is bought with money (the manual's example: oil +10, heavy tank -18 -> -24 money/min)") {
    const nav::MapDef m = openMap();
    Run r(m);
    r.start();
    r.step({makeSpawnRegiment(0, 0, r.seq(0), "reg_heavy", {80_fx, 80_fx}, kEast)});
    const Ledger l = ledgerOf(r.sim, 0);
    CHECK(l.oilNet() == -8_fx);
    CHECK(l.ammoNet() == 0_fx);
    CHECK(l.shortfallCost == 24_fx);
    CHECK(l.moneyNet() == 1_fx);  // 25 - 24
    // Ammo short too: 2 money per ammo.
    r.step({makeSpawnRegiment(0, 0, r.seq(0), "reg_heavy", {80_fx, 90_fx}, kEast)});
    const Ledger l2 = ledgerOf(r.sim, 0);
    CHECK(l2.oilNet() == -26_fx);
    CHECK(l2.ammoNet() == -20_fx);
    CHECK(l2.shortfallCost == 26_fx * 3 + 20_fx * 2);
}

TEST_CASE("economy: money stays within 0 .. 99999; bankrupt while broke with a negative income, solvent again after") {
    const nav::MapDef m = openMap();
    Run r(m);
    r.start();
    r.step({makeDebugSetMoney(0, 0, r.seq(0), 200000_fx)});
    CHECK(r.kingdom().money == 99999_fx);
    r.step({makeDebugSetMoney(0, 0, r.seq(0), 1_fx), makeSpawnRegiment(0, 0, r.seq(0), "reg_heavy", {80_fx, 80_fx}, kEast),
            makeSpawnRegiment(0, 0, r.seq(0), "reg_heavy", {80_fx, 90_fx}, kEast)});
    CHECK(ledgerOf(r.sim, 0).moneyNet() < 0_fx);
    REQUIRE(r.until(SimEvent::Type::Bankrupt, 200) != nullptr);
    CHECK(r.kingdom().bankrupt);
    CHECK(r.kingdom().money == 0_fx);
    r.steps(50);
    CHECK(r.kingdom().money == 0_fx);  // never below zero
    for (const EntityId g : r.regiments("reg_heavy")) r.step({makeDebugDestroy(0, 0, r.seq(0), g)});
    r.steps(2);
    CHECK(r.count(SimEvent::Type::Solvent) == 1);
    CHECK_FALSE(r.kingdom().bankrupt);
    CHECK(r.kingdom().money > 0_fx);
}

// ------------------------------------------------------------------------------------- placement

TEST_CASE("placement: footprint from Size, snapped to cells and 90 degrees; map, ground, padding, spacing, one Corps HQ") {
    const nav::MapDef m = openMap();
    Run r(m);
    const DefSet& d = defs();
    const std::uint32_t corps = d.buildingIndex("corps"), depot = d.buildingIndex("depot");
    Placement p = placeBuilding(r.sim, corps, {40.3_fx, 39.8_fx}, Angle::fromDegrees(30_fx), 0);
    CHECK(p.ok);
    CHECK(p.rect.x0 == 35);
    CHECK(p.rect.x1 == 44);
    CHECK(p.rect.y0 == 35);
    CHECK(p.center == FVec2{40_fx, 40_fx});
    CHECK(p.facing == kEast);  // axis_locked: 30 degrees snaps to 0
    CHECK_FALSE(placeBuilding(r.sim, depot, {3_fx, 40_fx}, kEast, 0).ok);        // padding off the map
    CHECK_FALSE(placeBuilding(r.sim, depot, {102_fx, 102_fx}, kEast, 0).ok);    // in the pond
    CHECK(std::string(placeBuilding(r.sim, depot, {97_fx, 97_fx}, kEast, 0).why) == "not buildable ground");  // padding in the pond
    r.start();
    // 8x8 depot next to the 10x10 corps (cells 35..44): its cells must stay 2 away from 44.
    CHECK_FALSE(placeBuilding(r.sim, depot, {49_fx, 40_fx}, kEast, 0).ok);  // cells 45..52: touching
    CHECK(std::string(placeBuilding(r.sim, depot, {50_fx, 40_fx}, kEast, 0).why) == "too close to a building");
    CHECK(placeBuilding(r.sim, depot, {51_fx, 40_fx}, kEast, 0).ok);        // cells 47..54: a 2-cell gap
    CHECK(std::string(placeBuilding(r.sim, corps, {80_fx, 80_fx}, kEast, 0).why) == "only one Corps HQ");
    CHECK(placeBuilding(r.sim, corps, {80_fx, 80_fx}, kEast, 1).ok);        // another kingdom may
    // The corps is stamped into the nav grid.
    const nav::NavGrid& g = r.sim.nav()->grid;
    CHECK(g.stampCount(g.index(35, 35)) == 1);
    CHECK(g.stampCount(g.index(44, 44)) == 1);
    CHECK(g.stampCount(g.index(45, 44)) == 0);
}

// ---------------------------------------------------------------------------------------- recruit

TEST_CASE("recruit: paid when queued; units come out one by one, then the regiment heads for the deployment point") {
    const nav::MapDef m = openMap();
    Run r(m);
    const EntityId corps = r.start();
    const EntityId hqReg = r.recruit(corps, "reg_hq");
    CHECK(r.kingdom().money < 500_fx - 225_fx + 1_fx);
    const EntityId hq = r.unpack(hqReg, {80_fx, 40_fx});
    const std::uint32_t t0 = r.sim.tick();
    const Fixed before = r.kingdom().money;
    r.step({makeRecruit(0, 0, r.seq(0), hq, "reg_inf")});
    REQUIRE_FALSE(r.rejected());
    CHECK(r.kingdom().money < before - 49_fx);
    const std::vector<EntityId> forming = r.regiments("reg_inf");
    REQUIRE(forming.size() == 1);
    CHECK(r.membersOf(forming[0]) == 0);   // the regiment exists, empty
    // 4 s for 4 units: one every 10 ticks, the first after one interval.
    std::vector<std::uint32_t> arrivals;
    for (int i = 0; i < 60; ++i) {
        const std::size_t n = r.membersOf(forming[0]);
        r.step();
        if (r.membersOf(forming[0]) > n) arrivals.push_back(r.sim.tick() - 1 - t0);
    }
    CHECK(arrivals == std::vector<std::uint32_t>{10, 20, 30, 40});
    CHECK(r.count(SimEvent::Type::Recruited) == 2);  // the HQ truck, then this
    CHECK(r.sim.world().structures.get(hq)->queue.empty());
    // They form up at the deployment point (14 ahead of the HQ's centre).
    r.steps(300);
    CHECK(distance(r.sim.world().transforms.get(forming[0])->position, {94_fx, 40_fx}) < 2_fx);
}

TEST_CASE("recruit: unit and building limits (reservations included), queue of five, cancel refunds, money") {
    const nav::MapDef m = openMap();
    Run r(m);
    const EntityId corps = r.start();
    // The corps provides no unit limit: nothing that consumes one can be recruited at all; here the
    // HQ (unit limit 3) does it. Building limit 5: corps 1 + HQ truck 1.
    const EntityId hq = r.unpack(r.recruit(corps, "reg_hq"), {80_fx, 40_fx});
    CHECK(ledgerOf(r.sim, 0).unitLimit == 3_fx);
    r.step({makeDebugSetMoney(0, 0, r.seq(0), 5000_fx)});
    for (int i = 0; i < 3; ++i) {
        r.step({makeRecruit(0, 0, r.seq(0), hq, "reg_inf")});
        CHECK_FALSE(r.rejected());
    }
    r.step({makeRecruit(0, 0, r.seq(0), hq, "reg_inf")});  // a fourth: over the unit limit (queued ones count)
    CHECK(r.rejected());
    CHECK(ledgerOf(r.sim, 0).upkeep.unitLimitConsumed == 3_fx);
    // Cancel the third (not started yet): refunded.
    const Fixed money = r.kingdom().money;
    r.step({makeCancelRecruit(0, 0, r.seq(0), hq, 2)});
    CHECK_FALSE(r.rejected());
    CHECK(r.kingdom().money > money + 49_fx);
    r.step({makeCancelRecruit(0, 0, r.seq(0), hq, 0)});  // the one coming out can't be cancelled
    CHECK(r.rejected());
    // Building limit: 2 used of 5; three trucks more fill it.
    r.step({makeRecruit(0, 0, r.seq(0), corps, "pool_truck")});
    r.step({makeRecruit(0, 0, r.seq(0), corps, "depot_truck")});
    r.step({makeRecruit(0, 0, r.seq(0), corps, "depot_truck")});
    CHECK_FALSE(r.rejected());
    r.step({makeRecruit(0, 0, r.seq(0), corps, "depot_truck")});
    CHECK(r.rejected());
    // Not enough money.
    r.step({makeDebugSetMoney(0, 0, r.seq(0), 10_fx), makeRecruit(0, 0, r.seq(0), hq, "reg_inf")});
    CHECK(r.rejected());
    // Not in its RecruitComponent.
    r.step({makeDebugSetMoney(0, 0, r.seq(0), 5000_fx), makeRecruit(0, 0, r.seq(0), corps, "reg_inf")});
    CHECK(r.rejected());
    // Someone else's building.
    r.step({makeRecruit(0, 1, r.seq(1), hq, "reg_inf")});
    CHECK(r.rejected());
}

TEST_CASE("recruit: required structures (deployed) and required properties (research applying to the recruiter)") {
    const nav::MapDef m = openMap();
    Run r(m);
    const EntityId corps = r.start();
    const EntityId hq = r.unpack(r.recruit(corps, "reg_hq"), {80_fx, 40_fx});
    r.step({makeDebugSetMoney(0, 0, r.seq(0), 5000_fx)});
    r.step({makeRecruit(0, 0, r.seq(0), hq, "reg_heavy")});
    CHECK(r.rejected());  // no motor pool, no tech_heavy
    // tech_heavy: researched at the HQ (it applies to armorhq, which the HQ has).
    r.step({makeResearch(0, 0, r.seq(0), hq, "tech_heavy")});
    REQUIRE_FALSE(r.rejected());
    CHECK(r.kingdom().money < 4851_fx);  // 150 paid
    REQUIRE(r.until(SimEvent::Type::ResearchDone, 100) != nullptr);
    const std::vector<std::uint32_t> props = effectiveProperties(r.sim, hq);
    CHECK(std::binary_search(props.begin(), props.end(), defs().propertyIndex("tech_heavy")));
    const std::vector<std::uint32_t> corpsProps = effectiveProperties(r.sim, corps);
    CHECK_FALSE(std::binary_search(corpsProps.begin(), corpsProps.end(), defs().propertyIndex("tech_heavy")));
    r.step({makeRecruit(0, 0, r.seq(0), hq, "reg_heavy")});
    CHECK(r.rejected());  // still no motor pool
    // A motor pool truck, unpacked: now it can.
    const EntityId poolTruck = r.recruit(corps, "pool_truck");
    const EntityId pool = r.unpack(poolTruck, {40_fx, 80_fx});
    CHECK(r.sim.world().structures.get(pool) != nullptr);
    r.step({makeRecruit(0, 0, r.seq(0), hq, "reg_heavy")});
    CHECK_FALSE(r.rejected());
}

// --------------------------------------------------------------------------------------- research

TEST_CASE("research: paid at the start, granted after research_time; once only; cancel refunds; AV +4 for mg units") {
    const nav::MapDef m = openMap();
    Run r(m);
    const EntityId corps = r.start();
    r.step({makeResearch(0, 0, r.seq(0), corps, "tech_mg")});
    REQUIRE_FALSE(r.rejected());
    CHECK(r.kingdom().money < 401_fx);
    r.step({makeResearch(0, 0, r.seq(0), corps, "tech_mg")});  // busy
    CHECK(r.rejected());
    r.step({makeCancelResearch(0, 0, r.seq(0), corps)});
    CHECK(r.kingdom().money > 500_fx);  // refunded (plus income)
    r.step({makeResearch(0, 0, r.seq(0), corps, "tech_mg")});
    const std::uint32_t t0 = r.sim.tick() - 1;
    const SimEvent* done = r.until(SimEvent::Type::ResearchDone, 100);
    REQUIRE(done != nullptr);
    CHECK(r.logTick.back() - t0 == 60);  // 6 s
    CHECK(done->detail == defs().propertyIndex("tech_mg"));
    CHECK(r.kingdom().properties == std::vector<std::uint32_t>{defs().propertyIndex("tech_mg")});
    r.step({makeResearch(0, 0, r.seq(0), corps, "tech_mg")});  // already have it
    CHECK(r.rejected());
    // It applies to units with "mg": +4 AV on the mgun, nothing on riflemen.
    r.step({makeSpawnRegiment(0, 0, r.seq(0), "reg_inf", {80_fx, 80_fx}, kEast)});
    const World& w = r.sim.world();
    EntityId mgun, rifle;
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        const EntityId u = w.members.ids()[i];
        (defs().units[w.defRefs.get(u)->index].ids == "mgun" ? mgun : rifle) = u;
    }
    CHECK(researchAttackValue(r.sim, mgun) == 4_fx);
    CHECK(researchAttackValue(r.sim, rifle) == 0_fx);
    const EntityId org = w.members.get(mgun)->organization;
    CHECK(attackMultiplierOf(r.sim, org, 4_fx) > attackMultiplierOf(r.sim, org));
}

// ------------------------------------------------------------------------------------ pack/unpack

TEST_CASE("pack / unpack: health floor, timers, stamping, health and XP carried, re-ordering cancels, blocked spot fails") {
    const nav::MapDef m = openMap();
    Run r(m);
    const EntityId corps = r.start();
    const EntityId hqReg = r.recruit(corps, "reg_hq");
    r.step({makeDebugSetExperience(0, 0, r.seq(0), {hqReg}, 120_fx)});
    // Unpack: drives there; the footprint is stamped from the start of unpacking.
    r.step({makeUnpack(0, 0, r.seq(0), hqReg, {80_fx, 40_fx}, kEast)});
    REQUIRE_FALSE(r.rejected());
    CHECK(r.sim.world().deploys.has(hqReg));
    const nav::NavGrid& g = r.sim.nav()->grid;
    CHECK(g.stampCount(g.index(80, 40)) == 0);
    REQUIRE(r.until(SimEvent::Type::Spawned) != nullptr);  // the building appears (unpacking)
    const EntityId hq = r.structures("hq").at(0);
    CHECK(r.sim.world().structures.get(hq)->state == Structure::State::Unpacking);
    CHECK(g.stampCount(g.index(80, 40)) == 1);
    CHECK(r.sim.world().structures.get(hq)->xp == 120_fx);
    r.step();
    CHECK_FALSE(r.sim.world().registry.alive(hqReg));  // the truck regiment gave way
    const std::uint32_t t0 = r.sim.tick();
    REQUIRE(r.until(SimEvent::Type::Unpacked, 200) != nullptr);
    CHECK(r.sim.tick() - t0 == 79);  // 8 s from the first unpacking tick
    // Below half health it can't pack.
    Health* h = r.sim.world().health.get(hq);
    h->current = h->max * 0.4_fx;
    r.step({makePack(0, 0, r.seq(0), hq)});
    CHECK(r.rejected());
    h = r.sim.world().health.get(hq);
    h->current = h->max * 0.8_fx;
    // Nor while recruiting.
    r.step({makeRecruit(0, 0, r.seq(0), hq, "reg_inf"), makePack(0, 0, r.seq(0), hq)});
    CHECK(r.rejected());
    REQUIRE(r.until(SimEvent::Type::Recruited) != nullptr);
    r.step({makePack(0, 0, r.seq(0), hq)});
    REQUIRE_FALSE(r.rejected());
    CHECK(r.sim.world().structures.get(hq)->state == Structure::State::Packing);
    CHECK(ledgerOf(r.sim, 0).production.unitLimitProvided == 0_fx);  // packing: no production (the truck doesn't exist yet)
    const SimEvent* packed = r.until(SimEvent::Type::Packed, 200);
    REQUIRE(packed != nullptr);
    const EntityId again = packed->other;
    r.step();
    CHECK_FALSE(r.sim.world().registry.alive(hq));
    CHECK(g.stampCount(g.index(80, 40)) == 0);
    REQUIRE(r.regiments("reg_hq") == std::vector<EntityId>{again});
    for (std::size_t i = 0; i < r.sim.world().members.size(); ++i) {
        if (r.sim.world().members.data()[i].organization != again) continue;
        const Health* th = r.sim.world().health.get(r.sim.world().members.ids()[i]);
        CHECK(abs(th->current - th->max * 0.8_fx) < 0.001_fx);  // the building's health fraction
    }
    CHECK(r.sim.world().engagements.get(again)->xp == 120_fx);
    // Re-ordered elsewhere on the way: the unpack is dropped.
    r.step({makeUnpack(0, 0, r.seq(0), again, {80_fx, 80_fx}, kEast)});
    r.step({makeMoveGroup(0, 0, r.seq(0), {again}, {60_fx, 60_fx})});
    r.step();
    CHECK_FALSE(r.sim.world().deploys.has(again));
    // The spot taken before it arrives: DeployFailed, no building.
    r.step({makeUnpack(0, 0, r.seq(0), again, {80_fx, 80_fx}, kEast)});
    r.step({makeDebugPlaceBlocker(0, 1, r.seq(1), {80_fx, 80_fx}, {1_fx, 1_fx})});
    REQUIRE(r.until(SimEvent::Type::DeployFailed, 600) != nullptr);
    CHECK(r.structures("hq").empty());
    CHECK(r.sim.world().registry.alive(again));
    // Can't unpack a regiment without a truck.
    r.step({makeSpawnRegiment(0, 0, r.seq(0), "reg_inf", {20_fx, 90_fx}, kEast)});
    r.step({makeUnpack(0, 0, r.seq(0), r.regiments("reg_inf").back(), {20_fx, 110_fx}, kEast)});
    CHECK(r.rejected());
}

// ---------------------------------------------------------------------------------- save / load

TEST_CASE("bases save / load: mid-recruit, mid-research, mid-unpack states continue identically") {
    const nav::MapDef m = openMap();
    Run r(m);
    const EntityId corps = r.start();
    const EntityId hq = r.unpack(r.recruit(corps, "reg_hq"), {80_fx, 40_fx});
    r.step({makeDebugSetMoney(0, 0, r.seq(0), 3000_fx), makeRecruit(0, 0, r.seq(0), hq, "reg_inf"),
            makeRecruit(0, 0, r.seq(0), hq, "reg_inf"), makeResearch(0, 0, r.seq(0), corps, "tech_mg"),
            makeRecruit(0, 0, r.seq(0), corps, "depot_truck")});
    r.steps(25);
    const std::vector<std::uint8_t> saved = saveState(r.sim);
    Sim copy({7, &defs(), &m});
    std::string err;
    REQUIRE_MESSAGE(loadState(copy, saved, &err), err);
    CHECK(stateHash(copy) == stateHash(r.sim));
    for (int i = 0; i < 300; ++i) {
        r.step();
        copy.step();
    }
    CHECK(stateHash(copy) == stateHash(r.sim));
    CHECK(copy.world().kingdoms[0].properties == r.kingdom().properties);
}

TEST_CASE("bases commands encode and decode") {
    const std::vector<Command> cmds = {
        makeStartPlayer(3, 1, 0, "gbr_corps", {12.5_fx, -3_fx}, Angle::fromUnits(1234)),
        makeRecruit(3, 1, 1, EntityId{77}, "regiment_gbr_infantry"),
        makeCancelRecruit(3, 1, 2, EntityId{77}, 3),
        makeUnpack(3, 1, 3, EntityId{99}, {40_fx, 41_fx}, Angle::fromUnits(16384)),
        makePack(3, 1, 4, EntityId{77}),
        makeResearch(3, 1, 5, EntityId{77}, "tech_foraging"),
        makeCancelResearch(3, 1, 6, EntityId{77}),
        makeDebugSetMoney(3, 1, 7, 123.25_fx),
        makeDebugGrantProperty(3, 1, 8, "tech_mg"),
    };
    for (const Command& c : cmds) {
        std::vector<std::uint8_t> bytes;
        encodeCommand(c, bytes);
        std::size_t pos = 0;
        Command back;
        REQUIRE(decodeCommand(bytes, pos, back));
        CHECK(pos == bytes.size());
        back.tick = c.tick;
        back.seq = c.seq;
        CHECK(back == c);
    }
}

TEST_CASE("committed economy replay plays identically on this compiler (fixture: start, recruit, unpack, research)") {
    // sim/tests/fixtures/economy_m6.rpl: these test defs, a 128 map with a pond, 1500 ticks of the
    // scenario below, recorded with MSVC x64. CORPS_WRITE_FIXTURES=1 re-records it.
    const nav::MapDef m = openMap();
    Replay rep;
    rep.header.simVersion = simVersion();
    rep.header.dataHash = defs().dataHash;
    rep.header.mapSpec = m.name;
    rep.header.mapHash = m.mapHash;
    rep.header.matchSeed = 7;
    rep.header.tickCount = 1500;
    rep.header.players = {"A", "B"};
    rep.commands = {makeStartPlayer(0, 0, 0, "corps", {40_fx, 40_fx}, kEast), makeStartPlayer(0, 1, 0, "corps", {90_fx, 90_fx}, kEast)};
    {
        // Entity ids depend only on the commands so far: find them on a scratch run.
        Sim scratch({7, &defs(), &m});
        scratch.step(rep.commands);
        const std::vector<EntityId> corps = scratch.world().structures.ids();
        REQUIRE(corps.size() == 2);
        rep.commands.push_back(makeRecruit(1, 0, 1, corps[0], "reg_hq"));
        rep.commands.push_back(makeResearch(1, 0, 2, corps[0], "tech_mg"));
        rep.commands.push_back(makeRecruit(1, 1, 1, corps[1], "depot_truck"));
        CommandQueue q;
        for (const Command& c : rep.commands) q.schedule(c, 1);
        for (std::uint32_t t = 1; t < 100; ++t) scratch.step(q.take(t));
        const std::vector<EntityId> regs = scratch.world().regiments.ids();
        REQUIRE(regs.size() == 2);
        rep.commands.push_back(makeUnpack(100, 0, 3, regs[0], {80_fx, 40_fx}, kEast));
        rep.commands.push_back(makeUnpack(100, 1, 2, regs[1], {60_fx, 100_fx}, kEast));
    }
    RunOptions opt;
    opt.map = &m;
    const std::string path = CORPS_SOURCE_DIR "/sim/tests/fixtures/economy_m6.rpl";
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
    CHECK(recorded.commands == rep.commands);
    const RunResult run = runReplay(recorded, &defs(), opt);
    CHECK_FALSE(divergenceFromRecorded(recorded, run).found);
    CHECK(run.finalHash == recorded.hashes.back().hash);
    // The film has what part 1 is about.
    Sim sim({7, &defs(), &m});
    CommandQueue q;
    for (const Command& c : recorded.commands) q.schedule(c, 0);
    std::size_t recruited = 0, unpacked = 0, research = 0, failed = 0, rejected = 0;
    for (std::uint32_t t = 0; t < recorded.header.tickCount; ++t) {
        sim.step(q.take(t));
        for (const SimEvent& e : sim.events()) {
            recruited += e.type == SimEvent::Type::Recruited;
            unpacked += e.type == SimEvent::Type::Unpacked;
            research += e.type == SimEvent::Type::ResearchDone;
            failed += e.type == SimEvent::Type::DeployFailed;
            if (e.type == SimEvent::Type::DeployFailed) {
                const Transform* tr = sim.world().transforms.get(e.entity);
                const Order* o = sim.world().orders.get(e.entity);
                MESSAGE("tick " << t << ": deploy failed, player " << int(e.player) << " anchor " << tr->position.x.floorInt() << ","
                                << tr->position.y.floorInt() << " goal " << o->goal.x.floorInt() << "," << o->goal.y.floorInt()
                                << " blocked " << o->blocked);
            }
            if (e.type == SimEvent::Type::CommandRejected) {
                ++rejected;
                MESSAGE("tick " << t << ": rejected command type " << static_cast<int>(e.command) << " of player " << int(e.player));
            }
        }
    }
    CHECK(failed == 0);
    CHECK(rejected == 0);
    CHECK(recruited == 2);
    CHECK(unpacked == 2);
    CHECK(research == 1);
    CHECK(sim.world().kingdoms[0].money > Fixed{});
}
