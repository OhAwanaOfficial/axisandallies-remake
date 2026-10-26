#include "systems/bases.hpp"

#include <algorithm>

#include "core/ascii.hpp"
#include "core/sim.hpp"
#include "nav/map_def.hpp"
#include "nav/nav_world.hpp"
#include "systems/combat.hpp"
#include "systems/economy.hpp"
#include "systems/morale.hpp"
#include "systems/movement.hpp"
#include "systems/orders.hpp"
#include "systems/supply.hpp"

namespace corps {

using namespace fixed_literals;

namespace {

constexpr std::uint16_t kQuarterTurn = 16384;

std::uint32_t ticksOf(Fixed seconds) {
    const std::int64_t t = (seconds * static_cast<std::int32_t>(kTicksPerSecond)).ceilInt();
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(t, 1, 0x7FFFFFFF));
}

// A timer started during tick t (by a command, or earlier in this Timers stage) is counted down in
// that same Timers stage, so it starts one higher: it then ends exactly `seconds` after tick t.
std::uint32_t timerOf(Fixed seconds) {
    return ticksOf(seconds) + 1;
}

const BuildingDef* buildingOf(const Sim& sim, EntityId id, std::uint32_t* index = nullptr) {
    const DefSet* d = sim.config().defs;
    const DefRef* r = d != nullptr ? sim.world().defRefs.get(id) : nullptr;
    if (r == nullptr || r->kind != DefRef::Kind::Building || r->index >= d->buildings.size()) return nullptr;
    if (index != nullptr) *index = r->index;
    return &d->buildings[r->index];
}

// The sender's structure `id`, alive.
Structure* ownStructure(Sim& sim, const Command& c, EntityId id) {
    World& w = sim.world();
    Structure* s = w.registry.alive(id) ? w.structures.get(id) : nullptr;
    return s != nullptr && s->player == c.player ? s : nullptr;
}
const Structure* ownStructure(const Sim& sim, std::uint8_t player, EntityId id) {
    const World& w = sim.world();
    const Structure* s = w.registry.alive(id) ? w.structures.get(id) : nullptr;
    return s != nullptr && s->player == player ? s : nullptr;
}

// The recruit list entry of building `b` named `ids` (its own IDS or the regiment's): its regiment.
std::uint32_t recruitRegiment(const DefSet& d, const BuildingDef& b, std::string_view ids) {
    for (const RecruitRef& r : b.recruitRefs) {
        if (iequals(r.ids, ids) || iequals(d.regiments[r.regiment].ids, ids)) return r.regiment;
    }
    return kNoDef;
}

std::uint32_t researchItem(const BuildingDef& b, std::string_view propertyIds) {
    for (std::uint32_t i = 0; i < b.research.size(); ++i) {
        if (iequals(b.research[i].propertyIds, propertyIds)) return i;
    }
    return Structure::kNone;
}

Blocker blocked(std::string code, std::string detail = {}) {
    return {std::move(code), std::move(detail)};
}

bool hasDeployed(const Sim& sim, std::uint8_t player, std::string_view buildingIds) {
    const World& w = sim.world();
    for (std::size_t i = 0; i < w.structures.size(); ++i) {
        const Structure& s = w.structures.data()[i];
        if (s.player != player || s.state != Structure::State::Deployed) continue;
        const BuildingDef* b = buildingOf(sim, w.structures.ids()[i]);
        if (b != nullptr && iequals(b->ids, buildingIds)) return true;
    }
    return false;
}

// Whether the player has (or is unpacking toward) a Corps HQ, besides truck regiment `ignore`.
bool hasCorps(const Sim& sim, std::uint8_t player, EntityId ignore) {
    const World& w = sim.world();
    const DefSet& d = *sim.config().defs;
    for (std::size_t i = 0; i < w.structures.size(); ++i) {
        const BuildingDef* b = buildingOf(sim, w.structures.ids()[i]);
        if (w.structures.data()[i].player == player && b != nullptr && b->providesCorps) return true;
    }
    for (std::size_t i = 0; i < w.deploys.size(); ++i) {
        const EntityId org = w.deploys.ids()[i];
        const Regiment* r = w.regiments.get(org);
        const std::uint32_t bi = w.deploys.data()[i].building;
        if (org != ignore && r != nullptr && r->player == player && bi < d.buildings.size() && d.buildings[bi].providesCorps) return true;
    }
    return false;
}

FVec2 localPoint(const Transform& t, FVec2 offset) {
    return t.position + offset.rotated(t.facing);
}

EntityId spawnStructure(Sim& sim, std::uint32_t bi, const Placement& pl, std::uint8_t player, Structure::State state,
                        std::uint32_t stateTicks, Fixed healthFraction, CommandType cause) {
    World& w = sim.world();
    const BuildingDef& b = sim.config().defs->buildings[bi];
    const EntityId id = w.registry.create();
    if (id.isNull()) return id;
    w.transforms.set(id, {pl.center, pl.facing});
    w.defRefs.set(id, {DefRef::Kind::Building, bi});
    const Fixed hp = max(b.health * healthFraction, min(b.health, 1_fx));
    w.health.set(id, {hp, b.health});
    w.footprints.set(id, pl.rect);
    sim.nav()->grid.stamp({pl.rect.x0, pl.rect.y0, pl.rect.x1, pl.rect.y1});
    attachCombat(sim, id, player);
    Structure s;
    s.state = state;
    s.player = player;
    s.stateTicks = stateTicks;
    w.structures.set(id, s);
    if (player < kMaxPlayers) w.kingdoms[player].active = true;
    sim.emit({SimEvent::Type::Spawned, id, player, cause});
    return id;
}

// An empty regiment of def `regiment` at `pos`; units join it one by one (recruiting).
EntityId createRegiment(Sim& sim, std::uint8_t player, std::uint32_t regiment, FVec2 pos, Angle facing, CommandType cause) {
    World& w = sim.world();
    const DefSet& d = *sim.config().defs;
    const RegimentDef& reg = d.regiments[regiment];
    const EntityId org = w.registry.create();
    if (org.isNull()) return org;
    w.transforms.set(org, {pos, facing});
    w.defRefs.set(org, {DefRef::Kind::Regiment, regiment});
    Regiment r;
    r.layout = reg.layoutIndex;
    r.player = player;
    if (reg.layoutIndex != kNoDef) {
        const LayoutDef& l = d.layouts[reg.layoutIndex];
        const std::uint32_t f = l.formation(l.defaultFormationIds);
        r.formation = static_cast<std::uint16_t>(f != kNoDef ? f : 0);
    }
    w.regiments.set(org, r);
    Order idle;
    idle.facing = facing;
    idle.goal = pos;
    w.orders.set(org, idle);
    attachEngagement(sim, org);
    initRegimentMorale(sim, org);
    sim.emit({SimEvent::Type::Spawned, org, player, cause});
    return org;
}

// Unit `e` of a regiment's plan joins the regiment being recruited, with full morale.
bool addRecruit(Sim& sim, EntityId org, const RecruitElement& e, bool first, FVec2 pos, Angle facing) {
    if (addRegimentUnit(sim, org, e, first, pos, facing, CommandType::Recruit).isNull()) return false;
    if (Engagement* g = sim.world().engagements.get(org)) g->morale = moraleMaxOf(sim, org);
    return true;
}

// The members of a regiment, ascending.
std::vector<EntityId> membersOf(const World& w, EntityId org) {
    std::vector<EntityId> out;
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        if (w.members.data()[i].organization == org) out.push_back(w.members.ids()[i]);
    }
    return out;
}

