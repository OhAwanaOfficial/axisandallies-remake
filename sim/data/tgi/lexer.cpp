#include "data/tgi/lexer.hpp"

#include "core/ascii.hpp"

namespace corps::tgi {

std::string stripComments(std::string_view text, bool* unterminatedBlock, std::uint32_t* blockStartLine) {
    std::string out;
    out.reserve(text.size());
    const std::size_t n = text.size();
    std::uint32_t line = 1;
    bool inBlock = false;
    bool inString = false;
    std::uint32_t blockLine = 0;

    for (std::size_t i = 0; i < n; ++i) {
        char c = text[i];
        if (c == '\r') {
            if (i + 1 < n && text[i + 1] == '\n') ++i;
            c = '\n';
        }
        if (c == '\n') {
            out.push_back('\n');
            ++line;
            inString = false;  // strings never span lines
            continue;
        }
        if (inBlock) {
            if (c == '*' && i + 1 < n && text[i + 1] == '/') {
                inBlock = false;
                ++i;
            }
            continue;
        }
        if (inString) {
            out.push_back(c);
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
            out.push_back(c);
            continue;
        }
        if (c == ';' || (c == '/' && i + 1 < n && text[i + 1] == '/')) {
            while (i + 1 < n && text[i + 1] != '\n' && text[i + 1] != '\r') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '*') {
            inBlock = true;
            blockLine = line;
            out.push_back(' ');  // a comment separates tokens, as in C
            ++i;
            continue;
        }
        out.push_back(c);
    }
    if (unterminatedBlock != nullptr) *unterminatedBlock = inBlock;
    if (blockStartLine != nullptr) *blockStartLine = inBlock ? blockLine : 0;
    return out;
}

LexedFile lexTgi(std::string_view utf8) {
    LexedFile out;
    out.stripped = stripComments(utf8, &out.unterminatedBlockComment, &out.blockCommentLine);
    const std::string_view s = out.stripped;
    std::uint32_t number = 1;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t end = s.find('\n', start);
        if (end == std::string_view::npos) end = s.size();
        const std::string_view raw = s.substr(start, end - start);
        const std::string_view t = trimAscii(raw);
        LexLine line;
        line.number = number;
        line.begin = static_cast<std::uint32_t>(t.empty() ? start : static_cast<std::size_t>(t.data() - s.data()));
        line.length = static_cast<std::uint32_t>(t.size());
        if (t.empty()) {
            line.kind = LineKind::Blank;
        } else if (t[0] == '[') {
            line.kind = LineKind::Header;
        } else if (t[0] == '{' || t[0] == '}') {
            line.kind = LineKind::Brace;
        } else {
            line.kind = LineKind::Field;
        }
        if (line.kind != LineKind::Blank) out.lines.push_back(line);
        ++number;
        if (end == s.size()) break;
        start = end + 1;
    }
    return out;
}

}  // namespace corps::tgi
