"""Fail if `float`/`double` (or floating-point literals) appear in sim/ code.

Usage: python -I tools/check_no_float.py <sim_dir>
       python -I tools/check_no_float.py --self-test
Comments and string/char literals are ignored.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

EXTS = {".cpp", ".cc", ".cxx", ".hpp", ".h", ".hh", ".inl", ".ipp"}

_STRIP = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|(?<![\w])'(?:\\.|[^'\\\n])+'",
    re.DOTALL,
)
_RULES = [
    (re.compile(r"\b(?:float|double)\b"), "float/double type"),
    (
        re.compile(r"(?<![\w.])(?:\d+\.\d*|\.\d+)(?:[eE][+-]?\d+)?[fFlL]?(?![\w.])"),
        "floating-point literal",
    ),
]


def _blank(match: re.Match[str]) -> str:
    return re.sub(r"[^\n]", " ", match.group(0))


def scanText(text: str) -> list[tuple[int, str]]:
    code = _STRIP.sub(_blank, text)
    hits: list[tuple[int, str]] = []
    for lineno, line in enumerate(code.splitlines(), 1):
        for rx, label in _RULES:
            if rx.search(line):
                hits.append((lineno, label))
    return hits


def scanDir(root: Path) -> int:
    bad = 0
    for path in sorted(root.rglob("*")):
        if path.suffix.lower() not in EXTS or not path.is_file():
            continue
        for lineno, label in scanText(path.read_text(encoding="utf-8", errors="replace")):
            print(f"{path}:{lineno}: {label} is forbidden in sim/")
            bad += 1
    return bad


def selfTest() -> int:
    must_fail = ["float x;", "double y = 1;", "auto z = 1.5;", "auto w = 2.0f;", "int a = 1; float\nb;"]
    must_pass = [
        "// float in comment",
        "/* double */ int x = 1'000;",
        'const char* s = "float 1.5";',
        "int x = 5; auto c = 'f';",
    ]
    ok = all(scanText(s) for s in must_fail) and not any(scanText(s) for s in must_pass)
    print("self-test", "ok" if ok else "FAILED")
    return 0 if ok else 1


def main(argv: list[str]) -> int:
    if len(argv) == 2 and argv[1] == "--self-test":
        return selfTest()
    if len(argv) != 2:
        print(__doc__)
        return 2
    root = Path(argv[1])
    if not root.is_dir():
        print(f"not a directory: {root}")
        return 2
    bad = scanDir(root)
    print("no_float_in_sim:", "FAIL" if bad else "ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
