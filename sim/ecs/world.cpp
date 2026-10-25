#include "ecs/world.hpp"

namespace corps {

World::World() {
    for (std::size_t p = 0; p < kMaxPlayers; ++p) kingdoms[p].team = static_cast<std::uint8_t>(p);
}

std::vector<EntityId> World::cleanup(const std::function<void(EntityId)>& beforeRemove) {
    std::vector<EntityId> dead = registry.cleanup();
    for (const EntityId id : dead) {
        if (beforeRemove) beforeRemove(id);
        forEachPool([&](auto& pool) { pool.removeIndex(id.index()); });
    }
    return dead;
}

}  // namespace corps
