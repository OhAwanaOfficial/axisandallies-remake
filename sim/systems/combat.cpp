#include "systems/combat.hpp"

#include <algorithm>
#include <tuple>

#include "combat/rules.hpp"
#include "nav/map_def.hpp"
#include "nav/nav_world.hpp"
#include "systems/economy.hpp"
#include "systems/morale.hpp"
#include "systems/orders.hpp"
#include "systems/stages.hpp"

namespace corps {

using namespace fixed_literals;

namespace {

constexpr Fixed kTick = 0.1_fx;
constexpr std::int32_t kBucket = 8;             // spatial buckets (cells)
constexpr std::uint32_t kNoTerrain = 0xFFFFFFFFu;
constexpr Fixed kDefaultZoc = 18_fx;
constexpr Fixed kMinRadius = 0.3_fx;
constexpr Fixed kMaxTargetRadius = 3_fx;        // query margin for target radii
constexpr std::uint32_t kNever = 0xFFFFFFFFu;

std::uint32_t ticksOf(Fixed seconds) {  // ≥ 1
    const std::int64_t t = (seconds * Fixed::fromInt(static_cast<std::int32_t>(kTicksPerSecond))).ceilInt();
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(t, 1, 0xFFFF));
}

// Angle units per tick for a rate in degrees per second.
std::int32_t turnUnits(Fixed degreesPerSecond) {
    const std::int64_t u = (degreesPerSecond * kTick * Fixed::fromInt(65536) / Fixed::fromInt(360)).floorInt();
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(u, 1, 32768));
}

std::int32_t angleUnits(Fixed degrees) {
    const std::int64_t u = (degrees * Fixed::fromInt(65536) / Fixed::fromInt(360)).floorInt();
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(u, 0, 65536));
}

Angle turnToward(Angle from, Angle to, std::int32_t maxUnits) {
    const std::int32_t d = Angle::delta(from, to);
    if (d > maxUnits) return from + Angle::fromUnits(static_cast<std::uint32_t>(maxUnits));
    if (d < -maxUnits) return from - Angle::fromUnits(static_cast<std::uint32_t>(maxUnits));
    return to;
}

struct Timers {
    std::uint32_t sample = 50, stalemate = 100, reform = 100, regroup = 100;
    Fixed attackFraction = 0.6_fx;
    bool keepKill = true;
};

Timers timersOf(const DefSet& d) {
    Timers t;
    const SVarsDef& v = d.svars;
    t.sample = ticksOf(v.getFixed("PrimarySampleTime", 5_fx));
    t.stalemate = ticksOf(v.getFixed("StalemateTime", 10_fx));
    t.reform = ticksOf(v.getFixed("ReformTime", 10_fx));
    t.regroup = ticksOf(v.getFixed("RegroupTime", 10_fx));
    t.attackFraction = v.getFixed("CAITargetInRangeZOCMultiplier", 0.6_fx);
    t.keepKill = v.getBool("KillTargetIsHighestPriority", true);
    return t;
}

// Everything that can fight or be shot this tick: live units and buildings with health left.
struct Fighter {
    EntityId id;
    FVec2 pos;
    Angle facing;
    std::uint8_t player = 0;
    EntityId org;  // its regiment, or null (a building on its own)
    const CombatProfile* profile = nullptr;
    const std::vector<WeaponDef>* weapons = nullptr;
    const std::vector<TurretDef>* turrets = nullptr;
    bool checkResist = false;
    Fixed radius;
    std::uint64_t types = 0;  // target types: its profile's, plus routing while its regiment routs
};

