// sim/math tests (M3 part 1): Fixed arithmetic, 128-bit helpers, isqrt, binary-angle trig, FVec2, Rng.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "math/angle.hpp"
#include "math/fixed.hpp"
#include "math/fvec2.hpp"
#include "math/int128.hpp"
#include "math/isqrt.hpp"
#include "math/rng.hpp"
#include "test_env.hpp"

using namespace corps;
using namespace corps::fixed_literals;
using i128::U128;

namespace {

constexpr std::int64_t kOne = Fixed::kOne;

Fixed raw(std::int64_t r) {
    return Fixed::fromRaw(r);
}

// Reference: round-half-up of (a.raw * b.raw) / 2^32 using the portable 128-bit path.
std::int64_t refMul(std::int64_t a, std::int64_t b, bool* saturated) {
    const bool neg = (a < 0) != (b < 0);
    const U128 m = i128::mulPortable(i128::magnitude(a), i128::magnitude(b));
    U128 q = i128::shr(m, 32);
    const std::uint64_t f = m.lo & 0xFFFFFFFFu;
    if (neg ? f > 0x80000000u : f >= 0x80000000u) q = i128::add(q, {0, 1});
    *saturated = q.hi != 0 || q.lo > (neg ? (std::uint64_t{1} << 63) : (std::uint64_t{1} << 63) - 1);
    if (*saturated) return neg ? Fixed::kMinRaw : Fixed::kMaxRaw;
    return neg ? static_cast<std::int64_t>(~q.lo + 1) : static_cast<std::int64_t>(q.lo);
}

struct OverflowWatch {
    std::uint64_t start = fixedOverflowCount();
    std::uint64_t delta() const { return fixedOverflowCount() - start; }
};

}  // namespace

// ------------------------------------------------------------------------------------- int128

TEST_CASE("int128: fast multiply/divide paths match the portable reference bit for bit") {
    Rng rng(12345, 1);
    const std::uint64_t edges[] = {0, 1, 2, 3, 0xFFFFFFFFu, 0x100000000ULL, 0x7FFFFFFFFFFFFFFFULL,
                                   0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, 0xDEADBEEFCAFEBABEULL};
    for (std::uint64_t a : edges) {
        for (std::uint64_t b : edges) {
            CHECK(i128::mul(a, b) == i128::mulPortable(a, b));
            if (b != 0) {
                const U128 n{a >> 1, b ^ a};
                const auto f = i128::div(n, b);
                const auto p = i128::divPortable(n, b);
                CHECK(f.quot == p.quot);
                CHECK(f.rem == p.rem);
            }
        }
    }
    for (int i = 0; i < 20000; ++i) {
        const std::uint64_t a = (static_cast<std::uint64_t>(rng.nextU32()) << 32) | rng.nextU32();
        const std::uint64_t b = (static_cast<std::uint64_t>(rng.nextU32()) << (rng.below(33))) | rng.nextU32() | 1u;
        REQUIRE(i128::mul(a, b) == i128::mulPortable(a, b));
        const U128 n{a % (b + 1) , b * 7 + a};
        const auto f = i128::div(n, b);
        const auto p = i128::divPortable(n, b);
        REQUIRE(f.quot == p.quot);
        REQUIRE(f.rem == p.rem);
    }
}

TEST_CASE("int128: identities") {
    CHECK(i128::mulPortable(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL) == U128{0xFFFFFFFFFFFFFFFEULL, 1});
    const auto d = i128::divPortable({1, 0}, 3);  // 2^64 / 3
    CHECK(d.quot == U128{0, 0x5555555555555555ULL});
    CHECK(d.rem == 1);
    CHECK(i128::shl({0, 1}, 127) == U128{0x8000000000000000ULL, 0});
    CHECK(i128::shr({0x8000000000000000ULL, 0}, 127) == U128{0, 1});
    CHECK(i128::magnitude(Fixed::kMinRaw) == 0x8000000000000000ULL);
}

// ------------------------------------------------------------------------------------- Fixed

