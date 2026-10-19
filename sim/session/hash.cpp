#include "session/bytes.hpp"

// xxHash 0.8.2 (third_party/xxhash, BSD-2). XXH3's output is identical for every SIMD path and
// platform; only speed differs.
#define XXH_INLINE_ALL
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

namespace corps {

std::uint64_t hashBytes(const void* data, std::size_t size) noexcept {
    return static_cast<std::uint64_t>(XXH3_64bits(data, size));
}

std::uint64_t hashBytes(const std::vector<std::uint8_t>& bytes) noexcept {
    return hashBytes(bytes.data(), bytes.size());
}

}  // namespace corps
