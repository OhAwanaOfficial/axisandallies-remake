# Final merge (run in Session A at about 14:20, after both sessions have stopped)

1. In the main folder: commit anything left on `m7-ai`. Check that `m8-net` has its last commit (it's in the shared repo through the worktree).
2. `git switch main` (or the default branch), then `git merge --no-ff m7-ai`, then `git merge --no-ff m8-net`.
3. Resolve conflicts. Shared files were append-only, so keep both sides. Command codes: A used 300–399, B used 400–499.
4. Wire the real AIController into B's "dropped player → AI" interface if it isn't already.
5. Rebuild Release + RelWithDebInfo, run every test, run determinism-check on every fixture, and run the Godot `--selftest`. Re-record a golden hash only when you can explain the behaviour change in the commit message.
6. Update `docs/MILESTONES.md` Progress with the final state of M7, M8, M9 and M10, and append both final reports to `docs/DECISIONS.md`.
7. Commit, tag `v0.8-sprint`, push. Print a short summary: what works now, what's half done, and the next step for each.
8. If the merge can't be made green before the usage runs out, leave `main` untouched and push both branches. Write the blocking issue at the top of `docs/DECISIONS.md`.
