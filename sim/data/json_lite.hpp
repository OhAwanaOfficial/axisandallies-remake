#pragma once

// Minimal strict JSON reader for tool output (e.g. tools/rmp.py map.json). Lives in sim/, so it
// never converts numbers to floating point: numbers are kept as their source text and read with
// asInt() (exact integers only). Objects keep their keys in source order (looked up linearly,
// never iterated through a hash container).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace corps::json {

struct Value {
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    std::string text;  // Number: source text; String: decoded UTF-8
    std::vector<Value> items;                            // Array
    std::vector<std::pair<std::string, Value>> members;  // Object, source order

    const Value* get(std::string_view key) const;  // Object member or nullptr
    std::optional<std::int64_t> asInt() const;     // Number without fraction/exponent
    bool isArray() const { return type == Type::Array; }
};

struct ParseResult {
    bool ok = false;
    std::string error;  // with byte offset
    Value value;
};

ParseResult parse(std::string_view text);

}  // namespace corps::json
