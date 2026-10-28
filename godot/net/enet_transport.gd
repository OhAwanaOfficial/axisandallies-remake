class_name EnetTransport
extends RefCounted
## The lockstep transport (M8, NETCODE §4): Godot's low-level ENetConnection, host-relayed star.
##   ch0 reliable ordered: CmdPacket, TickBundle, hash reports, lobby (JSON after byte 16)
##   ch1 unreliable: ping / pong
##   ch2 reliable: chat
## The host keeps one ENetPacketPeer per remote slot; a client has one peer, the host.

const CH_LOCKSTEP := 0
const CH_PING := 1
const CH_CHAT := 2
const CHANNELS := 3
const DEFAULT_PORT := 6900
const LOBBY := 16  # first byte of a lobby message (UTF-8 JSON follows)

var conn := ENetConnection.new()
var is_host := false
var slot_peers := {}        # host: slot -> ENetPacketPeer; client: 0 -> the host
var _peer_slot := {}        # ENetPacketPeer instance id -> slot
var _server: ENetPacketPeer  # client: the connection to the host
var connected := false
var events: Array = []       # [{type: "connect" | "disconnect" | "message", slot, channel, data}]


func host(port: int, max_peers: int) -> Error:
	is_host = true
	var err := conn.create_host_bound("*", port, max_peers, CHANNELS)
	connected = err == OK
	return err


func join(address: String, port: int) -> Error:
	is_host = false
	var err := conn.create_host(1, CHANNELS)
	if err != OK:
		return err
	_server = conn.connect_to_host(address, port, CHANNELS)
	return OK if _server != null else ERR_CANT_CONNECT


## Host: `peer` (from a "connect" event's `peer`) now speaks for `slot`.
func bind_slot(peer: ENetPacketPeer, slot: int) -> void:
	slot_peers[slot] = peer
	_peer_slot[peer.get_instance_id()] = slot


func slot_of(peer: ENetPacketPeer) -> int:
	return int(_peer_slot.get(peer.get_instance_id(), -1))


## Pumps ENet and appends what arrived to `events`. Returns the events and clears them.
func poll() -> Array:
	if conn == null:
		return []
	while true:
		var ev: Array = conn.service(0)
		var kind: int = ev[0]
		if kind == ENetConnection.EVENT_NONE or kind == ENetConnection.EVENT_ERROR:
			break
		var peer: ENetPacketPeer = ev[1]
		match kind:
			ENetConnection.EVENT_CONNECT:
				if not is_host:
					connected = true
					slot_peers[0] = peer
				events.append({"type": "connect", "peer": peer, "slot": slot_of(peer)})
			ENetConnection.EVENT_DISCONNECT:
				if not is_host:
					connected = false
				events.append({"type": "disconnect", "peer": peer, "slot": slot_of(peer)})
			ENetConnection.EVENT_RECEIVE:
				var data := peer.get_packet()
				events.append({"type": "message", "peer": peer, "slot": slot_of(peer), "channel": int(ev[3]), "data": data})
	var out := events
	events = []
	return out


## Sends to one peer (host: a slot's; client: the host when `peer` is null).
func send(peer: ENetPacketPeer, channel: int, data: PackedByteArray) -> void:
	var p := peer if peer != null else _server
	if p == null:
		return
	var flags := ENetPacketPeer.FLAG_UNSEQUENCED if channel == CH_PING else ENetPacketPeer.FLAG_RELIABLE
	p.send(channel, data, flags)


func send_to_host(channel: int, data: PackedByteArray) -> void:
	send(null, channel, data)


func broadcast(channel: int, data: PackedByteArray) -> void:
	for slot in slot_peers:
		send(slot_peers[slot], channel, data)


func send_lobby(peer: ENetPacketPeer, msg: Dictionary) -> void:
	send(peer, CH_LOCKSTEP, lobby_bytes(msg))


func broadcast_lobby(msg: Dictionary) -> void:
	broadcast(CH_LOCKSTEP, lobby_bytes(msg))


static func lobby_bytes(msg: Dictionary) -> PackedByteArray:
	var b := PackedByteArray([LOBBY])
	b.append_array(JSON.stringify(msg).to_utf8_buffer())
	return b


## The lobby message in `data`, or {} if it isn't one.
static func lobby_message(data: PackedByteArray) -> Dictionary:
	if data.is_empty() or data[0] != LOBBY:
		return {}
	var parsed: Variant = JSON.parse_string(data.slice(1).get_string_from_utf8())
	return parsed if parsed is Dictionary else {}


func flush() -> void:
	if conn != null:
		conn.flush()


func close() -> void:
	if conn == null:
		return
	for slot in slot_peers:
		(slot_peers[slot] as ENetPacketPeer).peer_disconnect_later()
	conn.flush()
	conn.destroy()
	conn = null
