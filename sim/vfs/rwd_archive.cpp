#include "vfs/rwd_archive.hpp"

#include <algorithm>
#include <cstring>
#include <numeric>

#include "vfs/path.hpp"

namespace corps::vfs {
namespace {

constexpr std::uint64_t kTrailerSize = 288;
constexpr std::uint64_t kDataOffsetFromEnd = 128;
constexpr std::uint64_t kTocOffsetFromEnd = 32;
constexpr std::uint64_t kMinEntryBytes = 28;  // 6-byte header + one UTF-16 unit + 20-byte tail
constexpr int kMaxNameLength = 400;

std::int32_t readI32(const unsigned char* p) {
    const std::uint32_t v = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                            (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    return static_cast<std::int32_t>(v);
}

std::int16_t readI16(const unsigned char* p) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0] | (p[1] << 8)));
}

std::int64_t readI64(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return static_cast<std::int64_t>(v);
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// UTF-16LE → UTF-8. Returns false if a code unit is one of .NET's GetInvalidFileNameChars()
// other than the separators: those end the TOC in the reference extractor.
bool utf16NameToUtf8(const unsigned char* p, int units, std::string& out) {
    out.clear();
    for (int i = 0; i < units; ++i) {
        std::uint32_t cu = static_cast<std::uint16_t>(p[2 * i] | (p[2 * i + 1] << 8));
        if (cu < 32 || cu == '"' || cu == '<' || cu == '>' || cu == '|' || cu == ':' || cu == '*' || cu == '?') {
            return false;
        }
        if (cu >= 0xD800 && cu <= 0xDBFF && i + 1 < units) {
            const std::uint32_t lo = static_cast<std::uint16_t>(p[2 * i + 2] | (p[2 * i + 3] << 8));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cu = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            } else {
                cu = 0xFFFD;
            }
        } else if (cu >= 0xD800 && cu <= 0xDFFF) {
            cu = 0xFFFD;
        }
        appendUtf8(out, cu);
    }
    return true;
}

void putI32(std::string& out, std::int32_t v) {
    const auto u = static_cast<std::uint32_t>(v);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
}

void putI64(std::string& out, std::int64_t v) {
    const auto u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
}

}  // namespace

std::unique_ptr<RwdArchive> RwdArchive::open(const std::filesystem::path& file, std::string displayName,
                                             std::string& error) {
    std::unique_ptr<RwdArchive> archive(new RwdArchive());
    archive->displayName = std::move(displayName);
    archive->stream.open(file, std::ios::binary);
    if (!archive->stream) {
        error = "cannot open " + pathToUtf8(file);
        return nullptr;
    }
    archive->stream.seekg(0, std::ios::end);
    const std::streamoff size = archive->stream.tellg();
    if (size < 0) {
        error = "cannot size " + pathToUtf8(file);
        return nullptr;
    }
    archive->fileSize = static_cast<std::uint64_t>(size);
    if (!archive->parse(error)) {
        error = pathToUtf8(file) + ": " + error;
        return nullptr;
    }
    return archive;
}

std::unique_ptr<RwdArchive> RwdArchive::openMemory(std::string bytes, std::string displayName,
                                                   std::string& error) {
    std::unique_ptr<RwdArchive> archive(new RwdArchive());
    archive->displayName = std::move(displayName);
    archive->inMemory = true;
    archive->memory = std::move(bytes);
    archive->fileSize = archive->memory.size();
    if (!archive->parse(error)) return nullptr;
    return archive;
}

bool RwdArchive::readAt(std::uint64_t offset, std::uint64_t size, std::string& out) const {
    out.clear();
    if (offset > fileSize || size > fileSize - offset) return false;
    if (inMemory) {
        out.assign(memory, static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
        return true;
    }
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0 && !stream.read(out.data(), static_cast<std::streamsize>(size))) {
        out.clear();
        return false;
    }
    return true;
}

