# Session A: M7 Skirmish AI, then M9 gameplay content

Read first: `CLAUDE.md`, `prompts/SPRINT_RULES.md` (binding), `docs/AI.md` (§7a: what exists), `docs/MILESTONES.md` (Progress, M6 close-out), `docs/GAME_DESIGN.md`, `docs/ARCHITECTURE.md` §4.5.

Where the AI lives: since v0.6 it's C++ in `ai/` (`ai/controller.{hpp,cpp}`, library `corps_ai`, linked by `simcli` and the GDExtension), not GDScript. `RuleAi` with a `Plan` is the scripted dummy and the slice player; `SimBridge.set_controller(player, kind, nation)` attaches one, and it thinks before each `step()`. Build the M7 AIController there too (a new controller class next to `RuleAi`, or new files in `ai/`), so `simcli` can run AI-vs-AI headless and determinism stays checkable. `simcli slice` and the Godot `--slice-test` / `--defeat-test` are the existing play-tests: keep them green.

Setup (once): `git switch -c m7-ai` in the main project folder (or switch to it if it exists). Build Release + RelWithDebInfo, run all tests, and confirm the vertical slice still runs before changing anything.

Work through the backlog in order. Each task = implement, test, update docs, commit, push. Check the time before every task.

## Backlog

**A1. AI read API (fog-filtered).** A read-only query layer the AI uses: own kingdom (resources, income, limits, buildings, regiments, research, army XP), visible enemies only (per the team's vision), last-seen memory with timestamps, cities and their owners, map passability per class, and supply coverage. Expose it through SimBridge, plus a headless equivalent in `simcli` so AI-vs-AI can run with no rendering. AI must never read hidden state; add a test that a hidden enemy is invisible to the API.

**A2. AIController skeleton.** Host-side controller per AI.md §2: runs on its own staggered intervals (strategic 2 s, operational 1 s, tactical 0.5 s at Medium), emits ordinary Commands, has its own seeded RNG and an APM cap. Can be attached to any kingdom in the skirmish setup screen (replace the scripted dummy as the default opponent, and keep the dummy available as "Scripted").

**A3. Managers.** Economy manager (keep oil/ammo net ≥ 0, plan depots and unit-limit buildings ahead of production, respect the building limit), Production manager (utility formula from AI.md §3.2 with a counter table derived from damage types vs properties), Base layout manager (StructureLayouts / layout_*_aa templates, same placement check as the player), Research manager.

**A4. Egos.** Opening/LowIncome → MainGame → EndGame plus the Defend overlay, with thresholds in a personality resource. Load what maps cleanly from the original SAI TGIs (initial ego, egos, factions/generals, difficulty).

**A5. Operational layer.** Influence maps (threat with last-seen decay, own strength, supply coverage, objective value), objective scoring, task forces (1–6 regiments, division for attack, group for defense), lifecycle Assemble → Move (Column) → Stage (switch to Assault outside enemy ZOC, wait for efficiency) → Engage → Consolidate/Retreat, and supply-line planning (depot or repack HQ toward the objective).

**A6. Tactical layer.** Engage/avoid by combat value ratio, retreat-before-rout (units < 40% or morale near break), focus fire on CO/artillery/AA when worth it, terrain preference (infantry to forest/jungle).

**A7. Personalities and difficulty.** The 8 personalities from AI.md §5 and Easy/Medium/Hard/Expert per AI.md §4 (intervals, APM cap, micro toggles, memory decay, optional handicap). Lobby dropdowns.

**A8. Debug overlay.** Per-AI toggle: influence maps, objectives with scores, task forces with state and target, last 20 decisions with reason strings.

**A9. Headless soak + regression gate.** `simcli soak --ai A --ai B --map ... --games N --seed S` (model it on `simcli slice`): win rates per personality, game length, average income, "stuck" incidents (task force with no progress for 60 s). Run at least 20 games on Golden Fields and the generated test map, fix the worst stuck cases, record the table in AI.md. Gate: the new AI must beat the scripted dummy ≥ 90% of games.

**A10. M7 close-out.** Tune until Medium AI wins against the dummy and loses to a sensible scripted "good player" build. Update MILESTONES.md, tag `v0.7` on the branch.

## If you finish everything (M9 content, in this order)
1. Engineers: construction of bunkers (MG/AA/Artillery) and repair of buildings, with the command panel buttons. The AI's engineer manager uses them.
2. Generals and special ops: the 4 per general from `SpecialOps/*.tgi` and `Generals/<NAT>/*.tgi`, army XP costs, cooldown bars, Y/H/N/J hotkeys, the special-ops bar. AI special-ops manager.
3. Airfield: stocked fighters/bombers, Air Recon and Air Strike missions (paid per use), fighters auto-defend; aircraft steering per PATHFINDING §4.2.
4. All five nations plus PER/SAU playable in the setup screen, with nation-exclusive regiments and per-nation stats; a golden test per nation.
5. Fog modes NONE / EXPLORED / FULL (black fog) with per-team vision shared between allies.
6. Team commands: flares, Report, tribute.
7. Naval units and amphibious transports (only if 1–6 are done).
