class_name Hud
extends Control
## The battle HUD (M6 part 3) in the original's art (UiArt) and layout (UI/Game/*.tgi, 1024 units):
##  * control panel (control_panel.tgi): the nation's Background.tga across the bottom, its round
##    buttons (supply border, menu, chat, pause; the objectives / technology / log / politics tabs);
##  * stats bar (economy_bar.tgi, EconomyBar.PNG): money reserve and per-minute income, oil and
##    ammo balance per minute, buildings and units used of their limits, with the font's resource
##    glyphs; the bleed icon (game_interface.tgi UpkeepShortageIcon) when upkeep is short;
##  * command panel (action_buttons.tgi: 4 x 3 buttons of 42 at 831, 108): the selection's
##    commands. A building: Deploy (trucks) / Recruit (regiments), Research, Pack, and Cancel while
##    researching. A truck regiment: Unpack. Regiments: Stop, Attack, Run, group behaviour.
##    Greyed buttons say what is missing (SimBridge.building_commands: the sim's own checks);
##  * info panel (the centre frame): the selected regiment's strength, health, morale, efficiency,
##    level, state; or the building's state, production queue (ProductionQueueSlot art, click a
##    slot to cancel it), research, supply and capture;
##  * minimap (Minimap), message log (MessageLog).
## The HUD only reads SimBridge and asks the battle (`command` signal) to queue Commands.

signal command(kind: String, data: Dictionary)
signal minimap_clicked(map_pos: Vector2, button: int)

const PANEL := Vector2(1024, 256)
const GRID_ORIGIN := Vector2(831, 108)
const GRID_PITCH := Vector2(47.67, 48)
const GRID_SLOTS := 12
const INFO := Rect2(300, 68, 500, 180)
const RESOURCES := [  # economy_bar.tgi EconomyBarWidget: label x, width; glyph
	["money", 6, 127, "money"], ["ammo", 139, 80, "ammo"], ["oil", 224, 78, "fuel"],
	["building", 306, 72, "building"], ["unit", 381, 75, "unitlimit"]]

var bridge: SimBridge
var art: UiArt
var player := 0
var panel: Control
var screen: Control        # screen-anchored layer (1024 units, scaled)
var background: TextureRect
var econ_labels := {}
var econ_icons := {}
var bleed: TextureRect
var grid: Array[HudButton] = []
var minimap: Minimap
var log: MessageLog
var info_root: Control
var info := {}             # named info-panel nodes
var queue_slots: Array[HudButton] = []
var round_buttons := {}
var banner: Label
var supply_on := false
var paused := false
var selection := {"regiments": PackedInt64Array(), "building": 0}
var page := 0
var commands_now: Array = []   # what the grid shows: [{kind, ...}]
var economy_now := {}
var stats := {"refreshes": 0}
var _tip_style: StyleBox
var _bound: Array = []         # [node, property, texture path] for _retexture()


func setup(p_bridge: SimBridge, p_art: UiArt, p_player: int) -> void:
	bridge = p_bridge
	art = p_art
	player = p_player
	art.choose(get_viewport_rect().size)  # the set to build with
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_tip_style = art.panel_style("/UI/%s/Game/Panel/InfoTile", "png")
	theme = Theme.new()
	theme.default_font = art.font()
	theme.default_font_size = 12
	screen = Control.new()
	screen.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(screen)
	panel = Control.new()
	panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	panel.size = PANEL
	add_child(panel)
	_build_panel()
	log = MessageLog.new()
	log.position = Vector2(57, 45)
	screen.add_child(log)
	log.setup(art)
	bleed = TextureRect.new()
	_bind(bleed, "texture", "/UI/%s/Game/ControlPanel/BleedIcon.png")
	bleed.position = Vector2(609, 2)
	bleed.size = Vector2(40, 40)
	bleed.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	bleed.visible = false
	bleed.tooltip_text = bridge.localize("#game_interface_UpkeepShortageIcon_ToolTip")
	screen.add_child(bleed)
	banner = Label.new()
	banner.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	banner.add_theme_font_size_override("font_size", 40)
	banner.add_theme_color_override("font_outline_color", Color.BLACK)
	banner.add_theme_constant_override("outline_size", 8)
	banner.mouse_filter = Control.MOUSE_FILTER_IGNORE
	banner.visible = false
	banner.position = Vector2(0, 250)
	banner.size = Vector2(1024, 60)
	screen.add_child(banner)
	get_viewport().size_changed.connect(layout)
	layout()
	art.flush()


