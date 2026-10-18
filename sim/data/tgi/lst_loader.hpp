#pragma once

// index*.lst → ordered file list (TGI_FORMAT §6.2–6.3).

#include <cstdint>
#include <string>
#include <vector>

#include "data/tgi/diagnostics.hpp"
#include "vfs/vfs.hpp"

namespace corps::tgi {

struct LoadEntry {
    const vfs::VfsFile* file = nullptr;
    std::string listPath;        // the .lst that named it
    std::uint32_t listLine = 0;  // line of the spec in that list
};

struct LoadOrder {
    std::vector<LoadEntry> files;        // .tgi (and anything else that isn't .lst), in load order
    std::vector<std::string> lists;      // every .lst processed, in processing order
    std::vector<std::string> emptyWildcards;  // "list:line spec" for wildcards that matched nothing (silent)
};

// Sort used inside one wildcard expansion: ordinal on the upper-cased path (NTFS order).
void sortWildcardMatches(std::vector<const vfs::VfsFile*>& matches);

LoadOrder buildLoadOrder(const vfs::Vfs& vfs, Diagnostics& diags);

}  // namespace corps::tgi
