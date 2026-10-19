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

struct World {
    EntityRegistry registry;
    Pool<Transform> transforms;
    Pool<DefRef> defRefs;
    Pool<Health> health;
    Pool<Member> members;
    Pool<Footprint> footprints;
    Pool<Path> paths;

    // Fixed pool order: serializer sections and component removal follow it.
    template <class F>
    void forEachPool(F&& f) {
        f(transforms);
        f(defRefs);
        f(health);
        f(members);
        f(footprints);
        f(paths);
    }

    // Runs the registry's deferred destruction and drops the dead entities' components.
    // `beforeRemove` sees each dead entity (ascending) while its components still exist.
    std::vector<EntityId> cleanup(const std::function<void(EntityId)>& beforeRemove = {});
};

}  // namespace corps
