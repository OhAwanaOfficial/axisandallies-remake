#include "session/state_io.hpp"

#include "nav/nav_world.hpp"
#include "session/bytes.hpp"

namespace corps {

namespace {

// --- per-component field order (part of the format; append fields only with a version bump) ---

void put(ByteWriter& w, const Transform& t) {
    w.vec2(t.position);
    w.angle(t.facing);
}
void get(ByteReader& r, Transform& t) {
    t.position = r.vec2();
    t.facing = r.angle();
}
void put(ByteWriter& w, const DefRef& d) {
    w.u8(static_cast<std::uint8_t>(d.kind));
    w.u32(d.index);
}
void get(ByteReader& r, DefRef& d) {
    d.kind = static_cast<DefRef::Kind>(r.u8());
    d.index = r.u32();
}
void put(ByteWriter& w, const Health& h) {
    w.fixed(h.current);
    w.fixed(h.max);
}
void get(ByteReader& r, Health& h) {
    h.current = r.fixed();
    h.max = r.fixed();
}
void put(ByteWriter& w, const Member& m) {
    w.entity(m.organization);
    w.u16(m.slot);
}
void get(ByteReader& r, Member& m) {
    m.organization = r.entity();
    m.slot = r.u16();
}
void put(ByteWriter& w, const Footprint& f) {
    w.u32(static_cast<std::uint32_t>(f.x0));
    w.u32(static_cast<std::uint32_t>(f.y0));
    w.u32(static_cast<std::uint32_t>(f.x1));
    w.u32(static_cast<std::uint32_t>(f.y1));
}
void get(ByteReader& r, Footprint& f) {
    f.x0 = static_cast<std::int32_t>(r.u32());
    f.y0 = static_cast<std::int32_t>(r.u32());
    f.x1 = static_cast<std::int32_t>(r.u32());
    f.y1 = static_cast<std::int32_t>(r.u32());
}
void put(ByteWriter& w, const Path& p) {
    w.u8(static_cast<std::uint8_t>(p.status));
    w.u8(p.moveClass);
    w.u32(p.requestId);
    w.u32(static_cast<std::uint32_t>(p.cost));
    w.u32(static_cast<std::uint32_t>(p.waypoints.size()));
    for (const std::uint32_t c : p.waypoints) w.u32(c);
}
void get(ByteReader& r, Path& p) {
    p.status = static_cast<Path::Status>(r.u8());
    p.moveClass = r.u8();
    p.requestId = r.u32();
    p.cost = static_cast<std::int32_t>(r.u32());
    const std::uint32_t n = r.count(4);
    p.waypoints.clear();
    for (std::uint32_t i = 0; i < n && r.ok; ++i) p.waypoints.push_back(r.u32());
}
void put(ByteWriter& w, const Regiment& g) {
    w.u32(g.layout);
    w.u8(g.player);
    w.u8(g.moveClass);
    w.u16(g.formation);
    w.u16(g.runRestore);
    w.u8(g.compression);
    w.u8(g.compressionHold);
    w.entity(g.group);
}
void get(ByteReader& r, Regiment& g) {
    g.layout = r.u32();
    g.player = r.u8();
    g.moveClass = r.u8();
    g.formation = r.u16();
    g.runRestore = r.u16();
    g.compression = r.u8();
    g.compressionHold = r.u8();
    g.group = r.entity();
}
void put(ByteWriter& w, const Order& o) {
    w.u8(static_cast<std::uint8_t>(o.state));
    w.boolean(o.needPath);
    w.boolean(o.hasFacing);
    w.boolean(o.blocked);
    w.u8(o.repaths);
    w.u8(o.window);
    w.angle(o.facing);
    w.vec2(o.goal);
    w.u32(o.waypoint);
    w.u32(o.stateTicks);
    w.fixed(o.progress);
    w.fixed(o.expected);
}
void get(ByteReader& r, Order& o) {
    o.state = static_cast<Order::State>(r.u8());
    o.needPath = r.boolean();
    o.hasFacing = r.boolean();
    o.blocked = r.boolean();
    o.repaths = r.u8();
    o.window = r.u8();
    o.facing = r.angle();
    o.goal = r.vec2();
    o.waypoint = r.u32();
    o.stateTicks = r.u32();
    o.progress = r.fixed();
    o.expected = r.fixed();
}
void put(ByteWriter& w, const Mover& m) {
    w.vec2(m.velocity);
}
void get(ByteReader& r, Mover& m) {
    m.velocity = r.vec2();
}
void putIds(ByteWriter& w, const std::vector<EntityId>& v) {
    w.u32(static_cast<std::uint32_t>(v.size()));
    for (const EntityId id : v) w.entity(id);
}
void getIds(ByteReader& r, std::vector<EntityId>& v) {
    const std::uint32_t n = r.count(4);
    v.clear();
    for (std::uint32_t i = 0; i < n && r.ok; ++i) v.push_back(r.entity());
}
void put(ByteWriter& w, const Group& g) {
    w.u8(g.player);
    w.boolean(g.division);
    w.u32(g.groupFormation);
    putIds(w, g.members);
    putIds(w, g.slotOrder);
}
void get(ByteReader& r, Group& g) {
    g.player = r.u8();
    g.division = r.boolean();
    g.groupFormation = r.u32();
    getIds(r, g.members);
    getIds(r, g.slotOrder);
}

void put(ByteWriter& w, const Combat& c) {
    w.u8(c.player);
    putIds(w, c.targets);
    w.u32(static_cast<std::uint32_t>(c.cooldown.size()));
    for (const std::uint16_t v : c.cooldown) w.u16(v);
    w.u32(static_cast<std::uint32_t>(c.turretYaw.size()));
    for (const Angle a : c.turretYaw) w.angle(a);
    w.u32(static_cast<std::uint32_t>(c.turretIdle.size()));
    for (const std::uint16_t v : c.turretIdle) w.u16(v);
    w.entity(c.faceTarget);
}
void get(ByteReader& r, Combat& c) {
    c.player = r.u8();
    getIds(r, c.targets);
    std::uint32_t n = r.count(2);
    c.cooldown.clear();
    for (std::uint32_t i = 0; i < n && r.ok; ++i) c.cooldown.push_back(r.u16());
    n = r.count(2);
    c.turretYaw.clear();
    for (std::uint32_t i = 0; i < n && r.ok; ++i) c.turretYaw.push_back(r.angle());
    n = r.count(2);
    c.turretIdle.clear();
    for (std::uint32_t i = 0; i < n && r.ok; ++i) c.turretIdle.push_back(r.u16());
    c.faceTarget = r.entity();
}
void put(ByteWriter& w, const Engagement& e) {
    w.u8(static_cast<std::uint8_t>(e.state));
    w.u8(static_cast<std::uint8_t>(e.mode));
    w.boolean(e.suspended);
    w.boolean(e.closing);
    w.boolean(e.entrenched);
    w.u16(e.sampleTicks);
    w.u16(e.noTarget);
    w.u32(e.stateTicks);
    w.u32(e.lastCombat);
    w.u32(e.graceUntil);
    w.entity(e.target);
    w.vec2(e.goal);
    w.vec2(e.resumeGoal);
    w.boolean(e.resumeHasFacing);
    w.angle(e.resumeFacing);
    w.fixed(e.morale);
    w.fixed(e.efficiency);
    w.fixed(e.xp);
    w.u8(e.level);
    w.u32(e.stillTicks);
    w.entity(e.lastAttacker);
}
void get(ByteReader& r, Engagement& e) {
    e.state = static_cast<Engagement::State>(r.u8());
    e.mode = static_cast<Engagement::Mode>(r.u8());
    e.suspended = r.boolean();
    e.closing = r.boolean();
    e.entrenched = r.boolean();
    e.sampleTicks = r.u16();
    e.noTarget = r.u16();
    e.stateTicks = r.u32();
    e.lastCombat = r.u32();
    e.graceUntil = r.u32();
    e.target = r.entity();
    e.goal = r.vec2();
    e.resumeGoal = r.vec2();
    e.resumeHasFacing = r.boolean();
    e.resumeFacing = r.angle();
    e.morale = r.fixed();
    e.efficiency = r.fixed();
    e.xp = r.fixed();
    e.level = r.u8();
    e.stillTicks = r.u32();
    e.lastAttacker = r.entity();
}
void put(ByteWriter& w, const Projectile& p) {
    w.entity(p.shooter);
    w.entity(p.target);
    w.vec2(p.aim);
    w.u32(p.ability);
    w.u8(p.player);
    w.u16(p.ticksLeft);
    w.fixed(p.attackMultiplier);
}
void get(ByteReader& r, Projectile& p) {
    p.shooter = r.entity();
    p.target = r.entity();
    p.aim = r.vec2();
    p.ability = r.u32();
    p.player = r.u8();
    p.ticksLeft = r.u16();
    p.attackMultiplier = r.fixed();
}

void putU32s(ByteWriter& w, const std::vector<std::uint32_t>& v) {
    w.u32(static_cast<std::uint32_t>(v.size()));
    for (const std::uint32_t x : v) w.u32(x);
}
void getU32s(ByteReader& r, std::vector<std::uint32_t>& v) {
    const std::uint32_t n = r.count(4);
    v.clear();
    for (std::uint32_t i = 0; i < n && r.ok; ++i) v.push_back(r.u32());
}
void put(ByteWriter& w, const Structure& s) {
    w.u8(static_cast<std::uint8_t>(s.state));
    w.u8(s.player);
    w.u32(s.stateTicks);
    w.fixed(s.xp);
    putU32s(w, s.properties);
    w.u32(static_cast<std::uint32_t>(s.queue.size()));
    for (const RecruitOrder& o : s.queue) {
        w.u32(o.regiment);
        w.fixed(o.paid);
    }
    w.entity(s.recruit);
    w.u16(s.nextElement);
    w.u32(s.recruitTicks);
    w.u32(s.research);
    w.u32(s.researchTicks);
    w.fixed(s.researchPaid);
}
void get(ByteReader& r, Structure& s) {
    s.state = static_cast<Structure::State>(r.u8());
    s.player = r.u8();
    s.stateTicks = r.u32();
    s.xp = r.fixed();
    getU32s(r, s.properties);
    const std::uint32_t n = r.count(12);
    s.queue.clear();
    for (std::uint32_t i = 0; i < n && r.ok; ++i) {
        RecruitOrder o;
        o.regiment = r.u32();
        o.paid = r.fixed();
        s.queue.push_back(o);
    }
    s.recruit = r.entity();
    s.nextElement = r.u16();
    s.recruitTicks = r.u32();
    s.research = r.u32();
    s.researchTicks = r.u32();
    s.researchPaid = r.fixed();
}
void put(ByteWriter& w, const Deploy& d) {
    w.u32(d.building);
    w.vec2(d.at);
    w.angle(d.facing);
}
void get(ByteReader& r, Deploy& d) {
    d.building = r.u32();
    d.at = r.vec2();
    d.facing = r.angle();
}

template <class T>
void putPool(ByteWriter& w, const Pool<T>& pool) {
    w.u32(static_cast<std::uint32_t>(pool.size()));
    for (std::size_t i = 0; i < pool.size(); ++i) {
        w.entity(pool.ids()[i]);
        put(w, pool.data()[i]);
    }
}

template <class T>
bool getPool(ByteReader& r, Pool<T>& pool, const EntityRegistry& reg) {
    pool.clear();
    const std::uint32_t n = r.count(5);
    EntityId prev{};
    for (std::uint32_t i = 0; i < n && r.ok; ++i) {
        const EntityId id = r.entity();
        T value{};
        get(r, value);
        if (!reg.alive(id) || (i > 0 && !(prev < id))) return false;  // ascending, live entities only
        pool.set(id, value);
        prev = id;
    }
    return r.ok;
}

constexpr const char* kPoolTags[] = {"XFRM", "DREF", "HLTH", "MEMB", "FTPR", "PATH", "REGT", "ORDR", "MOVR", "GRUP",
                                    "COMB", "ENGT", "PROJ", "STRC", "DPLY"};

std::uint64_t mapHashOf(const Sim& sim) {
    return sim.config().map != nullptr ? sim.config().map->mapHash : 0;
}

// Nav state that isn't derivable from the map + footprints: graph version, clusters stamped since
// the last rebuild, and the path service (queue, the job in progress, budget debt).
void putNav(ByteWriter& w, const nav::NavWorld& nav) {
    w.u32(nav.hpa.version());
    const auto& dirty = nav.grid.dirtyClusters();
    w.u32(static_cast<std::uint32_t>(dirty.size()));
    for (const std::uint32_t k : dirty) w.u32(k);
    nav.paths.save(w);
}

}  // namespace

std::vector<std::uint8_t> saveState(const Sim& sim) {
    ByteWriter w;
    w.tag("CSIM");
    w.u32(kStateFormatVersion);
    w.u32(sim.tick());
    w.u64(sim.config().matchSeed);
    w.u64(sim.config().defs != nullptr ? sim.config().defs->dataHash : 0);
    w.u64(mapHashOf(sim));
    w.u64(combatTuningHash(sim.tuning()));
    w.u64(sim.rng().rawState());
    w.u64(sim.rng().rawInc());

    const EntityRegistry::Raw reg = sim.world().registry.toRaw();
    w.tag("ENTS");
    w.u32(static_cast<std::uint32_t>(reg.generations.size()));
    for (std::size_t i = 0; i < reg.generations.size(); ++i) {
        w.u16(reg.generations[i]);
        w.u8(static_cast<std::uint8_t>(reg.states[i]));
    }
    w.u32(static_cast<std::uint32_t>(reg.freeList.size()));
    for (const std::uint32_t i : reg.freeList) w.u32(i);
    w.u32(static_cast<std::uint32_t>(reg.pendingDestroy.size()));
    for (const EntityId id : reg.pendingDestroy) w.entity(id);

    int poolNo = 0;
    const_cast<World&>(sim.world()).forEachPool([&](const auto& pool) {
        w.tag(kPoolTags[poolNo++]);
        putPool(w, pool);
    });

    w.tag("KING");
    for (const Kingdom& k : sim.world().kingdoms) {
        w.boolean(k.active);
        w.fixed(k.armyXp);
        w.boolean(k.started);
        w.fixed(k.money);
        w.boolean(k.bankrupt);
        putU32s(w, k.properties);
    }

    w.tag("NAVS");
    w.boolean(sim.nav() != nullptr);
    if (sim.nav() != nullptr) putNav(w, *sim.nav());

    return std::move(w.buf);
}

std::uint64_t stateHash(const Sim& sim) {
    return hashBytes(saveState(sim));
}

bool loadState(Sim& sim, const std::vector<std::uint8_t>& bytes, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };
    ByteReader r(bytes);
    if (!r.expectTag("CSIM")) return fail("not a sim state (missing CSIM)");
    if (r.u32() != kStateFormatVersion) return fail("unsupported state format version");
    const std::uint32_t tick = r.u32();
    if (r.u64() != sim.config().matchSeed) return fail("match seed differs from this Sim's config");
    if (r.u64() != (sim.config().defs != nullptr ? sim.config().defs->dataHash : 0)) return fail("data hash differs (other TGI data)");
    if (r.u64() != mapHashOf(sim)) return fail("map hash differs (other map)");
    if (r.u64() != combatTuningHash(sim.tuning())) return fail("combat tuning differs");
    const std::uint64_t rngState = r.u64();
    const std::uint64_t rngInc = r.u64();

