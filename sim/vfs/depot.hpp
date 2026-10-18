#pragma once

// A depot is one mounted source of files: an archive (RwdArchive) or a folder (DirDepot).

#include <cstdint>
#include <string>
#include <vector>

namespace corps::vfs {

struct DepotEntry {
    std::string path;  // normalised, original case ("Templates/template_thing.tgi")
    std::string key;   // pathKey(path): lower-case lookup key
    std::uint64_t size = 0;
};

class Depot {
public:
    virtual ~Depot() = default;

    // Short display name, e.g. "Data.rwd" or "data/".
    virtual const std::string& name() const = 0;

    // Sorted by key, keys unique.
    virtual const std::vector<DepotEntry>& entries() const = 0;

    // Reads the whole file. Returns false (and leaves `out` empty) on I/O error.
    virtual bool read(std::size_t entryIndex, std::string& out) const = 0;
};

}  // namespace corps::vfs
