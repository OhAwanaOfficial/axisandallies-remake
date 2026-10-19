#pragma once

// PathService (PATHFINDING §3.1): FIFO queue of path requests ordered by (requestTick, requester,
// id), worked off with a fixed node-expansion budget per tick. Deterministic because the budget is
// a count, not a time.
//
// A request runs as a job of work units: [resolve endpoints + abstract HPA* search], then one unit
// per refined segment (cluster-bounded A*), with smoothing at the end. The service suspends between
// units: a unit that starts runs to completion, and any overrun is carried into the next tick as
// debt, so the average spend equals the budget. Jobs whose nav graph changed (Hpa::version) restart.
//
// Unreachable goals are replaced by the nearest reachable cell (by distance², then cell index);
// a blocked start (e.g. a unit standing on a just-stamped building) moves to the nearest passable cell.

#include <cstdint>
#include <vector>

#include "ecs/entity.hpp"
#include "nav/grid_search.hpp"
#include "nav/hpa.hpp"
#include "session/bytes.hpp"

namespace corps::nav {

// Node expansions per tick (PATHFINDING §3.1). Part of the rules: changing it changes results' timing.
constexpr std::int32_t kPathBudgetPerTick = 20000;

enum class PathStatus : std::uint8_t { Found = 0, FoundNearest = 1, Unreachable = 2, NoStart = 3 };
const char* pathStatusName(PathStatus s);

struct PathRequest {
    std::uint32_t id = 0;
    std::uint32_t tick = 0;
    EntityId requester;
    MoveClass cls = MoveClass::Foot;
    std::uint32_t start = 0;  // cells
    std::uint32_t goal = 0;
};

struct PathResult {
    std::uint32_t requestId = 0;
    EntityId requester;
    MoveClass cls = MoveClass::Foot;
    PathStatus status = PathStatus::Unreachable;
    std::vector<std::uint32_t> waypoints;  // smoothed: start, corners..., goal (cells)
    std::vector<std::uint32_t> cells;      // the unsmoothed cell path (every step a legal move)
    std::int32_t cost = 0;                 // abstract path cost (grid step units)
    std::uint32_t expansions = 0;
    std::uint32_t goal = 0;                // the goal actually used (nearest reachable if needed)
};

class PathService {
public:
    std::uint32_t request(EntityId who, MoveClass c, std::uint32_t startCell, std::uint32_t goalCell, std::uint32_t tick);

    // Spends `budget` expansions (minus carried debt) on the queue; appends completed results.
    void step(Hpa& hpa, std::int32_t budget, std::vector<PathResult>& out);

    // Runs one request to completion immediately (tools, tests). Does not touch the queue.
    PathResult solveNow(Hpa& hpa, MoveClass c, std::uint32_t startCell, std::uint32_t goalCell);

    std::size_t pending() const { return queue.size() + (active ? 1 : 0); }
    std::int32_t debt() const { return debt_; }

    // Canonical state (part of the sim state hash).
    void save(ByteWriter& w) const;
    bool load(ByteReader& r);

private:
    struct Job {
        PathRequest req;
        std::uint32_t navVersion = 0;
        std::uint8_t phase = 0;  // 0 = resolve + abstract, 1 = refining, 2 = done
        PathStatus status = PathStatus::Found;
        std::uint32_t start = 0, goal = 0;
        std::vector<std::uint32_t> abstract;  // S, nodes..., G
        std::size_t segment = 0;              // next abstract segment to refine
        std::vector<std::uint32_t> cells;     // refined so far
        std::int32_t cost = 0;
        std::uint32_t expansions = 0;
    };

    // Runs one work unit of `job`; returns expansions spent.
    std::uint32_t runUnit(Hpa& hpa, Job& job);
    std::uint32_t resolveAndSearch(Hpa& hpa, Job& job);
    std::uint32_t refineSegment(Hpa& hpa, Job& job);
    PathResult finish(const Hpa& hpa, Job& job) const;

    std::vector<PathRequest> queue;  // sorted
    bool active = false;
    Job job;
    std::int32_t debt_ = 0;
    std::uint32_t nextId = 1;
    SearchScratch gridScratch;
    SearchScratch absScratch;
};

// Smooths a cell path by string pulling with NavGrid::lineOfSight (lookahead window of 64 cells;
// a shortcut may not cross a cell costlier than the most expensive cell it replaces).
std::vector<std::uint32_t> smoothPath(const NavGrid& g, MoveClass c, const std::vector<std::uint32_t>& cells);

}  // namespace corps::nav
