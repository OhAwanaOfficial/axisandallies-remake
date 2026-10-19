#pragma once

// The entity world: registry + every component pool. Components mirror the original's TGI
// components (ARCHITECTURE §4.3); they arrive with their systems. Pools are listed in a FIXED
// order in forEachPool, which is the order of the canonical serializer and of destruction.

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

struct World {
    EntityRegistry registry;
    Pool<Transform> transforms;
    Pool<DefRef> defRefs;
    Pool<Health> health;
    Pool<Member> members;

    // Fixed pool order: serializer sections and component removal follow it.
    template <class F>
    void forEachPool(F&& f) {
        f(transforms);
        f(defRefs);
        f(health);
        f(members);
    }

    // Runs the registry's deferred destruction and drops the dead entities' components.
    std::vector<EntityId> cleanup();
};

}  // namespace corps
