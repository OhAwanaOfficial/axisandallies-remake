// M3 part 2: ECS, commands, tick loop, canonical serializer + hash, replays, determinism checks.

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <iterator>

#include "core/command.hpp"
#include "core/sim.hpp"
#include "ecs/entity.hpp"
#include "ecs/pool.hpp"
#include "ecs/world.hpp"
#include "math/rng.hpp"
#include "session/bytes.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"
#include "test_env.hpp"
#include "vfs/path.hpp"

using namespace corps;
using namespace corps::fixed_literals;

// ------------------------------------------------------------------------------------- ECS

TEST_CASE("EntityRegistry: ids, generations and deferred destruction") {
    EntityRegistry reg;
    const EntityId a = reg.create();
    const EntityId b = reg.create();
    CHECK_FALSE(a.isNull());
    CHECK(a.index() == 0);
    CHECK(b.index() == 1);
    CHECK(a.generation() == 1);
    CHECK(reg.alive(a));
    CHECK(reg.destroyLater(a));
    CHECK(reg.destroyLater(a));  // idempotent
    CHECK(reg.alive(a));         // still alive until cleanup
    const auto dead = reg.cleanup();
    REQUIRE(dead.size() == 1);
    CHECK(dead[0] == a);
    CHECK_FALSE(reg.alive(a));
    CHECK_FALSE(reg.destroyLater(a));  // stale id
    const EntityId c = reg.create();     // reuses index 0 with a new generation
    CHECK(c.index() == 0);
    CHECK(c.generation() == 2);
    CHECK(c != a);
    CHECK_FALSE(reg.alive(a));
    CHECK(reg.alive(c));
    CHECK_FALSE(reg.alive(EntityId{}));
    CHECK(reg.aliveCount() == 2);
}

TEST_CASE("EntityRegistry: free list is FIFO and a slot retires instead of wrapping its generation") {
    EntityRegistry reg;
    std::vector<EntityId> ids;
    for (int i = 0; i < 4; ++i) ids.push_back(reg.create());
    reg.destroyLater(ids[2]);
    reg.destroyLater(ids[0]);
    reg.cleanup();
    CHECK(reg.create().index() == 0);  // ascending destruction order → 0 freed first
    CHECK(reg.create().index() == 2);
    CHECK(reg.create().index() == 4);

    EntityRegistry r2;
    EntityId e = r2.create();
    for (std::uint32_t g = 1; g < EntityId::kMaxGeneration; ++g) {
        r2.destroyLater(e);
        r2.cleanup();
        e = r2.create();
        REQUIRE(e.index() == 0);
    }
    CHECK(e.generation() == EntityId::kMaxGeneration);
    r2.destroyLater(e);
    r2.cleanup();
    const EntityId next = r2.create();
    CHECK(next.index() == 1);  // slot 0 retired
    CHECK(r2.toRaw().states[0] == EntityRegistry::SlotState::Retired);
}

TEST_CASE("EntityRegistry: raw round trip and validation") {
    EntityRegistry reg;
    for (int i = 0; i < 10; ++i) reg.create();
    reg.destroyLater(EntityId::make(3, 1));
    reg.destroyLater(EntityId::make(7, 1));
    reg.cleanup();
    reg.destroyLater(EntityId::make(5, 1));
    EntityRegistry copy;
    REQUIRE(copy.fromRaw(reg.toRaw()));
    CHECK(copy.aliveCount() == reg.aliveCount());
    CHECK(copy.create() == reg.create());
    EntityRegistry::Raw bad = reg.toRaw();
    bad.freeList.push_back(0);  // an alive slot in the free list
    CHECK_FALSE(copy.fromRaw(bad));
}

