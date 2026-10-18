#include "data/tgi/diagnostics.hpp"

#include <array>

namespace corps::tgi {
namespace {

struct CodeInfo {
    std::string_view name;
    Severity severity;
};

constexpr std::array<CodeInfo, static_cast<std::size_t>(DiagCode::Count_)> kCodes = {{
    {"decoded-cp1252", Severity::Info},
    {"unterminated-block-comment", Severity::Warning},
    {"header-missing-bracket", Severity::Warning},
    {"header-stray-equals", Severity::Warning},
    {"header-trailing-text", Severity::Warning},
    {"header-empty", Severity::Warning},
    {"header-extra-words", Severity::Warning},
    {"header-template-and-inherit", Severity::Warning},
    {"doubled-quote", Severity::Warning},
    {"unparsed-line", Severity::Warning},
    {"eof-unclosed-block", Severity::Warning},
    {"late-open-brace", Severity::Info},
    {"extra-close-brace", Severity::Warning},
    {"text-after-brace", Severity::Warning},
    {"anonymous-block", Severity::Warning},
    {"field-at-root", Severity::Warning},
    {"nested-template", Severity::Warning},
    {"decl-at-root", Severity::Warning},
    {"lst-missing-file", Severity::Warning},
    {"lst-recursion", Severity::Error},
    {"lst-duplicate", Severity::Warning},
    {"lst-unreadable", Severity::Error},
    {"template-redefined", Severity::Warning},
    {"unknown-template", Severity::Error},
    {"template-cycle", Severity::Error},
    {"type-redeclared", Severity::Warning},
    {"undeclared-repeat", Severity::Info},
    {"duplicate-ids", Severity::Warning},
    {"missing-ids", Severity::Warning},
    {"decimal-comma", Severity::Warning},
    {"bad-value", Severity::Warning},
}};

}  // namespace

std::string_view codeName(DiagCode code) {
    return kCodes[static_cast<std::size_t>(code)].name;
}

Severity defaultSeverity(DiagCode code) {
    return kCodes[static_cast<std::size_t>(code)].severity;
}

std::string_view severityName(Severity s) {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warn";
        case Severity::Error: return "error";
    }
    return "?";
}

void Diagnostics::report(DiagCode code, std::string_view file, std::uint32_t line, std::string message) {
    report(defaultSeverity(code), code, file, line, std::move(message));
}

void Diagnostics::report(Severity severity, DiagCode code, std::string_view file, std::uint32_t line,
                         std::string message) {
    Diagnostic d;
    d.severity = severity;
    d.code = code;
    d.file = std::string(file);
    d.line = line;
    d.message = std::move(message);
    items.push_back(std::move(d));
}

std::size_t Diagnostics::count(Severity s) const {
    std::size_t n = 0;
    for (const auto& d : items) n += (d.severity == s) ? 1 : 0;
    return n;
}

std::size_t Diagnostics::count(DiagCode c) const {
    std::size_t n = 0;
    for (const auto& d : items) n += (d.code == c) ? 1 : 0;
    return n;
}

std::string formatDiagnostic(const Diagnostic& d) {
    std::string out;
    if (!d.file.empty()) {
        out += d.file;
        if (d.line != 0) out += ":" + std::to_string(d.line);
        out += ": ";
    }
    out += severityName(d.severity);
    out += " ";
    out += codeName(d.code);
    out += ": ";
    out += d.message;
    return out;
}

std::string summarizeDiagnostics(const Diagnostics& diags) {
    std::array<std::size_t, static_cast<std::size_t>(DiagCode::Count_)> counts{};
    std::array<Severity, static_cast<std::size_t>(DiagCode::Count_)> worst{};
    for (const auto& d : diags.all()) {
        const auto i = static_cast<std::size_t>(d.code);
        if (counts[i] == 0 || d.severity > worst[i]) worst[i] = d.severity;
        ++counts[i];
    }
    std::string out;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] == 0) continue;
        std::string sev(severityName(worst[i]));
        sev.resize(6, ' ');
        std::string name(kCodes[i].name);
        if (name.size() < 28) name.resize(28, ' ');
        out += "  " + sev + name + " " + std::to_string(counts[i]) + "\n";
    }
    return out;
}

}  // namespace corps::tgi
