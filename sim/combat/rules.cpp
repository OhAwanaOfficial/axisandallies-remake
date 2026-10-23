#include "combat/rules.hpp"

#include <algorithm>

#include "core/ascii.hpp"
#include "data/tgi/value_parse.hpp"
#include "nav/map_def.hpp"

namespace corps {

namespace {

// Generated test maps name their cells gen_<kind>; forest cells get the data's forest properties.
const char* genTerrainProperties(std::string_view name, int which) {
    if (iequals(name, "gen_forest")) return which == 0 ? "terrain_forest_infantry" : "terrain_forest_vehicles";
    return nullptr;
}

}  // namespace

CombatRules::CombatRules(const DefSet* defs, const nav::MapDef* map, const CombatTuning& tuning) : defs_(defs), tuning_(tuning) {
    if (defs == nullptr) return;
    const DefSet& d = *defs;
    damageTypes_ = d.damageTypes.size();
    for (const UnitDef& u : d.units) units_.push_back(profileOf(u.properties, u.defense));
    for (const BuildingDef& b : d.buildings) buildings_.push_back(profileOf(b.properties, b.defense));

    for (const AbilityDef& a : d.abilities) {
        AbilityRules r;
        for (const std::string& term : a.targets) {
            if (term.size() < 2 || (term[0] != '+' && term[0] != '-')) continue;
            const bool plus = term[0] == '+';
            const std::string_view name = std::string_view(term).substr(1);
            if (iequals(name, "enemy")) {
                r.filter.enemy = plus;
            } else if (iequals(name, "friend")) {
                r.filter.friendly = plus;
            } else if (const std::uint32_t t = d.targetTypeIndex(name); t != kNoDef && t < 64) {
                (plus ? r.filter.require : r.filter.forbid) |= std::uint64_t{1} << t;
            }
        }
        for (const EffectDef& e : a.effects) {
            if (!iequals(e.type, "MAIN_DAMAGE") && !iequals(e.type, "ADD_DAMAGE")) continue;
            const std::uint32_t t = d.damageTypeIndex(e.damageType);
            if (t == kNoDef || e.amount <= Fixed{}) continue;
            r.damage.push_back({t, e.amount});
        }
        r.attack = !r.damage.empty() && r.filter.enemy && iequals(a.abilityType, "attack");
        abilities_.push_back(std::move(r));
    }

    if (map != nullptr) {
        for (const std::string& name : map->terrainNames) {
            std::vector<std::uint32_t> props;
            const auto add = [&](std::string_view ids) {
                const std::uint32_t p = d.propertyIndex(ids);
                if (p != kNoDef) props.push_back(p);
            };
            if (genTerrainProperties(name, 0) != nullptr) {
                add(genTerrainProperties(name, 0));
                add(genTerrainProperties(name, 1));
            } else if (const std::uint32_t t = d.terrainOfName(name); t != kNoDef) {
                for (const std::string& p : d.terrains[t].properties) add(p);
            }
            terrainProperties_.push_back(std::move(props));
        }
    }
    for (const char* ids : {"infantry_entrench", "vehicle_entrench"}) {
        if (const std::uint32_t p = d.propertyIndex(ids); p != kNoDef) entrenchProperties_.push_back(p);
    }
}

CombatProfile CombatRules::profileOf(const std::vector<std::string>& properties, Fixed defense) const {
    CombatProfile p;
    p.resist.assign(damageTypes_, Fixed{});
    p.defense = defense;
    for (const std::string& ids : properties) {
        const std::uint32_t i = defs_->propertyIndex(ids);
        if (i == kNoDef) continue;
        p.properties.push_back(i);
        const PropertyDef& prop = defs_->properties[i];
        for (const std::string& flag : prop.targetFlags) {
            const std::uint32_t t = defs_->targetTypeIndex(flag);
            if (t != kNoDef && t < 64) p.targetTypes |= std::uint64_t{1} << t;
        }
        p.defense = p.defense + prop.defenseAdd;
        for (const auto& [type, amount] : prop.resist) {
            const std::uint32_t t = defs_->damageTypeIndex(type);
            if (t != kNoDef) p.resist[t] = p.resist[t] + amount;
        }
    }
    std::sort(p.properties.begin(), p.properties.end());
    p.properties.erase(std::unique(p.properties.begin(), p.properties.end()), p.properties.end());
    return p;
}

bool CombatRules::applies(const CombatProfile& p, std::uint32_t property) const {
    for (const std::string& req : defs_->properties[property].requiredProperties) {
        const std::uint32_t r = defs_->propertyIndex(req);
        if (r == kNoDef || !std::binary_search(p.properties.begin(), p.properties.end(), r)) return false;
    }
    return true;
}

void CombatRules::addProperty(TargetStats& s, std::uint32_t property) const {
    const PropertyDef& prop = defs_->properties[property];
    s.defense = s.defense + prop.defenseAdd;
    for (const auto& [type, amount] : prop.resist) {
        const std::uint32_t t = defs_->damageTypeIndex(type);
        if (t != kNoDef) s.resist[t] = s.resist[t] + amount;
    }
}

TargetStats CombatRules::statsOf(const CombatProfile& p, std::uint32_t terrainId, bool entrenched) const {
    TargetStats s{p.defense, p.resist};
    if (defs_ == nullptr) return s;
    if (terrainId < terrainProperties_.size()) {
        for (const std::uint32_t prop : terrainProperties_[terrainId]) {
            if (applies(p, prop)) addProperty(s, prop);
        }
    }
    if (entrenched) {
        for (const std::uint32_t prop : entrenchProperties_) {
            if (applies(p, prop)) addProperty(s, prop);
        }
    }
    return s;
}

Fixed CombatRules::damage(const DamageHit& hit, const TargetStats& target, Fixed attackMultiplier) const {
    if (hit.amount <= Fixed{} || attackMultiplier <= Fixed{}) return Fixed{};
    const Fixed one = Fixed::fromInt(1);
    const Fixed resist = hit.type < target.resist.size() ? std::clamp(target.resist[hit.type], tuning_.resistMin, tuning_.resistMax) : Fixed{};
    const Fixed raw = hit.amount * (one - resist);
    const Fixed defense = max(target.defense, Fixed{});
    const Fixed mitigated = raw * tuning_.defenseK / (tuning_.defenseK + tuning_.defenseD * defense);
    return max(mitigated, tuning_.minDamage) * attackMultiplier;
}

bool CombatRules::passes(const TargetFilter& f, bool enemy, std::uint64_t types) {
    if (f.enemy && !enemy) return false;
    if (f.friendly && enemy) return false;
    if ((types & f.require) != f.require) return false;
    return (types & f.forbid) == 0;
}

}  // namespace corps
