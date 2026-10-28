// The AI read API (ai/view): own kingdom, the enemies the team sees and nothing else, last-seen
// memory, shared vision between allies, cities, supply and passability. A small hand-built
// DefSet (no game data needed).

#include <doctest/doctest.h>

#include <algorithm>

#include "ai/view.hpp"
#include "core/sim.hpp"
#include "data/def_compiler.hpp"
#include "data/defs.hpp"
#include "nav/map_def.hpp"
#include "session/bytes.hpp"
#include "systems/bases.hpp"
#include "systems/supply.hpp"

using namespace corps;
using namespace corps::fixed_literals;

namespace {

UnitDef rifle() {
    UnitDef u;
    u.ids = "rifle";
    u.moveClass = UnitMoveClass::Foot;
    u.speedMax = 5_fx;
    u.turnType = TurnType::Instant;
    u.blockingRadius = 0.4_fx;
    u.health = 100_fx;
    u.detectionRange = 30_fx;
    return u;
}

// rifle (sees 30 m), reg_inf (4 rifles), corps (Corps HQ, supply 40), city (zone 20).
DefSet makeDefs() {
    DefSet d;
    d.properties = {PropertyDef{"corpshq"}};
    d.units = {rifle()};
    RegimentDef inf;
    inf.ids = "reg_inf";
    inf.slotIds = {"rifle", "rifle", "rifle", "rifle"};
    inf.recruitTime = 8_fx;
    d.regiments = {inf};
    BuildingDef corps;
    corps.ids = "corps";
    corps.health = 2000_fx;
    corps.shape = FootprintShape::Square;
    corps.squareSize = 8_fx;
    corps.padding = 1_fx;
    corps.tileLocked = corps.axisLocked = true;
    corps.providesCorps = true;
    corps.hq = true;
    corps.properties = {"corpshq"};
    corps.supplyRange = 40_fx;
    corps.production.money = 25_fx;
    BuildingDef city = corps;
    city.ids = "city";
    city.providesCorps = city.hq = false;
    city.properties.clear();
    city.mobile = false;
    city.cityZoneRadius = 20_fx;
    city.captureable = true;
    city.destructible = false;
    city.supplyRange = 0_fx;
    city.production.money = 15_fx;
    d.buildings = {city, corps};
    d.economy.startLocation.buildingLimitProvided = 10_fx;
    for (RegimentDef& g : d.regiments) {
        for (const std::string& s : g.slotIds) g.slots.push_back({ElementKind::Unit, d.unitIndex(s)});
    }
    tgi::Diagnostics diags;
    linkBases(d, diags);
    d.dataHash = hashBytes(serializeDefs(d));
    return d;
}

const DefSet& defs() {
    static const DefSet set = makeDefs();
    return set;
}

// 200 x 200 open ground, a block of water at x 90..110, y 0..20; a neutral city at (100, 150).
nav::MapDef openMap() {
    nav::MapDef m;
    m.name = "view-test";
    m.width = m.height = 200;
    m.kind.assign(m.cellCount(), nav::TerrainKind::Ground);
    for (std::int32_t y = 0; y < 20; ++y) {
        for (std::int32_t x = 90; x < 110; ++x) m.kind[static_cast<std::size_t>(y) * 200 + static_cast<std::size_t>(x)] = nav::TerrainKind::Water;
    }
    m.terrainId.assign(m.cellCount(), 0);
    m.terrainNames = {"open"};
    m.things = {{"city", {100_fx, 150_fx}, Angle{}, -1, "name_1"}};
    REQUIRE(m.finalize());
    return m;
}

struct Run {
    Sim sim;
    std::uint16_t seqs[4] = {0, 0, 0, 0};
    explicit Run(const nav::MapDef& m) : sim({3, &defs(), &m}) {}
    void step(std::vector<Command> cmds = {}) {
        for (Command& c : cmds) c.tick = sim.tick();
        sim.step(cmds);
        for (const SimEvent& e : sim.events()) REQUIRE(e.type != SimEvent::Type::CommandRejected);
    }
    void steps(int n) {
        for (int i = 0; i < n; ++i) step();
    }
    EntityId spawn(std::uint8_t p, FVec2 at) {
        step({makeSpawnRegiment(0, p, seqs[p]++, "reg_inf", at, Angle{})});
        for (const SimEvent& e : sim.events()) {
            if (e.type == SimEvent::Type::Spawned && sim.world().regiments.has(e.entity)) return e.entity;
        }
        FAIL("no regiment");
        return {};
    }
    void move(std::uint8_t p, EntityId r, FVec2 to) { step({makeMoveGroup(0, p, seqs[p]++, {r}, to)}); }
};

bool shows(const std::vector<ai::Seen>& v, EntityId id) {
    return std::any_of(v.begin(), v.end(), [&](const ai::Seen& s) { return s.id == id; });
}

}  // namespace

