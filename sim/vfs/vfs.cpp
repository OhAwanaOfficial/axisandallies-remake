#include "vfs/vfs.hpp"

#include <algorithm>
#include <system_error>

#include "core/ascii.hpp"
#include "vfs/dir_depot.hpp"
#include "vfs/path.hpp"
#include "vfs/rwd_archive.hpp"

namespace corps::vfs {

void Vfs::mount(std::unique_ptr<Depot> depot) {
    depots.push_back(std::move(depot));
    rebuild();
}

void Vfs::rebuild() {
    // Merge the sorted per-depot indexes; the latest depot holding a key wins.
    std::vector<VfsFile> all;
    for (std::size_t d = 0; d < depots.size(); ++d) {
        const auto& entries = depots[d]->entries();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            VfsFile f;
            f.path = entries[i].path;
            f.key = entries[i].key;
            f.depot = static_cast<std::uint32_t>(d);
            f.entry = i;
            all.push_back(std::move(f));
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const VfsFile& a, const VfsFile& b) { return a.key < b.key; });
    merged.clear();
    for (auto& f : all) {
        if (!merged.empty() && merged.back().key == f.key) {
            const std::uint32_t shadowed = merged.back().shadowed + 1;
            merged.back() = std::move(f);
            merged.back().shadowed = shadowed;
        } else {
            merged.push_back(std::move(f));
        }
    }
}

const VfsFile* Vfs::find(std::string_view path) const {
    const std::string key = pathKey(path);
    const auto it = std::lower_bound(merged.begin(), merged.end(), key,
                                     [](const VfsFile& f, const std::string& k) { return f.key < k; });
    return (it != merged.end() && it->key == key) ? &*it : nullptr;
}

bool Vfs::read(const VfsFile& file, std::string& out) const {
    return depots[file.depot]->read(file.entry, out);
}

bool Vfs::read(std::string_view path, std::string& out) const {
    const VfsFile* f = find(path);
    if (f == nullptr) {
        out.clear();
        return false;
    }
    return read(*f, out);
}

std::vector<const VfsFile*> Vfs::glob(std::string_view pattern) const {
    const std::string norm = pathKey(pattern);
    const std::string dir(parentDir(norm));
    const std::string_view namePattern = fileName(norm);
    std::vector<const VfsFile*> out;
    if (namePattern.find('*') == std::string_view::npos) {
        if (const VfsFile* f = find(norm)) out.push_back(f);
        return out;
    }
    const std::string prefix = dir.empty() ? std::string() : dir + "/";
    auto it = std::lower_bound(merged.begin(), merged.end(), prefix,
                               [](const VfsFile& f, const std::string& k) { return f.key < k; });
    for (; it != merged.end() && it->key.compare(0, prefix.size(), prefix) == 0; ++it) {
        const std::string_view rest = std::string_view(it->key).substr(prefix.size());
        if (rest.find('/') != std::string_view::npos) continue;  // not recursive
        if (globMatch(namePattern, rest)) out.push_back(&*it);
    }
    return out;
}

bool mountGameInstall(Vfs& vfs, const std::filesystem::path& gameDir,
                      const std::vector<std::filesystem::path>& userOverlays, std::vector<std::string>& log) {
    std::error_code ec;
    std::size_t mounted = 0;
    const auto mountRwd = [&](const std::filesystem::path& file, const std::string& display) {
        if (!std::filesystem::is_regular_file(file, ec)) {
            log.push_back("skip " + display + " (not found)");
            return;
        }
        std::string error;
        if (auto a = RwdArchive::open(file, display, error)) {
            log.push_back("mount " + display + " (" + std::to_string(a->entries().size()) + " files)");
            vfs.mount(std::move(a));
            ++mounted;
        } else {
            log.push_back("skip " + display + ": " + error);
        }
    };
    const auto mountDir = [&](const std::filesystem::path& dir, const std::string& display) {
        if (!std::filesystem::is_directory(dir, ec)) {
            log.push_back("skip " + display + " (not found)");
            return;
        }
        std::string error;
        if (auto d = DirDepot::open(dir, display, error)) {
            log.push_back("mount " + display + " (" + std::to_string(d->entries().size()) + " files)");
            vfs.mount(std::move(d));
            ++mounted;
        } else {
            log.push_back("skip " + display + ": " + error);
        }
    };

    mountRwd(gameDir / "Data.rwd", "Data.rwd");
    mountDir(gameDir / "data", "data/");

    std::vector<std::filesystem::path> skins;
    if (std::filesystem::is_directory(gameDir / "skins", ec)) {
        for (std::filesystem::directory_iterator it(gameDir / "skins", ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fileEc;
            if (it->is_regular_file(fileEc) && iendsWith(pathToUtf8(it->path().filename()), ".rwd")) {
                skins.push_back(it->path());
            }
        }
    }
    std::sort(skins.begin(), skins.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return upperOrdinalLess(pathToUtf8(a.filename()), pathToUtf8(b.filename()));
    });
    for (const auto& s : skins) mountRwd(s, "skins/" + pathToUtf8(s.filename()));

    mountRwd(gameDir / "Music.rwd", "Music.rwd");
    mountRwd(gameDir / "Speech.rwd", "Speech.rwd");
    for (const auto& overlay : userOverlays) mountDir(overlay, pathToUtf8(overlay) + "/");
    return mounted > 0;
}

}  // namespace corps::vfs
