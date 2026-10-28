extends Control
## The Films menu (M8, NETCODE §8): every *.rpl in user:// (LastFilm.rpl is written after each
## match), with its map, length and players. Play opens it in the battle view (see FilmControls).
## `--film=PATH` plays that film at once (tests).

const BACKGROUND := Color(0.11, 0.12, 0.09)

var list: VBoxContainer
var status: Label


func _ready() -> void:
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
	title.text = "Films"
	title.add_theme_font_size_override("font_size", 28)
	title.add_theme_color_override("font_color", Color(1.0, 0.92, 0.6))
	box.add_child(title)
	list = VBoxContainer.new()
	box.add_child(list)
	status = Label.new()
	box.add_child(status)
	var back := Button.new()
	back.text = "Back"
	back.custom_minimum_size = Vector2(120, 32)
	back.pressed.connect(func() -> void: get_tree().change_scene_to_file("res://scenes/skirmish_setup.tscn"))
	box.add_child(back)
	_fill()
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--film="):
			play.call_deferred(a.trim_prefix("--film="))


func _fill() -> void:
	var dir := DirAccess.open("user://")
	var files: Array[String] = []
	if dir != null:
		for f in dir.get_files():
			if f.get_extension().to_lower() == "rpl":
				files.append(f)
	files.sort()
	if files.is_empty():
		status.text = "No films yet: every match is recorded to LastFilm.rpl when it ends."
	for f in files:
		var path := ProjectSettings.globalize_path("user://" + f)
		var info: Dictionary = SimBridge.film_info(path)
		var row := HBoxContainer.new()
		var l := Label.new()
		l.custom_minimum_size = Vector2(620, 0)
		if bool(info.ok):
			var t := int(info.ticks)
			l.text = "%s   %s   %d:%02d   %s   (%d keyframes)" % [f, str(info.map_spec).get_file(), t / 600, (t / 10) % 60,
				", ".join(info.players), int(info.keyframes)]
		else:
			l.text = "%s   (%s)" % [f, str(info.error)]
		row.add_child(l)
		var b := Button.new()
		b.text = "Play"
		b.disabled = not bool(info.ok)
		b.pressed.connect(func() -> void: play(path))
		row.add_child(b)
		list.add_child(row)


## The map a film names: a generated spec, or the cache map whose source is the film's map.
static func resolve_map(map_spec: String) -> String:
	if map_spec.begins_with("gen:") or map_spec.begins_with("arena:"):
		return map_spec
	var loader := CacheLoader.new("")
	var root := loader.cache_dir.path_join("maps")
	var dir := DirAccess.open(root)
	if dir == null:
		return ""
	for n in dir.get_directories():
		var p := root.path_join(n).path_join("map.json")
		var f := FileAccess.open(p, FileAccess.READ)
		if f == null:
			continue
		var head := f.get_buffer(2048).get_string_from_utf8()
		if head.contains(JSON.stringify(map_spec)):
			return p
	return ""


func play(path: String) -> void:
	var info: Dictionary = SimBridge.film_info(path)
	if not bool(info.ok):
		status.text = str(info.error)
		return
	var spec := resolve_map(str(info.map_spec))
	if spec == "":
		status.text = "The film's map %s isn't in cache/maps (tools/rmp.py)." % info.map_spec
		print("FILM: " + status.text)
		return
	var s := Skirmish.defaults(spec)
	s.seed = int(info.seed)
	s.film = path
	s.ai = ""
	s.players = info.players
	Skirmish.settings = s
	status.text = "Loading..."
	await get_tree().process_frame
	get_tree().change_scene_to_file("res://scenes/battle.tscn")
