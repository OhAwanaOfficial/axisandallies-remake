class_name UnitBatches
extends Node3D
## Low and Medium quality units: one MultiMesh per unit def instead of one node tree per unit, so
## a unit type costs one draw call per material however many units it has (ARCHITECTURE §4.4).
##
## Each def's model is merged into one mesh in unit space (UnitViews.MODEL_YAW applied), surfaces
## grouped by material. Skinned models get their move (Walk/Jog) and idle (Idle/Fidget1) clips
## baked into a vertex animation texture: a row per frame, a texel per vertex (position in RGB,
## octahedral normal packed into A), skinned on the CPU once at load. unit_vat.gdshader plays it from TIME and each
## instance's custom data (moving, phase), so animation costs no CPU per frame. Instance buffers
## come from SimBridge.unit_instances(), which interpolates between ticks in C++.
##
## Each batch rotates through BUFFERS MultiMeshes, showing the one written this frame: rewriting a
## buffer the GPU may still be drawing from makes the driver wait for it (ANGLE on an Intel HD
## 3000 lost ~10 ms a frame to that with 500 units).

const VAT_FPS := 15.0
const VAT_MAX_FRAMES := 40       # per clip; longer clips are sampled more coarsely
const VAT_MAX_WIDTH := 8192      # texture width limit on Direct3D 10.1-class GPUs (ANGLE)
const MOVE_CLIPS := ["Walk", "Jog"]
const IDLE_CLIPS := ["Idle", "Fidget1"]
const BUFFERS := 3

var models: UnitModels
var bridge: SimBridge
var animate := true              # false: static rest pose, no vertex animation (profiling)
var vertex_lighting := false     # Low quality: light per vertex instead of per pixel
var anim_lod_distance := 120.0   # beyond this camera distance animation steps frame by frame
var batches := {}                # unit def index -> {units: [MultiMeshInstance3D], rings: [...], vat, placeholder}
var stats := {"units": 0, "placeholders": 0, "batches": 0, "bake_ms": 0}
var _selected := PackedInt64Array()
var _bounds := AABB()
var _shaders := {}               # cull mode -> Shader
var _arrays := {}                # "mesh id/surface" -> surface arrays


func setup(p_models: UnitModels, p_bridge: SimBridge, terrain: BattleTerrain) -> void:
	models = p_models
	bridge = p_bridge
	bridge.set_view_heights(terrain.heights, terrain.hw, BattleTerrain.TILE)
	_bounds = AABB(Vector3(-16, -50, -16), Vector3(terrain.cells.x + 32, 150, terrain.cells.y + 32))


func sync(_snap: Dictionary, alpha: float) -> void:
	var inst: Dictionary = bridge.unit_instances(alpha, _selected)
	var total := 0
	var boxes := 0
	for def in inst:
		var d: Dictionary = inst[def]
		var b := _batch(def)
		_fill(b.units, d.count, d.units)
		_fill(b.rings, d.ring_count, d.rings)
		total += d.count
		if b.placeholder:
			boxes += d.count
	for def in batches:
		if not inst.has(def):
			_fill(batches[def].units, 0, PackedFloat32Array())
			_fill(batches[def].rings, 0, PackedFloat32Array())
	stats.units = total
	stats.placeholders = boxes
	_syncs += 1


func set_selected(selected_orgs: Dictionary) -> void:
	_selected = PackedInt64Array(selected_orgs.keys())


var _syncs := 0


## Writes the instances into this frame's MultiMesh of `ring` and shows only that one.
func _fill(ring: Array, count: int, buf: PackedFloat32Array) -> void:
	var k := _syncs % BUFFERS
	for i in ring.size():
		var mmi: MultiMeshInstance3D = ring[i]
		mmi.visible = i == k and count > 0
	var mm: MultiMesh = (ring[k] as MultiMeshInstance3D).multimesh
	if mm.instance_count != count:
		mm.instance_count = count
	if count > 0:
		mm.buffer = buf


# ---------------------------------------------------------------- batches

func _batch(def: int) -> Dictionary:
	if batches.has(def):
		return batches[def]
	var t0 := Time.get_ticks_msec()
	var proto: Dictionary = models.proto(def) if def >= 0 else {"radius": 0.5}
	var built := _build_mesh(proto) if proto.has("root") else {}
	var placeholder: bool = built.is_empty()
	var mesh: Mesh = _placeholder_mesh(proto.radius) if placeholder else built.mesh
	var b := {
		"units": _multimesh(mesh, "Units_%d" % def, true),
		"rings": _multimesh(_ring_mesh(proto.radius), "Rings_%d" % def, false),
		"vat": built.get("vat", false),
		"placeholder": placeholder,
	}
	batches[def] = b
	stats.batches = batches.size()
	stats.bake_ms += Time.get_ticks_msec() - t0
	return b