## Places the panel (bottom centre) and the screen layer for the current window size.
func layout() -> void:
	var view := get_viewport_rect().size
	if art.choose(view):
		_retexture()
	var s := art.scale
	panel.scale = Vector2(s, s)
	panel.position = Vector2(roundf((view.x - PANEL.x * s) * 0.5), roundf(view.y - PANEL.y * s))
	screen.scale = Vector2(s, s)
	screen.position = Vector2(roundf((view.x - 1024.0 * s) * 0.5), 0)
	screen.size = Vector2(1024, view.y / s)


## A static texture: set now, and again by _retexture() when the resolution set changes.
func _bind(node: Object, property: String, path: String) -> void:
	node.set(property, art.tex(path))
	_bound.append([node, property, path])


## The panel's art (again after the resolution set changed).
func _retexture() -> void:
	for b in _bound:
		(b[0] as Object).set(b[1], art.tex(b[2]))
	(background as AlphaHit).hit_image = art.image_of("/UI/%s/Game/ControlPanel/Background.tga")
	for key in round_buttons:
		var b: HudButton = round_buttons[key].button
		_frame(b, round_buttons[key].art)
		set_toggle(key, round_buttons[key].on)
	art.flush()


# ---------------------------------------------------------------- building the panel

func _build_panel() -> void:
	background = AlphaHit.new()
	_bind(background, "texture", "/UI/%s/Game/ControlPanel/Background.tga")
	(background as AlphaHit).hit_image = art.image_of("/UI/%s/Game/ControlPanel/Background.tga")
	background.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	background.size = PANEL
	background.mouse_filter = Control.MOUSE_FILTER_STOP
	panel.add_child(background)

	# Stats bar.
	var bar := TextureRect.new()
	_bind(bar, "texture", "/UI/%s/Game/ControlPanel/EconomyBar.png")
	bar.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	bar.position = Vector2(320, 8)
	bar.size = Vector2(469, 31)
	bar.mouse_filter = Control.MOUSE_FILTER_STOP
	panel.add_child(bar)
	# Icons first, then the labels: two draw batches (atlas, font) instead of ten.
	for r in RESOURCES:
		var icon := TextureRect.new()
		_bind(icon, "texture", art.glyph(r[3]))
		icon.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
		icon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
		icon.size = Vector2(16, 16)
		icon.position = Vector2(float(r[1]) + 2, 7)
		icon.mouse_filter = Control.MOUSE_FILTER_IGNORE
		bar.add_child(icon)
	for r in RESOURCES:
		var lab := _label(12)
		lab.position = Vector2(float(r[1]) + 20, 6)
		lab.size = Vector2(float(r[2]) - 22, 19)
		lab.mouse_filter = Control.MOUSE_FILTER_STOP  # the tooltip
		bar.add_child(lab)
		econ_labels[r[0]] = lab
		econ_icons[r[0]] = lab

	# Command grid.
	for i in GRID_SLOTS:
		var b := HudButton.new()
		_frame_action(b)
		b.position = GRID_ORIGIN + Vector2(i % 4, i / 4) * GRID_PITCH
		b.size = Vector2(42, 42)
		b.tip_style = _tip_style
		b.tip_font = art.font()
		b.visible = false
		var k := i
		b.pressed.connect(func() -> void: _grid_pressed(k))
		panel.add_child(b)
		grid.append(b)

	# Round buttons and side tabs (control_panel.tgi).
	_round("supply", "ButtonSupplyBorder", Vector2(9, 30), Vector2(54, 54), "#control_panel_ButtonSupplyBorder_ToolTip",
		"#control_panel_ButtonSupplyBorder_tooltip_2", {"normal": "Unpressed", "hover": "UnpressedFocused", "pressed": "PressedFocused",
		"on": "PressedUnfocused"})
	_round("menu", "ButtonOptions", Vector2(9, 203), Vector2(54, 54), "#control_panel_ButtonMenu_ToolTip", "#control_panel_ButtonMenu_tooltip_2",
		{"normal": "Unpressed", "hover": "UnpressedFocused", "pressed": "PressedFocused", "on": "Pressed"})
	_round("pause", "ButtonPause", Vector2(245, 203), Vector2(54, 54), "#control_panel_ButtonPause_ToolTip", "#control_panel_ButtonPause_tooltip_2",
		{"normal": "Unpressed", "hover": "UnpressedFocused", "pressed": "PressedFocused", "on": "Pressed"})
	for t in [["objectives", "Objectives", 838], ["technology", "Technology", 883], ["log", "Log", 928], ["politics", "Politics", 973]]:
		var key := "MessageLog" if t[1] == "Log" else str(t[1])
		_round(t[0], "SidePanelTab" + t[1], Vector2(t[2], 40), Vector2(45, 52), "#control_panel_Button%s_ToolTip" % key,
			"#control_panel_Button%s_tooltip_2" % key, {"normal": "Unselected", "hover": "UnselectedFocused", "pressed": "SelectedFocused",
			"on": "Selected"})

	minimap = Minimap.new()
	minimap.position = Vector2(60, 42)
	_bind(minimap, "city_icon", "Minimap/Icons/IconCityCenter.tga")
	minimap.clicked.connect(func(p: Vector2, b: int) -> void: minimap_clicked.emit(p, b))
	panel.add_child(minimap)

	_build_info()


