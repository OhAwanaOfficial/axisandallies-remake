extends Control
## The multiplayer lobby (M8, NETCODE §7): host or join by IP, the player list (nation, team,
## colour, AI slots), the map and money (host), chat, and Start once everyone has confirmed the
## same MatchConfig. The battle then runs in lockstep through the `Net` autoload.
##
## `--lobby-host[=PORT]` / `--lobby-join=IP[:PORT]` (with `--name=N`) skip the buttons, and
## `--lobby-autostart=N` makes the host press Start once N humans are in (two-instance tests).

const BACKGROUND := Color(0.11, 0.12, 0.09)

var bridge := SimBridge.new()
var maps: Array = []
var name_edit: LineEdit
var addr_edit: LineEdit
var port_edit: SpinBox
var connect_box: HBoxContainer
var list: VBoxContainer
var map_pick: OptionButton
var money_pick: OptionButton
var chat_log: RichTextLabel
var chat_edit: LineEdit
var start_button: Button
var add_ai_button: Button
var status: Label
var _autostart := 0


func _ready() -> void:
	_find_maps()
	_build()
	Net.lobby_changed.connect(_refresh)
	Net.chat_received.connect(_on_chat)
	Net.match_starting.connect(_on_start)
	Net.failed.connect(func(why: String) -> void: status.text = why)
	var args := {}
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--"):
			var kv := a.trim_prefix("--").split("=", true, 1)
			args[kv[0]] = kv[1] if kv.size() > 1 else ""
	if args.has("name"):
		name_edit.text = str(args.name)
	_autostart = int(args.get("lobby-autostart", "0"))
	if args.has("lobby-host"):
		if str(args["lobby-host"]) != "":
			port_edit.value = int(args["lobby-host"])
		_host.call_deferred()
	elif args.has("lobby-join"):
		var a := str(args["lobby-join"]).split(":")
		addr_edit.text = a[0]
		if a.size() > 1:
			port_edit.value = int(a[1])
		_join.call_deferred()
	_refresh()


func _find_maps() -> void:
	var loader := CacheLoader.new("")
	var root := loader.cache_dir.path_join("maps")
	var dir := DirAccess.open(root)
	if dir != null:
		var names := dir.get_directories()
		names.sort()
		for n in names:
			var spec := root.path_join(n).path_join("map.json")
			if FileAccess.file_exists(spec):
				maps.append([n.replace("_", " ").capitalize(), spec, n])
	maps.append(["Generated test map (no cities)", "gen:7:256", "gen:7:256"])


func _build() -> void:
	var bg := ColorRect.new()
	bg.color = BACKGROUND
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(bg)
	var margin := MarginContainer.new()
	margin.set_anchors_preset(Control.PRESET_FULL_RECT)
	for side in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 24)
	add_child(margin)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 10)
	margin.add_child(box)
	var title := Label.new()
	title.text = "Multiplayer"
	title.add_theme_font_size_override("font_size", 28)
	title.add_theme_color_override("font_color", Color(1.0, 0.92, 0.6))
	box.add_child(title)

	connect_box = HBoxContainer.new()
	box.add_child(connect_box)
	name_edit = _edit(connect_box, "Name", "Player", 140)
	addr_edit = _edit(connect_box, "Host address", "127.0.0.1", 160)
	port_edit = SpinBox.new()
	port_edit.min_value = 1024
	port_edit.max_value = 65535
	port_edit.value = EnetTransport.DEFAULT_PORT
	connect_box.add_child(port_edit)
	_button(connect_box, "Host", _host)
	_button(connect_box, "Join", _join)
	_button(connect_box, "Back", func() -> void:
		Net.leave()
		get_tree().change_scene_to_file("res://scenes/skirmish_setup.tscn"))

	var settings := HBoxContainer.new()
	box.add_child(settings)
	var ml := Label.new()
	ml.text = "Map"
	settings.add_child(ml)
	map_pick = OptionButton.new()
	for m in maps:
		map_pick.add_item(m[0])
	map_pick.item_selected.connect(func(_i: int) -> void: _push_settings())
	settings.add_child(map_pick)
	var mo := Label.new()
	mo.text = "  Starting money"
	settings.add_child(mo)
	money_pick = OptionButton.new()
	for v in Skirmish.MONEY:
		money_pick.add_item("The map's" if v == 0 else str(v))
	money_pick.item_selected.connect(func(_i: int) -> void: _push_settings())
	settings.add_child(money_pick)
	add_ai_button = _button(settings, "Add AI", func() -> void: Net.host_add_ai())

	var players_title := Label.new()
	players_title.text = "Players"
	players_title.add_theme_color_override("font_color", Color(0.95, 0.9, 0.75))
	box.add_child(players_title)
	list = VBoxContainer.new()
	box.add_child(list)

	chat_log = RichTextLabel.new()
	chat_log.custom_minimum_size = Vector2(0, 160)
	chat_log.size_flags_vertical = Control.SIZE_EXPAND_FILL
	chat_log.scroll_following = true
	box.add_child(chat_log)
	var chat_row := HBoxContainer.new()
	box.add_child(chat_row)
	chat_edit = LineEdit.new()
	chat_edit.placeholder_text = "Chat"
	chat_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	chat_edit.text_submitted.connect(func(t: String) -> void:
		Net.send_chat(t)
		chat_edit.text = "")
	chat_row.add_child(chat_edit)
	start_button = _button(chat_row, "Start", func() -> void: Net.host_start())
	status = Label.new()
	box.add_child(status)


