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
#include "math/fvec2.hpp"

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
    Fixed area;                       // blast radius (0: the target only)
    Fixed volleyArea;
    std::uint32_t maxTargets = 0;     // units an area attack may hit (0: no limit)
    Fixed attenuationRangeMin = Fixed::fromInt(1);   // area attenuation (GAME_DESIGN §6.3)
    Fixed attenuationFactorMax = Fixed::fromInt(1);
    std::string projectileIds;
    std::uint32_t projectileIndex = kNoDef;  // into DefSet::projectiles
    std::vector<std::string> targets;  // "+enemy", "-air", ... as written
    std::vector<EffectDef> effects;
};

// [DamageType] (Game/damage_types.tgi): gunfire (the design's Pierce), armor_piercing (AP),
// explosive (Blast) and flame.
struct DamageTypeDef {
    std::string ids;
    std::int32_t damageFilter = -1;
};

// [TargetType] (Game/target_types_aa.tgi): air, tank, infantry, building, routing, ...
struct TargetTypeDef {
    std::string ids;
};

// [Property] records, with the modifiers combat reads: ResistDamageModifier (the fraction of one
// damage type's damage ignored; fractions from all of a unit's properties add up) and AddModifier
// DV (defense).
struct PropertyDef {
    std::string ids;
    std::vector<std::string> targetFlags;         // enabled_target_flags: target types it grants
    std::vector<std::string> requiredProperties;  // required_properties (terrain ones: infantry / vehicle)
    Fixed defenseAdd;                             // AddModifier DV
    std::vector<std::pair<std::string, Fixed>> resist;  // (damage type IDS, amount), data order
};

// [Terrain] (Terrain/<biome>/*.tgi): its group and the properties standing on it gives
// (terrain_forest_infantry, ...). Map cells name the terrain or one of its variations.
struct TerrainDef {
    std::string ids;
    std::string group;
    std::vector<std::string> properties;
    std::vector<std::string> variations;  // Variation IDS
};

// PROJECTILE things' ProjectileComponent.
enum class ProjectileType : std::uint8_t { Antigrav = 0, Lob = 1, Bomb = 2 };
struct ProjectileDef {
    std::string ids;
    Fixed speed;   // projectile_speed, units/s (0: CombatTuning::defaultProjectileSpeed)
    ProjectileType type = ProjectileType::Antigrav;
    Fixed life;    // projectile_life, s (0: until it arrives)
    Fixed spread;  // view only
};

// PerformerComponent/Turret (yaw only: the sim is 2D).
struct TurretDef {
    std::string ids;
    Fixed yawRate;      // degrees per second
    Fixed yawRange;     // degrees (360: all round)
    Fixed yawStart;     // degrees: rest yaw relative to the body
    Fixed resetTime;    // seconds without a target before it turns back
    bool canReset = true;
    bool canPreaim = true;
};

// One Ability slot of a PerformerComponent or DefenderComponent: the AbilityDefinition it uses
// and how the actor uses it. A weapon with a turret aims with it; without one it is a "local"
// gun and the body has to face the target.
struct WeaponDef {
    static constexpr std::uint32_t kNoTurret = 0xFFFFFFFFu;
    std::uint32_t ability = kNoDef;       // into DefSet::abilities
    std::uint32_t turret = kNoTurret;     // into the owner's turrets
    Fixed priority;                       // Ability priority (higher first)
    Fixed fuzzyRange;                     // extra acquisition range
    Fixed reloadTime;                     // added to the definition's recharge_time
};

// What a regiment slot holds: a unit (or truck), or a building (an unpacked HQ).
enum class ElementKind : std::uint8_t { None = 0, Unit = 1, Building = 2 };
struct ElementRef {
    ElementKind kind = ElementKind::None;
    std::uint32_t index = kNoDef;  // into DefSet::units or DefSet::buildings
};

// Movement class of a unit, from its property tags (PATHFINDING §2): aircraft → Air, seacraft →
// Naval, tank → Tracked, other vehicle → Wheeled, else Foot. The first five values equal
// nav::MoveClass; Air has no grid.
enum class UnitMoveClass : std::uint8_t { Foot = 0, Wheeled = 1, Tracked = 2, Naval = 3, Amphibious = 4, Air = 5 };
// MoverComponent turn_type: INSTANT (infantry), IN_PLACE (tanks pivot), WHILE_MOVING (wheels, ships).
enum class TurnType : std::uint8_t { Instant = 0, InPlace = 1, WhileMoving = 2 };