    if (!r.expectTag("ENTS")) return fail("missing ENTS");
    EntityRegistry::Raw raw;
    const std::uint32_t slots = r.count(3);
    for (std::uint32_t i = 0; i < slots && r.ok; ++i) {
        raw.generations.push_back(r.u16());
        raw.states.push_back(static_cast<EntityRegistry::SlotState>(r.u8()));
    }
    const std::uint32_t nFree = r.count(4);
    for (std::uint32_t i = 0; i < nFree && r.ok; ++i) raw.freeList.push_back(r.u32());
    const std::uint32_t nPending = r.count(4);
    for (std::uint32_t i = 0; i < nPending && r.ok; ++i) raw.pendingDestroy.push_back(r.entity());
    if (!r.ok) return fail("truncated entity registry");

    World world;
    if (!world.registry.fromRaw(raw)) return fail("inconsistent entity registry");
    int poolNo = 0;
    bool poolsOk = true;
    world.forEachPool([&](auto& pool) {
        if (!poolsOk) return;
        if (!r.expectTag(kPoolTags[poolNo++]) || !getPool(r, pool, world.registry)) poolsOk = false;
    });
    if (!poolsOk) return fail("bad component pool");
    if (!r.expectTag("KING")) return fail("missing KING");
    for (Kingdom& k : world.kingdoms) {
        k.active = r.boolean();
        k.armyXp = r.fixed();
        k.started = r.boolean();
        k.money = r.fixed();
        k.bankrupt = r.boolean();
        getU32s(r, k.properties);
    }

