extends Control
## The skirmish setup screen (M6 close-out): map, your nation, the opponent's nation (a dummy AI),
## starting money and the match seed. Start hands the choices to the battle through the `Skirmish`
## autoload. In the original's art where it has some (the staging screen's flags, the game's
## MenuTile panel and A&A font), read from the install through SimBridge.mount.
##
## `--slice-test[=PATH]`: the headless play-test of the vertical slice. The screen picks Golden
## Fields, GBR against GER, the map's money and seed 2, and presses Start with the scripted slice
## player on your side; the battle plays the match out, shows the end screen, writes the replay to
## PATH (default user://slice.rpl) and quits (0 when the slice path was played through).
## `--defeat-test[=PATH]`: the same with the sides swapped (the dummy AI plays you, the slice
## player the enemy): the match must end on the defeat screen.

const BACKGROUND := Color(0.11, 0.12, 0.09)

var bridge := SimBridge.new()
var art: UiArt
var maps: Array = []          # [[label, spec]]
var map_pick: OptionButton
var nation_pick: OptionButton
var enemy_pick: OptionButton
var money_pick: OptionButton
var seed_pick: SpinBox
var flags := {}               # "you" / "enemy" -> TextureRect
var start_button: Button
var status: Label


func _ready() -> void:
	if Skirmish.back_test_code >= 0:  # the defeat test's New skirmish brought us back
		print("SLICE TEST: New skirmish returned to the setup screen")
		get_tree().quit(Skirmish.back_test_code)
		return
	var game := _game_dir()
	if game != "":
		bridge.mount(game)
	art = UiArt.new(bridge, "gbr")
	_find_maps()
	_build()
	var test := _slice_test_path()
	var defeat := false
	for a in OS.get_cmdline_user_args():
		if a == "--defeat-test" or a.begins_with("--defeat-test="):
			defeat = true
			test = a.trim_prefix("--defeat-test=") if a.contains("=") else "user://defeat.rpl"
	if test != "":
		_run_slice_test.call_deferred(test, defeat)
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--screenshot=") and test == "":
			for i in 30:
				await get_tree().process_frame
			await RenderingServer.frame_post_draw
			get_viewport().get_texture().get_image().save_png(a.trim_prefix("--screenshot="))
			get_tree().quit()


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
				maps.append([n.replace("_", " ").capitalize(), spec])
	maps.append(["Generated test map (no cities)", "gen:7:256"])


func _build() -> void:
	var bg := ColorRect.new()
	bg.color = BACKGROUND
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(bg)
	var font: Font = art.font()
	theme = Theme.new()
	theme.default_font = font
	theme.default_font_size = 16
	var center := CenterContainer.new()
	center.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(center)
	var panel := PanelContainer.new()
	var style := art.panel_style("/UI/%s/Game/Panel/MenuTile")
	if style != null:
		panel.add_theme_stylebox_override("panel", style)
	panel.custom_minimum_size = Vector2(620, 0)
	center.add_child(panel)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 12)
	panel.add_child(box)
	var title := Label.new()
	title.text = "Skirmish"
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_size_override("font_size", 30)
	title.add_theme_color_override("font_color", Color(1.0, 0.92, 0.6))
	box.add_child(title)
	var grid := GridContainer.new()
	grid.columns = 3
	grid.add_theme_constant_override("h_separation", 14)
	grid.add_theme_constant_override("v_separation", 10)
	box.add_child(grid)

	map_pick = OptionButton.new()
	for m in maps:
		map_pick.add_item(m[0])
	_row(grid, "Map", null, map_pick)
	nation_pick = _nations("gbr")
	flags.you = _flag()
	_row(grid, "You", flags.you, nation_pick)
	enemy_pick = _nations("ger")
	flags.enemy = _flag()
	_row(grid, "Opponent (dummy AI)", flags.enemy, enemy_pick)
	money_pick = OptionButton.new()
	for v in Skirmish.MONEY:
		money_pick.add_item("The map's" if v == 0 else str(v))
	_row(grid, "Starting money", null, money_pick)
	seed_pick = SpinBox.new()
	seed_pick.min_value = 1
	seed_pick.max_value = 999999
	seed_pick.value = 1
	_row(grid, "Seed", null, seed_pick)
	nation_pick.item_selected.connect(func(_i: int) -> void: _update_flags())
	enemy_pick.item_selected.connect(func(_i: int) -> void: _update_flags())
	_update_flags()

	var about := Label.new()
	about.text = "Both sides start with their Corps HQ. Deploy division HQs, recruit, take the cities,\nand destroy every enemy HQ to win. The dummy AI defends its base and attacks every few minutes."
	about.add_theme_font_size_override("font_size", 13)
	about.add_theme_color_override("font_color", Color(0.85, 0.85, 0.8))
	box.add_child(about)
	var buttons := HBoxContainer.new()
	buttons.alignment = BoxContainer.ALIGNMENT_CENTER
	buttons.add_theme_constant_override("separation", 24)
	box.add_child(buttons)
	start_button = Button.new()
	start_button.text = "Start"
	start_button.custom_minimum_size = Vector2(150, 40)
	start_button.pressed.connect(_start)
	buttons.add_child(start_button)
	var multi := Button.new()  # M8
	multi.text = "Multiplayer"
	multi.custom_minimum_size = Vector2(150, 40)
	multi.pressed.connect(func() -> void: get_tree().change_scene_to_file("res://scenes/lobby.tscn"))
	buttons.add_child(multi)
	var films := Button.new()  # M8
	films.text = "Films"
	films.custom_minimum_size = Vector2(150, 40)
	films.pressed.connect(func() -> void: get_tree().change_scene_to_file("res://scenes/films.tscn"))
	buttons.add_child(films)
	var quit := Button.new()
	quit.text = "Quit"
	quit.custom_minimum_size = Vector2(150, 40)
	quit.pressed.connect(func() -> void: get_tree().quit())
	buttons.add_child(quit)
	status = Label.new()
	status.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	box.add_child(status)


