extends SceneTree
## Headless smoke test for CacheLoader against a real cache/ (skips when there is none).
##   godot --headless --path godot --script res://tests/test_cache_loader.gd

const Loader := preload("res://scripts/tools/cache_loader.gd")

func _init() -> void:
	var cl = Loader.new()
	if cl.load_manifest() != OK:
		print("SKIP: no cache/manifest.json at ", cl.cache_dir)
		quit(0)
		return
	var fails := 0
	for rel in ["Units/PER/PERTankMedium/PERTankMedium.glb", "Units/PER/PERInfantry/PERInfantry.glb"]:
		var r: Dictionary = cl.load_model(rel)
		if r.has("error"):
			print("FAIL ", rel, ": ", r.error); fails += 1; continue
		var root: Node3D = r.root
		var meshes := root.find_children("*", "MeshInstance3D", true, false)
		var skels := root.find_children("*", "Skeleton3D", true, false)
		var textured := 0
		for m in meshes:
			var mat = (m as MeshInstance3D).mesh.surface_get_material(0)
			if mat is BaseMaterial3D and (mat as BaseMaterial3D).albedo_texture != null:
				textured += 1
		print(rel, ": meshes=", meshes.size(), " textured=", textured, " skeletons=", skels.size(),
			" bones=", (skels[0] as Skeleton3D).get_bone_count() if skels.size() > 0 else 0,
			" material->texture=", r.materials.size())
		if meshes.is_empty() or textured == 0:
			fails += 1
		if rel.contains("Infantry"):
			var anims: Dictionary = cl.load_animations("Shared/Animations/Units/Infantry/Walk.anim.glb")
			for n in anims:
				var res: Array = Loader.retarget(anims[n], root, root)
				var a: Animation = res[0]
				print("  clip ", n, ": ", anims[n].get_track_count(), " tracks -> ", res[1], " retargeted, length ",
					a.length, " extras ", a.get_meta("extras", {}))
				if res[1] == 0:
					fails += 1
				if a.get_track_count() > 0:
					print("  first track: ", a.track_get_path(0))
		root.free()
	print("RESULT: ", "OK" if fails == 0 else "%d failure(s)" % fails)
	quit(1 if fails else 0)
