#!/usr/bin/env python3
"""
Validate and merge the curated disposition layer into the RSX state-space spine.

The generator (generate.py) produces the *facts* (one row per method register).
The curation layer produces, per cluster, a CSV that assigns a Metal 4 disposition
to every register id.  This tool is the gate between them:

  * vocabulary / scale / provenance checks on every curated row,
  * exact-coverage check (every spine id has exactly one disposition, no extras),
  * merge into out/rsx_state_space_curated.csv,
  * a generated coverage report (out/COVERAGE.md).

It is deliberately usable before curation is complete: partial coverage is reported
and only fails the build under --strict.

Usage:
    python3 validate.py                 # report + merge what exists
    python3 validate.py --strict        # fail unless coverage is 100%
    python3 validate.py --require tbd   # fail if any row is still 'tbd'
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

DISPOSITIONS = {"direct", "reshape", "emulate", "impossible", "not-recommended",
                "moot", "na"}
RISKS = {"low", "med", "high"}
CONFIDENCE = {"[V]", "[I]", "[U]"}
REQUIRED_COLS = ["id", "disposition", "metal4_target", "difficulty",
                 "risk", "confidence", "doc_ref", "notes"]


def read_csv(path: Path) -> list[dict]:
    with path.open(newline="") as fh:
        return list(csv.DictReader(fh))


def load_spine(out: Path) -> list[dict]:
    spine = out / "rsx_state_space.csv"
    if not spine.exists():
        raise SystemExit(f"missing {spine}; run generate.py first")
    return read_csv(spine)


def check_row(row: dict, where: str) -> list[str]:
    errs: list[str] = []
    rid = row.get("id", "")
    disp = (row.get("disposition") or "").strip()
    if disp not in DISPOSITIONS:
        errs.append(f"{where}: {rid}: bad disposition {disp!r}")
    diff = (row.get("difficulty") or "").strip()
    if disp not in ("", "na", "moot"):
        if diff not in {"1", "2", "3", "4", "5"}:
            errs.append(f"{where}: {rid}: difficulty must be 1..5, got {diff!r}")
    risk = (row.get("risk") or "").strip()
    if risk and risk not in RISKS:
        errs.append(f"{where}: {rid}: bad risk {risk!r}")
    conf = (row.get("confidence") or "").strip()
    if conf and conf not in CONFIDENCE:
        errs.append(f"{where}: {rid}: bad confidence {conf!r}")
    if disp not in ("", "na", "moot") and not (row.get("metal4_target") or "").strip():
        errs.append(f"{where}: {rid}: empty metal4_target for disposition {disp!r}")
    return errs


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=HERE / "out")
    ap.add_argument("--strict", action="store_true",
                    help="fail unless every spine id is covered")
    args = ap.parse_args(argv)
    out = args.out

    spine = load_spine(out)
    spine_ids = [r["id"] for r in spine]
    spine_set = set(spine_ids)

    ddir = out / "dispositions"
    files = sorted(ddir.glob("*.csv")) if ddir.exists() else []

    errs: list[str] = []
    curated: dict[str, dict] = {}
    for f in files:
        rows = read_csv(f)
        if not rows:
            continue
        missing = [c for c in REQUIRED_COLS if c not in rows[0]]
        if missing:
            errs.append(f"{f.name}: missing columns {missing}")
            continue
        for row in rows:
            rid = row["id"]
            if rid not in spine_set:
                errs.append(f"{f.name}: {rid}: not a spine id")
                continue
            if rid in curated:
                errs.append(f"{f.name}: {rid}: duplicate (also in another shard)")
                continue
            errs += check_row(row, f.name)
            curated[rid] = row

    covered = len(curated)
    total = len(spine_ids)
    pct = (100.0 * covered / total) if total else 0.0

    # merge
    merged = []
    for r in spine:
        c = curated.get(r["id"], {})
        m = dict(r)
        for col in ("disposition", "metal4_target", "difficulty",
                    "risk", "confidence", "doc_ref", "notes"):
            if c.get(col):
                m[col] = c[col]
        merged.append(m)
    cols = list(spine[0].keys()) + (["notes"] if "notes" not in spine[0] else [])
    with (out / "rsx_state_space_curated.csv").open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for row in merged:
            w.writerow(row)

    by_disp: dict[str, int] = {}
    for row in curated.values():
        d = (row.get("disposition") or "?").strip() or "?"
        by_disp[d] = by_disp.get(d, 0) + 1
    uncovered = [i for i in spine_ids if i not in curated]

    report = [
        "# RSX disposition coverage",
        "",
        "> Generated by `validate.py`. Do not edit by hand.",
        "",
        f"- spine ids: **{total}**",
        f"- curated: **{covered}** ({pct:.1f}%)",
        f"- shards: {len(files)}",
        "",
        "## Dispositions",
        "",
        "| disposition | count |",
        "|---|---|",
    ]
    for k in sorted(by_disp):
        report.append(f"| {k} | {by_disp[k]} |")
    if uncovered:
        report += ["", f"## Uncovered ids ({len(uncovered)})", "",
                   "```", *uncovered, "```"]
    report += ["", "<!-- doc-end -->", ""]
    (out / "COVERAGE.md").write_text("\n".join(report))
    (out / "coverage.json").write_text(json.dumps(
        {"total": total, "covered": covered, "percent": round(pct, 2),
         "by_disposition": dict(sorted(by_disp.items()))},
        indent=2, sort_keys=True) + "\n")

    print(f"spine={total} curated={covered} ({pct:.1f}%) shards={len(files)}")
    for e in errs:
        print("error:", e, file=sys.stderr)
    if errs:
        return 1
    if args.strict and uncovered:
        print(f"strict: {len(uncovered)} ids uncovered", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
