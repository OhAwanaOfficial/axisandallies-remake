# Project Corps (working name)

A modern reimplementation of the 2004 TimeGate/Atari regiment-based WWII RTS. It uses Godot 4 for presentation, a deterministic C++ simulation, and lockstep multiplayer, and it **runs on the data from your own copy of the original game**.

> This repo contains no game assets. You need an installed copy of the original game, and the import tool converts its files locally.

## Docs
| Doc | What's in it |
|---|---|
| [ARCHITECTURE](docs/ARCHITECTURE.md) | Modules, sim/view split, ECS components mirroring the original, tick order, tech stack |
| [GAME_DESIGN](docs/GAME_DESIGN.md) | Rules: economy, regiments, formations, morale, supply, bases, generals, modes |
| [PATHFINDING](docs/PATHFINDING.md) | Nav grid, HPA*, regiment formations, divisions, avoidance, determinism |
| [AI](docs/AI.md) | Host-side AI controllers: egos, managers, influence maps, task forces |
| [NETCODE](docs/NETCODE.md) | Lockstep, host-relayed star, input delay, desync tooling, replays |
| [ASSET_PIPELINE](docs/ASSET_PIPELINE.md) | RWD/NIF/KF/DDS findings, import pipeline, legal model |
| [TGI_FORMAT](docs/TGI_FORMAT.md) | TGI grammar, `index*.lst` load order, template inheritance and list-merge rules |
| [MILESTONES](docs/MILESTONES.md) | M0 → M12 with acceptance criteria |

## Build and test (Windows)
You need Visual Studio 2022 Build Tools (C++ workload), CMake 3.22+, Git, Python 3.11+ and Godot 4.3+.
Point the tools at your install by creating `config/game_path.cfg` (gitignored) with one line, e.g.
`C:/Users/you/Desktop/Axis & Allies`.

```bash
cmake --preset windows
cmake --build --preset windows-debug
ctest --preset windows-debug
build\windows\simcli\Debug\simcli.exe dump-defs --explain per_tank_medium
```
`ctest` runs the sim unit tests, the golden tests against your install (skipped without
`config/game_path.cfg`), the no-float-in-`sim/` check and the Python tool tests. For a quicker loop without the
GDExtension, use the `windows-sim-only` / `windows-sim-debug` presets. The first configure downloads doctest and
godot-cpp, and the godot-cpp bindings take several minutes to build.

The build puts the GDExtension in `godot/bin/`. On a fresh checkout, run `godot --headless --path godot --import`
once so Godot registers it; `godot --path godot res://scenes/main.tscn` then shows `SimBridge.hello()`.

Which build the editor runs: the editor and `godot --path` load `godot/bin/windows/debug/simbridge.dll`, a
template_debug GDExtension. Two configurations write it, and the last one built wins:
* `windows-debug`: unoptimized, for stepping through the bridge. The sim is about 40x slower.
* `windows-relwithdebinfo`: optimized, with symbols. Use this to play or measure the battle view. Its first build
  compiles godot-cpp once more (about 40 min on the dev laptop). It also regenerates the godot-cpp bindings, so the
  next Debug and Release builds each recompile godot-cpp once.

`windows-release` writes a template_release GDExtension to `godot/bin/windows/release/`, for exported games. The
editor binary doesn't load it.

## Run the sim headless (M3)
```bash
build\windows\simcli\Debug\simcli.exe show-def regiment_per_infantry
build\windows\simcli\Debug\simcli.exe spawn-test --regiment regiment_per_infantry --ticks 100
build\windows\simcli\Debug\simcli.exe record --out x.rpl --ticks 600 --commands-per-tick 4
build\windows\simcli\Debug\simcli.exe determinism-check x.rpl --per-stage
```
`spawn-test` compiles the TGI data into typed Defs, spawns a regiment from them, runs 100 ticks and prints the state hash. It's identical in Debug and Release. `determinism-check` replays a film twice (per tick, or per stage with `--per-stage`) and against the hashes stored in the file.

