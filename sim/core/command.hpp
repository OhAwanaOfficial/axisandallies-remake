#pragma once

// Commands: the only way to change sim state (CLAUDE.md rule 2). Encoded as in NETCODE §3:
//   Command { u8 type; u8 player; varint payload... }   (tick and seq travel in the bundle)
//
// Gameplay commands (MoveGroup, Deploy, ...) are added together with their systems (M4+). Until
// then the set holds what the sim can act on: spawning from data definitions plus test commands
// that exercise the ECS, the RNG and the serializer for replays and determinism checks.

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "ecs/entity.hpp"
#include "math/angle.hpp"
#include "math/fvec2.hpp"

namespace corps {

enum class CommandType : std::uint8_t {
    Noop = 0,
    SpawnRegiment = 1,  // create a regiment (and its units) from a RegimentDef by IDS
    // 2..199: gameplay commands, assigned when their systems land.
    DebugSpawn = 200,
    DebugDestroy = 201,
    DebugSetTransform = 202,
    DebugRngDraw = 203,
};

struct NoPayload {
    friend bool operator==(const NoPayload&, const NoPayload&) noexcept { return true; }
};
struct SpawnRegimentPayload {
    std::string regimentIds;  // RegimentDef IDS, e.g. "regiment_per_infantry"
    FVec2 position;
    Angle facing;
    friend bool operator==(const SpawnRegimentPayload& a, const SpawnRegimentPayload& b) noexcept {
        return a.regimentIds == b.regimentIds && a.position == b.position && a.facing == b.facing;
    }
};
struct DebugSpawnPayload {
    FVec2 position;
    Angle facing;
    friend bool operator==(const DebugSpawnPayload& a, const DebugSpawnPayload& b) noexcept {
        return a.position == b.position && a.facing == b.facing;
    }
};
struct DebugDestroyPayload {
    EntityId target;
    friend bool operator==(const DebugDestroyPayload& a, const DebugDestroyPayload& b) noexcept { return a.target == b.target; }
};
struct DebugSetTransformPayload {
    EntityId target;
    FVec2 position;
    Angle facing;
    friend bool operator==(const DebugSetTransformPayload& a, const DebugSetTransformPayload& b) noexcept {
        return a.target == b.target && a.position == b.position && a.facing == b.facing;
    }
};
struct DebugRngDrawPayload {
    std::uint16_t count = 0;
    friend bool operator==(const DebugRngDrawPayload& a, const DebugRngDrawPayload& b) noexcept { return a.count == b.count; }
};

using CommandPayload = std::variant<NoPayload, SpawnRegimentPayload, DebugSpawnPayload, DebugDestroyPayload,
                                    DebugSetTransformPayload, DebugRngDrawPayload>;

struct Command {
    std::uint32_t tick = 0;  // the tick it is applied at
    std::uint8_t player = 0;
    std::uint16_t seq = 0;   // per-player sequence number; orders a player's commands in one tick
    CommandType type = CommandType::Noop;
    CommandPayload payload;

    friend bool operator==(const Command& a, const Command& b) noexcept {
        return a.tick == b.tick && a.player == b.player && a.seq == b.seq && a.type == b.type && a.payload == b.payload;
    }
};

// Builders (keep type and payload consistent).
Command makeNoop(std::uint32_t tick, std::uint8_t player, std::uint16_t seq);
Command makeSpawnRegiment(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::string ids, FVec2 pos, Angle facing);
Command makeDebugSpawn(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, FVec2 pos, Angle facing);
Command makeDebugDestroy(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId target);
Command makeDebugSetTransform(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId target, FVec2 pos, Angle facing);
Command makeDebugRngDraw(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::uint16_t count);

// Byte encoding of one command's {type, player, payload} (tick/seq are carried by the container).
// Signed numbers are zig-zag varints; Fixed is its raw int64.
void encodeCommand(const Command& c, std::vector<std::uint8_t>& out);
// Decodes one command from bytes[pos...]; advances pos. false on malformed or unknown input.
bool decodeCommand(const std::vector<std::uint8_t>& bytes, std::size_t& pos, Command& out);

// Varint helpers shared by the replay format.
void putVarint(std::vector<std::uint8_t>& out, std::uint64_t v);
bool getVarint(const std::vector<std::uint8_t>& in, std::size_t& pos, std::uint64_t& v);

// Session-side queue of commands waiting for their tick (replay player, lockstep scheduler).
// Ordered by (tick, player, seq, arrival). Not sim state: Sim::step takes one tick's commands.
class CommandQueue {
public:
    // Rejects (false) commands for ticks that have already started (tick < nextTick).
    bool schedule(const Command& c, std::uint32_t nextTick);
    // Removes and returns every command for `tick`, in (player, seq, arrival) order.
    std::vector<Command> take(std::uint32_t tick);
    const std::vector<Command>& pending() const noexcept { return queue; }
    void restore(std::vector<Command> commands) { queue = std::move(commands); }

private:
    std::vector<Command> queue;
};

}  // namespace corps
