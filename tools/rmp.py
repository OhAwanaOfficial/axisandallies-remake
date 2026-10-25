"""Decode the terrain layers of an original map (.rmp, docs/ASSET_PIPELINE.md section 2.1). M10 spike.

  python -I tools/rmp.py "<game>/Data.rwd" "Maps/DeveloperMaps/(1v1)Golden Fields.RMP" --out cache/maps/golden
  python -I tools/rmp.py path/to/map.rmp --out DIR
  python -I tools/rmp.py "<game>/Data.rwd" --survey          # header facts for every map in the archive

Writes DIR/map.json (sizes, terrain name table, per-tile and per-vertex layers, row-major, row 0 =
world y 0; placed things, kingdoms and match rules) and PNG previews: height.png, mountain.png,
surface.png, terrain.png, class.png, blocking.png, settlement.png. Decoded so far: map size, the
two height layers, terrain type per tile, terrain class (ground / mountain / water / map border),
settlement-zone flag, blocking; placed objects (DYNAMIC_THINGS: IDS, position, facing, owning
kingdom, name), KINGDOMS (object id, name, colour) and RULES (name, value). Not decoded: the
TERRAIN trailer (about 99 KB on Golden Fields), the per-vertex byte after each height grid, the
rest of each thing record, and the other chunks. See ASSET_PIPELINE section 2.1.

Never commit or upload the output: it is derived from the player's game files.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
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
    things: list["Thing"] = field(default_factory=list)
    kingdoms: list["Kingdom"] = field(default_factory=list)
    rules: list["Rule"] = field(default_factory=list)


@dataclass
class Thing:
    """A placed object (DYNAMIC_THINGS record)."""
    ids: str
    x: float  # metres (= sim cells)
    y: float
    z: float
    ox: float  # facing vector
    oy: float
    owner: int  # the owning kingdom's object id (KINGDOMS), 0 if none
    name: str  # a map string key such as "name_6" ("" if none)


@dataclass
class Kingdom:
    object_id: int
    name: str  # kingdom_0 ...
    color: str


@dataclass
class Rule:
    name: str
    type: int  # 0 bool, 1 ratio, 2 number, 3 int
    value: float


# DYNAMIC_THINGS record, after the u16-prefixed IDS: u32 kind, u32 object id, u32 2, 6 zero bytes,
# f32 x, y, z, f32 facing x, y, ...; at body offset 68 a u32 14 and then the owner (a kingdom's object
# id). Named things (town centres) carry a u16-prefixed "name_N" string near the end. Verified on
# Golden Fields against the town centres, the start locations and the Corps placeholders.
THING_POS = 18
THING_OWNER_TAG = 68


def decode_things(d: bytes, index: bytes) -> list[Thing]:
    (count,) = struct.unpack_from("<I", index, 4)
    sizes = struct.unpack_from(f"<{count}I", index, 8)
    out = []
    p = 8
    for size in sizes:
        if p + size > len(d):
            raise MapError("thing record runs past the chunk")
        ids, e = _wstr(d, p)
        x, y, z, ox, oy = struct.unpack_from("<5f", d, e + THING_POS)
        owner = 0
        if e + THING_OWNER_TAG + 8 <= p + size and struct.unpack_from("<I", d, e + THING_OWNER_TAG)[0] == 14:
            (owner,) = struct.unpack_from("<I", d, e + THING_OWNER_TAG + 4)
        name = ""
        i = e + THING_OWNER_TAG + 8
        while i + 2 < p + size:  # a "name_N" string, if any
            (n,) = struct.unpack_from("<H", d, i)
            if 3 <= n <= 32 and i + 2 + 2 * n <= p + size:
                try:
                    s = d[i + 2:i + 2 + 2 * n].decode("utf-16-le")
                except UnicodeDecodeError:
                    s = ""
                if s.startswith("name_") and s.isascii() and s.isprintable():
                    name = s
                    break
            i += 1
        out.append(Thing(ids, x, y, z, ox, oy, owner, name))
        p += size
    return out


TRANSLATE_PAIR = re.compile(r"ids_old\s*=\s*([A-Za-z0-9_]+)\s+ids_new\s*=\s*([A-Za-z0-9_]+)")


def translations(text: str) -> dict[str, str]:
    """Thing IDS renames from translate.tgi (ids_old -> ids_new, lower case). Read from the raw text,
    commented-out blocks included: the TGI parser (non-nesting comments) sees lines 1292-5310 as one
    comment, but maps such as Golden Fields name things only those renames reach
    (chaparral_village_town_center -> mediterranean_city_center01 -> mediterranean_city_center01_large),
    so the original game evidently applies them."""
    out: dict[str, str] = {}
    for old, new in TRANSLATE_PAIR.findall(text):
        out.setdefault(old.lower(), new.lower())
    return out


def translated(ids: str, renames: dict[str, str]) -> str:
    seen = set()
    cur = ids.lower()
    while cur in renames and cur not in seen:
        seen.add(cur)
        cur = renames[cur]
    return cur


def decode_kingdoms(d: bytes) -> list[Kingdom]:
    """Each kingdom record: wstr "kingdom" (its Thing), u32 4, u32 object id, u32 2, wstr name; later the
    name again and its colour."""
    out = []
    key = struct.pack("<H", 7) + "kingdom".encode("utf-16-le")
    p = d.find(key)
    while p >= 0:
        e = p + len(key)
        tag, obj, _two = struct.unpack_from("<III", d, e)
        if tag == 4:
            name, q = _wstr(d, e + 12)
            again = d.find(struct.pack("<H", len(name)) + name.encode("utf-16-le"), q)
            color = _wstr(d, again + 2 + 2 * len(name))[0] if again >= 0 else ""
            out.append(Kingdom(obj, name, color))
        p = d.find(key, e)
    return out


def decode_rules(d: bytes) -> list[Rule]:
    """u32 version, u32 0, wstr game type, u32 count, then per rule: wstr name, u8 0, u32 type, value
    (type 0: u8 bool; 1: f32 ratio; 2: f32 number; 3: u32 int)."""
    _game_type, p = _wstr(d, 8)
    (count,) = struct.unpack_from("<I", d, p)
    p += 4
    out = []
    for _ in range(count):
        name, p = _wstr(d, p)
        (typ,) = struct.unpack_from("<I", d, p + 1)
        p += 5
        if typ == 0:
            v = d[p]
            p += 1
        elif typ in (1, 2):
            (v,) = struct.unpack_from("<f", d, p)
            p += 4
        else:
            (v,) = struct.unpack_from("<I", d, p)
            p += 4
        out.append(Rule(name, typ, float(v)))
    return out


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
    m = DecodedMap(source, [c.name for c in chunks], height, mountain, terrain)
    if "DYNAMIC_THINGS" in by and "DYNAMIC_THINGS_INDEX" in by:
        m.things = decode_things(rwd.rmp_chunk_bytes(data, by["DYNAMIC_THINGS"]), rwd.rmp_chunk_bytes(data, by["DYNAMIC_THINGS_INDEX"]))
    if "KINGDOMS" in by:
        m.kingdoms = decode_kingdoms(rwd.rmp_chunk_bytes(data, by["KINGDOMS"]))
    if "RULES" in by:
        m.rules = decode_rules(rwd.rmp_chunk_bytes(data, by["RULES"]))
    return m


def export(m: DecodedMap, out: Path, scale: int, renames: dict[str, str] | None = None) -> dict:
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
    # Placed things, kingdoms and rules. Positions and facings as integer millimetres (the sim never
    # parses floats); `owner` is the kingdom's index in `kingdoms` (-1: none).
    kingdom_index = {k.object_id: i for i, k in enumerate(m.kingdoms)}
    renames = renames or {}
    doc["things"] = [{"ids": t.ids, "ids_translated": translated(t.ids, renames), "x_mm": int(round(t.x * 1000)), "y_mm": int(round(t.y * 1000)),
                      "facing_x_mm": int(round(t.ox * 1000)), "facing_y_mm": int(round(t.oy * 1000)),
                      "owner": kingdom_index.get(t.owner, -1), "name": t.name} for t in m.things]
    doc["kingdoms"] = [{"name": k.name, "color": k.color, "object_id": k.object_id} for k in m.kingdoms]
    doc["rules"] = [{"name": r.name, "type": r.type, "value": r.value, "value_milli": int(round(r.value * 1000))} for r in m.rules]
    summary["things"] = len(m.things)
    summary["kingdoms"] = [k.name for k in m.kingdoms]
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
        renames: dict[str, str] = {}
        if a.map:
            with open(a.source, "rb") as f:
                toc = rwd.read_toc(f)
                hit = next((e for e in toc if e.name.lower() == a.map.replace("\\", "/").lower()), None)
                if hit is None:
                    raise MapError(f"{a.map} not found in {a.source}")
                data, name = rwd.read_entry(f, hit), hit.name
                tr = next((e for e in toc if e.name.lower() == "translate.tgi"), None)
                if tr is not None:
                    renames = translations(rwd.read_entry(f, tr).decode("utf-8", "replace"))
        else:
            data, name = Path(a.source).read_bytes(), a.source
        m = decode_map(data, name)
        if not a.out:
            ap.error("--out is required unless --survey")
        summary = export(m, Path(a.out), a.scale, renames)
        print(json.dumps(summary, indent=1))
        return 0
    except (OSError, MapError, rwd.RwdError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
