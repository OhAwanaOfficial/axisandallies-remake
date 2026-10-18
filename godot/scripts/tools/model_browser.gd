extends Control
## Model Browser (docs/ASSET_PIPELINE.md §4): every imported unit and building from cache/, with
## its animation clips, attach points (AE dummies and other empties), selection box and biome
## textures. Reads cache/manifest.json written by tools/import_game.py. View-only tool.
##
## Run:  godot --path godot res://scenes/tools/model_browser.tscn [-- --cache=DIR] [-- --model=REL]
## Mouse: left-drag orbit, right/middle-drag pan, wheel zoom. F frames the model.

const Loader := preload("res://scripts/tools/cache_loader.gd")
const BIOMES := ["Temperate", "Arctic", "Desert"]
const ATTACH_RX := "(?i)(ae|dummy|point|hitch|flag|window|door|exhaust|spotlight|camera|barrel|turret)"

var loader: Loader
var models: Array = []  # manifest "models" entries
var current: Node3D
var current_entry: Dictionary = {}
var materials: Dictionary = {}  # Material -> texture cache path
var original_textures: Dictionary = {}  # Material -> Texture2D
var player: AnimationPlayer
var overlay_root: Node3D

var filter_edit: LineEdit
var category_opt: OptionButton
var model_list: ItemList
var clip_list: ItemList
var biome_opt: OptionButton
var attach_check: CheckBox
var box_check: CheckBox
var hidden_check: CheckBox
var info: RichTextLabel
var status: Label
var world: Node3D
var camera: Camera3D
var orbit := Vector2(-0.6, 0.5)  # yaw, pitch
var distance := 8.0
var focus := Vector3.ZERO
var dragging := 0


func _ready() -> void:
	_build_ui()
	loader = Loader.new()
	var err := loader.load_manifest()
	if err != OK:
		status.text = "No manifest at %s: run tools/import_game.py first (or pass -- --cache=DIR)" % loader.cache_dir
		return
	models = loader.manifest.get("models", [])
	var cats := {}
	for m in models:
		cats[str(m.src).get_slice("/", 0)] = true
	category_opt.add_item("All")
	for c in cats.keys():
		category_opt.add_item(c)
	_refresh_list()
	status.text = "%d models, %d clip files, %d textures (cache: %s)" % [models.size(),
		loader.manifest.get("clips", []).size(), loader.manifest.get("textures", []).size(), loader.cache_dir]
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--model="):
			_select_by_glb(arg.trim_prefix("--model="))
		elif arg.begins_with("--screenshot="):
			_screenshot_later(arg.trim_prefix("--screenshot="))


# ---------------------------------------------------------------- UI
func _build_ui() -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)
	var split := HSplitContainer.new()
	split.set_anchors_preset(Control.PRESET_FULL_RECT)
	split.split_offset = 340
	add_child(split)

	var left := VBoxContainer.new()
	left.custom_minimum_size.x = 320
	split.add_child(left)
	filter_edit = LineEdit.new()
	filter_edit.placeholder_text = "Filter (e.g. PER tank, Buildings/GBR)"
	filter_edit.text_changed.connect(func(_t: String) -> void: _refresh_list())
	left.add_child(filter_edit)
	category_opt = OptionButton.new()
	category_opt.item_selected.connect(func(_i: int) -> void: _refresh_list())
	left.add_child(category_opt)
	model_list = ItemList.new()
	model_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	model_list.item_selected.connect(_on_model_selected)
	left.add_child(model_list)

	left.add_child(_label("Animation clips (click to play)"))
	clip_list = ItemList.new()
	clip_list.custom_minimum_size.y = 170
	clip_list.item_selected.connect(_on_clip_selected)
	left.add_child(clip_list)

	var row := HBoxContainer.new()
	left.add_child(row)
	row.add_child(_label("Biome"))
	biome_opt = OptionButton.new()
	for b in BIOMES:
		biome_opt.add_item(b)
	biome_opt.item_selected.connect(_on_biome_selected)
	row.add_child(biome_opt)
	attach_check = _check(left, "Attach points", true)
	box_check = _check(left, "Selection box", true)
	hidden_check = _check(left, "Show hidden nodes", false)
	info = RichTextLabel.new()
	info.custom_minimum_size.y = 150
	info.bbcode_enabled = true
	info.selection_enabled = true
	left.add_child(info)

	var right := VBoxContainer.new()
	right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	split.add_child(right)
	var svc := SubViewportContainer.new()
	svc.stretch = true
	svc.size_flags_vertical = Control.SIZE_EXPAND_FILL
	svc.gui_input.connect(_on_view_input)
	right.add_child(svc)
	var vp := SubViewport.new()
	vp.msaa_3d = Viewport.MSAA_4X
	svc.add_child(vp)
	status = _label("")
	right.add_child(status)

	world = Node3D.new()
	vp.add_child(world)
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.22, 0.24, 0.27)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.environment.ambient_light_color = Color(0.55, 0.55, 0.6)
	world.add_child(env)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, -35, 0)
	sun.shadow_enabled = true
	world.add_child(sun)
	world.add_child(_grid())
	camera = Camera3D.new()
	world.add_child(camera)
	overlay_root = Node3D.new()
	world.add_child(overlay_root)
	_update_camera()


