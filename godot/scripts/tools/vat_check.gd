extends SceneTree
## Checks UnitBatches' CPU skinning (the vertex animation bake) against Godot's own skinning
## (AnimationPlayer + MeshInstance3D.bake_mesh_from_current_skeleton_pose) for one model and clip.
## Skinned parts only. Needs a renderer (not --headless): skins bind through the RenderingServer.
## godot --path godot --script res://scripts/tools/vat_check.gd -- Units/PER/PERInfantry/PERInfantry.glb Walk

const SAMPLES := 6

var _frames := 0
var _s := {}
var _worst := 0.0
var _count := 0


## Frame 1 builds the reference; then each sample seeks on one frame and compares on the next,
## so the skeleton and skin have settled.
func _process(_delta: float) -> bool:
	_frames += 1
	if _frames == 1:
		_prepare()
		return false
	if _s.is_empty() or _frames < 3:
		return false
	var k := (_frames - 3) / 2
	if k >= SAMPLES:
		_finish()
		return false
	var t: float = _s.anim.length * k / SAMPLES
	if (_frames - 3) % 2 == 0:
		(_s.ap as AnimationPlayer).seek(t, true)
	else:
		_compare(t)
	return false


func _prepare() -> void:
	var args := OS.get_cmdline_user_args()
	var rel := args[0] if args.size() > 0 else "Units/PER/PERInfantry/PERInfantry.glb"
	var clip := args[1] if args.size() > 1 else "Walk"
	var loader := CacheLoader.new()
	loader.load_manifest()
	var r: Dictionary = loader.load_model(rel)
	if r.has("error"):
		print("FAIL ", r.error)
		quit(1)
		return
	var root: Node3D = r.root
	for ap in root.find_children("*", "AnimationPlayer", true, false):
		ap.free()
	var anim: Animation = null
	for clip_glb in loader.manifest.get("compatible_clips", {}).get(rel, []):
		if str(clip_glb).get_file().trim_suffix(".anim.glb") == clip:
			var anims: Dictionary = loader.load_animations(clip_glb)
			for an in anims:
				var rt: Array = CacheLoader.retarget(anims[an], root, root)
				if rt[1] > 0:
					anim = rt[0]
					break
	if anim == null:
		print("FAIL no clip ", clip)
		quit(1)
		return
	var ref: Node3D = root.duplicate()
	ref.rotation.y = UnitViews.MODEL_YAW
	get_root().add_child(ref)
	var ap := AnimationPlayer.new()
	ref.add_child(ap)
	ap.root_node = ap.get_path_to(ref)
	var lib := AnimationLibrary.new()
	lib.add_animation(clip, anim)
	ap.add_animation_library("", lib)
	ap.play(clip)
	ap.pause()
	_s = {"root": root, "anim": anim, "clip": clip, "rel": rel, "ref": ref, "ap": ap, "batches": UnitBatches.new(),
		"proto": {"root": root, "anims": {clip: anim}}}


func _compare(t: float) -> void:
	var batches: UnitBatches = _s.batches
	var ref: Node3D = _s.ref
	var rig := batches._rig(_s.proto)
	var parts := batches._parts(rig)
	batches._apply_clip(rig, _s.anim, t)
	var pose: Array = batches._pose(rig, parts)
	var base := 0
	var worst := 0.0
	for p in parts:
		var ref_mi := ref.get_node(rig.get_path_to(p.mi)) as MeshInstance3D
		var n := 0
		for s in ref_mi.mesh.get_surface_count():
			n += (ref_mi.mesh.surface_get_arrays(s)[Mesh.ARRAY_VERTEX] as PackedVector3Array).size()
		if ref_mi.skin != null:
			var xf := ref.transform * ref.global_transform.affine_inverse() * ref_mi.global_transform
			var baked := ref_mi.bake_mesh_from_current_skeleton_pose()
			var verts := PackedVector3Array()
			for s in baked.get_surface_count():
				verts.append_array(baked.surface_get_arrays(s)[Mesh.ARRAY_VERTEX])
			for i in verts.size():
				worst = maxf(worst, (pose[0][base + i] as Vector3).distance_to(xf * verts[i]))
				_count += 1
		base += n
	rig.free()
	print("  t=%.2f worst %.4f" % [t, worst])
	_worst = maxf(_worst, worst)


func _finish() -> void:
	var ok := _worst < 0.01
	print("%s %s: %d vertex samples, worst difference %.4f units -> %s" % [str(_s.rel).get_file(), _s.clip, _count,
		_worst, "PASS" if ok else "FAIL"])
	(_s.ref as Node).free()
	(_s.root as Node).free()
	(_s.batches as Node).free()
	_s = {}
	quit(0 if ok else 1)
