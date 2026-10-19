#include "nav/grid_search.hpp"

#include <algorithm>

namespace corps::nav {

void SearchScratch::prepare(std::size_t cells) {
    if (gen.size() != cells) {
        gen.assign(cells, 0);
        gcost.assign(cells, 0);
        parent.assign(cells, 0);
        closedFlag.assign(cells, 0);
        current = 0;
    }
}

void SearchScratch::begin() {
    if (++current == 0) {  // wrapped: really clear once every 2^32 searches
        std::fill(gen.begin(), gen.end(), 0);
        current = 1;
    }
}

void SearchScratch::set(std::uint32_t c, std::int32_t gv, std::uint32_t par) {
    if (gen[c] != current) {
        gen[c] = current;
        closedFlag[c] = 0;
    }
    gcost[c] = gv;
    parent[c] = par;
}

CellRect wholeMap(const NavGrid& grid) {
    return CellRect{0, 0, grid.width() - 1, grid.height() - 1};
}

namespace {

bool inRect(const NavGrid& g, std::uint32_t cell, const CellRect& r) {
    const std::int32_t x = g.xOf(cell), y = g.yOf(cell);
    return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
}

}  // namespace

GridPath gridAStar(const NavGrid& grid, MoveClass c, std::uint32_t from, std::uint32_t to, const CellRect& bounds,
                   SearchScratch& s) {
    GridPath out;
    s.prepare(static_cast<std::size_t>(grid.width()) * static_cast<std::size_t>(grid.height()));
    s.begin();
    const std::int32_t hScale = 2 * grid.minCost(c);
    const std::int32_t tx = grid.xOf(to), ty = grid.yOf(to);
    const auto heur = [&](std::uint32_t cell) { return octile(grid.xOf(cell) - tx, grid.yOf(cell) - ty) * hScale; };
    std::vector<OpenEntry> open;
    s.set(from, 0, from);
    open.push_back({heur(from), heur(from), from});
    while (!open.empty()) {
        std::pop_heap(open.begin(), open.end(), OpenGreater{});
        const OpenEntry e = open.back();
        open.pop_back();
        if (s.closed(e.cell) || e.f - e.h != s.g(e.cell)) continue;  // stale entry
        s.close(e.cell);
        ++out.expansions;
        if (e.cell == to) {
            out.found = true;
            out.cost = s.g(to);
            for (std::uint32_t cur = to;; cur = s.parentOf(cur)) {
                out.cells.push_back(cur);
                if (cur == from) break;
            }
            std::reverse(out.cells.begin(), out.cells.end());
            return out;
        }
        const std::int32_t x = grid.xOf(e.cell), y = grid.yOf(e.cell);
        const std::int32_t gHere = s.g(e.cell);
        for (int d = 0; d < 8; ++d) {
            const std::int32_t step = grid.stepCost(c, x, y, d);
            if (step == 0) continue;
            const std::uint32_t nb = grid.index(x + kDirX[d], y + kDirY[d]);
            if (!inRect(grid, nb, bounds) || s.closed(nb)) continue;
            const std::int32_t ng = gHere + step;
            if (ng < s.g(nb)) {
                s.set(nb, ng, e.cell);
                const std::int32_t hh = heur(nb);
                open.push_back({ng + hh, hh, nb});
                std::push_heap(open.begin(), open.end(), OpenGreater{});
            }
        }
    }
    return out;
}

std::uint32_t gridDijkstra(const NavGrid& grid, MoveClass c, std::uint32_t from, const CellRect& bounds,
                           const std::vector<std::uint32_t>& targets, std::vector<std::int32_t>& outCost, SearchScratch& s) {
    s.prepare(static_cast<std::size_t>(grid.width()) * static_cast<std::size_t>(grid.height()));
    s.begin();
    outCost.assign(targets.size(), INT32_MAX);
    std::size_t remaining = targets.size();
    std::uint32_t expansions = 0;
    std::vector<OpenEntry> open;
    s.set(from, 0, from);
    open.push_back({0, 0, from});
    while (!open.empty() && remaining > 0) {
        std::pop_heap(open.begin(), open.end(), OpenGreater{});
        const OpenEntry e = open.back();
        open.pop_back();
        if (s.closed(e.cell) || e.f != s.g(e.cell)) continue;
        s.close(e.cell);
        ++expansions;
        for (std::size_t i = 0; i < targets.size(); ++i) {  // targets lists are short (cluster nodes)
            if (targets[i] == e.cell && outCost[i] == INT32_MAX) {
                outCost[i] = e.f;
                --remaining;
            }
        }
        const std::int32_t x = grid.xOf(e.cell), y = grid.yOf(e.cell);
        for (int d = 0; d < 8; ++d) {
            const std::int32_t step = grid.stepCost(c, x, y, d);
            if (step == 0) continue;
            const std::uint32_t nb = grid.index(x + kDirX[d], y + kDirY[d]);
            if (!inRect(grid, nb, bounds) || s.closed(nb)) continue;
            const std::int32_t ng = e.f + step;
            if (ng < s.g(nb)) {
                s.set(nb, ng, e.cell);
                open.push_back({ng, 0, nb});
                std::push_heap(open.begin(), open.end(), OpenGreater{});
            }
        }
    }
    return expansions;
}

}  // namespace corps::nav
