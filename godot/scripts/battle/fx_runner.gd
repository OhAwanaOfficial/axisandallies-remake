class_name FxRunner
extends Node3D
## Plays the view side of combat (ARCHITECTURE §4.5) from SimBridge events. The sim emits Fired
## (shooter, target, ability), Hit (shot of an ability landed at a point, on a primary target)
## and Died (unit, killer, position, facing); this runs the FXCs the data attaches to them:
##
##   Fired: the ability's WhenStart / WhenDo FXCs aimed at SELF: CREATE_VFX at the muzzle bone
##     (location), PLAY_SFX, PLAY_OBJECT_ANIMATION (Recoil1, High quality only).
##   Hit:   its FXCs aimed at TARGET_THING / TARGET_HIT_POSITION / TARGET_GROUND, filtered by
##     target_flags (+infantry, -infantry, +building ...), plus its projectile's WhenDestroyed FXCs.
##   Died:  the unit's View WhenDestroyed PLAY_SFX and BodyComponent WhenHealth(0) CREATE_VFX, then
##     the body: vehicles swap to their aspect_death model (merged into one mesh per material);
##     infantry play a Die clip (High: a node with an AnimationPlayer; Low / Medium: UnitBatches'
##     baked corpses). Bodies stay rot_time seconds, then sink over sink_time.
##
## Everything a unit type can need (records, decoded sounds, death model) is loaded by prewarm()
## when the type first appears, so a fight doesn't stall on first shots and deaths.
##
## Sounds are the original WAVs (Audio records: files, volume, pitch_adjust, range,
## simultaneous_limit), read from the game's VFS through SimBridge and decoded once. VFX are not
## the original NIF particle systems: each VFX record maps by its model's name to a few procedural
## sprites (muzzle flash, fireball, spark, smoke, dust) drawn by fx_sprite.gdshader from two
## MultiMeshes, written only on ticks that spawn something. The view never touches sim state.

const BUFFERS := 3
## Per quality: sprite capacity (additive, smoke), sound voices, bodies (wrecks + node corpses).
const BUDGET := {
	"low": {"sprites": 128, "smoke": 64, "voices": 8, "bodies": 16, "secondary": false},
	"medium": {"sprites": 256, "smoke": 128, "voices": 16, "bodies": 48, "secondary": true},
	"high": {"sprites": 384, "smoke": 192, "voices": 24, "bodies": 96, "secondary": true},
}
const SINK_DEPTH := 2.0

var bridge: SimBridge
var models: UnitModels
var terrain: BattleTerrain
var units                         # UnitBatches or UnitViews (recoil, corpses)
var quality := "low"
var clock := 0.0                  # fx_time: advances with the sim speed, stops when paused
var stats := {"fired": 0, "hits": 0, "deaths": 0, "sounds": 0, "sprites": 0, "bodies": 0, "corpses": 0, "recoils": 0}

var _abilities := {}              # ability index -> {fire: [fxc], hit: [fxc]}
var _vfx := {}                    # vfx ids -> [[kind, size, life], ...]
var _sounds := {}                 # audio ids -> {streams, volume_db, limit, unit_size, max_distance, pitch} or {}
var _wav := {}                    # VFS path -> AudioStreamWAV or null
var _voices: Array[AudioStreamPlayer3D] = []
var _voice_ids: Array[String] = []
var _listener: AudioListener3D
var _pools: Array = []            # [additive, smoke]: {rings, data: PackedFloat32Array, cap, next, count, dirty}
var _bodies: Array = []           # {node, until_rot, until_gone, y}
var _death_meshes := {}           # death model path -> Mesh or null (merged, unit space)
var _rng := RandomNumberGenerator.new()
var _bounds := AABB()
static var _globals := false


## fx_time is a global shader uniform (sprites, banners and baked corpses read it); it must exist
## before those shaders compile.
static func ensure_globals() -> void:
	if not _globals:
		RenderingServer.global_shader_parameter_add(&"fx_time", RenderingServer.GLOBAL_VAR_TYPE_FLOAT, 0.0)
		_globals = true


func setup(p_bridge: SimBridge, p_models: UnitModels, p_terrain: BattleTerrain) -> void:
	bridge = p_bridge
	models = p_models
	terrain = p_terrain
	_rng.seed = 12345  # view-only randomness (sound variant, pitch, sprite rotation)
	_bounds = AABB(Vector3(-16, -50, -16), Vector3(terrain.cells.x + 32, 150, terrain.cells.y + 32))
	_listener = AudioListener3D.new()
	add_child(_listener)
	_listener.make_current()