func _edit(parent: Control, placeholder: String, text: String, width: float) -> LineEdit:
	var e := LineEdit.new()
	e.placeholder_text = placeholder
	e.text = text
	e.custom_minimum_size = Vector2(width, 0)
	parent.add_child(e)
	return e


func _button(parent: Control, text: String, f: Callable) -> Button:
	var b := Button.new()
	b.text = text
	b.custom_minimum_size = Vector2(90, 32)
	b.pressed.connect(f)
	parent.add_child(b)
	return b


func _identify() -> bool:
	status.text = "Loading the game data..."
	await get_tree().process_frame
	if not Net.identify(bridge, _game_dir()):
		status.text = Net.status_text
		print("NET lobby: " + Net.status_text)
		return false
	print("NET lobby: data %s, sim %s" % [Net.data_hash, Net.sim_version])
	return true


func _host() -> void:
	if not await _identify():
		return
	Net.host_lobby(int(port_edit.value), name_edit.text)
	_push_settings()


func _join() -> void:
	if not await _identify():
		return
	Net.join_lobby(addr_edit.text, int(port_edit.value), name_edit.text)


func _push_settings() -> void:
	if Net.is_host:
		Net.host_settings(str(maps[map_pick.selected][2]), Skirmish.MONEY[money_pick.selected])


var _last_status := ""


func _refresh() -> void:
	status.text = Net.status_text
	if Net.status_text != _last_status:
		_last_status = Net.status_text
		print("NET lobby: %s (%d players)" % [_last_status, Net.players.size()])
	connect_box.visible = not Net.in_lobby
	map_pick.disabled = not Net.is_host
	money_pick.disabled = not Net.is_host
	add_ai_button.visible = Net.is_host
	start_button.visible = Net.is_host
	for i in maps.size():
		if str(maps[i][2]) == Net.map_spec:
			map_pick.select(i)
	for c in list.get_children():
		c.queue_free()
	var humans := 0
	for p in Net.players:
		humans += 1 if str(p.ai) == "" else 0
		var row := HBoxContainer.new()
		var who := Label.new()
		who.custom_minimum_size = Vector2(200, 0)
		who.text = "%d  %s%s" % [int(p.slot) + 1, str(p.name), "  (you)" if int(p.slot) == Net.slot else ""]
		row.add_child(who)
		var mine := int(p.slot) == Net.slot or (Net.is_host and str(p.ai) != "")
		var s := int(p.slot)
		var nation := OptionButton.new()
		for n in Skirmish.NATIONS:
			nation.add_item(Skirmish.NATION_NAMES[n])
		nation.select(Skirmish.NATIONS.find(str(p.nation)))
		nation.disabled = not mine
		nation.item_selected.connect(func(i: int) -> void: _set_slot(s, "nation", Skirmish.NATIONS[i]))
		row.add_child(nation)
		var team := OptionButton.new()
		for t in 4:
			team.add_item("Team %d" % (t + 1))
		team.select(int(p.team))
		team.disabled = not mine
		team.item_selected.connect(func(i: int) -> void: _set_slot(s, "team", i))
		row.add_child(team)
		var color := OptionButton.new()
		for c in Net.COLORS:
			color.add_item(c.capitalize())
		color.select(int(p.color))
		color.disabled = not mine
		color.item_selected.connect(func(i: int) -> void: _set_slot(s, "color", i))
		row.add_child(color)
		var kind := Label.new()
		kind.text = "  human" if str(p.ai) == "" else "  AI (%s)" % str(p.ai)
		row.add_child(kind)
		if Net.is_host and s != 0:
			_button(row, "Remove", func() -> void: Net.host_remove(s))
		list.add_child(row)
	if Net.is_host and _autostart > 0 and humans >= _autostart and Net.config.is_empty():
		Net.host_start()


func _set_slot(s: int, key: String, value: Variant) -> void:
	if s == Net.slot:
		Net.request_set(key, value)
	else:
		Net.host_set(s, key, value)


func _on_chat(player: int, text: String) -> void:
	chat_log.append_text("%s: %s\n" % ["*" if player < 0 else Net.player_name(player), text])


func _on_start(config: Dictionary) -> void:
	print("NET lobby: starting %s, seed %d, %d players" % [config.map, int(config.seed), config.players.size()])
	var me := {}
	var other := {}
	for p in config.players:
		if int(p.slot) == Net.slot:
			me = p
		elif other.is_empty():
			other = p
	# The config names the map by its cache folder (paths differ between machines).
	var spec := ""
	for m in maps:
		if str(m[2]) == str(config.map):
			spec = str(m[1])
	if spec == "":
		status.text = "This machine has no map %s in its cache (tools/rmp.py)" % config.map
		Net.leave()
		return
	var s := Skirmish.defaults(spec)
	s.nation = str(me.get("nation", "gbr"))
	s.enemy = str(other.get("nation", "ger"))
	s.money = int(config.money)
	s.seed = int(config.seed)
	s.ai = ""
	s.net = true
	Skirmish.settings = s
	status.text = "Loading the game data and the map..."
	await get_tree().process_frame
	get_tree().change_scene_to_file("res://scenes/battle.tscn")


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
