#include "math/fixed.hpp"

namespace corps {

std::string formatFixed(Fixed v) {
    const bool negative = v.raw < 0;
    // Magnitude as uint64 (works for INT64_MIN too).
    const std::uint64_t mag = negative ? (~static_cast<std::uint64_t>(v.raw) + 1) : static_cast<std::uint64_t>(v.raw);
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