func _label(t: String) -> Label:
	var l := Label.new()
	l.text = t
	return l


func _check(parent: Control, t: String, on: bool) -> CheckBox:
	var c := CheckBox.new()
	c.text = t
	c.button_pressed = on
	c.toggled.connect(func(_b: bool) -> void: _rebuild_overlays())
	parent.add_child(c)
	return c


func _grid() -> MeshInstance3D:
	var im := ImmediateMesh.new()
	im.surface_begin(Mesh.PRIMITIVE_LINES)
	for i in range(-20, 21):
		var c := Color(0.4, 0.4, 0.4) if i % 5 else Color(0.6, 0.6, 0.6)
		im.surface_set_color(c)
		im.surface_add_vertex(Vector3(i, 0, -20))
		im.surface_add_vertex(Vector3(i, 0, 20))
		im.surface_add_vertex(Vector3(-20, 0, i))
		im.surface_add_vertex(Vector3(20, 0, i))
	im.surface_end()
	var mi := MeshInstance3D.new()
	mi.mesh = im
	mi.material_override = _line_material(Color.WHITE, true)
	return mi


func _line_material(c: Color, vertex_color := false) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.albedo_color = c
	m.vertex_color_use_as_albedo = vertex_color
	m.no_depth_test = not vertex_color
	return m


func _refresh_list() -> void:
	model_list.clear()
	var words := filter_edit.text.to_lower().split(" ", false)
	var cat := category_opt.get_item_text(category_opt.selected) if category_opt.item_count else "All"
	for i in models.size():
		var src := str(models[i].src)
		if cat != "All" and src.get_slice("/", 0) != cat:
			continue
		var low := src.to_lower()
		var ok := true
		for w in words:
			if not low.contains(w):
				ok = false
				break
		if ok:
			var idx := model_list.add_item(src)
			model_list.set_item_metadata(idx, i)


func _select_by_glb(glb: String) -> void:
	for i in model_list.item_count:
		if str(models[model_list.get_item_metadata(i)].glb).to_lower() == glb.to_lower() \
				or str(models[model_list.get_item_metadata(i)].src).to_lower() == glb.to_lower():
			model_list.select(i)
			model_list.ensure_current_is_visible()
			_on_model_selected(i)
			return


# ---------------------------------------------------------------- model / clips
func _on_model_selected(idx: int) -> void:
	var entry: Dictionary = models[model_list.get_item_metadata(idx)]
	if current:
		current.queue_free()
		current = null
	original_textures.clear()
	var r: Dictionary = loader.load_model(entry.glb)
	if r.has("error"):
		status.text = "%s: %s" % [entry.glb, r.error]
		return
	current = r.root
	current_entry = entry
	materials = r.materials
	world.add_child(current)
	player = AnimationPlayer.new()
	player.name = "BrowserPlayer"
	current.add_child(player)
	player.root_node = player.get_path_to(current)
	_apply_hidden()
	_fill_clips()
	if biome_opt.selected != 0:
		_on_biome_selected(biome_opt.selected)
	_frame()
	_rebuild_overlays()
	_show_info()


func _fill_clips() -> void:
	clip_list.clear()
	var lib := AnimationLibrary.new()
	# Animation embedded in the model itself (NiKeyframeController), from GLTFDocument's player.
	for ap in current.find_children("*", "AnimationPlayer", true, false):
		if ap == player:
			continue
		var p := ap as AnimationPlayer
		for ln in p.get_animation_library_list():
			for an in p.get_animation_library(ln).get_animation_list():
				var a: Animation = p.get_animation_library(ln).get_animation(an)
				var rt: Array = Loader.retarget(a, current, current)
				_add_clip(lib, "(model) " + an, rt[0], "%d tracks" % rt[1])
		p.stop()
	var compat: Dictionary = loader.manifest.get("compatible_clips", {})
	for clip_glb in compat.get(current_entry.glb, []):
		var anims: Dictionary = loader.load_animations(clip_glb)
		for an in anims:
			var rt: Array = Loader.retarget(anims[an], current, current)
			if rt[1] > 0:
				var short := str(clip_glb).get_file().trim_suffix(".anim.glb")
				var shared := "shared " if str(clip_glb).begins_with("Shared/") else ""
				_add_clip(lib, "%s%s (%s)" % [shared, short, an], rt[0], "%d tracks" % rt[1])
	player.add_animation_library("", lib)
	if clip_list.item_count == 0:
		clip_list.add_item("(no clips)")
		clip_list.set_item_disabled(0, true)