// --- timers ---------------------------------------------------------------------------------------

// A truck regiment that reached its spot becomes its building (or gives up if it no longer fits).
void startUnpack(Sim& sim, EntityId org, const Deploy& d) {
    World& w = sim.world();
    const Regiment* r = w.regiments.get(org);
    const std::uint8_t player = r->player;
    const Placement pl = placeBuilding(sim, d.building, d.at, d.facing, player, org);
    if (!pl.ok) {
        sim.emit({SimEvent::Type::DeployFailed, org, player, CommandType::Unpack});
        return;
    }
    Fixed hp, hpMax;
    const std::vector<EntityId> members = membersOf(w, org);
    for (const EntityId m : members) {
        if (const Health* h = w.health.get(m)) {
            hp = hp + max(h->current, Fixed{});
            hpMax = hpMax + h->max;
        }
    }
    const Fixed fraction = hpMax > Fixed{} ? hp / hpMax : 1_fx;
    const Engagement* g = w.engagements.get(org);
    const Fixed xp = g != nullptr ? g->xp : Fixed{};
    const BuildingDef& b = sim.config().defs->buildings[d.building];
    const EntityId id = spawnStructure(sim, d.building, pl, player, Structure::State::Unpacking, timerOf(b.unpackingLength), fraction,
                                       CommandType::Unpack);
    if (id.isNull()) return;
    w.structures.get(id)->xp = xp;
    for (const EntityId m : members) w.registry.destroyLater(m);
    w.registry.destroyLater(org);
}

