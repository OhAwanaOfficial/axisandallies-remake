extends Node3D
## Repro attempt (no GDExtension, no assets) for the GL Compatibility / ANGLE (D3D11) crash in
## RasterizerCanvasGLES3::_render_items: memcpy into the pointer glMapBufferRange returned for a
## canvas instance buffer, when the GPU runs far behind the CPU while 2D text changes.
##
## Loads, each optional: --count=N skinned animated characters (Skeleton3D + Skin +
## AnimationPlayer, --parts=K meshes each; GLES3 skins them with transform feedback every frame),
## a full-screen quad with --gpu-loops=N of fragment work (GPU-bound frames), and a multi-line
## outlined Label changing every frame. Prints "REPRO ok" if it survives --seconds.
##
## Options (after --): --count=N (0), --parts=K (3), --bones=B (12), --gpu-loops=N (0),
##   --seconds=S (90), --no-label, --static-label, --drain (read back a 1x1 SubViewport every
##   frame: the GPU never runs behind).

var label: Label
var frames := 0
var t0 := 0
var args := {}
var probe: SubViewport


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		var kv := a.trim_prefix("--").split("=", true, 1)
		args[kv[0]] = kv[1] if kv.size() > 1 else "1"
	var count := int(args.get("count", "0"))
	var parts := int(args.get("parts", "3"))
	var bones := int(args.get("bones", "12"))
	var loops := int(args.get("gpu-loops", "0"))
	var sun := DirectionalLight3D.new()
	sun.rotation = Vector3(-0.9, 0.6, 0)
	add_child(sun)
	var cam := Camera3D.new()
	add_child(cam)
	cam.look_at_from_position(Vector3(0, 40, 50), Vector3.ZERO)
	if loops > 0:
		_add_gpu_load(loops)
	if count > 0:
		var proto := _character(parts, bones)
		var anim := _animation(bones)
		var side := ceili(sqrt(count))
		for i in count:
			var c := proto.duplicate() as Node3D
			c.position = Vector3((i % side - side * 0.5) * 2.2, 0, (i / side - side * 0.5) * 2.2)
			add_child(c)
			var ap := AnimationPlayer.new()
			c.add_child(ap)
			ap.root_node = ap.get_path_to(c)
			var lib := AnimationLibrary.new()
			lib.add_animation("walk", anim)
			ap.add_animation_library("", lib)
			ap.play("walk")
			ap.seek(randf() * anim.length, true)
	if not args.has("no-label"):
		var layer := CanvasLayer.new()
		add_child(layer)
		label = Label.new()
		label.position = Vector2(10, 10)
		label.add_theme_constant_override("outline_size", 4)
		label.text = "static label"
		layer.add_child(label)
	if args.has("drain"):
		probe = SubViewport.new()
		probe.size = Vector2i(1, 1)
		probe.render_target_update_mode = SubViewport.UPDATE_ALWAYS
		add_child(probe)
	t0 = Time.get_ticks_msec()
	print("REPRO start: %d characters x %d parts, gpu loops %d, renderer %s, driver %s, adapter %s" % [count, parts, loops,
		RenderingServer.get_current_rendering_method(), RenderingServer.get_current_rendering_driver_name(),
		RenderingServer.get_video_adapter_name()])


func _process(_delta: float) -> void:
	frames += 1
	var secs := (Time.get_ticks_msec() - t0) / 1000.0
	if probe != null:
		probe.get_texture().get_image()  # waits for the GPU to finish everything queued so far
	if label != null and not args.has("static-label"):
		var lines: Array[String] = []
		for k in 8:
			lines.append("frame %d  line %d  %.2f s  %d fps  %d" % [frames, k, secs, Engine.get_frames_per_second(), (frames * 7919 + k) % 100000])
		label.text = "\n".join(lines)
	if secs > float(args.get("seconds", "90")):
		print("REPRO ok: %d frames in %.1f s (%.1f fps)" % [frames, secs, frames / secs])
		get_tree().quit()


