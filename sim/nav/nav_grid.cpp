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
        cellKey[c].assign(n, 0);
        keyLabel[c].assign(static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch) * kLocalSlots, 0);
        localBoxes[c].resize(static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch));
        borderLinks[c].resize(static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch));
    }
    recomputeAllLabels();
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
    for (std::size_t ci = 0; ci < kMoveClassCount; ++ci) {
        if (!labelsStale[ci]) continue;
        labelsStale[ci] = false;
        // Only dirty clusters' cells changed. Their links are on their own E / S borders and on
        // the W neighbour's E and the N neighbour's S border.
        for (const std::uint32_t k : dirty) labelCluster(ci, k);
        for (const std::uint32_t k : dirty) {
            linkBorders(ci, k);
            if (static_cast<std::int32_t>(k) % cw > 0) linkBorders(ci, k - 1);
            if (static_cast<std::int32_t>(k) >= cw) linkBorders(ci, k - static_cast<std::uint32_t>(cw));
        }
        joinLabels(ci);
    }
}

void NavGrid::recomputeAllLabels() {
    const auto clusters = static_cast<std::uint32_t>(cw * ch);
    for (std::size_t ci = 0; ci < kMoveClassCount; ++ci) {
        labelsStale[ci] = false;
        for (std::uint32_t k = 0; k < clusters; ++k) labelCluster(ci, k);
        for (std::uint32_t k = 0; k < clusters; ++k) linkBorders(ci, k);
        joinLabels(ci);
    }
}

void NavGrid::labelCluster(std::size_t ci, std::uint32_t k) {
    // Components are 4-connected: a diagonal step needs both orthogonal cells open, so it never
    // connects anything the two orthogonal steps don't. One raster pass over the cluster links
    // every open cell to its open W / N neighbour (union-find whose root is always the set's
    // lowest cell), a second pass numbers the roots in order and fills the local boxes.
    const CellRect r = clusterRect(k);
    const std::int32_t rw = r.x1 - r.x0 + 1, rh = r.y1 - r.y0 + 1;
    std::uint16_t up[kClusterSize * kClusterSize];
    std::uint8_t local[kClusterSize * kClusterSize];
    const auto find = [&](std::uint16_t a) {
        while (up[a] != a) {
            up[a] = up[up[a]];  // path halving
            a = up[a];
        }
        return a;
    };
    const std::uint8_t* cost = eff[ci].data();
    const auto uw = static_cast<std::uint32_t>(w);
    for (std::int32_t y = 0; y < rh; ++y) {
        const std::uint32_t row = index(r.x0, r.y0 + y);
        for (std::int32_t x = 0; x < rw; ++x) {
            const std::uint32_t cell = row + static_cast<std::uint32_t>(x);
            const auto i = static_cast<std::uint16_t>(y * rw + x);
            const bool west = x > 0 && cost[cell] != 0 && cost[cell - 1] != 0;
            const bool north = y > 0 && cost[cell] != 0 && cost[cell - uw] != 0;
            if (west && north) {
                // Already one set when the NW cell is open (W-NW and NW-N are links).
                if (cost[cell - uw - 1] != 0) {
                    up[i] = up[i - 1];
                } else {
                    const std::uint16_t a = find(static_cast<std::uint16_t>(i - 1));
                    const std::uint16_t b = find(static_cast<std::uint16_t>(i - rw));
                    const std::uint16_t lo = std::min(a, b);
                    up[a] = lo;
                    up[b] = lo;
                    up[i] = lo;
                }
            } else if (west) {
                up[i] = up[i - 1];
            } else if (north) {
                up[i] = up[i - rw];
            } else {
                up[i] = i;
            }
        }
    }
    // up[i] < i for every non-root, so in ascending order up[up[i]] is already a root.
    std::vector<CellRect>& boxes = localBoxes[ci][k];
    boxes.clear();
    std::uint32_t* key = cellKey[ci].data();
    const std::uint32_t keyBase = k * kLocalSlots;
    for (std::int32_t y = 0; y < rh; ++y) {
        const std::uint32_t row = index(r.x0, r.y0 + y);
        for (std::int32_t x = 0; x < rw; ++x) {
            const std::uint32_t cell = row + static_cast<std::uint32_t>(x);
            const auto i = static_cast<std::uint16_t>(y * rw + x);
            if (cost[cell] == 0) {
                key[cell] = keyBase;
                continue;
            }
            if (up[i] == i) {
                boxes.push_back(CellRect{r.x0 + x, r.y0 + y, r.x0 + x, r.y0 + y});
                local[i] = static_cast<std::uint8_t>(boxes.size());
                key[cell] = keyBase + local[i];
                continue;
            }
            up[i] = up[up[i]];
            const std::uint8_t l = local[up[i]];
            local[i] = l;
            key[cell] = keyBase + l;
            CellRect& box = boxes[l - 1u];
            box.x0 = std::min(box.x0, r.x0 + x);
            box.x1 = std::max(box.x1, r.x0 + x);
            box.y1 = r.y0 + y;  // rows ascend; y0 is the root's row
        }
    }
}

