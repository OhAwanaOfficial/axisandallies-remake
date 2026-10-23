#include "systems/stages.hpp"

#include "core/sim.hpp"
#include "nav/nav_world.hpp"

namespace corps {

void runEconomy(Sim&) {}
void runTimers(Sim&) {}

// Rebuild the clusters touched by stamps since last tick, spend the expansion budget, and hand
// finished paths to their requesters (dropped if the requester died or has asked again since).
void runPathfinding(Sim& sim) {
    nav::NavWorld* nav = sim.nav();
    if (nav == nullptr) return;
    nav->hpa.rebuildDirty();
    std::vector<nav::PathResult> done;
    nav->paths.step(nav->hpa, nav::kPathBudgetPerTick, done);
    World& w = sim.world();
    for (nav::PathResult& r : done) {
        Path* p = w.registry.alive(r.requester) ? w.paths.get(r.requester) : nullptr;
        if (p == nullptr || p->requestId != r.requestId) continue;
        p->status = static_cast<Path::Status>(r.status);
        p->cost = r.cost;
        p->waypoints = std::move(r.waypoints);
    }
}
void runVision(Sim&) {}
void runMorale(Sim&) {}
void runSupply(Sim&) {}
void runCapture(Sim&) {}

}  // namespace corps