TEST_CASE("ai view: a hidden enemy is invisible; one within detection range shows, with its units and health") {
    const nav::MapDef m = openMap();
    Run run(m);
    const EntityId mine = run.spawn(0, {20_fx, 100_fx});
    const EntityId far = run.spawn(1, {170_fx, 100_fx});
    const EntityId near = run.spawn(1, {40_fx, 100_fx});
    run.step();
    ai::View v(0);
    v.update(run.sim);
    REQUIRE(v.regiments.size() == 1);
    CHECK(v.regiments[0].id == mine);
    CHECK(v.regiments[0].units == 4);
    CHECK(v.regiments[0].health == 400_fx);
    CHECK_FALSE(shows(v.visible(), far));
    CHECK_FALSE(shows(v.remembered(), far));
    CHECK(v.lastSeen(far) == nullptr);
    REQUIRE(shows(v.visible(), near));
    CHECK(v.visible()[0].units == 4);
    CHECK(v.visible()[0].health == 400_fx);
    CHECK(v.visible()[0].player == 1);
    CHECK(v.enemies == std::vector<std::uint8_t>{1});
    CHECK(v.sees({40_fx, 100_fx}));
    CHECK_FALSE(v.sees({170_fx, 100_fx}));
    // Destroyed in plain sight: no longer visible, and forgotten (its spot is seen empty).
    const std::vector<EntityId> units = run.sim.world().members.ids();  // a copy: destroying changes the pool
    for (const EntityId u : units) {
        if (run.sim.world().members.get(u)->organization == near) run.step({makeDebugDestroy(0, 1, run.seqs[1]++, u)});
    }
    run.steps(2);
    v.update(run.sim);
    CHECK_FALSE(shows(v.visible(), near));
    CHECK(v.lastSeen(near) == nullptr);
}

TEST_CASE("ai view: an enemy that walks out of sight is remembered where it was last seen, then forgotten") {
    const nav::MapDef m = openMap();
    Run run(m);
    run.spawn(0, {20_fx, 100_fx});
    const EntityId them = run.spawn(1, {45_fx, 100_fx});
    ai::View v(0, 600);
    v.update(run.sim);
    REQUIRE(shows(v.visible(), them));
    run.move(1, them, {180_fx, 100_fx});
    std::uint32_t lost = 0;
    for (int i = 0; i < 600 && lost == 0; ++i) {
        run.step();
        v.update(run.sim);
        if (!shows(v.visible(), them)) lost = run.sim.tick();
    }
    REQUIRE(lost != 0);
    const ai::Seen* s = v.lastSeen(them);
    REQUIRE(s != nullptr);
    CHECK(s->tick < lost);
    CHECK(s->at.x < 70_fx);  // where it was, not where it is
    CHECK(shows(v.remembered(), them));
    // It left from the edge of sight, so its last-seen spot is out of sight: only time forgets it.
    run.steps(600);
    v.update(run.sim);
    CHECK(v.lastSeen(them) == nullptr);
}

TEST_CASE("ai view: allies share vision; allies' regiments aren't enemies") {
    const nav::MapDef m = openMap();
    Run run(m);
    run.step({makeSetMatchRules(0, 0, run.seqs[0]++, {0, 0, 1}, 0_fx, true, 0_fx)});
    run.spawn(0, {20_fx, 100_fx});
    const EntityId friendRegiment = run.spawn(1, {150_fx, 20_fx});
    const EntityId enemy = run.spawn(2, {170_fx, 20_fx});
    run.step();
    ai::View v(0);
    v.update(run.sim);
    CHECK(shows(v.visible(), enemy));  // seen by the ally
    CHECK_FALSE(shows(v.visible(), friendRegiment));
    CHECK(v.allies == std::vector<std::uint8_t>{1});
    CHECK(v.enemies == std::vector<std::uint8_t>{2});
}

TEST_CASE("ai view: own kingdom, cities, supply coverage and passability") {
    const nav::MapDef m = openMap();
    Run run(m);
    run.step({makeStartPlayer(0, 0, run.seqs[0]++, "corps", {30_fx, 30_fx}, Angle{})});
    run.steps(2);
    ai::View v(0);
    v.update(run.sim);
    CHECK(v.alive());
    REQUIRE(v.buildings.size() == 1);
    CHECK(v.buildings[0].corps);
    CHECK(v.ledger.production.money >= 25_fx);
    REQUIRE(v.cities().size() == 1);
    CHECK(v.cities()[0].owner == kNeutral);
    CHECK(v.cities()[0].radius == 20_fx);
    CHECK(v.supplied({40_fx, 40_fx}));
    CHECK_FALSE(v.supplied({150_fx, 150_fx}));
    CHECK(v.passable(nav::MoveClass::Foot, {50_fx, 50_fx}));
    CHECK_FALSE(v.passable(nav::MoveClass::Foot, {100_fx, 10_fx}));
    CHECK_FALSE(v.passable(nav::MoveClass::Foot, {-5_fx, 10_fx}));
}
