# Game Design Document

**Goal:** a faithful, modernized remake of the 2004 TimeGate/Atari regiment-based RTS. It should be **mechanically identical by default**, with modern quality-of-life on top and nothing that changes the balance unless a setting turns it on.

Sources for this document: the original data files in the install (`_rwd_extract/`, `data/`), `AxA_PC_Manual.pdf` and `Readme.txt`. **Numbers quoted here are examples. The TGI files are always the source of truth**, because the sim reads them directly.

> Legend: **[DATA]** read from TGI · **[MANUAL]** from the PDF manual · **[CALIBRATE]** formula lives in `AA.exe` and must be measured against the original · **[NEW]** our addition (off by default if it affects balance)

---

## 1. Pillars

1. **Command regiments, not soldiers.** Players command 2–8 unit regiments. Formation, morale and experience matter more than clicking speed.
2. **Mobile bases and supply lines.** Bases pack onto trucks and move, and armies live or die by zones of supply.
3. **Economy as upkeep.** Money is stockpiled, but oil and ammo are income-only upkeep. Overspending drains money.
4. **Generals and special ops.** Army experience is spent on each general's unique special operations.
5. **Two layers:** a real-time battlefield, plus (later) the turn-based World War II strategic map.

---

## 2. Game modes (priority order)

| # | Mode | Notes | Milestone |
|---|---|---|---|
| 1 | **Skirmish vs AI** (custom map or random map) | Core loop. Uses 1v1–4v4, FFA, up to 8 kingdoms **[DATA `KingdomsMax = 8`]** | Vertical slice → M7 |
| 2 | **Multiplayer LAN / Internet** | Lockstep, same options as skirmish | M8 |
| 3 | **Replays ("Films")** | Free camera, per-player view, speed control | M3 (tech), M8 (UI) |
| 4 | **World War II** (turn-based world map + RTS battles) | Single-player in the original | M11 |
| 5 | **Campaigns** (Allies/Axis missions, tutorial) | Scripted triggers from `game_events`/map files | M12 |
| 6 | **Map editor** | Optional | Later |

Skirmish setup options **[MANUAL][DATA]**: map or random-map theatre (RMC: France, India, Philippines, ...), map size, terrain sliders, fog (NONE / EXPLORED / FULL "black fog"), starting money (default **500**), starting army experience (default **200**), city control ratio, difficulty, teams, nation and general per player, time of day/sun (`sun_default/morning/dusk/night`).

---

## 3. Factions

| Nation | Code | Generals (4 each) **[MANUAL][DATA]** |
|---|---|---|
| United States | USA | Patton, Eisenhower, Arnold, + 1 |
| Great Britain | GBR | Montgomery, Mountbatten, Wavell, Wingate |
| Germany | GER | (from data) |
| Soviet Union | RUS | (from data) |
| Japan | JPN | (from data) |
| *Persia (mod)* | PER | cloned from GBR by your `tools/clone-nation.ps1` |
| *Saudi Arabia (mod)* | SAU | cloned from GBR |

* All nations share a **common unit pool**, with per-nation stat differences (e.g. German tanks are stronger) and **1–2 nation-exclusive units** (e.g. USA: Assault Infantry, Flamethrower Infantry, Marines; GBR: SAS) **[MANUAL]**.
* The full roster of generals and special ops is loaded from `Generals/<NAT>/*.tgi` and `SpecialOps/*.tgi` in `Data.rwd` (not yet extracted, so extract in M1).
* Mods like PER/SAU must load with no code changes. That's a test case.

---

## 4. Core loop (one skirmish)

```
Start: Corps HQ (deployed) + starting money + army XP
  → Corps HQ deploys Division HQs, depots and brigades from trucks (unpack)
  → Division HQs recruit regiments (cost money, add upkeep)
  → Regiments fight, capture cities, earn XP (unit) and army XP (player)
  → Army XP buys special ops; money buys research
  → Push supply forward (Supply Depots / captured cities / repacked bases)
  → Win: eliminate enemies or meet the city control ratio
```

---

## 5. Economy **[DATA `resources.tgi`, `resource_list.tgi`, `SVars_AA.tgi`; MANUAL p.32–34]**