func _add_gpu_load(loops: int) -> void:
	var quad := MeshInstance3D.new()
	var qm := QuadMesh.new()
	qm.size = Vector2(2, 2)
	quad.mesh = qm
	var sh := Shader.new()
	sh.code = """
shader_type spatial;
render_mode unshaded, cull_disabled, skip_vertex_transform, depth_draw_never, depth_test_disabled;
uniform int loops = 400;
void vertex() { POSITION = vec4(VERTEX.xy, 0.999, 1.0); }
void fragment() {
	vec2 p = FRAGCOORD.xy * 0.01;
	float v = 0.0;
	for (int i = 0; i < loops; i++) { v += sin(p.x * float(i) * 0.37 + TIME) * cos(p.y * float(i) * 0.21); }
	ALBEDO = vec3(0.2 + 0.001 * v, 0.3, 0.4);
}
"""
	var mat := ShaderMaterial.new()
	mat.shader = sh
	mat.set_shader_parameter("loops", loops)
	mat.render_priority = -100
	quad.material_override = mat
	quad.extra_cull_margin = 16384
	add_child(quad)


## A character: a Skeleton3D chain of `bones` bones and `parts` skinned cylinder meshes along it.
func _character(parts: int, bones: int) -> Node3D:
	var root := Node3D.new()
	var sk := Skeleton3D.new()
	sk.name = "Skeleton"
	root.add_child(sk)
	var seg := 2.0 / bones
	for b in bones:
		sk.add_bone("b%d" % b)
		if b > 0:
			sk.set_bone_parent(b, b - 1)
		sk.set_bone_rest(b, Transform3D(Basis(), Vector3(0, seg if b > 0 else 0.0, 0)))
		sk.set_bone_pose_position(b, Vector3(0, seg if b > 0 else 0.0, 0))
	var skin := Skin.new()
	for b in bones:
		skin.add_named_bind("b%d" % b, Transform3D(Basis(), Vector3(0, -seg * b, 0)))
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.5, 0.45, 0.35)
	for p in parts:
		var mi := MeshInstance3D.new()
		mi.mesh = _skinned_cylinder(0.12 + 0.05 * p, 2.0, bones, mat)
		mi.position.x = (p - parts * 0.5) * 0.15
		sk.add_child(mi)
		mi.skin = skin
		mi.skeleton = NodePath("..")
	return root


func _skinned_cylinder(radius: float, height: float, bones: int, mat: Material) -> ArrayMesh:
	var cyl := CylinderMesh.new()
	cyl.top_radius = radius
	cyl.bottom_radius = radius
	cyl.height = height
	cyl.radial_segments = 12
	cyl.rings = bones
	var arr := cyl.get_mesh_arrays()
	var verts: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
	var bi := PackedInt32Array()
	var bw := PackedFloat32Array()
	for v in verts.size():
		verts[v].y += height * 0.5
		var f := clampf(verts[v].y / height * bones, 0.0, bones - 1.001)
		var b0 := int(f)
		var w := f - b0
		bi.append_array([b0, mini(b0 + 1, bones - 1), 0, 0])
		bw.append_array([1.0 - w, w, 0.0, 0.0])
	arr[Mesh.ARRAY_VERTEX] = verts
	arr[Mesh.ARRAY_BONES] = bi
	arr[Mesh.ARRAY_WEIGHTS] = bw
	var m := ArrayMesh.new()
	m.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
	m.surface_set_material(0, mat)
	return m


## A 1-second loop swaying every bone.
func _animation(bones: int) -> Animation:
	var a := Animation.new()
	a.length = 1.0
	a.loop_mode = Animation.LOOP_LINEAR
	for b in bones:
		var t := a.add_track(Animation.TYPE_ROTATION_3D)
		a.track_set_path(t, NodePath("Skeleton:b%d" % b))
		for k in 5:
			var ang := sin(k / 4.0 * TAU + b * 0.3) * 0.25
			a.rotation_track_insert_key(t, k / 4.0, Quaternion(Vector3(0, 0, 1), ang))
	return a
