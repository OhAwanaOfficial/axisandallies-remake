#include "systems/orders.hpp"

#include <algorithm>

#include "core/ascii.hpp"
#include "core/sim.hpp"
#include "nav/nav_world.hpp"
#include "systems/combat.hpp"
#include "systems/movement.hpp"
#include "systems/stages.hpp"

namespace corps {

namespace {

// The sender's live regiments among `ids`, ascending and unique.
std::vector<EntityId> validRegiments(const Sim& sim, const Command& c, std::vector<EntityId> ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::vector<EntityId> out;
    const World& w = sim.world();
    for (const EntityId id : ids) {
        const Regiment* r = w.registry.alive(id) ? w.regiments.get(id) : nullptr;
        if (r != nullptr && r->player == c.player && w.orders.has(id)) out.push_back(id);
    }
    return out;
}

void setIdle(Order& o) {
    o.state = Order::State::Idle;
    o.needPath = false;
    o.stateTicks = 0;
    o.window = 0;
    o.progress = Fixed{};
    o.expected = Fixed{};
}

void startOrder(Order& o, FVec2 goal, bool hasFacing, Angle facing) {
    o.state = Order::State::WaitPath;
    o.needPath = true;
    o.goal = goal;
    o.hasFacing = hasFacing;
    o.facing = facing;
    o.blocked = false;
    o.repaths = 0;
    o.waypoint = 0;
    o.stateTicks = 0;
    o.window = 0;
    o.progress = Fixed{};
    o.expected = Fixed{};
}

void cancelRun(Regiment& r) {
    if (r.runRestore != Regiment::kNone) {
        r.formation = r.runRestore;
        r.runRestore = Regiment::kNone;
    }
}

void startRun(const Sim& sim, Regiment& r) {
    if (sim.config().defs == nullptr || r.layout == kNoDef) return;
    const std::uint32_t col = sim.config().defs->layouts[r.layout].formation("regiment_column");
    if (col == kNoDef) return;
    if (r.runRestore == Regiment::kNone) r.runRestore = r.formation;
    r.formation = static_cast<std::uint16_t>(col);
}

FVec2 clampToMap(const Sim& sim, FVec2 p) {
    const nav::NavGrid& g = sim.nav()->grid;
    const Fixed lo = 0.5_fx;
    return {std::clamp(p.x, lo, Fixed::fromInt(g.width()) - lo), std::clamp(p.y, lo, Fixed::fromInt(g.height()) - lo)};
}

FVec2 centroid(const Sim& sim, const std::vector<EntityId>& regs) {
    FVec2 sum;
    for (const EntityId id : regs) sum += sim.world().transforms.get(id)->position;
    return sum / Fixed::fromInt(static_cast<std::int32_t>(regs.size()));
}

void computeSlotOrder(const Sim& sim, Group& g) {
    std::vector<std::pair<Fixed, EntityId>> strength;
    for (const EntityId id : g.members) strength.emplace_back(regimentStrength(sim, id), id);
    std::sort(strength.begin(), strength.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? b.first < a.first : a.second < b.second;  // strongest first, then id
    });
    g.slotOrder.clear();
    for (const auto& [s, id] : strength) g.slotOrder.push_back(id);
}

std::uint32_t groupFormationFor(const Sim& sim, const Group& g) {
    const DefSet* defs = sim.config().defs;
    if (defs == nullptr || g.members.empty()) return kNoDef;
    const Regiment* r = sim.world().regiments.get(g.members.front());
    if (r != nullptr && r->layout != kNoDef) {
        const LayoutDef& l = defs->layouts[r->layout];
        if (r->formation < l.formations.size()) {
            const std::uint32_t f = l.formations[r->formation].formationIndex;
            if (f != kNoDef && defs->formations[f].groupFormationIndex != kNoDef) return defs->formations[f].groupFormationIndex;
        }
    }
    return defs->groupFormationIndex("group_defensive");
}

// Stops a division: its anchor and every member.
void stopGroup(Sim& sim, EntityId groupId) {
    World& w = sim.world();
    Group* g = w.groups.get(groupId);
    if (g == nullptr) return;
    if (Order* go = w.orders.get(groupId)) setIdle(*go);
    for (const EntityId m : g->members) {
        if (Order* o = w.orders.get(m)) setIdle(*o);
        if (Regiment* r = w.regiments.get(m)) cancelRun(*r);
    }
}

// Takes a regiment out of its group; a group left with fewer than two regiments dissolves.
void leaveGroup(Sim& sim, EntityId reg) {
    World& w = sim.world();
    Regiment* r = w.regiments.get(reg);
    if (r == nullptr || r->group.isNull()) return;
    const EntityId gid = r->group;
    r->group = EntityId{};
    if (Order* o = w.orders.get(reg); o != nullptr && o->state == Order::State::Following) setIdle(*o);
    Group* g = w.groups.get(gid);
    if (g == nullptr) return;
    g->members.erase(std::remove(g->members.begin(), g->members.end(), reg), g->members.end());
    g->slotOrder.erase(std::remove(g->slotOrder.begin(), g->slotOrder.end(), reg), g->slotOrder.end());
    if (g->members.size() >= 2) return;
    for (const EntityId m : g->members) {
        if (Regiment* mr = w.regiments.get(m)) mr->group = EntityId{};
        if (Order* o = w.orders.get(m); o != nullptr && o->state == Order::State::Following) setIdle(*o);
    }
    g->members.clear();
    g->slotOrder.clear();
    if (Order* go = w.orders.get(gid)) setIdle(*go);
    w.registry.destroyLater(gid);
}

// The group made of exactly `regs` (>= 2, ascending): the existing one if they already form it,
// else a new loose group (members leave their old groups).
EntityId formGroup(Sim& sim, const std::vector<EntityId>& regs, std::uint8_t player) {
    World& w = sim.world();
    const EntityId first = w.regiments.get(regs.front())->group;
    if (!first.isNull()) {
        const Group* g = w.groups.get(first);
        if (g != nullptr && g->members == regs) return first;
    }
    for (const EntityId r : regs) leaveGroup(sim, r);
    const EntityId gid = w.registry.create();
    if (gid.isNull()) return gid;
    Group g;
    g.player = player;
    g.members = regs;
    computeSlotOrder(sim, g);
    g.groupFormation = groupFormationFor(sim, g);
    const Transform* lead = w.transforms.get(g.slotOrder.front());
    w.transforms.set(gid, {centroid(sim, regs), lead->facing});
    Order idle;
    idle.goal = centroid(sim, regs);
    idle.facing = lead->facing;
    w.orders.set(gid, idle);
    w.groups.set(gid, g);
    for (const EntityId r : regs) w.regiments.get(r)->group = gid;
    return gid;
}

}  // namespace

