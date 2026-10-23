class_name BattleTerrain
extends Node3D
## Terrain for the battle view, built from SimBridge.map_info() (the sim's MapDef) and the
## original terrain textures in cache/ (SimBridge.terrain_table(): Terrain/Biome TGI records).
##
## Sim (x, y) maps to Godot (x, height, y). Heights are view-only: real maps have them per 2-unit
## tile corner (HEIGHT_MAP + MOUNTAIN_HEIGHT_MAP); generated test maps get a small relief from
## their terrain kinds so mountains and water read at a glance.

const TILE := 2.0
const LAYER_SIZE := 256
const MAX_LAYERS := 64
# Generated-map terrain kinds (nav::TerrainKind order) -> terrain keywords to pick a variation.
const GEN_KEYWORDS := {
	"gen_ground": ["grass"], "gen_road": ["dirt"], "gen_forest": ["forestdeciduous", "forest"],
	"gen_beach": ["shore", "beach"], "gen_shallow": ["shallow", "shore", "water"],
	"gen_water": ["ocean", "water"], "gen_mountain": ["mountain"], "gen_border": ["mountainpeak", "mountain"],
}
const GEN_HEIGHT := [0.0, 0.0, 0.1, -0.2, -0.6, -1.2, 3.5, 5.0]  # per TerrainKind

var cells := Vector2i.ZERO      # nav cells (1 unit)
var tiles := Vector2i.ZERO      # 2-unit tiles
var heights := PackedFloat32Array()  # (tiles + 1)^2 vertex heights, row-major
var hw := 0                     # vertices per row
var mesh_instance: MeshInstance3D
var info := ""
var _low_shader: Shader


func build(map: Dictionary, table: Dictionary, loader: CacheLoader) -> void:
	cells = Vector2i(map.width, map.height)
	tiles = Vector2i(ceili(cells.x / TILE), ceili(cells.y / TILE))
	hw = tiles.x + 1
	_build_heights(map)
	var mesh := _build_mesh()
	mesh_instance = MeshInstance3D.new()
	mesh_instance.name = "TerrainMesh"
	mesh_instance.mesh = mesh
	mesh_instance.material_override = _build_material(map, table, loader)
	add_child(mesh_instance)
	_build_water(map)


## Low quality: one texture sample per pixel from the tile's own layer, no blending across tile
## borders, lit per vertex (terrain.gdshader built with LOW defined). Off: the blended four-layer
## shader lit per pixel.
func set_low_quality(low: bool) -> void:
	var mat := mesh_instance.material_override as ShaderMaterial
	if mat == null:
		return
	if low and _low_shader == null:
		var base: Shader = load("res://scripts/battle/terrain.gdshader")
		_low_shader = Shader.new()
		_low_shader.code = base.code.replace("shader_type spatial;", "shader_type spatial;\nrender_mode vertex_lighting;\n#define LOW")
	mat.shader = _low_shader if low else load("res://scripts/battle/terrain.gdshader")


func height_at(x: float, y: float) -> float:
	if hw == 0:
		return 0.0
	var fx := clampf(x / TILE, 0.0, tiles.x - 0.001)
	var fy := clampf(y / TILE, 0.0, tiles.y - 0.001)
	var ix := int(fx)
	var iy := int(fy)
	var tx := fx - ix
	var ty := fy - iy
	var h00 := heights[iy * hw + ix]
	var h10 := heights[iy * hw + ix + 1]
	var h01 := heights[(iy + 1) * hw + ix]
	var h11 := heights[(iy + 1) * hw + ix + 1]
	return lerpf(lerpf(h00, h10, tx), lerpf(h01, h11, tx), ty)


func to_world(p: Vector2, lift := 0.0) -> Vector3:
	return Vector3(p.x, height_at(p.x, p.y) + lift, p.y)


## Where a ray meets the ground (sim x, y), or null.
func pick(from: Vector3, dir: Vector3) -> Variant:
	var prev := from
	var t := 0.0
	while t < 2000.0:
		t += 1.0
		var p := from + dir * t
		if p.y <= height_at(p.x, p.z):
			var a := prev
			var b := p
			for i in 20:
				var m := (a + b) * 0.5
				if m.y <= height_at(m.x, m.z):
					b = m
				else:
					a = m
			return Vector2(b.x, b.z)
		prev = p
	return null


