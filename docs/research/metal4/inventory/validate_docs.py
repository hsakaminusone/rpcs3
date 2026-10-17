#!/usr/bin/env python3
"""
Deterministic validation of the curated design layer against primary sources.

The design documents make claims that are, in principle, mechanically checkable.
This tool checks them without network access, so that a fabricated symbol or a
mis-attributed citation cannot propagate silently through the fan-out:

  * citations  -- every `file:line` resolves to a real file and an in-range line
  * registers  -- every NV*/GCM_* method name exists in the generated spine
  * metal-api  -- every MTL*/MTL4* type symbol exists in the metal-cpp headers
  * doc/shard  -- each LLD's id table covers exactly its cluster's shard ids

metal-cpp is the local oracle for the Metal API surface.  It is not the SDK, so
a symbol absent from it is reported as *unverified*, never as certainly wrong;
the allowlists record the known exceptions (RPCS3-owned MTL* names, local
templates, macros, the namespace tokens themselves).

Usage:
    python3 validate_docs.py [--strict] [--metalcpp PATH] [--refs A,B,C]
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
METAL4 = HERE.parent
DESIGN = METAL4 / "design"
OUT = HERE / "out"
REPO = HERE.parents[3]

DEFAULT_METALCPP = Path(os.environ.get(
    "METALCPP_DIR", "/tmp/metal4-refs/metal-cpp"))
DEFAULT_REFS = os.environ.get(
    "METAL4_REFS",
    "/tmp/metal4-refs/MoltenVK,/tmp/metal4-refs/mesa,"
    "/tmp/metal4-refs/godot,/tmp/shadPS4")

# Tokens that match the MTL* shape but are not metal-cpp API types.
METAL_ALLOW = {
    "MTL", "MTL4",             # namespaces
    "MTL_DEBUG_LAYER",         # env var, not an API symbol
    "MTLGSRender",             # RPCS3's own render class
    "MTLTraits",               # RPCS3 template parameter (design-local)
    "MTLDraw",                 # internal shorthand, not an API type
    "MTLFX", "MTL4FX",         # MetalFX namespaces (tokens themselves)
}
# Registers the docs deliberately name as *absent* from RPCS3.
REG_ALLOW = {
    "NV4097_SET_BACK_END_WRITE_ENABLE",
    "NV4097_SET_MATERIAL_",
    "NV4097_SET_TEXGEN_",
}

CITE_RE = re.compile(
    r"(?<![\w/])([A-Za-z0-9_][A-Za-z0-9_./-]*\.(?:hpp|h|mm|cpp|py)):(\d+)(?:-(\d+))?")
FILE_RE = re.compile(
    r"[A-Za-z0-9_][A-Za-z0-9_./-]*\.(?:hpp|h|mm|cpp|py|rs|html)")
REG_RE = re.compile(r"\b(NV[0-9A-Fa-f]{4}_[A-Z0-9_]+|GCM_[A-Z0-9_]+)\b")
METAL_RE = re.compile(r"\b(MTL4?[A-Z][A-Za-z0-9_]*)\b")
ID_CELL_RE = re.compile(r"^M_[A-Z0-9_]+$")
NEG_RE = re.compile(
    r"\b(?:not|no|never|without|absent|nonexistent|isn't|aren't|"
    r"doesn't|don't|unbound|unused|inert)\b", re.I)

CITE_EXTS = {".h", ".hpp", ".cpp", ".mm", ".py"}


def index_files(root: Path) -> dict[str, list[Path]]:
    idx: dict[str, list[Path]] = {}
    if not root.exists():
        return idx
    for p in root.rglob("*"):
        if p.is_file() and p.suffix in CITE_EXTS:
            idx.setdefault(p.name, []).append(p)
    return idx


def metal_symbols(root: Path) -> set[str]:
    """Flattened metal-cpp names: namespace prefix + declared type / enum case."""
    known: set[str] = set()
    if not root.exists():
        return known
    for p in root.rglob("*.hpp"):
        text = p.read_text(errors="replace")
        nss = set(re.findall(
            r"namespace\s+(MTL4FX|MTLFX|MTL4|MTL|NS|CA)\b", text))
        if not nss:
            continue
        types: set[str] = set()
        types |= set(re.findall(
            r"\b(?:class|struct|protocol)\s+([A-Za-z_][A-Za-z0-9_]*)", text))
        types |= set(re.findall(r"\busing\s+([A-Za-z_][A-Za-z0-9_]*)\s*=", text))
        for m in re.finditer(
                r"_(?:MTL_ENUM|MTL_OPTIONS)\(\s*[^,]+,\s*"
                r"([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*\{(.*?)\}", text, re.S):
            types.add(m.group(1))
            types |= set(re.findall(r"^\s*([A-Za-z_][A-Za-z0-9_]*)",
                                    m.group(2), re.M))
        # constants: `_MTL_CONST(Type, Name)` / `_MTL_PRIVATE_DEF_*(...)` are
        # declared *without* the MTL prefix, so namespace-prefix them
        types |= set(re.findall(
            r"_(?:MTL_CONST|MTL_PRIVATE_DEF_(?:STR|CONST|CLS|SEL))"
            r"\(\s*[^,]+,\s*([A-Za-z_][A-Za-z0-9_]*)", text))
        for ns in nss:
            for n in types:
                known.add(ns + n)
        # C functions carry their own MTL prefix already
        known |= set(re.findall(
            r"(?:MTL_EXPORT|extern\s+\"C\")\s+[^;()]*?"
            r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", text))
    return known


def docs() -> list[Path]:
    found = sorted(DESIGN.glob("*.md")) if DESIGN.exists() else []
    found += sorted(METAL4.glob("*.md"))
    return found


def load_shards() -> dict[str, set[str]]:
    shards: dict[str, set[str]] = {}
    ddir = OUT / "dispositions"
    if ddir.exists():
        for f in sorted(ddir.glob("*.csv")):
            with f.open(newline="") as fh:
                shards[f.stem] = {r["id"] for r in csv.DictReader(fh)}
    return shards


def doc_table_ids(path: Path) -> set[str]:
    ids: set[str] = set()
    for ln in path.read_text(errors="replace").splitlines():
        if not ln.lstrip().startswith("|"):
            continue
        first = ln.strip().strip("|").split("|")[0].strip().strip("` ")
        if ID_CELL_RE.match(first):
            ids.add(first)
    return ids


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--metalcpp", type=Path, default=DEFAULT_METALCPP)
    ap.add_argument("--refs", default=DEFAULT_REFS,
                    help="comma-separated external reference roots")
    args = ap.parse_args(argv)

    idx = index_files(REPO)
    for extra in [args.metalcpp, *[Path(r) for r in args.refs.split(",") if r]]:
        for k, v in index_files(extra).items():
            idx.setdefault(k, []).extend(v)
    metal = metal_symbols(args.metalcpp) | METAL_ALLOW
    have_metal = args.metalcpp.exists()
    have_refs = any(Path(r).exists() for r in args.refs.split(",") if r)

    spine_csv = OUT / "rsx_state_space.csv"
    spine: set[str] = set()
    if spine_csv.exists():
        with spine_csv.open(newline="") as fh:
            spine = {r["name"] for r in csv.DictReader(fh)}
    shards = load_shards()

    lines_cache: dict[Path, int] = {}

    def nlines(p: Path) -> int:
        if p not in lines_cache:
            lines_cache[p] = p.read_text(errors="replace").count("\n") + 1
        return lines_cache[p]

    errs: list[str] = []
    warns: dict[str, list[str]] = defaultdict(list)
    negated: dict[str, list[str]] = defaultdict(list)
    notes: list[str] = []
    n_cite = n_reg = n_metal = 0

    for md in docs():
        text = md.read_text(errors="replace")
        rel = md.relative_to(METAL4)
        lines = text.splitlines()
        # blank file tokens (same length, so match offsets stay valid) so that
        # `Foo.hpp` is not read as the symbol `Foo`
        body = FILE_RE.sub(lambda mo: " " * len(mo.group()), text)

        def record(tok: str, pos: int) -> None:
            ln = text.count("\n", 0, pos)
            ctx = " ".join(lines[max(0, ln - 2):ln + 3])
            (negated if NEG_RE.search(ctx) else warns)[tok].append(str(rel))

        for m in CITE_RE.finditer(text):
            fname, a, b = m.group(1), int(m.group(2)), m.group(3)
            n_cite += 1
            cands = idx.get(Path(fname).name)
            if not cands:
                msg = f"{rel}: citation {fname}:{a}: file not found"
                if have_refs:
                    errs.append(msg)
                else:
                    notes.append(msg + " (reference trees absent)")
                continue
            end = int(b) if b else a
            total = max(nlines(c) for c in cands)
            if a < 1 or end > total or end < a:
                errs.append(
                    f"{rel}: citation {fname}:{a}"
                    f"{'-' + b if b else ''}: out of range (file has {total})")

        for m in REG_RE.finditer(body):
            n_reg += 1
            tok = m.group(1)
            if tok.endswith("_"):          # wildcard, e.g. NV4097_SET_MATERIAL_*
                continue
            if tok not in spine and tok not in REG_ALLOW:
                record(tok, m.start())

        if have_metal:
            for m in METAL_RE.finditer(body):
                n_metal += 1
                tok = m.group(1)
                if tok not in metal:
                    record(tok, m.start())

        cname = md.stem[4:] if md.stem.startswith("LLD-") else None
        if cname and cname in shards:
            want, got = shards[cname], doc_table_ids(md)
            missing, extra = want - got, got - want
            if missing:
                errs.append(f"{rel}: table missing {len(missing)} shard ids "
                            f"(e.g. {sorted(missing)[:3]})")
            if extra:
                errs.append(f"{rel}: table has {len(extra)} ids not in shard "
                            f"(e.g. {sorted(extra)[:3]})")

    print(f"docs: {len(docs())}  citations: {n_cite}  registers: {n_reg}  "
          f"metal: {n_metal}  shards: {len(shards)}")
    print(f"errors: {len(errs)}  unverified: {len(warns)}  "
          f"named-as-absent: {len(negated)}  notes: {len(notes)}")
    for e in errs:
        print("error:", e, file=sys.stderr)
    for n in notes:
        print("note: ", n, file=sys.stderr)
    for tok in sorted(warns):
        where = sorted(set(warns[tok]))
        print(f"warn:  {tok}  ({len(warns[tok])}x: {', '.join(where)})",
              file=sys.stderr)
    for tok in sorted(negated):
        where = sorted(set(negated[tok]))
        print(f"note:  {tok} (named as absent)  ({', '.join(where)})")

    if errs:
        return 1
    if args.strict and warns:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