## Sprite pools and voices for quality `q` (keeps the decoded sounds).
func set_quality(q: String, p_units) -> void:
	quality = q
	units = p_units
	var b: Dictionary = BUDGET[q]
	for p in _pools:
		for mmi in p.rings:
			(mmi as Node).queue_free()
	_pools = [_pool(b.sprites, "blend_add"), _pool(b.smoke, "blend_mix")]
	# One long-expired sprite each, so the shaders compile now rather than at the first shot.
	for p in _pools:
		_sprite(p, Vector3.ZERO, 0.0, 0.001, 0 if p == _pools[0] else 3, clock - 10.0)
	_death_meshes.clear()  # merged with this quality's lighting
	while _voices.size() > b.voices:
		_voices.pop_back().queue_free()
		_voice_ids.pop_back()
	while _voices.size() < b.voices:
		var v := AudioStreamPlayer3D.new()
		v.attenuation_model = AudioStreamPlayer3D.ATTENUATION_INVERSE_DISTANCE
		add_child(v)
		_voices.append(v)
		_voice_ids.append("")
	while _bodies.size() > b.bodies:
		_drop_body(0)


## Advances fx_time and the bodies, rewrites the sprite buffers that changed, keeps the listener
## at the camera's focus on the ground. `rate` is the sim speed (0 when paused).
func update(delta: float, rate: float, listen_at: Vector3) -> void:
	clock += delta * rate
	RenderingServer.global_shader_parameter_set(&"fx_time", clock)
	_listener.global_position = listen_at + Vector3(0, 12, 0)
	var i := 0
	while i < _bodies.size():
		var body: Dictionary = _bodies[i]
		if clock >= body.until_gone:
			_drop_body(i)
			continue
		if clock > body.until_rot:
			var k: float = (clock - body.until_rot) / maxf(body.until_gone - body.until_rot, 0.01)
			(body.node as Node3D).position.y = body.y - k * SINK_DEPTH
		i += 1
	var n := 0
	for p in _pools:
		if p.dirty:
			_flush(p)
		n += p.count
	stats.sprites = n
	stats.bodies = _bodies.size()


## Loads what unit def `def`'s effects need: its record, its weapons' FXCs with their sounds and
## sprites, its death sounds and its death model.
func prewarm(def: int) -> void:
	var info := models.info(def)
	for ids in info.death_sfx:
		_sound(ids)
	for ids in info.death_vfx:
		_vfx_spec(ids)
	if info.death_model != "":
		_death_mesh(info.death_model)
	for a in (bridge.unit_visual(def).get("abilities", PackedInt32Array()) as PackedInt32Array):
		var ab := _ability(a)
		for f in ab.fire + ab.hit:
			if f.type == "PLAY_SFX":
				_sound(f.ids)
			elif f.type == "CREATE_VFX":
				_vfx_spec(f.ids)


## Runs the effects of this tick's events. `snap` is the snapshot after the tick, `prev_snap` the
## one before (units killed this tick are only in that one).
func handle(events: Array, snap: Dictionary, prev_snap: Dictionary) -> void:
	for e in events:
		match str(e.type):
			"fired":
				stats.fired += 1
				_on_fired(e, snap)
			"hit":
				stats.hits += 1
				_on_hit(e, snap, prev_snap)
			"died":
				stats.deaths += 1
				_on_died(e, prev_snap)


# ---------------------------------------------------------------- events

func _on_fired(e: Dictionary, snap: Dictionary) -> void:
	var ab := _ability(int(e.detail))
	if ab.fire.is_empty():
		return
	var at: Vector2 = e.at
	var i := _index(snap, int(e.entity))
	var def: int = snap.def[i] if i >= 0 else -1
	# Aim: toward the target (turrets turn to it), else the body's facing.
	var heading: float = e.facing
	var t := _index(snap, int(e.other))
	if t >= 0:
		var to: Vector2 = (snap.cur_pos[t] as Vector2) - at
		if to.length_squared() > 0.01:
			heading = to.angle()
	for f in ab.fire:
		match f.type:
			"PLAY_SFX":
				_play_sound(f.ids, terrain.to_world(at, 1.0))
			"CREATE_VFX":
				_spawn_vfx(f.ids, _muzzle(def, f.location, at, heading))
			"PLAY_OBJECT_ANIMATION":
				if units != null and units.has_method("play_oneshot") and units.play_oneshot(int(e.entity), f.ids):
					stats.recoils += 1


