class_name BaseOverlays
extends Node3D
## Ground overlays for bases (M6 part 3):
##  * the placement ghost: the footprint a truck regiment would unpack into at the cursor, green
##    where SimBridge.placement() says it fits and red where it doesn't (with the reason), a box as
##    tall as the building plus an outline on the ground;
##  * the supply-zone overlay (Ctrl+S or the panel's supply-border button): a ring on the ground
##    round every zone of the player's team: green active, yellow besieged (still supplying),
##    grey switched off; with the enemy's zones in red when `show_enemy`.

const SEGMENTS := 64
const OK_COLOR := Color(0.25, 1.0, 0.35)
const BAD_COLOR := Color(1.0, 0.2, 0.15)

var terrain: BattleTerrain
var ghost: MeshInstance3D
var ghost_lines: MeshInstance3D
var rings: MeshInstance3D
var ghost_ok := false
var ghost_why := ""
var zone_count := 0


func setup(p_terrain: BattleTerrain) -> void:
	terrain = p_terrain
	ghost = MeshInstance3D.new()
	ghost.mesh = BoxMesh.new()
	ghost.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	var gm := StandardMaterial3D.new()
	gm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	gm.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	gm.albedo_color = Color(OK_COLOR, 0.3)
	gm.no_depth_test = false
	ghost.material_override = gm
	ghost.visible = false
	add_child(ghost)
	ghost_lines = _line_mesh()
	add_child(ghost_lines)
	rings = _line_mesh()
	add_child(rings)


func _line_mesh() -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.mesh = ImmediateMesh.new()
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.vertex_color_use_as_albedo = true
	m.no_depth_test = true
	mi.material_override = m
	return mi


## Shows the ghost for placement `pl` (SimBridge.placement) of a building `height` tall; hides it
## when `pl` is empty.
func show_ghost(pl: Dictionary, height: float) -> void:
	var im: ImmediateMesh = ghost_lines.mesh
	im.clear_surfaces()
	if pl.is_empty():
		ghost.visible = false
		return
	ghost_ok = bool(pl.ok)
	ghost_why = str(pl.get("why", ""))
	var r: Rect2i = pl.rect
	var center := Vector2(r.position) + Vector2(r.size) * 0.5
	var col := OK_COLOR if ghost_ok else BAD_COLOR
	(ghost.mesh as BoxMesh).size = Vector3(r.size.x, height, r.size.y)
	ghost.position = terrain.to_world(center, height * 0.5)
	(ghost.material_override as StandardMaterial3D).albedo_color = Color(col, 0.3)
	ghost.visible = true
	im.surface_begin(Mesh.PRIMITIVE_LINES)
	var a := Vector2(r.position)
	var corners := [a, a + Vector2(r.size.x, 0), a + Vector2(r.size), a + Vector2(0, r.size.y)]
	for i in 4:
		_seg(im, corners[i], corners[(i + 1) % 4], col)
	im.surface_end()


## Rings for `zones` (SimBridge.supply_zones) of `team`; none when `on` is false.
func show_zones(zones: Array, team: int, on: bool, show_enemy := false) -> void:
	var im: ImmediateMesh = rings.mesh
	im.clear_surfaces()
	zone_count = 0
	if not on:
		return
	var began := false
	for z in zones:
		var own := int(z.team) == team
		if not own and not show_enemy:
			continue
		var col: Color
		if not own:
			col = Color(1.0, 0.3, 0.25)
		elif not z.active:
			col = Color(0.6, 0.6, 0.6)
		elif z.besieged:
			col = Color(1.0, 0.85, 0.2)
		else:
			col = Color(0.3, 1.0, 0.4)
		if not began:
			im.surface_begin(Mesh.PRIMITIVE_LINES)
			began = true
		var c: Vector2 = z.center
		var r := float(z.radius)
		var prev := c + Vector2(r, 0)
		for k in range(1, SEGMENTS + 1):
			var p := c + Vector2.from_angle(TAU * k / SEGMENTS) * r
			_seg(im, prev, p, col)
			prev = p
		zone_count += 1
	if began:
		im.surface_end()


func _seg(im: ImmediateMesh, a: Vector2, b: Vector2, col: Color) -> void:
	var cells := Vector2(terrain.cells)
	a = a.clamp(Vector2.ZERO, cells - Vector2.ONE * 0.01)
	b = b.clamp(Vector2.ZERO, cells - Vector2.ONE * 0.01)
	im.surface_set_color(col)
	im.surface_add_vertex(terrain.to_world(a, 0.35))
	im.surface_set_color(col)
	im.surface_add_vertex(terrain.to_world(b, 0.35))
