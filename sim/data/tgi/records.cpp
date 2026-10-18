#include "data/tgi/records.hpp"

#include <algorithm>

#include "core/ascii.hpp"

namespace corps::tgi {

std::string_view policyName(RecordPolicy p) {
    switch (p) {
        case RecordPolicy::ByIds: return "by-ids";
        case RecordPolicy::Singleton: return "singleton";
        case RecordPolicy::Ordered: return "ordered";
    }
    return "?";
}

TypeRegistry TypeRegistry::defaults(SymbolTable& symbols) {
    TypeRegistry r;
    // Singleton merge: SVars_AA.tgi overrides SVars.tgi. Minimap is one IDS-less record.
    for (std::string_view t : {"SVars", "Vars", "Minimap"}) r.set(symbols.intern(t), RecordPolicy::Singleton);
    // Ordered records: Translate is an IDS rename table, Text is keyed by language=.
    for (std::string_view t : {"Translate", "Text"}) r.set(symbols.intern(t), RecordPolicy::Ordered);
    // Identity by IDS, last wins. Every other top-level type in the 1,860 loaded files: each of
    // their records has an IDS and none repeats (surveyed 2026-10-17).
    for (std::string_view t :
         {"Thing",          "Property",        "VFX",          "Audio",          "AbilityDefinition",
          "Terrain",        "Faction",         "Nation",       "AbilityType",    "ActorCommand",
          "ActorState",     "Banner",          "Biome",        "Bone",           "Campaign",
          "ConnectionType", "Credits",         "Cursor",       "DamageType",     "DenizenType",
          "Description",    "DifficultyLevel", "DynamicCampaign", "Ego",         "EnchantmentType",
          "EquipmentType",  "Font",            "Formation",    "GameEvent",      "GameType",
          "Glyph",          "GraphicsPreset",  "GroupFormation", "Handicap",     "HotkeyConfigurationData",
          "KingdomColor",   "MessageType",     "MGAI",         "MGCommand",      "MGDivision",
          "MGHelpPopup",    "MGMap",           "MGMapTable",   "NameList",       "OrganizationLayout",
          "PlacementGroup", "PlayerRank",      "Resolution",   "Resource",       "RMC",
          "SAI",            "ScoringPanel",    "ScoringTab",   "Set",            "Statistic",
          "Sun",            "TargetType",      "TeamCommand",  "TerrainGroup",   "ThingCategory",
          "TransitionSet",  "TriggerElement",  "Weather",      "WorldRule"}) {
        r.set(symbols.intern(t), RecordPolicy::ByIds);
    }
    return r;
}

void TypeRegistry::set(Symbol type, RecordPolicy policy) {
    const auto it = std::lower_bound(entries.begin(), entries.end(), type,
                                     [](const auto& e, Symbol s) { return e.first < s; });
    if (it != entries.end() && it->first == type) {
        it->second = policy;
    } else {
        entries.insert(it, {type, policy});
    }
}

bool TypeRegistry::known(Symbol type) const {
    const auto it = std::lower_bound(entries.begin(), entries.end(), type,
                                     [](const auto& e, Symbol s) { return e.first < s; });
    return it != entries.end() && it->first == type;
}

RecordPolicy TypeRegistry::policy(Symbol type) const {
    const auto it = std::lower_bound(entries.begin(), entries.end(), type,
                                     [](const auto& e, Symbol s) { return e.first < s; });
    return (it != entries.end() && it->first == type) ? it->second : RecordPolicy::Ordered;
}

const DefIndex::ByIdsEntry* DefIndex::find(Symbol type, std::string_view ids) const {
    const std::string key = asciiLowerCopy(ids);
    const auto it = std::lower_bound(byIds.begin(), byIds.end(), std::make_pair(type, std::string_view(key)),
                                     [](const ByIdsEntry& e, const std::pair<Symbol, std::string_view>& k) {
                                         return e.type != k.first ? e.type < k.first : e.idsKey < k.second;
                                     });
    return (it != byIds.end() && it->type == type && it->idsKey == key) ? &*it : nullptr;
}

DefIndex buildDefIndex(const std::vector<Record>& records, const TypeRegistry& registry, Resolver& resolver,
                       Diagnostics& diags) {
    DefIndex out;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const Record& rec = records[i];
        if (!registry.known(rec.type)) out.unknownTypes.push_back(i);
        switch (registry.policy(rec.type)) {
            case RecordPolicy::ByIds: {
                if (rec.ids.empty()) {
                    diags.report(DiagCode::MissingIds, rec.file->path, rec.node->loc.line,
                                 "[" + rec.node->name + "] has no IDS; kept as an ordered record");
                    out.ordered.push_back(i);
                    break;
                }
                DefIndex::ByIdsEntry e;
                e.type = rec.type;
                e.idsKey = asciiLowerCopy(rec.ids);
                e.record = i;
                const auto it = std::lower_bound(out.byIds.begin(), out.byIds.end(), e,
                                                 [](const DefIndex::ByIdsEntry& a, const DefIndex::ByIdsEntry& b) {
                                                     return a.type != b.type ? a.type < b.type : a.idsKey < b.idsKey;
                                                 });
                if (it != out.byIds.end() && it->type == e.type && it->idsKey == e.idsKey) {
                    const Record& prev = records[it->record];
                    diags.report(DiagCode::DuplicateIds, rec.file->path, rec.node->loc.line,
                                 "[" + rec.node->name + "] IDS '" + std::string(rec.ids) + "' also defined at " +
                                     prev.file->path + ":" + std::to_string(prev.node->loc.line) + "; the later one wins");
                    out.replaced.push_back(it->record);
                    it->record = i;
                } else {
                    out.byIds.insert(it, std::move(e));
                }
                break;
            }
            case RecordPolicy::Singleton: {
                DefIndex::Singleton* s = nullptr;
                for (auto& existing : out.singletons) {
                    if (existing.type == rec.type) s = &existing;
                }
                if (s == nullptr) {
                    DefIndex::Singleton fresh;
                    fresh.type = rec.type;
                    fresh.firstRecord = i;
                    fresh.records.push_back(i);
                    fresh.merged = rec.tree;
                    out.singletons.push_back(std::move(fresh));
                } else {
                    s->records.push_back(i);
                    resolver.overlayRecord(s->merged, *rec.node);
                }
                break;
            }
            case RecordPolicy::Ordered:
                out.ordered.push_back(i);
                break;
        }
    }
    std::sort(out.replaced.begin(), out.replaced.end());
    return out;
}

}  // namespace corps::tgi
