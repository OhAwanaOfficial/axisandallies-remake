#pragma once

// Runs the whole TGI pipeline once (TGI_FORMAT §1): LstLoader → TextDecode → Lexer → Parser →
// TemplateTable (sequential binding) → Resolver → Records → DefIndex. A pure function of the bytes
// in the depots, so its output is deterministic.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "data/tgi/diagnostics.hpp"
#include "data/tgi/lst_loader.hpp"
#include "data/tgi/parser.hpp"
#include "data/tgi/records.hpp"
#include "data/tgi/resolver.hpp"
#include "data/tgi/symbols.hpp"
#include "data/tgi/template_table.hpp"
#include "data/tgi/text_decode.hpp"
#include "vfs/vfs.hpp"

namespace corps::tgi {

struct LoadedFile {
    std::string path;   // VFS path, winning depot's spelling
    std::string depot;  // depot display name
    TextEncoding encoding = TextEncoding::Utf8;
    std::string listPath;
    std::uint32_t listLine = 0;
    std::unique_ptr<AstFile> ast;  // fileId == index in TgiDatabase::files
};

struct TgiOptions {
    ResolveOptions resolve;
    // Optional host clock (microseconds) for phase timings. sim/ never reads a clock itself; the
    // timings are diagnostics only and never influence the result.
    std::uint64_t (*hostClockMicros)() = nullptr;
};

struct PhaseTiming {
    const char* phase = "";
    std::uint64_t micros = 0;
};

struct TgiDatabase {
    SymbolTable symbols;
    Diagnostics diags;
    LoadOrder order;
    std::vector<LoadedFile> files;  // load order
    TemplateTable templates;
    Bindings bindings;
    std::unique_ptr<Resolver> resolver;
    TypeRegistry registry;
    std::vector<Record> records;  // every top-level data node, load order
    DefIndex defs;
    std::vector<PhaseTiming> timings;  // filled only when TgiOptions::hostClockMicros is set

    std::vector<const AstFile*> astsById() const;
    const std::string& pathOf(std::uint32_t fileId) const;
    // Template name, or "instance" for kOriginInstance, or "" for none.
    std::string originName(TemplateId origin) const;
    // "Thing per_tank_medium" style lookup across all ByIds types; type may be empty (any type).
    std::vector<const Record*> findRecords(std::string_view type, std::string_view ids) const;
};

std::unique_ptr<TgiDatabase> loadTgiDatabase(const vfs::Vfs& vfs, const TgiOptions& options = {});

}  // namespace corps::tgi
