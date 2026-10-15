#!/usr/bin/env python3
"""
Mechanically extract the RSX state space from the RPCS3 source tree.

This is the *completeness spine* generator for the RSX -> Metal 4 design work.  It
parses the authoritative in-tree sources (never a hand-written list) and emits a
canonical, stable-ID inventory of everything the guest can put into RSX state.

Three spaces are extracted and kept distinct:

  COMMAND space  -- the FIFO method registers the guest writes
                    (gcm_enums.h method enum; word-indexed, byte addr = word * 4)
  DECODE space   -- per-register bitfield layout (rsx_decode.h registers_decoder<>)
  VALUE space    -- enumerations the registers can carry (CELL_GCM_* formats, blend
                    factors, comparison functions, ...) from gcm_enums.h

plus the dispatch table (rsx_methods.cpp), the argument-table slots
(VK/vkutils/pipeline_binding_table.h) and the NV47/HW implementation modules.

Outputs (CSV + JSON) are written under OUT/ and are meant to be reviewed and diffed
in git.  The script is deterministic: running it twice yields byte-identical files
(asserted by test_generate.py).

Usage:
    python3 generate.py [--root <repo root>] [--out <dir>] [--quiet]

Exit code is non-zero if the extraction sanity checks fail.
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path


HERE = Path(__file__).resolve().parent
DEFAULT_ROOT = HERE.parents[3]  # <repo>/docs/research/metal4/inventory -> <repo>

# Namespaces that own FIFO methods.  Everything else in gcm_enums.h is value space.
METHOD_PREFIXES = ("NV4097_", "NV406E_", "NV0039_", "NV3062_", "NV309E_", "NV308A_", "NV3089_")
METHOD_EXTRA = ("GCM_", "FIFO::", "FIFO_")

# Mechanical clustering of the command space, ordered: first match wins.  Cluster
# names are the unit of work for the low-level design docs.
CLUSTER_RULES: list[tuple[str, tuple[str, ...]]] = [
    ("nv0039-dma", ("NV0039",)),
    ("nv2d-surface", ("NV3062", "NV309E")),
    ("nv308a-image", ("NV308A",)),
    ("nv3089-image", ("NV3089",)),
    ("sync-flip", ("NV406E_", "SEMAPHORE", "FLIP", "GET_REPORT", "CLEAR_REPORT",
                   "NOTIFY", "WAIT_FOR_IDLE", "SET_REFERENCE", "PIPE_NOP",
                   "DRAW_BARRIER", "USER_COMMAND", "DRIVER_QUEUE")),
    ("draw", ("DRAW_ARRAYS", "DRAW_INDEX_ARRAY", "INLINE_ARRAY", "ARRAY_ELEMENT",
              "SET_BEGIN_END", "INDEX_ARRAY", "RESTART_INDEX", "VERTEX_DATA_BASE",
              "SET_RENDER_ENABLE")),
    ("vertex", ("VERTEX_DATA", "VERTEX_ATTRIB", "TRANSFORM_", "VERTEX_TEXTURE_",
                "INDEXED_CONSTANT", "SHADER_PACKER")),
    ("texture", ("TEXTURE_", "TEX_COORD_CONTROL", "SHADER_PROGRAM", "SHADER_CONTROL",
                 "SHADER_WINDOW", "COLOR_KEY_COLOR", "NO_PARANOID")),
    ("surface", ("SURFACE_", "CONTEXT_DMA_COLOR_", "WINDOW_", "CLEAR_SURFACE",
                 "CLEAR_RECT", "COLOR_CLEAR", "CONTEXT_DMA_STATE", "CYLINDRICAL_WRAP",
                 "BACK_END_WRITE", "SET_CONTEXT_DMA", "SET_OBJECT")),
    ("depth-rop", ("DEPTH_", "STENCIL_", "ZMIN_MAX", "ZSTENCIL_CLEAR", "COLOR_MASK",
                   "ZCULL", "ZPASS", "SCULL", "ANTI_ALIASING", "REDUCE_DST",
                   "CONTEXT_DMA_ZETA", "CLIP_ID")),
    ("blend", ("BLEND_", "ALPHA_", "DITHER", "LOGIC_OP", "SHADE_MODE", "FLAT_SHADE")),
    ("raster", ("VIEWPORT_", "SCISSOR_", "CLIP_", "CULL_", "FRONT_FACE", "POLYGON_",
                "POLY_", "LINE_", "POINT_", "USER_CLIP", "TWO_SIDED_STENCIL", "SHADE")),
    ("fixed-function", ("SPECULAR", "TWO_SIDE_LIGHT", "MATERIAL", "FOG_", "TEXGEN")),
]


def classify(name: str) -> str:
    for cluster, needles in CLUSTER_RULES:
        if any(n in name for n in needles):
            return cluster
    return "misc"



def rsx_dir(root: Path) -> Path:
    return root / "rpcs3" / "Emu" / "RSX"


# --------------------------------------------------------------------------- #
# Data model
# --------------------------------------------------------------------------- #

@dataclass
class DecoderField:
    register: str
    method: str
    offset: int
    width: int


@dataclass
class Binding:
    register: str
    kind: str
    handler: str
    step: int = 1
    count: int = 1


@dataclass
class Register:
    name: str
    word: int = -1                 # value as written in the method enum (methods[] index)
    source: str = ""
    has_decoder: bool = False
    decoder_fields: list[DecoderField] = field(default_factory=list)
    binding: Binding | None = None

    @property
    def addr(self) -> int:         # byte address
        return self.word * 4 if self.word >= 0 else -1

    @property
    def id(self) -> str:
        return f"M_{self.name}"

    @property
    def addr_hex(self) -> str:
        return f"0x{self.addr:04x}" if self.addr >= 0 else ""

    @property
    def group(self) -> str:
        return self.name.split("_", 1)[0]

    @property
    def cluster(self) -> str:
        return classify(self.name)


# --------------------------------------------------------------------------- #
# Enum / method space
# --------------------------------------------------------------------------- #

_ENUM_BLOCK_RE = re.compile(
    r"\benum(?:\s+class)?\s*([A-Za-z_]\w*)?\s*(?::\s*[A-Za-z_][\w:]*\s*)?\{")
_ENUM_ENTRY_RE = re.compile(r"\s*([A-Za-z_]\w*)\s*(?:=\s*(.+?))?\s*$")


def _eval(expr: str) -> int | None:
    """Evaluate a tiny subset of C++ constant expressions used in these enums."""
    expr = expr.strip()
    shift = 0
    m = re.match(r"^(.*?)\s*>>\s*(\d+)$", expr)
    if m:
        expr, shift = m.group(1).strip(), int(m.group(2))
    try:
        if expr.lower().startswith("0x"):
            v = int(expr, 16)
        elif re.fullmatch(r"-?\d+", expr):
            v = int(expr)
        else:
            return None
    except ValueError:
        return None
    return v >> shift


def parse_gcm_enums(path: Path) -> tuple[dict[str, int], list[dict]]:
    """Return (method name -> word index, list of value-space enum entries)."""
    methods: dict[str, int] = {}
    values: list[dict] = []
    if not path.exists():
        return methods, values
    text = path.read_text(errors="replace")
    for em in _ENUM_BLOCK_RE.finditer(text):
        enum_name = em.group(1) or ""
        brace = text.find("{", em.start())
        end = _brace_match(text, brace)
        if brace < 0 or end < 0:
            continue
        body = text[brace + 1:end]
        body = re.sub(r"//[^\n]*", "", body)
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        last: int | None = None
        for tok in body.split(","):
            tm = _ENUM_ENTRY_RE.match(tok)
            if not tm or not tm.group(1):
                continue
            name = tm.group(1)
            if tm.group(2) is not None:
                v = _eval(tm.group(2))
            else:
                v = (last + 1) if last is not None else None
            if v is None:
                continue
            last = v
            values.append({"enum": enum_name, "name": name, "value": v,
                           "value_hex": f"0x{v:x}", "source": path.name})
            if name.startswith(METHOD_PREFIXES) or name.startswith(METHOD_EXTRA):
                methods.setdefault(name, v)
    return methods, values


# --------------------------------------------------------------------------- #
# Decode space
# --------------------------------------------------------------------------- #

def _brace_match(text: str, open_idx: int) -> int:
    depth = 0
    i = open_idx
    while i < len(text):
        if text[i:i + 2] == "//":
            j = text.find("\n", i)
            if j < 0:
                return -1
            i = j
            continue
        if text[i:i + 2] == "/*":
            j = text.find("*/", i)
            if j < 0:
                return -1
            i = j + 2
            continue
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


_DEC_SPEC_RE = re.compile(r"struct\s+registers_decoder\s*<\s*([^>]+?)\s*>")
_METHOD_SIG_RE = re.compile(r"([A-Za-z_]\w*)\s*\(\s*\)\s*const")
_BF_RE = re.compile(r"bf_decoder\s*<\s*(\d+)\s*,\s*(\d+)\s*>")


def parse_decoders(path: Path) -> tuple[set[str], dict[str, list[DecoderField]]]:
    """Return (registers that have a decoder, register -> bitfields)."""
    have: set[str] = set()
    fields: dict[str, list[DecoderField]] = {}
    if not path.exists():
        return have, fields
    text = path.read_text(errors="replace")
    for m in _DEC_SPEC_RE.finditer(text):
        reg = " ".join(m.group(1).split())
        brace = text.find("{", m.end())
        if brace < 0:
            continue
        end = _brace_match(text, brace)
        if end < 0:
            continue
        have.add(reg)
        block = text[brace:end]
        for bm in _BF_RE.finditer(block):
            off, width = int(bm.group(1)), int(bm.group(2))
            # nearest preceding `method() const`
            sig = None
            for sm in _METHOD_SIG_RE.finditer(block[:bm.start()]):
                sig = sm.group(1)
            fields.setdefault(reg, []).append(
                DecoderField(register=reg, method=sig or "", offset=off, width=width))
    return have, fields


# --------------------------------------------------------------------------- #
# Dispatch table
# --------------------------------------------------------------------------- #

_BIND_RE = re.compile(r"\bbind\(\s*([^,]+?)\s*,\s*([A-Za-z_][\w:]*)\s*\)")
_BIND_RANGE_RE = re.compile(
    r"\bbind_range\s*<\s*([^,]+?)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([^>]+?)\s*>")
_BIND_ARRAY_RE = re.compile(
    r"\bbind_array\(\s*([^,]+?)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([A-Za-z_][\w:]*)\s*\)")


def _strip(expr: str) -> str:
    return " ".join(expr.split())


def parse_bindings(path: Path, methods: dict[str, int]) -> dict[str, Binding]:
    out: dict[str, Binding] = {}
    if not path.exists():
        return out
    text = path.read_text(errors="replace")
    for m in _BIND_RE.finditer(text):
        name = _strip(m.group(1))
        out.setdefault(name, Binding(register=name, kind="bind", handler=m.group(2)))
    for m in _BIND_RANGE_RE.finditer(text):
        name = _strip(m.group(1))
        out.setdefault(name, Binding(register=name, kind="bind_range",
                                     handler=_strip(m.group(4)),
                                     step=int(m.group(2)), count=int(m.group(3))))
    for m in _BIND_ARRAY_RE.finditer(text):
        name = _strip(m.group(1))
        out.setdefault(name, Binding(register=name, kind="bind_array",
                                     handler=m.group(4),
                                     step=int(m.group(2)), count=int(m.group(3))))
    return out


_BINDING_SLOT_RE = re.compile(r"\bu8\s+([A-Za-z_]\w*)\s*=\s*(\d+)\s*;")
_FUNC_RE = re.compile(r"^\s*(?:void|bool|u32|s32|u8|u16)\s+([A-Za-z_]\w*)\s*\(")


def parse_binding_table(path: Path) -> list[dict]:
    slots = []
    if not path.exists():
        return slots
    for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        m = _BINDING_SLOT_RE.search(line)
        if m:
            slots.append({"name": m.group(1), "slot": int(m.group(2)),
                          "source": f"{path.name}:{n}"})
    return slots


def parse_nv47_modules(hw_dir: Path) -> list[dict]:
    mods = []
    if not hw_dir.exists():
        return mods
    for f in sorted(hw_dir.glob("*.cpp")):
        funcs = []
        for line in f.read_text(errors="replace").splitlines():
            m = _FUNC_RE.match(line)
            if m:
                funcs.append(m.group(1))
        mods.append({"module": f.stem, "file": f.name,
                     "n_functions": len(funcs), "functions": funcs})
    return mods


# --------------------------------------------------------------------------- #
# Shader facet space
# --------------------------------------------------------------------------- #
#
# The RSX shader ISA is state too: the guest uploads a vertex/fragment microcode
# whose *encoding* is fixed.  We extract the finite facet space (opcodes,
# register types/precisions, instruction bitfields, program control fields) from
# the two program headers, which are the authoritative definitions.

_SHADER_ENUM_IDS = {
    "sca_opcode": ("SH_V_SCA_", "RSX_SCA_OPCODE_"),
    "vec_opcode": ("SH_V_VEC_", "RSX_VEC_OPCODE_"),
    "vp_reg_type": ("SH_V_REG_", "RSX_VP_REGISTER_TYPE_"),
    "register_type": ("SH_F_REG_", "RSX_FP_REGISTER_TYPE_"),
    "register_precision": ("SH_F_PREC_", "RSX_FP_PRECISION_"),
}

_OPNAME_ARRAY_RE = re.compile(r"(\w*op_names)\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", re.S)
_OPNAME_STR_RE = re.compile(r'"([^"]*)"')
_UNION_RE = re.compile(r"\bunion\s+([A-Za-z_]\w*)\s*\{")
_STRUCT_OPEN_RE = re.compile(r"\bstruct\s*\{")
_BITFIELD_RE = re.compile(r"\b(?:u32|u8|u16|s32)\s+([A-Za-z_]\w*)\s*:\s*(\d+)\s*$")
_ANON_BIT_RE = re.compile(r"\b(?:u32|u8|u16)\s*:\s*(\d+)\s*$")


def parse_instr_bitfields(path: Path, prefix: str) -> list[dict]:
    """Bitfields of the instruction unions (VP: D0..D3,Src ; FP: OPDEST,Src0..2)."""
    out: list[dict] = []
    if not path.exists():
        return out
    text = path.read_text(errors="replace")
    for um in _UNION_RE.finditer(text):
        name = um.group(1)
        brace = text.find("{", um.end() - 1)
        end = _brace_match(text, brace)
        if brace < 0 or end < 0:
            continue
        block = text[brace:end]
        for sm in _STRUCT_OPEN_RE.finditer(block):
            ob = block.find("{", sm.end() - 1)
            eb = _brace_match(block, ob)
            if eb < 0:
                continue
            off = 0
            for chunk in block[ob + 1:eb].split(";"):
                bm = _BITFIELD_RE.search(chunk)
                if bm:
                    out.append({"id": f"{prefix}{name}.{bm.group(1)}", "kind": "bitfield",
                                "name": f"{name}::{bm.group(1)}",
                                "value": f"bit {off}:{bm.group(2)}", "source": path.name})
                    off += int(bm.group(2))
                    continue
                am = _ANON_BIT_RE.search(chunk)
                if am:
                    off += int(am.group(1))
    return out


def parse_struct_fields(path: Path, name: str, prefix: str) -> list[dict]:
    """Top-level data members of a program struct (method bodies excluded)."""
    out: list[dict] = []
    if not path.exists():
        return out
    text = path.read_text(errors="replace")
    m = re.search(r"\bstruct\s+" + re.escape(name) + r"\b", text)
    if not m:
        return out
    brace = text.find("{", m.end())
    end = _brace_match(text, brace)
    if brace < 0 or end < 0:
        return out
    depth = 0
    for line in text[brace + 1:end].splitlines():
        s = line.strip()
        if depth == 0 and s and not s.startswith("//"):
            fm = re.match(r"([A-Za-z_][\w:<>\*&\s]*?)\s+([A-Za-z_]\w*)\s*(?:=|;|\{)", s)
            if fm:
                out.append({"id": prefix + fm.group(2), "kind": "field",
                            "name": f"{name}::{fm.group(2)}",
                            "value": fm.group(1).strip(), "source": path.name})
        depth += line.count("{") - line.count("}")
    return out


def parse_shader_facets(vp: Path, fp: Path) -> list[dict]:
    facets: list[dict] = []
    for path in (vp, fp):
        _, vals = parse_gcm_enums(path)
        for v in vals:
            spec = _SHADER_ENUM_IDS.get(v["enum"])
            if not spec:
                continue
            idp, strip = spec
            nm = v["name"]
            if strip and nm.startswith(strip):
                nm = nm[len(strip):]
            facets.append({"id": idp + nm, "kind": "enum", "name": v["name"],
                           "value": f"0x{v['value']:x}", "source": path.name})
    if fp.exists():
        text = fp.read_text(errors="replace")
        for m in _OPNAME_ARRAY_RE.finditer(text):
            seen: set[str] = set()
            for i, nm in enumerate(_OPNAME_STR_RE.findall(m.group(2))):
                key = f"SH_F_OP_{nm}"
                if key in seen:
                    key = f"{key}_{i}"
                seen.add(f"SH_F_OP_{nm}")
                facets.append({"id": key, "kind": "opcode",
                               "name": f"{m.group(1)}[{i}]", "value": nm,
                               "source": fp.name})
    facets += parse_instr_bitfields(vp, "SH_V_BF_")
    facets += parse_instr_bitfields(fp, "SH_F_BF_")
    facets += parse_struct_fields(vp, "RSXVertexProgram", "SH_V_FIELD_")
    facets += parse_struct_fields(fp, "RSXFragmentProgram", "SH_F_FIELD_")
    return facets


# --------------------------------------------------------------------------- #
# Assembly
# --------------------------------------------------------------------------- #

def build(root: Path) -> dict:
    rsx = rsx_dir(root)
    methods, values = parse_gcm_enums(rsx / "gcm_enums.h")
    have_dec, dec_fields = parse_decoders(rsx / "rsx_decode.h")
    bindings = parse_bindings(rsx / "rsx_methods.cpp", methods)

    registers: list[Register] = []
    for name, word in methods.items():
        reg = Register(name=name, word=word, source="gcm_enums.h")
        if name in have_dec:
            reg.has_decoder = True
            reg.decoder_fields = dec_fields.get(name, [])
        b = bindings.get(name)
        if b:
            reg.binding = b
        registers.append(reg)
    registers.sort(key=lambda r: (r.word, r.name))

    known = {r.name for r in registers}
    orphan_bindings = [b for k, b in bindings.items() if b.register not in known]
    slots = parse_binding_table(rsx / "VK" / "vkutils" / "pipeline_binding_table.h")
    nv47 = parse_nv47_modules(rsx / "NV47" / "HW")
    facets = parse_shader_facets(rsx / "Program" / "RSXVertexProgram.h",
                                 rsx / "Program" / "RSXFragmentProgram.h")

    return {"registers": registers, "values": values,
            "orphan_bindings": orphan_bindings, "slots": slots, "nv47": nv47,
            "shader_facets": facets}


# --------------------------------------------------------------------------- #
# Checks
# --------------------------------------------------------------------------- #

def check(model: dict) -> list[str]:
    errs: list[str] = []
    regs = model["registers"]
    if len(regs) < 250:
        errs.append(f"only {len(regs)} method registers (expected >= 250)")
    names = [r.name for r in regs]
    if len(names) != len(set(names)):
        errs.append("duplicate register names")
    by = {r.name: r for r in regs}
    if "NV4097_SET_SURFACE_FORMAT" not in by:
        errs.append("NV4097_SET_SURFACE_FORMAT missing")
    elif by["NV4097_SET_SURFACE_FORMAT"].addr != 0x208:
        errs.append("NV4097_SET_SURFACE_FORMAT addr != 0x208")
    nd = sum(1 for r in regs if r.has_decoder)
    if nd < 150:
        errs.append(f"only {nd} registers have a decoder (expected >= 150)")
    nb = sum(1 for r in regs if r.binding)
    if nb < 40:
        errs.append(f"only {nb} registers have a binding (expected >= 40)")
    if len(model["values"]) < 400:
        errs.append(f"only {len(model['values'])} value-space constants (expected >= 400)")
    if len(model["slots"]) < 10:
        errs.append("binding table too small")
    if len(model["shader_facets"]) < 80:
        errs.append(f"only {len(model['shader_facets'])} shader facets (expected >= 80)")
    return errs


# --------------------------------------------------------------------------- #
# Writers
# --------------------------------------------------------------------------- #

def write_csv(path: Path, rows: list[dict], cols: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)


def emit(model: dict, out: Path) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    regs = model["registers"]

    spine, fields = [], []
    for r in regs:
        b = r.binding
        spine.append({
            "id": r.id, "name": r.name, "group": r.group, "cluster": r.cluster,
            "addr": r.addr_hex, "addr_dec": r.addr, "word": r.word,
            "has_decoder": int(r.has_decoder), "n_fields": len(r.decoder_fields),
            "binding_kind": b.kind if b else "", "handler": b.handler if b else "",
            "step": b.step if b else "", "count": b.count if b else "",
            "source": r.source,
            "disposition": "", "metal4_target": "", "difficulty": "",
            "risk": "", "confidence": "", "doc_ref": "",
        })
        for f in r.decoder_fields:
            fields.append({
                "id": f"{r.id}.{f.method or 'bit' + str(f.offset)}",
                "register": r.name, "field": f.method,
                "bit_offset": f.offset, "bit_width": f.width,
                "source": f"{r.source}; rsx_decode.h",
            })

    write_csv(out / "rsx_state_space.csv", spine, list(spine[0].keys()))
    write_csv(out / "rsx_decode_fields.csv", fields, list(fields[0].keys()))
    write_csv(out / "rsx_value_space.csv", model["values"],
              ["enum", "name", "value", "value_hex", "source"])
    write_csv(out / "rsx_binding_slots.csv", model["slots"], ["name", "slot", "source"])
    write_csv(out / "rsx_shader_facets.csv", model["shader_facets"],
              ["id", "kind", "name", "value", "source"])

    # Per-cluster shards: the unit of work for the low-level design docs.
    clusters: dict[str, list[dict]] = {}
    for row in spine:
        clusters.setdefault(row["cluster"], []).append(row)
    cdir = out / "clusters"
    if cdir.exists():
        for old in cdir.glob("*.csv"):
            old.unlink()
    for cname, rows in clusters.items():
        write_csv(cdir / f"{cname}.csv", rows, list(spine[0].keys()))
    (out / "clusters.json").write_text(json.dumps(
        {k: len(v) for k, v in sorted(clusters.items())},
        indent=2, sort_keys=True) + "\n")

    stats = {
        "registers": len(regs),
        "with_decoder": sum(1 for r in regs if r.has_decoder),
        "decoder_fields": len(fields),
        "with_binding": sum(1 for r in regs if r.binding),
        "binding_kinds": _count(regs, lambda r: r.binding.kind if r.binding else "unbound"),
        "value_constants": len(model["values"]),
        "value_enums": len({v["enum"] for v in model["values"]}),
        "orphan_bindings": len(model["orphan_bindings"]),
        "binding_slots": len(model["slots"]),
        "nv47_modules": len(model["nv47"]),
        "shader_facets": len(model["shader_facets"]),
        "clusters": len(clusters),
    }
    (out / "rsx_stats.json").write_text(json.dumps(stats, indent=2, sort_keys=True) + "\n")
    (out / "rsx_inventory.json").write_text(json.dumps({
        "registers": [{
            "id": r.id, "name": r.name, "addr": r.addr, "source": r.source,
            "has_decoder": r.has_decoder,
            "fields": [asdict(f) for f in r.decoder_fields],
            "binding": asdict(r.binding) if r.binding else None} for r in regs],
        "orphan_bindings": [asdict(b) for b in model["orphan_bindings"]],
        "slots": model["slots"], "nv47": model["nv47"],
    }, indent=2, sort_keys=True) + "\n")
    emit_summary(out, spine, fields, stats, clusters, model)
    return stats


def emit_summary(out: Path, spine: list[dict], fields: list[dict], stats: dict,
                 clusters: dict[str, list[dict]], model: dict) -> None:
    L: list[str] = []
    A = L.append
    A("# RSX state-space inventory — generated summary")
    A("")
    A("> **Generated by `generate.py`. Do not edit by hand.** This is the mechanical")
    A("> completeness spine for the RSX -> Metal 4 design work; the curation layer")
    A("> (`disposition`/`metal4_target`/...) is filled separately per cluster.")
    A("")
    A("## Global counts")
    A("")
    A("| metric | value |")
    A("|---|---|")
    for k in ("registers", "with_decoder", "decoder_fields", "with_binding",
              "value_constants", "value_enums", "orphan_bindings", "binding_slots",
              "nv47_modules", "shader_facets", "clusters"):
        A(f"| {k} | {stats[k]} |")
    A("")
    A(f"Binding kinds: " + ", ".join(f"`{k}`={v}" for k, v in stats["binding_kinds"].items()))
    A("")
    A("## Clusters")
    A("")
    A("| cluster | registers | with decoder | bound |")
    A("|---|---|---|---|")
    for cname, rows in sorted(clusters.items()):
        nd = sum(1 for r in rows if r["has_decoder"] == 1)
        nb = sum(1 for r in rows if r["binding_kind"])
        A(f"| {cname} | {len(rows)} | {nd} | {nb} |")
    A("")
    for cname, rows in sorted(clusters.items()):
        A(f"## Cluster: `{cname}` ({len(rows)} registers)")
        A("")
        A("| id | addr | dec | flds | binding | handler | disposition |")
        A("|---|---|---|---|---|---|---|")
        for r in rows:
            bind = r["binding_kind"]
            if r["count"] and int(r["count"]) > 1:
                bind = f"{bind} x{r['count']}"
            A(f"| `{r['id']}` | {r['addr']} | {r['has_decoder']} | {r['n_fields']} | "
              f"{bind} | {r['handler']} | {r['disposition']} |")
        A("")
    # decode fields
    A("## Decoded bitfields")
    A("")
    A(f"{len(fields)} bitfield extractions across "
      f"{len({f['register'] for f in fields})} registers.")
    A("")
    A("| register | field | bit offset | width |")
    A("|---|---|---|---|")
    for f in fields:
        A(f"| `{f['register']}` | {f['field']} | {f['bit_offset']} | {f['bit_width']} |")
    A("")
    # value space
    A("## Value space (enums the registers can carry)")
    A("")
    by_enum: dict[str, int] = {}
    for v in model["values"]:
        by_enum[v["enum"]] = by_enum.get(v["enum"], 0) + 1
    A("| enum | constants |")
    A("|---|---|")
    for e, n in sorted(by_enum.items()):
        A(f"| `{e or '(anonymous)'}` | {n} |")
    A("")
    # nv47
    A("## NV47/HW implementation modules")
    A("")
    A("| module | functions |")
    A("|---|---|")
    for m in model["nv47"]:
        A(f"| `{m['module']}.cpp` | {m['n_functions']} |")
    A("")
    A("## Shader facet space (VP/FP instruction encoding)")
    A("")
    kinds2: dict[str, int] = {}
    for f in model["shader_facets"]:
        kinds2[f["kind"]] = kinds2.get(f["kind"], 0) + 1
    A("| kind | count |")
    A("|---|---|")
    for k, n in sorted(kinds2.items()):
        A(f"| `{k}` | {n} |")
    A("")
    A("| id | kind | value |")
    A("|---|---|---|")
    for f in model["shader_facets"]:
        A(f"| `{f['id']}` | {f['kind']} | {f['value']} |")
    A("")
    (out / "SUMMARY.md").write_text("\n".join(L) + "\n")


def _count(regs, key) -> dict:
    d: dict = {}
    for r in regs:
        k = key(r)
        d[k] = d.get(k, 0) + 1
    return dict(sorted(d.items()))


# --------------------------------------------------------------------------- #
# Entry point
# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    ap.add_argument("--out", type=Path, default=HERE / "out")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    model = build(args.root)
    errs = check(model)
    stats = emit(model, args.out)

    if not args.quiet:
        print(f"root: {args.root}")
        print(f"out:  {args.out}")
        for k, v in stats.items():
            print(f"  {k}: {v}")
        if errs:
            print("SANITY FAILURES:", file=sys.stderr)
            for e in errs:
                print(f"  - {e}", file=sys.stderr)
    return 1 if errs else 0


if __name__ == "__main__":
    raise SystemExit(main())