class Field {
public:
    explicit Field(const Sim& sim) {
        const World& w = sim.world();
        const DefSet& defs = *sim.config().defs;
        const CombatRules& rules = sim.combatRules();
        const std::uint32_t rt = defs.targetTypeIndex("routing");
        const std::uint64_t routingType = rt != kNoDef && rt < 64 ? std::uint64_t{1} << rt : 0;
        for (std::size_t i = 0; i < w.combats.size(); ++i) {
            const EntityId id = w.combats.ids()[i];
            const Transform* t = w.transforms.get(id);
            const Health* h = w.health.get(id);
            const DefRef* d = w.defRefs.get(id);
            if (!w.registry.alive(id) || t == nullptr || h == nullptr || d == nullptr || h->current <= Fixed{}) continue;
            Fighter f;
            f.id = id;
            f.pos = t->position;
            f.facing = t->facing;
            f.player = w.combats.data()[i].player;
            if (const Member* m = w.members.get(id)) f.org = m->organization;
            if (d->kind == DefRef::Kind::Unit && d->index < defs.units.size()) {
                const UnitDef& u = defs.units[d->index];
                f.profile = rules.unit(d->index);
                f.weapons = &u.weapons;
                f.turrets = &u.turrets;
                f.checkResist = u.checkDamageResistance;
                f.radius = max(u.blockingRadius, kMinRadius);
            } else if (d->kind == DefRef::Kind::Building && d->index < defs.buildings.size()) {
                const BuildingDef& b = defs.buildings[d->index];
                f.profile = rules.building(d->index);
                f.weapons = &b.weapons;
                f.turrets = &b.turrets;
                f.checkResist = b.checkDamageResistance;
                f.radius = max(b.blockingRadius, kMinRadius);
            } else {
                continue;
            }
            if (f.profile == nullptr) continue;
            f.types = f.profile->targetTypes;
            if (const Engagement* e = f.org.isNull() ? nullptr : w.engagements.get(f.org); e != nullptr && e->state == Engagement::State::Routing) {
                f.types |= routingType;  // actor_state_routing: enabled_target_flags routing
            }
            fs.push_back(f);
        }
        if (const nav::NavWorld* nav = sim.nav()) {
            bw = std::max<std::int32_t>(1, (nav->grid.width() + kBucket - 1) / kBucket);
            bh = std::max<std::int32_t>(1, (nav->grid.height() + kBucket - 1) / kBucket);
        }
        start.assign(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) + 1, 0);
        for (const Fighter& f : fs) ++start[bucketOf(f.pos) + 1];
        for (std::size_t b = 1; b < start.size(); ++b) start[b] += start[b - 1];
        std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
        items.resize(fs.size());
        for (std::uint32_t i = 0; i < fs.size(); ++i) items[fill[bucketOf(fs[i].pos)]++] = i;
    }

    std::vector<Fighter> fs;  // ascending id

    const Fighter* find(EntityId id) const {
        const auto it = std::lower_bound(fs.begin(), fs.end(), id, [](const Fighter& f, EntityId x) { return f.id < x; });
        return it != fs.end() && it->id == id ? &*it : nullptr;
    }

    // Indexes (ascending) of fighters whose centre is within `r` of `p`.
    void within(FVec2 p, Fixed r, std::vector<std::uint32_t>& out) const {
        out.clear();
        const std::int64_t span = r.ceilInt() / kBucket + 1;
        const std::int64_t cx = bx(p), cy = by(p);
        const Fixed r2 = r * r;
        for (std::int64_t y = std::max<std::int64_t>(cy - span, 0); y <= std::min<std::int64_t>(cy + span, bh - 1); ++y) {
            for (std::int64_t x = std::max<std::int64_t>(cx - span, 0); x <= std::min<std::int64_t>(cx + span, bw - 1); ++x) {
                const std::size_t b = static_cast<std::size_t>(y * bw + x);
                for (std::uint32_t k = start[b]; k < start[b + 1]; ++k) {
                    const FVec2 d = fs[items[k]].pos - p;
                    if (abs(d.x) <= r && abs(d.y) <= r && d.lengthSq() <= r2) out.push_back(items[k]);
                }
            }
        }
        std::sort(out.begin(), out.end());
    }

private:
    std::int64_t bx(FVec2 p) const { return std::clamp<std::int64_t>(p.x.floorInt() / kBucket, 0, bw - 1); }
    std::int64_t by(FVec2 p) const { return std::clamp<std::int64_t>(p.y.floorInt() / kBucket, 0, bh - 1); }
    std::size_t bucketOf(FVec2 p) const { return static_cast<std::size_t>(by(p) * bw + bx(p)); }

    std::int32_t bw = 1, bh = 1;
    std::vector<std::uint32_t> start, items;
};

// An instant (non-projectile) shot fired this tick; hits land together after every fighter fired.
struct Hit {
    std::uint32_t ability;
    EntityId shooter;
    std::uint8_t player;
    EntityId target;
    FVec2 point;
    Fixed mult;
};

struct Ctx {
    Sim& sim;
    World& w;
    const DefSet& defs;
    const CombatRules& rules;
    const CombatTuning& tuning;
    const Field& field;
    std::uint32_t tick;
    std::vector<Hit> hits;
};

Fixed healthOf(const World& w, EntityId id) {
    const Health* h = w.health.get(id);
    return h != nullptr ? h->current : Fixed{};
}

std::uint32_t terrainAt(const Sim& sim, FVec2 p) {
    const nav::MapDef* map = sim.config().map;
    if (map == nullptr || sim.nav() == nullptr || map->terrainId.size() != map->cellCount()) return kNoTerrain;
    return map->terrainId[sim.cellAt(p)];
}

bool entrenchedOf(const World& w, EntityId org) {
    const Engagement* e = org.isNull() ? nullptr : w.engagements.get(org);
    return e != nullptr && e->entrenched;
}

TargetStats statsOf(const Ctx& c, const Fighter& t) {
    TargetStats s = c.rules.statsOf(*t.profile, terrainAt(c.sim, t.pos), entrenchedOf(c.w, t.org));
    s.defense = s.defense + levelDefenseOf(c.sim, t.org) + researchDefenseValue(c.sim, t.id);  // level DV, research DV
    return s;
}

// The shooter's damage multiplier: its regiment's blended attack efficiency, level AV and the
// AV its kingdom's research gives it.
Fixed shooterMultiplier(const Ctx& c, const Fighter& f) {
    return attackMultiplierOf(c.sim, f.org, researchAttackValue(c.sim, f.id));  // level AV + research AV (M6)
}

