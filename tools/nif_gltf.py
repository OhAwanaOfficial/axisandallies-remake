"""Gamebryo NIF / KF -> glTF 2.0 binary (.glb). M2; replaces the M1 spike.

Only dependency: pyffi (pure Python). glTF is written directly (JSON + BIN chunk), no numpy.

Handles: NiNode hierarchies, NiTriShape and NiTriStrips geometry, vertex colours, materials
(NiTexturingProperty base map, NiMaterialProperty, NiAlphaProperty blend/test, NiStencilProperty
two-sided), skinning (NiSkinInstance / NiSkinData, falling back to NiSkinPartition weights),
NiKeyframeController animation embedded in a NIF, and NiControllerSequence clips in KF files.
Particle systems, lights and other effect controllers are skipped and reported.

Conventions:
* Gamebryo is Z-up; a root node "ZupToYup" rotates the scene -90 deg about X for glTF/Godot.
* Clip files (KF) hold one flat node per animated target, named like the NIF node it drives.
  Consumers retarget by name, as Gamebryo does (see the Model Browser).
* Node extras: nif_type, hidden (NiAVObject flag bit 0), selection_box (local AABB of SelectionBox).
* Animation extras: duration, cycle_type, frequency, text_keys [[time, text], ...].
"""

from __future__ import annotations

import io
import json
import math
import struct
import time
from array import array
from dataclasses import dataclass, field
from typing import Callable, Iterable, Sequence
from urllib.parse import quote

import sys  # noqa: E402
import types  # noqa: E402

# pyffi 2.2.3 compatibility with Python >= 3.12:
time.clock = time.perf_counter  # type: ignore[attr-defined]  # removed in 3.8, still called
if "distutils.cmd" not in sys.modules:  # distutils removed in 3.12; pyffi only subclasses Command
    try:
        import distutils.cmd  # noqa: F401
    except ImportError:
        _cmd = types.ModuleType("distutils.cmd")
        _cmd.Command = type("Command", (), {})  # type: ignore[attr-defined]
        sys.modules.setdefault("distutils", types.ModuleType("distutils"))
        sys.modules["distutils.cmd"] = _cmd

from pyffi.formats.nif import NifFormat  # noqa: E402

# Bump when the output changes, so import_game.py reconverts cached files.
CONVERTER_VERSION = 3
SAMPLE_FPS = 30.0
ZUP_TO_YUP = [-math.sqrt(0.5), 0.0, 0.0, math.sqrt(0.5)]  # quaternion x, y, z, w

# glTF enums
FLOAT, U16, U32 = 5126, 5123, 5125
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963

GEOMETRY = (NifFormat.NiTriShape, NifFormat.NiTriStrips)
SCENE_NODES = (NifFormat.NiNode, NifFormat.NiTriShape, NifFormat.NiTriStrips, NifFormat.NiCamera)

# Resolves the texture file name stored in the NIF -> URI relative to the output .glb, or None.
TextureResolver = Callable[[str], "str | None"]
# Stores a texture embedded in the NIF (DDS bytes, index) -> URI relative to the output .glb, or None.
EmbeddedTextureSink = Callable[[bytes, int], "str | None"]
Mat4 = list[list[float]]


@dataclass
class ConvertReport:
    nodes: int = 0
    meshes: int = 0
    skins: int = 0
    animations: int = 0
    textures: list[str] = field(default_factory=list)
    missing_textures: list[str] = field(default_factory=list)
    skipped: dict[str, int] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)
    node_names: list[str] = field(default_factory=list)
    clip_targets: list[str] = field(default_factory=list)
    clips: list[dict] = field(default_factory=list)

    def skip(self, what: str) -> None:
        self.skipped[what] = self.skipped.get(what, 0) + 1


def read_nif(data: bytes):
    nif = NifFormat.Data()
    f = io.BytesIO(data)
    nif.inspect(f)
    nif.read(f)
    return nif


def s(b) -> str:
    return b.decode("latin1") if isinstance(b, (bytes, bytearray)) else str(b or "")


# ---------------------------------------------------------------- small linear algebra
def mat3(r) -> list[list[float]]:
    """pyffi Matrix33 (row-vector convention) -> 3x3 column-vector rotation."""
    return [[r.m_11, r.m_21, r.m_31], [r.m_12, r.m_22, r.m_32], [r.m_13, r.m_23, r.m_33]]


