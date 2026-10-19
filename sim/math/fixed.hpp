#pragma once

// Q32.32 fixed-point number stored in int64 (ARCHITECTURE §4.3). Range ±2^31, resolution 2^-32.
//
// Determinism rules:
//   * No signed-overflow UB: + and - go through uint64 and saturate.
//   * * and / use 128-bit intermediates (math/int128.hpp), then saturate to [min, max].
//   * Rounding: multiply rounds half up (floor(p + 1/2)); divide rounds to nearest, ties away from 0.
//   * Division by zero: +max for x > 0, min for x < 0, 0 for 0/0.
//   Every saturation or division by zero bumps fixedOverflowCount() (diagnostics only; the count is
//   never read by sim logic, so it can't affect results).

#include <cstdint>
#include <limits>
#include <string>

#include "math/int128.hpp"

namespace corps {

std::uint64_t& fixedOverflowCounter() noexcept;
inline std::uint64_t fixedOverflowCount() noexcept { return fixedOverflowCounter(); }

struct Fixed {
    static constexpr int kFracBits = 32;
    static constexpr std::int64_t kOne = std::int64_t{1} << kFracBits;
    static constexpr std::int64_t kMaxRaw = std::numeric_limits<std::int64_t>::max();
    static constexpr std::int64_t kMinRaw = std::numeric_limits<std::int64_t>::min();

    std::int64_t raw = 0;

    static constexpr Fixed fromRaw(std::int64_t r) noexcept { return Fixed{r}; }
    static constexpr Fixed fromInt(std::int32_t v) noexcept { return Fixed{static_cast<std::int64_t>(v) * kOne}; }
    static constexpr Fixed max() noexcept { return Fixed{kMaxRaw}; }
    static constexpr Fixed min() noexcept { return Fixed{kMinRaw}; }
    static constexpr Fixed epsilon() noexcept { return Fixed{1}; }
    // num / den, rounded to nearest (den == 0 follows the division-by-zero rule).
    static Fixed fromRatio(std::int64_t num, std::int64_t den) noexcept;

    // Conversions to integers (all exact for every value, no overflow).
    constexpr std::int64_t floorInt() const noexcept { return raw >> kFracBits; }  // arithmetic shift (C++20)
    constexpr std::uint64_t fracRaw() const noexcept { return static_cast<std::uint64_t>(raw) & 0xFFFFFFFFu; }
    constexpr std::int64_t ceilInt() const noexcept { return floorInt() + (fracRaw() != 0 ? 1 : 0); }
    constexpr std::int64_t roundInt() const noexcept { return floorInt() + (fracRaw() >= 0x80000000u ? 1 : 0); }  // half up
    constexpr std::int64_t truncInt() const noexcept { return raw < 0 ? ceilInt() : floorInt(); }
    constexpr Fixed frac() const noexcept { return Fixed{static_cast<std::int64_t>(fracRaw())}; }  // v - floor(v), >= 0

    friend constexpr bool operator==(Fixed a, Fixed b) noexcept { return a.raw == b.raw; }
    friend constexpr bool operator!=(Fixed a, Fixed b) noexcept { return a.raw != b.raw; }
    friend constexpr bool operator<(Fixed a, Fixed b) noexcept { return a.raw < b.raw; }
    friend constexpr bool operator>(Fixed a, Fixed b) noexcept { return a.raw > b.raw; }
    friend constexpr bool operator<=(Fixed a, Fixed b) noexcept { return a.raw <= b.raw; }
    friend constexpr bool operator>=(Fixed a, Fixed b) noexcept { return a.raw >= b.raw; }

    friend Fixed operator+(Fixed a, Fixed b) noexcept;
    friend Fixed operator-(Fixed a, Fixed b) noexcept;
    friend Fixed operator-(Fixed a) noexcept;
    friend Fixed operator*(Fixed a, Fixed b) noexcept;
    friend Fixed operator/(Fixed a, Fixed b) noexcept;
    Fixed& operator+=(Fixed b) noexcept { return *this = *this + b; }
    Fixed& operator-=(Fixed b) noexcept { return *this = *this - b; }
    Fixed& operator*=(Fixed b) noexcept { return *this = *this * b; }
    Fixed& operator/=(Fixed b) noexcept { return *this = *this / b; }

