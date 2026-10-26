class_name MessageLog
extends Control
## The message log (UI/Game/message_log.tgi: at 57, 45, 564 wide, growing down): the newest
## messages with their message-type icon (Game/message_types.tgi), each fading out after a while;
## and the full history in the original's side panel (game_interface.tgi SideMessageLogPanel,
## MenuTile frame), toggled by the control panel's log tab.

const SHOW := 6
const LIFE_MS := 9000
const FADE_MS := 1500
const ICONS := {
	"generic": "Game/MessageTypeIcons/IconGenericEvent.png",
	"warning": "Game/MessageTypeIcons/IconGenericWarning.png",
	"error": "Game/MessageTypeIcons/IconGenericError.png",
	"regiment": "Game/MessageTypeIcons/IconCompanyGeneric.png",
	"regiment_bad": "Game/MessageTypeIcons/IconCompanyNegative.png",
	"routed": "Game/MessageTypeIcons/IconCompanyRouted.png",
	"destroyed": "Game/MessageTypeIcons/IconCompanyDestroyed.png",
	"building": "Game/MessageTypeIcons/IconBuildingGeneric.png",
	"building_bad": "Game/MessageTypeIcons/IconBuildingNegative.png",
	"captured": "Game/MessageTypeIcons/IconBuildingCaptured.png",
	"defeated": "Game/MessageTypeIcons/IconKingdomDefeated.png",
	"quest_good": "Game/MessageTypeIcons/IconQuestPositive.png",
	"quest_bad": "Game/MessageTypeIcons/IconQuestNegative.png",
}

const ROW := 19.0

var art: UiArt
var entries: Array = []        # {text, kind, color, ms}
var lines: Control             # the transient rows: an icon column and a text column (two draw batches)
var icons_col: Control
var texts_col: Control
var history_panel: PanelContainer
var history_box: VBoxContainer
var font_size := 12


func setup(p_art: UiArt) -> void:
	art = p_art
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	lines = Control.new()
	lines.mouse_filter = Control.MOUSE_FILTER_IGNORE
	lines.size = Vector2(564, ROW * SHOW)
	add_child(lines)
	icons_col = Control.new()
	icons_col.mouse_filter = Control.MOUSE_FILTER_IGNORE
	lines.add_child(icons_col)
	texts_col = Control.new()
	texts_col.mouse_filter = Control.MOUSE_FILTER_IGNORE
	lines.add_child(texts_col)


## Adds a message. `kind`: a key of ICONS.
func add(text: String, kind := "generic", color := Color.WHITE) -> void:
	var e := {"text": text, "kind": kind, "color": color, "ms": Time.get_ticks_msec()}
	entries.append(e)
	if entries.size() > 200:
		entries.pop_front()
	_rebuild()
	if history_box != null and history_panel.visible:
		history_box.add_child(_row(e))


func latest() -> String:
	return "" if entries.is_empty() else str(entries.back().text)


func _rebuild() -> void:
	for col in [icons_col, texts_col]:
		for c in col.get_children():
			c.queue_free()
	var now := Time.get_ticks_msec()
	var fresh: Array = []
	for i in range(entries.size() - 1, -1, -1):
		var e: Dictionary = entries[i]
		if now - int(e.ms) > LIFE_MS + FADE_MS or fresh.size() >= SHOW:
			break
		fresh.push_front(e)
	for k in fresh.size():
		var e: Dictionary = fresh[k]
		var icon := _icon(e)
		icon.position = Vector2(0, k * ROW)
		icons_col.add_child(icon)
		var lab := _text(e)
		lab.position = Vector2(24, k * ROW)
		texts_col.add_child(lab)
	art.flush()


func _icon(e: Dictionary) -> TextureRect:
	var icon := TextureRect.new()
	icon.texture = art.tex(ICONS.get(e.kind, ICONS.generic))
	icon.size = Vector2(16, ROW)
	icon.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	icon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	icon.mouse_filter = Control.MOUSE_FILTER_IGNORE
	icon.set_meta("ms", e.ms)
	return icon


func _text(e: Dictionary) -> Label:
	var lab := Label.new()
	lab.text = e.text
	lab.add_theme_font_override("font", art.font())
	lab.add_theme_font_size_override("font_size", font_size)
	lab.add_theme_color_override("font_color", e.color)
	lab.add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.6))
	lab.add_theme_constant_override("shadow_offset_x", 1)
	lab.add_theme_constant_override("shadow_offset_y", 1)
	lab.mouse_filter = Control.MOUSE_FILTER_IGNORE
	lab.set_meta("ms", e.ms)
	return lab


## A history row: icon and text.
func _row(e: Dictionary) -> Control:
	var row := HBoxContainer.new()
	row.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var icon := _icon(e)
	icon.custom_minimum_size = Vector2(16, ROW)
	row.add_child(icon)
	row.add_child(_text(e))
	return row


## Fades the transient lines (call a few times a second).
func tick() -> void:
	var now := Time.get_ticks_msec()
	var drop := false
	for col in [icons_col, texts_col]:
		for item in col.get_children():
			var age := now - int(item.get_meta("ms", now))
			if age > LIFE_MS + FADE_MS:
				drop = true
			elif age > LIFE_MS:
				(item as CanvasItem).modulate.a = 1.0 - float(age - LIFE_MS) / FADE_MS
	if drop:
		_rebuild()


## The history panel (created on first use) at `rect` in the HUD's layout units.
func toggle_history(parent: Control, rect: Rect2) -> void:
	if history_panel == null:
		history_panel = PanelContainer.new()
		var st := art.panel_style("/UI/%s/Game/Panel/MenuTile")
		if st != null:
			history_panel.add_theme_stylebox_override("panel", st)
		var scroll := ScrollContainer.new()
		scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
		history_panel.add_child(scroll)
		history_box = VBoxContainer.new()
		history_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		scroll.add_child(history_box)
		history_panel.visible = false
		parent.add_child(history_panel)
	history_panel.position = rect.position
	history_panel.size = rect.size
	history_panel.visible = not history_panel.visible
	if history_panel.visible:
		for c in history_box.get_children():
			c.queue_free()
		for e in entries:
			history_box.add_child(_row(e))