func _frame_action(b: HudButton) -> void:
	_bind(b, "texture_normal", "/UI/%s/Game/SharedTextures/ButtonUnfocused.png")
	_bind(b, "texture_hover", "/UI/%s/Game/SharedTextures/ButtonFocused.png")
	_bind(b, "texture_pressed", "/UI/%s/Game/SharedTextures/ButtonPressed.png")
	_bind(b, "texture_disabled", "/UI/%s/Game/SharedTextures/ButtonUnfocused.png")


func _round(key: String, base: String, at: Vector2, sz: Vector2, title_key: String, body_key: String, states: Dictionary) -> void:
	var b := HudButton.new()
	b.position = at
	b.size = sz
	b.tip_style = _tip_style
	b.tip_font = art.font()
	b.set_tip(bridge.localize(title_key), bridge.localize(body_key))
	var a := {"base": base, "states": states}
	round_buttons[key] = {"button": b, "art": a, "on": false}
	_frame(b, a)
	b.pressed.connect(func() -> void: _round_pressed(key))
	panel.add_child(b)


func _frame(b: HudButton, a: Dictionary) -> void:
	var p := "/UI/%s/Game/ControlPanel/" + str(a.base)
	var st: Dictionary = a.states
	b.texture_normal = art.tex(p + str(st.normal) + ".tga")
	b.texture_hover = art.tex(p + str(st.hover) + ".tga")
	b.texture_pressed = art.tex(p + str(st.pressed) + ".tga")
	b.set_meta("tex_on", art.tex(p + str(st.on) + ".tga"))
	b.set_meta("tex_off", b.texture_normal)


## Toggle look for a round button.
func set_toggle(key: String, on: bool) -> void:
	var r: Dictionary = round_buttons[key]
	r.on = on
	var b: HudButton = r.button
	b.texture_normal = b.get_meta("tex_on") if on else b.get_meta("tex_off")


func _round_pressed(key: String) -> void:
	match key:
		"supply":
			supply_on = not supply_on
			set_toggle("supply", supply_on)
			command.emit("supply", {"on": supply_on})
		"pause":
			command.emit("pause", {})
		"log":
			log.toggle_history(screen, Rect2(279, 53, 542, 400))
			set_toggle("log", log.history_panel.visible)
		_:
			command.emit(key, {})


func _label(size_px: int, color := Color.WHITE) -> Label:
	var l := Label.new()
	l.add_theme_font_size_override("font_size", size_px)
	l.add_theme_color_override("font_color", color)
	l.add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.55))
	l.add_theme_constant_override("shadow_offset_x", 1)
	l.add_theme_constant_override("shadow_offset_y", 1)
	l.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return l


