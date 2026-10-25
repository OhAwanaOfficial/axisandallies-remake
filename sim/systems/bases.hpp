#pragma once

// Bases (M6 part 1, GAME_DESIGN §7, §9; PATHFINDING §4.1): player buildings (Structure), their
// placement, packing onto trucks and unpacking, recruiting and research. Commands 10..16 and the
// debug commands 209 / 210; the Timers stage (runBases) advances everything that takes time.
//
// Placement (placeBuilding): the footprint comes from the building's Size (SQUARE square_size,
// RECT rect_width x rect_height, CIRCLE 2 x blocking_radius), turned by the facing (axis_locked:
// snapped to 90 degrees) and snapped to whole cells. It is valid when the footprint and BaseTerrain
// padding around it are inside the map and passable for foot and wheeled movement (no water,
// cliffs or buildings), no building stamp is within EconomyDef::buildingPadding (BuildingPaddingMeters
// = 2) of the footprint, and, for a Corps HQ (property corpshq), the kingdom has no other.
// placement_groups are only the editor's palettes; they add no rule.
//
// Pack / unpack: a truck regiment ordered to Unpack drives to the footprint's centre (Deploy). On
// arrival it is checked again; then the trucks give way to the building, which stamps its
// footprint at once (units there walk out) and unpacks for its unpacking_length, keeping the
// trucks' health fraction and the regiment's XP. Pack (health at least pack_health_min, nothing
// queued or researched) takes packing_length, then the building gives way to its packed HQ
// regiment, or to its truck's convoy regiment. Unpacking and packing buildings don't produce.
//
// Recruit: a deployed building queues up to kMaxRecruitQueue regiments from its RecruitComponent
// (a bare truck: its convoy regiment). Checks: the regiment's and units' money cost, the unit and
// building limits (reservations included), and every unit's required_structures (all of them,
// owned and deployed) and required_properties (on the recruiting building: its own, its LOCAL
// research, and the kingdom's research that applies to it). The money is taken when queued. The
// front regiment is recruited one unit at a time: an empty regiment appears at the building's
// creation offset, heading for its deployment offset, and a unit joins every
// recruit_time / units (the first after one interval) [CALIBRATE]. Recruiting ignores supply
// until supply zones exist (part 2).
//
// Research: a deployed building researches one ResearchItem at a time; the money is taken at the
// start; after research_time the property is granted: GLOBAL to the kingdom, LOCAL to the
// building. SUPPORT (settlements) is not supported yet.

#include <cstdint>
#include <vector>

#include "core/command.hpp"
#include "data/defs.hpp"
#include "ecs/world.hpp"

namespace corps {

class Sim;

constexpr std::size_t kMaxRecruitQueue = 5;

struct Placement {
    bool ok = false;
    const char* why = "";
    FVec2 center;    // snapped
    Angle facing;    // snapped (axis_locked)
    Footprint rect;  // the cells it stamps
};

// Where building `building` would stand for `player` at `at` facing `facing`, and whether it may.
// `ignore`: a truck regiment whose own pending Corps HQ doesn't count (it is the one placing).
Placement placeBuilding(const Sim& sim, std::uint32_t building, FVec2 at, Angle facing, std::uint8_t player,
                        EntityId ignore = {});

// The units a regiment is recruited with, in order: element kind and def, and its layout element
// (Member::slot). Matches the order SpawnRegiment creates them in.
struct RecruitElement {
    ElementRef element;
    std::uint32_t slot = 0;
};
std::vector<RecruitElement> recruitPlan(const DefSet& defs, std::uint32_t regiment);
// The unit and building limits regiment `regiment` consumes from plan element `from` on (and the
// regiment's own when `withRegiment`).
ResourceAmounts recruitLimits(const DefSet& defs, std::uint32_t regiment, std::uint32_t from, bool withRegiment);
// The money it costs: the regiment's cost and every unit's.
Fixed recruitCost(const DefSet& defs, std::uint32_t regiment);

void applyStartPlayer(Sim& sim, const Command& c, const BaseOrderPayload& p);
void applyRecruit(Sim& sim, const Command& c, const BaseOrderPayload& p);
void applyCancelRecruit(Sim& sim, const Command& c, const BaseOrderPayload& p);
void applyUnpack(Sim& sim, const Command& c, const BaseOrderPayload& p);
void applyPack(Sim& sim, const Command& c, const BaseOrderPayload& p);
void applyResearch(Sim& sim, const Command& c, const BaseOrderPayload& p);
void applyCancelResearch(Sim& sim, const Command& c, const BaseOrderPayload& p);
void applyDebugSetMoney(Sim& sim, const Command& c, const BaseOrderPayload& p);
void applyDebugGrantProperty(Sim& sim, const Command& c, const BaseOrderPayload& p);

// Timers stage: deploys arriving, unpack / pack timers, recruiting, research.
void runBases(Sim& sim);

}  // namespace corps
