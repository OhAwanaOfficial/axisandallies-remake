"""Tests for tools/rwd.py on synthetic archives (no game data). Run: python -I -m unittest discover -s tools/tests"""

from __future__ import annotations

import io
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import rwd  # noqa: E402


def build_rwd(files: list[tuple[str, bytes]], file_types: list[int] | None = None) -> bytes:
    """TGCK image: 16-byte header, file bodies, TOC, 288-byte trailer with the two footer offsets."""
    data_offset = 16
    body = bytearray()
    toc = bytearray()
    for i, (name, content) in enumerate(files):
        units = name.encode("utf-16-le")
        ftype = file_types[i] if file_types else i + 1
        toc += struct.pack("<ih", ftype, len(units) // 2) + units
        toc += struct.pack("<qqi", len(body), len(content), 0)
        body += content
    toc_offset = data_offset + len(body)
    trailer = bytearray(288)
    struct.pack_into("<i", trailer, 288 - rwd.FOOTER_DATA_OFFSET, data_offset)
    struct.pack_into("<i", trailer, 288 - rwd.FOOTER_TOC_OFFSET, toc_offset)
    return b"TGCK" + struct.pack("<iii", 2, 3, 2) + bytes(body) + bytes(toc) + bytes(trailer)


def build_rmp(chunks: list[tuple[str, bytes]]) -> bytes:
    out = bytearray(b"TGCK" + struct.pack("<iii", 2, len(chunks), 0))
    records = bytearray()
    for name, raw in chunks:
        stored = raw if name == "MAP" else zlib.compress(raw)
        offset = len(out)
        out += stored
        records += name.encode("utf-16-le").ljust(64, b"\0")
        records += struct.pack("<qqqq", offset, len(stored), 0x1234, len(raw))
    return bytes(out + records)


class RwdTests(unittest.TestCase):
    def test_toc_and_read(self) -> None:
        img = build_rwd([("Units\\a.tgi", b"[Thing]"), ("b/c.dds", b"DDS xyz")])
        f = io.BytesIO(img)
        toc = rwd.read_toc(f)
        self.assertEqual([e.name for e in toc], ["Units/a.tgi", "b/c.dds"])  # '\' normalised
        self.assertEqual(rwd.read_entry(f, toc[1]), b"DDS xyz")
        self.assertEqual(rwd.read_entry(f, toc[0], 3), b"[Th")

    def test_file_type_zero_is_not_a_terminator(self) -> None:
        img = build_rwd([("a", b"1"), ("b", b"22"), ("c", b"333")], file_types=[5, 0, 7])
        self.assertEqual(len(rwd.read_toc(io.BytesIO(img))), 3)

    def test_rejects_non_tgck(self) -> None:
        with self.assertRaises(rwd.RwdError):
            rwd.read_toc(io.BytesIO(b"PK\3\4" + bytes(400)))

    def test_extract_refuses_path_traversal(self) -> None:
        with self.assertRaises(rwd.RwdError):
            rwd.safe_dest(Path("out"), "../evil.txt")
        with self.assertRaises(rwd.RwdError):
            rwd.safe_dest(Path("out"), "C:/evil.txt")
        self.assertEqual(rwd.safe_dest(Path("out"), "a/b.txt"), Path("out/a/b.txt"))

    def test_extract_command(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            arc = Path(tmp) / "t.rwd"
            arc.write_bytes(build_rwd([("x/one.tgi", b"1"), ("x/two.nif", b"2")]))
            rc = rwd.main(["rwd.py", "extract", str(arc), str(Path(tmp) / "out"), "--pattern", r"\.TGI$"])
            self.assertEqual(rc, 0)
            self.assertEqual((Path(tmp) / "out/x/one.tgi").read_bytes(), b"1")
            self.assertFalse((Path(tmp) / "out/x/two.nif").exists())

    def test_rmp_chunks(self) -> None:
        img = build_rmp([("MAP", b"\0\0\0\0"), ("TERRAIN", b"t" * 1000), ("MAPSTRINGS", b"[Text language=Default]")])
        chunks = rwd.read_rmp(img)
        self.assertEqual([c.name for c in chunks], ["MAP", "TERRAIN", "MAPSTRINGS"])
        self.assertEqual(rwd.rmp_chunk_bytes(img, chunks[1]), b"t" * 1000)
        self.assertEqual(rwd.rmp_chunk_bytes(img, chunks[0]), b"\0\0\0\0")
        self.assertTrue(rwd.rmp_chunk_bytes(img, chunks[2]).startswith(b"[Text"))


if __name__ == "__main__":
    unittest.main()