void applyMoveGroup(Sim& sim, const Command& c, const MoveGroupPayload& p, bool run) {
    if (sim.nav() == nullptr) return sim.reject(c);
    const std::vector<EntityId> regs = validRegiments(sim, c, p.regiments);
    if (regs.empty()) return sim.reject(c);
    World& w = sim.world();
    const FVec2 goal = clampToMap(sim, p.goal);
    for (const EntityId id : regs) {
        clearEngagement(sim, id);
        Regiment& r = *w.regiments.get(id);
        if (run) {
            startRun(sim, r);
        } else {
            cancelRun(r);
        }
    }
    if (regs.size() == 1) {
        leaveGroup(sim, regs.front());
        startOrder(*w.orders.get(regs.front()), goal, p.hasFacing, p.facing);
        return;
    }
    const EntityId gid = formGroup(sim, regs, c.player);
    if (gid.isNull()) return sim.reject(c);
    Group& g = *w.groups.get(gid);
    computeSlotOrder(sim, g);
    g.groupFormation = groupFormationFor(sim, g);
    const FVec2 from = centroid(sim, regs);
    const FVec2 d = goal - from;
    const Angle heading = d.isZero() ? w.transforms.get(g.slotOrder.front())->facing : d.angle();
    if (!g.division) {
        // Loose group: every regiment paths on its own to its slot around the goal.
        if (Order* go = w.orders.get(gid)) setIdle(*go);
        for (std::size_t k = 0; k < g.slotOrder.size(); ++k) {
            const FVec2 target = clampToMap(sim, goal + groupSlotOffset(sim, g, k).rotated(heading));
            startOrder(*w.orders.get(g.slotOrder[k]), target, p.hasFacing, p.facing);
        }
        return;
    }
    // Division: one anchor paths for everyone; members hold their slots around it.
    Transform& anchor = *w.transforms.get(gid);
    Order& go = *w.orders.get(gid);
    if (go.state == Order::State::Idle) anchor = {from, heading};
    startOrder(go, goal, p.hasFacing, p.facing);
    for (const EntityId m : g.members) {
        Order& o = *w.orders.get(m);
        setIdle(o);
        o.state = Order::State::Following;
        o.blocked = false;
    }
}

