#pragma once

// Combat lookups derived once per Sim from the DefSet, the map and the tuning (GAME_DESIGN §6.3).
// Not state: a pure function of the config, rebuilt identically after a load.
//
// * Profiles: per unit and building def, the intrinsic properties as indexes, the target types
//   they grant (enabled_target_flags), defense + DV, and per-damage-type resistance sums.
// * Situational properties: terrain (the Terrain record a map cell names; generated maps' forest)
//   and entrenchment apply only when their required_properties are all on the target.
// * Ability rules: the target filter ("+enemy, -air, ...": the target must be an enemy and have
//   every + type and none of the - types; other words are ignored) and the damage effects.

#include <cstdint>
#include <vector>

#include "combat/tuning.hpp"
#include "data/defs.hpp"

namespace corps::nav {
struct MapDef;
}

namespace corps {

struct DamageHit {
    std::uint32_t type = 0;  // DefSet::damageTypes index
    Fixed amount;
};

struct TargetFilter {
    bool enemy = false;       // +enemy
    bool friendly = false;    // +friend
    std::uint64_t require = 0;  // target-type bits that must all be present
    std::uint64_t forbid = 0;   // target-type bits that must all be absent
};

struct AbilityRules {
    TargetFilter filter;
    std::vector<DamageHit> damage;  // MAIN_DAMAGE and ADD_DAMAGE effects
    Fixed moraleLoss;               // MORALE_LOSS effect amounts: a morale "breaker" (mortars, artillery)
    bool attack = false;            // has damage and can hit enemies
};

struct CombatProfile {
    std::vector<std::uint32_t> properties;  // DefSet::properties indexes, sorted
    std::uint64_t targetTypes = 0;
    Fixed defense;                          // BodyComponent defense + intrinsic DV
    std::vector<Fixed> resist;              // per damage type
};

// A target's defense and resistances where it stands now.
struct TargetStats {
    Fixed defense;
    std::vector<Fixed> resist;
};

class CombatRules {
public:
    CombatRules(const DefSet* defs, const nav::MapDef* map, const CombatTuning& tuning);

    const CombatTuning& tuning() const noexcept { return tuning_; }
    std::size_t damageTypeCount() const noexcept { return damageTypes_; }
    const CombatProfile* unit(std::uint32_t def) const { return def < units_.size() ? &units_[def] : nullptr; }
    const CombatProfile* building(std::uint32_t def) const { return def < buildings_.size() ? &buildings_[def] : nullptr; }
    const AbilityRules* ability(std::uint32_t a) const { return a < abilities_.size() ? &abilities_[a] : nullptr; }

    // Stats of a target with profile `p` standing on map terrain `terrainId` (MapDef::terrainId of
    // its cell; 0xFFFF for none), entrenched or not.
    TargetStats statsOf(const CombatProfile& p, std::uint32_t terrainId, bool entrenched) const;

    // Damage one hit does after resistances, defense and the attacker's multiplier (≥ 0).
    Fixed damage(const DamageHit& hit, const TargetStats& target, Fixed attackMultiplier) const;

    // Whether an ability with `filter` may hit a target with `targetTypes`, enemy or not.
    static bool passes(const TargetFilter& filter, bool enemy, std::uint64_t targetTypes);

private:
    void addProperty(TargetStats& s, std::uint32_t property) const;
    bool applies(const CombatProfile& p, std::uint32_t property) const;
    CombatProfile profileOf(const std::vector<std::string>& properties, Fixed defense) const;

    const DefSet* defs_ = nullptr;
    CombatTuning tuning_;
    std::size_t damageTypes_ = 0;
    std::vector<CombatProfile> units_;
    std::vector<CombatProfile> buildings_;
    std::vector<AbilityRules> abilities_;
    std::vector<std::vector<std::uint32_t>> terrainProperties_;  // per MapDef terrain name index
    std::vector<std::uint32_t> entrenchProperties_;              // infantry_entrench, vehicle_entrench
};

}  // namespace corps
