#pragma once

// Hierarchical A* abstract graph (PATHFINDING §3): 16x16-cell clusters; for every movement class,
// entrance nodes on cluster borders and intra-cluster edges from bounded Dijkstra runs.
//
// Entrances: along each shared border, maximal runs of cell pairs (one cell each side) with the
// same non-zero crossing cost become entrances: one in the middle of a run shorter than 6, one at
// each end of longer runs. Splitting runs by cost keeps roads and forest edges as their own
// entrances (otherwise HPA* can't follow a road across a border). Each entrance gives a node on both sides joined by an inter edge (one straight step).
// Nodes are identified by their cell index, so rebuilding a cluster never renumbers anything else.

#include <array>
#include <cstdint>
#include <vector>

#include "nav/grid_search.hpp"
#include "nav/nav_grid.hpp"

namespace corps::nav {

struct HpaEdge {
    std::uint32_t cell = 0;  // target node cell
    std::int32_t cost = 0;
};

struct ClusterGraph {
    std::vector<std::uint32_t> nodes;               // ascending cell indexes
    std::vector<std::vector<HpaEdge>> edges;        // per node: intra edges, then inter edges
};

class Hpa {
public:
    explicit Hpa(NavGrid& grid);

    // Rebuilds the clusters the grid marked dirty plus their neighbours (ascending cluster order),
    // recomputes component labels, clears the dirty set and bumps version(). No-op if nothing dirty.
    void rebuildDirty();
    void rebuildAll();

    std::uint32_t version() const { return version_; }
    void restoreVersion(std::uint32_t v) { version_ = v; }

    const ClusterGraph& cluster(MoveClass c, std::uint32_t cluster) const { return graphs[static_cast<std::size_t>(c)][cluster]; }
    // Index of `cell` among its cluster's nodes, or -1.
    std::int32_t nodeIndex(MoveClass c, std::uint32_t cell) const;
    std::size_t nodeCount(MoveClass c) const;
    std::size_t edgeCount(MoveClass c) const;
    const NavGrid& grid() const { return grid_; }

private:
    void buildCluster(MoveClass c, std::uint32_t cluster);
    // Entrance cell pairs on the border between two adjacent clusters (lo is left/top of hi).
    // `sideBySide`: lo is left of hi (column border); otherwise lo is above hi (row border).
    void borderEntrances(MoveClass c, std::uint32_t lo, std::uint32_t hi, bool sideBySide,
                         std::vector<std::pair<std::uint32_t, std::uint32_t>>& out) const;

    NavGrid& grid_;
    std::array<std::vector<ClusterGraph>, kMoveClassCount> graphs;
    std::uint32_t version_ = 0;
    SearchScratch scratch;
};

}  // namespace corps::nav