func _multimesh(mesh: Mesh, node_name: String, shadows: bool) -> Array:
	var ring: Array = []
	for i in BUFFERS:
		var mm := MultiMesh.new()
		mm.transform_format = MultiMesh.TRANSFORM_3D
		mm.use_custom_data = true
		mm.mesh = mesh
		if "custom_aabb" in mm:
			mm.set("custom_aabb", _bounds)
		var mmi := MultiMeshInstance3D.new()
		mmi.name = "%s_%d" % [node_name, i]
		mmi.multimesh = mm
		mmi.custom_aabb = _bounds
		mmi.visible = false
		if not shadows:
			mmi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		add_child(mmi)
		ring.append(mmi)
	return ring


## The model merged into one ArrayMesh in unit space; {} if it has no visible mesh.
func _build_mesh(proto: Dictionary) -> Dictionary:
	var anims: Dictionary = proto.anims
	var clips: Array = []
	if animate:
		for n in [UnitModels.pick_clip(anims, MOVE_CLIPS), UnitModels.pick_clip(anims, IDLE_CLIPS)]:
			if n != "":
				clips.append({"name": n})
	var rig := _rig(proto)
	var parts := _parts(rig)
	if parts.is_empty():
		rig.free()
		return {}
	var skeletal := parts.any(func(p: Dictionary) -> bool: return p.skel != null)
	if not skeletal:
		clips.clear()
	# Frames: [positions, normals] per sampled pose, every vertex of every part in order.
	var frames: Array = []
	for c in clips:
		var a: Animation = anims[c.name]
		var length := maxf(a.length, 0.05)
		var n := clampi(ceili(length * VAT_FPS), 2, VAT_MAX_FRAMES)
		c.row = frames.size()
		c.frames = n
		c.fps = n / length
		var r := _rig(proto)
		var rp := _parts(r)
		for k in n:
			_apply_clip(r, a, length * k / n)
			frames.append(_pose(r, rp))
		r.free()
	var rest: Array = frames[0] if not frames.is_empty() else _pose(rig, parts)
	var vert_count: int = (rest[0] as PackedVector3Array).size()
	var vat := not frames.is_empty() and vert_count <= VAT_MAX_WIDTH
	var vat_tex: ImageTexture = _vat_texture(frames, vert_count) if vat else null
	var clip_move: Vector4 = _clip_vec(clips[0]) if vat else Vector4.ZERO
	var clip_idle: Vector4 = _clip_vec(clips[clips.size() - 1]) if vat else Vector4.ZERO  # one clip: both use it

	# Surfaces grouped by material.
	var groups := {}  # material key -> {material, verts, normals, uvs, uv2, indices}
	var base := 0
	for p in parts:
		var mi: MeshInstance3D = p.mi
		for s in mi.mesh.get_surface_count():
			var arr := _surface(mi.mesh, s)
			var nv: int = (arr[Mesh.ARRAY_VERTEX] as PackedVector3Array).size()
			var mat := mi.get_active_material(s)
			var key := _material_key(mat)
			if not groups.has(key):
				groups[key] = {"material": mat, "verts": PackedVector3Array(), "normals": PackedVector3Array(),
					"uvs": PackedVector2Array(), "uv2": PackedVector2Array(), "indices": PackedInt32Array()}
			var g: Dictionary = groups[key]
			var gv: PackedVector3Array = g.verts
			var gn: PackedVector3Array = g.normals
			var guv: PackedVector2Array = g.uvs
			var guv2: PackedVector2Array = g.uv2
			var gi: PackedInt32Array = g.indices
			var first := gv.size()
			for k in nv:
				gv.append(rest[0][base + k])
				gn.append(rest[1][base + k])
				guv2.append(Vector2(base + k, 0))
			var uv = arr[Mesh.ARRAY_TEX_UV]
			if uv is PackedVector2Array and (uv as PackedVector2Array).size() == nv:
				guv.append_array(uv)
			else:
				for k in nv:
					guv.append(Vector2.ZERO)
			var idx = arr[Mesh.ARRAY_INDEX]
			if idx is PackedInt32Array and not (idx as PackedInt32Array).is_empty():
				for i in idx:
					gi.append(first + i)
			else:
				for k in nv:
					gi.append(first + k)
			g.verts = gv
			g.normals = gn
			g.uvs = guv
			g.uv2 = guv2
			g.indices = gi
			base += nv
	rig.free()

	var mesh := ArrayMesh.new()
	for key in groups:
		var g: Dictionary = groups[key]
		var arrays := []
		arrays.resize(Mesh.ARRAY_MAX)
		arrays[Mesh.ARRAY_VERTEX] = g.verts
		arrays[Mesh.ARRAY_NORMAL] = g.normals
		arrays[Mesh.ARRAY_TEX_UV] = g.uvs
		arrays[Mesh.ARRAY_TEX_UV2] = g.uv2
		arrays[Mesh.ARRAY_INDEX] = g.indices
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
		var mat: Material = g.material
		if vat:
			mat = _vat_material(g.material, vat_tex, clip_move, clip_idle)
		elif vertex_lighting and mat is BaseMaterial3D:
			mat = mat.duplicate()
			(mat as BaseMaterial3D).shading_mode = BaseMaterial3D.SHADING_MODE_PER_VERTEX
		mesh.surface_set_material(mesh.get_surface_count() - 1, mat)
	# Animated vertices leave the rest pose: bound every frame.
	var box := AABB(rest[0][0], Vector3.ZERO)
	for f in (frames if vat else [rest]):
		for v in f[0]:
			box = box.expand(v)
	mesh.custom_aabb = box.grow(0.1)
	return {"mesh": mesh, "vat": vat}


