#include "core/command.hpp"

#include <algorithm>

namespace corps {

namespace {

Command base(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, CommandType type) {
    Command c;
    c.tick = tick;
    c.player = player;
    c.seq = seq;
    c.type = type;
    return c;
}

std::uint64_t zigzag(std::int64_t v) {
    return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
}

std::int64_t unzigzag(std::uint64_t v) {
    return static_cast<std::int64_t>(v >> 1) ^ -static_cast<std::int64_t>(v & 1u);
}

void putFixed(std::vector<std::uint8_t>& out, Fixed f) {
    putVarint(out, zigzag(f.raw));
}

void putVec(std::vector<std::uint8_t>& out, FVec2 v) {
    putFixed(out, v.x);
    putFixed(out, v.y);
}

bool getFixed(const std::vector<std::uint8_t>& in, std::size_t& pos, Fixed& f) {
    std::uint64_t v = 0;
    if (!getVarint(in, pos, v)) return false;
    f = Fixed::fromRaw(unzigzag(v));
    return true;
}

bool getVec(const std::vector<std::uint8_t>& in, std::size_t& pos, FVec2& v) {
    return getFixed(in, pos, v.x) && getFixed(in, pos, v.y);
}

bool getU(const std::vector<std::uint8_t>& in, std::size_t& pos, std::uint64_t max, std::uint64_t& v) {
    return getVarint(in, pos, v) && v <= max;
}

}  // namespace

Command makeNoop(std::uint32_t tick, std::uint8_t player, std::uint16_t seq) {
    return base(tick, player, seq, CommandType::Noop);
}

Command makeSpawnRegiment(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::string ids, FVec2 pos, Angle facing) {
    Command c = base(tick, player, seq, CommandType::SpawnRegiment);
    c.payload = SpawnRegimentPayload{std::move(ids), pos, facing};
    return c;
}

Command makeDebugSpawn(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, FVec2 pos, Angle facing) {
    Command c = base(tick, player, seq, CommandType::DebugSpawn);
    c.payload = DebugSpawnPayload{pos, facing};
    return c;
}

Command makeDebugDestroy(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId target) {
    Command c = base(tick, player, seq, CommandType::DebugDestroy);
    c.payload = DebugDestroyPayload{target};
    return c;
}

Command makeDebugSetTransform(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId target, FVec2 pos, Angle facing) {
    Command c = base(tick, player, seq, CommandType::DebugSetTransform);
    c.payload = DebugSetTransformPayload{target, pos, facing};
    return c;
}

Command makeDebugRngDraw(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::uint16_t count) {
    Command c = base(tick, player, seq, CommandType::DebugRngDraw);
    c.payload = DebugRngDrawPayload{count};
    return c;
}

Command makeDebugPlaceBlocker(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, FVec2 pos, FVec2 halfExtent) {
    Command c = base(tick, player, seq, CommandType::DebugPlaceBlocker);
    c.payload = DebugPlaceBlockerPayload{pos, halfExtent};
    return c;
}

Command makeDebugRequestPath(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId target, FVec2 goal,
                             std::uint8_t moveClass) {
    Command c = base(tick, player, seq, CommandType::DebugRequestPath);
    c.payload = DebugRequestPathPayload{target, goal, moveClass};
    return c;
}

void putVarint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

bool getVarint(const std::vector<std::uint8_t>& in, std::size_t& pos, std::uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (pos >= in.size()) return false;
        const std::uint8_t b = in[pos++];
        v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return shift < 63 || b <= 1;  // reject bits beyond 64
    }
    return false;
}

void encodeCommand(const Command& c, std::vector<std::uint8_t>& out) {
    out.push_back(static_cast<std::uint8_t>(c.type));
    out.push_back(c.player);
    switch (c.type) {
    case CommandType::Noop:
        break;
    case CommandType::SpawnRegiment: {
        const auto& p = std::get<SpawnRegimentPayload>(c.payload);
        putVarint(out, p.regimentIds.size());
        out.insert(out.end(), p.regimentIds.begin(), p.regimentIds.end());
        putVec(out, p.position);
        putVarint(out, p.facing.units);
        break;
    }
    case CommandType::DebugSpawn: {
        const auto& p = std::get<DebugSpawnPayload>(c.payload);
        putVec(out, p.position);
        putVarint(out, p.facing.units);
        break;
    }
    case CommandType::DebugDestroy:
        putVarint(out, std::get<DebugDestroyPayload>(c.payload).target.bits);
        break;
    case CommandType::DebugSetTransform: {
        const auto& p = std::get<DebugSetTransformPayload>(c.payload);
        putVarint(out, p.target.bits);
        putVec(out, p.position);
        putVarint(out, p.facing.units);
        break;
    }
    case CommandType::DebugRngDraw:
        putVarint(out, std::get<DebugRngDrawPayload>(c.payload).count);
        break;
    case CommandType::DebugPlaceBlocker: {
        const auto& p = std::get<DebugPlaceBlockerPayload>(c.payload);
        putVec(out, p.position);
        putVec(out, p.halfExtent);
        break;
    }
    case CommandType::DebugRequestPath: {
        const auto& p = std::get<DebugRequestPathPayload>(c.payload);
        putVarint(out, p.target.bits);
        putVec(out, p.goal);
        putVarint(out, p.moveClass);
        break;
    }
    }
}

