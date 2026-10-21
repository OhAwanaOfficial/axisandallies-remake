extends Node3D
## Battle view (M4 part 3): the sim through SimBridge, terrain from the map, unit models with render
## interpolation, RTS camera, selection and move orders. The view only ever *queues commands*.
##
## Controls
##   Left click / drag: select regiments (click a unit, or box). Shift adds. Double-click: all
##     visible regiments of that type. Ctrl+1..9 saves the selection, 1..9 recalls it (twice: jump).
##   Right click: move. Right drag: move and face the drag direction. Alt+right click: Run.
##   Q / W / E: Assault / Normal / Column.  V: group behaviour (division) on/off.  S: stop.
##   B: drop a debug blocker at the cursor.  Space: pause.  + / -: sim speed.
##   Camera: arrows or screen edges pan, wheel zooms, middle drag or PgUp/PgDn rotates, Home resets.
##   F1 cost grid (selected regiment's class), F2 paths, F3 slot targets and anchors, F4 HUD.
##
## Command line (after `--`): --map=PATH.json | --gen=SEED:SIZE, --seed=N, --cache=DIR,
##   --screenshot=PATH (renders after --frames=N frames and quits), --demo (orders a move at start),
##   --zoom=DIST, --select-all, --cost / --paths / --slots (overlays on at start),
##   --selftest (drives the controls with synthetic input events, prints PASS/FAIL and quits),
##   --regiments=N (N mixed regiments, half per side, crossing the map in groups of six),
##   --measure=FRAMES (after a warm-up, prints fps and sim step time over FRAMES frames and quits),
##   --pause-sim (with --regiments: freeze the sim after 20 ticks to measure rendering alone).

const TICK := 0.1
const PLAYER := 0
const FORMATIONS := {KEY_Q: "regiment_aggressive", KEY_W: "regiment_defensive", KEY_E: "regiment_column"}
const FORMATION_LABELS := {"regiment_aggressive": "Assault", "regiment_defensive": "Normal", "regiment_column": "Column"}
const DOUBLE_CLICK_MS := 350
const DRAG_PX := 6.0
const CLASS_NAMES := ["foot", "wheeled", "tracked", "naval", "amphibious", "air"]
const DEMO_REGIMENTS := ["regiment_per_infantry", "regiment_usa_infantry", "regiment_ger_tank_medium",
	"regiment_gbr_tank_heavy", "regiment_usa_halftrack", "regiment_gbr_infantry_at", "regiment_per_infantry"]
const ENEMY_REGIMENTS := ["regiment_ger_infantry", "regiment_ger_tank_medium", "regiment_ger_infantry"]

var bridge := SimBridge.new()
var loader: CacheLoader
var terrain: BattleTerrain
var units: UnitViews
var cam: RtsCamera
var hud: Label
var box_rect: ColorRect
var overlay_root: Node3D

var snap := {}
var accum := 0.0
var speed := 1.0
var paused := false
var selected := {}            # regiment bits -> true
var groups := {}              # 1..9 -> Array of regiment bits
var last_group_key := -1
var last_group_time := 0
var regiment_defs := {}       # regiment bits -> regiment def index
var events_log: Array[String] = []
var overlay := {"cost": false, "paths": false, "slots": false, "hud": true}
var cost_class := -1
var cost_mesh: MeshInstance3D
var lines: MeshInstance3D
var slot_marks: MultiMeshInstance3D

var _left_down := false
var _left_from := Vector2.ZERO
var _right_down := false
var _right_from_screen := Vector2.ZERO
var _right_from_ground: Variant = null
var _last_click_ms := 0
var _frames := 0


func _ready() -> void:
	var args := _args()
	loader = CacheLoader.new(args.get("cache", ""))
	if loader.load_manifest() != OK:
		push_warning("cache/manifest.json not found in %s: units fall back to boxes, terrain to colours" % loader.cache_dir)
	var game := _game_dir()
	var map_spec := _map_spec(args)
	# Loading the TGI data and the map takes seconds: show that before blocking.
	var loading_layer := CanvasLayer.new()
	var loading := Label.new()
	loading.text = "Loading game data and map %s ..." % map_spec.get_file()
	loading.position = Vector2(20, 20)
	loading_layer.add_child(loading)
	add_child(loading_layer)
	for i in 3:
		await get_tree().process_frame
	var t0 := Time.get_ticks_msec()
	if not bridge.start(game, map_spec, int(args.get("seed", "1"))):
		_fatal("SimBridge.start failed: %s (game %s, map %s)" % [bridge.last_error(), game, map_spec])
		return
	var map: Dictionary = bridge.map_info()
	print("map %s (%dx%d cells, biome %s) loaded in %d ms" % [map.name, map.width, map.height, map.biome, Time.get_ticks_msec() - t0])

	_build_scene(map)
	if args.has("regiments"):
		_spawn_many(int(args.regiments))
	else:
		_spawn_demo(args.has("demo"))
	snap = bridge.snapshot()
	loading_layer.queue_free()
	if args.has("selftest"):
		_selftest()