# ---------------------------------------------------------------- info panel

func _build_info() -> void:
	info_root = Control.new()
	info_root.position = INFO.position
	info_root.size = INFO.size
	info_root.mouse_filter = Control.MOUSE_FILTER_IGNORE
	panel.add_child(info_root)
	var frame := TextureRect.new()
	_bind(frame, "texture", "/UI/%s/Game/ObjectDisplay/BorderUnfocusedPortrait.tga")
	frame.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	frame.position = Vector2(10, 14)
	frame.size = Vector2(96, 96)
	frame.mouse_filter = Control.MOUSE_FILTER_IGNORE
	info_root.add_child(frame)
	info.frame = frame
	var icon := TextureRect.new()
	icon.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	icon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	icon.position = Vector2(22, 26)
	icon.size = Vector2(72, 72)
	icon.mouse_filter = Control.MOUSE_FILTER_IGNORE
	info_root.add_child(icon)
	info.icon = icon
	var name := _label(14, Color(1.0, 0.92, 0.6))
	name.position = Vector2(120, 6)
	name.size = Vector2(370, 20)
	info_root.add_child(name)
	info.name = name
	for k in 3:
		var row := Control.new()
		row.position = Vector2(120, 30 + k * 17)
		row.size = Vector2(360, 15)
		row.mouse_filter = Control.MOUSE_FILTER_IGNORE
		info_root.add_child(row)
		var cap := _label(11)
		cap.size = Vector2(80, 15)
		row.add_child(cap)
		var back := ColorRect.new()
		back.color = Color(0, 0, 0, 0.6)
		back.position = Vector2(82, 3)
		back.size = Vector2(180, 9)
		back.mouse_filter = Control.MOUSE_FILTER_IGNORE
		row.add_child(back)
		var fill := ColorRect.new()
		fill.position = Vector2(83, 4)
		fill.size = Vector2(0, 7)
		fill.mouse_filter = Control.MOUSE_FILTER_IGNORE
		row.add_child(fill)
		var val := _label(11)
		val.position = Vector2(268, 0)
		val.size = Vector2(95, 15)
		row.add_child(val)
		info["bar%d" % k] = {"row": row, "cap": cap, "fill": fill, "val": val}
	var text := _label(11, Color(0.9, 0.9, 0.85))
	text.position = Vector2(120, 82)
	text.size = Vector2(370, 46)
	info_root.add_child(text)
	info.text = text
	# Production queue: slot 1 (64) with its progress bar, then slots 2..5 (32).
	for q in 5:
		var b := HudButton.new()
		var big := q == 0
		var p := "/UI/%s/Game/ControlPanel/ProductionQueueSlot" + ("1" if big else "2-7")
		_bind(b, "texture_normal", p + ".PNG")
		_bind(b, "texture_hover", p + "Focused.png")
		_bind(b, "texture_pressed", p + "Pressed.png")
		b.position = Vector2(10, 112) if big else Vector2(80 + (q - 1) * 34, 144)
		b.size = Vector2(64, 64) if big else Vector2(32, 32)
		b.inset = 8.0 if big else 4.0
		b.tip_style = _tip_style
		b.tip_font = art.font()
		b.visible = false
		var k := q
		b.pressed.connect(func() -> void: command.emit("cancel_recruit", {"building": selection.building, "index": k}))
		info_root.add_child(b)
		queue_slots.append(b)
	var prog := ColorRect.new()
	prog.color = Color(0.35, 0.85, 0.35)
	prog.position = Vector2(10, 172)
	prog.size = Vector2(0, 5)
	prog.mouse_filter = Control.MOUSE_FILTER_IGNORE
	info_root.add_child(prog)
	info.progress = prog
	var research := HudButton.new()
	_frame_action(research)
	research.position = Vector2(448, 128)
	research.size = Vector2(42, 42)
	research.tip_style = _tip_style
	research.tip_font = art.font()
	research.visible = false
	research.pressed.connect(func() -> void: command.emit("cancel_research", {"building": selection.building}))
	info_root.add_child(research)
	info.research = research
	var rprog := ColorRect.new()
	rprog.color = Color(0.4, 0.7, 1.0)
	rprog.position = Vector2(448, 172)
	rprog.size = Vector2(0, 5)
	rprog.mouse_filter = Control.MOUSE_FILTER_IGNORE
	info_root.add_child(rprog)
	info.research_progress = rprog


