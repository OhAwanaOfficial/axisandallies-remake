# Milestones

Each milestone ends with something you can **run and see**. Work through them in order with Claude Code, one milestone (or sub-task) per session, and have Claude read `CLAUDE.md` plus the relevant doc first.

Suggested model per milestone: **Opus** = design-heavy or tricky correctness, **Sonnet** = most implementation, **Haiku** = mechanical edits.

| # | Milestone | Deliverable / acceptance criteria | Model / effort |
|---|---|---|---|
| **M0** | Repo & build skeleton | Git repo, `.gitignore` (assets blocked). CMake builds `sim` (static lib) + `simcli` + tests. godot-cpp GDExtension builds and loads in Godot 4.x. `SimBridge.hello()` callable from GDScript. CI (GitHub Actions) builds on Windows + Linux. A "no float in sim/" check passes. | Sonnet · medium |
| **M1** | Data layer | `rwd.py list/extract` + C++ `RwdArchive`. VFS with overlay order. **TGI lexer/parser/template resolver**. Loader follows `index_aa.lst`. `simcli dump-defs` prints the resolved `per_tank_medium` (health 400, AP 65, range 20...). **Golden tests** on 20 TGI files. PER/SAU load. Unknown-key report. **Also:** dump the full `Data.rwd` TOC and answer the open questions in ASSET_PIPELINE §6. | **Opus · high** for the parser/resolver design, Sonnet for the rest |
| **M2** | Asset import + Model Browser | `import_game.py` produces `cache/` (static and skinned NIF → glb, KF → clips, DDS/TGA/PNG copied). Godot **Model Browser** shows every unit and building, plays its KF clips, shows attach points. Biome texture switch works. | Sonnet · medium (Opus for skinning bugs) |
| **M3** | Sim core | Fixed-point math lib with tests (mul/div/sqrt/trig tables). ECS pools, tick loop, command queue, sim RNG. Canonical serializer + **state hash**. Replay record/playback in `simcli`. Determinism test (run twice, Debug vs Release → same hashes). | **Opus · high** |
| **M4** | Map, camera, movement | Test map (RMC-like generator: flat terrain + blocking blobs + forest cost) rendered in Godot. RTS camera, selection (click/box/double-click), right-click move. Nav grid, HPA*, regiment anchor + formation slots, local avoidance, groups/divisions. Overlays per PATHFINDING §8. | **Opus · high** for nav, Sonnet for camera/UI |
| **M5** | Combat | Abilities, turrets, projectiles, damage types, ZOC auto-engage, engagement states, morale/rout, XP levels, formation combat efficiency (blending), entrenchment, death and FXC events (SFX/VFX/recoil anim). **`simcli duel` calibration harness** + first calibration pass vs the original. | Opus · high (formulas), Sonnet (systems) |
| **M6** | Economy & bases → **Vertical slice** | Corps HQ, trucks pack/unpack with trailers, placement rules, Division HQs recruit regiments (unit-by-unit fill while in supply), depots, upkeep + shortfall conversion, unit/building limits, supply zones + under siege, healing/replacement, research (properties), city capture, victory. Full HUD (stats bar, command panel, info panel, minimap). **Playable 1v1 skirmish vs a dummy scripted opponent.** | Sonnet · medium/high |
| **M7** | Skirmish AI v1 | AIController per AI.md: Egos, Economy/Production/Base managers, influence maps, task forces, formation discipline, retreat-before-rout. Debug overlay. Headless AI-vs-AI soak. **Beats Easy-you; loses to good-you.** | **Opus · high** for design, Sonnet to implement |
| **M8** | Multiplayer | Lobby, lockstep scheduler, ENet transport, adaptive input delay, hash exchange + desync dumps + `simcli diff/bisect`, drop → AI takeover, replays UI (Films). **2-player LAN game for 30 min with zero desyncs**, then a 4-player internet test. | **Opus · high** |
| **M9** | Content breadth | All 5 nations + mods, all regiments, airfield/air missions, naval units and amphibious transports, engineers and bunkers (construction, repair), generals and all special ops (the HUD's special-ops and group bars), fog modes, team commands/flares/tribute, flow fields if needed. | Sonnet · medium |
| **M10** | Original maps | Reverse the map binary format, load skirmish maps (`(1v1)Golden Fields`, `(4v4)NorthernFrance`, ...), full RMC random-map generator. | Opus · high (reverse engineering) |
| **M11** | World War II mode | World map (WorldMap NIFs), territories, armies, purchase/upgrade/tech, turn flow, Quick Resolve, RTS battle hand-off, metagame AI. | Sonnet · medium, Opus for AI |
| **M12** | Campaigns & polish | Trigger/objective system from `game_events`, campaign missions, Bink → OGV cutscenes, music, options, localization, accessibility, performance pass. | Sonnet · medium |

### Progress

| # | Status | Tag | What's in it |
|---|---|---|---|
| **M0** | Done | v0.2 (with M1–M2) | CMake (sim, simcli, doctest tests), godot-cpp GDExtension with `SimBridge`, Godot 4 project, CI on Windows and Linux (green at v0.2; the clang-cl and Clang jobs added in M3 haven't run yet, since nothing was pushed after v0.2), no-float check. |
| **M1** | Done | v0.2 | TGI lexer, parser, template resolver and `index_aa.lst` loader (TGI_FORMAT.md); `rwd.py` and `RwdArchive`; VFS with the overlay order; `simcli dump-defs`; golden tests on 20 real files; PER/SAU load. |
| **M2** | Done | v0.2 | `import_game.py` (static and skinned NIF → glb, KF → clips, textures) and the Godot Model Browser. |
| **M3** | Done | — (in v0.4) | Fixed Q32.32, trig tables, PCG32; ECS pools; 13-stage tick; commands; canonical state and xxh3 hash; replays; `determinism-check`; typed Def compiler; `spawn-test` identical in Debug and Release. |
| **M4** | Done | v0.4 | See below. |
| **M5** | Parts 1–3 done | — | See below. Left: the first calibration pass against the original (CALIBRATION.md). |
| **M6** | Done | v0.6 | See below. Not in the slice, moved to M9: construction (bunkers) and repair, the special-ops and group bars. |

**M4 details:**
* **Nav:**
  * MapDef (deterministic test-map generator, plus `tools/rmp.py` maps, from the M10 spike).
  * Per-class cost grids, labels, building stamps.
  * HPA* within 1.25× of optimal; nearest-reachable fallback; smoothing.
  * Budgeted PathService, sped up 2–3× in a performance pass.
* **Movement:**
  * Layouts, formations and group formations from the data.
  * Regiment anchors and slots with slowest-unit speed, turning and wheeling, Column / single-file compression in chokepoints, arrival.
  * Loose groups and divisions.
  * Spatial-hash avoidance; stuck → repath.
  * Commands MoveGroup, Run, Stop, SetFormation, SetGroupBehaviour.
* **View:**
  * SimBridge (map, snapshots, events, commands).
  * Battle scene: textured terrain from the original data, interpolated unit models, RTS camera, selection (click, box, double-click, Ctrl+number), right-click move with facing, Alt Run, Q/W/E, F1–F3 overlays.
* **Checks:**
  * `simcli nav-bench`, `scenario` and `perf`.
  * Golden-path, movement and replay fixtures.
  * Godot `--selftest`.
* **Numbers:** 160 regiments on a 1024² map at 10.7 ms p99 per tick (6–8 ms average on ticks with a new building). Sim step 3.9–4.4 ms inside Godot with the optimized GDExtension.

Carried over from M4:
* building ticks on big maps: **done** (cluster-level labels, 6–8 ms avg at 1024², PATHFINDING §10);
* the Release GDExtension crashing in the editor: **done**. Debug, RelWithDebInfo (optimized, what the editor runs) and Release all build and run the battle view. The Godot 2D canvas crash (GLES3 canvas renderer, unchecked `glMapBufferRange` when the GPU runs >100 ms behind; 4.6.3 to 4.8-beta1) is fixed on our side by the `GpuPacer` autoload, so the HUD is 2D again (0 of 14 crashes with it, against 7 of 8 without). The engine patch and an issue draft are in `tools/godot_canvas_crash/`, not filed (ARCHITECTURE §4.4);
* TGI load time: 0.7 s in Release, so no DefSet cache is needed;
* lazy path refinement;
* real-map roads (objects, M10);
* fps on the dev laptop's GPU: **done**. Low/Medium/High graphics presets; Low and Medium draw units as one MultiMesh per type with baked vertex animation. 500 units at 41 fps on Low at 1280×720 (was 8 fps with 70 units; ARCHITECTURE §4.4). The target PC isn't tested yet.

**M5 part 1 details (sim only):**
* **Data:**
  * Weapons per Ability slot (priority, fuzzy range, turret) and turrets (yaw rate, range, reset) for units and buildings, including `DefenderComponent`.
  * Projectiles, damage types, target types, properties (resistances, DV, target flags, required properties) and terrain properties.
  * Attenuation and `max_targets` on abilities.
* **Rules:**
  * The placeholder damage formula, with every constant in `sim/combat/tuning.hpp` (`CombatTuning`, part of the Sim config and the state hash).
  * Resistances from intrinsic, terrain and entrenchment properties.
  * Target filters and target choice (KillTargetIsHighestPriority, check_damage_resistance, the Attack target first).
* **Engagement:**
  * ZOC auto-engage for aggressive formations, holding the move and resuming it.
  * The state machine: Idle, Engaging, Stalemate, Reform, Regroup (Retreating and Routing wait for morale).
  * Closing in when nothing is in range.
* **Weapons:** turrets turning at their yaw rate, local guns facing the target, recharge and reload, homing projectiles, area damage with `max_targets` and attenuation, simultaneous instant hits.
* **Deaths:** dead units and emptied regiments are destroyed, with Died events.
* **Commands:** Attack (7), AttackMove (8), DebugSpawnBuilding (206), DebugSetEntrenched (207).
* **Formats:** state v4, replay v3 (tuning, `arena:` maps).
* **Tools:** `simcli duel` and [CALIBRATION.md](CALIBRATION.md) (10 scenarios, results table empty).
* **Checks:**
  * `test_combat.cpp` (formula, properties, filters, auto-engage, Attack, AttackMove, turrets, projectiles, area, target choice, buildings, formations, entrenchment, save/load mid-fight, command codec).
  * Real-data golden tests (combat defs, a GER vs GBR duel).
  * `simcli_duel` in CTest, with determinism-check of its replay.
* **Numbers:** the 160-regiment perf run, now with the armies fighting, at 6.7 ms p99 per tick; combat stages about 0.7 ms on average.

**M5 part 2 details (sim only):**
* **Data:** unit `morale_value`, regiment experience levels (from each organization's ExperienceComponent: infantry 150/350/600, paratroopers 100/250/500) and `fake_cv_value`, the layouts' retreat formation, and property modifiers.
* **Morale** (`sim/systems/morale.*`): every SVars factor (engagement loss with the ×3 factor, death, slayer, breaker, replenish, retreat penalty), entrenchment's loss rate, the breaking point, rout (flee from the last attacker with random spread, deaf to orders), exhaustion and rally.
* **Experience:** kill, rout and engage XP; levels applying AV, DV, HEALTH, MORALE and MORALE_RECOVERY_RATE; army XP per player (`World::kingdoms`) with the faction multiplier and the periodic gain.
* **Efficiency and entrenchment:** formation efficiency blending at the SVars rates; the entrenchment timer (`entrenchment_time` 30 s halted).
* **Terrain:** terrain defense on real maps (variation names resolve to Terrain records), with a golden test on Golden Fields.
* **Commands:** ordered Retreat (9), DebugSetExperience (208). Events: Routed, LevelUp.
* **Formats:** state v5 (`KING`, morale fields).
* **Duel:** ends at a rout (or `--until-wiped`); `--xp-a/-b`, `--retreat-a/-b`; prints morale, XP, levels and army XP. CTest `simcli_duel_morale` + determinism-check.
* **Numbers:** the 160-regiment perf run with morale at 7.0 ms p99 per tick; the Morale stage 0.30 ms on average.
* **Fixtures:** `sim/tests/fixtures/combat_m5.rpl`, a data-free combat film (3 routs, a level-up, 7 deaths) replayed by every CI job; the synthetic fixtures and the movement golden hash re-recorded.

**M5 part 3 details (view; ARCHITECTURE §4.4 "Combat in the view"):**
* **Events:** `Fired` and `Died` carry the ability, position and facing; new `Hit` (impact point, primary target, ability). Outputs only: hashes and fixtures unchanged.
* **SimBridge:** event details; `record`, `ability_record`, `unit_record`, `read_file`; `regiment_info` with health, morale, efficiency, XP, level and engagement; `banner_instances`; `queue_attack`, `queue_attack_move`, `queue_retreat`.
* **FXC runner** (`fx_runner.gd`): the abilities' and projectiles' CREATE_VFX / PLAY_SFX / PLAY_OBJECT_ANIMATION FXCs on Fired and Hit, with `target_flags`; death sounds and debris; the original WAVs (decoded from the VFS, per-record volume, range, pitch spread, simultaneous limit, pooled 3D voices); procedural sprites per VFX model; Recoil1 on High; vehicle death models and infantry Die clips (baked into the VAT on Low and Medium), rotting and sinking per BodyComponent.
* **Banners:** flag plus health, morale and efficiency bars per regiment, one MultiMesh built in C++.
* **Input:** right click on an enemy attacks; A + click attack-moves or attacks; the original attack cursor; the selected regiment's info panel.
* **Checks:** 9 new self-test checks (23 in all); `--fight`, `--speed`, `--warmup`, `--no-fx` for screenshots and profiling.
* **Numbers:** on Low, effects and banners cost about 0.5 ms a frame, in the 71-unit demo fight and with 160 regiments fighting (28.8 fps vs 29.1 off).

**M6 part 1 details (sim only; GAME_DESIGN §5, §7, §9):**
* **Data:** buildings' Size, padding, lock flags, pack / unpack times and health floor, DeploymentInfo offsets, research items, resolved recruit lists; trucks' `unpack_to` and production; units' required structures (split) and properties; `EconomyDef` (start money 500, caps, prices 2 / 3, padding 2, the start location's building limit 20); trucks linked to buildings and HQ regiments (packed / unpacked); `convoy_<truck>` regiments with a one-element layout for trucks no regiment holds.
* **Economy** (`sim/systems/economy.*`): per-kingdom ledger (production, upkeep, shortfall bought with money, unit / building limits with reservations), money per tick, bankruptcy; research AV / DV in combat.
* **Bases** (`sim/systems/bases.*`): placement, StartPlayer, pack / unpack via trucks with stamping, recruiting unit by unit with requirements, research granting properties (GLOBAL / LOCAL).
* **Commands** 10–16 and 209–210; events Recruited, Unpacked, Packed, ResearchDone, Bankrupt, Solvent, DeployFailed.
* **Formats:** state v6 (`STRC`, `DPLY`, economy in `KING`); DefSet layout v5.
* **Movement fix:** an anchor slides around a building corner its smoothed path clips instead of re-pathing into it (PATHFINDING §5).
* **Tests:** `test_economy.cpp` (13 cases on hand-built defs: income, the manual's shortfall example, bankruptcy, limits, placement, recruiting, requirements, research, pack / unpack, save / load, encoding, a replay fixture `sim/tests/fixtures/economy_m6.rpl`), 2 golden cases on the GBR data, `simcli economy` + determinism check in CTest. Golden movement hash and the M3–M5 fixtures re-recorded (format change; behaviour checks unchanged).

**M6 part 2 details (sim only; GAME_DESIGN §5, §8, §12):**
* **Map things:** `tools/rmp.py` decodes `DYNAMIC_THINGS`, `KINGDOMS` and `RULES` and resolves IDS renames from `translate.tgi` (ASSET_PIPELINE §2.1); `MapDef` gains things, kingdoms and rules (in the map hash); `mapStarts` and nation placeholders give the start positions.
* **Data:** supply range, relay, stockpile max, `supply_required`, `resupply_rate`, `supply_used`, HQ flags (HQComponent and HQ trucks), captureable / destructible, city zone radius; `EconomyDef` siege seconds 7, rate 0.25, `SupplyProvideUnderSiege`, morale supply bonus 1, capture health 0.3, capture XP multiplier 1, city ratio, team victory. DefSet layout v6.
* **Supply** (`sim/systems/supply.*`): zones shared by allies, relays, under siege, healing, free replacement keeping XP and level, morale supply bonus, `supply_required` production, recruiting only in supply, the bankrupt effects (×0.25 in supply, attrition out of it).
* **Cities and victory:** map cities spawn as neutral Structures; capture in 5 s with no enemy in the zone; elimination when the last HQ is gone; team victory and the city control ratio; teams in combat.
* **Commands** 17 (`SetMatchRules`) and 211–212 (`DebugSetHealth`, `DebugSetOwner`); events Replaced, CityCaptured, KingdomDefeated, TeamVictory, CityRatioVictory. StartPlayer uses the map's starting money.
* **Formats:** state v7 (supply and capture in `STRC`, `SUPL`, team and elimination in `KING`, `MTCH`).
* **Tests:** `test_supply.cpp` (17 cases on hand-built defs and a two-city map: zones, allies, relays, healing, replacement, bankruptcy, siege both ways, supply_required, capture, contest, recapture, elimination, allies' victory, city ratio, save / load, encoding, map.json things, a replay fixture `sim/tests/fixtures/supply_m6.rpl`), 2 golden cases on the real data and Golden Fields, `simcli skirmish` (Golden Fields, 8 GBR vs 2 GER regiments until the German HQ falls) + determinism check in CTest. Golden movement hash and the M3–M6 fixtures re-recorded (format change; behaviour checks unchanged).

**M6 part 3 details (Godot HUD; ARCHITECTURE §4.4, GAME_DESIGN §14):**
* **HUD** in the original art (`godot/scripts/hud/`): control panel, stats bar, command panel with the reasons a button is greyed, info panel with the production queue and research, diamond minimap with the camera box and click-to-move, message log with history, placement ghost, Ctrl+S supply overlay; the player's nation skin through `SimBridge.read_file_for_nation`; the 1024 or 1280 set.
* **Battle view:** both kingdoms start with their Corps HQ at the map's start positions; buildings are drawn; building selection; the camera's default heading is diagonal as in the original.
* **Sim:** `recruitBlocker` / `researchBlocker` / `packBlocker` (the commands use them too) and a test of their reasons.
* **Checks:** 17 new selftest checks (41 in all).

**M6 close-out (the vertical slice; v0.6):**
* **Skirmish setup screen** (`godot/scripts/menu/skirmish_setup.gd`, the `Skirmish` autoload): map (every `cache/maps/*/map.json`, or a generated test map), your nation and the opponent's (GBR, GER, USA, RUS, JPN; the staging screen's flags), starting money (the map's, 500, 1000, 2000, 5000), seed. Launching the game without options opens it.
* **Dummy opponent** (`ai/`, AI.md §7a): deploys an Infantry HQ and a Motor Pool, keeps oil and ammo out of the red, recruits rifle and AT infantry, defends its buildings, sends a wave every 4 minutes from minute 6.
* **Victory / defeat screens** (`godot/scripts/hud/end_screen.gd`): result, how it ended, match time, regiments recruited, units and buildings lost, cities held; continue watching, new skirmish (back to the setup screen), quit.
* **Play-tests:**
  * `simcli slice`: a scripted player plays the slice path headless against the dummy and checks it: Infantry and Armour HQs deployed, the four regiment types recruited, both cities taken, the enemy eliminated, victory. It records the match.
  * `godot --headless --path godot -- --slice-test`: the same through the game itself. The setup screen is filled in and started, the battle runs to the victory screen, and the replay is written.
  * `--defeat-test`: the mirror (the dummy plays you) ends on the defeat screen, and New skirmish returns to the setup screen.
  * All of these run in CTest with determinism checks of their replays. GBR against GER on Golden Fields (seed 2): victory at 18:57, 11,476 ticks, deterministic at every stage. The Godot run's replay ends on the live game's hash.
* **Sim:**
  * Starting money is a match rule: `SetMatchRules` gains `startMoney`, the state goes to v8 (`MTCH`).
  * A building that dies drops the regiment it was still filling with no unit yet. An empty HQ regiment kept a kingdom alive forever; HQ regiments now count only with a unit left.
  * The M3–M6 fixtures and the movement golden hash are re-recorded.
* **SimBridge:** `set_controller` (the AI on the host, thinking before each step), `queue_match_rules`, `save_replay` (every command queued since start, re-run to fill the hashes and checked against the live state), `drain_ai_notes`, `mount` (the install's files before a match).
* **Against the definition below:** one map (Golden Fields), you against a scripted dummy, GBR against GER (the setup screen offers the other three nations too). The data's chain needs the Mechanized HQ before the Armour HQ and the Motor Pool for AT infantry, and both are part of the slice path. Supply, upkeep, morale, XP and formations are all in play. The cities and the Corps HQ fall, and the match ends on the elimination rule (every HQ, division HQs included; GAME_DESIGN §12). The deterministic replay of the whole match is checked in CTest. **Not met: stable 60 fps.** The dev laptop's 50 Hz panel holds V-Sync to about 24 fps with or without the HUD; uncapped, Low runs the 71-unit demo at 74–86 fps and 499 units at 48 fps (ARCHITECTURE §4.4).

### Vertical-slice definition (end of M6)
One map, two players (you vs a scripted dummy), one nation each (GBR vs GER). Corps HQ → Infantry/Armour Division HQs → 4 regiment types (rifle infantry, AT infantry, light tank, medium tank) → supply, upkeep, morale, XP, formations → capture 2 cities → destroy the enemy Corps HQ to win. Stable 60 fps, deterministic replay of the whole match.

### Working rhythm with Claude Code
1. Start the session with: *"Read CLAUDE.md and docs/<X>.md. We're on M<n>, task <t>. Plan first, then implement."*
2. Keep each PR small: one system plus its tests.
3. After every sim change, run `simcli determinism-check` (from M3 onward).
4. When a design changes, **update the doc in the same PR**, so the docs stay the source of truth.
