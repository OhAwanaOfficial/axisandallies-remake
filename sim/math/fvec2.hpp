#pragma once

// 2D fixed-point vector on the ground plane (ARCHITECTURE §4.3: the sim is 2D, x/y).
// dot/cross/lengthSq accumulate in 128 bits and round once; length() works for every
// representable vector (no intermediate overflow).

#include "math/angle.hpp"
#include "math/fixed.hpp"

namespace corps {

struct FVec2 {
    Fixed x;
    Fixed y;

    static constexpr FVec2 zero() noexcept { return {}; }
    static FVec2 fromAngle(Angle a) noexcept { return {cos(a), sin(a)}; }  // unit vector

    friend FVec2 operator+(FVec2 a, FVec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
    friend FVec2 operator-(FVec2 a, FVec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
    friend FVec2 operator-(FVec2 a) noexcept { return {-a.x, -a.y}; }
    friend FVec2 operator*(FVec2 a, Fixed s) noexcept { return {a.x * s, a.y * s}; }
    friend FVec2 operator*(Fixed s, FVec2 a) noexcept { return {a.x * s, a.y * s}; }
    friend FVec2 operator/(FVec2 a, Fixed s) noexcept { return {a.x / s, a.y / s}; }
    FVec2& operator+=(FVec2 b) noexcept { return *this = *this + b; }
    FVec2& operator-=(FVec2 b) noexcept { return *this = *this - b; }
    friend constexpr bool operator==(FVec2 a, FVec2 b) noexcept { return a.x == b.x && a.y == b.y; }
    friend constexpr bool operator!=(FVec2 a, FVec2 b) noexcept { return !(a == b); }

    bool isZero() const noexcept { return x.raw == 0 && y.raw == 0; }
    Fixed lengthSq() const noexcept;  // saturates beyond the Fixed range (|v| > ~46340)
    Fixed length() const noexcept;    // floor of the exact length, saturates only above Fixed::max
    // Unit vector in the same direction; zero stays zero.
    FVec2 normalized() const noexcept;
    Angle angle() const noexcept { return atan2(y, x); }
    FVec2 rotated(Angle a) const noexcept;
    FVec2 perp() const noexcept { return {-y, x}; }  // +90 degrees
};

Fixed dot(FVec2 a, FVec2 b) noexcept;
Fixed cross(FVec2 a, FVec2 b) noexcept;  // a.x*b.y - a.y*b.x (z of the 3D cross product)
Fixed distance(FVec2 a, FVec2 b) noexcept;
Fixed distanceSq(FVec2 a, FVec2 b) noexcept;
FVec2 lerp(FVec2 a, FVec2 b, Fixed t) noexcept;

}  // namespace corps
