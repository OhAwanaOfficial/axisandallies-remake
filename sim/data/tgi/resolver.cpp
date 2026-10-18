#include "data/tgi/resolver.hpp"

#include <unordered_map>

namespace corps::tgi {
namespace {

// Field lookup for one overlay call. Small nodes scan linearly; big flat nodes (localisation
// tables, SVars) get a probe-only index so overlay stays linear.
class FieldLookup {
public:
    explicit FieldLookup(std::vector<ResolvedField>& fields) : fields(fields) {
        if (fields.size() >= kIndexThreshold) buildIndex();
    }

    ResolvedField& findOrAdd(const AstField& f) {
        if (indexed) {
            if (const auto it = index.find(f.keySym); it != index.end()) return fields[it->second];
        } else {
            for (auto& rf : fields) {
                if (rf.key == f.keySym) return rf;
            }
        }
        ResolvedField rf;
        rf.key = f.keySym;
        rf.spelling = f.key;
        fields.push_back(std::move(rf));
        if (indexed) {
            index.emplace(f.keySym, fields.size() - 1);
        } else if (fields.size() >= kIndexThreshold) {
            buildIndex();
        }
        return fields.back();
    }

private:
    static constexpr std::size_t kIndexThreshold = 48;

    void buildIndex() {
        indexed = true;
        index.reserve(fields.size() * 2);
        for (std::size_t i = 0; i < fields.size(); ++i) index.emplace(fields[i].key, i);
    }

