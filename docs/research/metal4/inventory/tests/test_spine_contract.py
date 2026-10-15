#!/usr/bin/env python3
"""
Spine-level contract tests: global invariants across the whole inventory.

These are the invariants that no single cluster owns: id uniqueness, the shard
partition, artifact/stat agreement, decode-field shape and disposition coverage.
The per-dial contracts live in the cluster / value / facet / slot modules.
"""
from __future__ import annotations

import unittest

import _corpus


class TestSpineContract(unittest.TestCase):
    def test_register_ids_unique(self):
        ids = [r.id for r in _corpus.registers()]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertTrue(all(r.id == "M_" + r.name for r in _corpus.registers()))

    def test_shards_partition_the_spine(self):
        spine = {r.id for r in _corpus.registers()}
        seen: dict[str, str] = {}
        for cluster in _corpus.clusters():
            for rid in _corpus.cluster_shard_ids(cluster):
                self.assertNotIn(rid, seen, f"{rid} in {seen.get(rid)} and {cluster}")
                seen[rid] = cluster
        self.assertEqual(set(seen), spine)

    def test_cluster_counts_match_artifacts(self):
        for cluster, count in _corpus.clusters().items():
            with self.subTest(cluster=cluster):
                self.assertEqual(len(_corpus.registers_by_cluster(cluster)), count)

    def test_counts_match_stats(self):
        stats = _corpus.stats()
        self.assertEqual(len(_corpus.registers()), stats["registers"])
        self.assertEqual(len(_corpus.values()), stats["value_constants"])
        self.assertEqual(len(_corpus.facets()), stats["shader_facets"])
        self.assertEqual(len(_corpus.slots()), stats["binding_slots"])
        self.assertEqual(len(_corpus.clusters()), stats["clusters"])
        self.assertEqual(len(_corpus.decode_fields()), stats["decoder_fields"])

    def test_decode_fields_are_well_formed(self):
        ids = [f.id for f in _corpus.decode_fields()]
        self.assertEqual(len(ids), len(set(ids)))
        for f in _corpus.decode_fields():
            with self.subTest(field=f.id):
                self.assertGreater(f.bit_width, 0)
                self.assertLessEqual(f.bit_offset + f.bit_width, 32)
                self.assertGreaterEqual(f.bit_offset, 0)

    def test_every_register_is_curated(self):
        for reg in _corpus.registers():
            with self.subTest(register=reg.id):
                self.assertNotEqual(reg.disposition, "")

    def test_coverage_is_complete(self):
        cov = _corpus.coverage()
        self.assertEqual(cov["covered"], cov["total"])
        self.assertEqual(cov["percent"], 100.0)

    def test_deviations_are_exactly_the_pinned_set(self):
        observed = _corpus.deviations()
        self.assertEqual(observed["register"],
                         {k: list(v) for k, v in _corpus.REGISTER_DEVIATIONS.items()})
        self.assertEqual(observed["value"],
                         {k: list(v) for k, v in _corpus.VALUE_DEVIATIONS.items()})
        self.assertEqual(observed["enum"],
                         {k: list(v) for k, v in _corpus.ENUM_DEVIATIONS.items()})
        self.assertEqual(observed["facet"],
                         {k: list(v) for k, v in _corpus.FACET_DEVIATIONS.items()})
        self.assertEqual(observed["slot"],
                         {k: list(v) for k, v in _corpus.SLOT_DEVIATIONS.items()})

    def test_dial_counts_are_pinned(self):
        counts = _corpus.dial_counts()
        self.assertEqual(counts["registers"], 282)
        self.assertEqual(counts["values"], 768)
        self.assertEqual(counts["facets"], 275)
        self.assertEqual(counts["slots"], 12)
        self.assertEqual(counts["base_cases"], 1337)
        self.assertEqual(counts["permutations"], 9496)


if __name__ == "__main__":
    unittest.main()