// Routing, retreating and exhausted regiments are passive: no auto-engage, no fire.
bool passive(Engagement::State s) {
    return s == Engagement::State::Routing || s == Engagement::State::Retreating || s == Engagement::State::Exhausted;
}

bool formationAggressive(const DefSet& defs, const Regiment& r) {
    if (r.layout == kNoDef) return true;
    const LayoutDef& l = defs.layouts[r.layout];
    if (r.formation >= l.formations.size()) return true;
    const std::uint32_t fi = l.formations[r.formation].formationIndex;
    return fi == kNoDef || defs.formations[fi].aggressive;
}

bool active(Engagement::State s) {
    return s == Engagement::State::Engaging || s == Engagement::State::Stalemate || s == Engagement::State::Reform ||
           s == Engagement::State::Regroup;
}

void markCombat(Ctx& c, EntityId org) {
    if (Engagement* e = org.isNull() ? nullptr : c.w.engagements.get(org)) e->lastCombat = c.tick + 1;
}

// --- engagement ------------------------------------------------------------------------------

void setState(Engagement& e, Engagement::State s) {
    e.state = s;
    e.stateTicks = 0;
}

bool moving(const Order* o) {
    return o != nullptr && (o->state == Order::State::WaitPath || o->state == Order::State::Moving ||
                            o->state == Order::State::Wheeling || o->state == Order::State::Arriving);
}

// The regiment stops where it is to fight. A player's move is kept to resume afterwards; its own
// close-in move just ends; an Attack chase ends (the attack is the order).
void hold(Ctx& c, Engagement& e, EntityId id, EntityId owner) {
    Order* o = c.w.orders.get(owner);
    if (!moving(o)) {
        e.closing = false;
        return;
    }
    if (e.closing || e.mode == Engagement::Mode::Attack) {
        stopOrder(c.sim, owner);
        e.closing = false;
        return;
    }
    if (!e.suspended && owner == id) {
        e.resumeGoal = o->goal;
        e.resumeHasFacing = o->hasFacing;
        e.resumeFacing = o->facing;
    }
    if (holdOrder(c.sim, owner)) e.suspended = true;
}

void engage(Ctx& c, Engagement& e, EntityId id, EntityId owner) {
    setState(e, Engagement::State::Engaging);
    e.noTarget = 0;
    hold(c, e, id, owner);
}

// No unit has had a target for closeInSeconds: the regiment moves toward `toward` (the nearest
// enemy, or the Attack target). Division members hold their slots instead.
void closeIn(Ctx& c, Engagement& e, EntityId id, EntityId owner, FVec2 toward) {
    if (owner != id) return;
    if (e.entrenched && e.mode != Engagement::Mode::Attack) return;  // dug in: it holds the position
    Order* o = c.w.orders.get(id);
    if (!e.closing) {
        if (moving(o) && !e.suspended && e.mode != Engagement::Mode::Attack) {
            e.resumeGoal = o->goal;
            e.resumeHasFacing = o->hasFacing;
            e.resumeFacing = o->facing;
            e.suspended = true;
        }
        e.closing = true;
        chaseOrder(c.sim, id, toward);
    } else if (!moving(o) || distance(o->goal, toward) > c.tuning.chaseRepathDistance) {
        chaseOrder(c.sim, id, toward);
    }
}

// The fight is over for this regiment: its close-in move stops, and a move it held resumes, unless
// another regiment of its division still fights (that one then owns the hold).
void finish(Ctx& c, Engagement& e, EntityId id, EntityId owner) {
    setState(e, Engagement::State::Idle);
    e.noTarget = 0;
    if (e.closing) {
        stopOrder(c.sim, id);
        e.closing = false;
    }
    if (!e.suspended) return;
    e.suspended = false;
    if (owner != id) {
        if (const Group* g = c.w.groups.get(owner)) {
            for (const EntityId m : g->members) {
                Engagement* me = m == id ? nullptr : c.w.engagements.get(m);
                if (me != nullptr && active(me->state)) {
                    me->suspended = true;
                    return;
                }
            }
        }
        resumeOrder(c.sim, owner);
        return;
    }
    restartOrder(c.sim, id, e.resumeGoal, e.resumeHasFacing, e.resumeFacing);
}

void reform(Ctx& c, Engagement& e, EntityId id) {
    setState(e, Engagement::State::Reform);
    e.noTarget = 0;
    if (e.closing) {
        stopOrder(c.sim, id);
        e.closing = false;
    }
}

bool targetPosition(const Ctx& c, EntityId target, const std::vector<EntityId>& liveOrgs, FVec2& out) {
    if (!c.w.registry.alive(target)) return false;
    if (c.w.regiments.has(target)) {
        if (!std::binary_search(liveOrgs.begin(), liveOrgs.end(), target)) return false;
        out = c.w.transforms.get(target)->position;
        return true;
    }
    const Fighter* f = c.field.find(target);
    if (f == nullptr || healthOf(c.w, target) <= Fixed{}) return false;
    out = f->pos;
    return true;
}

