class_name EndScreen
extends Control
## The victory / defeat screen (M6 close-out): the battlefield dimmed behind the original's
## MenuTile panel, the result, how the match ended, and its statistics for both sides (match time,
## regiments recruited, units lost, buildings lost, cities held at the end). Continue watching
## closes it (the sim runs on); New skirmish goes back to the setup screen; Quit quits.

signal new_skirmish
signal quit_game

var title: Label
var won := false


## `stats`: {time: "m:ss", rows: [[caption, yours, theirs]]}.
func setup(art: UiArt, p_won: bool, reason: String, stats: Dictionary) -> void:
	won = p_won
	set_anchors_preset(Control.PRESET_FULL_RECT)
	position = Vector2.ZERO
	size = get_viewport_rect().size
	get_viewport().size_changed.connect(_fit)
	mouse_filter = Control.MOUSE_FILTER_STOP
	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.6)
	dim.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(dim)
	var center := CenterContainer.new()
	center.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(center)
	# The panel art is see-through in places: a solid backing behind it.
	var backing := ColorRect.new()
	backing.color = Color(0.1, 0.11, 0.08, 0.92)
	backing.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var panel := PanelContainer.new()
	var style := art.panel_style("/UI/%s/Game/Panel/MenuTile")
	if style != null:
		panel.add_theme_stylebox_override("panel", style)
	panel.custom_minimum_size = Vector2(520, 0)
	center.add_child(panel)
	panel.add_child(backing)
	backing.show_behind_parent = true
	panel.resized.connect(func() -> void:
		backing.position = Vector2(12, 12)
		backing.size = panel.size - Vector2(24, 24))
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 10)
	panel.add_child(box)
	var font := art.font()
	title = _label(box, "VICTORY" if won else "DEFEAT", 40, Color(0.45, 1.0, 0.45) if won else Color(1.0, 0.38, 0.3), font)
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	var why := _label(box, reason, 15, Color(0.95, 0.92, 0.8), font)
	why.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_label(box, "Match time %s" % stats.get("time", "?"), 14, Color(0.85, 0.85, 0.8), font).horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	var grid := GridContainer.new()
	grid.columns = 3
	grid.add_theme_constant_override("h_separation", 30)
	box.add_child(grid)
	for cell in ["", "You", "Enemy"]:
		_label(grid, cell, 14, Color(1.0, 0.92, 0.6), font)
	for row in stats.get("rows", []):
		_label(grid, str(row[0]), 14, Color(0.95, 0.95, 0.9), font)
		_label(grid, str(row[1]), 14, Color.WHITE, font).horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		_label(grid, str(row[2]), 14, Color.WHITE, font).horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	var buttons := HBoxContainer.new()
	buttons.alignment = BoxContainer.ALIGNMENT_CENTER
	buttons.add_theme_constant_override("separation", 16)
	box.add_child(buttons)
	for b in [["Continue watching", func() -> void: queue_free()], ["New skirmish", func() -> void: new_skirmish.emit()],
			["Quit", func() -> void: quit_game.emit()]]:
		var btn := Button.new()
		btn.text = b[0]
		btn.custom_minimum_size = Vector2(150, 36)
		btn.add_theme_font_override("font", font)
		btn.pressed.connect(b[1])
		buttons.add_child(btn)


func _fit() -> void:
	if is_inside_tree():
		size = get_viewport_rect().size


func _label(parent: Control, text: String, size_px: int, color: Color, font: Font) -> Label:
	var l := Label.new()
	l.text = text
	l.add_theme_font_override("font", font)
	l.add_theme_font_size_override("font_size", size_px)
	l.add_theme_color_override("font_color", color)
	l.add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.6))
	l.add_theme_constant_override("shadow_offset_x", 1)
	l.add_theme_constant_override("shadow_offset_y", 1)
	parent.add_child(l)
	return l