bool RwdArchive::parse(std::string& error) {
    if (fileSize < kTrailerSize + 4) {
        error = "file too small for a TGCK archive";
        return false;
    }
    std::string head;
    if (!readAt(0, 4, head) || head != "TGCK") {
        error = "missing TGCK signature";
        return false;
    }
    std::string word;
    if (!readAt(fileSize - kDataOffsetFromEnd, 4, word)) {
        error = "cannot read data offset";
        return false;
    }
    const std::int32_t dataOffset = readI32(reinterpret_cast<const unsigned char*>(word.data()));
    if (!readAt(fileSize - kTocOffsetFromEnd, 4, word)) {
        error = "cannot read TOC offset";
        return false;
    }
    const std::int32_t tocOffset = readI32(reinterpret_cast<const unsigned char*>(word.data()));
    const std::uint64_t tocEnd = fileSize - kTrailerSize;
    if (tocOffset < 0 || static_cast<std::uint64_t>(tocOffset) > tocEnd) {
        error = "TOC offset out of range";
        return false;
    }

    std::string toc;
    if (!readAt(static_cast<std::uint64_t>(tocOffset), fileSize - static_cast<std::uint64_t>(tocOffset), toc)) {
        error = "cannot read TOC";
        return false;
    }
    const auto* base = reinterpret_cast<const unsigned char*>(toc.data());
    std::uint64_t pos = static_cast<std::uint64_t>(tocOffset);
    const auto at = [&](std::uint64_t absolute) { return base + (absolute - static_cast<std::uint64_t>(tocOffset)); };
    const auto has = [&](std::uint64_t absolute, std::uint64_t n) { return absolute + n <= fileSize; };
    const auto fileLen = static_cast<std::int64_t>(fileSize);

    while (pos + kMinEntryBytes <= tocEnd) {
        const std::int32_t fileType = readI32(at(pos));
        const std::int16_t nameLength = readI16(at(pos + 4));
        pos += 6;
        if (nameLength <= 0 || nameLength > kMaxNameLength) break;
        const auto nameBytes = static_cast<std::uint64_t>(nameLength) * 2;
        if (pos + nameBytes + 20 > tocEnd || !has(pos, nameBytes + 20)) break;
        RwdTocEntry entry;
        const bool nameOk = utf16NameToUtf8(at(pos), nameLength, entry.name);
        pos += nameBytes;
        const std::int64_t offset = readI64(at(pos)) + dataOffset;
        const std::int64_t size = readI64(at(pos + 8));
        entry.unknown = readI32(at(pos + 16));
        pos += 20;
        if (!nameOk || size < 0 || size > fileLen || offset < 0 || offset > fileLen) break;
        entry.fileType = fileType;
        entry.offset = static_cast<std::uint64_t>(offset);
        entry.size = static_cast<std::uint64_t>(size);
        tocEntries.push_back(std::move(entry));
    }

    std::vector<std::size_t> order(tocEntries.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::vector<std::string> keys(tocEntries.size());
    for (std::size_t i = 0; i < tocEntries.size(); ++i) keys[i] = pathKey(tocEntries[i].name);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });
    for (std::size_t i : order) {
        if (!index.empty() && index.back().key == keys[i]) {
            ++duplicates;  // first TOC entry wins
            continue;
        }
        DepotEntry e;
        e.path = normalizePath(tocEntries[i].name);
        e.key = keys[i];
        e.size = tocEntries[i].size;
        index.push_back(std::move(e));
        indexToToc.push_back(i);
    }
    return true;
}

bool RwdArchive::read(std::size_t entryIndex, std::string& out) const {
    if (entryIndex >= index.size()) {
        out.clear();
        return false;
    }
    const RwdTocEntry& e = tocEntries[indexToToc[entryIndex]];
    return readAt(e.offset, e.size, out);
}

std::string buildRwdImage(const std::vector<std::pair<std::string, std::string>>& files) {
    constexpr std::int32_t kDataOffset = 30;  // what Data.rwd uses
    std::string out = "TGCK";
    out.resize(static_cast<std::size_t>(kDataOffset), '\0');
    std::vector<std::int64_t> offsets;
    for (const auto& f : files) {
        offsets.push_back(static_cast<std::int64_t>(out.size()) - kDataOffset);
        out += f.second;
    }
    const auto tocOffset = static_cast<std::int32_t>(out.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        putI32(out, static_cast<std::int32_t>(i + 1));
        const std::string& name = files[i].first;  // test names are ASCII
        out.push_back(static_cast<char>(name.size() & 0xFF));
        out.push_back(static_cast<char>((name.size() >> 8) & 0xFF));
        for (char c : name) {
            out.push_back(c);
            out.push_back('\0');
        }
        putI64(out, offsets[i]);
        putI64(out, static_cast<std::int64_t>(files[i].second.size()));
        putI32(out, 0);
    }
    out.append(64, '\0');  // nameLength 0 ends the table
    std::string trailer(kTrailerSize, '\0');
    std::string word;
    putI32(word, kDataOffset);
    trailer.replace(kTrailerSize - kDataOffsetFromEnd, 4, word);
    word.clear();
    putI32(word, tocOffset);
    trailer.replace(kTrailerSize - kTocOffsetFromEnd, 4, word);
    out += trailer;
    return out;
}

}  // namespace corps::vfs
