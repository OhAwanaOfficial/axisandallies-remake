"""Overlay VFS over the original install, same mount order as sim/vfs/vfs.cpp (CLAUDE.md):

    Data.rwd -> data/ -> skins/*.rwd -> Music.rwd -> Speech.rwd -> user overlays

A later depot replaces the whole file at a path. Keys are lower-case with '/' separators.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterator

import rwd


@dataclass(frozen=True)
class VfsFile:
    path: str  # spelling from the winning depot
    depot: str  # e.g. "Data.rwd", "data/"
    size: int
    _archive: Path | None  # .rwd file, or None for a loose file
    _offset: int
    _disk: Path | None  # loose file on disk

    @property
    def key(self) -> str:
        return self.path.lower()

    def source_id(self) -> str:
        """Stable identity of the bytes for incremental imports (depot + offset/mtime + size)."""
        if self._disk is not None:
            st = self._disk.stat()
            return f"{self.depot}|{st.st_mtime_ns}|{st.st_size}"
        return f"{self.depot}|{self._offset}|{self.size}"


class Vfs:
    def __init__(self) -> None:
        self.files: dict[str, VfsFile] = {}
        self.mounted: list[str] = []
        self._handles: dict[Path, BinaryIO] = {}

    # -- mounting
    def mount_rwd(self, archive: Path, name: str) -> None:
        with open(archive, "rb") as f:
            toc = rwd.read_toc(f)
        for e in toc:
            self.files[e.name.lower()] = VfsFile(e.name, name, e.size, archive, e.offset, None)
        self.mounted.append(f"{name} ({len(toc)} files)")

    def mount_dir(self, root: Path, name: str) -> None:
        n = 0
        for dirpath, _, filenames in os.walk(root):
            for fn in filenames:
                disk = Path(dirpath) / fn
                rel = disk.relative_to(root).as_posix()
                self.files[rel.lower()] = VfsFile(rel, name, disk.stat().st_size, None, 0, disk)
                n += 1
        self.mounted.append(f"{name} ({n} files)")

    # -- access
    def find(self, path: str) -> VfsFile | None:
        return self.files.get(path.replace("\\", "/").lstrip("/").lower())

    def read(self, f: VfsFile) -> bytes:
        if f._disk is not None:
            return f._disk.read_bytes()
        assert f._archive is not None
        h = self._handles.get(f._archive)
        if h is None:
            h = self._handles[f._archive] = open(f._archive, "rb")
        h.seek(f._offset)
        return h.read(f.size)

    def iter_ext(self, *exts: str) -> Iterator[VfsFile]:
        exts_l = tuple(e.lower() for e in exts)
        for k in sorted(self.files):
            if k.endswith(exts_l):
                yield self.files[k]

    def close(self) -> None:
        for h in self._handles.values():
            h.close()
        self._handles.clear()


def mount_game(game: Path, user_overlays: list[Path] | None = None) -> Vfs:
    v = Vfs()
    if (game / "Data.rwd").is_file():
        v.mount_rwd(game / "Data.rwd", "Data.rwd")
    if (game / "data").is_dir():
        v.mount_dir(game / "data", "data/")
    skins = sorted((p for p in (game / "skins").glob("*.rwd")), key=lambda p: p.name.upper()) if (game / "skins").is_dir() else []
    for s in skins:
        v.mount_rwd(s, f"skins/{s.name}")
    for a in ("Music.rwd", "Speech.rwd"):
        if (game / a).is_file():
            v.mount_rwd(game / a, a)
    if user_overlays is None:
        profile = os.environ.get("USERPROFILE")
        user_overlays = ([Path(profile) / "Documents" / "Axis&Allies" / "data"] if profile else []) + [game / "Userdata"]
    for o in user_overlays:
        if o.is_dir():
            v.mount_dir(o, f"{o}/")
    if not v.files:
        raise FileNotFoundError(f"nothing to mount in {game}")
    return v


def game_dir_from_config(start: Path) -> Path | None:
    """config/game_path.cfg, searched upward from `start` (same rule as simcli)."""
    for d in [start, *start.parents]:
        cfg = d / "config" / "game_path.cfg"
        if cfg.is_file():
            for line in cfg.read_text(encoding="utf-8").splitlines():
                t = line.strip()
                if t and t[0] not in "#;":
                    return Path(t.strip('"'))
    return None
