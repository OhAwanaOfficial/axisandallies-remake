#pragma once

// Text and JSON renderings of resolved data, used by `simcli dump-defs` and the golden tests.
// Every output iterates in load order or sorted-key order, never hash order.

#include <cstdint>
#include <string>
#include <string_view>

#include "data/tgi/pipeline.hpp"

namespace corps::tgi {

std::uint64_t fnv1a64(std::string_view bytes, std::uint64_t seed = 1469598103934665603ULL);
std::string hex64(std::uint64_t v);

// Canonical, provenance-free rendering (stable input for hashes):
//   [Type]
//     key : type = value     (": type" only when declared, "= value" only when set, "!" when required)
std::string dumpCanonical(const ResolvedNode& node);

// Canonical dump of the whole definition set: by-IDS winners, merged singletons, ordered records,
// each in load order. Its hash is the "resolved hash" printed by dump-defs.
std::string dumpDefinitionSet(const TgiDatabase& db);

// Human-readable tree with type, value and provenance (file:line, origin) per field.
std::string dumpWithProvenance(const TgiDatabase& db, const ResolvedNode& node, int depth = 0);

// `dump-defs --explain`: inheritance chain plus either the whole tree or one path.
// Path syntax: "BodyComponent.health", "PerformerComponent/Ability[0]/AbilityDefinition.range",
// "category". Returns false (with a message in `out`) if the path doesn't resolve.
bool explainRecord(const TgiDatabase& db, const Record& record, std::string_view path, std::string& out);

// Template chain of a record, outermost first: "Thing (file) → T1 (file:line) → …".
std::string describeChain(const TgiDatabase& db, const Record& record);

// `dump-defs --json`.
std::string definitionsJson(const TgiDatabase& db);

// `dump-defs --report-unknown`: unknown top-level types, then fields that are set but never
// declared with a type (nothing consumes them yet, so "unconsumed" = all of them).
std::string reportUnknown(const TgiDatabase& db);

}  // namespace corps::tgi
