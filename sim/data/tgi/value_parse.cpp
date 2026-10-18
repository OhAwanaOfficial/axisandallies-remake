#include "data/tgi/value_parse.hpp"

#include "core/ascii.hpp"

namespace corps::tgi {
namespace {

constexpr std::uint64_t kMaxWhole = std::uint64_t{1} << 31;  // |integer part| bound for Q32.32

// Binary expansion of 0.d1d2...dk: returns 32 fraction bits, the next (round) bit and whether
// anything non-zero remains (sticky). Exact for any number of digits.
void decimalFractionBits(std::string digits, std::uint32_t& bits, bool& roundBit, bool& sticky) {
    while (!digits.empty() && digits.back() == '0') digits.pop_back();
    bits = 0;
    roundBit = false;
    for (int b = 0; b < 33; ++b) {
        int carry = 0;
        for (std::size_t i = digits.size(); i-- > 0;) {
            const int d = (digits[i] - '0') * 2 + carry;
            digits[i] = static_cast<char>('0' + d % 10);
            carry = d / 10;
        }
        while (!digits.empty() && digits.back() == '0') digits.pop_back();
        if (b < 32) {
            bits = (bits << 1) | static_cast<std::uint32_t>(carry);
        } else {
            roundBit = carry != 0;
        }
    }
    sticky = !digits.empty();
}

}  // namespace

FixedParse parseFixed(std::string_view text) {
    FixedParse r;
    const std::string_view s = trimAscii(text);
    std::size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        ++i;
    }
    std::string whole;
    std::string frac;
    while (i < s.size() && isAsciiDigit(s[i])) whole.push_back(s[i++]);
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && isAsciiDigit(s[i])) frac.push_back(s[i++]);
    }
    if (whole.empty() && frac.empty()) {
        r.error = "not a number";
        return r;
    }
    if (i < s.size() && (s[i] == 'f' || s[i] == 'F')) ++i;  // C float suffix, ignored
    bool percent = false;
    if (i < s.size() && s[i] == '%') {
        percent = true;
        ++i;
    }
    if (i < s.size()) {
        if (s[i] == ',') {
            r.decimalComma = true;  // "0,5" → leading number
        } else {
            r.error = "unexpected '" + std::string(s.substr(i)) + "'";
            return r;
        }
    }

    if (percent) {  // ÷100 exactly: move the decimal point two places left
        if (whole.size() < 2) whole.insert(0, 2 - whole.size(), '0');
        frac.insert(0, whole.substr(whole.size() - 2));
        whole.resize(whole.size() - 2);
    }

    std::size_t z = 0;
    while (z < whole.size() && whole[z] == '0') ++z;
    whole.erase(0, z);
    if (whole.size() > 10) {
        r.error = "out of range";
        return r;
    }
    std::uint64_t w = 0;
    for (char c : whole) w = w * 10 + static_cast<std::uint64_t>(c - '0');
    if (w > kMaxWhole) {
        r.error = "out of range";
        return r;
    }

    std::uint32_t bits = 0;
    bool roundBit = false;
    bool sticky = false;
    decimalFractionBits(frac, bits, roundBit, sticky);
    std::uint64_t mag = (w << Fixed::kFracBits) | bits;
    if (roundBit && (sticky || (mag & 1U) != 0)) ++mag;  // nearest, ties to even

    const std::uint64_t limit = negative ? (std::uint64_t{1} << 63) : (std::uint64_t{1} << 63) - 1;
    if (mag > limit) {
        r.error = "out of range";
        return r;
    }
    r.value.raw = negative ? static_cast<std::int64_t>(std::uint64_t{0} - mag) : static_cast<std::int64_t>(mag);
    r.ok = true;
    return r;
}

