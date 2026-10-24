# Combat calibration

The damage formula in the sim is a placeholder (GAME_DESIGN §6.3). The data gives every amount,
range, recharge time, resistance and defense, but not how the original combines them. This
document is the procedure for fitting our formula to the original: run the same ten duels in both
games, record the results, and tune the constants until ours land within **±10 % of the
original's time to kill**, with the same winner and about the same survivors.

## 1. The constants

Every constant the data doesn't define lives in `sim/combat/tuning.hpp` (`CombatTuning`). Nothing
else in the sim hard-codes a combat number.

```
resist = clamp(Σ ResistDamageModifier amounts of the target for the damage type, resistMin, resistMax)
raw    = amount × (1 − resist)
final  = max(minDamage, raw × defenseK / (defenseK + defenseD × (defense + ΣDV))) × attack multiplier
```

| Name | Default | What it does |
|---|---|---|
| `defenseK` | 10 | Mitigation K in K / (K + D × defense) |
| `defenseD` | 1 | Mitigation D |
| `resistMin` | −1 | Lower clamp of the summed resistances (terrain can make a unit weaker) |
| `resistMax` | 0.9 | Upper clamp |
| `minDamage` | 1 | Least damage a hit does, before the attack multiplier |
| `turretAimTolerance` | 3° | A turret fires once this close to the bearing |
| `localFireArc` | 60° | A gun without a turret fires when the body faces within this of the target |
| `defaultProjectileSpeed` | 30 | Units/s, for projectiles whose data has no speed |
| `chaseRepathDistance` | 3 | Attack re-paths when its target moved this far from the goal |
| `moveGraceSeconds` | 5 | No auto-engage this long after a Move or Run |
| `closeInSeconds` | 2 | A fighting regiment with no unit in range this long moves toward the enemy |
| `attackValueScale` | 0.01 | Damage × (1 + AV × this), for experience levels' AV (+4 … +40) |
| `xpPerHealth` | 0.01 | A unit's combat value for XP: health_max × this (infantry 2, tank 4) |
| `fleeDistance` | 40 | How far a routing regiment runs |
| `fleeSpread` | 45° | Random spread either side of straight away from its attacker |
| `rallyFraction` | 0.5 | Exhausted until morale is this far from the breaking point to full |