TEST_CASE("Fixed: literals are exact and round to nearest, ties to even") {
    static_assert((1.5_fx).raw == kOne + kOne / 2);
    static_assert((0.25_fx).raw == kOne / 4);
    static_assert((3_fx).raw == 3 * kOne);
    static_assert((0.1_fx).raw == 429496730);  // 0.1 * 2^32 = 429496729.6
    static_assert((2147483647_fx).raw == std::int64_t{2147483647} * kOne);
    // 2^-33 is exactly half an ulp: ties to even → 0; 3 * 2^-33 → 2 ulp.
    static_assert((0.000000000116415321826934814453125_fx).raw == 0);
    static_assert((0.000000000349245965480804443359375_fx).raw == 2);
    CHECK(formatFixed(12.375_fx) == "12.375");
}

TEST_CASE("Fixed: add/sub/neg saturate instead of overflowing") {
    OverflowWatch w;
    CHECK((1.5_fx + 2.25_fx) == 3.75_fx);
    CHECK((1_fx - 2.5_fx).raw == -kOne - kOne / 2);
    CHECK(Fixed::max() + raw(1) == Fixed::max());
    CHECK(Fixed::min() - raw(1) == Fixed::min());
    CHECK(Fixed::max() - Fixed::min() == Fixed::max());
    CHECK(Fixed::min() + Fixed::max() == raw(-1));  // no overflow
    CHECK(-Fixed::min() == Fixed::max());
    CHECK(-Fixed::max() == raw(Fixed::kMinRaw + 1));
    CHECK(w.delta() == 4);
}

TEST_CASE("Fixed: multiply rounds half up and matches the 128-bit reference") {
    CHECK(2_fx * 3_fx == 6_fx);
    CHECK(1.5_fx * -2_fx == -3_fx);
    CHECK(0.5_fx * 0.5_fx == 0.25_fx);
    // half-ulp products: +0.5 ulp → up, -0.5 ulp → up (towards +inf) = 0
    CHECK(raw(1) * 0.5_fx == raw(1));
    CHECK(raw(-1) * 0.5_fx == raw(0));
    CHECK(raw(3) * 0.5_fx == raw(2));   // 1.5 ulp → 2
    CHECK(raw(-3) * 0.5_fx == raw(-1)); // -1.5 ulp → -1
    CHECK(raw(1) * raw(1) == raw(0));   // 2^-64 rounds to 0

    Rng rng(7, 3);
    for (int i = 0; i < 50000; ++i) {
        const auto a = static_cast<std::int64_t>((static_cast<std::uint64_t>(rng.nextU32()) << 32) | rng.nextU32()) >> rng.below(40);
        const auto b = static_cast<std::int64_t>((static_cast<std::uint64_t>(rng.nextU32()) << 32) | rng.nextU32()) >> rng.below(40);
        bool sat = false;
        const std::int64_t expect = refMul(a, b, &sat);
        REQUIRE((raw(a) * raw(b)).raw == expect);
    }
}

TEST_CASE("Fixed: multiply overflow saturates with the right sign") {
    OverflowWatch w;
    CHECK(Fixed::max() * 2_fx == Fixed::max());
    CHECK(Fixed::max() * -2_fx == Fixed::min());
    CHECK(Fixed::min() * Fixed::min() == Fixed::max());
    CHECK(65536_fx * 32768_fx == Fixed::max());  // 2^31 is just out of range
    CHECK(65536_fx * -32768_fx == Fixed::min()); // -2^31 is exactly representable
    CHECK(w.delta() == 4);
    CHECK(Fixed::min() * 1_fx == Fixed::min());  // exact, no saturation
    CHECK(w.delta() == 4);
    CHECK((Fixed::max() * 3) == Fixed::max());
    CHECK((5_fx * -3) == -15_fx);
}

