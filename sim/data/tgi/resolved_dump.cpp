#include "data/tgi/resolved_dump.hpp"

#include <map>
#include <vector>

#include "core/ascii.hpp"

namespace corps::tgi {
namespace {

enum class RecordStatus : std::uint8_t { Ordered, Winner, Replaced, SingletonFirst, SingletonPart };

std::vector<RecordStatus> recordStatuses(const TgiDatabase& db) {
    std::vector<RecordStatus> st(db.records.size(), RecordStatus::Ordered);
    for (const auto& e : db.defs.byIds) st[e.record] = RecordStatus::Winner;
    for (std::size_t i : db.defs.replaced) st[i] = RecordStatus::Replaced;
    for (const auto& s : db.defs.singletons) {
        for (std::size_t i : s.records) st[i] = RecordStatus::SingletonPart;
        st[s.firstRecord] = RecordStatus::SingletonFirst;
    }
    return st;
}

std::string_view statusName(RecordStatus s) {
    switch (s) {
        case RecordStatus::Ordered: return "ordered";
        case RecordStatus::Winner: return "winner";
        case RecordStatus::Replaced: return "replaced";
        case RecordStatus::SingletonFirst: return "singleton";
        case RecordStatus::SingletonPart: return "singleton-part";
    }
    return "?";
}

void indentTo(std::string& out, int depth) {
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
}

void canonNode(std::string& out, const ResolvedNode& n, int depth) {
    indentTo(out, depth);
    out += "[";
    out += n.spelling;
    out += "]\n";
    for (const auto& f : n.fields) {
        indentTo(out, depth + 1);
        out += f.spelling;
        if (f.type != TypeWord::None) {
            out += " : ";
            out += typeWordName(f.type);
        }
        if (f.value != nullptr) {
            out += " = ";
            if (f.value->quoted) {
                out += "\"" + f.value->value + "\"";
            } else {
                out += f.value->value;
            }
        } else if (f.required) {
            out += " !";
        }
        out += "\n";
    }
    for (const auto& c : n.children) canonNode(out, *c, depth + 1);
}

std::string loc(const TgiDatabase& db, const AstField& f) {
    return db.pathOf(f.loc.file) + ":" + std::to_string(f.loc.line);
}

std::string loc(const TgiDatabase& db, const AstNode& n) {
    return db.pathOf(n.loc.file) + ":" + std::to_string(n.loc.line);
}

std::string renderValue(const AstField& f) {
    return f.quoted ? "\"" + f.value + "\"" : f.value;
}

// Label for a child: "Name", or "Name[k]" when it is one of several or a declared list item.
std::string childLabel(const ResolvedNode& parent, const ResolvedNode& child) {
    std::size_t ordinal = 0;
    std::size_t total = 0;
    for (const auto& c : parent.children) {
        if (c->name != child.name) continue;
        if (c.get() == &child) ordinal = total;
        ++total;
    }
    const ChildDecl* d = parent.decl(child.name);
    if (total > 1 || (d != nullptr && d->list)) return std::string(child.spelling) + "[" + std::to_string(ordinal) + "]";
    return std::string(child.spelling);
}

std::string describeField(const TgiDatabase& db, const ResolvedField& f) {
    std::string out;
    if (f.value != nullptr) {
        out += renderValue(*f.value);
    } else if (f.cleared != nullptr) {
        out += "(cleared)";
    } else {
        out += "(unset)";
    }
    out += "   ; ";
    out += f.type != TypeWord::None ? std::string(typeWordName(f.type)) : std::string("untyped");
    if (f.required) out += ", required";
    if (f.value != nullptr) {
        out += "  " + loc(db, *f.value) + " (" + db.originName(f.valueOrigin) + ")";
    } else if (f.cleared != nullptr) {
        out += "  " + loc(db, *f.cleared) + " (" + db.originName(f.valueOrigin) + ")";
    } else if (f.decl != nullptr) {
        out += "  declared " + loc(db, *f.decl) + " (" + db.originName(f.declOrigin) + ")";
    }
    return out;
}

void provenanceNode(std::string& out, const TgiDatabase& db, const ResolvedNode& n, const std::string& label,
                    int depth) {
    indentTo(out, depth);
    out += "[" + label + "]";
    if (n.base >= 0) out += "  template=" + db.originName(n.base);
    if (n.node != nullptr) out += "  @" + loc(db, *n.node);
    out += "\n";
    for (const auto& f : n.fields) {
        indentTo(out, depth + 1);
        out += std::string(f.spelling) + " = " + describeField(db, f) + "\n";
        if (f.history.size() > 1) {
            for (std::size_t i = 0; i + 1 < f.history.size(); ++i) {
                const FieldWrite& w = f.history[i];
                indentTo(out, depth + 3);
                out += "was ";
                if (w.field->hasValue) {
                    out += renderValue(*w.field);
                } else if (w.field->type != TypeWord::None) {
                    out += "(declared " + std::string(typeWordName(w.field->type)) + ")";
                } else {
                    out += "(cleared)";
                }
                out += "  " + loc(db, *w.field) + " (" + db.originName(w.origin) + ")\n";
            }
        }
    }
    for (const auto& c : n.children) provenanceNode(out, db, *c, childLabel(n, *c), depth + 1);
}

// JSON -------------------------------------------------------------------------------------------

void jsonString(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[u >> 4];
                    out += kHex[u & 15];
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void jsonNode(std::string& out, const TgiDatabase& db, const ResolvedNode& n) {
    out += "{\"name\":";
    jsonString(out, n.spelling);
    if (n.base >= 0) {
        out += ",\"template\":";
        jsonString(out, db.originName(n.base));
    }
    if (n.node != nullptr) {
        out += ",\"file\":" + std::to_string(n.node->loc.file) + ",\"line\":" + std::to_string(n.node->loc.line);
    }
    out += ",\"fields\":[";
    bool first = true;
    for (const auto& f : n.fields) {
        if (!first) out += ',';
        first = false;
        out += "{\"key\":";
        jsonString(out, f.spelling);
        if (f.type != TypeWord::None) {
            out += ",\"type\":";
            jsonString(out, typeWordName(f.type));
        }
        out += ",\"value\":";
        if (f.value != nullptr) {
            jsonString(out, f.value->value);
            if (f.value->quoted) out += ",\"quoted\":true";
            out += ",\"file\":" + std::to_string(f.value->loc.file) + ",\"line\":" + std::to_string(f.value->loc.line);
            out += ",\"origin\":";
            jsonString(out, db.originName(f.valueOrigin));
        } else {
            out += "null";
        }
        if (f.required) out += ",\"required\":true";
        out += '}';
    }
    out += "],\"children\":[";
    first = true;
    for (const auto& c : n.children) {
        if (!first) out += ',';
        first = false;
        jsonNode(out, db, *c);
    }
    out += "]}";
}

struct Agg {
    std::size_t count = 0;
    std::string first;
};

void collectUndeclared(const TgiDatabase& db, const ResolvedNode& n, const std::string& prefix,
                       std::map<std::string, Agg>& out) {
    for (const auto& f : n.fields) {
        if (f.type != TypeWord::None || f.required || f.value == nullptr) continue;
        Agg& a = out[prefix + "." + asciiLowerCopy(f.spelling)];
        if (a.count++ == 0) a.first = loc(db, *f.value) + " (" + db.originName(f.valueOrigin) + ")";
    }
    for (const auto& c : n.children) collectUndeclared(db, *c, prefix + "/" + asciiLowerCopy(c->spelling), out);
}

}  // namespace

std::uint64_t fnv1a64(std::string_view bytes, std::uint64_t seed) {
    std::uint64_t h = seed;
    for (char c : bytes) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

std::string hex64(std::uint64_t v) {
    static const char* kHex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[v & 15];
        v >>= 4;
    }
    return out;
}

std::string dumpCanonical(const ResolvedNode& node) {
    std::string out;
    canonNode(out, node, 0);
    return out;
}

std::string dumpDefinitionSet(const TgiDatabase& db) {
    const std::vector<RecordStatus> st = recordStatuses(db);
    std::string out;
    for (std::size_t i = 0; i < db.records.size(); ++i) {
        const Record& r = db.records[i];
        switch (st[i]) {
            case RecordStatus::Replaced:
            case RecordStatus::SingletonPart:
                continue;
            case RecordStatus::SingletonFirst: {
                for (const auto& s : db.defs.singletons) {
                    if (s.firstRecord != i) continue;
                    out += "== singleton " + std::string(r.typeSpelling) + "\n";
                    canonNode(out, s.merged, 0);
                }
                continue;
            }
            case RecordStatus::Winner:
                out += "== " + std::string(r.typeSpelling) + " " + std::string(r.ids) + "\n";
                break;
            case RecordStatus::Ordered:
                out += "== ordered " + std::string(r.typeSpelling) + "\n";
                break;
        }
        canonNode(out, r.tree, 0);
    }
    return out;
}

std::string dumpWithProvenance(const TgiDatabase& db, const ResolvedNode& node, int depth) {
    std::string out;
    provenanceNode(out, db, node, std::string(node.spelling), depth);
    return out;
}

std::string describeChain(const TgiDatabase& db, const Record& record) {
    std::string out = std::string(record.typeSpelling) + " (" + loc(db, *record.node) + ")";
    TemplateId t = record.tree.base;
    if (t == kUnknownTemplate) out += " -> ? (unknown template '" + record.node->parentRef + "')";
    int guard = 0;
    while (t >= 0 && guard++ < 256) {
        const TemplateDef& d = db.templates.def(t);
        out += " -> " + d.node->name + " (" + d.file->path + ":" + std::to_string(d.node->loc.line) + ")";
        if (d.parent == kUnknownTemplate) out += " -> ? (unknown template '" + d.node->parentRef + "')";
        t = d.parent;
    }
    return out;
}

bool explainRecord(const TgiDatabase& db, const Record& record, std::string_view path, std::string& out) {
    out += std::string(record.typeSpelling) + " " + std::string(record.ids) + "\n";
    out += "chain: " + describeChain(db, record) + "\n";
    if (path.empty()) {
        out += dumpWithProvenance(db, record.tree);
        return true;
    }

    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = path.find('/', start);
        segments.push_back(path.substr(start, slash == std::string_view::npos ? path.npos : slash - start));
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    std::string_view fieldName;
    std::string_view& last = segments.back();
    if (const std::size_t dot = last.find('.'); dot != std::string_view::npos) {
        fieldName = last.substr(dot + 1);
        last = last.substr(0, dot);
    }

    const ResolvedNode* node = &record.tree;
    std::string walked;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        std::string_view seg = segments[i];
        if (seg.empty()) continue;
        std::size_t nth = 0;
        if (const std::size_t br = seg.find('['); br != std::string_view::npos && seg.back() == ']') {
            for (char c : seg.substr(br + 1, seg.size() - br - 2)) {
                if (!isAsciiDigit(c)) {
                    out += "bad index in '" + std::string(seg) + "'\n";
                    return false;
                }
                nth = nth * 10 + static_cast<std::size_t>(c - '0');
            }
            seg = seg.substr(0, br);
        }
        const ResolvedNode* next = nullptr;
        std::size_t seen = 0;
        for (const auto& c : node->children) {
            if (!iequals(c->spelling, seg)) continue;
            if (seen++ == nth) {
                next = c.get();
                break;
            }
        }
        // A single last segment with no '.' may name a field of the current node.
        if (next == nullptr && i + 1 == segments.size() && fieldName.empty() && seg == segments[i]) {
            fieldName = seg;
            break;
        }
        if (next == nullptr) {
            out += "no child '" + std::string(segments[i]) + "' under [" + std::string(node->spelling) + "]" +
                   (walked.empty() ? "" : " (" + walked + ")") + "; children:";
            for (const auto& c : node->children) out += " " + childLabel(*node, *c);
            out += "\n";
            return false;
        }
        walked += (walked.empty() ? "" : "/") + childLabel(*node, *next);
        node = next;
    }

