#include "ai/view.hpp"

#include <algorithm>

#include "core/sim.hpp"
#include "nav/nav_world.hpp"
#include "systems/bases.hpp"
#include "systems/orders.hpp"

namespace corps::ai {

namespace {

Fixed sq(Fixed r) { return r * r; }

}  // namespace

View::View(std::uint8_t player, std::uint32_t memoryTicks) : me(player), memoryTicks(memoryTicks) {}

void View::update(const Sim& sim) {
    const World& w = sim.world();
    now = sim.tick();
    money = armyXp = Fixed{};
    bankrupt = started = eliminated = false;
    ledger = Ledger{};
    research.clear();
    buildings.clear();
    regiments.clear();
    allies.clear();
    enemies.clear();
    eyes.clear();
    seenNow.clear();
    cityList.clear();
    zones.clear();
    grid = sim.nav() != nullptr ? &sim.nav()->grid : nullptr;
    if (me >= kMaxPlayers || sim.config().defs == nullptr) return;
    const DefSet& d = *sim.config().defs;
    const Kingdom& k = w.kingdoms[me];
    myTeam = k.team;
    started = k.started;
    eliminated = k.eliminated;
    money = k.money;
    armyXp = k.armyXp;
    bankrupt = k.bankrupt;
    research = k.properties;
    ledger = ledgerOf(sim, me);
    for (std::uint8_t p = 0; p < kMaxPlayers; ++p) {
        const Kingdom& o = w.kingdoms[p];
        if (p == me || (!o.started && !o.active) || o.eliminated) continue;
        (w.hostile(p, me) ? enemies : allies).push_back(p);
    }
    const auto friendly = [&](std::uint8_t p) { return p < kMaxPlayers && !w.hostile(p, me); };
    const auto hostile = [&](std::uint8_t p) { return p < kMaxPlayers && w.hostile(p, me); };

    // The team's eyes: living units (their own range, else their regiment's) and structures.
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        const EntityId id = w.members.ids()[i];
        const Regiment* r = w.regiments.get(w.members.data()[i].organization);
        const Transform* t = w.transforms.get(id);
        const DefRef* ref = w.defRefs.get(id);
        if (r == nullptr || t == nullptr || ref == nullptr || !friendly(r->player) || !w.registry.alive(id)) continue;
        Fixed range = ref->kind == DefRef::Kind::Unit && ref->index < d.units.size() ? d.units[ref->index].detectionRange : Fixed{};
        if (range <= Fixed{}) {
            const DefRef* rr = w.defRefs.get(w.members.data()[i].organization);
            if (rr != nullptr && rr->kind == DefRef::Kind::Regiment && rr->index < d.regiments.size()) range = d.regiments[rr->index].detectionRange;
        }
        if (range > Fixed{}) eyes.push_back({t->position, sq(range)});
    }
    for (std::size_t i = 0; i < w.structures.size(); ++i) {
        const EntityId id = w.structures.ids()[i];
        const Transform* t = w.transforms.get(id);
        if (t != nullptr && friendly(w.structures.data()[i].player) && w.registry.alive(id)) {
            eyes.push_back({t->position, sq(Fixed::fromInt(kStructureSight))});
        }
    }

    // Structures: own, cities, and visible hostile ones.
    for (std::size_t i = 0; i < w.structures.size(); ++i) {
        const EntityId id = w.structures.ids()[i];
        const Structure& s = w.structures.data()[i];
        const DefRef* ref = w.defRefs.get(id);
        const Transform* t = w.transforms.get(id);
        if (ref == nullptr || ref->kind != DefRef::Kind::Building || ref->index >= d.buildings.size() || t == nullptr ||
            !w.registry.alive(id)) {
            continue;
        }
        const BuildingDef& b = d.buildings[ref->index];
        const Health* h = w.health.get(id);
        if (b.cityZoneRadius > Fixed{}) cityList.push_back({id, t->position, s.player, b.cityZoneRadius, cityValue(d, ref->index)});
        if (s.player == me) {
            OwnBuilding o;
            o.id = id;
            o.def = ref->index;
            o.at = t->position;
            o.state = s.state;
            o.health = h != nullptr ? h->current : Fixed{};
            o.healthMax = h != nullptr ? h->max : Fixed{};
            o.corps = b.providesCorps;
            o.hq = b.hq;
            o.queued = static_cast<std::uint32_t>(s.queue.size());
            for (const RecruitOrder& q : s.queue) o.queue.push_back(q.regiment);
            o.researching = s.research != Structure::kNone;
            o.inSupply = s.inSupply;
            o.underSiege = s.underSiege;
            buildings.push_back(o);
        } else if (hostile(s.player) && sees(t->position)) {
            seenNow.push_back({id, s.player, true, ref->index, t->position, 1, h != nullptr ? h->current : Fixed{}, b.hq || b.providesCorps, now});
        }
    }