func _build_scene(map: Dictionary) -> void:
	var env := WorldEnvironment.new()
	var e := Environment.new()
	e.background_mode = Environment.BG_COLOR
	e.background_color = Color(0.55, 0.68, 0.82)
	e.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	e.ambient_light_color = Color(0.55, 0.58, 0.62)
	e.ambient_light_energy = 0.45
	e.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	e.tonemap_white = 1.6
	env.environment = e
	add_child(env)
	var sun := DirectionalLight3D.new()
	sun.rotation = Vector3(deg_to_rad(-55), deg_to_rad(35), 0)
	sun.light_energy = 0.75
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 250.0
	add_child(sun)

	terrain = BattleTerrain.new()
	terrain.name = "Terrain"
	add_child(terrain)
	terrain.build(map, bridge.terrain_table(map.biome), loader)
	print(terrain.info)

	units = UnitViews.new()
	units.name = "Units"
	add_child(units)
	units.setup(loader, bridge, terrain)

	overlay_root = Node3D.new()
	overlay_root.name = "Overlays"
	add_child(overlay_root)
	lines = MeshInstance3D.new()
	lines.mesh = ImmediateMesh.new()
	var lm := StandardMaterial3D.new()
	lm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	lm.vertex_color_use_as_albedo = true
	lm.no_depth_test = true
	lines.material_override = lm
	overlay_root.add_child(lines)
	slot_marks = MultiMeshInstance3D.new()
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	var sphere := SphereMesh.new()
	sphere.radius = 0.25
	sphere.height = 0.5
	sphere.radial_segments = 8
	sphere.rings = 4
	mm.mesh = sphere
	slot_marks.multimesh = mm
	var sm := StandardMaterial3D.new()
	sm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	sm.vertex_color_use_as_albedo = true
	slot_marks.material_override = sm
	overlay_root.add_child(slot_marks)

	cam = RtsCamera.new()
	cam.name = "Camera"
	add_child(cam)

	var layer := CanvasLayer.new()
	add_child(layer)
	hud = Label.new()
	hud.position = Vector2(10, 8)
	hud.add_theme_color_override("font_color", Color.WHITE)
	hud.add_theme_color_override("font_outline_color", Color.BLACK)
	hud.add_theme_constant_override("outline_size", 4)
	layer.add_child(hud)
	box_rect = ColorRect.new()
	box_rect.color = Color(0.4, 0.8, 1.0, 0.18)
	box_rect.visible = false
	box_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	layer.add_child(box_rect)


func _spawn_demo(order_move: bool) -> void:
	var w := float(terrain.cells.x)
	var h := float(terrain.cells.y)
	var west: Vector2 = bridge.find_open_area(Vector2(28, h / 2), 20)
	var east: Vector2 = bridge.find_open_area(Vector2(w - 28, h / 2), 12)
	if west.x < 0:
		west = Vector2(w * 0.2, h * 0.5)
	if east.x < 0:
		east = Vector2(w * 0.8, h * 0.5)
	var facing := (east - west).angle()
	var spots := [Vector2(-8, -14), Vector2(8, -14), Vector2(-8, 0), Vector2(8, 0), Vector2(-8, 14), Vector2(8, 14),
		Vector2(-22, 0)]
	for i in DEMO_REGIMENTS.size():
		bridge.queue_spawn(DEMO_REGIMENTS[i], west + spots[i].rotated(facing), facing, PLAYER)
	for i in ENEMY_REGIMENTS.size():
		bridge.queue_spawn(ENEMY_REGIMENTS[i], east + Vector2(0, -12 + 12 * i).rotated(facing), facing + PI, 1)
	bridge.step()
	_learn_regiments()
	cam.setup(terrain, west + (east - west) * 0.15)
	var args := _args()
	if args.has("zoom"):
		cam.distance = float(args.zoom)
		cam.focus_on(west)
	if args.has("select-all"):
		for bits in regiment_defs:
			if _player_of(bits) == PLAYER:
				selected[bits] = true
		units.set_selected(selected)
	for key in ["cost", "paths", "slots"]:
		if args.has(key):
			overlay[key] = true
	if order_move:
		var mine := PackedInt64Array()
		for bits in regiment_defs:
			if _player_of(bits) == PLAYER:
				mine.append(bits)
		bridge.queue_move(mine, (west + east) * 0.5, false, false, 0.0, PLAYER)


