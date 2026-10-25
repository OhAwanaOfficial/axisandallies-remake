#include "systems/morale.hpp"

#include <algorithm>

#include "combat/rules.hpp"
#include "nav/nav_world.hpp"
#include "systems/combat.hpp"
#include "systems/movement.hpp"
#include "systems/orders.hpp"
#include "systems/stages.hpp"

namespace corps {

using namespace fixed_literals;

namespace {

constexpr Fixed kTick = 0.1_fx;

std::uint32_t ticksOf(Fixed seconds) {
    const std::int64_t t = (seconds * Fixed::fromInt(static_cast<std::int32_t>(kTicksPerSecond))).ceilInt();
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(t, 1, 0x7FFFFFFF));
}

// Everything the morale rules read from SVars (SVars.tgi merged with SVars_AA.tgi).
struct Vars {
    std::uint32_t sample = 50, routing = 100, retreating = 100;
    Fixed engagementLoss, engagementFactorMax, deathFactor, retreatPenalty;
    Fixed slayerBase, slayerMin, slayerMax, breakerBase, breakerMin, breakerMax, replenish;
    Fixed xpMin, xpMax, xpKill, xpRout, xpEngage, xpDefault, xpFaction, xpPeriodic;
    Fixed climb, fall;
};

Vars varsOf(const DefSet& d) {
    const SVarsDef& v = d.svars;
    Vars x;
    x.sample = ticksOf(v.getFixed("PrimarySampleTime", 5_fx));
    x.routing = ticksOf(v.getFixed("RoutingTime", 10_fx));
    x.retreating = ticksOf(v.getFixed("RetreatingTime", 10_fx));
    x.engagementLoss = v.getFixed("MoraleEngagementLoss", 0.5_fx);
    x.engagementFactorMax = v.getFixed("MoraleEngagementLossFactorMax", 3_fx);
    x.deathFactor = v.getFixed("MoraleDeathFactor", 0.25_fx);
    x.retreatPenalty = v.getFixed("MoraleRetreatPenalty", 0_fx);
    x.slayerBase = v.getFixed("MoraleSlayerBase", 0.5_fx);
    x.slayerMin = v.getFixed("MoraleSlayerFactorMin", 0.5_fx);
    x.slayerMax = v.getFixed("MoraleSlayerFactorMax", 3_fx);
    x.breakerBase = v.getFixed("MoraleBreakerBase", 3_fx);
    x.breakerMin = v.getFixed("MoraleBreakerFactorMin", 0.5_fx);
    x.breakerMax = v.getFixed("MoraleBreakerFactorMax", 2_fx);
    x.replenish = v.getFixed("MoraleReplenish", 2_fx);
    x.xpMin = v.getFixed("ExperienceGainMinimum", 0_fx);
    x.xpMax = v.getFixed("ExperienceGainMaximum", 10_fx);
    x.xpKill = v.getFixed("ExperienceKillMultiplier", 1_fx);
    x.xpRout = v.getFixed("ExperienceRoutMultiplier", 1_fx);
    x.xpEngage = v.getFixed("ExperienceEngageMultiplier", 1_fx);
    x.xpDefault = v.getFixed("ExperienceDefaultValue", 1_fx);
    x.xpFaction = v.getFixed("ExperienceFactionGainMultiplier", 1_fx);
    x.xpPeriodic = v.getFixed("ExperienceFactionGainPeriodic", 1_fx);
    x.climb = v.getFixed("AttackEfficiencyPointsPerSecondClimbing", 0.01_fx);
    x.fall = v.getFixed("AttackEfficiencyPointsPerSecondFalling", 0.02_fx);
    return x;
}

const DefSet& defsOf(const Sim& sim) {
    return *sim.config().defs;
}

const RegimentDef* regimentDefOf(const Sim& sim, EntityId reg) {
    const DefRef* d = sim.world().defRefs.get(reg);
    if (sim.config().defs == nullptr || d == nullptr || d->kind != DefRef::Kind::Regiment) return nullptr;
    return d->index < defsOf(sim).regiments.size() ? &defsOf(sim).regiments[d->index] : nullptr;
}

const ExperienceLevelDef* levelOf(const Sim& sim, EntityId reg) {
    const RegimentDef* rd = regimentDefOf(sim, reg);
    const Engagement* e = sim.world().engagements.get(reg);
    if (rd == nullptr || e == nullptr || e->level >= rd->experienceLevels.size()) return nullptr;
    return &rd->experienceLevels[e->level];
}

// Unit or building def facts the morale rules need.
struct Facts {
    Fixed moraleValue;
    Fixed healthMax;
    const std::vector<std::string>* properties = nullptr;
};

Facts factsOf(const Sim& sim, EntityId e) {
    Facts f;
    const DefRef* d = sim.world().defRefs.get(e);
    if (sim.config().defs == nullptr || d == nullptr) return f;
    const DefSet& defs = defsOf(sim);
    if (d->kind == DefRef::Kind::Unit && d->index < defs.units.size()) {
        f.moraleValue = defs.units[d->index].moraleValue;
        f.healthMax = defs.units[d->index].health;
        f.properties = &defs.units[d->index].properties;
    } else if (d->kind == DefRef::Kind::Building && d->index < defs.buildings.size()) {
        f.moraleValue = defs.buildings[d->index].moraleValue;
        f.healthMax = defs.buildings[d->index].health;
        f.properties = &defs.buildings[d->index].properties;
    }
    return f;
}

std::uint8_t playerOf(const Sim& sim, EntityId reg) {
    const Regiment* r = sim.world().regiments.get(reg);
    return r != nullptr ? r->player : 0;
}

EntityId orgOf(const Sim& sim, EntityId e) {
    const Member* m = sim.world().members.get(e);
    return m != nullptr ? m->organization : EntityId{};
}

// MORALE_LOSS_RATE of an entrenched regiment: × (1 + amount) for each entrench property that
// applies to its first living unit (data: × 1.5) [CALIBRATE: the data's 0.5 may mean "half"].
Fixed lossRateOf(const Sim& sim, EntityId reg) {
    const Engagement* e = sim.world().engagements.get(reg);
    Fixed rate = Fixed::fromInt(1);
    if (e == nullptr || !e->entrenched || sim.config().defs == nullptr) return rate;
    const World& w = sim.world();
    const std::vector<std::string>* props = nullptr;
    for (std::size_t i = 0; i < w.members.size() && props == nullptr; ++i) {
        if (w.members.data()[i].organization != reg) continue;
        const Health* h = w.health.get(w.members.ids()[i]);
        if (h != nullptr && h->current > Fixed{}) props = factsOf(sim, w.members.ids()[i]).properties;
    }
    if (props == nullptr) return rate;
    const DefSet& d = defsOf(sim);
    for (const char* ids : {"infantry_entrench", "vehicle_entrench"}) {
        const std::uint32_t p = d.propertyIndex(ids);
        if (p == kNoDef) continue;
        const PropertyDef& prop = d.properties[p];
        bool applies = true;
        for (const std::string& req : prop.requiredProperties) {
            applies = applies && std::find(props->begin(), props->end(), req) != props->end();
        }
        if (!applies) continue;
        for (const ModifierDef& m : prop.modifiers) {
            if (m.attribute == "MORALE_LOSS_RATE" && m.op == ModifierDef::Op::Multiply) rate = rate * (Fixed::fromInt(1) + m.amount);
        }
    }
    return rate;
}

void loseMorale(Sim& sim, EntityId reg, Fixed amount) {
    Engagement* e = sim.world().engagements.get(reg);
    if (e == nullptr || amount <= Fixed{}) return;
    e->morale = max(Fixed{}, e->morale - amount * lossRateOf(sim, reg));
}

void gainMorale(Sim& sim, EntityId reg, Fixed amount) {
    Engagement* e = sim.world().engagements.get(reg);
    if (e == nullptr || amount <= Fixed{}) return;
    e->morale = min(moraleMaxOf(sim, reg), e->morale + amount);
}

// A unit's combat value for experience: health_max × xpPerHealth.
Fixed combatValueOfUnit(const Sim& sim, EntityId unit) {
    return factsOf(sim, unit).healthMax * sim.tuning().xpPerHealth;
}

Fixed combatValueOfRegiment(const Sim& sim, EntityId reg) {
    const RegimentDef* rd = regimentDefOf(sim, reg);
    if (rd != nullptr && rd->fakeCombatValue >= Fixed{}) return rd->fakeCombatValue;
    Fixed sum;
    const World& w = sim.world();
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        if (w.members.data()[i].organization == reg) sum = sum + combatValueOfUnit(sim, w.members.ids()[i]);
    }
    return sum;
}

