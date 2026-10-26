#include "systems/supply.hpp"

#include <algorithm>

#include "core/ascii.hpp"
#include "core/sim.hpp"
#include "nav/map_def.hpp"
#include "nav/nav_world.hpp"
#include "systems/bases.hpp"
#include "systems/combat.hpp"
#include "systems/morale.hpp"
#include "systems/movement.hpp"

namespace corps {

using namespace fixed_literals;

namespace {

constexpr Fixed kCaptureSeconds = 5_fx;   // [CALIBRATE]
constexpr Fixed kCaptureMargin = 4_fx;    // capture radius = half the footprint + this [CALIBRATE]
const Fixed kTicksPerMinute = Fixed::fromInt(60 * static_cast<std::int32_t>(kTicksPerSecond));

std::uint32_t ticksOf(Fixed seconds) {
    const std::int64_t t = (seconds * static_cast<std::int32_t>(kTicksPerSecond)).ceilInt();
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(t, 1, 0x7FFFFFFF));
}

const BuildingDef* buildingOf(const Sim& sim, EntityId id) {
    const DefSet* d = sim.config().defs;
    const DefRef* r = d != nullptr ? sim.world().defRefs.get(id) : nullptr;
    if (r == nullptr || r->kind != DefRef::Kind::Building || r->index >= d->buildings.size()) return nullptr;
    return &d->buildings[r->index];
}

const RegimentDef* regimentOf(const Sim& sim, EntityId id) {
    const DefSet* d = sim.config().defs;
    const DefRef* r = d != nullptr ? sim.world().defRefs.get(id) : nullptr;
    if (r == nullptr || r->kind != DefRef::Kind::Regiment || r->index >= d->regiments.size()) return nullptr;
    return &d->regiments[r->index];
}

// A living unit (or building element) of a player's regiment.
struct Body {
    EntityId id;
    EntityId org;
    FVec2 pos;
    std::uint8_t player = 0;
    std::uint8_t team = 0;
};

std::vector<Body> liveBodies(const Sim& sim) {
    std::vector<Body> out;
    const World& w = sim.world();
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        const EntityId id = w.members.ids()[i];
        const Regiment* r = w.regiments.get(w.members.data()[i].organization);
        const Health* h = w.health.get(id);
        const Transform* t = w.transforms.get(id);
        if (!w.registry.alive(id) || r == nullptr || h == nullptr || t == nullptr || h->current <= Fixed{}) continue;
        out.push_back({id, w.members.data()[i].organization, t->position, r->player, w.teamOf(r->player)});
    }
    return out;
}

Fixed siegeRadius(const BuildingDef& b) {
    if (b.controlRange > Fixed{}) return b.controlRange;
    if (b.cityZoneRadius > Fixed{}) return b.cityZoneRadius;
    return b.supplyRange;
}

// The living members of a regiment, ascending.
std::vector<EntityId> livingMembers(const World& w, EntityId org) {
    std::vector<EntityId> out;
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        if (w.members.data()[i].organization != org) continue;
        const EntityId id = w.members.ids()[i];
        const Health* h = w.health.get(id);
        if (w.registry.alive(id) && h != nullptr && h->current > Fixed{}) out.push_back(id);
    }
    return out;
}

// Ticks between free replacements: the regiment's recruit interval, slowed by `factor` (< 1).
std::uint32_t replaceInterval(const RegimentDef& rd, std::size_t units, Fixed factor) {
    const std::uint32_t total = ticksOf(rd.recruitTime);
    const auto n = static_cast<std::uint32_t>(std::max<std::size_t>(units, 1));
    const std::uint32_t per = std::max<std::uint32_t>(1, (total + n - 1) / n);
    if (factor >= Fixed::fromInt(1)) return per;
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>((Fixed::fromInt(static_cast<std::int32_t>(per)) / factor).ceilInt(), 1, 0x7FFFFFFF));
}

// The first unit of the regiment's plan whose layout element has no living member (none: -1).
std::int64_t firstMissing(const Sim& sim, const std::vector<RecruitElement>& plan, const std::vector<EntityId>& living) {
    const World& w = sim.world();
    std::vector<std::uint32_t> slots;
    for (const EntityId m : living) slots.push_back(w.members.get(m)->slot);
    std::sort(slots.begin(), slots.end());
    for (std::size_t i = 0; i < plan.size(); ++i) {
        if (plan[i].element.kind != ElementKind::Unit) continue;
        if (!std::binary_search(slots.begin(), slots.end(), plan[i].slot)) return static_cast<std::int64_t>(i);
    }
    return -1;
}