const MANY_TYPES := ["regiment_ger_infantry", "regiment_ger_tank_medium", "regiment_usa_infantry",
	"regiment_usa_halftrack", "regiment_gbr_tank_heavy", "regiment_gbr_infantry_at", "regiment_per_infantry",
	"regiment_usa_tank_light"]


## Performance setup (M4 close-out): like `simcli perf`, open spots on a 22-cell lattice, the
## west third for player 0 and the east third for player 1, then groups of six cross over.
func _spawn_many(n: int) -> void:
	var w := terrain.cells.x
	var h := terrain.cells.y
	var foot: PackedByteArray = bridge.cost_grid(0)
	var wheels: PackedByteArray = bridge.cost_grid(1)
	var open := func(x: int, y: int) -> bool:
		for dy in range(-7, 8, 7):
			for dx in range(-7, 8, 7):
				var i := (y + dy) * w + x + dx
				if foot[i] == 0 or wheels[i] == 0:
					return false
		return true
	var west: Array[Vector2] = []
	var east: Array[Vector2] = []
	for y in range(22, h - 22, 22):
		for x in range(22, w - 22, 22):
			if open.call(x, y):
				if x < w / 3:
					west.append(Vector2(x + 0.5, y + 0.5))
				elif x > w * 2 / 3:
					east.append(Vector2(x + 0.5, y + 0.5))
	var half := n / 2
	for i in n:
		var side := west if i < half else east
		var k := i if i < half else i - half
		var count := half if i < half else n - half
		if side.is_empty():
			continue
		var at: Vector2 = side[k * side.size() / count]
		bridge.queue_spawn(MANY_TYPES[i % MANY_TYPES.size()], at, 0.0 if i < half else PI, 0 if i < half else 1)
	bridge.step()
	_learn_regiments()
	for p in [0, 1]:
		var mine := PackedInt64Array()
		var keys := regiment_defs.keys()
		keys.sort()
		for bits in keys:
			if _player_of(bits) == p:
				mine.append(bits)
		for s0 in range(0, mine.size(), 6):
			var grp := mine.slice(s0, s0 + 6)
			var a: Vector2 = bridge.regiment_info(grp[0]).anchor
			bridge.queue_move(grp, Vector2(w - a.x, a.y), false, false, 0.0, p)
	cam.setup(terrain, Vector2(w * 0.5, h * 0.5))
	cam.distance = 160.0
	print("spawned %d regiments" % regiment_defs.size())
	if _args().has("pause-sim"):
		# Render-only measurement: let the orders start, then freeze the sim (the views keep
		# interpolating and syncing every frame).
		for i in 20:
			bridge.step()
		snap = bridge.snapshot()
		paused = true


var _measure := {"frames": 0, "frame_ms": [], "step_ms": [], "steps": 0}


func _measure_frame(delta: float, step_ms: float, steps: int) -> void:
	var want := int(_args().measure)
	_measure.frames += 1
	if _measure.frames <= 60:  # warm-up: shaders, first paths
		return
	(_measure.frame_ms as Array).append(delta * 1000.0)
	if steps > 0:
		(_measure.step_ms as Array).append(step_ms / steps)
	if (_measure.frame_ms as Array).size() >= want:
		var f: Array = _measure.frame_ms
		var st: Array = _measure.step_ms
		f.sort()
		var total := 0.0
		for x in f:
			total += x
		var stotal := 0.0
		for x in st:
			stotal += x
		st.sort()
		print("MEASURE %d regiments, %d units, %d frames: avg %.1f fps (frame %.1f ms, p95 %.1f ms); sim step avg %.2f ms, max %.2f ms; renderer %s" % [
			regiment_defs.size(), units.stats.units, f.size(), 1000.0 / (total / f.size()), total / f.size(),
			f[int(f.size() * 0.95)], stotal / maxi(st.size(), 1), st.back() if not st.is_empty() else 0.0,
			RenderingServer.get_video_adapter_name()])
		get_tree().quit()


