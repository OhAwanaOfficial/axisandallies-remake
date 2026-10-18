#pragma once

// Locale-free ASCII helpers. TGI identifiers and VFS paths compare ASCII case-insensitively
// (TGI_FORMAT §2); never use <cctype> or std::locale for that.

#include <string>
#include <string_view>

namespace corps {

constexpr char asciiLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr char asciiUpper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

constexpr bool isAsciiSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r' || c == '\n';
}

constexpr bool isAsciiDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

inline std::string asciiLowerCopy(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = asciiLower(c);
    return out;
}

inline std::string asciiUpperCopy(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = asciiUpper(c);
    return out;
}

constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) return false;
    }
    return true;
}

constexpr bool istartsWith(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

constexpr bool iendsWith(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

constexpr std::string_view trimAscii(std::string_view s) noexcept {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && isAsciiSpace(s[b])) ++b;
    while (e > b && isAsciiSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

}  // namespace corps
