#include "ecs/world.hpp"

namespace corps {

std::vector<EntityId> World::cleanup() {
    std::vector<EntityId> dead = registry.cleanup();
    for (const EntityId id : dead) {
        forEachPool([&](auto& pool) { pool.removeIndex(id.index()); });
    }
    return dead;
}

}  // namespace corps
