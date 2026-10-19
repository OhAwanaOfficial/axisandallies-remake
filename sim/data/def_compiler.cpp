#include "data/def_compiler.hpp"

#include <algorithm>
#include <map>

#include "core/ascii.hpp"
#include "data/tgi/value_parse.hpp"
#include "session/bytes.hpp"

namespace corps {

using namespace tgi;

namespace {

class Compiler {
public:
    Compiler(const TgiDatabase& db, Diagnostics& diags) : db(db), diags(diags) {}

    DefSet run();

private:
    // --- field access ---------------------------------------------------------------------------
    Symbol sym(std::string_view s) const { return db.symbols.find(s); }

    const ResolvedField* fieldOf(const ResolvedNode* n, std::string_view key) const {
        if (n == nullptr) return nullptr;
        const Symbol k = sym(key);
        return k == kNoSymbol ? nullptr : n->field(k);
    }
    const ResolvedNode* childOf(const ResolvedNode* n, std::string_view name, std::size_t nth = 0) const {
        if (n == nullptr) return nullptr;
        const Symbol k = sym(name);
        return k == kNoSymbol ? nullptr : n->child(k, nth);
    }
    std::size_t countOf(const ResolvedNode* n, std::string_view name) const {
        if (n == nullptr) return 0;
        const Symbol k = sym(name);
        return k == kNoSymbol ? 0 : n->childCount(k);
    }
    // Raw text of a set, non-null value ("" otherwise).
    std::string text(const ResolvedNode* n, std::string_view key) const {
        const ResolvedField* f = fieldOf(n, key);
        if (f == nullptr || !f->hasValue() || isNull(f->text())) return {};
        return std::string(trimAscii(f->text()));
    }
    ValueSite site(const ResolvedField* f, std::string_view key) const {
        ValueSite s;
        s.diags = &diags;
        s.key = key;
        if (f != nullptr && f->value != nullptr) {
            s.file = db.pathOf(f->value->loc.file);
            s.line = f->value->loc.line;
        }
        return s;
    }
    Fixed fx(const ResolvedNode* n, std::string_view key, Fixed fallback = {}) {
        const ResolvedField* f = fieldOf(n, key);
        if (f == nullptr || !f->hasValue() || isNull(f->text())) return fallback;
        const std::uint64_t before = diags.count(Severity::Warning) + diags.count(Severity::Error);
        const Fixed v = readFixed(f->text(), fallback, site(f, key));
        if (diags.count(Severity::Warning) + diags.count(Severity::Error) != before) ++stats.badValues;
        return v;
    }
    bool flag(const ResolvedNode* n, std::string_view key, bool fallback) {
        const ResolvedField* f = fieldOf(n, key);
        if (f == nullptr || !f->hasValue() || isNull(f->text())) return fallback;
        return readBool(f->text(), fallback, site(f, key));
    }
    static std::string locKey(std::string_view s) {
        const auto k = localizedKey(s);
        return k ? std::string(*k) : std::string(s);
    }
    std::string templateName(const Record& r) const {
        return r.node != nullptr ? r.node->parentRef : std::string();
    }

    ResourceAmounts resources(const ResolvedNode* n) {
        ResourceAmounts a;
        a.money = fx(n, "money");
        a.oil = fx(n, "oil");
        a.ammo = fx(n, "ammo");
        a.unitLimitProvided = fx(n, "unit_limit_provided");
        a.unitLimitConsumed = fx(n, "unit_limit_consumed");
        a.buildingLimitProvided = fx(n, "building_limit_provided");
        a.buildingLimitConsumed = fx(n, "building_limit_consumed");
        return a;
    }
    std::vector<std::string> properties(const ResolvedNode* thing) const {
        std::vector<std::string> out;
        const ResolvedNode* pc = childOf(thing, "PropertiesComponent");
        for (std::size_t i = 0; i < countOf(pc, "Property"); ++i) {
            std::string p = text(childOf(pc, "Property", i), "property_IDS");
            if (!p.empty()) out.push_back(asciiLowerCopy(p));
        }
        return out;
    }