# ---------------------------------------------------------------- loop

func _process(delta: float) -> void:
	if snap.is_empty():
		return
	var steps := 0
	var step_us := 0
	if not paused:
		accum += delta * speed
		while accum >= TICK and steps < 8:
			var t0 := Time.get_ticks_usec()
			bridge.step()
			step_us += Time.get_ticks_usec() - t0
			accum -= TICK
			steps += 1
		if steps > 0:
			snap = bridge.snapshot()
			_learn_regiments()
			_drain_events()
		if steps == 8:
			accum = 0.0
	var alpha := clampf(accum / TICK, 0.0, 1.0) if not paused else 1.0
	units.sync(snap, alpha)
	units.set_selected(selected)
	_update_overlays()
	_update_hud()
	_frames += 1
	var args := _args()
	if args.has("measure"):
		_measure_frame(delta, step_us / 1000.0, steps)
	if args.has("screenshot") and not args.has("selftest") and _frames == int(args.get("frames", "120")):
		_save_screenshot(str(args.screenshot))


func _learn_regiments() -> void:
	var ids: PackedInt64Array = snap.get("id", PackedInt64Array())
	if ids.is_empty():
		snap = bridge.snapshot()
		ids = snap.id
	var live := {}
	for i in ids.size():
		if snap.kind[i] == 1:
			live[ids[i]] = true
			if not regiment_defs.has(ids[i]):
				regiment_defs[ids[i]] = snap.def[i]
	for bits in regiment_defs.keys():
		if not live.has(bits):
			regiment_defs.erase(bits)
			selected.erase(bits)


func _drain_events() -> void:
	for e in bridge.drain_events():
		if e.type in ["arrived", "blocked", "rejected"]:
			var name: String = bridge.regiment_def(regiment_defs.get(e.entity, -1)).get("ids", "#%d" % e.entity) if e.type != "rejected" else "command %d" % e.command
			events_log.append("t%d %s %s" % [e.tick, e.type, name])
	while events_log.size() > 6:
		events_log.pop_front()


# ---------------------------------------------------------------- input

func _unhandled_input(ev: InputEvent) -> void:
	if snap.is_empty():
		return  # still loading
	if ev is InputEventMouseButton:
		var mb := ev as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_LEFT:
			if mb.pressed:
				_left_down = true
				_left_from = mb.position
			elif _left_down:
				_left_down = false
				box_rect.visible = false
				if mb.position.distance_to(_left_from) > DRAG_PX:
					_select_box(Rect2(_left_from, Vector2.ZERO).expand(mb.position), mb.shift_pressed)
				else:
					var now := Time.get_ticks_msec()
					_select_click(mb.position, mb.shift_pressed, now - _last_click_ms < DOUBLE_CLICK_MS)
					_last_click_ms = now
		elif mb.button_index == MOUSE_BUTTON_RIGHT:
			if mb.pressed:
				_right_down = true
				_right_from_screen = mb.position
				_right_from_ground = _ground_at(mb.position)
			elif _right_down:
				_right_down = false
				_order_move(mb.position, mb.alt_pressed)
	elif ev is InputEventMouseMotion and _left_down:
		var m := (ev as InputEventMouseMotion).position
		if m.distance_to(_left_from) > DRAG_PX:
			box_rect.visible = true
			var r := Rect2(_left_from, Vector2.ZERO).expand(m)
			box_rect.position = r.position
			box_rect.size = r.size
	elif ev is InputEventKey and ev.pressed and not ev.echo:
		_key(ev as InputEventKey)


