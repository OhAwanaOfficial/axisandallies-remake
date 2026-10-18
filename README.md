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
once so Godot registers it; `godot --path godot` then shows `SimBridge.hello()`.

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