void runRegiments(Ctx& c, const Timers& tm) {
    World& w = c.w;
    std::vector<EntityId> liveOrgs;
    for (const Fighter& f : c.field.fs) {
        if (!f.org.isNull()) liveOrgs.push_back(f.org);
    }
    std::sort(liveOrgs.begin(), liveOrgs.end());
    liveOrgs.erase(std::unique(liveOrgs.begin(), liveOrgs.end()), liveOrgs.end());
    // The attack abilities each regiment's units carry: a regiment only engages enemies it can hit.
    std::vector<std::pair<EntityId, std::uint32_t>> arms;
    for (const Fighter& f : c.field.fs) {
        if (f.org.isNull() || f.weapons == nullptr) continue;
        for (const WeaponDef& wd : *f.weapons) {
            const AbilityRules* ar = c.rules.ability(wd.ability);
            if (ar != nullptr && ar->attack) arms.emplace_back(f.org, wd.ability);
        }
    }
    std::sort(arms.begin(), arms.end());
    arms.erase(std::unique(arms.begin(), arms.end()), arms.end());
    // Regiments with at least one unit whose target (from last tick's targeting) is in firing
    // range; fuzzy_range only widens acquisition.
    std::vector<EntityId> aiming;
    for (const Fighter& f : c.field.fs) {
        const Combat* cb = f.org.isNull() ? nullptr : w.combats.get(f.id);
        if (cb == nullptr || f.weapons == nullptr) continue;
        for (std::size_t j = 0; j < cb->targets.size() && j < f.weapons->size(); ++j) {
            const Fighter* t = cb->targets[j].isNull() ? nullptr : c.field.find(cb->targets[j]);
            const std::uint32_t ab = (*f.weapons)[j].ability;
            if (t != nullptr && ab != kNoDef && distance(f.pos, t->pos) <= c.defs.abilities[ab].range + t->radius) {
                aiming.push_back(f.org);
                break;
            }
        }
    }
    std::sort(aiming.begin(), aiming.end());
    std::vector<std::uint32_t> near;
    // The nearest enemy within `r` of `p` that one of `org`'s attack abilities can target.
    const auto nearestEnemy = [&](EntityId org, FVec2 p, Fixed r, std::uint8_t player) -> const Fighter* {
        const auto lo = std::lower_bound(arms.begin(), arms.end(), std::make_pair(org, std::uint32_t{0}));
        if (lo == arms.end() || lo->first != org) return nullptr;
        c.field.within(p, r, near);
        const Fighter* best = nullptr;
        Fixed bestD;
        for (const std::uint32_t i : near) {
            const Fighter& t = c.field.fs[i];
            if (t.player == player) continue;
            bool can = false;
            for (auto it = lo; it != arms.end() && it->first == org && !can; ++it) {
                can = CombatRules::passes(c.rules.ability(it->second)->filter, true, t.types);
            }
            if (!can) continue;
            const Fixed d = (t.pos - p).lengthSq();
            if (best == nullptr || d < bestD) {  // ties: lower id (ascending scan)
                best = &t;
                bestD = d;
            }
        }
        return best;
    };
    const std::uint32_t closeTicks = ticksOf(c.tuning.closeInSeconds);
    for (std::size_t i = 0; i < w.engagements.size(); ++i) {
        const EntityId id = w.engagements.ids()[i];
        Engagement& e = w.engagements.data()[i];
        const Regiment* r = w.regiments.get(id);
        const Transform* a = w.transforms.get(id);
        const DefRef* dr = w.defRefs.get(id);
        if (r == nullptr || a == nullptr || dr == nullptr || !std::binary_search(liveOrgs.begin(), liveOrgs.end(), id)) continue;
        if (e.state == Engagement::State::Routing || e.state == Engagement::State::Retreating) {
            ++e.stateTicks;  // the Morale stage runs these
            continue;
        }
        const RegimentDef& rd = c.defs.regiments[dr->index];
        const Fixed zoc = rd.controlRange > Fixed{} ? rd.controlRange : kDefaultZoc;
        const bool running = r->runRestore != Regiment::kNone;
        const bool aggressive = formationAggressive(c.defs, *r) && !running;
        const EntityId owner = orderOwner(c.sim, id);
        bool sample = false;
        if (e.sampleTicks <= 1) {
            sample = true;
            e.sampleTicks = static_cast<std::uint16_t>(tm.sample);
        } else {
            --e.sampleTicks;
        }
        const bool hit = e.lastCombat == c.tick;  // it hit or was hit last tick
        const bool mayEngage = c.tick >= e.graceUntil && e.state != Engagement::State::Exhausted &&
                               (aggressive || e.mode != Engagement::Mode::None);
        const auto hostiles = [&] { return nearestEnemy(id, a->position, zoc, r->player) != nullptr; };
        const std::uint32_t quiet = e.lastCombat == 0 ? kNever : c.tick + 1 - e.lastCombat;
        const bool hasTarget = std::binary_search(aiming.begin(), aiming.end(), id);
        const bool fighting = e.state == Engagement::State::Engaging || e.state == Engagement::State::Stalemate;
        if (fighting && !hasTarget) {
            if (e.noTarget < 0xFFFF) ++e.noTarget;
        } else {
            e.noTarget = 0;
        }
        if (fighting && hasTarget && e.closing) {  // in range again: stop and fight
            stopOrder(c.sim, id);
            e.closing = false;
        }

        if (e.mode == Engagement::Mode::Attack) {
            FVec2 tpos;
            if (!targetPosition(c, e.target, liveOrgs, tpos)) {
                e.mode = Engagement::Mode::None;
                e.target = EntityId{};
                if (fighting) reform(c, e, id);
            } else {
                if (distance(a->position, tpos) <= zoc * tm.attackFraction) {
                    if (!fighting) engage(c, e, id, owner);
                    if (e.noTarget >= closeTicks) closeIn(c, e, id, owner, tpos);
                } else {
                    if (e.state != Engagement::State::Idle) setState(e, Engagement::State::Idle);
                    e.closing = false;
                    e.noTarget = 0;
                    const Order* o = w.orders.get(id);
                    if (o != nullptr && (o->state == Order::State::Idle || o->state == Order::State::Engaged ||
                                         distance(o->goal, tpos) > c.tuning.chaseRepathDistance)) {
                        chaseOrder(c.sim, id, tpos);
                    }
                }
                ++e.stateTicks;
                continue;
            }
        }
        if (fighting && e.noTarget >= closeTicks) {
            if (const Fighter* near1 = nearestEnemy(id, a->position, zoc, r->player)) closeIn(c, e, id, owner, near1->pos);
        }
        switch (e.state) {
        case Engagement::State::Idle:
            if (mayEngage && (sample || hit) && hostiles()) {
                engage(c, e, id, owner);
            } else if (e.mode == Engagement::Mode::AttackMove && !e.suspended) {
                const Order* o = w.orders.get(owner);
                if (o != nullptr && o->state == Order::State::Idle) e.mode = Engagement::Mode::None;  // arrived
            }
            break;
        case Engagement::State::Engaging:
            if (hit) break;
            if (sample && !hostiles()) {
                reform(c, e, id);
            } else if (e.stateTicks >= tm.stalemate && quiet >= tm.stalemate) {
                setState(e, Engagement::State::Stalemate);
            }
            break;
        case Engagement::State::Stalemate:
            if (hit) {
                e.state = Engagement::State::Engaging;  // keeps its time in the fight
            } else if ((sample && !hostiles()) || e.stateTicks >= tm.stalemate) {
                reform(c, e, id);
            }
            break;
        case Engagement::State::Reform:
            if (mayEngage && (sample || hit) && hostiles()) {
                engage(c, e, id, owner);
            } else if (e.stateTicks >= tm.reform) {
                if (!r->group.isNull()) {
                    setState(e, Engagement::State::Regroup);
                } else {
                    finish(c, e, id, owner);
                }
            }
            break;
        case Engagement::State::Regroup:
            if (mayEngage && (sample || hit) && hostiles()) {
                engage(c, e, id, owner);
            } else if (e.stateTicks >= tm.regroup) {
                finish(c, e, id, owner);
            }
            break;
        default:
            break;  // Retreating / Routing: morale (M5 part 2)
        }
        ++e.stateTicks;
    }
}