    // Integer scaling without the Q32.32 multiply (exact, saturating).
    friend Fixed operator*(Fixed a, std::int32_t k) noexcept;
    friend Fixed operator*(std::int32_t k, Fixed a) noexcept { return a * k; }
};

Fixed abs(Fixed v) noexcept;  // abs(min) saturates to max
constexpr Fixed min(Fixed a, Fixed b) noexcept { return b < a ? b : a; }
constexpr Fixed max(Fixed a, Fixed b) noexcept { return a < b ? b : a; }
constexpr Fixed clamp(Fixed v, Fixed lo, Fixed hi) noexcept { return v < lo ? lo : (hi < v ? hi : v); }

// Rounds a 128-bit signed product (magnitude + sign) shifted right by 32 to the nearest raw value,
// ties up (towards +inf), and saturates. Shared by Fixed and FVec2 (single rounding for dot/cross).
Fixed roundProduct(i128::U128 magnitude, bool negative) noexcept;

// Exact decimal rendering: "-1.5", "0.25", "3" (every Q32.32 value has <= 32 fraction digits).
std::string formatFixed(Fixed v);

namespace fixed_literals {
namespace detail {
// Compile-time decimal → Fixed, rounded to nearest, ties to even (the same rule as the TGI parser).
consteval Fixed parseLiteral(const char* s) {
    std::uint64_t whole = 0;
    i128::U128 fracNum{};      // fraction digits as an integer
    i128::U128 fracDen{0, 1};  // 10^digits
    bool seenDot = false;
    int digits = 0;
    for (const char* p = s; *p != '\0'; ++p) {
        const char c = *p;
        if (c == '\'') continue;
        if (c == '.') {
            if (seenDot) throw "two decimal points in _fx literal";
            seenDot = true;
            continue;
        }
        if (c < '0' || c > '9') throw "_fx literals are decimal digits with an optional '.'";
        if (!seenDot) {
            whole = whole * 10 + static_cast<std::uint64_t>(c - '0');
            if (whole > 0x7FFFFFFFu) throw "_fx literal out of range";
        } else {
            if (++digits > 36) throw "_fx literal has too many fraction digits";  // 10^36 < 2^120
            fracNum = i128::add(i128::shl(fracNum, 3), i128::add(i128::shl(fracNum, 1), {0, static_cast<std::uint64_t>(c - '0')}));
            fracDen = i128::add(i128::shl(fracDen, 3), i128::shl(fracDen, 1));
        }
    }
    // round(fracNum * 2^32 / fracDen), ties to even. fracNum*2^32 can exceed 128 bits, so
    // produce the 32 fraction bits by binary long division instead.
    std::uint64_t q = 0;
    i128::U128 rem = fracNum;
    for (int bit = 0; bit < 32; ++bit) {  // binary long division producing 32 fraction bits
        rem = i128::shl(rem, 1);
        q <<= 1;
        if (fracDen <= rem) {
            rem = i128::sub(rem, fracDen);
            q |= 1u;
        }
    }
    const i128::U128 twice = i128::shl(rem, 1);
    if (fracDen < twice || (twice == fracDen && (q & 1u))) ++q;
    return Fixed::fromRaw(static_cast<std::int64_t>((whole << 32) + q));
}
}  // namespace detail

// 1.5_fx, 0.25_fx, 3_fx — parsed from the literal's characters at compile time, never as a float.
consteval Fixed operator""_fx(const char* s) { return detail::parseLiteral(s); }
}  // namespace fixed_literals

namespace fixed_consts {
inline constexpr Fixed kZero = Fixed::fromRaw(0);
inline constexpr Fixed kOne = Fixed::fromRaw(Fixed::kOne);
inline constexpr Fixed kHalf = Fixed::fromRaw(Fixed::kOne / 2);
inline constexpr Fixed kPi = Fixed::fromRaw(13493037705);      // round(pi * 2^32)
inline constexpr Fixed kTwoPi = Fixed::fromRaw(26986075409);   // round(2 pi * 2^32)
inline constexpr Fixed kHalfPi = Fixed::fromRaw(6746518852);   // round(pi/2 * 2^32)
}  // namespace fixed_consts

}  // namespace corps
