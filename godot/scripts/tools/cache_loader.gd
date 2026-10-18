class_name CacheLoader
extends RefCounted
## Loads what tools/import_game.py wrote to cache/ at runtime (no editor import step):
## models (.glb), clip files (.anim.glb) and DDS/TGA/PNG textures.
##
## Clips are retargeted by node name, as Gamebryo does: each track's target name is looked up as
## a Skeleton3D bone first, then as a Node3D anywhere under the model.

const ZUP_ROOT := "ZupToYup"

var cache_dir: String
var manifest: Dictionary = {}


static func default_cache_dir() -> String:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--cache="):
			return arg.trim_prefix("--cache=")
	var env := OS.get_environment("CORPS_CACHE")
	if env != "":
		return env
	return ProjectSettings.globalize_path("res://").path_join("../cache").simplify_path()


func _init(dir: String = "") -> void:
	cache_dir = dir if dir != "" else default_cache_dir()


func load_manifest() -> Error:
	var path := cache_dir.path_join("manifest.json")
	if not FileAccess.file_exists(path):
		return ERR_FILE_NOT_FOUND
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	if typeof(parsed) != TYPE_DICTIONARY:
		return ERR_PARSE_ERROR
	manifest = parsed
	return OK


func abs_path(rel: String) -> String:
	return cache_dir.path_join(rel)


## Loads a model. Returns {root: Node3D, materials: {Material: texture cache path}, error: String}.
func load_model(rel: String) -> Dictionary:
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var err := doc.append_from_file(abs_path(rel), state)
	if err != OK:
		return {"error": "GLTF load failed (%s)" % error_string(err)}
	var root: Node3D = doc.generate_scene(state, 30.0, false, false) as Node3D
	if root == null:
		return {"error": "generate_scene returned nothing"}
	_convert_importer_meshes(root)
	# Map each Material to the cache texture it uses (material extras written by nif_gltf.py).
	var mats := {}
	var json_mats: Array = state.json.get("materials", [])
	var godot_mats := state.get_materials()
	var base_dir := rel.get_base_dir()
	for i in range(min(json_mats.size(), godot_mats.size())):
		var extras: Dictionary = json_mats[i].get("extras", {})
		var uri: String = extras.get("texture", "")
		if uri != "":
			mats[godot_mats[i]] = base_dir.path_join(uri).simplify_path()
	return {"root": root, "materials": mats, "state": state}


## ImporterMeshInstance3D (what GLTFDocument produces outside the editor import) -> MeshInstance3D.
static func _convert_importer_meshes(root: Node) -> void:
	for n in root.find_children("*", "ImporterMeshInstance3D", true, false):
		var imi := n as ImporterMeshInstance3D
		var mi := MeshInstance3D.new()
		mi.name = imi.name
		mi.transform = imi.transform
		mi.visible = imi.visible
		if imi.mesh:
			mi.mesh = imi.mesh.get_mesh()
		mi.skin = imi.skin
		mi.skeleton = imi.skeleton_path
		for k in imi.get_meta_list():
			mi.set_meta(k, imi.get_meta(k))
		var parent := imi.get_parent()
		var idx := imi.get_index()
		var kids := imi.get_children()
		parent.remove_child(imi)
		parent.add_child(mi)
		parent.move_child(mi, idx)
		mi.owner = root
		for c in kids:
			imi.remove_child(c)
			mi.add_child(c)
		imi.free()


## Animations stored in a clip file (or a model's own embedded animation), as {name: Animation}.
func load_animations(rel: String) -> Dictionary:
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	if doc.append_from_file(abs_path(rel), state) != OK:
		return {}
	# Keep constant tracks: KF clips zero the accum root (Bip01) and move the height to
	# "Bip01 NonAccum"; dropping the constant Bip01 tracks would leave the model's rest height on top.
	var root := doc.generate_scene(state, 30.0, false, false)
	var out := {}
	if root:
		for ap in root.find_children("*", "AnimationPlayer", true, false):
			var player := ap as AnimationPlayer
			for lib_name in player.get_animation_library_list():
				var lib := player.get_animation_library(lib_name)
				for anim_name in lib.get_animation_list():
					out[anim_name] = lib.get_animation(anim_name)
		root.free()
	# glTF animation extras (duration, cycle_type, text_keys) from the importer.
	var extras := {}
	for a in state.json.get("animations", []):
		extras[a.get("name", "")] = a.get("extras", {})
	for k in out:
		(out[k] as Animation).set_meta("extras", extras.get(k, {}))
	return out


## Copy of `anim` with every track pointed at the bone/node of the same name under `model_root`
## (paths relative to `player_root`). Tracks whose target is missing are dropped; returns the copy
## and how many tracks matched.
static func retarget(anim: Animation, model_root: Node, player_root: Node) -> Array:
	var bones := {}  # validated bone name -> "Skeleton path:bone"
	for sk in model_root.find_children("*", "Skeleton3D", true, false):
		var skel := sk as Skeleton3D
		var sp := str(player_root.get_path_to(skel))
		for b in skel.get_bone_count():
			bones[skel.get_bone_name(b).validate_node_name()] = sp + ":" + skel.get_bone_name(b)
	var nodes := {}
	for n in model_root.find_children("*", "Node3D", true, false):
		if not nodes.has(str(n.name)):
			nodes[str(n.name)] = str(player_root.get_path_to(n))
	var out := Animation.new()
	out.length = anim.length
	out.loop_mode = anim.loop_mode
	var matched := 0
	for t in anim.get_track_count():
		var path := anim.track_get_path(t)
		var target := str(path.get_concatenated_subnames()) if path.get_subname_count() > 0 \
			else str(path.get_name(path.get_name_count() - 1))
		target = target.validate_node_name()
		var new_path := ""
		if bones.has(target):
			new_path = bones[target]
		elif nodes.has(target):
			new_path = nodes[target]
		if new_path == "":
			continue
		var nt := out.add_track(anim.track_get_type(t))
		out.track_set_path(nt, NodePath(new_path))
		out.track_set_interpolation_type(nt, anim.track_get_interpolation_type(t))
		for k in anim.track_get_key_count(t):
			out.track_insert_key(nt, anim.track_get_key_time(t, k), anim.track_get_key_value(t, k),
				anim.track_get_key_transition(t, k))
		matched += 1
	if anim.has_meta("extras"):
		out.set_meta("extras", anim.get_meta("extras"))
	return [out, matched]


## Loads a DDS/TGA/PNG from the cache as a texture (DDS keeps its DXT compression and mipmaps).
func load_texture(rel: String) -> Texture2D:
	var path := abs_path(rel)
	if not FileAccess.file_exists(path):
		return null
	var img := Image.new()
	var err := OK
	if rel.get_extension().to_lower() == "dds":
		err = img.load_dds_from_buffer(FileAccess.get_file_as_bytes(path))
	else:
		err = img.load(path)
	if err != OK:
		return null
	return ImageTexture.create_from_image(img)