// --- targeting -------------------------------------------------------------------------------

bool canHit(const Fighter& self, const Fighter& t, const AbilityDef& ab, const AbilityRules& ar, Fixed reach) {
    if (t.player == self.player || !CombatRules::passes(ar.filter, true, t.types)) return false;
    const Fixed d = distance(self.pos, t.pos);
    return d <= reach + t.radius && d >= ab.minRange;
}

void runTargeting(Ctx& c, const Timers& tm) {
    World& w = c.w;
    std::vector<std::uint32_t> near;
    for (const Fighter& f : c.field.fs) {
        Combat* cb = w.combats.get(f.id);
        if (cb == nullptr || f.weapons == nullptr || f.weapons->empty()) continue;
        const Engagement* e = f.org.isNull() ? nullptr : w.engagements.get(f.org);
        const bool armed = f.org.isNull() || (e != nullptr && !passive(e->state) &&
                                               (e->state == Engagement::State::Engaging || e->state == Engagement::State::Stalemate ||
                                                e->mode == Engagement::Mode::Attack));
        cb->faceTarget = EntityId{};
        if (!armed) {
            std::fill(cb->targets.begin(), cb->targets.end(), EntityId{});
            continue;
        }
        const EntityId focus = e != nullptr && e->mode == Engagement::Mode::Attack ? e->target : EntityId{};
        for (std::size_t j = 0; j < f.weapons->size() && j < cb->targets.size(); ++j) {
            const WeaponDef& wd = (*f.weapons)[j];
            const AbilityRules* ar = c.rules.ability(wd.ability);
            EntityId& cur = cb->targets[j];
            if (ar == nullptr || !ar->attack) {
                cur = EntityId{};
                continue;
            }
            const AbilityDef& ab = c.defs.abilities[wd.ability];
            const Fixed reach = ab.range + wd.fuzzyRange;
            const Fighter* old = c.field.find(cur);
            const bool keep = tm.keepKill && old != nullptr && healthOf(w, cur) > Fixed{} && canHit(f, *old, ab, *ar, reach);
            if (!keep) {
                cur = EntityId{};
                // Best: the Attack target's units first, then (check_damage_resistance) the most
                // damage per hit, then the nearest, then the lowest id.
                std::tuple<int, Fixed, Fixed, EntityId> best{2, Fixed{}, Fixed{}, EntityId{}};
                c.field.within(f.pos, reach + kMaxTargetRadius, near);
                for (const std::uint32_t k : near) {
                    const Fighter& t = c.field.fs[k];
                    if (!canHit(f, t, ab, *ar, reach)) continue;
                    Fixed expected;
                    if (f.checkResist) {
                        const TargetStats s = statsOf(c, t);
                        for (const DamageHit& h : ar->damage) expected = expected + c.rules.damage(h, s, Fixed::fromInt(1));
                    }
                    const int pref = !focus.isNull() && (t.id == focus || t.org == focus) ? 0 : 1;
                    const std::tuple<int, Fixed, Fixed, EntityId> key{pref, -expected, (t.pos - f.pos).lengthSq(), t.id};
                    if (cur.isNull() || key < best) {
                        best = key;
                        cur = t.id;
                    }
                }
            }
            if (wd.turret == WeaponDef::kNoTurret && cb->faceTarget.isNull()) cb->faceTarget = cur;
        }
    }
}

