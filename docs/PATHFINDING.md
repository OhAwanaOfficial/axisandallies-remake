# Pathfinding & Movement Design

Scope: how regiments, units, groups/divisions, trucks with trailers, ships and aircraft get from A to B **deterministically** inside the C++ sim. Everything here runs in `sim/nav` and `sim/systems/movement`. All math is fixed-point, and all tie-breaks are by integer id or cell index.

---

## 1. Requirements

| Requirement | Why |
|---|---|
| **Deterministic** across machines, compilers and Debug/Release | Lockstep multiplayer and replays |
| **Regiment-centric**: one path per regiment, not per soldier | The original commands regiments of 2–8 units in formations |
| Formation-aware movement (slot offsets per layout, wheel on turns, compress in chokepoints) | Readability and faithfulness |
| Groups/divisions move at the slowest member's speed and arrive together | `GroupSpeedMinimum`, `GroupSupplyZoneBuffer` **[DATA SVars]** |
| Dynamic obstacles: buildings that **pack and unpack**, bunkers, destroyed bridges | Mobile bases are core |
| Terrain costs per movement class (jungle and forest slow, mountains and ocean block ground) | Terrain matters |
| Budget: ≤ 3 ms per tick worst case, ~1,300 units, up to 160 regiments | Performance target |
| Graceful unreachable handling (move to nearest reachable point) | UX |

---

## 2. World representation

* **Sim plane:** 2D (x, y) in **world units** (the tank model is ≈3.3 units long and rifles range 12, so 1 unit ≈ 1–2 m). Height is view-only.
* **Nav grid:** square cells of **1.0 world unit**. A 512×512-unit map is 262k cells, about 1 byte per cell per layer, which is fine.
* **Movement classes** (bitmask), one per regiment. A regiment uses its **most restrictive member's** class (e.g. mechanized infantry with halftracks → Wheeled):

| Class | Members | Notes |
|---|---|---|
| `Foot` | infantry, engineers, paratroopers | Can enter forest and jungle (slow) |
| `Wheeled` | trucks, halftracks, armoured cars | Faster on roads, can't enter dense forest (tunable) |
| `Tracked` | tanks, SPA | Like wheeled but crosses rough terrain better |
| `Naval` | battleships, carriers, transports | Water cells only |
| `Air` | fighters, bombers, air transport | **No grid pathing**: steering with a turn radius over everything |
| `Amphibious` | landing transports | Water plus beach cells, unloads at shore |

### 2.1 Layers (rebuilt or patched incrementally)
1. **Static terrain cost** per class: `uint8 cost[class][cell]` (0 = blocked, 1–254 = cost multiplier ×16). Built at map load from the terrain-type map and doodads (trees and rocks marked blocking in their TGI `Size`).
2. **Static structures**: map-placed buildings and city structures stamp their footprint (`square_size`, `blocking_shape`).
3. **Dynamic structures**: player buildings when unpacked, bunkers when constructed. **Unstamped when packed or destroyed.** Every stamp or unstamp marks the touched **clusters dirty** (see §3).
4. **Units are not in the grid.** They're handled by local avoidance (§6). That keeps the grid stable and the paths cheap.

Roads: a per-cell "road" flag gives a speed multiplier and a cost bonus for Wheeled/Tracked (data-driven per terrain).

---

## 3. Global search: Hierarchical A* (HPA*)

* The grid is split into **clusters of 16×16 cells**. For each class, **entrance nodes** sit on cluster borders, and **intra-cluster edges** are precomputed with small Dijkstra runs, all integer costs.
* **Connected-component labels** per class (flood fill) give an **O(1) reachability check** before searching. If the goal isn't reachable, replace it with the nearest reachable cell found by a spiral search around the goal, tie-broken by cell index.
* Query flow:
  1. Insert the start and goal into the abstract graph (temporary edges to the entrances of their clusters).
  2. **A\*** on the abstract graph, with an octile-distance integer heuristic (straight = 10, diagonal = 14) scaled by the class's minimum cost.
  3. **Refine** lazily: expand only the next 2–3 abstract segments into cell paths, so we don't do the work again when orders change.
  4. **Smooth** with grid line-of-sight string pulling (Bresenham-style supercover, integer) for natural straight lines.
