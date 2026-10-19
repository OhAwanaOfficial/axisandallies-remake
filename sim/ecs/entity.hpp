#pragma once

// Entity ids and the registry (ARCHITECTURE §4.3).
//
// EntityId is 32 bits: generation (12, high) | index (20, low). Generations start at 1, so 0 is the
// null id. Destroying an entity is deferred to the Cleanup stage; then its slot's generation is
// bumped and the index goes to the back of a FIFO free list. A slot whose generation would wrap
// past 4095 is retired instead of reused, so a stale id can never alias a newer entity.

#include <cstdint>
#include <vector>

namespace corps {

struct EntityId {
    static constexpr std::uint32_t kIndexBits = 20;
    static constexpr std::uint32_t kIndexMask = (1u << kIndexBits) - 1u;
    static constexpr std::uint32_t kMaxGeneration = (1u << (32 - kIndexBits)) - 1u;  // 4095
    static constexpr std::uint32_t kMaxIndex = kIndexMask;

    std::uint32_t bits = 0;

    static constexpr EntityId make(std::uint32_t index, std::uint32_t generation) noexcept {
        return EntityId{(generation << kIndexBits) | (index & kIndexMask)};
    }
    constexpr std::uint32_t index() const noexcept { return bits & kIndexMask; }
    constexpr std::uint32_t generation() const noexcept { return bits >> kIndexBits; }
    constexpr bool isNull() const noexcept { return bits == 0; }

    friend constexpr bool operator==(EntityId a, EntityId b) noexcept { return a.bits == b.bits; }
    friend constexpr bool operator!=(EntityId a, EntityId b) noexcept { return a.bits != b.bits; }
    // Ascending EntityId order means ascending index (the canonical iteration order).
    friend constexpr bool operator<(EntityId a, EntityId b) noexcept { return a.index() < b.index(); }
};

class EntityRegistry {
public:
    enum class SlotState : std::uint8_t { Free = 0, Alive = 1, Retired = 2 };

    // New entity, or the null id when every index is in use or retired.
    EntityId create();
    bool alive(EntityId id) const noexcept;
    // Marks for destruction at the next cleanup(); false if the id is not alive. Idempotent.
    bool destroyLater(EntityId id);
    // Destroys everything marked, in ascending index order; returns the destroyed ids (ascending).
    std::vector<EntityId> cleanup();

    std::size_t aliveCount() const noexcept { return aliveCount_; }
    std::size_t slotCount() const noexcept { return generations.size(); }

    // Canonical state (serializer / save-load).
    struct Raw {
        std::vector<std::uint16_t> generations;
        std::vector<SlotState> states;
        std::vector<std::uint32_t> freeList;  // FIFO order, head first
        std::vector<EntityId> pendingDestroy; // ascending
    };
    Raw toRaw() const;
    // Validates and adopts a raw state; false (and unchanged) if inconsistent.
    bool fromRaw(const Raw& raw);

private:
    std::vector<std::uint16_t> generations;
    std::vector<SlotState> states;
    std::vector<std::uint32_t> freeList;
    std::size_t freeHead = 0;
    std::vector<EntityId> pending;  // kept sorted, unique
    std::size_t aliveCount_ = 0;
};

}  // namespace corps