void finishPack(Sim& sim, EntityId id, Structure& s) {
    World& w = sim.world();
    const BuildingDef* b = buildingOf(sim, id);
    const Transform t = *w.transforms.get(id);
    const Health* h = w.health.get(id);
    const Fixed fraction = h != nullptr && h->max > Fixed{} ? h->current / h->max : 1_fx;
    const Fixed xp = s.xp;
    const std::uint8_t player = s.player;
    const EntityId org = sim.spawnRegimentAt(player, b->packedRegiment, t.position, t.facing, CommandType::Pack);
    if (org.isNull()) return;  // no entity left: it stays packing and tries again next tick
    for (const EntityId m : membersOf(w, org)) {
        if (Health* mh = w.health.get(m)) mh->current = max(mh->max * fraction, min(mh->max, 1_fx));
    }
    if (xp > Fixed{} && w.engagements.has(org)) setExperience(sim, org, xp);
    w.registry.destroyLater(id);
    sim.emit({SimEvent::Type::Packed, id, player, CommandType::Pack, org});
}

void recruitStep(Sim& sim, EntityId id, Structure& s) {
    World& w = sim.world();
    const DefSet& d = *sim.config().defs;
    if (s.queue.empty()) return;
    const std::uint32_t regiment = s.queue.front().regiment;
    const std::vector<RecruitElement> plan = recruitPlan(d, regiment);
    const BuildingDef* b = buildingOf(sim, id);
    const Transform t = *w.transforms.get(id);
    const FVec2 creation = localPoint(t, b->creationOffset);
    const FVec2 deployment = localPoint(t, b->deploymentOffset);
    const auto interval = [&] {
        const std::uint32_t total = ticksOf(d.regiments[regiment].recruitTime);
        return std::max<std::uint32_t>(1, (total + static_cast<std::uint32_t>(plan.size()) - 1) / std::max<std::uint32_t>(1, static_cast<std::uint32_t>(plan.size())));
    };
    const auto done = [&] {
        s.queue.erase(s.queue.begin());
        s.recruit = EntityId{};
        s.nextElement = 0;
        s.recruitTicks = 0;
    };
    if (s.recruit.isNull()) {
        s.recruit = createRegiment(sim, s.player, regiment, creation, t.facing, CommandType::Recruit);
        if (s.recruit.isNull()) return;  // no entity left: try again next tick
        s.nextElement = 0;
        s.recruitTicks = interval();
        restartOrder(sim, s.recruit, deployment, true, t.facing);
        return;
    }
    if (!w.registry.alive(s.recruit)) {  // destroyed while forming: what came out is lost, the rest too
        done();
        return;
    }
    if (s.recruitTicks > 1) {
        --s.recruitTicks;
        return;
    }
    if (s.nextElement < plan.size()) {
        if (!addRecruit(sim, s.recruit, plan[s.nextElement], s.nextElement == 0, creation, t.facing)) return;
        ++s.nextElement;
        if (orderOwner(sim, s.recruit) == s.recruit) restartOrder(sim, s.recruit, deployment, true, t.facing);
    }
    if (s.nextElement >= plan.size()) {
        sim.emit({SimEvent::Type::Recruited, s.recruit, s.player, CommandType::Recruit, id, regiment});
        done();
    } else {
        s.recruitTicks = interval();
    }
}

void grant(Sim& sim, Structure& s, const ResearchItemDef& item) {
    std::vector<std::uint32_t>& to = item.scope == ResearchItemDef::Scope::Local ? s.properties : sim.world().kingdoms[s.player].properties;
    const auto it = std::lower_bound(to.begin(), to.end(), item.property);
    if (it == to.end() || *it != item.property) to.insert(it, item.property);
}