* **Dynamic updates:** when a building stamps or unstamps, rebuild the entrances and intra-edges of the dirty clusters on the **next tick** (deterministic order: ascending cluster index), then bump a `navVersion`. Regiments whose path crosses a dirty cluster revalidate that segment.

**Determinism details**
* The open list is a binary heap keyed on `(f, h, cellIndex)`. The full tuple guarantees an identical pop order everywhere.
* No floats. Costs are `int32`.
* No caches keyed by pointer. Any path cache is keyed by `(class, startCluster, goalCluster, navVersion)` in a deterministic map.

### 3.1 Request budget
* `PathService` keeps a **FIFO queue** ordered by `(requestTick, regimentId)` (implemented as `(requestTick, requester EntityId, request id)`; see §9 for how suspension is done).
* Each tick it spends a fixed **node-expansion budget** (e.g. 20,000 expansions). A search that runs out of budget **suspends** and resumes next tick. Results are applied when the search finishes. That's deterministic because the budget is a count, not a time.
* While waiting, a regiment starts moving **straight toward the goal**, which looks responsive. Once the path arrives it follows the path. (If the straight line is blocked, it simply waits; the delay is a tick or two.)
* **Flow fields** (optional optimization, M9): when ≥ 4 regiments of the same class request the same goal cluster in the same tick, compute one integer flow field over the relevant clusters and share it.

---

## 4. Regiment movement (the important part)

A regiment has an invisible **anchor** (its logical position and heading). Its units follow **formation slots** relative to the anchor.

```
anchor follows path (speed = min member speed × formation speed mult × terrain mult)
for each unit i: slotTarget_i = anchor.pos + rotate(layoutOffset[formation][slot_i], anchor.heading)
unit i steers toward slotTarget_i (arrive behaviour) + local avoidance
```

* **Layouts** come from data: `OrganizationComponent.layout_IDS` (e.g. `regiment_infantry`, `regiment_vehicle_light`) → `Organizations/Layouts/*.tgi` (offset per slot per formation: Assault, Normal, Column, Retreat).
* **Speed:** the anchor never outruns its slowest living unit by more than one slot-length (it slows down to wait), so formations don't stretch apart.
* **Turning:** heading changes by at most `turnRate` per tick. For large turns (> 90°) the formation **wheels**: the anchor pauses briefly while units re-slot. Tanks use their own turn rate.
* **Chokepoints:** before advancing, the anchor samples the corridor width ahead (cached clearance per cell = distance to the nearest blocked cell, precomputed as a distance transform). If the clearance is narrower than the formation width, it **temporarily switches the slot layout to Column**. This doesn't change the combat formation or efficiency, only the slot positions.
* **Arrival:** the regiment is done when the anchor reaches the goal and ≥ 80% of units are within tolerance of their slots, or after a timeout.
* **Facing on arrival:** a right-drag sets an explicit facing. Otherwise the facing is the direction of travel.

### 4.1 Trucks with trailers (packed buildings)
* A packed building is a regiment of truck(s), where each truck tows a trailer (`trailer_actor`, `TrailerHitch` node offset) **[DATA]**.
* Trailer kinematics: the trailer follows the hitch point with a fixed-length bar constraint (integer approximation). It's view-relevant only. The sim footprint is truck plus trailer length, used for clearance checks.
* **Unpack placement** (M6 part 1, GAME_DESIGN §7): the footprint and its padding must be on passable ground for foot and wheeled units, at least `BuildingPaddingMeters` (2) from any other building stamp. `placement_groups` turned out to be editor palettes, not rules. The trucks path to the footprint's centre, then the building's unpack timer runs (7–10 s) and the footprint is stamped at the start of unpacking, so other units route around it. Trailers are view-only for now.

### 4.2 Aircraft
* No grid. Steering with a **minimum turn radius** and cruise speed. Missions are scripted state machines: take off from the airfield or carrier → fly a waypoint path → strike or recon → return → land.
* Fighters on intercept use pursuit with lead prediction (integer).

### 4.3 Ships
* Naval class grid (water cells, with a deeper-water margin for battleships if data requires). Same HPA*, same regiment and formation system, with a large turn radius.
* Battleship shore bombardment range is limited ("can't fire too far inland").

---

## 5. Groups & divisions