TEST_CASE("Pool: iteration stays in ascending EntityId order under random churn") {
    Rng rng(5, 5);
    EntityRegistry reg;
    Pool<int> pool;
    std::vector<EntityId> live;
    for (int step = 0; step < 5000; ++step) {
        if (live.empty() || rng.below(3) != 0) {
            const EntityId id = reg.create();
            pool.set(id, static_cast<int>(id.bits));
            live.push_back(id);
        } else {
            const std::size_t k = rng.below(static_cast<std::uint32_t>(live.size()));
            const EntityId id = live[k];
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
            CHECK(pool.removeIndex(id.index()));
            reg.destroyLater(id);
            reg.cleanup();
        }
        if (step % 97 == 0) {
            REQUIRE(std::is_sorted(pool.ids().begin(), pool.ids().end()));
            REQUIRE(pool.size() == live.size());
        }
    }
    for (std::size_t i = 0; i < pool.size(); ++i) {
        REQUIRE(*pool.get(pool.ids()[i]) == static_cast<int>(pool.ids()[i].bits));
    }
    const EntityId stale = EntityId::make(pool.ids()[0].index(), pool.ids()[0].generation() + 1);
    CHECK(pool.get(stale) == nullptr);
}

// ------------------------------------------------------------------------------------- commands

TEST_CASE("Command encoding round-trips every type; malformed input is rejected") {
    const std::vector<Command> cmds = {
        makeNoop(5, 1, 2),
        makeSpawnRegiment(6, 0, 1, "regiment_per_infantry", {-12.5_fx, 7_fx}, Angle::fromUnits(1234)),
        makeDebugSpawn(7, 3, 9, {Fixed::min(), Fixed::max()}, Angle::fromUnits(65535)),
        makeDebugDestroy(8, 2, 1, EntityId::make(77, 3)),
        makeDebugSetTransform(9, 2, 2, EntityId::make(1, 4095), {0.5_fx, -0.25_fx}, Angle{}),
        makeDebugRngDraw(10, 0, 0, 65535),
    };
    for (const Command& c : cmds) {
        std::vector<std::uint8_t> b;
        encodeCommand(c, b);
        Command d;
        d.tick = c.tick;
        d.seq = c.seq;
        std::size_t pos = 0;
        REQUIRE(decodeCommand(b, pos, d));
        CHECK(pos == b.size());
        CHECK(d == c);
        for (std::size_t cut = 0; cut + 1 < b.size(); ++cut) {  // every truncation fails cleanly
            std::vector<std::uint8_t> t(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(cut));
            std::size_t p = 0;
            Command x;
            CHECK_FALSE(decodeCommand(t, p, x));
        }
    }
    std::vector<std::uint8_t> unknown = {150, 0};
    std::size_t pos = 0;
    Command x;
    CHECK_FALSE(decodeCommand(unknown, pos, x));
}

TEST_CASE("CommandQueue: ordering, ties and past ticks") {
    CommandQueue q;
    CHECK(q.schedule(makeDebugRngDraw(5, 1, 0, 1), 0));
    CHECK(q.schedule(makeDebugRngDraw(5, 0, 3, 2), 0));
    CHECK(q.schedule(makeDebugRngDraw(4, 1, 0, 3), 0));
    CHECK(q.schedule(makeDebugRngDraw(5, 0, 3, 4), 0));  // same key: arrival order
    CHECK_FALSE(q.schedule(makeDebugRngDraw(2, 0, 0, 5), 3));
    CHECK(q.take(3).empty());
    const auto t4 = q.take(4);
    REQUIRE(t4.size() == 1);
    const auto t5 = q.take(5);
    REQUIRE(t5.size() == 3);
    CHECK(std::get<DebugRngDrawPayload>(t5[0].payload).count == 2);
    CHECK(std::get<DebugRngDrawPayload>(t5[1].payload).count == 4);
    CHECK(t5[2].player == 1);
}

// ------------------------------------------------------------------------------------- tick loop

TEST_CASE("Sim runs the 13 stages in ARCHITECTURE order every tick") {
    Sim sim({1, nullptr});
    std::vector<Stage> seen;
    sim.afterStage = [&](Sim&, Stage s) { seen.push_back(s); };
    sim.step();
    sim.step();
    REQUIRE(seen.size() == 2 * kStageCount);
    for (std::size_t i = 0; i < seen.size(); ++i) CHECK(seen[i] == static_cast<Stage>(i % kStageCount));
    CHECK(sim.tick() == 2);
    CHECK(std::string(stageName(Stage::Pathfinding)) == "Pathfinding");
}

