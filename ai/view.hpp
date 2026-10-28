#pragma once

// The AI's read API (docs/AI.md §2a): everything a host-side AI may know about the match, filtered
// by its team's vision. Controllers read this instead of the World, so they can't see what a
// player couldn't: own kingdom (resources, income, limits, buildings, regiments, research, army
// XP), the enemies its team sees now, a last-seen memory with timestamps, the cities and their
// owners (cities are always shown, like EXPLORED fog), passability per move class, and its team's
// supply coverage.
//
// Vision is computed here, read-only, until the sim's Vision stage exists (M9 fog): a hostile
// unit or building is visible when it is within detection_range of a living unit of the team
// (UnitDef, else its RegimentDef) or within kStructureSight of one of the team's structures.
// Everything is integer / Fixed maths over the sim's ascending-id pools, so it is deterministic.

#include <cstdint>
#include <map>
#include <vector>

#include "ecs/entity.hpp"
#include "ecs/world.hpp"
#include "math/fixed.hpp"
#include "math/fvec2.hpp"
#include "nav/nav_grid.hpp"
#include "systems/economy.hpp"
#include "systems/supply.hpp"

namespace corps {
class Sim;
}

namespace corps::ai {

// BuildingDef has no detection range yet: structures see this far (metres).
inline constexpr std::int64_t kStructureSight = 40;

// A hostile regiment or structure, as the team saw it (now, or last).
struct Seen {
    EntityId id;
    std::uint8_t player = 0;
    bool structure = false;
    std::uint32_t def = 0;       // RegimentDef or BuildingDef index
    FVec2 at;                    // anchor / building position
    std::uint32_t units = 0;     // living units it showed (1 for a structure)
    Fixed health;                // their summed health
    bool hq = false;             // an HQ (keeps its kingdom alive) or the Corps HQ
    std::uint32_t tick = 0;      // when last seen
};

struct OwnRegiment {
    EntityId id;
    std::uint32_t def = 0;
    FVec2 at;
    std::uint32_t units = 0;
    Fixed health, healthMax;
    bool hq = false;
    bool truck = false;          // its trucks unpack into a building
    std::uint32_t unpacksTo = 0; // that building (kNoDef if not a truck)
    bool forming = false;        // still being recruited
    bool deploying = false;      // a truck on its way to unpack
    bool idle = false;           // no move order and no attack order
    bool orderable = true;       // not routing, retreating or exhausted
    Engagement::State state = Engagement::State::Idle;
    Engagement::Mode mode = Engagement::Mode::None;
    EntityId target;
    Fixed morale, efficiency, xp;
    bool inSupply = false;
};

struct OwnBuilding {
    EntityId id;
    std::uint32_t def = 0;
    FVec2 at;
    Structure::State state = Structure::State::Deployed;
    Fixed health, healthMax;
    bool corps = false;          // provides the Corps
    bool hq = false;
    std::uint32_t queued = 0;    // recruit orders waiting
    std::vector<std::uint32_t> queue;  // their RegimentDef indexes, front first
    bool researching = false;
    bool inSupply = false;
    bool underSiege = false;
};

struct City {
    EntityId id;
    FVec2 at;
    std::uint8_t owner = kNeutral;
    Fixed radius;                // capture zone
    Fixed value;                 // money per minute it yields (its share of the city ratio)
};

class View {
public:
    explicit View(std::uint8_t player, std::uint32_t memoryTicks = 3 * 60 * 10);

    // Re-reads the sim (call once per think).
    void update(const Sim& sim);

    std::uint8_t player() const { return me; }
    std::uint8_t team() const { return myTeam; }
    std::uint32_t tick() const { return now; }
    bool alive() const { return started && !eliminated; }

    // Own kingdom.
    Fixed money, armyXp;
    bool bankrupt = false;
    Ledger ledger;                          // income, upkeep, limits
    std::vector<std::uint32_t> research;    // GLOBAL properties granted
    std::vector<OwnBuilding> buildings;     // ascending id
    std::vector<OwnRegiment> regiments;     // ascending id
    std::vector<std::uint8_t> allies;       // other living kingdoms of the team
    std::vector<std::uint8_t> enemies;      // living hostile kingdoms

    // Hostile regiments and structures the team sees this update (ascending id).
    const std::vector<Seen>& visible() const { return seenNow; }
    // Everything seen within memoryTicks that wasn't seen gone (ascending id; includes visible()).
    std::vector<Seen> remembered() const;
    const Seen* lastSeen(EntityId id) const;

    const std::vector<City>& cities() const { return cityList; }

    // The team's vision and supply.
    bool sees(FVec2 at) const;
    bool supplied(FVec2 at) const;
    const std::vector<SupplyZone>& supply() const { return zones; }   // the team's active zones

    // Terrain: passable for a move class (cell of `at`); outside the map: false.
    bool passable(nav::MoveClass c, FVec2 at) const;
    std::int32_t mapWidth() const { return grid ? grid->width() : 0; }
    std::int32_t mapHeight() const { return grid ? grid->height() : 0; }

private:
    struct Eye {
        FVec2 at;
        Fixed rangeSq;
    };
    std::uint8_t me = 0;
    std::uint8_t myTeam = 0;
    std::uint32_t memoryTicks = 0;
    std::uint32_t now = 0;
    bool started = false, eliminated = false;
    const nav::NavGrid* grid = nullptr;
    std::vector<Eye> eyes;
    std::vector<Seen> seenNow;
    std::map<EntityId, Seen> memory;
    std::vector<City> cityList;
    std::vector<SupplyZone> zones;
};

}  // namespace corps::ai