* **Group** (loose): each regiment pathfinds independently to `goal + groupFormationOffset[i]` (offsets from `group_formations.tgi`: `group_aggressive`, `group_defensive`, `group_column`). Each one keeps its own speed.
* **Division** (tight, Group Behaviour on):
  * One **division anchor** pathfinds using the most restrictive class among member regiments.
  * Each regiment's anchor targets `divisionAnchor + rotate(groupOffset[slot], heading)`. Assign strong regiments to front slots and weak ones to rear slots (sort by combat value, tie-break by id).
  * Division speed = slowest regiment speed, but never below `GroupSpeedMinimum = 1.0`.
  * **Coordinated arrival** at supply zones: stop `GroupSupplyZoneBuffer = 5` units outside the zone and wait for stragglers, then enter together.
  * Group zone: if a regiment drifts outside `GroupZoneRangeMin = 18` from the anchor, the anchor slows down.

---

## 6. Local avoidance (unit level)

Kept simple and **integer-only**. No floating-point RVO/ORCA.

* **Spatial hash** with 4×4-cell buckets, rebuilt each tick, and units iterated in ascending EntityId.
* Steering = `arrive(slotTarget)` + Σ `separation` from neighbours within `radiusSum + margin`, weighted:
  * same regiment: weak (they can overlap slightly; looks fine at RTS zoom)
  * friendly other regiment: medium, and the **lower-priority one yields** (priority = moving > idle, then by id)
  * enemy: strong, though combat usually stops movement first
  * buildings: hard. Units are pushed out of stamped cells.
* Velocity is clamped to max speed × terrain multiplier at the unit's current cell.
* **Stuck detection:** if a regiment's anchor progress is under 10% of expected for 20 ticks, re-request the path from the current position. After 3 failures, accept the current position (the order completes as "blocked").

---

## 7. Interaction with combat

* **Zone of control (ZOC):** a regiment in an aggressive formation scans its ZOC each `PrimarySampleTime` (and on events). If it finds a valid enemy, the **movement order is suspended** and the engagement controller takes over.
* **Attack orders:** path to the target's position, stop when within `ZOC radius × CAITargetInRangeZOCMultiplier (0.6)`, and re-path if the target moves more than X cells (re-path throttled to once per 10 ticks).
* **Retreat/rout:** pick the **nearest friendly supply zone** (path cost, not straight-line distance) for retreat. A rout moves away from the threat centroid along a cheap path, possibly into danger, matching the original "as likely to run deeper into trouble" behaviour. Use the sim RNG for that choice.
* **Entrenchment** timer resets when the anchor moves more than 1 cell.

---

## 8. Debug & tests

* View overlays (F-key toggles in debug builds): cost grid per class, clusters and entrances, component labels, a regiment's abstract and smoothed path, slot targets, clearance heatmap, path queue length and budget use.
* `simcli nav-bench <map>`: random start/goal pairs, reports average and p99 expansions and time.
* **Golden-path tests:** fixed maps plus queries → hash of the resulting paths must equal a committed value (catches accidental nondeterminism).
* Determinism test: same replay under Debug and Release builds, and MSVC vs Clang → identical per-tick hashes.

---

## 9. Implementation status (M4 part 1: sim only)

Code: `sim/nav/` (`map_def`, `nav_grid`, `grid_search`, `hpa`, `path_service`, `nav_world`), the Pathfinding stage in `sim/systems/stages.cpp`, tests in `sim/tests/test_nav.cpp`, `simcli nav-bench` in `simcli/nav_bench.cpp`. No movement yet: paths are computed and stored, nothing follows them (part 2).

**Maps (`MapDef`).** Immutable and shared like the DefSet, passed in `SimConfig::map`, identified by `mapHash` (xxh3 of its canonical bytes) in states and replays. One cell = 1 world unit; an original 2-unit tile becomes 2×2 cells. Terrain kinds: Ground, Road, Forest, Beach, Shallow, Water, Mountain, Border. Heights are stored as `Fixed` for the view; the sim never reads them. Sources:
* `generateTestMap(seed, size)`: border ring, 1–2 oceans (beach → shallow → deep), 3–5 mountain blobs, 6–10 forest blobs, one road row and one road column. Named `gen:SEED:SIZE`, which is enough to rebuild it.
* `loadRmpJson`: `tools/rmp.py` output (ASSET_PIPELINE §2.1.1). Kind per tile from the class bits (border > mountain > water, "shallow" in the name → Shallow) then name keywords ("trans" → Ground, shore/beach, forest/jungle, road). Original roads are objects, not terrain, so real maps have no Road cells until M10 imports objects.

