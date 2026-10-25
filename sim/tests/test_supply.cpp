// M6 part 2: supply zones (allies, relays, siege), healing, free replacement keeping XP, the
// bankrupt effects, supply_required production, recruiting in supply, map cities and capture,
// elimination, team and city-ratio victory, save / load, command encoding, map things, and a
// committed replay fixture.
//
// Uses a small hand-built DefSet (no game data needed).

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>

#include "core/sim.hpp"
#include "data/def_compiler.hpp"
#include "data/defs.hpp"
#include "nav/map_def.hpp"
#include "nav/nav_world.hpp"
#include "session/bytes.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"
#include "systems/bases.hpp"
#include "systems/economy.hpp"
#include "systems/supply.hpp"
#include "test_env.hpp"
#include "vfs/path.hpp"

using namespace corps;
using namespace corps::fixed_literals;

namespace {

UnitDef unit(std::string ids, Fixed health) {
    UnitDef u;
    u.ids = std::move(ids);
    u.moveClass = UnitMoveClass::Foot;
    u.speedMax = 3_fx;
    u.turnType = TurnType::Instant;
    u.blockingRadius = 0.4_fx;
    u.health = health;
    u.resupplyRate = 0.3_fx;  // as the data's infantry
    return u;
}

UnitDef truck(std::string ids, std::string unpackTo) {
    UnitDef u = unit(std::move(ids), 500_fx);
    u.moveClass = UnitMoveClass::Wheeled;
    u.category = "TRUCK";
    u.blockingRadius = 0.65_fx;
    u.unpackToIds = std::move(unpackTo);
    u.unpackingLength = 1_fx;
    u.upkeep.buildingLimitConsumed = 1_fx;
    u.resupplyRate = 0.1_fx;
    return u;
}

BuildingDef building(std::string ids, Fixed size) {
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
    b.packingLength = 1_fx;
    b.unpackingLength = 1_fx;
    b.creationOffset = {7_fx, 0_fx};
    b.deploymentOffset = {12_fx, 0_fx};
    b.upkeep.buildingLimitConsumed = 1_fx;
    return b;
}

// corps (Corps HQ, 8x8, supply 40, +25 money; packs onto corps_truck; recruits the relay and mine trucks), relay (supply
// depot, 4x4, a relay of range 20), mine (4x4, +30 money, supply_required), city (8x8, zone 30,
// supply 35, +15 money, captureable, indestructible). reg_inf: 4 riflemen (100 health), recruit
// time 8 s (a replacement every 2 s), shortfall 0.01/s, XP levels at 0 and 10 (x1.5 health).
// reg_costly: one rifleman, 400 money upkeep (it makes its kingdom bankrupt).
DefSet makeDefs(bool supplyUnderSiege) {
    DefSet d;
    d.properties = {PropertyDef{"corpshq"}};
    d.units = {truck("corps_truck", "corps"), truck("mine_truck", "mine"), truck("relay_truck", "relay"), unit("rifle", 100_fx)};
    RegimentDef inf;
    inf.ids = "reg_inf";
    inf.slotIds = {"rifle", "rifle", "rifle", "rifle"};
    inf.recruitTime = 8_fx;
    inf.shortfallDamageRate = 0.01_fx;
    ExperienceLevelDef l0, l1;
    l0.required = 0_fx;
    l0.healthMultiplier = 1_fx;
    l1.required = 10_fx;
    l1.healthMultiplier = 1.5_fx;
    inf.experienceLevels = {l0, l1};
    RegimentDef costly;
    costly.ids = "reg_costly";
    costly.slotIds = {"rifle"};
    costly.recruitTime = 2_fx;
    costly.shortfallDamageRate = 0.01_fx;
    costly.upkeep.money = 400_fx;
    d.regiments = {inf, costly};
    BuildingDef corps = building("corps", 8_fx);
    corps.providesCorps = true;
    corps.properties = {"corpshq"};
    corps.supplyRange = 40_fx;
    corps.production.money = 25_fx;
    corps.recruits = {"mine_truck", "relay_truck"};
    BuildingDef relay = building("relay", 4_fx);
    relay.supplyRange = 20_fx;
    relay.supplyRelay = true;
    relay.upkeep.buildingLimitConsumed = 0_fx;
    BuildingDef mine = building("mine", 4_fx);
    mine.production.money = 30_fx;
    mine.supplyRequired = true;
    BuildingDef city = building("city", 8_fx);
    city.mobile = false;
    city.health = 500_fx;
    city.cityZoneRadius = 30_fx;
    city.captureable = true;
    city.destructible = false;
    city.supplyRange = 35_fx;
    city.production.money = 15_fx;
    city.upkeep.buildingLimitConsumed = 0_fx;
    d.buildings = {city, corps, mine, relay};
    d.economy.startLocation.buildingLimitProvided = 10_fx;
    d.economy.supplyUnderSiege = supplyUnderSiege;
    for (RegimentDef& g : d.regiments) {
        for (const std::string& s : g.slotIds) g.slots.push_back({ElementKind::Unit, d.unitIndex(s)});
    }
    tgi::Diagnostics diags;
    linkBases(d, diags);
    d.dataHash = hashBytes(serializeDefs(d));
    return d;
}

const DefSet& defs() {
    static const DefSet set = makeDefs(true);
    return set;
}
const DefSet& strictDefs() {  // SupplyProvideUnderSiege = false: a besieged zone switches off
    static const DefSet set = makeDefs(false);
    return set;
}

// 128 x 128 open ground; neutral cities "A" at (30, 100) and "B" at (100, 30).
const FVec2 kCityA{30_fx, 100_fx};
const FVec2 kCityB{100_fx, 30_fx};
nav::MapDef cityMap() {
    nav::MapDef m;
    m.name = "supply-test";
    m.width = m.height = 128;
    m.kind.assign(m.cellCount(), nav::TerrainKind::Ground);
    m.terrainId.assign(m.cellCount(), 0);
    m.terrainNames = {"open"};
    m.things = {{"city", kCityA, Angle{}, -1, "name_1"}, {"tree", {60_fx, 60_fx}, Angle{}, -1, ""}, {"city", kCityB, Angle{}, -1, "name_2"}};
    REQUIRE(m.finalize());
    return m;
}

const Angle kEast{};

struct Run {
    Sim sim;
    std::uint16_t seqs[4] = {0, 0, 0, 0};
    std::vector<SimEvent> log;
    Run(const nav::MapDef& m, const DefSet& d = defs()) : sim({7, &d, &m}) {}

