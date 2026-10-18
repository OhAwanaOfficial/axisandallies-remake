"""Import the original game's art into cache/ (docs/ASSET_PIPELINE.md §4). M2.

  python -I tools/import_game.py [--game DIR] [--out cache] [--only REGEX] [--jobs N] [--force]

* Mounts the install in autoexec order (tools/gamevfs.py, same as sim/vfs).
* NIF -> <path>.glb and KF -> <path>.anim.glb (tools/nif_gltf.py), keeping the original relative paths,
  so TGI fields (resource_prefix, kf, icon, TextureSearchPath) resolve without remapping.
* DDS / TGA / PNG -> copied as-is (Godot loads DDS/DXT natively, also at runtime).
* cache/manifest.json: per source the identity of its bytes (depot + offset/mtime + size) and the
  outputs; unchanged sources are skipped on the next run. Also lists models, clips, textures and,
  per model, the clips that can drive it (retargeting is by node name, like Gamebryo).

Everything written here is derived from the player's own game files: never commit cache/.
Requires pyffi (pip install pyffi). Python 3.11+.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import posixpath
import re
import sys
import time
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gamevfs  # noqa: E402

MANIFEST_VERSION = 1
TEXTURE_EXTS = (".dds", ".tga", ".png")
MODEL_EXTS = (".nif",)
CLIP_EXTS = (".kf",)
COMPAT_MIN = 0.9  # a clip drives a model if >= 90% of its targets exist in the model

# ---------------------------------------------------------------- worker side
_W: dict = {}


def _init_worker(game: str, out: str, textures: dict[str, str], by_name: dict[str, list[str]]) -> None:
    _W["vfs"] = gamevfs.mount_game(Path(game))
    _W["out"] = Path(out)
    _W["textures"] = textures  # lower-case key -> cache path (as spelled)
    _W["by_name"] = by_name  # lower-case file name -> [lower-case keys]


def resolve_texture(nif_path: str, tex: str) -> tuple[str | None, str]:
    """NIF texture name -> cache path (relative to the cache root). Returns (path, how)."""
    tex = tex.replace("\\", "/").strip()
    if not tex:
        return None, "empty"
    nif_dir = posixpath.dirname(nif_path).lower()
    cands = []
    if tex.startswith("/"):
        cands.append(tex.lstrip("/").lower())
    else:
        cands.append(posixpath.normpath(posixpath.join(nif_dir, tex)).lower())
        cands.append(tex.lower())
    textures = _W["textures"]
    for c in cands:
        if c in textures:
            return textures[c], "exact"
    # Fall back to the same file name elsewhere (TextureSearchPath / shared texture folders):
    # pick the candidate sharing the longest directory prefix with the NIF.
    name = posixpath.basename(tex).lower()
    hits = _W["by_name"].get(name, [])
    if not hits:
        stem = name.rsplit(".", 1)[0]
        hits = [k for ext in TEXTURE_EXTS for k in _W["by_name"].get(stem + ext, [])]
    if not hits:
        return None, "missing"

    def score(k: str) -> tuple[int, int]:
        common = os.path.commonprefix([nif_dir + "/", posixpath.dirname(k) + "/"])
        return (common.count("/"), -len(k))

    return textures[max(hits, key=score)], "by-name"


def _convert(kind: str, src: str) -> dict:
    import nif_gltf  # imported in the worker (pyffi is slow to import)

    vfs = _W["vfs"]
    f = vfs.find(src)
    t = time.time()
    res: dict = {"src": f.path, "source_id": f.source_id(), "kind": kind,
                 "converter": nif_gltf.CONVERTER_VERSION}
    data = b""
    try:
        data = vfs.read(f)
        if kind == "model":
            out_rel = str(PurePosixPath(f.path).with_suffix(".glb"))
            out_dir = posixpath.dirname(out_rel)
            how: dict[str, str] = {}

            def resolver(tex: str) -> str | None:
                path, h = resolve_texture(f.path, tex)
                how[tex] = h
                return posixpath.relpath(path, out_dir or ".") if path else None

            stem = PurePosixPath(out_rel).stem

            def embedded(dds: bytes, i: int) -> str:
                name = f"{stem}.embedded{i}.dds"
                (_W["out"] / out_dir / name).parent.mkdir(parents=True, exist_ok=True)
                (_W["out"] / out_dir / name).write_bytes(dds)
                return name

            glb, rep = nif_gltf.convert_nif(data, resolver, embedded)
            res["texture_lookup"] = {k: v for k, v in how.items() if v != "exact"}
        else:
            out_rel = str(PurePosixPath(f.path).with_suffix(".anim.glb"))
            glb, rep = nif_gltf.convert_kf(data)
        dest = _W["out"] / out_rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(glb)
        res.update(out=out_rel, ok=True, bytes=len(glb), nodes=rep.nodes, meshes=rep.meshes, skins=rep.skins,
                   animations=rep.animations, skipped=rep.skipped, warnings=rep.warnings[:20],
                   missing_textures=sorted(set(rep.missing_textures)), clips=rep.clips,
                   textures=sorted({posixpath.normpath(posixpath.join(posixpath.dirname(out_rel), u))
                                    for u in rep.textures}))
        if kind == "model":
            res["node_names"] = sorted({n for n in rep.node_names if n})
        else:
            res["targets"] = rep.clip_targets
    except Exception as exc:  # noqa: BLE001 - one bad file must not stop the import
        msg = f"{type(exc).__name__}: {exc}"
        if kind == "model" and data[:39] == b"NetImmerse File Format, Version 4.2.2.0":
            msg = "NetImmerse 4.2.2.0 file: pyffi 2.2.3 cannot read its embedded NiPixelData (" + msg + ")"
        res.update(ok=False, error=msg[:500])
    res["seconds"] = round(time.time() - t, 3)
    return res


# ---------------------------------------------------------------- driver
def _converter_version() -> int:
    import nif_gltf

    return nif_gltf.CONVERTER_VERSION


def copy_textures(vfs: gamevfs.Vfs, out: Path, old: dict, rx: re.Pattern | None) -> tuple[dict, int]:
    entries, copied = {}, 0
    for f in vfs.iter_ext(*TEXTURE_EXTS):
        if rx and not rx.search(f.path):
            continue
        sid = f.source_id()
        prev = old.get(f.key)
        dest = out / f.path
        if not (prev and prev.get("source_id") == sid and dest.is_file()):
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(vfs.read(f))
            copied += 1
        entries[f.key] = {"src": f.path, "source_id": sid, "kind": "texture", "out": f.path, "ok": True,
                          "depot": f.depot}
    return entries, copied


def clip_compat(models: list[dict], clips: list[dict]) -> dict[str, list[str]]:
    """Per model, the clips that can drive it: same folder as the model, or a shared clip folder
    (Shared/Animations/...), and at least COMPAT_MIN of the clip's targets named in the model."""
    out: dict[str, list[str]] = {}
    for m in models:
        names = {n.lower() for n in m.get("node_names", [])}
        mdir = posixpath.dirname(m["src"]).lower()
        hits = []
        for c in clips:
            cdir = posixpath.dirname(c["src"]).lower()
            if cdir != mdir and not cdir.startswith("shared/animations/"):
                continue
            tg = [t.lower() for t in c.get("targets", [])]
            if tg and sum(t in names for t in tg) / len(tg) >= COMPAT_MIN:
                hits.append(c["out"])
        if hits:
            out[m["out"]] = sorted(hits, key=lambda p: (not p.lower().startswith(mdir), p.lower()))
    return out


