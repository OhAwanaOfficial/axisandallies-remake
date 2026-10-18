"""Tests for tools/nif_gltf.py and tools/import_game.py helpers (no game data needed).

Skipped when pyffi isn't installed (it is only needed for the import tools).
Run: python -I -m unittest discover -s tools/tests
"""

from __future__ import annotations

import json
import math
import struct
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace as NS

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    import nif_gltf as N
except ImportError:  # pyffi missing
    N = None  # type: ignore[assignment]
import import_game as IG  # noqa: E402  (no pyffi needed at import time)


def key(t, v, fwd=None, bwd=None, tbc=(0.0, 0.0, 0.0)):
    return NS(time=t, value=v, forward=fwd, backward=bwd, tbc=NS(t=tbc[0], b=tbc[1], c=tbc[2]))


def group(interp, keys):
    return NS(interpolation=interp, num_keys=len(keys), keys=keys)


@unittest.skipIf(N is None, "pyffi not installed")
class KeyEvalTests(unittest.TestCase):
    def test_linear_and_clamp(self) -> None:
        g = group(N.LINEAR, [key(0.0, 0.0), key(2.0, 4.0)])
        self.assertAlmostEqual(N.eval_keygroup(g, 1.0)[0], 2.0)
        self.assertAlmostEqual(N.eval_keygroup(g, -1.0)[0], 0.0)
        self.assertAlmostEqual(N.eval_keygroup(g, 9.0)[0], 4.0)

    def test_quadratic_hits_keys_and_uses_tangents(self) -> None:
        g = group(N.QUADRATIC, [key(0.0, 0.0, fwd=0.0, bwd=0.0), key(1.0, 1.0, fwd=0.0, bwd=0.0)])
        self.assertAlmostEqual(N.eval_keygroup(g, 0.5)[0], 0.5)  # zero tangents: smoothstep midpoint
        self.assertLess(N.eval_keygroup(g, 0.25)[0], 0.25)  # ease-in

    def test_const_steps(self) -> None:
        g = group(N.CONST, [key(0.0, 1.0), key(1.0, 5.0)])
        self.assertEqual(N.eval_keygroup(g, 0.99)[0], 1.0)

    def test_tbc_zero_params_is_catmull_rom(self) -> None:
        g = group(N.TBC, [key(0.0, 0.0), key(1.0, 1.0), key(2.0, 2.0)])
        # First key has no predecessor, so its out-tangent is half the slope: 0.5 + h10*0.5 + h11*1 = 0.4375
        self.assertAlmostEqual(N.eval_keygroup(g, 0.5)[0], 0.4375, places=6)
        self.assertAlmostEqual(N.eval_keygroup(g, 1.5)[0], 1.5625, places=6)

    def test_sample_times_are_sorted_unique_and_cover_range(self) -> None:
        ts = N.sample_times(0.0, 1.0, [0.5, 0.5, 2.0], True)
        self.assertEqual(ts, sorted(set(ts)))
        self.assertEqual((ts[0], ts[-1]), (0.0, 1.0))
        self.assertIn(0.5, ts)
        self.assertNotIn(2.0, ts)


@unittest.skipIf(N is None, "pyffi not installed")
class MathTests(unittest.TestCase):
    def test_quat_from_identity_and_z90(self) -> None:
        self.assertEqual([round(c, 6) for c in N.quat_from_mat3([[1, 0, 0], [0, 1, 0], [0, 0, 1]])], [0, 0, 0, 1])
        q = N.quat_from_mat3([[0, -1, 0], [1, 0, 0], [0, 0, 1]])  # +90 deg about Z
        self.assertAlmostEqual(q[2], math.sqrt(0.5), places=6)
        self.assertAlmostEqual(q[3], math.sqrt(0.5), places=6)

    def test_euler_order_x_then_y_then_z(self) -> None:
        q = N.euler_xyz_to_quat(math.pi / 2, 0.0, math.pi / 2)
        expect = N.qmul([0, 0, math.sqrt(0.5), math.sqrt(0.5)], [math.sqrt(0.5), 0, 0, math.sqrt(0.5)])
        for a, b in zip(q, expect):
            self.assertAlmostEqual(a, b)

    def test_inverse(self) -> None:
        rot = NS(m_11=0.0, m_12=1.0, m_13=0.0, m_21=-1.0, m_22=0.0, m_23=0.0, m_31=0.0, m_32=0.0, m_33=1.0)
        m = N.xform(rot, NS(x=1.0, y=2.0, z=3.0), 2.0)
        ident = N.matmul(m, N.inverse(m))
        for i in range(4):
            for j in range(4):
                self.assertAlmostEqual(ident[i][j], 1.0 if i == j else 0.0)

    def test_quat_sign_continuity(self) -> None:
        qs = N.fix_quat_signs([[0, 0, 0, 1], [0, 0, 0, -1]])
        self.assertEqual(qs[1], [0, 0, 0, 1])