| Resource | Stockpiled? | Source | Notes |
|---|---|---|---|
| **Money** | Yes (reserve can't go below 0) | Every base building's `Production`, captured cities | Shown as `466 (+90)`: reserve (+ per-minute income) |
| **Oil** | **No**, per-minute income only | Corps HQ, Division HQs, **Oil Depot** | Upkeep from regiments |
| **Ammo** | **No**, per-minute income only | Corps HQ, Division HQs, **Ammo Depot** | Upkeep from regiments and bunkers |
| **Unit limit** | Capacity | `unit_limit_provided` from buildings (Corps ≈ +3) | Max **20** **[DATA `EconomyLimitedResourceMax = 20`]**. Each regiment consumes 1. |
| **Building limit** | Capacity | `building_limit_provided` | Supply Depots don't count. Oil/Ammo Depots count as 1 each. |
| **Army XP** | Yes | Earned in combat | Spent on special ops |

**Shortfall rule:** if oil or ammo income is negative, the deficit is bought with money every minute at **1 ammo = 2 money, 1 oil = 3 money** **[DATA `PurchaseCost`]**.
Example **[MANUAL]**: oil +10, add a heavy tank regiment (−18 oil) → oil −8/min → **−24 money/min**.

**Bankrupt state** (reserve = 0 and net money income < 0) **[MANUAL]**: in-supply units resupply much more slowly, out-of-supply units lose health until they die, and you can't construct or repair.

Example values (GBR template, same for PER) **[DATA]**:

| Thing | Cost (money) | Upkeep | Production |
|---|---|---|---|
| Corps truck / Corps HQ | 400 | — | +12 money, +10 ammo, +5 oil |
| Armour Division HQ | 315 | — | +20 money, +10 ammo, +15 oil, +3 unit limit |
| Infantry regiment | 50 | 10 ammo | — |
| Medium tank regiment | 200 | 20 ammo, 15 oil | — |

Income is applied **per sim tick, pro-rated** (income/min ÷ 600 ticks) using fixed-point. The HUD shows per-minute values.

**Implemented (M6 part 1, `sim/systems/economy.*`):**
* **Start:** `StartPlayer` gives a kingdom its Corps HQ (deployed) and **500 money** **[DATA `ResourceStockpileDefault`]**, or the map's own `ResourceStockpileDefault` rule (RULES chunk; Golden Fields: 1000). The start location (Thing `start_loc`) provides **building limit 20** **[DATA]**.
* **Who produces and pays:** every deployed building (not while packing or unpacking; one with `supply_required` only while in supply; cities a kingdom holds included), every truck (packed HQs keep producing: the Corps truck +12 money, +5 oil, +10 ammo; an HQ truck also +5 unit limit) **[DATA]**, every regiment (its organization's upkeep: infantry 10 ammo, unit limit 1) and every unit with an upkeep.
* **Shortfall prices** come from the Resource records' `PurchaseCost` (ammo 2, oil 3). The money stockpile is clamped to 0 .. `EconomyStockpileResourceMax` (99999); the unit limit provided is capped at `EconomyLimitedResourceMax` (20).
* **Limits:** a regiment's `unit_limit_consumed` (infantry 1) and a truck's or building's `building_limit_consumed` (1; a supply depot 0) count while they exist; queued and half-recruited regiments reserve what they will consume.
* **Bankrupt** (no money, negative net income) is a kingdom flag with events (`Bankrupt` / `Solvent`). Since M6 part 2: in supply its regiments heal and get replacements at ×0.25 (`DenizenResupplyRateUnderSiege`, stacking with a siege) **[CALIBRATE]**; out of supply every unit loses `shortfall_damage_rate` (0.01) × max health per second until it dies. "No construction or repair" waits for construction.

---

## 6. Units, regiments, combat

### 6.1 Regiments **[MANUAL p.34; DATA `Regiments/*.tgi`]**
* A regiment is an **Organization** with ordered **slots** (`front`, `rear guard`, `flank`, `support`, `captain`) and a **layout** (`regiment_infantry`, `regiment_vehicle_light`, ...) that defines slot positions per formation.
  * Infantry regiment = 4 Rifle + 1 MG + 1 Officer. Medium tank regiment = Medium Tank(s) + Command Tank.
* **Deploying:** the first unit appears after a few seconds, then the rest fill in one by one **while in supply** **[MANUAL]**.
* **The captain/CO** slot carries the officer bonuses (morale). If the CO dies, it is replaced by resupply like any other unit (verify in the original).
* **Amphibious variants** (`*_amphib`) are used when a `transport_actor` exists (naval landings).

### 6.2 Unit stats (examples) **[DATA]**

| Unit | Health | Defense | Speed | Main attack |
|---|---|---|---|---|
| Rifle infantry | 200 | 1 | 2.5 | Rifle: **Pierce 10**, recharge 3 s, range 12. Grenade: **Blast 20** |
| Medium tank | 400 | 6 | 3.5 (accel 3) | Turret gun: **AP 65**, area 1, recharge 6 s, range 20 |
| Armour Div HQ (building) | 2000 | 5 | — | MG: Pierce 20, area 2, 4 targets, recharge 2 s, range 20 |

### 6.3 Damage model **[DATA structure][CALIBRATE formula]**
Known from data (compiled into the DefSet in M5 part 1):
* **Damage types** (`Game/damage_types.tgi`): `gunfire` (small arms; the effect template is `PierceDamageEffect`), `armor_piercing` (`APDamageEffect`), `explosive` (`BlastDamageEffect`) and `flame`.
  * An ability's `MAIN_DAMAGE` and `ADD_DAMAGE` effects each deal one type.
  * `area` is the blast radius. `max_targets` caps how many units it hits (the GER bunker: 4). `attenuation_range_min` and `attenuation_factor_max` shape the falloff (bunker MG: 0.5 / 0.5).
* **Resistances live on properties.** A `ResistDamageModifier` ignores a fraction of one damage type, and a unit's properties add up:
  * `vehicle`: gunfire 0.4, explosive 0.2. `tank` adds gunfire 0.2 and explosive 0.2, so a tank ignores 60 % of small arms.
  * `infantry`: armor_piercing 0.2. `bunker`: gunfire 0.2, explosive 0.2. `building`: armor_piercing 0.2.
  * An `AddModifier` on attribute `DV` adds to defense.
  * **Terrain:** Terrain records list properties, which apply only when the unit has their `required_properties`. `terrain_forest_infantry` and `terrain_jungle_infantry` give DV +4 and armor_piercing 0.2; `terrain_city_infantry` the same; vehicles in forest, jungle and city resist gunfire −0.2.
  * **Entrenchment:** `infantry_entrench` gives DV +4, armor_piercing 0.4, explosive 0.4; `vehicle_entrench` DV +4, 0.2, 0.2.
* **Target types** (`air, tank, infantry, artillery, building, vehicle, engineer, commanding_officer, depot, routing, sabotaged, mine, water, org_*`) come from each property's `enabled_target_flags`. Ability filters such as `+enemy, -air, -mine, -water` or the AA's `+enemy, +air` use them.
* **Weapons:** a `PerformerComponent` (or a building's `DefenderComponent`) lists Ability slots, each with:
  * an `AbilityDefinition`;
  * `priority` (turret guns 20, local guns 1);
  * `fuzzy_range` (extra acquisition range, usually 3);
  * an optional `turret_ids`.
  `check_damage_resistance = true` (tanks, AT infantry) makes the actor weigh resistances when choosing targets.
* **Turrets:** `yaw_rotation_per_sec_degrees` (GER medium tank 37, bunker 90), yaw range, `reset_time` (10 s).
* **Projectiles:** `projectile_speed` (medium tank shell 50, rocket 20).

**Implemented formula** (placeholder until calibrated; every constant is in `sim/combat/tuning.hpp`):
```
resist = clamp(Σ resistances of the target for the damage type, resistMin −1, resistMax 0.9)
raw    = amount × (1 − resist)
final  = max(minDamage 1, raw × K / (K + D × (defense + ΣDV))) × attack multiplier      # K 10, D 1
```
* **Where the data feeds in:** the target's own properties, the terrain under it and entrenchment give `resist` and `ΣDV`. The attack multiplier is the attacker's formation (`ATTACK_EFFICIENCY_DESIRED`: Assault 1, Normal 0.75, Column 0.5).
* **Area shots** hit the nearest enemies within `area` of the impact, at most `max_targets`. Damage is full up to `attenuation_range_min` and falls linearly to `attenuation_factor_max` at the edge **[CALIBRATE: the data's exact meaning]**.
* **No friendly fire, no hit chance.**
* **Not yet:** XP multipliers (M5 part 2).

**Calibration harness:** [CALIBRATION.md](CALIBRATION.md) lists 10 duel scenarios to build in the original (rifle vs rifle on open ground, in forest, entrenched and in Column; tank vs tank; AT vs tank; rifle vs tank; halftrack MG vs infantry; infantry vs bunker; jungle), with a results table. `simcli duel` runs the same fights here. Tune the constants until our results fall within ±10 % of the original's time to kill.

### 6.4 Formations & combat efficiency **[MANUAL p.36–38]**

| Formation (hotkey) | Combat efficiency | Speed | Behaviour |
|---|---|---|---|
| **Assault** [Q] (default) | +0% | −25% | Aggressive |
| **Normal/Aggressive** [W] | −25% | +0% | Aggressive |
| **Column** [E] | −50% | +25% | Passive (ignores enemies) |

In the data these are `regiment_aggressive` (Assault), `regiment_defensive` (Normal/Aggressive) and `regiment_column` (Column) in `Organizations/Formations/regiment_formations.tgi`. The speed and efficiency numbers above come from their modifiers.

* **Efficiency blends gradually** to the new formation's level after a change. The data gives the rate: SVars `AttackEfficiencyPointsPerSecondClimbing = 0.01` and `...Falling = 0.02`. Assault → Column takes 25 s to settle at 0.5; going back takes 50 s. **Implemented (M5 part 2):** the regiment's actual efficiency is its damage multiplier; a routing regiment aims at 0.
* **Run** [R] / Alt+right-click: temporary Column, then back to the previous formation on arrival.
* Aggressive regiments **auto-engage** anything in their zone of control (ZOC). A tank regiment in Assault formation crossing an enemy base will stop and destroy every building on the way. That's intended.
  * **Implemented (M5 part 1):**
    * The ZOC is the RegimentDef's `control_range` around the anchor.
    * Assault and Normal engage unless running; Column and Run never do.
    * A regiment engages only enemies its weapons can target, so an unarmed regiment, or flak among tanks, passes by.
    * The move goes on hold and resumes after the fight.
    * After a Move or Run order, a regiment doesn't auto-engage for 5 s (`moveGraceSeconds`), so it can be ordered out of a fight.

### 6.5 Orders **[MANUAL]**
Move [M], Attack [A] (chases), Run [R], Stop [S], Guard [G] (follow/hold and engage nearby), plus Unpack/Pack [P], Deploy [D], Build (engineers), Repair, Group Behaviour [V].
* **Implemented (M5 part 1):**
  * **Attack** (command 7) takes 1–6 regiments and a regiment, unit or building of another player. Each regiment chases the target until its anchor is within `CAITargetInRangeZOCMultiplier` (0.6) × ZOC, then engages. While attacking, its units shoot the target's units first.
  * **AttackMove** (command 8) is a MoveGroup that engages anything on the way, whatever the formation, then carries on.
  * Move, Run and Stop end a fight.
* **Implemented (M5 part 2): Retreat** (command 9) moves 1–6 regiments to a point, each on its own:
  * in the layout's retreat formation (`retreat_formation_IDS`, or the data's rule "last organized formation if not specified", which is Column);
  * passive: it neither auto-engages nor fires;
  * it costs `MoraleRetreatPenalty` once (0 in A&A).
  * On arrival, and after at least `RetreatingTime`, it reforms and gets its formation back.
  * A retreating regiment can still rout if its morale breaks.

### 6.6 Engagement state machine **[DATA `SVars_AA.tgi`; actor_states]**
States: `Idle → Engaging → (Retreating | Routing | Stalemate) → Reform/Regroup → Idle`
* `PrimarySampleTime = 5 s` (re-evaluate engagement), `RetreatingTime = 10`, `RoutingTime = 10`, `ReformTime = 10`, `StalemateTime = 10`, `RegroupTime = 10`.
* `KillTargetIsHighestPriority = true`: keep shooting a target that's about to die.
* `CAITargetInRangeZOCMultiplier = 0.6`: when ordered to attack, move to 60% of ZOC radius before stopping.
* `mind_retreating`: ordered retreat → RETREAT formation, PASSIVE, can still resupply.
* `mind_exhausted`: BROKEN formation, removed from group, morale replenishes.
* **Implemented (M5 part 1, `sim/systems/combat.*`):**
  * **Sampling:** every PrimarySampleTime (staggered by entity) a regiment samples its ZOC; being hit also triggers a sample.
  * **Idle → Engaging:** the move goes on hold.
  * **Engaging → Stalemate:** no hit dealt or taken for StalemateTime. Stalemate → Reform after another StalemateTime, or as soon as the ZOC is clear.
  * **Engaging → Reform** when a sample finds the ZOC clear. **Reform → Regroup** (in a group) **→ Idle**: the held move resumes. Each takes its SVars time. Units fire only while Engaging or Stalemate, or under an Attack order.
  * **Closing in:** a fighting regiment with no unit in firing range for 2 s (`closeInSeconds`) moves toward the nearest enemy and stops once one is in range. Division members hold their slots instead.
  * **Targets:** `KillTargetIsHighestPriority` keeps a valid target. Otherwise a unit takes the Attack target's units first, then (with check_damage_resistance) the most damage per hit, then the nearest.
  * **Retreating** (ordered, above) and **Routing** / **Exhausted** (morale, §6.7) are run by the Morale stage (M5 part 2, `sim/systems/morale.*`). Routing and retreating regiments are passive. Routing ones are deaf to orders and get the `routing` target type.

### 6.7 Morale **[DATA SVars][MANUAL p.40]**
* Blue bar with a **breaking point**. Below it, the regiment **routs**: Column formation, flees, uncontrollable until it stops (may run the wrong way).
* Loss: being engaged (`MoraleEngagementLoss = 0.5`/sample, scaled up to ×3), unit deaths (`MoraleDeathFactor = 0.25`), "breaker" attacks (flamethrowers, `MoraleBreakerBase = 3.0` ×0.5–2), fear enchantments.
* Gain: time out of combat (`MoraleReplenish = 2.0`), supply (`+MoraleSupplyBonus = 1.0`), killing enemies ("slayer", `MoraleSlayerBase = 0.5` ×0.5–3), courage enchantments, officers.
* **Implemented (M5 part 2, `sim/systems/morale.*`).** The data fixes the sizes; how the factors scale is our reading **[CALIBRATE]**.
  * **Ceiling:** morale is "defined by elements". The ceiling is the sum of the living units' ElementComponent `morale_value` (infantry 5, so a 9-man regiment has 45; tanks 20), plus the level's MORALE per unit. It starts full.
  * **Breaking point:** the organization's `morale_breakpoint` (infantry 20, armour 15).
  * **Per sample** (`PrimarySampleTime`, staggered by entity):
    * Engaged: − `MoraleEngagementLoss` × clamp(enemy health in its zone / its own health, 1, `MoraleEngagementLossFactorMax`).
    * Idle, reforming, regrouping or exhausted: + `MoraleReplenish` × the level's `MORALE_RECOVERY_RATE`.
    * Retreating and routing don't replenish (actor_states `morale_replenish = false`).
    * The supply bonus arrives with supply (M6).
  * **A unit dies:** its regiment loses `morale_value` × `MoraleDeathFactor`. The killer's regiment gains `MoraleSlayerBase` × clamp(victim `morale_value` / killer `morale_value`, `MoraleSlayerFactorMin`, `Max`), so infantry killing a tank gains the most.
  * **Breakers:** in the data these are abilities with a `MoraleLossEffect` (mortars 0.75, artillery, bombers 0.5, battleships 1.5), not flamethrowers. A hit costs `MoraleBreakerBase` × clamp(effect amount, `MoraleBreakerFactorMin`, `Max`).
  * **Loss rate:** losses are multiplied by `MORALE_LOSS_RATE` modifiers. Entrenchment's says `MultiplyModifier 0.5`, which by the data's own convention (× (1 + amount)) means × 1.5 **[CALIBRATE: it may mean "half"]**.
  * **Rout:** a fighting or retreating regiment at or below its breaking point routs. It:
    * leaves its group and drops its orders;
    * takes its retreat formation;
    * runs `fleeDistance` (40) away from its last attacker, ± `fleeSpread` (45°) at random (the sim RNG: it may run the wrong way);
    * is deaf to commands for `RoutingTime`;
    * gives its last attacker rout XP.
  * **Exhausted:** after the rout the regiment is exhausted (mind_exhausted): passive, still controllable, replenishing. Once morale is `rallyFraction` (half) of the way from the breaking point to full, it reforms and returns to Idle.
  * Courage and fear enchantments, officers' bonuses and supply wait for their systems (M6, M9).

### 6.8 Experience **[MANUAL p.40]**

| Level | XP needed | Insignia |
|---|---|---|
| Recruit | 0 | — |
| Regular | 100 | 1 chevron |
| Veteran | 250 | 2 chevrons |
| Elite | 500 | First sergeant |

Replacement units from resupply keep the regiment's XP. A whole new regiment starts as Recruit. Bonuses per level: **[CALIBRATE]** (check unit tooltips in the original).

* **Implemented (M5 part 2).** The data has the levels: each organization template has an `ExperienceComponent` with four `ExperienceLevel`s, and the data wins over the manual where they differ.
  * **Infantry** needs **150 / 350 / 600** XP. Its levels give AV +4/+8/+16, DV +1/+2/+3, HEALTH × 1.05/1.1/1.2, MORALE +0/+0.5/+1 per unit, and MORALE_RECOVERY_RATE × 1.05/1.1/1.15.
  * **Paratroopers** need 100 / 250 / 500, the manual's numbers.
  * **Armour** has the same thresholds as infantry, with AV +10/+20/+40 and MORALE +1/+2/+4.
  * **How each bonus applies:** DV adds to defense. AV multiplies damage by (1 + AV × `attackValueScale` 0.01) **[CALIBRATE]**. HEALTH scales the units' health when the level is reached. MORALE raises the ceiling.
  * **XP (SVars "experience"):**
    * A kill earns clamp(victim combat value, `ExperienceGainMinimum` 0, `Maximum` 10) × `ExperienceKillMultiplier`. Combat value = health_max × `xpPerHealth` 0.01 (infantry 2, tank 4) **[CALIBRATE: the original uses a combat value (CV) built from the `CombatValue*` SVars]**, or the regiment's `fake_cv_value`.
    * Breaking an enemy earns the routed regiment's combat value × `ExperienceRoutMultiplier`.
    * Every engaged sample earns `ExperienceDefaultValue` × `ExperienceEngageMultiplier`.
    * Capture arrives in M6.
  * **Army XP** (the player's, for generals and special ops, §9) is every XP its regiments earn × `ExperienceFactionGainMultiplier`, plus `ExperienceFactionGainPeriodic` (1) per second while the player has something on the map. The `StartingExperience` world rule (default 200) belongs to game setup (M6).

### 6.9 Entrenchment
A regiment that stays still long enough becomes entrenched (sandbags on the banner) and gets the `infantry_entrench`/`vehicle_entrench` property bonuses **[DATA actor_states]**. Time to entrench: the organization's `entrenchment_time`, **30 s** for infantry, vehicle and armour regiments.
* **Implemented (M5 part 2):**
  * Ticks count while the regiment's move order is idle or on hold, and it isn't routing or retreating. At `entrenchment_time` it is entrenched.
  * Entrenched infantry gets DV +4, armor_piercing and explosive resist 0.4 each, and morale loss × 1.5 (§6.7); vehicles get DV +4 and 0.2 resist.
  * Any move resets it. An entrenched regiment without an Attack order doesn't close in: it holds its position.

### 6.10 Terrain
Terrain groups: dirt, snow, water, desert (+ forest/jungle/mountain blobs from RMC). Each terrain gives per-movement-class **speed** and per-unit-type **defense** modifiers (e.g. jungle: big infantry defense bonus, big vehicle penalty, slow) **[MANUAL]**. Mountains and ocean block ground units.
* **Defense (implemented in M5):** each `Terrain` record lists properties (`terrain_forest_infantry`, `terrain_city_vehicles`, ...) that apply to the units standing on it when they have the property's `required_properties`.
  * Infantry in forest, jungle or city: DV +4 and armor_piercing resist 0.2.
  * Vehicles there: gunfire resist −0.2, so they take more from small arms.
  * Map cells name a terrain variation (`temperate_forest02`), which resolves to its Terrain; generated maps' forest cells use the forest properties. A golden test checks Golden Fields.
  * The properties' `TerrainSpeedModifier`s aren't wired into movement yet (movement still uses the PATHFINDING §2 cost table).

### 6.11 Groups & divisions **[MANUAL p.38; DATA `group_formations.tgi`]**
* Up to **6 regiments** per group. Ctrl+number saves it to the unit bar. (Movement commands carry at most 6 regiments; how the sim forms and dissolves groups is in PATHFINDING §10.)
* **Group** = independent regiments sharing an order.
* **Division** (Group Behaviour [V]) = acts as one big regiment. Regiments take positions from `group_aggressive / group_defensive / group_column` offsets (stronger in front, weaker behind), move at the slowest member's speed, and arrive together.

---

## 7. Bases & buildings **[MANUAL p.30–31, 46–47; DATA Buildings/*]**

| Building | Role | Notes |
|---|---|---|
| **Corps HQ** | Hub. Deploys all other buildings (except bunkers), basic research | Only one allowed (`provides_corps` target type) |
| **Infantry / Mechanized / Armour / Airborne Division HQ** | Recruit regiments | Have built-in MG defenders and provide unit limit |
| **Airfield** | Stocks fighters and bombers. Air Recon / Air Strike missions (paid per use) | Fighters auto-defend |
| **Engineer Brigade / Motor Pool / Artillery Brigade** | Unlock advanced regiments and research | `required_structures` in unit data |
| **Supply / Oil / Ammo Depot** | Supply zone / oil income / ammo income | Supply depot is free of the building limit |
| **Bunkers** (MG, AA, Artillery) | Built by **Engineers**, static defense | Have ammo upkeep |
| **Cities** (neutral settlements) | Capture by occupying the centre with no enemies present. Big money and supply zone | `SettlementComponent`, village layouts per region |

**Pack/unpack** **[DATA `MobileDeploymentComponent`]**: a building packs onto trucks (+ trailer) in its `packing_length`, can't pack below `pack_health_min = 50%` health, moves like a regiment, then unpacks at a valid placement (red ghost if invalid). The times differ per building **[DATA]**: Corps HQ 10 s, division HQs 8 s, depots and brigades 7 s (their trucks' own `unpacking_length` is 7 s).

**Implemented (M6 part 1, `sim/systems/bases.*`):**
* **HQ regiments:** a division HQ is a regiment in two forms **[DATA]**. Packed, `regiment_gbr_hq_infantry` holds the truck `gbr_truck_infantry`, whose `unpack_to_actor_ids` names the building `gbr_hq_infantry`. Unpacked, `regiment_gbr_hq_infantry_unpacked` holds the building. The Corps HQ recruits the packed form; the truck costs what the building does (225).
* **Convoys:** a truck no regiment holds (the Corps truck, depot and brigade trucks, which the Corps HQ recruits bare) travels as a synthetic one-truck regiment `convoy_<truck>`, with the truck on the anchor.
* **Placement** (`placeBuilding`): the footprint is `Size` (SQUARE `square_size`: HQs 10, depots 8; RECT `rect_width` × `rect_height`; CIRCLE 2 × `blocking_radius`), turned by the facing (`axis_locked`: snapped to 90°) and snapped to whole cells. It and its `BaseTerrain` `padding` (1) must be inside the map and passable for foot and wheeled units; no building may be within `BuildingPaddingMeters` (2) of it; a kingdom has one Corps HQ (property `corpshq`). **`placement_groups` are only the editor's palettes** (`group_nation_gbr`, …: an IDS and a name); they add no rule.
* **Unpack:** the truck regiment drives to the footprint's centre; on arrival the placement is checked again, then the trucks give way to the building, which stamps its footprint at once and unpacks for its `unpacking_length`. It keeps the trucks' health fraction and the regiment's XP. Re-ordering the trucks cancels; a spot taken meanwhile gives `DeployFailed`.
* **Pack:** health at least `pack_health_min`, nothing queued or researching; after `packing_length` the building gives way to its packed HQ regiment (or its truck's convoy), health fraction and XP kept.
* **Recruit** (`RecruitComponent`): up to 5 regiments queued per building, paid when queued (the regiment's cost plus its units', e.g. a truck's), refundable until it starts. The front regiment appears empty at the building's `DenizenComponent/DeploymentInfo` `creation_offset` and heads for its `deployment_offset`; a unit joins every `recruit_time` ÷ units (infantry regiments: 15 s), the first after one interval **[CALIBRATE]**. Every unit's `required_structures` must all be owned and deployed (a unit listing several needs all of them), and its `required_properties` must be on the recruiting building (its own, its LOCAL research, or the kingdom's research that applies to it). Recruiting (the next unit) pauses while the building is outside every active supply zone of its team (M6 part 2).

**Biomes:** buildings and trailers have `Arctic/` and `Desert/` texture variants. Choose by map biome.

---

## 8. Supply & healing **[MANUAL p.39; DATA SVars]**
* **Zone of supply**: a circle around every major base building, Supply Depot and owned city. Allies share supply zones.
* Attached regiments **in supply** heal unit by unit and **replace dead units for free**, keeping XP. That's why retreating a beaten regiment beats rebuilding it.
* **Under siege**: if an enemy is near (or the building was damaged within `UnderSiegeRecentlyDamagedSeconds = 7`), that building's supply zone **deactivates**. Exception flag: `SupplyProvideUnderSiege`.
* Out of supply while bankrupt: health drains.
* HUD: supply-border toggle [Ctrl+S].

**Implemented (M6 part 2, `sim/systems/supply.*`, Supply stage):**
* **Zones:** `SupplyComponent` `supply_range` around every deployed Structure that has one (the GBR Corps HQ 60) and every city a kingdom holds (55). Allies (same team, `SetMatchRules`) share them. A relay (`is_relay`) supplies only while it stands in another active zone of its team, chains included. Unpacking or packing buildings, neutral cities and eliminated kingdoms supply nothing. A regiment is in supply when its anchor is in an active zone of its team and its `OrganizationComponent` `supply_used` is set.
* **Under siege:** an enemy unit within the supplier's siege radius (its `control_range`, else a city's `zone_radius` 42, else its `supply_range`) **[CALIBRATE]**, or a hit taken within `UnderSiegeRecentlyDamagedSeconds` (7 s). The data sets **`SupplyProvideUnderSiege = true`**, so a besieged zone keeps supplying at `DenizenResupplyRateUnderSiege` (×0.25). With the flag false it switches off. A regiment in several zones uses an unbesieged one.
* **Healing:** each unit heals `ElementComponent` `resupply_rate` × max health per minute (infantry 0.3, tanks 0.1). Buildings don't heal (repair waits for construction).
* **Replacement:** a regiment missing a unit gets one free every `recruit_time` ÷ units (infantry: 15 s ÷ 9), lowest missing layout element first, at its formation slot **[CALIBRATE: the original may walk it in from the supplier]**. It joins at the regiment's level (its health multiplier) and keeps the regiment's XP (XP lives on the regiment). It raises the morale ceiling but doesn't refill morale. Regiments still being recruited aren't replaced. Event `Replaced`.
* **Morale:** `MoraleSupplyBonus` (1) is added to every morale replenishment in supply.
* **Production and recruiting:** buildings with `EconomyComponent` `supply_required` produce only in supply; recruiting pauses outside supply.

---

## 9. Generals, special ops, research
* Each player picks **1 of 4 generals** for their nation. Each general has **4 special operations** (hotkeys Y/H/N/J), with a cooldown bar plus an **army XP cost** per use.
* Special ops are defined in data (`SpecialOps/*.tgi`, `ability_special_op` templates) and implemented as abilities/enchantments (e.g. `ench_type_courage/fear/radiation`).
* **Research** **[DATA `ResearchComponent`]**: per-building research items, each with a property, a type (`LOCAL` = this actor, `SUPPORT` = settlement, `GLOBAL` = kingdom), required structures/properties, time and cost. Research grants **properties**, and properties modify stats. This is the general modifier system: one `PropertyDef` applies stat deltas and multipliers and unlocks.
  * **Implemented (M6 part 1):** one item at a time per building, paid at the start (refunded on cancel), granted after `research_time` (the Corps HQ's six GLOBAL items: 60 s, 50–150 money). A technology property applies to the kingdom's actors that have its own `required_properties`: `tech_improved_machineguns` (AV +4) to actors with `mg`, `tech_heavy_tank` to the armour HQ (`armorhq`), which is what lets that HQ recruit heavy tanks (their `required_properties`). Research AV and DV add to combat now; other attributes (supply range, resupply rate, detection) wait for their systems. The items' `required_structures` / `required_properties` are "false" in the data. SUPPORT items wait for cities.

---

## 10. Fog of war & vision
* Fog modes: `NONE`, `EXPLORED` (default: terrain and cities visible, units hidden outside vision), `FULL` (black fog, removed permanently once explored).
* Per-team vision grid. Allies share vision (map and minimap).
* `DetectionComponent` gives sight radius. Air Recon special op/mission reveals an area temporarily.

---

## 11. Allies & team commands
Shared supply and vision, flares (attack here, defend here, etc.), Report (share stats bar), tribute (send/request money, units, buildings), chat **[MANUAL p.41–42]**.

---

## 12. Victory conditions
Elimination (destroy all HQs/units per `game_types`), **city control ratio** (percentage of city value held), and scenario objectives (campaign). The exact elimination rule is read from `game_types_aa.tgi` and `game_events.tgi`.

**Implemented (M6 part 2, `sim/systems/supply.*`, Capture stage).** `game_types_aa.tgi`'s `NORMAL` has no rule fields, so these rules are ours:
* **Cities:** every map thing whose def has a `CityComponent` (after translate.tgi's renames: Golden Fields' `chaparral_village_town_center` → `mediterranean_city_center01_large`) is a Structure from the start: neutral (unless its map kingdom is a playable one), full health, footprint stamped, no weapons, so nothing targets it (cities are indestructible here). Village blocks (`med_villageblock12x12_*`: not captureable, no production) are scenery and aren't spawned. A city produces its `Production` (15 money, 10 oil, 10 ammo) for its holder and supplies like a depot.
* **Capture:** a kingdom's unit within the capture radius (half the footprint + 4 m) **[CALIBRATE]** and no unit of any other team within `zone_radius` (42 m), for 5 s **[CALIBRATE]**. The city's health rises to at least `CaptureHealthFactor` (0.3) × max, and the nearest unit's regiment gains `ExperienceDefaultValue` × `ExperienceCaptureMultiplier` XP. Contesting or leaving resets it. A held city falls the same way when its holder has no unit in the zone. Event `CityCaptured`.
* **Elimination:** a kingdom that started is eliminated once it has no HQ left: no Structure with `HQComponent` (Corps HQ, division HQs) and no regiment that is an HQ or carries an HQ's truck (a packing or moving HQ counts). Event `KingdomDefeated`.
* **Victory:** with `TeamVictory` (true) the last team with a kingdom standing wins, once two teams have played; without it, the last kingdom. With a city victory ratio above 0 (the map's `CityVictoryRatio` rule, else the SVar; `SetMatchRules` at tick 0 overrides; Golden Fields: 0), a team holding at least that share of the cities' value (their money production) wins at once. Events `TeamVictory` / `CityRatioVictory`; the match keeps its winner and end tick, and the sim runs on.
* **Teams:** `SetMatchRules` (command 17, tick 0 only) sets each player's team, the city ratio and team victory. By default each kingdom is its own team. Combat, siege and capture treat other teams as enemies and allies as friends.

---

## 13. World War II mode (later, M11) **[MANUAL p.18–25]**
* Turn-based world map: territories (money per turn), capitals, armies (Infantry, Mechanized, Armoured, Air Support upgrades, immobile Defensive armies).
* Move 1 territory per turn and fight 1 battle per turn. Win by capturing **2 enemy capitals**.
* A battle is either **Quick Resolve** (auto-calc from army counts, types and tech) or an **RTS battle**: each army adds starting money, and the most advanced army type limits which regiments you can build.
* Data: `MetaGame/` (WorldMap NIFs, Armies, Divisions, DynamicCampaigns, RandomMap).

---

## 14. Presentation
* **Camera:** pan, edge-scroll, zoom (wheel/PgUp/PgDn), rotate (MMB, Num4/Num6).
* **HUD layout as in the original** (minimap bottom-left, info panel centre, command panel bottom-right, stats bar, special-ops and group bars top-left). Use the original UI textures. Support any resolution with 16:9 anchoring; the original used 800/1024/1280 asset sets, so pick the closest set.
* **Audio:** unit acknowledgements (`*_select`, `*_move`, `*_attack`), weapons, deaths, music. Bink videos convert to OGV for intros and cutscenes.

### Modern QoL **[NEW]** (none change the balance)
Rebindable hotkeys (load the original `Localization/Hotkeys` as defaults), UI scaling, widescreen, a 1–4× replay speed slider, a "show ranges" overlay, observer mode, an idle-engineer/truck indicator, and (later) Steam Workshop for TGI mods.

---

## 15. Out of scope for v1
GameSpy services, autopatcher, 56k-era bandwidth modes, DirectX-specific options.
