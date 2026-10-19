#pragma once

// Low-level grid searches (PATHFINDING §3): A* and multi-target Dijkstra on the nav grid,
// restricted to a cell rectangle (a cluster, or the whole map for reference searches).
// Deterministic: the open list is a binary heap keyed on (f, h, cell); relaxation keeps the first
// parent found at equal cost; neighbours are visited in the fixed kDir order. Costs are int32.

#include <cstdint>
#include <vector>

#include "nav/nav_grid.hpp"

namespace corps::nav {

// Reusable per-cell scratch (generation-stamped, so a search never clears the whole map).
class SearchScratch {
public:
    void prepare(std::size_t cells);
    void begin();  // new search: invalidates all entries in O(1)

    bool seen(std::uint32_t c) const { return gen[c] == current; }
    std::int32_t g(std::uint32_t c) const { return seen(c) ? gcost[c] : INT32_MAX; }
    std::uint32_t parentOf(std::uint32_t c) const { return parent[c]; }
    bool closed(std::uint32_t c) const { return seen(c) && closedFlag[c]; }
    void set(std::uint32_t c, std::int32_t gv, std::uint32_t par);
    void close(std::uint32_t c) { closedFlag[c] = 1; }

private:
    std::vector<std::uint32_t> gen;
    std::vector<std::int32_t> gcost;
    std::vector<std::uint32_t> parent;
    std::vector<std::uint8_t> closedFlag;
    std::uint32_t current = 0;
};

struct OpenEntry {
    std::int32_t f = 0;
    std::int32_t h = 0;
    std::uint32_t cell = 0;
};
// Min-heap order on (f, h, cell).
struct OpenGreater {
    bool operator()(const OpenEntry& a, const OpenEntry& b) const {
        if (a.f != b.f) return a.f > b.f;
        if (a.h != b.h) return a.h > b.h;
        return a.cell > b.cell;
    }
};

struct GridPath {
    bool found = false;
    std::int32_t cost = 0;
    std::vector<std::uint32_t> cells;  // from..to inclusive
    std::uint32_t expansions = 0;
};

// A* from `from` to `to` inside `bounds` (both must lie inside it).
GridPath gridAStar(const NavGrid& grid, MoveClass c, std::uint32_t from, std::uint32_t to, const CellRect& bounds,
                   SearchScratch& s);

// Dijkstra from `from` inside `bounds`; outCost[i] = cost to targets[i] (INT32_MAX if unreachable).
// Stops early once every target is settled. Returns expansions.
std::uint32_t gridDijkstra(const NavGrid& grid, MoveClass c, std::uint32_t from, const CellRect& bounds,
                           const std::vector<std::uint32_t>& targets, std::vector<std::int32_t>& outCost, SearchScratch& s);

CellRect wholeMap(const NavGrid& grid);

}  // namespace corps::nav
