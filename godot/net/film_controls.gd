class_name FilmControls
extends CanvasLayer
## Film playback controls in the battle view (M8 B7). The battle steps the film with
## SimBridge.film_step(); its own keys still work: Space pauses, +/- change the speed (up to 8x),
## and the camera is free. Here: PageUp / PageDown seek one minute back / forward, Home / End jump
## to the start / end, V switches whose view (selection, HUD) you get.
## `--film-test=N`: seek around, check the hashes against straight play, print and quit.

var battle: Node
var bridge: SimBridge
var players: Array = []
var label: Label


func setup(b: Node, br: SimBridge, names: Array) -> void:
	battle = b
	bridge = br
	players = names
	layer = 8
	label = Label.new()
	label.position = Vector2(20, 90)
	label.add_theme_font_size_override("font_size", 18)
	label.add_theme_color_override("font_color", Color(0.9, 0.95, 1.0))
	add_child(label)
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--film-test"):
			_self_test.call_deferred()


func _process(_delta: float) -> void:
	if bridge == null:
		return
	var t := bridge.tick()
	var n := bridge.film_length()
	var who: String = str(players[battle.PLAYER]) if battle.PLAYER < players.size() else "player %d" % battle.PLAYER
	label.text = "FILM  %d:%02d / %d:%02d   x%s%s   viewing %s   [PgUp/PgDn seek 1 min, Home/End, V view, Space pause]" % [
		t / 600, (t / 10) % 60, n / 600, (n / 10) % 60, str(battle.speed), "  PAUSED" if battle.paused else "", who]


func _unhandled_key_input(event: InputEvent) -> void:
	var k := event as InputEventKey
	if k == null or not k.pressed or k.echo:
		return
	match k.keycode:
		KEY_PAGEDOWN:
			seek(bridge.tick() + 600)
		KEY_PAGEUP:
			seek(bridge.tick() - 600)
		KEY_HOME:
			seek(0)
		KEY_END:
			seek(bridge.film_length())
		KEY_V:
			battle.PLAYER = (battle.PLAYER + 1) % maxi(players.size(), 2)
			battle.selected = {}
		_:
			return
	get_viewport().set_input_as_handled()


func seek(tick: int) -> void:
	bridge.film_seek(clampi(tick, 0, bridge.film_length()))
	battle.snap = bridge.snapshot()
	battle.accum = 0.0


func _self_test() -> void:
	var n := bridge.film_length()
	var t0 := Time.get_ticks_msec()
	seek(n)
	var end_hash := bridge.state_hash()
	var seek_end_ms := Time.get_ticks_msec() - t0
	seek(n / 2)
	var mid := bridge.state_hash()
	seek(0)
	for i in n / 2:
		bridge.film_step()
	var ok := bridge.state_hash() == mid
	seek(n)
	ok = ok and bridge.state_hash() == end_hash
	print("FILM TEST %s: %d ticks, seek to the end %d ms, mid hash %s, end hash %s" % ["PASSED" if ok else "FAILED", n, seek_end_ms, mid, end_hash])
	get_tree().quit(0 if ok else 1)