func _key(k: InputEventKey) -> void:
	var regs := _selected_ids()
	if FORMATIONS.has(k.keycode) and not k.ctrl_pressed:
		if not regs.is_empty():
			bridge.queue_formation(regs, FORMATIONS[k.keycode], PLAYER)
		return
	match k.keycode:
		KEY_V:
			if regs.size() >= 1:
				var info: Dictionary = bridge.regiment_info(regs[0])
				bridge.queue_group_behaviour(regs, not info.get("division", false), PLAYER)
		KEY_S:
			if not regs.is_empty():
				bridge.queue_stop(regs, PLAYER)
		KEY_B:
			var g: Variant = _ground_at(get_viewport().get_mouse_position())
			if g != null:
				bridge.queue_blocker(g, Vector2(2, 2), PLAYER)
		KEY_SPACE:
			paused = not paused
		KEY_EQUAL, KEY_KP_ADD:
			speed = minf(speed * 2.0, 8.0)
		KEY_MINUS, KEY_KP_SUBTRACT:
			speed = maxf(speed / 2.0, 0.25)
		KEY_F1:
			overlay.cost = not overlay.cost
			cost_class = -1
		KEY_F2:
			overlay.paths = not overlay.paths
		KEY_F3:
			overlay.slots = not overlay.slots
		KEY_F4:
			overlay.hud = not overlay.hud
		_:
			if k.keycode >= KEY_1 and k.keycode <= KEY_9:
				var n := k.keycode - KEY_0
				if k.ctrl_pressed:
					groups[n] = selected.keys()
				elif groups.has(n):
					selected.clear()
					for bits in groups[n]:
						if regiment_defs.has(bits):
							selected[bits] = true
					units.set_selected(selected)
					var now := Time.get_ticks_msec()
					if last_group_key == n and now - last_group_time < 400:
						_jump_to_selection()
					last_group_key = n
					last_group_time = now


func _select_click(at: Vector2, add: bool, double: bool) -> void:
	var hit := _unit_at(at)
	if not add:
		selected.clear()
	if hit != 0:
		if double and regiment_defs.has(hit):
			# Every own regiment of the same type with a unit on screen.
			var def: int = regiment_defs[hit]
			var rect := get_viewport().get_visible_rect()
			for i in (snap.id as PackedInt64Array).size():
				if snap.kind[i] != 0 or snap.player[i] != PLAYER:
					continue
				var org: int = snap.org[i]
				if regiment_defs.get(org, -2) == def and _on_screen(snap.cur_pos[i], rect):
					selected[org] = true
		elif add and selected.has(hit):
			selected.erase(hit)
		else:
			selected[hit] = true
	units.set_selected(selected)


func _select_box(r: Rect2, add: bool) -> void:
	if not add:
		selected.clear()
	for i in (snap.id as PackedInt64Array).size():
		if snap.kind[i] != 0 or snap.player[i] != PLAYER:
			continue
		var p := cam.camera.unproject_position(terrain.to_world(snap.cur_pos[i], 0.6))
		if r.has_point(p) and not cam.camera.is_position_behind(terrain.to_world(snap.cur_pos[i])):
			selected[snap.org[i]] = true
	units.set_selected(selected)


## The own regiment whose unit is nearest the cursor (within 24 px), or 0.
func _unit_at(at: Vector2) -> int:
	var best := 24.0
	var hit := 0
	for i in (snap.id as PackedInt64Array).size():
		if snap.kind[i] != 0 or snap.player[i] != PLAYER:
			continue
		var w := terrain.to_world(snap.cur_pos[i], 0.6)
		if cam.camera.is_position_behind(w):
			continue
		var d := cam.camera.unproject_position(w).distance_to(at)
		if d < best:
			best = d
			hit = snap.org[i]
	return hit


func _order_move(release: Vector2, run: bool) -> void:
	var regs := _selected_ids()
	if regs.is_empty() or _right_from_ground == null:
		return
	var goal: Vector2 = _right_from_ground
	var end: Variant = _ground_at(release)
	var has_facing := false
	var facing := 0.0
	if end != null and release.distance_to(_right_from_screen) > DRAG_PX * 2 and (end as Vector2).distance_to(goal) > 2.0:
		has_facing = true
		facing = ((end as Vector2) - goal).angle()  # sim angle: atan2(dy, dx) in sim (x, y)
	bridge.queue_move(regs, goal, run, has_facing, facing, PLAYER)


func _selected_ids() -> PackedInt64Array:
	var out := PackedInt64Array()
	var keys := selected.keys()
	keys.sort()
	for bits in keys:
		out.append(bits)
	return out


func _ground_at(screen: Vector2) -> Variant:
	if cam == null or cam.camera == null:
		return null
	return terrain.pick(cam.camera.project_ray_origin(screen), cam.camera.project_ray_normal(screen))


func _on_screen(p: Vector2, rect: Rect2) -> bool:
	var w := terrain.to_world(p)
	return not cam.camera.is_position_behind(w) and rect.has_point(cam.camera.unproject_position(w))


func _jump_to_selection() -> void:
	var sum := Vector2.ZERO
	var n := 0
	for bits in selected:
		var info: Dictionary = bridge.regiment_info(bits)
		if not info.is_empty():
			sum += info.anchor
			n += 1
	if n > 0:
		cam.focus_on(sum / n)