    const DefSet& d() const { return *sim.config().defs; }
    std::uint16_t seq(std::uint8_t p) { return seqs[p]++; }
    void step(std::vector<Command> cmds = {}) {
        for (Command& c : cmds) c.tick = sim.tick();
        sim.step(cmds);
        log.insert(log.end(), sim.events().begin(), sim.events().end());
    }
    void steps(int n) {
        for (int i = 0; i < n; ++i) step();
    }
    bool rejected() const {
        return std::any_of(sim.events().begin(), sim.events().end(), [](const SimEvent& e) { return e.type == SimEvent::Type::CommandRejected; });
    }
    std::size_t count(SimEvent::Type t) const {
        return static_cast<std::size_t>(std::count_if(log.begin(), log.end(), [t](const SimEvent& e) { return e.type == t; }));
    }
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
    std::vector<EntityId> structures(const char* ids) const {
        std::vector<EntityId> out;
        const World& w = sim.world();
        for (std::size_t i = 0; i < w.structures.size(); ++i) {
            if (d().buildings[w.defRefs.get(w.structures.ids()[i])->index].ids == ids) out.push_back(w.structures.ids()[i]);
        }
        return out;
    }
    EntityId start(std::uint8_t player, FVec2 at) {
        step({makeStartPlayer(0, player, seq(player), "corps", at, kEast)});
        REQUIRE_FALSE(rejected());
        for (const EntityId id : structures("corps")) {
            if (sim.world().structures.get(id)->player == player) return id;
        }
        FAIL("no corps");
        return {};
    }
    EntityId spawn(std::uint8_t player, const char* ids, FVec2 at) {
        step({makeSpawnRegiment(0, player, seq(player), ids, at, kEast)});
        REQUIRE_FALSE(rejected());
        for (const SimEvent& e : sim.events()) {
            if (e.type == SimEvent::Type::Spawned && sim.world().regiments.has(e.entity)) return e.entity;
        }
        FAIL("no regiment");
        return {};
    }
    std::vector<EntityId> members(EntityId org) const {
        std::vector<EntityId> out;
        const World& w = sim.world();
        for (std::size_t i = 0; i < w.members.size(); ++i) {
            const EntityId id = w.members.ids()[i];
            if (w.members.data()[i].organization == org && w.registry.alive(id)) out.push_back(id);
        }
        return out;
    }
    const Supply& supply(EntityId org) const {
        const Supply* s = sim.world().supplies.get(org);
        REQUIRE(s != nullptr);
        return *s;
    }
    Fixed health(EntityId e) const { return sim.world().health.get(e)->current; }
    // A truck recruited at the Corps HQ, unpacked at `at`; the building.
    EntityId deploy(std::uint8_t player, EntityId corps, const char* truckIds, FVec2 at) {
        step({makeRecruit(0, player, seq(player), corps, truckIds)});
        REQUIRE_FALSE(rejected());
        const SimEvent* rec = until(SimEvent::Type::Recruited);
        REQUIRE(rec != nullptr);
        const EntityId org = rec->entity;
        step({makeUnpack(0, player, seq(player), org, at, kEast)});
        REQUIRE_FALSE(rejected());
        const SimEvent* up = until(SimEvent::Type::Unpacked);
        REQUIRE(up != nullptr);
        return up->entity;
    }
};

}  // namespace

