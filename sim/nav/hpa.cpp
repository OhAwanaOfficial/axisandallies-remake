#include "nav/hpa.hpp"

#include <algorithm>

namespace corps::nav {

namespace {
constexpr std::int32_t kLongRun = 6;  // runs this long or longer get an entrance at each end
}

Hpa::Hpa(NavGrid& grid) : grid_(grid) {
    for (auto& g : graphs) g.resize(static_cast<std::size_t>(grid_.clustersX()) * static_cast<std::size_t>(grid_.clustersY()));
    rebuildAll();
}

void Hpa::rebuildAll() {
    for (std::size_t ci = 0; ci < kMoveClassCount; ++ci) {
        for (std::uint32_t k = 0; k < graphs[ci].size(); ++k) buildCluster(static_cast<MoveClass>(ci), k);
    }
    grid_.recomputeLabels();
    grid_.clearDirty();
}

void Hpa::rebuildDirty() {
    if (grid_.dirtyClusters().empty()) return;
    // Dirty clusters plus their 4 neighbours (shared borders), ascending and unique.
    std::vector<std::uint32_t> todo;
    const std::int32_t cw = grid_.clustersX(), chh = grid_.clustersY();
    for (const std::uint32_t k : grid_.dirtyClusters()) {
        const std::int32_t cx = static_cast<std::int32_t>(k) % cw, cy = static_cast<std::int32_t>(k) / cw;
        todo.push_back(k);
        if (cx > 0) todo.push_back(k - 1);
        if (cx + 1 < cw) todo.push_back(k + 1);
        if (cy > 0) todo.push_back(k - static_cast<std::uint32_t>(cw));
        if (cy + 1 < chh) todo.push_back(k + static_cast<std::uint32_t>(cw));
    }
    std::sort(todo.begin(), todo.end());
    todo.erase(std::unique(todo.begin(), todo.end()), todo.end());
    for (std::size_t ci = 0; ci < kMoveClassCount; ++ci) {
        for (const std::uint32_t k : todo) buildCluster(static_cast<MoveClass>(ci), k);
    }
    grid_.recomputeLabels();
    grid_.clearDirty();
    ++version_;
}

void Hpa::borderEntrances(MoveClass c, std::uint32_t lo, std::uint32_t hi, bool sideBySide,
                          std::vector<std::pair<std::uint32_t, std::uint32_t>>& out) const {
    const CellRect a = grid_.clusterRect(lo);
    const CellRect b = grid_.clusterRect(hi);
    const bool vertical = sideBySide;  // side by side: the border is a pair of columns
    const std::int32_t len = vertical ? (a.y1 - a.y0 + 1) : (a.x1 - a.x0 + 1);
    const auto cellsAt = [&](std::int32_t i) {
        return vertical ? std::pair{grid_.index(a.x1, a.y0 + i), grid_.index(b.x0, a.y0 + i)}
                        : std::pair{grid_.index(a.x0 + i, a.y1), grid_.index(a.x0 + i, b.y0)};
    };
    // Crossing cost at border position i (0 = closed). A run is a maximal stretch of equal crossing
    // cost, so a road crossing the border gets its own entrance instead of being averaged away.
    const auto crossing = [&](std::int32_t i) {
        const auto p = cellsAt(i).first;
        return grid_.stepCost(c, grid_.xOf(p), grid_.yOf(p), vertical ? 0 : 2);  // E or S
    };
    std::int32_t i = 0;
    while (i < len) {
        const std::int32_t k = crossing(i);
        if (k == 0) {
            ++i;
            continue;
        }
        std::int32_t j = i;
        while (j + 1 < len && crossing(j + 1) == k) ++j;
        if (j - i + 1 < kLongRun) {
            out.push_back(cellsAt((i + j) / 2));
        } else {
            out.push_back(cellsAt(i));
            out.push_back(cellsAt(j));
        }
        i = j + 1;
    }
}

void Hpa::buildCluster(MoveClass c, std::uint32_t k) {
    ClusterGraph& cg = graphs[static_cast<std::size_t>(c)][k];
    cg.nodes.clear();
    cg.edges.clear();
    const std::int32_t cw = grid_.clustersX(), chh = grid_.clustersY();
    const std::int32_t cx = static_cast<std::int32_t>(k) % cw, cy = static_cast<std::int32_t>(k) / cw;
    // (own cell, partner cell) for every entrance on this cluster's borders
    std::vector<std::pair<std::uint32_t, std::uint32_t>> mine;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> tmp;
    const auto collect = [&](std::uint32_t lo, std::uint32_t hi, bool sideBySide, bool iAmLo) {
        tmp.clear();
        borderEntrances(c, lo, hi, sideBySide, tmp);
        for (const auto& [p, q] : tmp) mine.emplace_back(iAmLo ? p : q, iAmLo ? q : p);
    };
    if (cx > 0) collect(k - 1, k, true, false);
    if (cx + 1 < cw) collect(k, k + 1, true, true);
    if (cy > 0) collect(k - static_cast<std::uint32_t>(cw), k, false, false);
    if (cy + 1 < chh) collect(k, k + static_cast<std::uint32_t>(cw), false, true);

    for (const auto& e : mine) cg.nodes.push_back(e.first);
    std::sort(cg.nodes.begin(), cg.nodes.end());
    cg.nodes.erase(std::unique(cg.nodes.begin(), cg.nodes.end()), cg.nodes.end());
    cg.edges.assign(cg.nodes.size(), {});

    // Intra edges: bounded Dijkstra from every node to the others (symmetric costs).
    const CellRect rect = grid_.clusterRect(k);
    std::vector<std::int32_t> costs;
    for (std::size_t i = 0; i < cg.nodes.size(); ++i) {
        gridDijkstra(grid_, c, cg.nodes[i], rect, cg.nodes, costs, scratch);
        for (std::size_t j = 0; j < cg.nodes.size(); ++j) {
            if (j != i && costs[j] != INT32_MAX) cg.edges[i].push_back({cg.nodes[j], costs[j]});
        }
    }
    // Inter edges to partner nodes in neighbour clusters (one straight step).
    std::sort(mine.begin(), mine.end());
    for (const auto& [own, partner] : mine) {
        const auto it = std::lower_bound(cg.nodes.begin(), cg.nodes.end(), own);
        const std::size_t i = static_cast<std::size_t>(it - cg.nodes.begin());
        const std::int32_t dx = grid_.xOf(partner) - grid_.xOf(own), dy = grid_.yOf(partner) - grid_.yOf(own);
        int dir = 0;
        for (int d = 0; d < 4; ++d) {
            if (kDirX[d] == dx && kDirY[d] == dy) dir = d;
        }
        cg.edges[i].push_back({partner, grid_.stepCost(c, grid_.xOf(own), grid_.yOf(own), dir)});
    }
}

std::int32_t Hpa::nodeIndex(MoveClass c, std::uint32_t cell) const {
    const ClusterGraph& cg = cluster(c, grid_.clusterOf(cell));
    const auto it = std::lower_bound(cg.nodes.begin(), cg.nodes.end(), cell);
    return (it != cg.nodes.end() && *it == cell) ? static_cast<std::int32_t>(it - cg.nodes.begin()) : -1;
}

std::size_t Hpa::nodeCount(MoveClass c) const {
    std::size_t n = 0;
    for (const auto& cg : graphs[static_cast<std::size_t>(c)]) n += cg.nodes.size();
    return n;
}

std::size_t Hpa::edgeCount(MoveClass c) const {
    std::size_t n = 0;
    for (const auto& cg : graphs[static_cast<std::size_t>(c)]) {
        for (const auto& e : cg.edges) n += e.size();
    }
    return n;
}

}  // namespace corps::nav