    // Regiments: own, and visible hostile ones (a regiment shows when any of its units does).
    std::vector<EntityId> forming;
    for (std::size_t i = 0; i < w.structures.size(); ++i) {
        if (w.structures.data()[i].player == me && !w.structures.data()[i].recruit.isNull()) forming.push_back(w.structures.data()[i].recruit);
    }
    std::sort(forming.begin(), forming.end());
    std::map<EntityId, std::size_t> index;  // regiment -> its entry in `regiments` or `seenNow`
    std::map<EntityId, bool> isVisible;
    for (std::size_t i = 0; i < w.regiments.size(); ++i) {
        const EntityId id = w.regiments.ids()[i];
        const Regiment& r = w.regiments.data()[i];
        const DefRef* ref = w.defRefs.get(id);
        const Transform* t = w.transforms.get(id);
        if (ref == nullptr || ref->kind != DefRef::Kind::Regiment || ref->index >= d.regiments.size() || t == nullptr ||
            !w.registry.alive(id)) {
            continue;
        }
        const RegimentDef& rd = d.regiments[ref->index];
        if (r.player == me) {
            OwnRegiment o;
            o.id = id;
            o.def = ref->index;
            o.at = t->position;
            o.hq = rd.hq;
            o.unpacksTo = kNoDef;
            o.forming = std::binary_search(forming.begin(), forming.end(), id);
            o.deploying = w.deploys.has(id);
            const Engagement* e = w.engagements.get(id);
            if (e != nullptr) {
                o.state = e->state;
                o.mode = e->mode;
                o.target = e->target;
                o.morale = e->morale;
                o.efficiency = e->efficiency;
                o.xp = e->xp;
            }
            o.orderable = e == nullptr || (e->state != Engagement::State::Routing && e->state != Engagement::State::Retreating &&
                                           e->state != Engagement::State::Exhausted);
            const Order* ord = w.orders.get(orderOwner(sim, id));
            o.idle = (ord == nullptr || ord->state == Order::State::Idle) && o.mode == Engagement::Mode::None && o.orderable;
            const Supply* sup = w.supplies.get(id);
            o.inSupply = sup != nullptr && sup->inSupply;
            index[id] = regiments.size();
            regiments.push_back(o);
        } else if (hostile(r.player)) {
            index[id] = seenNow.size();
            isVisible[id] = false;
            seenNow.push_back({id, r.player, false, ref->index, t->position, 0, Fixed{}, rd.hq, now});
        }
    }
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        const EntityId id = w.members.ids()[i];
        const EntityId org = w.members.data()[i].organization;
        const auto it = index.find(org);
        const Transform* t = w.transforms.get(id);
        if (it == index.end() || t == nullptr || !w.registry.alive(id)) continue;
        const Health* h = w.health.get(id);
        const DefRef* ref = w.defRefs.get(id);
        const Regiment* r = w.regiments.get(org);
        if (r->player == me) {
            OwnRegiment& o = regiments[it->second];
            ++o.units;
            if (h != nullptr) {
                o.health += h->current;
                o.healthMax += h->max;
            }
            if (ref != nullptr && ref->kind == DefRef::Kind::Unit && d.units[ref->index].unpackTo != kNoDef) {
                o.truck = true;
                o.unpacksTo = d.units[ref->index].unpackTo;
            }
        } else if (sees(t->position)) {
            Seen& s = seenNow[it->second];
            isVisible[org] = true;
            ++s.units;
            if (h != nullptr) s.health += h->current;
        }
    }
    seenNow.erase(std::remove_if(seenNow.begin(), seenNow.end(),
                                 [&](const Seen& s) { return !s.structure && !isVisible[s.id]; }),
                  seenNow.end());
    std::sort(seenNow.begin(), seenNow.end(), [](const Seen& a, const Seen& b) { return a.id < b.id; });

    // Supply: the team's active zones.
    for (const SupplyZone& z : supplyZones(sim)) {
        if (z.active && z.team == myTeam) zones.push_back(z);
    }

    // Memory: refresh what is seen; forget what is old, and what the team now sees isn't there.
    for (const Seen& s : seenNow) memory[s.id] = s;
    for (auto it = memory.begin(); it != memory.end();) {
        const Seen& s = it->second;
        const bool stale = now >= s.tick + memoryTicks;
        const bool gone = s.tick != now && sees(s.at);
        it = stale || gone ? memory.erase(it) : std::next(it);
    }
}

std::vector<Seen> View::remembered() const {
    std::vector<Seen> out;
    out.reserve(memory.size());
    for (const auto& [id, s] : memory) out.push_back(s);
    return out;
}

const Seen* View::lastSeen(EntityId id) const {
    const auto it = memory.find(id);
    return it != memory.end() ? &it->second : nullptr;
}

bool View::sees(FVec2 at) const {
    return std::any_of(eyes.begin(), eyes.end(), [&](const Eye& e) { return distanceSq(e.at, at) <= e.rangeSq; });
}

bool View::supplied(FVec2 at) const { return zoneAt(zones, myTeam, at) != nullptr; }

bool View::passable(nav::MoveClass c, FVec2 at) const {
    if (grid == nullptr) return false;
    return grid->passable(c, static_cast<std::int32_t>(at.x.floorInt()), static_cast<std::int32_t>(at.y.floorInt()));
}

}  // namespace corps::ai
