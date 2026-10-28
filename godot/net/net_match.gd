extends Node
## A lockstep match over ENet without a view (M8): the transport test, the soak test and the
## desync test. Every instance runs the same sim; each slot is played by a host-side controller
## ("slice", "dummy") on the machine that owns the slot, so the commands really cross the network.
##
##   host: godot --headless --path godot res://net/net_match.tscn -- --net-host --players=2 --ticks=600
##   join: godot --headless --path godot res://net/net_match.tscn -- --net-join=127.0.0.1
## Options: --port=6900, --map=SPEC (default gen:7:256), --seed=N, --delay=D (fixed; default
## adaptive), --ai=slice,dummy (controller per slot), --drop-after=MS (drop a silent player and
## let the dummy take over), --debug-desync=TICK (this instance perturbs its sim after TICK),
## --stall-at=TICK --stall-ms=MS (this instance stops sending for a while), --out=DIR (dumps).
## Prints "NET MATCH OK slot S tick T hash H" and exits 0; a desync exits 2 after writing
## desync_<slot>_<tick>.state and desync_<slot>.rpl to --out (default user://).

const PING_MS := 500
const ADAPT_MS := 2000
const WAIT_MS := 3000
const MAX_TICKS_PER_FRAME := 200
const FRAME_BUDGET_US := 14000

var bridge := SimBridge.new()
var net := NetSession.new()
var tr := EnetTransport.new()
var args := {}
var phase := "lobby"          # lobby -> running -> done
var slot := -1
var players := 2
var config := {}
var _next_slot := 1
var _ready_slots := {}               # host: slot -> true
var _rtt := {}                 # host: slot -> last RTT (ms)
var _jitter := {}              # host: slot -> smoothed |ΔRTT| (ms)
var _ping_id := 0
var _last_ping := 0
var _last_adapt := 0
var _waiting_shown := false
var _target_ticks := 600
var _fixed_delay := 0
var _t_start := 0
var _stall_until := 0


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--"):
			var kv := a.trim_prefix("--").split("=", true, 1)
			args[kv[0]] = kv[1] if kv.size() > 1 else "1"
	_target_ticks = int(args.get("ticks", "600"))
	_fixed_delay = int(args.get("delay", "0"))
	var port := int(args.get("port", str(EnetTransport.DEFAULT_PORT)))
	if args.has("net-host"):
		players = int(args.get("players", "2"))
		slot = 0
		if tr.host(port, players - 1) != OK:
			_finish(4, "cannot listen on port %d" % port)
			return
		print("NET host listening on %d for %d players" % [port, players])
		if players == 1:
			_host_start()
	elif args.has("net-join"):
		var addr := str(args["net-join"])
		if addr == "1":
			addr = "127.0.0.1"
		if tr.join(addr, port) != OK:
			_finish(4, "cannot connect to %s:%d" % [addr, port])
			return
		print("NET joining %s:%d" % [addr, port])
	else:
		_finish(4, "pass --net-host or --net-join=IP")


func _process(_delta: float) -> void:
	if phase == "done":
		return
	var t0 := Time.get_ticks_usec()
	var steps := 0
	while true:
		_pump()
		if phase != "running":
			break
		if not _try_step():
			break
		steps += 1
		if steps >= MAX_TICKS_PER_FRAME or Time.get_ticks_usec() - t0 > FRAME_BUDGET_US:
			break
		if bridge.tick() > _target_ticks:
			break
	tr.flush()
	if phase == "running":
		_host_housekeeping()
		_check_end()


# ---------------------------------------------------------------- network

func _pump() -> void:
	var now := Time.get_ticks_msec()
	for ev in tr.poll():
		match str(ev.type):
			"connect":
				if tr.is_host:
					_host_on_connect(ev.peer)
			"disconnect":
				if tr.is_host:
					print("NET slot %d disconnected" % int(ev.slot))
				elif phase != "done":
					_finish(5, "the host left")
					return
			"message":
				_on_message(ev, now)
	if phase != "running":
		return
	if now < _stall_until:
		return  # the stall test: this instance goes silent
	if tr.is_host:
		for b in net.host_poll(now):
			tr.broadcast(EnetTransport.CH_LOCKSTEP, b)
	else:
		for p in net.outgoing():
			tr.send_to_host(EnetTransport.CH_LOCKSTEP, p)


func _on_message(ev: Dictionary, now: int) -> void:
	var data: PackedByteArray = ev.data
	var lobby := EnetTransport.lobby_message(data)
	if not lobby.is_empty():
		if tr.is_host:
			_host_on_lobby(int(ev.slot), lobby)
		else:
			_client_on_lobby(lobby)
		return
	if phase != "running":
		return
	var r: Dictionary = net.receive(data, int(ev.slot), now)
	match str(r.kind):
		"ping":
			tr.send(ev.peer, EnetTransport.CH_PING, r.reply)
		"pong":
			var rtt := now - int(r.sent_ms)
			var s := int(ev.slot)
			if _rtt.has(s):
				_jitter[s] = int(0.75 * float(_jitter.get(s, 0)) + 0.25 * absf(rtt - int(_rtt[s])))
			_rtt[s] = rtt
		"chat":
			print("NET chat %d: %s" % [int(r.player), str(r.text)])
		"bad":
			push_warning("NET malformed message from slot %d" % int(ev.slot))


