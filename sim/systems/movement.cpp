#include "systems/movement.hpp"

#include <algorithm>

#include "nav/nav_world.hpp"
#include "systems/stages.hpp"

namespace corps {

using namespace move;

namespace {

constexpr Fixed kUnitsPerRadian = 10430.378_fx;  // 65536 / 2π
constexpr std::uint16_t kFar = 0xFFFF;
constexpr Fixed kStill = 0.05_fx;

// ------------------------------------------------------------------------------ small helpers

const UnitDef* unitDefOf(const Sim& sim, EntityId id) {
    const DefRef* d = sim.world().defRefs.get(id);
    if (d == nullptr || d->kind != DefRef::Kind::Unit || sim.config().defs == nullptr) return nullptr;
    return &sim.config().defs->units[d->index];
}

Fixed radiusOf(const UnitDef* u) {
    return u != nullptr && u->blockingRadius > Fixed{} ? u->blockingRadius : kDefaultUnitRadius;
}

bool insideMap(const nav::NavGrid& g, FVec2 p) {
    return p.x >= Fixed{} && p.y >= Fixed{} && p.x < Fixed::fromInt(g.width()) && p.y < Fixed::fromInt(g.height());
}

std::uint32_t cellOf(const nav::NavGrid& g, FVec2 p) {
    return g.index(static_cast<std::int32_t>(p.x.floorInt()), static_cast<std::int32_t>(p.y.floorInt()));
}

FVec2 cellCenter(const nav::NavGrid& g, std::uint32_t cell) {
    return {Fixed::fromInt(g.xOf(cell)) + 0.5_fx, Fixed::fromInt(g.yOf(cell)) + 0.5_fx};
}

bool passable(const nav::NavGrid& g, std::uint8_t cls, FVec2 p) {
    if (!insideMap(g, p)) return false;
    if (!hasNavClass(cls)) return true;
    return g.cost(static_cast<nav::MoveClass>(cls), cellOf(g, p)) != 0;
}

// Speed multiplier of the terrain under p: kCostUnit / cost (roads faster, forest slower).
Fixed terrainMult(const nav::NavGrid& g, std::uint8_t cls, FVec2 p) {
    if (!hasNavClass(cls) || !insideMap(g, p)) return Fixed::fromInt(1);
    const std::uint8_t cost = g.cost(static_cast<nav::MoveClass>(cls), cellOf(g, p));
    return cost == 0 ? Fixed::fromInt(1) : Fixed::fromRatio(nav::kCostUnit, cost);
}

Angle turnToward(Angle from, Angle to, std::int32_t maxUnits) {
    const std::int32_t d = std::clamp(Angle::delta(from, to), -maxUnits, maxUnits);
    return from + Angle::fromUnits(static_cast<std::uint32_t>(d));
}

std::int32_t absDelta(Angle a, Angle b) {
    const std::int32_t d = Angle::delta(a, b);
    return d < 0 ? -d : d;
}

// Lets a step go only into passable cells; slides along one axis when the other is blocked.
// Moving out of a blocked cell is always allowed.
FVec2 slide(const nav::NavGrid& g, std::uint8_t cls, FVec2 from, FVec2 to) {
    if (!passable(g, cls, from) && insideMap(g, to)) return to;
    if (passable(g, cls, to)) return to;
    const FVec2 xOnly{to.x, from.y};
    if (passable(g, cls, xOnly)) return xOnly;
    const FVec2 yOnly{from.x, to.y};
    if (passable(g, cls, yOnly)) return yOnly;
    return from;
}

// Nearest passable cell centre within `radius` cells (by distance², then cell index).
bool nearestPassable(const nav::NavGrid& g, std::uint8_t cls, FVec2 p, std::int32_t radius, FVec2& out) {
    if (!insideMap(g, p)) return false;
    const std::int32_t cx = static_cast<std::int32_t>(p.x.floorInt()), cy = static_cast<std::int32_t>(p.y.floorInt());
    std::int64_t best = INT64_MAX;
    std::uint32_t bestCell = 0;
    for (std::int32_t y = cy - radius; y <= cy + radius; ++y) {
        for (std::int32_t x = cx - radius; x <= cx + radius; ++x) {
            if (!g.inside(x, y) || g.cost(static_cast<nav::MoveClass>(cls), g.index(x, y)) == 0) continue;
            const std::int64_t d2 = static_cast<std::int64_t>(x - cx) * (x - cx) + static_cast<std::int64_t>(y - cy) * (y - cy);
            const std::uint32_t cell = g.index(x, y);
            if (d2 < best || (d2 == best && cell < bestCell)) {
                best = d2;
                bestCell = cell;
            }
        }
    }
    if (best == INT64_MAX) return false;
    out = cellCenter(g, bestCell);
    return true;
}

void computeClearance(const nav::NavGrid& g, MovementCache& cache) {
    const std::int32_t w = g.width(), h = g.height();
    for (std::size_t ci = 0; ci < nav::kMoveClassCount; ++ci) {
        const auto c = static_cast<nav::MoveClass>(ci);
        std::vector<std::uint16_t>& d = cache.clearance[ci];
        d.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), kFar);
        const auto at = [&](std::int32_t x, std::int32_t y) -> std::uint32_t {
            return g.inside(x, y) ? d[g.index(x, y)] : 0u;  // outside the map counts as blocked
        };
        const auto relax = [&](std::uint32_t cell, std::uint32_t v) {
            if (v < d[cell]) d[cell] = static_cast<std::uint16_t>(std::min<std::uint32_t>(v, kFar));
        };
        for (std::int32_t y = 0; y < h; ++y) {
            for (std::int32_t x = 0; x < w; ++x) {
                const std::uint32_t cell = g.index(x, y);
                if (g.cost(c, cell) == 0) {
                    d[cell] = 0;
                    continue;
                }
                relax(cell, at(x - 1, y) + 10);
                relax(cell, at(x - 1, y - 1) + 14);
                relax(cell, at(x, y - 1) + 10);
                relax(cell, at(x + 1, y - 1) + 14);
            }
        }
        for (std::int32_t y = h - 1; y >= 0; --y) {
            for (std::int32_t x = w - 1; x >= 0; --x) {
                const std::uint32_t cell = g.index(x, y);
                if (d[cell] == 0) continue;
                relax(cell, at(x + 1, y) + 10);
                relax(cell, at(x + 1, y + 1) + 14);
                relax(cell, at(x, y + 1) + 10);
                relax(cell, at(x - 1, y + 1) + 14);
            }
        }
    }
}