// --- damage ----------------------------------------------------------------------------------

void kill(Ctx& c, const Fighter& v, EntityId killer) {
    World& w = c.w;
    w.health.get(v.id)->current = Fixed{};
    w.registry.destroyLater(v.id);
    c.sim.emit({SimEvent::Type::Died, v.id, v.player, CommandType::Noop, killer, 0, v.pos, v.facing});
    moraleOnKill(c.sim, v.id, killer);
    if (v.org.isNull()) return;
    for (const Fighter& f : c.field.fs) {
        if (f.org == v.org && f.id != v.id && healthOf(w, f.id) > Fixed{}) return;
    }
    w.registry.destroyLater(v.org);  // its last unit: the regiment goes too
}

// Area attenuation: full damage up to attenuation_range_min from the impact, falling linearly to
// attenuation_factor_max at the edge of the area [CALIBRATE: the data's exact meaning].
Fixed attenuation(const AbilityDef& ab, Fixed d) {
    const Fixed one = Fixed::fromInt(1);
    if (d <= ab.attenuationRangeMin || ab.area <= ab.attenuationRangeMin) return one;
    const Fixed t = std::clamp((d - ab.attenuationRangeMin) / (ab.area - ab.attenuationRangeMin), Fixed{}, one);
    return one - (one - ab.attenuationFactorMax) * t;
}

