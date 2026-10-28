// M8: lockstep wire format (NETCODE §4): fuzz-style round trips, mutations and truncations.

#include <doctest/doctest.h>

#include "core/command.hpp"
#include "math/rng.hpp"
#include "net/packets.hpp"

using namespace corps;
using namespace corps::net;

namespace {

Command randomCommand(Rng& rng, std::uint32_t tick, std::uint8_t player, std::uint16_t seq) {
    const auto fx = [&rng] { return Fixed::fromRaw(static_cast<std::int64_t>((std::uint64_t{rng.nextU32()} << 32) | rng.nextU32())); };
    const auto id = [&rng] { return EntityId::make(rng.nextU32() % 5000, rng.nextU32() % 4095); };
    const auto ids = [&rng, &id] {
        std::vector<EntityId> v(1 + rng.nextU32() % kMaxCommandRegiments);
        for (EntityId& e : v) e = id();
        return v;
    };
    const Angle a = Angle::fromUnits(rng.nextU32() & 0xFFFF);
    switch (rng.nextU32() % 12) {
    case 0: return makeNoop(tick, player, seq);
    case 1: return makeDebugSpawn(tick, player, seq, {fx(), fx()}, a);
    case 2: return makeDebugRngDraw(tick, player, seq, static_cast<std::uint16_t>(rng.nextU32()));
    case 3: return makeMoveGroup(tick, player, seq, ids(), {fx(), fx()});
    case 4: return makeMoveGroupFacing(tick, player, seq, ids(), {fx(), fx()}, a);
    case 5: return makeAttack(tick, player, seq, ids(), id());
    case 6: return makeSetFormation(tick, player, seq, ids(), "regiment_column");
    case 7: return makeRecruit(tick, player, seq, id(), "regiment_ger_infantry");
    case 8: return makeUnpack(tick, player, seq, id(), {fx(), fx()}, a);
    case 9: return makeStop(tick, player, seq, ids());
    case 10: return makeSpawnRegiment(tick, player, seq, "regiment_per_infantry", {fx(), fx()}, a);
    default: return makeDebugSetMoney(tick, player, seq, fx());
    }
}

Message randomMessage(Rng& rng) {
    const std::uint32_t tick = rng.nextU32();
    switch (rng.nextU32() % 6) {
    case 0: {
        CmdPacket p;
        p.tick = tick;
        p.seq = static_cast<std::uint16_t>(rng.nextU32());
        p.player = static_cast<std::uint8_t>(rng.nextU32() % kMaxNetPlayers);
        const std::uint32_t n = rng.nextU32() % (kMaxCommandsPerPlayerTick + 1);
        for (std::uint32_t i = 0; i < n; ++i) p.commands.push_back(randomCommand(rng, tick, p.player, static_cast<std::uint16_t>(i)));
        return p;
    }
    case 1: {
        TickBundle b;
        b.tick = tick;
        const std::uint32_t n = rng.nextU32() % 40;
        for (std::uint32_t i = 0; i < n; ++i) {
            b.commands.push_back(randomCommand(rng, tick, static_cast<std::uint8_t>(rng.nextU32() % 8),
                                               static_cast<std::uint16_t>(rng.nextU32() % 40)));
        }
        const std::uint32_t m = rng.nextU32() % 4;
        for (std::uint32_t i = 0; i < m; ++i) {
            b.controls.push_back({static_cast<ControlType>(400 + rng.nextU32() % 5), static_cast<std::uint8_t>(rng.nextU32() % 8),
                                  rng.nextU32()});
        }
        return b;
    }
    case 2: return HashReport{tick, static_cast<std::uint8_t>(rng.nextU32() % 8), (std::uint64_t{rng.nextU32()} << 32) | rng.nextU32()};
    case 3: return Ping{tick, rng.nextU32()};
    case 4: return Pong{tick, rng.nextU32()};
    default: {
        Chat c;
        c.player = static_cast<std::uint8_t>(rng.nextU32() % 8);
        const std::uint32_t n = rng.nextU32() % 200;
        for (std::uint32_t i = 0; i < n; ++i) c.text.push_back(static_cast<char>(32 + rng.nextU32() % 90));
        return c;
    }
    }
}

}  // namespace

TEST_CASE("Packets: random messages round-trip exactly") {
    Rng rng(2026, 8);
    std::size_t bytes = 0;
    for (int i = 0; i < 2000; ++i) {
        const Message m = randomMessage(rng);
        const std::vector<std::uint8_t> b = encodeMessage(m);
        bytes += b.size();
        Message back;
        REQUIRE(decodeMessage(b, back));
        CHECK(back == m);
        CHECK(encodeMessage(back) == b);
    }
    CHECK(bytes > 0);
}

TEST_CASE("Packets: truncation, trailing bytes and random corruption never crash or mis-decode") {
    Rng rng(77, 1);
    for (int i = 0; i < 400; ++i) {
        const Message m = randomMessage(rng);
        const std::vector<std::uint8_t> b = encodeMessage(m);
        Message out;
        for (std::size_t cut = 0; cut < b.size(); ++cut) {
            const std::vector<std::uint8_t> t(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(cut));
            if (!decodeMessage(t, out)) continue;
            // A shorter prefix can only decode when it is itself a complete, valid message: it
            // must then re-encode to exactly those bytes.
            CHECK(encodeMessage(out) == t);
        }
        std::vector<std::uint8_t> longer = b;
        longer.push_back(0);
        CHECK_FALSE(decodeMessage(longer, out));
        for (int k = 0; k < 8; ++k) {
            std::vector<std::uint8_t> f = b;
            f[rng.nextU32() % f.size()] ^= static_cast<std::uint8_t>(1u << (rng.nextU32() % 8));
            if (!decodeMessage(f, out)) continue;
            // Accepted corruption (a flipped value) still decodes to a message that round-trips.
            Message again;
            REQUIRE(decodeMessage(encodeMessage(out), again));
            CHECK(again == out);
        }
    }
    Message out;
    CHECK_FALSE(decodeMessage({}, out));
    CHECK_FALSE(decodeMessage({99, 0, 0}, out));
}

TEST_CASE("Packets: a CmdPacket over the per-tick command limit is refused") {
    CmdPacket p;
    p.player = 1;
    for (std::size_t i = 0; i < kMaxCommandsPerPlayerTick; ++i) p.commands.push_back(makeNoop(0, 1, static_cast<std::uint16_t>(i)));
    std::vector<std::uint8_t> b = encodeMessage(p);
    Message out;
    REQUIRE(decodeMessage(b, out));
    // Claim one more command and append it: the decoder refuses the count.
    b[7] = static_cast<std::uint8_t>(kMaxCommandsPerPlayerTick + 1);
    encodeCommand(makeNoop(0, 1, 0), b);
    CHECK_FALSE(decodeMessage(b, out));
    // Bad player and a control type outside 400..404.
    CmdPacket q;
    q.player = 9;
    CHECK_FALSE(decodeMessage(encodeMessage(q), out));
    TickBundle t;
    t.controls.push_back({static_cast<ControlType>(499), 0, 0});
    CHECK_FALSE(decodeMessage(encodeMessage(t), out));
    // Size: an empty heartbeat is 9 bytes, a typical move 20-30 (NETCODE §10).
    CmdPacket empty;
    CHECK(encodeMessage(empty).size() == 9);
}
