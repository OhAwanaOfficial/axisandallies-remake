# Final sprint: shared rules for both sessions

Two Claude Code sessions run **in parallel** until the subscription ends:
* **Session A** (`prompts/SESSION_A.md`): M7 Skirmish AI, then M9 gameplay content. Main folder, branch `m7-ai`.
* **Session B** (`prompts/SESSION_B.md`): M8 Multiplayer, then M10 map objects. Worktree `../Remake-net`, branch `m8-net`.
* At the end, Session A merges both branches (`prompts/MERGE.md`).

## Time box
* **STOP_NEW_WORK_AT = 14:15 local time (Asia/Karachi, UTC+5).** The user may edit this line.
* Run `date` (or `Get-Date` in PowerShell) **before starting every task**. If it's at or past STOP_NEW_WORK_AT, don't start a new task. Finish or revert the one in progress, commit, push your branch and print a final report.
* Don't stop early for any other reason. When a task is done, go straight to the next one in your backlog without asking. When the backlog is empty, continue with the "If you finish everything" list.

## Working autonomously
* **Don't ask the user questions.** The user wants all the remaining usage turned into finished, committed work. When a choice is ambiguous, pick the option that best fits the design docs. Record it as one line in `docs/DECISIONS.md` (date, session, decision, why), then keep going.
* Use plan-then-implement inside your own head. Don't wait for plan approval.
* You may use subagents for wide searches or independent subtasks if it helps, but never let two agents edit the same file.

## Never lose work
* **Commit after every finished task** (tests green, determinism-check green). Use a small commit, an imperative message and the attribution trailer from CLAUDE.md/system settings. Push the branch after every commit (`git push -u origin <branch>`). If there's no remote, just commit.
* Never commit with a broken build or failing tests. If a task can't be finished, revert its partial changes (`git stash` or `git checkout -- .`), write what blocked it in `docs/DECISIONS.md`, commit that note, and move to the next task.
* The usage limit can hit at any moment, so keep each task under about 45 minutes of work. Split bigger ones.

## Quality bar (from CLAUDE.md, still binding)
* `sim/` stays deterministic: no floats, no Godot headers, no clocks, threads or unordered iteration. Every sim change gets tests plus `simcli determinism-check` on the committed fixtures. Re-record golden hashes only when the change intends to alter behaviour, and say so in the commit message.
* Never commit game assets or anything from `cache/`.
* Keep the Low graphics preset at ≥ 30 fps on the dev laptop (i5-2520M, HD 3000) for any view change. Measure with the existing `--speed`/`--warmup` options and report the number.
* Update the matching `docs/*.md` and the Progress table in `docs/MILESTONES.md` in the same commit as the feature.

## Avoiding merge conflicts between A and B
* **Session A owns:** `godot/ai/`, `godot/scripts/` HUD/game files it needs, `sim/systems/` gameplay files, `sim/data/`, `docs/AI.md`, `docs/GAME_DESIGN.md`.
* **Session B owns:** `sim/session/`, a new `sim/net/` or `godot/net/`, `gdext/` networking parts, lobby/menu scenes, `tools/rmp.py`, `sim/nav/map_def*`, `docs/NETCODE.md`, `docs/ASSET_PIPELINE.md`.
* **Shared files** (`sim/core/command.*`, `gdext/sim_bridge.*`, `CMakeLists.txt`, `docs/MILESTONES.md`, `docs/ARCHITECTURE.md`): **append only**. Add new functions, commands and sections at the end, never reorder or reformat existing code. Session A uses command codes 300–399 for anything new, Session B uses 400–499.

## Final report (print it, and also append it to docs/DECISIONS.md)
What got done (commits), what's half done or blocked and why, the test/determinism status, measured numbers, and the exact next step for each unfinished item.
