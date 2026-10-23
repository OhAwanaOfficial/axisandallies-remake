#pragma once

// Every combat constant that isn't in the original data, in one place (GAME_DESIGN §6.3,
// docs/CALIBRATION.md). The data gives amounts, ranges, recharge times, resistances, defense and
// the engagement timers (SVars); this struct holds the placeholder damage formula's constants and
// the few behaviour constants the data doesn't define. Calibration changes these numbers only.
//
//   resist = clamp(sum of the target's ResistDamageModifier amounts for the damage type,
//                  resistMin, resistMax)
//   raw    = amount × (1 − resist)
//   final  = max(minDamage, raw × defenseK / (defenseK + defenseD × (defense + ΣDV)))
//            × the attacker's formation attack multiplier
//
// The tuning is part of the Sim config: its hash goes into the state header, so replays recorded
// with other numbers don't verify (NETCODE §7). `simcli duel --tune name=value,...` overrides it.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "math/fixed.hpp"

namespace corps {

using namespace fixed_literals;

struct CombatTuning {
    // --- damage formula [CALIBRATE] -------------------------------------------------------------
    Fixed defenseK = 10_fx;        // mitigation = K / (K + D × defense)
    Fixed defenseD = 1_fx;
    Fixed resistMin = -1_fx;       // clamp of the summed resistances
    Fixed resistMax = 0.9_fx;
    Fixed minDamage = 1_fx;        // per hit, before the attack multiplier

    // --- aiming and projectiles -----------------------------------------------------------------
    Fixed turretAimTolerance = 3_fx;   // degrees: a turret fires once this close to the bearing
    Fixed localFireArc = 60_fx;        // degrees each side: a local gun fires when the body is this close
    Fixed defaultProjectileSpeed = 30_fx;  // units/s, for projectiles whose data has no speed

    // --- engagement -----------------------------------------------------------------------------
    Fixed chaseRepathDistance = 3_fx;  // Attack: re-path when the target moved this far from the goal
    Fixed moveGraceSeconds = 5_fx;     // after a Move / Run order, no auto-engage for this long
    Fixed closeInSeconds = 2_fx;       // engaged with no unit in range this long: move toward the enemy
};

// Name → field table for --tune and the docs; `set` returns false for an unknown name.
struct CombatTuningField {
    const char* name;
    Fixed CombatTuning::*field;
    const char* meaning;
};
const std::vector<CombatTuningField>& combatTuningFields();
bool setCombatTuning(CombatTuning& t, std::string_view name, Fixed value);
// Parses "name=value,name=value"; false and `error` on a bad item.
bool parseCombatTuning(CombatTuning& t, std::string_view spec, std::string& error);
// The fields that differ from the defaults, as parseCombatTuning reads them ("" for the defaults).
std::string formatCombatTuning(const CombatTuning& t);

// xxh3 of the fields in table order (state header, replays).
std::uint64_t combatTuningHash(const CombatTuning& t);

}  // namespace corps
