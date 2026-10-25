extends Node
## GPU pacing guard for the GL Compatibility renderer (autoload `GpuPacer`, ARCHITECTURE §4.4).
##
## Godot's GLES3 2D renderer (4.6.3 to 4.8-beta1) crashes when the GPU runs far behind the CPU.
## At the start of each 2D pass it reuses a canvas instance buffer from three frames back: if that
## buffer's fence isn't signalled within 100 ms it reuses it anyway, then writes it through
## glMapBufferRange and memcpy without checking the pointer. Through ANGLE (D3D11) on a weak GPU,
## with frames slower than ~100 ms, the map fails and the memcpy writes through null
## (RasterizerCanvasGLES3::_render_items).
##
## The guard keeps the GPU from running behind when frames are slow: while the smoothed frame
## time is above SLOW_MS it reads back a 1x1 SubViewport at the start of each frame, which waits
## for the GPU to finish everything queued so far. At normal frame rates it does nothing.
## While it drains, CPU and GPU no longer overlap, so a frame takes their sum; it judges the frame
## time the game would have without draining instead: the larger of the CPU part and the GPU
## wait. (Judging the drained frame time kept it on after a slow loading frame, halving Low's fps.)
## It starts on and engages at once on any single slow frame; it releases only once the smoothed
## estimate is fast again. (Waiting for the average to climb left the first heavy frames after a
## load undrained: 1 crash in 6.)
## It is off on the RenderingDevice renderers (Forward+, Mobile), which don't use that code, and
## with `--no-gpu-pacer` (to reproduce the crash).

# The crash needs GPU frames well over 100 ms (seen at 170-250 ms, never at 100 ms or less). The
# release threshold sits above Medium's 73-79 ms on the dev laptop, so a slow loading frame
# doesn't leave it draining there (it did with 70 ms: 13.4 fps fell to 10.5).
const SLOW_MS := 95.0       # engage above this smoothed undrained frame time
const FAST_MS := 85.0       # release below this

var active := true           # draining this frame (on until the first frames prove fast)
var enabled := false
var drains := 0              # frames drained so far
var estimate_ms := SLOW_MS   # smoothed frame time without draining
var _probe: SubViewport
var _last_us := 0
var _wait_us := 0            # how long the last drain waited for the GPU (0: no drain)


func _ready() -> void:
	process_priority = -1000  # before every other _process: the drain covers the previous frames
	enabled = RenderingServer.get_current_rendering_method() == "gl_compatibility" \
		and not "--no-gpu-pacer" in OS.get_cmdline_user_args()
	if not enabled:
		set_process(false)
		return
	_probe = SubViewport.new()
	_probe.name = "GpuPacerProbe"
	_probe.size = Vector2i(1, 1)
	_probe.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_probe.transparent_bg = true
	add_child(_probe)


func _process(_delta: float) -> void:
	var now := Time.get_ticks_usec()
	if _last_us > 0:
		var frame := (now - _last_us) / 1000.0
		if _wait_us > 0:
			var wait := _wait_us / 1000.0
			frame = maxf(frame - wait, wait)  # undrained: CPU and GPU overlap again
		estimate_ms = lerpf(estimate_ms, frame, 0.2)
		active = estimate_ms > FAST_MS if active else (frame > SLOW_MS or estimate_ms > SLOW_MS)
	_last_us = now
	_wait_us = 0
	if active:
		_probe.get_texture().get_image()  # blocks until the GPU has drawn every frame queued so far
		_wait_us = maxi(Time.get_ticks_usec() - now, 1)
		drains += 1
