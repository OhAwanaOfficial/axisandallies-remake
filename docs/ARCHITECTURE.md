# Architecture

Working name: **Project Corps**. This is a placeholder. Don't use "Axis & Allies" in a public project name because Hasbro owns the trademark.

This is a **reimplementation of the 2004 TimeGate RTS engine** that reads the original game's data (models, textures, TGI rules) from the player's own installed copy. The model is the same one OpenMW, OpenRA and OpenTTD use: the repository contains only our code and tools. Game assets are never committed or redistributed (see [ASSET_PIPELINE.md](ASSET_PIPELINE.md#legal-model-bring-your-own-data)).

Related docs: [GAME_DESIGN.md](GAME_DESIGN.md), [PATHFINDING.md](PATHFINDING.md), [AI.md](AI.md), [NETCODE.md](NETCODE.md), [ASSET_PIPELINE.md](ASSET_PIPELINE.md), [MILESTONES.md](MILESTONES.md).

---

## 1. What we learned from the original (evidence)

| Observation | Where | Consequence for us |
|---|---|---|
| All gameplay is **data-driven text (TGI)** with template inheritance (`[Thing Template=AAUnitPERVehicleMediumTank]`), loaded in the order set by `index*.lst` | `data/index_aa.lst`, `data/templates_AA.lst`, every `*.tgi` | Build a **TGI parser and template resolver** and use the original data as the rules source. Faithfulness then comes for free, and existing mods (the PER/SAU nations) work. |
| Numeric data type `fixed` everywhere; log shows `KKC_FixedPointDataValue` pools | `resource_list.tgi`, `SVars_AA.tgi`, `Logs/log-9-ok.log` | The original sim was **fixed-point**, which is the classic sign of deterministic lockstep. We do the same. |
| Replays (`.RFM` "films") and `LAST FILM.RFM` are auto-saved for every game | `Readme.txt` | The original was **command-stream lockstep**. The netcode and replays follow the same model ([NETCODE.md](NETCODE.md)). |
| Multiplayer uses UDP 6900/5860, and lag scales with players, regiments and AI players | `Readme.txt` | Same topology class. Keep AI cheap on the host. |
| Actors are built from **components**: `BodyComponent`, `MoverComponent`, `PerformerComponent` (abilities, turrets), `EconomyComponent`, `OrganizationComponent` (regiment slots), `MobileDeploymentComponent` (pack/unpack), `RecruitComponent`, `ResearchComponent`, `SupplyComponent`, `DetectionComponent`, `DefenderComponent`, `PropertiesComponent`, `SettlementComponent` | `Templates/Components/*.tgi`, unit/building TGIs | Our entity model **mirrors these components 1:1**, so TGI blocks map directly onto sim components. |
| Presentation hooks are declared in data as `WhenDo`, `WhenCreated`, `WhenDestroyed`, `WhenSelected` etc. → `FXC_PlaySFX`, `FXC_CreateVFX`, `FXC_PlayObjectAnimation`, `FXC_Equipment` | unit TGIs, log pools `KKC_FXC*` | The sim emits typed **events**, and the presentation layer runs the FXC lists from data. |
| A **depot** (virtual filesystem) system layers `Data.rwd` → `data/` → user folder, with later depots overriding earlier ones | `startup/autoexec.txt` | Our VFS uses the same overlay order. This is how the `data/` PER/SAU clones override and extend `Data.rwd`. |
| Class prefixes `KKC_`/`KKS_` and SVars comments like "bigger than in K2" | logs, `SVars_AA.tgi` | The engine is TimeGate's **Kohan II-era RTS engine** (inferred). Kohan II mechanics (supply zones, regiment companies, morale/rout, ZOC) apply. |
| Models are **Gamebryo NIF 10.1.0.0**, animations are **KF** (`NiControllerSequence`), textures are **DXT5 DDS** | verified by parsing (see ASSET_PIPELINE) | Convert offline to glTF. Godot loads DDS natively. |

---

## 2. Technology choices

| Layer | Choice | Why |
|---|---|---|
| Engine / renderer / UI / audio | **Godot 4.x** (pin the latest stable at project start) | Free, glTF-native, imports DDS, good 2D UI for the RTS HUD, easy scripting in GDScript (similar to Python). |
| Simulation core | **C++20 static library** with no Godot dependency, exposed to Godot through **GDExtension (godot-cpp)** | Determinism needs full control of integer math, memory layout and iteration order, plus the speed to run around 1,300 units and pathfinding at 10 Hz. It also builds as a **headless CLI** for tests, replays, desync bisection and AI-vs-AI soak runs. |
| Gameplay glue, UI, camera, input, FX | **GDScript** | Fast iteration. None of it affects the sim's outcome. |
| AI controllers | **GDScript first**, moving hot paths to C++ if needed | AI runs only on the host and emits commands ([AI.md](AI.md)), so it **does not need to be deterministic**. |
| Tools (archive reader, NIF→glTF, data dumps) | **Python 3** (+ the existing PowerShell scripts) | Already proven: `pyffi` parses the NIF/KF files. |
| Build | CMake for the sim lib and tests, SCons/CMake for the GDExtension, Godot export templates | Standard. |
| Tests | doctest/Catch2 (C++), GUT (GDScript, optional), pytest (tools) | |
| Networking | ENet through Godot's **low-level** `ENetConnection`/`PacketPeer` API, not the high-level RPC/MultiplayerSynchronizer | Lockstep sends commands, not replicated state ([NETCODE.md](NETCODE.md)). |

---

## 3. The big picture

```mermaid
flowchart LR
  subgraph Disk["Player's own game install (read-only)"]
    RWD["Data.rwd / Music.rwd / Speech.rwd / Video*.rwd / skins/*.rwd"]
    LOOSE["data/ (mods, e.g. PER/SAU)"]
  end
  subgraph Tools["Offline import (Python)"]
    IMP["import_game.py: RWD reader, NIF/KF→glTF, DDS/TGA, Bink→OGV"]
  end
  CACHE["cache/ (gitignored): .glb, textures, audio, video"]
  subgraph Runtime
    VFS["VFS / Depots (overlay order)"]
    TGI["TGI parser + template resolver → typed Defs"]
    SIM["SIM CORE (C++, fixed-point, deterministic)"]
    VIEW["Presentation (Godot): render, interpolation, FXC events, audio, HUD"]
    CTRL["Controllers: Local input, AI (host only), Network peers"]
    SESS["Session: lockstep scheduler, replay recorder, save/load, desync check"]
  end
  RWD --> IMP --> CACHE --> VIEW
  RWD --> VFS
  LOOSE --> VFS
  VFS --> TGI --> SIM
  CTRL -- Commands --> SESS -- "Commands for tick T" --> SIM
  SIM -- "Snapshot + Events" --> VIEW
  SIM -- "read-only queries" --> CTRL
```

### Hard boundary: sim vs view

* The **sim** is a pure function: `State(T+1) = Step(State(T), Commands(T))`. It never calls Godot, never reads the clock, never uses `float`/`double`, and never iterates hash containers in unspecified order.
* The **view** reads the sim through a **read-only snapshot API** and **interpolates** positions and orientations between the last two ticks for smooth 60+ fps rendering.
* Everything the player does becomes a **Command** (`MoveRegiment`, `SetFormation`, `Deploy`, `Unpack`, `Recruit`, `Research`, `UseSpecialOp`, ...). Commands are the only way to change the sim. That makes replays, multiplayer and AI the same thing.

---

## 4. Runtime modules

### 4.1 VFS / Depots (`sim/vfs`, also used by the view)
* Mount order copied from `startup/autoexec.txt`: `Data.rwd` → `data/` → `Music.rwd` → `Speech.rwd` (locale) → user overlay (`%USERDATA%/data`, `Userdata/`). **Later mounts win.**
* `RwdArchive` reads the TGCK format (documented in [ASSET_PIPELINE.md §2](ASSET_PIPELINE.md#2-rwd-archive-format-tgck)), with case-insensitive paths and `/` separators.
* Works directly on the `.rwd` files, so you don't need `_rwd_extract/`. That folder stays useful for browsing.

### 4.2 Data layer: TGI → Defs (`sim/data`)
* **Lexer/parser** for TGI: sections `[Type Template=X] { ... }`, flat sections without braces (e.g. `[BodyComponent] health = 200`), `key = value`, typed template declarations (`fixed`, `int`, `float`, `bool`, `flag`, `string`, `point2`, `point3`, `[list X Template=Y]`), comments `;`, `;;`, `/* */`, localized refs `"#ids"`, Windows-1252 encoding, case-insensitive keys (`IDS`/`ids`, `Template`/`template`).
* **Loader** follows `index*.lst` (alphabetical, wildcards, nested `.lst` processed in place). Templates must load before data.
* **Template resolver** builds the inheritance chain and applies overrides: scalars replace, `[list]` children append, named children deep-merge, and a child that names its own `template=` rebases. Templates bind in load order. The full grammar, rules and edge cases are in [TGI_FORMAT.md](TGI_FORMAT.md).
* **Def compiler** turns the resolved node tree into **typed, immutable C++ structs** (`UnitDef`, `RegimentDef`, `BuildingDef`, `AbilityDef`, `ResearchDef`, `NationDef`, `GeneralDef`, `SpecialOpDef`, `RmcDef`, `SVars`, `WorldRules`...). Numbers marked `fixed` become fixed-point at compile time.
* A **data hash** over the compiled defs is used for multiplayer and replay compatibility checks.
* **Implemented (M3):** `sim/data/def_compiler` produces `UnitDef` (categories `UNIT` and `TRUCK`), `RegimentDef` (category `ORGANIZATION`), `BuildingDef` (`BUILDING`), `AbilityDef` (top-level, plus every inline `PerformerComponent/Ability` as `<owner>#<n>`), `NationDef` and `SVarsDef`, all with Fixed numbers.
  * Regiment slots resolve to a unit/truck or a building. An unpacked HQ regiment's slot holds the HQ building itself.
  * Other categories (doodads, effects, metagame, trailers…) aren't compiled yet; `simcli show-def` prints the category histogram.
  * `dataHash = xxh3_64(serializeDefs(defs))`.
* Unknown keys are kept and logged rather than dropped, so we can find what we haven't implemented yet (`--report-unknown`).

### 4.3 Simulation core (`sim/`)
**Data-oriented ECS-lite** (`sim/ecs`).
* **`EntityId`** is 32 bits: a 12-bit generation (high) and a 20-bit index (low). Generations start at 1, so 0 is the null id.
* **Destruction** is deferred to the Cleanup stage. The freed index then goes to the back of a FIFO free list.
* **Retirement:** a slot whose generation would wrap past 4095 is retired, never reused, so a stale id can't alias a newer entity.
* **Pools:** each component lives in a sparse-set pool whose dense arrays stay sorted by entity index. A plain walk is therefore always **ascending EntityId** (deterministic).
* **Current components:** `Transform`, `DefRef`, `Health` and `Member`. More arrive with their systems.

Components mirror the original:

| Our component | Original TGI block | Key fields |
|---|---|---|
| `Body` | `BodyComponent` | health, defense, damage resistances, `Size` (blocking shape) |
| `Mover` | `MoverComponent` | speed_max, acceleration, movement class, trailer |
| `Performer` | `PerformerComponent` | abilities (turreted or local), turret yaw rates, attack_height, cv_bias |
| `Economy` | `EconomyComponent` | Cost, Upkeep, Production lists |
| `Organization` | `OrganizationComponent` | layout_ids, slots (element ids), formation, morale, xp, combat efficiency |
| `Element` | `ElementComponent` | parent organization, slot index, required structures/properties |
| `MobileDeployment` | `MobileDeploymentComponent` | unpack_to, pack/unpack timers, pack_health_min |
| `Recruit` | `RecruitComponent` | recruitable organization ids, queue |
| `Research` | `ResearchComponent` | research items, progress |
| `Supply` | `SupplyComponent` | zone radius, active/under-siege |
| `Detection` | `DetectionComponent` | sight radii, detect hidden |
| `Defender` | `DefenderComponent` | control_range, built-in turret abilities |
| `Properties` | `PropertiesComponent` | set of property ids (tags and modifiers) |
| `Settlement` | `SettlementComponent` | cities: capture, level, structures |
| `Kingdom` (per player) | n/a | resources, limits, army XP, general, tech, team |

**Fixed tick order (10 Hz, 100 ms per tick).** It never changes at runtime:

1. **Apply commands** for this tick, sorted by player id, then sequence, then position in the bundle. `Sim::step(commands)` receives exactly one tick's commands. The **CommandQueue** that holds future commands belongs to the session (replay player, lockstep scheduler), not to the sim. Peers receive future bundles at different times, so future commands must not be part of the hashed state. A command for any other tick is rejected deterministically.
2. **Economy**: production, upkeep, oil/ammo shortfall → money conversion, limits.
3. **Construction / pack / unpack / recruit / research** timers.
4. **Orders → plans**: regiment/group order state machines (move, attack, guard, run, retreat).
5. **Pathfinding service**: process the request queue with a fixed node-expansion budget ([PATHFINDING.md](PATHFINDING.md)).
6. **Movement**: regiment anchor advance, formation slots, unit steering and local avoidance.
7. **Vision / fog**: per-team visibility grid.
8. **Engagement & targeting**: ZOC checks, engagement state machine, target selection.
9. **Abilities**: fire, spawn projectiles, apply effects/damage.
10. **Morale / experience / entrenchment / combat efficiency** blending.
11. **Supply & healing / replacement**, under-siege flags.
12. **Capture, objectives, victory** (`game_events`).
13. **Cleanup** (deaths → events), then the **state hash** (every N ticks).

Events (`UnitFired`, `UnitDied`, `RegimentRouted`, `BuildingPacked`, ...) are appended to a per-tick buffer that the view drains.

**Math library (`sim/math`, M3).**
* **`Fixed`:** Q32.32 in `int64`, range ±2³¹, resolution 2⁻³².
  * Multiply and divide use 128-bit intermediates (`math/int128.hpp`). There's a portable constexpr version; fast paths use `__int128` on GCC/Clang or `_umul128`/`_udiv128` on MSVC x64, and clang-cl divides portably. A test checks the fast and portable paths give identical bits.
  * **Rounding:** multiply rounds half up; divide rounds to nearest, ties away from zero.
  * **Overflow:** results **saturate** to min/max. Division by zero gives ±max (0/0 gives 0). There's no signed-overflow undefined behaviour anywhere.
  * Saturations are counted for diagnostics only; the sim never reads the count.
  * `1.5_fx` literals are parsed from their characters at compile time, never as a float.
* **`isqrt64`/`isqrt128`** give the exact floor square root; Fixed `sqrt` is built on them.
* **`Angle`** is a 16-bit binary angle (65536 = one full turn).
  * `sin`/`cos` use a 4,097-entry quarter-wave table with linear interpolation (error about 2·10⁻⁸).
  * `atan2` folds the input into one octant and uses a 4,097-entry arctan table. It inverts sin/cos to within 1 unit for all 65,536 angles.
  * The tables (`math/trig_tables.inc`) come from `tools/gen_trig_tables.py`, which uses exact decimal arithmetic (no libm). CTest checks the committed tables are current.
* **`FVec2`:** dot and cross accumulate in 128 bits and round once. `length()` works for every representable vector.
* **`Rng`** is PCG32 (pcg32_random_r; the reference vector is a test).
  * **One stream is owned by the sim**, derived from the match seed. Per-system sub-streams are `Rng::derive(matchSeed, streamId)` with fixed `RngStream` ids, so adding a system never shifts another system's draws.
  * Bounded draws use Lemire's unbiased method.
* **Cross-platform check:** a fixed checksum over 10k mixed operations must match on every compiler and build type.

**Canonical state, replays and determinism tooling (`sim/session`, M3).**
* **`saveState`/`loadState`:** one byte layout (format v3) used for the state hash, saves and desync dumps. It holds the header (tick, seed, data hash, map hash, RNG), the entity registry, each component pool by ascending id (Transform, DefRef, Health, Member, Footprint, Path, Regiment, Order, Mover, Group), then the nav section: graph version, clusters dirty since the last rebuild, and the PathService queue and suspended job. The nav grid and HPA* graph are not stored; loading rebuilds them from the map and the live footprints. **The state hash is `xxh3_64`** of those bytes; xxHash 0.8.2 is vendored in `third_party/`.
* **Replays (`.rpl`):** header (sim version = git tree hash of `sim/`, data hash, seed), commands, and `(tick, hash)` every 10 ticks, followed by an integrity hash.
* **`simcli` commands:** `record` / `play` / `determinism-check [--every-tick] [--per-stage]`. The determinism check runs the replay twice from fresh Sims and compares per-tick hashes, and per-stage hashes with `--per-stage`. It also compares against the hashes stored in the file, which may come from another build or OS. A committed synthetic fixture (`sim/tests/fixtures/synthetic_300.rpl`) is replayed on every CI job.
* **Tick rate:** we run **10 Hz** as specified here and in NETCODE. The original's `SVars` says `GameTicksPerGameSecond = 16`. It's one constant (`kTicksPerSecond`), so it can change before movement (M4) bakes in per-tick numbers.
* The sim is **2D**: the ground plane (x, y). Height is view-only (terrain heightmap sampling for rendering). Air units carry a layer flag, not a z coordinate.

### 4.4 Presentation (`godot/`)
* `SimBridge` (GDExtension class) owns the sim instance and exposes `step(commands)`, `snapshot()` and `drain_events()`.
* **Render interpolation**: `alpha = time_since_tick / tick_len`, and the view lerps between prev and curr positions (the snapshot keeps both).
* **ActorView** per entity: instanced from the cached `.glb` resolved by `resource_prefix`, with skins per biome (`Arctic/`, `Desert/` subfolders) and per nation.
* **FXC runner**: runs the data-declared FXC lists for events (`FXC_PlaySFX`, `FXC_CreateVFX`, `FXC_PlayObjectAnimation`, `FXC_Equipment`, `FXC_ChangeSkin`...). Each FXC type is a small GDScript handler.
* **HUD**: stats bar (money reserve, +income, oil, ammo, unit limit, building limit), operations bar (army XP, special ops), command panel, info panel (regiment banner, health/morale/efficiency bars), minimap, message log, team commands and flares. The original UI art is in `_rwd_extract/UI/` and can be used directly.
* **Camera**: RTS camera (pan, edge scroll, zoom, rotate), with selection by click, box and double-click (same type).

**Implemented (M4 part 3)** in `gdext/sim_bridge.*` and `godot/scripts/battle/`, scene `res://scenes/battle.tscn` (the main scene):

* **SimBridge** owns the game data (VFS, TGI database, DefSet), the MapDef and the Sim.
  * **Set-up and static data:** `start(game_dir, map_spec, seed)`, `map_info()` (size, view heights, terrain names and per-cell terrain/kind), `terrain_table(biome)`, `unit_visual(def)`, `regiment_def(def)`, `find_open_area()`.
    * `terrain_table` gives every TGI `Terrain` variation's texture (relative to its record's folder), data colour and repeat size.
    * `unit_visual` gives the cache model path: `resource_prefix` relative to the unit's TGI folder, `.glb`.
  * **Commands:** `queue_*` for spawn, move (Run, explicit facing), stop, formation, group behaviour and debug blockers. Queued commands are applied by the next `step()`. Selections over six regiments are sent as several groups.
  * **Reading:** `snapshot()` returns parallel packed arrays per entity: id, kind, def, organization, player, moving, and previous and current position and facing. `drain_events()`, and the debug queries `regiment_info()` (state, formation, compression, slot targets, path) and `cost_grid(class)`.
  * Floats exist only on the bridge's side. Everything handed to the sim is `Fixed`, and the view can't reach sim state except through commands.
* **Loop:** `battle.gd` steps the sim at 10 Hz from an accumulator (up to 8 ticks per frame; Space pauses; +/- changes speed) and renders at `alpha = accum / 0.1` between the snapshot's previous and current transforms.
* **Terrain** (`terrain.gd`, `terrain.gdshader`):
  * Mesh: a heightfield from the map's view heights. Generated test maps get a small relief from their terrain kinds.
  * Texturing: one `Texture2DArray` layer per used variation (the original `.tga` from `cache/Terrain/...`, or the data colour when missing), picked per 2-unit tile and blended across tile borders. Each repeats every `texture_width` tiles.
  * Water: a translucent plane at the median land height − 0.25 over water.
  * Not yet: the original transition and edge textures, detail and macro textures, animated water.
* **Units:** `unit_models.gd` loads one prototype per UnitDef from the cached `.glb` (hidden helper nodes such as `SelectionBox` stay hidden) with its Walk/Jog/Idle/Fidget1 clips from `compatible_clips`, retargeted once. Two renderers draw them, picked by the graphics quality (below):
  * `unit_batches.gd` (Low, Medium): one `MultiMesh` per UnitDef, so a unit type costs one draw call per material.
    * The model is merged into one mesh in unit space, surfaces grouped by material.
    * Skinned models: the move and idle clips are skinned on the CPU at load and baked into a vertex animation texture (VAT): a row per frame (15 per second, at most 40 per clip), a texel per vertex with the position in RGB and an octahedral normal packed into A. `unit_vat.gdshader` plays it from `TIME` and each instance's custom data (moving, phase), blending two frames near the camera and stepping frame by frame beyond `anim_lod_distance` (60 on Low, 150 on Medium). Animation costs no CPU per frame.
    * Instance buffers come from `SimBridge.unit_instances(alpha, selected_orgs)`, which interpolates every unit in C++ and places it on the terrain heights handed over by `set_view_heights()`. Selection rings are a second MultiMesh per type, of selected units only.
    * Each batch rotates through 3 MultiMeshes: rewriting a buffer the GPU is still drawing from makes ANGLE wait for the GPU (about 10 ms a frame with 500 units on the HD 3000).
    * `godot --path godot --script res://scripts/tools/vat_check.gd -- <model.glb> <clip>` compares the bake with Godot's own skinning (needs a renderer, not `--headless`). PERInfantry and GERInfantry, Walk and Idle: difference 0.0000.
  * `unit_views.gd` (High): a copy of the model and an AnimationPlayer per unit, blending between clips.
  * Facing: sim θ → `rotation.y = -θ`, and imported models face Godot +Z, so the model is turned by +90°.
  * Not yet: biome and nation skins, death and attack animations, FXC.
* **Graphics quality** (F6 cycles it and saves it to `user://settings.cfg`; `--quality=low|medium|high` overrides it). The default is Low when Godot fell back to GL Compatibility (no Vulkan or D3D12), Medium otherwise.

  | | Low | Medium | High |
  |---|---|---|---|
  | Units | MultiMesh + VAT | MultiMesh + VAT | node tree + AnimationPlayer per unit |
  | Lighting | per vertex (terrain and units) | per pixel | per pixel |
  | Terrain shader | one layer sample per pixel, hard tile borders | four layers blended across tile borders | same as Medium |
  | Shadows | off | one orthogonal map, 120 m, only units cast | four splits, 250 m, terrain casts too |
* **Camera** (`rts_camera.gd`): pan with the arrows or the screen edges, zoom with the wheel, rotate with middle drag or PgUp/PgDn, Home resets. Fixed 52° pitch.
* **Input:**
  * **Selection:** left click picks the regiment of the nearest own unit (24 px); box drag; Shift adds; double-click selects all visible own regiments of that RegimentDef (the OS times it and Godot flags the second press `double_click`, so it works at any frame rate); Ctrl+1..9 stores the selection, 1..9 recalls it (tap twice to jump the camera).
  * **Orders:** right click moves, right drag moves with a facing, Alt+right click runs. Q/W/E set Assault/Normal/Column, V toggles group behaviour, S stops, B drops a debug blocker.
* **Debug overlays:** F1 cost grid of the selected regiment's class (red blocked, green normal, blue roads, yellow/orange slower), F2 paths (remaining part bright), F3 slot targets with anchors and headings, F4 HUD (tick, fps, the selection's formation, state, group and compression, recent arrival/blocked events).
* **Checks:** `godot --path godot -- --selftest` drives the controls with synthetic input events, prints PASS/FAIL and exits 1 if any check failed:
  * click, Shift-click, double-click, box, Ctrl+1/1;
  * Q/E formations, right-click move to the clicked ground point, Alt+right-click Run;
  * F1–F3 overlays, every unit drawn, and F6 cycling through the three qualities with every unit drawn in each.
  `--screenshot=PATH --frames=N` renders a frame and quits.
* **Open:**
  * GDExtension builds, by Godot template kind:
    * The editor and `godot --path` load the template_debug library `bin/windows/debug/simbridge.dll`. Two configs write it: Debug (unoptimized) and RelWithDebInfo (optimized, with symbols; `windows-relwithdebinfo`). The battle view uses RelWithDebInfo. After a Debug build, delete the DLL before building RelWithDebInfo: MSBuild sees the newer Debug DLL as up to date and doesn't relink (a Debug DLL imports `MSVCP140D`/`VCRUNTIME140D`; its sim step is ~40× slower).
    * Release writes the template_release library `bin/windows/release/`, for exports.
    * The root CMakeLists strips godot-cpp 4.3's hardcoded `/MDd`, which it applies whenever `CMAKE_BUILD_TYPE` is empty, as with Visual Studio.
    * It also defines `DEBUG_ENABLED` and `DEBUG_METHODS_ENABLED` for godot-cpp's RelWithDebInfo; godot-cpp itself defines them only in Debug.
  * **Known crash (Godot 4.7.2, dev laptop), avoided by drawing the HUD in 3D:** with 160 regiments moving and any 2D item visible, Godot crashes (signal 11) in its 2D canvas renderer.
    * Where: `msvcrt!memmove` under `LocalVector`, called from the code that culls and renders the CanvasItem tree (same offsets in every crash: main+153bd7d, 153f1ef, 379d9df, 379dc02 in the official 4.7.2 exe).
    * Not the GDExtension: a GDScript-only project with no extension crashes the same way (below). An AddressSanitizer build of sim and SimBridge also runs the scenario clean.
    * **Workaround (`battle.gd`):** the HUD is a `Label3D` 1 m in front of the camera, sized so one font pixel is one screen pixel. With no CanvasItem visible, the crash doesn't happen. `--hud2d` brings back the 2D `Label` to reproduce it.
    * 160-regiment `--measure` runs:

      | HUD | Crashed |
      |---|---|
      | 2D Label, text set every frame | 3 of 5 |
      | 2D Label, text set at most 4 times a second | 5 of 5 with the laptop otherwise idle (each ~36 s after spawn); 3 of 17 in earlier runs, some with another Godot running |
      | Hidden (F4 or `--no-hud`) | 0 of 10 |
      | Label3D (now the default) | 0 of 10 |

      So throttling the text doesn't help; only having no visible CanvasItem does. The box-select `ColorRect` is still 2D, so a crash during a long drag is possible.
    * Minimal repro (no GDExtension): a 3D scene with 1,060 instanced skinned, animated characters plus a 2D `Label` updated every frame. It crashed in 2 of 15 runs with the same backtrace; boxes instead of characters crashed 0 of 9, and 0 of 3 with the Label hidden. The project and a draft upstream issue are in the session notes (`canvas_crash_repro/`, `godot-canvas-crash-issue-draft.md`); the issue isn't filed yet.
  * **Every config loads in Godot.** The editor and `godot --path` load only the library in the debug slot (`bin/windows/debug/`): Debug or RelWithDebInfo, whichever was linked last (see above). The Release (template_release) DLL also runs the battle view in the editor when copied into that slot: self test 14/14, 160 regiments at 7.6 fps, sim step 3.9 ms. Godot itself loads it only in exported release builds.
  * **TGI load time (Release):** mounting the archives, loading and resolving the TGI and compiling the Defs takes 0.7 s from a cold process (`simcli show-def`; load and resolve alone, 0.46–0.54 s). That's under the 5 s threshold, so there is no binary DefSet cache.
  * Battle view fps (dev laptop, HD 3000 through ANGLE, 1280×720, gen:7:1024):
    * 160 regiments (1,060 units): 9.3 fps with the sim paused (frame 108 ms; M4 close-out).
    * 6 regiments (40 units): 13.1 fps (frame 76 ms; M4 close-out).
    * Rendering the scene, not the units, dominates on this GPU; each unit adds about 0.03 ms.
    * 160 regiments with the sim running, optimized (RelWithDebInfo) DLL: 7.5 fps (frame 133 ms, p95 144–147 ms). The sim takes 3.5–4.4 ms per tick (max 6–11 ms), so rendering bounds the frame.
    * Caution: `--measure` times frames with `_process` `delta`, which Godot clamps to about 8 physics ticks (133 ms), so it can't report less than ~7.5 fps. The real rate is lower: 360 frames took 58–60 s (about 6 fps) in the Label3D runs. In a GDScript-only test scene, `delta` said 130 ms while frames took 1.1 s.
    * The Debug DLL also gives 7.5 fps, but its sim takes about 146 ms per tick, so the sim falls behind real time.
    * This laptop is far below the §7 target PC.
  * Loading the TGI data and the gen:7:1024 map (`SimBridge.start`) takes 3.0 s on the dev laptop with the optimized GDExtension, 29 s with the Debug one.
  * The dev laptop's GPU (HD 3000) has no Vulkan or D3D12, so Godot runs the GL Compatibility renderer through ANGLE. It ran the battle view at about 8 fps with 70 units before the graphics presets; see **Rendering performance** below.

**Rendering performance (dev laptop: i5-2520M, Intel HD 3000 through ANGLE, GL Compatibility, 1280×720, V-Sync off).**

`godot --path godot -- --profile=FRAMES [--quality=Q] [--regiments=N]` times wall-clock frames after a 90-frame warm-up and splits them into our script work (sim step, snapshot, view sync, selection, overlays, HUD), Godot's process and render CPU time, and draw calls and primitives. Switches remove one cost at a time: `--no-anim`, `--hide-units`, `--no-shadows`, `--plain-terrain`. The GPU timer reads 0 on ANGLE, so GPU cost shows up as the CPU waiting, mostly inside the instance-buffer upload ("view sync").

*Before* (per-unit nodes, i.e. today's High), 71-unit demo on Golden Fields:

| Run | Frame | fps | Render CPU | Draw calls | Primitives |
|---|---|---|---|---|---|
| Baseline | 120.9 ms | 8.3 | 111 ms | 790 | 230k |
| No AnimationPlayers | 56.0 ms | 17.9 | 48 ms | 789 | 230k |
| Units hidden | 50.5 ms | 19.8 | 1.6 ms | 8 | 164k |
| Shadows off | 77.3 ms | 12.9 | 63 ms | 230 | 52k |
| Flat terrain material | 110.6 ms | 9.0 | 100 ms | 790 | 230k |

Where the time went: sim + GDScript about 2 ms (sim step 0.16, snapshot 0.12, view sync 1.5); skeletal animation about 45–50 ms of CPU (AnimationPlayers and skeletons, even with the units hidden); unit draw calls about 60 ms (11 mesh parts per unit, and shadows draw them all again); the terrain shader about 10 ms.

*After:*

| Scene | Low | Medium | High |
|---|---|---|---|
| 71-unit demo, Golden Fields | **13.2 ms, 75.6 fps** (35 draws) | 38.9 ms, 25.7 fps | 120.2 ms, 8.3 fps |
| 499 units (`--regiments=75`), Golden Fields | **24.3 ms, 41.2 fps** (p95 25.4 ms, 41 draws, 243k primitives) | 59.5 ms, 16.8 fps | 436 ms, 2.3 fps (4,755 draws) |
| 1,060 units (`--regiments=160`), gen:7:1024 | 46.3 ms, 21.6 fps (p95 47.9 ms, 974k primitives; sim 1.5 ms a frame) | 87.7 ms, 11.4 fps | not measured |

500 units on Low, step by step: MultiMesh + VAT with two RGBA32F texels per vertex 43.7 ms → position and normal in one texel, animation LOD and vertex lighting 34.3 ms → rotating the instance buffers 24.3 ms. With the units hidden the frame is 11.7 ms (terrain, HUD, sim), so the 500 units cost about 12 ms. The sim takes 0.4–0.7 ms per frame (about 4 ms per tick).


### 4.5 Controllers
* `LocalPlayerController`: input → Commands (hotkeys from `Localization/Hotkeys/*.txt`).
* `AIController` ([AI.md](AI.md)): reads the sim through the same read-only API, filtered by fog for its team, and emits Commands. It runs on the host.
* `RemotePeer`: commands arriving over the network.

### 4.6 Session
* `LockstepScheduler` ([NETCODE.md](NETCODE.md)): gathers commands for tick T+delay, advances only when tick T's bundle is complete, and stays in step with real time.
* `ReplayRecorder` / `ReplayPlayer`: header (versions, data hash, seed, settings, players) plus the command stream.
* `SaveGame`: full sim serialization (canonical byte order, stable ordering). Saving and loading must reproduce the identical state hash.
* `DesyncDetector`: hash exchange, and dumps state on mismatch.

---

## 5. Repository layout

```
Remake/                         ← this repo (move it out of the game folder whenever you like)
  CLAUDE.md                     ← rules for Claude Code sessions
  README.md
  docs/                         ← these design docs
  sim/                          ← C++20, NO Godot headers allowed
    math/  core/  data/  vfs/  ecs/  systems/  nav/  session/
    tests/
  simcli/                       ← headless runner: replay, hash, soak, defs dump
  gdext/                        ← godot-cpp bindings (SimBridge)
  godot/                        ← Godot project
    project.godot
    scenes/  scripts/  ui/  fxc/  ai/  net/
  tools/                        ← Python: rwd.py, nif2gltf.py, import_game.py, tgi_dump.py
  cache/                        ← GENERATED from the player's game install, gitignored
  config/game_path.cfg          ← where the original install lives, gitignored
```

---

## 6. Cross-cutting rules (also in CLAUDE.md)

1. **No floats in `sim/`.** CI greps for `float`/`double` in `sim/` and fails if it finds any (outside `// view-only` debug helpers that are compiled out).
2. **No Godot headers in `sim/`.** The sim must build and run in `simcli` alone.
3. **No unordered iteration** in `sim/`. Use `std::vector` plus sorted ids, or a deterministic open-addressing map with insertion-order iteration.
4. **No wall clock, threads or `rand()` in `sim/`.** Parallelism is allowed only for work that writes to disjoint, pre-allocated slots and is reduced in a fixed order (not before M8).
5. **Never commit game assets.** `.gitignore` blocks `*.rwd *.nif *.kf *.dds *.tga *.bik` and `cache/`.
6. Every new sim feature needs a **determinism test**: run a recorded replay twice (and in Debug vs Release) and compare per-tick hashes.

---

## 7. Performance budget (target PC: mid-range 2020 laptop)

| Item | Budget |
|---|---|
| Worst case | 8 kingdoms × 20 regiments × up to 8 units ≈ **1,300 units** plus about 300 buildings and projectiles |
| Sim tick (10 Hz) | ≤ **15 ms** at the 99th percentile on one core (≈15% of the tick period). M4: 10.7 ms p99 with 160 regiments on a 1024² map (PATHFINDING §10) |
| Pathfinding | ≤ 3 ms per tick budgeted, the rest amortized |
| Render | 60 fps at 1080p. Units are MultiMesh-instanced per type with baked vertex animation on Low and Medium (§4.4); the dev laptop runs 500 units at 41 fps on Low at 720p. |
| Memory | < 2 GB |

---

## 8. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Exact combat formulas are inside `AA.exe`, not in data | Build a **calibration harness**: set up identical 1v1 regiment fights in the original (editor maps) and in ours, measure time-to-kill and survivors, and tune the formula until they match within tolerance. See GAME_DESIGN §6. |
| Map binary format not yet identified (only string tables extracted so far) | Start with **RMC-driven random maps** (the RMC data is plain TGI). Reverse the map format later (M10). |
| Skinned biped animation conversion | pyffi already parses `NiSkinInstance`/`NiSkinData` and KF. Convert the Bip01 skeleton to a glTF skin, with one clip per KF. |
| Determinism bugs | Fixed-point everywhere, hash every 10 ticks in debug builds, and `simcli bisect` to find the first divergent tick and system. |
| Scope creep (WWII metagame, campaigns, naval, air) | Vertical slice first ([MILESTONES.md](MILESTONES.md)). Everything else is data plus incremental systems. |
