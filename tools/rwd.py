"""Read TimeGate RWD archives (TGCK format, docs/ASSET_PIPELINE.md section 2).

Python port of the original tools/extract-rwd.ps1. Same TOC rules as sim/vfs/rwd_archive.cpp.

usage:
  python -I tools/rwd.py list    <archive.rwd> [--pattern REGEX] [--long] [--summary]
  python -I tools/rwd.py extract <archive.rwd> <out_dir> [--pattern REGEX]
  python -I tools/rwd.py magic   <archive.rwd> [--pattern REGEX]   # first bytes of each file
  python -I tools/rwd.py rmp     <map.rmp> [--out DIR]             # list/extract map chunks

REGEX is matched case-insensitively against the stored name (with '/' separators).
Never commit extracted files: they are the player's game assets.
"""

from __future__ import annotations

import argparse
import collections
import errno
import re
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import BinaryIO, Iterator

MAGIC = b"TGCK"
FOOTER_DATA_OFFSET = 128  # int32 at EOF-128
FOOTER_TOC_OFFSET = 32  # int32 at EOF-32
TOC_END_GAP = 288  # TOC ends before EOF-288
MAX_NAME_UNITS = 400
INVALID_NAME_CHARS = set('<>:"|?*\0')


@dataclass(frozen=True)
class Entry:
    name: str  # normalised to '/' separators
    file_type: int
    offset: int  # absolute
    size: int
    unknown: int


class RwdError(Exception):
    pass


def read_toc(f: BinaryIO) -> list[Entry]:
    f.seek(0, 2)
    file_size = f.tell()
    f.seek(0)
    if f.read(4) != MAGIC:
        raise RwdError("not a TGCK archive")
    if file_size < TOC_END_GAP + 4:
        raise RwdError("file too small")
    f.seek(file_size - FOOTER_DATA_OFFSET)
    (data_offset,) = struct.unpack("<i", f.read(4))
    f.seek(file_size - FOOTER_TOC_OFFSET)
    (toc_offset,) = struct.unpack("<i", f.read(4))
    toc_end = file_size - TOC_END_GAP
    if not 0 < toc_offset < toc_end:
        raise RwdError(f"bad TOC offset {toc_offset}")
    f.seek(toc_offset)
    toc = f.read(toc_end - toc_offset)

    entries: list[Entry] = []
    pos = 0
    # fileType 0 can appear mid-table, so the table ends when an entry stops looking valid.
    while pos + 28 <= len(toc):  # smallest entry: 6-byte header, 1 UTF-16 unit, 20-byte tail
        file_type, name_len = struct.unpack_from("<ih", toc, pos)
        if not 0 < name_len <= MAX_NAME_UNITS or pos + 6 + name_len * 2 + 20 > len(toc):
            break
        pos += 6
        try:
            name = toc[pos : pos + name_len * 2].decode("utf-16-le")
        except UnicodeDecodeError:
            break
        pos += name_len * 2
        rel_offset, size, unknown = struct.unpack_from("<qqi", toc, pos)
        pos += 20
        offset = rel_offset + data_offset
        if INVALID_NAME_CHARS & set(name) or size < 0 or offset < 0 or offset + size > file_size:
            break
        entries.append(Entry(name.replace("\\", "/"), file_type, offset, size, unknown))
    return entries


def read_entry(f: BinaryIO, e: Entry, limit: int | None = None) -> bytes:
    f.seek(e.offset)
    return f.read(e.size if limit is None else min(limit, e.size))


def safe_dest(out_dir: Path, name: str) -> Path:
    rel = PurePosixPath(name)
    if rel.is_absolute() or ".." in rel.parts or (rel.parts and ":" in rel.parts[0]):
        raise RwdError(f"refusing unsafe path {name!r}")
    return out_dir.joinpath(*rel.parts)


def filtered(entries: list[Entry], pattern: str | None) -> Iterator[Entry]:
    rx = re.compile(pattern, re.IGNORECASE) if pattern else None
    return (e for e in entries if rx is None or rx.search(e.name))


def cmd_list(args: argparse.Namespace) -> int:
    with open(args.archive, "rb") as f:
        entries = read_toc(f)
    sel = list(filtered(entries, args.pattern))
    if args.summary:
        by_ext: collections.Counter[str] = collections.Counter()
        bytes_ext: collections.Counter[str] = collections.Counter()
        for e in sel:
            ext = PurePosixPath(e.name).suffix.lower() or "(none)"
            by_ext[ext] += 1
            bytes_ext[ext] += e.size
        for ext, n in by_ext.most_common():
            print(f"{n:7}  {bytes_ext[ext] / 2**20:9.1f} MB  {ext}")
    else:
        for e in sel:
            print(f"{e.file_type:6}  {e.size:10}  {e.unknown:4}  {e.name}" if args.long else e.name)
    dup = len(entries) - len({e.name.lower() for e in entries})
    print(f"{len(sel)} of {len(entries)} entries; {dup} duplicate names", file=sys.stderr)
    return 0


