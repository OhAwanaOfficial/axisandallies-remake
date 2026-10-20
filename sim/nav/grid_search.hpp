#pragma once

// Low-level grid searches (PATHFINDING §3): A* and multi-target Dijkstra on the nav grid,
// restricted to a cell rectangle (a cluster, or the whole map for reference searches).
// Deterministic: the open list is a binary heap keyed on (f, h, cell); relaxation keeps the first
// parent found at equal cost; neighbours are visited in the fixed kDir order. Costs are int32.

#include <cstdint>
#include <vector>

#include "nav/nav_grid.hpp"

namespace corps::nav {

// Open-list entry, ordered by (f, h, cell). f and h are never negative, so they pack into one
// key that compares in (f, h) order.
struct OpenEntry {
    std::uint64_t key = 0;
    std::uint32_t cell = 0;
    std::uint32_t id = 0;  // abstract search: the node's scratch id (not part of the order)

    static OpenEntry make(std::int32_t f, std::int32_t h, std::uint32_t cell, std::uint32_t id = 0) {
        return {static_cast<std::uint64_t>(static_cast<std::uint32_t>(f)) << 32 | static_cast<std::uint32_t>(h), cell, id};
    }
    std::int32_t f() const { return static_cast<std::int32_t>(key >> 32); }
    std::int32_t h() const { return static_cast<std::int32_t>(key & 0xFFFFFFFFu); }
};

// Reusable search scratch: one record per search node (generation-stamped, so a search never
// clears it) plus the open list. Grid searches index it by rect-local cell (see grid_search.cpp),
// the abstract search by abstract node id. One record per node keeps its state in one cache line.
class SearchScratch {
public:
    void prepare(std::size_t nodes);  // grows only
    void begin();  // new search: invalidates all entries in O(1) and empties the open list

    bool seen(std::uint32_t c) const { return node[c].gen == current; }
    std::int32_t g(std::uint32_t c) const { return seen(c) ? node[c].g : INT32_MAX; }
    std::uint32_t parentOf(std::uint32_t c) const { return node[c].parent; }
    void set(std::uint32_t c, std::int32_t gv, std::uint32_t par) {
        Node& n = node[c];
        if (n.gen != current) {
            n.gen = current;
            n.flags = 0;
        }
        n.g = gv;
        n.parent = par;
    }
    // Target marks for gridDijkstra (cleared by begin()). A marked cell is otherwise unseen.
    // (No closed set: the searches' heuristics are consistent, see grid_search.cpp.)
    void markTarget(std::uint32_t c);
    bool isTarget(std::uint32_t c) const { return seen(c) && (node[c].flags & kTarget) != 0; }

    // Open list: a binary min-heap in (f, h, cell) order. Searches never push two entries that
    // compare equal (a node is only pushed again with a smaller g), so the pop order does not
    // depend on how the heap is arranged.
    void push(const OpenEntry& e);
    OpenEntry pop();
    bool openEmpty() const { return open.empty(); }

private:
    static constexpr std::uint32_t kTarget = 1;
    struct Node {
        std::uint32_t gen = 0;
        std::int32_t g = 0;
        std::uint32_t parent = 0;
        std::uint32_t flags = 0;
    };
    std::vector<Node> node;
    std::vector<OpenEntry> open;
    std::uint32_t current = 0;
};

struct GridPath {
    bool found = false;
    std::int32_t cost = 0;
    std::vector<std::uint32_t> cells;  // from..to inclusive
    std::uint32_t expansions = 0;
};

// A* from `from` to `to` inside `bounds` (`from` must lie inside it; a `to` outside is never found).
GridPath gridAStar(const NavGrid& grid, MoveClass c, std::uint32_t from, std::uint32_t to, const CellRect& bounds,
                   SearchScratch& s);
// The same search, but appends the path's cells after `from` (from excluded) to `cells` instead of
// returning them; the result's `cells` stays empty. Saves the allocations when refining segments.
GridPath gridAStarAppend(const NavGrid& grid, MoveClass c, std::uint32_t from, std::uint32_t to, const CellRect& bounds,
                         SearchScratch& s, std::vector<std::uint32_t>& cells);

// Dijkstra from `from` (inside `bounds`) within `bounds`; outCost[i] = cost to targets[i] (INT32_MAX if unreachable).
// Stops early once every target is settled. Returns expansions.
std::uint32_t gridDijkstra(const NavGrid& grid, MoveClass c, std::uint32_t from, const CellRect& bounds,
                           const std::vector<std::uint32_t>& targets, std::vector<std::int32_t>& outCost, SearchScratch& s);

CellRect wholeMap(const NavGrid& grid);

}  // namespace corps::nav