func _bar(k: int, caption: String, frac: float, value: String, color: Color) -> void:
	var b: Dictionary = info["bar%d" % k]
	(b.row as Control).visible = caption != ""
	(b.cap as Label).text = caption
	(b.fill as ColorRect).size.x = 178.0 * clampf(frac, 0.0, 1.0)
	(b.fill as ColorRect).color = color
	(b.val as Label).text = value


func _show_info_none() -> void:
	info_root.visible = false


func _show_regiment(regs: PackedInt64Array) -> void:
	var r: Dictionary = bridge.regiment_info(regs[0])
	if r.is_empty():
		info_root.visible = false
		return
	info_root.visible = true
	var ids: String = bridge.regiment_def(int(r.def)).get("ids", "?")
	var ti: Dictionary = bridge.thing_info(ids)
	(info.icon as TextureRect).texture = art.tex(str(ti.get("icon", "")))
	var name := str(ti.get("name", ids))
	(info.name as Label).text = name + ("   +%d" % (regs.size() - 1) if regs.size() > 1 else "")
	var hp := float(r.health)
	var hpmax := maxf(float(r.health_max), 1.0)
	_bar(0, bridge.localize("#ui_tooltip_item_title_health"), hp / hpmax, "%d / %d" % [roundi(hp), roundi(hpmax)],
		Color(0.2, 0.85, 0.25).lerp(Color(0.9, 0.2, 0.15), 1.0 - hp / hpmax))
	if r.has("morale"):
		var mmax := maxf(float(r.morale_max), 1.0)
		_bar(1, bridge.localize("#ui_tooltip_item_title_morale"), float(r.morale) / mmax, "%d / %d" % [roundi(r.morale), roundi(mmax)],
			Color(0.35, 0.6, 1.0))
		_bar(2, bridge.localize("#ui_tooltip_item_title_attack_efficiency"), float(r.efficiency), "%d%%" % roundi(float(r.efficiency) * 100.0),
			Color(1.0, 0.8, 0.25))
	else:
		_bar(1, "", 0, "", Color.WHITE)
		_bar(2, "", 0, "", Color.WHITE)
	var lines: Array[String] = []
	var lv := int(r.get("level", 0))
	lines.append("%s %d   %s %d   units %d" % [bridge.localize("#ui_tooltip_item_title_experience"), roundi(float(r.get("xp", 0))),
		"level", lv, int(r.units_alive)])
	var state := str(r.get("engagement", r.state))
	lines.append("%s%s%s" % [state.capitalize(), "   entrenched" if r.get("entrenched", false) else "",
		"   in supply" if r.get("in_supply", false) else ("   " + bridge.localize("#ui_tooltip_queue_paused_oos"))])
	(info.text as Label).text = "\n".join(lines)
	for b in queue_slots:
		b.visible = false
	(info.progress as ColorRect).size.x = 0
	(info.research as HudButton).visible = false
	(info.research_progress as ColorRect).size.x = 0