Fixed clearanceAt(const nav::NavGrid& g, const MovementCache& cache, std::uint8_t cls, FVec2 p) {
    if (!hasNavClass(cls)) return Fixed::fromInt(1000);
    if (!insideMap(g, p)) return Fixed{};
    return Fixed::fromRatio(cache.clearance[cls][cellOf(g, p)], 10);
}

// ------------------------------------------------------------------------------ per-tick context

struct UnitRef {
    EntityId org;
    EntityId unit;
};

struct RegInfo {
    std::size_t lo = 0, hi = 0;  // range in Ctx::units (living mobile members)
    Fixed nominal;               // slowest unit speed × formation multiplier
    Fixed maxRadius;
    std::int32_t formation = -1;  // effective layout formation for slots (-1 = single file)
};

struct Ctx {
    Sim& sim;
    World& w;
    const DefSet& defs;
    const nav::NavGrid& g;
    const MovementCache& cache;
    std::vector<UnitRef> units;    // mobile members sorted by (organization, unit)
    std::vector<FVec2> offsets;    // slot offset per entry of `units`
};

std::pair<std::size_t, std::size_t> membersOf(const Ctx& ctx, EntityId org) {
    const auto lo = std::lower_bound(ctx.units.begin(), ctx.units.end(), org, [](const UnitRef& u, EntityId o) { return u.org < o; });
    auto hi = lo;
    while (hi != ctx.units.end() && hi->org == org) ++hi;
    return {static_cast<std::size_t>(lo - ctx.units.begin()), static_cast<std::size_t>(hi - ctx.units.begin())};
}

Fixed formationSpeed(const DefSet& defs, const Regiment& r) {
    if (r.layout == kNoDef) return Fixed::fromInt(1);
    const LayoutDef& l = defs.layouts[r.layout];
    if (r.formation >= l.formations.size()) return Fixed::fromInt(1);
    const std::uint32_t f = l.formations[r.formation].formationIndex;
    return f == kNoDef ? Fixed::fromInt(1) : defs.formations[f].speedMultiplier;
}

Fixed halfWidth(const DefSet& defs, const Regiment& r, std::uint32_t formation) {
    if (r.layout == kNoDef) return 1_fx;
    const LayoutDef& l = defs.layouts[r.layout];
    return formation < l.formations.size() ? l.formations[formation].halfWidth : 1_fx;
}

std::uint32_t columnOf(const DefSet& defs, const Regiment& r) {
    return r.layout == kNoDef ? kNoDef : defs.layouts[r.layout].formation("regiment_column");
}

// Fills ctx.offsets for a regiment's members from its compression level.
void computeOffsets(Ctx& ctx, const Regiment& r, RegInfo& info) {
    std::uint32_t f = r.formation;
    if (r.compression == 1) {
        const std::uint32_t col = columnOf(ctx.defs, r);
        if (col != kNoDef) f = col;
    }
    info.formation = r.compression == 2 ? -1 : static_cast<std::int32_t>(f);
    for (std::size_t i = info.lo; i < info.hi; ++i) {
        const Member* m = ctx.w.members.get(ctx.units[i].unit);
        ctx.offsets[i] = layoutOffset(ctx.defs, r, f, m->slot);
    }
    if (r.compression != 2) return;
    // Single file: front to back by forward offset, then element.
    std::vector<std::pair<Fixed, std::size_t>> order;
    for (std::size_t i = info.lo; i < info.hi; ++i) order.emplace_back(ctx.offsets[i].x, i);
    std::sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
        if (a.first != b.first) return b.first < a.first;
        return ctx.w.members.get(ctx.units[a.second].unit)->slot < ctx.w.members.get(ctx.units[b.second].unit)->slot;
    });
    const Fixed spacing = info.maxRadius * 2 + 0.5_fx;
    for (std::size_t k = 0; k < order.size(); ++k) ctx.offsets[order[k].second] = {-(spacing * static_cast<std::int32_t>(k)), Fixed{}};
}

FVec2 slotTarget(const Transform& anchor, FVec2 offset) {
    return anchor.position + offset.rotated(anchor.facing);
}