TEST_CASE("Sim applies commands in (player, seq) order and rejects other ticks") {
    Sim sim({7, nullptr});
    sim.step({makeDebugSpawn(0, 1, 0, {1_fx, 1_fx}, Angle{}), makeDebugSpawn(0, 0, 5, {2_fx, 2_fx}, Angle{}),
              makeDebugSpawn(0, 0, 1, {3_fx, 3_fx}, Angle{}), makeDebugSpawn(3, 0, 0, {9_fx, 9_fx}, Angle{})});
    const auto& t = sim.world().transforms;
    REQUIRE(t.size() == 3);
    CHECK(t.data()[0].position.x == 3_fx);  // player 0 seq 1 first
    CHECK(t.data()[1].position.x == 2_fx);
    CHECK(t.data()[2].position.x == 1_fx);
    const auto rejected = std::count_if(sim.events().begin(), sim.events().end(),
                                        [](const SimEvent& e) { return e.type == SimEvent::Type::CommandRejected; });
    CHECK(rejected == 1);
    // Destroy happens at Cleanup of the same tick; the component is gone afterwards.
    const EntityId first = t.ids()[0];
    sim.step({makeDebugDestroy(1, 0, 0, first)});
    CHECK_FALSE(sim.world().registry.alive(first));
    CHECK(sim.world().transforms.size() == 2);
    // Spawning a regiment without data is rejected, not a crash.
    sim.step({makeSpawnRegiment(2, 0, 0, "regiment_x", {}, Angle{})});
    CHECK(sim.world().registry.aliveCount() == 2);
}

// ------------------------------------------------------------------------------------- serializer

namespace {
Sim busySim(std::uint64_t seed, std::uint32_t ticks) {
    const Replay rep = recordSynthetic(seed, seed + 1, ticks, 4, nullptr);
    Sim sim({seed, nullptr});
    CommandQueue q;
    for (const Command& c : rep.commands) q.schedule(c, 0);
    for (std::uint32_t t = 0; t < ticks; ++t) sim.step(q.take(t));
    return sim;
}
}  // namespace

TEST_CASE("State save/load round-trips and the loaded sim continues identically") {
    Sim a = busySim(11, 120);
    const std::vector<std::uint8_t> saved = saveState(a);
    Sim b({11, nullptr});
    std::string err;
    REQUIRE(loadState(b, saved, &err));
    CHECK(saveState(b) == saved);
    CHECK(stateHash(b) == stateHash(a));
    const std::vector<Command> more = {makeDebugSpawn(a.tick(), 0, 0, {5_fx, 5_fx}, Angle{}),
                                       makeDebugRngDraw(a.tick(), 1, 0, 3)};
    for (int i = 0; i < 20; ++i) {
        std::vector<Command> cmds;
        if (i == 0) cmds = more;
        a.step(cmds);
        b.step(cmds);
        REQUIRE(stateHash(a) == stateHash(b));
    }
}