func _show_building(id: int) -> void:
	var s: Dictionary = bridge.structure_info(id)
	if s.is_empty():
		info_root.visible = false
		return
	info_root.visible = true
	var ti: Dictionary = bridge.thing_info(str(s.ids))
	(info.icon as TextureRect).texture = art.tex(str(ti.get("icon", "")))
	(info.name as Label).text = str(ti.get("name", s.ids))
	var hp := float(s.health)
	var hpmax := maxf(float(s.health_max), 1.0)
	_bar(0, bridge.localize("#ui_tooltip_item_title_health"), hp / hpmax, "%d / %d" % [roundi(hp), roundi(hpmax)],
		Color(0.2, 0.85, 0.25).lerp(Color(0.9, 0.2, 0.15), 1.0 - hp / hpmax))
	if s.state != "deployed":
		_bar(1, str(s.state).capitalize(), float(s.progress), "%d%%" % roundi(float(s.progress) * 100.0), Color(0.9, 0.75, 0.3))
	elif bool(s.city) and float(s.capture) > 0.0:
		_bar(1, "Capture", float(s.capture), "%d%%" % roundi(float(s.capture) * 100.0), Color(1.0, 0.5, 0.2))
	else:
		_bar(1, "", 0, "", Color.WHITE)
	_bar(2, "", 0, "", Color.WHITE)
	var prod: Dictionary = s.production
	var lines: Array[String] = []
	if float(prod.money) + float(prod.oil) + float(prod.ammo) > 0.0:
		lines.append("%s: +%d money  +%d oil  +%d ammo / min" % [bridge.localize("#ui_tooltip_item_title_production"),
			roundi(prod.money), roundi(prod.oil), roundi(prod.ammo)])
	var status: Array[String] = []
	if int(s.player) == 255:
		status.append("Neutral")
	elif int(s.player) == player:
		status.append("In supply" if s.in_supply else bridge.localize("#ui_tooltip_queue_paused_oos"))
	if s.under_siege:
		status.append(bridge.localize("#options_dialog_Child_text_13"))
	if float(s.supply_range) > 0.0:
		status.append("supply range %d m" % roundi(s.supply_range))
	lines.append("   ".join(status))
	(info.text as Label).text = "\n".join(lines)
	var q: Array = s.get("queue", [])
	for i in queue_slots.size():
		var b := queue_slots[i]
		b.visible = int(s.player) == player and s.state == "deployed" and i < q.size()
		if i < q.size():
			b.set_icon(art.tex(str(q[i].icon)))
			b.set_tip(str(q[i].name), bridge.localize("#action_buttons_CancelRecruit_tooltip"))
			b.disabled = i == 0 and float(s.recruit_progress) > 0.0
		else:
			b.set_icon(null)
			b.set_tip("")
			b.disabled = true
	(info.progress as ColorRect).size.x = 64.0 * float(s.recruit_progress) if not q.is_empty() else 0.0
	var rs: Dictionary = s.get("research", {})
	var rb: HudButton = info.research
	rb.visible = not rs.is_empty()
	if not rs.is_empty():
		rb.set_icon(art.tex(str(rs.get("icon", ""))))
		rb.set_tip(str(rs.get("name", rs.ids)), bridge.localize("#action_buttons_ActorCommandCancel_tooltip"))
	(info.research_progress as ColorRect).size.x = 42.0 * float(rs.get("progress", 0.0)) if not rs.is_empty() else 0.0


# ---------------------------------------------------------------- refresh

## Refreshes everything from the sim (the battle calls this a few times a second).
func refresh(sel: Dictionary) -> void:
	stats.refreshes += 1
	if sel.building != selection.building or sel.regiments != selection.regiments:
		page = 0
	selection = sel
	_refresh_economy()
	var regs: PackedInt64Array = sel.regiments
	if int(sel.building) != 0:
		_show_building(int(sel.building))
	elif not regs.is_empty():
		_show_regiment(regs)
	else:
		_show_info_none()
	_refresh_commands()
	log.tick()
	art.flush()  # icons loaded for the first time this refresh