void addArmyXp(Sim& sim, std::uint8_t player, Fixed xp) {
    if (player < kMaxPlayers) sim.world().kingdoms[player].armyXp = sim.world().kingdoms[player].armyXp + xp;
}

// Level ups: health scales with the new level's HEALTH multiplier.
void applyLevel(Sim& sim, EntityId reg) {
    Engagement* e = sim.world().engagements.get(reg);
    const RegimentDef* rd = regimentDefOf(sim, reg);
    if (e == nullptr || rd == nullptr || rd->experienceLevels.empty()) return;
    std::uint8_t level = 0;
    for (std::size_t k = 0; k < rd->experienceLevels.size() && k < 255; ++k) {
        if (e->xp >= rd->experienceLevels[k].required) level = static_cast<std::uint8_t>(k);
    }
    if (level == e->level) return;
    const Fixed from = rd->experienceLevels[e->level].healthMultiplier;
    const Fixed to = rd->experienceLevels[level].healthMultiplier;
    e->level = level;
    World& w = sim.world();
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        if (w.members.data()[i].organization != reg) continue;
        Health* h = w.health.get(w.members.ids()[i]);
        if (h == nullptr || h->current <= Fixed{} || from <= Fixed{}) continue;
        h->max = h->max * to / from;
        h->current = h->current * to / from;
    }
    sim.emit({SimEvent::Type::LevelUp, reg, playerOf(sim, reg), CommandType::Noop, EntityId{}});
}

