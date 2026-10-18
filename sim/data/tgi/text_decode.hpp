#pragma once

// TGI_FORMAT §2: a UTF-8 BOM means UTF-8 (BOM stripped); else valid UTF-8 stays UTF-8; else the
// bytes are Windows-1252. Output is always UTF-8.

#include <cstdint>
#include <string>
#include <string_view>

namespace corps::tgi {

enum class TextEncoding : std::uint8_t { Utf8Bom, Utf8, Cp1252 };

std::string_view encodingName(TextEncoding e);

struct DecodedText {
    std::string utf8;
    TextEncoding encoding = TextEncoding::Utf8;
};

DecodedText decodeText(std::string_view bytes);

// Strict UTF-8 validation (no overlong forms, no surrogates, max U+10FFFF).
bool isValidUtf8(std::string_view bytes);

// Windows-1252 → UTF-8. The five bytes cp1252 leaves undefined (0x81 0x8D 0x8F 0x90 0x9D) map to
// the C1 control with the same value, as MultiByteToWideChar does.
std::string cp1252ToUtf8(std::string_view bytes);

}  // namespace corps::tgi