func _rig(proto: Dictionary) -> Node3D:
	var rig: Node3D = (proto.root as Node3D).duplicate()
	rig.rotation.y = UnitViews.MODEL_YAW
	return rig


## Visible mesh parts: {mi, skel, skinned, binds (skin bind -> bone), attach, bone}.
func _parts(rig: Node3D) -> Array:
	var out: Array = []
	for n in rig.find_children("*", "MeshInstance3D", true, false):
		var mi := n as MeshInstance3D
		if mi.mesh == null or not _shown(mi, rig):
			continue
		var p := {"mi": mi, "skel": null, "skinned": false, "binds": PackedInt32Array(), "attach": null, "bone": -1}
		if mi.skin != null:
			var sk := mi.get_node_or_null(mi.skeleton) as Skeleton3D
			if sk != null:
				p.skel = sk
				p.skinned = true
				var binds := PackedInt32Array()
				for i in mi.skin.get_bind_count():
					var bname := mi.skin.get_bind_name(i)
					var bone := sk.find_bone(bname) if bname != &"" else mi.skin.get_bind_bone(i)
					binds.append(maxi(bone, 0))
				p.binds = binds
		else:
			var up := mi.get_parent()
			while up != null and up != rig:
				if up is BoneAttachment3D and up.get_parent() is Skeleton3D:
					var ba := up as BoneAttachment3D
					var sk := ba.get_parent() as Skeleton3D
					p.attach = ba
					p.skel = sk
					p.bone = ba.bone_idx if ba.bone_idx >= 0 else sk.find_bone(ba.bone_name)
					if p.bone < 0:
						p.attach = null
						p.skel = null
					break
				up = up.get_parent()
		out.append(p)
	return out


func _shown(n: Node, top: Node) -> bool:
	while n != null and n != top:
		if n is Node3D and not (n as Node3D).visible:
			return false
		n = n.get_parent()
	return true


