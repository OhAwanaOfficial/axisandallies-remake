#pragma once

// Map definition for the sim (PATHFINDING §2): immutable after creation, shared read-only by every
// Sim (like DefSet), identified by mapHash in states and replays.
//
// Nav cells are 1 world unit (PATHFINDING §2). An original map tile is 2 world units
// (ASSET_PIPELINE §2.1.1), so every tile becomes 2x2 cells. Cell (x, y) covers world
// [x, x+1) x [y, y+1); index = y * width + x. Heights are view-only (the sim never reads them).

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "math/angle.hpp"
#include "math/fixed.hpp"
#include "math/fvec2.hpp"

namespace corps::nav {

// Movement classes with grid pathing. Air has none: it steers over everything (PATHFINDING §2).
enum class MoveClass : std::uint8_t { Foot, Wheeled, Tracked, Naval, Amphibious, Count };
constexpr std::size_t kMoveClassCount = static_cast<std::size_t>(MoveClass::Count);
const char* moveClassName(MoveClass c);
bool parseMoveClass(std::string_view name, MoveClass& out);

enum class TerrainKind : std::uint8_t { Ground, Road, Forest, Beach, Shallow, Water, Mountain, Border, Count };
constexpr std::size_t kTerrainKindCount = static_cast<std::size_t>(TerrainKind::Count);
const char* terrainKindName(TerrainKind k);

// Cell cost multiplier x16 (16 = 1.0); 0 = blocked. Interim table until per-terrain TGI speed
// modifiers are wired in (GAME_DESIGN §6.10): forest and jungle slow, mountains and ocean block
// ground, roads favour wheels and tracks.
constexpr std::uint8_t kCostUnit = 16;
std::uint8_t terrainCost(TerrainKind k, MoveClass c);

// A placed object of an original map (DYNAMIC_THINGS, tools/rmp.py): a Thing by IDS at a position,
// owned by one of the map's kingdoms (index into MapDef::kingdoms; -1: none).
struct MapThing {
    std::string ids;  // after translate.tgi renames (tools/rmp.py "ids_translated")
    FVec2 position;
    Angle facing;
    std::int32_t owner = -1;
    std::string name;  // map string key ("name_6"), if any
};
struct MapKingdom {
    std::string name;   // kingdom_0 ...
    std::string color;  // blue, red, gray ...
};
// A match rule of the map (RULES: CityVictoryRatio, ResourceStockpileDefault, ...).
struct MapRule {
    std::string name;
    Fixed value;
};

struct MapDef {
    std::string name;
    std::uint32_t width = 0;   // cells
    std::uint32_t height = 0;  // cells
    std::vector<TerrainKind> kind;          // per cell
    std::vector<std::uint16_t> terrainId;   // per cell, index into terrainNames (view/debug)
    std::vector<std::string> terrainNames;
    std::uint32_t heightWidth = 0;          // vertices per row (view-only)
    std::vector<Fixed> heights;             // view-only ground height per vertex
    std::vector<MapThing> things;           // placed objects (towns, start locations, ...), file order
    std::vector<MapKingdom> kingdoms;
    std::vector<MapRule> rules;
    std::uint64_t mapHash = 0;              // xxh3 of the canonical bytes (call finalize())

    std::size_t cellCount() const { return static_cast<std::size_t>(width) * height; }
    bool inside(std::int32_t x, std::int32_t y) const {
        return x >= 0 && y >= 0 && static_cast<std::uint32_t>(x) < width && static_cast<std::uint32_t>(y) < height;
    }
    // Validates sizes and computes mapHash. false if inconsistent.
    bool finalize();
    // A rule's value (case-insensitive name), or `fallback`.
    Fixed rule(std::string_view ruleName, Fixed fallback) const;
};

// Deterministic test map (PATHFINDING §8): flat ground with a blocked border ring, ocean blobs
// with beaches, mountain blobs, forest blobs and two roads. Same (seed, size) -> same map, everywhere.
MapDef generateTestMap(std::uint64_t seed, std::uint32_t sizeCells);
// Parses a generated map's name "gen:SEED:SIZE" (decimal). false for anything else.
bool parseGenSpec(std::string_view spec, std::uint64_t& seed, std::uint32_t& size);

// Duel arena (simcli duel, docs/CALIBRATION.md): a square of one terrain inside a blocked border.
// `terrain` names a Terrain or variation IDS (its properties apply to units on it) or "open";
// names containing "forest" or "jungle" are TerrainKind::Forest, others Ground. Name
// "arena:TERRAIN:SIZE".
MapDef generateArenaMap(std::string_view terrain, std::uint32_t size);
bool parseArenaSpec(std::string_view spec, std::string& terrain, std::uint32_t& size);

// Loads tools/rmp.py map.json (needs its integer "vertex_height_mm" layers). Terrain kind per tile
// comes from the class bits (mountain / water / border) and terrain-name keywords (shore/beach,
// shallows, forest/jungle). Each tile becomes 2x2 cells. Returns false and sets `error` on failure.
bool loadRmpJson(std::string_view jsonText, MapDef& out, std::string& error);

}  // namespace corps::nav
