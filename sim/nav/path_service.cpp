#include "nav/path_service.hpp"

#include <algorithm>
#include <cstdlib>

namespace corps::nav {

namespace {

constexpr std::size_t kSmoothWindow = 64;
constexpr std::int32_t kLocalRange = 3 * kClusterSize;  // Chebyshev cells; below this, also try a local A*

bool queueLess(const PathRequest& a, const PathRequest& b) {
    if (a.tick != b.tick) return a.tick < b.tick;
    if (a.requester.bits != b.requester.bits) return a.requester.bits < b.requester.bits;
    return a.id < b.id;
}

// Nearest cell to `around` inside `clip` satisfying pred (by distance², then cell index): spiral of
// square rings, each clipped to `clip`, starting at the first ring that reaches it. Returns false
// if none. `scanned` counts the cells looked at (they are charged to the budget).
template <class Pred>
bool nearestCell(const NavGrid& g, std::uint32_t around, const CellRect& clip, Pred pred, std::uint32_t& out,
                 std::uint32_t& scanned);

}  // namespace

const char* pathStatusName(PathStatus s) {
    switch (s) {
    case PathStatus::Found: return "found";
    case PathStatus::FoundNearest: return "nearest";
    case PathStatus::Unreachable: return "unreachable";
    case PathStatus::NoStart: return "no-start";
    }
    return "?";
}

namespace {
template <class Pred>
bool nearestCell(const NavGrid& g, std::uint32_t around, const CellRect& clipIn, Pred pred, std::uint32_t& out,
                 std::uint32_t& scanned) {
    const CellRect clip{std::max(clipIn.x0, 0), std::max(clipIn.y0, 0), std::min(clipIn.x1, g.width() - 1),
                        std::min(clipIn.y1, g.height() - 1)};
    if (clip.empty()) return false;
    const std::int32_t cx = g.xOf(around), cy = g.yOf(around);
    const auto dist = [](std::int32_t v, std::int32_t lo, std::int32_t hi) { return v < lo ? lo - v : (v > hi ? v - hi : 0); };
    const std::int32_t rMin = std::max(dist(cx, clip.x0, clip.x1), dist(cy, clip.y0, clip.y1));
    const std::int32_t rMax = std::max({cx - clip.x0, clip.x1 - cx, cy - clip.y0, clip.y1 - cy});
    std::int64_t bestD2 = INT64_MAX;
    std::uint32_t best = 0;
    const auto consider = [&](std::int32_t x, std::int32_t y) {
        ++scanned;
        const std::uint32_t cell = g.index(x, y);
        if (!pred(cell)) return;
        const std::int64_t d2 = static_cast<std::int64_t>(x - cx) * (x - cx) + static_cast<std::int64_t>(y - cy) * (y - cy);
        if (d2 < bestD2 || (d2 == bestD2 && cell < best)) {
            bestD2 = d2;
            best = cell;
        }
    };
    for (std::int32_t r = rMin; r <= rMax; ++r) {
        if (static_cast<std::int64_t>(r) * r > bestD2) break;  // every cell of ring r is at least r away
        const std::int32_t ya = std::max(cy - r, clip.y0), yb = std::min(cy + r, clip.y1);
        const std::int32_t xa = std::max(cx - r, clip.x0), xb = std::min(cx + r, clip.x1);
        for (std::int32_t y = ya; y <= yb; ++y) {
            if (y == cy - r || y == cy + r) {
                for (std::int32_t x = xa; x <= xb; ++x) consider(x, y);
            } else {
                if (cx - r >= clip.x0) consider(cx - r, y);
                if (cx + r <= clip.x1 && r != 0) consider(cx + r, y);
            }
        }
    }
    if (bestD2 == INT64_MAX) return false;
    out = best;
    return true;
}
}  // namespace

std::vector<std::uint32_t> smoothPath(const NavGrid& g, MoveClass c, const std::vector<std::uint32_t>& cells) {
    std::vector<std::uint32_t> out;
    if (cells.empty()) return out;
    out.push_back(cells.front());
    std::vector<std::uint8_t> maxCost;  // maxCost[k] = costliest cell of cells[i..i+k]
    std::size_t i = 0;
    while (i + 1 < cells.size()) {
        const std::size_t end = std::min(cells.size() - 1, i + kSmoothWindow);
        maxCost.assign(1, g.cost(c, cells[i]));
        for (std::size_t j = i + 1; j <= end; ++j) maxCost.push_back(std::max(maxCost.back(), g.cost(c, cells[j])));
        // The farthest visible cell in the window (the next cell is always reachable).
        std::size_t best = i + 1;
        for (std::size_t j = end; j > i + 1; --j) {
            if (g.lineOfSight(c, cells[i], cells[j], maxCost[j - i])) {
                best = j;
                break;
            }
        }
        out.push_back(cells[best]);
        i = best;
    }
    return out;
}

std::uint32_t PathService::request(EntityId who, MoveClass c, std::uint32_t start, std::uint32_t goal, std::uint32_t tick) {
    PathRequest r;
    r.id = nextId++;
    r.tick = tick;
    r.requester = who;
    r.cls = c;
    r.start = start;
    r.goal = goal;
    queue.insert(std::upper_bound(queue.begin(), queue.end(), r, queueLess), r);
    return r.id;
}

void PathService::step(Hpa& hpa, std::int32_t budget, std::vector<PathResult>& out) {
    std::int32_t left = budget - debt_;
    debt_ = 0;
    while (left > 0) {
        if (!active) {
            if (queue.empty()) break;
            job = Job{};
            job.req = queue.front();
            queue.erase(queue.begin());
            active = true;
        }
        left -= static_cast<std::int32_t>(runUnit(hpa, job));
        if (job.phase == 2) {
            out.push_back(finish(hpa, job));
            active = false;
        }
    }
    if (left < 0) debt_ = -left;
}

PathResult PathService::solveNow(Hpa& hpa, MoveClass c, std::uint32_t start, std::uint32_t goal) {
    Job j;
    j.req.cls = c;
    j.req.start = start;
    j.req.goal = goal;
    while (j.phase != 2) runUnit(hpa, j);
    return finish(hpa, j);
}

std::uint32_t PathService::runUnit(Hpa& hpa, Job& j) {
    if (j.phase == 1 && j.navVersion != hpa.version()) {  // the graph changed under us: start over
        j.phase = 0;
        j.abstract.clear();
        j.cells.clear();
        j.segment = 0;
    }
    std::uint32_t spent = 0;
    if (j.phase == 0) {
        spent = resolveAndSearch(hpa, j);
    } else if (j.phase == 1) {
        spent = refineSegment(hpa, j);
    }
    j.expansions += spent;
    return std::max<std::uint32_t>(spent, 1);  // every unit costs at least 1, so a tick always ends
}

std::uint32_t PathService::resolveAndSearch(Hpa& hpa, Job& j) {
    const NavGrid& g = hpa.grid();
    const MoveClass c = j.req.cls;
    j.navVersion = hpa.version();
    std::uint32_t spent = 0;
    const std::uint32_t n = static_cast<std::uint32_t>(g.width()) * static_cast<std::uint32_t>(g.height());
    if (j.req.start >= n || j.req.goal >= n) {
        j.status = PathStatus::NoStart;
        j.phase = 2;
        return 1;
    }
    // Endpoints
    j.start = j.req.start;
    if (g.cost(c, j.start) == 0 &&
        !nearestCell(g, j.req.start, wholeMap(g), [&](std::uint32_t cell) { return g.cost(c, cell) != 0; }, j.start, spent)) {
        j.status = PathStatus::NoStart;
        j.phase = 2;
        return spent;
    }
    const std::uint32_t lab = g.label(c, j.start);
    j.goal = j.req.goal;
    j.status = PathStatus::Found;
    if (g.label(c, j.goal) != lab) {
        if (!nearestCell(g, j.req.goal, g.labelBox(c, lab), [&](std::uint32_t cell) { return g.label(c, cell) == lab; }, j.goal,
                         spent)) {
            j.status = PathStatus::Unreachable;
            j.phase = 2;
            return spent;
        }
        j.status = PathStatus::FoundNearest;
    }
    if (j.start == j.goal) {
        j.abstract = {j.start};
        j.cells = {j.start};
        j.cost = 0;
        j.phase = 2;
        return spent + 1;
    }

    // Insert start and goal into the abstract graph (bounded Dijkstra inside their clusters).
    const std::uint32_t sCl = g.clusterOf(j.start), gCl = g.clusterOf(j.goal);
    const ClusterGraph& sg = hpa.cluster(c, sCl);
    const ClusterGraph& gg = hpa.cluster(c, gCl);
    std::vector<std::int32_t> sCost, gCost;
    spent += gridDijkstra(g, c, j.start, g.clusterRect(sCl), sg.nodes, sCost, gridScratch);
    spent += gridDijkstra(g, c, j.goal, g.clusterRect(gCl), gg.nodes, gCost, gridScratch);

    // A* on the abstract graph from S to G. Nodes are cells; heap keyed on (f, h, cell).
    absScratch.prepare(n);
    absScratch.begin();
    const std::int32_t hScale = 2 * g.minCost(c);
    const std::int32_t gx = g.xOf(j.goal), gy = g.yOf(j.goal);
    const auto heur = [&](std::uint32_t cell) { return octile(g.xOf(cell) - gx, g.yOf(cell) - gy) * hScale; };
    std::vector<OpenEntry> open;
    const auto relax = [&](std::uint32_t from, std::uint32_t to, std::int32_t gv) {
        if (absScratch.closed(to) || gv >= absScratch.g(to)) return;
        absScratch.set(to, gv, from);
        const std::int32_t hh = heur(to);
        open.push_back({gv + hh, hh, to});
        std::push_heap(open.begin(), open.end(), OpenGreater{});
    };
    absScratch.set(j.start, 0, j.start);
    open.push_back({heur(j.start), heur(j.start), j.start});
    std::int32_t found = INT32_MAX;
    while (!open.empty()) {
        std::pop_heap(open.begin(), open.end(), OpenGreater{});
        const OpenEntry e = open.back();
        open.pop_back();
        const std::int32_t gv = e.f - e.h;
        if (absScratch.closed(e.cell) || gv != absScratch.g(e.cell)) continue;
        absScratch.close(e.cell);
        ++spent;
        if (e.cell == j.goal) {
            found = gv;
            break;
        }
        if (e.cell == j.start) {
            for (std::size_t i = 0; i < sg.nodes.size(); ++i) {
                if (sCost[i] != INT32_MAX) relax(e.cell, sg.nodes[i], sCost[i]);
            }
        }
        const std::int32_t ni = hpa.nodeIndex(c, e.cell);
        if (ni >= 0) {
            const ClusterGraph& cg = hpa.cluster(c, g.clusterOf(e.cell));
            for (const HpaEdge& ed : cg.edges[static_cast<std::size_t>(ni)]) relax(e.cell, ed.cell, gv + ed.cost);
            if (g.clusterOf(e.cell) == gCl) {
                const auto it = std::lower_bound(gg.nodes.begin(), gg.nodes.end(), e.cell);
                const auto gi = static_cast<std::size_t>(it - gg.nodes.begin());
                if (it != gg.nodes.end() && *it == e.cell && gCost[gi] != INT32_MAX) relax(e.cell, j.goal, gv + gCost[gi]);
            }
        }
    }
    // Short requests: HPA*'s detours through entrances cost the most here (relative to the path),
    // so also run a plain A* in a box around start and goal and keep the cheaper result.
    const std::int32_t sx = g.xOf(j.start), sy = g.yOf(j.start), tx = g.xOf(j.goal), ty = g.yOf(j.goal);
    if (std::max(std::abs(sx - tx), std::abs(sy - ty)) <= kLocalRange) {
        const CellRect box{std::max(std::min(sx, tx) - kClusterSize, 0), std::max(std::min(sy, ty) - kClusterSize, 0),
                           std::min(std::max(sx, tx) + kClusterSize, g.width() - 1),
                           std::min(std::max(sy, ty) + kClusterSize, g.height() - 1)};
        GridPath local = gridAStar(g, c, j.start, j.goal, box, gridScratch);
        spent += local.expansions;
        if (local.found && local.cost <= found) {
            j.abstract = {j.start, j.goal};
            j.cells = std::move(local.cells);
            j.cost = local.cost;
            j.phase = 2;
            return spent;
        }
    }
    if (found != INT32_MAX) {
        j.abstract.clear();
        for (std::uint32_t cur = j.goal;; cur = absScratch.parentOf(cur)) {
            j.abstract.push_back(cur);
            if (cur == j.start) break;
        }
        std::reverse(j.abstract.begin(), j.abstract.end());
        j.cost = found;
    } else {
        // Same label but no abstract route: cannot happen with consistent labels; fail safe.
        j.status = PathStatus::Unreachable;
        j.phase = 2;
        return spent;
    }
    j.cells = {j.start};
    j.segment = 0;
    j.phase = 1;
    return spent;
}

std::uint32_t PathService::refineSegment(Hpa& hpa, Job& j) {
    const NavGrid& g = hpa.grid();
    const std::uint32_t a = j.abstract[j.segment];
    const std::uint32_t b = j.abstract[j.segment + 1];
    std::uint32_t spent = 1;
    if (g.clusterOf(a) != g.clusterOf(b)) {
        j.cells.push_back(b);  // inter edge: one step across the border
    } else {
        const GridPath p = gridAStar(g, j.req.cls, a, b, g.clusterRect(g.clusterOf(a)), gridScratch);
        spent = p.expansions;
        if (!p.found) {  // graph out of date in a way the version didn't catch: restart
            j.phase = 0;
            return spent;
        }
        j.cells.insert(j.cells.end(), p.cells.begin() + 1, p.cells.end());
    }
    if (++j.segment + 1 >= j.abstract.size()) j.phase = 2;
    return spent;
}

PathResult PathService::finish(const Hpa& hpa, Job& j) const {
    PathResult r;
    r.requestId = j.req.id;
    r.requester = j.req.requester;
    r.cls = j.req.cls;
    r.status = j.status;
    r.cost = j.cost;
    r.expansions = j.expansions;
    r.goal = j.goal;
    if (j.status == PathStatus::Found || j.status == PathStatus::FoundNearest) {
        r.waypoints = smoothPath(hpa.grid(), j.req.cls, j.cells);
        r.cells = j.cells;
    }
    return r;
}

// ------------------------------------------------------------------------------- state

namespace {
void putReq(ByteWriter& w, const PathRequest& r) {
    w.u32(r.id);
    w.u32(r.tick);
    w.entity(r.requester);
    w.u8(static_cast<std::uint8_t>(r.cls));
    w.u32(r.start);
    w.u32(r.goal);
}
PathRequest getReq(ByteReader& r) {
    PathRequest q;
    q.id = r.u32();
    q.tick = r.u32();
    q.requester = r.entity();
    q.cls = static_cast<MoveClass>(r.u8());
    q.start = r.u32();
    q.goal = r.u32();
    return q;
}
void putCells(ByteWriter& w, const std::vector<std::uint32_t>& v) {
    w.u32(static_cast<std::uint32_t>(v.size()));
    for (const std::uint32_t c : v) w.u32(c);
}
bool getCells(ByteReader& r, std::vector<std::uint32_t>& v) {
    const std::uint32_t n = r.count(4);
    v.clear();
    for (std::uint32_t i = 0; i < n && r.ok; ++i) v.push_back(r.u32());
    return r.ok;
}
}  // namespace

void PathService::save(ByteWriter& w) const {
    w.u32(nextId);
    w.i64(debt_);
    w.u32(static_cast<std::uint32_t>(queue.size()));
    for (const PathRequest& q : queue) putReq(w, q);
    w.boolean(active);
    if (active) {
        putReq(w, job.req);
        w.u32(job.navVersion);
        w.u8(job.phase);
        w.u8(static_cast<std::uint8_t>(job.status));
        w.u32(job.start);
        w.u32(job.goal);
        putCells(w, job.abstract);
        w.u32(static_cast<std::uint32_t>(job.segment));
        putCells(w, job.cells);
        w.i64(job.cost);
        w.u32(job.expansions);
    }
}

bool PathService::load(ByteReader& r) {
    PathService s;
    s.nextId = r.u32();
    s.debt_ = static_cast<std::int32_t>(r.i64());
    const std::uint32_t n = r.count(21);
    for (std::uint32_t i = 0; i < n && r.ok; ++i) s.queue.push_back(getReq(r));
    s.active = r.boolean();
    if (s.active) {
        s.job.req = getReq(r);
        s.job.navVersion = r.u32();
        s.job.phase = r.u8();
        s.job.status = static_cast<PathStatus>(r.u8());
        s.job.start = r.u32();
        s.job.goal = r.u32();
        if (!getCells(r, s.job.abstract)) return false;
        s.job.segment = r.u32();
        if (!getCells(r, s.job.cells)) return false;
        s.job.cost = static_cast<std::int32_t>(r.i64());
        s.job.expansions = r.u32();
        if (s.job.phase > 1 || (s.job.phase == 1 && s.job.segment + 1 >= s.job.abstract.size())) return false;
    }
    if (!r.ok || !std::is_sorted(s.queue.begin(), s.queue.end(), queueLess)) return false;
    queue = std::move(s.queue);
    active = s.active;
    job = std::move(s.job);
    debt_ = s.debt_;
    nextId = s.nextId;
    return true;
}

}  // namespace corps::nav