func _on_hit(e: Dictionary, snap: Dictionary, prev_snap: Dictionary) -> void:
	var ab := _ability(int(e.detail))
	if ab.hit.is_empty():
		return
	var at: Vector2 = e.at
	# What was hit: infantry, building or neither (vehicles, the ground).
	var infantry := false
	var building := false
	var bits := int(e.entity)
	var s := snap
	var i := _index(snap, bits)
	if i < 0:
		s = prev_snap
		i = _index(prev_snap, bits)
	if i >= 0:
		building = s.kind[i] == 3
		if s.kind[i] == 0:
			infantry = bool(models.info(s.def[i]).infantry)
	for f in ab.hit:
		if not _flags_pass(f.flags, infantry, building):
			continue
		var h := 0.0
		match f.at:
			"target_thing":
				h = 1.0 if infantry else 1.4
			"target_hit_position":
				h = 0.7
			_:
				h = 0.1
		var p := terrain.to_world(at + Vector2(_rng.randf_range(-0.3, 0.3), _rng.randf_range(-0.3, 0.3)), h)
		if f.type == "PLAY_SFX":
			_play_sound(f.ids, p)
		elif f.type == "CREATE_VFX":
			_spawn_vfx(f.ids, p)


func _on_died(e: Dictionary, prev_snap: Dictionary) -> void:
	var i := _index(prev_snap, int(e.entity))
	if i < 0 or prev_snap.kind[i] != 0:
		return
	var def: int = prev_snap.def[i]
	var info := models.info(def)
	var at: Vector2 = e.at
	var pos := terrain.to_world(at)
	for ids in info.death_sfx:
		_play_sound(ids, pos + Vector3(0, 1, 0))
	for ids in info.death_vfx:
		_spawn_vfx(ids, pos + Vector3(0, 1.2, 0))
	var facing: float = e.facing
	if info.death_model != "":
		_add_wreck(info.death_model, pos, facing, info.rot_time, info.sink_time)
	elif quality == "high":
		_add_corpse_node(def, int(e.entity), pos, facing, info.rot_time, info.sink_time)
	elif units != null and units.has_method("add_corpse"):
		if units.add_corpse(def, pos, facing, int(prev_snap.player[i]), clock, info.rot_time, info.sink_time):
			stats.corpses += 1


# ---------------------------------------------------------------- abilities' FXCs

## {fire: [...], hit: [...]} of DefSet ability `index`; each FXC {type, ids, at, flags, location}.
func _ability(index: int) -> Dictionary:
	if _abilities.has(index):
		return _abilities[index]
	var out := {"fire": [], "hit": []}
	_abilities[index] = out
	var rec: Dictionary = bridge.ability_record(index)
	for c in rec.get("_children", []):
		if c._name in ["whenstart", "whendo"]:
			var f := _fxc(c)
			if f.is_empty():
				continue
			if str(f.at).begins_with("target"):
				out.hit.append(f)
			else:
				out.fire.append(f)
	# A projectile's impact: its WhenDestroyed FXCs (SELF is where it burst).
	var proj := str(rec.get("projectile_ids", "null"))
	if proj != "null" and proj != "":
		var pr: Dictionary = bridge.record("thing", proj)
		for c in pr.get("_children", []):
			if c._name != "view":
				continue
			for w in c._children:
				if w._name == "whendestroyed":
					var f := _fxc(w)
					if not f.is_empty():
						out.hit.append(f)
	return out


func _fxc(n: Dictionary) -> Dictionary:
	var type := str(n.get("type", ""))
	var ids := ""
	match type:
		"CREATE_VFX":
			ids = str(n.get("vfx_ids", ""))
		"PLAY_SFX":
			ids = str(n.get("audio_ids", ""))
		"PLAY_OBJECT_ANIMATION":
			ids = str(n.get("object_animation_ids", ""))
		_:
			return {}
	if ids == "" or ids.to_lower() == "null":
		return {}
	var flags := str(n.get("target_flags", "null")).to_lower()
	return {"type": type, "ids": ids, "at": str(n.get("at", "self")).to_lower(),
		"flags": flags if flags != "null" else "", "location": str(n.get("location", "null"))}


