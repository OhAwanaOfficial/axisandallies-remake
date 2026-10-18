#include "vfs/dir_depot.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "vfs/path.hpp"

namespace corps::vfs {

bool readFileBytes(const std::filesystem::path& file, std::string& out) {
    out.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(out.data(), size)) {
        out.clear();
        return false;
    }
    return true;
}

std::unique_ptr<DirDepot> DirDepot::open(const std::filesystem::path& root, std::string displayName,
                                         std::string& error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        error = "not a directory: " + pathToUtf8(root);
        return nullptr;
    }
    std::unique_ptr<DirDepot> depot(new DirDepot());
    depot->displayName = std::move(displayName);
    depot->root = root;

    // Enumeration order is OS-defined, so everything is sorted afterwards.
    const auto opts = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(root, opts, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) continue;
        const std::filesystem::path rel = it->path().lexically_relative(root);
        DepotEntry entry;
        entry.path = normalizePath(pathToUtf8(rel));
        entry.key = pathKey(entry.path);
        entry.size = static_cast<std::uint64_t>(it->file_size(fileEc));
        depot->index.push_back(std::move(entry));
    }
    if (ec) {
        error = "cannot enumerate " + pathToUtf8(root) + ": " + ec.message();
        return nullptr;
    }
    std::sort(depot->index.begin(), depot->index.end(), [](const DepotEntry& a, const DepotEntry& b) {
        return a.key != b.key ? a.key < b.key : a.path < b.path;
    });
    // A case-sensitive file system can hold two spellings of one key; keep the first.
    depot->index.erase(std::unique(depot->index.begin(), depot->index.end(),
                                   [](const DepotEntry& a, const DepotEntry& b) { return a.key == b.key; }),
                       depot->index.end());
    return depot;
}

bool DirDepot::read(std::size_t entryIndex, std::string& out) const {
    if (entryIndex >= index.size()) {
        out.clear();
        return false;
    }
    return readFileBytes(root / pathFromUtf8(index[entryIndex].path), out);
}

}  // namespace corps::vfs
