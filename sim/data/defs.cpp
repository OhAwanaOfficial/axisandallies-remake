#include "data/defs.hpp"

#include <algorithm>

#include "core/ascii.hpp"
#include "session/bytes.hpp"

namespace corps {

namespace {

// Def vectors are sorted by lower-case IDS (compileDefs), so lookups are binary searches.
template <class T>
std::uint32_t findIndex(const std::vector<T>& v, std::string_view ids) {
    const std::string key = asciiLowerCopy(ids);
    const auto it = std::lower_bound(v.begin(), v.end(), key, [](const T& d, const std::string& k) { return asciiLowerCopy(d.ids) < k; });
    return (it != v.end() && iequals(it->ids, key)) ? static_cast<std::uint32_t>(it - v.begin()) : kNoDef;
}

template <class T>
const T* findDef(const std::vector<T>& v, std::string_view ids) {
    const std::uint32_t i = findIndex(v, ids);
    return i == kNoDef ? nullptr : &v[i];
}

void put(ByteWriter& w, const ResourceAmounts& a) {
    w.fixed(a.money);
    w.fixed(a.oil);
    w.fixed(a.ammo);
    w.fixed(a.unitLimitProvided);
    w.fixed(a.unitLimitConsumed);
    w.fixed(a.buildingLimitProvided);
    w.fixed(a.buildingLimitConsumed);
}

void putStrings(ByteWriter& w, const std::vector<std::string>& v) {
    w.u32(static_cast<std::uint32_t>(v.size()));
    for (const auto& s : v) w.str(s);
}

void putIndexes(ByteWriter& w, const std::vector<std::uint32_t>& v) {
    w.u32(static_cast<std::uint32_t>(v.size()));
    for (const std::uint32_t i : v) w.u32(i);
}

}  // namespace

const UnitDef* DefSet::unit(std::string_view ids) const { return findDef(units, ids); }
const RegimentDef* DefSet::regiment(std::string_view ids) const { return findDef(regiments, ids); }
const BuildingDef* DefSet::building(std::string_view ids) const { return findDef(buildings, ids); }
const AbilityDef* DefSet::ability(std::string_view ids) const { return findDef(abilities, ids); }
const NationDef* DefSet::nation(std::string_view ids) const { return findDef(nations, ids); }
std::uint32_t DefSet::unitIndex(std::string_view ids) const { return findIndex(units, ids); }
std::uint32_t DefSet::regimentIndex(std::string_view ids) const { return findIndex(regiments, ids); }
std::uint32_t DefSet::buildingIndex(std::string_view ids) const { return findIndex(buildings, ids); }

const SVar* SVarsDef::find(std::string_view name) const {
    const std::string key = asciiLowerCopy(name);
    const auto it = std::lower_bound(vars.begin(), vars.end(), key, [](const SVar& v, const std::string& k) { return asciiLowerCopy(v.name) < k; });
    return (it != vars.end() && iequals(it->name, key)) ? &*it : nullptr;
}

Fixed SVarsDef::getFixed(std::string_view name, Fixed fallback) const {
    const SVar* v = find(name);
    return v != nullptr && (v->type == SVarType::Fixed || v->type == SVarType::Int) ? v->fixedValue : fallback;
}

std::int64_t SVarsDef::getInt(std::string_view name, std::int64_t fallback) const {
    const SVar* v = find(name);
    return v != nullptr && (v->type == SVarType::Int || v->type == SVarType::Fixed) ? v->intValue : fallback;
}

bool SVarsDef::getBool(std::string_view name, bool fallback) const {
    const SVar* v = find(name);
    return v != nullptr && v->type == SVarType::Bool ? v->boolValue : fallback;
}

std::vector<std::uint8_t> serializeDefs(const DefSet& d) {
    ByteWriter w;
    w.tag("DEFS");
    w.u32(1);  // layout version
    w.u32(static_cast<std::uint32_t>(d.abilities.size()));
    for (const AbilityDef& a : d.abilities) {
        w.str(a.ids);
        w.str(a.ownerIds);
        w.str(a.nameKey);
        w.str(a.abilityType);
        w.fixed(a.range);
        w.fixed(a.minRange);
        w.fixed(a.rechargeTime);
        w.fixed(a.area);
        w.str(a.projectileIds);
        putStrings(w, a.targets);
        w.u32(static_cast<std::uint32_t>(a.effects.size()));
        for (const EffectDef& e : a.effects) {
            w.str(e.type);
            w.str(e.damageType);
            w.fixed(e.amount);
            w.str(e.objectIds);
        }
    }
    w.u32(static_cast<std::uint32_t>(d.units.size()));
    for (const UnitDef& u : d.units) {
        w.str(u.ids);
        w.str(u.nameKey);
        w.str(u.templateName);
        w.str(u.category);
        w.fixed(u.health);
        w.fixed(u.defense);
        w.fixed(u.speedMax);
        w.fixed(u.acceleration);
        w.fixed(u.turnRadius);
        w.fixed(u.detectionRange);
        w.fixed(u.remoteDetectionRange);
        w.fixed(u.blockingRadius);
        putIndexes(w, u.abilities);
        putStrings(w, u.properties);
        w.str(u.requiredStructures);
        put(w, u.cost);
        put(w, u.upkeep);
        w.str(u.resourcePrefix);
    }
    w.u32(static_cast<std::uint32_t>(d.regiments.size()));
    for (const RegimentDef& g : d.regiments) {
        w.str(g.ids);
        w.str(g.nameKey);
        w.str(g.templateName);
        w.str(g.layoutIds);
        putStrings(w, g.slotIds);
        w.u32(static_cast<std::uint32_t>(g.slots.size()));
        for (const ElementRef& e : g.slots) {
            w.u8(static_cast<std::uint8_t>(e.kind));
            w.u32(e.index);
        }
        w.fixed(g.controlRange);
        w.fixed(g.operationalRange);
        w.fixed(g.moraleBreakpoint);
        w.fixed(g.entrenchmentTime);
        w.fixed(g.recruitTime);
        w.fixed(g.shortfallDamageRate);
        w.fixed(g.detectionRange);
        w.boolean(g.recruitFull);
        w.boolean(g.inSupplyDetached);
        w.str(g.transportActor);
        putStrings(w, g.properties);
        put(w, g.cost);
        put(w, g.upkeep);
    }
    w.u32(static_cast<std::uint32_t>(d.buildings.size()));
    for (const BuildingDef& b : d.buildings) {
        w.str(b.ids);
        w.str(b.nameKey);
        w.str(b.templateName);
        w.fixed(b.health);
        w.fixed(b.defense);
        w.fixed(b.blockingRadius);
        putIndexes(w, b.abilities);
        putStrings(w, b.recruits);
        putStrings(w, b.properties);
        put(w, b.cost);
        put(w, b.upkeep);
        put(w, b.production);
    }
    w.u32(static_cast<std::uint32_t>(d.nations.size()));
    for (const NationDef& n : d.nations) {
        w.str(n.ids);
        w.str(n.name);
        putStrings(w, n.factions);
        w.u32(static_cast<std::uint32_t>(n.placeholders.size()));
        for (const auto& [p, t] : n.placeholders) {
            w.str(p);
            w.str(t);
        }
        w.str(n.resourcePrefix);
    }
    w.u32(static_cast<std::uint32_t>(d.svars.vars.size()));
    for (const SVar& v : d.svars.vars) {
        w.str(v.name);
        w.u8(static_cast<std::uint8_t>(v.type));
        w.str(v.text);
        w.fixed(v.fixedValue);
        w.i64(v.intValue);
        w.boolean(v.boolValue);
    }
    return std::move(w.buf);
}

}  // namespace corps
