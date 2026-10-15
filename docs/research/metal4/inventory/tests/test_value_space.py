#!/usr/bin/env python3
"""
Value-space contract tests: every constant a register can carry.

One case per constant (`V_`-family dial, 768 at this revision) plus one case per
enum domain (10).  Mirrors `design/LLD-value-space.md` (the exhaustive
format/blend/compare table) and the VALUE space of `inventory/SCHEMA.md`.
"""
from __future__ import annotations

import unittest
from collections import Counter

import _corpus


class TestValueSpace(_corpus.DialTest):
    def test_value_contract(self):
        for val in _corpus.values():
            self.assertContract(val.name, _corpus.value_violations(val),
                                _corpus.VALUE_DEVIATIONS.get(val.name, ()))

    def test_enum_contract(self):
        for enum in sorted(_corpus.VALUE_DOMAINS):
            self.assertContract(enum or "(anonymous)", _corpus.enum_violations(enum),
                                _corpus.ENUM_DEVIATIONS.get(enum, ()))

    def test_anonymous_bucket_is_a_lump(self):
        anon = [v for v in _corpus.values() if v.enum == _corpus.ANONYMOUS_ENUM]
        self.assertGreater(len(anon), 0)
        self.assertEqual(len({v.name for v in anon}), len(anon))
        self.assertLess(len({v.value for v in anon}), len(anon))

    def test_counts_match_stats(self):
        self.assertEqual(len(_corpus.values()), _corpus.stats()["value_constants"])
        self.assertEqual(len({v.enum for v in _corpus.values()}),
                         _corpus.stats()["value_enums"])

    def test_enum_set_matches_domains(self):
        self.assertEqual({v.enum for v in _corpus.values()},
                         set(_corpus.VALUE_DOMAINS))

    def test_dense_enums_are_fully_enumerated(self):
        for enum, dom in _corpus.VALUE_DOMAINS.items():
            if not dom.dense:
                continue
            with self.subTest(enum=enum):
                vals = sorted(v.value for v in _corpus.values() if v.enum == enum)
                self.assertEqual(vals, list(range(dom.lo, dom.hi + 1)))

    def test_sources_are_the_method_enum_header(self):
        sources = Counter(v.source for v in _corpus.values())
        self.assertEqual(set(sources), {_corpus.VALUE_SOURCE})


if __name__ == "__main__":
    unittest.main()