void researchStep(Sim& sim, EntityId id, Structure& s) {
    if (s.research == Structure::kNone) return;
    if (s.researchTicks > 1) {
        --s.researchTicks;
        return;
    }
    const BuildingDef* b = buildingOf(sim, id);
    if (b != nullptr && s.research < b->research.size() && s.player < kMaxPlayers) {
        const ResearchItemDef& item = b->research[s.research];
        grant(sim, s, item);
        sim.emit({SimEvent::Type::ResearchDone, id, s.player, CommandType::Research, EntityId{}, item.property});
    }
    s.research = Structure::kNone;
    s.researchTicks = 0;
    s.researchPaid = Fixed{};
}

}  // namespace

// --- placement --------------------------------------------------------------------------------------

Placement placeBuilding(const Sim& sim, std::uint32_t building, FVec2 at, Angle facing, std::uint8_t player, EntityId ignore) {
    Placement out;
    const DefSet* d = sim.config().defs;
    const nav::NavWorld* nav = sim.nav();
    if (d == nullptr || nav == nullptr || building >= d->buildings.size()) {
        out.why = "no data or map";
        return out;
    }
    const BuildingDef& b = d->buildings[building];
    out.facing = facing;
    if (b.axisLocked) out.facing = Angle::fromUnits(((static_cast<std::uint32_t>(facing.units) + kQuarterTurn / 2) / kQuarterTurn) * kQuarterTurn);
    const bool sideways = ((static_cast<std::uint32_t>(out.facing.units) + kQuarterTurn / 2) / kQuarterTurn) % 2 == 1;
    Fixed ex, ey;  // extent along x (the building's forward at facing 0) and y
    switch (b.shape) {
    case FootprintShape::Square: ex = ey = b.squareSize; break;
    case FootprintShape::Rect: ex = b.rectHeight; ey = b.rectWidth; break;
    case FootprintShape::Circle: ex = ey = b.blockingRadius * 2; break;
    }
    if (sideways) std::swap(ex, ey);
    const std::int32_t wc = static_cast<std::int32_t>(std::max<std::int64_t>(1, ex.ceilInt()));
    const std::int32_t hc = static_cast<std::int32_t>(std::max<std::int64_t>(1, ey.ceilInt()));
    const auto x0 = static_cast<std::int32_t>((at.x - Fixed::fromInt(wc) / 2_fx).roundInt());
    const auto y0 = static_cast<std::int32_t>((at.y - Fixed::fromInt(hc) / 2_fx).roundInt());
    out.rect = {x0, y0, x0 + wc - 1, y0 + hc - 1};
    out.center = {Fixed::fromInt(x0) + Fixed::fromInt(wc) / 2_fx, Fixed::fromInt(y0) + Fixed::fromInt(hc) / 2_fx};
    const nav::NavGrid& g = nav->grid;
    const auto pad = static_cast<std::int32_t>(std::max<std::int64_t>(0, b.padding.ceilInt()));
    for (std::int32_t y = out.rect.y0 - pad; y <= out.rect.y1 + pad; ++y) {
        for (std::int32_t x = out.rect.x0 - pad; x <= out.rect.x1 + pad; ++x) {
            if (!g.inside(x, y)) {
                out.why = "outside the map";
                return out;
            }
            if (!g.passable(nav::MoveClass::Foot, x, y) || !g.passable(nav::MoveClass::Wheeled, x, y)) {
                out.why = "not buildable ground";
                return out;
            }
        }
    }
    const auto gap = static_cast<std::int32_t>(std::max<std::int64_t>(0, d->economy.buildingPadding.ceilInt()));
    for (std::int32_t y = std::max(0, out.rect.y0 - gap); y <= std::min(g.height() - 1, out.rect.y1 + gap); ++y) {
        for (std::int32_t x = std::max(0, out.rect.x0 - gap); x <= std::min(g.width() - 1, out.rect.x1 + gap); ++x) {
            if (g.stampCount(g.index(x, y)) != 0) {
                out.why = "too close to a building";
                return out;
            }
        }
    }
    if (b.providesCorps && hasCorps(sim, player, ignore)) {
        out.why = "only one Corps HQ";
        return out;
    }
    out.ok = true;
    return out;
}

// --- recruit plans ------------------------------------------------------------------------------------

