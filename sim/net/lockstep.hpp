#pragma once

// Lockstep scheduling (NETCODE §2–3, §6), transport-free. The host is a relay and a clock: it
// collects every peer's CmdPacket for tick T and broadcasts TickBundle(T). Each peer runs tick T
// only once TickBundle(T) is there, so every machine steps the same commands on the same tick.
//
// Not sim state: nothing here is hashed or saved. Time comes in as integer milliseconds from the
// caller (no wall clock in sim/), and every container is ordered.
//
// Session control entries (SetInputDelay, PlayerLeft, SetSpeed, Pause, DesyncDetected) ride in the
// bundles next to the sim commands, with codes 400–499. They change pacing and who controls a
// kingdom, never sim state, so they aren't sim Commands (CommandType is a u8; DECISIONS.md).

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "core/command.hpp"

namespace corps::net {

constexpr std::uint8_t kMaxNetPlayers = 8;
constexpr std::uint32_t kMinInputDelay = 2;
constexpr std::uint32_t kMaxInputDelay = 8;
constexpr std::uint32_t kDefaultInputDelay = 3;
constexpr std::uint32_t kTickMs = 100;                // 10 Hz
constexpr std::size_t kMaxCommandsPerPlayerTick = 32;  // NETCODE §9 rate limit

enum class ControlType : std::uint16_t {
    SetInputDelay = 400,   // value: the new D, from this bundle's tick on
    PlayerLeft = 401,      // player: who left; from this tick their kingdom is the host AI's
    SetSpeed = 402,        // value: game speed in percent (100 = 10 ticks/s)
    Pause = 403,           // value: 1 paused, 0 running
    DesyncDetected = 404,  // value: the tick whose hashes disagreed
};

struct Control {
    ControlType type = ControlType::SetInputDelay;
    std::uint8_t player = 0;
    std::uint32_t value = 0;
    friend bool operator==(const Control& a, const Control& b) noexcept {
        return a.type == b.type && a.player == b.player && a.value == b.value;
    }
};

// One peer's commands for one tick. Empty packets are heartbeats ("I may advance to `tick`").
struct CmdPacket {
    std::uint32_t tick = 0;
    std::uint8_t player = 0;
    std::uint16_t seq = 0;  // the packet's sequence number for this player (0, 1, 2...)
    std::vector<Command> commands;
    friend bool operator==(const CmdPacket& a, const CmdPacket& b) noexcept {
        return a.tick == b.tick && a.player == b.player && a.seq == b.seq && a.commands == b.commands;
    }
};

// Everything that happens at `tick`: all players' commands, sorted by (player, seq), and controls.
struct TickBundle {
    std::uint32_t tick = 0;
    std::vector<Command> commands;
    std::vector<Control> controls;
    friend bool operator==(const TickBundle& a, const TickBundle& b) noexcept {
        return a.tick == b.tick && a.commands == b.commands && a.controls == b.controls;
    }
};

// A peer's state hash after `tick` completed (every 10 ticks).
struct HashReport {
    std::uint32_t tick = 0;
    std::uint8_t player = 0;
    std::uint64_t hash = 0;
    friend bool operator==(const HashReport& a, const HashReport& b) noexcept {
        return a.tick == b.tick && a.player == b.player && a.hash == b.hash;
    }
};

// The host's ordering authority. Each slot is a human peer (remote or local) or an AI the host
// drives itself; both hand their packets to onPacket.
class LockstepHost {
public:
    // `playerMask`: bit p set = slot p takes part and must send a packet for every tick.
    explicit LockstepHost(std::uint8_t playerMask, std::uint32_t inputDelay = kDefaultInputDelay);

    enum class PacketResult { Accepted, Late, Duplicate, UnknownPlayer, Trimmed };
    // Takes a peer's packet. Late (its bundle already went out) and duplicate packets are dropped;
    // commands past kMaxCommandsPerPlayerTick are cut (Trimmed). Commands are re-stamped with the
    // packet's tick and player, so a peer can't act for someone else.
    PacketResult onPacket(CmdPacket packet, std::uint32_t nowMs);

    // Bundles that became complete, oldest first. A bundle goes out when every active player's
    // packet for its tick is in. `nowMs` dates the start of a stall.
    std::vector<TickBundle> poll(std::uint32_t nowMs);