void NavGrid::linkBorders(std::size_t ci, std::uint32_t k) {
    // Open cells facing each other across the E and S borders join their local components. A
    // run of open pairs usually lies in one component per side, so repeats of the last pair are
    // skipped.
    std::vector<std::pair<std::uint32_t, std::uint32_t>>& links = borderLinks[ci][k];
    links.clear();
    const std::uint8_t* cost = eff[ci].data();
    const std::uint32_t* key = cellKey[ci].data();
    const CellRect r = clusterRect(k);
    const auto add = [&](std::uint32_t a, std::uint32_t b) {
        if (cost[a] == 0 || cost[b] == 0) return;
        const std::pair<std::uint32_t, std::uint32_t> p{key[a], key[b]};
        if (links.empty() || links.back() != p) links.push_back(p);
    };
    if (r.x1 + 1 < w) {
        for (std::int32_t y = r.y0; y <= r.y1; ++y) add(index(r.x1, y), index(r.x1 + 1, y));
    }
    if (r.y1 + 1 < h) {
        for (std::int32_t x = r.x0; x <= r.x1; ++x) add(index(x, r.y1), index(x, r.y1 + 1));
    }
}

void NavGrid::joinLabels(std::size_t ci) {
    // Union-find over the local components (root = the set's lowest key), then number the sets
    // in ascending key order and merge their local boxes. A component's box is the union of its
    // local boxes, so it is exact.
    const auto clusters = static_cast<std::uint32_t>(cw * ch);
    std::vector<std::uint32_t>& up = keyParent;
    up.resize(static_cast<std::size_t>(clusters) * kLocalSlots);
    const auto find = [&](std::uint32_t a) {
        while (up[a] != a) {
            up[a] = up[up[a]];  // path halving
            a = up[a];
        }
        return a;
    };
    for (std::uint32_t k = 0; k < clusters; ++k) {
        const std::uint32_t keyBase = k * kLocalSlots;
        const auto n = static_cast<std::uint32_t>(localBoxes[ci][k].size());
        for (std::uint32_t l = 1; l <= n; ++l) up[keyBase + l] = keyBase + l;
    }
    for (std::uint32_t k = 0; k < clusters; ++k) {
        for (const auto& [p, q] : borderLinks[ci][k]) {
            const std::uint32_t a = find(p), b = find(q);
            if (a < b) {
                up[b] = a;
            } else if (b < a) {
                up[a] = b;
            }
        }
    }
    std::vector<std::uint32_t>& lab = keyLabel[ci];
    std::vector<CellRect>& boxes = labelBoxes[ci];
    boxes.assign(1, CellRect{});  // label 0 = blocked
    for (std::uint32_t k = 0; k < clusters; ++k) {
        const std::uint32_t keyBase = k * kLocalSlots;
        lab[keyBase] = 0;
        const std::vector<CellRect>& local = localBoxes[ci][k];
        for (std::uint32_t l = 1; l <= local.size(); ++l) {
            const std::uint32_t key = keyBase + l;
            const std::uint32_t root = find(key);
            const CellRect& lb = local[l - 1];
            if (root == key) {  // a set's lowest key comes first, so it opens the label
                lab[key] = static_cast<std::uint32_t>(boxes.size());
                boxes.push_back(lb);
                continue;
            }
            const std::uint32_t id = lab[root];
            lab[key] = id;
            CellRect& box = boxes[id];
            box.x0 = std::min(box.x0, lb.x0);
            box.y0 = std::min(box.y0, lb.y0);
            box.x1 = std::max(box.x1, lb.x1);
            box.y1 = std::max(box.y1, lb.y1);
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
