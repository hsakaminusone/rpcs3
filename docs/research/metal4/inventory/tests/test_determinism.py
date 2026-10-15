#!/usr/bin/env python3
"""
Determinism tests: same inputs -> same artifacts, every run.

The inventory is regenerated into two throwaway directories and compared
byte-for-byte; the checked-in artifacts are hashed twice; and the corpus build is
shown to be independent of CSV row order.  No GPU, network, clock or randomness.
"""
from __future__ import annotations

import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import _corpus


def _hash_tree(root: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            out[str(p.relative_to(root))] = hashlib.sha256(p.read_bytes()).hexdigest()
    return out


class TestDeterminism(unittest.TestCase):
    def test_generate_cli_twice_identical(self):
        a, b = Path(tempfile.mkdtemp()), Path(tempfile.mkdtemp())
        for dest in (a, b):
            subprocess.run(
                [sys.executable, str(_corpus.INVENTORY / "generate.py"),
                 "--root", str(_corpus.REPO), "--out", str(dest), "--quiet"],
                check=True)
        self.assertEqual(_hash_tree(a), _hash_tree(b))

    def test_checked_in_artifacts_hash_stable(self):
        first = _hash_tree(_corpus.OUT)
        second = _hash_tree(_corpus.OUT)
        self.assertEqual(first, second)
        self.assertIn("rsx_state_space.csv", first)
        self.assertIn("rsx_value_space.csv", first)

    def test_corpus_load_is_order_independent(self):
        rows = _corpus._read_csv(_corpus.OUT / "rsx_state_space.csv")
        forward = {r["id"]: r for r in rows}
        reverse = {r["id"]: r for r in reversed(rows)}
        self.assertEqual(forward, reverse)
        self.assertEqual(set(forward), set(_corpus._by_id()))

    def test_dial_counts_are_auditable(self):
        counts = _corpus.dial_counts()
        self.assertEqual(counts["registers"], len(_corpus.registers()))
        self.assertEqual(counts["values"], len(_corpus.values()))
        self.assertEqual(counts["facets"], len(_corpus.facets()))
        self.assertEqual(counts["slots"], len(_corpus.slots()))
        self.assertEqual(counts["base_cases"],
                         counts["registers"] + counts["values"]
                         + counts["facets"] + counts["slots"])
        self.assertEqual(counts["permutations"],
                         sum(_corpus.space_size(s) for s in _corpus.PERM_SPACES))


if __name__ == "__main__":
    unittest.main()
