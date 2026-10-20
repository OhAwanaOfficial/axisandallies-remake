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
// aren't are skipped, and the command is rejected if none is left.

#include "core/command.hpp"

namespace corps {

class Sim;

void applyMoveGroup(Sim& sim, const Command& c, const MoveGroupPayload& p, bool run);
void applyStop(Sim& sim, const Command& c, const RegimentListPayload& p);
void applySetFormation(Sim& sim, const Command& c, const SetFormationPayload& p);
void applySetGroupBehaviour(Sim& sim, const Command& c, const SetGroupBehaviourPayload& p);

// Cleanup hook: a destroyed regiment leaves its group.
void onEntityDestroyed(Sim& sim, EntityId id);

}  // namespace corps