def xform(rot, trans, scale) -> Mat4:
    m3 = mat3(rot)
    sc = float(scale)
    return [[m3[0][0] * sc, m3[0][1] * sc, m3[0][2] * sc, trans.x],
            [m3[1][0] * sc, m3[1][1] * sc, m3[1][2] * sc, trans.y],
            [m3[2][0] * sc, m3[2][1] * sc, m3[2][2] * sc, trans.z],
            [0.0, 0.0, 0.0, 1.0]]


def identity() -> Mat4:
    return [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def matmul(a: Mat4, b: Mat4) -> Mat4:
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def inverse(m: Mat4) -> Mat4:
    a = [row[:] + [1.0 if i == j else 0.0 for j in range(4)] for i, row in enumerate(m)]
    for c in range(4):
        p = max(range(c, 4), key=lambda r: abs(a[r][c]))
        if abs(a[p][c]) < 1e-12:
            raise ValueError("singular matrix")
        a[c], a[p] = a[p], a[c]
        piv = a[c][c]
        a[c] = [v / piv for v in a[c]]
        for r in range(4):
            if r != c and a[r][c]:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return [row[4:] for row in a]


def quat_from_mat3(m) -> list[float]:
    """Rotation matrix (column vectors) -> unit quaternion [x, y, z, w]."""
    t = m[0][0] + m[1][1] + m[2][2]
    if t > 0:
        k = math.sqrt(t + 1.0) * 2
        q = [(m[2][1] - m[1][2]) / k, (m[0][2] - m[2][0]) / k, (m[1][0] - m[0][1]) / k, 0.25 * k]
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        k = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        q = [0.25 * k, (m[0][1] + m[1][0]) / k, (m[0][2] + m[2][0]) / k, (m[2][1] - m[1][2]) / k]
    elif m[1][1] > m[2][2]:
        k = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        q = [(m[0][1] + m[1][0]) / k, 0.25 * k, (m[1][2] + m[2][1]) / k, (m[0][2] - m[2][0]) / k]
    else:
        k = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
        q = [(m[0][2] + m[2][0]) / k, (m[1][2] + m[2][1]) / k, 0.25 * k, (m[1][0] - m[0][1]) / k]
    return normalize(q)


def normalize(q: Sequence[float]) -> list[float]:
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return [c / n for c in q]


def qmul(a, b) -> list[float]:
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz]


def slerp(a, b, t) -> list[float]:
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b, d = [-c for c in b], -d
    if d > 0.9995:
        return normalize([x + (y - x) * t for x, y in zip(a, b)])
    th = math.acos(min(1.0, d))
    sa, sb, st = math.sin((1 - t) * th), math.sin(t * th), math.sin(th)
    return normalize([(x * sa + y * sb) / st for x, y in zip(a, b)])


def euler_xyz_to_quat(x: float, y: float, z: float) -> list[float]:
    """Gamebryo XYZ rotation keys: X applied first, then Y, then Z (q = qz * qy * qx)."""
    qx = [math.sin(x / 2), 0.0, 0.0, math.cos(x / 2)]
    qy = [0.0, math.sin(y / 2), 0.0, math.cos(y / 2)]
    qz = [0.0, 0.0, math.sin(z / 2), math.cos(z / 2)]
    return qmul(qz, qmul(qy, qx))


# ---------------------------------------------------------------- key evaluation
LINEAR, QUADRATIC, TBC, XYZ, CONST = 1, 2, 3, 4, 5


def _vec(v) -> list[float]:
    if hasattr(v, "x"):
        return [v.x, v.y, v.z]
    return [float(v)]


def _lin(a, b, u):
    return [x + (y - x) * u for x, y in zip(a, b)]


def _tbc_tangents(keys, i):
    p = _vec(keys[i].value)
    pv = _vec(keys[i - 1].value) if i > 0 else p
    nx = _vec(keys[i + 1].value) if i + 1 < len(keys) else p
    t, b, c = keys[i].tbc.t, keys[i].tbc.b, keys[i].tbc.c
    d0 = [x - y for x, y in zip(p, pv)]
    d1 = [x - y for x, y in zip(nx, p)]
    out_t = [0.5 * ((1 - t) * (1 + b) * (1 + c) * a + (1 - t) * (1 - b) * (1 - c) * bb) for a, bb in zip(d0, d1)]
    in_t = [0.5 * ((1 - t) * (1 + b) * (1 - c) * a + (1 - t) * (1 - b) * (1 + c) * bb) for a, bb in zip(d0, d1)]
    return in_t, out_t


