class_name UnitViews
extends Node3D
## One view per sim unit, built from the imported model of its UnitDef (resource_prefix → cache
## .glb) and moved by render interpolation between the snapshot's previous and current tick
## (ARCHITECTURE §4.4). The view never touches sim state.
##
## Facing: sim angle θ is counter-clockwise from +x towards +y; sim (x, y) is Godot (x, z), so a
## node faces the sim heading with rotation.y = -θ. Imported models face Godot -Z after the
## Z-up → Y-up conversion face Godot +Z (checked against the tank barrels), so MODEL_YAW turns
## them onto local +X.

const MODEL_YAW := PI / 2.0
const WALK_SPEED_SCALE := 1.0
const PLAYER_COLORS := [Color(0.25, 0.55, 1.0), Color(1.0, 0.3, 0.25), Color(0.3, 0.9, 0.35), Color(1.0, 0.85, 0.2)]

var loader: CacheLoader
var bridge: SimBridge
var terrain: BattleTerrain
var protos := {}          # unit def index -> {root, anims: {name: Animation}, model, error}
var views := {}           # entity bits -> {node, player, def, anim, ring, org}
var model_index := {}     # lower-case cache path -> cache path (for case-insensitive lookups)
var stats := {"units": 0, "placeholders": 0}


func setup(p_loader: CacheLoader, p_bridge: SimBridge, p_terrain: BattleTerrain) -> void:
	loader = p_loader
	bridge = p_bridge
	terrain = p_terrain
	for m in loader.manifest.get("models", []):
		model_index[str(m.glb).to_lower()] = m.glb


## Creates and removes views to match the snapshot, then places them at interpolation `alpha`.
func sync(snap: Dictionary, alpha: float) -> void:
	var ids: PackedInt64Array = snap.id
	var kinds: PackedByteArray = snap.kind
	var seen := {}
	for i in ids.size():
		if kinds[i] != 0:
			continue
		var bits := ids[i]
		seen[bits] = true
		var v: Dictionary = views.get(bits, {})
		if v.is_empty():
			v = _create_view(bits, snap.def[i], snap.player[i], snap.org[i])
			views[bits] = v
		var p := (snap.prev_pos[i] as Vector2).lerp(snap.cur_pos[i], alpha)
		var f := lerp_angle(snap.prev_facing[i], snap.cur_facing[i], alpha)
		var node: Node3D = v.node
		node.position = terrain.to_world(p)
		node.rotation.y = -f
		_animate(v, snap.moving[i] != 0)
	for bits in views.keys():
		if not seen.has(bits):
			(views[bits].node as Node3D).queue_free()
			views.erase(bits)
	stats.units = views.size()


func set_selected(selected_orgs: Dictionary) -> void:
	for bits in views:
		var v: Dictionary = views[bits]
		(v.ring as Node3D).visible = selected_orgs.has(v.org)


# ---------------------------------------------------------------- views

func _create_view(bits: int, def: int, player: int, org: int) -> Dictionary:
	var node := Node3D.new()
	node.name = "Unit_%d" % bits
	add_child(node)
	var proto := _proto(def)
	var model: Node3D = null
	var anim_player: AnimationPlayer = null
	if proto.has("root"):
		model = (proto.root as Node3D).duplicate()
		model.rotation.y = MODEL_YAW
		node.add_child(model)
		if not (proto.anims as Dictionary).is_empty():
			anim_player = AnimationPlayer.new()
			model.add_child(anim_player)
			anim_player.root_node = anim_player.get_path_to(model)
			var lib := AnimationLibrary.new()
			for n in proto.anims:
				lib.add_animation(n, proto.anims[n])
			anim_player.add_animation_library("", lib)
	else:
		model = _placeholder(proto.get("radius", 0.5), player)
		node.add_child(model)
		stats.placeholders += 1
	var ring := _ring(proto.get("radius", 0.5), player)
	ring.visible = false
	node.add_child(ring)
	return {"node": node, "player": player, "def": def, "org": org, "anim": anim_player, "ring": ring, "playing": ""}


func _animate(v: Dictionary, moving: bool) -> void:
	var ap: AnimationPlayer = v.anim
	if ap == null:
		return
	var want := ""
	for n in (["Walk", "Jog"] if moving else ["Idle", "Fidget1"]):
		if ap.has_animation(n):
			want = n
			break
	if want != "" and want != v.playing:
		ap.play(want, 0.2)
		v.playing = want


func _proto(def: int) -> Dictionary:
	if protos.has(def):
		return protos[def]
	var vis: Dictionary = bridge.unit_visual(def)
	var proto := {"radius": maxf(float(vis.get("radius", 0.5)), 0.3), "ids": vis.get("ids", "?")}
	protos[def] = proto
	var rel := _resolve_model(str(vis.get("model", "")))
	if rel == "":
		proto.error = "no cached model for %s (%s)" % [vis.get("ids", "?"), vis.get("model", "")]
		push_warning(proto.error)
		return proto
	var r: Dictionary = loader.load_model(rel)
	if r.has("error"):
		proto.error = r.error
		push_warning("%s: %s" % [rel, r.error])
		return proto
	var root: Node3D = r.root
	for n in root.find_children("*", "Node3D", true, false):
		var ex: Dictionary = n.get_meta("extras", {})
		if ex.get("hidden", false):
			(n as Node3D).visible = false
	for ap in root.find_children("*", "AnimationPlayer", true, false):
		ap.queue_free()
	proto.root = root
	proto.model = rel
	proto.anims = _load_clips(rel, root)
	return proto


func _resolve_model(rel: String) -> String:
	if rel == "":
		return ""
	var key := rel.to_lower()
	if model_index.has(key):
		return model_index[key]
	var tail := key.get_file()
	for k in model_index:
		if str(k).ends_with("/" + tail):
			return model_index[k]
	return ""


## Walk / Jog / Idle / Fidget1 clips compatible with the model, retargeted to it.
func _load_clips(rel: String, root: Node3D) -> Dictionary:
	var out := {}
	var compat: Dictionary = loader.manifest.get("compatible_clips", {})
	for clip_glb in compat.get(rel, []):
		var short := str(clip_glb).get_file().trim_suffix(".anim.glb")
		if short not in ["Walk", "Jog", "Idle", "Fidget1"] or out.has(short):
			continue
		var anims: Dictionary = loader.load_animations(clip_glb)
		for an in anims:
			var rt: Array = CacheLoader.retarget(anims[an], root, root)
			if rt[1] > 0:
				var a: Animation = rt[0]
				a.loop_mode = Animation.LOOP_LINEAR
				out[short] = a
				break
	return out


func _placeholder(radius: float, player: int) -> Node3D:
	var mi := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(radius * 2.2, radius * 1.6, radius * 1.6)
	mi.mesh = box
	mi.position.y = radius * 0.8
	var m := StandardMaterial3D.new()
	m.albedo_color = PLAYER_COLORS[player % PLAYER_COLORS.size()]
	mi.material_override = m
	return mi


func _ring(radius: float, player: int) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var torus := TorusMesh.new()
	torus.inner_radius = radius + 0.15
	torus.outer_radius = radius + 0.35
	torus.rings = 24
	torus.ring_segments = 4
	mi.mesh = torus
	mi.position.y = 0.08
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.albedo_color = PLAYER_COLORS[player % PLAYER_COLORS.size()].lightened(0.4)
	mi.material_override = m
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return mi
