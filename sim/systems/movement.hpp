#pragma once

// Regiment movement (PATHFINDING §4–6, GAME_DESIGN §6.4–6.5, §6.11): orders → anchors → slots →
// units. Shared by the Orders stage (systems/orders.cpp: commands and path requests) and the
// Movement stage (systems/movement.cpp).
//
// Frames: a regiment's Transform is its invisible anchor; slot targets are anchor.position +
// offset.rotated(anchor.facing) with layout offsets in the regiment frame (+x forward, +y left).
// A division's Group entity has its own anchor; member regiments target group-formation slots.
//
// Everything is integer / Fixed and iterates pools by ascending EntityId.

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/sim.hpp"
#include "nav/map_def.hpp"

namespace corps {

using namespace fixed_literals;

namespace move {

constexpr Fixed kTick = 0.1_fx;  // seconds per tick (10 Hz)
constexpr Fixed kSlotTolerance = 1.5_fx;                   // a unit "is in its slot" within this
constexpr Fixed kWaypointReach = 0.5_fx;
constexpr Fixed kSlotLength = 3_fx;            // the anchor waits once a unit lags more than this
constexpr std::uint32_t kArriveTimeoutTicks = 100;
constexpr std::uint32_t kDivisionArriveTimeoutTicks = 150;
constexpr std::uint32_t kWheelTimeoutTicks = 30;
constexpr std::int32_t kWheelAngle = 16384;    // > 90° to the next waypoint: wheel in place
constexpr std::int32_t kAnchorTurnPerTick = 2048;  // 11.25° per tick
constexpr std::uint8_t kStuckWindowTicks = 20;
constexpr std::uint8_t kMaxRepaths = 3;
constexpr std::uint8_t kDecompressHoldTicks = 10;
constexpr std::int32_t kBucketCells = 4;       // spatial hash buckets (PATHFINDING §6)
constexpr Fixed kDefaultUnitRadius = 0.5_fx;
constexpr Fixed kAvoidMargin = 0.25_fx;
constexpr Fixed kArriveGain = 2_fx;            // per second: approach speed = distance × gain
constexpr Fixed kSeparationGain = 10_fx;       // per second per unit of overlap
constexpr std::int32_t kPivotAngle = 5461;     // in-place turners stop to pivot beyond 30°
constexpr std::uint32_t kArrivedPercent = 80;  // % of living units in their slots to finish

// Defaults when SVars lack them.
constexpr Fixed kGroupSpeedMinimum = 1_fx;
constexpr Fixed kGroupZoneRangeMin = 18_fx;

}  // namespace move

// Derived per-map data the Movement stage needs; rebuilt whenever the nav graph version changes.
// Not sim state: it is a pure function of the grid (which the state hash already pins).
struct MovementCache {
    bool valid = false;
    std::uint32_t navVersion = 0;
    // Chamfer distance (10 per straight, 14 per diagonal step) from each cell to the nearest cell
    // the class can't enter (or the map edge), per nav class. clearance/10 = world units.
    std::array<std::vector<std::uint16_t>, nav::kMoveClassCount> clearance;
};

// --- helpers shared by orders.cpp and movement.cpp ----------------------------------------------

// The nav class a regiment or group paths with (Air has none).
bool hasNavClass(std::uint8_t unitMoveClass);
// Most restrictive of two UnitMoveClass values (Naval > Wheeled > Tracked > Foot; Air only if both).
std::uint8_t restrictiveClass(std::uint8_t a, std::uint8_t b);

// Regiment slot offset of layout element `element` in formation `formation` (index into the
// layout's formations), or a fallback when the layout doesn't have it.
FVec2 layoutOffset(const DefSet& defs, const Regiment& r, std::uint32_t formation, std::uint32_t element);

// Division slot k of a group: group-formation offsets sorted front to back (forward desc, then
// |lateral| asc, then left first); slots past the formation's six trail behind.
FVec2 groupSlotOffset(const Sim& sim, const Group& g, std::size_t k);

// Speed of a regiment's slowest living unit × its formation's SPEED_MAX multiplier (units/s).
// 0 if it has no living units.
Fixed regimentSpeed(const Sim& sim, EntityId regiment, const Regiment& r);

// World-space slot target of each of a regiment's mobile units (ascending), as the Movement stage
// steers them: formation, Column or single file per the regiment's compression. View/debug use.
std::vector<std::pair<EntityId, FVec2>> slotTargets(const Sim& sim, EntityId regiment);

// Sum of current health of a regiment's units: the "combat value" that orders division slots.
Fixed regimentStrength(const Sim& sim, EntityId regiment);

}  // namespace corps