def eval_keygroup(group, t: float) -> list[float] | None:
    """Evaluate a NIF KeyGroup (float or Vector3 keys) at time t."""
    keys = list(group.keys)[: group.num_keys]
    if not keys:
        return None
    if t <= keys[0].time or len(keys) == 1:
        return _vec(keys[0].value)
    if t >= keys[-1].time:
        return _vec(keys[-1].value)
    i = max(j for j in range(len(keys) - 1) if keys[j].time <= t)
    k0, k1 = keys[i], keys[i + 1]
    u = (t - k0.time) / ((k1.time - k0.time) or 1e-9)
    p0, p1 = _vec(k0.value), _vec(k1.value)
    interp = group.interpolation
    if interp == CONST:
        return p0
    if interp == QUADRATIC:
        m0, m1 = _vec(k0.forward), _vec(k1.backward)
    elif interp == TBC:
        m0, m1 = _tbc_tangents(keys, i)[1], _tbc_tangents(keys, i + 1)[0]
    else:
        return _lin(p0, p1, u)
    u2, u3 = u * u, u * u * u
    h00, h10, h01, h11 = 2 * u3 - 3 * u2 + 1, u3 - 2 * u2 + u, -2 * u3 + 3 * u2, u3 - u2
    return [h00 * a + h10 * b + h01 * c + h11 * d for a, b, c, d in zip(p0, m0, p1, m1)]


def key_times(group) -> list[float]:
    return [k.time for k in list(group.keys)[: group.num_keys]]


def sample_times(t0: float, t1: float, exact: Iterable[float], resample: bool) -> list[float]:
    ts = {round(x, 6) for x in exact if t0 - 1e-6 <= x <= t1 + 1e-6}
    if resample:
        n = max(1, int(math.ceil((t1 - t0) * SAMPLE_FPS)))
        ts.update(round(t0 + (t1 - t0) * i / n, 6) for i in range(n + 1))
    ts.update([round(t0, 6), round(t1, 6)])
    return sorted(ts)


def eval_rotation(kd, t: float) -> list[float] | None:
    if kd.rotation_type == XYZ:
        ang = [eval_keygroup(g, t) for g in kd.xyz_rotations]
        x, y, z = (a[0] if a is not None else 0.0 for a in ang)
        return euler_xyz_to_quat(x, y, z)
    keys = list(kd.quaternion_keys)[: kd.num_rotation_keys]
    if not keys:
        return None

    def q(k):
        return [k.value.x, k.value.y, k.value.z, k.value.w]

    if t <= keys[0].time or len(keys) == 1:
        return q(keys[0])
    if t >= keys[-1].time:
        return q(keys[-1])
    i = max(j for j in range(len(keys) - 1) if keys[j].time <= t)
    if kd.rotation_type == CONST:
        return q(keys[i])
    u = (t - keys[i].time) / ((keys[i + 1].time - keys[i].time) or 1e-9)
    return slerp(q(keys[i]), q(keys[i + 1]), u)  # quadratic/TBC quaternion keys approximated by slerp


def fix_quat_signs(qs: list[list[float]]) -> list[list[float]]:
    for i in range(1, len(qs)):
        if sum(a * b for a, b in zip(qs[i - 1], qs[i])) < 0:
            qs[i] = [-c for c in qs[i]]
    return qs


# ---------------------------------------------------------------- embedded textures
PX_RGB8, PX_RGBA8, PX_DXT1, PX_DXT5 = 0, 1, 4, 5  # NiPixelData.pixel_format values seen in this game


