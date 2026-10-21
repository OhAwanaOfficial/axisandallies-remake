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
| **M9** | Content breadth | All 5 nations + mods, all regiments, airfield/air missions, naval units and amphibious transports, engineers and bunkers, generals and all special ops, fog modes, team commands/flares/tribute, flow fields if needed. | Sonnet · medium |
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
* **Numbers:** 160 regiments on a 1024² map at 10.7 ms p99 per tick.

Carried over from M4:
* building ticks on big maps (label rebuild, PATHFINDING §10);
* the Release GDExtension crashing in the editor (ARCHITECTURE §4.4);
* lazy path refinement;
* real-map roads (objects, M10);
* fps on the dev laptop's GPU (rendering, not the sim; the target PC isn't tested yet).

### Vertical-slice definition (end of M6)
One map, two players (you vs a scripted dummy), one nation each (GBR vs GER). Corps HQ → Infantry/Armour Division HQs → 4 regiment types (rifle infantry, AT infantry, light tank, medium tank) → supply, upkeep, morale, XP, formations → capture 2 cities → destroy the enemy Corps HQ to win. Stable 60 fps, deterministic replay of the whole match.

### Working rhythm with Claude Code
1. Start the session with: *"Read CLAUDE.md and docs/<X>.md. We're on M<n>, task <t>. Plan first, then implement."*
2. Keep each PR small: one system plus its tests.
3. After every sim change, run `simcli determinism-check` (from M3 onward).
4. When a design changes, **update the doc in the same PR**, so the docs stay the source of truth.