    // Removes `player` from the wait set; the next bundle carries PlayerLeft(player).
    void dropPlayer(std::uint8_t player);
    // Puts `player` back in the wait set from nextBundleTick on, for a local stand-in (the AI that
    // takes over a dropped kingdom). Packets already queued for it are discarded.
    void addPlayer(std::uint8_t player);
    // Queues a control for the next bundle (SetInputDelay is clamped to 2..8 and also changes the
    // host's own D).
    void queueControl(Control control);
    // Adaptive D from the peers' round-trip times and jitter (ms): ceil((maxRtt/2 + 2×jitter +
    // processing) / tickLen) + 1, clamped to 2..8 (NETCODE §2).
    static std::uint32_t adaptiveDelay(std::uint32_t maxRttMs, std::uint32_t jitterMs, std::uint32_t processingMs = 10);

    // Hash reports: a mismatch for a tick queues DesyncDetected(tick) once.
    void onHashReport(const HashReport& report);
    std::optional<std::uint32_t> desyncTick() const noexcept { return desync_; }

    std::uint32_t nextBundleTick() const noexcept { return next_; }
    std::uint32_t inputDelay() const noexcept { return delay_; }
    std::uint8_t activeMask() const noexcept { return active_; }
    // Players whose packet for nextBundleTick is missing (empty while nothing is pending).
    std::vector<std::uint8_t> waitingOn() const;
    // How long the host has waited for nextBundleTick (0 while not stalled).
    std::uint32_t stalledForMs(std::uint32_t nowMs) const noexcept;

private:
    std::uint8_t active_;
    std::uint32_t delay_;
    std::uint32_t next_ = 0;
    std::map<std::uint32_t, std::map<std::uint8_t, CmdPacket>> pending_;  // tick -> player -> packet
    std::vector<Control> controls_;  // for the next bundle
    std::optional<std::uint32_t> waitStartMs_;
    std::map<std::uint32_t, std::map<std::uint8_t, std::uint64_t>> hashes_;  // tick -> player -> hash
    std::optional<std::uint32_t> desync_;
};

// One machine's side: sends its packets ahead by D ticks and steps the sim from bundles.
class LockstepPeer {
public:
    explicit LockstepPeer(std::uint8_t player, std::uint32_t inputDelay = kDefaultInputDelay);

    // A stand-in that joins at `tick` (an AI taking over a dropped player): it runs no earlier
    // tick and sends its first packet for `tick`.
    void startAt(std::uint32_t tick);
    // A command from this player's UI (or AI); its tick, player and seq are assigned when sent.
    void issue(Command command);
    // The packets to send now: one for every tick in (lastSent, nextTick + D]. Local commands go
    // into the last one (at most 32; the rest wait), and while D shrinks and nothing new may be
    // sent, they wait too.
    std::vector<CmdPacket> takeOutgoing();

    // Stores a bundle (any order; duplicates and ticks already run are ignored). False if ignored.
    bool onBundle(TickBundle bundle);
    bool canStep() const;
    // Removes the bundle for nextTick, applies its controls and advances. Call only if canStep().
    TickBundle takeTick();

    std::uint8_t player() const noexcept { return player_; }
    std::uint32_t nextTick() const noexcept { return next_; }
    std::uint32_t inputDelay() const noexcept { return delay_; }
    std::uint32_t speedPercent() const noexcept { return speed_; }
    bool paused() const noexcept { return paused_; }
    std::uint8_t leftMask() const noexcept { return left_; }
    std::optional<std::uint32_t> desyncTick() const noexcept { return desync_; }
    // Bundles received but not yet run (>2: the peer should run catch-up ticks, NETCODE §6).
    std::size_t queuedBundles() const noexcept { return bundles_.size(); }
    // Last tick a packet was sent for (nullopt before the first).
    std::optional<std::uint32_t> lastSent() const noexcept { return lastSent_; }

private:
    std::uint8_t player_;
    std::uint32_t delay_;
    std::uint32_t next_ = 0;
    std::optional<std::uint32_t> lastSent_;
    std::uint16_t packetSeq_ = 0;
    std::uint16_t commandSeq_ = 0;
    std::vector<Command> outbox_;
    std::map<std::uint32_t, TickBundle> bundles_;
    std::uint32_t speed_ = 100;
    bool paused_ = false;
    std::uint8_t left_ = 0;
    std::optional<std::uint32_t> desync_;
};

}  // namespace corps::net
