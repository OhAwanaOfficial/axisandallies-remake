#pragma once

// Movement orders (GAME_DESIGN §6.5, §6.11; PATHFINDING §5): MoveGroup / Run / Stop /
// SetFormation / SetGroupBehaviour, group bookkeeping, and the Orders stage, which turns
// `Order::needPath` into PathService requests.
//
// Groups: ordering several regiments together makes them one Group (reused when the same set is
// ordered again). Ordering a single regiment takes it out of its group. A group is a loose group
// (each regiment paths on its own to goal + its group-formation offset) or, with group behaviour
// on, a division (one anchor paths for all; members hold slots around it). Groups shrinking below
// two regiments dissolve. Commands only accept the sending player's live regiments; ids that
// aren't are skipped, and the command is rejected if none is left. Routing regiments don't obey.
//
// Combat (systems/combat.cpp) puts moves on hold while a regiment fights (Order::State::Engaged)
// and resumes them; Attack and AttackMove (M5) set the regiment's Engagement mode.

#include "core/command.hpp"

namespace corps {

class Sim;

void applyMoveGroup(Sim& sim, const Command& c, const MoveGroupPayload& p, bool run);
void applyStop(Sim& sim, const Command& c, const RegimentListPayload& p);
void applySetFormation(Sim& sim, const Command& c, const SetFormationPayload& p);
void applySetGroupBehaviour(Sim& sim, const Command& c, const SetGroupBehaviourPayload& p);
void applyAttack(Sim& sim, const Command& c, const AttackPayload& p);
void applyAttackMove(Sim& sim, const Command& c, const MoveGroupPayload& p);
void applySetEntrenched(Sim& sim, const Command& c, const DebugSetEntrenchedPayload& p);
void applyRetreat(Sim& sim, const Command& c, const MoveGroupPayload& p);
void applySetExperience(Sim& sim, const Command& c, const DebugSetExperiencePayload& p);

// --- for combat -----------------------------------------------------------------------------------
// The entity whose Order moves a regiment: its division's Group, or the regiment itself.
EntityId orderOwner(const Sim& sim, EntityId regiment);
// A moving order goes on hold (Engaged), keeping its goal. true if it was moving.
bool holdOrder(Sim& sim, EntityId owner);
// An order on hold resumes with a fresh path to its goal.
void resumeOrder(Sim& sim, EntityId owner);
// Starts a move of `owner` (a regiment) to `goal` again (a held move resuming after a close-in).
void restartOrder(Sim& sim, EntityId owner, FVec2 goal, bool hasFacing, Angle facing);
// Stops `owner`'s move where it is.
void stopOrder(Sim& sim, EntityId owner);
// Takes a regiment out of its group (rout, retreat).
void detachFromGroup(Sim& sim, EntityId regiment);
// Moves a single regiment toward `goal` (Attack chasing its target); keeps its Engagement.
void chaseOrder(Sim& sim, EntityId regiment, FVec2 goal);

// Cleanup hook: a destroyed regiment leaves its group.
void onEntityDestroyed(Sim& sim, EntityId id);

}  // namespace corps