func _add_clip(lib: AnimationLibrary, label: String, anim: Animation, detail: String) -> void:
	var extras: Dictionary = anim.get_meta("extras", {})
	anim.loop_mode = Animation.LOOP_NONE if extras.get("cycle_type", "loop") == "clamp" else Animation.LOOP_LINEAR
	var key := "clip_%d" % lib.get_animation_list().size()
	lib.add_animation(key, anim)
	var i := clip_list.add_item("%s  [%.2fs, %s]" % [label, anim.length, detail])
	clip_list.set_item_metadata(i, key)


func _on_clip_selected(idx: int) -> void:
	var key: Variant = clip_list.get_item_metadata(idx)
	if key == null or player == null:
		return
	player.stop()
	player.play(key)
	var a := player.get_animation(key)
	var tk: Array = a.get_meta("extras", {}).get("text_keys", [])
	status.text = "Playing %s, text keys: %s" % [clip_list.get_item_text(idx), tk]


# ---------------------------------------------------------------- biome textures
func _on_biome_selected(idx: int) -> void:
	if current == null:
		return
	var biome: String = BIOMES[idx]
	var swapped := 0
	for mat in materials:
		var m := mat as BaseMaterial3D
		if m == null:
			continue
		if not original_textures.has(m):
			original_textures[m] = m.albedo_texture
		var path: String = materials[mat]
		if biome == "Temperate":
			m.albedo_texture = original_textures[m]
			continue
		var variant := path.get_base_dir().path_join(biome).path_join(path.get_file())
		var tex := loader.load_texture(variant)
		if tex:
			m.albedo_texture = tex
			swapped += 1
		else:
			m.albedo_texture = original_textures[m]
	status.text = "Biome %s: %d of %d textures have a %s/ variant" % [biome, swapped, materials.size(), biome] \
		if biome != "Temperate" else "Biome Temperate (original textures)"


# ---------------------------------------------------------------- overlays
func _apply_hidden() -> void:
	for n in current.find_children("*", "Node3D", true, false):
		var ex: Dictionary = n.get_meta("extras", {})
		if ex.get("hidden", false):
			(n as Node3D).visible = hidden_check.button_pressed


func _rebuild_overlays() -> void:
	for c in overlay_root.get_children():
		c.queue_free()
	if current:
		for h in current.find_children("*", "RemoteTransform3D", true, false):
			h.queue_free()
	if current == null:
		return
	_apply_hidden()
	var rx := RegEx.create_from_string(ATTACH_RX)
	for n in current.find_children("*", "Node3D", true, false):
		if n is MeshInstance3D or n is Skeleton3D or n is AnimationPlayer or n == current:
			continue
		var node := n as Node3D
		var ex: Dictionary = node.get_meta("extras", {})
		if box_check.button_pressed and ex.has("selection_box"):
			_add_box(node, ex.selection_box)
		if attach_check.button_pressed and node.get_child_count() == 0 and rx.search(str(node.name)):
			_add_marker(node)


func _add_marker(node: Node3D) -> void:
	var holder := RemoteTransform3D.new()
	var marker := Node3D.new()
	overlay_root.add_child(marker)
	node.add_child(holder)
	holder.remote_path = holder.get_path_to(marker)
	holder.update_scale = false
	var im := ImmediateMesh.new()
	im.surface_begin(Mesh.PRIMITIVE_LINES)
	for axis in [[Vector3.RIGHT, Color.RED], [Vector3.UP, Color.GREEN], [Vector3.BACK, Color.BLUE]]:
		im.surface_set_color(axis[1])
		im.surface_add_vertex(Vector3.ZERO)
		im.surface_add_vertex(axis[0] * 0.25)
	im.surface_end()
	var mi := MeshInstance3D.new()
	mi.mesh = im
	mi.material_override = _line_material(Color.WHITE, true)
	(mi.material_override as StandardMaterial3D).no_depth_test = true
	marker.add_child(mi)
	var label := Label3D.new()
	label.text = str(node.name)
	label.pixel_size = 0.004
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.no_depth_test = true
	label.modulate = Color(1, 0.9, 0.4)
	label.position = Vector3(0, 0.12, 0)
	marker.add_child(label)