std::vector<RecruitElement> recruitPlan(const DefSet& defs, std::uint32_t regiment) {
    std::vector<RecruitElement> out;
    if (regiment >= defs.regiments.size()) return out;
    const RegimentDef& reg = defs.regiments[regiment];
    const LayoutDef* layout = reg.layoutIndex != kNoDef ? &defs.layouts[reg.layoutIndex] : nullptr;
    const std::size_t layoutElements = layout != nullptr ? layout->elementSlot.size() : 0;
    std::uint32_t extra = 0;
    for (std::size_t s = 0; s < reg.slots.size(); ++s) {
        const ElementRef& el = reg.slots[s];
        if (el.kind == ElementKind::None) continue;
        if (layout != nullptr && s < layout->slots.size()) {
            for (std::uint32_t e = 0; e < layoutElements; ++e) {
                if (layout->elementSlot[e] == s) out.push_back({el, e});
            }
        } else {
            out.push_back({el, static_cast<std::uint32_t>(layoutElements) + extra++});
        }
    }
    return out;
}

ResourceAmounts recruitLimits(const DefSet& defs, std::uint32_t regiment, std::uint32_t from, bool withRegiment) {
    ResourceAmounts a;
    if (regiment >= defs.regiments.size()) return a;
    if (withRegiment) {
        a.unitLimitConsumed = defs.regiments[regiment].upkeep.unitLimitConsumed;
        a.buildingLimitConsumed = defs.regiments[regiment].upkeep.buildingLimitConsumed;
    }
    const std::vector<RecruitElement> plan = recruitPlan(defs, regiment);
    for (std::size_t i = from; i < plan.size(); ++i) {
        if (plan[i].element.kind != ElementKind::Unit) continue;
        const UnitDef& u = defs.units[plan[i].element.index];
        a.unitLimitConsumed = a.unitLimitConsumed + u.upkeep.unitLimitConsumed;
        a.buildingLimitConsumed = a.buildingLimitConsumed + u.upkeep.buildingLimitConsumed;
    }
    return a;
}

EntityId addRegimentUnit(Sim& sim, EntityId org, const RecruitElement& e, bool first, FVec2 pos, Angle facing, CommandType cause,
                         Fixed healthFactor) {
    World& w = sim.world();
    const DefSet& d = *sim.config().defs;
    Regiment* r = w.regiments.get(org);
    if (r == nullptr) return {};
    const std::uint8_t player = r->player;
    const EntityId id = w.registry.create();
    if (id.isNull()) return id;
    w.transforms.set(id, {pos, facing});
    if (e.element.kind == ElementKind::Unit) {
        const UnitDef& u = d.units[e.element.index];
        w.defRefs.set(id, {DefRef::Kind::Unit, e.element.index});
        w.health.set(id, {u.health * healthFactor, u.health * healthFactor});
        w.movers.set(id, {});
        const auto cls = static_cast<std::uint8_t>(u.moveClass);
        r = w.regiments.get(org);
        r->moveClass = first ? cls : restrictiveClass(r->moveClass, cls);
    } else {
        const Fixed hp = d.buildings[e.element.index].health * healthFactor;
        w.defRefs.set(id, {DefRef::Kind::Building, e.element.index});
        w.health.set(id, {hp, hp});
    }
    w.members.set(id, {org, static_cast<std::uint16_t>(e.slot)});
    attachCombat(sim, id, player);
    sim.emit({SimEvent::Type::Spawned, id, player, cause});
    return id;
}

Fixed recruitCost(const DefSet& defs, std::uint32_t regiment) {
    if (regiment >= defs.regiments.size()) return {};
    Fixed cost = defs.regiments[regiment].cost.money;
    for (const RecruitElement& e : recruitPlan(defs, regiment)) {
        if (e.element.kind == ElementKind::Unit) cost = cost + defs.units[e.element.index].cost.money;
    }
    return cost;
}

// --- commands -----------------------------------------------------------------------------------------