// Largest distance of a member from its slot.
Fixed maxLag(const Ctx& ctx, const RegInfo& info, const Transform& anchor) {
    Fixed lag;
    for (std::size_t i = info.lo; i < info.hi; ++i) {
        const Transform* t = ctx.w.transforms.get(ctx.units[i].unit);
        lag = max(lag, distance(t->position, slotTarget(anchor, ctx.offsets[i])));
    }
    return lag;
}

// The anchor never outruns its slowest unit by more than one slot length (PATHFINDING §4).
Fixed lagFactor(Fixed lag) {
    if (lag <= kSlotLength) return Fixed::fromInt(1);
    if (lag >= kSlotLength * 2) return Fixed{};
    return (kSlotLength * 2 - lag) / kSlotLength;
}

// Percentage test: at least kArrivedPercent of the members are within tolerance of their slots.
bool unitsSettled(const Ctx& ctx, const RegInfo& info, const Transform& anchor) {
    const std::size_t n = info.hi - info.lo;
    if (n == 0) return true;
    std::size_t in = 0;
    for (std::size_t i = info.lo; i < info.hi; ++i) {
        const Transform* t = ctx.w.transforms.get(ctx.units[i].unit);
        if (distance(t->position, slotTarget(anchor, ctx.offsets[i])) <= kSlotTolerance) ++in;
    }
    return in * 100 >= n * kArrivedPercent;
}

void resetWindow(Order& o) {
    o.window = 0;
    o.progress = Fixed{};
    o.expected = Fixed{};
}

void restoreRun(Regiment* r) {
    if (r != nullptr && r->runRestore != Regiment::kNone) {
        r->formation = r->runRestore;
        r->runRestore = Regiment::kNone;
    }
}

std::uint8_t playerOf(const World& w, EntityId id) {
    if (const Regiment* r = w.regiments.get(id)) return r->player;
    if (const Group* g = w.groups.get(id)) return g->player;
    return 0;
}

void finish(Ctx& ctx, EntityId id, Order& o, bool arrived) {
    o.state = Order::State::Idle;
    o.needPath = false;
    o.blocked = !arrived;
    o.stateTicks = 0;
    resetWindow(o);
    ctx.sim.emit({arrived ? SimEvent::Type::Arrived : SimEvent::Type::Blocked, id, playerOf(ctx.w, id), CommandType::Noop});
    restoreRun(ctx.w.regiments.get(id));
    if (const Group* grp = ctx.w.groups.get(id)) {  // a division finishes for all its members
        for (const EntityId m : grp->members) {
            Order* mo = ctx.w.orders.get(m);
            if (mo == nullptr) continue;
            mo->state = Order::State::Idle;
            mo->blocked = !arrived;
            mo->stateTicks = 0;
            restoreRun(ctx.w.regiments.get(m));
            ctx.sim.emit({arrived ? SimEvent::Type::Arrived : SimEvent::Type::Blocked, m, playerOf(ctx.w, m), CommandType::Noop});
        }
    }
}

// Stuck or blocked ahead: ask for a new path from here; give up after kMaxRepaths.
void repath(Ctx& ctx, EntityId id, Order& o) {
    if (++o.repaths > kMaxRepaths) return finish(ctx, id, o, false);
    o.needPath = true;
    o.state = Order::State::WaitPath;
    o.stateTicks = 0;
    resetWindow(o);
    if (Path* p = ctx.w.paths.get(id)) {  // the old path must not be picked up again
        p->status = Path::Status::Pending;
        p->requestId = 0;
    }
}

FVec2 waypointPos(const Ctx& ctx, const Order& o, const Path& p, std::uint32_t k) {
    if (k + 1 >= p.waypoints.size() && p.status == Path::Status::Found) return o.goal;
    return cellCenter(ctx.g, p.waypoints[std::min<std::size_t>(k, p.waypoints.size() - 1)]);
}

void enterArriving(Ctx& ctx, Transform& anchor, Order& o) {
    o.state = Order::State::Arriving;
    o.stateTicks = 0;
    if (o.hasFacing) anchor.facing = o.facing;  // slots rotate now; units settle into them
    (void)ctx;
}