void applyStop(Sim& sim, const Command& c, const RegimentListPayload& p) {
    const std::vector<EntityId> regs = validRegiments(sim, c, p.regiments);
    if (regs.empty()) return sim.reject(c);
    World& w = sim.world();
    for (const EntityId id : regs) {
        clearEngagement(sim, id);
        Regiment& r = *w.regiments.get(id);
        const Group* g = r.group.isNull() ? nullptr : w.groups.get(r.group);
        if (g != nullptr && g->division) {
            stopGroup(sim, r.group);
            continue;
        }
        cancelRun(r);
        setIdle(*w.orders.get(id));
    }
}

void applySetFormation(Sim& sim, const Command& c, const SetFormationPayload& p) {
    const std::vector<EntityId> regs = validRegiments(sim, c, p.regiments);
    const DefSet* defs = sim.config().defs;
    std::size_t applied = 0;
    for (const EntityId id : regs) {
        Regiment& r = *sim.world().regiments.get(id);
        if (defs == nullptr || r.layout == kNoDef) continue;
        const std::uint32_t f = defs->layouts[r.layout].formation(asciiLowerCopy(p.formationIds));
        if (f == kNoDef) continue;
        r.formation = static_cast<std::uint16_t>(f);
        r.runRestore = Regiment::kNone;
        ++applied;
    }
    if (applied == 0) sim.reject(c);
}

void applySetGroupBehaviour(Sim& sim, const Command& c, const SetGroupBehaviourPayload& p) {
    const std::vector<EntityId> regs = validRegiments(sim, c, p.regiments);
    if (regs.empty()) return sim.reject(c);
    World& w = sim.world();
    EntityId gid;
    if (regs.size() == 1) {
        gid = w.regiments.get(regs.front())->group;
        if (gid.isNull()) return sim.reject(c);
    } else {
        gid = formGroup(sim, regs, c.player);
        if (gid.isNull()) return sim.reject(c);
    }
    Group& g = *w.groups.get(gid);
    if (g.division != p.division) {
        stopGroup(sim, gid);  // switching behaviour stops the group; the next MoveGroup uses it
        g.division = p.division;
        if (g.division) *w.transforms.get(gid) = {centroid(sim, g.members), w.transforms.get(g.members.front())->facing};
    }
    computeSlotOrder(sim, g);
    g.groupFormation = groupFormationFor(sim, g);
}

void applyAttack(Sim& sim, const Command& c, const AttackPayload& p) {
    if (sim.nav() == nullptr) return sim.reject(c);
    World& w = sim.world();
    // The target: a live regiment, unit or building of another player.
    std::uint8_t owner = c.player;
    if (w.registry.alive(p.target)) {
        if (const Regiment* tr = w.regiments.get(p.target)) {
            owner = tr->player;
        } else if (const Combat* tc = w.combats.get(p.target); tc != nullptr && w.transforms.has(p.target)) {
            owner = tc->player;
        }
    }
    const std::vector<EntityId> regs = validRegiments(sim, c, p.regiments);
    if (owner == c.player || regs.empty()) return sim.reject(c);
    for (const EntityId id : regs) {
        // It stops what it was doing; the Engagement stage chases the target or engages at once.
        clearEngagement(sim, id);
        leaveGroup(sim, id);
        cancelRun(*w.regiments.get(id));
        setIdle(*w.orders.get(id));
        Engagement* e = w.engagements.get(id);
        if (e == nullptr) continue;
        e->mode = Engagement::Mode::Attack;
        e->target = p.target;
        e->graceUntil = 0;
    }
}

void applyAttackMove(Sim& sim, const Command& c, const MoveGroupPayload& p) {
    if (sim.nav() == nullptr) return sim.reject(c);
    const std::vector<EntityId> regs = validRegiments(sim, c, p.regiments);
    if (regs.empty()) return sim.reject(c);
    applyMoveGroup(sim, c, p, false);
    for (const EntityId id : regs) {
        Engagement* e = sim.world().engagements.get(id);
        if (e == nullptr) continue;
        e->mode = Engagement::Mode::AttackMove;
        e->goal = clampToMap(sim, p.goal);
        e->graceUntil = 0;
    }
}