    AbilityDef ability(const ResolvedNode* def, std::string ids, std::string owner) {
        AbilityDef a;
        a.ids = std::move(ids);
        a.ownerIds = std::move(owner);
        a.nameKey = locKey(text(def, "name"));
        a.abilityType = asciiLowerCopy(text(def, "ability_type"));
        a.range = fx(def, "range");
        a.minRange = fx(def, "min_range");
        a.rechargeTime = fx(def, "recharge_time");
        a.area = fx(def, "area");
        a.projectileIds = asciiLowerCopy(text(def, "projectile_ids"));
        const std::string targets = text(def, "targets");  // splitWordList returns views into it
        for (const std::string_view t : splitWordList(targets)) a.targets.emplace_back(asciiLowerCopy(t));
        for (std::size_t i = 0; i < countOf(def, "Effect"); ++i) {
            const ResolvedNode* e = childOf(def, "Effect", i);
            EffectDef ef;
            ef.type = text(e, "type");
            ef.damageType = asciiLowerCopy(text(e, "damage_type_ids"));
            ef.amount = fx(e, "amount");
            ef.objectIds = asciiLowerCopy(text(e, "object_ids"));
            a.effects.push_back(std::move(ef));
        }
        return a;
    }

    // Inline abilities of a unit/building: PerformerComponent/Ability[i]/AbilityDefinition.
    std::vector<std::uint32_t> inlineAbilities(const ResolvedNode* thing, const std::string& owner) {
        std::vector<std::uint32_t> out;
        const ResolvedNode* pc = childOf(thing, "PerformerComponent");
        for (std::size_t i = 0; i < countOf(pc, "Ability"); ++i) {
            const ResolvedNode* def = childOf(childOf(pc, "Ability", i), "AbilityDefinition");
            if (def == nullptr) continue;
            out.push_back(static_cast<std::uint32_t>(defs.abilities.size()));
            defs.abilities.push_back(ability(def, owner + "#" + std::to_string(i), owner));
        }
        return out;
    }

    void unit(const Record& r, const std::string& ids, const std::string& category) {
        const ResolvedNode* t = &r.tree;
        UnitDef u;
        u.ids = ids;
        u.category = category;
        u.nameKey = locKey(text(t, "name"));
        u.templateName = templateName(r);
        const ResolvedNode* body = childOf(t, "BodyComponent");
        u.health = fx(body, "health");
        u.defense = fx(body, "defense");
        const ResolvedNode* mover = childOf(t, "MoverComponent");
        u.speedMax = fx(mover, "speed_max");
        u.acceleration = fx(mover, "acceleration");
        u.turnRadius = fx(mover, "turn_radius");
        const ResolvedNode* det = childOf(t, "DetectionComponent");
        u.detectionRange = fx(det, "detection_range");
        u.remoteDetectionRange = fx(det, "remote_detection_range");
        u.blockingRadius = fx(childOf(t, "Size"), "blocking_radius");
        u.abilities = inlineAbilities(t, ids);
        u.properties = properties(t);
        u.requiredStructures = asciiLowerCopy(text(childOf(t, "ElementComponent"), "required_structures"));
        const ResolvedNode* econ = childOf(t, "EconomyComponent");
        u.cost = resources(childOf(econ, "Cost"));
        u.upkeep = resources(childOf(econ, "Upkeep"));
        u.resourcePrefix = text(childOf(t, "View"), "resource_prefix");
        defs.units.push_back(std::move(u));
    }

    void regiment(const Record& r, const std::string& ids) {
        const ResolvedNode* t = &r.tree;
        RegimentDef g;
        g.ids = ids;
        g.nameKey = locKey(text(t, "name"));
        g.templateName = templateName(r);
        const ResolvedNode* org = childOf(t, "OrganizationComponent");
        g.layoutIds = asciiLowerCopy(text(org, "layout_IDS"));
        for (std::size_t i = 0; i < countOf(org, "Slot"); ++i) {
            g.slotIds.push_back(asciiLowerCopy(text(childOf(org, "Slot", i), "element_IDS")));
        }
        g.controlRange = fx(org, "control_range");
        g.operationalRange = fx(org, "operational_range");
        g.moraleBreakpoint = fx(org, "morale_breakpoint");
        g.entrenchmentTime = fx(org, "entrenchment_time");
        g.recruitTime = fx(org, "recruit_time");
        g.shortfallDamageRate = fx(org, "shortfall_damage_rate");
        g.recruitFull = flag(org, "recruit_full", false);
        g.inSupplyDetached = flag(org, "in_supply_detached", true);
        g.detectionRange = fx(childOf(t, "DetectionComponent"), "detection_range");
        g.transportActor = asciiLowerCopy(text(t, "transport_actor"));
        g.properties = properties(t);
        const ResolvedNode* econ = childOf(t, "EconomyComponent");
        g.cost = resources(childOf(econ, "Cost"));
        g.upkeep = resources(childOf(econ, "Upkeep"));
        defs.regiments.push_back(std::move(g));
    }

