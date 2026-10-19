#include "math/fixed.hpp"

namespace corps {

std::uint64_t& fixedOverflowCounter() noexcept {
    static std::uint64_t count = 0;
    return count;
}

namespace {

constexpr std::uint64_t kMaxMag = static_cast<std::uint64_t>(Fixed::kMaxRaw);  // 2^63 - 1
constexpr std::uint64_t kMinMag = kMaxMag + 1;                                 // |min| = 2^63

Fixed saturated(bool negative) noexcept {
    ++fixedOverflowCounter();
    return negative ? Fixed::min() : Fixed::max();
}

// Signed result from magnitude + sign, saturating.
Fixed fromMagnitude(i128::U128 mag, bool negative) noexcept {
    if (mag.hi != 0) return saturated(negative);
    if (negative) {
        if (mag.lo > kMinMag) return saturated(true);
        return Fixed::fromRaw(static_cast<std::int64_t>(~mag.lo + 1u));
    }
    if (mag.lo > kMaxMag) return saturated(false);
    return Fixed::fromRaw(static_cast<std::int64_t>(mag.lo));
}

}  // namespace

Fixed operator+(Fixed a, Fixed b) noexcept {
    const std::uint64_t r = static_cast<std::uint64_t>(a.raw) + static_cast<std::uint64_t>(b.raw);
    const auto s = static_cast<std::int64_t>(r);
    // Overflow iff both operands have the same sign and the result's sign differs.
    if ((a.raw < 0) == (b.raw < 0) && (s < 0) != (a.raw < 0)) return saturated(a.raw < 0);
    return Fixed::fromRaw(s);
}

Fixed operator-(Fixed a, Fixed b) noexcept {
    const std::uint64_t r = static_cast<std::uint64_t>(a.raw) - static_cast<std::uint64_t>(b.raw);
    const auto s = static_cast<std::int64_t>(r);
    if ((a.raw < 0) != (b.raw < 0) && (s < 0) != (a.raw < 0)) return saturated(a.raw < 0);
    return Fixed::fromRaw(s);
}

Fixed operator-(Fixed a) noexcept {
    if (a.raw == Fixed::kMinRaw) return saturated(false);
    return Fixed::fromRaw(-a.raw);
}

Fixed roundProduct(i128::U128 mag, bool negative) noexcept {
    // value = ±mag / 2^32, rounded half up (towards +inf). For a negative value -m/2^32 that is
    // -floor((m - 1)/2^32 ... ) — handled by rounding the magnitude half DOWN when negative.
    const std::uint64_t fracBits = mag.lo & 0xFFFFFFFFu;
    i128::U128 q = i128::shr(mag, 32);
    const bool roundAway = negative ? fracBits > 0x80000000u : fracBits >= 0x80000000u;
    if (roundAway) q = i128::add(q, {0, 1});
    return fromMagnitude(q, negative && !(q.hi == 0 && q.lo == 0));
}

Fixed operator*(Fixed a, Fixed b) noexcept {
    const bool negative = (a.raw < 0) != (b.raw < 0);
    return roundProduct(i128::mul(i128::magnitude(a.raw), i128::magnitude(b.raw)), negative);
}

Fixed operator*(Fixed a, std::int32_t k) noexcept {
    const bool negative = (a.raw < 0) != (k < 0);
    return fromMagnitude(i128::mul(i128::magnitude(a.raw), i128::magnitude(k)), negative);
}

Fixed operator/(Fixed a, Fixed b) noexcept {
    if (b.raw == 0) {
        if (a.raw == 0) {
            ++fixedOverflowCounter();
            return Fixed::fromRaw(0);
        }
        return saturated(a.raw < 0);
    }
    const bool negative = (a.raw < 0) != (b.raw < 0);
    const std::uint64_t d = i128::magnitude(b.raw);
    const i128::DivResult r = i128::div(i128::shl({0, i128::magnitude(a.raw)}, 32), d);
    i128::U128 q = r.quot;
    // Round to nearest, ties away from zero: compare 2*rem with d without overflow.
    if (r.rem >= d - r.rem) q = i128::add(q, {0, 1});
    return fromMagnitude(q, negative);
}

Fixed Fixed::fromRatio(std::int64_t num, std::int64_t den) noexcept {
    if (den == 0) {
        if (num == 0) {
            ++fixedOverflowCounter();
            return Fixed::fromRaw(0);
        }
        return saturated(num < 0);
    }
    const bool negative = (num < 0) != (den < 0);
    const std::uint64_t d = i128::magnitude(den);
    const i128::DivResult r = i128::div(i128::shl({0, i128::magnitude(num)}, 32), d);
    i128::U128 q = r.quot;
    if (r.rem >= d - r.rem) q = i128::add(q, {0, 1});
    return fromMagnitude(q, negative);
}

Fixed abs(Fixed v) noexcept {
    return v.raw < 0 ? -v : v;
}

std::string formatFixed(Fixed v) {
    const bool negative = v.raw < 0;
    const std::uint64_t mag = i128::magnitude(v.raw);
    const std::uint64_t whole = mag >> Fixed::kFracBits;
    std::uint64_t frac = mag & 0xFFFFFFFFULL;

    std::string out = negative ? "-" : "";
    out += std::to_string(whole);
    if (frac != 0) {
        out += '.';
        // frac / 2^32 in decimal: multiply by 10, take the carry out of bit 32. Terminates within
        // 32 digits because 2^32 divides 10^32. frac*10 < 2^36 never overflows.
        while (frac != 0) {
            frac *= 10;
            out += static_cast<char>('0' + static_cast<int>(frac >> Fixed::kFracBits));
            frac &= 0xFFFFFFFFULL;
        }
    }
    return out;
}

}  // namespace corps
