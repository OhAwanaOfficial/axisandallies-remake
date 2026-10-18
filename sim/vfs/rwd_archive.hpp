#pragma once

// RWD archive reader, TGCK format (ASSET_PIPELINE §2). Files are stored uncompressed.
//
//   offset 0        char[4] "TGCK"
//   offset EOF-128  int32 dataOffset
//   offset EOF-32   int32 tocOffset
//   TOC from tocOffset until an entry stops looking valid (fileType 0 is NOT a terminator):
//     int32 fileType, int16 nameLength (UTF-16 units, 1..400), wchar name[nameLength] (UTF-16LE),
//     int64 offset (+ dataOffset = absolute), int64 size, int32 unknown

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "vfs/depot.hpp"

namespace corps::vfs {

struct RwdTocEntry {
    std::string name;  // as stored (UTF-8, original separators)
    std::int32_t fileType = 0;
    std::uint64_t offset = 0;  // absolute
    std::uint64_t size = 0;
    std::int32_t unknown = 0;
};

class RwdArchive final : public Depot {
public:
    static std::unique_ptr<RwdArchive> open(const std::filesystem::path& file, std::string displayName,
                                            std::string& error);
    // For tests: an archive image held in memory.
    static std::unique_ptr<RwdArchive> openMemory(std::string bytes, std::string displayName,
                                                  std::string& error);

    const std::string& name() const override { return displayName; }
    const std::vector<DepotEntry>& entries() const override { return index; }
    bool read(std::size_t entryIndex, std::string& out) const override;

    // Raw table of contents in archive order (for `rwd list`-style tools).
    const std::vector<RwdTocEntry>& toc() const { return tocEntries; }
    // TOC entries dropped from entries() because an earlier entry had the same key.
    std::size_t duplicateCount() const { return duplicates; }

private:
    RwdArchive() = default;
    bool parse(std::string& error);
    bool readAt(std::uint64_t offset, std::uint64_t size, std::string& out) const;

    std::string displayName;
    bool inMemory = false;
    std::string memory;
    mutable std::ifstream stream;
    std::uint64_t fileSize = 0;

    std::vector<RwdTocEntry> tocEntries;
    std::vector<DepotEntry> index;
    std::vector<std::size_t> indexToToc;
    std::size_t duplicates = 0;
};

// Builds a TGCK image from (name, bytes) pairs. Used by tests to make synthetic archives.
std::string buildRwdImage(const std::vector<std::pair<std::string, std::string>>& files);

}  // namespace corps::vfs