@unittest.skipIf(N is None, "pyffi not installed")
class GlbWriterTests(unittest.TestCase):
    def test_glb_layout(self) -> None:
        g = N.Gltf()
        acc = g.accessor([(0.0, 0.0, 0.0), (1.0, 2.0, 3.0)], N.FLOAT, "VEC3", N.ARRAY_BUFFER)
        mesh = g.add("meshes", {"primitives": [{"attributes": {"POSITION": acc}}]})
        n = g.add("nodes", {"name": "a", "mesh": mesh})
        glb = g.to_glb([n])
        magic, version, total = struct.unpack_from("<III", glb, 0)
        self.assertEqual((magic, version, total), (0x46546C67, 2, len(glb)))
        jlen, jtype = struct.unpack_from("<II", glb, 12)
        self.assertEqual(jtype, 0x4E4F534A)
        self.assertEqual(jlen % 4, 0)
        j = json.loads(glb[20 : 20 + jlen])
        self.assertEqual(j["accessors"][0]["max"], [1.0, 2.0, 3.0])
        self.assertEqual(j["nodes"][-1]["name"], "ZupToYup")
        self.assertNotIn("skins", j)  # empty arrays are omitted
        blen, btype = struct.unpack_from("<II", glb, 20 + jlen)
        self.assertEqual((btype, blen), (0x004E4942, j["buffers"][0]["byteLength"]))


class ImportHelperTests(unittest.TestCase):
    def setUp(self) -> None:
        textures = {"units/per/t/base.dds": "Units/PER/T/Base.dds",
                    "units/gbr/t/shared.dds": "Units/GBR/T/Shared.dds",
                    "units/per/other/shared.dds": "Units/PER/Other/Shared.dds",
                    "textures/x.tga": "Textures/X.tga"}
        by_name: dict[str, list[str]] = {}
        for k in textures:
            by_name.setdefault(k.rsplit("/", 1)[1], []).append(k)
        IG._W.update(textures=textures, by_name=by_name)

    def test_exact_relative_to_nif(self) -> None:
        self.assertEqual(IG.resolve_texture("Units/PER/T/T.nif", "base.DDS"), ("Units/PER/T/Base.dds", "exact"))

    def test_root_relative(self) -> None:
        self.assertEqual(IG.resolve_texture("Units/PER/T/T.nif", "/Textures/X.tga")[0], "Textures/X.tga")

    def test_by_name_prefers_nearest_folder(self) -> None:
        self.assertEqual(IG.resolve_texture("Units/PER/T/T.nif", "Shared.dds"), ("Units/PER/Other/Shared.dds", "by-name"))

    def test_extension_fallback_and_missing(self) -> None:
        self.assertEqual(IG.resolve_texture("A/b.nif", "x.bmp")[0], "Textures/X.tga")
        self.assertEqual(IG.resolve_texture("A/b.nif", "nope.dds"), (None, "missing"))

    def test_clip_compat(self) -> None:
        models = [{"out": "U/A/A.glb", "src": "U/A/A.nif", "node_names": ["Bip01", "Bip01 Pelvis", "Turret"]},
                  {"out": "U/B/B.glb", "src": "U/B/B.nif", "node_names": ["Turret"]}]
        clips = [{"out": "Shared/Animations/Units/I/Walk.anim.glb", "src": "Shared/Animations/Units/I/Walk.kf",
                  "targets": ["Bip01", "Bip01 Pelvis"]},
                 {"out": "U/A/ARecoil.anim.glb", "src": "U/A/ARecoil.kf", "targets": ["Turret"]},
                 {"out": "U/C/CRecoil.anim.glb", "src": "U/C/CRecoil.kf", "targets": ["Turret"]}]
        compat = IG.clip_compat(models, clips)
        self.assertEqual(compat["U/A/A.glb"], ["U/A/ARecoil.anim.glb", "Shared/Animations/Units/I/Walk.anim.glb"])
        self.assertNotIn("U/B/B.glb", compat)  # other folders' clips are not offered


if __name__ == "__main__":
    unittest.main()
