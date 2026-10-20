#include "nav/nav_grid.hpp"

#include <algorithm>

namespace corps::nav {

NavGrid::NavGrid(const MapDef& map)
    : w(static_cast<std::int32_t>(map.width)), h(static_cast<std::int32_t>(map.height)) {
    cw = (w + kClusterSize - 1) / kClusterSize;
    ch = (h + kClusterSize - 1) / kClusterSize;
    const std::size_t n = map.cellCount();
    stamps.assign(n, 0);
    for (std::size_t c = 0; c < kMoveClassCount; ++c) {
        base[c].resize(n);
        std::int32_t lo = 255;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t v = terrainCost(map.kind[i], static_cast<MoveClass>(c));
            base[c][i] = v;
            if (v != 0) lo = std::min<std::int32_t>(lo, v);
        }
        minCost_[c] = lo == 255 ? kCostUnit : lo;
        eff[c] = base[c];
        labels[c].assign(n, 0);
    }
    labelsStale.fill(true);
    recomputeLabels();
}

void NavGrid::refreshEff(std::uint32_t cell) {
    for (std::size_t c = 0; c < kMoveClassCount; ++c) {
        const std::uint8_t v = stamps[cell] != 0 ? 0 : base[c][cell];
        if (eff[c][cell] != v) {
            eff[c][cell] = v;
            labelsStale[c] = true;
        }
    }
}

std::int32_t NavGrid::stepCost(MoveClass c, std::int32_t x, std::int32_t y, int dir) const {
    const std::int32_t nx = x + kDirX[dir];
    const std::int32_t ny = y + kDirY[dir];
    if (!inside(nx, ny)) return 0;
    const std::uint8_t to = cost(c, index(nx, ny));
    if (to == 0) return 0;
    const std::uint8_t from = cost(c, index(x, y));
    if (from == 0) return 0;
    if (dir >= 4) {  // diagonal: both orthogonal neighbours must be passable
        if (!passable(c, nx, y) || !passable(c, x, ny)) return 0;
        return kDiagonal * (from + to);
    }
    return kStraight * (from + to);
}

CellRect NavGrid::clusterRect(std::uint32_t cluster) const {
    const auto cx = static_cast<std::int32_t>(cluster) % cw;
    const auto cy = static_cast<std::int32_t>(cluster) / cw;
    CellRect r;
    r.x0 = cx * kClusterSize;
    r.y0 = cy * kClusterSize;
    r.x1 = std::min(w, r.x0 + kClusterSize) - 1;
    r.y1 = std::min(h, r.y0 + kClusterSize) - 1;
    return r;
}

void NavGrid::markDirty(std::uint32_t cluster) {
    const auto it = std::lower_bound(dirty.begin(), dirty.end(), cluster);
    if (it == dirty.end() || *it != cluster) dirty.insert(it, cluster);
}

void NavGrid::touch(const CellRect& r) {
    // A stamp on a cluster's edge also changes the neighbour's entrances; Hpa rebuilds neighbours
    // of dirty clusters, so marking the clusters the rect overlaps is enough.
    for (std::int32_t cy = r.y0 / kClusterSize; cy <= r.y1 / kClusterSize; ++cy) {
        for (std::int32_t cx = r.x0 / kClusterSize; cx <= r.x1 / kClusterSize; ++cx) {
            markDirty(static_cast<std::uint32_t>(cy * cw + cx));
        }
    }
}

void NavGrid::stamp(const CellRect& in) {
    CellRect r{std::max(0, in.x0), std::max(0, in.y0), std::min(w - 1, in.x1), std::min(h - 1, in.y1)};
    if (r.empty()) return;
    for (std::int32_t y = r.y0; y <= r.y1; ++y) {
        for (std::int32_t x = r.x0; x <= r.x1; ++x) {
            const std::uint32_t cell = index(x, y);
            std::uint16_t& s = stamps[cell];
            if (s != 0xFFFF) ++s;
            refreshEff(cell);
        }
    }
    touch(r);
}

void NavGrid::unstamp(const CellRect& in) {
    CellRect r{std::max(0, in.x0), std::max(0, in.y0), std::min(w - 1, in.x1), std::min(h - 1, in.y1)};
    if (r.empty()) return;
    for (std::int32_t y = r.y0; y <= r.y1; ++y) {
        for (std::int32_t x = r.x0; x <= r.x1; ++x) {
            const std::uint32_t cell = index(x, y);
            std::uint16_t& s = stamps[cell];
            if (s != 0) --s;
            refreshEff(cell);
        }
    }
    touch(r);
}

