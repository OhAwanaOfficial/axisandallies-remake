#pragma once

// Exact integer square roots (floor) and the Fixed square root built on them.

#include <cstdint>

#include "math/fixed.hpp"
#include "math/int128.hpp"

namespace corps {

// floor(sqrt(v)), bit by bit (no floating point, identical everywhere).
constexpr std::uint64_t isqrt64(std::uint64_t v) noexcept {
    std::uint64_t res = 0;
    std::uint64_t bit = std::uint64_t{1} << 62;
    while (bit > v) bit >>= 2;
    while (bit != 0) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

// floor(sqrt(v)) for a 128-bit value; the result fits in 64 bits.
constexpr std::uint64_t isqrt128(i128::U128 v) noexcept {
    if (v.hi == 0) return isqrt64(v.lo);
    i128::U128 res{};
    i128::U128 bit{std::uint64_t{1} << 62, 0};  // 2^126
    while (v < bit) bit = i128::shr(bit, 2);
    while (!(bit == i128::U128{})) {
        const i128::U128 rb = i128::add(res, bit);
        if (rb <= v) {
            v = i128::sub(v, rb);
            res = i128::add(i128::shr(res, 1), bit);
        } else {
            res = i128::shr(res, 1);
        }
        bit = i128::shr(bit, 2);
    }
    return res.lo;
}

// sqrt of a Fixed value, floor of the exact result (to the 2^-32 grid). Negative input → 0 and
// counts as an overflow event (a domain error).
inline Fixed sqrt(Fixed v) noexcept {
    if (v.raw < 0) {
        ++fixedOverflowCounter();
        return Fixed::fromRaw(0);
    }
    // sqrt(raw / 2^32) * 2^32 = sqrt(raw * 2^32)
    return Fixed::fromRaw(static_cast<std::int64_t>(isqrt128(i128::shl({0, static_cast<std::uint64_t>(v.raw)}, 32))));
}

}  // namespace corps
