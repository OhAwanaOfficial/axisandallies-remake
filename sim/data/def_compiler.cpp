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
        a.volleyArea = fx(def, "volley_area");
        a.maxTargets = static_cast<std::uint32_t>(std::clamp<std::int64_t>(fx(def, "max_targets").floorInt(), 0, 1000000));
        a.attenuationRangeMin = fx(def, "attenuation_range_min", Fixed::fromInt(1));
        a.attenuationFactorMax = fx(def, "attenuation_factor_max", Fixed::fromInt(1));
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

    std::vector<std::string> words(const ResolvedNode* n, std::string_view key) const {
        std::vector<std::string> out;
        const std::string t = text(n, key);  // splitWordList returns views into it
        for (const std::string_view w : splitWordList(t)) out.emplace_back(asciiLowerCopy(w));
        return out;
    }

    // Turret[i] of a PerformerComponent / DefenderComponent, appended to `out`.
    void turretsOf(const ResolvedNode* comp, std::vector<TurretDef>& out) {
        for (std::size_t i = 0; i < countOf(comp, "Turret"); ++i) {
            const ResolvedNode* t = childOf(comp, "Turret", i);
            TurretDef d;
            d.ids = asciiLowerCopy(text(t, "turret_ids"));
            d.yawRate = fx(t, "yaw_rotation_per_sec_degrees", Fixed::fromInt(360));
            d.yawRange = fx(t, "yaw_rotation_range_degrees", Fixed::fromInt(360));
            d.yawStart = fx(t, "yaw_start_degrees");
            d.resetTime = fx(t, "reset_time");
            d.canReset = flag(t, "can_reset", true);
            d.canPreaim = flag(t, "can_preaim", true);
            out.push_back(std::move(d));
        }
    }

    // Inline abilities of a unit/building component: Ability[i]/AbilityDefinition, numbered
    // "<owner>#<n>" from `next` on. Weapons get the abilities' pre-sort indexes (remapped later).
    void weaponsOf(const ResolvedNode* comp, const std::string& owner, std::size_t& next, std::vector<std::uint32_t>& abilities,
                   std::vector<WeaponDef>& weapons, const std::vector<TurretDef>& turrets) {
        for (std::size_t i = 0; i < countOf(comp, "Ability"); ++i, ++next) {
            const ResolvedNode* slot = childOf(comp, "Ability", i);
            const ResolvedNode* def = childOf(slot, "AbilityDefinition");
            if (def == nullptr) continue;
            const auto index = static_cast<std::uint32_t>(defs.abilities.size());
            defs.abilities.push_back(ability(def, owner + "#" + std::to_string(next), owner));
            abilities.push_back(index);
            WeaponDef w;
            w.ability = index;
            w.priority = fx(slot, "priority");
            w.fuzzyRange = fx(slot, "fuzzy_range");
            w.reloadTime = fx(slot, "reload_time");
            const std::string turret = asciiLowerCopy(text(slot, "turret_ids"));
            for (std::uint32_t k = 0; !turret.empty() && k < turrets.size(); ++k) {
                if (turrets[k].ids == turret) {
                    w.turret = k;
                    break;
                }
            }
            weapons.push_back(w);
        }
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
        const std::string turn = asciiLowerCopy(text(mover, "turn_type"));
        u.turnType = turn == "in_place" ? TurnType::InPlace : turn == "while_moving" ? TurnType::WhileMoving : TurnType::Instant;
        const ResolvedNode* det = childOf(t, "DetectionComponent");
        u.detectionRange = fx(det, "detection_range");
        u.remoteDetectionRange = fx(det, "remote_detection_range");
        u.blockingRadius = fx(childOf(t, "Size"), "blocking_radius");
        const ResolvedNode* perf = childOf(t, "PerformerComponent");
        turretsOf(perf, u.turrets);
        std::size_t next = 0;
        weaponsOf(perf, ids, next, u.abilities, u.weapons, u.turrets);
        u.checkDamageResistance = flag(perf, "check_damage_resistance", false);
        u.moraleValue = fx(childOf(t, "ElementComponent"), "morale_value");
        u.properties = properties(t);
        const auto has = [&](std::string_view p) { return std::find(u.properties.begin(), u.properties.end(), p) != u.properties.end(); };
        u.moveClass = has("aircraft") ? UnitMoveClass::Air
                      : has("seacraft") ? UnitMoveClass::Naval
                      : has("tank") ? UnitMoveClass::Tracked
                      : has("vehicle") ? UnitMoveClass::Wheeled
                                       : UnitMoveClass::Foot;
        const ResolvedNode* element = childOf(t, "ElementComponent");
        u.requiredStructures = asciiLowerCopy(text(element, "required_structures"));
        u.requiredStructureIds = words(element, "required_structures");
        u.requiredProperties = words(element, "required_properties");
        const ResolvedNode* econ = childOf(t, "EconomyComponent");
        u.cost = resources(childOf(econ, "Cost"));
        u.upkeep = resources(childOf(econ, "Upkeep"));
        u.production = resources(childOf(econ, "Production"));
        u.resourcePrefix = text(childOf(t, "View"), "resource_prefix");
        const ResolvedNode* mobile = childOf(t, "MobileDeploymentComponent");
        u.unpackToIds = asciiLowerCopy(text(mobile, "unpack_to_actor_ids"));
        u.unpackingLength = fx(mobile, "unpacking_length");
        u.resupplyRate = fx(element, "resupply_rate");
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
        g.supplyUsed = flag(org, "supply_used", true);
        g.hq = childOf(t, "HQComponent") != nullptr;
        g.detectionRange = fx(childOf(t, "DetectionComponent"), "detection_range");
        g.transportActor = asciiLowerCopy(text(t, "transport_actor"));
        g.properties = properties(t);
        const ResolvedNode* econ = childOf(t, "EconomyComponent");
        g.cost = resources(childOf(econ, "Cost"));
        g.upkeep = resources(childOf(econ, "Upkeep"));
        const ResolvedNode* xp = childOf(t, "ExperienceComponent");
        g.fakeCombatValue = fx(xp, "fake_cv_value", Fixed::fromInt(-1));
        for (std::size_t i = 0; i < countOf(xp, "ExperienceLevel"); ++i) {
            const ResolvedNode* lv = childOf(xp, "ExperienceLevel", i);
            ExperienceLevelDef l;
            l.required = fx(lv, "experience_required");
            const Fixed one = Fixed::fromInt(1);
            for (const ModifierDef& m : modifiers(childOf(lv, "Property"))) {
                if (m.attribute == "AV" && m.op == ModifierDef::Op::Add) l.attack = l.attack + m.amount;
                if (m.attribute == "DV" && m.op == ModifierDef::Op::Add) l.defense = l.defense + m.amount;
                if (m.attribute == "MORALE" && m.op == ModifierDef::Op::Add) l.morale = l.morale + m.amount;
                if (m.attribute == "HEALTH" && m.op == ModifierDef::Op::Multiply) l.healthMultiplier = l.healthMultiplier * (one + m.amount);
                if (m.attribute == "MORALE_RECOVERY_RATE" && m.op == ModifierDef::Op::Multiply) {
                    l.moraleRecovery = l.moraleRecovery * (one + m.amount);
                }
            }
            g.experienceLevels.push_back(l);
        }
        std::stable_sort(g.experienceLevels.begin(), g.experienceLevels.end(),
                         [](const ExperienceLevelDef& a, const ExperienceLevelDef& b) { return a.required < b.required; });
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
        const ResolvedNode* perf = childOf(t, "PerformerComponent");
        const ResolvedNode* defender = childOf(t, "DefenderComponent");
        turretsOf(perf, b.turrets);
        turretsOf(defender, b.turrets);
        std::size_t next = 0;
        weaponsOf(perf, ids, next, b.abilities, b.weapons, b.turrets);
        weaponsOf(defender, ids, next, b.abilities, b.weapons, b.turrets);
        b.controlRange = fx(defender, "control_range");
        b.moraleValue = fx(childOf(t, "ElementComponent"), "morale_value");
        b.checkDamageResistance = flag(perf, "check_damage_resistance", false) || flag(defender, "check_damage_resistance", false);
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
        const ResolvedNode* size = childOf(t, "Size");
        const std::string shape = asciiUpperCopy(text(size, "blocking_shape"));
        b.shape = shape == "SQUARE" ? FootprintShape::Square : shape == "RECT" ? FootprintShape::Rect : FootprintShape::Circle;
        b.squareSize = fx(size, "square_size");
        b.rectWidth = fx(size, "rect_width");
        b.rectHeight = fx(size, "rect_height");
        b.padding = fx(childOf(t, "BaseTerrain"), "padding");
        const ResolvedNode* flags = childOf(t, "Flags");
        b.tileLocked = flag(flags, "tile_locked", false);
        b.axisLocked = flag(flags, "axis_locked", false);
        b.providesCorps = std::find(b.properties.begin(), b.properties.end(), "corpshq") != b.properties.end();
        const ResolvedNode* mobile = childOf(t, "MobileDeploymentComponent");
        b.mobile = mobile != nullptr;
        b.packHealthMin = fx(mobile, "pack_health_min");
        b.packingLength = fx(mobile, "packing_length");
        b.unpackingLength = fx(mobile, "unpacking_length");
        const ResolvedNode* supply = childOf(t, "SupplyComponent");
        b.supplyRange = fx(supply, "supply_range");
        b.supplyRelay = flag(supply, "is_relay", false);
        b.stockpileMax = fx(supply, "stockpile_max");
        b.supplyRequired = flag(econ, "supply_required", false);
        b.captureable = flag(body, "captureable", false);
        b.destructible = flag(body, "destructible", true);
        b.cityZoneRadius = fx(childOf(t, "CityComponent"), "zone_radius");
        const ResolvedNode* deploy = childOf(childOf(t, "DenizenComponent"), "DeploymentInfo");
        b.creationOffset = point(deploy, "creation_offset");
        b.deploymentOffset = point(deploy, "deployment_offset");
        const ResolvedNode* rc2 = childOf(t, "ResearchComponent");
        for (std::size_t i = 0; i < countOf(rc2, "ResearchItem"); ++i) {
            const ResolvedNode* item = childOf(rc2, "ResearchItem", i);
            ResearchItemDef d;
            d.propertyIds = asciiLowerCopy(text(item, "property_IDS"));
            const std::string scope = asciiUpperCopy(text(item, "type"));
            d.scope = scope == "LOCAL" ? ResearchItemDef::Scope::Local
                      : scope == "SUPPORT" ? ResearchItemDef::Scope::Support
                                           : ResearchItemDef::Scope::Global;
            d.time = fx(item, "research_time");
            d.cost = resources(childOf(item, "ResearchCost"));
            if (!d.propertyIds.empty()) b.research.push_back(std::move(d));
        }
        defs.buildings.push_back(std::move(b));
    }

    // The start location (Thing start_loc) gives a kingdom its building limit.
    void startLocation(const Record& r) {
        defs.economy.startLocation = resources(childOf(childOf(&r.tree, "EconomyComponent"), "Production"));
    }

    // Resource ammo / oil: PurchaseCost money (the shortfall price, GAME_DESIGN §5).
    void resource(const Record& r, const std::string& ids) {
        const ResolvedNode* pc = childOf(&r.tree, "PurchaseCost");
        if (pc == nullptr) return;
        if (ids == "ammo") defs.economy.ammoPrice = fx(pc, "money", defs.economy.ammoPrice);
        if (ids == "oil") defs.economy.oilPrice = fx(pc, "money", defs.economy.oilPrice);
    }

    // A "right, forward" point (DeploymentInfo offsets) in the sim's frame (+x forward, +y left).
    FVec2 point(const ResolvedNode* n, std::string_view key) {
        const ResolvedField* f = fieldOf(n, key);
        if (f == nullptr || !f->hasValue() || isNull(f->text())) return {};
        const auto p = readPoint(f->text(), 2, site(f, key));
        return {p[1], -p[0]};
    }

    // Data offsets are (right, forward); the sim's regiment frame is (+x forward, +y left).
    FVec2 offset(const ResolvedNode* n) {
        const ResolvedField* f = fieldOf(n, "offset");
        if (f == nullptr || !f->hasValue() || isNull(f->text())) return {};
        const auto p = readPoint(f->text(), 2, site(f, "offset"));
        return {p[1], -p[0]};
    }

    void layout(const Record& r, const std::string& ids) {
        const ResolvedNode* t = &r.tree;
        LayoutDef l;
        l.ids = ids;
        l.defaultFormationIds = asciiLowerCopy(text(t, "default_formation_ids"));
        l.combatFormationIds = asciiLowerCopy(text(t, "combat_formation_ids"));
        l.retreatFormationIds = asciiLowerCopy(text(t, "retreat_formation_ids"));
        std::vector<std::string> elementProperty;
        for (std::size_t i = 0; i < countOf(t, "Slot"); ++i) {
            const ResolvedNode* s = childOf(t, "Slot", i);
            LayoutSlotDef sd;
            sd.property = asciiLowerCopy(text(s, "property_IDS"));
            sd.number = static_cast<std::uint32_t>(std::clamp<std::int64_t>(fx(s, "number", Fixed::fromInt(1)).floorInt(), 0, 64));
            sd.primary = flag(s, "primary", false);
            sd.required = flag(s, "required", true);
            sd.locked = flag(s, "locked", false);
            for (std::uint32_t k = 0; k < sd.number; ++k) {
                l.elementSlot.push_back(static_cast<std::uint32_t>(l.slots.size()));
                elementProperty.push_back(sd.property);
            }
            l.slots.push_back(std::move(sd));
        }
        for (std::size_t i = 0; i < countOf(t, "Formation"); ++i) {
            const ResolvedNode* fnode = childOf(t, "Formation", i);
            LayoutFormationDef f;
            f.formationIds = asciiLowerCopy(text(fnode, "formation_IDS"));
            std::vector<std::pair<std::string, FVec2>> positions;
            for (std::size_t p = 0; p < countOf(fnode, "Position"); ++p) {
                const ResolvedNode* pn = childOf(fnode, "Position", p);
                positions.emplace_back(asciiLowerCopy(text(pn, "property_IDS")), offset(pn));
            }
            // Element e (property p) takes the k-th unused position tagged p; leftovers trail behind.
            std::vector<bool> used(positions.size(), false);
            for (std::size_t e = 0; e < elementProperty.size(); ++e) {
                FVec2 o{Fixed::fromInt(-2) * static_cast<std::int32_t>(e + 1), Fixed{}};
                for (std::size_t p = 0; p < positions.size(); ++p) {
                    if (!used[p] && positions[p].first == elementProperty[e]) {
                        used[p] = true;
                        o = positions[p].second;
                        break;
                    }
                }
                f.offsets.push_back(o);
                f.halfWidth = max(f.halfWidth, abs(o.y));
            }
            l.formations.push_back(std::move(f));
        }
        defs.layouts.push_back(std::move(l));
    }

    void formation(const Record& r, const std::string& ids) {
        const ResolvedNode* t = &r.tree;
        FormationDef f;
        f.ids = ids;
        f.nameKey = locKey(text(t, "name"));
        f.organized = flag(t, "organized", true);
        f.aggressive = flag(t, "aggressive", false);
        f.herding = flag(t, "herding", false);
        f.groupFormationIds = asciiLowerCopy(text(t, "group_formation_ids"));
        const Fixed one = Fixed::fromInt(1);
        for (std::size_t i = 0; i < countOf(t, "Property"); ++i) {
            const ResolvedNode* prop = childOf(t, "Property", i);
            for (std::size_t m = 0; m < countOf(prop, "Modifier"); ++m) {
                const ResolvedNode* mod = childOf(prop, "Modifier", m);
                const std::string attr = asciiUpperCopy(text(mod, "attribute"));
                const Fixed amount = fx(mod, "amount");
                if (attr == "SPEED_MAX") f.speedMultiplier = f.speedMultiplier * (one + amount);
                if (attr == "ATTACK_EFFICIENCY_DESIRED") f.attackMultiplier = f.attackMultiplier * (one + amount);
            }
        }
        defs.formations.push_back(std::move(f));
    }

    void groupFormation(const Record& r, const std::string& ids) {
        const ResolvedNode* t = &r.tree;
        GroupFormationDef g;
        g.ids = ids;
        for (std::size_t i = 0; i < countOf(t, "Position"); ++i) g.offsets.push_back(offset(childOf(t, "Position", i)));
        defs.groupFormations.push_back(std::move(g));
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

    void damageType(const Record& r, const std::string& ids) {
        DamageTypeDef d;
        d.ids = ids;
        d.damageFilter = static_cast<std::int32_t>(fx(&r.tree, "damage_filter", Fixed::fromInt(-1)).floorInt());
        defs.damageTypes.push_back(std::move(d));
    }

    // AddModifier / MultiplyModifier children of a property node (ResistDamageModifier excluded).
    std::vector<ModifierDef> modifiers(const ResolvedNode* prop) {
        std::vector<ModifierDef> out;
        for (std::size_t i = 0; i < countOf(prop, "Modifier"); ++i) {
            const ResolvedNode* m = childOf(prop, "Modifier", i);
            const std::string type = asciiUpperCopy(text(m, "type"));
            if (type != "ADD" && type != "MULTIPLY") continue;
            ModifierDef md;
            md.attribute = asciiUpperCopy(text(m, "attribute"));
            md.op = type == "ADD" ? ModifierDef::Op::Add : ModifierDef::Op::Multiply;
            md.amount = fx(m, "amount");
            if (!md.attribute.empty()) out.push_back(std::move(md));
        }
        return out;
    }

    void property(const Record& r, const std::string& ids) {
        const ResolvedNode* t = &r.tree;
        PropertyDef p;
        p.ids = ids;
        p.targetFlags = words(t, "enabled_target_flags");
        p.requiredProperties = words(t, "required_properties");
        for (std::size_t i = 0; i < countOf(t, "Modifier"); ++i) {
            const ResolvedNode* m = childOf(t, "Modifier", i);
            if (asciiUpperCopy(text(m, "type")) != "RESIST_DAMAGE") continue;
            const std::string dt = asciiLowerCopy(text(m, "damage_type"));
            if (!dt.empty()) p.resist.emplace_back(dt, fx(m, "amount"));
        }
        p.modifiers = modifiers(t);
        for (const ModifierDef& m : p.modifiers) {
            if (m.attribute == "DV" && m.op == ModifierDef::Op::Add) p.defenseAdd = p.defenseAdd + m.amount;
        }
        defs.properties.push_back(std::move(p));
    }

    void terrain(const Record& r, const std::string& ids) {
        const ResolvedNode* t = &r.tree;
        TerrainDef d;
        d.ids = ids;
        d.group = asciiLowerCopy(text(t, "terrain_group"));
        for (std::size_t i = 0; i < countOf(t, "Property"); ++i) {
            const std::string p = asciiLowerCopy(text(childOf(t, "Property", i), "property_IDS"));
            if (!p.empty()) d.properties.push_back(p);
        }
        for (std::size_t i = 0; i < countOf(t, "Variation"); ++i) {
            const std::string v = asciiLowerCopy(text(childOf(t, "Variation", i), "IDS"));
            if (!v.empty()) d.variations.push_back(v);
        }
        defs.terrains.push_back(std::move(d));
    }

    void projectile(const Record& r, const std::string& ids) {
        const ResolvedNode* pc = childOf(&r.tree, "ProjectileComponent");
        ProjectileDef p;
        p.ids = ids;
        p.speed = fx(pc, "projectile_speed");
        const std::string type = asciiUpperCopy(text(pc, "projectile_type"));
        p.type = type == "LOB" ? ProjectileType::Lob : type == "BOMB" ? ProjectileType::Bomb : ProjectileType::Antigrav;
        p.life = fx(pc, "projectile_life");
        p.spread = fx(pc, "projectile_spread");
        defs.projectiles.push_back(std::move(p));
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

}  // namespace

// Trucks, HQ regiments, convoys and recruit lists (GAME_DESIGN §7). Runs after every def is
// sorted and regiment slots are resolved; it may add regiments, so it re-sorts them and resolves
// regiment indexes last.
void linkBases(DefSet& defs, tgi::Diagnostics& diags) {
    for (UnitDef& u : defs.units) {
        u.unpackTo = u.unpackToIds.empty() ? kNoDef : defs.buildingIndex(u.unpackToIds);
        if (!u.unpackToIds.empty() && u.unpackTo == kNoDef) {
            diags.report(DiagCode::BadValue, "", 0, "truck " + u.ids + ": unpacks to unknown building '" + u.unpackToIds + "'");
        }
    }
    // A truck no regiment holds travels alone: give it a one-slot convoy regiment, laid out by the
    // one-element "convoy" layout (the truck on the anchor; without a layout a lone unit's slot
    // would trail 1.5 behind and beside it, into buildings it drives around).
    std::vector<bool> held(defs.units.size(), false);
    for (const RegimentDef& g : defs.regiments) {
        for (const ElementRef& e : g.slots) {
            if (e.kind == ElementKind::Unit) held[e.index] = true;
        }
    }
    for (std::uint32_t i = 0; i < defs.units.size(); ++i) {
        const UnitDef& u = defs.units[i];
        if (u.unpackTo == kNoDef || held[i]) continue;
        RegimentDef g;
        g.ids = "convoy_" + asciiLowerCopy(u.ids);
        g.nameKey = u.nameKey;
        g.convoy = true;
        g.slotIds = {asciiLowerCopy(u.ids)};
        g.slots = {{ElementKind::Unit, i}};
        g.recruitTime = u.unpackingLength;  // a bare truck rolls out of its depot as fast as it unpacks
        g.layoutIds = "convoy";
        defs.regiments.push_back(std::move(g));
    }
    if (defs.layoutIndex("convoy") == kNoDef) {
        LayoutDef l;
        l.ids = "convoy";
        l.slots.push_back({"truck", 1, true, true, false});
        l.elementSlot = {0};
        LayoutFormationDef f;
        f.offsets = {FVec2{}};
        l.formations.push_back(std::move(f));
        defs.layouts.push_back(std::move(l));
        sortByIds(defs.layouts);
    }
    sortByIds(defs.regiments);
    for (RegimentDef& g : defs.regiments) g.layoutIndex = g.layoutIds.empty() ? kNoDef : defs.layoutIndex(g.layoutIds);
    for (std::uint32_t bi = 0; bi < defs.buildings.size(); ++bi) {
        BuildingDef& b = defs.buildings[bi];
        for (std::uint32_t ui = 0; ui < defs.units.size() && b.truck == kNoDef; ++ui) {
            if (defs.units[ui].unpackTo == bi) b.truck = ui;
        }
        for (std::uint32_t gi = 0; gi < defs.regiments.size(); ++gi) {
            for (const ElementRef& e : defs.regiments[gi].slots) {
                if (b.truck != kNoDef && b.packedRegiment == kNoDef && e.kind == ElementKind::Unit && e.index == b.truck) {
                    b.packedRegiment = gi;
                }
                if (b.unpackedRegiment == kNoDef && e.kind == ElementKind::Building && e.index == bi) b.unpackedRegiment = gi;
            }
        }
        if (b.mobile && b.unpackingLength <= Fixed{} && b.truck != kNoDef) b.unpackingLength = defs.units[b.truck].unpackingLength;
        for (ResearchItemDef& r : b.research) r.property = defs.propertyIndex(r.propertyIds);
        b.hq = b.providesCorps;
        b.recruitRefs.clear();
        for (const std::string& a : b.recruits) {
            RecruitRef ref;
            ref.ids = a;
            ref.regiment = defs.regimentIndex(a);
            if (ref.regiment == kNoDef) {
                if (const std::uint32_t ui = defs.unitIndex(a); ui != kNoDef) ref.regiment = defs.regimentIndex("convoy_" + a);
            }
            if (ref.regiment != kNoDef) b.recruitRefs.push_back(std::move(ref));
        }
    }
    for (std::uint32_t bi = 0; bi < defs.buildings.size(); ++bi) {
        BuildingDef& b = defs.buildings[bi];
        if (b.unpackedRegiment != kNoDef && defs.regiments[b.unpackedRegiment].hq) b.hq = true;
        if (b.packedRegiment != kNoDef && defs.regiments[b.packedRegiment].hq) b.hq = true;
    }
    for (RegimentDef& g : defs.regiments) {
        for (const ElementRef& e : g.slots) {
            if (e.kind == ElementKind::Unit && defs.units[e.index].unpackTo != kNoDef && defs.buildings[defs.units[e.index].unpackTo].hq) g.hq = true;
        }
    }
}

namespace {

DefSet Compiler::run() {
    const Symbol thingSym = sym("thing");
    const Symbol abilitySym = sym("abilitydefinition");
    const Symbol nationSym = sym("nation");
    const Symbol svarsSym = sym("svars");
    const Symbol layoutSym = sym("organizationlayout");
    const Symbol formationSym = sym("formation");
    const Symbol groupFormationSym = sym("groupformation");
    const Symbol damageTypeSym = sym("damagetype");
    const Symbol targetTypeSym = sym("targettype");
    const Symbol propertySym = sym("property");
    const Symbol terrainSym = sym("terrain");
    const Symbol resourceSym = sym("resource");
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
            else if (category == "PROJECTILE") projectile(r, ids);
            else if (category == "START_LOC") startLocation(r);
            else ++stats.thingsSkipped;
        } else if (e.type == abilitySym) {
            defs.abilities.push_back(ability(&r.tree, ids, ""));
        } else if (e.type == nationSym) {
            nation(r, ids);
        } else if (e.type == layoutSym) {
            layout(r, ids);
        } else if (e.type == formationSym) {
            formation(r, ids);
        } else if (e.type == groupFormationSym) {
            groupFormation(r, ids);
        } else if (e.type == damageTypeSym) {
            damageType(r, ids);
        } else if (e.type == targetTypeSym) {
            defs.targetTypes.push_back({ids});
        } else if (e.type == propertySym) {
            property(r, ids);
        } else if (e.type == terrainSym) {
            terrain(r, ids);
        } else if (e.type == resourceSym) {
            resource(r, ids);
        }
    }
    for (const DefIndex::Singleton& s : db.defs.singletons) {
        if (s.type == svarsSym) svars(s.merged);
    }
    EconomyDef& econ = defs.economy;
    econ.startMoney = defs.svars.getFixed("ResourceStockpileDefault", econ.startMoney);
    econ.stockpileMax = defs.svars.getFixed("EconomyStockpileResourceMax", econ.stockpileMax);
    econ.limitedMax = defs.svars.getFixed("EconomyLimitedResourceMax", econ.limitedMax);
    econ.buildingPadding = defs.svars.getFixed("BuildingPaddingMeters", econ.buildingPadding);
    econ.siegeDamageSeconds = defs.svars.getFixed("UnderSiegeRecentlyDamagedSeconds", econ.siegeDamageSeconds);
    econ.underSiegeRate = defs.svars.getFixed("DenizenResupplyRateUnderSiege", econ.underSiegeRate);
    econ.supplyUnderSiege = defs.svars.getBool("SupplyProvideUnderSiege", econ.supplyUnderSiege);
    econ.moraleSupplyBonus = defs.svars.getFixed("MoraleSupplyBonus", econ.moraleSupplyBonus);
    econ.captureHealthFactor = defs.svars.getFixed("CaptureHealthFactor", econ.captureHealthFactor);
    econ.captureXpMultiplier = defs.svars.getFixed("ExperienceCaptureMultiplier", econ.captureXpMultiplier);
    econ.cityVictoryRatio = defs.svars.getFixed("CityVictoryRatio", econ.cityVictoryRatio);
    econ.teamVictory = defs.svars.getBool("TeamVictory", econ.teamVictory);

    // Inline abilities were appended in unit/building order; sort everything by IDS and remap.
    std::vector<std::string> abilityIdsByOld;
    abilityIdsByOld.reserve(defs.abilities.size());
    for (const AbilityDef& a : defs.abilities) abilityIdsByOld.push_back(asciiLowerCopy(a.ids));
    sortByIds(defs.abilities);
    sortByIds(defs.units);
    sortByIds(defs.regiments);
    sortByIds(defs.buildings);
    sortByIds(defs.nations);
    sortByIds(defs.layouts);
    sortByIds(defs.formations);
    sortByIds(defs.groupFormations);
    sortByIds(defs.damageTypes);
    sortByIds(defs.targetTypes);
    sortByIds(defs.properties);
    sortByIds(defs.terrains);
    sortByIds(defs.projectiles);
    for (AbilityDef& a : defs.abilities) a.projectileIndex = a.projectileIds.empty() ? kNoDef : defs.projectileIndex(a.projectileIds);
    for (FormationDef& f : defs.formations) f.groupFormationIndex = defs.groupFormationIndex(f.groupFormationIds);
    for (LayoutDef& l : defs.layouts) {
        for (LayoutFormationDef& f : l.formations) f.formationIndex = defs.formationIndex(f.formationIds);
        // retreat_formation_IDS, else "last organized formation if not specified" (the data's own rule).
        l.retreatFormation = l.retreatFormationIds.empty() ? kNoDef : l.formation(l.retreatFormationIds);
        for (std::uint32_t k = static_cast<std::uint32_t>(l.formations.size()); l.retreatFormation == kNoDef && k-- > 0;) {
            const std::uint32_t fi = l.formations[k].formationIndex;
            if (fi != kNoDef && defs.formations[fi].organized) l.retreatFormation = k;
        }
    }
    for (RegimentDef& g : defs.regiments) g.layoutIndex = defs.layoutIndex(g.layoutIds);
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
    const auto remapWeapons = [&](std::vector<WeaponDef>& weapons) {
        std::vector<std::uint32_t> list;
        for (const WeaponDef& w : weapons) list.push_back(w.ability);
        remap(list);
        for (std::size_t k = 0; k < weapons.size(); ++k) weapons[k].ability = list[k];
    };
    for (UnitDef& u : defs.units) {
        remap(u.abilities);
        remapWeapons(u.weapons);
    }
    for (BuildingDef& b : defs.buildings) {
        remap(b.abilities);
        remapWeapons(b.weapons);
    }

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
    linkBases(defs, diags);

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