    void building(const Record& r, const std::string& ids) {
        const ResolvedNode* t = &r.tree;
        BuildingDef b;
        b.ids = ids;
        b.nameKey = locKey(text(t, "name"));
        b.templateName = templateName(r);
        const ResolvedNode* body = childOf(t, "BodyComponent");
        b.health = fx(body, "health");
        b.defense = fx(body, "defense");
        b.blockingRadius = fx(childOf(t, "Size"), "blocking_radius");
        b.abilities = inlineAbilities(t, ids);
        const ResolvedNode* rc = childOf(t, "RecruitComponent");
        for (std::size_t i = 0; i < countOf(rc, "RecruitActor"); ++i) {
            const std::string a = asciiLowerCopy(text(childOf(rc, "RecruitActor", i), "actor_ids"));
            if (!a.empty()) b.recruits.push_back(a);
        }
        b.properties = properties(t);
        const ResolvedNode* econ = childOf(t, "EconomyComponent");
        b.cost = resources(childOf(econ, "Cost"));
        b.upkeep = resources(childOf(econ, "Upkeep"));
        b.production = resources(childOf(econ, "Production"));
        defs.buildings.push_back(std::move(b));
    }

    void nation(const Record& r, const std::string& ids) {
        const ResolvedNode* t = &r.tree;
        NationDef n;
        n.ids = ids;
        n.name = text(t, "name");
        const std::string factions = text(t, "factions");
        for (const std::string_view f : splitWordList(factions)) n.factions.emplace_back(asciiLowerCopy(f));
        for (std::size_t i = 0; i < countOf(t, "Placeholder"); ++i) {
            const ResolvedNode* p = childOf(t, "Placeholder", i);
            n.placeholders.emplace_back(asciiLowerCopy(text(p, "placeholder_IDS")), asciiLowerCopy(text(p, "thing_IDS")));
        }
        n.resourcePrefix = text(t, "resource_prefix");
        defs.nations.push_back(std::move(n));
    }

    void svars(const ResolvedNode& merged) {
        for (const ResolvedField& f : merged.fields) {
            if (!f.hasValue()) continue;
            SVar v;
            v.name = std::string(f.spelling);
            v.text = std::string(trimAscii(f.text()));
            switch (f.type) {
            case TypeWord::Fixed: case TypeWord::Float: case TypeWord::KF32: v.type = SVarType::Fixed; break;
            case TypeWord::Int: v.type = SVarType::Int; break;
            case TypeWord::Flag: case TypeWord::Bool: v.type = SVarType::Bool; break;
            default: v.type = SVarType::String; break;
            }
            const ValueSite s = site(&f, v.name);
            if (v.type == SVarType::Fixed) {
                v.fixedValue = readFixed(v.text, Fixed{}, s);
                v.intValue = v.fixedValue.floorInt();
            } else if (v.type == SVarType::Int) {
                v.intValue = readInt(v.text, 0, s);
                v.fixedValue = Fixed::fromRaw(v.intValue * Fixed::kOne);
            } else if (v.type == SVarType::Bool) {
                v.boolValue = readBool(v.text, false, s);
            } else {
                // Untyped/string vars: keep numeric views when the text happens to be numeric.
                const FixedParse p = parseFixed(v.text);
                if (p.ok) v.fixedValue = p.value;
                if (const auto b = parseBool(v.text)) v.boolValue = *b;
            }
            defs.svars.vars.push_back(std::move(v));
        }
        std::sort(defs.svars.vars.begin(), defs.svars.vars.end(),
                  [](const SVar& a, const SVar& b) { return asciiLowerCopy(a.name) < asciiLowerCopy(b.name); });
    }