From the data, untouched by calibration:
* the attack efficiency (the formation's `ATTACK_EFFICIENCY_DESIRED`: Assault 1, Normal 0.75, Column 0.5), which blends at the SVars rates (0.01/s up, 0.02/s down);
* resistances (`vehicle`: gunfire 0.4, explosive 0.2; `tank` adds gunfire 0.2, explosive 0.2; `infantry`: armor_piercing 0.2; terrain and entrenchment properties);
* defense and DV, experience levels;
* turret yaw rates, projectile speeds, area, `max_targets`, attenuation;
* the SVars engagement timers and every morale and experience factor (GAME_DESIGN §6.7–6.8).

Two readings of the data are guesses that calibration should also check:

* **Area attenuation:** full damage up to `attenuation_range_min` units from the impact, then
  linear down to `attenuation_factor_max` at the edge of `area` (scenarios 8 and 9).
* **Hit chance:** none; every shot hits. If the original's time to kill varies a lot between
  runs of the same scenario, it probably has one.
* **Morale scaling** (GAME_DESIGN §6.7): which ratios the engagement, slayer and breaker factors
  use, and whether entrenchment multiplies morale loss by 1.5 or halves it. The time to rout
  (scenarios 1–4, 10) is where this shows.

## 2. Our side: `simcli duel`

```bash
simcli duel REG_A REG_B|BUILDING [TERRAIN] [--distance D] [--formation-a F] [--formation-b F] [--entrench-a] [--entrench-b] [--xp-a N] [--xp-b N] [--retreat-a S] [--retreat-b S] [--until-wiped] [--max-seconds S] [--size N] [--seed S] [--tune name=value,...] [--out F.rpl] [--verbose]
```

* The arena is a flat square (96 by default) of one terrain inside a blocked border. TERRAIN is
  `open`, `forest`, `jungle`, `city`, `sand`, `snow` (the first Terrain record whose properties
  include `terrain_<word>_infantry`), or any Terrain or variation IDS.
* A spawns in the west facing east, B in the east facing west, `--distance` apart (30 by default).
* On the next tick both get Attack on each other. A building just defends; so does an entrenched
  side (`--entrench-x`), which holds its ground.
* `--xp-x N` starts a side with N experience (its level applies). `--retreat-x S` orders it to
  retreat to its own edge S seconds in.
* It runs until one side routs or has no units left (`--until-wiped`: until no units are left).
* It prints:
  * the winner and the time to rout or to kill (when the loser broke, or its last unit died);
  * per side: survivors and their health, shots, damage dealt, and the time of its first loss;
  * per side: morale (start, end, breaking point), state, XP and level, efficiency, entrenchment, rout time;
  * the army XP of both players.
* `--tune defenseK=12,defenseD=1.5` overrides constants for that run.
* `--out` writes a replay that carries the tuning. `simcli determinism-check` verifies it.
* `--verbose` dumps the engagement state and each unit's target, range and aim at the end.

Duels are deterministic. A seed changes nothing yet, because there are no random draws in combat.

## 3. The original's side

**Set-up.** Build each scenario in the original's map editor on a flat map. A skirmish against an
opponent that is told not to move also works. For each scenario:

* Place the regiments full strength, at full health, out of each other's zone of control, about
  30 units apart and facing each other.
* Use the formation listed (Assault unless it says otherwise).
* For an entrenched regiment, wait until the sandbags show on its banner before you start.

**Start.** Order A to attack B (and B to attack A, unless B is a building), at the same moment.

**Record.** Record the game, as a film or as video, and read off:

* the time from the attack order to the first death, on each side;
* the time to kill: when the loser's last unit died;
* the survivors: how many units, and their health bars (estimate in tenths);
* anything odd: units that never fired, a side that broke and ran.

**Repeat.** Run each scenario at least 3 times and note the spread.

**Morale.** If a regiment routs in the original, note when: the scenario ends there, as in
`simcli duel` by default. Run it once more to the end for the time to kill (`--until-wiped` on
our side).

## 4. The ten scenarios

| # | A | B | Terrain | Notes | Calibrates | `simcli duel` |
|---|---|---|---|---|---|---|
| 1 | GER rifle infantry | GBR rifle infantry | open | Both Assault | `defenseK`, `defenseD` (gunfire vs infantry) | `duel regiment_ger_infantry regiment_gbr_infantry` |
| 2 | GER rifle infantry | GBR rifle infantry | forest | Both inside the forest | Terrain DV and resistances | `duel regiment_ger_infantry regiment_gbr_infantry forest` |
| 3 | GER rifle infantry | GBR rifle infantry, entrenched | open | B waits for its sandbags | Entrenchment DV and resistances | `duel regiment_ger_infantry regiment_gbr_infantry open --entrench-b` |
| 4 | GER rifle infantry in Column | GBR rifle infantry | open | A in Column (−50 % efficiency) | Attack multiplier | `duel regiment_ger_infantry regiment_gbr_infantry open --formation-a regiment_column` |
| 5 | GER medium tank | GBR heavy tank | open | Turreted AP shells | AP vs armour, turret turning, shell flight | `duel regiment_ger_tank_medium regiment_gbr_tank_heavy` |
| 6 | GBR AT infantry | GER medium tank | open | Rockets vs armour | AP rockets, check_damage_resistance | `duel regiment_gbr_infantry_at regiment_ger_tank_medium` |
| 7 | GER rifle infantry | GER medium tank | open | Small arms against armour | `resistMax`, `minDamage` | `duel regiment_ger_infantry regiment_ger_tank_medium` |
| 8 | USA halftrack | GER rifle infantry | open | Area machine gun | Area, `max_targets`, attenuation | `duel regiment_usa_halftrack regiment_ger_infantry` |
| 9 | GBR rifle infantry | GER bunker | open | Infantry attacks a bunker | Building defense, bunker MG (4 targets) | `duel regiment_gbr_infantry ger_bunker` |
| 10 | GER rifle infantry | GBR rifle infantry | jungle | Pacific biome, both inside the jungle | Jungle terrain properties | `duel regiment_ger_infantry regiment_gbr_infantry jungle` |

## 5. Results

The original's columns are measured as described in §3. "Ours" is the `simcli duel` output with
the tuning in the last column. Δ = (ours − original) / original, for the time to kill.

| # | Original: first death A / B (s) | Original: time to rout (s) | Original: time to kill (s) | Original: winner, survivors (units, health) | Runs, spread | Ours: time to rout / kill (s) | Ours: winner, survivors | Δ | Tuning |
|---|---|---|---|---|---|---|---|---|---|
| 1 | | | | | | | | | |
| 2 | | | | | | | | | |
| 3 | | | | | | | | | |
| 4 | | | | | | | | | |
| 5 | | | | | | | | | |
| 6 | | | | | | | | | |
| 7 | | | | | | | | | |
| 8 | | | | | | | | | |
| 9 | | | | | | | | | |
| 10 | | | | | | | | | |

## 6. Tuning loop

1. Fill in the original's columns for all ten scenarios.
2. Fit the formula first, on scenarios 1, 5 and 7: infantry on infantry, armour on armour, small
   arms on armour. `defenseK` and `defenseD` set how much defense matters; `resistMax` and
   `minDamage` set how hopeless small arms are against tanks.
3. Then check the situational ones (2, 3, 4, 10). If they are off while 1 is right, the data's
   modifiers aren't combined the way we think (for example, terrain DV might replace defense
   rather than add to it). Write down what changed, in GAME_DESIGN §6.3.
4. Then the area and building cases (8, 9). Adjust the attenuation reading if needed.
5. Commit the new defaults in `tuning.hpp` together with the filled-in table. Any change of a
   default changes the state hash, so re-record the golden values the tests print.

## 7. Starting point (defaults, before calibration)

For orientation only: `simcli duel` with the default tuning, M5 part 2 (morale on). The winner
is the same in every scenario whether it ends at the rout or runs to the end.

| # | Ours: time to rout | Ours: time to kill (`--until-wiped`) | Winner |
|---|---|---|---|
| 1 | 55.5 s | 98.5 s | B |
| 2 | 77.0 s | 146.1 s | B |
| 3 | 52.6 s | no kill in 600 s (A rallies out of reach) | B (entrenched, holds its ground) |
| 4 | 49.3 s | 80.1 s | B |
| 5 | 102.6 s | 130.0 s | B |
| 6 | 65.1 s | 99.0 s | B |
| 7 | 60.1 s | 95.1 s | B |
| 8 | 64.7 s | 108.6 s | A |
| 9 | 105.2 s | no kill in 600 s (the bunker can't chase) | B (bunker) |
| 10 | 77.0 s | 146.1 s | B |

M5 part 1, before morale, had times to kill of 130–228 s for the infantry scenarios.

GER against GER rifle infantry ends with both sides wiped out at 187.9 s, as it should: instant
hits land together at the end of each tick, so entity order gives no side an edge.
