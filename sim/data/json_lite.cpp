#include "data/json_lite.hpp"

#include "data/tgi/value_parse.hpp"

namespace corps::json {

namespace {

class Parser {
public:
    explicit Parser(std::string_view t) : s(t) {}

    bool value(Value& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        ws();
        if (p >= s.size()) return fail("unexpected end");
        const char c = s[p];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.type = Value::Type::String;
            return string(out.text);
        }
        if (c == 't' || c == 'f' || c == 'n') return literal(out);
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("unexpected character");
    }

    bool end() {
        ws();
        return p == s.size() || fail("trailing characters");
    }

    std::string error;

private:
    bool fail(const char* why) {
        if (error.empty()) error = std::string(why) + " at byte " + std::to_string(p);
        return false;
    }
    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
    }
    bool object(Value& out, int depth) {
        out.type = Value::Type::Object;
        ++p;
        ws();
        if (p < s.size() && s[p] == '}') {
            ++p;
            return true;
        }
        for (;;) {
            ws();
            std::string key;
            if (p >= s.size() || s[p] != '"' || !string(key)) return fail("expected object key");
            ws();
            if (p >= s.size() || s[p++] != ':') return fail("expected ':'");
            Value v;
            if (!value(v, depth + 1)) return false;
            out.members.emplace_back(std::move(key), std::move(v));
            ws();
            if (p < s.size() && s[p] == ',') {
                ++p;
                continue;
            }
            if (p < s.size() && s[p] == '}') {
                ++p;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }
    bool array(Value& out, int depth) {
        out.type = Value::Type::Array;
        ++p;
        ws();
        if (p < s.size() && s[p] == ']') {
            ++p;
            return true;
        }
        for (;;) {
            Value v;
            if (!value(v, depth + 1)) return false;
            out.items.push_back(std::move(v));
            ws();
            if (p < s.size() && s[p] == ',') {
                ++p;
                continue;
            }
            if (p < s.size() && s[p] == ']') {
                ++p;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }
    bool string(std::string& out) {
        ++p;  // opening quote
        while (p < s.size()) {
            const char c = s[p++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p >= s.size()) break;
            const char e = s[p++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (p + 4 > s.size()) return fail("bad \\u escape");
                std::uint32_t cp = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = s[p++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<std::uint32_t>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<std::uint32_t>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<std::uint32_t>(h - 'A' + 10);
                    else return fail("bad \\u escape");
                }
                // UTF-8 encode (surrogate pairs are not needed for our tool output; kept as-is)
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            default:
                return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool literal(Value& out) {
        if (s.substr(p, 4) == "true") {
            out.type = Value::Type::Bool;
            out.boolean = true;
            p += 4;
            return true;
        }
        if (s.substr(p, 5) == "false") {
            out.type = Value::Type::Bool;
            p += 5;
            return true;
        }
        if (s.substr(p, 4) == "null") {
            out.type = Value::Type::Null;
            p += 4;
            return true;
        }
        return fail("bad literal");
    }
    // JSON grammar: -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?
    bool number(Value& out) {
        const std::size_t b = p;
        const auto digit = [&] { return p < s.size() && s[p] >= '0' && s[p] <= '9'; };
        const auto digits = [&] {
            if (!digit()) return false;
            while (digit()) ++p;
            return true;
        };
        if (s[p] == '-') ++p;
        if (p < s.size() && s[p] == '0') {
            ++p;
        } else if (!digits()) {
            return fail("bad number");
        }
        if (p < s.size() && s[p] == '.') {
            ++p;
            if (!digits()) return fail("bad number");
        }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            ++p;
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
            if (!digits()) return fail("bad number");
        }
        out.type = Value::Type::Number;
        out.text = std::string(s.substr(b, p - b));
        return true;
    }

    std::string_view s;
    std::size_t p = 0;
};

}  // namespace

const Value* Value::get(std::string_view key) const {
    for (const auto& [k, v] : members) {
        if (k == key) return &v;
    }
    return nullptr;
}

std::optional<std::int64_t> Value::asInt() const {
    if (type != Type::Number) return std::nullopt;
    return tgi::parseInt(text);
}

ParseResult parse(std::string_view text) {
    ParseResult r;
    Parser p(text);
    if (p.value(r.value, 0) && p.end()) {
        r.ok = true;
    } else {
        r.error = p.error;
    }
    return r;
}

}  // namespace corps::json
