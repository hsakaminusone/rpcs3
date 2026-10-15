#!/usr/bin/env python3
"""
Shared corpus, contract assertions and dial definitions for the RSX -> Metal 4 suite.

The suite is a *contract* layer over the mechanically-generated state-space
inventory: one case per dial (register / value / shader facet / binding slot),
each asserted against the SCHEMA vocabulary and the curated disposition layer.
This module loads the inventory exactly once, exposes typed dial records, and
turns each contract into a stable list of violation *codes* so a case can pin the
exact set of deviations it tolerates.

Violation codes
---------------
register: `id-dup`, `addr-inconsistent`, `cluster-mismatch`, `disposition`,
          `difficulty`, `risk`, `confidence`, `target`, `doc-ref`, `decode`
value:    `name-dup`, `value-range`, `group`, `source`, `prefix`, `block`,
          `block-dup`
enum:     `alias`, `gap`
facet:    `id-dup`, `name-dup`, `kind`, `source`
slot:     `range`, `name`, `kind`, `index-dup`, `gap`, `placeholder`
perm:     `member`, `no-target`, `impossible`, `undispositioned`, `no-fallback`

Every code that is expected on the current tree is listed in a `*_DEVIATIONS`
registry; a case asserts `violations == registry`, so a *new* deviation fails and
a *fixed* deviation also fails (the registry is never stale).
"""
from __future__ import annotations

import csv
import importlib.util
import itertools
import json
import re
import sys
import unittest
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

HERE = Path(__file__).resolve().parent
INVENTORY = HERE.parent
OUT = INVENTORY / "out"
METAL4 = INVENTORY.parent
DESIGN = METAL4 / "design"
REPO = INVENTORY.parents[3]

DISPOSITIONS = {"direct", "reshape", "emulate", "impossible", "not-recommended",
                "moot", "na"}
RISKS = {"low", "med", "high"}
CONFIDENCE = {"[V]", "[I]", "[U]"}
DIFFICULTIES = {"1", "2", "3", "4", "5"}
FACET_KINDS = {"opcode", "bitfield", "enum", "field"}
BIND_KINDS = {"setAddress", "setTexture", "setSamplerState"}
VALUE_SOURCE = "gcm_enums.h"


# --------------------------------------------------------------------------- #
# Dial records
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class Register:
    id: str
    name: str
    group: str
    cluster: str
    addr: str
    addr_dec: int
    word: int
    has_decoder: bool
    n_fields: int
    binding_kind: str
    handler: str
    source: str
    disposition: str
    metal4_target: str
    difficulty: str
    risk: str
    confidence: str
    doc_ref: str
    notes: str


@dataclass(frozen=True)
class Value:
    enum: str
    name: str
    value: int
    value_hex: str
    source: str
    block: str


@dataclass(frozen=True)
class Facet:
    id: str
    kind: str
    name: str
    value: str
    source: str


@dataclass(frozen=True)
class Slot:
    name: str
    slot: int
    source: str
    placeholder: bool


@dataclass(frozen=True)
class DecodeField:
    id: str
    register: str
    field: str
    bit_offset: int
    bit_width: int


@dataclass(frozen=True)
class Domain:
    lo: int
    hi: int
    dense: bool


@dataclass(frozen=True)
class PermSpace:
    name: str
    dims: tuple[tuple[str, tuple[str, ...]], ...]
    owners: dict[str, str]
    unrepresentable: dict[str, tuple[str, str]]


# --------------------------------------------------------------------------- #
# Loaders (each reads the generated inventory once)
# --------------------------------------------------------------------------- #

def _read_csv(path: Path) -> list[dict]:
    with path.open(newline="") as fh:
        return list(csv.DictReader(fh))


@lru_cache(maxsize=1)
def _dispositions() -> dict[str, dict]:
    merged: dict[str, dict] = {}
    for f in sorted((OUT / "dispositions").glob("*.csv")):
        for row in _read_csv(f):
            merged[row["id"]] = row
    return merged


