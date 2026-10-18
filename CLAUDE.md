# CLAUDE.md: Project Corps (working name)

A reimplementation of TimeGate's 2004 regiment-based WWII RTS engine. It **reads the original game's data from the player's own install** (Bring Your Own Data). Godot 4.x for presentation, a **deterministic fixed-point C++ sim** for gameplay, and lockstep multiplayer.

## Read before working
- `docs/ARCHITECTURE.md`: module boundaries, tick order, component model
- `docs/GAME_DESIGN.md`: rules (TGI data is the source of truth for numbers)
- `docs/TGI_FORMAT.md`: TGI grammar, load order, template resolution rules (M1)
- `docs/PATHFINDING.md`, `docs/AI.md`, `docs/NETCODE.md`, `docs/ASSET_PIPELINE.md`
- `docs/MILESTONES.md`: what we're building now

## Hard rules
1. **Never commit or upload game assets** (`*.rwd *.nif *.kf *.dds *.tga *.bik`, anything in `cache/`). Code and tools only.
2. **`sim/` is deterministic:**
   - no `float`/`double` (use `Fixed` Q32.32, `FVec2`, trig tables)
   - no Godot headers, no wall clock, no threads, no `rand()`; only the sim `Rng`
   - no iteration over unordered containers; iterate by ascending `EntityId`
   - every state change goes through a **Command** applied at a scheduled tick
3. Sim changes need tests. From M3 on, also run `simcli determinism-check`.
4. The view never mutates sim state. It reads snapshots and events only.
5. The AI is a host-side controller that emits Commands. It is **not** part of the sim.
6. When behaviour or design changes, update the matching `docs/*.md` in the same change.

## Original game install
Path from `config/game_path.cfg` (gitignored). Example: `C:/Users/lenovo/Desktop/Axis & Allies`.
Depot order (later wins): `Data.rwd` → `data/` → `skins/*.rwd` → `Music.rwd` → `Speech.rwd` → user overlay.
`data/` contains the user's PER/SAU nation mods. They must keep loading.

## Layout
`sim/` C++20 core · `simcli/` headless runner · `gdext/` Godot bindings · `godot/` Godot project · `tools/` Python importers · `cache/` generated (gitignored)

## Conventions
- C++: `snake_case` files, `PascalCase` types, `camelCase` functions, `m_` not used; prefer `struct` + free functions in systems
- GDScript: Godot style guide, typed GDScript
- Python tools: 3.11+, run with `-I`, type hints, no network access needed
- Commits: imperative mood, one system per commit
