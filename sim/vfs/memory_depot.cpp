#include "vfs/memory_depot.hpp"

#include <algorithm>

#include "vfs/path.hpp"

namespace corps::vfs {

void MemoryDepot::add(std::string path, std::string bytes) {
    DepotEntry e;
    e.path = normalizePath(path);
    e.key = pathKey(e.path);
    e.size = bytes.size();
    const auto it = std::lower_bound(index.begin(), index.end(), e.key,
                                     [](const DepotEntry& d, const std::string& k) { return d.key < k; });
    const auto pos = static_cast<std::size_t>(it - index.begin());
    if (it != index.end() && it->key == e.key) {
        index[pos] = std::move(e);
        contents[pos] = std::move(bytes);
        return;
    }
    index.insert(it, std::move(e));
    contents.insert(contents.begin() + static_cast<std::ptrdiff_t>(pos), std::move(bytes));
}

bool MemoryDepot::read(std::size_t entryIndex, std::string& out) const {
    if (entryIndex >= contents.size()) {
        out.clear();
        return false;
    }
    out = contents[entryIndex];
    return true;
}

}  // namespace corps::vfs
