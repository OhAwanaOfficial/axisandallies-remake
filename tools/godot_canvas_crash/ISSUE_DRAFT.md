# Draft: GLES3 canvas renderer writes through a null glMapBufferRange pointer when the GPU runs >100 ms behind

*Godot doesn't accept AI-written issues. Rewrite this in your own words before you file it.*

**Tested versions:**

| Version | Crashed |
|---|---|
| 4.7.2-stable | 3 of 4 runs |
| 4.6.3-stable | 3 of 4 runs |
| 4.8-beta1 | 2 of 4 runs |

`master` has the same code.

**System:** Windows 10, Intel HD Graphics 3000, driver 9.17.10.4459. Compatibility renderer through ANGLE (D3D11, `ANGLE 2.1.1 aaebda1c5a40`). The GPU has only OpenGL 3.1, so the native `opengl3` driver can't be tested.

**Issue description.** It is a segfault in `RasterizerCanvasGLES3::_render_items`. It happens when the GPU is far behind the CPU (frames of 150–250 ms) and a 2D `Label` changes its text every frame.

Symbolized backtrace (4.8-beta1, official debug symbols):

```
msvcrt!memmove
RasterizerCanvasGLES3::_render_items+0x537
RasterizerCanvasGLES3::canvas_render_items+0xf5d
RendererCanvasCull::_render_canvas_item_tree+0x258
RendererCanvasCull::render_canvas+0x102
```

`_render_items` calls `memcpy(buffer, state.instance_data_array, ...)` on the result of `glMapBufferRange(..., GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT)` without checking it for null. The same pattern is in the light UBO upload and the overflow path in `_record_item_commands`.

Before that, `canvas_render_items` reuses a data buffer older than two frames after `glClientWaitSync(fence, 0, 100 ms)`. It ignores whether the wait timed out, so with slow GPU frames it writes, unsynchronized, into a buffer the GPU may still be reading.

What changed the crash rate (4 runs each, 160 regiments of skinned units):

| Change | Crashed |
|---|---|
| None (baseline) | 3/4 |
| Separate render thread | 4/4 |
| `item_buffer_size` 1024 / 4096 / 65536 | 5/8, 4/4, 2/4 |
| MSDF font | 2/4 |
| No subpixel positioning or oversampling | 2/4 |
| 2D snapping | 1/4 |
| Nearest canvas filter | 1/4 |
| V-Sync off | 2/4 |
| Only a `ColorRect` visible, no text | 0/4 |
| Static label text | 0/4 |
| Reading back a 1×1 SubViewport each slow frame (so the GPU never runs behind) | 0/12 |

Under a debugger it didn't crash (0/3).

**Suggested fix** (patch attached, against 4.7.2):

1. When the fence wait times out, allocate a new buffer instead of reusing the busy one.
2. Null-check every `glMapBufferRange` result, falling back to `glBufferSubData`.

Related: #122724 (the same unchecked map and memcpy, Android Adreno 610, closed as not planned) and #85201.

**Steps to reproduce.** No small project reproduces it yet: a GDScript-only scene with skinned characters, a GPU-bound shader and a changing Label survived every run here. It reproduces reliably in our project's battle view (see README.md).