struct UnitDef {
    std::string ids;
    std::string nameKey;
    std::string templateName;  // the Thing's template, e.g. AAUnitPERVehicleMediumTank
    std::string category;      // UNIT, or TRUCK (packable vehicles such as HQ trucks)
    Fixed health;
    Fixed defense;
    Fixed speedMax;            // world units per second
    Fixed acceleration;        // units/s²; 0 = instant
    Fixed turnRadius;          // 0 = turns on the spot
    TurnType turnType = TurnType::Instant;
    UnitMoveClass moveClass = UnitMoveClass::Foot;
    Fixed detectionRange;
    Fixed remoteDetectionRange;
    Fixed blockingRadius;
    std::vector<std::uint32_t> abilities;  // indexes into DefSet::abilities
    std::vector<WeaponDef> weapons;        // PerformerComponent Ability slots
    std::vector<TurretDef> turrets;
    bool checkDamageResistance = false;    // weigh targets' resistances when choosing
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
    std::uint32_t layoutIndex = kNoDef;   // into DefSet::layouts
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
    std::vector<WeaponDef> weapons;     // PerformerComponent, then DefenderComponent Ability slots
    std::vector<TurretDef> turrets;
    Fixed controlRange;                 // DefenderComponent control_range (its ZOC; 0: none)
    bool checkDamageResistance = false;
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

// [Formation] (Organizations/Formations/regiment_formations.tgi): regiment_aggressive is the
// manual's Assault, regiment_defensive its Normal/Aggressive, regiment_column Column.
struct FormationDef {
    std::string ids;
    std::string nameKey;
    bool organized = true;
    bool aggressive = false;
    bool herding = false;
    std::string groupFormationIds;            // the group formation a division uses with it
    std::uint32_t groupFormationIndex = kNoDef;
    Fixed speedMultiplier = Fixed::fromInt(1);   // product of (1 + SPEED_MAX MultiplyModifier)
    Fixed attackMultiplier = Fixed::fromInt(1);  // product of (1 + ATTACK_EFFICIENCY_DESIRED ...)
};

// Layout offsets are stored in the sim's regiment frame: +x = forward (the data's +y), +y = left
// (the data's -x; the data's +x is taken to be the regiment's right).
struct LayoutSlotDef {
    std::string property;  // front / support / captain ...
    std::uint32_t number = 1;
    bool primary = false;
    bool required = true;
    bool locked = false;
};
struct LayoutFormationDef {
    std::string formationIds;
    std::uint32_t formationIndex = kNoDef;  // into DefSet::formations
    std::vector<FVec2> offsets;             // per layout element (see LayoutDef::elementSlot)
    Fixed halfWidth;                        // max |lateral offset|: the formation's half width
};
// [OrganizationLayout] (Organizations/Layouts/*.tgi). A regiment's slot i uses layout slot i;
// layout slot j holds `number` elements, so the layout has sum(number) elements. Element e with
// property p gets the k-th position tagged p in a formation, where k counts earlier elements with p.
struct LayoutDef {
    std::string ids;
    std::string defaultFormationIds;
    std::string combatFormationIds;
    std::string retreatFormationIds;
    std::vector<LayoutSlotDef> slots;
    std::vector<std::uint32_t> elementSlot;  // element -> layout slot
    std::vector<LayoutFormationDef> formations;

    // Index into `formations` of the formation with this IDS, or kNoDef.
    std::uint32_t formation(std::string_view formationIds) const;
};
// [GroupFormation] (Game/group_formations.tgi): regiment offsets in a division, sim frame.
struct GroupFormationDef {
    std::string ids;
    std::vector<FVec2> offsets;
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
    std::vector<LayoutDef> layouts;
    std::vector<FormationDef> formations;
    std::vector<GroupFormationDef> groupFormations;
    std::vector<DamageTypeDef> damageTypes;
    std::vector<TargetTypeDef> targetTypes;
    std::vector<PropertyDef> properties;
    std::vector<TerrainDef> terrains;
    std::vector<ProjectileDef> projectiles;
    SVarsDef svars;
    std::uint64_t dataHash = 0;
    DefCompileStats stats;

    // Lookup by IDS (case-insensitive). Returns nullptr / kNoDef when absent.
    std::uint32_t layoutIndex(std::string_view ids) const;
    std::uint32_t formationIndex(std::string_view ids) const;
    std::uint32_t groupFormationIndex(std::string_view ids) const;
    const UnitDef* unit(std::string_view ids) const;
    const RegimentDef* regiment(std::string_view ids) const;
    const BuildingDef* building(std::string_view ids) const;
    const AbilityDef* ability(std::string_view ids) const;
    const NationDef* nation(std::string_view ids) const;
    std::uint32_t unitIndex(std::string_view ids) const;
    std::uint32_t regimentIndex(std::string_view ids) const;
    std::uint32_t buildingIndex(std::string_view ids) const;
    std::uint32_t damageTypeIndex(std::string_view ids) const;
    std::uint32_t targetTypeIndex(std::string_view ids) const;
    std::uint32_t propertyIndex(std::string_view ids) const;
    std::uint32_t terrainIndex(std::string_view ids) const;
    std::uint32_t projectileIndex(std::string_view ids) const;
    // The terrain a map cell's name refers to: a Terrain IDS or one of its Variation IDS.
    std::uint32_t terrainOfName(std::string_view name) const;
};

// Canonical bytes of a DefSet (fixed field order); dataHash = xxh3_64 of these bytes.
std::vector<std::uint8_t> serializeDefs(const DefSet& defs);

}  // namespace corps