    const TgiDatabase& db;
    Diagnostics& diags;
    DefSet defs;
    DefCompileStats stats;
};

template <class T>
void sortByIds(std::vector<T>& v) {
    std::stable_sort(v.begin(), v.end(), [](const T& a, const T& b) { return asciiLowerCopy(a.ids) < asciiLowerCopy(b.ids); });
}

DefSet Compiler::run() {
    const Symbol thingSym = sym("thing");
    const Symbol abilitySym = sym("abilitydefinition");
    const Symbol nationSym = sym("nation");
    const Symbol svarsSym = sym("svars");
    std::map<std::string, std::size_t> categories;

    // byIds is sorted by (type symbol, IDS), so this walk is deterministic.
    for (const DefIndex::ByIdsEntry& e : db.defs.byIds) {
        const Record& r = db.records[e.record];
        const std::string ids = e.idsKey;
        if (e.type == thingSym) {
            ++stats.thingsSeen;
            const std::string category = asciiUpperCopy(text(&r.tree, "category"));
            ++categories[category];
            if (category == "UNIT" || category == "TRUCK") unit(r, ids, category);
            else if (category == "ORGANIZATION") regiment(r, ids);
            else if (category == "BUILDING") building(r, ids);
            else ++stats.thingsSkipped;
        } else if (e.type == abilitySym) {
            defs.abilities.push_back(ability(&r.tree, ids, ""));
        } else if (e.type == nationSym) {
            nation(r, ids);
        }
    }
    for (const DefIndex::Singleton& s : db.defs.singletons) {
        if (s.type == svarsSym) svars(s.merged);
    }

    // Inline abilities were appended in unit/building order; sort everything by IDS and remap.
    std::vector<std::string> abilityIdsByOld;
    abilityIdsByOld.reserve(defs.abilities.size());
    for (const AbilityDef& a : defs.abilities) abilityIdsByOld.push_back(asciiLowerCopy(a.ids));
    sortByIds(defs.abilities);
    sortByIds(defs.units);
    sortByIds(defs.regiments);
    sortByIds(defs.buildings);
    sortByIds(defs.nations);
    std::vector<std::pair<std::string, std::uint32_t>> abilityIndex;  // (lower IDS, new index), sorted
    abilityIndex.reserve(defs.abilities.size());
    for (std::uint32_t k = 0; k < defs.abilities.size(); ++k) abilityIndex.emplace_back(asciiLowerCopy(defs.abilities[k].ids), k);
    std::sort(abilityIndex.begin(), abilityIndex.end());
    const auto remap = [&](std::vector<std::uint32_t>& list) {
        for (std::uint32_t& i : list) {
            const std::string& key = abilityIdsByOld[i];
            const auto it = std::lower_bound(abilityIndex.begin(), abilityIndex.end(), key,
                                             [](const auto& e, const std::string& k) { return e.first < k; });
            i = (it != abilityIndex.end() && it->first == key) ? it->second : kNoDef;
        }
    };
    for (UnitDef& u : defs.units) remap(u.abilities);
    for (BuildingDef& b : defs.buildings) remap(b.abilities);

    // Regiment slots -> unit (or truck) / building indexes. Empty (null) slots stay None.
    for (RegimentDef& g : defs.regiments) {
        for (const std::string& s : g.slotIds) {
            ElementRef ref;
            if (const std::uint32_t ui = defs.unitIndex(s); ui != kNoDef) {
                ref = {ElementKind::Unit, ui};
            } else if (const std::uint32_t bi = defs.buildingIndex(s); bi != kNoDef) {
                ref = {ElementKind::Building, bi};
            } else if (!s.empty()) {
                ++stats.missingSlotUnits;
                diags.report(DiagCode::BadValue, "", 0, "regiment " + g.ids + ": slot element '" + s + "' is not a unit or building");
            }
            g.slots.push_back(ref);
        }
    }

    stats.categories.assign(categories.begin(), categories.end());
    defs.stats = stats;
    defs.dataHash = hashBytes(serializeDefs(defs));
    return std::move(defs);
}

}  // namespace

DefSet compileDefs(const TgiDatabase& db, Diagnostics& diags) {
    return Compiler(db, diags).run();
}

}  // namespace corps
