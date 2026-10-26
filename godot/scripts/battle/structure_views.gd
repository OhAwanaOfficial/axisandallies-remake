class_name StructureViews
extends Node3D
## Buildings (M6 part 3): one node per sim Structure (Corps HQ, division HQs, depots, cities) with
## its imported model, or a box the size of its footprint when the cache has none. Unpacking and
## packing buildings are drawn translucent; a selected one gets a ring. Synced from
## SimBridge.structures() whenever the HUD refreshes (buildings don't move). The view never
## touches sim state.

var models: UnitModels
var terrain: BattleTerrain
var views := {}        # structure id -> {node, ids, state, ring}
var stats := {"buildings": 0, "placeholders": 0}


func setup(p_models: UnitModels, p_terrain: BattleTerrain) -> void:
	models = p_models
	terrain = p_terrain


func sync(list: Array, selected: int) -> void:
	var live := {}
	for s in list:
		var id: int = s.id
		live[id] = true
		var v: Dictionary = views.get(id, {})
		if v.is_empty():
			v = _create(s)
			views[id] = v
		if v.state != s.state:
			v.state = s.state
			_set_ghost(v.node, s.state != "deployed")
		(v.ring as Node3D).visible = id == selected
	for id in views.keys():
		if not live.has(id):
			(views[id].node as Node3D).queue_free()
			views.erase(id)
	stats.buildings = views.size()


func _create(s: Dictionary) -> Dictionary:
	var node := Node3D.new()
	node.name = "Building_%d" % s.id
	add_child(node)
	var r: Rect2i = s.get("rect", Rect2i(0, 0, 4, 4))
	var center := Vector2(r.position) + Vector2(r.size) * 0.5
	node.position = terrain.to_world(center)
	node.rotation.y = -float(s.facing)
	var proto := models.building_proto(str(s.ids))
	if proto.has("root"):
		var model: Node3D = (proto.root as Node3D).duplicate()
		model.rotation.y = UnitViews.MODEL_YAW
		node.add_child(model)
	else:
		var box := MeshInstance3D.new()
		var bm := BoxMesh.new()
		bm.size = Vector3(r.size.x, 3.0, r.size.y)
		box.mesh = bm
		box.position.y = 1.5
		box.rotation.y = float(s.facing)  # the rect is axis-aligned in the world
		var m := StandardMaterial3D.new()
		var p := int(s.player)
		m.albedo_color = Color(0.6, 0.6, 0.55) if p == 255 else UnitViews.PLAYER_COLORS[p % UnitViews.PLAYER_COLORS.size()].darkened(0.3)
		box.material_override = m
		node.add_child(box)
		stats.placeholders += 1
	var ring := MeshInstance3D.new()
	var tm := TorusMesh.new()
	var rad := maxf(r.size.x, r.size.y) * 0.75
	tm.inner_radius = rad - 0.25
	tm.outer_radius = rad
	tm.rings = 32
	ring.mesh = tm
	ring.position.y = 0.3
	var rm := StandardMaterial3D.new()
	rm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	rm.albedo_color = Color(0.4, 1.0, 0.4)
	ring.material_override = rm
	ring.visible = false
	node.add_child(ring)
	return {"node": node, "ids": s.ids, "state": "", "ring": ring}


## Unpacking / packing: drawn see-through.
func _set_ghost(node: Node3D, ghost: bool) -> void:
	for n in node.find_children("*", "GeometryInstance3D", true, false):
		var gi := n as GeometryInstance3D
		if gi is MeshInstance3D and (gi as MeshInstance3D).mesh is TorusMesh:
			continue
		gi.transparency = 0.55 if ghost else 0.0
