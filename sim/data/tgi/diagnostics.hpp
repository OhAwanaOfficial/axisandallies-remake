#pragma once

// TGI diagnostics (TGI_FORMAT §10): recover the way the data needs, and log every recovery with
// severity, code and file:line.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace corps::tgi {

enum class Severity : std::uint8_t { Info, Warning, Error };

enum class DiagCode : std::uint8_t {
    // decoding / lexing (§2–3)
    DecodedCp1252,
    UnterminatedBlockComment,
    // headers and fields (§3.3–3.5)
    HeaderMissingBracket,
    HeaderStrayEquals,
    HeaderTrailingText,
    HeaderEmpty,
    HeaderExtraWords,
    HeaderTemplateAndInherit,
    DoubledQuote,
    UnparsedLine,
    // blocks (§5)
    EofUnclosedBlock,
    LateOpenBrace,
    ExtraCloseBrace,
    TextAfterBrace,
    AnonymousBlock,
    FieldAtRoot,
    NestedTemplate,
    DeclAtRoot,
    // load order (§6)
    LstMissingFile,
    LstRecursion,
    LstDuplicate,
    LstUnreadable,
    // templates and resolution (§7)
    TemplateRedefined,
    UnknownTemplate,
    TemplateCycle,
    TypeRedeclared,
    UndeclaredRepeat,
    // records and values (§8–9)
    DuplicateIds,
    MissingIds,
    DecimalComma,
    BadValue,
    Count_
};

std::string_view codeName(DiagCode code);
Severity defaultSeverity(DiagCode code);
std::string_view severityName(Severity s);

struct Diagnostic {
    Severity severity = Severity::Warning;
    DiagCode code = DiagCode::UnparsedLine;
    std::string file;  // VFS path ("" for whole-run diagnostics)
    std::uint32_t line = 0;
    std::string message;
};

class Diagnostics {
public:
    void report(DiagCode code, std::string_view file, std::uint32_t line, std::string message);
    void report(Severity severity, DiagCode code, std::string_view file, std::uint32_t line, std::string message);

    const std::vector<Diagnostic>& all() const { return items; }
    std::size_t count(Severity s) const;
    std::size_t count(DiagCode c) const;
    bool empty() const { return items.empty(); }
    void clear() { items.clear(); }

private:
    std::vector<Diagnostic> items;
};

// "file:line: warning header-missing-bracket: message"
std::string formatDiagnostic(const Diagnostic& d);

// One line per code that occurred, in DiagCode order: "  warn  header-missing-bracket  3".
std::string summarizeDiagnostics(const Diagnostics& diags);

}  // namespace corps::tgi