**Interim cost table** (×16, 0 = blocked; until per-terrain TGI speed modifiers are wired in):

| Kind | Foot | Wheeled | Tracked | Naval | Amphibious |
|---|---|---|---|---|---|
| Ground | 16 | 16 | 16 | 0 | 16 |
| Road | 16 | 12 | 12 | 0 | 16 |
| Forest | 24 | 48 | 32 | 0 | 24 |
| Beach | 16 | 20 | 20 | 0 | 16 |
| Shallow / Water | 0 | 0 | 0 | 16 | 16 |
| Mountain / Border | 0 | 0 | 0 | 0 | 0 |

**Grid.** 8-way moves, no corner cutting (a diagonal needs both orthogonal neighbours passable; labels use the same rule, so "same label" means reachable). Step cost = (10 | 14) × (cost[from] + cost[to]), int32. Heuristic = octile × 2 × the class's minimum cost (admissible with roads). Building stamps are ref-counted per cell and block every class; each stamp or unstamp marks the touched clusters dirty. Labels also keep a bounding box per component. Because a diagonal needs both orthogonal cells open, components are simply 4-connected. **Labels are kept per cluster** (see "Cluster-level labels" below): each cluster has its own local components (one union-find raster pass over its 256 cells), and a union-find over the links where local components touch across cluster borders joins them into the map's components. A cell's label is the label of its local component; a component's box is the union of its local boxes, so it is exact. Components are numbered in order of their first local component (by cluster, then local index), which depends only on the current costs, so an incremental update gives the same labels as a full one. A stamp relabels only the dirty clusters, and only for classes whose effective costs it actually changed.

**HPA\*.** 16×16 clusters. Along each shared border, a run is a maximal stretch of cell pairs with the **same non-zero crossing cost**: one entrance in the middle of a run shorter than 6, one at each end of longer runs. Splitting runs by cost (rather than by passability only) keeps roads and forest edges as their own entrances. Without it, wheeled paths that should follow a road came out up to 1.5× optimal. Nodes are identified by cell index; intra edges come from bounded Dijkstra runs (moves are symmetric, so each node only searches for the nodes after it and the cost matrix is mirrored). For the abstract search each node also has a dense id (cluster × 64 + index; a cluster has at most 60 edge cells), and edges cache their target's id and coordinates; rebuilding a cluster relinks its neighbours' inter edges. `rebuildDirty()` (start of the Pathfinding stage) rebuilds dirty clusters plus their 4 neighbours for every class in ascending order, relabels the dirty clusters, and bumps `version`. A test checks that the incremental rebuild equals a from-scratch build.

**Query** (`PathService`, one job at a time, FIFO by (requestTick, requester EntityId, request id)):
1. A start on a blocked cell moves to the nearest passable cell. A goal in another component moves to the nearest cell with the start's label (status `FoundNearest`). Both use a square-ring spiral ordered by (distance², cell index), clipped to the component's bounding box.
2. Bounded Dijkstra from start and goal to their clusters' entrances, then A\* on the abstract graph (heap keyed on (f, h, cell)).
3. **Short-request shortcut:** when start and goal are within 48 cells (Chebyshev), a plain A\* in their bounding box ± 16 cells also runs, and the cheaper result wins (ties go to the local path). HPA\*'s entrance detours cost the most, relative to path length, on short paths.
4. Refinement: each abstract segment is one cluster-bounded A\* (an inter edge is a single step). **Deviation from §3:** all segments are refined before the result is delivered. Lazy refinement of the next 2–3 segments needs the movement system, so it lands with M4 part 2.
5. Smoothing: string pulling in a 64-cell look-ahead window. Each waypoint jumps to the farthest cell in the window with line of sight (integer supercover; at an exact corner both side cells must be open). A shortcut may not cross a cell costlier than the costliest cell it replaces.

**Budget** (`kPathBudgetPerTick` = 20,000). **Deviation from §3.1:** a job suspends **between work units**, not mid-search. The units are (endpoint resolution + abstract search + short-request shortcut) and then one refined segment each. A unit that has started runs to completion, and any overrun is carried into the next tick as debt, so spend averages the budget. Every unit costs at least 1. Results do not depend on the budget (tested with budgets 1, 37 and 20,000). A suspended job whose graph `version` changed restarts from scratch.

