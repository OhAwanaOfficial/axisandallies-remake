#include "vfs/path.hpp"

#include "core/ascii.hpp"

namespace corps::vfs {

std::string normalizePath(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        const char s = (c == '\\') ? '/' : c;
        if (s == '/' && (out.empty() || out.back() == '/')) continue;  // leading or doubled '/'
        out.push_back(s);
    }
    while (out.size() >= 2 && out[0] == '.' && out[1] == '/') out.erase(0, 2);
    if (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

std::string pathKey(std::string_view path) {
    std::string out = normalizePath(path);
    for (char& c : out) c = asciiLower(c);
    return out;
}

std::string_view parentDir(std::string_view normalizedPath) {
    const std::size_t slash = normalizedPath.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : normalizedPath.substr(0, slash);
}

std::string_view fileName(std::string_view normalizedPath) {
    const std::size_t slash = normalizedPath.rfind('/');
    return slash == std::string_view::npos ? normalizedPath : normalizedPath.substr(slash + 1);
}

bool globMatch(std::string_view pattern, std::string_view name) {
    // Iterative wildcard match with single-star backtracking.
    std::size_t p = 0;
    std::size_t n = 0;
    std::size_t starP = std::string_view::npos;
    std::size_t starN = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starN = n;
        } else if (p < pattern.size() && asciiLower(pattern[p]) == asciiLower(name[n])) {
            ++p;
            ++n;
        } else if (starP != std::string_view::npos) {
            p = starP + 1;
            n = ++starN;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool upperOrdinalLess(std::string_view a, std::string_view b) {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto ca = static_cast<unsigned char>(asciiUpper(a[i]));
        const auto cb = static_cast<unsigned char>(asciiUpper(b[i]));
        if (ca != cb) return ca < cb;
    }
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;  // identical ignoring case: fall back to exact bytes so the order is total
}

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    const std::u8string u8(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(u8);
}

std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

}  // namespace corps::vfs