func _player_of(bits: int) -> int:
	return int(bridge.regiment_info(bits).get("player", -1))


# ---------------------------------------------------------------- overlays and HUD

func _update_overlays() -> void:
	var im: ImmediateMesh = lines.mesh
	im.clear_surfaces()
	var regs := _selected_ids()
	# F1: cost grid of the first selected regiment's class (foot when nothing is selected).
	if overlay.cost:
		var cls := 0
		if not regs.is_empty():
			cls = int(bridge.regiment_info(regs[0]).get("move_class", 0))
		cls = mini(cls, 4)
		if cost_mesh == null or cls != cost_class or snap.get("tick", 0) % 10 == 0:
			_build_cost_overlay(cls)
		cost_mesh.visible = true
	elif cost_mesh:
		cost_mesh.visible = false
	var marks: Array[Transform3D] = []
	var mark_colors: Array[Color] = []
	if overlay.paths or overlay.slots:
		var subjects := regs if not regs.is_empty() else PackedInt64Array(regiment_defs.keys())
		var drew := false
		for bits in subjects:
			var info: Dictionary = bridge.regiment_info(bits)
			if info.is_empty():
				continue
			var col: Color = UnitViews.PLAYER_COLORS[int(info.player) % UnitViews.PLAYER_COLORS.size()]
			if overlay.paths:
				var path: PackedVector2Array = info.path
				if path.size() >= 2:
					if not drew:
						im.surface_begin(Mesh.PRIMITIVE_LINES)
						drew = true
					var from: Vector2 = info.anchor
					for k in range(int(info.waypoint), path.size()):
						_line(im, from, path[k], Color.YELLOW)
						from = path[k]
					for k in range(1, path.size()):
						_line(im, path[k - 1], path[k], Color(1, 1, 0.3, 0.35))
			if overlay.slots:
				var slots: PackedVector2Array = info.slots
				for s in slots:
					marks.append(Transform3D(Basis(), terrain.to_world(s, 0.3)))
					mark_colors.append(col.lightened(0.5))
				marks.append(Transform3D(Basis().scaled(Vector3(2, 2, 2)), terrain.to_world(info.anchor, 0.5)))
				mark_colors.append(Color.WHITE)
				if not drew:
					im.surface_begin(Mesh.PRIMITIVE_LINES)
					drew = true
				var heading := Vector2.from_angle(float(info.facing)) * 4.0
				_line(im, info.anchor, (info.anchor as Vector2) + heading, Color.WHITE)
		if drew:
			im.surface_end()
	var mm := slot_marks.multimesh
	mm.instance_count = marks.size()
	for i in marks.size():
		mm.set_instance_transform(i, marks[i])
		mm.set_instance_color(i, mark_colors[i])


func _line(im: ImmediateMesh, a: Vector2, b: Vector2, c: Color) -> void:
	im.surface_set_color(c)
	im.surface_add_vertex(terrain.to_world(a, 0.4))
	im.surface_set_color(c)
	im.surface_add_vertex(terrain.to_world(b, 0.4))


func _build_cost_overlay(cls: int) -> void:
	cost_class = cls
	var costs: PackedByteArray = bridge.cost_grid(cls)
	var w := terrain.cells.x
	var h := terrain.cells.y
	var img := Image.create(w, h, false, Image.FORMAT_RGBA8)
	for y in h:
		for x in w:
			var c := costs[y * w + x]
			var col: Color
			if c == 0:
				col = Color(0.9, 0.1, 0.1, 0.55)
			elif c <= 12:
				col = Color(0.2, 0.6, 1.0, 0.35)  # cheaper than ground (roads)
			elif c <= 16:
				col = Color(0.2, 0.9, 0.3, 0.18)
			elif c <= 24:
				col = Color(1.0, 0.85, 0.2, 0.35)
			else:
				col = Color(1.0, 0.5, 0.1, 0.45)
			img.set_pixel(x, y, col)
	if cost_mesh == null:
		cost_mesh = MeshInstance3D.new()
		cost_mesh.mesh = terrain.mesh_instance.mesh
		cost_mesh.position.y = 0.05
		cost_mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		var mat := ShaderMaterial.new()
		mat.shader = load("res://scripts/battle/cost_overlay.gdshader")
		cost_mesh.material_override = mat
		overlay_root.add_child(cost_mesh)
	var m: ShaderMaterial = cost_mesh.material_override
	m.set_shader_parameter("cost_tex", ImageTexture.create_from_image(img))
	m.set_shader_parameter("cell_count", Vector2(w, h))


