#pragma once

// Supply, cities, capture and victory (M6 part 2, GAME_DESIGN §8, §12).
//
// Supply zones: a circle of SupplyComponent supply_range around every deployed Structure that has
// one (Corps HQ, depots, ...) and every city a player holds. Allies (same Kingdom::team) share
// them. A relay (is_relay) supplies only while it stands in another active zone of its team.
// Unpacking / packing buildings, neutral cities and eliminated kingdoms supply nothing.
//
// Under siege: an enemy unit within the supplier's siege radius (its control_range, else a city's
// zone_radius, else its supply_range) [CALIBRATE], or a hit taken within
// UnderSiegeRecentlyDamagedSeconds (7 s). A besieged zone switches off, unless
// SupplyProvideUnderSiege (true in the data): then it keeps supplying at DenizenResupplyRateUnderSiege
// (x0.25). A regiment in several zones uses the best one.
//
// In supply (its anchor inside an active zone of its team, and OrganizationComponent supply_used):
//   * its units heal resupply_rate x max health per minute (ElementComponent; infantry 0.3, tanks 0.1);
//   * a dead unit is replaced for free every recruit_time / units (the regiment's recruit
//     interval), lowest missing layout element first, at its formation slot [CALIBRATE: the
//     original may walk it in from the supplier]; replacements keep the regiment's XP (it lives on
//     the regiment) and level, and add to its morale ceiling without refilling morale;
//   * morale replenishment gets MoraleSupplyBonus (systems/morale);
//   * buildings with EconomyComponent supply_required produce only in supply (systems/economy),
//     and buildings recruit only in supply (systems/bases).
// Bankrupt kingdoms (systems/economy): in supply, healing and replacement run at x0.25 (the
// DenizenResupplyRateUnderSiege factor, stacking with a siege); out of supply, every unit loses
// OrganizationComponent shortfall_damage_rate x max health per second until it dies.
//
// Cities: every map thing whose def has a CityComponent (after translate.tgi renames) is spawned
// at Sim construction as a Structure: owned by nobody (kNeutral) unless its map kingdom is a
// playable one (owns a placeholder_corps* thing), with full health, its footprint stamped, no
// weapons (so nothing targets it: cities are indestructible here). A city produces its
// EconomyComponent Production for its holder and supplies like any supplier.
//
// Capture (Capture stage): a player's unit within the city's capture radius (half its footprint
// + 4 m) [CALIBRATE], and no unit of any other team within zone_radius (42 m), for 5 s
// [CALIBRATE] gives it the city: its health rises to at least CaptureHealthFactor (0.3) x max, the
// regiment of the nearest unit gains ExperienceDefaultValue x ExperienceCaptureMultiplier XP.
// Contesting or leaving resets the progress. Event CityCaptured.
//
// Victory (Capture stage, GAME_DESIGN §12; game_types_aa.tgi defines no rule fields, so these are
// ours): a kingdom that started (StartPlayer) is eliminated once it has no HQ left: no Structure
// with HQComponent (or its truck's) and no regiment with a unit left that is one or carries one
// (KingdomDefeated). A building that dies drops the regiment it was recruiting if that is still
// empty (core/sim.cpp cleanup).
// With TeamVictory (true) the last team with a kingdom standing wins (else the last kingdom), once
// at least two have played. With a city victory ratio > 0 (map rule or SetMatchRules; Golden
// Fields: 0) a team (or kingdom) holding at least that share of the cities' value (their money
// production) wins at once (CityRatioVictory). The match then records the winner; the sim goes on.

#include <cstdint>
#include <string>
#include <vector>

#include "core/command.hpp"
#include "data/defs.hpp"
#include "ecs/world.hpp"

namespace corps::nav {
struct MapDef;
}

namespace corps {

class Sim;

struct SupplyZone {
    EntityId source;      // the supplying Structure
    FVec2 center;
    Fixed radius;
    std::uint8_t team = kNeutral;
    bool relay = false;
    bool besieged = false;
    bool active = false;  // supplies this tick (relays: linked; besieged: only if SupplyProvideUnderSiege)
};

// Every supplier's zone this tick, ascending by source. Derived (recomputed), not state.
std::vector<SupplyZone> supplyZones(const Sim& sim);
// The best active zone of `team` containing `at`: unbesieged first, then lowest source (null: none).
const SupplyZone* zoneAt(const std::vector<SupplyZone>& zones, std::uint8_t team, FVec2 at);

// Map cities (see above). Called by the Sim constructor when it has data and a map.
void spawnMapCities(Sim& sim);
// A city's value for the city victory ratio: its money production per minute.
Fixed cityValue(const DefSet& defs, std::uint32_t building);
// Whether `building` is a city centre (CityComponent zone_radius > 0, captureable).
bool isCity(const DefSet& defs, std::uint32_t building);

// Start positions of a map: one per playable map kingdom (owner of a placeholder_corps* thing), in
// kingdom order; `player` is the kingdom's index.
struct MapStart {
    std::uint8_t player = 0;
    std::string placeholder;  // placeholder_corpsunpacked ...
    FVec2 position;
    Angle facing;
};
std::vector<MapStart> mapStarts(const nav::MapDef& map);
// The nation's thing for a map placeholder (NationDef placeholders), e.g. ("gbr" or "nation_gbr",
// "placeholder_corpsunpacked") -> "gbr_corps". Empty if none.
std::string placeholderThing(const DefSet& defs, std::string_view nation, std::string_view placeholder);

void applySetMatchRules(Sim& sim, const Command& c, const MatchRulesPayload& p);
void applyDebugSetHealth(Sim& sim, const Command& c, const BaseOrderPayload& p);
void applyDebugSetOwner(Sim& sim, const Command& c, const BaseOrderPayload& p);

// Supply stage: zones, siege, healing, replacement, bankrupt attrition.
void runSupply(Sim& sim);
// Capture stage: city capture, elimination, victory.
void runCapture(Sim& sim);

}  // namespace corps
