extends SceneTree
## Prints mesh parts, surfaces, skins and vertex counts of unit models (performance planning).
## godot --headless --path godot --script res://scripts/tools/model_stats.gd -- Units/PER/PERInfantry/PERInfantry.glb ...


func _initialize() -> void:
	var loader := CacheLoader.new()
	loader.load_manifest()
	for rel in OS.get_cmdline_user_args():
		var r: Dictionary = loader.load_model(rel)
		if r.has("error"):
			print(rel, ": ", r.error)
			continue
		var root: Node3D = r.root
		var skels := root.find_children("*", "Skeleton3D", true, false)
		var line := "%s: %d skeleton(s)" % [rel.get_file(), skels.size()]
		for sk in skels:
			line += " (%d bones)" % (sk as Skeleton3D).get_bone_count()
		print(line)
		for n in root.find_children("*", "MeshInstance3D", true, false):
			var mi := n as MeshInstance3D
			if mi.mesh == null:
				continue
			var verts := 0
			for s in mi.mesh.get_surface_count():
				verts += (mi.mesh.surface_get_arrays(s)[Mesh.ARRAY_VERTEX] as PackedVector3Array).size()
			var hidden: bool = mi.get_meta("extras", {}).get("hidden", false) or not mi.is_visible_in_tree()
			print("   %-28s surfaces %d verts %5d skin %s%s" % [mi.name, mi.mesh.get_surface_count(), verts,
				"yes" if mi.skin != null else "no", "  (hidden)" if hidden else ""])
			for s in mi.mesh.get_surface_count():
				var m := mi.get_active_material(s)
				var desc := "none" if m == null else m.get_class()
				if m is StandardMaterial3D:
					var sm := m as StandardMaterial3D
					desc += " tex=%s transp=%d cull=%d color=%s shading=%d" % [sm.albedo_texture != null, sm.transparency,
						sm.cull_mode, sm.albedo_color, sm.shading_mode]
				print("      surface %d: %s" % [s, desc])
		root.free()
	quit()