// ------------------------------------------------------------------------------------------ zones

TEST_CASE("supply: the Corps HQ's zone supplies its regiments; outside it, nothing") {
    const nav::MapDef m = cityMap();
    Run r(m);
    const EntityId corps = r.start(0, {30_fx, 30_fx});
    const EntityId in = r.spawn(0, "reg_inf", {55_fx, 30_fx});   // 25 from the HQ (range 40)
    const EntityId out = r.spawn(0, "reg_inf", {85_fx, 70_fx});  // 68 away
    r.step();
    CHECK(r.supply(in).inSupply);
    CHECK_FALSE(r.supply(in).besieged);
    CHECK_FALSE(r.supply(out).inSupply);
    CHECK(r.sim.world().structures.get(corps)->inSupply);  // in its own zone
    const std::vector<SupplyZone> zones = supplyZones(r.sim);
    REQUIRE(zones.size() == 1);  // neutral cities supply nobody
    CHECK(zones[0].source == corps);
    CHECK(zones[0].radius == 40_fx);
    CHECK(zones[0].active);
}

TEST_CASE("supply: allies share zones (SetMatchRules teams at tick 0 only)") {
    const nav::MapDef m = cityMap();
    Run r(m);
    r.step({makeSetMatchRules(0, 0, r.seq(0), {0, 0}, 0_fx, true), makeStartPlayer(0, 0, r.seq(0), "corps", {30_fx, 30_fx}, kEast)});
    REQUIRE_FALSE(r.rejected());
    CHECK(r.sim.world().kingdoms[1].team == 0);
    const EntityId ally = r.spawn(1, "reg_inf", {55_fx, 30_fx});
    const EntityId enemy = r.spawn(2, "reg_inf", {30_fx, 55_fx});
    r.step();
    CHECK(r.supply(ally).inSupply);
    CHECK_FALSE(r.supply(enemy).inSupply);
    r.step({makeSetMatchRules(0, 0, r.seq(0), {0, 1}, 0_fx, true)});
    CHECK(r.rejected());  // after tick 0
    // Invalid rules are refused at tick 0 too.
    Run q(m);
    q.step({makeSetMatchRules(0, 0, q.seq(0), {0, 9}, 0_fx, true)});
    CHECK(q.rejected());
}

TEST_CASE("supply: a relay supplies only while it stands in another active zone of its team") {
    const nav::MapDef m = cityMap();
    Run r(m);
    const EntityId corps = r.start(0, {30_fx, 30_fx});
    r.step({makeDebugSetMoney(0, 0, r.seq(0), 5000_fx)});
    const EntityId far = r.deploy(0, corps, "relay_truck", {85_fx, 60_fx});   // 62 from the HQ
    const EntityId near = r.deploy(0, corps, "relay_truck", {60_fx, 40_fx});  // 32 from the HQ
    const std::vector<SupplyZone> zones = supplyZones(r.sim);
    REQUIRE(zones.size() == 3);
    for (const SupplyZone& z : zones) {
        if (z.source == far) CHECK_FALSE(z.active);
        if (z.source == near) CHECK(z.active);
    }
    // A regiment past the HQ's range but inside the linked relay's is in supply.
    const EntityId org = r.spawn(0, "reg_inf", {74_fx, 44_fx});  // 44.7 from the HQ, 14.6 from the relay
    r.step();
    CHECK(r.supply(org).inSupply);
}

// ------------------------------------------------------------------------- healing, replacement

