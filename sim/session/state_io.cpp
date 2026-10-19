#include "session/state_io.hpp"

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

constexpr const char* kPoolTags[] = {"XFRM", "DREF", "HLTH", "MEMB"};

}  // namespace

std::vector<std::uint8_t> saveState(const Sim& sim) {
    ByteWriter w;
    w.tag("CSIM");
    w.u32(kStateFormatVersion);
    w.u32(sim.tick());
    w.u64(sim.config().matchSeed);
    w.u64(sim.config().defs != nullptr ? sim.config().defs->dataHash : 0);
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

    if (!r.ok || r.remaining() != 0) return fail("truncated or trailing bytes");

    sim.world() = std::move(world);
    sim.rng() = Rng::fromRaw(rngState, rngInc);
    sim.restoreTick(tick);
    return true;
}

}  // namespace corps