std::optional<std::int64_t> parseInt(std::string_view text) {
    const std::string_view s = trimAscii(text);
    std::size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        ++i;
    }
    if (i >= s.size()) return std::nullopt;
    std::uint64_t mag = 0;
    const std::uint64_t limit = negative ? (std::uint64_t{1} << 63) : (std::uint64_t{1} << 63) - 1;
    for (; i < s.size(); ++i) {
        if (!isAsciiDigit(s[i])) return std::nullopt;
        const auto d = static_cast<std::uint64_t>(s[i] - '0');
        if (mag > (limit - d) / 10) return std::nullopt;
        mag = mag * 10 + d;
    }
    return negative ? static_cast<std::int64_t>(std::uint64_t{0} - mag) : static_cast<std::int64_t>(mag);
}

std::optional<bool> parseBool(std::string_view text) {
    const std::string_view s = trimAscii(text);
    if (iequals(s, "true") || iequals(s, "yes") || s == "1") return true;
    if (iequals(s, "false") || iequals(s, "no") || s == "0") return false;
    return std::nullopt;
}

bool isNull(std::string_view text) {
    return iequals(trimAscii(text), "null");
}

PointParse parsePoint(std::string_view text, int dims) {
    PointParse r;
    r.ok = true;
    int count = 0;
    std::size_t start = 0;
    const std::string_view s = trimAscii(text);
    while (true) {
        const std::size_t comma = s.find(',', start);
        const std::string_view part = trimAscii(s.substr(start, comma == std::string_view::npos ? s.npos : comma - start));
        if (count < dims) {
            const FixedParse fp = parseFixed(part);
            if (fp.ok && !fp.decimalComma) {
                r.v[static_cast<std::size_t>(count)] = fp.value;
            } else {
                r.ok = false;
            }
        }
        ++count;
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    r.countMismatch = count != dims;
    return r;
}

std::vector<std::string_view> splitWordList(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ',' || isAsciiSpace(text[i]))) ++i;
        const std::size_t b = i;
        while (i < text.size() && text[i] != ',' && !isAsciiSpace(text[i])) ++i;
        if (i > b) out.push_back(text.substr(b, i - b));
    }
    return out;
}

std::optional<std::string_view> localizedKey(std::string_view text) {
    if (!text.empty() && text.front() == '#') return text.substr(1);
    return std::nullopt;
}

Fixed readFixed(std::string_view text, Fixed fallback, const ValueSite& site) {
    const FixedParse p = parseFixed(text);
    if (!p.ok) {
        if (site.diags != nullptr) {
            site.diags->report(DiagCode::BadValue, site.file, site.line,
                               std::string(site.key) + " = '" + std::string(text) + "' is not a fixed number (" + p.error + ")");
        }
        return fallback;
    }
    if (p.decimalComma && site.diags != nullptr) {
        site.diags->report(DiagCode::DecimalComma, site.file, site.line,
                           std::string(site.key) + " = '" + std::string(text) + "': decimal comma, leading number used");
    }
    return p.value;
}

std::int64_t readInt(std::string_view text, std::int64_t fallback, const ValueSite& site) {
    if (const auto v = parseInt(text)) return *v;
    if (site.diags != nullptr) {
        site.diags->report(DiagCode::BadValue, site.file, site.line,
                           std::string(site.key) + " = '" + std::string(text) + "' is not an integer");
    }
    return fallback;
}

bool readBool(std::string_view text, bool fallback, const ValueSite& site) {
    if (const auto v = parseBool(text)) return *v;
    if (site.diags != nullptr) {
        site.diags->report(DiagCode::BadValue, site.file, site.line,
                           std::string(site.key) + " = '" + std::string(text) + "' is not a bool");
    }
    return fallback;
}

std::array<Fixed, 3> readPoint(std::string_view text, int dims, const ValueSite& site) {
    const PointParse p = parsePoint(text, dims);
    if ((!p.ok || p.countMismatch) && site.diags != nullptr) {
        site.diags->report(DiagCode::BadValue, site.file, site.line,
                           std::string(site.key) + " = '" + std::string(text) + "': expected " + std::to_string(dims) +
                               " fixed components (missing ones are 0)");
    }
    return p.v;
}

}  // namespace corps::tgi
