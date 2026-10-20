#pragma once

// The entity world: registry + every component pool. Components mirror the original's TGI
// components (ARCHITECTURE §4.3); they arrive with their systems. Pools are listed in a FIXED
// order in forEachPool, which is the order of the canonical serializer and of destruction.

#include <functional>
#include <vector>

#include "ecs/entity.hpp"
#include "ecs/pool.hpp"
#include "math/angle.hpp"
#include "math/fvec2.hpp"

namespace corps {

// Ground-plane placement (the sim is 2D; height is view-only).
struct Transform {
    FVec2 position;
    Angle facing;
    friend bool operator==(const Transform& a, const Transform& b) noexcept {
        return a.position == b.position && a.facing == b.facing;
    }
};

// Which data definition an entity was spawned from (index into the DefSet's array of that kind).
struct DefRef {
    enum class Kind : std::uint8_t { Unit = 1, Regiment = 2, Building = 3 };
    Kind kind = Kind::Unit;
    std::uint32_t index = 0;
    friend bool operator==(const DefRef& a, const DefRef& b) noexcept { return a.kind == b.kind && a.index == b.index; }
};

// Health from BodyComponent (M3 spawn-test; combat systems arrive in M5).
struct Health {
    Fixed current;
    Fixed max;
    friend bool operator==(const Health& a, const Health& b) noexcept { return a.current == b.current && a.max == b.max; }
};

// Membership of a unit in its regiment (OrganizationComponent slot).
struct Member {
    EntityId organization;
    std::uint16_t slot = 0;
    friend bool operator==(const Member& a, const Member& b) noexcept { return a.organization == b.organization && a.slot == b.slot; }
};

// Cells a building blocks in the nav grid (inclusive rect, PATHFINDING §2.1). Stamped while the
// entity is alive; unstamped when it is destroyed.
struct Footprint {
    std::int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    friend bool operator==(const Footprint& a, const Footprint& b) noexcept {
        return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
    }
};

// Latest path request of an entity and, once the PathService finishes it, the smoothed result.
struct Path {
    enum class Status : std::uint8_t { Found = 0, FoundNearest = 1, Unreachable = 2, NoStart = 3, Pending = 4 };
    Status status = Status::Pending;
    std::uint8_t moveClass = 0;  // nav::MoveClass
    std::uint32_t requestId = 0;
    std::int32_t cost = 0;
    std::vector<std::uint32_t> waypoints;  // nav cells, start..goal
    friend bool operator==(const Path& a, const Path& b) noexcept {
        return a.status == b.status && a.moveClass == b.moveClass && a.requestId == b.requestId && a.cost == b.cost &&
               a.waypoints == b.waypoints;
    }
};

// A regiment (on the organization entity; PATHFINDING §4). Its Transform is the invisible anchor.
struct Regiment {
    static constexpr std::uint16_t kNone = 0xFFFF;
    std::uint32_t layout = 0xFFFFFFFFu;   // DefSet::layouts index (none: legacy placement)
    std::uint8_t player = 0;
    std::uint8_t moveClass = 0;           // UnitMoveClass: the most restrictive member's
    std::uint16_t formation = 0;          // chosen formation: index into the layout's formations
    std::uint16_t runRestore = kNone;     // formation to go back to after a Run
    std::uint8_t compression = 0;         // chokepoints: 0 = none, 1 = layout Column, 2 = single file
    std::uint8_t compressionHold = 0;     // ticks of wide clearance seen before decompressing
    EntityId group;                       // Group entity, or null
    friend bool operator==(const Regiment& a, const Regiment& b) noexcept {
        return a.layout == b.layout && a.player == b.player && a.moveClass == b.moveClass && a.formation == b.formation &&
               a.runRestore == b.runRestore && a.compression == b.compression && a.compressionHold == b.compressionHold &&
               a.group == b.group;
    }
};

// Movement order state of an anchor: a regiment, or a division's Group entity.
struct Order {
    enum class State : std::uint8_t { Idle = 0, WaitPath = 1, Moving = 2, Wheeling = 3, Arriving = 4, Following = 5 };
    State state = State::Idle;
    bool needPath = false;      // the Orders stage requests a path next tick
    bool hasFacing = false;     // explicit facing on arrival
    bool blocked = false;       // the last order ended without reaching the goal
    std::uint8_t repaths = 0;   // stuck re-requests in this order
    std::uint8_t window = 0;    // ticks in the current stuck-detection window
    Angle facing;
    FVec2 goal;
    std::uint32_t waypoint = 0; // next Path waypoint
    std::uint32_t stateTicks = 0;
    Fixed progress;             // distance actually moved in the window
    Fixed expected;             // distance the anchor's speed allowed in the window
    friend bool operator==(const Order& a, const Order& b) noexcept {
        return a.state == b.state && a.needPath == b.needPath && a.hasFacing == b.hasFacing && a.blocked == b.blocked &&
               a.repaths == b.repaths && a.window == b.window && a.facing == b.facing && a.goal == b.goal &&
               a.waypoint == b.waypoint && a.stateTicks == b.stateTicks && a.progress == b.progress && a.expected == b.expected;
    }
};

// A unit's current velocity (world units per second).
struct Mover {
    FVec2 velocity;
    friend bool operator==(const Mover& a, const Mover& b) noexcept { return a.velocity == b.velocity; }
};

// Up to 6 regiments sharing orders (GAME_DESIGN §6.11). As a division (`division`), the Group
// entity's Transform/Order/Path are the division anchor and members hold group-formation slots.
struct Group {
    static constexpr std::size_t kMaxMembers = 6;
    std::uint8_t player = 0;
    bool division = false;
    std::uint32_t groupFormation = 0xFFFFFFFFu;  // DefSet::groupFormations index
    std::vector<EntityId> members;               // ascending
    std::vector<EntityId> slotOrder;             // members by slot: strongest first (division)
    friend bool operator==(const Group& a, const Group& b) noexcept {
        return a.player == b.player && a.division == b.division && a.groupFormation == b.groupFormation &&
               a.members == b.members && a.slotOrder == b.slotOrder;
    }
};

struct World {
    EntityRegistry registry;
    Pool<Transform> transforms;
    Pool<DefRef> defRefs;
    Pool<Health> health;
    Pool<Member> members;
    Pool<Footprint> footprints;
    Pool<Path> paths;
    Pool<Regiment> regiments;
    Pool<Order> orders;
    Pool<Mover> movers;
    Pool<Group> groups;

    // Fixed pool order: serializer sections and component removal follow it.
    template <class F>
    void forEachPool(F&& f) {
        f(transforms);
        f(defRefs);
        f(health);
        f(members);
        f(footprints);
        f(paths);
        f(regiments);
        f(orders);
        f(movers);
        f(groups);
    }

    // Runs the registry's deferred destruction and drops the dead entities' components.
    // `beforeRemove` sees each dead entity (ascending) while its components still exist.
    std::vector<EntityId> cleanup(const std::function<void(EntityId)>& beforeRemove = {});
};

}  // namespace corps
