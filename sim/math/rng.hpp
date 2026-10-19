#pragma once

// The sim's only random number generator (CLAUDE.md: no rand()). PCG32 (XSH RR, 64-bit state,
// O'Neill's pcg32_random_r), seeded from the match seed. Per-system sub-streams are derived
// deterministically from (match seed, stream id), so adding a system never shifts another's draws.

#include <cstdint>

#include "math/fixed.hpp"

namespace corps {

// SplitMix64 finaliser: good bit mixing for deriving seeds.
constexpr std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

class Rng {
public:
    constexpr Rng() noexcept { seed(0x853C49E6748FEA9BULL, 0xDA3E39CB94B95BDBULL >> 1); }
    constexpr Rng(std::uint64_t initState, std::uint64_t stream) noexcept { seed(initState, stream); }

    // pcg32_srandom_r(initstate, initseq).
    constexpr void seed(std::uint64_t initState, std::uint64_t stream) noexcept {
        state = 0;
        inc = (stream << 1u) | 1u;
        nextU32();
        state += initState;
        nextU32();
    }

    // Independent stream for one system: same match seed + same id → same sequence, everywhere.
    static constexpr Rng derive(std::uint64_t matchSeed, std::uint64_t streamId) noexcept {
        return Rng(splitmix64(matchSeed ^ splitmix64(streamId)), streamId);
    }

    constexpr std::uint32_t nextU32() noexcept {
        const std::uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const auto rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
    }

    // Uniform in [0, n) without modulo bias (Lemire's method with rejection). n == 0 → 0.
    constexpr std::uint32_t below(std::uint32_t n) noexcept {
        if (n == 0) return 0;
        std::uint64_t m = static_cast<std::uint64_t>(nextU32()) * n;
        auto low = static_cast<std::uint32_t>(m);
        if (low < n) {
            const std::uint32_t threshold = (0u - n) % n;
            while (low < threshold) {
                m = static_cast<std::uint64_t>(nextU32()) * n;
                low = static_cast<std::uint32_t>(m);
            }
        }
        return static_cast<std::uint32_t>(m >> 32);
    }

    // Uniform in [lo, hi] (inclusive). lo > hi returns lo.
    constexpr std::int32_t range(std::int32_t lo, std::int32_t hi) noexcept {
        if (hi <= lo) return lo;
        const std::uint32_t span = static_cast<std::uint32_t>(static_cast<std::int64_t>(hi) - lo) + 1u;
        const std::uint32_t r = span == 0 ? nextU32() : below(span);  // span 0 = full 2^32 range
        return static_cast<std::int32_t>(static_cast<std::int64_t>(lo) + r);
    }

    // Uniform Fixed in [0, 1) with 32 random fraction bits.
    constexpr Fixed fixed01() noexcept { return Fixed::fromRaw(nextU32()); }

    // true with probability p (p <= 0 never, p >= 1 always).
    constexpr bool chance(Fixed p) noexcept {
        if (p.raw <= 0) return false;
        if (p.raw >= Fixed::kOne) return true;
        return nextU32() < static_cast<std::uint64_t>(p.raw);
    }

    // State for the canonical serializer (and save/load).
    constexpr std::uint64_t rawState() const noexcept { return state; }
    constexpr std::uint64_t rawInc() const noexcept { return inc; }
    static constexpr Rng fromRaw(std::uint64_t st, std::uint64_t in) noexcept {
        Rng r;
        r.state = st;
        r.inc = in | 1u;
        return r;
    }

    friend constexpr bool operator==(const Rng& a, const Rng& b) noexcept { return a.state == b.state && a.inc == b.inc; }

private:
    std::uint64_t state = 0;
    std::uint64_t inc = 1;
};

}  // namespace corps