void applyStartPlayer(Sim& sim, const Command& c, const BaseOrderPayload& p) {
    const DefSet* d = sim.config().defs;
    if (d == nullptr || sim.nav() == nullptr || c.player >= kMaxPlayers) return sim.reject(c);
    Kingdom& k = sim.world().kingdoms[c.player];
    const std::uint32_t bi = d->buildingIndex(p.ids);
    if (k.started || bi == kNoDef) return sim.reject(c);
    const Placement pl = placeBuilding(sim, bi, p.position, p.facing, c.player);
    if (!pl.ok) return sim.reject(c);
    if (spawnStructure(sim, bi, pl, c.player, Structure::State::Deployed, 0, 1_fx, c.type).isNull()) return sim.reject(c);
    k.started = true;
    // The map's ResourceStockpileDefault rule (Golden Fields: 1000) over the SVar.
    k.money = std::clamp(sim.config().map != nullptr ? sim.config().map->rule("ResourceStockpileDefault", d->economy.startMoney) : d->economy.startMoney,
                         Fixed{}, d->economy.stockpileMax);
}

Blocker recruitBlocker(const Sim& sim, std::uint8_t player, EntityId building, std::string_view ids) {
    const Structure* s = ownStructure(sim, player, building);
    const BuildingDef* b = s != nullptr ? buildingOf(sim, building) : nullptr;
    if (b == nullptr || player >= kMaxPlayers) return blocked("unknown");
    if (s->state != Structure::State::Deployed) return blocked("not_deployed");
    if (s->queue.size() >= kMaxRecruitQueue) return blocked("queue_full");
    const DefSet& d = *sim.config().defs;
    const std::uint32_t regiment = recruitRegiment(d, *b, ids);
    if (regiment == kNoDef) return blocked("unknown");
    const std::vector<RecruitElement> plan = recruitPlan(d, regiment);
    if (plan.empty()) return blocked("unknown");
    // Requirements of every unit: structures owned and deployed, properties on this building.
    const std::vector<std::uint32_t> props = effectiveProperties(sim, building);
    for (const RecruitElement& e : plan) {
        if (e.element.kind != ElementKind::Unit) continue;
        const UnitDef& u = d.units[e.element.index];
        for (const std::string& need : u.requiredStructureIds) {
            if (!hasDeployed(sim, player, need)) return blocked("requires_structure", need);
        }
        for (const std::string& need : u.requiredProperties) {
            const std::uint32_t pi = d.propertyIndex(need);
            if (pi == kNoDef || !std::binary_search(props.begin(), props.end(), pi)) return blocked("requires_property", need);
        }
        if (u.unpackTo != kNoDef && d.buildings[u.unpackTo].providesCorps && hasCorps(sim, player, EntityId{})) return blocked("corps_exists");
    }
    // Limits (reservations included) and money.
    const Ledger l = ledgerOf(sim, player);
    const ResourceAmounts need = recruitLimits(d, regiment, 0, true);
    if (need.unitLimitConsumed > Fixed{} && l.upkeep.unitLimitConsumed + need.unitLimitConsumed > l.unitLimit) return blocked("unit_limit");
    if (need.buildingLimitConsumed > Fixed{} && l.upkeep.buildingLimitConsumed + need.buildingLimitConsumed > l.buildingLimit) {
        return blocked("building_limit");
    }
    const Kingdom& k = sim.world().kingdoms[player];
    const Fixed cost = recruitCost(d, regiment);
    if (!k.started) return blocked("not_started");
    if (k.money < cost) return blocked("money", std::to_string(cost.roundInt()));
    return {};
}

void applyRecruit(Sim& sim, const Command& c, const BaseOrderPayload& p) {
    if (!recruitBlocker(sim, c.player, p.target, p.ids).ok()) return sim.reject(c);
    const DefSet& d = *sim.config().defs;
    const std::uint32_t regiment = recruitRegiment(d, *buildingOf(sim, p.target), p.ids);
    const Fixed cost = recruitCost(d, regiment);
    Kingdom& k = sim.world().kingdoms[c.player];
    k.money = k.money - cost;
    ownStructure(sim, c, p.target)->queue.push_back({regiment, cost});
}

void applyCancelRecruit(Sim& sim, const Command& c, const BaseOrderPayload& p) {
    Structure* s = ownStructure(sim, c, p.target);
    if (s == nullptr || p.index >= s->queue.size() || (p.index == 0 && !s->recruit.isNull())) return sim.reject(c);
    Kingdom& k = sim.world().kingdoms[c.player];
    k.money = std::min(k.money + s->queue[p.index].paid, sim.config().defs->economy.stockpileMax);
    s->queue.erase(s->queue.begin() + p.index);
}

