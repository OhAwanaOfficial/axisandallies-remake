#include "data/tgi/parser.hpp"

#include <array>

#include "core/ascii.hpp"
#include "data/tgi/lexer.hpp"

namespace corps::tgi {
namespace {

constexpr std::array<std::string_view, 12> kTypeNames = {"",      "string", "localized", "flag",   "bool", "fixed",
                                                         "int",   "float",  "point2",    "point3", "kf32", "fxc"};

// Up to the first two whitespace-separated tokens and the total count (field key sides have 1–2).
struct Tokens {
    std::size_t count = 0;
    std::string_view first;
    std::string_view second;
};

Tokens splitWhitespace(std::string_view s) {
    Tokens out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isAsciiSpace(s[i])) ++i;
        const std::size_t b = i;
        while (i < s.size() && !isAsciiSpace(s[i])) ++i;
        if (i > b) {
            if (out.count == 0) out.first = s.substr(b, i - b);
            if (out.count == 1) out.second = s.substr(b, i - b);
            ++out.count;
        }
    }
    return out;
}

// Header tokens: words, '=' as its own token, and "…" as one token (quotes removed).
struct HeaderToken {
    std::string text;
    bool equals = false;
};

std::vector<HeaderToken> tokenizeHeader(std::string_view s) {
    std::vector<HeaderToken> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (isAsciiSpace(c)) {
            ++i;
        } else if (c == '=') {
            out.push_back({"=", true});
            ++i;
        } else if (c == '"') {
            const std::size_t close = s.find('"', i + 1);
            const std::size_t end = close == std::string_view::npos ? s.size() : close;
            out.push_back({std::string(s.substr(i + 1, end - i - 1)), false});
            i = close == std::string_view::npos ? s.size() : close + 1;
        } else {
            const std::size_t b = i;
            while (i < s.size() && !isAsciiSpace(s[i]) && s[i] != '=' && s[i] != '"') ++i;
            out.push_back({std::string(s.substr(b, i - b)), false});
        }
    }
    return out;
}

bool isModifier(std::string_view w) {
    return iequals(w, "list") || iequals(w, "optional");
}

class Parser {
public:
    Parser(std::uint32_t fileId, std::string path, SymbolTable& symbols, Diagnostics& diags)
        : symbols(symbols), diags(diags) {
        file.fileId = fileId;
        file.path = std::move(path);
    }

    AstFile run(std::string_view utf8) {
        const LexedFile lexed = lexTgi(utf8);
        if (lexed.unterminatedBlockComment) {
            diags.report(DiagCode::UnterminatedBlockComment, file.path, lexed.blockCommentLine,
                         "'/*' is never closed; the rest of the file is a comment");
        }
        for (const LexLine& line : lexed.lines) {
            lineNo = line.number;
            processLine(line.kind, lexed.text(line));
        }
        finish();
        return std::move(file);
    }

private:
    struct Open {
        AstNode node;
    };

    void processLine(LineKind kind, std::string_view text) {
        switch (kind) {
            case LineKind::Blank: break;
            case LineKind::Header: processHeader(text); break;
            case LineKind::Brace: processBraces(text); break;
            case LineKind::Field: processField(text); break;
        }
    }

    // Re-classifies text left over after a brace or a header's ']'.
    void processRest(std::string_view rest) {
        rest = trimAscii(rest);
        if (rest.empty()) return;
        if (rest[0] == '[') {
            processHeader(rest);
        } else if (rest[0] == '{' || rest[0] == '}') {
            processBraces(rest);
        } else {
            processField(rest);
        }
    }