def main(argv: list[str]) -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", help="original install (default: config/game_path.cfg)")
    ap.add_argument("--out", default=str(here.parent / "cache"))
    ap.add_argument("--only", help="regex on the source path (models, clips and textures)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--force", action="store_true", help="reconvert everything")
    args = ap.parse_args(argv[1:])

    game = Path(args.game) if args.game else gamevfs.game_dir_from_config(here)
    if game is None or not game.is_dir():
        print("error: no game install (pass --game or create config/game_path.cfg)", file=sys.stderr)
        return 2
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    rx = re.compile(args.only, re.IGNORECASE) if args.only else None
    t_start = time.time()

    vfs = gamevfs.mount_game(game)
    print("mounted:", "; ".join(vfs.mounted))
    man_path = out / "manifest.json"
    old_files: dict = {}
    if man_path.is_file() and not args.force:
        try:
            old = json.loads(man_path.read_text(encoding="utf-8"))
            if old.get("version") == MANIFEST_VERSION:
                old_files = old.get("files", {})
        except (OSError, ValueError):
            pass

    tex_entries, copied = copy_textures(vfs, out, old_files, rx)
    if rx:  # keep the full texture index for lookups even when filtering
        for f in vfs.iter_ext(*TEXTURE_EXTS):
            tex_entries.setdefault(f.key, old_files.get(f.key) or {"src": f.path, "out": f.path, "kind": "texture"})
    print(f"textures: {len(tex_entries)} indexed, {copied} copied")
    textures = {k: v["out"] for k, v in tex_entries.items()}
    by_name: dict[str, list[str]] = {}
    for k in textures:
        by_name.setdefault(posixpath.basename(k), []).append(k)

    converter = _converter_version()
    jobs, files = [], dict(tex_entries)
    for kind, exts in (("model", MODEL_EXTS), ("clip", CLIP_EXTS)):
        for f in vfs.iter_ext(*exts):
            prev = old_files.get(f.key)
            if rx and not rx.search(f.path):
                if prev:
                    files[f.key] = prev
                continue
            if (prev and prev.get("source_id") == f.source_id() and prev.get("ok")
                    and prev.get("converter") == converter and (out / prev["out"]).is_file()):
                files[f.key] = prev
                continue
            jobs.append((kind, f.path))
    print(f"converting {len(jobs)} file(s) with {args.jobs} worker(s) "
          f"({sum(1 for v in files.values() if v.get('kind') != 'texture')} up to date)")
    vfs.close()

    done = failed = 0
    with cf.ProcessPoolExecutor(args.jobs, initializer=_init_worker,
                                initargs=(str(game), str(out), textures, by_name)) as ex:
        futs = [ex.submit(_convert, k, p) for k, p in jobs]
        for fut in cf.as_completed(futs):
            r = fut.result()
            files[r["src"].lower()] = r
            done += 1
            if not r.get("ok"):
                failed += 1
                print(f"  FAIL {r['src']}: {r.get('error')}")
            if done % 200 == 0 or done == len(jobs):
                print(f"  {done}/{len(jobs)} ({failed} failed, {time.time() - t_start:.0f}s)", flush=True)

    models = sorted((v for v in files.values() if v.get("kind") == "model" and v.get("ok")), key=lambda v: v["src"].lower())
    clips = sorted((v for v in files.values() if v.get("kind") == "clip" and v.get("ok")), key=lambda v: v["src"].lower())
    manifest = {
        "version": MANIFEST_VERSION,
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "game": str(game),
        "depots": vfs.mounted,
        "files": dict(sorted(files.items())),
        "models": [{"glb": m["out"], "src": m["src"], "meshes": m["meshes"], "skins": m["skins"],
                    "animations": m["animations"], "textures": m.get("textures", []),
                    "missing_textures": m.get("missing_textures", [])} for m in models],
        "clips": [{"glb": c["out"], "src": c["src"], "clips": c.get("clips", []), "targets": len(c.get("targets", []))}
                  for c in clips],
        "textures": sorted(v["out"] for v in tex_entries.values()),
        "compatible_clips": clip_compat(models, clips),
    }
    tmp = man_path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    tmp.replace(man_path)

    bad = [v for v in files.values() if v.get("kind") in ("model", "clip") and not v.get("ok")]
    skipped: dict[str, int] = {}
    for v in files.values():
        for k, n in (v.get("skipped") or {}).items():
            skipped[k] = skipped.get(k, 0) + n
    miss = sum(len(v.get("missing_textures", [])) for v in models)
    print(f"done in {time.time() - t_start:.0f}s: {len(models)} models, {len(clips)} clip files, "
          f"{len(bad)} failures, {miss} unresolved texture refs")
    if skipped:
        print("skipped blocks:", ", ".join(f"{k} {n}" for k, n in sorted(skipped.items(), key=lambda kv: -kv[1])[:12]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
