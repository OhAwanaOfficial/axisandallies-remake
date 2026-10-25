#pragma once

// Morale, experience, entrenchment and combat efficiency: M5 part 2 (GAME_DESIGN §6.4, §6.6–6.9).
// The Morale stage runs per regiment (ascending id); combat (systems/combat.cpp) reports kills
// and hits through the hooks below as they happen.
//
// Morale (SVars, "for morale"):
// * A regiment's morale is at most the sum of its living units' morale_value (ElementComponent;
//   infantry 5), plus its experience level's MORALE per unit. It starts full.
// * Every PrimarySampleTime (staggered by entity):
//   * engaged (Engaging, Stalemate): − MoraleEngagementLoss × clamp(enemy strength in its zone /
//     its own strength, 1, MoraleEngagementLossFactorMax), strength = health of living units;
//   * otherwise (Idle, Reform, Regroup, Exhausted): + MoraleReplenish × the level's
//     MORALE_RECOVERY_RATE (supply's MoraleSupplyBonus arrives with supply, M6);
//   * Retreating and Routing don't replenish (actor_states: morale_replenish = false).
// * A unit dies: its regiment loses morale_value × MoraleDeathFactor; the killer's regiment gains
//   MoraleSlayerBase × clamp(victim morale_value / killer morale_value, MoraleSlayerFactorMin, Max).
// * A "breaker" hit (an ability with a MoraleLossEffect: mortars, artillery, bombers): −
//   MoraleBreakerBase × clamp(effect amount, MoraleBreakerFactorMin, Max).
// * Losses are multiplied by the regiment's MORALE_LOSS_RATE modifiers (entrenchment: × 1.5).
// * An ordered Retreat costs MoraleRetreatPenalty once.
// * At or below RegimentDef morale_breakpoint while fighting, the regiment routs: it leaves its
//   group, drops its orders, and runs fleeDistance away from its last attacker (± fleeSpread at
//   random, sim RNG) in its retreat formation, deaf to commands, for RoutingTime. Then it is
//   Exhausted (passive, recovering) until morale is rallyFraction of the way from the breaking
//   point to full, and reforms.
//
// Experience (SVars, "experience"; ExperienceComponent levels on the regiment):
// * Kill: clamp(victim combat value, ExperienceGainMinimum, Maximum) × ExperienceKillMultiplier,
//   combat value = health_max × xpPerHealth (or the victim regiment's fake_cv_value).
// * Rout: the routed regiment's combat value (clamped) × ExperienceRoutMultiplier, to the regiment
//   that last hit it. Engaged: ExperienceDefaultValue × ExperienceEngageMultiplier per sample.
// * Levels at experience_required: AV (damage × (1 + AV × attackValueScale)), DV (defense),
//   HEALTH (units' health scaled), MORALE (per unit), MORALE_RECOVERY_RATE.
// * Army XP: every XP a player's regiment earns × ExperienceFactionGainMultiplier, plus
//   ExperienceFactionGainPeriodic per second for every player with something on the map.
//
// Combat efficiency: the actual attack efficiency moves toward the formation's
// (ATTACK_EFFICIENCY_DESIRED) at AttackEfficiencyPointsPerSecondClimbing (0.01/s) going up and
// ...Falling (0.02/s) going down; routing regiments aim at 0.
//
// Entrenchment: a regiment whose move order is idle or on hold, and that isn't routing or
// retreating, counts halted ticks; after RegimentDef entrenchment_time (30 s) it is entrenched
// (infantry_entrench / vehicle_entrench apply). Any move resets it.

#include "core/sim.hpp"

namespace corps {

// Morale, efficiency and level for a newly spawned regiment (after its units).
void initRegimentMorale(Sim& sim, EntityId regiment);

// --- combat hooks ----------------------------------------------------------------------------
// `victim` (unit or building) was killed by `killer` (a unit or building, may be dead).
void moraleOnKill(Sim& sim, EntityId victim, EntityId killer);
// `victim` was hit by an attack of ability `ability` from `shooter`.
void moraleOnHit(Sim& sim, EntityId victim, EntityId shooter, std::uint32_t ability);

// Damage multiplier of a regiment's attacks: efficiency × (1 + (AV + extraAv) × attackValueScale),
// AV from its experience level, extraAv from research (M6). 1 for entities not in a regiment.
Fixed attackMultiplierOf(const Sim& sim, EntityId regiment, Fixed extraAv = {});
// DV a regiment's experience level adds to its units' defense.
Fixed levelDefenseOf(const Sim& sim, EntityId regiment);
// Morale ceiling: its living units' morale_value (+ level MORALE each).
Fixed moraleMaxOf(const Sim& sim, EntityId regiment);

// Sets a regiment's XP (DebugSetExperience) and applies the level it reaches.
void setExperience(Sim& sim, EntityId regiment, Fixed xp);
// Adds XP to a regiment (and its share to the army XP), as a kill does (city capture, M6).
void grantExperience(Sim& sim, EntityId regiment, Fixed xp);
// Ordered Retreat (command 9): retreat formation, passive, MoraleRetreatPenalty.
void startRetreat(Sim& sim, EntityId regiment, FVec2 goal);

}  // namespace corps