**Sim integration.**
* Components: `Footprint` (stamped cell rect) and `Path` (status incl. Pending, move class, request id, cost, smoothed waypoints).
* Debug commands: `DebugPlaceBlocker` (204, a building stand-in) and `DebugRequestPath` (205). Destroying a footprint entity unstamps it at Cleanup; the rebuild happens in the next tick's Pathfinding stage.
* Finished paths are written to requesters that are still alive and haven't asked again since.
* State format v2 stores the graph version, pending dirty clusters and the PathService (queue, suspended job, debt, next id). Loading swaps the stamps for the loaded footprints and rebuilds the touched clusters.
* Replay format v2 names the map.

**Measured** (Release, MSVC, 500 random queries per class, `nav-bench --optimal`):

| Map | Build | Foot ms/query | Expansions avg / p99 | Cost vs optimal (mean / worst) |
|---|---|---|---|---|
| gen:7:256 | 96 ms | 0.10 | 768 / 2,405 | 1.027 / 1.139 (all classes ≤ 1.173) |
| Golden Fields (256²) | 80 ms | 0.11 | 803 / 1,772 | 1.028 / 1.114 (all classes ≤ 1.163) |
| Kursk (448², tracked) | 356 ms | 0.22 | 1,211 / 2,763 | 1.024 / 1.099 (all classes ≤ 1.126) |

Tests enforce mean ≤ 1.05× and worst ≤ 1.25× on generated maps (`simcli nav-bench --optimal` exits non-zero past the same bounds).

**Performance pass** (after M4 part 1). The searches were made faster without changing any result: every path, cost, expansion count and therefore every tick timing is identical. `nav-bench` prints the same `NAV BENCH HASH` before and after, and the golden-path hashes and the nav fixture are unchanged. Changes:
* Grid searches run in **rect-local coordinates** (stride = next power of two ≥ the rect width), so a cluster search's scratch is 256 records instead of the whole map, and x/y are a shift and a mask instead of two divisions. Local index order equals map index order, so the (f, h, cell) heap pops in the same order.
* Scratch is one 16-byte record per node (gen, g, parent, flags). The open list lives in the scratch, so a search allocates nothing. It is a binary heap with (f, h) packed in one 64-bit key and a branch-free child pick.
* **No closed set.** The heuristic is consistent (octile × 2 × min cost never drops by more than a step costs; abstract edges cost at least their octile length), so an expanded node is never improved. Every push lowers g, so `g == stored g` alone rejects stale entries. This skips exactly what the closed-set checks skipped.
* Effective cost per class (terrain, or 0 under a stamp) is kept up to date on stamp/unstamp, so a cost lookup is one byte load.
* Cluster Dijkstra marks its targets in the scratch instead of scanning the target list on every expansion. Refinement appends straight into the job's cell list (`gridAStarAppend`).
* Abstract search uses dense node ids and edge-cached target ids and coordinates, instead of map-sized scratch, a binary search per expansion and divisions per relax.
* Labels: union-find instead of a per-cell flood fill through `stepCost`, and only for classes a stamp changed.
* Intra edges: half the Dijkstra runs (symmetric costs).

Measured on the dev laptop (Intel i5-2520M, 2 cores, 2011; Release, MSVC). Each figure is the minimum of 3 interleaved runs at high priority on one pinned core. "ms per full tick" = `nav-bench` total time ÷ ticks at the 20,000 budget, so it averages full ticks plus the short final one:

| Map | ms per full tick, before → after (land classes) | Naval | NavWorld build | `rebuildDirty` after one 4×4 stamp |
|---|---|---|---|---|
| gen:7:256 | 5.8–7.3 → 2.5–3.0 | 1.8 → 0.8 | 377 → 96 ms | 24–33 → 5–6 ms |
| Golden Fields (256²) | 5.7–6.0 → 2.5–2.8 | 1.3 → 0.6 | 280 → 80 ms | 23–30 → 5–7 ms |
| Kursk (448²) | 7.0–7.4 → 3.1–3.9 | 0.8 → 0.4 | 1,147 → 356 ms | 75–119 → 13–17 ms (labels were 62–100 ms of it) |