def pixel_data_to_dds(pix) -> bytes:
    """NiPixelData -> DDS bytes (pyffi 2.2.3's own save_as_dds is broken). RGB8 is widened to RGBA8."""
    fmt = int(pix.pixel_format)
    raw = bytes(bytearray(pix.pixel_data[0]))
    mips = list(pix.mipmaps)[: pix.num_mipmaps]
    w, h = mips[0].width, mips[0].height
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000  # CAPS HEIGHT WIDTH PIXELFORMAT MIPMAPCOUNT
    if fmt in (PX_DXT1, PX_DXT5):
        body = raw
        pf = struct.pack("<II4sIIIII", 32, 0x4, b"DXT1" if fmt == PX_DXT1 else b"DXT5", 0, 0, 0, 0, 0)
        flags |= 0x80000  # LINEARSIZE
        pitch = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if fmt == PX_DXT1 else 16)
    elif fmt in (PX_RGB8, PX_RGBA8):
        bpp = pix.bytes_per_pixel or (3 if fmt == PX_RGB8 else 4)
        if fmt == PX_RGB8:
            out = bytearray()
            for mp in mips:
                n = mp.width * mp.height
                seg = raw[mp.offset: mp.offset + n * bpp]
                for i in range(0, len(seg), bpp):
                    out += seg[i:i + 3] + bytes((255,))
            body = bytes(out)
        else:
            body = raw
        pf = struct.pack("<II4sIIIII", 32, 0x41, bytes(4), 32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000)  # RGB|ALPHA
        flags |= 0x8  # PITCH
        pitch = w * 4
    else:
        raise ValueError(f"unsupported NiPixelData format {fmt}")
    caps = 0x1000 | (0x400008 if len(mips) > 1 else 0)  # TEXTURE (| COMPLEX | MIPMAP)
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, flags, h, w, pitch, 0, len(mips)) + pf
    header += struct.pack("<IIII4x", caps, 0, 0, 0)
    return header + body