## Pathfinding bench (M4)
```bash
build\windows\simcli\Release\simcli.exe nav-bench --gen 7 --size 256 --queries 500 --optimal
python -I tools/rmp.py "<game>/Data.rwd" "Maps/DeveloperMaps/(1v1)Golden Fields.RMP" --out cache/maps/golden_fields
build\windows\simcli\Release\simcli.exe nav-bench --map cache/maps/golden_fields/map.json --optimal
build\windows\simcli\Release\simcli.exe record --out nav.rpl --gen 7 --size 128 --ticks 300 --commands-per-tick 4
build\windows\simcli\Release\simcli.exe determinism-check nav.rpl --per-stage
```
`nav-bench` runs random start/goal pairs per movement class through the budgeted PathService and reports expansions (avg, p50, p99), time, fallbacks and a hash of every path; `--optimal` also compares each path's cost with a full-grid A* (fails past 1.05x mean / 1.25x worst). `--map` takes `tools/rmp.py` output; `--gen SEED --size N` makes a deterministic test map. Replays recorded on a map regenerate it (`gen:` maps) or need the same `--map` file.

## Regiment movement scenario (M4)
```bash
build\windows\simcli\Release\simcli.exe scenario --out scenario.rpl
build\windows\simcli\Debug\simcli.exe determinism-check scenario.rpl --per-stage
build\windows\simcli\Release\simcli.exe show-def regiment_infantry
```
`scenario` spawns six mixed regiments (infantry, tanks, a halftrack) from the game data on the generated test map and moves them east: a division, a loose group, a Run, a formation change, a Stop and re-order, and a building dropped on the route. It prints when each regiment arrived and the state hash, and records the film. `show-def` also prints layouts, formations and group formations.

## Play the battle view (M4)
```bash
cmake --build --preset windows-relwithdebinfo --target simbridge
godot --path godot
godot --path godot -- --gen=7:256
godot --path godot -- --map=../cache/maps/kursk/map.json --demo
```
The main scene loads the game data and a map (Golden Fields from `cache/maps/` when present, else the generated
test map), spawns six of your regiments and three enemy ones, and runs the sim at 10 ticks per second.
* Select with a click, a box or a double-click (same type). Ctrl+1..9 stores a selection and 1..9 recalls it.
* Right click moves, right drag also sets the facing, Alt+right click runs. Q/W/E set the formation, V toggles a
  division, S stops.
* The camera pans with the arrows or the screen edges, zooms with the wheel and rotates with the middle button.
* F1/F2/F3 toggle the cost-grid, path and slot overlays.

Needs `tools/import_game.py` output in `cache/` for models and terrain textures; without it, units are boxes and
terrain uses the data colours. Loading takes about 3 s with the optimized GDExtension, and up to 30 s with the Debug one.

## Performance check (M4)
```bash
build\windows\simcli\Release\simcli.exe perf --ticks 3000 --out perf.rpl
godot --path godot -- --gen=7:1024 --regiments=160 --measure=300
```
`perf` puts 160 regiments on the largest generated map (1024²), sends them across in groups and divisions, drops a
building every 100 ticks, and prints tick times (avg, p50, p95, p99, max), the cost of building ticks and the time per
stage. `--measure` makes the battle view print its fps (add `--pause-sim` to measure rendering alone). Measure with the
`windows-relwithdebinfo` GDExtension. On the dev laptop, Godot 4.7.2 sometimes crashes in its 2D canvas renderer
during this run (ARCHITECTURE §4.4).

## Import the original art and browse it (M2)
```bash
python -m venv .venv
.venv/Scripts/python -m pip install -r tools/requirements.txt
.venv/Scripts/python -I tools/import_game.py
godot --path godot res://scenes/tools/model_browser.tscn
```
The importer reads the install named in `config/game_path.cfg` and writes `cache/`: `.glb` models,
`.anim.glb` clips, copied textures and `manifest.json`. Re-runs only convert changed files; use `--only REGEX`
to import a subset. The Model Browser loads `cache/` at runtime. Never commit or upload `cache/`.
