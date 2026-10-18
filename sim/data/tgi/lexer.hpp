#pragma once

// TGI lexical pass (TGI_FORMAT §3.1–3.2): comment stripping, then line classification.
//
// Comments: ';' and "//" run to end of line; "/* */" is NON-nesting and may span lines. All three
// are quote-aware: '"' toggles "in string" for the rest of the line (strings never span lines),
// and comment markers inside a string are text. Inside a block comment only "*/" matters.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace corps::tgi {

enum class LineKind : std::uint8_t { Blank, Header, Brace, Field };

struct LexLine {
    LineKind kind = LineKind::Blank;
    std::uint32_t number = 0;  // 1-based source line
    std::uint32_t begin = 0;   // trimmed text = stripped.substr(begin, length)
    std::uint32_t length = 0;
};

struct LexedFile {
    std::string stripped;  // comments removed, line endings normalised to '\n'
    std::vector<LexLine> lines;
    bool unterminatedBlockComment = false;
    std::uint32_t blockCommentLine = 0;  // where the unterminated "/*" started

    std::string_view text(const LexLine& l) const { return std::string_view(stripped).substr(l.begin, l.length); }
};

// CRLF, LF and lone CR all end a line.
std::string stripComments(std::string_view text, bool* unterminatedBlock = nullptr,
                          std::uint32_t* blockStartLine = nullptr);

LexedFile lexTgi(std::string_view utf8);

}  // namespace corps::tgi
