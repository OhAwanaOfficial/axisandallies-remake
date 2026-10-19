#pragma once

// Runtime nav grid (PATHFINDING §2.1): static terrain cost per movement class, building stamps
// (ref-counted per cell, any stamp blocks every class), connected-component labels per class, and
// integer line of sight. Units are not in the grid (local avoidance handles them, §6).
//
// Moves are 8-way. A diagonal step is allowed only when both orthogonal neighbours are passable
// (no corner cutting); component labels use the same rule, so "same label" means "reachable".
// Step cost = (10 straight | 14 diagonal) * (cost[from] + cost[to]), all int32.

#include <array>
#include <cstdint>
#include <vector>

#include "nav/map_def.hpp"

namespace corps::nav {

constexpr std::int32_t kClusterSize = 16;  // cells per cluster side (PATHFINDING §3)
constexpr std::int32_t kStraight = 10;
constexpr std::int32_t kDiagonal = 14;

// dx, dy for the 8 directions: E, W, S, N, SE, SW, NE, NW (orthogonals first).
constexpr std::int32_t kDirX[8] = {1, -1, 0, 0, 1, -1, 1, -1};
constexpr std::int32_t kDirY[8] = {0, 0, 1, -1, 1, 1, -1, -1};

struct CellRect {
    std::int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;  // inclusive; empty when x1 < x0
    bool empty() const { return x1 < x0 || y1 < y0; }
};

class NavGrid {
public:
    explicit NavGrid(const MapDef& map);

    std::int32_t width() const { return w; }
    std::int32_t height() const { return h; }
    std::uint32_t index(std::int32_t x, std::int32_t y) const { return static_cast<std::uint32_t>(y) * static_cast<std::uint32_t>(w) + static_cast<std::uint32_t>(x); }
    std::int32_t xOf(std::uint32_t cell) const { return static_cast<std::int32_t>(cell % static_cast<std::uint32_t>(w)); }
    std::int32_t yOf(std::uint32_t cell) const { return static_cast<std::int32_t>(cell / static_cast<std::uint32_t>(w)); }
    bool inside(std::int32_t x, std::int32_t y) const { return x >= 0 && y >= 0 && x < w && y < h; }

    // Effective cost (0 = blocked) including stamps.
    std::uint8_t cost(MoveClass c, std::uint32_t cell) const {
        return stamps[cell] != 0 ? 0 : base[static_cast<std::size_t>(c)][cell];
    }
    bool passable(MoveClass c, std::int32_t x, std::int32_t y) const { return inside(x, y) && cost(c, index(x, y)) != 0; }
    // Cost of stepping from (x, y) in direction dir (0..7), or 0 if not allowed.
    std::int32_t stepCost(MoveClass c, std::int32_t x, std::int32_t y, int dir) const;
    // Smallest non-zero static cost of a class (for the admissible heuristic).
    std::int32_t minCost(MoveClass c) const { return minCost_[static_cast<std::size_t>(c)]; }

    // Building stamps. Every touched cluster becomes dirty (rebuilt by Hpa::rebuildDirty).
    void stamp(const CellRect& r);
    void unstamp(const CellRect& r);
    std::uint16_t stampCount(std::uint32_t cell) const { return stamps[cell]; }
    const std::vector<std::uint32_t>& dirtyClusters() const { return dirty; }  // ascending, unique
    void clearDirty() { dirty.clear(); }
    void markDirty(std::uint32_t cluster);

    // Component labels (0 = blocked cell). Recomputed for all classes on demand.
    void recomputeLabels();
    std::uint32_t label(MoveClass c, std::uint32_t cell) const { return labels[static_cast<std::size_t>(c)][cell]; }
    // Bounding box of a component (label != 0).
    const CellRect& labelBox(MoveClass c, std::uint32_t lab) const { return labelBoxes[static_cast<std::size_t>(c)][lab]; }

    // Line of sight for path smoothing: every cell the segment between the two cell centres
    // touches (supercover) must be passable with cost <= maxCost; at an exact corner crossing both
    // side cells must be. Integer only.
    bool lineOfSight(MoveClass c, std::uint32_t from, std::uint32_t to, std::uint8_t maxCost) const;

    // Clusters
    std::int32_t clustersX() const { return cw; }
    std::int32_t clustersY() const { return ch; }
    std::uint32_t clusterOf(std::uint32_t cell) const {
        return static_cast<std::uint32_t>((yOf(cell) / kClusterSize) * cw + xOf(cell) / kClusterSize);
    }
    CellRect clusterRect(std::uint32_t cluster) const;

private:
    void touch(const CellRect& r);

    std::int32_t w = 0, h = 0, cw = 0, ch = 0;
    std::array<std::vector<std::uint8_t>, kMoveClassCount> base;
    std::array<std::int32_t, kMoveClassCount> minCost_{};
    std::vector<std::uint16_t> stamps;
    std::array<std::vector<std::uint32_t>, kMoveClassCount> labels;
    std::array<std::vector<CellRect>, kMoveClassCount> labelBoxes;
    std::vector<std::uint32_t> dirty;
};

// Octile distance in step units (straight 10, diagonal 14).
inline std::int32_t octile(std::int32_t dx, std::int32_t dy) {
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;
    return dx > dy ? kStraight * (dx - dy) + kDiagonal * dy : kStraight * (dy - dx) + kDiagonal * dx;
}

}  // namespace corps::nav
