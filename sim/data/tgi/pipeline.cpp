#include "data/tgi/pipeline.hpp"

#include "core/ascii.hpp"
#include "data/tgi/value_parse.hpp"

namespace corps::tgi {

std::vector<const AstFile*> TgiDatabase::astsById() const {
    std::vector<const AstFile*> out;
    out.reserve(files.size());
    for (const auto& f : files) out.push_back(f.ast.get());
    return out;
}

const std::string& TgiDatabase::pathOf(std::uint32_t fileId) const {
    static const std::string kUnknown = "?";
    return fileId < files.size() ? files[fileId].path : kUnknown;
}

std::string TgiDatabase::originName(TemplateId origin) const {
    if (origin == kOriginInstance) return "instance";
    if (origin < 0 || static_cast<std::size_t>(origin) >= templates.size()) return "";
    return templates.def(origin).node->name;
}

std::vector<const Record*> TgiDatabase::findRecords(std::string_view type, std::string_view ids) const {
    std::vector<const Record*> out;
    for (const auto& r : records) {
        if (!type.empty() && !iequals(r.typeSpelling, type)) continue;
        if (iequals(r.ids, ids)) out.push_back(&r);
    }
    return out;
}

std::unique_ptr<TgiDatabase> loadTgiDatabase(const vfs::Vfs& vfs, const TgiOptions& options) {
    auto db = std::make_unique<TgiDatabase>();
    std::uint64_t mark = options.hostClockMicros != nullptr ? options.hostClockMicros() : 0;
    const auto phase = [&](const char* name) {
        if (options.hostClockMicros == nullptr) return;
        const std::uint64_t now = options.hostClockMicros();
        db->timings.push_back({name, now - mark});
        mark = now;
    };

    db->order = buildLoadOrder(vfs, db->diags);
    phase("load-order");

    std::vector<std::string> contents(db->order.files.size());
    for (std::size_t i = 0; i < db->order.files.size(); ++i) {
        const LoadEntry& entry = db->order.files[i];
        if (!vfs.read(*entry.file, contents[i])) {
            db->diags.report(DiagCode::LstUnreadable, entry.file->path, 0, "cannot read listed file");
            contents[i].clear();
        }
    }
    phase("read");

    for (std::size_t i = 0; i < db->order.files.size(); ++i) {
        const LoadEntry& entry = db->order.files[i];
        LoadedFile lf;
        lf.path = entry.file->path;
        lf.depot = vfs.depot(entry.file->depot).name();
        lf.listPath = entry.listPath;
        lf.listLine = entry.listLine;
        const auto fileId = static_cast<std::uint32_t>(db->files.size());
        DecodedText text = decodeText(contents[i]);
        std::string().swap(contents[i]);
        lf.encoding = text.encoding;
        lf.ast = std::make_unique<AstFile>(parseTgi(text.utf8, fileId, lf.path, db->symbols, db->diags));
        db->files.push_back(std::move(lf));
    }
    phase("decode+parse");

    const std::vector<const AstFile*> asts = db->astsById();
    bindTemplates(asts, db->symbols, db->templates, db->bindings, db->diags);
    phase("bind");
    db->resolver = std::make_unique<Resolver>(db->templates, db->bindings, asts, db->diags, options.resolve);

    const Symbol idsSym = db->symbols.intern("IDS");
    for (const AstFile* file : asts) {
        for (const AstNode& node : file->nodes) {
            if (node.kind != NodeKind::Data) continue;
            Record rec;
            rec.type = node.nameSym;
            rec.typeSpelling = node.name;
            rec.node = &node;
            rec.file = file;
            rec.tree = db->resolver->resolveTopLevel(node);
            if (const ResolvedField* f = rec.tree.field(idsSym); f != nullptr && f->hasValue() && !isNull(f->text())) {
                rec.ids = f->text();
            }
            db->records.push_back(std::move(rec));
        }
    }

    phase("resolve");
    db->registry = TypeRegistry::defaults(db->symbols);
    db->defs = buildDefIndex(db->records, db->registry, *db->resolver, db->diags);
    phase("def-index");
    return db;
}

}  // namespace corps::tgi
