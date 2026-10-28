# Session B: M8 Multiplayer, then M10 map objects

Read first: `CLAUDE.md`, `prompts/SPRINT_RULES.md` (binding), `docs/NETCODE.md`, `docs/MILESTONES.md` (Progress), `docs/ARCHITECTURE.md` §4.6, `docs/ASSET_PIPELINE.md` §2.1.

You work in the git worktree `../Remake-net` on branch `m8-net` (the user created it). Check that `config/game_path.cfg` exists and that `cache/` resolves (a junction to the main folder's cache). If either is missing, recreate them from the main folder before anything else. Build Release + RelWithDebInfo into this worktree's own build folder and run all tests first.

Work through the backlog in order. Each task = implement, test, update docs, commit, push. Check the time before every task.

## Backlog

**B1. Lockstep scheduler (sim-side, transport-free).** Per NETCODE §2–3: commands scheduled for tick T+D, a tick advances only when the bundle for T exists, `SetInputDelay` as a command effective at a future tick, `PlayerLeft(player, tick)`. Implement against an in-memory loopback transport first, with tests for: out-of-order arrival, a stalled peer, a delay change, a player leaving.

**B2. Packet codec.** `CmdPacket`, `TickBundle`, hash reports, control messages per NETCODE §4 (little-endian, varints), with fuzz-style round-trip tests and a per-player per-tick command rate limit.

**B3. ENet transport.** Godot low-level `ENetConnection` (channels: reliable ordered for commands/control, unreliable for ping, reliable for chat). Host-relayed star. RTT/jitter measurement and adaptive D (2–8). Two Godot instances on one PC must play a match over 127.0.0.1.

**B4. Desync tooling.** Hash exchange every 10 ticks, `DesyncDetected` broadcast, full state dumps from every peer, `simcli diff a.state b.state` (first differing entity/field) and `simcli bisect replay.rpl` (first divergent tick and stage). Test it by injecting a deliberate desync behind a debug flag.

**B5. Lobby.** Host/join by IP, player list (nation, general, team, colour, AI slot), map and settings, `MatchConfig` hash agreement, version and **dataHash** check (refuse peers with different mods), chat. Host-generated match seed.

**B6. Stalls, drops, speed.** "Waiting for player…" after 3 s, drop after a timeout, a dropped player's kingdom handed to an AI slot (use the scripted dummy if Session A's AIController isn't merged yet, behind an interface so the real AI plugs in later), catch-up ticks for a peer that fell behind, host `SetSpeed` and pause.

**B7. Replays ("Films").** Always record `LastFilm.rpl` per NETCODE §8 with keyframe snapshots every 60 s for seeking. A Films menu: list, play, pause, 1–8× speed, seek, free camera and per-player view.

**B8. Soak test.** Two (or more) local instances, scripted inputs, 30 game-minutes at max speed with zero desyncs, run under Debug on one side and RelWithDebInfo on the other. Record results in NETCODE.md. Fix every desync found (B4 tools). Update MILESTONES.md, tag `v0.8` on the branch.

## If you finish everything (M10 map objects, in this order)
1. Decode `DYNAMIC_THINGS` and `DYNAMIC_THINGS_INDEX` in `tools/rmp.py`: placed objects (forests, buildings, villages, bridges, roads) by IDS with position and rotation, and stamp their blocking/cost into the nav grid (integer float32→Fixed conversion, never the FPU in `sim/`).
2. Decode `KINGDOMS`, `TEAMS` and `OBJECTIVES` enough for start positions and skirmish setup on all 6 boxed skirmish maps and the developer maps.
3. Load every boxed skirmish map in the setup screen with its original objects rendered from `cache/`.
4. Roads: speed and cost bonus from the road objects/terrain.
5. A full RMC random-map generator from `RandomMap/*.tgi` (terrain blobs, streaks, noise, villages, start corps), deterministic from the seed.
6. The remaining unknowns in ASSET_PIPELINE §2.1 (v7 flags, per-vertex byte, the TERRAIN trailer): document anything you learn.