func _refresh_economy() -> void:
	var e: Dictionary = bridge.economy(player)
	economy_now = e
	if e.is_empty():
		return
	var net := float(e.money_net)
	_econ("money", "%d (%s%d)" % [floori(float(e.money)), "+" if net >= 0 else "", roundi(net)],
		Color(1.0, 0.45, 0.4) if e.bankrupt else Color.WHITE,
		"Money: %d\nproduction %+d, upkeep %d, shortfall %d per minute" % [floori(float(e.money)), roundi(e.money_production),
		roundi(e.money_upkeep), roundi(e.shortfall)])
	_econ("ammo", "%+d" % roundi(e.ammo), Color(1.0, 0.5, 0.4) if float(e.ammo) < 0 else Color.WHITE,
		"Ammo per minute: production %d, upkeep %d" % [roundi(e.ammo_production), roundi(e.ammo_upkeep)])
	_econ("oil", "%+d" % roundi(e.oil), Color(1.0, 0.5, 0.4) if float(e.oil) < 0 else Color.WHITE,
		"Oil per minute: production %d, upkeep %d" % [roundi(e.oil_production), roundi(e.oil_upkeep)])
	_econ("building", "%d/%d" % [roundi(e.building_used), roundi(e.building_limit)],
		Color(1.0, 0.85, 0.3) if float(e.building_used) >= float(e.building_limit) else Color.WHITE,
		bridge.localize("#template_game_Building_Limit_Consumed_ToolTip"))
	_econ("unit", "%d/%d" % [roundi(e.unit_used), roundi(e.unit_limit)],
		Color(1.0, 0.85, 0.3) if float(e.unit_used) >= float(e.unit_limit) else Color.WHITE,
		bridge.localize("#template_game_Unit_Limit_Consumed_ToolTip"))
	bleed.visible = float(e.shortfall) > 0.0 or bool(e.bankrupt)
	bleed.modulate = Color(1, 0.4, 0.4) if e.bankrupt else Color.WHITE
	if bool(e.match_over) and not banner.visible:
		banner.visible = true
		var won := int(e.winner) == int(e.team)
		banner.text = "VICTORY" if won else "DEFEAT"
		banner.add_theme_color_override("font_color", Color(0.4, 1.0, 0.4) if won else Color(1.0, 0.35, 0.3))


func _econ(key: String, text: String, color: Color, tip: String) -> void:
	var l: Label = econ_labels[key]
	if l.text != text:
		l.text = text
	l.add_theme_color_override("font_color", color)
	(econ_icons[key] as Control).tooltip_text = tip


## The grid for the selection (see the class comment).
func _refresh_commands() -> void:
	var list: Array = []
	var regs: PackedInt64Array = selection.regiments
	if int(selection.building) != 0:
		var s: Dictionary = bridge.structure_info(int(selection.building))
		if not s.is_empty() and int(s.player) == player:
			list = bridge.building_commands(int(selection.building), player)
			if s.has("research"):
				list.append({"kind": "cancel_research", "name": bridge.localize("#action_buttons_ActorCommandCancel_name"),
					"icon": "/UI/%s/Game/ActionButtons/IconCancel.png", "description": bridge.localize("#action_buttons_ActorCommandCancel_tooltip"),
					"enabled": true, "code": ""})
	elif not regs.is_empty():
		var up: Dictionary = bridge.unpack_info(regs[0]) if regs.size() == 1 else {}
		if not up.is_empty():
			list.append({"kind": "unpack", "name": bridge.localize("#actor_commands_unpack_name") + ": " + str(up.name),
				"icon": "/UI/%s/Game/ActionButtons/IconUnPack.png", "description": bridge.localize("#actor_commands_unpack_tooltip"),
				"enabled": true, "code": "", "ids": up.ids, "regiment": regs[0]})
		var division: bool = bridge.regiment_info(regs[0]).get("division", false)
		list.append({"kind": "stop", "name": bridge.localize("#actor_commands_stop_name"), "icon": "/UI/%s/Game/ActionButtons/IconStop.png",
			"description": bridge.localize("#actor_commands_stop_tooltip"), "enabled": true, "code": ""})
		list.append({"kind": "attack", "name": bridge.localize("#actor_commands_kill_name"), "icon": "/UI/%s/Game/ActionButtons/IconAttack.png",
			"description": bridge.localize("#actor_commands_kill_tooltip"), "enabled": true, "code": ""})
		list.append({"kind": "run", "name": bridge.localize("#actor_commands_retreat_name"), "icon": "/UI/%s/Game/ActionButtons/IconRetreat.png",
			"description": bridge.localize("#actor_commands_retreat_tooltip"), "enabled": true, "code": ""})
		var gkey := "disable" if division else "enable"
		list.append({"kind": "group", "name": bridge.localize("#actor_commands_aa_group_behavior_%s_name" % gkey),
			"icon": "/UI/%s/Game/ActionButtons/IconGroupBehavior" + ("Disable" if division else "Enable") + ".png",
			"description": bridge.localize("#actor_commands_aa_group_behavior_%s_tooltip" % gkey), "enabled": regs.size() >= 2,
			"code": "" if regs.size() >= 2 else "group_size", "division": division})
	commands_now = list
	var per_page := GRID_SLOTS if list.size() <= GRID_SLOTS else GRID_SLOTS - 1
	var pages := maxi(1, ceili(float(list.size()) / per_page))
	page = mini(page, pages - 1)
	for i in GRID_SLOTS:
		var b := grid[i]
		var k := page * per_page + i
		if pages > 1 and i == GRID_SLOTS - 1:
			b.visible = true
			b.set_icon(art.tex("/UI/%s/Game/ActionButtons/IconNextPage.png"))
			b.set_enabled(true)
			b.set_tip("%d / %d" % [page + 1, pages])
			b.set_meta("cmd", {"kind": "next_page"})
			continue
		if k >= list.size():
			b.visible = false
			continue
		var c: Dictionary = list[k]
		b.visible = true
		b.set_icon(art.tex(str(c.get("icon", ""))))
		b.set_enabled(bool(c.get("enabled", true)))
		b.set_tip(str(c.get("name", "")), _body(c), reason(c))
		b.set_meta("cmd", c)


