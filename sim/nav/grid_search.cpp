#include "nav/grid_search.hpp"

#include <algorithm>

namespace corps::nav {

void SearchScratch::prepare(std::size_t nodes) {
    if (node.size() < nodes) node.resize(nodes);  // new records have gen 0, never current (begin() makes it >= 1)
}

void SearchScratch::begin() {
    if (++current == 0) {  // wrapped: really clear once every 2^32 searches
        for (Node& n : node) n.gen = 0;
        current = 1;
    }
    open.clear();
}

void SearchScratch::markTarget(std::uint32_t c) {
    Node& n = node[c];
    if (n.gen != current) {
        n.gen = current;
        n.g = INT32_MAX;
        n.flags = 0;
    }
    n.flags |= kTarget;
}

namespace {

// (f, h, cell) order, written without branches: heap comparisons are close to coin flips.
inline bool before(const OpenEntry& a, const OpenEntry& b) {
    return (a.key < b.key) | ((a.key == b.key) & (a.cell < b.cell));
}

}  // namespace

void SearchScratch::push(const OpenEntry& e) {
    std::size_t i = open.size();
    open.push_back(e);
    while (i > 0) {  // sift up
        const std::size_t p = (i - 1) / 2;
        if (!before(e, open[p])) break;
        open[i] = open[p];
        i = p;
    }
    open[i] = e;
}

OpenEntry SearchScratch::pop() {
    const OpenEntry top = open.front();
    const OpenEntry last = open.back();
    open.pop_back();
    const std::size_t n = open.size();
    if (n != 0) {  // sift `last` down from the root
        std::size_t i = 0;
        for (;;) {
            std::size_t c = 2 * i + 1;
            if (c >= n) break;
            if (c + 1 < n) c += static_cast<std::size_t>(before(open[c + 1], open[c]));
            if (!before(open[c], last)) break;
            open[i] = open[c];
            i = c;
        }
        open[i] = last;
    }
    return top;
}

CellRect wholeMap(const NavGrid& grid) {
    return CellRect{0, 0, grid.width() - 1, grid.height() - 1};
}

namespace {

// Searches run in rect-local coordinates: local index li = (y - y0) * stride + (x - x0), with the
// stride the next power of two >= the rect width. The scratch then only spans the rect (a cluster
// search touches 256 records, not the whole map) and x, y come from a shift and a mask. Ordering
// cells by local index is the same as ordering them by map index (both are row-major over the same
// rows), so the (f, h, cell) heap pops in exactly the order it would with map indexes.
//
// No closed set: the heuristic is consistent (h = octile x 2 x the class's minimum cost never drops
// by more than a step costs), so an expanded cell is never improved again, and every push lowers a
// cell's g, so an entry whose g is no longer the cell's g is stale. "ng < g(nb)" and "g == g(cell)"
// alone therefore skip exactly what a closed-set check would.
struct Frame {
    std::int32_t x0 = 0, y0 = 0, rw = 0, rh = 0;
    std::uint32_t shift = 0, mask = 0, base = 0, w = 0;