// WaitPath → Moving, Moving: follow the path. `nominal` excludes lag waiting (stuck detection).
void followPath(Ctx& ctx, EntityId id, Transform& a, Order& o, std::uint8_t cls, Fixed nominal, Fixed factor) {
    const Path* p = ctx.w.paths.get(id);
    if (o.state == Order::State::WaitPath) {
        if (o.needPath || p == nullptr || p->status == Path::Status::Pending) return;
        if (p->status == Path::Status::Unreachable || p->status == Path::Status::NoStart || p->waypoints.empty()) {
            return finish(ctx, id, o, false);
        }
        o.waypoint = 1;
        o.stateTicks = 0;
        resetWindow(o);
        if (p->waypoints.size() <= 1 && distance(a.position, o.goal) <= kWaypointReach) return enterArriving(ctx, a, o);
        if (p->waypoints.size() <= 1) o.waypoint = 0;
        o.state = Order::State::Moving;
    }
    if (o.state != Order::State::Moving || p == nullptr) return;

    FVec2 target = waypointPos(ctx, o, *p, o.waypoint);
    const FVec2 to = target - a.position;
    const Fixed dist = to.length();
    if (dist > kWaypointReach) {
        const Angle desired = to.angle();
        if (absDelta(a.facing, desired) > kWheelAngle) {  // big turn: wheel in place first
            a.facing = desired;
            o.state = Order::State::Wheeling;
            o.stateTicks = 0;
            return;
        }
        a.facing = turnToward(a.facing, desired, kAnchorTurnPerTick);
    }
    const Fixed terrain = terrainMult(ctx.g, cls, a.position);
    Fixed remaining = nominal * terrain * factor * kTick;
    Fixed moved;
    for (int guard = 0; guard < 16 && remaining > Fixed{}; ++guard) {
        const FVec2 d = target - a.position;
        const Fixed len = d.length();
        const bool reaches = len <= remaining;
        const FVec2 next = reaches ? target : a.position + d * (remaining / len);
        if (passable(ctx.g, cls, a.position) && !passable(ctx.g, cls, next)) {  // a building appeared ahead
            repath(ctx, id, o);
            return;
        }
        a.position = next;
        const Fixed step = reaches ? len : remaining;
        moved += step;
        remaining -= step;
        if (!reaches) break;
        if (o.waypoint + 1 >= p->waypoints.size()) {
            enterArriving(ctx, a, o);
            break;
        }
        ++o.waypoint;
        target = waypointPos(ctx, o, *p, o.waypoint);
    }
    // Stuck detection: under 10% of the expected progress over a window → new path.
    o.expected += nominal * terrain * kTick;
    o.progress += moved;
    if (++o.window >= kStuckWindowTicks) {
        const bool stuck = o.state == Order::State::Moving && o.progress * 10 < o.expected;
        resetWindow(o);
        if (stuck) {
            repath(ctx, id, o);
        } else {
            o.repaths = 0;  // "3 failures" means 3 in a row
        }
    }
}

// Compression for chokepoints (PATHFINDING §4): clearance ahead vs the formation's half width.
void updateCompression(Ctx& ctx, Regiment& r, const RegInfo& info, const Transform& a, const Order& o) {
    if (!hasNavClass(r.moveClass) || r.layout == kNoDef) return;
    Fixed clear = clearanceAt(ctx.g, ctx.cache, r.moveClass, a.position);
    if (o.state == Order::State::Moving) {
        const FVec2 dir = FVec2::fromAngle(a.facing);
        for (std::int32_t k = 2; k <= 8; k += 2) {
            const FVec2 s = a.position + dir * Fixed::fromInt(k);
            if (insideMap(ctx.g, s)) clear = min(clear, clearanceAt(ctx.g, ctx.cache, r.moveClass, s));
        }
    }
    const Fixed margin = info.maxRadius + 0.5_fx;
    const std::uint32_t col = columnOf(ctx.defs, r);
    std::uint8_t wanted = 2;
    if (clear >= halfWidth(ctx.defs, r, r.formation) + margin) {
        wanted = 0;
    } else if (col != kNoDef && clear >= halfWidth(ctx.defs, r, col) + margin) {
        wanted = 1;
    }
    if (wanted > r.compression) {
        r.compression = wanted;
        r.compressionHold = 0;
    } else if (wanted < r.compression) {
        if (++r.compressionHold >= kDecompressHoldTicks) {
            r.compression = wanted;
            r.compressionHold = 0;
        }
    } else {
        r.compressionHold = 0;
    }
}

// ------------------------------------------------------------------------------ units

struct UnitState {
    EntityId id;
    FVec2 pos;
    FVec2 vel;
    Angle facing;
    Fixed radius;
    const UnitDef* def = nullptr;
    std::uint8_t cls = 0;
    std::uint8_t player = 0;
    EntityId org;
    bool regimentMoving = false;
    bool hasTarget = false;
    FVec2 target;
    Angle restFacing;
    Fixed speedMult = Fixed::fromInt(1);
};

bool movingState(Order::State s) {
    return s == Order::State::Moving || s == Order::State::Wheeling || s == Order::State::Following || s == Order::State::Arriving;
}

