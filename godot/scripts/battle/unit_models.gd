class_name UnitModels
extends RefCounted
## Loads and caches the imported model of each UnitDef (resource_prefix → cache .glb) with its
## Walk / Jog / Idle / Fidget1 clips. Shared by both unit renderers: UnitViews (one node tree per
## unit, High quality) and UnitBatches (one MultiMesh per unit def, Low and Medium).

var loader: CacheLoader
var bridge: SimBridge
var protos := {}          # unit def index -> {root, anims: {name: Animation}, model, radius, ids, error}
var model_index := {}     # lower-case cache path -> cache path (for case-insensitive lookups)


func _init(p_loader: CacheLoader, p_bridge: SimBridge) -> void:
	loader = p_loader
	bridge = p_bridge
	for m in loader.manifest.get("models", []):
		model_index[str(m.glb).to_lower()] = m.glb


## The model of unit def `def`: "root" (a Node3D outside the tree, nodes hidden by the exporter
## already invisible, no AnimationPlayers) and "anims", or "error" when it has none.
func proto(def: int) -> Dictionary:
	if protos.has(def):
		return protos[def]
	var vis: Dictionary = bridge.unit_visual(def)
	var p := {"radius": maxf(float(vis.get("radius", 0.5)), 0.3), "ids": vis.get("ids", "?")}
	protos[def] = p
	var rel := _resolve_model(str(vis.get("model", "")))
	if rel == "":
		p.error = "no cached model for %s (%s)" % [vis.get("ids", "?"), vis.get("model", "")]
		push_warning(p.error)
		return p
	var r: Dictionary = loader.load_model(rel)
	if r.has("error"):
		p.error = r.error
		push_warning("%s: %s" % [rel, r.error])
		return p
	var root: Node3D = r.root
	for n in root.find_children("*", "Node3D", true, false):
		var ex: Dictionary = n.get_meta("extras", {})
		if ex.get("hidden", false):
			(n as Node3D).visible = false
	for ap in root.find_children("*", "AnimationPlayer", true, false):
		ap.queue_free()
	p.root = root
	p.model = rel
	p.anims = _load_clips(rel, root)
	return p


## The first of `names` the model has a clip for, or "".
static func pick_clip(anims: Dictionary, names: Array) -> String:
	for n in names:
		if anims.has(n):
			return n
	return ""


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