void applyUnpack(Sim& sim, const Command& c, const BaseOrderPayload& p) {
    World& w = sim.world();
    const DefSet* d = sim.config().defs;
    const Regiment* r = w.registry.alive(p.target) ? w.regiments.get(p.target) : nullptr;
    if (d == nullptr || sim.nav() == nullptr || r == nullptr || r->player != c.player) return sim.reject(c);
    if (const Engagement* g = w.engagements.get(p.target);
        g != nullptr && (g->state == Engagement::State::Routing || g->state == Engagement::State::Exhausted)) {
        return sim.reject(c);
    }
    std::uint32_t building = kNoDef;
    for (const EntityId m : membersOf(w, p.target)) {
        const DefRef* dr = w.defRefs.get(m);
        if (dr != nullptr && dr->kind == DefRef::Kind::Unit && d->units[dr->index].unpackTo != kNoDef) {
            building = d->units[dr->index].unpackTo;
            break;
        }
    }
    if (building == kNoDef) return sim.reject(c);
    const Placement pl = placeBuilding(sim, building, p.position, p.facing, c.player, p.target);
    if (!pl.ok) return sim.reject(c);
    w.deploys.set(p.target, {building, pl.center, pl.facing});
    detachFromGroup(sim, p.target);
    clearEngagement(sim, p.target);
    restartOrder(sim, p.target, pl.center, true, pl.facing);
}

Blocker packBlocker(const Sim& sim, std::uint8_t player, EntityId building) {
    const Structure* s = ownStructure(sim, player, building);
    const BuildingDef* b = s != nullptr ? buildingOf(sim, building) : nullptr;
    const Health* h = sim.world().health.get(building);
    if (b == nullptr || h == nullptr) return blocked("unknown");
    if (s->state != Structure::State::Deployed) return blocked("not_deployed");
    if (!b->mobile || b->packedRegiment == kNoDef) return blocked("not_mobile");
    if (h->current < h->max * b->packHealthMin) return blocked("health");
    if (!s->queue.empty() || s->research != Structure::kNone) return blocked("busy");
    return {};
}

void applyPack(Sim& sim, const Command& c, const BaseOrderPayload& p) {
    if (!packBlocker(sim, c.player, p.target).ok()) return sim.reject(c);
    Structure* s = ownStructure(sim, c, p.target);
    s->state = Structure::State::Packing;
    s->stateTicks = timerOf(buildingOf(sim, p.target)->packingLength);
}

Blocker researchBlocker(const Sim& sim, std::uint8_t player, EntityId building, std::string_view propertyIds) {
    const Structure* s = ownStructure(sim, player, building);
    const BuildingDef* b = s != nullptr ? buildingOf(sim, building) : nullptr;
    if (b == nullptr || player >= kMaxPlayers) return blocked("unknown");
    if (s->state != Structure::State::Deployed) return blocked("not_deployed");
    if (s->research != Structure::kNone) return blocked("researching");
    const std::uint32_t item = researchItem(*b, propertyIds);
    if (item == Structure::kNone) return blocked("unknown");
    const ResearchItemDef& r = b->research[item];
    if (r.property == kNoDef || r.scope == ResearchItemDef::Scope::Support) return blocked("unknown");
    const World& w = sim.world();
    const Kingdom& k = w.kingdoms[player];
    const std::vector<std::uint32_t>& have = r.scope == ResearchItemDef::Scope::Local ? s->properties : k.properties;
    if (std::binary_search(have.begin(), have.end(), r.property)) return blocked("researched");
    if (r.scope == ResearchItemDef::Scope::Global) {  // not twice at once
        for (std::size_t i = 0; i < w.structures.size(); ++i) {
            const Structure& o = w.structures.data()[i];
            const BuildingDef* ob = buildingOf(sim, w.structures.ids()[i]);
            if (o.player == player && o.research != Structure::kNone && ob != nullptr && o.research < ob->research.size() &&
                ob->research[o.research].property == r.property) {
                return blocked("researching");
            }
        }
    }
    if (!k.started) return blocked("not_started");
    if (k.money < r.cost.money) return blocked("money", std::to_string(r.cost.money.roundInt()));
    return {};
}

