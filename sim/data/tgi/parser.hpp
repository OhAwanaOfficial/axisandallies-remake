#pragma once

// Lines → per-file AST (TGI_FORMAT §3.3–5). The AST is immutable after parsing and keeps source
// order everywhere; nothing in it is a hash container.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "data/tgi/diagnostics.hpp"
#include "data/tgi/symbols.hpp"

namespace corps::tgi {

enum class NodeKind : std::uint8_t { Template, Decl, Data };

// Type words (§3.4), case-insensitive in the source.
enum class TypeWord : std::uint8_t { None, String, Localized, Flag, Bool, Fixed, Int, Float, Point2, Point3, KF32, FXC };

std::string_view typeWordName(TypeWord t);  // canonical lower-case spelling ("kf32", "fxc")
TypeWord parseTypeWord(std::string_view word);

struct Loc {
    std::uint32_t file = 0;  // index into the loaded-file table
    std::uint32_t line = 0;
};

struct AstField {
    std::string key;  // literal, spaces and '|' kept
    Symbol keySym = kNoSymbol;
    TypeWord type = TypeWord::None;
    bool hasValue = false;  // false: "type name" declaration or bare key
    bool quoted = false;    // the value had an outer "…" pair that was removed
    std::string value;      // §3.5: outer quotes removed, no escape processing
    Loc loc;
};

struct AstAttr {
    std::string key;
    std::string value;
};

struct AstNode {
    NodeKind kind = NodeKind::Data;
    std::string name;  // node type ("Thing", "BodyComponent") or template name
    Symbol nameSym = kNoSymbol;
    bool listMod = false;      // Decl only
    bool optionalMod = false;  // Decl only
    bool hasParentRef = false;
    std::string parentRef;  // from template= / inherit=
    Symbol parentSym = kNoSymbol;
    std::vector<AstAttr> attrs;  // other attrs, e.g. language=
    std::vector<AstField> fields;
    std::vector<AstNode> children;
    bool braced = false;
    bool anonymous = false;  // '{' with no header; dropped from the tree
    Loc loc;
    std::uint32_t index = 0;  // pre-order index within the file (header order)
};

struct AstFile {
    std::uint32_t fileId = 0;
    std::string path;
    std::vector<AstNode> nodes;  // top level, file order
    std::uint32_t nodeCount = 0;  // total nodes, all depths
};

// Parses already-decoded UTF-8 text. Diagnostics carry `path`.
AstFile parseTgi(std::string_view utf8, std::uint32_t fileId, std::string path, SymbolTable& symbols,
                 Diagnostics& diags);

// --- pieces exposed for tests -------------------------------------------------------------------

struct ParsedHeader {
    NodeKind kind = NodeKind::Data;
    std::string name;
    bool listMod = false;
    bool optionalMod = false;
    bool hasParentRef = false;
    std::string parentRef;
    std::vector<AstAttr> attrs;
    bool missingBracket = false;
    std::string trailing;  // text after ']' (trimmed)
    int strayEquals = 0;
    std::vector<std::string> extraWords;
    bool templateAndInherit = false;
};

ParsedHeader parseHeaderLine(std::string_view line);

struct ParsedField {
    bool ok = false;  // false → unparsed-line
    std::string key;
    TypeWord type = TypeWord::None;
    bool hasValue = false;
    bool quoted = false;
    bool doubledQuote = false;  // "#key"" → trailing '"' trimmed
    std::string value;
};

ParsedField parseFieldLine(std::string_view line);

// Debug dump of an AST (simcli dump-tgi).
std::string dumpAst(const AstFile& file);

}  // namespace corps::tgi