TEST_CASE("Fixed: division rounds to nearest (ties away from zero) and handles zero") {
    CHECK(1_fx / 4_fx == 0.25_fx);
    CHECK(-7_fx / 2_fx == -3.5_fx);
    CHECK(1_fx / 3_fx == raw(1431655765));   // 0.333... * 2^32 = 1431655765.33
    CHECK(2_fx / 3_fx == raw(2863311531));   // 2863311530.67 → up
    CHECK(raw(1) / 2_fx == raw(1));          // 0.5 ulp → away from zero
    CHECK(raw(-1) / 2_fx == raw(-1));
    OverflowWatch w;
    CHECK(5_fx / 0_fx == Fixed::max());
    CHECK(-5_fx / 0_fx == Fixed::min());
    CHECK(0_fx / 0_fx == 0_fx);
    CHECK(Fixed::max() / 0.5_fx == Fixed::max());
    CHECK(Fixed::min() / -1_fx == Fixed::max());
    CHECK(w.delta() == 5);
    CHECK(Fixed::min() / 1_fx == Fixed::min());
    CHECK(Fixed::fromRatio(1, 3) == 1_fx / 3_fx);
    CHECK(Fixed::fromRatio(-22, 7) == raw(-13498468645));  // -22/7 * 2^32 = -13498468644.57
    // a / b * b ≈ a within one ulp for random values
    Rng rng(99, 5);
    for (int i = 0; i < 20000; ++i) {
        const Fixed a = raw(static_cast<std::int32_t>(rng.nextU32()) * std::int64_t{rng.range(1, 4000)});
        const Fixed b = raw(static_cast<std::int64_t>(rng.range(1, 1 << 30)) * (rng.chance(0.5_fx) ? -1 : 1) * 64);
        const Fixed back = (a / b) * b;
        REQUIRE(abs(back - a).raw <= abs(b).raw / kOne + 2);
    }
}

TEST_CASE("Fixed: integer conversions") {
    CHECK((2.5_fx).floorInt() == 2);
    CHECK((-2.5_fx).floorInt() == -3);
    CHECK((2.5_fx).ceilInt() == 3);
    CHECK((-2.5_fx).ceilInt() == -2);
    CHECK((2.5_fx).roundInt() == 3);
    CHECK((-2.5_fx).roundInt() == -2);  // half up
    CHECK((-2.6_fx).roundInt() == -3);
    CHECK((2.5_fx).truncInt() == 2);
    CHECK((-2.5_fx).truncInt() == -2);
    CHECK((-2_fx).truncInt() == -2);
    CHECK((-2.25_fx).frac() == 0.75_fx);
    CHECK(Fixed::max().floorInt() == 2147483647);
    CHECK(Fixed::max().ceilInt() == 2147483648LL);
    CHECK(Fixed::min().floorInt() == -2147483648LL);
    CHECK(Fixed::min().roundInt() == -2147483648LL);
    CHECK(abs(Fixed::min()) == Fixed::max());
    CHECK(clamp(5_fx, 0_fx, 2_fx) == 2_fx);
}

// ------------------------------------------------------------------------------------- isqrt

