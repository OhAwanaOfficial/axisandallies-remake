#pragma once

#include <string>
#include <utility>
#include <vector>

#include "vfs/depot.hpp"

namespace corps::vfs {

// Files held in memory. Used by tests (synthetic fixtures) and tools; a later add() of the same
// path replaces the earlier one.
class MemoryDepot final : public Depot {
public:
    explicit MemoryDepot(std::string displayName) : displayName(std::move(displayName)) {}

    void add(std::string path, std::string bytes);

    const std::string& name() const override { return displayName; }
    const std::vector<DepotEntry>& entries() const override { return index; }
    bool read(std::size_t entryIndex, std::string& out) const override;

private:
    std::string displayName;
    std::vector<DepotEntry> index;
    std::vector<std::string> contents;  // parallel to index
};

}  // namespace corps::vfs
