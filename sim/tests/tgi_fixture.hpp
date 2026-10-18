#pragma once

// Helpers for TGI tests built from synthetic fixtures (no game text in the repo, CLAUDE.md rule 1).
// Fixtures are written as one string literal per line so tools/check_no_float.py, which strips
// single-line literals only, never sees their contents.

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "data/tgi/pipeline.hpp"
#include "data/tgi/resolver.hpp"
#include "vfs/memory_depot.hpp"
#include "vfs/vfs.hpp"

namespace corps::test {

using Files = std::vector<std::pair<std::string, std::string>>;

// A Vfs with one in-memory depot holding `files`.
inline std::unique_ptr<vfs::Vfs> makeVfs(const Files& files, std::string depotName = "mem") {
    auto depot = std::make_unique<vfs::MemoryDepot>(std::move(depotName));
    for (const auto& [path, text] : files) depot->add(path, text);
    auto v = std::make_unique<vfs::Vfs>();
    v->mount(std::move(depot));
    return v;
}

// Loads `tgiFiles` in the given order through a generated index.lst.
struct Loaded {
    std::unique_ptr<vfs::Vfs> vfs;
    std::unique_ptr<tgi::TgiDatabase> db;

    const tgi::Record* record(std::string_view ids) const {
        const auto r = db->findRecords("", ids);
        return r.empty() ? nullptr : r.back();
    }
    tgi::Symbol sym(std::string_view s) const { return db->symbols.find(s); }
    std::size_t count(tgi::DiagCode c) const { return db->diags.count(c); }
};

inline Loaded loadTgi(const Files& tgiFiles, bool trackHistory = false) {
    Files all = tgiFiles;
    std::string index;
    for (const auto& f : tgiFiles) index += f.first + "\n";
    all.emplace_back("index_test.lst", index);
    Loaded out;
    out.vfs = makeVfs(all);
    tgi::TgiOptions options;
    options.resolve.trackHistory = trackHistory;
    out.db = tgi::loadTgiDatabase(*out.vfs, options);
    return out;
}

// Walks "A/B[1]/C" from a node; nullptr if missing.
inline const tgi::ResolvedNode* at(const Loaded& l, const tgi::ResolvedNode& root, std::string_view path) {
    const tgi::ResolvedNode* n = &root;
    std::size_t start = 0;
    while (start < path.size()) {
        std::size_t slash = path.find('/', start);
        if (slash == std::string_view::npos) slash = path.size();
        std::string_view seg = path.substr(start, slash - start);
        std::size_t nth = 0;
        if (const std::size_t br = seg.find('['); br != std::string_view::npos) {
            nth = static_cast<std::size_t>(std::stoul(std::string(seg.substr(br + 1))));
            seg = seg.substr(0, br);
        }
        n = n->child(l.sym(seg), nth);
        if (n == nullptr) return nullptr;
        start = slash + 1;
    }
    return n;
}

// Value of field `key` in `node`, or "<none>" when absent/unset.
inline std::string value(const Loaded& l, const tgi::ResolvedNode* node, std::string_view key) {
    if (node == nullptr) return "<no node>";
    const tgi::ResolvedField* f = node->field(l.sym(key));
    return (f != nullptr && f->hasValue()) ? std::string(f->text()) : std::string("<none>");
}

}  // namespace corps::test