TEST_CASE("State load rejects corrupt and mismatched input") {
    Sim a = busySim(3, 50);
    std::vector<std::uint8_t> s = saveState(a);
    std::string err;
    Sim wrongSeed({4, nullptr});
    CHECK_FALSE(loadState(wrongSeed, s, &err));
    Sim b({3, nullptr});
    for (std::size_t cut : {std::size_t{0}, std::size_t{4}, s.size() / 2, s.size() - 1}) {
        std::vector<std::uint8_t> t(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(loadState(b, t, &err));
    }
    std::vector<std::uint8_t> extra = s;
    extra.push_back(0);
    CHECK_FALSE(loadState(b, extra, &err));
    CHECK(b.tick() == 0);  // failed loads leave the sim untouched
}

TEST_CASE("State hash changes with any state change") {
    Sim a({9, nullptr});
    const std::uint64_t h0 = stateHash(a);
    a.step({makeDebugRngDraw(0, 0, 0, 1)});
    const std::uint64_t h1 = stateHash(a);
    CHECK(h1 != h0);
    Sim b({9, nullptr});
    b.step();
    CHECK(stateHash(b) != h1);  // same tick, no RNG draw
    CHECK(hashBytes(std::vector<std::uint8_t>{}) == 0x2D06800538D394C2ULL);  // XXH3_64("") reference
}

// ------------------------------------------------------------------------------------- replays

TEST_CASE("Replay encode/decode round-trips; corruption is detected") {
    const Replay rep = recordSynthetic(21, 22, 200, 3, nullptr);
    const std::vector<std::uint8_t> bytes = encodeReplay(rep);
    Replay back;
    std::string err;
    REQUIRE(decodeReplay(bytes, back, &err));
    CHECK(back.header.matchSeed == 21);
    CHECK(back.header.tickCount == 200);
    CHECK(back.commands == rep.commands);
    CHECK(back.hashes == rep.hashes);
    std::vector<std::uint8_t> flipped = bytes;
    flipped[bytes.size() / 2] ^= 0x10;
    CHECK_FALSE(decodeReplay(flipped, back, &err));
}

TEST_CASE("Replay playback reproduces the recorded hashes; two runs agree tick by tick") {
    const Replay rep = recordSynthetic(31, 32, 300, 4, nullptr);
    RunOptions every;
    every.everyTick = true;
    const RunResult a = runReplay(rep, nullptr, every);
    const RunResult b = runReplay(rep, nullptr, every);
    CHECK(a.hashes.size() == 300);
    CHECK_FALSE(firstDivergence(a, b).found);
    CHECK_FALSE(divergenceFromRecorded(rep, a).found);
    CHECK(a.rejectedSchedules == 0);
}

TEST_CASE("determinism check pinpoints an injected divergence (tick and stage)") {
    const Replay rep = recordSynthetic(41, 42, 120, 3, nullptr);
    RunOptions clean;
    clean.perStage = true;
    clean.everyTick = true;
    RunOptions dirty = clean;
    dirty.afterStage = [](Sim& s, Stage st) {
        if (s.tick() == 77 && st == Stage::Morale) s.rng().nextU32();  // a "system" drawing randomly
    };
    const RunResult a = runReplay(rep, nullptr, clean);
    const RunResult b = runReplay(rep, nullptr, dirty);
    const Divergence d = firstDivergence(a, b);
    REQUIRE(d.found);
    CHECK(d.tick == 77);
    CHECK(d.stage == static_cast<int>(Stage::Morale));
    CHECK(divergenceFromRecorded(rep, b).found);
}

TEST_CASE("committed synthetic replay plays identically on this compiler (cross-platform fixture)") {
    // sim/tests/fixtures/synthetic_300.rpl: 300 ticks, seed 2026, 3 random commands per tick,
    // recorded with MSVC x64. Every CI platform must reproduce its stored hashes.
    const std::string path = CORPS_SOURCE_DIR "/sim/tests/fixtures/synthetic_300.rpl";
    std::ifstream in(vfs::pathFromUtf8(path), std::ios::binary);
    if (testEnvSet("CORPS_WRITE_FIXTURES") || !in) {
        const Replay rep = recordSynthetic(2026, 2027, 300, 3, nullptr);
        std::ofstream out(vfs::pathFromUtf8(path), std::ios::binary);
        const auto b = encodeReplay(rep);
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        FAIL_CHECK("fixture (re)written; commit it and re-run");
        return;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    Replay rep;
    std::string err;
    REQUIRE(decodeReplay(bytes, rep, &err));
    const RunResult run = runReplay(rep, nullptr);
    const Divergence d = divergenceFromRecorded(rep, run);
    CHECK_FALSE(d.found);
    CHECK(run.finalHash == rep.hashes.back().hash);
    // The fixture must also match a fresh recording (recorder and sim both deterministic).
    const Replay again = recordSynthetic(2026, 2027, 300, 3, nullptr);
    CHECK(again.commands == rep.commands);
    CHECK(again.hashes == rep.hashes);
}
