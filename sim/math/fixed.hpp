#pragma once

// Q32.32 fixed-point number stored in int64 (ARCHITECTURE §4.3). M1 only needs the representation
// for parsing data; arithmetic (mul/div/sqrt/trig) arrives with the M3 math library.

#include <cstdint>
#include <string>

namespace corps {

struct Fixed {
    static constexpr int kFracBits = 32;
    static constexpr std::int64_t kOne = std::int64_t{1} << kFracBits;

    std::int64_t raw = 0;

    static constexpr Fixed fromRaw(std::int64_t r) noexcept { return Fixed{r}; }
    static constexpr Fixed fromInt(std::int32_t v) noexcept { return Fixed{static_cast<std::int64_t>(v) * kOne}; }

    friend constexpr bool operator==(Fixed a, Fixed b) noexcept { return a.raw == b.raw; }
    friend constexpr bool operator!=(Fixed a, Fixed b) noexcept { return a.raw != b.raw; }
    friend constexpr bool operator<(Fixed a, Fixed b) noexcept { return a.raw < b.raw; }
};

// Exact decimal rendering of a Fixed value (every Q32.32 value has a finite decimal expansion of at
// most 32 fractional digits). Trailing zeros are dropped; "-1.5", "0.25", "3".
std::string formatFixed(Fixed v);

}  // namespace corps