bool decodeCommand(const std::vector<std::uint8_t>& in, std::size_t& pos, Command& out) {
    if (pos + 2 > in.size()) return false;
    out.type = static_cast<CommandType>(in[pos++]);
    out.player = in[pos++];
    std::uint64_t v = 0;
    switch (out.type) {
    case CommandType::Noop:
        out.payload = NoPayload{};
        return true;
    case CommandType::SpawnRegiment: {
        SpawnRegimentPayload p;
        if (!getU(in, pos, 256, v) || pos + v > in.size()) return false;
        p.regimentIds.assign(in.begin() + static_cast<std::ptrdiff_t>(pos), in.begin() + static_cast<std::ptrdiff_t>(pos + v));
        pos += v;
        if (!getVec(in, pos, p.position) || !getU(in, pos, 0xFFFF, v)) return false;
        p.facing = Angle::fromUnits(static_cast<std::uint32_t>(v));
        out.payload = std::move(p);
        return true;
    }
    case CommandType::DebugSpawn: {
        DebugSpawnPayload p;
        if (!getVec(in, pos, p.position) || !getU(in, pos, 0xFFFF, v)) return false;
        p.facing = Angle::fromUnits(static_cast<std::uint32_t>(v));
        out.payload = p;
        return true;
    }
    case CommandType::DebugDestroy:
        if (!getU(in, pos, 0xFFFFFFFFu, v)) return false;
        out.payload = DebugDestroyPayload{EntityId{static_cast<std::uint32_t>(v)}};
        return true;
    case CommandType::DebugSetTransform: {
        DebugSetTransformPayload p;
        if (!getU(in, pos, 0xFFFFFFFFu, v)) return false;
        p.target = EntityId{static_cast<std::uint32_t>(v)};
        if (!getVec(in, pos, p.position) || !getU(in, pos, 0xFFFF, v)) return false;
        p.facing = Angle::fromUnits(static_cast<std::uint32_t>(v));
        out.payload = p;
        return true;
    }
    case CommandType::DebugRngDraw:
        if (!getU(in, pos, 0xFFFF, v)) return false;
        out.payload = DebugRngDrawPayload{static_cast<std::uint16_t>(v)};
        return true;
    case CommandType::DebugPlaceBlocker: {
        DebugPlaceBlockerPayload p;
        if (!getVec(in, pos, p.position) || !getVec(in, pos, p.halfExtent)) return false;
        out.payload = p;
        return true;
    }
    case CommandType::DebugRequestPath: {
        DebugRequestPathPayload p;
        if (!getU(in, pos, 0xFFFFFFFFu, v)) return false;
        p.target = EntityId{static_cast<std::uint32_t>(v)};
        if (!getVec(in, pos, p.goal) || !getU(in, pos, 0xFF, v)) return false;
        p.moveClass = static_cast<std::uint8_t>(v);
        out.payload = p;
        return true;
    }
    }
    return false;  // unknown type
}

bool CommandQueue::schedule(const Command& c, std::uint32_t nextTick) {
    if (c.tick < nextTick) return false;
    // Stable insert after every command with the same (tick, player, seq): arrival order breaks ties.
    const auto it = std::upper_bound(queue.begin(), queue.end(), c, [](const Command& a, const Command& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.player != b.player) return a.player < b.player;
        return a.seq < b.seq;
    });
    queue.insert(it, c);
    return true;
}

std::vector<Command> CommandQueue::take(std::uint32_t tick) {
    const auto end = std::find_if(queue.begin(), queue.end(), [tick](const Command& c) { return c.tick != tick; });
    // The queue is sorted by tick and nothing older than `tick` can be left (schedule rejects it).
    std::vector<Command> out(queue.begin(), end);
    queue.erase(queue.begin(), end);
    return out;
}

}  // namespace corps
