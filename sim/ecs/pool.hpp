#pragma once

// Component pool: a sparse set whose dense arrays stay sorted by entity index, so a plain walk is
// always ascending EntityId (the canonical, deterministic iteration order; ARCHITECTURE §6).
// Insertion keeps the order with a binary search; at our entity counts (~2,000) that is cheap.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "ecs/entity.hpp"

namespace corps {

template <class T>
class Pool {
public:
    // Adds or replaces the component of `id`.
    T& set(EntityId id, const T& value) {
        const std::uint32_t i = id.index();
        if (i < sparse.size() && sparse[i] != 0) {
            const std::size_t pos = sparse[i] - 1;
            ids_[pos] = id;
            data_[pos] = value;
            return data_[pos];
        }
        if (sparse.size() <= i) sparse.resize(std::size_t{i} + 1, 0);
        const auto it = std::lower_bound(ids_.begin(), ids_.end(), id);
        const auto pos = static_cast<std::size_t>(it - ids_.begin());
        ids_.insert(it, id);
        data_.insert(data_.begin() + static_cast<std::ptrdiff_t>(pos), value);
        for (std::size_t k = pos; k < ids_.size(); ++k) sparse[ids_[k].index()] = static_cast<std::uint32_t>(k + 1);
        return data_[pos];
    }

    // Removes the component of the entity at `index` (any generation). false if absent.
    bool removeIndex(std::uint32_t index) {
        if (index >= sparse.size() || sparse[index] == 0) return false;
        const std::size_t pos = sparse[index] - 1;
        sparse[index] = 0;
        ids_.erase(ids_.begin() + static_cast<std::ptrdiff_t>(pos));
        data_.erase(data_.begin() + static_cast<std::ptrdiff_t>(pos));
        for (std::size_t k = pos; k < ids_.size(); ++k) sparse[ids_[k].index()] = static_cast<std::uint32_t>(k + 1);
        return true;
    }

    // The component of exactly this id (generation must match), or nullptr.
    T* get(EntityId id) noexcept {
        const std::uint32_t i = id.index();
        if (i >= sparse.size() || sparse[i] == 0) return nullptr;
        const std::size_t pos = sparse[i] - 1;
        return ids_[pos] == id ? &data_[pos] : nullptr;
    }
    const T* get(EntityId id) const noexcept { return const_cast<Pool*>(this)->get(id); }
    bool has(EntityId id) const noexcept { return get(id) != nullptr; }

    std::size_t size() const noexcept { return ids_.size(); }
    // Parallel arrays in ascending EntityId order.
    const std::vector<EntityId>& ids() const noexcept { return ids_; }
    std::vector<T>& data() noexcept { return data_; }
    const std::vector<T>& data() const noexcept { return data_; }

    void clear() {
        ids_.clear();
        data_.clear();
        sparse.clear();
    }

private:
    std::vector<EntityId> ids_;
    std::vector<T> data_;
    std::vector<std::uint32_t> sparse;  // entity index → dense position + 1 (0 = absent)
};

}  // namespace corps