void addExperience(Sim& sim, EntityId reg, Fixed xp, const Vars& v) {
    Engagement* e = reg.isNull() ? nullptr : sim.world().engagements.get(reg);
    if (e == nullptr || xp <= Fixed{}) return;
    e->xp = e->xp + xp;
    addArmyXp(sim, playerOf(sim, reg), xp * v.xpFaction);
    applyLevel(sim, reg);
}

Fixed clampedGain(Fixed value, const Vars& v) {
    return std::clamp(value, v.xpMin, v.xpMax);
}

Fixed formationEfficiency(const Sim& sim, const Regiment& r) {
    if (sim.config().defs == nullptr || r.layout == kNoDef) return Fixed::fromInt(1);
    const LayoutDef& l = defsOf(sim).layouts[r.layout];
    if (r.formation >= l.formations.size()) return Fixed::fromInt(1);
    const std::uint32_t fi = l.formations[r.formation].formationIndex;
    return fi != kNoDef ? defsOf(sim).formations[fi].attackMultiplier : Fixed::fromInt(1);
}

void useRetreatFormation(Sim& sim, Regiment& r) {
    if (sim.config().defs == nullptr || r.layout == kNoDef) return;
    const std::uint32_t f = defsOf(sim).layouts[r.layout].retreatFormation;
    if (f == kNoDef) return;
    if (r.runRestore == Regiment::kNone) r.runRestore = r.formation;  // restored on arrival, like Run
    r.formation = static_cast<std::uint16_t>(f);
}

void restoreFormation(Regiment& r) {
    if (r.runRestore != Regiment::kNone) {
        r.formation = r.runRestore;
        r.runRestore = Regiment::kNone;
    }
}

