#pragma once

// Stage entry points (ARCHITECTURE §4.3). Each system gets the sim for one stage of one tick.
// They are empty until their milestone; keeping them in the loop now fixes the order for good.

namespace corps {

class Sim;

// Economy: runEconomy (systems/economy.hpp). Timers: runBases (systems/bases.hpp).
void runOrders(Sim& sim);       // M4: regiment/group order state machines -> plans
void runPathfinding(Sim& sim);  // M4: request queue with a fixed node-expansion budget
void runMovement(Sim& sim);     // M4: anchors, formation slots, steering, avoidance
void runVision(Sim& sim);       // M4/M9: per-team visibility grid
void runEngagement(Sim& sim);   // M5: ZOC, engagement states, target selection (systems/combat.cpp)
void runAbilities(Sim& sim);    // M5: fire, projectiles, effects, damage, deaths (systems/combat.cpp)
void runMorale(Sim& sim);       // M5: morale, XP, entrenchment, combat efficiency (systems/morale.cpp)
void runSupply(Sim& sim);       // M6: supply zones, healing, replacement, under siege
void runCapture(Sim& sim);      // M6: capture, objectives, victory

}  // namespace corps
