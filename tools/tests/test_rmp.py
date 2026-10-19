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


class RmpTests(unittest.TestCase):
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