// Health multiplier of the regiment's level relative to level 0 (replacements join at its level).
Fixed levelHealthFactor(const Sim& sim, EntityId org, const RegimentDef& rd) {
    const Engagement* e = sim.world().engagements.get(org);
    if (e == nullptr || rd.experienceLevels.empty() || e->level >= rd.experienceLevels.size()) return Fixed::fromInt(1);
    const Fixed base = rd.experienceLevels[0].healthMultiplier;
    return base > Fixed{} ? rd.experienceLevels[e->level].healthMultiplier / base : Fixed::fromInt(1);
}

bool replaceUnit(Sim& sim, EntityId org, const RegimentDef& rd, const RecruitElement& e) {
    World& w = sim.world();
    const Regiment* r = w.regiments.get(org);
    const Transform* a = w.transforms.get(org);
    if (r == nullptr || a == nullptr) return false;
    FVec2 pos = a->position;
    const DefSet& d = *sim.config().defs;
    if (r->layout != kNoDef && e.slot < d.layouts[r->layout].elementSlot.size()) {
        pos = a->position + layoutOffset(d, *r, r->formation, e.slot).rotated(a->facing);
    }
    const Angle facing = a->facing;
    const EntityId u = addRegimentUnit(sim, org, e, false, pos, facing, CommandType::Noop, levelHealthFactor(sim, org, rd));
    if (u.isNull()) return false;
    sim.emit({SimEvent::Type::Replaced, u, w.regiments.get(org)->player, CommandType::Noop, org, 0, pos, facing});
    return true;
}

// Bankrupt attrition: a unit dies of shortfall.
void starve(Sim& sim, EntityId unit, std::uint8_t player) {
    World& w = sim.world();
    w.health.get(unit)->current = Fixed{};
    w.registry.destroyLater(unit);
    const Transform* t = w.transforms.get(unit);
    sim.emit({SimEvent::Type::Died, unit, player, CommandType::Noop, EntityId{}, 0, t != nullptr ? t->position : FVec2{},
              t != nullptr ? t->facing : Angle{}});
    moraleOnKill(sim, unit, EntityId{});
}

bool hasHq(const Sim& sim, std::uint8_t player) {
    const World& w = sim.world();
    for (std::size_t i = 0; i < w.structures.size(); ++i) {
        const EntityId id = w.structures.ids()[i];
        const BuildingDef* b = buildingOf(sim, id);
        const Health* h = w.health.get(id);
        if (w.structures.data()[i].player == player && w.registry.alive(id) && b != nullptr && b->hq &&
            (h == nullptr || h->current > Fixed{})) {
            return true;
        }
    }
    for (std::size_t i = 0; i < w.regiments.size(); ++i) {
        const EntityId id = w.regiments.ids()[i];
        const RegimentDef* rd = regimentOf(sim, id);
        if (w.regiments.data()[i].player != player || !w.registry.alive(id) || rd == nullptr || !rd->hq) continue;
        for (std::size_t m = 0; m < w.members.size(); ++m) {  // only with a unit left
            const Health* h = w.health.get(w.members.ids()[m]);
            if (w.members.data()[m].organization == id && h != nullptr && h->current > Fixed{}) return true;
        }
    }
    return false;
}

void endMatch(Sim& sim, SimEvent::Type type, std::uint8_t winner) {
    Match& m = sim.world().match;
    m.over = true;
    m.winner = winner;
    m.endTick = sim.tick();
    sim.emit({type, EntityId{}, winner, CommandType::Noop, EntityId{}, winner});
}

