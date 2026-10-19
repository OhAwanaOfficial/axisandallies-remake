#include "math/fvec2.hpp"

#include "math/int128.hpp"
#include "math/isqrt.hpp"

namespace corps {

namespace {

// Signed 128-bit value in two's complement (wraps mod 2^128; our sums stay within ±2^127).
i128::U128 signedProduct(std::int64_t a, std::int64_t b) noexcept {
    const i128::U128 m = i128::mul(i128::magnitude(a), i128::magnitude(b));
    return ((a < 0) != (b < 0)) ? i128::sub({}, m) : m;
}

// a*b + c*d (or a*b - c*d) in Q32.32, rounded once (half up) and saturated.
Fixed mulAdd(Fixed a, Fixed b, Fixed c, Fixed d, bool subtract) noexcept {
    const i128::U128 p = signedProduct(a.raw, b.raw);
    const i128::U128 q = signedProduct(c.raw, d.raw);
    const i128::U128 s = subtract ? i128::sub(p, q) : i128::add(p, q);
    const bool negative = (s.hi >> 63) != 0;
    return roundProduct(negative ? i128::sub({}, s) : s, negative);
}

}  // namespace

Fixed dot(FVec2 a, FVec2 b) noexcept {
    return mulAdd(a.x, b.x, a.y, b.y, false);
}

Fixed cross(FVec2 a, FVec2 b) noexcept {
    return mulAdd(a.x, b.y, a.y, b.x, true);
}

Fixed FVec2::lengthSq() const noexcept {
    return dot(*this, *this);
}

Fixed FVec2::length() const noexcept {
    const std::uint64_t mx = i128::magnitude(x.raw);
    const std::uint64_t my = i128::magnitude(y.raw);
    const i128::U128 sum = i128::add(i128::mul(mx, mx), i128::mul(my, my));  // < 2^127
    const std::uint64_t r = isqrt128(sum);  // sqrt(raw^2 + raw^2) is the length in raw units
    if (r > static_cast<std::uint64_t>(Fixed::kMaxRaw)) {
        ++fixedOverflowCounter();
        return Fixed::max();
    }
    return Fixed::fromRaw(static_cast<std::int64_t>(r));
}

FVec2 FVec2::normalized() const noexcept {
    const Fixed len = length();
    if (len.raw == 0) return {};
    return {x / len, y / len};
}

FVec2 FVec2::rotated(Angle a) const noexcept {
    const Fixed c = cos(a);
    const Fixed s = sin(a);
    return {mulAdd(x, c, y, s, true), mulAdd(x, s, y, c, false)};
}

Fixed distance(FVec2 a, FVec2 b) noexcept {
    return (b - a).length();
}

Fixed distanceSq(FVec2 a, FVec2 b) noexcept {
    return (b - a).lengthSq();
}

FVec2 lerp(FVec2 a, FVec2 b, Fixed t) noexcept {
    return a + (b - a) * t;
}

}  // namespace corps