    void processHeader(std::string_view text) {
        if (!stack.empty() && !stack.back().node.braced) closeTop();  // a header ends a braceless body

        ParsedHeader h = parseHeaderLine(text);
        AstNode node;
        node.kind = h.kind;
        node.name = std::move(h.name);
        node.nameSym = symbols.intern(node.name);
        node.listMod = h.listMod;
        node.optionalMod = h.optionalMod;
        node.hasParentRef = h.hasParentRef;
        node.parentRef = std::move(h.parentRef);
        node.parentSym = h.hasParentRef ? symbols.intern(node.parentRef) : kNoSymbol;
        node.attrs = std::move(h.attrs);
        node.loc = {file.fileId, lineNo};
        node.index = nextIndex++;

        if (h.missingBracket) {
            diags.report(DiagCode::HeaderMissingBracket, file.path, lineNo, "header has no ']'; it ends at end of line");
        }
        for (int i = 0; i < h.strayEquals; ++i) {
            diags.report(DiagCode::HeaderStrayEquals, file.path, lineNo, "stray '=' in header dropped");
        }
        if (h.templateAndInherit) {
            diags.report(DiagCode::HeaderTemplateAndInherit, file.path, lineNo,
                         "header has both template= and inherit=; using template=");
        }
        if (node.name.empty()) {
            diags.report(DiagCode::HeaderEmpty, file.path, lineNo, "header has no name; node dropped");
            node.anonymous = true;
        }
        if (!h.extraWords.empty()) {
            std::string words;
            for (const auto& w : h.extraWords) words += (words.empty() ? "" : " ") + w;
            diags.report(DiagCode::HeaderExtraWords, file.path, lineNo, "extra header words ignored: " + words);
        }
        if (node.kind == NodeKind::Template && !stack.empty()) {
            diags.report(DiagCode::NestedTemplate, file.path, lineNo,
                         "template '" + node.name + "' defined inside another node");
        }
        if (node.kind == NodeKind::Decl && stack.empty()) {
            diags.report(DiagCode::DeclAtRoot, file.path, lineNo,
                         "schema declaration '" + node.name + "' at top level is ignored");
        }
        stack.push_back(Open{std::move(node)});

        if (!h.trailing.empty()) {
            if (h.trailing[0] != '{' && h.trailing[0] != '}') {
                diags.report(DiagCode::HeaderTrailingText, file.path, lineNo,
                             "text after ']' read as a separate line: " + h.trailing);
            }
            processRest(h.trailing);
        }
    }

    void processBraces(std::string_view text) {
        bool lastWasClose = false;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '{') {
                openBrace();
                lastWasClose = false;
            } else if (c == '}') {
                closeBrace();
                lastWasClose = true;
            } else if (isAsciiSpace(c)) {
                continue;
            } else {
                const std::string_view rest = text.substr(i);
                if (lastWasClose) {
                    diags.report(DiagCode::TextAfterBrace, file.path, lineNo,
                                 "text after '}' read as a separate line: " + std::string(rest));
                }
                processRest(rest);
                return;
            }
        }
    }

    void openBrace() {
        if (!stack.empty() && !stack.back().node.braced) {
            AstNode& top = stack.back().node;
            if (!top.fields.empty()) {
                diags.report(DiagCode::LateOpenBrace, file.path, lineNo,
                             "'{' after fields of braceless [" + top.name + "] (line " + std::to_string(top.loc.line) +
                                 "); the node becomes braced and keeps its fields");
            }
            top.braced = true;
            return;
        }
        diags.report(DiagCode::AnonymousBlock, file.path, lineNo, "'{' with no open header; block parsed and dropped");
        AstNode anon;
        anon.anonymous = true;
        anon.braced = true;
        anon.loc = {file.fileId, lineNo};
        anon.index = nextIndex++;
        stack.push_back(Open{std::move(anon)});
    }

    void closeBrace() {
        if (!stack.empty() && !stack.back().node.braced) closeTop();  // '}' also ends a braceless body
        if (stack.empty()) {
            diags.report(DiagCode::ExtraCloseBrace, file.path, lineNo, "'}' with no open block ignored");
            return;
        }
        closeTop();
    }

    void processField(std::string_view text) {
        ParsedField f = parseFieldLine(text);
        if (!f.ok) {
            diags.report(DiagCode::UnparsedLine, file.path, lineNo, "cannot parse line: " + std::string(text));
            return;
        }
        if (stack.empty()) {
            diags.report(DiagCode::FieldAtRoot, file.path, lineNo, "field outside any node dropped: " + f.key);
            return;
        }
        if (f.doubledQuote) {
            diags.report(DiagCode::DoubledQuote, file.path, lineNo, "trailing '\"' trimmed from " + f.key);
        }
        AstField field;
        field.key = std::move(f.key);
        field.keySym = symbols.intern(field.key);
        field.type = f.type;
        field.hasValue = f.hasValue;
        field.quoted = f.quoted;
        field.value = std::move(f.value);
        field.loc = {file.fileId, lineNo};
        stack.back().node.fields.push_back(std::move(field));
    }

    void closeTop() {
        AstNode node = std::move(stack.back().node);
        stack.pop_back();
        if (node.anonymous) return;
        if (stack.empty()) {
            file.nodes.push_back(std::move(node));
        } else {
            stack.back().node.children.push_back(std::move(node));
        }
    }

    void finish() {
        std::size_t unclosed = 0;
        std::uint32_t firstLine = 0;
        for (const Open& o : stack) {
            if (o.node.braced) {
                if (unclosed == 0) firstLine = o.node.loc.line;
                ++unclosed;
            }
        }
        if (unclosed > 0) {
            diags.report(DiagCode::EofUnclosedBlock, file.path, firstLine,
                         std::to_string(unclosed) + " block(s) still open at end of file; closed");
        }
        while (!stack.empty()) closeTop();
        file.nodeCount = nextIndex;
    }

    SymbolTable& symbols;
    Diagnostics& diags;
    AstFile file;
    std::vector<Open> stack;
    std::uint32_t lineNo = 0;
    std::uint32_t nextIndex = 0;
};