func _host_on_connect(peer: ENetPacketPeer) -> void:
	if _next_slot >= players or phase != "lobby":
		tr.send_lobby(peer, {"t": "refused", "why": "the match is full"})
		peer.peer_disconnect_later()
		return
	tr.bind_slot(peer, _next_slot)
	tr.send_lobby(peer, {"t": "welcome", "slot": _next_slot})
	print("NET slot %d connected" % _next_slot)
	_next_slot += 1
	if _next_slot == players:
		_host_start()


func _host_start() -> void:
	config = {"t": "start", "map": str(args.get("map", "gen:7:256")), "seed": int(args.get("seed", str(randi() % 100000 + 1))),
		"players": players, "delay": _fixed_delay if _fixed_delay > 0 else 3, "ai": str(args.get("ai", "slice,dummy")),
		"ticks": _target_ticks}
	tr.broadcast_lobby(config)
	if not _load():
		return
	_ready_slots[0] = true
	_maybe_go()


func _host_on_lobby(from: int, msg: Dictionary) -> void:
	if str(msg.get("t", "")) != "ready":
		return
	if str(msg.get("sim", "")) != bridge.sim_version() or str(msg.get("data", "")) != str(bridge.data_hash()):
		tr.send_lobby(tr.slot_peers[from], {"t": "refused", "why": "different sim version or game data (mods)"})
		print("NET refused slot %d: sim %s data %s (ours %s %s)" % [from, msg.get("sim", ""), msg.get("data", ""),
			bridge.sim_version(), bridge.data_hash()])
		return
	_ready_slots[from] = true
	_maybe_go()


func _maybe_go() -> void:
	if _ready_slots.size() < players:
		return
	tr.broadcast_lobby({"t": "go"})
	_begin()


func _client_on_lobby(msg: Dictionary) -> void:
	match str(msg.get("t", "")):
		"welcome":
			slot = int(msg.slot)
			print("NET joined as slot %d" % slot)
		"start":
			config = msg
			players = int(msg.players)
			_target_ticks = int(msg.get("ticks", _target_ticks))
			if _load():
				tr.send_lobby(null, {"t": "ready", "sim": bridge.sim_version(), "data": str(bridge.data_hash())})
		"go":
			_begin()
		"refused":
			_finish(3, "refused by the host: %s" % str(msg.get("why", "")))


# ---------------------------------------------------------------- the match

func _load() -> bool:
	var t0 := Time.get_ticks_msec()
	if not bridge.start(_game_dir(), str(config.map), int(config.seed)):
		_finish(4, "SimBridge.start failed: %s" % bridge.last_error())
		return false
	print("NET loaded %s seed %d in %d ms (sim %s, data %s)" % [config.map, int(config.seed), Time.get_ticks_msec() - t0,
		bridge.sim_version(), bridge.data_hash()])
	return true


## Tick 0 is the same on every machine (it follows from the MatchConfig), so it isn't sent: the
## rules and every player's HQ, then the lockstep starts at tick 1.
func _begin() -> void:
	var nations := ["gbr", "ger", "usa", "rus", "jpn"]
	var map: Dictionary = bridge.map_info()
	bridge.queue_match_rules(PackedInt32Array(), float(map.get("city_victory_ratio", 0.0)), true, 0.0, 0)
	var starts: Array = bridge.map_starts()
	var w := float(map.width)
	var h := float(map.height)
	for p in players:
		var corps: String = bridge.nation_thing(nations[p % nations.size()], "placeholder_corpsunpacked")
		if corps == "":
			corps = nations[p % nations.size()] + "_corps"
		var at := Vector2(w * (0.25 + 0.5 * (p % 2)), h * (0.3 + 0.4 * float(p / 2) / maxf(1.0, float((players - 1) / 2))))
		var facing := 0.0
		if starts.size() > p:
			at = starts[p].position
			facing = float(starts[p].facing)
		else:
			at = bridge.find_open_area(at, 8)
		bridge.queue_start_player(corps, at, facing, p)
	bridge.step()
	var kinds := str(config.get("ai", "slice,dummy")).split(",")
	var mine := 1 << slot
	if tr.is_host:
		net.start_host((1 << players) - 1, mine, slot, int(config.delay))
	else:
		net.start_client(slot, int(config.delay))
	var kind := kinds[slot] if slot < kinds.size() else "dummy"
	bridge.set_controller(slot, kind, nations[slot % nations.size()])
	phase = "running"
	_t_start = Time.get_ticks_msec()
	print("NET match started: slot %d plays %s, %d players, D=%d" % [slot, kind, players, int(config.delay)])


