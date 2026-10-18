#pragma once

// Interned identifiers (TGI_FORMAT §11.1). A Symbol is the ASCII-lower-cased spelling; original
// spellings stay in the AST for messages and dumps. Ids are handed out in first-seen order, which
// is load order, so they are deterministic, but they are NOT alphabetical: never sort output by id.
// The hash map is only ever probed, never iterated.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace corps::tgi {

using Symbol = std::uint32_t;
constexpr Symbol kNoSymbol = 0;  // id 0 is the empty string

class SymbolTable {
public:
    SymbolTable();

    Symbol intern(std::string_view spelling);           // folds case
    Symbol find(std::string_view spelling) const;       // kNoSymbol if never interned
    const std::string& folded(Symbol s) const { return names[s]; }
    std::size_t size() const { return names.size(); }

private:
    struct Hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept;
    };
    struct Eq {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    };

    std::vector<std::string> names;
    std::unordered_map<std::string, Symbol, Hash, Eq> lookup;  // probe-only
    std::string scratch;  // reused fold buffer, so lookups of known symbols don't allocate
};

}  // namespace corps::tgi
