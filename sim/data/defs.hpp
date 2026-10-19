#pragma once

// Typed, immutable data definitions compiled from resolved TGI (ARCHITECTURE §4.2, TGI_FORMAT §8).
// Every number the sim uses is Fixed or an integer; nothing here is a float. A DefSet never
// changes after compileDefs() and is shared read-only by every Sim; its dataHash goes into replays
// and lobbies (NETCODE §7) so peers with different data can't play together by accident.
//
// Only fields the sim needs (or will need soon) are compiled. View-only data (models, FX, audio)
// stays in the TGI database for the presentation layer.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "math/fixed.hpp"

namespace corps {

constexpr std::uint32_t kNoDef = 0xFFFFFFFFu;

// CostList / UpkeepList / ProductionList (Game/resource_list.tgi).
struct ResourceAmounts {
    Fixed money;
    Fixed oil;
    Fixed ammo;
    Fixed unitLimitProvided;
    Fixed unitLimitConsumed;
    Fixed buildingLimitProvided;
    Fixed buildingLimitConsumed;
};

struct EffectDef {
    std::string type;        // MAIN_DAMAGE, ADD_DAMAGE, HEAL, SUMMON, MORALE_LOSS, ...
    std::string damageType;  // damage_type_ids, e.g. armor_piercing
    Fixed amount;
    std::string objectIds;   // SummonEffect object_ids
};

struct AbilityDef {
    std::string ids;         // top-level AbilityDefinition IDS, or "<owner>#<n>" for inline ones
    std::string ownerIds;    // unit/building that declares it inline ("" for top-level)
    std::string nameKey;     // localization key without '#'
    std::string abilityType; // attack, repair, ...
    Fixed range;
    Fixed minRange;
    Fixed rechargeTime;
    Fixed area;
    std::string projectileIds;
    std::vector<std::string> targets;  // "+enemy", "-air", ... as written
    std::vector<EffectDef> effects;
};

// What a regiment slot holds: a unit (or truck), or a building (an unpacked HQ).
enum class ElementKind : std::uint8_t { None = 0, Unit = 1, Building = 2 };
struct ElementRef {
    ElementKind kind = ElementKind::None;
    std::uint32_t index = kNoDef;  // into DefSet::units or DefSet::buildings
};

struct UnitDef {
    std::string ids;
    std::string nameKey;
    std::string templateName;  // the Thing's template, e.g. AAUnitPERVehicleMediumTank
    std::string category;      // UNIT, or TRUCK (packable vehicles such as HQ trucks)
    Fixed health;
    Fixed defense;
    Fixed speedMax;
    Fixed acceleration;
    Fixed turnRadius;
    Fixed detectionRange;
    Fixed remoteDetectionRange;
    Fixed blockingRadius;
    std::vector<std::uint32_t> abilities;  // indexes into DefSet::abilities
    std::vector<std::string> properties;   // property_IDS, in resolved order
    std::string requiredStructures;
    ResourceAmounts cost;
    ResourceAmounts upkeep;
    std::string resourcePrefix;  // view: model/texture prefix (kept for SimBridge spawning visuals)
};

struct RegimentDef {
    std::string ids;
    std::string nameKey;
    std::string templateName;
    std::string layoutIds;
    std::vector<std::string> slotIds;     // element_IDS per slot, layout order
    std::vector<ElementRef> slots;        // resolved slot elements (kind None if missing)
    Fixed controlRange;
    Fixed operationalRange;
    Fixed moraleBreakpoint;
    Fixed entrenchmentTime;
    Fixed recruitTime;
    Fixed shortfallDamageRate;
    Fixed detectionRange;
    bool recruitFull = false;
    bool inSupplyDetached = true;
    std::string transportActor;
    std::vector<std::string> properties;
    ResourceAmounts cost;
    ResourceAmounts upkeep;
};

struct BuildingDef {
    std::string ids;
    std::string nameKey;
    std::string templateName;
    Fixed health;
    Fixed defense;
    Fixed blockingRadius;
    std::vector<std::uint32_t> abilities;
    std::vector<std::string> recruits;  // RecruitComponent actor_ids (regiments it can recruit)
    std::vector<std::string> properties;
    ResourceAmounts cost;
    ResourceAmounts upkeep;
    ResourceAmounts production;
};

struct NationDef {
    std::string ids;
    std::string name;  // literal or "#key"
    std::vector<std::string> factions;  // general ids
    std::vector<std::pair<std::string, std::string>> placeholders;  // placeholder_IDS -> thing_IDS
    std::string resourcePrefix;
};

enum class SVarType : std::uint8_t { Fixed, Int, Bool, String };

struct SVar {
    std::string name;  // as spelled in the data
    SVarType type = SVarType::String;
    std::string text;  // raw value
    Fixed fixedValue;
    std::int64_t intValue = 0;
    bool boolValue = false;
};

// [SVars] singleton (Game/SVars.tgi merged with Game/SVars_AA.tgi).
struct SVarsDef {
    std::vector<SVar> vars;  // sorted by lower-case name

    const SVar* find(std::string_view name) const;  // case-insensitive
    Fixed getFixed(std::string_view name, Fixed fallback) const;
    std::int64_t getInt(std::string_view name, std::int64_t fallback) const;
    bool getBool(std::string_view name, bool fallback) const;
};

struct DefCompileStats {
    std::size_t thingsSeen = 0;
    std::size_t thingsSkipped = 0;  // categories the sim doesn't compile (doodads, effects, ...)
    std::vector<std::pair<std::string, std::size_t>> categories;  // category -> count, sorted by name
    std::size_t missingSlotUnits = 0;  // slot elements that are neither a unit nor a building
    std::size_t badValues = 0;
};

struct DefSet {
    std::vector<UnitDef> units;          // all sorted by IDS (lower-case) for lookup
    std::vector<RegimentDef> regiments;
    std::vector<BuildingDef> buildings;
    std::vector<AbilityDef> abilities;
    std::vector<NationDef> nations;
    SVarsDef svars;
    std::uint64_t dataHash = 0;
    DefCompileStats stats;

    // Lookup by IDS (case-insensitive). Returns nullptr / kNoDef when absent.
    const UnitDef* unit(std::string_view ids) const;
    const RegimentDef* regiment(std::string_view ids) const;
    const BuildingDef* building(std::string_view ids) const;
    const AbilityDef* ability(std::string_view ids) const;
    const NationDef* nation(std::string_view ids) const;
    std::uint32_t unitIndex(std::string_view ids) const;
    std::uint32_t regimentIndex(std::string_view ids) const;
    std::uint32_t buildingIndex(std::string_view ids) const;
};

// Canonical bytes of a DefSet (fixed field order); dataHash = xxh3_64 of these bytes.
std::vector<std::uint8_t> serializeDefs(const DefSet& defs);

}  // namespace corps