// The regiment breaks: away from its last attacker (or the nearest enemy), ± fleeSpread at random.
void rout(Sim& sim, EntityId id, Engagement& e, const Vars& v) {
    World& w = sim.world();
    Regiment& r = *w.regiments.get(id);
    const Transform& a = *w.transforms.get(id);
    e.state = Engagement::State::Routing;
    e.stateTicks = 0;
    e.mode = Engagement::Mode::None;
    e.target = EntityId{};
    e.suspended = false;
    e.closing = false;
    e.entrenched = false;
    e.stillTicks = 0;
    detachFromGroup(sim, id);
    restoreFormation(r);
    useRetreatFormation(sim, r);
    FVec2 threat = a.position;
    if (!e.lastAttacker.isNull() && w.registry.alive(e.lastAttacker) && w.transforms.has(e.lastAttacker)) {
        threat = w.transforms.get(e.lastAttacker)->position;
    }
    Angle away = (a.position - threat).isZero() ? a.facing + Angle::fromUnits(Angle::kHalf) : (a.position - threat).angle();
    const std::int32_t spread = static_cast<std::int32_t>(Angle::fromDegrees(sim.tuning().fleeSpread).units);
    if (spread > 0) {
        const std::int32_t off = sim.rng().range(-spread, spread);
        away = away + Angle::fromUnits(static_cast<std::uint32_t>(off + 65536));
    }
    restartOrder(sim, id, a.position + FVec2::fromAngle(away) * sim.tuning().fleeDistance, false, Angle{});
    sim.emit({SimEvent::Type::Routed, id, r.player, CommandType::Noop, e.lastAttacker});
    // The regiment that broke it earns the rout.
    if (!e.lastAttacker.isNull() && w.registry.alive(e.lastAttacker)) {
        addExperience(sim, e.lastAttacker, clampedGain(combatValueOfRegiment(sim, id), v) * v.xpRout, v);
    }
}

}  // namespace

void initRegimentMorale(Sim& sim, EntityId regiment) {
    Engagement* e = sim.world().engagements.get(regiment);
    const Regiment* r = sim.world().regiments.get(regiment);
    if (e == nullptr || r == nullptr) return;
    e->level = 0;
    e->xp = Fixed{};
    e->morale = moraleMaxOf(sim, regiment);
    e->efficiency = formationEfficiency(sim, *r);
    if (r->player < kMaxPlayers) sim.world().kingdoms[r->player].active = true;
}

Fixed moraleMaxOf(const Sim& sim, EntityId regiment) {
    const World& w = sim.world();
    const ExperienceLevelDef* level = levelOf(sim, regiment);
    Fixed sum;
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        if (w.members.data()[i].organization != regiment) continue;
        const Health* h = w.health.get(w.members.ids()[i]);
        if (h == nullptr || h->current <= Fixed{}) continue;
        sum = sum + factsOf(sim, w.members.ids()[i]).moraleValue;
        if (level != nullptr) sum = sum + level->morale;
    }
    return sum;
}

Fixed attackMultiplierOf(const Sim& sim, EntityId regiment, Fixed extraAv) {
    const Engagement* e = regiment.isNull() ? nullptr : sim.world().engagements.get(regiment);
    if (e == nullptr) return Fixed::fromInt(1);
    const ExperienceLevelDef* level = levelOf(sim, regiment);
    const Fixed av = (level != nullptr ? level->attack : Fixed{}) + extraAv;
    return e->efficiency * (Fixed::fromInt(1) + av * sim.tuning().attackValueScale);
}

Fixed levelDefenseOf(const Sim& sim, EntityId regiment) {
    const ExperienceLevelDef* level = regiment.isNull() ? nullptr : levelOf(sim, regiment);
    return level != nullptr ? level->defense : Fixed{};
}

void setExperience(Sim& sim, EntityId regiment, Fixed xp) {
    Engagement* e = sim.world().engagements.get(regiment);
    if (e == nullptr) return;
    e->xp = max(xp, Fixed{});
    applyLevel(sim, regiment);
}