void checkVictory(Sim& sim) {
    World& w = sim.world();
    const DefSet& d = *sim.config().defs;
    const auto side = [&](std::size_t p) { return w.match.teamVictory ? w.teamOf(static_cast<std::uint8_t>(p)) : static_cast<std::uint8_t>(p); };
    std::vector<std::uint8_t> played, standing;
    for (std::size_t p = 0; p < kMaxPlayers; ++p) {
        if (!w.kingdoms[p].started) continue;
        played.push_back(side(p));
        if (!w.kingdoms[p].eliminated) standing.push_back(side(p));
    }
    const auto uniq = [](std::vector<std::uint8_t>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    uniq(played);
    uniq(standing);
    if (played.size() >= 2 && standing.size() <= 1) {
        return endMatch(sim, SimEvent::Type::TeamVictory, standing.empty() ? Match::kNoTeam : standing[0]);
    }
    if (w.match.cityVictoryRatio <= Fixed{} || played.empty()) return;
    Fixed total;
    std::array<Fixed, 256> held{};
    for (std::size_t i = 0; i < w.structures.size(); ++i) {
        const Structure& s = w.structures.data()[i];
        const DefRef* r = w.defRefs.get(w.structures.ids()[i]);
        if (r == nullptr || r->kind != DefRef::Kind::Building || !isCity(d, r->index)) continue;
        const Fixed v = cityValue(d, r->index);
        total = total + v;
        if (s.player < kMaxPlayers && !w.kingdoms[s.player].eliminated) held[side(s.player)] = held[side(s.player)] + v;
    }
    if (total <= Fixed{}) return;
    for (std::size_t k = 0; k < held.size(); ++k) {
        if (held[k] > Fixed{} && held[k] >= total * w.match.cityVictoryRatio) {
            return endMatch(sim, SimEvent::Type::CityRatioVictory, static_cast<std::uint8_t>(k));
        }
    }
}

}  // namespace

// --- zones -----------------------------------------------------------------------------------------

std::vector<SupplyZone> supplyZones(const Sim& sim) {
    std::vector<SupplyZone> out;
    const DefSet* d = sim.config().defs;
    if (d == nullptr) return out;
    const World& w = sim.world();
    const EconomyDef& e = d->economy;
    const std::uint32_t siegeTicks = ticksOf(e.siegeDamageSeconds);
    std::vector<Body> bodies;
    bool haveBodies = false;
    for (std::size_t i = 0; i < w.structures.size(); ++i) {
        const EntityId id = w.structures.ids()[i];
        const Structure& s = w.structures.data()[i];
        if (!w.registry.alive(id) || s.state != Structure::State::Deployed || s.player >= kMaxPlayers || w.kingdoms[s.player].eliminated) {
            continue;
        }
        const BuildingDef* b = buildingOf(sim, id);
        const Health* h = w.health.get(id);
        const Transform* t = w.transforms.get(id);
        if (b == nullptr || t == nullptr || b->supplyRange <= Fixed{} || (h != nullptr && h->current <= Fixed{})) continue;
        SupplyZone z;
        z.source = id;
        z.center = t->position;
        z.radius = b->supplyRange;
        z.team = w.teamOf(s.player);
        z.relay = b->supplyRelay;
        z.besieged = s.lastDamaged != 0 && sim.tick() + 1 - s.lastDamaged < siegeTicks;
        if (!z.besieged) {
            if (!haveBodies) {
                bodies = liveBodies(sim);
                haveBodies = true;
            }
            const Fixed r = siegeRadius(*b);
            for (const Body& u : bodies) {
                if (w.hostile(u.player, s.player) && distance(u.pos, z.center) <= r) {
                    z.besieged = true;
                    break;
                }
            }
        }
        out.push_back(z);
    }
    const auto supplies = [&](const SupplyZone& z) { return !z.besieged || e.supplyUnderSiege; };
    for (SupplyZone& z : out) z.active = !z.relay && supplies(z);
    // Relays: linked to an active zone of their team (chains included).
    for (bool changed = true; changed;) {
        changed = false;
        for (SupplyZone& z : out) {
            if (z.active || !z.relay || !supplies(z)) continue;
            for (const SupplyZone& o : out) {
                if (&o != &z && o.active && o.team == z.team && distance(z.center, o.center) <= o.radius) {
                    z.active = true;
                    changed = true;
                    break;
                }
            }
        }
    }
    return out;
}

const SupplyZone* zoneAt(const std::vector<SupplyZone>& zones, std::uint8_t team, FVec2 at) {
    const SupplyZone* best = nullptr;
    for (const SupplyZone& z : zones) {
        if (!z.active || z.team != team || distance(at, z.center) > z.radius) continue;
        if (best == nullptr || (best->besieged && !z.besieged)) best = &z;
    }
    return best;
}

// --- cities and map starts ------------------------------------------------------------------------

