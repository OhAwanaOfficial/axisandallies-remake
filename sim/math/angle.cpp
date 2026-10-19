#include "math/angle.hpp"

#include "math/int128.hpp"

namespace corps {

namespace {
#include "math/trig_tables.inc"

constexpr int kTableBits = 12;  // 4096 steps per quarter turn / per unit of atan argument

// Quarter-wave sine for offset 0..16384 (one quarter turn), interpolated between table entries
// 4 units apart; result rounded half up.
std::int64_t quarterSin(std::uint32_t offset) noexcept {
    const std::uint32_t idx = offset >> 2;
    const std::uint32_t f = offset & 3u;
    if (f == 0) return kSinQuarter[idx];
    return (kSinQuarter[idx] * static_cast<std::int64_t>(4 - f) + kSinQuarter[idx + 1] * static_cast<std::int64_t>(f) + 2) >> 2;
}

// atan(t) for t = ratio / 2^32 in [0, 1], in binary-angle units (rounded): 0..8192.
std::uint32_t atanUnits(std::uint64_t ratio) noexcept {
    const std::uint64_t idx = ratio >> (32 - kTableBits);
    const std::uint64_t f = ratio & ((std::uint64_t{1} << (32 - kTableBits)) - 1);  // 20 bits
    std::int64_t v = kAtanUnit[idx];
    if (f != 0) {
        const std::int64_t span = kAtanUnit[idx + 1] - kAtanUnit[idx];  // < 2^14
        v += (span * static_cast<std::int64_t>(f) + (std::int64_t{1} << 19)) >> 20;
    }
    return static_cast<std::uint32_t>((v + (1 << 15)) >> 16);
}

}  // namespace

Fixed sin(Angle a) noexcept {
    const std::uint32_t q = a.units >> 14;
    const std::uint32_t o = a.units & 0x3FFFu;
    const std::int64_t v = quarterSin((q & 1u) ? 16384u - o : o);
    return Fixed::fromRaw(q >= 2 ? -v : v);
}

Fixed cos(Angle a) noexcept {
    return sin(Angle::fromUnits(a.units + Angle::kQuarter));
}

Angle atan2(Fixed y, Fixed x) noexcept {
    const std::uint64_t ax = i128::magnitude(x.raw);
    const std::uint64_t ay = i128::magnitude(y.raw);
    if (ax == 0 && ay == 0) return Angle{};
    // base angle in [0, 16384] units for the first quadrant
    std::uint32_t base = 0;
    if (ay <= ax) {
        const std::uint64_t ratio = i128::div(i128::shl({0, ay}, 32), ax).quot.lo;  // <= 2^32
        base = atanUnits(ratio);
    } else {
        const std::uint64_t ratio = i128::div(i128::shl({0, ax}, 32), ay).quot.lo;
        base = 16384u - atanUnits(ratio);
    }
    std::uint32_t u = 0;
    if (x.raw >= 0) {
        u = y.raw >= 0 ? base : Angle::kFullTurn - base;
    } else {
        u = y.raw >= 0 ? Angle::kHalf - base : Angle::kHalf + base;
    }
    return Angle::fromUnits(u);
}

Angle Angle::fromDegrees(Fixed degrees) noexcept {
    // units = degrees * 65536 / 360, rounded; reduce degrees mod 360 first so the product fits.
    std::int64_t r = degrees.raw % (std::int64_t{360} * Fixed::kOne);
    if (r < 0) r += std::int64_t{360} * Fixed::kOne;
    // r < 360 * 2^32 < 2^41; r * 65536 < 2^57 fits; divide by 360 * 2^32 with rounding.
    const std::int64_t num = r * 65536;
    const std::int64_t den = std::int64_t{360} * Fixed::kOne;
    return fromUnits(static_cast<std::uint32_t>((num + den / 2) / den));
}

Fixed Angle::toDegrees() const noexcept {
    return Fixed::fromRatio(static_cast<std::int64_t>(units) * 360, 65536);
}

}  // namespace corps