    if (!r.expectTag("NAVS")) return fail("missing NAVS");
    const bool hasNav = r.boolean();
    if (hasNav != (sim.config().map != nullptr)) return fail("nav section doesn't match the map");
    std::uint32_t navVersion = 0;
    std::vector<std::uint32_t> dirty;
    nav::PathService paths;
    if (hasNav) {
        navVersion = r.u32();
        const std::uint32_t nd = r.count(4);
        for (std::uint32_t i = 0; i < nd && r.ok; ++i) dirty.push_back(r.u32());
        if (!paths.load(r)) return fail("bad path service state");
        for (const auto& fp : world.footprints.data()) {
            if (fp.x0 < 0 || fp.y0 < 0 || fp.x1 < fp.x0 || fp.y1 < fp.y0 ||
                static_cast<std::uint32_t>(fp.x1) >= sim.config().map->width || static_cast<std::uint32_t>(fp.y1) >= sim.config().map->height)
                return fail("footprint outside the map");
        }
    }

    if (!r.ok || r.remaining() != 0) return fail("truncated or trailing bytes");

    if (hasNav) {
        // Swap the old footprints' stamps for the loaded ones and rebuild every touched cluster
        // (plus any the old state still had dirty): the graph then equals a full build for the
        // loaded stamps (incremental == full, tested). Then restore what isn't derivable.
        nav::NavWorld& nav = *sim.nav();
        for (const auto& fp : sim.world().footprints.data()) nav.grid.unstamp({fp.x0, fp.y0, fp.x1, fp.y1});
        for (const auto& fp : world.footprints.data()) nav.grid.stamp({fp.x0, fp.y0, fp.x1, fp.y1});
        nav.hpa.rebuildDirty();
        for (const std::uint32_t k : dirty) nav.grid.markDirty(k);
        nav.hpa.restoreVersion(navVersion);
        nav.paths = std::move(paths);
    }
    sim.world() = std::move(world);
    sim.rng() = Rng::fromRaw(rngState, rngInc);
    sim.restoreTick(tick);
    return true;
}

}  // namespace corps
