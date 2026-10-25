"""Tests for tools/rmp.py on synthetic map chunks (no game data). Run: python -I -m unittest discover -s tools/tests"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import rmp  # noqa: E402
from test_rwd import build_rmp  # noqa: E402


def wstr(s: str) -> bytes:
    return struct.pack("<H", len(s)) + s.encode("utf-16-le")


def height_chunk(width: int, base: float, bump: float = 0.0) -> bytes:
    n = width * width
    vals = [base] * n
    vals[n // 2] = base + bump
    return (struct.pack("<IfffII", 1, -10.0, 10.0, base, 1, n) + struct.pack(f"<{n}f", *vals)
            + bytes(range(4)) * (n // 4) + bytes(n % 4))


def terrain_chunk(side: int, names: list[str], tiles: list[tuple[int, int, int, int]], v7: bool) -> bytes:
    out = struct.pack("<II", 7 if v7 else 6, 1) + wstr("chaparral") + struct.pack("<I", len(names))
    out += b"".join(wstr(n) for n in names)
    out += struct.pack("<II", side * side, 8 if v7 else 6)
    for idx, cls, flags, orient in tiles:
        if v7:
            out += struct.pack("<HHHH", idx, orient, flags, cls)
        else:
            out += struct.pack("<HHH", (cls << 8) | idx, flags, orient)
    return out + b"TRAILER"


def thing_record(ids: str, kind: int, obj: int, pos: tuple[float, float, float], facing: tuple[float, float], owner: int,
                 name: str = "") -> bytes:
    body = struct.pack("<III", kind, obj, 2) + bytes(6) + struct.pack("<5f", *pos, *facing)
    body += bytes(rmp.THING_OWNER_TAG - len(body)) + struct.pack("<II", 14, owner) + bytes(16)
    if name:
        body += wstr(name) + bytes(8)
    return wstr(ids) + body


class RmpTests(unittest.TestCase):
    def test_translations_follow_chains_inside_comments(self) -> None:
        text = """/* commented out
[Translate template=Translate]
{ node = Thing
  ids_old = chaparral_village_town_center
  ids_new = mediterranean_city_center01 }
*/
[Translate template=Translate]
{ node = Thing
  ids_old = mediterranean_city_center01
  ids_new = mediterranean_city_center01_large }"""
        renames = rmp.translations(text)
        self.assertEqual(rmp.translated("Chaparral_Village_Town_Center", renames), "mediterranean_city_center01_large")
        self.assertEqual(rmp.translated("start_loc", renames), "start_loc")

    def test_things_kingdoms_rules(self) -> None:
        recs = [thing_record("chaparral_village_town_center", 3, 6, (210.0, 74.0, 2.0), (0.0, -1.0), 5, "name_6"),
                thing_record("placeholder_corpsunpacked", 3, 75, (205.0, 145.0, 2.0), (0.0, -1.0), 1)]
        things = struct.pack("<II", 0, len(recs)) + b"".join(recs)
        index = struct.pack("<II", 0, len(recs)) + b"".join(struct.pack("<I", len(r)) for r in recs)
        got = rmp.decode_things(things, index)
        self.assertEqual([t.ids for t in got], ["chaparral_village_town_center", "placeholder_corpsunpacked"])
        self.assertEqual((got[0].x, got[0].y, got[0].owner, got[0].name), (210.0, 74.0, 5, "name_6"))
        self.assertEqual((got[1].x, got[1].y, got[1].owner, got[1].name), (205.0, 145.0, 1, ""))
        kingdoms = b""
        for obj, name, color in [(1, "kingdom_0", "blue"), (2, "kingdom_1", "red")]:
            kingdoms += wstr("kingdom") + struct.pack("<III", 4, obj, 2) + wstr(name) + bytes(20) + wstr(name) + wstr(color)
        k = rmp.decode_kingdoms(struct.pack("<II", 3, 2) + kingdoms)
        self.assertEqual([(x.object_id, x.name, x.color) for x in k], [(1, "kingdom_0", "blue"), (2, "kingdom_1", "red")])
        rules = struct.pack("<II", 4, 0) + wstr("NORMAL") + struct.pack("<I", 3)
        rules += wstr("Rated") + b"\0" + struct.pack("<IB", 0, 1)
        rules += wstr("CityVictoryRatio") + b"\0" + struct.pack("<If", 1, 0.5)
        rules += wstr("ResourceStockpileDefault") + b"\0" + struct.pack("<If", 2, 1000.0)
        got_rules = rmp.decode_rules(rules)
        self.assertEqual([(x.name, x.value) for x in got_rules], [("Rated", 1.0), ("CityVictoryRatio", 0.5), ("ResourceStockpileDefault", 1000.0)])

    def test_height_layer(self) -> None:
        h = rmp.decode_height(height_chunk(5, 2.0, 3.5))
        self.assertEqual(h.width, 5)
        self.assertEqual(h.default, 2.0)
        self.assertAlmostEqual(max(h.values), 5.5)
        self.assertEqual(len(h.vertex_bytes), 25)
        with self.assertRaises(rmp.MapError):
            rmp.decode_height(struct.pack("<IfffII", 1, 0, 0, 0, 1, 24) + bytes(200))  # not square

    def test_terrain_v6_and_v7(self) -> None:
        names = ["grass", "water_deep", "mountain"]
        tiles = [(0, 0, 0, 0), (1, rmp.CLASS_WATER, 0, 1), (2, rmp.CLASS_MOUNTAIN, rmp.FLAG_SETTLEMENT, 2), (0, rmp.CLASS_BORDER, 0, 3)]
        for v7 in (False, True):
            t = rmp.decode_terrain(terrain_chunk(2, names, tiles, v7))
            self.assertEqual(t.tiles_per_side, 2)
            self.assertEqual(t.index, [0, 1, 2, 0])
            self.assertEqual(t.cls, [0, rmp.CLASS_WATER, rmp.CLASS_MOUNTAIN, rmp.CLASS_BORDER])
            self.assertEqual(t.orient, [0, 1, 2, 3])
            self.assertEqual(t.trailer_bytes, len(b"TRAILER"))
        bad = terrain_chunk(2, names, [(9, 0, 0, 0)] * 4, False)
        with self.assertRaises(rmp.MapError):
            rmp.decode_terrain(bad)

    def test_full_map_export(self) -> None:
        names = ["grass", "water_deep", "mountain"]
        tiles = [(0, 0, 0, 0), (1, rmp.CLASS_WATER, 0, 0), (2, rmp.CLASS_MOUNTAIN, rmp.FLAG_SETTLEMENT, 0), (0, 0, 0, 0)]
        data = build_rmp([("MAP", b"\0\0\0\0"), ("TERRAIN", terrain_chunk(2, names, tiles, False)),
                          ("HEIGHT_MAP", height_chunk(3, 2.0, 1.0)), ("MOUNTAIN_HEIGHT_MAP", height_chunk(3, 0.0, 4.0))])
        m = rmp.decode_map(data, "synthetic")
        with tempfile.TemporaryDirectory() as tmp:
            summary = rmp.export(m, Path(tmp), scale=2)
            self.assertEqual(summary["world_size_m"], 4)
            self.assertEqual(summary["counts"]["blocked_tiles"], 2)
            self.assertEqual(summary["counts"]["settlement_tiles"], 1)
            self.assertAlmostEqual(summary["surface"]["max"], 7.0)
            import json
            layers = json.loads((Path(tmp) / "map.json").read_text(encoding="utf-8"))["layers"]
            # Integer millimetre copies for the C++ sim (no float parsing there).
            self.assertEqual(layers["vertex_height_mm"], [2000] * 4 + [3000] + [2000] * 4)
            self.assertEqual(layers["vertex_mountain_mm"], [0] * 4 + [4000] + [0] * 4)
            png = (Path(tmp) / "blocking.png").read_bytes()
            self.assertTrue(png.startswith(b"\x89PNG"))
            w, h = struct.unpack(">II", png[16:24])
            self.assertEqual((w, h), (4, 4))
            self.assertTrue(zlib.decompress(png[png.index(b"IDAT") + 4:png.index(b"IEND") - 8]))

    def test_grid_mismatch_is_rejected(self) -> None:
        data = build_rmp([("MAP", b"\0\0\0\0"), ("TERRAIN", terrain_chunk(2, ["g"], [(0, 0, 0, 0)] * 4, False)),
                          ("HEIGHT_MAP", height_chunk(5, 2.0)), ("MOUNTAIN_HEIGHT_MAP", height_chunk(5, 0.0))])
        with self.assertRaises(rmp.MapError):
            rmp.decode_map(data, "bad")


if __name__ == "__main__":
    unittest.main()
