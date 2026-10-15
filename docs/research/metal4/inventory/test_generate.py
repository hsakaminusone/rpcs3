#!/usr/bin/env python3
"""
Conformance tests for the RSX state-space extractor (generate.py).

These are *live* tests: they parse the real RPCS3 source tree and assert that the
extraction still sees the things the mapping depends on.  They are deliberately
tolerant of growth (counts are lower bounds) but strict about identity (specific
registers, addresses, handlers, slots), so upstream refactors that break the spine
fail loudly here rather than silently in a generated document.

Run:
    python3 -m unittest -v test_generate
or
    python3 test_generate.py
"""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _load():
    spec = importlib.util.spec_from_file_location("generate", HERE / "generate.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["generate"] = mod
    spec.loader.exec_module(mod)
    return mod


g = _load()


class TestEval(unittest.TestCase):
    def test_hex(self):
        self.assertEqual(g._eval("0x208"), 0x208)

    def test_shift(self):
        self.assertEqual(g._eval("0x00000208 >> 2"), 0x82)
        self.assertEqual(g._eval("0x2100 >> 2"), 0x840)

    def test_decimal(self):
        self.assertEqual(g._eval("12"), 12)

    def test_symbolic_is_none(self):
        self.assertIsNone(g._eval("SOME_OTHER_CONST"))
        self.assertIsNone(g._eval("1 << 4"))


class TestModel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = g.build(g.DEFAULT_ROOT)
        cls.regs = cls.model["registers"]
        cls.by = {r.name: r for r in cls.regs}
        cls.fields = g.parse_decoders(
            g.rsx_dir(g.DEFAULT_ROOT) / "rsx_decode.h")[1]

    # ---- command space ------------------------------------------------- #

    def test_method_count(self):
        self.assertGreaterEqual(len(self.regs), 250)

    def test_no_duplicate_names(self):
        names = [r.name for r in self.regs]
        self.assertEqual(len(names), len(set(names)))

    def test_known_addresses(self):
        # (name, byte address) -- these are load-bearing and must never drift.
        expected = {
            "NV4097_SET_SURFACE_FORMAT": 0x208,
            "NV4097_SET_SURFACE_CLIP_HORIZONTAL": 0x200,
            "NV4097_SET_BEGIN_END": 0x1808,
            "NV4097_DRAW_ARRAYS": 0x1814,
            "NV4097_INLINE_ARRAY": 0x1818,
            "NV4097_DRAW_INDEX_ARRAY": 0x1824,
            "NV4097_CLEAR_SURFACE": 0x1d94,
            "NV4097_SET_COLOR_CLEAR_VALUE": 0x1d90,
            "NV4097_SET_ZSTENCIL_CLEAR_VALUE": 0x1d8c,
            "NV4097_SET_SHADER_PROGRAM": 0x8e4,
            "NV4097_SET_VERTEX_DATA_ARRAY_FORMAT": 0x1740,
            "NV4097_SET_VIEWPORT_HORIZONTAL": 0xa00,
            "NV4097_SET_SCISSOR_HORIZONTAL": 0x8c0,
        }
        for name, addr in expected.items():
            self.assertIn(name, self.by, name)
            self.assertEqual(self.by[name].addr, addr, name)

    def test_addresses_are_word_multiples(self):
        # Every method address is 4-byte aligned (the enum stores word indices).
        for r in self.regs:
            self.assertEqual(r.addr % 4, 0, r.name)

    def test_msb_first_family_present(self):
        for prefix in ("NV4097_", "NV406E_", "NV0039_", "NV308A_", "NV3089_"):
            self.assertTrue(any(r.name.startswith(prefix) for r in self.regs),
                            f"no registers for {prefix}")

    # ---- decode space -------------------------------------------------- #

    def test_decoder_coverage(self):
        have = sum(1 for r in self.regs if r.has_decoder)
        self.assertGreaterEqual(have, 150)

    def test_surface_format_fields(self):
        r = self.by["NV4097_SET_SURFACE_FORMAT"]
        self.assertTrue(r.has_decoder)
        names = {f.method for f in r.decoder_fields}
        for want in ("color_fmt_raw", "depth_fmt_raw", "type_raw", "antialias_raw",
                     "log2width", "log2height"):
            self.assertIn(want, names, want)

    def test_viewport_fields(self):
        r = self.by["NV4097_SET_VIEWPORT_HORIZONTAL"]
        widths = sorted((f.offset, f.width) for f in r.decoder_fields)
        self.assertIn((0, 16), widths)
        self.assertIn((16, 16), widths)

    def test_bitfields_within_word(self):
        for r in self.regs:
            for f in r.decoder_fields:
                self.assertGreater(f.width, 0, f)
                self.assertLessEqual(f.offset + f.width, 32, (r.name, f))

    # ---- dispatch ------------------------------------------------------ #

    def test_known_handlers(self):
        expected = {
            "NV4097_SET_BEGIN_END": "nv4097::set_begin_end",
            "NV4097_CLEAR_SURFACE": "nv4097::clear",
            "NV4097_DRAW_ARRAYS": "nv4097::draw_arrays",
            "NV4097_DRAW_INDEX_ARRAY": "nv4097::draw_index_array",
            "NV4097_INLINE_ARRAY": "nv4097::draw_inline_array",
            "NV4097_SET_SURFACE_FORMAT": "nv4097::set_surface_format",
            "NV4097_SET_COLOR_MASK": "nv4097::set_color_mask",
            "NV4097_SET_BLEND_EQUATION": "nv4097::set_blend_equation",
        }
        for name, handler in expected.items():
            self.assertIn(name, self.by, name)
            b = self.by[name].binding
            self.assertIsNotNone(b, name)
            self.assertEqual(b.handler, handler, name)

    def test_binding_kinds(self):
        kinds = {}
        for r in self.regs:
            if r.binding:
                kinds[r.binding.kind] = kinds.get(r.binding.kind, 0) + 1
        self.assertGreaterEqual(kinds.get("bind", 0), 50)
        self.assertGreaterEqual(kinds.get("bind_range", 0), 15)
        self.assertGreaterEqual(kinds.get("bind_array", 0), 5)

    def test_texture_range(self):
        # The 16 fragment textures are bound as a strided range.
        b = self.by["NV4097_SET_TEXTURE_OFFSET"].binding
        self.assertIsNotNone(b)
        self.assertEqual(b.kind, "bind_range")
        self.assertEqual(b.count, 16)
        self.assertEqual(b.step, 8)

    # ---- binding table ------------------------------------------------- #

    def test_binding_slots(self):
        slots = {s["name"]: s["slot"] for s in self.model["slots"]}
        self.assertEqual(slots.get("vertex_params_bind_slot"), 0)
        self.assertEqual(slots.get("fragment_constant_buffers_bind_slot"), 2)
        self.assertEqual(slots.get("textures_first_bind_slot"), 12)
        self.assertGreaterEqual(len(slots), 10)

    # ---- value space --------------------------------------------------- #

    def test_value_space_size(self):
        self.assertGreaterEqual(len(self.model["values"]), 400)

    def test_known_value_constants(self):
        names = {v["name"] for v in self.model["values"]}
        for want in ("CELL_GCM_TEXTURE_A8R8G8B8", "CELL_GCM_TEXTURE_DEPTH24_D8",
                     "CELL_GCM_TEXTURE_COMPRESSED_DXT1", "CELL_GCM_FUNC_ADD"):
            self.assertIn(want, names, want)

    # ---- structural invariants ---------------------------------------- #

    def test_ids_unique_and_stable(self):
        ids = [r.id for r in self.regs]
        self.assertEqual(len(ids), len(set(ids)))
        for r in self.regs:
            self.assertTrue(r.id.startswith("M_"))
            self.assertEqual(r.id, "M_" + r.name)

    def test_checks_pass(self):
        self.assertEqual(g.check(self.model), [])

    def test_nv47_modules(self):
        mods = {m["module"]: m for m in self.model["nv47"]}
        self.assertIn("nv4097", mods)
        self.assertIn("nv3089", mods)
        self.assertGreaterEqual(mods["nv4097"]["n_functions"], 5)

    def test_clustering_total(self):
        # Every register lands in exactly one cluster.
        c = {}
        for r in self.regs:
            c[r.cluster] = c.get(r.cluster, 0) + 1
        self.assertEqual(sum(c.values()), len(self.regs))
        for want in ("draw", "vertex", "texture", "surface", "depth-rop",
                     "blend", "raster", "sync-flip", "nv0039-dma"):
            self.assertIn(want, c, want)
            self.assertGreater(c[want], 0, want)

    def test_draw_cluster_has_core_ops(self):
        draw = {r.name for r in self.regs if r.cluster == "draw"}
        for want in ("NV4097_DRAW_ARRAYS", "NV4097_DRAW_INDEX_ARRAY",
                     "NV4097_INLINE_ARRAY", "NV4097_SET_BEGIN_END"):
            self.assertIn(want, draw, want)


class TestShaderFacets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.facets = g.build(g.DEFAULT_ROOT)["shader_facets"]
        cls.by = {f["id"]: f for f in cls.facets}

    def test_count(self):
        self.assertGreaterEqual(len(self.facets), 200)

    def test_kinds(self):
        kinds = {}
        for f in self.facets:
            kinds[f["kind"]] = kinds.get(f["kind"], 0) + 1
        for k in ("enum", "opcode", "bitfield", "field"):
            self.assertGreater(kinds.get(k, 0), 0, k)

    def test_known_vp_opcodes(self):
        for i in ("SH_V_VEC_MOV", "SH_V_VEC_TXL", "SH_V_VEC_DP4",
                  "SH_V_SCA_RCP", "SH_V_SCA_LIT", "SH_V_SCA_BRB"):
            self.assertIn(i, self.by, i)

    def test_known_fp_opcodes(self):
        for i in ("SH_F_OP_KIL", "SH_F_OP_TEX", "SH_F_OP_TXL",
                  "SH_F_OP_LOOP", "SH_F_OP_RET"):
            self.assertIn(i, self.by, i)

    def test_register_types_and_precision(self):
        for i in ("SH_V_REG_TEMP", "SH_V_REG_INPUT", "SH_V_REG_CONSTANT",
                  "SH_F_REG_TEMP", "SH_F_PREC_HALF", "SH_F_PREC_SATURATE"):
            self.assertIn(i, self.by, i)

    def test_instruction_bitfields(self):
        self.assertEqual(self.by["SH_V_BF_D1.vec_opcode"]["value"], "bit 22:5")
        self.assertEqual(self.by["SH_V_BF_D1.sca_opcode"]["value"], "bit 27:5")
        self.assertEqual(self.by["SH_F_BF_OPDEST.opcode"]["value"], "bit 24:6")
        self.assertEqual(self.by["SH_F_BF_SRC0.abs"]["value"], "bit 29:1")

    def test_program_fields(self):
        for i in ("SH_V_FIELD_ctrl", "SH_V_FIELD_output_mask",
                  "SH_F_FIELD_texcoord_control_mask",
                  "SH_F_FIELD_mrt_buffers_count"):
            self.assertIn(i, self.by, i)


class TestDeterminism(unittest.TestCase):
    def test_build_twice_identical(self):
        a = g.build(g.DEFAULT_ROOT)
        b = g.build(g.DEFAULT_ROOT)
        ja = json.dumps(g.emit(a, Path(tempfile.mkdtemp())), sort_keys=True)
        jb = json.dumps(g.emit(b, Path(tempfile.mkdtemp())), sort_keys=True)
        self.assertEqual(ja, jb)

    def test_emit_csv_row_counts(self):
        model = g.build(g.DEFAULT_ROOT)
        out = Path(tempfile.mkdtemp())
        stats = g.emit(model, out)
        with (out / "rsx_state_space.csv").open() as fh:
            n = sum(1 for _ in fh) - 1
        self.assertEqual(n, stats["registers"])
        with (out / "rsx_decode_fields.csv").open() as fh:
            nf = sum(1 for _ in fh) - 1
        self.assertEqual(nf, stats["decoder_fields"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