void applyResearch(Sim& sim, const Command& c, const BaseOrderPayload& p) {
    if (!researchBlocker(sim, c.player, p.target, p.ids).ok()) return sim.reject(c);
    Structure* s = ownStructure(sim, c, p.target);
    const std::uint32_t item = researchItem(*buildingOf(sim, p.target), p.ids);
    const ResearchItemDef& r = buildingOf(sim, p.target)->research[item];
    Kingdom& k = sim.world().kingdoms[c.player];
    k.money = k.money - r.cost.money;
    s->research = item;
    s->researchTicks = timerOf(r.time);
    s->researchPaid = r.cost.money;
}

void applyCancelResearch(Sim& sim, const Command& c, const BaseOrderPayload& p) {
    Structure* s = ownStructure(sim, c, p.target);
    if (s == nullptr || s->research == Structure::kNone) return sim.reject(c);
    Kingdom& k = sim.world().kingdoms[c.player];
    k.money = std::min(k.money + s->researchPaid, sim.config().defs->economy.stockpileMax);
    s->research = Structure::kNone;
    s->researchTicks = 0;
    s->researchPaid = Fixed{};
}

void applyDebugSetMoney(Sim& sim, const Command& c, const BaseOrderPayload& p) {
    const DefSet* d = sim.config().defs;
    if (d == nullptr || c.player >= kMaxPlayers) return sim.reject(c);
    sim.world().kingdoms[c.player].money = std::clamp(p.amount, Fixed{}, d->economy.stockpileMax);
}

void applyDebugGrantProperty(Sim& sim, const Command& c, const BaseOrderPayload& p) {
    const DefSet* d = sim.config().defs;
    const std::uint32_t pi = d != nullptr ? d->propertyIndex(p.ids) : kNoDef;
    if (pi == kNoDef || c.player >= kMaxPlayers) return sim.reject(c);
    std::vector<std::uint32_t>& to = sim.world().kingdoms[c.player].properties;
    const auto it = std::lower_bound(to.begin(), to.end(), pi);
    if (it == to.end() || *it != pi) to.insert(it, pi);
}

// --- Timers stage ---------------------------------------------------------------------------------------

void runBases(Sim& sim) {
    World& w = sim.world();
    if (sim.config().defs == nullptr || (w.deploys.size() == 0 && w.structures.size() == 0)) return;
    // Truck regiments on their way to unpack: re-ordered elsewhere, arrived, or stopped short.
    const std::vector<EntityId> deploying = w.deploys.ids();
    for (const EntityId org : deploying) {
        const Deploy* d = w.deploys.get(org);
        const Order* o = w.registry.alive(org) ? w.orders.get(org) : nullptr;
        const Transform* t = w.transforms.get(org);
        if (d == nullptr) continue;
        if (o == nullptr || t == nullptr || !w.regiments.has(org) || o->goal != d->at) {
            w.deploys.removeIndex(org.index());
            continue;
        }
        if (o->state != Order::State::Idle) continue;
        const Deploy deploy = *d;
        w.deploys.removeIndex(org.index());
        if (distance(t->position, deploy.at) <= 2_fx) {
            startUnpack(sim, org, deploy);
        } else {
            sim.emit({SimEvent::Type::DeployFailed, org, w.regiments.get(org)->player, CommandType::Unpack});
        }
    }
    const std::vector<EntityId> ids = w.structures.ids();
    std::vector<SupplyZone> zones;
    bool haveZones = false;
    for (const EntityId id : ids) {
        Structure* s = w.registry.alive(id) ? w.structures.get(id) : nullptr;
        if (s == nullptr) continue;
        switch (s->state) {
        case Structure::State::Unpacking:
            if (s->stateTicks > 1) {
                --s->stateTicks;
            } else {
                s->state = Structure::State::Deployed;
                s->stateTicks = 0;
                sim.emit({SimEvent::Type::Unpacked, id, s->player, CommandType::Unpack});
            }
            break;
        case Structure::State::Packing:
            if (s->stateTicks > 1) {
                --s->stateTicks;
            } else {
                finishPack(sim, id, *s);
            }
            break;
        case Structure::State::Deployed:
            if (!s->queue.empty()) {
                if (!haveZones) {
                    zones = supplyZones(sim);
                    haveZones = true;
                }
                if (zoneAt(zones, w.teamOf(s->player), w.transforms.get(id)->position) != nullptr) recruitStep(sim, id, *s);
            }
            s = w.structures.get(id);  // recruiting adds entities (pools may move)
            researchStep(sim, id, *s);
            break;
        }
    }
}

}  // namespace corps