TEST_CASE("isqrt: exact floor for edge and random values") {
    CHECK(isqrt64(0) == 0);
    CHECK(isqrt64(1) == 1);
    CHECK(isqrt64(3) == 1);
    CHECK(isqrt64(4) == 2);
    CHECK(isqrt64(0xFFFFFFFFFFFFFFFFULL) == 0xFFFFFFFFu);
    CHECK(isqrt64(0xFFFFFFFE00000001ULL) == 0xFFFFFFFFu);  // (2^32-1)^2
    CHECK(isqrt64(0xFFFFFFFE00000000ULL) == 0xFFFFFFFEu);
    CHECK(isqrt128({0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL}) == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(isqrt128({1, 0}) == 0x100000000ULL);  // sqrt(2^64)
    Rng rng(4242, 9);
    for (int i = 0; i < 100000; ++i) {
        const U128 v{(static_cast<std::uint64_t>(rng.nextU32()) << 32 | rng.nextU32()) >> rng.below(64),
                     static_cast<std::uint64_t>(rng.nextU32()) << 32 | rng.nextU32()};
        const std::uint64_t r = isqrt128(v);
        REQUIRE(i128::mulPortable(r, r) <= v);
        if (r != 0xFFFFFFFFFFFFFFFFULL) REQUIRE(v < i128::mulPortable(r + 1, r + 1));
    }
}

TEST_CASE("sqrt(Fixed)") {
    CHECK(sqrt(4_fx) == 2_fx);
    CHECK(sqrt(2.25_fx) == 1.5_fx);
    CHECK(sqrt(0_fx) == 0_fx);
    CHECK(sqrt(raw(1)) == raw(65536));  // sqrt(2^-32) = 2^-16
    CHECK(sqrt(2_fx) == raw(6074000999));  // floor(sqrt(2) * 2^32)
    CHECK(sqrt(Fixed::max()).raw == 199032864766430LL);  // floor(sqrt((2^63-1) * 2^32))
    OverflowWatch w;
    CHECK(sqrt(-1_fx) == 0_fx);
    CHECK(w.delta() == 1);
}

// ------------------------------------------------------------------------------------- Angle

TEST_CASE("Angle: exact cardinal values and wrap-around") {
    CHECK(sin(Angle{}) == 0_fx);
    CHECK(cos(Angle{}) == 1_fx);
    CHECK(sin(Angle::fromUnits(16384)) == 1_fx);
    CHECK(cos(Angle::fromUnits(16384)) == 0_fx);
    CHECK(sin(Angle::fromUnits(32768)) == 0_fx);
    CHECK(cos(Angle::fromUnits(32768)) == -1_fx);
    CHECK(sin(Angle::fromUnits(49152)) == -1_fx);
    CHECK((Angle::fromUnits(65000) + Angle::fromUnits(1000)).units == 464);
    CHECK((Angle::fromUnits(100) - Angle::fromUnits(200)).units == 65436);
    CHECK(Angle::delta(Angle::fromUnits(65000), Angle::fromUnits(100)) == 636);
    CHECK(Angle::delta(Angle::fromUnits(100), Angle::fromUnits(65000)) == -636);
    CHECK(Angle::fromDegrees(90_fx).units == 16384);
    CHECK(Angle::fromDegrees(-90_fx).units == 49152);
    CHECK(Angle::fromDegrees(450_fx).units == 16384);
    CHECK(Angle::fromDegrees(37_fx).units == 6736);  // 37/360 * 65536 = 6735.6
    CHECK(Angle::fromUnits(8192).toDegrees() == 45_fx);
}

TEST_CASE("Angle: sin/cos over all 65536 angles (unit circle, symmetry, accuracy)") {
    // 1e-6 tolerance on sin^2+cos^2 (table interpolation error is ~2e-8)
    const Fixed tol = 0.000001_fx;
    for (std::uint32_t u = 0; u < 65536; ++u) {
        const Angle a = Angle::fromUnits(u);
        const Fixed s = sin(a);
        const Fixed c = cos(a);
        REQUIRE(abs(s * s + c * c - 1_fx) <= tol);
        REQUIRE(sin(-a) == -s);                                   // odd
        REQUIRE(cos(-a) == c);                                    // even
        REQUIRE(sin(Angle::fromUnits(u + 32768)) == -s);          // half-turn
        REQUIRE(sin(Angle::fromUnits(32768 - u)) == s);           // sin(pi - x) = sin x
        REQUIRE(s <= 1_fx);
        REQUIRE(s >= -1_fx);
    }
}

TEST_CASE("Angle: atan2 inverts sin/cos to within one unit for every angle") {
    for (std::uint32_t u = 0; u < 65536; ++u) {
        const Angle a = Angle::fromUnits(u);
        const Angle back = atan2(sin(a), cos(a));
        REQUIRE(std::abs(Angle::delta(a, back)) <= 1);
        // scale invariance (large and small vectors)
        const Angle big = atan2(sin(a) * 10000_fx, cos(a) * 10000_fx);
        REQUIRE(std::abs(Angle::delta(a, big)) <= 1);
    }
}

TEST_CASE("Angle: atan2 axes, quadrants and degenerate input") {
    CHECK(atan2(0_fx, 0_fx).units == 0);
    CHECK(atan2(0_fx, 5_fx).units == 0);
    CHECK(atan2(5_fx, 0_fx).units == 16384);
    CHECK(atan2(0_fx, -5_fx).units == 32768);
    CHECK(atan2(-5_fx, 0_fx).units == 49152);
    CHECK(atan2(1_fx, 1_fx).units == 8192);
    CHECK(atan2(1_fx, -1_fx).units == 24576);
    CHECK(atan2(-1_fx, -1_fx).units == 40960);
    CHECK(atan2(-1_fx, 1_fx).units == 57344);
    CHECK(atan2(raw(1), raw(1)).units == 8192);  // tiny
    CHECK(atan2(Fixed::max(), Fixed::max()).units == 8192);
    CHECK(atan2(Fixed::min(), Fixed::min()).units == 40960);
    CHECK(atan2(Fixed::min(), 0_fx).units == 49152);
}

// ------------------------------------------------------------------------------------- FVec2

TEST_CASE("FVec2: arithmetic, dot, cross, length") {
    const FVec2 a{3_fx, 4_fx};
    CHECK(a.length() == 5_fx);
    CHECK(a.lengthSq() == 25_fx);
    CHECK(dot(a, FVec2{1_fx, 2_fx}) == 11_fx);
    CHECK(cross(FVec2{1_fx, 0_fx}, FVec2{0_fx, 1_fx}) == 1_fx);
    CHECK(cross(FVec2{0_fx, 1_fx}, FVec2{1_fx, 0_fx}) == -1_fx);
    CHECK(a + a == FVec2{6_fx, 8_fx});
    CHECK(a * 0.5_fx == FVec2{1.5_fx, 2_fx});
    CHECK(distance(FVec2{1_fx, 1_fx}, FVec2{4_fx, 5_fx}) == 5_fx);
    CHECK(lerp(FVec2{}, a, 0.5_fx) == FVec2{1.5_fx, 2_fx});
    CHECK(a.perp() == FVec2{-4_fx, 3_fx});
}

TEST_CASE("FVec2: length and normalize for tiny, huge and zero vectors") {
    CHECK(FVec2{}.normalized() == FVec2{});
    CHECK(FVec2{}.length() == 0_fx);
    const FVec2 tiny{raw(3), raw(4)};
    CHECK(tiny.length() == raw(5));
    CHECK(tiny.normalized() == FVec2{0.6_fx, 0.8_fx});
    // Huge: lengthSq saturates, length does not.
    const FVec2 huge{1000000000_fx, 1000000000_fx};
    OverflowWatch w;
    CHECK(huge.lengthSq() == Fixed::max());
    CHECK(w.delta() == 1);
    CHECK(huge.length() == raw(6074000999952099384LL));  // floor(1e9 * sqrt 2 * 2^32)
    const FVec2 n = huge.normalized();
    CHECK(abs(n.length() - 1_fx) <= raw(4));
    CHECK(FVec2{Fixed::max(), Fixed::max()}.length() == Fixed::max());  // saturates (2^31.5)
    // Unit vectors from angles normalise to themselves (within rounding)
    for (std::uint32_t u = 0; u < 65536; u += 97) {
        const FVec2 v = FVec2::fromAngle(Angle::fromUnits(u));
        REQUIRE(abs(v.length() - 1_fx) <= 0.000001_fx);
        REQUIRE(std::abs(Angle::delta(v.angle(), Angle::fromUnits(u))) <= 1);
    }
}

TEST_CASE("FVec2: rotation") {
    const FVec2 v{2_fx, 0_fx};
    CHECK(v.rotated(Angle::fromUnits(16384)) == FVec2{0_fx, 2_fx});
    CHECK(v.rotated(Angle::fromUnits(32768)) == FVec2{-2_fx, 0_fx});
    const FVec2 r = FVec2{3_fx, 4_fx}.rotated(Angle::fromUnits(12345));
    CHECK(abs(r.length() - 5_fx) <= 0.00001_fx);
}

// ------------------------------------------------------------------------------------- Rng

TEST_CASE("Rng: PCG32 reference sequence (pcg32-demo: seed 42, stream 54)") {
    Rng r(42, 54);
    const std::uint32_t expect[] = {0xa15c02b7u, 0x7b47f409u, 0xba1d3330u, 0x83d2f293u, 0xbfa4784bu, 0xcbed606eu};
    for (std::uint32_t e : expect) CHECK(r.nextU32() == e);
}

TEST_CASE("Rng: below() is in range and unbiased enough; edge cases") {
    Rng r(1, 2);
    CHECK(r.below(0) == 0);
    CHECK(r.below(1) == 0);
    std::uint32_t counts[6] = {};
    for (int i = 0; i < 60000; ++i) {
        const std::uint32_t v = r.below(6);
        REQUIRE(v < 6);
        ++counts[v];
    }
    for (std::uint32_t c : counts) CHECK((c > 9500 && c < 10500));
    for (int i = 0; i < 1000; ++i) {
        const std::int32_t v = r.range(-3, 3);
        REQUIRE((v >= -3 && v <= 3));
    }
    CHECK(r.range(5, 5) == 5);
    CHECK(r.range(5, 1) == 5);
    const std::int32_t full = r.range(INT32_MIN, INT32_MAX);  // full range does not loop forever
    (void)full;
    CHECK_FALSE(r.chance(0_fx));
    CHECK(r.chance(1_fx));
    int hits = 0;
    for (int i = 0; i < 10000; ++i) hits += r.chance(0.25_fx) ? 1 : 0;
    CHECK((hits > 2300 && hits < 2700));
    for (int i = 0; i < 1000; ++i) {
        const Fixed f = r.fixed01();
        REQUIRE(f >= 0_fx);
        REQUIRE(f < 1_fx);
    }
}

TEST_CASE("Rng: derived streams are reproducible and independent; raw state round-trips") {
    Rng a = Rng::derive(777, 1);
    Rng b = Rng::derive(777, 1);
    Rng c = Rng::derive(777, 2);
    int same = 0;
    for (int i = 0; i < 100; ++i) {
        const std::uint32_t x = a.nextU32();
        REQUIRE(x == b.nextU32());
        same += (x == c.nextU32()) ? 1 : 0;
    }
    CHECK(same < 2);
    const Rng copy = Rng::fromRaw(a.rawState(), a.rawInc());
    CHECK(copy == a);
}

// ------------------------------------------------------------------------------------- cross-platform

TEST_CASE("math checksum is identical on every compiler and build type") {
    // FNV-1a over the results of a fixed mix of operations. If this changes on one platform only,
    // that platform is not deterministic. If it changes everywhere, the math changed: update it.
    Rng r(2026, 10);
    std::uint64_t h = 0xcbf29ce484222325ULL;
    const auto mix = [&h](std::int64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= static_cast<std::uint64_t>(v >> (8 * i)) & 0xFFu;
            h *= 0x100000001b3ULL;
        }
    };
    for (int i = 0; i < 10000; ++i) {
        const Fixed a = raw(static_cast<std::int64_t>(static_cast<std::int32_t>(r.nextU32())) << r.below(24));
        const Fixed b = raw((static_cast<std::int64_t>(static_cast<std::int32_t>(r.nextU32())) << r.below(24)) | 1);
        const Angle ang = Angle::fromUnits(r.nextU32());
        mix((a * b).raw);
        mix((a / b).raw);
        mix((a + b).raw);
        mix(sqrt(abs(a)).raw);
        mix(sin(ang).raw);
        mix(cos(ang).raw);
        mix(atan2(a, b).units);
        const FVec2 v{a, b};
        mix(v.length().raw);
        mix(v.normalized().x.raw);
        mix(v.rotated(ang).y.raw);
        mix(dot(v, FVec2{b, a}).raw);
    }
    if (testEnvSet("CORPS_PRINT_GOLDEN")) std::printf("math checksum: 0x%016llx\n", static_cast<unsigned long long>(h));
    CHECK(h == 0x720035b414532db3ULL);  // first computed with MSVC x64 Debug
}
