#include "session/state_io.hpp"

#include <map>
#include <string>

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
    w.boolean(s.inSupply);
    w.boolean(s.underSiege);
    w.u32(s.lastDamaged);
    w.u8(s.capturer);
    w.u32(s.captureTicks);
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
    s.inSupply = r.boolean();
    s.underSiege = r.boolean();
    s.lastDamaged = r.u32();
    s.capturer = r.u8();
    s.captureTicks = r.u32();
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
void put(ByteWriter& w, const Supply& s) {
    w.boolean(s.inSupply);
    w.boolean(s.besieged);
    w.u32(s.replaceTicks);
}
void get(ByteReader& r, Supply& s) {
    s.inSupply = r.boolean();
    s.besieged = r.boolean();
    s.replaceTicks = r.u32();
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
                                    "COMB", "ENGT", "PROJ", "STRC", "DPLY", "SUPL"};

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
        w.u8(k.team);
        w.boolean(k.eliminated);
    }
    const Match& m = sim.world().match;
    w.tag("MTCH");
    w.fixed(m.cityVictoryRatio);
    w.boolean(m.teamVictory);
    w.fixed(m.startMoney);
    w.boolean(m.over);
    w.u8(m.winner);
    w.u32(m.endTick);

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
        k.team = r.u8();
        k.eliminated = r.boolean();
    }
    if (!r.expectTag("MTCH")) return fail("missing MTCH");
    world.match.cityVictoryRatio = r.fixed();
    world.match.teamVictory = r.boolean();
    world.match.startMoney = r.fixed();
    world.match.over = r.boolean();
    world.match.winner = r.u8();
    world.match.endTick = r.u32();

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

// --- simcli diff ---

namespace {

struct StateItem {
    std::string label;
    std::size_t start = 0;
    std::size_t end = 0;
};

std::string entityLabel(EntityId id) {
    return "#" + std::to_string(id.index()) + "." + std::to_string(id.generation());
}

template <class T>
void walkPool(ByteReader& r, const Pool<T>&, const char* tag, std::vector<StateItem>& out) {
    const std::uint32_t n = r.count(5);
    for (std::uint32_t i = 0; i < n && r.ok; ++i) {
        const std::size_t start = r.position();
        const EntityId id = r.entity();
        T value{};
        get(r, value);
        out.push_back({std::string(tag) + " " + entityLabel(id), start, r.position()});
    }
}

// Field names inside an entry (byte offsets; the u32 entity id comes first) for the common components.
std::string fieldAt(const std::string& label, std::size_t offset) {
    struct Field {
        const char* tag;
        std::size_t from;
        const char* name;
    };
    static const Field kFields[] = {
        {"XFRM", 0, "entity"}, {"XFRM", 4, "position.x"},   {"XFRM", 12, "position.y"}, {"XFRM", 20, "facing"},
        {"HLTH", 0, "entity"}, {"HLTH", 4, "current"},      {"HLTH", 12, "max"},
        {"MOVR", 0, "entity"}, {"MOVR", 4, "velocity.x"},   {"MOVR", 12, "velocity.y"},
        {"MEMB", 0, "entity"}, {"MEMB", 4, "organization"}, {"MEMB", 8, "slot"},
        {"DREF", 0, "entity"}, {"DREF", 4, "kind"},         {"DREF", 5, "index"},
        {"ORDR", 0, "entity"}, {"ORDR", 4, "state"},        {"ORDR", 5, "flags"},      {"ORDR", 10, "facing"},
        {"ORDR", 12, "goal"},  {"ORDR", 28, "waypoint"},    {"ORDR", 32, "stateTicks"}, {"ORDR", 36, "progress"},
        {"ORDR", 44, "expected"},
    };
    const std::string tag = label.substr(0, 4);
    const char* name = nullptr;
    for (const Field& f : kFields) {
        if (tag == f.tag && offset >= f.from) name = f.name;
    }
    return name != nullptr ? std::string(name) : "byte +" + std::to_string(offset);
}

bool walkState(const std::vector<std::uint8_t>& bytes, std::vector<StateItem>& out) {
    ByteReader r(bytes);
    const auto field = [&](const char* label, int n) {
        const std::size_t start = r.position();
        for (int i = 0; i < n; ++i) r.u8();
        out.push_back({std::string("header ") + label, start, r.position()});
    };
    if (!r.expectTag("CSIM")) return false;
    field("format", 4);
    field("tick", 4);
    field("matchSeed", 8);
    field("dataHash", 8);
    field("mapHash", 8);
    field("tuningHash", 8);
    field("rng.state", 8);
    field("rng.inc", 8);
    if (!r.expectTag("ENTS")) return false;
    const std::uint32_t slots = r.count(3);
    for (std::uint32_t i = 0; i < slots && r.ok; ++i) {
        const std::size_t start = r.position();
        r.u16();
        r.u8();
        out.push_back({"ENTS slot " + std::to_string(i) + " (generation, state)", start, r.position()});
    }
    std::size_t start = r.position();
    const std::uint32_t nFree = r.count(4);
    for (std::uint32_t i = 0; i < nFree && r.ok; ++i) r.u32();
    out.push_back({"ENTS free list", start, r.position()});
    start = r.position();
    const std::uint32_t nPending = r.count(4);
    for (std::uint32_t i = 0; i < nPending && r.ok; ++i) r.u32();
    out.push_back({"ENTS pending destruction", start, r.position()});
    World world;
    int poolNo = 0;
    bool ok = r.ok;
    world.forEachPool([&](const auto& pool) {
        if (!ok) return;
        const char* tag = kPoolTags[poolNo++];
        if (!r.expectTag(tag)) {
            ok = false;
            return;
        }
        out.push_back({std::string(tag) + " count", r.position(), r.position() + 4});
        walkPool(r, pool, tag, out);
        ok = r.ok;
    });
    if (!ok || !r.expectTag("KING")) return false;
    for (std::size_t p = 0; p < kMaxPlayers; ++p) {
        start = r.position();
        r.boolean();
        r.fixed();
        r.boolean();
        r.fixed();
        r.boolean();
        const std::uint32_t n = r.count(4);
        for (std::uint32_t i = 0; i < n && r.ok; ++i) r.u32();
        r.u8();
        r.boolean();
        out.push_back({"KING player " + std::to_string(p), start, r.position()});
    }
    start = r.position();
    if (!r.expectTag("MTCH")) return false;
    r.fixed();
    r.boolean();
    r.fixed();
    r.boolean();
    r.u8();
    r.u32();
    out.push_back({"MTCH", start, r.position()});
    out.push_back({"NAVS (nav version, dirty clusters, path service)", r.position(), bytes.size()});
    return r.ok;
}

std::string hexRange(const std::vector<std::uint8_t>& b, std::size_t from, std::size_t to) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    for (std::size_t i = from; i < to && i < from + 48; ++i) {
        s.push_back(kHex[b[i] >> 4]);
        s.push_back(kHex[b[i] & 15]);
    }
    if (to - from > 48) s += "...";
    return s;
}

}  // namespace