## Every part's vertices (unit space) in the rig's current pose: [positions, normals].
func _pose(rig: Node3D, parts: Array) -> Array:
	var pos := PackedVector3Array()
	var nrm := PackedVector3Array()
	var unit_xf := rig.transform
	var globals := {}  # Skeleton3D -> bone global poses
	for p in parts:
		var mi: MeshInstance3D = p.mi
		var sk: Skeleton3D = p.skel
		var bones: Array = []
		if sk != null:
			if not globals.has(sk):
				globals[sk] = _bone_globals(sk)
			bones = globals[sk]
		var xf: Transform3D
		var mats: Array[Transform3D] = []
		if p.skinned:
			xf = unit_xf * _chain(rig, sk)
			for i in (p.binds as PackedInt32Array).size():
				mats.append(bones[p.binds[i]] * mi.skin.get_bind_pose(i))
		elif p.attach != null:
			xf = unit_xf * _chain(rig, sk) * bones[p.bone] * _chain(p.attach, mi)
		else:
			xf = unit_xf * _chain(rig, mi)
		for s in mi.mesh.get_surface_count():
			var arr := _surface(mi.mesh, s)
			var verts: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
			var normals = arr[Mesh.ARRAY_NORMAL]
			var has_n: bool = normals is PackedVector3Array and (normals as PackedVector3Array).size() == verts.size()
			if p.skinned and arr[Mesh.ARRAY_BONES] != null and arr[Mesh.ARRAY_WEIGHTS] != null:
				var bi: PackedInt32Array = arr[Mesh.ARRAY_BONES]
				var bw: PackedFloat32Array = arr[Mesh.ARRAY_WEIGHTS]
				var stride := bi.size() / maxi(verts.size(), 1)
				for k in verts.size():
					var pp := Vector3.ZERO
					var np := Vector3.ZERO
					var nk: Vector3 = normals[k] if has_n else Vector3.UP
					for j in stride:
						var w := bw[k * stride + j]
						if w <= 0.0:
							continue
						var m: Transform3D = mats[mini(bi[k * stride + j], mats.size() - 1)]
						pp += (m * verts[k]) * w
						np += (m.basis * nk) * w
					pos.append(xf * pp)
					nrm.append((xf.basis * np).normalized())
			else:
				for k in verts.size():
					pos.append(xf * verts[k])
					nrm.append((xf.basis * (normals[k] if has_n else Vector3.UP)).normalized())
	return [pos, nrm]


## Product of the local transforms from just below `top` down to `node`.
func _chain(top: Node, node: Node) -> Transform3D:
	var xf := Transform3D.IDENTITY
	var n := node
	while n != null and n != top:
		if n is Node3D:
			xf = (n as Node3D).transform * xf
		n = n.get_parent()
	return xf


func _bone_globals(sk: Skeleton3D) -> Array:
	var g: Array = []
	g.resize(sk.get_bone_count())
	for b in g.size():
		_bone_global(sk, b, g)
	return g


func _bone_global(sk: Skeleton3D, b: int, g: Array) -> Transform3D:
	if g[b] != null:
		return g[b]
	var parent := sk.get_bone_parent(b)
	var local := sk.get_bone_pose(b)
	g[b] = local if parent < 0 else _bone_global(sk, parent, g) * local
	return g[b]


## Poses the rig at time `t` of `anim` (track paths relative to the model root).
func _apply_clip(rig: Node3D, anim: Animation, t: float) -> void:
	for tr in anim.get_track_count():
		if not anim.track_is_enabled(tr):
			continue
		var path := anim.track_get_path(tr)
		var node := rig.get_node_or_null(NodePath(str(path.get_concatenated_names())))
		if node == null:
			continue
		var sub := str(path.get_concatenated_subnames())
		var sk := node as Skeleton3D
		var bone := sk.find_bone(sub) if sk != null and sub != "" else -1
		match anim.track_get_type(tr):
			Animation.TYPE_POSITION_3D:
				var v := anim.position_track_interpolate(tr, t)
				if bone >= 0:
					sk.set_bone_pose_position(bone, v)
				elif node is Node3D and sub == "":
					(node as Node3D).position = v
			Animation.TYPE_ROTATION_3D:
				var q := anim.rotation_track_interpolate(tr, t)
				if bone >= 0:
					sk.set_bone_pose_rotation(bone, q)
				elif node is Node3D and sub == "":
					(node as Node3D).quaternion = q
			Animation.TYPE_SCALE_3D:
				var v := anim.scale_track_interpolate(tr, t)
				if bone >= 0:
					sk.set_bone_pose_scale(bone, v)
				elif node is Node3D and sub == "":
					(node as Node3D).scale = v
			Animation.TYPE_VALUE:
				if sub != "":
					node.set_indexed(NodePath(sub), anim.value_track_interpolate(tr, t))


func _surface(mesh: Mesh, s: int) -> Array:
	var key := "%d/%d" % [mesh.get_instance_id(), s]
	if not _arrays.has(key):
		_arrays[key] = mesh.surface_get_arrays(s)
	return _arrays[key]


func _material_key(mat: Material) -> String:
	if mat is StandardMaterial3D:
		var sm := mat as StandardMaterial3D
		return "std %s %s %d %d %d" % [sm.albedo_texture.get_rid().get_id() if sm.albedo_texture else 0, sm.albedo_color,
			sm.cull_mode, sm.transparency, sm.shading_mode]
	return "mat %d" % (mat.get_instance_id() if mat else 0)