# ---------------------------------------------------------------- heights and mesh

func _build_heights(map: Dictionary) -> void:
	heights.resize(hw * (tiles.y + 1))
	var src: PackedFloat32Array = map.heights
	if src.size() == heights.size():
		heights = src.duplicate()
		return
	# Generated map: relief from the terrain kind of the cells around each vertex.
	var kinds: PackedByteArray = map.cell_kind
	for vy in tiles.y + 1:
		for vx in hw:
			var sum := 0.0
			var n := 0
			for dy in [-1, 0]:
				for dx in [-1, 0]:
					var cx := clampi(vx * 2 + dx, 0, cells.x - 1)
					var cy := clampi(vy * 2 + dy, 0, cells.y - 1)
					sum += GEN_HEIGHT[kinds[cy * cells.x + cx]]
					n += 1
			heights[vy * hw + vx] = sum / n


func _build_mesh() -> ArrayMesh:
	var verts := PackedVector3Array()
	var normals := PackedVector3Array()
	var indices := PackedInt32Array()
	verts.resize(hw * (tiles.y + 1))
	normals.resize(verts.size())
	for vy in tiles.y + 1:
		for vx in hw:
			var i := vy * hw + vx
			verts[i] = Vector3(vx * TILE, heights[i], vy * TILE)
			var hl := heights[vy * hw + maxi(vx - 1, 0)]
			var hr := heights[vy * hw + mini(vx + 1, hw - 1)]
			var hd := heights[maxi(vy - 1, 0) * hw + vx]
			var hu := heights[mini(vy + 1, tiles.y) * hw + vx]
			normals[i] = Vector3(hl - hr, 2.0 * TILE, hd - hu).normalized()
	for ty in tiles.y:
		for tx in tiles.x:
			var a := ty * hw + tx
			var b := a + 1
			var c := a + hw
			var d := c + 1
			indices.append_array([a, b, c, b, d, c])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


## Water (TerrainKind Shallow / Water cells): one translucent plane just under the typical land
## height, so carved river and lake beds read as water. Deep-water variations have no texture.
func _build_water(map: Dictionary) -> void:
	var kinds: PackedByteArray = map.cell_kind
	var land := PackedFloat32Array()
	var any_water := false
	for vy in range(0, tiles.y + 1, 2):
		for vx in range(0, hw, 2):
			var k := kinds[mini(vy * 2, cells.y - 1) * cells.x + mini(vx * 2, cells.x - 1)]
			if k == 4 or k == 5:
				any_water = true
			elif k <= 3:
				land.append(heights[vy * hw + vx])
	if not any_water or land.is_empty():
		return
	land.sort()  # the median land height: mountains don't lift the water
	var plane := PlaneMesh.new()
	plane.size = Vector2(cells)
	var mi := MeshInstance3D.new()
	mi.name = "Water"
	mi.mesh = plane
	mi.position = Vector3(cells.x * 0.5, land[land.size() / 2] - 0.25, cells.y * 0.5)
	var m := StandardMaterial3D.new()
	m.albedo_color = Color(0.16, 0.32, 0.42, 0.78)
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	m.roughness = 0.15
	m.metallic_specular = 0.8
	mi.material_override = m
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(mi)


# ---------------------------------------------------------------- textures

## The variation for a map terrain name: itself for real maps; for generated maps the first (by IDS)
## variation of the map's biome whose terrain matches the kind's keywords (temperate_grass01, ...).
func _variation_for(name: String, variations: Dictionary, biome: String) -> String:
	var key := name.to_lower()
	if variations.has(key):
		return key
	var prefix := biome.to_lower() + "_"
	for kw in GEN_KEYWORDS.get(key, []):
		var best := ""
		for v in variations:
			var terrain := str(variations[v].terrain)
			if terrain.begins_with(prefix) and terrain.trim_prefix(prefix).begins_with(kw) and (best == "" or str(v) < best):
				best = v
		if best != "":
			return best
	return ""