That is about 3,000–3,500 → 5,000–8,500 expansions per ms. A Kursk stamp on a cluster corner (more clusters rebuilt) went from 92–135 to 19–29 ms. The busiest tick runs over its average because a started work unit always finishes (debt). Worst single ticks were about 3.2–3.5 ms on the 256² maps and 4.1–4.8 ms on Kursk.

**Budget vs the 3 ms target (§1).** On this laptop, the 20,000 budget fits 3 ms per tick on 256² maps (2.5–3.0 ms). On Kursk the land classes average 3.1–3.9 ms. **15,000 expansions per tick fits 3 ms on all three maps** on this machine (at about 5,000 expansions per ms, the slowest Kursk class). `kPathBudgetPerTick` stays 20,000 for now: changing it is a rules change (it moves when paths are delivered, so replay fixtures change), and a current desktop core is roughly 2× this 2011 laptop's. Revisit when real request rates and the target hardware are known. The remaining time is split roughly evenly: abstract search (~40% on Kursk), the two per-query cluster Dijkstras (20–45%), refinement (~25%) and smoothing (~7%, not counted as expansions).

**Cluster-level labels** (after M4). Labels were the last per-stamp cost that grew with the map: every stamp relabelled each touched class over the whole map (about 20 ms at 512², 55–115 ms at 1024²). Now:
* Each cluster labels its own cells: local components numbered by their lowest cell, with a box each. A cell stores its key, cluster × 129 + local index (a 16×16 cluster has at most 128 components, since no two touch), and its label is a lookup by that key.
* Each cluster keeps the (key, key) links where open cells face each other across its E and S borders. Consecutive equal links are stored once, so a border run usually costs one link.
* A stamp relabels the dirty clusters of each changed class and redoes the links on their four borders. A union-find over all local components and links (root = lowest key) then renumbers the components in key order and merges the local boxes. That pass is linear in the number of local components and links, which scales with the clusters (4,096 at 1024²), not the cells (1M).
* The partition and the boxes are exactly those of a whole-map labelling, so `nearestCell` scans and charges the same cells, and every path, cost and expansion count is unchanged: `NAV BENCH HASH`, the golden-path hashes, the nav fixture and the `simcli perf` state hashes are all the same. Label values changed (they are numbered by first local component now); nothing compares them except for equality. Tests check labels and boxes against a whole-map flood fill through stamps and unstamps, and that a clean neighbour's border links are refreshed.
* `simcli perf --ticks 1000`, same machine and method (3 interleaved runs at high priority on one core): ticks with a new building on gen:7:1024 went from 55–81 ms avg (max 61–97) to 6.2–8.0 ms avg (max 7.2–10.2). On gen:7:512 they went from 16–20 ms avg (max 18–26) to 5.6–6.2 ms avg (max 8.0–9.2). The Pathfinding stage's worst tick at 1024² went from 58–92 ms to 7–11 ms. NavWorld build time is unchanged within noise.

**Open:**
* Kursk land classes are still above 3 ms per full tick on the dev laptop (see above).
* Golden-path hashes and the nav fixture (`sim/tests/fixtures/synthetic_nav_300.rpl`) are recorded with MSVC. The Clang CI legs check them on the next push.

---

## 10. Implementation status (M4 part 2: movement, sim only)