def cmd_extract(args: argparse.Namespace) -> int:
    out_dir = Path(args.out_dir)
    n = 0
    with open(args.archive, "rb") as f:
        for e in filtered(read_toc(f), args.pattern):
            dest = safe_dest(out_dir, e.name)
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(read_entry(f, e))
            print(e.name)
            n += 1
    print(f"Extracted {n} file(s).", file=sys.stderr)
    return 0


def cmd_magic(args: argparse.Namespace) -> int:
    with open(args.archive, "rb") as f:
        for e in filtered(read_toc(f), args.pattern):
            head = read_entry(f, e, 16)
            text = "".join(chr(b) if 32 <= b < 127 else "." for b in head)
            print(f"{head.hex(' '):47}  {text:16}  {e.size:10}  {e.name}")
    return 0


@dataclass(frozen=True)
class Chunk:
    name: str
    offset: int
    stored_size: int
    stamp: int  # unknown 8 bytes, identical for every chunk of one map
    size: int  # uncompressed


RMP_RECORD = 96  # wchar name[32] + int64 offset, stored size, stamp, uncompressed size


def read_rmp(data: bytes) -> list[Chunk]:
    """Map file (.rmp): 'TGCK', int32 version (2), int32 chunk count, int32 0, chunk bodies,
    then `count` 96-byte records ending at EOF. Bodies are zlib streams, except the 4-byte 'MAP'."""
    if data[:4] != MAGIC:
        raise RwdError("not a TGCK map file")
    version, count, _ = struct.unpack_from("<iii", data, 4)
    if version != 2 or not 0 < count < 1000:
        raise RwdError(f"unsupported map header (version {version}, {count} chunks)")
    start = len(data) - count * RMP_RECORD
    chunks = []
    for i in range(count):
        r = start + i * RMP_RECORD
        name = data[r : r + 64].decode("utf-16-le").split("\0")[0]
        offset, stored, stamp, size = struct.unpack_from("<qqqq", data, r + 64)
        if offset < 0 or offset + stored > start:
            raise RwdError(f"chunk {name!r} out of range")
        chunks.append(Chunk(name, offset, stored, stamp, size))
    return chunks


def rmp_chunk_bytes(data: bytes, c: Chunk) -> bytes:
    body = data[c.offset : c.offset + c.stored_size]
    if c.stored_size == c.size and not body.startswith(b"\x78"):
        return body
    raw = zlib.decompress(body)
    if len(raw) != c.size:
        raise RwdError(f"chunk {c.name!r}: {len(raw)} bytes, expected {c.size}")
    return raw


def cmd_rmp(args: argparse.Namespace) -> int:
    data = Path(args.map_file).read_bytes()
    chunks = read_rmp(data)
    out_dir = Path(args.out_dir) if args.out_dir else None
    for c in chunks:
        raw = rmp_chunk_bytes(data, c)
        print(f"{c.name:28} stored {c.stored_size:8}  size {c.size:8}")
        if out_dir:
            out_dir.mkdir(parents=True, exist_ok=True)
            (out_dir / f"{c.name}.bin").write_bytes(raw)
    return 0


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(prog="rwd.py", description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    pl = sub.add_parser("list")
    pl.add_argument("archive")
    pl.add_argument("--pattern")
    pl.add_argument("--long", action="store_true", help="fileType, size, unknown, name")
    pl.add_argument("--summary", action="store_true", help="counts and sizes per extension")
    pl.set_defaults(fn=cmd_list)
    pe = sub.add_parser("extract")
    pe.add_argument("archive")
    pe.add_argument("out_dir")
    pe.add_argument("--pattern")
    pe.set_defaults(fn=cmd_extract)
    pm = sub.add_parser("magic")
    pm.add_argument("archive")
    pm.add_argument("--pattern")
    pm.set_defaults(fn=cmd_magic)
    pr = sub.add_parser("rmp", help="list (and with --out, extract) the chunks of a map file")
    pr.add_argument("map_file")
    pr.add_argument("--out", dest="out_dir")
    pr.set_defaults(fn=cmd_rmp)
    args = p.parse_args(argv[1:])
    try:
        return args.fn(args)
    except BrokenPipeError:
        return 0
    except (OSError, RwdError, zlib.error) as exc:
        if isinstance(exc, OSError) and exc.errno == errno.EINVAL and exc.filename is None:
            return 0  # stdout closed by a pipe reader on Windows (e.g. `| head`)
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