func _update_hud() -> void:
	hud.visible = overlay.hud
	if not overlay.hud:
		return
	var lines_out: Array[String] = []
	lines_out.append("tick %d  %s  x%s  %d fps  %d units%s" % [snap.get("tick", 0), "PAUSED" if paused else "", str(speed),
		Engine.get_frames_per_second(), units.stats.units, "  (%d placeholders)" % units.stats.placeholders if units.stats.placeholders > 0 else ""])
	var shown := 0
	for bits in _selected_ids():
		if shown >= 6:
			lines_out.append("  ... %d more" % (selected.size() - shown))
			break
		var info: Dictionary = bridge.regiment_info(bits)
		if info.is_empty():
			continue
		var ids: String = bridge.regiment_def(int(info.def)).get("ids", "?")
		lines_out.append("  %s  %s  %s%s%s  %s%s%s" % [ids, FORMATION_LABELS.get(info.formation, info.formation), info.state,
			"  run" if info.running else "", "  division" if info.division else ("  group" if int(info.group) != 0 else ""),
			CLASS_NAMES[int(info.move_class)], ["", "  column", "  single file"][int(info.compression)], "  BLOCKED" if info.blocked else ""])
		shown += 1
	for e in events_log:
		lines_out.append("  " + e)
	lines_out.append("F1 cost  F2 paths  F3 slots  F4 HUD   Q/W/E formation  V division  S stop  Alt+RMB run  B blocker")
	hud.text = "\n".join(lines_out)


# ---------------------------------------------------------------- setup helpers

func _args() -> Dictionary:
	var out := {}
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--"):
			var kv := a.trim_prefix("--").split("=", true, 1)
			out[kv[0]] = kv[1] if kv.size() > 1 else "1"
	return out


func _game_dir() -> String:
	var env := OS.get_environment("CORPS_GAME")
	if env != "":
		return env
	var cfg := ProjectSettings.globalize_path("res://").path_join("../config/game_path.cfg").simplify_path()
	if FileAccess.file_exists(cfg):
		for line in FileAccess.get_file_as_string(cfg).split("\n"):
			var t := line.strip_edges()
			if t != "" and not t.begins_with("#") and not t.begins_with(";"):
				return t.trim_prefix("\"").trim_suffix("\"")
	return ""


func _map_spec(args: Dictionary) -> String:
	if args.has("map"):
		return str(args.map)
	if args.has("gen"):
		return "gen:" + str(args.gen)
	var gf := loader.cache_dir.path_join("maps/golden_fields/map.json")
	return gf if FileAccess.file_exists(gf) else "gen:7:256"


func _fatal(msg: String) -> void:
	push_error(msg)
	var l := Label.new()
	l.text = msg
	l.position = Vector2(20, 20)
	var layer := CanvasLayer.new()
	add_child(layer)
	layer.add_child(l)


func _save_screenshot(path: String) -> void:
	await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(path)
	print("screenshot: ", path)
	get_tree().quit()


# ---------------------------------------------------------------- self test (synthetic input)

