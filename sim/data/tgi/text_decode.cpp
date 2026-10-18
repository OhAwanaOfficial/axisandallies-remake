#include "data/tgi/text_decode.hpp"

#include <array>

namespace corps::tgi {
namespace {

// 0x80..0x9F in Windows-1252.
constexpr std::array<std::uint16_t, 32> kCp1252High = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,  // 80
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,  // 88
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,  // 90
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,  // 98
};

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

std::string_view encodingName(TextEncoding e) {
    switch (e) {
        case TextEncoding::Utf8Bom: return "utf-8-bom";
        case TextEncoding::Utf8: return "utf-8";
        case TextEncoding::Cp1252: return "cp1252";
    }
    return "?";
}

bool isValidUtf8(std::string_view bytes) {
    std::size_t i = 0;
    const std::size_t n = bytes.size();
    while (i < n) {
        const auto b0 = static_cast<unsigned char>(bytes[i]);
        if (b0 < 0x80) {
            ++i;
            continue;
        }
        std::size_t len = 0;
        std::uint32_t cp = 0;
        std::uint32_t minCp = 0;
        if ((b0 & 0xE0) == 0xC0) {
            len = 2;
            cp = b0 & 0x1F;
            minCp = 0x80;
        } else if ((b0 & 0xF0) == 0xE0) {
            len = 3;
            cp = b0 & 0x0F;
            minCp = 0x800;
        } else if ((b0 & 0xF8) == 0xF0) {
            len = 4;
            cp = b0 & 0x07;
            minCp = 0x10000;
        } else {
            return false;
        }
        if (i + len > n) return false;
        for (std::size_t k = 1; k < len; ++k) {
            const auto b = static_cast<unsigned char>(bytes[i + k]);
            if ((b & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3F);
        }
        if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

std::string cp1252ToUtf8(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 8);
    for (char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        if (b < 0x80) {
            out.push_back(c);
        } else if (b < 0xA0) {
            appendUtf8(out, kCp1252High[b - 0x80]);
        } else {
            appendUtf8(out, b);
        }
    }
    return out;
}

DecodedText decodeText(std::string_view bytes) {
    DecodedText out;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        out.encoding = TextEncoding::Utf8Bom;
        out.utf8.assign(bytes.substr(3));
        return out;
    }
    if (isValidUtf8(bytes)) {
        out.encoding = TextEncoding::Utf8;
        out.utf8.assign(bytes);
        return out;
    }
    out.encoding = TextEncoding::Cp1252;
    out.utf8 = cp1252ToUtf8(bytes);
    return out;
}

}  // namespace corps::tgi