    std::vector<ResolvedField>& fields;
    bool indexed = false;
    std::unordered_map<Symbol, std::size_t> index;  // probe-only
};

std::string_view typeName(TypeWord t) {
    return typeWordName(t);
}

// bool and flag parse the same way (§9.1), so switching between them is not a type change.
bool sameValueType(TypeWord a, TypeWord b) {
    const auto canon = [](TypeWord t) { return t == TypeWord::Flag ? TypeWord::Bool : t; };
    return canon(a) == canon(b);
}

}  // namespace

const ResolvedField* ResolvedNode::field(Symbol key) const {
    for (const auto& f : fields) {
        if (f.key == key) return &f;
    }
    return nullptr;
}

const ResolvedNode* ResolvedNode::child(Symbol childName, std::size_t nth) const {
    for (const auto& c : children) {
        if (c->name == childName) {
            if (nth == 0) return c.get();
            --nth;
        }
    }
    return nullptr;
}

std::size_t ResolvedNode::childCount(Symbol childName) const {
    std::size_t n = 0;
    for (const auto& c : children) n += (c->name == childName) ? 1 : 0;
    return n;
}

const ChildDecl* ResolvedNode::decl(Symbol declName) const {
    for (const auto& d : decls) {
        if (d.name == declName) return &d;
    }
    return nullptr;
}

Resolver::Resolver(const TemplateTable& table, const Bindings& bindings, std::vector<const AstFile*> filesById,
                   Diagnostics& diags, ResolveOptions options)
    : table(table), bindings(bindings), files(std::move(filesById)), diags(diags), options(options) {
    memo.resize(table.size());
    state.assign(table.size(), 0);
}

const std::string& Resolver::pathOf(const AstNode& n) const {
    static const std::string kUnknown = "?";
    return n.loc.file < files.size() && files[n.loc.file] != nullptr ? files[n.loc.file]->path : kUnknown;
}

const ResolvedNode& Resolver::resolveTemplate(TemplateId id) {
    if (id < 0 || static_cast<std::size_t>(id) >= memo.size()) return empty;
    const auto i = static_cast<std::size_t>(id);
    if (state[i] == 2) return *memo[i];
    const TemplateDef& def = table.def(id);
    if (state[i] == 1) {
        diags.report(DiagCode::TemplateCycle, pathOf(*def.node), def.node->loc.line,
                     "template '" + def.node->name + "' inherits from itself; resolved with an empty base");
        return empty;
    }
    state[i] = 1;
    ResolvedNode r = startFrom(def.parent);
    r.name = def.name;
    r.spelling = def.node->name;
    r.node = def.node;
    r.base = def.parent;
    r.origin = id;
    overlay(r, *def.node, id);
    memo[i] = std::make_unique<ResolvedNode>(std::move(r));
    state[i] = 2;
    return *memo[i];
}

ResolvedNode Resolver::resolveTopLevel(const AstNode& node) {
    const TemplateId bound = bindings.of(node);
    ResolvedNode r = startFrom(bound);
    stamp(r, node, kOriginInstance);
    r.base = bound;
    overlay(r, node, kOriginInstance);
    return r;
}

ResolvedNode Resolver::startFrom(TemplateId id) {
    if (id < 0) return ResolvedNode{};
    return resolveTemplate(id);  // copy
}

void Resolver::stamp(ResolvedNode& n, const AstNode& from, TemplateId origin) const {
    n.name = from.nameSym;
    n.spelling = from.name;
    n.node = &from;
    n.origin = origin;
}

ResolvedNode Resolver::declItemBase(const ChildDecl& d) {
    const auto key = std::make_pair(d.node->loc.file, d.node->index);
    if (const auto it = declMemo.find(key); it != declMemo.end()) return it->second;
    ResolvedNode r = startFrom(d.base);
    r.base = d.base;
    r.origin = d.origin;
    r.node = d.node;
    if (!d.node->fields.empty() || !d.node->children.empty()) overlay(r, *d.node, d.origin);  // inline item schema
    declMemo.emplace(key, r);
    return r;
}

void Resolver::overlay(ResolvedNode& r, const AstNode& a, TemplateId origin) {
    {
        FieldLookup lookup(r.fields);
        for (const AstField& f : a.fields) {
            ResolvedField& rf = lookup.findOrAdd(f);
            if (f.type != TypeWord::None) {
                if (rf.type != TypeWord::None && !sameValueType(rf.type, f.type) && rf.decl != nullptr) {
                    diags.report(DiagCode::TypeRedeclared, pathOf(a), f.loc.line,
                                 "'" + f.key + "' redeclared as " + std::string(typeName(f.type)) + " (was " +
                                     std::string(typeName(rf.type)) + " at " +
                                     (rf.decl->loc.file < files.size() ? files[rf.decl->loc.file]->path : "?") + ":" +
                                     std::to_string(rf.decl->loc.line) + ")");
                }
                rf.type = f.type;
                rf.decl = &f;
                rf.declOrigin = origin;
            }
            if (f.hasValue) {
                rf.value = &f;
                rf.cleared = nullptr;
                rf.valueOrigin = origin;
            } else if (f.type == TypeWord::None) {  // bare key: required, clears the inherited value
                rf.required = true;
                rf.value = nullptr;
                rf.cleared = &f;
                rf.valueOrigin = origin;
            }
            if (options.trackHistory) rf.history.push_back({&f, origin});
        }
    }

    // Names that occur more than once among A's own data children.
    std::vector<std::pair<Symbol, int>> counts;
    if (a.children.size() > 1) {
        for (const AstNode& c : a.children) {
            if (c.kind != NodeKind::Data) continue;
            bool found = false;
            for (auto& [sym, n] : counts) {
                if (sym == c.nameSym) {
                    ++n;
                    found = true;
                    break;
                }
            }
            if (!found) counts.emplace_back(c.nameSym, 1);
        }
    }
    const auto repeated = [&](Symbol s) {
        for (const auto& [sym, n] : counts) {
            if (sym == s) return n > 1;
        }
        return false;
    };

    for (const AstNode& c : a.children) {
        switch (c.kind) {
            case NodeKind::Template:
                break;  // nested template: warned by the parser, registered by the binder
            case NodeKind::Decl: {
                ChildDecl d;
                d.name = c.nameSym;
                d.spelling = c.name;
                d.list = c.listMod;
                d.optional = c.optionalMod;
                d.base = bindings.of(c);
                d.node = &c;
                d.origin = origin;
                bool replaced = false;
                for (auto& existing : r.decls) {
                    if (existing.name == d.name) {
                        existing = d;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) r.decls.push_back(d);
                break;
            }
            case NodeKind::Data:
                applyChild(r, a, c, origin, repeated(c.nameSym));
                break;
        }
    }
}

void Resolver::applyChild(ResolvedNode& r, const AstNode& parentAst, const AstNode& c, TemplateId origin,
                          bool repeatedHere) {
    const ChildDecl* decl = r.decl(c.nameSym);
    const TemplateId bound = c.hasParentRef ? bindings.of(c) : kNoTemplate;

    bool asList = decl != nullptr && decl->list;
    if (!asList && (repeatedHere || r.childCount(c.nameSym) > 1)) {
        asList = true;
        // Report once per (AST node, name): at the first repeat of that name in this node.
        bool firstRepeat = true;
        bool seenSelf = false;
        std::size_t before = 0;
        for (const AstNode& s : parentAst.children) {
            if (&s == &c) {
                seenSelf = true;
                break;
            }
            if (s.kind == NodeKind::Data && s.nameSym == c.nameSym) ++before;
        }
        firstRepeat = seenSelf && (repeatedHere ? before == 1 : before == 0);
        if (firstRepeat) {
            diags.report(DiagCode::UndeclaredRepeat, pathOf(c), c.loc.line,
                         "[" + c.name + "] repeated in [" + parentAst.name + "] without a [list " + c.name +
                             "] declaration; treated as a list");
        }
    }

    const auto makeFresh = [&]() {
        ResolvedNode fresh;
        if (c.hasParentRef) {
            fresh = startFrom(bound);
            fresh.base = bound;
        } else if (decl != nullptr) {
            fresh = declItemBase(*decl);
        }
        stamp(fresh, c, origin);
        overlay(fresh, c, origin);
        return fresh;
    };

    if (asList) {
        r.children.push_back(std::make_shared<const ResolvedNode>(makeFresh()));
        return;
    }
    std::size_t existing = r.children.size();
    for (std::size_t i = 0; i < r.children.size(); ++i) {
        if (r.children[i]->name == c.nameSym) {
            existing = i;
            break;
        }
    }
    if (existing == r.children.size()) {
        r.children.push_back(std::make_shared<const ResolvedNode>(makeFresh()));
    } else if (c.hasParentRef) {
        r.children[existing] = std::make_shared<const ResolvedNode>(makeFresh());  // rebase, same position
    } else {
        // Deep merge into a private copy (copy-on-write).
        auto merged = std::make_shared<ResolvedNode>(*r.children[existing]);
        overlay(*merged, c, origin);
        r.children[existing] = std::move(merged);
    }
}

}  // namespace corps::tgi