func _build_material(map: Dictionary, table: Dictionary, loader: CacheLoader) -> ShaderMaterial:
	var variations: Dictionary = table.get("variations", {})
	var names: PackedStringArray = map.terrain_names
	var layer_of_name := PackedInt32Array()
	var layer_keys: Array[String] = []
	var images: Array[Image] = []
	var repeat := PackedFloat32Array()
	var colors := PackedVector3Array()
	var has_tex := PackedFloat32Array()
	var textured := 0
	var untextured: Array[String] = []
	# Only names some tile uses get a layer (the name table lists every terrain in the game).
	var cell_terrain: PackedInt32Array = map.cell_terrain
	var used := {}
	for ty in tiles.y:
		for tx in tiles.x:
			used[cell_terrain[mini(ty * 2, cells.y - 1) * cells.x + mini(tx * 2, cells.x - 1)]] = true
	for ni in names.size():
		var n := names[ni]
		if not used.has(ni):
			layer_of_name.append(0)
			continue
		var v := _variation_for(n, variations, str(map.get("biome", "temperate")))
		var idx := layer_keys.find(v)
		if idx < 0 and layer_keys.size() < MAX_LAYERS:
			idx = layer_keys.size()
			layer_keys.append(v)
			var e: Dictionary = variations.get(v, {})
			var img: Image = null
			var tex_path: String = e.get("texture", "")
			if tex_path != "":
				var tex := loader.load_texture(tex_path)
				if tex:
					img = tex.get_image()
			var c: Color = e.get("color", Color(0.45, 0.5, 0.35))
			colors.append(Vector3(c.r, c.g, c.b))
			has_tex.append(1.0 if img != null else 0.0)
			# texture_width/height: tiles one texture repeat covers.
			repeat.append(maxf(float(e.get("width", 8.0)), 1.0) * TILE)
			if img == null:
				untextured.append("%s (%s)" % [v if v != "" else n, tex_path if tex_path != "" else "no texture"])
				img = Image.create(4, 4, false, Image.FORMAT_RGBA8)
				img.fill(c)
			else:
				textured += 1
			if img.is_compressed():
				img.decompress()
			img.convert(Image.FORMAT_RGBA8)
			img.resize(LAYER_SIZE, LAYER_SIZE, Image.INTERPOLATE_BILINEAR)
			img.generate_mipmaps()
			images.append(img)
		layer_of_name.append(maxi(idx, 0))
	if images.is_empty():
		var img := Image.create(LAYER_SIZE, LAYER_SIZE, true, Image.FORMAT_RGBA8)
		img.fill(Color(0.4, 0.5, 0.3))
		images.append(img)
		colors.append(Vector3(0.4, 0.5, 0.3))
		has_tex.append(0.0)
		repeat.append(16.0)
	var array := Texture2DArray.new()
	array.create_from_images(images)

	# Tile -> layer index (from the cell at the tile's lower corner).
	var idx_img := Image.create(tiles.x, tiles.y, false, Image.FORMAT_R8)
	for ty in tiles.y:
		for tx in tiles.x:
			var cx := mini(tx * 2, cells.x - 1)
			var cy := mini(ty * 2, cells.y - 1)
			var name_idx := cell_terrain[cy * cells.x + cx]
			var layer := layer_of_name[name_idx] if name_idx < layer_of_name.size() else 0
			idx_img.set_pixel(tx, ty, Color(layer / 255.0, 0, 0))
	var mat := ShaderMaterial.new()
	mat.shader = load("res://scripts/battle/terrain.gdshader")
	mat.set_shader_parameter("layers", array)
	mat.set_shader_parameter("tile_layer", ImageTexture.create_from_image(idx_img))
	mat.set_shader_parameter("tile_count", Vector2(tiles))
	mat.set_shader_parameter("tile_size", TILE)
	repeat.resize(MAX_LAYERS)
	colors.resize(MAX_LAYERS)
	has_tex.resize(MAX_LAYERS)
	mat.set_shader_parameter("layer_repeat", repeat)
	mat.set_shader_parameter("layer_color", colors)
	mat.set_shader_parameter("layer_has_texture", has_tex)
	info = "%d terrain layers (%d textured, biome %s)%s" % [layer_keys.size(), textured, map.get("biome", "?"),
		"; colour only: " + ", ".join(untextured) if not untextured.is_empty() else ""]
	return mat
