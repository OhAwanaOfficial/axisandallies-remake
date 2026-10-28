#pragma once

// Lockstep for the Godot side (M8, NETCODE.md): wraps net::LockstepHost / LockstepPeer and the
// packet codec. The transport (ENetConnection, godot/net/) moves the bytes; SimBridge steps the
// bundles. On the host, every local slot (the host's player and each AI) is a LockstepPeer whose
// packets go straight into the LockstepHost.

#include <cstdint>
#include <map>
#include <memory>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include "net/lockstep.hpp"

namespace godot {

class NetSession : public RefCounted {
    GDCLASS(NetSession, RefCounted)

protected:
    static void _bind_methods();

public:
    // Host: `player_mask` = every slot in the match; `local_mask` = the slots this machine sends
    // for (its player and the AI slots). `view_player`: the slot whose bundles step the sim.
    void start_host(int64_t player_mask, int64_t local_mask, int64_t view_player, int64_t input_delay);
    // Client: one slot.
    void start_client(int64_t player, int64_t input_delay);
    bool is_host() const { return host != nullptr; }

    // A SimBridge.take_commands() list: split by player into the local slots' outboxes.
    void issue(const PackedByteArray& commands);
    // Client: the encoded CmdPackets to send to the host now (channel 0).
    Array outgoing();
    // Host: sends the local slots' packets, collects complete bundles and returns them encoded for
    // broadcast (channel 0). They're already delivered to the local slots.
    Array host_poll(int64_t now_ms);
    // An incoming message from slot `from` (the host ignores a CmdPacket whose player isn't `from`).
    // Returns {"kind": "packet" | "bundle" | "hash" | "ping" | "pong" | "chat" | "bad", plus
    // "reply" (bytes to send back), "player", "text", "id", "sent_ms"}.
    Dictionary receive(const PackedByteArray& message, int64_t from, int64_t now_ms);

    bool can_step() const;
    // The view slot's bundle for its next tick, as a command list for SimBridge.step_commands().
    // Applies the bundle's controls (D, speed, pause, player left, desync).
    PackedByteArray take_tick();
    // After stepping tick `tick`: every 10th tick returns an encoded HashReport to send (the host
    // checks it itself and returns empty).
    PackedByteArray report_hash(int64_t tick, int64_t hash);

    // Host controls (queued for the next bundle).
    void set_input_delay(int64_t d);
    void set_speed(int64_t percent);
    void set_paused(bool paused);
    // Drops a remote player (PlayerLeft). `stand_in`: a local slot takes over its kingdom from the
    // next bundle (give the bridge a controller for it).
    void drop_player(int64_t player, bool stand_in);
    static int64_t adaptive_delay(int64_t max_rtt_ms, int64_t jitter_ms);

    // Encoders for the transport's own messages.
    static PackedByteArray encode_ping(int64_t id, int64_t now_ms);
    static PackedByteArray encode_chat(int64_t player, const String& text);

    // {next_tick, input_delay, queued, speed, paused, left_mask, desync_tick (-1), waiting_on
    // (Array, host), stalled_ms (host), next_bundle (host)}
    Dictionary status(int64_t now_ms) const;

private:
    corps::net::LockstepPeer* viewPeer();
    const corps::net::LockstepPeer* viewPeer() const;

    std::unique_ptr<corps::net::LockstepHost> host;
    std::map<uint8_t, corps::net::LockstepPeer> peers;  // local slots (a client: just its own)
    uint8_t view = 0;
    uint8_t droppedRemote = 0;  // remote slots dropped: their packets are ignored
};

}  // namespace godot