TEST_CASE("supply: units heal resupply_rate x max health per minute in supply, not outside") {
    const nav::MapDef m = cityMap();
    Run r(m);
    r.start(0, {30_fx, 30_fx});
    const EntityId in = r.spawn(0, "reg_inf", {55_fx, 30_fx});
    const EntityId out = r.spawn(0, "reg_inf", {85_fx, 70_fx});
    const EntityId a = r.members(in)[0];
    const EntityId b = r.members(out)[0];
    r.step({makeDebugSetHealth(0, 0, r.seq(0), a, 40_fx), makeDebugSetHealth(0, 0, r.seq(0), b, 40_fx)});
    REQUIRE_FALSE(r.rejected());
    r.steps(100);  // 10 s: +30/min x 1/6 (and the tick it was set)
    CHECK(abs(r.health(a) - 45.05_fx) < 0.01_fx);
    CHECK(r.health(b) == 40_fx);
    r.steps(1500);
    CHECK(r.health(a) == 100_fx);  // capped at max
}

TEST_CASE("supply: a dead unit is replaced for free every recruit_time / units, keeping the regiment's XP and level") {
    const nav::MapDef m = cityMap();
    Run r(m);
    r.start(0, {30_fx, 30_fx});
    const EntityId org = r.spawn(0, "reg_inf", {55_fx, 30_fx});
    const EntityId out = r.spawn(0, "reg_inf", {85_fx, 70_fx});
    r.step({makeDebugSetExperience(0, 0, r.seq(0), {org}, 12_fx)});  // level 1: x1.5 health
    const std::vector<EntityId> before = r.members(org);
    REQUIRE(before.size() == 4);
    const std::uint16_t lostSlot = r.sim.world().members.get(before[2])->slot;
    r.step({makeDebugDestroy(0, 0, r.seq(0), before[2]), makeDebugDestroy(0, 0, r.seq(0), before[3]),
            makeDebugDestroy(0, 0, r.seq(0), r.members(out)[0])});
    r.step();
    CHECK(r.members(org).size() == 2);
    const SimEvent* e = r.until(SimEvent::Type::Replaced, 100);
    REQUIRE(e != nullptr);
    CHECK(e->other == org);
    CHECK(r.sim.world().members.get(e->entity)->slot == lostSlot);           // the lowest missing element first
    CHECK(r.sim.world().health.get(e->entity)->max == 150_fx);               // joins at its level
    CHECK(r.sim.world().engagements.get(org)->xp == 12_fx);                  // XP kept
    REQUIRE(r.until(SimEvent::Type::Replaced, 30) != nullptr);               // 2 s later, the next
    CHECK(r.members(org).size() == 4);
    r.steps(100);
    CHECK(r.count(SimEvent::Type::Replaced) == 2);  // full again: no more
    CHECK(r.members(out).size() == 3);               // out of supply: none
}

TEST_CASE("supply: bankrupt kingdoms resupply at x0.25 in supply and starve out of supply") {
    const nav::MapDef m = cityMap();
    Run r(m);
    r.start(0, {30_fx, 30_fx});
    const EntityId in = r.spawn(0, "reg_inf", {55_fx, 30_fx});
    const EntityId out = r.spawn(0, "reg_inf", {85_fx, 70_fx});
    r.spawn(0, "reg_costly", {40_fx, 50_fx});  // 400 upkeep: net income negative
    r.step({makeDebugSetMoney(0, 0, r.seq(0), 0_fx), makeDebugSetHealth(0, 0, r.seq(0), r.members(in)[0], 40_fx)});
    r.step();
    REQUIRE(r.sim.world().kingdoms[0].bankrupt);
    const EntityId a = r.members(in)[0];
    const EntityId u = r.members(out)[0];
    const Fixed a0 = r.health(a);
    const Fixed u0 = r.health(u);
    r.steps(100);
    CHECK(abs(r.health(a) - a0 - 1.25_fx) < 0.01_fx);  // +5 in 10 s, x 0.25
    // Out of supply: shortfall_damage_rate 0.01 x 100 health per second.
    CHECK(abs(u0 - r.health(u) - 10_fx) < 0.01_fx);
    r.step({makeDebugSetHealth(0, 0, r.seq(0), u, 2_fx)});
    r.steps(25);
    CHECK_FALSE(r.sim.world().registry.alive(u));
    CHECK(r.count(SimEvent::Type::Died) >= 1);
    r.steps(1000);  // the rest of the regiment starves too, and the regiment goes
    CHECK_FALSE(r.sim.world().registry.alive(out));
    CHECK(r.sim.world().registry.alive(in));
}

// ------------------------------------------------------------------------------------------ siege