func _clip_vec(c: Dictionary) -> Vector4:
	return Vector4(c.row, c.frames, c.fps, 0.0)


func _vat_texture(frames: Array, verts: int) -> ImageTexture:
	var data := PackedFloat32Array()
	data.resize(verts * frames.size() * 4)
	for f in frames.size():
		var pos: PackedVector3Array = frames[f][0]
		var nrm: PackedVector3Array = frames[f][1]
		for k in verts:
			var o := (f * verts + k) * 4
			data[o] = pos[k].x
			data[o + 1] = pos[k].y
			data[o + 2] = pos[k].z
			data[o + 3] = _pack_normal(nrm[k])
	var img := Image.create_from_data(verts, frames.size(), false, Image.FORMAT_RGBAF, data.to_byte_array())
	return ImageTexture.create_from_image(img)


## Octahedral normal in two 12-bit halves of one float (exact: below 2^24); unit_vat.gdshader
## unpacks it. One texel per vertex halves the vertex texture fetches.
static func _pack_normal(n: Vector3) -> float:
	var a := absf(n.x) + absf(n.y) + absf(n.z)
	var p := Vector2(n.x, n.z) / maxf(a, 1e-6)
	if n.y < 0.0:
		p = Vector2((1.0 - absf(p.y)) * (1.0 if p.x >= 0.0 else -1.0), (1.0 - absf(p.x)) * (1.0 if p.y >= 0.0 else -1.0))
	var u := clampi(roundi((p.x * 0.5 + 0.5) * 4095.0), 0, 4095)
	var v := clampi(roundi((p.y * 0.5 + 0.5) * 4095.0), 0, 4095)
	return float(u * 4096 + v)


func _vat_material(orig: Material, vat: ImageTexture, clip_move: Vector4, clip_idle: Vector4) -> ShaderMaterial:
	var sm := orig as StandardMaterial3D
	var cull := sm.cull_mode if sm else BaseMaterial3D.CULL_BACK
	var m := ShaderMaterial.new()
	m.shader = _vat_shader(cull)
	m.set_shader_parameter("albedo", sm.albedo_color if sm else Color.WHITE)
	m.set_shader_parameter("use_tex", sm != null and sm.albedo_texture != null)
	if sm and sm.albedo_texture:
		m.set_shader_parameter("albedo_tex", sm.albedo_texture)
	m.set_shader_parameter("vat", vat)
	m.set_shader_parameter("clip_move", clip_move)
	m.set_shader_parameter("clip_idle", clip_idle)
	m.set_shader_parameter("anim_lod_distance", anim_lod_distance)
	return m


func _vat_shader(cull: int) -> Shader:
	if not _shaders.has(cull):
		var base: Shader = load("res://scripts/battle/unit_vat.gdshader")
		var mode: String = {BaseMaterial3D.CULL_BACK: "cull_back", BaseMaterial3D.CULL_FRONT: "cull_front",
			BaseMaterial3D.CULL_DISABLED: "cull_disabled"}.get(cull, "cull_back")
		if vertex_lighting:
			mode += ", vertex_lighting"
		if mode == "cull_back":
			_shaders[cull] = base
		else:
			var s := Shader.new()
			s.code = base.code.replace("render_mode cull_back;", "render_mode %s;" % mode)
			_shaders[cull] = s
	return _shaders[cull]


func _player_material(channel: int, lighten: float) -> ShaderMaterial:
	var m := ShaderMaterial.new()
	m.shader = load("res://scripts/battle/player_color.gdshader")
	var colors := PackedColorArray()
	for c in UnitViews.PLAYER_COLORS:
		colors.append((c as Color).lightened(lighten))
	m.set_shader_parameter("colors", colors)
	m.set_shader_parameter("channel", channel)
	return m


func _placeholder_mesh(radius: float) -> Mesh:
	var box := BoxMesh.new()
	box.size = Vector3(radius * 2.2, radius * 1.6, radius * 1.6)
	var arr := box.get_mesh_arrays()
	var verts: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
	for i in verts.size():
		verts[i].y += radius * 0.8
	arr[Mesh.ARRAY_VERTEX] = verts
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
	mesh.surface_set_material(0, _player_material(2, 0.0))
	return mesh


func _ring_mesh(radius: float) -> Mesh:
	var torus := TorusMesh.new()
	torus.inner_radius = radius + 0.15
	torus.outer_radius = radius + 0.35
	torus.rings = 24
	torus.ring_segments = 4
	torus.material = _player_material(0, 0.4)
	return torus