## target_flags: "+infantry" needs an infantry target, "-infantry" anything else; likewise building.
static func _flags_pass(flags: String, infantry: bool, building: bool) -> bool:
	if flags == "":
		return true
	for tok in flags.replace(",", " ").split(" ", false):
		var want := tok.begins_with("+")
		var prop := tok.trim_prefix("+").trim_prefix("-")
		var has := infantry if prop == "infantry" else (building if prop == "building" else false)
		if has != want:
			return false
	return true


## World position of the muzzle: the FXC's location bone in the unit's model turned to `heading`,
## or a guess from its size.
func _muzzle(def: int, bone: String, at: Vector2, heading: float) -> Vector3:
	var off: Variant = models.bone_offset(def, bone) if def >= 0 else null
	if off == null:
		var info := models.info(def) if def >= 0 else {"infantry": true}
		var r := float(models.proto(def).get("radius", 0.5)) if def >= 0 else 0.5
		off = Vector3(0.5, 1.3, 0.0) if info.infantry else Vector3(r * 2.2, 1.8, 0.0)
	var w := Basis(Vector3.UP, -heading) * (off as Vector3)
	return terrain.to_world(at) + w


# ---------------------------------------------------------------- sprites

## The sprites of VFX record `ids`, by its model's name: [[kind, size, life], ...].
func _vfx_spec(ids: String) -> Array:
	if _vfx.has(ids):
		return _vfx[ids]
	var rec: Dictionary = bridge.record("vfx", ids)
	var spec: Array = []
	var model := str(rec.get("model", "")).to_lower()
	if str(rec.get("category", "")) == "VFXMODEL" and model != "":
		var big := model.contains("tank") or model.contains("artillery") or model.contains("mortar") or model.contains("grenade") \
			or model.contains("rocket") or model.contains("rpg") or model.contains("bomb")
		var mg := model.contains("mg")
		if model.contains("debris"):
			spec = [[1, 4.5, 1.0], [3, 4.0, 4.0], [2, 2.0, 0.25]]
		elif model.contains("explosion"):
			if big:
				spec = [[1, 2.8, 0.7], [3, 2.5, 2.5], [4, 2.0, 1.2]]
			elif model.contains("ground"):
				spec = [[4, 0.6, 0.6]]
			elif model.contains("hard"):
				spec = [[2, 0.5, 0.15]]
			else:
				spec = [[4, 0.35, 0.4]]
		elif model.contains("shot"):
			if big:
				spec = [[0, 1.6, 0.09], [3, 1.4, 1.5]]
			elif mg:
				spec = [[0, 0.7, 0.05]]
			else:
				spec = [[0, 0.5, 0.06], [3, 0.4, 0.6]]
	_vfx[ids] = spec
	return spec


func _spawn_vfx(ids: String, p: Vector3) -> void:
	var spec := _vfx_spec(ids)
	var secondary: bool = BUDGET[quality].secondary
	for k in spec.size():
		var s: Array = spec[k]
		if k > 0 and not secondary and s[0] >= 3:
			continue  # Low: no trailing smoke / dust
		_sprite(_pools[1 if s[0] >= 3 else 0], p, s[1], s[2], s[0])


func _pool(cap: int, blend: String) -> Dictionary:
	var shader: Shader = load("res://scripts/battle/fx_sprite.gdshader")
	if blend != "blend_add":
		var s := Shader.new()
		s.code = shader.code.replace("render_mode blend_add,", "render_mode %s," % blend)
		shader = s
	var mat := ShaderMaterial.new()
	mat.shader = shader
	var quad := QuadMesh.new()
	quad.size = Vector2(1, 1)
	quad.material = mat
	var rings: Array = []
	for i in BUFFERS:
		var mm := MultiMesh.new()
		mm.transform_format = MultiMesh.TRANSFORM_3D
		mm.use_custom_data = true
		mm.mesh = quad
		var mmi := MultiMeshInstance3D.new()
		mmi.name = "Fx_%s_%d" % [blend, i]
		mmi.multimesh = mm
		mmi.custom_aabb = _bounds
		mmi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		mmi.visible = false
		add_child(mmi)
		rings.append(mmi)
	var data := PackedFloat32Array()
	data.resize(cap * 16)
	return {"rings": rings, "data": data, "cap": cap, "next": 0, "count": 0, "dirty": false, "shown": 0}


