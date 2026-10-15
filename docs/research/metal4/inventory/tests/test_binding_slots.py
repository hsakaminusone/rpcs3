#!/usr/bin/env python3
"""
Binding-slot contract tests: the argument-table layout.

One case per slot (`S_*` dial, 12 at this revision) plus the layout invariants.
Mirrors `design/HLD-04` §3.3 / `design/HLD-05` §3.2 and the binding extraction
from `VK/vkutils/pipeline_binding_table.h`.
"""
from __future__ import annotations

import unittest

import _corpus


class TestBindingSlots(_corpus.DialTest):
    def test_slot_contract(self):
        for slot in _corpus.slots():
            self.assertContract(slot.name, _corpus.slot_violations(slot),
                                _corpus.SLOT_DEVIATIONS.get(slot.name, ()))

    def test_indices_contiguous_modulo_reserved_gaps(self):
        idx = sorted({s.slot for s in _corpus.slots()})
        self.assertEqual(idx, [i for i in range(max(idx) + 1)
                               if i not in _corpus.SLOT_GAPS])

    def test_declared_kinds_are_valid(self):
        for slot in _corpus.slots():
            with self.subTest(slot=slot.name):
                self.assertIn(_corpus.SLOT_KINDS[slot.name], _corpus.BIND_KINDS)

    def test_count_matches_stats(self):
        self.assertEqual(len(_corpus.slots()), _corpus.stats()["binding_slots"])


if __name__ == "__main__":
    unittest.main()
