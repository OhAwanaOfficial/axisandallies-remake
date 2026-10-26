class_name Minimap
extends Control
## The minimap in the control panel's diamond (UI/Game/minimap.tgi: 200 x 200 at 60, 42). The map
## is drawn as the original does, turned 45 degrees to fill the diamond, map corner (0, 0) at the
## top; the battle camera's default heading looks the same way (RtsCamera.DEFAULT_YAW), so up on
## the minimap is up on screen until the camera turns. On it: terrain colours, regiments as
## player-coloured dots, buildings as squares (cities with the original's city icon), supply
## zones when the overlay is on, and the camera's view on the ground as a box.
## Left click or drag: centre the camera there. Right click: move the selection there.

signal clicked(map_pos: Vector2, button: int)

const SIZE := 200.0
const NEUTRAL := Color(0.85, 0.85, 0.85)

var map_size := Vector2(256, 256)
var map_tex: Texture2D
var dots := PackedVector2Array()
var dot_colors := PackedColorArray()
var buildings: Array = []      # [{pos, player, city, size}]
var zones: Array = []          # [{center, radius, color}]
var view_quad := PackedVector2Array()  # camera footprint corners (map coords), or empty
var city_icon: Texture2D
var _dragging := false


func _init() -> void:
	custom_minimum_size = Vector2(SIZE, SIZE)
	size = Vector2(SIZE, SIZE)
	mouse_filter = Control.MOUSE_FILTER_STOP


## Map point (sim x, y) -> minimap point.
func to_mini(p: Vector2) -> Vector2:
	var u := p.x / map_size.x
	var v := p.y / map_size.y
	return Vector2(SIZE * 0.5 + (u - v) * SIZE * 0.5, (u + v) * SIZE * 0.5)


## Minimap point -> map point (can be outside the map).
func to_map(m: Vector2) -> Vector2:
	var a := (m.x - SIZE * 0.5) / (SIZE * 0.5)
	var b := m.y / (SIZE * 0.5)
	return Vector2((b + a) * 0.5 * map_size.x, (b - a) * 0.5 * map_size.y)


func inside(m: Vector2) -> bool:
	return absf(m.x - SIZE * 0.5) + absf(m.y - SIZE * 0.5) <= SIZE * 0.5


func _has_point(p: Vector2) -> bool:
	return inside(p)


## The terrain picture: one pixel per cell from the terrain variations' colours (map_info
## cell_terrain + terrain_table colours), shaded by height, water and cliffs darker.
static func build_image(map: Dictionary, colors: Array) -> Image:
	var w := int(map.width)
	var h := int(map.height)
	var img := Image.create(w, h, false, Image.FORMAT_RGB8)
	var ids: PackedInt32Array = map.cell_terrain
	var kinds: PackedByteArray = map.cell_kind
	var heights: PackedFloat32Array = map.get("heights", PackedFloat32Array())
	var hw := int(map.get("height_width", 0))
	for y in h:
		for x in w:
			var i := y * w + x
			var t := ids[i]
			var c: Color = colors[t] if t >= 0 and t < colors.size() else Color(0.4, 0.5, 0.3)
			if hw > 1 and heights.size() >= hw * hw:
				var hx := mini(x / 2, hw - 2)
				var hy := mini(y / 2, hw - 2)
				var slope := heights[hy * hw + hx + 1] - heights[hy * hw + hx]
				c = c.lightened(clampf(slope * 0.15, 0.0, 0.25)) if slope > 0 else c.darkened(clampf(-slope * 0.15, 0.0, 0.25))
			match kinds[i]:  # nav::TerrainKind
				2:  # forest
					c = c.darkened(0.25)
				4, 5:  # shallow, water
					c = c.lerp(Color(0.15, 0.3, 0.55), 0.7)
				6:  # mountain
					c = c.darkened(0.3)
				7:  # border
					c = c.darkened(0.6)
			img.set_pixel(x, y, c)
	return img


func _draw() -> void:
	var top := to_mini(Vector2.ZERO)
	var right := to_mini(Vector2(map_size.x, 0))
	var bottom := to_mini(map_size)
	var left := to_mini(Vector2(0, map_size.y))
	var pts := PackedVector2Array([top, right, bottom, left])
	if map_tex != null:
		draw_colored_polygon(pts, Color.WHITE, PackedVector2Array([Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1)]), map_tex)
	else:
		draw_colored_polygon(pts, Color(0.25, 0.3, 0.2))
	for z in zones:
		var prev := to_mini(z.center + Vector2(z.radius, 0))
		for k in range(1, 33):
			var p := to_mini(z.center + Vector2.from_angle(TAU * k / 32) * z.radius)
			draw_line(prev, p, z.color, 1.0)
			prev = p
	# Plain shapes first, then the icons: each switch between them is another draw call.
	for b in buildings:
		if b.city and city_icon != null:
			continue
		var m := to_mini(b.pos)
		var s := clampf(float(b.size) * 0.5, 3.0, 6.0)
		draw_rect(Rect2(m - Vector2(s, s) * 0.5 - Vector2.ONE, Vector2(s, s) + Vector2(2, 2)), Color.BLACK)
		draw_rect(Rect2(m - Vector2(s, s) * 0.5, Vector2(s, s)), _color(int(b.player)))
	for i in dots.size():
		draw_rect(Rect2(to_mini(dots[i]) - Vector2(1.5, 1.5), Vector2(3, 3)), dot_colors[i])
	for b in buildings:
		if b.city and city_icon != null:
			draw_texture_rect(city_icon, Rect2(to_mini(b.pos) - Vector2(6, 6), Vector2(12, 12)), false,
				_color(int(b.player)) if int(b.player) != 255 else Color.WHITE)
	if view_quad.size() == 4:
		var q := PackedVector2Array()
		for p in view_quad:
			q.append(to_mini(p))
		q.append(q[0])
		draw_polyline(q, Color.WHITE, 1.0)


static func _color(player: int) -> Color:
	if player == 255:
		return NEUTRAL
	return UnitViews.PLAYER_COLORS[player % UnitViews.PLAYER_COLORS.size()]


func _gui_input(ev: InputEvent) -> void:
	if ev is InputEventMouseButton:
		var mb := ev as InputEventMouseButton
		if not inside(mb.position):
			return
		if mb.button_index == MOUSE_BUTTON_LEFT:
			_dragging = mb.pressed
			if mb.pressed:
				clicked.emit(to_map(mb.position).clamp(Vector2.ZERO, map_size), MOUSE_BUTTON_LEFT)
			accept_event()
		elif mb.button_index == MOUSE_BUTTON_RIGHT and mb.pressed:
			clicked.emit(to_map(mb.position).clamp(Vector2.ZERO, map_size), MOUSE_BUTTON_RIGHT)
			accept_event()
	elif ev is InputEventMouseMotion and _dragging:
		var m := (ev as InputEventMouseMotion).position
		clicked.emit(to_map(m).clamp(Vector2.ZERO, map_size), MOUSE_BUTTON_LEFT)
		accept_event()