func _body(c: Dictionary) -> String:
	var parts: Array[String] = []
	var d := str(c.get("description", ""))
	if d.length() > 260:
		d = d.substr(0, 257) + "..."
	if d != "":
		parts.append(d)
	if float(c.get("cost", 0.0)) > 0.0:
		parts.append("%s: %d" % [bridge.localize("#ui_tooltip_item_title_cost"), roundi(float(c.cost))])
	if float(c.get("time", 0.0)) > 0.0 and c.kind != "pack":
		parts.append("%d s" % roundi(float(c.time)))
	return "\n".join(parts)


## Why a command is greyed, in the original's words where it has them (strings_rtse.tgi).
func reason(c: Dictionary) -> String:
	var name := str(c.get("name", ""))
	match str(c.get("code", "")):
		"":
			return ""
		"requires_structure":
			return "%s: %s" % [bridge.localize("#ui_tooltip_item_title_requires"), c.detail_name]
		"requires_property":
			return "%s: %s" % [bridge.localize("#ui_tooltip_item_title_requires"),
				_fmt(bridge.localize("#recruit_missing_required_property"), [c.detail_name])]
		"money":
			var key := "#research_error_affording" if c.kind == "research" else "#recruit_error_affording"
			return _fmt(bridge.localize(key), [name, str(c.detail)])
		"unit_limit":
			return _fmt(bridge.localize("#recruit_error_unit_limit"), [name])
		"building_limit":
			return "%s: %d/%d" % [bridge.localize("#template_game_Building_Limit_Consumed_ToolTip"),
				roundi(economy_now.get("building_used", 0)), roundi(economy_now.get("building_limit", 0))]
		"researched":
			return _fmt(bridge.localize("#research_error_already_researched"), [name])
		"researching":
			return "Research in progress"
		"queue_full":
			return "The recruit queue is full (5)"
		"corps_exists":
			return "Only one Corps HQ"
		"not_deployed":
			return "Not deployed"
		"not_started":
			return "No economy (the kingdom hasn't started)"
		"health":
			return "Health below the packing minimum (50%)"
		"busy":
			return "Recruiting or researching"
		"not_mobile":
			return "Can't pack"
		"group_size":
			return "Select two or more regiments"
	return str(c.code)


static func _fmt(pattern: String, args: Array) -> String:
	var out := pattern
	for a in args:
		var i := out.find("%s")
		if i < 0:
			break
		out = out.substr(0, i) + str(a) + out.substr(i + 2)
	return out


func _grid_pressed(i: int) -> void:
	var b := grid[i]
	if not b.has_meta("cmd"):
		return
	var c: Dictionary = b.get_meta("cmd")
	if c.kind == "next_page":
		page += 1
		var per_page := GRID_SLOTS - 1
		if page * per_page >= commands_now.size():
			page = 0
		_refresh_commands()
		return
	if not bool(c.get("enabled", true)):
		return
	var data := c.duplicate()
	data["building"] = selection.building
	data["regiments"] = selection.regiments
	command.emit(str(c.kind), data)


## Messages (the battle turns events into these).
func message(text: String, kind := "generic", color := Color.WHITE) -> void:
	log.add(text, kind, color)