# ---------------------------------------------------------------- glTF writer
class Gltf:
    def __init__(self) -> None:
        self.j: dict = {"asset": {"version": "2.0", "generator": "Project Corps import_game.py"},
                        "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "accessors": [],
                        "bufferViews": [], "materials": [], "images": [], "textures": [], "skins": [],
                        "animations": [], "cameras": [],
                        "samplers": [{"magFilter": 9729, "minFilter": 9987}]}
        self.blob = bytearray()

    def add(self, kind: str, obj: dict) -> int:
        self.j[kind].append(obj)
        return len(self.j[kind]) - 1

    def accessor(self, rows: Sequence[Sequence[float]] | Sequence[float], ctype: int, typ: str,
                 target: int | None = None, minmax: bool = False) -> int:
        flat: list = []
        width = 1
        if rows and isinstance(rows[0], (list, tuple)):
            width = len(rows[0])  # type: ignore[arg-type]
            for r in rows:
                flat.extend(r)  # type: ignore[arg-type]
        else:
            flat = list(rows)
        code = {FLOAT: "f", U16: "H", U32: "I"}[ctype]
        data = array(code, flat).tobytes()
        while len(self.blob) % 4:
            self.blob += b"\0"
        view = {"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.blob += data
        acc = {"bufferView": self.add("bufferViews", view), "componentType": ctype,
               "count": len(flat) // width, "type": typ}
        if minmax or (typ == "VEC3" and target == ARRAY_BUFFER):
            cols = [flat[i::width] for i in range(width)]
            acc["min"], acc["max"] = [min(c) for c in cols], [max(c) for c in cols]
        return self.add("accessors", acc)

    def to_glb(self, root_children: list[int]) -> bytes:
        root = self.add("nodes", {"name": "ZupToYup", "rotation": ZUP_TO_YUP, "children": root_children})
        self.j["scenes"][0]["nodes"] = [root]
        j = {k: v for k, v in self.j.items() if v != []}
        while len(self.blob) % 4:
            self.blob += b"\0"
        j["buffers"] = [{"byteLength": len(self.blob)}]
        js = json.dumps(j, separators=(",", ":")).encode("utf-8")
        js += b" " * (-len(js) % 4)
        total = 12 + 8 + len(js) + 8 + len(self.blob)
        return (struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(js), 0x4E4F534A) + js
                + struct.pack("<II", len(self.blob), 0x004E4942) + bytes(self.blob))


# ---------------------------------------------------------------- NIF scene conversion
class NifConverter(Gltf):
    def __init__(self, resolve_texture: TextureResolver, report: ConvertReport,
                 embedded_sink: EmbeddedTextureSink | None = None) -> None:
        super().__init__()
        self.resolve_texture = resolve_texture
        self.embedded_sink = embedded_sink
        self.embedded: dict[int, str | None] = {}  # id(NiPixelData) -> URI
        self.r = report
        self.index: dict[int, int] = {}  # id(nif block) -> glTF node
        self.world: dict[int, Mat4] = {}  # id(nif block) -> world matrix (NIF space)
        self.skinned: list[tuple[int, object]] = []
        self.controllers: list[tuple[int, object]] = []
        self.mats: dict[tuple, int] = {}
        self.tex_index: dict[str, int] = {}

    def material(self, shape) -> int:
        tex = None
        embedded_uri: str | None = None
        alpha_mode, cutoff, double_sided = "OPAQUE", None, False
        diffuse, alpha, emissive = [1.0, 1.0, 1.0], 1.0, [0.0, 0.0, 0.0]
        for p in shape.properties:
            if p is None:
                continue
            if isinstance(p, NifFormat.NiTexturingProperty) and p.has_base_texture and p.base_texture.source:
                src = p.base_texture.source
                if getattr(src, "use_external", 1):
                    tex = s(src.file_name)
                else:
                    embedded_uri = self.embedded_texture(src)
            elif isinstance(p, NifFormat.NiMaterialProperty):
                d, e = p.diffuse_color, p.emissive_color
                diffuse, alpha, emissive = [d.r, d.g, d.b], float(p.alpha), [e.r, e.g, e.b]
            elif isinstance(p, NifFormat.NiAlphaProperty):
                if p.flags & (1 << 9):
                    alpha_mode, cutoff = "MASK", max(int(p.threshold), 1) / 255.0
                elif p.flags & 1:
                    alpha_mode = "BLEND"
            elif isinstance(p, NifFormat.NiStencilProperty):
                double_sided = getattr(p, "draw_mode", 0) == 3
        if alpha < 0.999 and alpha_mode == "OPAQUE":
            alpha_mode = "BLEND"
        uri = self.resolve_texture(tex) if tex else embedded_uri
        if tex:
            (self.r.textures if uri else self.r.missing_textures).append(uri or tex)
        key = (uri or tex or "", alpha_mode, cutoff, double_sided, tuple(round(c, 4) for c in diffuse),
               round(alpha, 4), tuple(round(c, 4) for c in emissive))
        if key in self.mats:
            return self.mats[key]
        pbr: dict = {"baseColorFactor": [*diffuse, alpha], "metallicFactor": 0.0, "roughnessFactor": 0.9}
        if uri:
            if uri not in self.tex_index:
                img = self.add("images", {"uri": quote(uri, safe="/")})
                self.tex_index[uri] = self.add("textures", {"source": img, "sampler": 0})
            pbr["baseColorTexture"] = {"index": self.tex_index[uri]}
        m: dict = {"name": (tex or "untextured").replace("\\", "/").rsplit("/", 1)[-1],
                   "pbrMetallicRoughness": pbr, "alphaMode": alpha_mode, "doubleSided": double_sided}
        if any(emissive):
            m["emissiveFactor"] = emissive
        if cutoff is not None:
            m["alphaCutoff"] = cutoff
        if tex:
            m["extras"] = {"nif_texture": tex, "texture": uri}
        self.mats[key] = self.add("materials", m)
        return self.mats[key]

    def embedded_texture(self, src) -> str | None:
        """NiSourceTexture with internal NiPixelData -> DDS written through the sink."""
        pix = getattr(src, "pixel_data", None)
        if pix is None or self.embedded_sink is None:
            self.r.warnings.append("embedded texture (NiPixelData) not exported")
            return None
        if id(pix) not in self.embedded:
            try:
                self.embedded[id(pix)] = self.embedded_sink(pixel_data_to_dds(pix), len(self.embedded))
            except Exception as exc:  # noqa: BLE001 - unusual pixel formats
                self.r.warnings.append(f"embedded texture not exported ({type(exc).__name__}: {exc})"[:160])
                self.embedded[id(pix)] = None
        return self.embedded[id(pix)]

    @staticmethod
    def uses_vertex_colors(shape) -> bool:
        for p in shape.properties:
            if isinstance(p, NifFormat.NiVertexColorProperty):
                return getattr(p, "vertex_mode", 2) == 2  # SRC_AMB_DIF; IGNORE/EMISSIVE are not albedo
        return True

    def primitive(self, shape) -> dict | None:
        d = shape.data
        if d is None or not d.num_vertices:
            return None
        if isinstance(d, NifFormat.NiTriStripsData):
            tris = list(d.get_triangles())
        else:
            tris = [(t.v_1, t.v_2, t.v_3) for t in d.triangles]
        nv = d.num_vertices
        tris = [t for t in tris if len(set(t)) == 3 and max(t) < nv]
        if not tris:
            return None
        attrs = {"POSITION": self.accessor([(v.x, v.y, v.z) for v in d.vertices], FLOAT, "VEC3", ARRAY_BUFFER)}
        if d.has_normals:
            attrs["NORMAL"] = self.accessor([(v.x, v.y, v.z) for v in d.normals], FLOAT, "VEC3", ARRAY_BUFFER)
        num_uv = (d.num_uv_sets & 0x3F) if hasattr(d, "num_uv_sets") else 0
        if num_uv and d.uv_sets:
            attrs["TEXCOORD_0"] = self.accessor([(uv.u, uv.v) for uv in d.uv_sets[0]], FLOAT, "VEC2", ARRAY_BUFFER)
        if d.has_vertex_colors and self.uses_vertex_colors(shape):
            attrs["COLOR_0"] = self.accessor([(c.r, c.g, c.b, c.a) for c in d.vertex_colors], FLOAT, "VEC4",
                                             ARRAY_BUFFER)
        flat = [i for t in tris for i in t]
        ind = self.accessor(flat, U16 if nv <= 0xFFFF else U32, "SCALAR", ELEMENT_ARRAY_BUFFER)
        return {"attributes": attrs, "indices": ind, "material": self.material(shape)}

    def visit(self, n, parent_world: Mat4) -> int | None:
        if n is None:
            return None
        tname = type(n).__name__
        if not isinstance(n, SCENE_NODES):
            self.r.skip(tname)
            return None
        name = s(n.name)
        local = xform(n.rotation, n.translation, n.scale)
        world = matmul(parent_world, local)
        node: dict = {"name": name or tname,
                      "translation": [n.translation.x, n.translation.y, n.translation.z],
                      "rotation": quat_from_mat3(mat3(n.rotation)), "scale": [float(n.scale)] * 3}
        extras: dict = {"nif_type": tname}
        if n.flags & 1:
            extras["hidden"] = True
        idx = self.add("nodes", node)
        self.index[id(n)] = idx
        self.world[id(n)] = world
        self.r.node_names.append(name)
        if getattr(n, "controller", None) is not None:
            self.controllers.append((idx, n))
        children: list[int] = []
        if isinstance(n, GEOMETRY):
            if name.lower().startswith("selectionbox"):
                d = n.data
                if d is not None and d.num_vertices:
                    xs, ys, zs = zip(*((p.x, p.y, p.z) for p in d.vertices))
                    extras["selection_box"] = {"min": [min(xs), min(ys), min(zs)], "max": [max(xs), max(ys), max(zs)]}
            else:
                prim = self.primitive(n)
                if prim is not None:
                    node["mesh"] = self.add("meshes", {"name": name, "primitives": [prim]})
                    self.r.meshes += 1
                    if getattr(n, "skin_instance", None) is not None:
                        self.skinned.append((idx, n))
        elif isinstance(n, NifFormat.NiCamera):
            h = max(abs(n.frustum_top - n.frustum_bottom), 1e-4)
            w = max(abs(n.frustum_right - n.frustum_left), 1e-4)
            near = max(float(n.frustum_near), 0.01)
            cam = self.add("cameras", {"type": "perspective", "perspective": {
                "yfov": 2 * math.atan(h / 2), "aspectRatio": w / h, "znear": near,
                "zfar": max(float(n.frustum_far), near * 10)}})
            # Gamebryo cameras look down +X with +Z up; glTF cameras look down -Z with +Y up.
            children.append(self.add("nodes", {"name": name + "_view", "rotation": [0.5, -0.5, -0.5, 0.5],
                                               "camera": cam}))
        node["extras"] = extras
        for c in getattr(n, "children", None) or []:
            ci = self.visit(c, world)
            if ci is not None:
                children.append(ci)
        if children:
            node["children"] = children
        return idx

    def build_skins(self) -> None:
        for node_idx, shape in self.skinned:
            inst = shape.skin_instance  # type: ignore[attr-defined]
            sd = inst.data
            bones = list(inst.bones)
            if any(b is None or id(b) not in self.index for b in bones):
                self.r.warnings.append(f"{s(shape.name)}: skin bone outside the hierarchy, left unskinned")  # type: ignore[attr-defined]
                continue
            nv = shape.data.num_vertices  # type: ignore[attr-defined]
            weights: list[list[tuple[int, float]]] = [[] for _ in range(nv)]
            if sd is not None and getattr(sd, "has_vertex_weights", 1):
                for bi, bd in enumerate(sd.bone_list):
                    for vw in bd.vertex_weights:
                        if vw.index < nv and vw.weight > 0:
                            weights[vw.index].append((bi, float(vw.weight)))
            if not any(weights) and inst.skin_partition is not None:
                for part in inst.skin_partition.skin_partition_blocks:
                    vmap = list(part.vertex_map) if part.has_vertex_map else list(range(part.num_vertices))
                    for pv, v in enumerate(vmap):
                        for k in range(part.num_weights_per_vertex):
                            w = part.vertex_weights[pv][k] if part.has_vertex_weights else 1.0
                            bl = part.bone_indices[pv][k] if part.has_bone_indices else k
                            if w > 0 and v < nv:
                                weights[v].append((part.bones[bl], float(w)))
            joints, wts = [], []
            for lst in weights:
                lst = sorted(lst, key=lambda x: -x[1])[:4] or [(0, 1.0)]
                tot = sum(w for _, w in lst) or 1.0
                lst = lst + [(0, 0.0)] * (4 - len(lst))
                joints.append([b for b, _ in lst])
                wts.append([w / tot for _, w in lst])
            # Inverse bind matrices. Gamebryo deforms skin-space vertices (the shape's own vertex data) with
            # world(bone) * NiSkinData.bone_list[i].skin_transform ("skin to bone"), so that is the IBM.
            # Many files are saved posed, so world(bone)^-1 * world(shape) is only a fallback.
            if sd is not None and len(sd.bone_list) == len(bones):
                ibm = [xform(bd.skin_transform.rotation, bd.skin_transform.translation, bd.skin_transform.scale)
                       for bd in sd.bone_list]
            else:
                shape_world = self.world[id(shape)]
                ibm = [matmul(inverse(self.world[id(b)]), shape_world) for b in bones]
                self.r.warnings.append(f"{s(shape.name)}: no NiSkinData bind pose, using the file pose")  # type: ignore[attr-defined]
            prim = self.j["meshes"][self.j["nodes"][node_idx]["mesh"]]["primitives"][0]
            prim["attributes"]["JOINTS_0"] = self.accessor(joints, U16, "VEC4", ARRAY_BUFFER)
            prim["attributes"]["WEIGHTS_0"] = self.accessor(wts, FLOAT, "VEC4", ARRAY_BUFFER)
            cols = [[m[r][c] for c in range(4) for r in range(4)] for m in ibm]  # column-major
            skin: dict = {"name": s(shape.name), "joints": [self.index[id(b)] for b in bones],  # type: ignore[attr-defined]
                          "inverseBindMatrices": self.accessor(cols, FLOAT, "MAT4")}
            if inst.skeleton_root is not None and id(inst.skeleton_root) in self.index:
                skin["skeleton"] = self.index[id(inst.skeleton_root)]
            self.j["nodes"][node_idx]["skin"] = self.add("skins", skin)
            self.r.skins += 1

    def build_embedded_animation(self) -> None:
        anim: dict = {"name": "embedded", "channels": [], "samplers": []}
        t_end = 0.0
        for node_idx, n in self.controllers:
            c = n.controller  # type: ignore[attr-defined]
            while c is not None:
                if isinstance(c, NifFormat.NiKeyframeController) and c.data is not None:
                    t_end = max(t_end, add_channels(self, anim, node_idx, c.data, float(c.start_time),
                                                    float(c.stop_time)))
                else:
                    self.r.skip(type(c).__name__)
                c = c.next_controller
        if anim["channels"]:
            anim["extras"] = {"duration": t_end}
            self.add("animations", anim)
            self.r.animations += 1
            self.r.clips.append({"name": "embedded", "duration": t_end, "text_keys": []})


def add_channels(g: Gltf, anim: dict, node_idx: int, kd, t0: float, t1: float) -> float:
    """Append translation/rotation/scale channels for one NiKeyframeData. Returns the clip length."""
    if t1 <= t0:
        t1 = t0 + 1e-3

    def channel(path: str, times: list[float], values: list, typ: str) -> None:
        tin = g.accessor([t - t0 for t in times], FLOAT, "SCALAR", minmax=True)
        tout = g.accessor(values, FLOAT, typ)
        anim["samplers"].append({"input": tin, "output": tout, "interpolation": "LINEAR"})
        anim["channels"].append({"sampler": len(anim["samplers"]) - 1, "target": {"node": node_idx, "path": path}})

    tr = kd.translations
    if tr.num_keys:
        ts = sample_times(t0, t1, key_times(tr), tr.interpolation != LINEAR)
        channel("translation", ts, [eval_keygroup(tr, t) for t in ts], "VEC3")
    if kd.rotation_type == XYZ:
        groups = list(kd.xyz_rotations)
        if any(gr.num_keys for gr in groups):
            ts = sample_times(t0, t1, [t for gr in groups for t in key_times(gr)], True)
            channel("rotation", ts, fix_quat_signs([eval_rotation(kd, t) for t in ts]), "VEC4")  # type: ignore[misc]
    elif kd.num_rotation_keys:
        keys = list(kd.quaternion_keys)[: kd.num_rotation_keys]
        ts = sample_times(t0, t1, [k.time for k in keys], kd.rotation_type not in (LINEAR, CONST))
        channel("rotation", ts, fix_quat_signs([eval_rotation(kd, t) for t in ts]), "VEC4")  # type: ignore[misc]
    sc = kd.scales
    if sc.num_keys:
        ts = sample_times(t0, t1, key_times(sc), sc.interpolation != LINEAR)
        channel("scale", ts, [eval_keygroup(sc, t) * 3 for t in ts], "VEC3")  # type: ignore[operator]
    return t1 - t0


# ---------------------------------------------------------------- entry points
def convert_nif(data: bytes, resolve_texture: TextureResolver,
                embedded_sink: EmbeddedTextureSink | None = None) -> tuple[bytes, ConvertReport]:
    nif = read_nif(data)
    rep = ConvertReport()
    conv = NifConverter(resolve_texture, rep, embedded_sink)
    roots: list[int] = []
    for root in nif.roots:
        if root is None:
            continue
        i = conv.visit(root, identity())
        if i is not None:
            roots.append(i)
    conv.build_skins()
    conv.build_embedded_animation()
    rep.nodes = len(conv.j["nodes"])
    return conv.to_glb(roots), rep


def clip_range(seq, ctrls, text_keys: list[tuple[float, str]]) -> tuple[float, float]:
    """KF 10.1.0.0 sequences carry no start/stop time (added in 10.1.0.106): use the 'start'/'end'
    text keys, else the controllers' own ranges."""
    t0, t1 = float(getattr(seq, "start_time", 0.0) or 0.0), float(getattr(seq, "stop_time", 0.0) or 0.0)
    if t1 > t0:
        return t0, t1
    marks = {v.strip().lower(): t for t, v in text_keys}
    if "start" in marks and "end" in marks and marks["end"] > marks["start"]:
        return marks["start"], marks["end"]
    if ctrls:
        return min(float(c.start_time) for c in ctrls), max(float(c.stop_time) for c in ctrls)
    return 0.0, 0.0


def convert_kf(data: bytes) -> tuple[bytes, ConvertReport]:
    """One glTF animation per NiControllerSequence; one flat node per target name."""
    nif = read_nif(data)
    rep = ConvertReport()
    g = Gltf()
    nodes: dict[str, int] = {}
    for seq in nif.roots:
        if not isinstance(seq, NifFormat.NiControllerSequence):
            if seq is not None:
                rep.skip(type(seq).__name__)
            continue
        anim: dict = {"name": s(seq.name), "channels": [], "samplers": []}
        links = [(lk, getattr(lk, "controller", None)) for lk in seq.controlled_blocks]
        ctrls = [c for _, c in links if isinstance(c, NifFormat.NiKeyframeController)]
        tk_raw = []
        if getattr(seq, "text_keys", None) is not None:
            tk_raw = [(float(k.time), s(k.value)) for k in seq.text_keys.text_keys]
        t0, t1 = clip_range(seq, ctrls, tk_raw)
        for link, ctrl in links:
            target = s(getattr(link, "target_name", b"") or getattr(link, "node_name", b""))
            if not target or ctrl is None:
                continue
            if not isinstance(ctrl, NifFormat.NiKeyframeController) or ctrl.data is None:
                rep.skip(type(ctrl).__name__)
                continue
            if target not in nodes:
                nodes[target] = g.add("nodes", {"name": target})
            add_channels(g, anim, nodes[target], ctrl.data, t0, t1)
        tk = [[round(t - t0, 6), v] for t, v in tk_raw]
        flags = ctrls[0].flags if ctrls else 8
        clip = {"name": anim["name"], "duration": round(t1 - t0, 6),
                "cycle_type": ("loop", "reverse", "clamp", "loop")[(flags >> 1) & 3],
                "frequency": float(ctrls[0].frequency) if ctrls else 1.0, "text_keys": tk}
        anim["extras"] = {k: v for k, v in clip.items() if k != "name"}
        if anim["channels"]:
            g.add("animations", anim)
            rep.animations += 1
            rep.clips.append(clip)
    rep.clip_targets = list(nodes)
    rep.nodes = len(g.j["nodes"])
    return g.to_glb(list(nodes.values())), rep
