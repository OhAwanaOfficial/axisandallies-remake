# Godot GLES3 2D canvas crash: repro attempts, patch, issue draft

Background, evidence and the project-side fix (the `GpuPacer` autoload) are in
`docs/ARCHITECTURE.md` §4.4, "Godot 2D canvas crash".

## Contents

| File | What it is |
|---|---|
| `repro/` | GDScript-only project, no GDExtension, no assets. It combines skinned, animated characters (skinned through transform feedback on GLES3), a GPU-bound full-screen shader and a multi-line outlined `Label` that changes every frame. **It does not crash on the dev laptop**: 0 of 4 runs, and earlier variants 0 of 15. The reliable repro is still the battle view (below). |
| `godot-gles3-canvas-map-fix.patch` | Engine fix draft against `4.7.2-stable` `drivers/gles3/rasterizer_canvas_gles3.cpp`. Not built or tested. |
| `ISSUE_DRAFT.md` | Upstream report text. Godot doesn't accept AI-written issues, so a person has to rewrite and file it. |

## Reliable repro (this project)

```bash
godot --path godot -- --gen=7:1024 --regiments=160 --quality=high --measure=450 --no-gpu-pacer
```

Run it on Godot 4.6.3, 4.7.2 or 4.8-beta1, GL Compatibility through ANGLE on an Intel HD 3000. It
crashes in most runs, 30–130 s after the regiments spawn. With the GpuPacer on (the default) it
doesn't crash.

## Repro project

```bash
godot --path tools/godot_canvas_crash/repro -- --count=150 --gpu-loops=1500 --seconds=100
```

Options: `--count` characters, `--parts` meshes each, `--gpu-loops` fragment work,
`--static-label`, `--no-label`, `--drain`.