bool isCity(const DefSet& defs, std::uint32_t building) {
    return building < defs.buildings.size() && defs.buildings[building].cityZoneRadius > Fixed{} && defs.buildings[building].captureable;
}

Fixed cityValue(const DefSet& defs, std::uint32_t building) {
    return building < defs.buildings.size() ? defs.buildings[building].production.money : Fixed{};
}

std::vector<MapStart> mapStarts(const nav::MapDef& map) {
    std::vector<MapStart> out;
    for (const nav::MapThing& t : map.things) {
        if (t.owner < 0 || t.owner >= static_cast<std::int32_t>(kMaxPlayers) || !istartsWith(t.ids, "placeholder_corps")) continue;
        const auto player = static_cast<std::uint8_t>(t.owner);
        if (std::any_of(out.begin(), out.end(), [&](const MapStart& s) { return s.player == player; })) continue;
        out.push_back({player, t.ids, t.position, t.facing});
    }
    std::sort(out.begin(), out.end(), [](const MapStart& a, const MapStart& b) { return a.player < b.player; });
    return out;
}

std::string placeholderThing(const DefSet& defs, std::string_view nation, std::string_view placeholder) {
    const NationDef* n = defs.nation(nation);
    if (n == nullptr) n = defs.nation("nation_" + std::string(nation));
    if (n == nullptr) return {};
    for (const auto& [from, to] : n->placeholders) {
        if (iequals(from, placeholder)) return to;
    }
    return {};
}

void spawnMapCities(Sim& sim) {
    const DefSet* d = sim.config().defs;
    const nav::MapDef* map = sim.config().map;
    if (d == nullptr || map == nullptr || sim.nav() == nullptr) return;
    std::vector<std::uint8_t> playable;
    for (const MapStart& s : mapStarts(*map)) playable.push_back(s.player);
    World& w = sim.world();
    const nav::NavGrid& g = sim.nav()->grid;
    for (const nav::MapThing& t : map->things) {
        const std::uint32_t bi = d->buildingIndex(t.ids);
        if (bi == kNoDef || !isCity(*d, bi)) continue;
        const BuildingDef& b = d->buildings[bi];
        // The footprint as placement computes it; the map decides where cities stand, so the
        // buildability checks don't apply.
        const Placement pl = placeBuilding(sim, bi, t.position, t.facing, kNeutral);
        if (!g.inside(pl.rect.x0, pl.rect.y0) || !g.inside(pl.rect.x1, pl.rect.y1)) continue;
        const EntityId id = w.registry.create();
        if (id.isNull()) return;
        const bool owned = t.owner >= 0 && std::find(playable.begin(), playable.end(), static_cast<std::uint8_t>(t.owner)) != playable.end();
        w.transforms.set(id, {pl.center, pl.facing});
        w.defRefs.set(id, {DefRef::Kind::Building, bi});
        w.health.set(id, {b.health, b.health});
        w.footprints.set(id, pl.rect);
        sim.nav()->grid.stamp({pl.rect.x0, pl.rect.y0, pl.rect.x1, pl.rect.y1});
        Structure s;
        s.player = owned ? static_cast<std::uint8_t>(t.owner) : kNeutral;
        w.structures.set(id, s);
    }
    sim.nav()->hpa.rebuildDirty();
}

// --- commands ----------------------------------------------------------------------------------------

void applySetMatchRules(Sim& sim, const Command& c, const MatchRulesPayload& p) {
    const DefSet* d = sim.config().defs;
    if (sim.tick() != 0 || p.teams.size() > kMaxPlayers || p.cityVictoryRatio < Fixed{} || p.cityVictoryRatio > Fixed::fromInt(1) ||
        p.startMoney < Fixed{} || (d != nullptr && p.startMoney > d->economy.stockpileMax)) {
        return sim.reject(c);
    }
    for (const std::uint8_t t : p.teams) {
        if (t >= kMaxPlayers) return sim.reject(c);
    }
    World& w = sim.world();
    for (std::size_t i = 0; i < p.teams.size(); ++i) w.kingdoms[i].team = p.teams[i];
    w.match.cityVictoryRatio = p.cityVictoryRatio;
    w.match.teamVictory = p.teamVictory;
    w.match.startMoney = p.startMoney;
}