void applySetEntrenched(Sim& sim, const Command& c, const DebugSetEntrenchedPayload& p) {
    const std::vector<EntityId> regs = validRegiments(sim, c, p.regiments);
    if (regs.empty()) return sim.reject(c);
    for (const EntityId id : regs) {
        if (Engagement* e = sim.world().engagements.get(id)) e->entrenched = p.entrenched;
    }
}

EntityId orderOwner(const Sim& sim, EntityId regiment) {
    const World& w = sim.world();
    const Regiment* r = w.regiments.get(regiment);
    if (r != nullptr && !r->group.isNull()) {
        const Group* g = w.groups.get(r->group);
        if (g != nullptr && g->division) return r->group;
    }
    return regiment;
}

bool holdOrder(Sim& sim, EntityId owner) {
    Order* o = sim.world().orders.get(owner);
    if (o == nullptr) return false;
    switch (o->state) {
    case Order::State::WaitPath:
    case Order::State::Moving:
    case Order::State::Wheeling:
    case Order::State::Arriving:
        o->state = Order::State::Engaged;
        o->needPath = false;
        o->stateTicks = 0;
        o->window = 0;
        o->progress = Fixed{};
        o->expected = Fixed{};
        return true;
    default:
        return false;
    }
}

void resumeOrder(Sim& sim, EntityId owner) {
    Order* o = sim.world().orders.get(owner);
    if (o == nullptr || o->state != Order::State::Engaged) return;
    startOrder(*o, o->goal, o->hasFacing, o->facing);
}

void restartOrder(Sim& sim, EntityId owner, FVec2 goal, bool hasFacing, Angle facing) {
    if (Order* o = sim.world().orders.get(owner)) startOrder(*o, clampToMap(sim, goal), hasFacing, facing);
}

void stopOrder(Sim& sim, EntityId owner) {
    if (Order* o = sim.world().orders.get(owner)) setIdle(*o);
}

void chaseOrder(Sim& sim, EntityId regiment, FVec2 goal) {
    World& w = sim.world();
    Regiment* r = w.regiments.get(regiment);
    Order* o = w.orders.get(regiment);
    if (r == nullptr || o == nullptr) return;
    leaveGroup(sim, regiment);
    cancelRun(*r);
    startOrder(*o, clampToMap(sim, goal), false, Angle{});
}

void onEntityDestroyed(Sim& sim, EntityId id) {
    const Regiment* r = sim.world().regiments.get(id);
    if (r != nullptr && !r->group.isNull()) leaveGroup(sim, id);
}

// Orders stage: anchors that need a path request one (regiments and division anchors alike).
void runOrders(Sim& sim) {
    nav::NavWorld* nav = sim.nav();
    if (nav == nullptr) return;
    World& w = sim.world();
    for (std::size_t i = 0; i < w.orders.size(); ++i) {
        Order& o = w.orders.data()[i];
        if (!o.needPath) continue;
        const EntityId id = w.orders.ids()[i];
        o.needPath = false;
        const Transform* t = w.transforms.get(id);
        if (t == nullptr) continue;
        std::uint8_t cls = 0;
        if (const Regiment* r = w.regiments.get(id)) {
            cls = r->moveClass;
        } else if (const Group* g = w.groups.get(id)) {
            bool any = false;
            for (const EntityId m : g->members) {
                const Regiment* mr = w.regiments.get(m);
                if (mr == nullptr) continue;
                cls = any ? restrictiveClass(cls, mr->moveClass) : mr->moveClass;
                any = true;
            }
        } else {
            continue;
        }
        Path path;
        path.moveClass = cls;
        if (!hasNavClass(cls)) {  // aircraft: straight line, no grid
            path.status = Path::Status::Found;
            path.waypoints = {sim.cellAt(t->position), sim.cellAt(o.goal)};
        } else {
            path.status = Path::Status::Pending;
            path.requestId =
                nav->paths.request(id, static_cast<nav::MoveClass>(cls), sim.cellAt(t->position), sim.cellAt(o.goal), sim.tick());
        }
        w.paths.set(id, std::move(path));
        o.state = Order::State::WaitPath;
        o.waypoint = 0;
        o.stateTicks = 0;
    }
}

}  // namespace corps
