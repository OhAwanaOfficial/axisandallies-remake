#pragma once

// Depot stack (ARCHITECTURE §4.1, TGI_FORMAT §6.1). Later mounts win: a later depot replaces
// the whole file at a path; contents never merge. Paths compare ASCII case-insensitively.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "vfs/depot.hpp"

namespace corps::vfs {

struct VfsFile {
    std::string path;          // spelling from the winning depot
    std::string key;           // lower-case lookup key
    std::uint32_t depot = 0;   // index into Vfs::depot()
    std::size_t entry = 0;     // index into that depot's entries()
    std::uint32_t shadowed = 0;  // how many earlier depots also had this path
};

class Vfs {
public:
    void mount(std::unique_ptr<Depot> depot);

    std::size_t depotCount() const { return depots.size(); }
    const Depot& depot(std::size_t i) const { return *depots[i]; }

    // Every visible file, sorted by key.
    const std::vector<VfsFile>& files() const { return merged; }

    const VfsFile* find(std::string_view path) const;
    bool read(const VfsFile& file, std::string& out) const;
    bool read(std::string_view path, std::string& out) const;

    // '*' wildcards in the last component only, not recursive, case-insensitive. The match set is
    // the union across depots (each path once). Result is sorted by key; callers that need the
    // engine's order re-sort (see LstLoader).
    std::vector<const VfsFile*> glob(std::string_view pattern) const;

private:
    void rebuild();

    std::vector<std::unique_ptr<Depot>> depots;
    std::vector<VfsFile> merged;
};

// Mounts the original install in autoexec order (CLAUDE.md, TGI_FORMAT §6.1):
// Data.rwd → data/ → skins/*.rwd → Music.rwd → Speech.rwd → each of `userOverlays` that exists.
// Missing optional pieces are skipped; returns false only if nothing at all could be mounted.
// Human-readable notes about what was (not) mounted are appended to `log`.
bool mountGameInstall(Vfs& vfs, const std::filesystem::path& gameDir,
                      const std::vector<std::filesystem::path>& userOverlays, std::vector<std::string>& log);

}  // namespace corps::vfs