@lru_cache(maxsize=1)
def registers() -> tuple[Register, ...]:
    disp = _dispositions()
    out: list[Register] = []
    for r in _read_csv(OUT / "rsx_state_space.csv"):
        c = disp.get(r["id"], {})
        out.append(Register(
            id=r["id"], name=r["name"], group=r["group"], cluster=r["cluster"],
            addr=r["addr"], addr_dec=int(r["addr_dec"]), word=int(r["word"]),
            has_decoder=r["has_decoder"] == "1", n_fields=int(r["n_fields"]),
            binding_kind=r["binding_kind"], handler=r["handler"],
            source=r["source"],
            disposition=(c.get("disposition") or "").strip(),
            metal4_target=(c.get("metal4_target") or "").strip(),
            difficulty=(c.get("difficulty") or "").strip(),
            risk=(c.get("risk") or "").strip(),
            confidence=(c.get("confidence") or "").strip(),
            doc_ref=(c.get("doc_ref") or "").strip(),
            notes=(c.get("notes") or "").strip()))
    return tuple(out)


@lru_cache(maxsize=1)
def values() -> tuple[Value, ...]:
    return tuple(Value(enum=r["enum"], name=r["name"], value=int(r["value"]),
                       value_hex=r["value_hex"], source=r["source"],
                       block=r["block"])
                 for r in _read_csv(OUT / "rsx_value_space.csv"))


@lru_cache(maxsize=1)
def facets() -> tuple[Facet, ...]:
    return tuple(Facet(id=r["id"], kind=r["kind"], name=r["name"],
                       value=r["value"], source=r["source"])
                 for r in _read_csv(OUT / "rsx_shader_facets.csv"))


@lru_cache(maxsize=1)
def slots() -> tuple[Slot, ...]:
    return tuple(Slot(name=r["name"], slot=int(r["slot"]), source=r["source"],
                      placeholder=r["placeholder"] == "1")
                 for r in _read_csv(OUT / "rsx_binding_slots.csv"))


@lru_cache(maxsize=1)
def decode_fields() -> tuple[DecodeField, ...]:
    return tuple(DecodeField(id=r["id"], register=r["register"], field=r["field"],
                             bit_offset=int(r["bit_offset"]),
                             bit_width=int(r["bit_width"]))
                 for r in _read_csv(OUT / "rsx_decode_fields.csv"))


@lru_cache(maxsize=1)
def clusters() -> dict[str, int]:
    return json.loads((OUT / "clusters.json").read_text())


@lru_cache(maxsize=1)
def stats() -> dict:
    return json.loads((OUT / "rsx_stats.json").read_text())


@lru_cache(maxsize=1)
def coverage() -> dict:
    return json.loads((OUT / "coverage.json").read_text())


@lru_cache(maxsize=1)
def _by_id() -> dict[str, Register]:
    return {r.id: r for r in registers()}


@lru_cache(maxsize=1)
def _by_name() -> dict[str, Register]:
    return {r.name: r for r in registers()}


@lru_cache(maxsize=1)
def _json_addr() -> dict[str, int]:
    inv = json.loads((OUT / "rsx_inventory.json").read_text())
    return {r["id"]: r["addr"] for r in inv["registers"]}


def registers_by_cluster(cluster: str) -> tuple[Register, ...]:
    return tuple(r for r in registers() if r.cluster == cluster)


def cluster_shard_ids(cluster: str) -> frozenset[str]:
    return frozenset(r["id"] for r in _read_csv(OUT / "clusters" / f"{cluster}.csv"))


def value_names() -> frozenset[str]:
    return frozenset(v.name for v in values())


# --------------------------------------------------------------------------- #
# Document resolution (reuses validate_docs.py's file index + citation regex)
# --------------------------------------------------------------------------- #

