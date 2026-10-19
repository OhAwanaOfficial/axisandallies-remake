#pragma once

// Little-endian, fixed-width byte writer/reader for canonical state, replays and the def hash.
// Fixed is written as its raw int64; strings as u32 length + bytes.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ecs/entity.hpp"
#include "math/angle.hpp"
#include "math/fixed.hpp"
#include "math/fvec2.hpp"

namespace corps {

class ByteWriter {
public:
    std::vector<std::uint8_t> buf;

    void u8(std::uint8_t v) { buf.push_back(v); }
    void u16(std::uint16_t v) { le(v, 2); }
    void u32(std::uint32_t v) { le(v, 4); }
    void u64(std::uint64_t v) { le(v, 8); }
    void i64(std::int64_t v) { le(static_cast<std::uint64_t>(v), 8); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void fixed(Fixed v) { i64(v.raw); }
    void vec2(FVec2 v) { fixed(v.x); fixed(v.y); }
    void angle(Angle a) { u16(a.units); }
    void entity(EntityId id) { u32(id.bits); }
    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    }
    void tag(std::string_view t) { buf.insert(buf.end(), t.begin(), t.begin() + 4); }  // 4 chars
    void bytes(const std::vector<std::uint8_t>& b) {
        u32(static_cast<std::uint32_t>(b.size()));
        buf.insert(buf.end(), b.begin(), b.end());
    }

private:
    void le(std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) buf.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
};

// Bounds-checked reader: any short read sets ok=false and returns zeros.
class ByteReader {
public:
    explicit ByteReader(const std::vector<std::uint8_t>& b, std::size_t start = 0) : data(b), pos(start) {}

    bool ok = true;

    std::uint8_t u8() { return static_cast<std::uint8_t>(le(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(le(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(le(4)); }
    std::uint64_t u64() { return le(8); }
    std::int64_t i64() { return static_cast<std::int64_t>(le(8)); }
    bool boolean() { return u8() != 0; }
    Fixed fixed() { return Fixed::fromRaw(i64()); }
    FVec2 vec2() {
        const Fixed x = fixed();
        return {x, fixed()};
    }
    Angle angle() { return Angle::fromUnits(u16()); }
    EntityId entity() { return EntityId{u32()}; }
    std::string str() {
        const std::uint32_t n = u32();
        if (!need(n)) return {};
        std::string s(data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        return s;
    }
    bool expectTag(std::string_view t) {  // 4 chars
        if (!need(4)) return false;
        for (std::size_t i = 0; i < 4; ++i) {
            if (data[pos + i] != static_cast<std::uint8_t>(t[i])) {
                ok = false;
                return false;
            }
        }
        pos += 4;
        return true;
    }
    std::vector<std::uint8_t> bytes() {
        const std::uint32_t n = u32();
        if (!need(n)) return {};
        std::vector<std::uint8_t> b(data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        return b;
    }
    // A count that can't possibly fit in the remaining bytes is malformed input.
    std::uint32_t count(std::size_t minBytesEach) {
        const std::uint32_t n = u32();
        if (minBytesEach != 0 && n > remaining() / minBytesEach) ok = false;
        return ok ? n : 0;
    }
    std::size_t remaining() const { return data.size() - pos; }
    std::size_t position() const { return pos; }

private:
    bool need(std::size_t n) {
        if (!ok || data.size() - pos < n) {
            ok = false;
            return false;
        }
        return true;
    }
    std::uint64_t le(int n) {
        if (!need(static_cast<std::size_t>(n))) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(data[pos + static_cast<std::size_t>(i)]) << (8 * i);
        pos += static_cast<std::size_t>(n);
        return v;
    }

    const std::vector<std::uint8_t>& data;
    std::size_t pos = 0;
};

// xxh3_64 of a byte buffer (the state hash, NETCODE §5).
std::uint64_t hashBytes(const std::vector<std::uint8_t>& bytes) noexcept;
std::uint64_t hashBytes(const void* data, std::size_t size) noexcept;

}  // namespace corps