func _selftest() -> void:
	var results: Array[String] = []
	var fails := 0
	var check := func(name: String, ok: bool) -> void:
		results.append(("PASS  " if ok else "FAIL  ") + name)
		if not ok:
			fails += 1
	for i in 10:
		await get_tree().process_frame
	var mine: Array = []
	for bits in regiment_defs:
		if _player_of(bits) == PLAYER:
			mine.append(bits)
	mine.sort()
	var first: int = mine[0]
	var screen_of := func(reg: int) -> Vector2:
		var info: Dictionary = bridge.regiment_info(reg)
		var slots: PackedVector2Array = info.slots
		return cam.camera.unproject_position(terrain.to_world(slots[0] if slots.size() > 0 else info.anchor, 0.6))
	var click := func(at: Vector2, button := MOUSE_BUTTON_LEFT, shift := false, alt := false) -> void:
		for pressed in [true, false]:
			var ev := InputEventMouseButton.new()
			ev.button_index = button
			ev.pressed = pressed
			ev.position = at
			ev.global_position = at
			ev.shift_pressed = shift
			ev.alt_pressed = alt
			Input.parse_input_event(ev)
			await get_tree().process_frame
	var key := func(code: Key, ctrl := false) -> void:
		for pressed in [true, false]:
			var ev := InputEventKey.new()
			ev.keycode = code
			ev.physical_keycode = code
			ev.pressed = pressed
			ev.ctrl_pressed = ctrl
			Input.parse_input_event(ev)
			await get_tree().process_frame
	var settle := func(ticks: int) -> void:
		var until: int = int(snap.tick) + ticks
		while int(snap.get("tick", 0)) < until:
			await get_tree().process_frame

	# Click selects the regiment of the unit under the cursor.
	await click.call(screen_of.call(first))
	check.call("click selects one regiment", selected.size() == 1 and selected.has(first))
	# Shift-click adds another.
	await click.call(screen_of.call(mine[1]), MOUSE_BUTTON_LEFT, true)
	check.call("shift-click adds", selected.size() == 2)
	# Double-click: every visible own regiment of that type.
	_last_click_ms = 0
	await click.call(screen_of.call(first))
	await click.call(screen_of.call(first))
	var same := 0
	for bits in mine:
		if regiment_defs[bits] == regiment_defs[first]:
			same += 1
	check.call("double-click selects the type (%d)" % same, selected.size() == same and same >= 2)
	# Box over the whole view.
	var size := get_viewport().get_visible_rect().size
	for step in [[true, Vector2(5, 5)], [null, size - Vector2(5, 5)], [false, size - Vector2(5, 5)]]:
		var ev: InputEvent
		if step[0] == null:
			ev = InputEventMouseMotion.new()
		else:
			ev = InputEventMouseButton.new()
			ev.button_index = MOUSE_BUTTON_LEFT
			ev.pressed = step[0]
		ev.position = step[1]
		Input.parse_input_event(ev)
		await get_tree().process_frame
	var visible_regs := 0
	for bits in mine:
		if _on_screen((bridge.regiment_info(bits).slots as PackedVector2Array)[0], get_viewport().get_visible_rect()):
			visible_regs += 1
	check.call("box selects the visible regiments (%d)" % visible_regs, selected.size() == visible_regs and visible_regs > 0)
	# Ctrl+1 stores, click on nothing clears, 1 recalls.
	var stored := selected.size()
	await key.call(KEY_1, true)
	await click.call(Vector2(size.x - 20, size.y * 0.5))
	var cleared := selected.is_empty()
	await key.call(KEY_1)
	check.call("Ctrl+1 / 1 store and recall a selection", cleared and selected.size() == stored)
	# E: Column for the selection.
	await click.call(screen_of.call(first))
	await key.call(KEY_E)
	await settle.call(2)
	check.call("E sets Column", bridge.regiment_info(first).formation == "regiment_column")
	await key.call(KEY_Q)
	await settle.call(2)
	check.call("Q sets Assault", bridge.regiment_info(first).formation == "regiment_aggressive")
	# Right click: move order.
	var anchor: Vector2 = bridge.regiment_info(first).anchor
	var target := cam.camera.unproject_position(terrain.to_world(anchor + Vector2(25, 4)))
	await click.call(target, MOUSE_BUTTON_RIGHT)
	await settle.call(3)
	var st: String = bridge.regiment_info(first).state
	check.call("right click orders a move (state %s)" % st, st != "idle")
	var goal: Vector2 = bridge.regiment_info(first).goal
	check.call("the goal is where the click hit the ground", goal.distance_to(anchor + Vector2(25, 4)) < 1.5)
	# Alt+right click: Run (temporary Column).
	await click.call(target, MOUSE_BUTTON_RIGHT, false, true)
	await settle.call(2)
	check.call("Alt+right click runs", bridge.regiment_info(first).running)
	# Overlays.
	await key.call(KEY_F1)
	await key.call(KEY_F2)
	await key.call(KEY_F3)
	await settle.call(2)
	check.call("F1 shows the cost grid", cost_mesh != null and cost_mesh.visible)
	check.call("F3 shows slot targets", slot_marks.multimesh.instance_count > 0)
	check.call("F2 draws the path", (lines.mesh as ImmediateMesh).get_surface_count() > 0)
	# Interpolation: views sit between the previous and current tick.
	var v: Dictionary = units.views.values()[0]
	check.call("unit views exist for every unit", units.views.size() == units.stats.units and units.views.size() > 0)
	var shot := str(_args().get("screenshot", ""))
	if shot != "":
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(shot)
	for r in results:
		print(r)
	print("SELFTEST %s: %d checks, %d failed" % ["PASSED" if fails == 0 else "FAILED", results.size(), fails])
	(v.node as Node3D).get_tree().quit(1 if fails > 0 else 0)
