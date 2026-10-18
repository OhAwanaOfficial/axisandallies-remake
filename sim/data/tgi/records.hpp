#pragma once

// Top-level records and identity policy (TGI_FORMAT §8). The resolver outputs EVERY top-level data
// node as a Record, in load order. Turning records into a definition set is per-type policy:
//   ByIds      key = (type, IDS case-insensitive); a duplicate replaces the earlier one and warns
//   Singleton  later records overlay earlier ones with the normal overlay rules (SVars, Vars)
//   Ordered    keep all, in order (Translate, Text, and every type the registry doesn't know)

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "data/tgi/diagnostics.hpp"
#include "data/tgi/parser.hpp"
#include "data/tgi/resolver.hpp"
#include "data/tgi/symbols.hpp"

namespace corps::tgi {

enum class RecordPolicy : std::uint8_t { ByIds, Singleton, Ordered };

std::string_view policyName(RecordPolicy p);

struct Record {
    Symbol type = kNoSymbol;
    std::string_view typeSpelling;
    const AstNode* node = nullptr;
    const AstFile* file = nullptr;
    ResolvedNode tree;
    std::string_view ids;  // resolved IDS/ids value, "" if none
};

class TypeRegistry {
public:
    // The policies for the types in the current corpus (§8 table).
    static TypeRegistry defaults(SymbolTable& symbols);

    void set(Symbol type, RecordPolicy policy);
    bool known(Symbol type) const;
    RecordPolicy policy(Symbol type) const;  // unknown types: Ordered

private:
    std::vector<std::pair<Symbol, RecordPolicy>> entries;  // sorted by symbol id, probed only
};

struct DefIndex {
    struct ByIdsEntry {
        Symbol type = kNoSymbol;
        std::string idsKey;  // ASCII lower-case IDS
        std::size_t record = 0;
    };
    struct Singleton {
        Symbol type = kNoSymbol;
        std::size_t firstRecord = 0;
        std::vector<std::size_t> records;  // all merged records, in load order
        ResolvedNode merged;
    };

    std::vector<ByIdsEntry> byIds;        // winners only, sorted by (type, idsKey) for lookup
    std::vector<Singleton> singletons;    // first-seen order
    std::vector<std::size_t> ordered;     // record indexes, load order
    std::vector<std::size_t> unknownTypes;  // records whose type the registry doesn't know
    std::vector<std::size_t> replaced;    // ByIds records that lost to a later duplicate

    // Winning record for (type, ids), or nullptr.
    const ByIdsEntry* find(Symbol type, std::string_view ids) const;
};

DefIndex buildDefIndex(const std::vector<Record>& records, const TypeRegistry& registry, Resolver& resolver,
                       Diagnostics& diags);

}  // namespace corps::tgi
