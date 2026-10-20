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
Known from data:
* Damage effects come in **types**: `PierceDamageEffect` (small arms), `APDamageEffect` (armour-piercing), `BlastDamageEffect` (explosive, `area` radius with `attenuation_range_min`/`attenuation_factor_max`).
* Targets have **defense** and **properties** (`property_infantry_unit`, vehicle armour properties, etc.). `check_damage_resistance = true` makes the attacker consider resistances when choosing targets.
* **Target types** (`air, tank, infantry, artillery, building, vehicle, engineer, commanding_officer, depot, routing, sabotaged, org_*`) are used for target priority and ability filters (e.g. the AA halftrack can only hit `air`).
* `max_targets`, `area`, `range`, `recharge_time`, turrets with yaw rate and range.

**Initial formula** (to be replaced by calibrated values):
```
raw      = effect.amount × typeVsArmorMultiplier(effect.type, target.properties)
mitig    = raw × K / (K + target.defense × D)         # K, D tuned
final    = mitig × attackerCombatEfficiency × attackerXPMult × terrainDefenseMult(target) × entrenchMult(target)
```
**Calibration harness (M5):** build 10 duel scenarios in the original game's map editor (e.g. 1 rifle regiment vs 1 rifle regiment on open ground, rifle vs medium tank, MG bunker vs infantry, in jungle and entrenched). Record time-to-kill and survivors from films and video. Tune `K`, `D` and the type multipliers until our `simcli duel` results fall within ±10%.

### 6.4 Formations & combat efficiency **[MANUAL p.36–38]**

| Formation (hotkey) | Combat efficiency | Speed | Behaviour |
|---|---|---|---|
| **Assault** [Q] (default) | +0% | −25% | Aggressive |
| **Normal/Aggressive** [W] | −25% | +0% | Aggressive |
| **Column** [E] | −50% | +25% | Passive (ignores enemies) |

In the data these are `regiment_aggressive` (Assault), `regiment_defensive` (Normal/Aggressive) and `regiment_column` (Column) in `Organizations/Formations/regiment_formations.tgi`. The speed and efficiency numbers above come from their modifiers.

* **Efficiency blends gradually** to the new formation's level after a change (rate **[CALIBRATE]**).
* **Run** [R] / Alt+right-click: temporary Column, then back to the previous formation on arrival.
* Aggressive regiments **auto-engage** anything in their zone of control (ZOC). A tank regiment in Assault formation crossing an enemy base will stop and destroy every building on the way. That's intended.

### 6.5 Orders **[MANUAL]**
Move [M], Attack [A] (chases), Run [R], Stop [S], Guard [G] (follow/hold and engage nearby), plus Unpack/Pack [P], Deploy [D], Build (engineers), Repair, Group Behaviour [V].

### 6.6 Engagement state machine **[DATA `SVars_AA.tgi`; actor_states]**
States: `Idle → Engaging → (Retreating | Routing | Stalemate) → Reform/Regroup → Idle`
* `PrimarySampleTime = 5 s` (re-evaluate engagement), `RetreatingTime = 10`, `RoutingTime = 10`, `ReformTime = 10`, `StalemateTime = 10`, `RegroupTime = 10`.
* `KillTargetIsHighestPriority = true`: keep shooting a target that's about to die.
* `CAITargetInRangeZOCMultiplier = 0.6`: when ordered to attack, move to 60% of ZOC radius before stopping.
* `mind_retreating`: ordered retreat → RETREAT formation, PASSIVE, can still resupply.
* `mind_exhausted`: BROKEN formation, removed from group, morale replenishes.

### 6.7 Morale **[DATA SVars][MANUAL p.40]**
* Blue bar with a **breaking point**. Below it, the regiment **routs**: Column formation, flees, uncontrollable until it stops (may run the wrong way).
* Loss: being engaged (`MoraleEngagementLoss = 0.5`/sample, scaled up to ×3), unit deaths (`MoraleDeathFactor = 0.25`), "breaker" attacks (flamethrowers, `MoraleBreakerBase = 3.0` ×0.5–2), fear enchantments.
* Gain: time out of combat (`MoraleReplenish = 2.0`), supply (`+MoraleSupplyBonus = 1.0`), killing enemies ("slayer", `MoraleSlayerBase = 0.5` ×0.5–3), courage enchantments, officers.

### 6.8 Experience **[MANUAL p.40]**

| Level | XP needed | Insignia |
|---|---|---|
| Recruit | 0 | — |
| Regular | 100 | 1 chevron |
| Veteran | 250 | 2 chevrons |
| Elite | 500 | First sergeant |

Replacement units from resupply keep the regiment's XP. A whole new regiment starts as Recruit. Bonuses per level: **[CALIBRATE]** (check unit tooltips in the original).

### 6.9 Entrenchment
A regiment that stays still long enough becomes entrenched (sandbags on the banner) and gets the `infantry_entrench`/`vehicle_entrench` property bonuses **[DATA actor_states]**. Time to entrench: **[CALIBRATE]**.

### 6.10 Terrain
Terrain groups: dirt, snow, water, desert (+ forest/jungle/mountain blobs from RMC). Each terrain gives per-movement-class **speed** and per-unit-type **defense** modifiers (e.g. jungle: big infantry defense bonus, big vehicle penalty, slow) **[MANUAL]**. Mountains and ocean block ground units.

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

**Pack/unpack** **[DATA `MobileDeploymentComponent`]**: a building packs onto trucks (+ trailer) in `packing_length = 7 s`, can't pack below `pack_health_min = 50%` health, moves like a regiment, then unpacks at a valid placement (red ghost if invalid). Placement rules come from `placement_groups*.tgi` and building `Size` (e.g. `square_size = 10`).

**Biomes:** buildings and trailers have `Arctic/` and `Desert/` texture variants. Choose by map biome.

---

## 8. Supply & healing **[MANUAL p.39; DATA SVars]**
* **Zone of supply**: a circle around every major base building, Supply Depot and owned city. Allies share supply zones.
* Attached regiments **in supply** heal unit by unit and **replace dead units for free**, keeping XP. That's why retreating a beaten regiment beats rebuilding it.
* **Under siege**: if an enemy is near (or the building was damaged within `UnderSiegeRecentlyDamagedSeconds = 7`), that building's supply zone **deactivates**. Exception flag: `SupplyProvideUnderSiege`.
* Out of supply while bankrupt: health drains.
* HUD: supply-border toggle [Ctrl+S].

---

## 9. Generals, special ops, research
* Each player picks **1 of 4 generals** for their nation. Each general has **4 special operations** (hotkeys Y/H/N/J), with a cooldown bar plus an **army XP cost** per use.
* Special ops are defined in data (`SpecialOps/*.tgi`, `ability_special_op` templates) and implemented as abilities/enchantments (e.g. `ench_type_courage/fear/radiation`).
* **Research** **[DATA `ResearchComponent`]**: per-building research items, each with a property, a type (`LOCAL` = this actor, `SUPPORT` = settlement, `GLOBAL` = kingdom), required structures/properties, time and cost. Research grants **properties**, and properties modify stats. This is the general modifier system: one `PropertyDef` applies stat deltas and multipliers and unlocks.

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