void dumpNode(std::string& out, const AstNode& n, int depth) {
    const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
    out += indent + "[";
    if (n.kind == NodeKind::Template) out += "Template ";
    if (n.optionalMod) out += "optional ";
    if (n.listMod) out += "list ";
    out += n.name;
    if (n.hasParentRef) out += " template=" + n.parentRef;
    for (const auto& a : n.attrs) out += " " + a.key + "=" + a.value;
    out += "]";
    out += n.braced ? " {}" : " (braceless)";
    out += "  @" + std::to_string(n.loc.line) + "\n";
    for (const auto& f : n.fields) {
        out += indent + "  ";
        if (f.type != TypeWord::None) out += std::string(typeWordName(f.type)) + " ";
        out += f.key;
        if (f.hasValue) out += f.quoted ? " = \"" + f.value + "\"" : " = " + f.value;
        out += "  @" + std::to_string(f.loc.line) + "\n";
    }
    for (const auto& c : n.children) dumpNode(out, c, depth + 1);
}

}  // namespace

std::string_view typeWordName(TypeWord t) {
    return kTypeNames[static_cast<std::size_t>(t)];
}

TypeWord parseTypeWord(std::string_view word) {
    for (std::size_t i = 1; i < kTypeNames.size(); ++i) {
        if (iequals(word, kTypeNames[i])) return static_cast<TypeWord>(i);
    }
    return TypeWord::None;
}