void NavGrid::recomputeLabels() {
    // Components are 4-connected: a diagonal step needs both orthogonal cells open, so it never
    // connects anything the two orthogonal steps don't. One raster pass links every open cell to
    // its open W / N neighbour (union-find whose root is always the set's lowest cell), a second
    // ascending pass numbers the roots in order and fills the bounding boxes. Labels come out
    // numbered by each component's lowest cell, as an ascending flood-fill scan would number them.
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<std::uint32_t>& up = unionParent;
    up.resize(n);
    const auto find = [&](std::uint32_t a) {
        while (up[a] != a) {
            up[a] = up[up[a]];  // path halving
            a = up[a];
        }
        return a;
    };
    const auto uw = static_cast<std::uint32_t>(w);
    for (std::size_t ci = 0; ci < kMoveClassCount; ++ci) {
        if (!labelsStale[ci]) continue;
        labelsStale[ci] = false;
        const std::uint8_t* cost = eff[ci].data();
        std::vector<std::uint32_t>& lab = labels[ci];
        std::vector<CellRect>& boxes = labelBoxes[ci];
        for (std::int32_t y = 0; y < h; ++y) {
            const std::uint32_t row = static_cast<std::uint32_t>(y) * uw;
            for (std::int32_t x = 0; x < w; ++x) {
                const std::uint32_t i = row + static_cast<std::uint32_t>(x);
                const bool west = x > 0 && cost[i] != 0 && cost[i - 1] != 0;
                const bool north = y > 0 && cost[i] != 0 && cost[i - uw] != 0;
                if (west && north) {
                    // Already one set when the NW cell is open (W-NW and NW-N are links).
                    if (cost[i - uw - 1] != 0) {
                        up[i] = up[i - 1];
                    } else {
                        const std::uint32_t a = find(i - 1), b = find(i - uw);
                        const std::uint32_t lo = std::min(a, b);
                        up[a] = lo;
                        up[b] = lo;
                        up[i] = lo;
                    }
                } else if (west) {
                    up[i] = up[i - 1];
                } else if (north) {
                    up[i] = up[i - uw];
                } else {
                    up[i] = i;
                }
            }
        }
        // up[i] < i for every non-root, so in ascending order up[up[i]] is already a root.
        boxes.assign(1, CellRect{});  // label 0 = blocked
        std::uint32_t next = 1;
        for (std::int32_t y = 0; y < h; ++y) {
            const std::uint32_t row = static_cast<std::uint32_t>(y) * uw;
            for (std::int32_t x = 0; x < w; ++x) {
                const std::uint32_t i = row + static_cast<std::uint32_t>(x);
                if (cost[i] == 0) {
                    lab[i] = 0;
                    continue;
                }
                if (up[i] == i) {
                    lab[i] = next++;
                    boxes.push_back(CellRect{x, y, x, y});
                    continue;
                }
                up[i] = up[up[i]];
                const std::uint32_t l = lab[up[i]];
                lab[i] = l;
                CellRect& box = boxes[l];
                box.x0 = std::min(box.x0, x);
                box.x1 = std::max(box.x1, x);
                box.y1 = y;  // rows ascend; y0 is the root's row
            }
        }
    }
}

bool NavGrid::lineOfSight(MoveClass c, std::uint32_t from, std::uint32_t to, std::uint8_t maxCost) const {
    // Supercover traversal of the segment between cell centres (Dedu's algorithm, integer).
    std::int32_t x = xOf(from), y = yOf(from);
    const std::int32_t x1 = xOf(to), y1 = yOf(to);
    const auto ok = [&](std::int32_t cx, std::int32_t cy) {
        if (!inside(cx, cy)) return false;
        const std::uint8_t v = cost(c, index(cx, cy));
        return v != 0 && v <= maxCost;
    };
    if (!ok(x, y)) return false;
    std::int32_t dx = x1 - x, dy = y1 - y;
    const std::int32_t sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;
    const std::int32_t ddx = 2 * dx, ddy = 2 * dy;
    if (ddx >= ddy) {
        std::int32_t err = dx, errPrev = dx;
        for (std::int32_t i = 0; i < dx; ++i) {
            x += sx;
            err += ddy;
            if (err > ddx) {
                y += sy;
                err -= ddx;
                if (err + errPrev < ddx) {
                    if (!ok(x, y - sy)) return false;
                } else if (err + errPrev > ddx) {
                    if (!ok(x - sx, y)) return false;
                } else {  // exact corner: both side cells
                    if (!ok(x, y - sy) || !ok(x - sx, y)) return false;
                }
            }
            if (!ok(x, y)) return false;
            errPrev = err;
        }
    } else {
        std::int32_t err = dy, errPrev = dy;
        for (std::int32_t i = 0; i < dy; ++i) {
            y += sy;
            err += ddx;
            if (err > ddy) {
                x += sx;
                err -= ddy;
                if (err + errPrev < ddy) {
                    if (!ok(x - sx, y)) return false;
                } else if (err + errPrev > ddy) {
                    if (!ok(x, y - sy)) return false;
                } else {
                    if (!ok(x - sx, y) || !ok(x, y - sy)) return false;
                }
            }
            if (!ok(x, y)) return false;
            errPrev = err;
        }
    }
    return true;
}

}  // namespace corps::nav