void applyDebugSetHealth(Sim& sim, const Command& c, const BaseOrderPayload& p) {
    World& w = sim.world();
    Health* h = w.registry.alive(p.target) ? w.health.get(p.target) : nullptr;
    if (h == nullptr || p.amount <= Fixed{} || h->current <= Fixed{}) return sim.reject(c);
    h->current = min(p.amount, h->max);
}

void applyDebugSetOwner(Sim& sim, const Command& c, const BaseOrderPayload& p) {
    World& w = sim.world();
    Structure* s = w.registry.alive(p.target) ? w.structures.get(p.target) : nullptr;
    if (s == nullptr || (p.index >= kMaxPlayers && p.index != kNeutral)) return sim.reject(c);
    s->player = p.index;
    s->capturer = kNeutral;
    s->captureTicks = 0;
    if (Combat* cb = w.combats.get(p.target)) cb->player = p.index;
    if (p.index < kMaxPlayers) w.kingdoms[p.index].active = true;
}

// --- stages --------------------------------------------------------------------------------------------

void runSupply(Sim& sim) {
    const DefSet* d = sim.config().defs;
    if (d == nullptr) return;
    World& w = sim.world();
    if (w.structures.size() == 0 && w.supplies.size() == 0) return;
    const EconomyDef& e = d->economy;
    const std::vector<SupplyZone> zones = supplyZones(sim);

    for (std::size_t i = 0; i < w.structures.size(); ++i) {
        const EntityId id = w.structures.ids()[i];
        Structure& s = w.structures.data()[i];
        const Transform* t = w.transforms.get(id);
        s.underSiege = false;
        for (const SupplyZone& z : zones) {
            if (z.source == id) s.underSiege = z.besieged;
        }
        s.inSupply = s.player < kMaxPlayers && t != nullptr && zoneAt(zones, w.teamOf(s.player), t->position) != nullptr;
    }

    std::vector<EntityId> forming;  // regiments still being recruited: not replaced
    for (const Structure& s : w.structures.data()) {
        if (!s.recruit.isNull()) forming.push_back(s.recruit);
    }
    std::sort(forming.begin(), forming.end());

    const std::vector<EntityId> regs = w.regiments.ids();
    for (const EntityId org : regs) {
        const Regiment* r = w.registry.alive(org) ? w.regiments.get(org) : nullptr;
        const Transform* a = w.transforms.get(org);
        const RegimentDef* rd = regimentOf(sim, org);
        if (r == nullptr || a == nullptr || rd == nullptr || r->player >= kMaxPlayers) continue;
        const std::uint8_t player = r->player;
        const bool bankrupt = w.kingdoms[player].bankrupt;
        const SupplyZone* z = zoneAt(zones, w.teamOf(player), a->position);
        Supply sp = w.supplies.has(org) ? *w.supplies.get(org) : Supply{};
        sp.inSupply = z != nullptr && rd->supplyUsed;
        sp.besieged = sp.inSupply && z->besieged;
        Fixed factor = Fixed::fromInt(1);
        if (sp.besieged) factor = factor * e.underSiegeRate;
        if (bankrupt) factor = factor * e.underSiegeRate;
        const std::vector<EntityId> living = livingMembers(w, org);
        if (sp.inSupply) {
            for (const EntityId m : living) {
                const DefRef* dr = w.defRefs.get(m);
                Health* h = w.health.get(m);
                if (dr == nullptr || dr->kind != DefRef::Kind::Unit || dr->index >= d->units.size() || h->current >= h->max) continue;
                const Fixed rate = d->units[dr->index].resupplyRate;
                if (rate > Fixed{}) h->current = min(h->max, h->current + h->max * rate * factor / kTicksPerMinute);
            }
            const std::vector<RecruitElement> plan = recruitPlan(*d, w.defRefs.get(org)->index);
            const std::int64_t missing = living.empty() || std::binary_search(forming.begin(), forming.end(), org) || factor <= Fixed{}
                                             ? -1
                                             : firstMissing(sim, plan, living);
            if (missing < 0) {
                sp.replaceTicks = 0;
            } else if (sp.replaceTicks == 0) {
                sp.replaceTicks = replaceInterval(*rd, plan.size(), factor);
            } else if (sp.replaceTicks > 1) {
                --sp.replaceTicks;
            } else {
                sp.replaceTicks = 0;
                if (replaceUnit(sim, org, *rd, plan[static_cast<std::size_t>(missing)]) &&
                    firstMissing(sim, plan, livingMembers(w, org)) >= 0) {
                    sp.replaceTicks = replaceInterval(*rd, plan.size(), factor);
                }
            }
        } else {
            sp.replaceTicks = 0;
            if (bankrupt && rd->supplyUsed && rd->shortfallDamageRate > Fixed{} && !living.empty()) {
                std::size_t left = 0;
                for (const EntityId m : living) {
                    Health* h = w.health.get(m);
                    h->current = h->current - h->max * rd->shortfallDamageRate / Fixed::fromInt(static_cast<std::int32_t>(kTicksPerSecond));
                    if (h->current <= Fixed{}) {
                        starve(sim, m, player);
                    } else {
                        ++left;
                    }
                }
                if (left == 0) w.registry.destroyLater(org);
            }
        }
        w.supplies.set(org, sp);
    }
}

