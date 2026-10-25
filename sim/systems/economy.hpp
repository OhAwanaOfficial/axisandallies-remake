#pragma once

// Economy (M6, GAME_DESIGN §5). Each kingdom that has started (StartPlayer) has a money
// stockpile; oil and ammo are income only. Per minute:
//
//   production  = the start location's + every deployed Structure's (one with supply_required only
//                 while in supply; cities held included) + every truck's (units with an
//                 EconomyComponent Production: trucks produce while packed)
//   upkeep      = every regiment's (OrganizationComponent's EconomyComponent) + every unit's +
//                 every Structure's (whatever its state)
//   oil, ammo   = production - upkeep; a deficit is bought with money at EconomyDef::oilPrice /
//                 ammoPrice (3 and 2: 1 oil = 3 money, 1 ammo = 2 money)
//   net money   = money production - money upkeep - that shortfall cost
//
// The Economy stage adds net / 600 to the stockpile every tick (10 ticks per second), clamped to
// 0 .. EconomyDef::stockpileMax. A kingdom with no money and a negative net income is bankrupt
// (events Bankrupt / Solvent). Its effects on resupply and attrition: systems/supply.
//
// Limits: the unit limit provided (capped at EconomyDef::limitedMax) and consumed (regiments'
// unit_limit_consumed), and the building limit provided (the start location's 20) and consumed
// (trucks and buildings). Regiments queued or being recruited reserve what they will consume.
//
// Totals are recomputed every tick in ascending entity order; they are not state.

#include <array>
#include <cstdint>
#include <vector>

#include "data/defs.hpp"
#include "ecs/world.hpp"

namespace corps {

class Sim;

struct Ledger {
    ResourceAmounts production;  // per minute; limits provided
    ResourceAmounts upkeep;      // per minute; limits consumed (including reservations)
    Fixed oilNet() const { return production.oil - upkeep.oil; }
    Fixed ammoNet() const { return production.ammo - upkeep.ammo; }
    Fixed shortfallCost = {};    // money per minute buying the oil / ammo deficit
    Fixed moneyNet() const { return production.money - upkeep.money - shortfallCost; }
    Fixed unitLimit = {};        // provided, capped
    Fixed buildingLimit = {};    // provided
};

// Every kingdom's ledger (index = player).
std::array<Ledger, kMaxPlayers> ledgers(const Sim& sim);
Ledger ledgerOf(const Sim& sim, std::uint8_t player);

// Economy stage.
void runEconomy(Sim& sim);

// --- properties (GAME_DESIGN §9) ------------------------------------------------------------------
// An actor's properties: its def's, the LOCAL research of its Structure, and the kingdom's GLOBAL
// research whose required_properties it has. Sorted property indexes.
std::vector<std::uint32_t> effectiveProperties(const Sim& sim, EntityId actor);
// AV / DV the kingdom's research adds to an actor (ADD modifiers of the research properties that
// apply to it). 0 without research.
Fixed researchAttackValue(const Sim& sim, EntityId actor);
Fixed researchDefenseValue(const Sim& sim, EntityId actor);

// The player owning an actor: a Structure's, a regiment's, or a unit's regiment's (none: 0xFF).
std::uint8_t ownerOf(const Sim& sim, EntityId actor);

}  // namespace corps