Code: `sim/systems/orders.cpp` (commands, groups, the Orders stage's path requests), `sim/systems/movement.cpp` (the Movement stage), data in `sim/data/def_compiler.cpp` (layouts, formations, group formations, unit move class and turn type). Tests in `sim/tests/test_movement.cpp` (data-free, with a hand-built DefSet) and `test_tgi_golden.cpp` (the real data). `simcli scenario` runs six mixed regiments across the test map.

**Data.**
* `OrganizationLayout` records become `LayoutDef`. Regiment slot *i* fills layout slot *i*, which holds `number` elements, so a PER infantry regiment is 3+1+1+1 riflemen, 2 MGs and a captain (9 men), and a tank regiment is 3 tanks plus a commander.
* In each layout formation, element *e* with property *p* (front, support, captain) takes the *k*-th position tagged *p*.
* Offsets are stored in the regiment frame: +x forward is the data's +y, and +y left is the data's −x. **Assumption:** the data's +x is the regiment's right. The view will confirm it.
* `Formation` records give speed and attack multipliers from their `MultiplyModifier`s: `regiment_aggressive` is the manual's Assault (speed ×0.75), `regiment_defensive` is Normal/Aggressive (attack ×0.75), and `regiment_column` is Column (speed ×1.25, attack ×0.5). Each also names its group formation.
* Units get a move class from their property tags: aircraft → Air, seacraft → Naval, tank → Tracked, vehicle → Wheeled, else Foot. A regiment uses its most restrictive member's class (Naval > Amphibious > Wheeled > Tracked > Foot).

**Commands.** All take 1–6 regiments; ids that aren't the sender's live regiments are skipped, and the command is rejected if none is left.
* `MoveGroup` (2), with optional explicit facing.
* `Run` (3): temporary Column, previous formation restored on arrival or on the next normal order.
* `Stop` (4): stops the whole division if the regiment is in one.
* `SetFormation` (5): by formation IDS.
* `SetGroupBehaviour` (6): turns division mode on or off; switching stops the group.

Ordering several regiments together makes them one `Group`, reused when the same set is ordered again. Ordering one regiment alone takes it out of its group. A group left with under two regiments dissolves.

**Anchor** (the regiment's Transform):
* Follows the path's waypoints, turning at most 11.25° per tick.
* A waypoint more than 90° off makes it **wheel**: the heading snaps, and the anchor waits until 80% of its units reach their new slots, or 3 s pass.
* Speed = slowest living unit × formation multiplier × terrain multiplier (16/cost of the cell, so roads are faster and forest slower) × a **lag factor**: 1 while every unit is within 3 units of its slot, falling linearly to 0 at 6.
* While its path is pending, the anchor holds. **Deviation:** §3.1's straight-line pre-move isn't done; paths arrive within a tick or two at the current budget.

**Chokepoints.**
* A per-class clearance map is sampled at the anchor and 2, 4, 6 and 8 units ahead.
  * It is the integer chamfer 10/14 distance to the nearest cell the class can't enter, **capped at 16 units**: compression only compares it with formation half widths under 10, so the cap changes no decision.
  * It is derived data, not state, and is built once at map load. Classes that block the same cells (feet, wheels, tracks) share one computation.
  * It is then patched incrementally: when footprints are placed or removed, only cells within the cap are recomputed, from a window grown by the cap, which gives exactly the full result.
* If the clearance is under the formation's half width + unit radius + 0.5, the slots switch to the layout's Column. If it's under the Column's half width too, they switch to a **single file** (synthesized: front to back, spacing 2 × radius + 0.5).
* The data's Column is not narrow: infantry is 5.5 wide.
* The layout widens back after 10 ticks of enough clearance, also when halted. Speed and the combat formation don't change.

**Arrival.**
* The anchor reaches the goal (the exact point, or the nearest reachable cell's centre), then waits for ≥ 80% of its units to be within 1.5 of their slots, or 10 s.
* An explicit facing is applied when arrival starts, so the units settle into the rotated slots.
* The `Arrived` / `Blocked` events report the outcome.

**Groups and divisions** (§5):
* **Loose group:**
  * Each regiment paths on its own to goal + its group-formation slot, rotated to the centroid→goal heading.
  * The group formation comes from the first member's formation (Assault → `group_aggressive`, and so on).
* **Division:**
  * The `Group` entity has its own anchor, `Order` and `Path`, and paths with the members' most restrictive class.
  * Speed = max(slowest member speed, `GroupSpeedMinimum`). It halves while any member is more than `GroupZoneRangeMin` (18) from its slot. It never stops for stragglers, which would read as stuck.
  * Slots: group-formation offsets sorted front to back (forward descending, then centre first, then left first), strongest regiment (total unit health) first.
  * Members steer straight at their slot. When the slot is blocked or out of line of sight, they follow the division's trail (its anchor, or the latest path waypoint they can see).
  * The division finishes when every member is in its slot with its units settled (or after 15 s), and all members get `Arrived` in the same tick.
  * **Not yet:** coordinated arrival outside supply zones (`GroupSupplyZoneBuffer`), since supply zones arrive in M6.

**Units** (§6):
* Steering = arrive at the slot (approach speed = distance × 2/s, capped) + separation.
* Neighbours come from a spatial hash of 4×4-cell buckets, filled by counting sort so each bucket stays in ascending id order. Every unit reacts to positions at the start of the phase (Jacobi update), so the processing order doesn't matter.
* Separation weights:
  * same regiment 0.25;
  * friendly regiments: the lower-priority one yields with weight 1 while the other takes 0.5 (priority: moving > idle, then lower regiment id);
  * enemies 1.
* Gain is 10/s per unit of overlap. Units of different regiments also sidestep to their right, so head-on pairs pass instead of stalling.
* Velocity is capped at speed_max × formation × terrain under the unit, with `acceleration` limiting the change per tick (0 = instant).
* `turn_type`:
  * INSTANT snaps.
  * IN_PLACE turns at speed_max / turn_radius rad/s and pivots in place when more than 30° off.
  * WHILE_MOVING turns at the same rate but always drives where it points.
* Units settling inside their slot creep without turning and face the regiment's way.
* Terrain and buildings are hard: a step into a blocked cell slides along one axis or stops, and a unit inside a stamped cell walks out to the nearest open cell.

**Stuck → repath.**
* Each 20-tick window compares the anchor's actual progress with its speed allowance (lag excluded). Under 10% → new path from the current position.
* A building appearing on the next step also re-paths at once. But first the anchor tries to slide along the free axis (M6 part 1): a path's corners are checked between cell centres while the anchor walks from where it actually is, so its line can clip the corner of a building cell it passes. Before this, a truck leaving its Corps HQ diagonally re-pathed into the same corner until the order ended blocked.
* After 3 consecutive failures the order ends as blocked.

**Measured.** `simcli scenario` on gen:7:256, six regiments (PER/USA/GBR-AT infantry, GER medium tank, GBR heavy tank, USA halftrack):

| Regiments | Order | Arrived at tick |
|---|---|---|
| Three regiments | Division | 1046, together |
| USA infantry | Loose group, switched to Column mid-way | 725 |
| GBR heavy tank | Stopped at tick 200, re-ordered alone with a facing | 800 |
| GBR AT infantry | Run | 742 |

The film determinism-checks per stage, and a Release recording plays identically in Debug.

**Performance (M4 close-out).** `simcli perf` on gen:7:1024 (the generator's largest map): 160 regiments of eight types (1,060 units), half per side, cross the map in groups of six, every third group a division. A 6×6 building drops every 100 ticks. 3,000 ticks, Release, MSVC, dev laptop (i5-2520M, 2011):

| | avg | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| Sim tick | 3.6 ms | 2.8 ms | 4.5 ms | **10.7 ms** | 117 ms |
| Ticks with a new building (29) | 57 ms | | | | 117 ms |
| Movement stage | 3.0 ms | | | | 5.9 ms |

* The 15 ms target (ARCHITECTURE §7) is at the 99th percentile, and it is met.
* **Fixed** (the worst hotspot): the clearance map was recomputed for the whole map on every stamp or unstamp. That cost 95–130 ms in Movement, plus the first tick. It is now capped and incremental (see Chokepoints) and built at map load.
* **Fixed** (found by the benchmark): divisions whose members started far apart stopped to wait for them and then counted as stuck. They now slow down instead.
* **Fixed** (the remaining hotspot): a stamp made `rebuildDirty` relabel the touched classes over the whole map, about 20 ms at 512² and 55–115 ms at 1024². Labels are now kept per cluster and only dirty clusters are relabelled (§9, "Cluster-level labels"). With 1,000 ticks (3 interleaved runs, min–max of each figure):

  | gen:7:1024 | before | after |
  |---|---|---|
  | Ticks with a new building (9), avg | 55–81 ms | **6.2–8.0 ms** |
  | Ticks with a new building, max | 61–97 ms | **7.2–10.2 ms** |
  | Pathfinding stage, max | 58–92 ms | 6.6–11.3 ms |
  | Sim tick p99 | 13.7–49 ms | 8.7–12.0 ms |

  Building ticks are now under the 15 ms target on the largest map. At 512² they dropped from 16–20 ms avg to 5.6–6.2 ms. State hashes are unchanged.
* The film (`simcli perf --out`) determinism-checks per stage over 3,000 ticks and plays identically in Debug.

**Open:**
* Lazy refinement of the next 2–3 segments (§3) is still not done; whole paths are refined.
* ZOC suspension of moves is M5.
* Aircraft fly straight lines with no missions.
* Trucks don't tow trailers, and ships use only their own turn radius.
* Retreat formations aren't used yet.
* The movement golden hash (`test_movement.cpp`) and fixtures are MSVC-recorded; the Clang CI legs check them on the next push.