@lru_cache(maxsize=1)
def _validate_docs():
    spec = importlib.util.spec_from_file_location(
        "_metal4_validate_docs", INVENTORY / "validate_docs.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


@lru_cache(maxsize=1)
def _file_index() -> dict[str, list[Path]]:
    return _validate_docs().index_files(REPO)


@lru_cache(maxsize=1)
def _line_counts() -> dict[Path, int]:
    return {p: p.read_text(errors="replace").count("\n") + 1
            for cands in _file_index().values() for p in cands}


_DOC_TOKEN = re.compile(r"\b(HLD-\d+|LLD-[a-z0-9-]+|\d{2}|README)\b")


def resolve_citation(token: str) -> str | None:
    """Return an error string if `file:line` does not resolve, else None."""
    m = _validate_docs().CITE_RE.search(token)
    if not m:
        return None
    fname, a, b = m.group(1), int(m.group(2)), m.group(3)
    cands = _file_index().get(Path(fname).name)
    if not cands:
        return f"{fname} not found"
    end = int(b) if b else a
    total = max(_line_counts()[c] for c in cands)
    if a < 1 or end > total or end < a:
        return f"{fname}:{a} out of range ({total})"
    return None


def resolve_doc_token(token: str) -> str | None:
    """Return an error string if a design-doc token does not resolve, else None."""
    if token.startswith("HLD-"):
        found = list(DESIGN.glob(f"{token}-*.md"))
    elif token.startswith("LLD-"):
        found = [p for p in (DESIGN / f"{token}.md",) if p.exists()]
    elif token == "README":
        found = [p for p in (DESIGN / "README.md", METAL4 / "README.md") if p.exists()]
    else:
        found = list(METAL4.glob(f"{token}-*.md"))
    return None if found else f"{token} not found"


def resolve_doc_ref(ref: str) -> list[str]:
    """Resolve every token in a disposition `doc_ref`; empty list means valid."""
    errs: list[str] = []
    inherited: str | None = None
    for part in re.split(r"[;,/]", ref):
        part = part.strip()
        if not part:
            continue
        cite = resolve_citation(part)
        if cite:
            errs.append(cite)
        if _validate_docs().CITE_RE.search(part):
            continue
        m = _DOC_TOKEN.search(part)
        if m:
            inherited = m.group(1)
        token = inherited
        if token is None:
            errs.append(f"no doc token in {part!r}")
            continue
        err = resolve_doc_token(token)
        if err:
            errs.append(err)
    return errs


def resolve_source(source: str) -> str | None:
    return None if _file_index().get(Path(source).name) else f"{source} not found"


def doc_table_ids(path: Path) -> frozenset[str]:
    """The `M_*` ids listed in a design document's markdown tables."""
    return frozenset(_validate_docs().doc_table_ids(path))


def register_by_id(reg_id: str) -> Register:
    return _by_id()[reg_id]


# --------------------------------------------------------------------------- #
# Value-space domains, prefixes and aliases
# --------------------------------------------------------------------------- #
#
# `dense` enums are pure ordinals: their whole domain [lo, hi] must be present
# exactly once.  The rest are sparse by nature -- bitmask enums (`CellGcmLocation`,
# `Method`, `CellGcmSystemMode`), a format-field-plus-location enum
# (`CellGcmTexture`), the method enum mirror (`CellGcmMethod`) or the anonymous
# bucket -- so they are checked for containment and exact alias identity instead
# of contiguity.  `CellGcmTexture`/`Method`/`CellGcmSystemMode` carry deliberate
# C++ aliases, recorded in `VALUE_ALIASES`; `enum_violations` asserts the observed
# alias groups equal the declared ones, so a new alias cannot slip in silently.

VALUE_DOMAINS: dict[str, Domain] = {
    "texture_dimension": Domain(1, 3, True),
    "texture_dimension_extended": Domain(0, 3, True),
    "CellGcmDefaultFifoMode": Domain(0, 2, True),
    "CellRescTableElement": Domain(0, 1, True),
    "CellGcmSystemMode": Domain(0, 1, False),
    "CellGcmTexture": Domain(0, 159, False),
    "CellGcmLocation": Domain(0, 0xFFFFFFFF, False),
    "CellGcmMethod": Domain(0, 0xFFFFFFFF, False),
    "Method": Domain(0, 0xFFFFFFFF, False),
    "": Domain(0, 0xFFFFFFFF, False),
}

ENUM_PREFIX: dict[str, str] = {
    "CellGcmDefaultFifoMode": "CELL_GCM_DEFAULT_FIFO_MODE_",
    "CellGcmSystemMode": "CELL_GCM_SYSTEM_MODE_",
    "CellGcmTexture": "CELL_GCM_TEXTURE_",
    "CellGcmLocation": "CELL_GCM_",
    "CellRescTableElement": "CELL_RESC_ELEMENT_",
    "texture_dimension_extended": "texture_dimension_",
    "Method": "RSX_",
}

VALUE_ALIASES: dict[str, dict[int, frozenset[str]]] = {
    "CellGcmSystemMode": {
        1: frozenset({"CELL_GCM_SYSTEM_MODE_IOMAP_512MB",
                      "CELL_GCM_SYSTEM_MODE_MASK"}),
    },
    "CellGcmTexture": {
        0: frozenset({"CELL_GCM_TEXTURE_SZ", "CELL_GCM_TEXTURE_NR"}),
    },
    "Method": {
        0: frozenset({"RSX_METHOD_INCREMENT_CMD", "RSX_METHOD_NOP_CMD"}),
        3: frozenset({"RSX_METHOD_CALL_CMD_MASK", "RSX_CALL_STACK_EMPTY"}),
        0x1FFFFFFC: frozenset({"RSX_METHOD_OLD_JUMP_OFFSET_MASK",
                               "RSX_METHOD_CALL_OFFSET_MASK"}),
        0xE0000003: frozenset({"RSX_METHOD_OLD_JUMP_CMD_MASK",
                               "RSX_METHOD_NEW_JUMP_CMD_MASK"}),
        0xE0030003: frozenset({"RSX_METHOD_INCREMENT_CMD_MASK",
                               "RSX_METHOD_NON_INCREMENT_CMD_MASK"}),
    },
}

ANONYMOUS_ENUM = ""


# --------------------------------------------------------------------------- #
# Deviation registries (the contract on the current tree)
# --------------------------------------------------------------------------- #

REGISTER_DEVIATIONS: dict[str, tuple[str, ...]] = {}

VALUE_DEVIATIONS: dict[str, tuple[str, ...]] = {}

ENUM_DEVIATIONS: dict[str, tuple[str, ...]] = {}

FACET_DEVIATIONS: dict[str, tuple[str, ...]] = {}

SLOT_DEVIATIONS: dict[str, tuple[str, ...]] = {}

# A field the source marks invalid/derived: re-initialized at runtime, owns no slot.
SLOT_PLACEHOLDERS: frozenset[str] = frozenset({"vertex_textures_first_bind_slot"})

SLOT_GAPS: tuple[int, ...] = (6, 7)


# --------------------------------------------------------------------------- #
# Contract checks
# --------------------------------------------------------------------------- #

@lru_cache(maxsize=1)
def _value_name_counts() -> dict[tuple[str, str], int]:
    d: dict[tuple[str, str], int] = {}
    for v in values():
        d[(v.enum, v.name)] = d.get((v.enum, v.name), 0) + 1
    return d


@lru_cache(maxsize=1)
def _facet_id_counts() -> dict[str, int]:
    d: dict[str, int] = {}
    for f in facets():
        d[f.id] = d.get(f.id, 0) + 1
    return d


@lru_cache(maxsize=1)
def _facet_name_counts() -> dict[str, int]:
    d: dict[str, int] = {}
    for f in facets():
        d[f.name] = d.get(f.name, 0) + 1
    return d


@lru_cache(maxsize=1)
def _slot_index_counts() -> dict[int, int]:
    """Slot occupancy over the *real* fields; a placeholder owns no slot."""
    d: dict[int, int] = {}
    for s in slots():
        if not s.placeholder:
            d[s.slot] = d.get(s.slot, 0) + 1
    return d


@lru_cache(maxsize=1)
def _fields_by_register() -> dict[str, tuple[DecodeField, ...]]:
    d: dict[str, list[DecodeField]] = {}
    for f in decode_fields():
        d.setdefault(f.register, []).append(f)
    return {k: tuple(v) for k, v in d.items()}


def register_violations(reg: Register, expected_cluster: str | None = None) -> list[str]:
    v: list[str] = []
    if expected_cluster is not None and reg.cluster != expected_cluster:
        v.append("cluster-mismatch")
    try:
        addr = int(reg.addr, 16)
        if addr != reg.addr_dec or addr != reg.word * 4:
            v.append("addr-inconsistent")
    except ValueError:
        v.append("addr-inconsistent")
    if _json_addr().get(reg.id) != reg.addr_dec:
        v.append("addr-inconsistent")
    if reg.disposition not in DISPOSITIONS:
        v.append("disposition")
    if reg.disposition in ("na", "moot"):
        if reg.difficulty and reg.difficulty not in DIFFICULTIES:
            v.append("difficulty")
    elif reg.difficulty not in DIFFICULTIES:
        v.append("difficulty")
    if reg.risk not in RISKS:
        v.append("risk")
    if reg.confidence not in CONFIDENCE:
        v.append("confidence")
    if reg.disposition not in ("na", "moot") and not reg.metal4_target:
        v.append("target")
    if not reg.doc_ref or resolve_doc_ref(reg.doc_ref):
        v.append("doc-ref")
    if reg.n_fields != len(_fields_by_register().get(reg.name, ())):
        v.append("decode")
    return v


def _line_of(text: str, offset: int) -> int:
    """1-based line number of `offset` in `text`."""
    return text.count("\n", 0, offset) + 1


_ENUM_BLOCK_RE = re.compile(
    r"\benum(?:\s+class)?\s*([A-Za-z_]\w*)?\s*(?::\s*[A-Za-z_][\w:]*\s*)?\{")


@lru_cache(maxsize=1)
def enum_block_ids() -> frozenset[str]:
    """`gcm_enums.h:<line>` for every enum block in the source header.

    Anchoring every value-space block id to a real `enum` in the header gives each
    anonymous `enum {}` a stable identity and makes the id non-tautological.  The
    header holds more blocks than the value space: the extra `enum class`
    declarations alias existing constants and introduce no new ones.
    """
    src = REPO / "rpcs3" / "Emu" / "RSX" / "gcm_enums.h"
    if not src.exists():
        return frozenset(v.block for v in values())
    text = src.read_text(errors="replace")
    return frozenset(f"gcm_enums.h:{_line_of(text, m.start())}"
                     for m in _ENUM_BLOCK_RE.finditer(text))


@lru_cache(maxsize=1)
def _names_in_multiple_blocks() -> frozenset[str]:
    seen: dict[str, set[str]] = {}
    for v in values():
        seen.setdefault(v.name, set()).add(v.block)
    return frozenset(n for n, blocks in seen.items() if len(blocks) > 1)


def anonymous_blocks() -> frozenset[str]:
    """Block ids of the constants that live in an unnamed `enum {}`."""
    return frozenset(v.block for v in values() if v.enum == ANONYMOUS_ENUM)


def value_violations(val: Value) -> list[str]:
    v: list[str] = []
    if val.enum not in VALUE_DOMAINS:
        v.append("group")
    if val.source != VALUE_SOURCE:
        v.append("source")
    dom = VALUE_DOMAINS.get(val.enum)
    if dom and not (dom.lo <= val.value <= dom.hi):
        v.append("value-range")
    prefix = ENUM_PREFIX.get(val.enum)
    if prefix and not val.name.startswith(prefix):
        v.append("prefix")
    if _value_name_counts().get((val.enum, val.name), 0) > 1:
        v.append("name-dup")
    if val.block not in enum_block_ids():
        v.append("block")
    if val.name in _names_in_multiple_blocks():
        v.append("block-dup")
    return v


def enum_violations(enum: str) -> list[str]:
    members = [v for v in values() if v.enum == enum]
    dom = VALUE_DOMAINS[enum]
    v: list[str] = []
    groups: dict[int, set[str]] = {}
    for m in members:
        groups.setdefault(m.value, set()).add(m.name)
    observed = {val: frozenset(names) for val, names in groups.items() if len(names) > 1}
    if enum != ANONYMOUS_ENUM and observed != VALUE_ALIASES.get(enum, {}):
        v.append("alias")
    if dom.dense:
        distinct = sorted(set(groups))
        if distinct != list(range(dom.lo, dom.hi + 1)):
            v.append("gap")
    return v


def facet_violations(f: Facet) -> list[str]:
    v: list[str] = []
    if f.kind not in FACET_KINDS:
        v.append("kind")
    if _facet_id_counts().get(f.id, 0) > 1:
        v.append("id-dup")
    if _facet_name_counts().get(f.name, 0) > 1:
        v.append("name-dup")
    if resolve_source(f.source):
        v.append("source")
    return v


SLOT_KINDS: dict[str, str] = {
    "vertex_params_bind_slot": "setAddress",
    "vertex_constant_buffers_bind_slot": "setAddress",
    "fragment_constant_buffers_bind_slot": "setAddress",
    "fragment_state_bind_slot": "setAddress",
    "fragment_texture_params_bind_slot": "setAddress",
    "vertex_buffers_first_bind_slot": "setTexture",
    "conditional_render_predicate_slot": "setAddress",
    "rasterizer_env_bind_slot": "setAddress",
    "instancing_lookup_table_bind_slot": "setAddress",
    "instancing_constants_buffer_slot": "setAddress",
    "textures_first_bind_slot": "setTexture",
    "vertex_textures_first_bind_slot": "setTexture",
}


def slot_violations(s: Slot) -> list[str]:
    v: list[str] = []
    if not 0 <= s.slot <= 31:
        v.append("range")
    if s.name not in SLOT_KINDS:
        v.append("name")
    if SLOT_KINDS.get(s.name) not in BIND_KINDS:
        v.append("kind")
    if s.placeholder != (s.name in SLOT_PLACEHOLDERS):
        v.append("placeholder")
    if not s.placeholder and _slot_index_counts().get(s.slot, 0) > 1:
        v.append("index-dup")
    return v


# --------------------------------------------------------------------------- #
# Permutation spaces (cross-products of combinable value state)
# --------------------------------------------------------------------------- #

BLEND_FACTORS: tuple[str, ...] = (
    "CELL_GCM_ZERO", "CELL_GCM_ONE", "CELL_GCM_SRC_COLOR",
    "CELL_GCM_ONE_MINUS_SRC_COLOR", "CELL_GCM_SRC_ALPHA",
    "CELL_GCM_ONE_MINUS_SRC_ALPHA", "CELL_GCM_DST_ALPHA",
    "CELL_GCM_ONE_MINUS_DST_ALPHA", "CELL_GCM_DST_COLOR",
    "CELL_GCM_ONE_MINUS_DST_COLOR", "CELL_GCM_SRC_ALPHA_SATURATE",
    "CELL_GCM_CONSTANT_COLOR", "CELL_GCM_ONE_MINUS_CONSTANT_COLOR",
    "CELL_GCM_CONSTANT_ALPHA", "CELL_GCM_ONE_MINUS_CONSTANT_ALPHA",
)

BLEND_EQUATIONS: tuple[str, ...] = (
    "CELL_GCM_FUNC_ADD", "CELL_GCM_FUNC_SUBTRACT",
    "CELL_GCM_FUNC_REVERSE_SUBTRACT", "CELL_GCM_MIN",
    "CELL_GCM_MAX", "CELL_GCM_FUNC_ADD_SIGNED",
    "CELL_GCM_FUNC_REVERSE_ADD_SIGNED", "CELL_GCM_FUNC_REVERSE_SUBTRACT_SIGNED",
)

TEXTURE_FORMATS: tuple[str, ...] = (
    "CELL_GCM_TEXTURE_B8", "CELL_GCM_TEXTURE_A1R5G5B5",
    "CELL_GCM_TEXTURE_A4R4G4B4", "CELL_GCM_TEXTURE_R5G6B5",
    "CELL_GCM_TEXTURE_A8R8G8B8", "CELL_GCM_TEXTURE_COMPRESSED_DXT1",
    "CELL_GCM_TEXTURE_COMPRESSED_DXT23", "CELL_GCM_TEXTURE_COMPRESSED_DXT45",
    "CELL_GCM_TEXTURE_G8B8", "CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8",
    "CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8", "CELL_GCM_TEXTURE_DEPTH24_D8",
    "CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT", "CELL_GCM_TEXTURE_DEPTH16",
    "CELL_GCM_TEXTURE_DEPTH16_FLOAT", "CELL_GCM_TEXTURE_COMPRESSED_HILO8",
    "CELL_GCM_TEXTURE_COMPRESSED_HILO_S8", "CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT",
    "CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT", "CELL_GCM_TEXTURE_D1R5G5B5",
)

ADDRESS_MODES: tuple[str, ...] = (
    "CELL_GCM_TEXTURE_WRAP", "CELL_GCM_TEXTURE_MIRROR",
    "CELL_GCM_TEXTURE_CLAMP_TO_EDGE", "CELL_GCM_TEXTURE_BORDER",
    "CELL_GCM_TEXTURE_CLAMP", "CELL_GCM_TEXTURE_MIRROR_ONCE_CLAMP_TO_EDGE",
    "CELL_GCM_TEXTURE_MIRROR_ONCE_BORDER", "CELL_GCM_TEXTURE_MIRROR_ONCE_CLAMP",
    "CELL_GCM_TEXTURE_CONVOLUTION_MIN",
)

FILTER_MODES: tuple[str, ...] = (
    "CELL_GCM_TEXTURE_NEAREST", "CELL_GCM_TEXTURE_LINEAR",
    "CELL_GCM_TEXTURE_NEAREST_NEAREST", "CELL_GCM_TEXTURE_LINEAR_NEAREST",
    "CELL_GCM_TEXTURE_NEAREST_LINEAR", "CELL_GCM_TEXTURE_LINEAR_LINEAR",
)

COMPARE_FUNCS: tuple[str, ...] = (
    "CELL_GCM_TEXTURE_ZFUNC_NEVER", "CELL_GCM_TEXTURE_ZFUNC_LESS",
    "CELL_GCM_TEXTURE_ZFUNC_EQUAL", "CELL_GCM_TEXTURE_ZFUNC_LEQUAL",
    "CELL_GCM_TEXTURE_ZFUNC_GREATER", "CELL_GCM_TEXTURE_ZFUNC_NOTEQUAL",
    "CELL_GCM_TEXTURE_ZFUNC_GEQUAL", "CELL_GCM_TEXTURE_ZFUNC_ALWAYS",
)

STENCIL_OPS: tuple[str, ...] = (
    "CELL_GCM_KEEP", "CELL_GCM_ZERO", "CELL_GCM_REPLACE", "CELL_GCM_INCR",
    "CELL_GCM_DECR", "CELL_GCM_INVERT", "CELL_GCM_INCR_WRAP",
    "CELL_GCM_DECR_WRAP",
)

SURFACE_FORMATS: tuple[str, ...] = (
    "CELL_GCM_SURFACE_A8R8G8B8", "CELL_GCM_SURFACE_B8",
    "CELL_GCM_SURFACE_G8B8", "CELL_GCM_SURFACE_F_W16Z16Y16X16",
    "CELL_GCM_SURFACE_F_W32Z32Y32X32", "CELL_GCM_SURFACE_F_X32",
    "CELL_GCM_SURFACE_X8B8G8R8_Z8B8G8R8", "CELL_GCM_SURFACE_X8B8G8R8_O8B8G8R8",
    "CELL_GCM_SURFACE_A8B8G8R8", "CELL_GCM_SURFACE_X1R5G5B5_Z1R5G5B5",
    "CELL_GCM_SURFACE_X1R5G5B5_O1R5G5B5", "CELL_GCM_SURFACE_R5G6B5",
    "CELL_GCM_SURFACE_X8R8G8B8_Z8R8G8B8", "CELL_GCM_SURFACE_X8R8G8B8_O8R8G8B8",
    "CELL_GCM_SURFACE_Z16", "CELL_GCM_SURFACE_Z24S8",
)

TARGET_COUNTS: tuple[str, ...] = ("1", "2", "3", "4")

PRIMITIVES: tuple[str, ...] = (
    "CELL_GCM_PRIMITIVE_POINTS", "CELL_GCM_PRIMITIVE_LINES",
    "CELL_GCM_PRIMITIVE_LINE_LOOP", "CELL_GCM_PRIMITIVE_LINE_STRIP",
    "CELL_GCM_PRIMITIVE_TRIANGLES", "CELL_GCM_PRIMITIVE_TRIANGLE_STRIP",
    "CELL_GCM_PRIMITIVE_TRIANGLE_FAN", "CELL_GCM_PRIMITIVE_QUADS",
    "CELL_GCM_PRIMITIVE_QUAD_STRIP", "CELL_GCM_PRIMITIVE_POLYGON",
)

CULL_FACES: tuple[str, ...] = ("CELL_GCM_FRONT", "CELL_GCM_BACK")
FRONT_FACES: tuple[str, ...] = ("CELL_GCM_CW", "CELL_GCM_CCW")

PERM_SPACES: tuple[PermSpace, ...] = (
    PermSpace(
        name="blend-factor-x-equation",
        dims=(("factor", BLEND_FACTORS), ("equation", BLEND_EQUATIONS)),
        owners={"factor": "M_NV4097_SET_BLEND_FUNC_SFACTOR",
                "equation": "M_NV4097_SET_BLEND_EQUATION"},
        unrepresentable={
            "CELL_GCM_FUNC_ADD_SIGNED": ("emulate", "MTLBlendOperationAdd"),
            "CELL_GCM_FUNC_REVERSE_ADD_SIGNED": ("emulate", "MTLBlendOperationAdd"),
            "CELL_GCM_FUNC_REVERSE_SUBTRACT_SIGNED": (
                "emulate", "MTLBlendOperationReverseSubtract"),
        }),
    PermSpace(
        name="texture-format-x-address-x-filter",
        dims=(("format", TEXTURE_FORMATS), ("address", ADDRESS_MODES),
              ("filter", FILTER_MODES)),
        owners={"format": "M_NV4097_SET_TEXTURE_FORMAT",
                "address": "M_NV4097_SET_TEXTURE_ADDRESS",
                "filter": "M_NV4097_SET_TEXTURE_FILTER"},
        unrepresentable={}),
    PermSpace(
        name="depth-func-x-stencil-ops-x-two-sided",
        dims=(("depth_func", COMPARE_FUNCS), ("stencil_fail", STENCIL_OPS),
              ("stencil_zfail", STENCIL_OPS), ("stencil_zpass", STENCIL_OPS),
              ("two_sided", ("0", "1"))),
        owners={"depth_func": "M_NV4097_SET_DEPTH_FUNC",
                "stencil_fail": "M_NV4097_SET_STENCIL_OP_FAIL",
                "stencil_zfail": "M_NV4097_SET_STENCIL_OP_ZFAIL",
                "stencil_zpass": "M_NV4097_SET_STENCIL_OP_ZPASS",
                "two_sided": "M_NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE"},
        unrepresentable={}),
    PermSpace(
        name="surface-format-x-target-count",
        dims=(("format", SURFACE_FORMATS), ("targets", TARGET_COUNTS)),
        owners={"format": "M_NV4097_SET_SURFACE_FORMAT",
                "targets": "M_NV4097_SET_SURFACE_COLOR_TARGET"},
        unrepresentable={}),
    PermSpace(
        name="primitive-x-cull-x-front-face",
        dims=(("primitive", PRIMITIVES), ("cull", CULL_FACES),
              ("front", FRONT_FACES)),
        owners={"primitive": "M_NV4097_SET_BEGIN_END",
                "cull": "M_NV4097_SET_CULL_FACE",
                "front": "M_NV4097_SET_FRONT_FACE"},
        unrepresentable={}),
)


def space_combinations(space: PermSpace) -> itertools.product:
    return itertools.product(*(members for _, members in space.dims))


def space_size(space: PermSpace) -> int:
    n = 1
    for _, members in space.dims:
        n *= len(members)
    return n


def permutation_violations(space: PermSpace, combo: tuple[str, ...]) -> list[str]:
    v: list[str] = []
    for (dim, members), val in zip(space.dims, combo):
        if val not in members:
            v.append("member")
        if val in space.unrepresentable:
            disp, fallback = space.unrepresentable[val]
            if disp not in DISPOSITIONS:
                v.append("undispositioned")
            if not fallback:
                v.append("no-fallback")
            continue
        owner = _by_id().get(space.owners[dim])
        if owner is None:
            v.append("no-target")
            continue
        if owner.disposition == "impossible":
            v.append("impossible")
        if not owner.metal4_target:
            v.append("no-target")
    return v


def space_member_violations(space: PermSpace) -> list[str]:
    """Every declared constituent value must exist in the value space."""
    known = value_names()
    v: list[str] = []
    for dim, members in space.dims:
        for name in members:
            if name not in known and not name.isdigit():
                v.append(f"member:{dim}:{name}")
    for name in space.unrepresentable:
        if name not in known:
            v.append(f"unrepresentable:{name}")
    for owner in space.owners.values():
        if owner not in _by_id():
            v.append(f"owner:{owner}")
    return v


def deviations() -> dict[str, dict[str, list[str]]]:
    """Every dial on the current tree that does not meet its contract, by kind."""
    return {
        "register": {r.id: register_violations(r) for r in registers()
                     if register_violations(r)},
        "value": {v.name: value_violations(v) for v in values()
                  if value_violations(v)},
        "enum": {e or "(anonymous)": enum_violations(e) for e in VALUE_DOMAINS
                 if enum_violations(e)},
        "facet": {f.id: facet_violations(f) for f in facets()
                  if facet_violations(f)},
        "slot": {s.name: slot_violations(s) for s in slots()
                 if slot_violations(s)},
    }


# --------------------------------------------------------------------------- #
# Auditable dial counts
# --------------------------------------------------------------------------- #

def dial_counts() -> dict[str, int]:
    return {
        "registers": len(registers()),
        "values": len(values()),
        "facets": len(facets()),
        "slots": len(slots()),
        "enums": len({v.enum for v in values()}),
        "value_blocks": len({v.block for v in values()}),
        "permutations": sum(space_size(s) for s in PERM_SPACES),
        "base_cases": len(registers()) + len(values()) + len(facets()) + len(slots()),
    }


# --------------------------------------------------------------------------- #
# Test base
# --------------------------------------------------------------------------- #

class DialTest(unittest.TestCase):
    """A TestCase that turns each dial into an individually-identified subTest."""

    def assertContract(self, dial_id: str, violations: list[str],
                       known: tuple[str, ...] = ()) -> None:
        with self.subTest(dial=dial_id):
            self.assertEqual(sorted(set(violations)), sorted(set(known)), dial_id)
