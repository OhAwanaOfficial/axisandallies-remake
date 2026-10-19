#pragma once

// Everything the sim owns for navigation on one map: the runtime grid (terrain + building stamps),
// the HPA* graph over it and the path request service. Built from a MapDef; pinned in memory
// (Hpa refers to the grid), so Sim holds it by unique_ptr.

#include "nav/hpa.hpp"
#include "nav/map_def.hpp"
#include "nav/nav_grid.hpp"
#include "nav/path_service.hpp"

namespace corps::nav {

struct NavWorld {
    explicit NavWorld(const MapDef& map) : grid(map), hpa(grid) {}
    NavWorld(const NavWorld&) = delete;
    NavWorld& operator=(const NavWorld&) = delete;

    NavGrid grid;
    Hpa hpa;
    PathService paths;
};

}  // namespace corps::nav
