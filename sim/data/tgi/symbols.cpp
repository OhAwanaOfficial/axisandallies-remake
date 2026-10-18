#include "data/tgi/symbols.hpp"

#include "core/ascii.hpp"

namespace corps::tgi {

std::size_t SymbolTable::Hash::operator()(std::string_view s) const noexcept {
    std::uint64_t h = 1469598103934665603ULL;  // FNV-1a
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    return static_cast<std::size_t>(h);
}

SymbolTable::SymbolTable() {
    names.emplace_back();
    lookup.emplace(std::string(), kNoSymbol);
}

Symbol SymbolTable::intern(std::string_view spelling) {
    scratch.assign(spelling);
    for (char& c : scratch) c = asciiLower(c);
    if (const auto it = lookup.find(std::string_view(scratch)); it != lookup.end()) return it->second;
    const auto id = static_cast<Symbol>(names.size());
    names.push_back(scratch);
    lookup.emplace(scratch, id);
    return id;
}

Symbol SymbolTable::find(std::string_view spelling) const {
    const std::string key = asciiLowerCopy(spelling);
    const auto it = lookup.find(std::string_view(key));
    return it == lookup.end() ? kNoSymbol : it->second;
}

}  // namespace corps::tgi
