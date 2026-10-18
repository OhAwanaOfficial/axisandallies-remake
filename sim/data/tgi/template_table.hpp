#pragma once

// One global, case-insensitive template namespace with SEQUENTIAL binding (TGI_FORMAT §7.1):
// every reference binds to the most recent definition already loaded (earlier in the same file or
// in an earlier file), and that binding never changes. A later redefinition affects only later
// references.

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "data/tgi/diagnostics.hpp"
#include "data/tgi/parser.hpp"
#include "data/tgi/symbols.hpp"

namespace corps::tgi {

using TemplateId = std::int32_t;
constexpr TemplateId kNoTemplate = -1;       // nothing referenced
constexpr TemplateId kUnknownTemplate = -2;  // referenced, but not defined yet (error reported)

struct TemplateDef {
    const AstNode* node = nullptr;
    const AstFile* file = nullptr;
    Symbol name = kNoSymbol;
    TemplateId parent = kNoTemplate;    // bound at the header
    TemplateId previous = kNoTemplate;  // definition this one replaced, if any
};

class TemplateTable {
public:
    TemplateId lookup(Symbol name) const;  // latest definition so far, or kNoTemplate
    TemplateId define(const AstFile& file, const AstNode& node, TemplateId parent);

    const TemplateDef& def(TemplateId id) const { return defs[static_cast<std::size_t>(id)]; }
    std::size_t size() const { return defs.size(); }

private:
    std::vector<TemplateDef> defs;
    std::unordered_map<Symbol, TemplateId> latest;  // probe-only
};

// Per-node bindings: [fileId][node.index]. For a node with template=/inherit= this is the bound
// definition (or kUnknownTemplate). For a TOP-LEVEL data node without one it is the same-named
// template if one exists (§7.3 rule 3). Otherwise kNoTemplate.
class Bindings {
public:
    void resizeFile(std::uint32_t fileId, std::uint32_t nodeCount);
    void set(const AstNode& node, TemplateId id);
    TemplateId of(const AstNode& node) const;

private:
    std::vector<std::vector<TemplateId>> perFile;
};

// Walks the files in load order (pre-order within each file: a header is seen before its body),
// binding references and registering templates. A template's own parent is bound BEFORE the
// template registers, so "[Template Foo inherit=Foo]" extends the previous Foo.
void bindTemplates(const std::vector<const AstFile*>& filesInLoadOrder, const SymbolTable& symbols,
                   TemplateTable& table, Bindings& bindings, Diagnostics& diags);

}  // namespace corps::tgi