TEST_CASE("supply: under siege (enemy near, or hit within 7 s) a zone supplies slowly; with SupplyProvideUnderSiege off it stops") {
    const nav::MapDef m = cityMap();
    for (const bool provide : {true, false}) {
        CAPTURE(provide);
        Run r(m, provide ? defs() : strictDefs());
        const EntityId corps = r.start(0, {30_fx, 30_fx});
        const EntityId own = r.spawn(0, "reg_inf", {55_fx, 30_fx});
        r.step();
        CHECK_FALSE(r.sim.world().structures.get(corps)->underSiege);
        const EntityId enemy = r.spawn(1, "reg_inf", {30_fx, 62_fx});  // 32 from the HQ: inside the siege radius (40)
        r.step();
        CHECK(r.sim.world().structures.get(corps)->underSiege);
        CHECK(r.supply(own).inSupply == provide);
        CHECK(r.supply(own).besieged == provide);
        if (provide) {
            const EntityId a = r.members(own)[0];
            r.step({makeDebugSetHealth(0, 0, r.seq(0), a, 40_fx)});
            r.steps(100);
            CHECK(abs(r.health(a) - 41.2625_fx) < 0.01_fx);  // DenizenResupplyRateUnderSiege 0.25
        } else {
            // A besieged Corps HQ out of supply doesn't recruit.
            r.step({makeRecruit(0, 0, r.seq(0), corps, "mine_truck")});
            REQUIRE_FALSE(r.rejected());
            r.steps(300);
            CHECK(r.count(SimEvent::Type::Recruited) == 0);
        }
        r.step({makeDebugSetTransform(0, 1, r.seq(1), enemy, {120_fx, 120_fx}, kEast)});
        for (const EntityId u : r.members(enemy)) r.step({makeDebugSetTransform(0, 1, r.seq(1), u, {120_fx, 120_fx}, kEast)});
        r.step();
        CHECK_FALSE(r.sim.world().structures.get(corps)->underSiege);
        if (!provide) CHECK(r.until(SimEvent::Type::Recruited, 300) != nullptr);  // recruiting resumes
        // A hit marks it besieged for UnderSiegeRecentlyDamagedSeconds (combat sets lastDamaged).
        r.sim.world().structures.get(corps)->lastDamaged = r.sim.tick() + 1;  // as a hit this tick would
        r.step();
        CHECK(r.sim.world().structures.get(corps)->underSiege);
        r.steps(68);
        CHECK(r.sim.world().structures.get(corps)->underSiege);
        r.steps(2);
        CHECK_FALSE(r.sim.world().structures.get(corps)->underSiege);
    }
}

TEST_CASE("supply: supply_required buildings produce only in supply") {
    const nav::MapDef m = cityMap();
    Run r(m);
    const EntityId corps = r.start(0, {30_fx, 30_fx});
    r.step({makeDebugSetMoney(0, 0, r.seq(0), 5000_fx)});
    const EntityId in = r.deploy(0, corps, "mine_truck", {55_fx, 35_fx});
    const EntityId out = r.deploy(0, corps, "mine_truck", {85_fx, 75_fx});
    r.step();
    CHECK(r.sim.world().structures.get(in)->inSupply);
    CHECK_FALSE(r.sim.world().structures.get(out)->inSupply);
    CHECK(ledgerOf(r.sim, 0).production.money == 25_fx + 30_fx);  // the HQ and the mine in supply
}

// ----------------------------------------------------------------------------------------- cities

TEST_CASE("cities: the map's cities spawn neutral; a lone regiment takes one in 5 s; contested, nothing") {
    const nav::MapDef m = cityMap();
    Run r(m);
    const std::vector<EntityId> cities = r.structures("city");
    REQUIRE(cities.size() == 2);  // the tree isn't a city
    const World& w = r.sim.world();
    for (const EntityId c : cities) {
        CHECK(w.structures.get(c)->player == kNeutral);
        CHECK(w.footprints.has(c));
        CHECK_FALSE(w.combats.has(c));  // no weapons, not a target: indestructible
    }
    const EntityId a = cities[0];
    CHECK(w.transforms.get(a)->position == kCityA);
    CHECK(r.sim.nav()->grid.stampCount(r.sim.cellAt(kCityA)) == 1);
    r.start(0, {30_fx, 30_fx});
    r.step({makeDebugSetHealth(0, 0, r.seq(0), a, 50_fx)});
    const EntityId org = r.spawn(0, "reg_inf", {31_fx, 93_fx});  // just south of A's footprint (96..103)
    const SimEvent* e = r.until(SimEvent::Type::CityCaptured, 100);
    REQUIRE(e != nullptr);
    CHECK(e->entity == a);
    CHECK(e->player == 0);
    CHECK(e->detail == kNeutral);
    CHECK(e->other == org);
    CHECK(r.count(SimEvent::Type::CityCaptured) == 1);
    CHECK(w.structures.get(a)->player == 0);
    CHECK(abs(r.health(a) - 150_fx) < 0.001_fx);       // CaptureHealthFactor 0.3 x 500
    CHECK(w.engagements.get(org)->xp == 1_fx);          // ExperienceDefaultValue x ExperienceCaptureMultiplier
    CHECK(ledgerOf(r.sim, 0).production.money == 25_fx + 15_fx);
    r.step();
    CHECK(r.supply(org).inSupply);  // the city supplies its holder
    // City B, contested: nothing happens while an enemy stands in its zone.
    const EntityId b = cities[1];
    r.spawn(0, "reg_inf", {93_fx, 31_fx});
    r.spawn(1, "reg_inf", {100_fx, 55_fx});  // 25 away: inside zone_radius 30
    r.steps(150);
    CHECK(w.structures.get(b)->player == kNeutral);
    CHECK(w.structures.get(b)->captureTicks == 0);
}

