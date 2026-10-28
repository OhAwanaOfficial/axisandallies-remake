# Sprint decisions (one line each: date, session, decision, why)

* 2026-10-28 A: CI on v0.6 (e42a735) failed on linux-gcc (Build Debug) and windows-clang-cl (Test Debug); MSVC and linux-clang passed. The logs need a GitHub sign-in, so the cause isn't known yet; not fixed in the sprint. Next step: open the two failed jobs signed in and fix what they show.
* 2026-10-28 A: the AI computes its team's vision itself (ai/view, read-only, from unit/regiment detection_range) because the sim's Vision stage is still empty until M9 fog; switch the view to the sim's grid then.
* 2026-10-28 A: buildings see 40 m in the AI view: BuildingDef has no detection range yet, and adding one changes the DefSet layout and data hash mid-sprint.
* 2026-10-28 A: cities are always visible to the AI with their owner, matching the default EXPLORED fog (GAME_DESIGN §10).
* 2026-10-28 A: the M6 dummy and slice player keep reading the World directly (omniscient, documented in AI.md §7a); only the new M7 AI uses the fog-filtered View.