void impact(Ctx& c, std::uint32_t ability, EntityId shooter, std::uint8_t player, EntityId primary, FVec2 point, Fixed mult) {
    const AbilityRules* ar = c.rules.ability(ability);
    if (ar == nullptr) return;
    const AbilityDef& ab = c.defs.abilities[ability];
    std::vector<std::pair<std::tuple<Fixed, EntityId>, const Fighter*>> victims;
    if (ab.area <= Fixed{}) {
        const Fighter* t = c.field.find(primary);
        if (t != nullptr && healthOf(c.w, primary) > Fixed{}) victims.push_back({{Fixed{}, t->id}, t});
    } else {
        std::vector<std::uint32_t> near;
        c.field.within(point, ab.area + kMaxTargetRadius, near);
        for (const std::uint32_t k : near) {
            const Fighter& t = c.field.fs[k];
            if (t.player == player || healthOf(c.w, t.id) <= Fixed{} || !CombatRules::passes(ar->filter, true, t.types)) continue;
            const Fixed d = distance(point, t.pos);
            if (d > ab.area + t.radius) continue;
            victims.push_back({{d, t.id}, &t});
        }
        std::sort(victims.begin(), victims.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (ab.maxTargets != 0 && victims.size() > ab.maxTargets) victims.resize(ab.maxTargets);
    }
    const Fighter* from = c.field.find(shooter);
    c.sim.emit({SimEvent::Type::Hit, primary, player, CommandType::Noop, shooter, ability, point, Angle{}});
    for (const auto& [key, t] : victims) {
        const TargetStats s = statsOf(c, *t);
        Fixed dmg;
        for (const DamageHit& h : ar->damage) dmg = dmg + c.rules.damage(h, s, mult);
        if (ab.area > Fixed{}) dmg = dmg * attenuation(ab, std::get<0>(key));
        Health* hp = c.w.health.get(t->id);
        if (hp == nullptr || hp->current <= Fixed{}) continue;
        hp->current = hp->current - dmg;
        moraleOnHit(c.sim, t->id, shooter, ability);
        markCombat(c, t->org);
        if (from != nullptr) markCombat(c, from->org);
        if (hp->current <= Fixed{}) kill(c, *t, shooter);
    }
}

// --- firing ----------------------------------------------------------------------------------

void moveProjectiles(Ctx& c) {
    World& w = c.w;
    const std::vector<EntityId> shots(w.projectiles.ids().begin(), w.projectiles.ids().end());
    for (const EntityId id : shots) {
        Projectile* p = w.projectiles.get(id);
        Transform* t = w.transforms.get(id);
        if (p == nullptr || t == nullptr || !w.registry.alive(id)) continue;
        if (w.registry.alive(p->target) && healthOf(w, p->target) > Fixed{}) {
            if (const Transform* tt = w.transforms.get(p->target)) p->aim = tt->position;  // homes on its target
        }
        const FVec2 step = (p->aim - t->position) / Fixed::fromInt(std::max<std::int32_t>(p->ticksLeft, 1));
        t->position = t->position + step;
        if (!step.isZero()) t->facing = step.angle();
        if (p->ticksLeft > 1) {
            --p->ticksLeft;
            continue;
        }
        const Projectile shot = *p;
        w.registry.destroyLater(id);
        impact(c, shot.ability, shot.shooter, shot.player, shot.target, shot.aim, shot.attackMultiplier);
    }
}

void fire(Ctx& c, const Fighter& f, const WeaponDef& wd, const Fighter& t) {
    const AbilityDef& ab = c.defs.abilities[wd.ability];
    const Fixed mult = shooterMultiplier(c, f);
    c.sim.emit({SimEvent::Type::Fired, f.id, f.player, CommandType::Noop, t.id, wd.ability, f.pos, f.facing});
    markCombat(c, f.org);
    if (ab.projectileIndex == kNoDef) {
        c.hits.push_back({wd.ability, f.id, f.player, t.id, t.pos, mult});  // lands with the others (simultaneous)
        return;
    }
    const ProjectileDef& pd = c.defs.projectiles[ab.projectileIndex];
    const Fixed speed = pd.speed > Fixed{} ? pd.speed : c.tuning.defaultProjectileSpeed;
    const Fixed d = distance(f.pos, t.pos);
    World& w = c.w;
    const EntityId id = w.registry.create();
    if (id.isNull()) {  // out of entities: the shot lands at once
        c.hits.push_back({wd.ability, f.id, f.player, t.id, t.pos, mult});
        return;
    }
    const FVec2 dir = t.pos - f.pos;
    w.transforms.set(id, {f.pos, dir.isZero() ? f.facing : dir.angle()});
    Projectile p;
    p.shooter = f.id;
    p.target = t.id;
    p.aim = t.pos;
    p.ability = wd.ability;
    p.player = f.player;
    p.ticksLeft = static_cast<std::uint16_t>(ticksOf(d / speed));
    p.attackMultiplier = mult;
    w.projectiles.set(id, p);
}

void runWeapons(Ctx& c) {
    World& w = c.w;
    const std::int32_t turretTol = angleUnits(c.tuning.turretAimTolerance);
    const std::int32_t localArc = angleUnits(c.tuning.localFireArc);
    for (const Fighter& f : c.field.fs) {
        if (f.weapons == nullptr || f.weapons->empty() || healthOf(w, f.id) <= Fixed{}) continue;
        Combat* cb = w.combats.get(f.id);
        if (cb == nullptr) continue;
        // Turrets turn toward their highest-priority weapon's target, or back to rest.
        for (std::size_t ti = 0; ti < f.turrets->size() && ti < cb->turretYaw.size(); ++ti) {
            const TurretDef& td = (*f.turrets)[ti];
            const Fighter* aim = nullptr;
            Fixed bestPriority;
            for (std::size_t j = 0; j < f.weapons->size() && j < cb->targets.size(); ++j) {
                if ((*f.weapons)[j].turret != ti || cb->targets[j].isNull()) continue;
                const Fighter* t = c.field.find(cb->targets[j]);
                if (t == nullptr || healthOf(w, t->id) <= Fixed{}) continue;
                if (aim == nullptr || (*f.weapons)[j].priority > bestPriority) {
                    aim = t;
                    bestPriority = (*f.weapons)[j].priority;
                }
            }
            const Angle rest = Angle::fromDegrees(td.yawStart);
            Angle want = rest;
            if (aim != nullptr) {
                want = (aim->pos - f.pos).isZero() ? cb->turretYaw[ti] : (aim->pos - f.pos).angle() - f.facing;
                if (td.yawRange < Fixed::fromInt(360)) {
                    const std::int32_t half = angleUnits(td.yawRange) / 2;
                    const std::int32_t d = std::clamp(Angle::delta(rest, want), -half, half);
                    want = d >= 0 ? rest + Angle::fromUnits(static_cast<std::uint32_t>(d)) : rest - Angle::fromUnits(static_cast<std::uint32_t>(-d));
                }
                cb->turretIdle[ti] = 0;
            } else {
                if (cb->turretIdle[ti] < 0xFFFF) ++cb->turretIdle[ti];
                if (!td.canReset || cb->turretIdle[ti] < ticksOf(td.resetTime)) want = cb->turretYaw[ti];
            }
            cb->turretYaw[ti] = turnToward(cb->turretYaw[ti], want, turnUnits(td.yawRate));
        }
        for (std::size_t j = 0; j < f.weapons->size() && j < cb->targets.size(); ++j) {
            if (cb->cooldown[j] > 0) --cb->cooldown[j];
            const EntityId tid = cb->targets[j];
            if (tid.isNull() || cb->cooldown[j] > 0) continue;
            const Fighter* t = c.field.find(tid);
            if (t == nullptr || healthOf(w, tid) <= Fixed{}) continue;
            const WeaponDef& wd = (*f.weapons)[j];
            const AbilityDef& ab = c.defs.abilities[wd.ability];
            const Fixed d = distance(f.pos, t->pos);
            if (d > ab.range + t->radius || d < ab.minRange) continue;
            const Angle bearing = (t->pos - f.pos).isZero() ? f.facing : (t->pos - f.pos).angle();
            const bool aimed = wd.turret != WeaponDef::kNoTurret && wd.turret < cb->turretYaw.size()
                                   ? std::abs(Angle::delta(f.facing + cb->turretYaw[wd.turret], bearing)) <= turretTol
                                   : std::abs(Angle::delta(f.facing, bearing)) <= localArc;
            if (!aimed) continue;
            cb->cooldown[j] = static_cast<std::uint16_t>(ticksOf(ab.rechargeTime + wd.reloadTime));
            fire(c, f, wd, *t);
        }
    }
}

}  // namespace