ParsedHeader parseHeaderLine(std::string_view line) {
    ParsedHeader h;
    line = trimAscii(line);
    std::string_view inner = line.substr(1);  // after '['
    // The header ends at the first ']' outside quotes, or at end of line (header-missing-bracket).
    bool inString = false;
    std::size_t close = std::string_view::npos;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '"') inString = !inString;
        if (inner[i] == ']' && !inString) {
            close = i;
            break;
        }
    }
    if (close == std::string_view::npos) {
        h.missingBracket = true;
    } else {
        h.trailing = std::string(trimAscii(inner.substr(close + 1)));
        inner = inner.substr(0, close);
    }

    const std::vector<HeaderToken> tokens = tokenizeHeader(inner);
    std::vector<std::string> words;
    bool haveName = false;  // an attr key can't sit in the name position
    std::string templateRef;
    std::string inheritRef;
    bool hasTemplate = false;
    bool hasInherit = false;
    for (std::size_t i = 0; i < tokens.size();) {
        const HeaderToken& t = tokens[i];
        if (!t.equals && haveName && i + 2 < tokens.size() && tokens[i + 1].equals && !tokens[i + 2].equals) {
            const std::string& key = t.text;
            const std::string& value = tokens[i + 2].text;
            if (iequals(key, "template")) {
                templateRef = value;
                hasTemplate = true;
            } else if (iequals(key, "inherit")) {
                inheritRef = value;
                hasInherit = true;
            } else {
                h.attrs.push_back({key, value});
            }
            i += 3;
            continue;
        }
        if (t.equals) {
            ++h.strayEquals;
            ++i;
            continue;
        }
        words.push_back(t.text);
        if (!isModifier(t.text) && !(words.size() == 1 && iequals(t.text, "template"))) haveName = true;
        ++i;
    }

    if (hasTemplate || hasInherit) {
        h.hasParentRef = true;
        h.parentRef = hasTemplate ? templateRef : inheritRef;
        h.templateAndInherit = hasTemplate && hasInherit;
    }

    std::size_t w = 0;
    if (words.size() >= 2 && iequals(words[0], "template")) {
        h.kind = NodeKind::Template;
        w = 1;
    } else {
        while (w + 1 < words.size() && isModifier(words[w])) {
            if (iequals(words[w], "list")) h.listMod = true;
            if (iequals(words[w], "optional")) h.optionalMod = true;
            ++w;
        }
        h.kind = (h.listMod || h.optionalMod) ? NodeKind::Decl : NodeKind::Data;
    }
    if (w < words.size()) {
        h.name = words[w];
        for (std::size_t k = w + 1; k < words.size(); ++k) h.extraWords.push_back(words[k]);
    }
    return h;
}

ParsedField parseFieldLine(std::string_view line) {
    ParsedField f;
    line = trimAscii(line);
    bool inString = false;
    std::size_t eq = std::string_view::npos;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') inString = !inString;
        if (line[i] == '=' && !inString) {
            eq = i;
            break;
        }
    }

    if (eq != std::string_view::npos) {
        const std::string_view keySide = trimAscii(line.substr(0, eq));
        std::string_view value = trimAscii(line.substr(eq + 1));
        if (keySide.empty()) return f;
        const Tokens tokens = splitWhitespace(keySide);
        const TypeWord type = tokens.count == 2 ? parseTypeWord(tokens.first) : TypeWord::None;
        if (type != TypeWord::None) {
            f.type = type;
            f.key = std::string(tokens.second);
        } else {
            f.key = std::string(keySide);
        }
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
            f.quoted = true;
            if (!value.empty() && value.front() == '#') {
                while (!value.empty() && value.back() == '"') {
                    value.remove_suffix(1);
                    f.doubledQuote = true;
                }
            }
        }
        f.hasValue = true;
        f.value = std::string(value);
        f.ok = true;
        return f;
    }

    const Tokens tokens = splitWhitespace(line);
    const TypeWord type = tokens.count == 2 ? parseTypeWord(tokens.first) : TypeWord::None;
    if (type != TypeWord::None) {
        f.type = type;
        f.key = std::string(tokens.second);
        f.ok = true;
    } else if (tokens.count == 1) {
        f.key = std::string(tokens.first);
        f.ok = true;
    }
    return f;
}

AstFile parseTgi(std::string_view utf8, std::uint32_t fileId, std::string path, SymbolTable& symbols,
                 Diagnostics& diags) {
    Parser p(fileId, std::move(path), symbols, diags);
    return p.run(utf8);
}

std::string dumpAst(const AstFile& file) {
    std::string out;
    for (const auto& n : file.nodes) dumpNode(out, n, 0);
    return out;
}

}  // namespace corps::tgi
