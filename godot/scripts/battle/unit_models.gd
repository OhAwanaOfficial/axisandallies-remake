class_name UnitModels
extends RefCounted
## Loads and caches the imported model of each UnitDef (resource_prefix → cache .glb) with its
## Walk / Jog / Idle / Fidget1 clips, the one-shot Recoil1 and Die1..Die4 clips, and what the
## effects need from its Thing record (info()). Shared by both unit renderers: UnitViews (one node
## tree per unit, High quality) and UnitBatches (one MultiMesh per unit def, Low and Medium), and
## by FxRunner.

const LOOP_CLIPS := ["Walk", "Jog", "Idle", "Fidget1"]
const DIE_CLIPS := ["Die1", "Die2", "Die3", "Die4"]

var loader: CacheLoader
var bridge: SimBridge
var protos := {}          # unit def index -> {root, anims: {name: Animation}, model, radius, ids, error}
var model_index := {}     # lower-case cache path -> cache path (for case-insensitive lookups)
var infos := {}           # unit def index -> info()
var _bones := {}          # "def/name" -> Vector3 (unit space) or null


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


## The model of building `ids` (its Thing record's resource_prefix): like proto(), with the
## construction stages hidden (ConstructionComponent: Stage* nodes, the finished one is Final).
func building_proto(ids: String) -> Dictionary:
	var key := "b:" + ids
	if protos.has(key):
		return protos[key]
	var info: Dictionary = bridge.thing_info(ids)
	var p := {"ids": ids, "info": info}
	protos[key] = p
	var rel := _resolve_model(str(info.get("model", "")))
	if rel == "":
		p.error = "no cached model for %s (%s)" % [ids, info.get("model", "")]
		return p
	var r: Dictionary = loader.load_model(rel)
	if r.has("error"):
		p.error = r.error
		return p
	var root: Node3D = r.root
	var has_final := false
	for n in root.find_children("*", "Node3D", true, false):
		var ex: Dictionary = n.get_meta("extras", {})
		if ex.get("hidden", false):
			(n as Node3D).visible = false
		if str(n.name).to_lower() == "final":
			has_final = true
	if has_final:
		for n in root.find_children("*", "Node3D", true, false):
			if str(n.name).to_lower().begins_with("stage"):
				(n as Node3D).visible = false
	for ap in root.find_children("*", "AnimationPlayer", true, false):
		ap.queue_free()
	p.root = root
	p.model = rel
	return p


## What the effects need from unit def `def`'s Thing record: properties (lower case), infantry,
## rot_time / sink_time (BodyComponent: corpse stays, then sinks), death_sfx (View WhenDestroyed
## PLAY_SFX audio ids), death_vfx (BodyComponent WhenHealth at 0 CREATE_VFX ids), attack_height,
## death_model (the aspect_death model in the cache, or "").
func info(def: int) -> Dictionary:
	if infos.has(def):
		return infos[def]
	var rec: Dictionary = bridge.unit_record(def)
	var out := {"properties": PackedStringArray(), "infantry": false, "rot_time": 5.0, "sink_time": 3.0,
		"death_sfx": PackedStringArray(), "death_vfx": PackedStringArray(), "attack_height": 1.2, "death_model": ""}
	for c in rec.get("_children", []):
		match str(c._name):
			"propertiescomponent":
				for p in c._children:
					if p._name == "property" and p.has("property_ids"):
						out.properties.append(str(p.property_ids).to_lower())
			"bodycomponent":
				out.rot_time = float(c.get("rot_time", 5))
				out.sink_time = float(c.get("sink_time", 3))
				for w in c._children:
					if w._name == "whenhealth" and str(w.get("type", "")) == "CREATE_VFX" and float(w.get("health", -1)) <= 0.0 							and float(w.get("health", -1)) > -0.5:
						out.death_vfx.append(str(w.vfx_ids))
			"view":
				for w in c._children:
					if w._name == "whendestroyed" and str(w.get("type", "")) == "PLAY_SFX":
						out.death_sfx.append(str(w.audio_ids))
					if w._name == "aspect" and str(w.get("ids", "")).to_lower() == "aspect_death":
						out.death_model = str(w.get("model", ""))
			"performercomponent":
				out.attack_height = float(c.get("attack_height", 1.2))
	out.infantry = "infantry" in out.properties
	# aspect_death's model is "%sDeath.nif": the unit's own model with that suffix.
	var dm := ""
	if out.death_model != "":
		var p := proto(def)
		if p.has("model"):
			var base := str(p.model).get_basename()
			dm = _resolve_model(str(out.death_model).replace("%s", base.get_file()).get_basename() + ".glb")
			if dm == "":
				dm = _resolve_model(base + "Death.glb")
	out.death_model = dm
	infos[def] = out
	return out


## Unit-space rest position (UnitViews.MODEL_YAW applied, +X forward) of the node or bone named
## `bone_name` in unit def `def`'s model (case-insensitive), or null.
func bone_offset(def: int, bone_name: String) -> Variant:
	var key := "%d/%s" % [def, bone_name.to_lower()]
	if _bones.has(key):
		return _bones[key]
	var found: Variant = null
	var p := proto(def)
	if p.has("root") and bone_name != "" and bone_name != "null":
		var root: Node3D = p.root
		var want := bone_name.to_lower()
		var yaw := Transform3D(Basis(Vector3.UP, UnitViews.MODEL_YAW), Vector3.ZERO)
		for n in root.find_children("*", "Node3D", true, false):
			if str(n.name).to_lower() == want:
				found = yaw * _chain(root, n).origin
				break
			if n is Skeleton3D:
				var sk := n as Skeleton3D
				for b in sk.get_bone_count():
					if sk.get_bone_name(b).to_lower() == want:
						found = yaw * (_chain(root, sk) * sk.get_bone_global_rest(b)).origin
						break
			if found != null:
				break
	_bones[key] = found
	return found


## Product of the local transforms below `top` down to `node` (the renderers replace the model
## root's own transform with MODEL_YAW).
static func _chain(top: Node, node: Node) -> Transform3D:
	var xf := Transform3D.IDENTITY
	var n := node
	while n != null and n != top:
		if n is Node3D:
			xf = (n as Node3D).transform * xf
		n = n.get_parent()
	return xf


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


## Walk / Jog / Idle / Fidget1 (looping), Recoil1 and Die1..Die4 (one-shot) clips compatible
## with the model, retargeted to it. Recoil clips are named after the model
## (GERTankMediumRecoil1): they are stored as "Recoil1".
func _load_clips(rel: String, root: Node3D) -> Dictionary:
	var out := {}
	var compat: Dictionary = loader.manifest.get("compatible_clips", {})
	for clip_glb in compat.get(rel, []):
		var short := str(clip_glb).get_file().trim_suffix(".anim.glb")
		if short.ends_with("Recoil1"):
			short = "Recoil1"
		var looping := short in LOOP_CLIPS
		if not (looping or short == "Recoil1" or short in DIE_CLIPS) or out.has(short):
			continue
		var anims: Dictionary = loader.load_animations(clip_glb)
		for an in anims:
			var rt: Array = CacheLoader.retarget(anims[an], root, root)
			if rt[1] > 0:
				var a: Animation = rt[0]
				a.loop_mode = Animation.LOOP_LINEAR if looping else Animation.LOOP_NONE
				out[short] = a
				break
	return out
