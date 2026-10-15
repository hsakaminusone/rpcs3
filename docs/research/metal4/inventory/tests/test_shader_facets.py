#!/usr/bin/env python3
"""
Shader-facet contract tests: the RSX vertex/fragment instruction encoding.

One case per facet (`SH_V_*`/`SH_F_*` dial, 275 at this revision): opcodes,
instruction bitfields, register-type/precision enums and program fields.  Mirrors
the SHADER space of `inventory/SCHEMA.md` and the shader work in `design/HLD-05`.
"""
from __future__ import annotations

import unittest
from collections import Counter

import _corpus


class TestShaderFacets(_corpus.DialTest):
    def test_facet_contract(self):
        for facet in _corpus.facets():
            self.assertContract(facet.id, _corpus.facet_violations(facet),
                                _corpus.FACET_DEVIATIONS.get(facet.id, ()))

    def test_kind_counts(self):
        counts = Counter(f.kind for f in _corpus.facets())
        self.assertEqual(counts["opcode"], 70)
        self.assertEqual(counts["bitfield"], 129)
        self.assertEqual(counts["enum"], 58)
        self.assertEqual(counts["field"], 18)

    def test_source_files_resolve(self):
        for source in sorted({f.source for f in _corpus.facets()}):
            with self.subTest(source=source):
                self.assertIsNone(_corpus.resolve_source(source))

    def test_ids_unique(self):
        ids = [f.id for f in _corpus.facets()]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(len(ids), _corpus.stats()["shader_facets"])


if __name__ == "__main__":
    unittest.main()
