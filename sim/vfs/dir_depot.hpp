#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "vfs/depot.hpp"

namespace corps::vfs {

// A plain folder, indexed recursively once at mount time.
class DirDepot final : public Depot {
public:
    // Returns nullptr (with `error` set) if `root` is not a readable directory.
    static std::unique_ptr<DirDepot> open(const std::filesystem::path& root, std::string displayName,
                                          std::string& error);

    const std::string& name() const override { return displayName; }
    const std::vector<DepotEntry>& entries() const override { return index; }
    bool read(std::size_t entryIndex, std::string& out) const override;

private:
    DirDepot() = default;

    std::string displayName;
    std::filesystem::path root;
    std::vector<DepotEntry> index;
};

// Reads a whole file from disk. Returns false on error.
bool readFileBytes(const std::filesystem::path& file, std::string& out);

}  // namespace corps::vfs
