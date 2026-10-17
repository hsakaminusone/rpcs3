#!/usr/bin/env python3
"""Contract tests for the `texture` cluster shard (mirrors `design/LLD-texture.md`)."""
from __future__ import annotations

import unittest

import _corpus


CLUSTER = "texture"


class TestClusterTexture(_corpus.DialTest):
    def test_register_contract(self):
        for reg in _corpus.registers_by_cluster(CLUSTER):
            self.assertContract(reg.id, _corpus.register_violations(reg, CLUSTER),
                                _corpus.REGISTER_DEVIATIONS.get(reg.id, ()))

    def test_shard_matches_spine(self):
        shard = _corpus.cluster_shard_ids(CLUSTER)
        spine = {r.id for r in _corpus.registers_by_cluster(CLUSTER)}
        self.assertEqual(shard, spine)
        self.assertEqual(len(shard), _corpus.clusters()[CLUSTER])

    def test_lld_doc_covers_shard(self):
        doc = _corpus.DESIGN / f"LLD-{CLUSTER}.md"
        self.assertTrue(doc.exists(), doc)
        self.assertEqual(_corpus.doc_table_ids(doc),
                         _corpus.cluster_shard_ids(CLUSTER))


if __name__ == "__main__":
    unittest.main()
