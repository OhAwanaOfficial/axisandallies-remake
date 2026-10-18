#pragma once

// Template resolver (TGI_FORMAT §7.2–7.5): AST + bindings → ResolvedNode trees with no
// inheritance left, carrying schema (declared types, child declarations) and per-field provenance.
//
//   resolve(node) = overlay(copy(resolve(base)), node)
//   overlay(R, A), in A's source order:
//     field k = v                          replace value (provenance → A's file:line)
//     child declared as a list in R        append item = overlay(resolve(itemBase), child)
//     named child, no template=, existing  deep merge: overlay(existing, child)
//     named child, template=, existing     rebase: replace with overlay(resolve(T), child)
//     named child, nothing existing        create from §7.3, then overlay
//     same name repeated, no list decl     treated as a list (undeclared-repeat)

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "data/tgi/diagnostics.hpp"
#include "data/tgi/parser.hpp"
#include "data/tgi/symbols.hpp"
#include "data/tgi/template_table.hpp"

namespace corps::tgi {

// Which body wrote something: a TemplateId, or kOriginInstance for a top-level data node.
constexpr TemplateId kOriginInstance = -1;

struct FieldWrite {
    const AstField* field = nullptr;
    TemplateId origin = kOriginInstance;
};

struct ResolvedField {
    Symbol key = kNoSymbol;
    std::string_view spelling;            // first spelling seen
    TypeWord type = TypeWord::None;       // declared type; None = never declared with a type
    const AstField* decl = nullptr;       // latest typed declaration
    const AstField* value = nullptr;      // latest assignment; nullptr = unset or cleared
    const AstField* cleared = nullptr;    // the bare key that cleared it (when value == nullptr)
    TemplateId declOrigin = kOriginInstance;
    TemplateId valueOrigin = kOriginInstance;
    bool required = false;                // declared as a bare key somewhere in the chain
    std::vector<FieldWrite> history;      // every write in order (only with ResolveOptions::trackHistory)

    bool hasValue() const { return value != nullptr; }
    std::string_view text() const { return value != nullptr ? std::string_view(value->value) : std::string_view{}; }
};

struct ChildDecl {
    Symbol name = kNoSymbol;
    std::string_view spelling;
    bool list = false;
    bool optional = false;
    TemplateId base = kNoTemplate;  // bound item/child template
    const AstNode* node = nullptr;  // declaration node: inline item schema and location
    TemplateId origin = kOriginInstance;
};

struct ResolvedNode;

// Children are shared copy-on-write: copying a node copies its fields and bumps the children's
// reference counts. The resolver clones a child only when an overlay changes it, so instances share
// every inherited subtree they don't touch. A shared node is never mutated.
using NodeRef = std::shared_ptr<const ResolvedNode>;

struct ResolvedNode {
    Symbol name = kNoSymbol;
    std::string_view spelling;
    const AstNode* node = nullptr;       // AST node that created (or last rebased) it
    TemplateId base = kNoTemplate;       // template it started from (kNoTemplate: decl or empty)
    TemplateId origin = kOriginInstance; // body that created it
    std::vector<ResolvedField> fields;   // declaration order, inherited first
    std::vector<NodeRef> children;       // inherited first, then local additions in source order
    std::vector<ChildDecl> decls;

    const ResolvedField* field(Symbol key) const;
    const ResolvedNode* child(Symbol name, std::size_t nth = 0) const;
    std::size_t childCount(Symbol name) const;
    const ChildDecl* decl(Symbol name) const;
};

struct ResolveOptions {
    bool trackHistory = false;  // keep every write per field (for --explain); costs memory
};

class Resolver {
public:
    // `filesById[i]` must be the AST with fileId i.
    Resolver(const TemplateTable& table, const Bindings& bindings, std::vector<const AstFile*> filesById,
             Diagnostics& diags, ResolveOptions options = {});

    // Memoised. kNoTemplate / kUnknownTemplate / a cycle give an empty node.
    const ResolvedNode& resolveTemplate(TemplateId id);

    // A top-level data node: its explicit template, else a same-named template (§7.3), else empty.
    ResolvedNode resolveTopLevel(const AstNode& node);

    // Overlays a later top-level record onto an already resolved one (singleton merge, §8).
    void overlayRecord(ResolvedNode& target, const AstNode& node) { overlay(target, node, kOriginInstance); }

private:
    ResolvedNode startFrom(TemplateId id);
    ResolvedNode declItemBase(const ChildDecl& decl);
    void overlay(ResolvedNode& r, const AstNode& a, TemplateId origin);
    void applyChild(ResolvedNode& r, const AstNode& parentAst, const AstNode& c, TemplateId origin, bool repeatedHere);
    void stamp(ResolvedNode& n, const AstNode& from, TemplateId origin) const;
    const std::string& pathOf(const AstNode& n) const;

    const TemplateTable& table;
    const Bindings& bindings;
    std::vector<const AstFile*> files;
    Diagnostics& diags;
    ResolveOptions options;

    std::vector<std::unique_ptr<ResolvedNode>> memo;
    std::vector<std::uint8_t> state;  // 0 new, 1 in progress, 2 done
    std::map<std::pair<std::uint32_t, std::uint32_t>, ResolvedNode> declMemo;  // (file, node index)
    ResolvedNode empty;
};

}  // namespace corps::tgi