## Writes a sprite into the pool's ring (overwriting the oldest when full).
func _sprite(p: Dictionary, at: Vector3, size: float, life: float, kind: int, start := -1.0e9) -> void:
	var o: int = p.next * 16
	var d: PackedFloat32Array = p.data
	var row := [size, 0, 0, at.x, 0, size, 0, at.y, 0, 0, size, at.z, clock if start < -1.0e8 else start, life, kind, _rng.randf()]
	for k in 16:
		d[o + k] = row[k]
	p.data = d
	p.next = (p.next + 1) % p.cap
	p.count = mini(p.count + 1, p.cap)
	p.dirty = true


func _flush(p: Dictionary) -> void:
	p.dirty = false
	p.shown = (p.shown + 1) % BUFFERS
	var mm: MultiMesh = (p.rings[p.shown] as MultiMeshInstance3D).multimesh
	if mm.instance_count != p.cap:
		mm.instance_count = p.cap
	mm.buffer = p.data
	mm.visible_instance_count = p.cap if p.count >= p.cap else p.count
	for i in BUFFERS:
		(p.rings[i] as MultiMeshInstance3D).visible = i == p.shown


# ---------------------------------------------------------------- sounds

func _play_sound(ids: String, at: Vector3) -> void:
	var sd := _sound(ids)
	if sd.is_empty() or (sd.streams as Array).is_empty():
		return
	if at.distance_to(_listener.global_position) > float(sd.max_distance) * 1.2:
		return
	var free := -1
	var same := 0
	for i in _voices.size():
		if _voices[i].playing:
			if _voice_ids[i] == ids:
				same += 1
		elif free < 0:
			free = i
	if free < 0 or same >= int(sd.limit):
		return
	var v := _voices[free]
	v.stream = sd.streams[_rng.randi() % (sd.streams as Array).size()]
	v.volume_db = sd.volume_db
	v.unit_size = sd.unit_size
	v.max_distance = sd.max_distance
	v.pitch_scale = 1.0 + _rng.randf_range(sd.pitch[0], sd.pitch[1])
	v.global_position = at
	v.play()
	_voice_ids[free] = ids
	stats.sounds += 1


## Audio record `ids`: its WAVs decoded, volume, range, simultaneous limit, pitch spread.
func _sound(ids: String) -> Dictionary:
	if _sounds.has(ids):
		return _sounds[ids]
	var rec: Dictionary = bridge.record("audio", ids)
	var out := {}
	if not rec.is_empty():
		var streams: Array = []
		for f in str(rec.get("files", "")).split(",", false):
			var s := _load_wav("Audio/" + f.strip_edges())
			if s != null:
				streams.append(s)
		var vol := _percent(str(rec.get("volume", "100%")))
		var rng := str(rec.get("range", "4, 64")).split(",", false)
		var pitch := str(rec.get("pitch_adjust", "0%, 0%")).split(",", false)
		out = {
			"streams": streams,
			"volume_db": linear_to_db(maxf(vol, 0.01)) - 4.0,
			"limit": maxi(int(rec.get("simultaneous_limit", "2")), 1),
			"unit_size": float(rng[0]) * 2.0 if rng.size() > 0 else 8.0,
			"max_distance": float(rng[1]) * 2.0 if rng.size() > 1 else 128.0,
			"pitch": [_percent(pitch[0]) if pitch.size() > 0 else 0.0, _percent(pitch[1]) if pitch.size() > 1 else 0.0],
		}
	_sounds[ids] = out
	return out


static func _percent(s: String) -> float:
	return float(s.strip_edges().trim_suffix("%")) / 100.0


