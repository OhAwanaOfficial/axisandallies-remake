"""Decode the terrain layers of an original map (.rmp, docs/ASSET_PIPELINE.md section 2.1). M10 spike.

  python -I tools/rmp.py "<game>/Data.rwd" "Maps/DeveloperMaps/(1v1)Golden Fields.RMP" --out cache/maps/golden
  python -I tools/rmp.py path/to/map.rmp --out DIR
  python -I tools/rmp.py "<game>/Data.rwd" --survey          # header facts for every map in the archive

Writes DIR/map.json (sizes, terrain name table, per-tile and per-vertex layers, row-major, row 0 =
world y 0) and PNG previews: height.png, mountain.png, surface.png, terrain.png, class.png,
blocking.png, settlement.png. Decoded so far: map size, the two height layers, terrain type per
tile, terrain class (ground / mountain / water / map border), settlement-zone flag, blocking.
Not decoded: the TERRAIN trailer (about 99 KB on Golden Fields), the per-vertex byte after each
height grid, objects (DYNAMIC_THINGS) and everything else. See ASSET_PIPELINE section 2.1.

Never commit or upload the output: it is derived from the player's game files.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rwd  # noqa: E402

METERS_PER_TILE = 2  # verified: object positions (DYNAMIC_THINGS) land on the height grid at x/2, y/2

# Terrain word high byte (per tile). Matches the TGI terrain templates: MountainTerrain type=MOUNTAIN
# (code forces speed_max to 0.0001), WaterTerrain water=true.
CLASS_BORDER = 0x01
CLASS_MOUNTAIN = 0x10
CLASS_WATER = 0x20
FLAG_SETTLEMENT = 0x04  # TERRAIN v6 tile flags: settlement zone (verified against town centres); v7 unknown


class MapError(Exception):
    pass


@dataclass
class HeightLayer:
    version: int
    range_min: float
    range_max: float
    default: float
    unknown: int
    width: int  # vertices per side
    values: list[float]
    vertex_bytes: bytes  # one byte per vertex after the grid (meaning unknown)
    trailing: bytes  # anything after that


@dataclass
class TerrainLayer:
    version: int
    unknown: int
    biome: str
    names: list[str]
    tiles_per_side: int
    record_size: int  # bytes per tile record: 6 (TERRAIN v6) or 8 (v7)
    index: list[int]  # index into names
    cls: list[int]  # class bits: 0x01 map border, 0x10 mountain, 0x20 water
    flags: list[int]  # v6: bit 2 = settlement zone (verified). v7: raw, meaning unknown
    orient: list[int]  # 0..3 (texture rotation, assumed)
    trailer_bytes: int  # undecoded remainder
    trailer_head: bytes = field(default=b"", repr=False)


def _wstr(d: bytes, p: int) -> tuple[str, int]:
    (n,) = struct.unpack_from("<H", d, p)
    end = p + 2 + 2 * n
    if end > len(d):
        raise MapError("string runs past the chunk")
    return d[p + 2:end].decode("utf-16-le"), end


def decode_height(d: bytes) -> HeightLayer:
    if len(d) < 24:
        raise MapError("height chunk too short")
    version, lo, hi, default, unknown, count = struct.unpack_from("<IfffII", d, 0)
    width = int(round(count ** 0.5))
    if width * width != count or 24 + 5 * count > len(d):
        raise MapError(f"height grid of {count} values is not square or does not fit")
    values = list(struct.unpack_from(f"<{count}f", d, 24))
    vb = d[24 + 4 * count: 24 + 5 * count]
    return HeightLayer(version, lo, hi, default, unknown, width, values, vb, d[24 + 5 * count:])


def decode_terrain(d: bytes) -> TerrainLayer:
    version, unknown = struct.unpack_from("<II", d, 0)
    biome, p = _wstr(d, 8)
    (n,) = struct.unpack_from("<I", d, p)
    p += 4
    names = []
    for _ in range(n):
        s, p = _wstr(d, p)
        names.append(s)
    tiles, rec = struct.unpack_from("<II", d, p)
    p += 8
    side = int(round(tiles ** 0.5))
    if side * side != tiles or rec not in (6, 8) or p + rec * tiles > len(d):
        raise MapError(f"unexpected tile table (tiles {tiles}, record {rec})")
    index, cls, flags, orient = [], [], [], []
    for i in range(tiles):
        if rec == 6:  # v6: u16 word (low byte = name index, high byte = class), u16 flags, u16 orientation
            w, f, o = struct.unpack_from("<HHH", d, p + 6 * i)
            index.append(w & 0xFF)
            cls.append(w >> 8)
            flags.append(f)
            orient.append(o)
        else:  # v7 (more than 256 names): u16 name index, u16 0..3 (orientation?), u16 flags (unknown), u16 class
            ix, o, f, c = struct.unpack_from("<HHHH", d, p + 8 * i)
            index.append(ix)
            cls.append(c & 0xFF)
            flags.append(f)
            orient.append(o)
    if max(index) >= len(names):
        raise MapError("terrain index beyond the name table")
    end = p + rec * tiles
    return TerrainLayer(version, unknown, biome, names, side, rec, index, cls, flags, orient, len(d) - end, d[end:end + 64])


# ------------------------------------------------------------------ PNG (stdlib only)
def write_png(path: Path, width: int, height: int, rgb_rows: list[bytes]) -> None:
    raw = b"".join(b"\x00" + row for row in rgb_rows)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def image(path: Path, w: int, h: int, pixel, scale: int = 4) -> None:
    """pixel(x, y) -> (r, g, b). Row 0 of the image is world y 0 (top)."""
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += bytes(pixel(x, y)) * scale
        rows.extend([bytes(row)] * scale)
    write_png(path, w * scale, h * scale, rows)


def gray(v: float, lo: float, hi: float) -> tuple[int, int, int]:
    t = 0.0 if hi <= lo else max(0.0, min(1.0, (v - lo) / (hi - lo)))
    g = int(round(t * 255))
    return (g, g, g)


def name_color(name: str, cls: int) -> tuple[int, int, int]:
    """Stable colour per terrain name, tinted by class (the TGI terrain_color is a later step)."""
    h = hashlib.md5(name.encode()).digest()
    base = (60 + h[0] % 90, 90 + h[1] % 100, 40 + h[2] % 80)  # greens/browns
    if cls & CLASS_WATER:
        return (20 + h[0] % 30, 60 + h[1] % 60, 140 + h[2] % 100)
    if cls & CLASS_MOUNTAIN:
        g = 110 + h[0] % 60
        return (g, g - 10, g - 25)
    return base


# ------------------------------------------------------------------ main
@dataclass
class DecodedMap:
    source: str
    chunks: list[str]
    height: HeightLayer
    mountain: HeightLayer
    terrain: TerrainLayer


def decode_map(data: bytes, source: str) -> DecodedMap:
    chunks = rwd.read_rmp(data)
    by = {c.name: c for c in chunks}
    for need in ("TERRAIN", "HEIGHT_MAP", "MOUNTAIN_HEIGHT_MAP"):
        if need not in by:
            raise MapError(f"missing chunk {need}")
    height = decode_height(rwd.rmp_chunk_bytes(data, by["HEIGHT_MAP"]))
    mountain = decode_height(rwd.rmp_chunk_bytes(data, by["MOUNTAIN_HEIGHT_MAP"]))
    terrain = decode_terrain(rwd.rmp_chunk_bytes(data, by["TERRAIN"]))
    if not (height.width == mountain.width == terrain.tiles_per_side + 1):
        raise MapError(f"grid sizes disagree: heights {height.width}, mountain {mountain.width}, tiles {terrain.tiles_per_side}")
    return DecodedMap(source, [c.name for c in chunks], height, mountain, terrain)


def export(m: DecodedMap, out: Path, scale: int) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    t, h, mt = m.terrain, m.height, m.mountain
    n, v = t.tiles_per_side, h.width
    surface = [a + b for a, b in zip(h.values, mt.values)]
    cls = t.cls
    settlement_known = t.record_size == 6
    blocked = [1 if c & (CLASS_MOUNTAIN | CLASS_WATER | CLASS_BORDER) else 0 for c in cls]
    summary = {
        "source": m.source,
        "chunks": m.chunks,
        "tiles_per_side": n,
        "vertices_per_side": v,
        "meters_per_tile": METERS_PER_TILE,
        "world_size_m": n * METERS_PER_TILE,
        "biome": t.biome,
        "height": {"version": h.version, "range": [h.range_min, h.range_max], "default": h.default,
                   "min": min(h.values), "max": max(h.values), "unknown_u32": h.unknown},
        "mountain_height": {"version": mt.version, "range": [mt.range_min, mt.range_max], "default": mt.default,
                            "min": min(mt.values), "max": max(mt.values), "nonzero_vertices": sum(1 for x in mt.values if x != 0)},
        "surface": {"min": min(surface), "max": max(surface)},
        "terrain": {"version": t.version, "unknown_u32": t.unknown, "names": len(t.names),
                    "record_size": t.record_size, "used": sorted(set(t.index)), "trailer_bytes_undecoded": t.trailer_bytes,
                    "settlement_flag_known": settlement_known},
        "counts": {"mountain_tiles": sum(1 for c in cls if c & CLASS_MOUNTAIN),
                   "water_tiles": sum(1 for c in cls if c & CLASS_WATER),
                   "border_tiles": sum(1 for c in cls if c & CLASS_BORDER),
                   "settlement_tiles": sum(1 for f in t.flags if f & FLAG_SETTLEMENT) if settlement_known else None,
                   "blocked_tiles": sum(blocked)},
    }
    doc = dict(summary)
    doc["layers"] = {
        "note": "row-major, row 0 = world y 0; tiles are n*n, vertices (n+1)*(n+1); vertex (x, y) is at world (2x, 2y) m",
        "terrain_names": t.names,
        "tile_terrain_index": t.index,
        "tile_class_bits": cls,
        "tile_flags": t.flags,
        "tile_orientation": t.orient,
        "tile_blocked_land": blocked,
        "vertex_height": [round(x, 5) for x in h.values],
        "vertex_mountain_offset": [round(x, 5) for x in mt.values],
        # Integer copies for the C++ sim, which never parses floats (CLAUDE.md rule 2).
        "vertex_height_mm": [int(round(x * 1000)) for x in h.values],
        "vertex_mountain_mm": [int(round(x * 1000)) for x in mt.values],
        "vertex_unknown_byte": list(h.vertex_bytes),
    }
    (out / "map.json").write_text(json.dumps(doc), encoding="utf-8")

    lo, hi = summary["surface"]["min"], summary["surface"]["max"]
    image(out / "height.png", v, v, lambda x, y: gray(h.values[y * v + x], min(h.values), max(h.values)), scale)
    image(out / "mountain.png", v, v, lambda x, y: gray(mt.values[y * v + x], min(mt.values), max(mt.values)), scale)
    image(out / "surface.png", v, v, lambda x, y: gray(surface[y * v + x], lo, hi), scale)
    image(out / "terrain.png", n, n, lambda x, y: name_color(t.names[t.index[y * n + x]], cls[y * n + x]), scale)

    def class_px(x: int, y: int) -> tuple[int, int, int]:
        c = cls[y * n + x]
        if c & CLASS_BORDER:
            return (200, 40, 40)
        if c & CLASS_WATER:
            return (40, 90, 200)
        if c & CLASS_MOUNTAIN:
            return (120, 110, 100)
        return (110, 160, 80)

    image(out / "class.png", n, n, class_px, scale)
    image(out / "blocking.png", n, n, lambda x, y: (30, 30, 30) if blocked[y * n + x] else (235, 235, 235), scale)
    if settlement_known:
        image(out / "settlement.png", n, n, lambda x, y: (240, 200, 40) if t.flags[y * n + x] & FLAG_SETTLEMENT else (50, 50, 60), scale)
    return summary


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("source", help="a .rmp file, or Data.rwd")
    ap.add_argument("map", nargs="?", help="map path inside the archive when source is a .rwd")
    ap.add_argument("--out", help="output directory (map.json + PNGs)")
    ap.add_argument("--scale", type=int, default=4, help="PNG pixels per tile/vertex")
    ap.add_argument("--survey", action="store_true", help="decode every .rmp in the archive and print a summary")
    a = ap.parse_args(argv[1:])
    try:
        if a.survey:
            with open(a.source, "rb") as f:
                toc = rwd.read_toc(f)
                ok = 0
                for e in toc:
                    if not e.name.lower().endswith(".rmp"):
                        continue
                    try:
                        m = decode_map(rwd.read_entry(f, e), e.name)
                        t = m.terrain
                        cls = t.cls
                        print(f"ok   {t.tiles_per_side:4}x{t.tiles_per_side:<4} {t.tiles_per_side * METERS_PER_TILE:4} m  "
                              f"terrain v{t.version}/{t.record_size} biome {t.biome:<14} names {len(t.names):3}  "
                              f"heights {min(m.height.values):6.2f}..{max(m.height.values):6.2f}  "
                              f"mtn {sum(1 for c in cls if c & CLASS_MOUNTAIN):5} water {sum(1 for c in cls if c & CLASS_WATER):5}  "
                              f"trailer {t.trailer_bytes:6}  {e.name}")
                        ok += 1
                    except (MapError, struct.error, UnicodeDecodeError, rwd.RwdError) as exc:
                        print(f"FAIL {e.name}: {exc}")
                print(f"{ok} maps decoded", file=sys.stderr)
            return 0
        if a.map:
            with open(a.source, "rb") as f:
                hit = next((e for e in rwd.read_toc(f) if e.name.lower() == a.map.replace("\\", "/").lower()), None)
                if hit is None:
                    raise MapError(f"{a.map} not found in {a.source}")
                data, name = rwd.read_entry(f, hit), hit.name
        else:
            data, name = Path(a.source).read_bytes(), a.source
        m = decode_map(data, name)
        if not a.out:
            ap.error("--out is required unless --survey")
        summary = export(m, Path(a.out), a.scale)
        print(json.dumps(summary, indent=1))
        return 0
    except (OSError, MapError, rwd.RwdError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
