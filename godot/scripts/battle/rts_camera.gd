class_name RtsCamera
extends Node3D
## RTS camera rig (ARCHITECTURE §4.4): the rig sits on the ground at the focus point; the camera
## orbits it at a fixed pitch. Pan with the arrow keys or the screen edges, zoom with the wheel,
## rotate with the middle mouse button (drag) or Page Up / Page Down. Home resets the rotation.

const EDGE_PX := 10
const PAN_SPEED := 1.1       # × distance per second
const ZOOM_MIN := 12.0
const ZOOM_MAX := 220.0
const ROTATE_SPEED := 1.8    # rad/s with the keys
const PITCH := deg_to_rad(52.0)

var camera: Camera3D
var terrain: BattleTerrain
var distance := 70.0
var yaw := 0.0
var bounds := Vector2(256, 256)
var _rotating := false


func setup(p_terrain: BattleTerrain, focus: Vector2) -> void:
	terrain = p_terrain
	bounds = Vector2(terrain.cells)
	camera = Camera3D.new()
	camera.far = 3000.0
	camera.fov = 45.0
	add_child(camera)
	focus_on(focus)


func focus_on(p: Vector2) -> void:
	position = Vector3(clampf(p.x, 0, bounds.x), 0, clampf(p.y, 0, bounds.y))
	_apply()


func _process(delta: float) -> void:
	var pan := Vector2.ZERO
	if Input.is_key_pressed(KEY_LEFT):
		pan.x -= 1
	if Input.is_key_pressed(KEY_RIGHT):
		pan.x += 1
	if Input.is_key_pressed(KEY_UP):
		pan.y -= 1
	if Input.is_key_pressed(KEY_DOWN):
		pan.y += 1
	var vp := get_viewport()
	if vp and DisplayServer.window_is_focused() and not _rotating:
		var m := vp.get_mouse_position()
		var size := vp.get_visible_rect().size
		if m.x >= 0 and m.y >= 0 and m.x < size.x and m.y < size.y:
			if m.x < EDGE_PX:
				pan.x -= 1
			elif m.x > size.x - EDGE_PX:
				pan.x += 1
			if m.y < EDGE_PX:
				pan.y -= 1
			elif m.y > size.y - EDGE_PX:
				pan.y += 1
	if Input.is_key_pressed(KEY_PAGEUP):
		yaw += ROTATE_SPEED * delta
	if Input.is_key_pressed(KEY_PAGEDOWN):
		yaw -= ROTATE_SPEED * delta
	if pan != Vector2.ZERO:
		var step := pan.normalized() * PAN_SPEED * distance * delta
		# Screen right/forward on the ground for the current yaw.
		var right := Vector2(cos(yaw), -sin(yaw))
		var fwd := Vector2(-sin(yaw), -cos(yaw))
		var p := Vector2(position.x, position.z) + right * step.x - fwd * step.y
		position.x = clampf(p.x, 0, bounds.x)
		position.z = clampf(p.y, 0, bounds.y)
	_apply()


func _unhandled_input(ev: InputEvent) -> void:
	if ev is InputEventMouseButton:
		var mb := ev as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_WHEEL_UP and mb.pressed:
			distance = maxf(ZOOM_MIN, distance * 0.88)
		elif mb.button_index == MOUSE_BUTTON_WHEEL_DOWN and mb.pressed:
			distance = minf(ZOOM_MAX, distance / 0.88)
		elif mb.button_index == MOUSE_BUTTON_MIDDLE:
			_rotating = mb.pressed
	elif ev is InputEventMouseMotion and _rotating:
		yaw -= (ev as InputEventMouseMotion).relative.x * 0.006
	elif ev is InputEventKey and ev.pressed and (ev as InputEventKey).keycode == KEY_HOME:
		yaw = 0.0


func _apply() -> void:
	if terrain:
		position.y = terrain.height_at(position.x, position.z)
	rotation.y = yaw
	camera.position = Vector3(0, sin(PITCH) * distance, cos(PITCH) * distance)
	camera.rotation = Vector3(-PITCH, 0, 0)