TEST_CASE("cities: an enemy takes a held city when the holder has no unit there") {
    const nav::MapDef m = cityMap();
    Run r(m);
    const EntityId a = r.structures("city")[0];
    r.step({makeDebugSetOwner(0, 0, r.seq(0), a, 0)});
    CHECK(r.sim.world().structures.get(a)->player == 0);
    r.spawn(1, "reg_inf", {31_fx, 93_fx});
    const SimEvent* e = r.until(SimEvent::Type::CityCaptured, 100);
    REQUIRE(e != nullptr);
    CHECK(e->player == 1);
    CHECK(e->detail == 0);
    r.step({makeDebugSetOwner(0, 0, r.seq(0), a, 7)});
    CHECK_FALSE(r.rejected());
    r.step({makeDebugSetOwner(0, 0, r.seq(0), a, 9)});
    CHECK(r.rejected());
}

// ---------------------------------------------------------------------------------------- victory

TEST_CASE("victory: a kingdom without an HQ is defeated; the last team standing wins") {
    const nav::MapDef m = cityMap();
    Run r(m);
    r.start(0, {30_fx, 30_fx});
    const EntityId enemy = r.start(1, {90_fx, 90_fx});
    r.steps(5);
    CHECK_FALSE(r.sim.world().match.over);
    r.step({makeDebugDestroy(0, 1, r.seq(1), enemy)});
    r.step();
    CHECK(r.count(SimEvent::Type::KingdomDefeated) == 1);
    CHECK(r.sim.world().kingdoms[1].eliminated);
    REQUIRE(r.count(SimEvent::Type::TeamVictory) == 1);
    const Match& mt = r.sim.world().match;
    CHECK(mt.over);
    CHECK(mt.winner == 0);
    r.steps(10);
    CHECK(r.count(SimEvent::Type::TeamVictory) == 1);  // once
}

TEST_CASE("victory: allies win together; a packing HQ still counts") {
    const nav::MapDef m = cityMap();
    Run r(m);
    r.step({makeSetMatchRules(0, 0, r.seq(0), {0, 1, 1}, 0_fx, true)});
    r.start(0, {30_fx, 30_fx});
    const EntityId b = r.start(1, {90_fx, 90_fx});
    const EntityId c = r.start(2, {60_fx, 65_fx});
    // Packing onto its truck: the HQ convoy keeps the kingdom alive.
    r.step({makePack(0, 2, r.seq(2), c)});
    REQUIRE_FALSE(r.rejected());
    REQUIRE(r.until(SimEvent::Type::Packed, 50) != nullptr);
    r.steps(3);
    CHECK_FALSE(r.sim.world().kingdoms[2].eliminated);
    r.step({makeDebugDestroy(0, 1, r.seq(1), b)});
    r.steps(2);
    CHECK(r.sim.world().kingdoms[1].eliminated);
    CHECK_FALSE(r.sim.world().match.over);  // its ally stands
    // The convoy goes too: team 1 is out.
    for (const SimEvent& e : r.log) {
        if (e.type == SimEvent::Type::Packed) r.step({makeDebugDestroy(0, 2, r.seq(2), e.other)});
    }
    r.steps(2);
    CHECK(r.sim.world().kingdoms[2].eliminated);
    CHECK(r.sim.world().match.over);
    CHECK(r.sim.world().match.winner == 0);
}