    Frame(const NavGrid& g, const CellRect& r)
        : x0(r.x0), y0(r.y0), rw(r.x1 - r.x0 + 1), rh(r.y1 - r.y0 + 1), w(static_cast<std::uint32_t>(g.width())) {
        while ((std::int32_t{1} << shift) < rw) ++shift;
        mask = (1u << shift) - 1;
        base = g.index(r.x0, r.y0);
    }
    std::uint32_t stride() const { return mask + 1; }
    std::size_t size() const { return static_cast<std::size_t>(stride()) * static_cast<std::size_t>(rh); }
    // Rect-local coordinates of a map cell (may lie outside the rect).
    void toLocal(const NavGrid& g, std::uint32_t cell, std::int32_t& lx, std::int32_t& ly) const {
        ly = g.yOf(cell);
        lx = static_cast<std::int32_t>(cell - static_cast<std::uint32_t>(ly) * w) - x0;
        ly -= y0;
    }
    bool contains(std::int32_t lx, std::int32_t ly) const { return lx >= 0 && ly >= 0 && lx < rw && ly < rh; }
    std::uint32_t local(std::int32_t lx, std::int32_t ly) const {
        return (static_cast<std::uint32_t>(ly) << shift) | static_cast<std::uint32_t>(lx);
    }
    std::uint32_t global(std::uint32_t li) const { return base + (li >> shift) * w + (li & mask); }
};

// Calls visit(step cost, dx, dy) for every legal move out of map cell `cell` (rect-local lx, ly)
// that stays inside the rect, in kDir order. Same rules as NavGrid::stepCost: both cells open, and
// a diagonal also needs both orthogonal cells open. A diagonal target inside the rect has both its
// orthogonal cells inside it, so testing the orthogonals against the rect rather than the map
// gives the same answer.
template <class Visit>
inline void forEachStep(const std::uint8_t* cost, const Frame& f, std::uint32_t cell, std::int32_t lx, std::int32_t ly,
                        Visit&& visit) {
    const std::int32_t from = cost[cell];
    if (from == 0) return;
    const std::uint32_t w = f.w;
    const std::int32_t e = lx + 1 < f.rw ? cost[cell + 1] : 0;
    const std::int32_t wv = lx > 0 ? cost[cell - 1] : 0;
    const std::int32_t s = ly + 1 < f.rh ? cost[cell + w] : 0;
    const std::int32_t n = ly > 0 ? cost[cell - w] : 0;
    if (e != 0) visit(kStraight * (from + e), 1, 0);
    if (wv != 0) visit(kStraight * (from + wv), -1, 0);
    if (s != 0) visit(kStraight * (from + s), 0, 1);
    if (n != 0) visit(kStraight * (from + n), 0, -1);
    if (e != 0 && s != 0) {
        const std::int32_t to = cost[cell + w + 1];
        if (to != 0) visit(kDiagonal * (from + to), 1, 1);
    }
    if (wv != 0 && s != 0) {
        const std::int32_t to = cost[cell + w - 1];
        if (to != 0) visit(kDiagonal * (from + to), -1, 1);
    }
    if (e != 0 && n != 0) {
        const std::int32_t to = cost[cell - w + 1];
        if (to != 0) visit(kDiagonal * (from + to), 1, -1);
    }
    if (wv != 0 && n != 0) {
        const std::int32_t to = cost[cell - w - 1];
        if (to != 0) visit(kDiagonal * (from + to), -1, -1);
    }
}

}  // namespace

GridPath gridAStarAppend(const NavGrid& grid, MoveClass c, std::uint32_t from, std::uint32_t to, const CellRect& bounds,
                         SearchScratch& s, std::vector<std::uint32_t>& cells) {
    GridPath out;
    const Frame fr(grid, bounds);
    s.prepare(fr.size());
    s.begin();
    const std::uint8_t* cost = grid.costs(c);
    const auto stride = static_cast<std::int32_t>(fr.stride());
    const std::int32_t hScale = 2 * grid.minCost(c);
    std::int32_t tx = 0, ty = 0, sx = 0, sy = 0;  // rect-local
    fr.toLocal(grid, to, tx, ty);
    fr.toLocal(grid, from, sx, sy);
    const std::uint32_t toLocal = fr.contains(tx, ty) ? fr.local(tx, ty) : UINT32_MAX;
    const std::uint32_t fromLocal = fr.local(sx, sy);
    const std::int32_t h0 = octile(sx - tx, sy - ty) * hScale;
    s.set(fromLocal, 0, fromLocal);
    s.push(OpenEntry::make(h0, h0, fromLocal));
    while (!s.openEmpty()) {
        const OpenEntry e = s.pop();
        const std::int32_t gHere = e.f() - e.h();
        if (gHere != s.g(e.cell)) continue;  // stale entry
        ++out.expansions;
        if (e.cell == toLocal) {
            out.found = true;
            out.cost = gHere;
            std::size_t len = 0;  // cells after `from`
            for (std::uint32_t cur = toLocal; cur != fromLocal; cur = s.parentOf(cur)) ++len;
            const std::size_t at = cells.size();
            cells.resize(at + len);
            std::size_t k = at + len;
            for (std::uint32_t cur = toLocal; cur != fromLocal; cur = s.parentOf(cur)) cells[--k] = fr.global(cur);
            return out;
        }
        const auto lx = static_cast<std::int32_t>(e.cell & fr.mask), ly = static_cast<std::int32_t>(e.cell >> fr.shift);
        forEachStep(cost, fr, fr.global(e.cell), lx, ly, [&](std::int32_t step, std::int32_t dx, std::int32_t dy) {
            const std::uint32_t nb = e.cell + static_cast<std::uint32_t>(dx + dy * stride);
            const std::int32_t ng = gHere + step;
            if (ng < s.g(nb)) {
                s.set(nb, ng, e.cell);
                const std::int32_t hh = octile(lx + dx - tx, ly + dy - ty) * hScale;
                s.push(OpenEntry::make(ng + hh, hh, nb));
            }
        });
    }
    return out;
}

GridPath gridAStar(const NavGrid& grid, MoveClass c, std::uint32_t from, std::uint32_t to, const CellRect& bounds,
                   SearchScratch& s) {
    std::vector<std::uint32_t> cells{from};
    GridPath out = gridAStarAppend(grid, c, from, to, bounds, s, cells);
    if (out.found) out.cells = std::move(cells);
    return out;
}

std::uint32_t gridDijkstra(const NavGrid& grid, MoveClass c, std::uint32_t from, const CellRect& bounds,
                           const std::vector<std::uint32_t>& targets, std::vector<std::int32_t>& outCost, SearchScratch& s) {
    const Frame fr(grid, bounds);
    s.prepare(fr.size());
    s.begin();
    outCost.assign(targets.size(), INT32_MAX);
    std::size_t remaining = targets.size();
    std::uint32_t expansions = 0;
    for (const std::uint32_t t : targets) {
        std::int32_t lx = 0, ly = 0;
        fr.toLocal(grid, t, lx, ly);
        if (fr.contains(lx, ly)) s.markTarget(fr.local(lx, ly));
    }
    const std::uint8_t* cost = grid.costs(c);
    const auto stride = static_cast<std::int32_t>(fr.stride());
    std::int32_t sx = 0, sy = 0;
    fr.toLocal(grid, from, sx, sy);
    const std::uint32_t fromLocal = fr.local(sx, sy);
    s.set(fromLocal, 0, fromLocal);
    s.push(OpenEntry::make(0, 0, fromLocal));
    while (!s.openEmpty() && remaining > 0) {
        const OpenEntry e = s.pop();
        const std::int32_t gHere = e.f();
        if (gHere != s.g(e.cell)) continue;  // stale entry
        ++expansions;
        const std::uint32_t cell = fr.global(e.cell);
        if (s.isTarget(e.cell)) {
            for (std::size_t i = 0; i < targets.size(); ++i) {
                if (targets[i] == cell && outCost[i] == INT32_MAX) {
                    outCost[i] = gHere;
                    --remaining;
                }
            }
        }
        const auto lx = static_cast<std::int32_t>(e.cell & fr.mask), ly = static_cast<std::int32_t>(e.cell >> fr.shift);
        forEachStep(cost, fr, cell, lx, ly, [&](std::int32_t step, std::int32_t dx, std::int32_t dy) {
            const std::uint32_t nb = e.cell + static_cast<std::uint32_t>(dx + dy * stride);
            const std::int32_t ng = gHere + step;
            if (ng < s.g(nb)) {
                s.set(nb, ng, e.cell);
                s.push(OpenEntry::make(ng, 0, nb));
            }
        });
    }
    return expansions;
}

}  // namespace corps::nav
