#pragma once

// Combat, M5 part 1 (GAME_DESIGN §6.3–6.6).
//
// Engagement stage (per regiment, ascending): every PrimarySampleTime (staggered by entity) the
// regiment looks for enemies inside its zone of control (RegimentDef control_range around its
// anchor) that one of its units' attack abilities can target. Aggressive formations (Assault,
// Normal) not running, and AttackMove orders, engage: the move goes on hold
// (Order::State::Engaged) and the regiment fights until the zone is clear,
// then Reform (ReformTime), Regroup when in a group (RegroupTime), and the move resumes. Without
// hits either way for StalemateTime it goes to Stalemate, and leaves the fight after another
// StalemateTime. Attack chases its target to CAITargetInRangeZOCMultiplier × ZOC, then engages.
// Being hit also triggers a sample. When no unit of a fighting regiment has a target in firing
// range for closeInSeconds, it moves toward the nearest enemy (or its Attack target) and stops
// again once one has; a move it held resumes afterwards (Engagement::resume*). Division members
// hold their slots instead. Retreating and Routing exist for morale (part 2).
//
// Then per fighter (ascending), each weapon keeps or picks a target: enemies in range + fuzzy
// range that pass the ability's target filter. KillTargetIsHighestPriority keeps a valid target.
// Otherwise: the Attack target's units first, then (check_damage_resistance) the most damage per
// hit, then the nearest, then the lowest id. Units fire only while their regiment is engaged or
// attacking; buildings always defend.
//
// Abilities stage: projectiles in flight move and hit; then per fighter, turrets turn toward
// their weapon's target at their yaw rate (and back to rest after reset_time), and each weapon
// whose recharge is over fires when aimed (turret within turretAimTolerance, local gun's body
// within localFireArc). A shot with a projectile spawns one that flies distance / speed; others
// hit at the end of the stage, all together, so a unit killed this tick has still fired (no
// advantage from entity order). Area shots hit the nearest enemies within `area` (at most max_targets), with
// linear attenuation from attenuation_range_min (full) to the edge (attenuation_factor_max).
// Damage: CombatRules::damage (tuning.hpp). A unit at 0 health dies (destroyed in Cleanup); a
// regiment with no living units goes with it. There is no friendly fire and no hit chance.

#include "core/sim.hpp"

namespace corps {

// Combat component for a newly spawned unit or building, sized from its def's weapons/turrets.
void attachCombat(Sim& sim, EntityId e, std::uint8_t player);
// Engagement component for a newly spawned regiment (first ZOC sample staggered by its id).
void attachEngagement(Sim& sim, EntityId regiment);
// Move / Run / Stop: the regiment stops fighting and forgets Attack / AttackMove; it won't
// auto-engage for CombatTuning::moveGraceSeconds.
void clearEngagement(Sim& sim, EntityId regiment);

}  // namespace corps
