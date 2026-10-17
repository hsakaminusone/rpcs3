#!/usr/bin/env python3
"""Tests for the disposition validator/merger (validate.py)."""
from __future__ import annotations

import csv
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


v = _load("validate")


def mk_spine(out: Path, ids: list[str]) -> None:
    out.mkdir(parents=True, exist_ok=True)
    cols = ["id", "name", "cluster", "addr", "has_decoder", "n_fields",
            "binding_kind", "handler", "disposition", "metal4_target",
            "difficulty", "risk", "confidence", "doc_ref"]
    with (out / "rsx_state_space.csv").open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        for i in ids:
            w.writerow({c: "" for c in cols} | {"id": i, "name": i[2:] if i.startswith("M_") else i})


def mk_disp(out: Path, name: str, rows: list[dict]) -> None:
    d = out / "dispositions"
    d.mkdir(parents=True, exist_ok=True)
    with (d / f"{name}.csv").open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=v.REQUIRED_COLS, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow({c: r.get(c, "") for c in v.REQUIRED_COLS})


GOOD = dict(id="M_NV4097_DRAW_ARRAYS", disposition="direct",
            metal4_target="drawPrimitives:vertexStart:vertexCount:",
            difficulty="2", risk="low", confidence="[V]", doc_ref="LLD-draw §3",
            notes="")


class TestCheckRow(unittest.TestCase):
    def test_good(self):
        self.assertEqual(v.check_row(dict(GOOD), "t"), [])

    def test_bad_disposition(self):
        r = dict(GOOD, disposition="whatever")
        self.assertTrue(v.check_row(r, "t"))

    def test_bad_difficulty(self):
        r = dict(GOOD, difficulty="9")
        self.assertTrue(v.check_row(r, "t"))

    def test_missing_target(self):
        r = dict(GOOD, metal4_target="")
        self.assertTrue(v.check_row(r, "t"))

    def test_na_needs_no_target(self):
        r = dict(GOOD, disposition="na", metal4_target="", difficulty="")
        self.assertEqual(v.check_row(r, "t"), [])

    def test_bad_confidence(self):
        r = dict(GOOD, confidence="yes")
        self.assertTrue(v.check_row(r, "t"))


class TestPipeline(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.ids = ["M_A", "M_B", "M_C"]
        mk_spine(self.tmp, self.ids)

    def test_full_coverage_merges(self):
        mk_disp(self.tmp, "c1", [
            dict(GOOD, id="M_A"), dict(GOOD, id="M_B"), dict(GOOD, id="M_C")])
        rc = v.main(["--out", str(self.tmp), "--strict"])
        self.assertEqual(rc, 0)
        rows = list(csv.DictReader((self.tmp / "rsx_state_space_curated.csv").open()))
        self.assertEqual([r["id"] for r in rows], self.ids)
        self.assertTrue(all(r["disposition"] == "direct" for r in rows))

    def test_partial_coverage_strict_fails(self):
        mk_disp(self.tmp, "c1", [dict(GOOD, id="M_A")])
        self.assertEqual(v.main(["--out", str(self.tmp)]), 0)
        self.assertEqual(v.main(["--out", str(self.tmp), "--strict"]), 1)

    def test_duplicate_across_shards_fails(self):
        mk_disp(self.tmp, "c1", [dict(GOOD, id="M_A")])
        mk_disp(self.tmp, "c2", [dict(GOOD, id="M_A")])
        self.assertEqual(v.main(["--out", str(self.tmp)]), 1)

    def test_unknown_id_fails(self):
        mk_disp(self.tmp, "c1", [dict(GOOD, id="M_NOPE")])
        self.assertEqual(v.main(["--out", str(self.tmp)]), 1)

    def test_no_dispositions_ok(self):
        self.assertEqual(v.main(["--out", str(self.tmp)]), 0)
        self.assertTrue((self.tmp / "COVERAGE.md").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