TEST_CASE("victory: the city control ratio wins at once") {
    const nav::MapDef m = cityMap();
    Run r(m);
    r.step({makeSetMatchRules(0, 0, r.seq(0), {}, 0.5_fx, true)});
    r.start(0, {30_fx, 30_fx});
    r.start(1, {90_fx, 90_fx});
    r.spawn(0, "reg_inf", {31_fx, 93_fx});
    const SimEvent* e = r.until(SimEvent::Type::CityRatioVictory, 100);
    REQUIRE(e != nullptr);
    CHECK(e->detail == 0);  // one city of two: 15 of 30 money
    CHECK(r.count(SimEvent::Type::CityCaptured) == 1);
    CHECK(r.sim.world().match.winner == 0);
}

// ----------------------------------------------------------------------------- state, commands, map

TEST_CASE("supply save / load: mid-capture and mid-replacement states continue identically") {
    const nav::MapDef m = cityMap();
    Run r(m);
    r.start(0, {30_fx, 30_fx});
    const EntityId org = r.spawn(0, "reg_inf", {55_fx, 30_fx});
    r.spawn(0, "reg_inf", {31_fx, 93_fx});
    r.step({makeDebugDestroy(0, 0, r.seq(0), r.members(org)[0])});
    r.steps(12);
    REQUIRE(r.supply(org).replaceTicks > 0);
    REQUIRE(r.sim.world().structures.get(r.structures("city")[0])->captureTicks > 0);
    const std::vector<std::uint8_t> saved = saveState(r.sim);
    Sim copy({7, &defs(), &m});
    std::string err;
    REQUIRE_MESSAGE(loadState(copy, saved, &err), err);
    CHECK(stateHash(copy) == stateHash(r.sim));
    for (int i = 0; i < 200; ++i) {
        r.step();
        copy.step();
    }
    CHECK(stateHash(copy) == stateHash(r.sim));
    CHECK(copy.world().structures.get(r.structures("city")[0])->player == 0);
}

TEST_CASE("supply commands encode and decode") {
    const std::vector<Command> cmds = {
        makeSetMatchRules(0, 0, 0, {0, 1, 1, 0}, 0.65_fx, false),
        makeSetMatchRules(0, 2, 1, {}, 0_fx, true),
        makeDebugSetHealth(3, 1, 2, EntityId{77}, 12.5_fx),
        makeDebugSetOwner(3, 1, 3, EntityId{78}, kNeutral),
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

TEST_CASE("map things: map.json things (translated IDS), kingdoms and rules; start positions; the hash covers them") {
    const std::string json = R"({"tiles_per_side": 4, "layers": {"terrain_names": ["grass"],
        "tile_terrain_index": [0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0],
        "tile_class_bits": [0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0],
        "vertex_height_mm": [0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0],
        "vertex_mountain_mm": [0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0]},
        "things": [{"ids": "Chaparral_Village_Town_Center", "ids_translated": "mediterranean_city_center01_large", "x_mm": 6500,
                    "y_mm": 2250, "facing_x_mm": 0, "facing_y_mm": -1000, "owner": 2, "name": "name_6"},
                   {"ids": "placeholder_corpsunpacked", "x_mm": 5000, "y_mm": 1000, "facing_x_mm": 0, "facing_y_mm": -1000, "owner": 1, "name": ""},
                   {"ids": "placeholder_corpsunpacked", "x_mm": 1000, "y_mm": 5000, "facing_x_mm": 1000, "facing_y_mm": 0, "owner": 0, "name": ""}],
        "kingdoms": [{"name": "kingdom_0", "color": "blue", "object_id": 1}, {"name": "kingdom_1", "color": "red", "object_id": 2},
                     {"name": "kingdom_2", "color": "gray", "object_id": 5}],
        "rules": [{"name": "CityVictoryRatio", "type": 1, "value_milli": 500}]})";
    nav::MapDef m;
    std::string err;
    REQUIRE_MESSAGE(nav::loadRmpJson(json, m, err), err);
    REQUIRE(m.things.size() == 3);
    CHECK(m.things[0].ids == "mediterranean_city_center01_large");
    CHECK(m.things[0].position == FVec2{6.5_fx, 2.25_fx});
    CHECK(m.things[0].owner == 2);
    CHECK(m.things[0].name == "name_6");
    CHECK(m.things[1].ids == "placeholder_corpsunpacked");
    REQUIRE(m.kingdoms.size() == 3);
    CHECK(m.kingdoms[2].color == "gray");
    CHECK(m.rule("cityvictoryratio", 0_fx) == 0.5_fx);
    CHECK(m.rule("FogOfWar", 1_fx) == 1_fx);
    const std::vector<MapStart> starts = mapStarts(m);
    REQUIRE(starts.size() == 2);  // kingdom_2 has no Corps placeholder: neutral
    CHECK(starts[0].player == 0);
    CHECK(starts[0].position == FVec2{1_fx, 5_fx});
    CHECK(starts[1].player == 1);
    const std::uint64_t h = m.mapHash;
    m.things[0].position.x = 7_fx;
    REQUIRE(m.finalize());
    CHECK(m.mapHash != h);
}

