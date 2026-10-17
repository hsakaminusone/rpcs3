#!/usr/bin/env python3
"""
Lint the curated design-document layer.

Checks that every generated cluster has (a) a low-level design document and
(b) a disposition shard, that every design document is terminated by the
`<!-- doc-end -->` sentinel, and that markdown tables have a consistent column
count in each block.  Reports gaps; only fails (under --strict) on structural
errors so it is usable before the design fan-out has finished.

Usage:
    python3 lint_docs.py [--strict]
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DESIGN = HERE.parent / "design"
OUT = HERE / "out"

SENTINEL = "<!-- doc-end -->"


def check_doc_end(path: Path) -> str | None:
    text = path.read_text(errors="replace").rstrip()
    if not text.endswith(SENTINEL):
        return f"{path.name}: missing trailing {SENTINEL}"
    return None


def check_tables(path: Path) -> list[str]:
    """A markdown table block must have a uniform cell count across its rows."""
    errs: list[str] = []
    lines = path.read_text(errors="replace").splitlines()
    i = 0
    while i < len(lines):
        if lines[i].lstrip().startswith("|") and i + 1 < len(lines) \
                and re.match(r"^\s*\|[\s:|-]+\|\s*$", lines[i + 1]):
            def cells(ln: str) -> int:
                return len(ln.strip().strip("|").split("|"))
            want = cells(lines[i])
            j = i
            while j < len(lines) and lines[j].lstrip().startswith("|"):
                if cells(lines[j]) != want:
                    errs.append(f"{path.name}:{j+1}: table row has "
                                f"{cells(lines[j])} cells, header has {want}")
                j += 1
            i = j
        else:
            i += 1
    return errs


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args(argv)

    errs: list[str] = []
    gaps: list[str] = []

    if DESIGN.exists():
        for md in sorted(DESIGN.glob("*.md")):
            e = check_doc_end(md)
            if e:
                errs.append(e)
            errs += check_tables(md)

    clusters: dict[str, int] = {}
    cj = OUT / "clusters.json"
    if cj.exists():
        clusters = json.loads(cj.read_text())

    for cname, count in sorted(clusters.items()):
        doc = DESIGN / f"LLD-{cname}.md"
        disp = OUT / "dispositions" / f"{cname}.csv"
        if not doc.exists():
            gaps.append(f"missing design/LLD-{cname}.md ({count} registers)")
        if not disp.exists():
            gaps.append(f"missing out/dispositions/{cname}.csv ({count} registers)")
        elif doc.exists():
            with disp.open(newline="") as fh:
                rows = list(csv.DictReader(fh))
            if len(rows) != count:
                errs.append(f"{cname}.csv: {len(rows)} rows, expected {count}")

    ndoc = len(list(DESIGN.glob("*.md"))) if DESIGN.exists() else 0
    print(f"design docs: {ndoc}  clusters: {len(clusters)}  "
          f"gaps: {len(gaps)}  errors: {len(errs)}")
    for e in errs:
        print("error:", e, file=sys.stderr)
    for g in gaps:
        print("gap:  ", g, file=sys.stderr)

    if errs:
        return 1
    if args.strict and gaps:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