func _row(grid: GridContainer, caption: String, icon: Control, control: Control) -> void:
	var l := Label.new()
	l.text = caption
	l.add_theme_color_override("font_color", Color(0.95, 0.9, 0.75))
	grid.add_child(l)
	grid.add_child(icon if icon != null else Control.new())
	control.custom_minimum_size = Vector2(320, 0)
	grid.add_child(control)


func _nations(pick: String) -> OptionButton:
	var o := OptionButton.new()
	for n in Skirmish.NATIONS:
		o.add_item(Skirmish.NATION_NAMES[n])
	o.select(Skirmish.NATIONS.find(pick))
	return o


func _flag() -> TextureRect:
	var t := TextureRect.new()
	t.custom_minimum_size = Vector2(48, 32)
	t.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	t.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	return t


func _update_flags() -> void:
	for k in ["you", "enemy"]:
		var n: String = Skirmish.NATIONS[(nation_pick if k == "you" else enemy_pick).selected]
		(flags[k] as TextureRect).texture = art.tex("UI/Default/Staging/Flag%s.PNG" % n.to_upper())
	art.flush()


func _start(scripted := false, test_path := "", defeat := false) -> void:
	var s := Skirmish.defaults(str(maps[map_pick.selected][1]))
	s.nation = Skirmish.NATIONS[nation_pick.selected]
	s.enemy = Skirmish.NATIONS[enemy_pick.selected]
	s.money = Skirmish.MONEY[money_pick.selected]
	s.seed = int(seed_pick.value)
	s.scripted = scripted
	s.slice_test = test_path
	s.defeat_test = defeat
	Skirmish.settings = s
	status.text = "Loading the game data and the map..."
	start_button.disabled = true
	await get_tree().process_frame
	get_tree().change_scene_to_file("res://scenes/battle.tscn")


# ---------------------------------------------------------------- the slice play-test

func _run_slice_test(path: String, defeat := false) -> void:
	var found := false
	for i in maps.size():
		if str(maps[i][1]).contains("golden_fields"):
			map_pick.select(i)
			found = true
	if not found or _game_dir() == "":
		print("SLICE TEST SKIPPED: no game install or no cache/maps/golden_fields/map.json (tools/rmp.py)")
		get_tree().quit(77)
		return
	nation_pick.select(Skirmish.NATIONS.find("gbr"))
	enemy_pick.select(Skirmish.NATIONS.find("ger"))
	money_pick.select(0)
	seed_pick.value = 2
	_update_flags()
	print("SLICE TEST: setup screen: %s, %s vs %s, money %s, seed %d" % [maps[map_pick.selected][0], Skirmish.NATIONS[nation_pick.selected],
		Skirmish.NATIONS[enemy_pick.selected], money_pick.get_item_text(money_pick.selected), int(seed_pick.value)])
	_start(true, path, defeat)


func _slice_test_path() -> String:
	for a in OS.get_cmdline_user_args():
		if a == "--slice-test":
			return "user://slice.rpl"
		if a.begins_with("--slice-test="):
			return a.trim_prefix("--slice-test=")
	return ""


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