    if (fieldName.empty()) {
        out += dumpWithProvenance(db, *node);
        return true;
    }
    for (const auto& f : node->fields) {
        if (!iequals(f.spelling, fieldName)) continue;
        out += (walked.empty() ? "" : walked + ".") + std::string(f.spelling) + " = " + describeField(db, f) + "\n";
        for (std::size_t i = 0; i + 1 < f.history.size(); ++i) {
            const FieldWrite& w = f.history[i];
            out += "    was ";
            if (w.field->hasValue) {
                out += renderValue(*w.field);
            } else if (w.field->type != TypeWord::None) {
                out += "(declared " + std::string(typeWordName(w.field->type)) + ")";
            } else {
                out += "(cleared)";
            }
            out += "  " + loc(db, *w.field) + " (" + db.originName(w.origin) + ")\n";
        }
        return true;
    }
    out += "no field '" + std::string(fieldName) + "' in [" + std::string(node->spelling) + "]\n";
    return false;
}

std::string definitionsJson(const TgiDatabase& db) {
    const std::vector<RecordStatus> st = recordStatuses(db);
    std::string out = "{\"format\":\"corps-tgi-defs/1\",\"files\":[";
    for (std::size_t i = 0; i < db.files.size(); ++i) {
        const LoadedFile& f = db.files[i];
        if (i != 0) out += ',';
        out += "{\"id\":" + std::to_string(i) + ",\"path\":";
        jsonString(out, f.path);
        out += ",\"depot\":";
        jsonString(out, f.depot);
        out += ",\"encoding\":";
        jsonString(out, encodingName(f.encoding));
        out += ",\"list\":";
        jsonString(out, f.listPath);
        out += ",\"list_line\":" + std::to_string(f.listLine) + "}";
    }
    out += "],\"records\":[";
    for (std::size_t i = 0; i < db.records.size(); ++i) {
        const Record& r = db.records[i];
        if (i != 0) out += ',';
        out += "{\"index\":" + std::to_string(i) + ",\"type\":";
        jsonString(out, r.typeSpelling);
        out += ",\"ids\":";
        if (r.ids.empty()) {
            out += "null";
        } else {
            jsonString(out, r.ids);
        }
        out += ",\"policy\":";
        jsonString(out, policyName(db.registry.policy(r.type)));
        out += ",\"status\":";
        jsonString(out, statusName(st[i]));
        out += ",\"known_type\":";
        out += db.registry.known(r.type) ? "true" : "false";
        out += ",\"tree\":";
        jsonNode(out, db, r.tree);
        out += '}';
    }
    out += "],\"singletons\":[";
    for (std::size_t i = 0; i < db.defs.singletons.size(); ++i) {
        const auto& s = db.defs.singletons[i];
        if (i != 0) out += ',';
        out += "{\"type\":";
        jsonString(out, db.records[s.firstRecord].typeSpelling);
        out += ",\"records\":[";
        for (std::size_t k = 0; k < s.records.size(); ++k) out += (k != 0 ? "," : "") + std::to_string(s.records[k]);
        out += "],\"tree\":";
        jsonNode(out, db, s.merged);
        out += '}';
    }
    out += "],\"diagnostics\":[";
    for (std::size_t i = 0; i < db.diags.all().size(); ++i) {
        const Diagnostic& d = db.diags.all()[i];
        if (i != 0) out += ',';
        out += "{\"severity\":";
        jsonString(out, severityName(d.severity));
        out += ",\"code\":";
        jsonString(out, codeName(d.code));
        out += ",\"file\":";
        jsonString(out, d.file);
        out += ",\"line\":" + std::to_string(d.line) + ",\"message\":";
        jsonString(out, d.message);
        out += '}';
    }
    out += "]}\n";
    return out;
}

std::string reportUnknown(const TgiDatabase& db) {
    std::map<std::string, Agg> types;
    for (std::size_t i : db.defs.unknownTypes) {
        const Record& r = db.records[i];
        Agg& a = types[std::string(r.typeSpelling)];
        if (a.count++ == 0) a.first = loc(db, *r.node);
    }
    std::string out = "unknown top-level types: " + std::to_string(types.size()) + " (" +
                      std::to_string(db.defs.unknownTypes.size()) + " records, kept as ordered records)\n";
    for (const auto& [name, a] : types) out += "  " + name + "  " + std::to_string(a.count) + "  first " + a.first + "\n";

    std::map<std::string, Agg> fields;
    for (const Record& r : db.records) collectUndeclared(db, r.tree, asciiLowerCopy(r.typeSpelling), fields);
    out += "undeclared fields (set, never declared with a type, not consumed yet): " + std::to_string(fields.size()) +
           " distinct paths\n";
    for (const auto& [path, a] : fields) out += "  " + path + "  " + std::to_string(a.count) + "  first " + a.first + "\n";
    return out;
}

}  // namespace corps::tgi
