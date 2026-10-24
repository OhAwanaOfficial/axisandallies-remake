class_name Banners
extends Node3D
## Regiment banners (GAME_DESIGN §6.7): over each regiment's units, its player's flag and bars for
## health (of the most it had), morale (of its maximum) and attack efficiency. Routing regiments'
## flags blink, entrenched ones show sandbags, selected ones a light border. One MultiMesh of
## constant-pixel-size quads (banner.gdshader), four instances per regiment, built in C++ by
## SimBridge.banner_instances() at the interpolated centroid of its living units. Same buffer
## rotation as UnitBatches.

const BUFFERS := 3
const LIFT := 3.2   # metres above the ground at the centroid

var bridge: SimBridge
var count := 0
var _rings: Array = []
var _turn := 0
var _selected := PackedInt64Array()
var _material: ShaderMaterial


func setup(p_bridge: SimBridge, terrain: BattleTerrain) -> void:
	bridge = p_bridge
	_material = ShaderMaterial.new()
	_material.shader = load("res://scripts/battle/banner.gdshader")
	var colors := PackedColorArray()
	for i in 8:
		colors.append(UnitViews.PLAYER_COLORS[i % UnitViews.PLAYER_COLORS.size()])
	_material.set_shader_parameter("player_colors", colors)
	_material.render_priority = 5
	var quad := QuadMesh.new()
	quad.size = Vector2(1, 1)
	quad.material = _material
	var bounds := AABB(Vector3(-16, -50, -16), Vector3(terrain.cells.x + 32, 150, terrain.cells.y + 32))
	for i in BUFFERS:
		var mm := MultiMesh.new()
		mm.transform_format = MultiMesh.TRANSFORM_3D
		mm.use_custom_data = true
		mm.mesh = quad
		var mmi := MultiMeshInstance3D.new()
		mmi.name = "Banners_%d" % i
		mmi.multimesh = mm
		mmi.custom_aabb = bounds
		mmi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		mmi.visible = false
		add_child(mmi)
		_rings.append(mmi)


func set_selected(selected_orgs: Dictionary) -> void:
	_selected = PackedInt64Array(selected_orgs.keys())


func sync(alpha: float) -> void:
	var buf: PackedFloat32Array = bridge.banner_instances(alpha, LIFT, _selected)
	count = buf.size() / 16
	_material.set_shader_parameter("viewport_height", get_viewport().get_visible_rect().size.y)
	_turn = (_turn + 1) % BUFFERS
	for i in BUFFERS:
		(_rings[i] as MultiMeshInstance3D).visible = i == _turn and count > 0
	var mm: MultiMesh = (_rings[_turn] as MultiMeshInstance3D).multimesh
	if mm.instance_count != count:
		mm.instance_count = count
	if count > 0:
		mm.buffer = buf
