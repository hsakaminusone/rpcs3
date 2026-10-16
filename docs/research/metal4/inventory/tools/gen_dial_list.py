#!/usr/bin/env python3
"""Emit the exhaustive Metal 4 dial list for rpcs3/tests/test_rsx_metal4_dials.cpp.

The inventory under out/ is the authority on which dials exist.  This script turns
it into C++ X-macro lists, one row per dial, and pre-verifies that every name it
emits appears in the RSX header the inventory cites.  The test then compiles those
names, so a name that the header does not define is a build error and not a
silent gap.

Run from docs/research/metal4/inventory:

    python3 tools/gen_dial_list.py > ../../../rpcs3/tests/rsx_metal4_dial_list.inc
"""

import csv
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "out")
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", "..", ".."))
RSX = os.path.join(REPO, "rpcs3", "Emu", "RSX")

HEADERS = {
    "gcm_enums.h": os.path.join(RSX, "gcm_enums.h"),
    "RSXVertexProgram.h": os.path.join(RSX, "Program", "RSXVertexProgram.h"),
    "RSXFragmentProgram.h": os.path.join(RSX, "Program", "RSXFragmentProgram.h"),
}


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def rows(name):
    with open(os.path.join(OUT, name), encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def num(text):
    return int(text, 0)


class Checker:
    """Every emitted name must be a token the cited header actually defines."""

    def __init__(self):
        self.text = {key: read(path) for key, path in HEADERS.items()}
        self.missing = []

    def has(self, header, token):
        if not re.search(r"\b" + re.escape(token) + r"\b", self.text[header]):
            self.missing.append((header, token))
            return False
        return True


def main():
    check = Checker()
    out = []
    counts = {}

    def emit(name, count):
        counts[name] = count

    # ---- the curated state space: 282 registers -----------------------------
    regs = rows("rsx_state_space_curated.csv")
    out.append("// The 282 curated registers: id, C++ constant, expected word.")
    out.append("#define RSX_METAL4_REGISTERS(X) \\")
    seen = set()
    n = 0
    for row in regs:
        symbol = row["name"]
        if symbol in seen:
            continue
        seen.add(symbol)
        check.has("gcm_enums.h", symbol)
        out.append("	X({}, {}, {}) \\".format(row["id"], symbol, num(row["word"])))
        n += 1
    out[-1] = out[-1].rstrip(" \\")
    out.append("")
    emit("registers", n)

    # ---- the value space: 768 constants -------------------------------------
    values = rows("rsx_value_space.csv")
    out.append("// The 768 value constants of gcm_enums.h.")
    out.append("#define RSX_METAL4_VALUES(X) \\")
    seen = set()
    n = 0
    for row in values:
        symbol = row["name"]

        # A lowercase enum name is a scoped `rsx::` enum, so its enumerators are
        # not visible unqualified.  The `gcm::` enums are unscoped.
        owner = row["enum"]
        if re.fullmatch(r"[a-z_][a-z0-9_]*", owner):
            symbol = "rsx::{}::{}".format(owner, symbol)

        if symbol in seen:
            continue
        seen.add(symbol)
        check.has("gcm_enums.h", row["name"])
        out.append('	X({}, "{}", {}) \\'.format(symbol, row["name"], num(row["value"])))
        n += 1
    out[-1] = out[-1].rstrip(" \\")
    out.append("")
    emit("values", n)

    # ---- the shader facets: 275 ---------------------------------------------
    facets = rows("rsx_shader_facets.csv")

    enums = [r for r in facets if r["kind"] == "enum"]
    out.append("// The 58 shader enums: symbol, expected value.")
    out.append("#define RSX_METAL4_FACET_ENUMS(X) \\")
    seen = set()
    n = 0
    for row in enums:
        symbol = row["name"]
        if symbol in seen:
            continue
        seen.add(symbol)
        check.has(row["source"], symbol)
        out.append('	X({}, "{}", {}) \\'.format(symbol, symbol, num(row["value"])))
        n += 1
    out[-1] = out[-1].rstrip(" \\")
    out.append("")
    emit("facet_enums", n)

    # opcode name table: rsx_fp_op_names[N] must equal the recorded string
    ops = [r for r in facets if r["kind"] == "opcode"]
    out.append("// The 70 fragment opcode names: index, expected spelling.")
    out.append("#define RSX_METAL4_FACET_OPCODES(X) \\")
    n = 0
    for row in ops:
        match = re.fullmatch(r"rsx_fp_op_names\[(\d+)\]", row["name"])
        if not match:
            raise SystemExit("opcode facet is not an array slot: " + row["name"])
        check.has(row["source"], "rsx_fp_op_names")
        out.append('	X({}, "{}") \\'.format(match.group(1), row["value"]))
        n += 1
    out[-1] = out[-1].rstrip(" \\")
    out.append("")
    emit("facet_opcodes", n)

    # bitfields: struct, member, shift, width
    bits = [r for r in facets if r["kind"] == "bitfield"]
    out.append("// The 129 shader bitfields: union, member, shift, width.")
    out.append("#define RSX_METAL4_FACET_BITFIELDS(X) \\")
    n = 0
    for row in bits:
        owner, _, member = row["name"].partition("::")
        match = re.fullmatch(r"bit (\d+):(\d+)", row["value"])
        if not match:
            raise SystemExit("bitfield facet has no bit range: " + row["value"])
        check.has(row["source"], owner)
        check.has(row["source"], member)
        out.append("	X({}, {}, {}, {}) \\".format(owner, member, match.group(1), match.group(2)))
        n += 1
    out[-1] = out[-1].rstrip(" \\")
    out.append("")
    emit("facet_bitfields", n)

    # struct members: existence
    fields = [r for r in facets if r["kind"] == "field"]
    out.append("// The 18 shader struct members: type, member.")
    out.append("#define RSX_METAL4_FACET_FIELDS(X) \\")
    n = 0
    for row in fields:
        owner, _, member = row["name"].partition("::")
        check.has(row["source"], owner)
        check.has(row["source"], member)
        out.append("	X({}, {}) \\".format(owner, member))
        n += 1
    out[-1] = out[-1].rstrip(" \\")
    out.append("")
    emit("facet_fields", n)

    total = sum(counts.values())
    print("// Generated by inventory/tools/gen_dial_list.py.  Do not edit.", file=sys.stderr)
    print("// counts: {} total {}".format(
        ", ".join("{}={}".format(k, v) for k, v in sorted(counts.items())), total),
        file=sys.stderr)

    if check.missing:
        print("MISSING FROM THE HEADERS:", file=sys.stderr)
        for header, token in sorted(set(check.missing)):
            print("  {} : {}".format(header, token), file=sys.stderr)
        raise SystemExit(1)

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