void moveUnits(Ctx& ctx, const std::vector<RegInfo>& infos) {
    World& w = ctx.w;
    const nav::NavGrid& g = ctx.g;
    // Snapshot (Jacobi update: every unit reacts to positions at the start of the phase).
    std::vector<UnitState> us;
    us.reserve(w.movers.size());
    for (std::size_t i = 0; i < w.movers.size(); ++i) {
        UnitState u;
        u.id = w.movers.ids()[i];
        const Transform* t = w.transforms.get(u.id);
        if (t == nullptr) continue;
        u.pos = t->position;
        u.facing = t->facing;
        u.vel = w.movers.data()[i].velocity;
        u.def = unitDefOf(ctx.sim, u.id);
        u.radius = radiusOf(u.def);
        u.cls = u.def != nullptr ? static_cast<std::uint8_t>(u.def->moveClass) : 0;
        if (const Member* m = w.members.get(u.id)) {
            u.org = m->organization;
            const Regiment* r = w.regiments.get(u.org);
            const Transform* anchor = w.transforms.get(u.org);
            const Order* o = w.orders.get(u.org);
            if (r != nullptr && anchor != nullptr && o != nullptr && w.registry.alive(u.org)) {
                u.player = r->player;
                u.regimentMoving = movingState(o->state);
                u.speedMult = formationSpeed(ctx.defs, *r);
                u.restFacing = anchor->facing;
                const auto [lo, hi] = membersOf(ctx, u.org);
                for (std::size_t k = lo; k < hi; ++k) {
                    if (ctx.units[k].unit == u.id) {
                        u.target = slotTarget(*anchor, ctx.offsets[k]);
                        u.hasTarget = true;
                    }
                }
            }
        }
        us.push_back(u);
    }
    (void)infos;

    // Spatial hash: kBucketCells-square buckets, counting sort keeps ascending ids per bucket.
    const std::int32_t bw = (g.width() + kBucketCells - 1) / kBucketCells, bh = (g.height() + kBucketCells - 1) / kBucketCells;
    const auto bucketOf = [&](FVec2 p) {
        const auto bx = std::clamp<std::int64_t>(p.x.floorInt() / kBucketCells, 0, bw - 1);
        const auto by = std::clamp<std::int64_t>(p.y.floorInt() / kBucketCells, 0, bh - 1);
        return static_cast<std::size_t>(by * bw + bx);
    };
    std::vector<std::uint32_t> start(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) + 1, 0);
    Fixed maxRadius;
    for (const UnitState& u : us) {
        ++start[bucketOf(u.pos) + 1];
        maxRadius = max(maxRadius, u.radius);
    }
    for (std::size_t b = 1; b < start.size(); ++b) start[b] += start[b - 1];
    std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
    std::vector<std::uint32_t> items(us.size());
    for (std::uint32_t i = 0; i < us.size(); ++i) items[fill[bucketOf(us[i].pos)]++] = i;

    std::vector<FVec2> newPos(us.size()), newVel(us.size());
    std::vector<Angle> newFacing(us.size());
    for (std::uint32_t i = 0; i < us.size(); ++i) {
        const UnitState& u = us[i];
        const UnitDef* d = u.def;
        const Fixed speedMax = d != nullptr ? d->speedMax : 2_fx;
        const Fixed maxSpeed = speedMax * u.speedMult * terrainMult(g, u.cls, u.pos);

        // Arrive at the slot.
        FVec2 desired;
        Fixed slotDist;
        if (u.hasTarget) {
            const FVec2 to = u.target - u.pos;
            slotDist = to.length();
            if (slotDist > kStill) desired = to * (min(maxSpeed, slotDist * kArriveGain) / slotDist);
        }
        // Separation from neighbours (PATHFINDING §6).
        FVec2 sep;
        const Fixed reachMax = u.radius + maxRadius + kAvoidMargin;
        const std::int32_t span = static_cast<std::int32_t>(reachMax.ceilInt() / kBucketCells) + 1;
        const std::int64_t ubx = std::clamp<std::int64_t>(u.pos.x.floorInt() / kBucketCells, 0, bw - 1);
        const std::int64_t uby = std::clamp<std::int64_t>(u.pos.y.floorInt() / kBucketCells, 0, bh - 1);
        for (std::int64_t by = std::max<std::int64_t>(uby - span, 0); by <= std::min<std::int64_t>(uby + span, bh - 1); ++by) {
            for (std::int64_t bx = std::max<std::int64_t>(ubx - span, 0); bx <= std::min<std::int64_t>(ubx + span, bw - 1); ++bx) {
                const std::size_t b = static_cast<std::size_t>(by * bw + bx);
                for (std::uint32_t k = start[b]; k < start[b + 1]; ++k) {
                    const std::uint32_t j = items[k];
                    if (j == i) continue;
                    const UnitState& o = us[j];
                    if (hasNavClass(u.cls) != hasNavClass(o.cls)) continue;  // aircraft don't collide with ground
                    const Fixed reach = u.radius + o.radius + kAvoidMargin;
                    const FVec2 away = u.pos - o.pos;
                    if (abs(away.x) >= reach || abs(away.y) >= reach) continue;
                    const Fixed dist = away.length();
                    if (dist >= reach) continue;
                    Fixed weight = Fixed::fromInt(1);  // enemies: strong
                    if (u.player == o.player) {
                        if (u.org == o.org) {
                            weight = 0.25_fx;  // same regiment: they may overlap a little
                        } else {
                            // Friendly regiments: the lower-priority one yields (moving > idle, then lower id).
                            const bool iYield = u.regimentMoving != o.regimentMoving ? !u.regimentMoving : o.org < u.org;
                            weight = iYield ? Fixed::fromInt(1) : 0.5_fx;
                        }
                    }
                    if (weight == Fixed{}) continue;
                    const FVec2 dir = dist > Fixed{} ? away / dist : FVec2{u.id < o.id ? -1_fx : 1_fx, Fixed{}};
                    // Push apart; units of different regiments also sidestep to the right (both units
                    // of a head-on pair veer to their own right and pass instead of stalling).
                    const FVec2 push = u.org == o.org ? dir : dir + FVec2{dir.y, -dir.x} / 2_fx;
                    sep += push * ((reach - dist) * weight * kSeparationGain);
                }
            }
        }
        FVec2 v = desired + sep;
        const Fixed speed0 = v.length();
        if (speed0 > maxSpeed && speed0 > Fixed{}) v = v * (maxSpeed / speed0);
        // Acceleration limit (0 = instant).
        if (d != nullptr && d->acceleration > Fixed{}) {
            const FVec2 dv = v - u.vel;
            const Fixed dvLen = dv.length();
            const Fixed maxDv = d->acceleration * kTick;
            if (dvLen > maxDv) v = u.vel + dv * (maxDv / dvLen);
        }
        // Turning (MoverComponent turn_type / turn_radius).
        Angle facing = u.facing;
        const Fixed speed = v.length();
        const bool instant = d == nullptr || d->turnType == TurnType::Instant || d->turnRadius <= Fixed{};
        const std::int32_t turnUnits =
            instant ? 32768 : std::max<std::int32_t>(1, static_cast<std::int32_t>((speedMax / d->turnRadius * kTick * kUnitsPerRadian).floorInt()));
        // Small corrections inside the slot of a halted regiment creep without turning; the unit
        // settles facing the regiment's way.
        const bool settling = u.hasTarget && !u.regimentMoving && slotDist <= kSlotTolerance;
        if (settling) {
            if (instant || d->turnType == TurnType::InPlace) facing = turnToward(facing, u.restFacing, turnUnits);
        } else if (speed > kStill) {
            const Angle want = v.angle();
            facing = turnToward(facing, want, turnUnits);
            if (!instant && d->turnType == TurnType::InPlace && absDelta(facing, want) > kPivotAngle) {
                v = FVec2{};  // pivot first
            } else if (!instant && d->turnType == TurnType::WhileMoving) {
                v = FVec2::fromAngle(facing) * speed;  // wheels go where they point
            }
        } else if (u.hasTarget && slotDist <= kSlotTolerance && (instant || d->turnType == TurnType::InPlace)) {
            facing = turnToward(facing, u.restFacing, turnUnits);  // in its slot: face the regiment's way
        }
        // Integrate; terrain and buildings are hard (slide along them, escape if inside).
        FVec2 np = u.pos + v * kTick;
        if (hasNavClass(u.cls) && !passable(g, u.cls, u.pos)) {
            FVec2 out;
            if (nearestPassable(g, u.cls, u.pos, 6, out)) {
                const FVec2 to = out - u.pos;
                const Fixed len = to.length();
                const Fixed stepLen = min(len, max(speedMax, 1_fx) * kTick);
                np = len > Fixed{} ? u.pos + to * (stepLen / len) : u.pos;
            } else {
                np = u.pos;
            }
        } else if (hasNavClass(u.cls)) {
            np = slide(g, u.cls, u.pos, np);
        } else {
            np = {std::clamp(np.x, Fixed{}, Fixed::fromInt(g.width()) - 0.01_fx), std::clamp(np.y, Fixed{}, Fixed::fromInt(g.height()) - 0.01_fx)};
        }
        newPos[i] = np;
        newVel[i] = (np - u.pos) / kTick;
        newFacing[i] = facing;
    }
    for (std::uint32_t i = 0; i < us.size(); ++i) {
        Transform* t = w.transforms.get(us[i].id);
        t->position = newPos[i];
        t->facing = newFacing[i];
        w.movers.get(us[i].id)->velocity = newVel[i];
    }
}

}  // namespace

