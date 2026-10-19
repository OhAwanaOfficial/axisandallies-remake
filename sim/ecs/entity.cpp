#include "ecs/entity.hpp"

#include <algorithm>

namespace corps {

EntityId EntityRegistry::create() {
    std::uint32_t index = 0;
    if (freeHead < freeList.size()) {
        index = freeList[freeHead++];
        if (freeHead == freeList.size()) {  // keep the vector from growing forever
            freeList.clear();
            freeHead = 0;
        }
    } else {
        if (generations.size() > EntityId::kMaxIndex) return EntityId{};
        index = static_cast<std::uint32_t>(generations.size());
        generations.push_back(1);
        states.push_back(SlotState::Free);
    }
    states[index] = SlotState::Alive;
    ++aliveCount_;
    return EntityId::make(index, generations[index]);
}

bool EntityRegistry::alive(EntityId id) const noexcept {
    const std::uint32_t i = id.index();
    return !id.isNull() && i < generations.size() && states[i] == SlotState::Alive && generations[i] == id.generation();
}

bool EntityRegistry::destroyLater(EntityId id) {
    if (!alive(id)) return false;
    const auto it = std::lower_bound(pending.begin(), pending.end(), id);
    if (it == pending.end() || *it != id) pending.insert(it, id);
    return true;
}

std::vector<EntityId> EntityRegistry::cleanup() {
    std::vector<EntityId> destroyed;
    destroyed.swap(pending);
    for (const EntityId id : destroyed) {
        const std::uint32_t i = id.index();
        --aliveCount_;
        if (generations[i] >= EntityId::kMaxGeneration) {
            states[i] = SlotState::Retired;  // never reused: its next generation would wrap
            continue;
        }
        ++generations[i];
        states[i] = SlotState::Free;
        freeList.push_back(i);
    }
    return destroyed;
}

EntityRegistry::Raw EntityRegistry::toRaw() const {
    Raw r;
    r.generations = generations;
    r.states = states;
    r.freeList.assign(freeList.begin() + static_cast<std::ptrdiff_t>(freeHead), freeList.end());
    r.pendingDestroy = pending;
    return r;
}

bool EntityRegistry::fromRaw(const Raw& raw) {
    if (raw.generations.size() != raw.states.size() || raw.generations.size() > std::size_t{EntityId::kMaxIndex} + 1) return false;
    std::size_t alive = 0;
    std::vector<std::uint8_t> inFree(raw.generations.size(), 0);
    for (std::size_t i = 0; i < raw.states.size(); ++i) {
        if (raw.generations[i] == 0 || raw.generations[i] > EntityId::kMaxGeneration) return false;
        if (raw.states[i] == SlotState::Alive) ++alive;
        else if (raw.states[i] != SlotState::Free && raw.states[i] != SlotState::Retired) return false;
    }
    for (const std::uint32_t i : raw.freeList) {
        if (i >= raw.states.size() || raw.states[i] != SlotState::Free || inFree[i]) return false;
        inFree[i] = 1;
    }
    for (std::size_t i = 0; i < raw.states.size(); ++i) {
        if (raw.states[i] == SlotState::Free && !inFree[i]) return false;
    }
    for (std::size_t k = 0; k < raw.pendingDestroy.size(); ++k) {
        const EntityId id = raw.pendingDestroy[k];
        const std::uint32_t i = id.index();
        if (i >= raw.states.size() || raw.states[i] != SlotState::Alive || raw.generations[i] != id.generation()) return false;
        if (k > 0 && !(raw.pendingDestroy[k - 1] < id)) return false;
    }
    generations = raw.generations;
    states = raw.states;
    freeList = raw.freeList;
    freeHead = 0;
    pending = raw.pendingDestroy;
    aliveCount_ = alive;
    return true;
}

}  // namespace corps