void grantExperience(Sim& sim, EntityId regiment, Fixed xp) {
    if (sim.config().defs == nullptr) return;
    addExperience(sim, regiment, xp, varsOf(defsOf(sim)));
}

void startRetreat(Sim& sim, EntityId regiment, FVec2 goal) {
    World& w = sim.world();
    Engagement* e = w.engagements.get(regiment);
    Regiment* r = w.regiments.get(regiment);
    if (e == nullptr || r == nullptr || sim.config().defs == nullptr) return;
    const Vars v = varsOf(defsOf(sim));
    e->state = Engagement::State::Retreating;
    e->stateTicks = 0;
    e->entrenched = false;
    e->stillTicks = 0;
    loseMorale(sim, regiment, v.retreatPenalty);
    detachFromGroup(sim, regiment);
    restoreFormation(*r);
    useRetreatFormation(sim, *r);
    restartOrder(sim, regiment, goal, false, Angle{});
}

void moraleOnKill(Sim& sim, EntityId victim, EntityId killer) {
    if (sim.config().defs == nullptr) return;
    const Vars v = varsOf(defsOf(sim));
    const Facts vf = factsOf(sim, victim);
    const EntityId vorg = orgOf(sim, victim);
    loseMorale(sim, vorg, vf.moraleValue * v.deathFactor);
    const EntityId korg = orgOf(sim, killer);
    if (korg.isNull() || !sim.world().registry.alive(korg)) return;
    const Facts kf = factsOf(sim, killer);
    if (kf.moraleValue > Fixed{}) {
        gainMorale(sim, korg, v.slayerBase * std::clamp(vf.moraleValue / kf.moraleValue, v.slayerMin, v.slayerMax));
    }
    addExperience(sim, korg, clampedGain(combatValueOfUnit(sim, victim), v) * v.xpKill, v);
}

void moraleOnHit(Sim& sim, EntityId victim, EntityId shooter, std::uint32_t ability) {
    const EntityId vorg = orgOf(sim, victim);
    Engagement* e = vorg.isNull() ? nullptr : sim.world().engagements.get(vorg);
    if (e == nullptr || sim.config().defs == nullptr) return;
    const EntityId sorg = orgOf(sim, shooter);
    if (!sorg.isNull()) e->lastAttacker = sorg;
    const AbilityRules* ar = sim.combatRules().ability(ability);
    if (ar == nullptr || ar->moraleLoss <= Fixed{}) return;
    const Vars v = varsOf(defsOf(sim));
    loseMorale(sim, vorg, v.breakerBase * std::clamp(ar->moraleLoss, v.breakerMin, v.breakerMax));
}