// ------------------------------------------------------------------------------ shared helpers

bool hasNavClass(std::uint8_t cls) {
    return cls < nav::kMoveClassCount;
}

std::uint8_t restrictiveClass(std::uint8_t a, std::uint8_t b) {
    // Rank: Air (only if both) < Foot < Tracked < Wheeled < Amphibious < Naval.
    const auto rank = [](std::uint8_t c) {
        switch (static_cast<UnitMoveClass>(c)) {
        case UnitMoveClass::Air: return 0;
        case UnitMoveClass::Foot: return 1;
        case UnitMoveClass::Tracked: return 2;
        case UnitMoveClass::Wheeled: return 3;
        case UnitMoveClass::Amphibious: return 4;
        case UnitMoveClass::Naval: return 5;
        }
        return 1;
    };
    return rank(a) >= rank(b) ? a : b;
}

FVec2 layoutOffset(const DefSet& defs, const Regiment& r, std::uint32_t formation, std::uint32_t element) {
    if (r.layout != kNoDef) {
        const LayoutDef& l = defs.layouts[r.layout];
        if (formation < l.formations.size() && element < l.formations[formation].offsets.size()) return l.formations[formation].offsets[element];
    }
    // No layout data for it: rows of three, 1.5 apart, behind the anchor.
    const auto row = static_cast<std::int32_t>(element / 3) + 1;
    const auto col = static_cast<std::int32_t>(element % 3) - 1;
    return {-1.5_fx * row, 1.5_fx * col};
}

FVec2 groupSlotOffset(const Sim& sim, const Group& g, std::size_t k) {
    std::vector<FVec2> offs;
    const DefSet* defs = sim.config().defs;
    if (defs != nullptr && g.groupFormation < defs->groupFormations.size()) {
        offs = defs->groupFormations[g.groupFormation].offsets;
    } else {
        offs = {{0_fx, 0_fx}, {0_fx, 10_fx}, {0_fx, -10_fx}, {-10_fx, 0_fx}, {-10_fx, 10_fx}, {-10_fx, -10_fx}};
    }
    std::stable_sort(offs.begin(), offs.end(), [](FVec2 a, FVec2 b) {
        if (a.x != b.x) return b.x < a.x;                // front first
        if (abs(a.y) != abs(b.y)) return abs(a.y) < abs(b.y);  // centre first
        return b.y < a.y;                                // left first
    });
    if (k < offs.size()) return offs[k];
    Fixed back = offs.empty() ? Fixed{} : offs.back().x;
    for (const FVec2 o : offs) back = min(back, o.x);
    return {back - 10_fx * static_cast<std::int32_t>(k - offs.size() + 1), Fixed{}};
}

