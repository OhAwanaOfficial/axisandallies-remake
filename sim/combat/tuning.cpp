#include "combat/tuning.hpp"

#include "core/ascii.hpp"
#include "data/tgi/value_parse.hpp"
#include "session/bytes.hpp"

namespace corps {

const std::vector<CombatTuningField>& combatTuningFields() {
    static const std::vector<CombatTuningField> fields = {
        {"defenseK", &CombatTuning::defenseK, "mitigation K in K / (K + D x defense)"},
        {"defenseD", &CombatTuning::defenseD, "mitigation D in K / (K + D x defense)"},
        {"resistMin", &CombatTuning::resistMin, "lower clamp of summed resistances"},
        {"resistMax", &CombatTuning::resistMax, "upper clamp of summed resistances"},
        {"minDamage", &CombatTuning::minDamage, "least damage a hit does"},
        {"turretAimTolerance", &CombatTuning::turretAimTolerance, "degrees off the bearing a turret may fire"},
        {"localFireArc", &CombatTuning::localFireArc, "degrees each side of the facing a local gun may fire"},
        {"defaultProjectileSpeed", &CombatTuning::defaultProjectileSpeed, "units/s for projectiles without data speed"},
        {"chaseRepathDistance", &CombatTuning::chaseRepathDistance, "Attack: re-path when the target moved this far"},
        {"moveGraceSeconds", &CombatTuning::moveGraceSeconds, "no auto-engage this long after a Move or Run"},
        {"closeInSeconds", &CombatTuning::closeInSeconds, "engaged with nothing in range this long: close in"},
        {"attackValueScale", &CombatTuning::attackValueScale, "damage x (1 + AV x this) for experience AV"},
        {"xpPerHealth", &CombatTuning::xpPerHealth, "a unit's combat value for XP: health_max x this"},
        {"fleeDistance", &CombatTuning::fleeDistance, "how far a routing regiment runs"},
        {"fleeSpread", &CombatTuning::fleeSpread, "degrees of random spread in a rout's direction"},
        {"rallyFraction", &CombatTuning::rallyFraction, "exhausted until morale is this far from breaking point to full"},
    };
    return fields;
}

bool setCombatTuning(CombatTuning& t, std::string_view name, Fixed value) {
    for (const CombatTuningField& f : combatTuningFields()) {
        if (iequals(f.name, name)) {
            t.*(f.field) = value;
            return true;
        }
    }
    return false;
}

bool parseCombatTuning(CombatTuning& t, std::string_view spec, std::string& error) {
    while (!spec.empty()) {
        const std::size_t comma = spec.find(',');
        const std::string_view item = trimAscii(spec.substr(0, comma));
        spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
        if (item.empty()) continue;
        const std::size_t eq = item.find('=');
        if (eq == std::string_view::npos) {
            error = "expected name=value, got '" + std::string(item) + "'";
            return false;
        }
        const std::string_view name = trimAscii(item.substr(0, eq));
        const tgi::FixedParse v = tgi::parseFixed(trimAscii(item.substr(eq + 1)));
        if (!v.ok) {
            error = "bad number in '" + std::string(item) + "'";
            return false;
        }
        if (!setCombatTuning(t, name, v.value)) {
            error = "unknown tuning '" + std::string(name) + "'";
            return false;
        }
    }
    return true;
}

std::string formatCombatTuning(const CombatTuning& t) {
    const CombatTuning defaults;
    std::string out;
    for (const CombatTuningField& f : combatTuningFields()) {
        if (t.*(f.field) == defaults.*(f.field)) continue;
        if (!out.empty()) out += ',';
        out += std::string(f.name) + "=" + formatFixed(t.*(f.field));
    }
    return out;
}

std::uint64_t combatTuningHash(const CombatTuning& t) {
    ByteWriter w;
    w.tag("TUNE");
    for (const CombatTuningField& f : combatTuningFields()) {
        w.str(f.name);
        w.fixed(t.*(f.field));
    }
    return hashBytes(w.buf);
}

}  // namespace corps