func _add_box(node: Node3D, box: Dictionary) -> void:
	var lo := Vector3(box["min"][0], box["min"][1], box["min"][2])
	var hi := Vector3(box["max"][0], box["max"][1], box["max"][2])
	var im := ImmediateMesh.new()
	im.surface_begin(Mesh.PRIMITIVE_LINES)
	var c := [Vector3(lo.x, lo.y, lo.z), Vector3(hi.x, lo.y, lo.z), Vector3(hi.x, hi.y, lo.z), Vector3(lo.x, hi.y, lo.z),
		Vector3(lo.x, lo.y, hi.z), Vector3(hi.x, lo.y, hi.z), Vector3(hi.x, hi.y, hi.z), Vector3(lo.x, hi.y, hi.z)]
	for e in [[0, 1], [1, 2], [2, 3], [3, 0], [4, 5], [5, 6], [6, 7], [7, 4], [0, 4], [1, 5], [2, 6], [3, 7]]:
		im.surface_add_vertex(c[e[0]])
		im.surface_add_vertex(c[e[1]])
	im.surface_end()
	var mi := MeshInstance3D.new()
	mi.mesh = im
	mi.material_override = _line_material(Color(0.3, 1.0, 0.4))
	# The SelectionBox node is usually flagged hidden, so follow it instead of parenting under it.
	var holder := RemoteTransform3D.new()
	overlay_root.add_child(mi)
	node.add_child(holder)
	holder.remote_path = holder.get_path_to(mi)


func _show_info() -> void:
	var e := current_entry
	var lines := ["[b]%s[/b]" % e.src, "glb: %s" % e.glb,
		"meshes %d, skins %d, embedded animations %d" % [e.meshes, e.skins, e.animations],
		"textures: %d" % e.textures.size()]
	if not e.missing_textures.is_empty():
		lines.append("[color=orange]missing textures: %s[/color]" % ", ".join(e.missing_textures))
	var file: Dictionary = loader.manifest.get("files", {}).get(str(e.src).to_lower(), {})
	if not file.get("warnings", []).is_empty():
		lines.append("[color=yellow]%s[/color]" % "\n".join(file.warnings))
	if not file.get("skipped", {}).is_empty():
		lines.append("skipped blocks: %s" % file.skipped)
	info.text = "\n".join(lines)


# ---------------------------------------------------------------- camera
func _frame() -> void:
	var aabb := AABB()
	var first := true
	for n in current.find_children("*", "MeshInstance3D", true, false):
		var mi := n as MeshInstance3D
		if not mi.is_visible_in_tree() or mi.mesh == null:
			continue
		var b := mi.global_transform * mi.get_aabb()
		aabb = b if first else aabb.merge(b)
		first = false
	if first:
		aabb = AABB(Vector3(-1, 0, -1), Vector3(2, 2, 2))
	focus = aabb.get_center()
	distance = max(aabb.size.length() * 1.1, 1.5)
	_update_camera()


func _update_camera() -> void:
	orbit.y = clamp(orbit.y, -1.4, 1.4)
	var dir := Vector3(cos(orbit.y) * sin(orbit.x), sin(orbit.y), cos(orbit.y) * cos(orbit.x))
	camera.position = focus + dir * distance
	camera.look_at(focus, Vector3.UP)
	camera.near = max(distance * 0.01, 0.01)
	camera.far = distance * 50 + 100


func _on_view_input(ev: InputEvent) -> void:
	if ev is InputEventMouseButton:
		var mb := ev as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_WHEEL_UP:
			distance *= 0.9
		elif mb.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			distance *= 1.1
		elif mb.pressed:
			dragging = mb.button_index
		else:
			dragging = 0
		_update_camera()
	elif ev is InputEventMouseMotion and dragging:
		var mm := ev as InputEventMouseMotion
		if dragging == MOUSE_BUTTON_LEFT:
			orbit += Vector2(-mm.relative.x, mm.relative.y) * 0.008
		else:
			var right := camera.global_transform.basis.x
			var up := camera.global_transform.basis.y
			focus += (-right * mm.relative.x + up * mm.relative.y) * distance * 0.0015
		_update_camera()


func _unhandled_key_input(ev: InputEvent) -> void:
	if ev is InputEventKey and ev.pressed and (ev as InputEventKey).keycode == KEY_F and current:
		_frame()


# ---------------------------------------------------------------- automation (docs, CI)
func _screenshot_later(path: String) -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--clip="):
			var want := arg.trim_prefix("--clip=").to_lower()
			for i in clip_list.item_count:
				if clip_list.get_item_text(i).to_lower().contains(want):
					clip_list.select(i)
					_on_clip_selected(i)
					break
		elif arg.begins_with("--biome="):
			var b := BIOMES.find(arg.trim_prefix("--biome="))
			if b >= 0:
				biome_opt.select(b)
				_on_biome_selected(b)
	for i in 30:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(path)
	print("screenshot: ", path)
	get_tree().quit()