Fixed regimentSpeed(const Sim& sim, EntityId regiment, const Regiment& r) {
    const World& w = sim.world();
    bool any = false;
    Fixed slowest;
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        if (w.members.data()[i].organization != regiment) continue;
        const EntityId u = w.members.ids()[i];
        const UnitDef* d = unitDefOf(sim, u);
        if (d == nullptr || !w.movers.has(u)) continue;
        slowest = any ? min(slowest, d->speedMax) : d->speedMax;
        any = true;
    }
    return any && sim.config().defs != nullptr ? slowest * formationSpeed(*sim.config().defs, r) : Fixed{};
}

Fixed regimentStrength(const Sim& sim, EntityId regiment) {
    const World& w = sim.world();
    Fixed sum;
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        if (w.members.data()[i].organization != regiment) continue;
        if (const Health* h = w.health.get(w.members.ids()[i])) sum += h->current;
    }
    return sum;
}

// ------------------------------------------------------------------------------ the stage

void runMovement(Sim& sim) {
    nav::NavWorld* nav = sim.nav();
    if (nav == nullptr || sim.config().defs == nullptr) return;
    const nav::NavGrid& g = nav->grid;
    MovementCache& cache = sim.moveCache();
    if (!cache.valid || cache.navVersion != nav->hpa.version()) {
        computeClearance(g, cache);
        cache.valid = true;
        cache.navVersion = nav->hpa.version();
    }
    World& w = sim.world();
    Ctx ctx{sim, w, *sim.config().defs, g, cache, {}, {}};
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        const EntityId u = w.members.ids()[i];
        if (w.movers.has(u) && w.transforms.has(u)) ctx.units.push_back({w.members.data()[i].organization, u});
    }
    std::stable_sort(ctx.units.begin(), ctx.units.end(), [](const UnitRef& a, const UnitRef& b) { return a.org < b.org; });
    ctx.offsets.assign(ctx.units.size(), FVec2{});

    // Per-regiment info and current slots.
    std::vector<RegInfo> infos(w.regiments.size());
    for (std::size_t i = 0; i < w.regiments.size(); ++i) {
        const EntityId id = w.regiments.ids()[i];
        const Regiment& r = w.regiments.data()[i];
        RegInfo& info = infos[i];
        std::tie(info.lo, info.hi) = membersOf(ctx, id);
        bool any = false;
        for (std::size_t k = info.lo; k < info.hi; ++k) {
            const UnitDef* d = unitDefOf(sim, ctx.units[k].unit);
            if (d == nullptr) continue;
            info.nominal = any ? min(info.nominal, d->speedMax) : d->speedMax;
            info.maxRadius = max(info.maxRadius, radiusOf(d));
            any = true;
        }
        info.nominal = info.nominal * formationSpeed(ctx.defs, r);
        computeOffsets(ctx, r, info);
    }
    const auto infoOf = [&](EntityId id) -> RegInfo* {
        const auto it = std::lower_bound(w.regiments.ids().begin(), w.regiments.ids().end(), id);
        return it != w.regiments.ids().end() && *it == id ? &infos[static_cast<std::size_t>(it - w.regiments.ids().begin())] : nullptr;
    };
    const Fixed groupSpeedMin = ctx.defs.svars.getFixed("GroupSpeedMinimum", kGroupSpeedMinimum);
    const Fixed zone = ctx.defs.svars.getFixed("GroupZoneRangeMin", kGroupZoneRangeMin);

    // 1. Division anchors (before their members, who target slots around them).
    for (std::size_t i = 0; i < w.groups.size(); ++i) {
        const EntityId gid = w.groups.ids()[i];
        const Group& grp = w.groups.data()[i];
        Order* go = w.orders.get(gid);
        Transform* ga = w.transforms.get(gid);
        if (!grp.division || grp.members.empty() || go == nullptr || ga == nullptr || go->state == Order::State::Idle) continue;
        ++go->stateTicks;
        std::uint8_t cls = 0;
        bool any = false;
        Fixed slowest, drift;
        for (std::size_t k = 0; k < grp.slotOrder.size(); ++k) {
            const EntityId m = grp.slotOrder[k];
            const Regiment* mr = w.regiments.get(m);
            const RegInfo* mi = infoOf(m);
            if (mr == nullptr || mi == nullptr || mi->hi == mi->lo) continue;
            cls = any ? restrictiveClass(cls, mr->moveClass) : mr->moveClass;
            slowest = any ? min(slowest, mi->nominal) : mi->nominal;
            any = true;
            drift = max(drift, distance(w.transforms.get(m)->position, slotTarget(*ga, groupSlotOffset(sim, grp, k))));
        }
        if (!any) continue;
        const Fixed nominal = max(slowest, groupSpeedMin);
        const Fixed factor = drift <= zone ? Fixed::fromInt(1) : (drift <= zone * 2 ? 0.5_fx : Fixed{});
        if (go->state == Order::State::Wheeling) {
            if (drift <= kSlotLength || go->stateTicks >= kWheelTimeoutTicks) {
                go->state = Order::State::Moving;
                go->stateTicks = 0;
            }
            continue;
        }
        if (go->state == Order::State::Arriving) {
            bool settled = true;
            for (std::size_t k = 0; k < grp.slotOrder.size(); ++k) {
                const EntityId m = grp.slotOrder[k];
                const RegInfo* mi = infoOf(m);
                if (mi == nullptr) continue;
                const Transform& ma = *w.transforms.get(m);
                if (distance(ma.position, slotTarget(*ga, groupSlotOffset(sim, grp, k))) > kSlotTolerance || !unitsSettled(ctx, *mi, ma)) {
                    settled = false;
                }
            }
            if (settled || go->stateTicks >= kDivisionArriveTimeoutTicks) finish(ctx, gid, *go, true);
            continue;
        }
        followPath(ctx, gid, *ga, *go, cls, nominal, factor);
    }

    // 2. Regiment anchors.
    for (std::size_t i = 0; i < w.regiments.size(); ++i) {
        const EntityId id = w.regiments.ids()[i];
        Regiment& r = w.regiments.data()[i];
        RegInfo& info = infos[i];
        Order* o = w.orders.get(id);
        Transform* a = w.transforms.get(id);
        if (o == nullptr || a == nullptr) continue;
        if (o->state == Order::State::Idle) {  // a halted regiment still widens out of a squeeze
            if (r.compression != 0 && info.hi != info.lo) {
                updateCompression(ctx, r, info, *a, *o);
                if (r.compression == 0) computeOffsets(ctx, r, info);
            }
            continue;
        }
        if (info.hi == info.lo) {  // no living units
            finish(ctx, id, *o, false);
            continue;
        }
        ++o->stateTicks;
        const Fixed factor = lagFactor(maxLag(ctx, info, *a));
        if (o->state == Order::State::Following) {
            const Group* grp = r.group.isNull() ? nullptr : w.groups.get(r.group);
            const Transform* ga = grp != nullptr ? w.transforms.get(r.group) : nullptr;
            if (grp == nullptr || !grp->division || ga == nullptr) {
                o->state = Order::State::Idle;
                continue;
            }
            std::size_t k = 0;
            while (k < grp->slotOrder.size() && grp->slotOrder[k] != id) ++k;
            FVec2 target = slotTarget(*ga, groupSlotOffset(sim, *grp, k));
            // Members steer straight at their slots; when that line is blocked they follow the
            // division's trail instead: its anchor, or the latest path waypoint they can see.
            if (hasNavClass(r.moveClass) && insideMap(g, a->position)) {
                const auto cls = static_cast<nav::MoveClass>(r.moveClass);
                const std::uint32_t here = cellOf(g, a->position);
                const auto sees = [&](FVec2 p) { return insideMap(g, p) && g.lineOfSight(cls, here, cellOf(g, p), 255); };
                if (!passable(g, r.moveClass, target) || !sees(target)) {
                    target = ga->position;
                    const Path* gp = w.paths.get(r.group);
                    const Order* go = w.orders.get(r.group);
                    if (!sees(target) && gp != nullptr && go != nullptr && !gp->waypoints.empty()) {
                        for (std::size_t wpi = std::min<std::size_t>(go->waypoint, gp->waypoints.size() - 1) + 1; wpi-- > 0;) {
                            const FVec2 c = cellCenter(g, gp->waypoints[wpi]);
                            if (sees(c)) {
                                target = c;
                                break;
                            }
                        }
                    }
                }
            }
            const FVec2 to = target - a->position;
            const Fixed d = to.length();
            if (d > kStill) {
                const Fixed step = min(d, info.nominal * terrainMult(g, r.moveClass, a->position) * factor * kTick);
                a->position = slide(g, r.moveClass, a->position, a->position + to * (step / d));
                a->facing = turnToward(a->facing, d > 1_fx ? to.angle() : ga->facing, kAnchorTurnPerTick);
            } else {
                a->facing = turnToward(a->facing, ga->facing, kAnchorTurnPerTick);
            }
        } else if (o->state == Order::State::Wheeling) {
            if (unitsSettled(ctx, info, *a) || o->stateTicks >= kWheelTimeoutTicks) {
                o->state = Order::State::Moving;
                o->stateTicks = 0;
                resetWindow(*o);
            }
        } else if (o->state != Order::State::Arriving) {
            followPath(ctx, id, *a, *o, r.moveClass, info.nominal, factor);
        }
        const std::uint8_t before = r.compression;
        updateCompression(ctx, r, info, *a, *o);
        if (r.compression != before) computeOffsets(ctx, r, info);
    }

    // 3. Units.
    moveUnits(ctx, infos);

    // 4. Arrivals (regiments whose anchor reached the goal wait for ≥80% of their units).
    for (std::size_t i = 0; i < w.regiments.size(); ++i) {
        const EntityId id = w.regiments.ids()[i];
        Order* o = w.orders.get(id);
        const Transform* a = w.transforms.get(id);
        if (o == nullptr || a == nullptr || o->state != Order::State::Arriving) continue;
        if (unitsSettled(ctx, infos[i], *a) || o->stateTicks >= kArriveTimeoutTicks) finish(ctx, id, *o, true);
    }
}

}  // namespace corps