## A RIFF WAVE file (PCM 8 or 16 bit) from the VFS as an AudioStreamWAV, or null.
func _load_wav(path: String) -> AudioStreamWAV:
	if _wav.has(path):
		return _wav[path]
	_wav[path] = null
	var b: PackedByteArray = bridge.read_file(path)
	if b.size() < 44 or b.slice(0, 4).get_string_from_ascii() != "RIFF" or b.slice(8, 12).get_string_from_ascii() != "WAVE":
		if b.is_empty():
			push_warning("sound not found: %s" % path)
		return null
	var fmt := -1
	var channels := 1
	var rate := 22050
	var bits := 16
	var pcm := PackedByteArray()
	var o := 12
	while o + 8 <= b.size():
		var id := b.slice(o, o + 4).get_string_from_ascii()
		var size := b.decode_u32(o + 4)
		var body := o + 8
		if id == "fmt ":
			fmt = b.decode_u16(body)
			channels = b.decode_u16(body + 2)
			rate = b.decode_u32(body + 4)
			bits = b.decode_u16(body + 14)
		elif id == "data":
			pcm = b.slice(body, mini(body + size, b.size()))
		o = body + size + (size & 1)
	if fmt != 1 or pcm.is_empty() or (bits != 8 and bits != 16):
		push_warning("unsupported WAV %s (format %d, %d bit)" % [path, fmt, bits])
		return null
	var s := AudioStreamWAV.new()
	if bits == 8:
		# WAV 8-bit is unsigned; AudioStreamWAV wants signed.
		for i in pcm.size():
			pcm[i] = (pcm[i] + 128) & 0xFF
		s.format = AudioStreamWAV.FORMAT_8_BITS
	else:
		s.format = AudioStreamWAV.FORMAT_16_BITS
	s.mix_rate = rate
	s.stereo = channels == 2
	s.data = pcm
	_wav[path] = s
	return s


# ---------------------------------------------------------------- bodies

func _add_wreck(model: String, pos: Vector3, facing: float, rot_time: float, sink_time: float) -> void:
	var mesh := _death_mesh(model)
	if mesh == null:
		return
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	if quality == "low":
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_add_body(mi, pos, facing, rot_time, sink_time)


## Death model `model` merged into one mesh per material in unit space (as UnitBatches merges
## unit models), so a wreck is one or two draws.
func _death_mesh(model: String) -> Mesh:
	if _death_meshes.has(model):
		return _death_meshes[model]
	_death_meshes[model] = null
	var r: Dictionary = models.loader.load_model(model)
	var root: Node3D = r.get("root")
	if root == null:
		push_warning("death model %s: %s" % [model, r.get("error", "?")])
		return null
	for n in root.find_children("*", "AnimationPlayer", true, false):
		n.free()
	for n in root.find_children("*", "Node3D", true, false):
		if (n.get_meta("extras", {}) as Dictionary).get("hidden", false):
			(n as Node3D).visible = false
	var merger := UnitBatches.new()
	merger.animate = false
	merger.vertex_lighting = quality == "low"
	var built: Dictionary = merger._build_mesh({"root": root, "anims": {}})
	merger.free()
	root.free()
	_death_meshes[model] = built.get("mesh")
	return _death_meshes[model]


func _add_corpse_node(def: int, bits: int, pos: Vector3, facing: float, rot_time: float, sink_time: float) -> void:
	var p := models.proto(def)
	if not p.has("root"):
		return
	var anims: Dictionary = p.anims
	var dies: Array = []
	for n in UnitModels.DIE_CLIPS:
		if anims.has(n):
			dies.append(n)
	if dies.is_empty():
		return
	var node := Node3D.new()
	var m := (p.root as Node3D).duplicate() as Node3D
	m.rotation.y = UnitViews.MODEL_YAW
	node.add_child(m)
	var ap := AnimationPlayer.new()
	m.add_child(ap)
	ap.root_node = ap.get_path_to(m)
	var lib := AnimationLibrary.new()
	var clip: String = dies[bits % dies.size()]
	lib.add_animation(clip, anims[clip])
	ap.add_animation_library("", lib)
	_add_body(node, pos, facing, rot_time, sink_time)
	ap.play(clip)
	stats.corpses += 1


func _add_body(node: Node3D, pos: Vector3, facing: float, rot_time: float, sink_time: float) -> void:
	while _bodies.size() >= int(BUDGET[quality].bodies):
		_drop_body(0)
	add_child(node)
	node.position = pos
	node.rotation.y = -facing
	_bodies.append({"node": node, "until_rot": clock + rot_time, "until_gone": clock + rot_time + maxf(sink_time, 0.1), "y": pos.y})


func _drop_body(i: int) -> void:
	(_bodies[i].node as Node).queue_free()
	_bodies.remove_at(i)


static func _index(snap: Dictionary, bits: int) -> int:
	if bits == 0 or not snap.has("id"):
		return -1
	return (snap.id as PackedInt64Array).find(bits)