void runCapture(Sim& sim) {
    const DefSet* d = sim.config().defs;
    if (d == nullptr) return;
    World& w = sim.world();
    bool any = w.structures.size() != 0;
    for (const Kingdom& k : w.kingdoms) any = any || (k.started && !k.eliminated);
    if (!any) return;
    const std::uint32_t needed = ticksOf(kCaptureSeconds);
    std::vector<Body> bodies;
    bool haveBodies = false;
    const std::vector<EntityId> ids = w.structures.ids();
    for (const EntityId id : ids) {
        const DefRef* r = w.defRefs.get(id);
        const Transform* t = w.transforms.get(id);
        const Footprint* fp = w.footprints.get(id);
        if (r == nullptr || t == nullptr || fp == nullptr || r->kind != DefRef::Kind::Building || !isCity(*d, r->index)) continue;
        if (!haveBodies) {
            bodies = liveBodies(sim);
            haveBodies = true;
        }
        Structure& s = *w.structures.get(id);
        const Fixed zoneR = d->buildings[r->index].cityZoneRadius;
        const Fixed captureR = Fixed::fromInt(std::max(fp->x1 - fp->x0 + 1, fp->y1 - fp->y0 + 1)) / 2_fx + kCaptureMargin;
        const Body* nearest = nullptr;
        Fixed nearestD;
        std::uint8_t team = kNeutral;
        bool mixed = false;
        for (const Body& u : bodies) {
            const Fixed dd = distance(u.pos, t->position);
            if (dd <= zoneR) {
                if (team == kNeutral) team = u.team;
                else if (team != u.team) mixed = true;
            }
            if (dd <= captureR && (nearest == nullptr || dd < nearestD)) {
                nearest = &u;
                nearestD = dd;
            }
        }
        const bool taking = nearest != nullptr && !mixed && (team == kNeutral || team == nearest->team) &&
                            nearest->team != w.teamOf(s.player) && !w.kingdoms[nearest->player].eliminated;
        if (!taking) {
            s.capturer = kNeutral;
            s.captureTicks = 0;
            continue;
        }
        if (s.capturer != nearest->player) {
            s.capturer = nearest->player;
            s.captureTicks = 0;
        }
        if (++s.captureTicks < needed) continue;
        const std::uint8_t previous = s.player;
        const std::uint8_t player = nearest->player;
        const EntityId org = nearest->org;
        s.player = player;
        s.capturer = kNeutral;
        s.captureTicks = 0;
        s.lastDamaged = 0;
        if (Health* h = w.health.get(id)) h->current = max(h->current, h->max * d->economy.captureHealthFactor);
        w.kingdoms[player].active = true;
        grantExperience(sim, org, d->svars.getFixed("ExperienceDefaultValue", 1_fx) * d->economy.captureXpMultiplier);
        sim.emit({SimEvent::Type::CityCaptured, id, player, CommandType::Noop, org, previous, t->position, t->facing});
    }

    for (std::size_t p = 0; p < kMaxPlayers; ++p) {
        Kingdom& k = w.kingdoms[p];
        if (!k.started || k.eliminated || hasHq(sim, static_cast<std::uint8_t>(p))) continue;
        k.eliminated = true;
        sim.emit({SimEvent::Type::KingdomDefeated, EntityId{}, static_cast<std::uint8_t>(p), CommandType::Noop});
    }
    if (!w.match.over) checkVictory(sim);
}

}  // namespace corps