void attachCombat(Sim& sim, EntityId e, std::uint8_t player) {
    World& w = sim.world();
    const DefSet* defs = sim.config().defs;
    const DefRef* d = w.defRefs.get(e);
    Combat cb;
    cb.player = player;
    const std::vector<WeaponDef>* weapons = nullptr;
    const std::vector<TurretDef>* turrets = nullptr;
    if (defs != nullptr && d != nullptr) {
        if (d->kind == DefRef::Kind::Unit && d->index < defs->units.size()) {
            weapons = &defs->units[d->index].weapons;
            turrets = &defs->units[d->index].turrets;
        } else if (d->kind == DefRef::Kind::Building && d->index < defs->buildings.size()) {
            weapons = &defs->buildings[d->index].weapons;
            turrets = &defs->buildings[d->index].turrets;
        }
    }
    if (weapons != nullptr) {
        cb.targets.assign(weapons->size(), EntityId{});
        cb.cooldown.assign(weapons->size(), 0);
    }
    if (turrets != nullptr) {
        for (const TurretDef& t : *turrets) cb.turretYaw.push_back(Angle::fromDegrees(t.yawStart));
        cb.turretIdle.assign(turrets->size(), 0);
    }
    w.combats.set(e, std::move(cb));
}

void attachEngagement(Sim& sim, EntityId regiment) {
    Engagement e;
    const std::uint32_t sample = sim.config().defs != nullptr ? timersOf(*sim.config().defs).sample : 50;
    e.sampleTicks = static_cast<std::uint16_t>(1 + regiment.index() % sample);
    sim.world().engagements.set(regiment, e);
}

void clearEngagement(Sim& sim, EntityId regiment) {
    Engagement* e = sim.world().engagements.get(regiment);
    if (e == nullptr) return;
    if (e->state != Engagement::State::Exhausted) e->state = Engagement::State::Idle;  // exhaustion outlasts orders
    e->mode = Engagement::Mode::None;
    e->target = EntityId{};
    e->suspended = false;
    e->closing = false;
    e->noTarget = 0;
    e->stateTicks = 0;
    e->graceUntil = sim.tick() + ticksOf(sim.tuning().moveGraceSeconds);
}

void runEngagement(Sim& sim) {
    if (sim.config().defs == nullptr) return;
    const Field field(sim);
    Ctx c{sim, sim.world(), *sim.config().defs, sim.combatRules(), sim.tuning(), field, sim.tick(), {}};
    const Timers tm = timersOf(c.defs);
    runRegiments(c, tm);
    runTargeting(c, tm);
}

void runAbilities(Sim& sim) {
    if (sim.config().defs == nullptr) return;
    const Field field(sim);
    Ctx c{sim, sim.world(), *sim.config().defs, sim.combatRules(), sim.tuning(), field, sim.tick(), {}};
    moveProjectiles(c);
    runWeapons(c);
    // Instant hits land after everyone fired, in firing order: a unit killed this tick still fired.
    for (const Hit& h : c.hits) impact(c, h.ability, h.shooter, h.player, h.target, h.point, h.mult);
}

}  // namespace corps