TEST_CASE("committed supply replay plays identically on this compiler (fixture: capture, replacement, starvation, victory)") {
    // sim/tests/fixtures/supply_m6.rpl: these test defs, the city map, 1200 ticks of the scenario
    // below, recorded with MSVC x64. CORPS_WRITE_FIXTURES=1 re-records it.
    const nav::MapDef m = cityMap();
    Replay rep;
    rep.header.simVersion = simVersion();
    rep.header.dataHash = defs().dataHash;
    rep.header.mapSpec = m.name;
    rep.header.mapHash = m.mapHash;
    rep.header.matchSeed = 7;
    rep.header.tickCount = 1200;
    rep.header.players = {"A", "B"};
    rep.commands = {makeSetMatchRules(0, 0, 0, {0, 1}, 0_fx, true), makeStartPlayer(0, 0, 1, "corps", {30_fx, 30_fx}, kEast),
                    makeStartPlayer(0, 1, 0, "corps", {100_fx, 100_fx}, kEast),
                    makeSpawnRegiment(0, 0, 2, "reg_inf", {50_fx, 40_fx}, kEast),
                    makeSpawnRegiment(0, 1, 1, "reg_inf", {60_fx, 60_fx}, kEast),
                    makeSpawnRegiment(0, 1, 2, "reg_costly", {110_fx, 80_fx}, kEast)};
    {
        Sim scratch({7, &defs(), &m});
        scratch.step(rep.commands);
        const std::vector<EntityId> regs = scratch.world().regiments.ids();
        REQUIRE(regs.size() == 3);
        std::vector<EntityId> p0Units, p1Corps;
        for (std::size_t i = 0; i < scratch.world().members.size(); ++i) {
            if (scratch.world().members.data()[i].organization == regs[0]) p0Units.push_back(scratch.world().members.ids()[i]);
        }
        for (std::size_t i = 0; i < scratch.world().structures.size(); ++i) {
            if (scratch.world().structures.data()[i].player == 1) p1Corps.push_back(scratch.world().structures.ids()[i]);
        }
        REQUIRE(p1Corps.size() == 1);
        rep.commands.push_back(makeMoveGroup(1, 0, 3, {regs[0]}, {100_fx, 22_fx}));  // to city B
        rep.commands.push_back(makeDebugDestroy(1, 0, 4, p0Units[1]));                // replaced on the way (in supply)
        rep.commands.push_back(makeDebugSetMoney(1, 1, 3, 0_fx));                    // B goes bankrupt
        rep.commands.push_back(makeDebugDestroy(1100, 1, 4, p1Corps[0]));             // and loses its HQ
    }
    RunOptions opt;
    opt.map = &m;
    const std::string path = CORPS_SOURCE_DIR "/sim/tests/fixtures/supply_m6.rpl";
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
    // The film has what part 2 is about.
    Sim sim({7, &defs(), &m});
    CommandQueue q;
    for (const Command& c : recorded.commands) q.schedule(c, 0);
    std::size_t captured = 0, replaced = 0, starved = 0, defeated = 0, won = 0, rejected = 0;
    for (std::uint32_t t = 0; t < recorded.header.tickCount; ++t) {
        sim.step(q.take(t));
        for (const SimEvent& e : sim.events()) {
            captured += e.type == SimEvent::Type::CityCaptured;
            replaced += e.type == SimEvent::Type::Replaced;
            starved += e.type == SimEvent::Type::Died && e.other.isNull();
            defeated += e.type == SimEvent::Type::KingdomDefeated;
            won += e.type == SimEvent::Type::TeamVictory;
            rejected += e.type == SimEvent::Type::CommandRejected;
        }
    }
    CHECK(captured == 1);
    CHECK(replaced >= 1);
    CHECK(starved >= 1);
    CHECK(defeated == 1);
    CHECK(won == 1);
    CHECK(rejected == 0);
    CHECK(sim.world().match.winner == 0);
}
