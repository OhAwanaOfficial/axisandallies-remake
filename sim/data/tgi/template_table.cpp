#include "data/tgi/template_table.hpp"

namespace corps::tgi {

TemplateId TemplateTable::lookup(Symbol name) const {
    const auto it = latest.find(name);
    return it == latest.end() ? kNoTemplate : it->second;
}

TemplateId TemplateTable::define(const AstFile& file, const AstNode& node, TemplateId parent) {
    TemplateDef d;
    d.node = &node;
    d.file = &file;
    d.name = node.nameSym;
    d.parent = parent;
    d.previous = lookup(node.nameSym);
    const auto id = static_cast<TemplateId>(defs.size());
    defs.push_back(d);
    latest[node.nameSym] = id;
    return id;
}

void Bindings::resizeFile(std::uint32_t fileId, std::uint32_t nodeCount) {
    if (perFile.size() <= fileId) perFile.resize(static_cast<std::size_t>(fileId) + 1);
    perFile[fileId].assign(nodeCount, kNoTemplate);
}

void Bindings::set(const AstNode& node, TemplateId id) {
    perFile[node.loc.file][node.index] = id;
}

TemplateId Bindings::of(const AstNode& node) const {
    if (node.loc.file >= perFile.size()) return kNoTemplate;
    const auto& v = perFile[node.loc.file];
    return node.index < v.size() ? v[node.index] : kNoTemplate;
}

namespace {

struct Binder {
    const SymbolTable& symbols;
    TemplateTable& table;
    Bindings& bindings;
    Diagnostics& diags;

    TemplateId bindRef(const AstFile& file, const AstNode& node) {
        const TemplateId id = table.lookup(node.parentSym);
        if (id == kNoTemplate) {
            diags.report(DiagCode::UnknownTemplate, file.path, node.loc.line,
                         "[" + node.name + "] refers to template '" + node.parentRef +
                             "', which is not defined before this point");
            return kUnknownTemplate;
        }
        return id;
    }

    void walk(const AstFile& file, const AstNode& node, bool topLevel) {
        TemplateId bound = kNoTemplate;
        if (node.hasParentRef) {
            bound = bindRef(file, node);
        } else if (topLevel && node.kind == NodeKind::Data) {
            bound = table.lookup(node.nameSym);  // implicit same-name template, may be none
        }
        bindings.set(node, bound);
        if (node.kind == NodeKind::Template) {
            const TemplateId prev = table.lookup(node.nameSym);
            if (prev != kNoTemplate) {
                const TemplateDef& p = table.def(prev);
                diags.report(DiagCode::TemplateRedefined, file.path, node.loc.line,
                             "template '" + node.name + "' redefined (previous: " + p.file->path + ":" +
                                 std::to_string(p.node->loc.line) + "); later references bind to this one");
            }
            table.define(file, node, bound);
        }
        for (const AstNode& child : node.children) walk(file, child, false);
    }
};

}  // namespace

void bindTemplates(const std::vector<const AstFile*>& filesInLoadOrder, const SymbolTable& symbols,
                   TemplateTable& table, Bindings& bindings, Diagnostics& diags) {
    Binder binder{symbols, table, bindings, diags};
    for (const AstFile* file : filesInLoadOrder) {
        bindings.resizeFile(file->fileId, file->nodeCount);
        for (const AstNode& node : file->nodes) binder.walk(*file, node, true);
    }
}

}  // namespace corps::tgi
