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
* `PathService` keeps a **FIFO queue** ordered by `(requestTick, regimentId)`.
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
* **Unpack placement:** check the footprint against the static and dynamic layers plus `placement_groups` rules (spacing to other buildings, not on water or mountain). The trucks path to the placement point, then the 7 s unpack timer runs and the footprint is stamped at the start of unpacking, so other units route around it.

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
