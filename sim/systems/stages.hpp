#pragma once

// Stage entry points (ARCHITECTURE §4.3). Each system gets the sim for one stage of one tick.
// They are empty until their milestone; keeping them in the loop now fixes the order for good.

namespace corps {

class Sim;

void runEconomy(Sim& sim);      // M6: production, upkeep, shortfall conversion, limits
void runTimers(Sim& sim);       // M6: construction / pack / unpack / recruit / research timers
void runOrders(Sim& sim);       // M4: regiment/group order state machines -> plans
void runPathfinding(Sim& sim);  // M4: request queue with a fixed node-expansion budget
void runMovement(Sim& sim);     // M4: anchors, formation slots, steering, avoidance
void runVision(Sim& sim);       // M4/M9: per-team visibility grid
void runEngagement(Sim& sim);   // M5: ZOC, engagement states, target selection
void runAbilities(Sim& sim);    // M5: fire, projectiles, effects, damage
void runMorale(Sim& sim);       // M5: morale, XP, entrenchment, combat efficiency
void runSupply(Sim& sim);       // M6: supply zones, healing, replacement, under siege
void runCapture(Sim& sim);      // M6: capture, objectives, victory

}  // namespace corps