func _try_step() -> bool:
	if not net.can_step():
		return false
	net.issue(bridge.take_commands())
	bridge.step_commands(net.take_tick())
	var t := bridge.tick() - 1
	if args.has("debug-desync") and t == int(args["debug-desync"]):
		bridge.debug_desync()
		print("NET debug: perturbed the sim after tick %d" % t)
	if args.has("stall-at") and t == int(args["stall-at"]):
		_stall_until = Time.get_ticks_msec() + int(args.get("stall-ms", "5000"))
		print("NET debug: going silent for %s ms at tick %d" % [args.get("stall-ms", "5000"), t])
	var report: PackedByteArray = net.report_hash(t, bridge.state_hash_bits())
	if not report.is_empty():
		tr.send_to_host(EnetTransport.CH_LOCKSTEP, report)
	var st: Dictionary = net.status(Time.get_ticks_msec())
	if int(st.get("desync_tick", -1)) >= 0:
		_on_desync(int(st.desync_tick))
		return false
	return true


func _host_housekeeping() -> void:
	if not tr.is_host:
		return
	var now := Time.get_ticks_msec()
	if now - _last_ping >= PING_MS:
		_last_ping = now
		_ping_id += 1
		tr.broadcast(EnetTransport.CH_PING, NetSession.encode_ping(_ping_id, now))
	if _fixed_delay == 0 and now - _last_adapt >= ADAPT_MS and not _rtt.is_empty():
		_last_adapt = now
		var max_rtt := 0
		var max_jit := 0
		for s in _rtt:
			max_rtt = maxi(max_rtt, int(_rtt[s]))
			max_jit = maxi(max_jit, int(_jitter.get(s, 0)))
		var d := NetSession.adaptive_delay(max_rtt, max_jit)
		var st: Dictionary = net.status(now)
		if d != int(st.input_delay):
			net.set_input_delay(d)
			print("NET input delay %d -> %d (max RTT %d ms, jitter %d ms)" % [int(st.input_delay), d, max_rtt, max_jit])
	var st2: Dictionary = net.status(now)
	var stalled := int(st2.get("stalled_ms", 0))
	if stalled >= WAIT_MS and not _waiting_shown:
		_waiting_shown = true
		print("NET Waiting for player %s..." % str(st2.waiting_on))
	elif stalled < WAIT_MS:
		_waiting_shown = false
	if args.has("drop-after") and stalled >= int(args["drop-after"]):
		for s in st2.waiting_on:
			if int(s) != slot:
				print("NET dropping slot %d after %d ms; the dummy AI takes over" % [int(s), stalled])
				net.drop_player(int(s), true)
				bridge.set_controller(int(s), "dummy", ["gbr", "ger", "usa", "rus", "jpn"][int(s) % 5])


func _check_end() -> void:
	if bridge.tick() <= _target_ticks:
		return
	var secs := (Time.get_ticks_msec() - _t_start) / 1000.0
	var st: Dictionary = net.status(Time.get_ticks_msec())
	print("NET MATCH OK slot %d tick %d hash %s (%.1f s, %.0f ticks/s, D=%d, left %d)" % [slot, bridge.tick(), bridge.state_hash(),
		secs, (bridge.tick() - 1) / maxf(secs, 0.001), int(st.input_delay), int(st.left_mask)])
	if args.has("replay"):
		var r: Dictionary = bridge.save_film(ProjectSettings.globalize_path(str(args.replay)), PackedStringArray(["slot 0", "slot 1"]))
		print("NET replay %s: %s" % [args.replay, "ok" if r.ok else str(r.get("error", ""))])
	# Keep relaying a little longer so the other side gets its last bundles.
	phase = "done"
	await get_tree().create_timer(1.0).timeout
	tr.close()
	get_tree().quit(0)


func _on_desync(tick: int) -> void:
	var out := ProjectSettings.globalize_path(str(args.get("out", "user://")))
	var state_path := out.path_join("desync_%d_%d.state" % [slot, tick])
	var f := FileAccess.open(state_path, FileAccess.WRITE)
	if f != null:
		f.store_buffer(bridge.save_state())
		f.close()
	var rep: Dictionary = bridge.save_replay(out.path_join("desync_%d.rpl" % slot))
	print("NET DESYNC at tick %d: wrote %s (tick %d) and the replay (%s)" % [tick, ProjectSettings.globalize_path(state_path),
		bridge.tick() - 1, "ok" if rep.ok else str(rep.get("error", ""))])
	phase = "done"
	await get_tree().create_timer(1.0).timeout
	tr.close()
	get_tree().quit(2)


func _finish(code: int, msg: String) -> void:
	print("NET FAILED: " + msg)
	phase = "done"
	tr.close()
	get_tree().quit(code)


func _game_dir() -> String:
	var env := OS.get_environment("CORPS_GAME")
	if env != "":
		return env
	var cfg := ProjectSettings.globalize_path("res://").path_join("../config/game_path.cfg").simplify_path()
	if FileAccess.file_exists(cfg):
		for line in FileAccess.get_file_as_string(cfg).split("\n"):
			var t := line.strip_edges()
			if t != "" and not t.begins_with("#") and not t.begins_with(";"):
				return t.trim_prefix("\"").trim_suffix("\"")
	return ""
