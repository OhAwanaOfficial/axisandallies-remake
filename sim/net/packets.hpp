#pragma once

// Wire format of the lockstep protocol (NETCODE §4). Little-endian, varints; one message per ENet
// packet, the first byte says which:
//
//   1 CmdPacket   u32 tick, u16 seq, u8 player, u8 count (≤ 32), Command[count]
//   2 TickBundle  u32 tick, varint nCommands, (varint seq, Command)*, varint nControls,
//                 (u16 type, u8 player, varint value)*
//   3 HashReport  u32 tick, u8 player, u64 hash
//   4 Ping        u32 id, u32 sentMs           5 Pong  u32 id, u32 sentMs (echoed)
//   6 Chat        u8 player, varint len (≤ 256), bytes
//
//   Command = encodeCommand's { u8 type; u8 player; varint payload... }; tick comes from the
//   container. Decoding is strict: unknown kinds, short input, trailing bytes, over-long fields
//   and more than kMaxCommandsPerPlayerTick commands in a CmdPacket are all rejected.
//
// Lobby messages (B5) take kinds 16 and up.

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "net/lockstep.hpp"

namespace corps::net {

constexpr std::size_t kMaxChatBytes = 256;
constexpr std::size_t kMaxBundleCommands = kMaxCommandsPerPlayerTick * kMaxNetPlayers;
constexpr std::size_t kMaxBundleControls = 64;

enum class MsgKind : std::uint8_t {
    CmdPacket = 1,
    TickBundle = 2,
    HashReport = 3,
    Ping = 4,
    Pong = 5,
    Chat = 6,
};

struct Ping {
    std::uint32_t id = 0;
    std::uint32_t sentMs = 0;
    friend bool operator==(const Ping& a, const Ping& b) noexcept { return a.id == b.id && a.sentMs == b.sentMs; }
};
struct Pong {
    std::uint32_t id = 0;
    std::uint32_t sentMs = 0;
    friend bool operator==(const Pong& a, const Pong& b) noexcept { return a.id == b.id && a.sentMs == b.sentMs; }
};
struct Chat {
    std::uint8_t player = 0;
    std::string text;
    friend bool operator==(const Chat& a, const Chat& b) noexcept { return a.player == b.player && a.text == b.text; }
};

using Message = std::variant<CmdPacket, TickBundle, HashReport, Ping, Pong, Chat>;

std::vector<std::uint8_t> encodeMessage(const Message& m);
// False on anything malformed (see above); `out` is then unspecified.
bool decodeMessage(const std::vector<std::uint8_t>& bytes, Message& out);

// A bare command list, varint n, (varint seq, Command)*: what the Godot side hands between
// SimBridge and NetSession. Decoded commands get `tick`. False on malformed input or n > max.
std::vector<std::uint8_t> encodeCommandList(const std::vector<Command>& commands);
bool decodeCommandList(const std::vector<std::uint8_t>& bytes, std::uint32_t tick, std::vector<Command>& out,
                       std::size_t max = kMaxBundleCommands);

}  // namespace corps::net
