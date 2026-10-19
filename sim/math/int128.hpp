#pragma once

// 128-bit helpers for Fixed arithmetic (ARCHITECTURE §4.3). The portable versions use 32-bit
// limbs and are constexpr; fast paths use __int128 (GCC/Clang) or the MSVC x64 intrinsics. Tests
// assert that both paths give identical bits, so compilers can't drift apart.

#include <cstdint>

#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
#include <intrin.h>
#define CORPS_MSVC_INTRINSICS 1
#elif defined(__SIZEOF_INT128__)
#define CORPS_HAS_INT128 1
#endif

namespace corps::i128 {

#if defined(CORPS_HAS_INT128)
__extension__ typedef unsigned __int128 Native;  // __extension__: no -Wpedantic warning
#endif

struct U128 {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;

    friend constexpr bool operator==(U128 a, U128 b) noexcept { return a.hi == b.hi && a.lo == b.lo; }
    friend constexpr bool operator<(U128 a, U128 b) noexcept { return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo); }
    friend constexpr bool operator<=(U128 a, U128 b) noexcept { return !(b < a); }
};

constexpr U128 add(U128 a, U128 b) noexcept {
    const std::uint64_t lo = a.lo + b.lo;
    return {a.hi + b.hi + (lo < a.lo ? 1u : 0u), lo};
}

constexpr U128 sub(U128 a, U128 b) noexcept {
    return {a.hi - b.hi - (a.lo < b.lo ? 1u : 0u), a.lo - b.lo};
}

constexpr U128 shl(U128 a, unsigned n) noexcept {
    if (n == 0) return a;
    if (n >= 128) return {};
    if (n >= 64) return {a.lo << (n - 64), 0};
    return {(a.hi << n) | (a.lo >> (64 - n)), a.lo << n};
}

constexpr U128 shr(U128 a, unsigned n) noexcept {
    if (n == 0) return a;
    if (n >= 128) return {};
    if (n >= 64) return {0, a.hi >> (n - 64)};
    return {a.hi >> n, (a.lo >> n) | (a.hi << (64 - n))};
}

// --- portable reference implementations --------------------------------------------------------

constexpr U128 mulPortable(std::uint64_t a, std::uint64_t b) noexcept {
    const std::uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32;
    const std::uint64_t b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    const std::uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const std::uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    return {p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (mid << 32) | (p00 & 0xFFFFFFFFu)};
}

// Quotient and remainder of n / d (d != 0). The quotient may need all 128 bits.
struct DivResult {
    U128 quot;
    std::uint64_t rem = 0;
};

constexpr DivResult divPortable(U128 n, std::uint64_t d) noexcept {
    // Two 64-bit schoolbook steps when possible, else bitwise long division of the high part.
    DivResult r;
    r.quot.hi = n.hi / d;
    std::uint64_t rem = n.hi % d;
    // Now divide (rem:lo) by d, where rem < d, bit by bit.
    std::uint64_t q = 0;
    for (int i = 63; i >= 0; --i) {
        const bool carry = (rem >> 63) != 0;
        rem = (rem << 1) | ((n.lo >> i) & 1u);
        q <<= 1;
        if (carry || rem >= d) {
            rem -= d;
            q |= 1u;
        }
    }
    r.quot.lo = q;
    r.rem = rem;
    return r;
}

// --- fast paths (identical results) ------------------------------------------------------------

inline U128 mul(std::uint64_t a, std::uint64_t b) noexcept {
#if defined(CORPS_HAS_INT128)
    const Native p = static_cast<Native>(a) * b;
    return {static_cast<std::uint64_t>(p >> 64), static_cast<std::uint64_t>(p)};
#elif defined(CORPS_MSVC_INTRINSICS)
    U128 r;
    r.lo = _umul128(a, b, &r.hi);
    return r;
#else
    return mulPortable(a, b);
#endif
}

inline DivResult div(U128 n, std::uint64_t d) noexcept {
// 128-bit division compiles to a compiler-rt call (__udivti3) that MSVC's linker lacks, so
// clang-cl (which defines _MSC_VER) uses the portable path.
#if defined(CORPS_HAS_INT128) && !defined(_MSC_VER)
    const Native nn = (static_cast<Native>(n.hi) << 64) | n.lo;
    const Native q = nn / d;
    return {{static_cast<std::uint64_t>(q >> 64), static_cast<std::uint64_t>(q)}, static_cast<std::uint64_t>(nn % d)};
#elif defined(CORPS_MSVC_INTRINSICS)
    // _udiv128 needs hi < d (64-bit quotient); split off the high quotient word first.
    DivResult r;
    r.quot.hi = n.hi / d;
    std::uint64_t rem = 0;
    r.quot.lo = _udiv128(n.hi % d, n.lo, d, &rem);
    r.rem = rem;
    return r;
#else
    return divPortable(n, d);
#endif
}

// Magnitude of a signed value as uint64 (INT64_MIN → 2^63), without signed overflow.
constexpr std::uint64_t magnitude(std::int64_t v) noexcept {
    return v < 0 ? (~static_cast<std::uint64_t>(v) + 1u) : static_cast<std::uint64_t>(v);
}

}  // namespace corps::i128