void runMorale(Sim& sim) {
    if (sim.config().defs == nullptr) return;
    World& w = sim.world();
    const DefSet& defs = defsOf(sim);
    const Vars v = varsOf(defs);
    const std::uint32_t tick = sim.tick();

    // Army XP: the periodic gain for every player with something on the map.
    for (std::size_t p = 0; p < kMaxPlayers; ++p) {
        if (w.kingdoms[p].active) w.kingdoms[p].armyXp = w.kingdoms[p].armyXp + v.xpPeriodic * kTick;
    }

    for (std::size_t i = 0; i < w.engagements.size(); ++i) {
        const EntityId id = w.engagements.ids()[i];
        Engagement& e = w.engagements.data()[i];
        Regiment* r = w.regiments.get(id);
        const Transform* a = w.transforms.get(id);
        const RegimentDef* rd = regimentDefOf(sim, id);
        if (r == nullptr || a == nullptr || rd == nullptr || !w.registry.alive(id)) continue;

        // Combat efficiency blends toward the formation's (0 while routing).
        const Fixed desired = e.state == Engagement::State::Routing ? Fixed{} : formationEfficiency(sim, *r);
        if (e.efficiency < desired) e.efficiency = min(desired, e.efficiency + v.climb * kTick);
        if (e.efficiency > desired) e.efficiency = max(desired, e.efficiency - v.fall * kTick);

        // Entrenchment: halted long enough.
        const Order* o = w.orders.get(orderOwner(sim, id));
        const bool halted = o != nullptr && (o->state == Order::State::Idle || o->state == Order::State::Engaged) &&
                            e.state != Engagement::State::Routing && e.state != Engagement::State::Retreating;
        if (halted) {
            if (e.stillTicks < 0x7FFFFFFF) ++e.stillTicks;
            if (rd->entrenchmentTime > Fixed{} && e.stillTicks >= ticksOf(rd->entrenchmentTime)) e.entrenched = true;
        } else {
            e.stillTicks = 0;
            e.entrenched = false;
        }

        const Fixed maxMorale = moraleMaxOf(sim, id);
        if (maxMorale <= Fixed{}) continue;  // no morale to lose (no living units, or none with morale_value)
        e.morale = min(e.morale, maxMorale);

        // The morale sample (staggered by entity).
        if ((tick + id.index()) % v.sample == 0) {
            switch (e.state) {
            case Engagement::State::Engaging:
            case Engagement::State::Stalemate: {
                const RegimentDef& def = *rd;
                const Fixed zoc = def.controlRange > Fixed{} ? def.controlRange : 18_fx;
                Fixed enemy;
                for (std::size_t k = 0; k < w.combats.size(); ++k) {
                    const EntityId u = w.combats.ids()[k];
                    if (!w.hostile(w.combats.data()[k].player, r->player)) continue;
                    const Transform* ut = w.transforms.get(u);
                    const Health* uh = w.health.get(u);
                    if (ut == nullptr || uh == nullptr || uh->current <= Fixed{} || distance(ut->position, a->position) > zoc) continue;
                    enemy = enemy + uh->current;
                }
                const Fixed own = regimentStrength(sim, id);
                const Fixed factor = own > Fixed{} ? std::clamp(enemy / own, Fixed::fromInt(1), v.engagementFactorMax) : v.engagementFactorMax;
                loseMorale(sim, id, v.engagementLoss * factor);
                addExperience(sim, id, v.xpDefault * v.xpEngage, v);
                break;
            }
            case Engagement::State::Idle:
            case Engagement::State::Reform:
            case Engagement::State::Regroup:
            case Engagement::State::Exhausted: {
                const ExperienceLevelDef* level = levelOf(sim, id);
                const Supply* sp = w.supplies.get(id);
                const Fixed bonus = sp != nullptr && sp->inSupply ? defs.economy.moraleSupplyBonus : Fixed{};  // MoraleSupplyBonus
                gainMorale(sim, id, v.replenish * (level != nullptr ? level->moraleRecovery : Fixed::fromInt(1)) + bonus);
                break;
            }
            default:
                break;  // Retreating, Routing: no replenishment
            }
        }

        // Breaking point, rout, exhaustion, rally.
        switch (e.state) {
        case Engagement::State::Engaging:
        case Engagement::State::Stalemate:
        case Engagement::State::Retreating:
            if (e.morale <= rd->moraleBreakpoint) rout(sim, id, e, v);
            else if (e.state == Engagement::State::Retreating && e.stateTicks >= v.retreating && w.orders.get(id)->state == Order::State::Idle) {
                e.state = Engagement::State::Reform;
                e.stateTicks = 0;
            }
            break;
        case Engagement::State::Routing:
            if (e.stateTicks >= v.routing) {
                e.state = Engagement::State::Exhausted;
                e.stateTicks = 0;
                stopOrder(sim, id);
                restoreFormation(*r);
            }
            break;
        case Engagement::State::Exhausted: {
            const Fixed rally = rd->moraleBreakpoint + (maxMorale - rd->moraleBreakpoint) * sim.tuning().rallyFraction;
            if (e.morale >= min(rally, maxMorale)) {
                e.state = Engagement::State::Reform;
                e.stateTicks = 0;
            }
            break;
        }
        default:
            break;
        }
    }
}

}  // namespace corps
