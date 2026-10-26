class_name HudButton
extends TextureButton
## A HUD button with the original's art: a frame (normal / hover / pressed textures), an optional
## icon drawn inside it, greyed when disabled, and a tooltip in the original's InfoTile panel
## (title, body, and a red line naming what is missing).

var icon_rect: TextureRect
var tip_title := ""
var tip_body := ""
var tip_warning := ""
var tip_style: StyleBox
var tip_font: Font
var inset := 1.0   # icon inset (layout units); the original presses the icon in by 3


func _init() -> void:
	ignore_texture_size = true
	stretch_mode = TextureButton.STRETCH_SCALE
	icon_rect = TextureRect.new()
	icon_rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	icon_rect.stretch_mode = TextureRect.STRETCH_SCALE
	icon_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(icon_rect)
	button_down.connect(func() -> void: _place_icon(3.0))
	button_up.connect(func() -> void: _place_icon(inset))
	resized.connect(func() -> void: _place_icon(inset))


func set_icon(t: Texture2D) -> void:
	icon_rect.texture = t
	_place_icon(inset)


func set_enabled(on: bool) -> void:
	disabled = not on
	modulate = Color.WHITE if on else Color(0.55, 0.55, 0.55)


func set_tip(title: String, body := "", warning := "") -> void:
	tip_title = title
	tip_body = body
	tip_warning = warning
	tooltip_text = title if title != "" else " "  # non-empty: Godot asks for the tooltip


func _place_icon(d: float) -> void:
	icon_rect.position = Vector2(d, d)
	icon_rect.size = size - Vector2(2 * d, 2 * d)


func _make_custom_tooltip(_for_text: String) -> Object:
	return HudButton.make_tooltip(tip_title, tip_body, tip_warning, tip_style, tip_font)


static func make_tooltip(title: String, body: String, warning: String, style: StyleBox, font: Font) -> Control:
	var panel := PanelContainer.new()
	if style != null:
		panel.add_theme_stylebox_override("panel", style)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	panel.add_child(box)
	var lines := [[title, Color(1.0, 0.92, 0.6), 14], [body, Color(0.92, 0.92, 0.92), 12], [warning, Color(1.0, 0.4, 0.3), 12]]
	for l in lines:
		if str(l[0]) == "":
			continue
		var lab := Label.new()
		lab.text = l[0]
		lab.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		lab.custom_minimum_size.x = 260
		lab.add_theme_color_override("font_color", l[1])
		lab.add_theme_font_size_override("font_size", l[2])
		if font != null:
			lab.add_theme_font_override("font", font)
		box.add_child(lab)
	return panel