StateDiff diffStates(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    StateDiff d;
    std::vector<StateItem> ia;
    std::vector<StateItem> ib;
    if (!walkState(a, ia) || !walkState(b, ib)) {
        d.malformed = true;
        d.equal = a == b;
        d.where = "unparseable state (format version?)";
        return d;
    }
    // Both lists are in canonical order. Items match by label: equal labels compare bytes, a label
    // in one state only is a difference.
    std::map<std::string, std::size_t> inA;
    std::map<std::string, std::size_t> inB;
    for (std::size_t i = 0; i < ia.size(); ++i) inA.emplace(ia[i].label, i);
    for (std::size_t i = 0; i < ib.size(); ++i) inB.emplace(ib[i].label, i);
    const auto note = [&](const std::string& where, const std::string& ha, const std::string& hb) {
        if (d.differingItems++ == 0) {
            d.equal = false;
            d.where = where;
            d.a = ha;
            d.b = hb;
        }
    };
    for (const StateItem& x : ia) {
        const auto it = inB.find(x.label);
        if (it == inB.end()) {
            note(x.label + " (only in a)", hexRange(a, x.start, x.end), "-");
            continue;
        }
        const StateItem& y = ib[it->second];
        const std::size_t na = x.end - x.start;
        const std::size_t nb = y.end - y.start;
        std::size_t k = 0;
        while (k < na && k < nb && a[x.start + k] == b[y.start + k]) ++k;
        if (k == na && k == nb) continue;
        const bool entity = x.label.size() > 5 && x.label[5] == '#';
        note(entity ? x.label + " " + fieldAt(x.label, k) : x.label + (k > 0 ? " byte +" + std::to_string(k) : ""),
             hexRange(a, x.start, x.end), hexRange(b, y.start, y.end));
    }
    for (const StateItem& y : ib) {
        if (inA.count(y.label) == 0) note(y.label + " (only in b)", "-", hexRange(b, y.start, y.end));
    }
    return d;
}

}  // namespace corps
