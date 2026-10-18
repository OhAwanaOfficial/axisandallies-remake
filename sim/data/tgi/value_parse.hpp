#pragma once

// Typed value parsing (TGI_FORMAT §9.1). Integer-only and exact: this lives in sim/, so there is
// no float/double anywhere, including for fields the TGI declares as `float` or `KF32`.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "data/tgi/diagnostics.hpp"
#include "math/fixed.hpp"

namespace corps::tgi {

struct FixedParse {
    bool ok = false;
    Fixed value;
    bool decimalComma = false;  // "0,5": leading number taken (as C atof would)
    std::string error;
};

// Accepts -1  +2  3.5  .3333  5.  1.0f  .5f  25%  133%  (and "0,5" with decimalComma set).
// Exact decimal parse, then ONE rounding to the nearest 2^-32, ties to even. No exponents.
FixedParse parseFixed(std::string_view text);

// int64, base 10, optional sign.
std::optional<std::int64_t> parseInt(std::string_view text);

// true/false/yes/no (any case), 1/0.
std::optional<bool> parseBool(std::string_view text);

// "null" in any case means "no value". "none" is a literal; "" is an empty string.
bool isNull(std::string_view text);

struct PointParse {
    bool ok = false;               // every present component parsed
    bool countMismatch = false;    // fewer or more components than asked for
    std::array<Fixed, 3> v{};      // missing components are 0
};

// Comma-separated Fixed components: "0,0", "-1, -1, -1", ".6,.6,.6", "0%, 0%". dims is 2 or 3.
PointParse parsePoint(std::string_view text, int dims);

// Split on commas and/or whitespace; '+'/'-' prefixes stay on the tokens.
std::vector<std::string_view> splitWordList(std::string_view text);

// "#ids_key" → "ids_key"; literal text → nullopt.
std::optional<std::string_view> localizedKey(std::string_view text);

// --- diagnostic-reporting readers for the def compiler -------------------------------------------
// A value that doesn't parse as its type reports bad-value and returns the fallback (§9.1).

struct ValueSite {
    Diagnostics* diags = nullptr;
    std::string_view file;
    std::uint32_t line = 0;
    std::string_view key;
};

Fixed readFixed(std::string_view text, Fixed fallback, const ValueSite& site);
std::int64_t readInt(std::string_view text, std::int64_t fallback, const ValueSite& site);
bool readBool(std::string_view text, bool fallback, const ValueSite& site);
std::array<Fixed, 3> readPoint(std::string_view text, int dims, const ValueSite& site);

}  // namespace corps::tgi
