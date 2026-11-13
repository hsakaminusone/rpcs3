#!/usr/bin/env python3
"""Metal 4 release scrubber.

This tool turns the development tree (metal4-dev) into the public tree by
deleting the diagnostic facility **as syntactic units**, then proving that no
diagnostic token survived.

It is a release tool.  It is tracked on metal4-dev and lists itself in
DELETE_FILES, so a release checkout carries no copy of it.

Usage
-----
    python3 .metal4-release-scrub.py check   [--root DIR] [--json OUT] [--allow-orphans]
    python3 .metal4-release-scrub.py apply   [--root DIR] [--force] [--json OUT] [--allow-orphans]
    python3 .metal4-release-scrub.py verify  [--root DIR] [--build-cmd CMD]
    python3 .metal4-release-scrub.py selftest

Run `apply` from the dev tree; it deletes this file along with the rest of the
diagnostic facility, so the release checkout cannot be scrubbed twice.

`check` is read-only and prints exactly what `apply` would do.  `apply` refuses
a dirty tracked tree unless --force.  `verify` runs the forbidden-token gate on
the tree as it stands, checks that the deletion list is honoured, runs
`git diff --check`, and optionally the build/test command.

Excision contract
-----------------
The source tree is written so every diagnostic can be removed without touching
production behaviour.  A diagnostic is one of:

1. A whole tracked file in DELETE_FILES / DELETE_PREFIXES.
2. A standalone diagnostic macro statement
       MTL_NOTE(...); MTL_PERF_SCOPE(...); MTL_PROBE_LATCH(...); ...
3. A preprocessor region gated on MTL_DEBUG_LOGS / MTL_PERF_ENABLED /
   MTL_DIAGNOSTICS.  An `#else` branch is production and is kept.
4. A marker-annotated unit.  The marker
       // TODO(excise): <level>; <reason>
   applies to the next syntactic unit (or, when inline, the statement on that
   line): a declaration, statement, if/else chain, function, struct or block.
5. A trusted explicit range
       // EXCISE-BEGIN <tag>
       ...
       // EXCISE-END <tag>
   for a diagnostic that is interleaved with production statements.  These
   ranges are for the cases where deleting "the next unit" would be wrong; the
   author is expected to have kept production writes outside the range.

Rules 2, 4 and 5 may not delete a unit containing `return`, `break`, `continue`
or `goto`.  Rule 4's control/statement units are checked against one rule: a
marked standalone unit may *read* anything, but it may only *assign* to
diagnostic state (`diagnostic_*`, `m_diag_*`, ...) or to a variable it declares
itself.  A write to production state is reported as NEEDS-REVIEW, because the
author must widen the annotation to an explicit range that keeps the production
write outside.  The compiler is the second gate; `verify --build-cmd` runs it.

The final gate is a forbidden-token scan over the whole tree.  A single
`MTL_NOTE`, `probe::`, `TODO(excise)` or `diagnostic_` in a surviving file fails
the release.  That is deliberate: it is better to stop and add a marker than to
ship a half-removed probe.

The token gate cannot see the other half of an excision: a declaration whose
only user was removed.  `check` and `apply` therefore also report *orphans* —
an identifier read by an excised line that now occurs exactly once in the whole
release tree and whose surviving occurrence looks like a declaration.  These are
reported, never deleted (the initialiser may call production code, and a
parameter name is part of a signature the call sites already match): mark the
declaration for excision, or `[[maybe_unused]]` the parameter.  `apply` refuses
while any remain; `--allow-orphans` is only for a list that has been read and
found to be false positives.
"""
from __future__ import annotations

import argparse
import collections
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

# ------------------------------------------------------------------ inventory

# Whole files that exist only for the diagnostic facility.
DELETE_FILES = {
    # The tool itself: a release checkout is the input, not another dev tree.
    ".metal4-release-scrub.py",
    "rpcs3/Emu/RSX/MTL/MTLInstrumentation.h",
    "rpcs3/Emu/RSX/Common/rsx_surface_diag.h",
    # Tests whose authority is the dev inventory, not the production contract.
    "rpcs3/tests/test_rsx_metal4_dials.cpp",
    "rpcs3/tests/test_rsx_metal4_interpreter.cpp",
    "rpcs3/tests/test_rsx_metal4_mapping.cpp",
    "rpcs3/tests/test_rsx_metal4_state.cpp",
    "rpcs3/tests/rsx_metal4_dial_list.inc",
}

# Subtrees that exist only for the investigation.
DELETE_PREFIXES = (
    "docs/research/",
)

# Test sources removed above, also removed from CMakeLists.
DELETED_TEST_LINES = (
    "test_rsx_metal4_dials.cpp",
    "test_rsx_metal4_interpreter.cpp",
    "test_rsx_metal4_mapping.cpp",
    "test_rsx_metal4_state.cpp",
    "rsx_metal4_dial_list.inc",
)

# A release tree may not contain any of these.
FORBIDDEN = (
    ("diagnostic macro", re.compile(r"\bMTL_(?:NOTE_WITNESS|NOTE_DUMP|NOTE|WARN|ERROR|PERF_[A-Z_]+|PROBE_[A-Z_]+)\b")),
    ("diagnostic guard", re.compile(r"\bMTL_(?:DEBUG_LOGS|PERF_ENABLED|DIAGNOSTICS)\b")),
    ("instrumentation include", re.compile(r"MTLInstrumentation\.h")),
    ("probe facility", re.compile(r"\b(?:mtl::)?probe::")),
    ("excise marker", re.compile(r"TODO\(excise\)")),
    ("diagnostic symbol", re.compile(r"\b(?:diagnostic_[a-z0-9_]+|m_diag_[a-z0-9_]+|diag_[a-z0-9_]+)")),
    ("FIFO diagnostic", re.compile(r"\b(?:diag_(?:put|read|phase|jump|park|resume|nop|desync)[a-z0-9_]*|on_fifo_desync|fifo_desync[a-z0-9_]*|dispatch_history|dispatch_entry|note_dispatch|note_label_release)\b")),
    ("debug timestamp", re.compile(r"\bdebug_commit_time\b")),
)

SOURCE_SUFFIXES = (".cpp", ".h", ".hpp", ".mm", ".m", ".c", ".cc", ".inc", ".glsl", ".txt")



# Identifier prefixes that mean "this is part of the diagnostic facility".
DIAG_PREFIXES = (
    "diagnostic_", "m_diag_", "diag_", "probe", "census_", "tape_",
    "deferred_dump", "dump_tex_", "dump_draw_", "fifo_desync", "dispatch_entry",
    "dispatch_history", "note_dispatch", "note_label_release", "on_fifo_desync",
    "debug_commit_time",
)

# ------------------------------------------------------------------ utilities


def git(root: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(root), *args])


def tracked_files(root: Path) -> list[str]:
    raw = git(root, "ls-files", "-z")
    return [p for p in raw.decode("utf-8", "surrogateescape").split("\0") if p]


def read_text(path: Path) -> tuple[str, bool]:
    raw = path.read_bytes()
    crlf = b"\r\n" in raw
    text = raw.decode("utf-8", "surrogateescape").replace("\r\n", "\n").replace("\r", "\n")
    return text, crlf


def write_text(path: Path, text: str, crlf: bool) -> None:
    if crlf:
        text = text.replace("\n", "\r\n")
    path.write_bytes(text.encode("utf-8", "surrogateescape"))


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def line_start(text: str, pos: int) -> int:
    return text.rfind("\n", 0, pos) + 1


def line_end(text: str, pos: int) -> int:
    end = text.find("\n", pos)
    return len(text) if end < 0 else end


def skip_trivia(text: str, i: int) -> int:
    n = len(text)
    while i < n:
        if text[i] in " \t\r\n":
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            break
    return i


def skip_raw_string(text: str, i: int) -> int:
    """i points at R in R"delim( ... )delim"."""
    paren = text.find("(", i + 2)
    if paren < 0:
        return len(text)
    delim = text[i + 2:paren]
    close = ")" + delim + '"'
    end = text.find(close, paren + 1)
    return len(text) if end < 0 else end + len(close)


def skip_quoted(text: str, i: int) -> int:
    quote = text[i]
    j = i + 1
    n = len(text)
    while j < n:
        if text[j] == "\\":
            j += 2
            continue
        if text[j] == quote:
            return j + 1
        j += 1
    return n


def match_pair(text: str, i: int) -> int:
    """Return the index of the delimiter matching text[i], or -1."""
    opener = text[i]
    closer = {"(": ")", "[": "]", "{": "}"}.get(opener)
    if closer is None:
        return -1
    depth = 0
    j = i
    n = len(text)
    while j < n:
        if text.startswith("//", j):
            k = text.find("\n", j)
            j = n if k < 0 else k + 1
            continue
        if text.startswith("/*", j):
            k = text.find("*/", j + 2)
            j = n if k < 0 else k + 2
            continue
        c = text[j]
        if c == "R" and j + 1 < n and text[j + 1] == '"':
            j = skip_raw_string(text, j)
            continue
        if c == '"':
            j = skip_quoted(text, j)
            continue
        if c == "'":
            j = skip_quoted(text, j)
            continue
        if c == opener:
            depth += 1
        elif c == closer:
            depth -= 1
            if depth == 0:
                return j
        j += 1
    return -1


def statement_end(text: str, i: int) -> int:
    """End (exclusive) of the statement starting at i.  Handles (), [], {}."""
    head = skip_trivia(text, i)
    if head < len(text) and text[head] == "{":
        close = match_pair(text, head)
        if close >= 0:
            return close + 1
    paren = bracket = brace = 0
    j = i
    n = len(text)
    while j < n:
        if text.startswith("//", j):
            k = text.find("\n", j)
            j = n if k < 0 else k + 1
            continue
        if text.startswith("/*", j):
            k = text.find("*/", j + 2)
            j = n if k < 0 else k + 2
            continue
        c = text[j]
        if c == "R" and j + 1 < n and text[j + 1] == '"':
            j = skip_raw_string(text, j)
            continue
        if c in "\"'":
            j = skip_quoted(text, j)
            continue
        if c == "(":
            paren += 1
        elif c == ")":
            paren = max(0, paren - 1)
        elif c == "[":
            bracket += 1
        elif c == "]":
            bracket = max(0, bracket - 1)
        elif c == "{":
            brace += 1
        elif c == "}":
            if brace == 0:
                return j
            brace -= 1
        elif c == ";" and paren == 0 and bracket == 0 and brace == 0:
            return j + 1
        j += 1
    return n


def next_ctrl_body(text: str, i: int) -> int:
    i = skip_trivia(text, i)
    if i >= len(text):
        return i
    if text[i] == "{":
        close = match_pair(text, i)
        return len(text) if close < 0 else close + 1
    return statement_end(text, i)


@dataclass
class Unit:
    start: int
    end: int
    kind: str
    name: str = ""
    code: str = ""


def parse_unit(text: str, start: int) -> Unit | None:
    """Parse one syntactic unit starting at/after `start`."""
    i = skip_trivia(text, start)
    n = len(text)
    if i >= n:
        return None

    # Preprocessor directive.
    if text[i] == "#":
        j = i
        while True:
            k = text.find("\n", j)
            if k < 0:
                return Unit(i, n, "directive", code=text[i:n])
            if text[k - 1] != "\\":
                return Unit(i, k + 1, "directive", code=text[i:k])
            j = k + 1

    m = re.match(r"[A-Za-z_]\w*", text[i:])
    word = m.group(0) if m else ""

    if word in ("if", "for", "while", "switch"):
        paren = text.find("(", i)
        if paren < 0:
            return Unit(i, statement_end(text, i), "statement", code=text[i:statement_end(text, i)])
        close = match_pair(text, paren)
        if close < 0:
            return None
        end = next_ctrl_body(text, close + 1)
        if word == "if":
            while True:
                j = skip_trivia(text, end)
                if not text.startswith("else", j):
                    break
                k = skip_trivia(text, j + 4)
                if text.startswith("if", k):
                    # An else-if chain is parsed as a nested if, so its own
                    # chain terminator is respected.
                    nested = parse_unit(text, k)
                    if nested is None:
                        break
                    end = nested.end
                else:
                    end = next_ctrl_body(text, k)
        return Unit(i, end, "control", code=text[i:end])

    if word == "else":
        return Unit(i, next_ctrl_body(text, i + 4), "control", code=text[i:next_ctrl_body(text, i + 4)])

    if word == "do":
        end = next_ctrl_body(text, i + 2)
        j = skip_trivia(text, end)
        if text.startswith("while", j):
            end = statement_end(text, j)
        return Unit(i, end, "control", code=text[i:end])

    if word in ("struct", "class", "union", "enum", "namespace"):
        brace = text.find("{", i)
        semi = text.find(";", i)
        if brace < 0 or (0 <= semi < brace):
            end = statement_end(text, i)
            return Unit(i, end, "statement", code=text[i:end])
        close = match_pair(text, brace)
        if close < 0:
            return None
        end = close + 1
        j = skip_trivia(text, end)
        if j < n and text[j] == ";":
            end = j + 1
        nm = re.search(r"\b(?:struct|class|union|enum(?:\s+class)?|namespace)\s+([A-Za-z_]\w*)", text[i:brace])
        return Unit(i, end, "type", name=nm.group(1) if nm else "", code=text[i:end])

    if text[i] == "{":
        close = match_pair(text, i)
        if close < 0:
            return None
        end = close + 1
        j = skip_trivia(text, end)
        if j < n and text[j] == ";":
            end = j + 1
        return Unit(i, end, "block", code=text[i:end])

    # Function body or statement.  Find the first '{' or ';' at base depth.
    paren = bracket = 0
    j = i
    brace_at = -1
    semi_at = -1
    eq_at = -1
    while j < n:
        if text.startswith("//", j):
            k = text.find("\n", j)
            j = n if k < 0 else k + 1
            continue
        if text.startswith("/*", j):
            k = text.find("*/", j + 2)
            j = n if k < 0 else k + 2
            continue
        c = text[j]
        if c == "R" and j + 1 < n and text[j + 1] == '"':
            j = skip_raw_string(text, j)
            continue
        if c in "\"'":
            j = skip_quoted(text, j)
            continue
        if c == "(":
            paren += 1
        elif c == ")":
            paren = max(0, paren - 1)
        elif c == "[":
            bracket += 1
        elif c == "]":
            bracket = max(0, bracket - 1)
        elif c == "=" and paren == 0 and bracket == 0:
            eq_at = j
        elif c == "{" and paren == 0 and bracket == 0:
            brace_at = j
            break
        elif c == ";" and paren == 0 and bracket == 0:
            semi_at = j
            break
        j += 1

    if brace_at >= 0 and (eq_at < 0 or eq_at > brace_at):
        close = match_pair(text, brace_at)
        if close < 0:
            return None
        sig = text[i:brace_at]
        calls = re.findall(r"([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*\(", sig)
        if brace_at == i or not calls:
            # Braced initialisation (`T x{0};`) or a bare block.
            end = close + 1
            j = skip_trivia(text, end)
            if j < n and text[j] == ";":
                end = j + 1
            return Unit(i, end, "statement", code=text[i:end])
        name = calls[-1]
        return Unit(i, close + 1, "function", name=name, code=text[i:close + 1])

    end = semi_at + 1 if semi_at >= 0 else statement_end(text, i)
    return Unit(i, end, "statement", code=text[i:end])


def mask_noncode(text: str) -> str:
    """Replace comments/strings with spaces, preserving newlines and length."""
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
            continue
        c = text[i]
        if c == "R" and i + 1 < n and text[i + 1] == '"':
            j = skip_raw_string(text, i)
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
            continue
        if c in "\"'":
            j = skip_quoted(text, i)
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
            continue
        i += 1
    return "".join(out)


def iter_code_regex(text: str, pattern: re.Pattern[str]):
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            k = text.find("\n", i)
            i = n if k < 0 else k + 1
            continue
        if text.startswith("/*", i):
            k = text.find("*/", i + 2)
            i = n if k < 0 else k + 2
            continue
        c = text[i]
        if c == "R" and i + 1 < n and text[i + 1] == '"':
            i = skip_raw_string(text, i)
            continue
        if c in "\"'":
            i = skip_quoted(text, i)
            continue
        m = pattern.match(text, i)
        if m:
            yield m
            i = m.end()
        else:
            i += 1


DIAG_WORD_RE = re.compile(r"\b(?:diagnostic_|m_diag_|diag_|probe|census_|tape_|deferred_dump|dump_tex_|dump_draw_|m_label_probe)")

DIAG_MACRO_NAME_RE = re.compile(r"\b(?:MTL_(?:NOTE_WITNESS|NOTE_DUMP|NOTE|WARN|ERROR|PERF_[A-Z_]+|PROBE_[A-Z_]+))\b")


def identifiers(code: str) -> list[str]:
    return re.findall(r"\b[A-Za-z_]\w*\b", code)


def declared_locals(code: str) -> set[str]:
    names: set[str] = set()
    for m in re.finditer(
        r"(?:const\s+|static\s+|constexpr\s+)?(?:auto|bool|u8|u16|u32|u64|s32|s64|f32|f64|usz|std::\w+|[A-Z]\w*)\s+([A-Za-z_]\w*)\s*(?:[=({;])",
        code,
    ):
        names.add(m.group(1))
    for m in re.finditer(r"for\s*\(\s*(?:const\s+)?(?:auto|u32|u64|s32|s64|usz)\s+([A-Za-z_]\w*)", code):
        names.add(m.group(1))
    return names


def is_diagnostic_expr(code: str) -> bool:
    """True when the unit is exactly one diagnostic macro call (with or without
    the trailing semicolon).  Such a unit needs no further checking: the whole
    statement is the facility."""
    stripped = mask_noncode(code).strip().rstrip(";").strip()
    m = DIAG_MACRO_NAME_RE.match(stripped)
    if not m:
        return False
    rest = stripped[m.end():].lstrip()
    return rest.startswith("(")


def validate_code_unit(unit: Unit) -> str | None:
    """Return a reason string when a statement/control unit is not provably
    diagnostic-only, else None.

    The contract for a marked standalone unit: it may read production state but
    may not write it.  Writes are the only way a diagnostic deletes behaviour
    when removed."""
    if is_diagnostic_expr(unit.code):
        return None
    code = mask_noncode(unit.code)
    if re.search(r"\b(return|break|continue|goto)\b", code):
        return "control transfer inside diagnostic unit"
    locals_ = declared_locals(code)
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*(?:=(?!=)|(?:<<|>>|[+\-*/%&|^])=|\+\+|--)", code):
        target = m.group(1)
        if target in locals_ or DIAG_WORD_RE.match(target):
            continue
        return f"writes non-diagnostic variable '{target}'"
    return None


# ------------------------------------------------------------------ edit engine


@dataclass
class Edit:
    path: str
    start: int
    end: int
    rule: str
    preview: str
    line: int


@dataclass
class Review:
    path: str
    line: int
    reason: str
    preview: str


@dataclass
class Result:
    text: str
    edits: list[Edit] = field(default_factory=list)
    reviews: list[Review] = field(default_factory=list)


Span = tuple[int, int, str]


def apply_spans(text: str, spans: list[Span], path: str,
                edits: list[Edit]) -> str:
    spans = sorted(spans, key=lambda s: (s[0], s[1]))
    last = -1
    for start, end, rule in spans:
        if start < last:
            raise RuntimeError(f"overlapping excise spans in {path} at {line_of(text, start)}")
        last = end
        edits.append(Edit(path, start, end, rule, text[start:min(end, start + 120)].replace("\n", " "), line_of(text, start)))
    for start, end, _ in reversed(spans):
        text = text[:start] + text[end:]
    return text


def line_offsets(text: str) -> list[int]:
    lines = text.splitlines(True)
    offsets: list[int] = []
    pos = 0
    for line in lines:
        offsets.append(pos)
        pos += len(line)
    offsets.append(pos)
    return offsets


def merge_spans(text: str, path: str, spans: list[Span],
                reviews: list[Review]) -> list[Span]:
    """Sort spans and drop the ones that are contained in an earlier span.

    A partial overlap means two annotations disagree about a region, which is a
    tree bug: report it and keep the earlier span.  Dropping the later span is
    fail-closed, because the surviving tokens then trip the residual gate."""
    out: list[Span] = []
    for span in sorted(spans, key=lambda s: (s[0], -s[1])):
        if span[0] >= span[1]:
            continue
        if out and span[0] < out[-1][1]:
            if span[1] <= out[-1][1]:
                continue
            reviews.append(Review(
                path, line_of(text, span[0]), "overlapping annotations drop this deletion",
                text[span[0]:min(span[1], span[0] + 120)].replace("\n", " ")))
            continue
        out.append(span)
    return out


MARKER_RE = re.compile(r"TODO\(excise\)\s*:")
BARE_MARKER_RE = re.compile(r"TODO\(excise\)")
GUARD_RE = re.compile(r"\bMTL_(?:DEBUG_LOGS|PERF_ENABLED|DIAGNOSTICS)\b|\bMTL_PROBE\b")


def _comment_block_start(lines: list[str], idx: int) -> int:
    """If the comment block immediately above line `idx` carries a marker, its
    start, else `idx`."""
    start = idx
    while start > 0 and lines[start - 1].strip().startswith("//"):
        start -= 1
    if start < idx and any(BARE_MARKER_RE.search(lines[k]) for k in range(start, idx)):
        return start
    return idx


def collect_guard_spans(text: str, path: str,
                        reviews: list[Review]) -> list[Span]:
    lines = text.splitlines(True)
    offsets = line_offsets(text)
    spans: list[Span] = []
    idx = 0
    while idx < len(lines):
        line = lines[idx]
        if not re.match(r"\s*#\s*(?:if|ifdef|ifndef)\b", line) or not GUARD_RE.search(line):
            idx += 1
            continue
        depth = 1
        else_idx = None
        elif_seen = False
        j = idx + 1
        while j < len(lines):
            if re.match(r"\s*#\s*if(?:def|ndef)?\b", lines[j]):
                depth += 1
            elif re.match(r"\s*#\s*endif\b", lines[j]):
                depth -= 1
                if depth == 0:
                    break
            elif depth == 1 and re.match(r"\s*#\s*else\b", lines[j]):
                else_idx = j
            elif depth == 1 and re.match(r"\s*#\s*elif\b", lines[j]):
                elif_seen = True
            j += 1
        if j >= len(lines):
            reviews.append(Review(path, idx + 1, "unbalanced diagnostic guard", line.strip()[:120]))
            break
        if elif_seen:
            reviews.append(Review(path, idx + 1, "diagnostic guard has an #elif branch", line.strip()[:120]))
            idx = j + 1
            continue
        begin = _comment_block_start(lines, idx)
        if else_idx is None:
            spans.append((offsets[begin], offsets[j + 1], f"guard-region:#{idx + 1}"))
        else:
            # The #else body is production: keep it, drop the header and #endif.
            spans.append((offsets[begin], offsets[else_idx + 1], f"guard-if-else:#{idx + 1}"))
            spans.append((offsets[j], offsets[j + 1], f"guard-endif:#{j + 1}"))
        idx = j + 1
    return spans


def collect_marker_spans(text: str, path: str, guarded: list[Span],
                         reviews: list[Review]) -> list[Span]:
    lines = text.splitlines(True)
    offsets = line_offsets(text)
    spans: list[Span] = []
    for idx, line in enumerate(lines):
        if not MARKER_RE.search(line):
            continue
        start_of_line = offsets[idx]
        if any(s[0] <= start_of_line < s[1] for s in guarded + spans):
            continue

        code_before = line.split("//", 1)[0]
        inline = bool(code_before.strip())
        if inline:
            parse_from = start_of_line + (len(line) - len(line.lstrip()))
        else:
            parse_from = offsets[idx + 1] if idx + 1 < len(lines) else len(text)

        unit = parse_unit(text, parse_from)
        if unit is None:
            reviews.append(Review(path, idx + 1, "marker has no parseable unit", line.strip()[:120]))
            continue

        if unit.kind == "directive":
            # A guard or include owns this; the prose pass removes the marker if
            # the directive is not itself being deleted.
            continue

        if unit.kind in ("function", "type"):
            short = unit.name.split("::")[-1] if unit.name else ""
            whole = "whole-unit" in line.lower()
            if not short:
                reviews.append(Review(path, idx + 1, "marker on an unnamed definition", line.strip()[:120]))
                continue
            if not DIAG_WORD_RE.match(short) and not whole:
                reviews.append(Review(path, idx + 1,
                    f"definition '{short}' is not diagnostic-named; mark it whole-unit if it must go",
                    line.strip()[:120]))
                continue
        else:
            reason = validate_code_unit(unit)
            if reason:
                reviews.append(Review(path, idx + 1, reason, unit.code.strip().replace("\n", " ")[:160]))
                continue

        end = unit.end
        if inline:
            end = max(end, line_end(text, start_of_line))
        if end < len(text) and text[end] == "\n":
            end += 1
        else:
            eol = line_end(text, end)
            if eol < len(text):
                end = max(end, eol + 1)
        spans.append((start_of_line if not inline else start_of_line, end, f"marker:{unit.kind}"))
    return spans


MACRO_RE = re.compile(r"\b(MTL_(?:NOTE_WITNESS|NOTE_DUMP|NOTE|WARN|ERROR|PERF_[A-Z_]+|PROBE_[A-Z_]+))\s*\(")


def collect_macro_spans(text: str, path: str, covered: list[Span],
                        reviews: list[Review]) -> list[Span]:
    spans: list[Span] = []
    for m in list(iter_code_regex(text, MACRO_RE)):
        start = line_start(text, m.start())
        if any(s[0] <= m.start() < s[1] for s in covered):
            continue
        if text[start:m.start()].strip():
            reviews.append(Review(path, line_of(text, m.start()), "macro embedded in an expression", text[start:line_end(text, m.start())].strip()[:160]))
            continue
        end = statement_end(text, m.start())
        after = text[end:line_end(text, end)]
        if after.strip() and not after.strip().startswith("//"):
            reviews.append(Review(path, line_of(text, m.start()), "macro statement has trailing code", after.strip()[:120]))
            continue
        end = line_end(text, end)
        if end < len(text):
            end += 1
        spans.append((start, end, "macro"))
    return spans


def collect_include_spans(text: str, path: str) -> list[Span]:
    spans: list[Span] = []
    lines = text.splitlines(True)
    offsets = line_offsets(text)
    for m in re.finditer(r'(?m)^[ \t]*#\s*include\s*"MTLInstrumentation\.h"[ \t]*\n?', text):
        begin = _comment_block_start(lines, line_of(text, m.start()) - 1)
        spans.append((offsets[begin], m.end(), "include"))
    return spans


def collect_prose_marker_spans(text: str, path: str, covered: list[Span],
                               reviews: list[Review]) -> list[Span]:
    spans: list[Span] = []
    for m in re.finditer(r"(?m)^.*$", text):
        line = m.group(0)
        if not BARE_MARKER_RE.search(line):
            continue
        if any(s[0] <= m.start() < s[1] for s in covered):
            continue
        stripped = line.strip()
        if stripped.startswith("//"):
            spans.append((m.start(), m.end() + (1 if m.end() < len(text) else 0), "prose-marker"))
            continue
        comment_at = line.find("//")
        if comment_at >= 0:
            spans.append((m.start() + comment_at, m.end(), "prose-marker"))
            continue
        reviews.append(Review(path, line_of(text, m.start()), "marker inside a block comment", stripped[:120]))
    return spans


DECL_RE = re.compile(
    r"(?:const\s+|static\s+|constexpr\s+|volatile\s+)*"
    r"(?:auto|bool|u8|u16|u32|u64|s32|s64|f32|f64|usz|void|[A-Za-z_]\w*(?:::\w+)*)"
    r"\s*(?:[*&]\s*)*([A-Za-z_]\w*)\s*(?:[=({;:,\[])"
)
ASSIGN_RE = re.compile(
    r"(?:(?P<obj>[A-Za-z_]\w*)\s*(?:\.|->)\s*)?"
    r"(?P<name>[A-Za-z_]\w*)\s*(?:=(?!=)|<<=|>>=|[+\-*/%&|^]=|\+\+|--)")


def declared_locals(code: str) -> set[str]:
    names = set(DECL_RE.findall(code))
    for m in re.finditer(r"for\s*\(\s*(?:const\s+)?(?:auto|u32|u64|s32|s64|usz)\s+([A-Za-z_]\w*)", code):
        names.add(m.group(1))
    return names


def production_writes(code: str) -> list[str]:
    masked = mask_noncode(code)
    locals_ = declared_locals(masked)
    out: list[str] = []
    for m in ASSIGN_RE.finditer(masked):
        target = m.group("name")
        obj = m.group("obj")
        if obj is not None:
            if obj in locals_ or DIAG_WORD_RE.match(obj):
                continue
            if obj in ("this", "state", "info", "properties", "descriptor"):
                pass
        if target in locals_ or DIAG_WORD_RE.match(target):
            continue
        if target not in out:
            out.append(target)
    return out


def is_diag_call(callee: str) -> bool:
    parts = callee.split("::")
    if len(parts) > 1 and parts[0] in ("probe", "mtl"):
        return True
    return bool(DIAG_WORD_RE.match(parts[-1]))


def control_condition(code: str) -> str | None:
    paren = code.find("(")
    if paren < 0:
        return None
    close = match_pair(code, paren)
    if close < 0:
        return None
    return code[paren + 1:close]


def condition_is_diagnostic(cond: str) -> bool:
    masked = mask_noncode(cond)
    if production_writes(cond):
        return False
    called = re.findall(r"([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*\(", masked)
    if called:
        return all(is_diag_call(c) for c in called)
    idents = [i for i in identifiers(masked) if i not in KEYWORD_WORDS]
    return bool(idents) and all(DIAG_WORD_RE.match(i) for i in idents)


KEYWORD_WORDS = {
    "if", "else", "for", "while", "switch", "return", "const", "constexpr",
    "auto", "static", "true", "false", "nullptr", "and", "or", "not",
}


def is_diagnostic_expr(code: str) -> bool:
    stripped = mask_noncode(code).strip().rstrip(";").strip()
    m = DIAG_MACRO_NAME_RE.match(stripped)
    if not m:
        return False
    return stripped[m.end():].lstrip().startswith("(")


def validate_code_unit(unit: Unit) -> str | None:
    """Return a reason string when a statement/control unit is not provably
    diagnostic-only, else None.

    A marked standalone unit may read production state but may not write it.
    A control unit whose condition is diagnostic-only is an experiment: removing
    it restores production behaviour, so its body is removed with it."""
    if is_diagnostic_expr(unit.code):
        return None
    if unit.kind == "control":
        cond = control_condition(unit.code)
        if cond is not None and condition_is_diagnostic(cond):
            return None
    if unit.kind == "block":
        return None
    masked = mask_noncode(unit.code)
    if re.search(r"\b(return|break|continue|goto)\b", masked):
        return "control transfer inside diagnostic unit"
    writes = production_writes(unit.code)
    if writes:
        return f"writes non-diagnostic variable '{writes[0]}'"
    return None


def transform_source(path: str, text: str) -> Result:
    result = Result(text)
    reviews = result.reviews

    explicit = collect_explicit_ranges(text, path)
    guards = collect_guard_spans(text, path, reviews)
    markers = collect_marker_spans(text, path, explicit + guards, reviews)
    so_far = explicit + guards + markers
    macros = collect_macro_spans(text, path, so_far, reviews)
    so_far += macros
    includes = collect_include_spans(text, path)
    so_far += includes
    prose = collect_prose_marker_spans(text, path, so_far, reviews)

    spans = merge_spans(text, path, explicit + guards + markers + macros + includes + prose, reviews)
    text = apply_spans(text, spans, path, result.edits)
    # Cosmetic: a deletion can leave several blank lines; keep at most two.
    text = re.sub(r"\n{4,}", "\n\n\n", text)
    result.text = text
    return result


def collect_explicit_ranges(text: str, path: str) -> list[Span]:
    lines = text.splitlines(True)
    offsets = line_offsets(text)
    stack: list[tuple[str, int]] = []
    spans: list[Span] = []
    for idx, line in enumerate(lines):
        m = re.match(r"\s*//\s*EXCISE-BEGIN\s+(\S+)", line)
        if m:
            stack.append((m.group(1), idx))
            continue
        m = re.match(r"\s*//\s*EXCISE-END\s+(\S+)", line)
        if m:
            if not stack or stack[-1][0] != m.group(1):
                raise RuntimeError(f"unbalanced EXCISE marker {m.group(1)} at {path}:{idx + 1}")
            tag, begin = stack.pop()
            spans.append((offsets[begin], offsets[idx + 1], f"explicit:{tag}"))
    if stack:
        raise RuntimeError(f"unclosed EXCISE range {stack[-1][0]} in {path}:{stack[-1][1] + 1}")
    return spans



# ------------------------------------------------------------------ CMake and docs


def transform_cmake(text: str, path: str) -> Result:
    result = Result(text)
    spans: list[tuple[int, int, str]] = []
    offsets = []
    pos = 0
    for line in text.splitlines(True):
        offsets.append(pos)
        pos += len(line)
    offsets.append(pos)
    for idx, line in enumerate(text.splitlines(True)):
        if any(name in line for name in DELETED_TEST_LINES) or "RPCS3_SOURCE_DIR=${CMAKE_SOURCE_DIR}" in line:
            spans.append((offsets[idx], offsets[idx + 1], "cmake"))
    result.text = apply_spans(text, spans, path, result.edits)
    return result


# ------------------------------------------------------------------ lint


def lint_files(root: Path, files: list[str], texts: dict[str, str], deleted: set[str]) -> list[tuple[str, int, str, str]]:
    problems: list[tuple[str, int, str, str]] = []
    for rel in files:
        if rel in deleted:
            continue
        if rel.startswith("3rdparty/"):
            continue
        if not (rel.startswith("rpcs3/") or rel.startswith("docs/") or rel.startswith(".github/") or "/" not in rel):
            continue
        text = texts.get(rel)
        if text is None:
            p = root / rel
            if not p.exists():
                continue
            try:
                raw = p.read_bytes()
            except OSError:
                continue
            if b"\0" in raw:
                continue
            text = raw.decode("utf-8", "surrogateescape").replace("\r\n", "\n")
        for idx, line in enumerate(text.splitlines(), 1):
            for label, pattern in FORBIDDEN:
                if pattern.search(line):
                    problems.append((rel, idx, label, line.strip()[:160]))
    return problems


# ------------------------------------------------------------------ orphans

# A declaration whose only remaining mention is the declaration itself was
# excised *around* rather than *with*: the unit that used it went, and the
# variable, function or parameter stayed.  That is the one leftover the token
# gate cannot see, because no forbidden word survived.  It is reported, never
# deleted: the initialiser may call production code, and a parameter's name is
# part of a signature that call sites already match.

KEYWORDS = frozenset("""
    alignas alignof asm auto bool break case catch char class const consteval
    constexpr continue decltype default delete do double else enum explicit export
    extern false float for friend goto if inline int long mutable namespace new
    noexcept nullptr operator or private protected public register return short
    signed sizeof static static_assert static_cast struct switch template this throw
    true try typedef typeid typename union unsigned using virtual void volatile while
    xor RSX_ENABLE_METAL4 MTL4 MTL NULL
""".split())


# A surviving occurrence only counts when it can be a declaration of the name.
# System headers are not in the tree, so a call into AppKit/Foundation (whose
# declaration lives in a framework header) would otherwise look like a
# declaration whose user went away.
NON_DECLARATION_PREV = frozenset("""
    return if else while for do switch case throw new delete sizeof alignof
    typeid co_await co_return co_yield goto
""".split())


def looks_like_declaration(code: str, name: str) -> bool:
    for match in re.finditer(rf"\b{re.escape(name)}\b", code):
        after = code[match.end():].lstrip()
        if not after or after[0] not in "=;{[(,":
            continue
        before = code[:match.start()].rstrip()
        if before.endswith((".", "->", "::")):
            continue
        prev = re.search(r"([A-Za-z_]\w*)\s*[*&\s]*$", before)
        if prev and prev.group(1) in NON_DECLARATION_PREV:
            continue
        return True
    return False


def orphan_candidates(original: str, transformed: str) -> dict[str, int]:
    """Identifiers used by a removed line that survive exactly once."""
    removed = collections.Counter(original.splitlines())
    for line in transformed.splitlines():
        removed[line] -= 1
    excised = "".join(f"{line}\n" for line, count in removed.items() if count > 0)
    if not excised.strip():
        return {}

    live = collections.Counter(identifiers(mask_noncode(transformed)))
    names = {name for name in identifiers(mask_noncode(excised))
             if name and name not in KEYWORDS and not name.isupper() and live.get(name) == 1}
    if not names:
        return {}

    located: dict[str, int] = {}
    for number, line in enumerate(transformed.splitlines(), 1):
        if not names - located.keys():
            break
        code = mask_noncode(line)
        for name in names - located.keys():
            if looks_like_declaration(code, name):
                located[name] = number
    return located


def find_orphans(original: dict[str, str], transformed: dict[str, str],
                 deleted: set[str]) -> list[tuple[str, int, str, str]]:
    """Orphans that are dead in the whole tree, not just in their own file.

    ``original`` is the tree before excision, ``transformed`` the tree after.  A
    name is only reported when exactly one occurrence is left anywhere, so a
    declaration in a header plus a surviving use elsewhere is never a hit.
    """
    global_counts: collections.Counter[str] = collections.Counter()
    for rel, text in transformed.items():
        if rel not in deleted:
            global_counts.update(identifiers(mask_noncode(text)))

    orphans: list[tuple[str, int, str, str]] = []
    for rel, before in original.items():
        if rel in deleted or rel not in transformed:
            continue
        lines = transformed[rel].splitlines()
        for name, number in orphan_candidates(before, transformed[rel]).items():
            if global_counts.get(name, 0) != 1:
                continue
            preview = lines[number - 1].strip()[:160] if 0 < number <= len(lines) else ""
            orphans.append((rel, number, f"orphaned after excision ({name})", preview))
    return orphans


# --------------------------------------------------------- empty bodies

# An excision that removed every statement of an `if` body leaves the branch
# behind with nothing to do.  That is not a compile error (`if (cond) { }` is
# legal, and the condition may hold a call that must still run), so it is
# reported: the author removes the branch or restructures it so the surviving
# call is not hidden inside an empty test.
EMPTY_BODY_RE = re.compile(r"\b(?:if|for|while|else)\b[^{};]*\{\s*\}")


def empty_control_bodies(text: str) -> list[tuple[int, str]]:
    masked = mask_noncode(text)
    lines = text.splitlines()
    found: list[tuple[int, str]] = []

    for match in EMPTY_BODY_RE.finditer(masked):
        number = masked.count("\n", 0, match.start()) + 1
        preview = lines[number - 1].strip()[:160] if number <= len(lines) else ""
        found.append((number, preview))

    return found


def find_empty_bodies(original: dict[str, str], transformed: dict[str, str],
                      deleted: set[str]) -> list[tuple[str, int, str, str]]:
    """An empty control body the excision created, per file."""
    leftovers: list[tuple[str, int, str, str]] = []

    for rel, text in transformed.items():
        if rel in deleted or rel not in original:
            continue

        # The branch line is the key; a file that keeps an empty `if` it always
        # had must not report it, and counting by key keeps the order stable.
        before = collections.Counter(" ".join(preview.split())
                                     for _, preview in empty_control_bodies(original[rel]))
        seen: collections.Counter[str] = collections.Counter()

        for number, preview in empty_control_bodies(text):
            key = " ".join(preview.split())
            seen[key] += 1

            if seen[key] > before.get(key, 0):
                leftovers.append((rel, number, "empty control body after excision", preview))

    return leftovers


# ------------------------------------------------------------------ driver


def should_delete(rel: str) -> bool:
    if rel in DELETE_FILES:
        return True
    return any(rel.startswith(prefix) for prefix in DELETE_PREFIXES)


def build_plan(root: Path) -> tuple[dict[str, str], dict[str, str], set[str], list[Edit], list[Review]]:
    files = tracked_files(root)
    original: dict[str, str] = {}
    for rel in files:
        p = root / rel
        if not p.exists():
            continue
        try:
            raw = p.read_bytes()
        except OSError:
            continue
        if b"\0" in raw:
            continue
        original[rel] = raw.decode("utf-8", "surrogateescape").replace("\r\n", "\n").replace("\r", "\n")

    transformed: dict[str, str] = dict(original)
    deleted: set[str] = set()
    edits: list[Edit] = []
    reviews: list[Review] = []

    for rel in files:
        if should_delete(rel):
            deleted.add(rel)
            continue
        if rel not in original:
            continue
        if rel == "rpcs3/CMakeLists.txt":
            r = transform_cmake(original[rel], rel)
        elif rel.endswith(SOURCE_SUFFIXES) or rel.endswith(".inc"):
            r = transform_source(rel, original[rel])
        else:
            continue
        transformed[rel] = r.text
        edits.extend(r.edits)
        reviews.extend(r.reviews)

    return original, transformed, deleted, edits, reviews


def print_summary(edit_count: int, deleted: set[str], reviews: list[Review], problems,
                  orphans: list[tuple[str, int, str, str]] | None = None,
                  dead: list[tuple[str, int, str, str]] | None = None,
                  max_lines: int = 40) -> None:
    print(f"files deleted:  {len(deleted)}")
    print(f"units excised:  {edit_count}")
    print(f"needs review:   {len(reviews)}")
    print(f"residuals:      {len(problems)}")
    print(f"orphans:        {len(orphans or [])}")
    print(f"empty bodies:   {len(dead or [])}")
    if reviews:
        print("\nNEEDS REVIEW (not removed; add a marker/range or fix the code):")
        for r in reviews[:max_lines]:
            print(f"  {r.path}:{r.line}: {r.reason}: {r.preview}")
        if len(reviews) > max_lines:
            print(f"  ... {len(reviews) - max_lines} more")
    if problems:
        print("\nRESIDUALS (release gate would fail):")
        for path, line, label, preview in problems[:max_lines]:
            print(f"  {path}:{line}: {label}: {preview}")
        if len(problems) > max_lines:
            print(f"  ... {len(problems) - max_lines} more")
    if orphans:
        print("\nORPHANS (a declaration whose only use was excised; mark it, or give a")
        print("parameter [[maybe_unused]], so the release build does not warn):")
        for path, line, label, preview in orphans[:max_lines]:
            print(f"  {path}:{line}: {label}: {preview}")
        if len(orphans) > max_lines:
            print(f"  ... {len(orphans) - max_lines} more")
    if dead:
        print("\nEMPTY BODIES (a branch the excision emptied; remove it, or keep the")
        print("surviving call outside an empty test):")
        for path, line, label, preview in dead[:max_lines]:
            print(f"  {path}:{line}: {label}: {preview}")
        if len(dead) > max_lines:
            print(f"  ... {len(dead) - max_lines} more")


def cmd_check(root: Path, json_out: str | None, allow_orphans: bool = False) -> int:
    original, transformed, deleted, edits, reviews = build_plan(root)
    problems = lint_files(root, tracked_files(root), transformed, deleted)
    orphans = find_orphans(original, transformed, deleted)
    dead = find_empty_bodies(original, transformed, deleted)
    print_summary(len(edits), deleted, reviews, problems, orphans, dead)
    if json_out:
        payload = {
            "root": str(root),
            "deleted": sorted(deleted),
            "edits": [e.__dict__ for e in edits],
            "reviews": [r.__dict__ for r in reviews],
            "residuals": [
                {"path": p, "line": l, "label": lab, "preview": s} for p, l, lab, s in problems
            ],
            "orphans": [
                {"path": p, "line": l, "label": lab, "preview": s} for p, l, lab, s in orphans
            ],
            "empty_bodies": [
                {"path": p, "line": l, "label": lab, "preview": s} for p, l, lab, s in dead
            ],
        }
        Path(json_out).write_text(json.dumps(payload, indent=2))
        print(f"\nmanifest: {json_out}")
    leftovers = orphans or dead
    return 1 if (reviews or problems or (leftovers and not allow_orphans)) else 0


def cmd_apply(root: Path, force: bool, json_out: str | None, allow_orphans: bool = False) -> int:
    if not force:
        dirty = git(root, "status", "--porcelain", "--untracked-files=no").decode().strip()
        if dirty:
            print("error: tracked tree is dirty; commit/stash first or pass --force", file=sys.stderr)
            return 2

    original, transformed, deleted, edits, reviews = build_plan(root)
    problems = lint_files(root, tracked_files(root), transformed, deleted)
    orphans = find_orphans(original, transformed, deleted)
    dead = find_empty_bodies(original, transformed, deleted)
    leftovers = orphans or dead
    if reviews or problems or (leftovers and not allow_orphans):
        print_summary(len(edits), deleted, reviews, problems, orphans, dead)
        if reviews or problems:
            print("\nrefusing to apply: resolve NEEDS REVIEW and RESIDUALS first", file=sys.stderr)
        else:
            print("\nrefusing to apply: fix the orphaned declarations and the emptied\n"
                  "branches (or pass --allow-orphans if every listed line is reviewed)",
                  file=sys.stderr)
        return 1

    # Preserve line endings per file.
    for rel, text in transformed.items():
        p = root / rel
        if not p.exists():
            continue
        _, crlf = read_text(p)
        old, _ = read_text(p)
        if old == text:
            continue
        write_text(p, text, crlf)

    for rel in sorted(deleted):
        p = root / rel
        if p.exists():
            p.unlink()

    # Remove empty dev-doc directories, but never touch untracked files.
    for prefix in DELETE_PREFIXES:
        d = root / prefix.rstrip("/")
        if d.exists():
            for sub in sorted(d.rglob("*"), reverse=True):
                if sub.is_dir():
                    try:
                        sub.rmdir()
                    except OSError:
                        pass
            try:
                d.rmdir()
            except OSError:
                pass

    print_summary(len(edits), deleted, reviews, problems, orphans, dead)
    if json_out:
        payload = {
            "root": str(root),
            "deleted": sorted(deleted),
            "edits": [e.__dict__ for e in edits],
            "reviews": [],
            "residuals": [],
            "orphans": [
                {"path": p, "line": l, "label": lab, "preview": s} for p, l, lab, s in orphans
            ],
            "empty_bodies": [
                {"path": p, "line": l, "label": lab, "preview": s} for p, l, lab, s in dead
            ],
        }
        Path(json_out).write_text(json.dumps(payload, indent=2))
        print(f"manifest: {json_out}")
    print("\napplied.  review `git diff`, then build and run the tests.")
    return 0


def cmd_verify(root: Path, build_cmd: str | None) -> int:
    files = tracked_files(root)
    deleted = {rel for rel in files if should_delete(rel)}
    problems = lint_files(root, files, {}, set())
    missing = [rel for rel in DELETE_FILES if (root / rel).exists() and rel not in deleted]
    bad = 0
    if problems:
        print(f"residuals: {len(problems)}")
        for path, line, label, preview in problems[:80]:
            print(f"  {path}:{line}: {label}: {preview}")
        bad = 1
    else:
        print("residuals: 0")
    if missing:
        print(f"deletion list not honoured: {missing}")
        bad = 1
    leftover_docs = [rel for rel in files
                     if rel.startswith("docs/research/") and (root / rel).exists()]
    if leftover_docs:
        print(f"docs/research/ still contains tracked files: {leftover_docs[:5]}")
        bad = 1
    for rel in DELETED_TEST_LINES:
        if list((root / "rpcs3/tests").glob(rel)):
            print(f"deleted test still present: {rel}")
            bad = 1
    check = subprocess.run(["git", "-C", str(root), "diff", "--check"], capture_output=True, text=True)
    if check.returncode:
        print(check.stdout or check.stderr)
        bad = 1
    if build_cmd:
        print(f"build: {build_cmd}")
        rc = subprocess.run(build_cmd, shell=True, cwd=root).returncode
        if rc:
            print(f"build failed: {rc}")
            bad = 1
    return bad


# ------------------------------------------------------------------ self-test


def selftest() -> int:
    failures = 0

    def check(name: str, got: str, want: str) -> None:
        nonlocal failures
        if got != want:
            failures += 1
            print(f"FAIL {name}\n--- got ---\n{got!r}\n--- want ---\n{want!r}")

    src = """int a;
// TODO(excise): level 1; a statement
MTL_NOTE(1, "x %s", q);
int b;
"""
    r = transform_source("t.cpp", src)
    check("marker macro", r.text, "int a;\nint b;\n")

    src = """// TODO(excise): level 1; multi-line call
MTL_PERF_SCOPE(1,
    "name(" /* ) */,
    value);
int b;
"""
    r = transform_source("t.cpp", src)
    check("nested macro", r.text, "int b;\n")

    src = """// TODO(excise): level 1; if chain
if (probe::variant("poison"))
{
    diagnostic_fill = 1;
}
else if (probe::at_least(2))
{
    log();
}
int b;
"""
    r = transform_source("t.cpp", src)
    check("if chain", r.text, "int b;\n")

    src = """// TODO(excise): level 1; a diagnostic experiment may change production state
if (diagnostic_force_lod0())
{
    state.min_lod = 0.0f;
}
int b;
"""
    r = transform_source("t.cpp", src)
    check("experiment", r.text, "int b;\n")

    src = """// TODO(excise): level 1; must not eat production state
if (ready)
{
    fill = 1;
}
int b;
"""
    r = transform_source("t.cpp", src)
    check("production write kept", r.text, src.split("\n", 1)[1])
    check("production write reviewed", str(len(r.reviews)), "1")

    src = """// TODO(excise): level 1; a definition with a production name
void attach_layer(int x)
{
    diagnostic_note(x);
}
int b;
"""
    r = transform_source("t.cpp", src)
    check("production definition kept", r.text, src.split("\n", 1)[1])
    check("production definition reviewed", str(len(r.reviews)), "1")

    src = """// TODO(excise): level 1; function
void diagnostic_dump(int x)
{
    if (x) { MTL_NOTE(1, "x"); }
}
int b;
"""
    r = transform_source("t.cpp", src)
    check("function", r.text, "int b;\n")

    src = """#if MTL_DEBUG_LOGS
MTL_NOTE(1, "x");
#else
rsx_log.notice("prod");
#endif
int b;
"""
    r = transform_source("t.cpp", src)
    check("guard else", r.text, "rsx_log.notice(\"prod\");\nint b;\n")

    src = """const char* s = "MTL_NOTE(1, \\"not a call\\")";
// MTL_NOTE(1, "comment");
int b;
"""
    r = transform_source("t.cpp", src)
    check("strings/comments", r.text, src)

    src = """// EXCISE-BEGIN probe
a();
b();
// EXCISE-END probe
int b;
"""
    r = transform_source("t.cpp", src)
    check("explicit range", r.text, "int b;\n")

    # Orphans: a declaration that survives with its last use excised.
    check(
        "orphan found",
        repr(orphan_candidates(
            'static int diag_count = 0;\nMTL_NOTE(1, "%d", diag_count);\nint b;\n',
            'static int diag_count = 0;\nint b;\n',
        )),
        "{'diag_count': 1}",
    )
    # A name that the excision removed entirely is not an orphan.
    check(
        "orphan gone",
        repr(orphan_candidates(
            'int b;\nMTL_NOTE(1, "x", gone_value);\n',
            'int b;\n',
        )),
        "{}",
    )
    # A name used more than once is not orphaned either.
    check(
        "orphan still used",
        repr(orphan_candidates(
            'int n = 0;\nMTL_NOTE(1, "%d", n);\nreturn n;\n',
            'int n = 0;\nreturn n;\n',
        )),
        "{}",
    )
    # An excision that removes a branch's whole body leaves an empty test.
    check("empty body found",
          repr(empty_control_bodies("if (x)\n{\n}\nint b = {};\n")),
          "[(1, 'if (x)')]")
    check("empty body not created",
          repr(empty_control_bodies("if (x)\n{\n    a();\n}\nint b = {};\n")),
          "[]")

    # A surviving use of an externally declared symbol is not an orphan.
    check(
        "orphan external",
        repr(orphan_candidates(
            'bool visible(NSView* v) { return !v.hiddenOrHasHiddenAncestor; }\n'
            'MTL_NOTE(1, "%d", v.hiddenOrHasHiddenAncestor);\n',
            'bool visible(NSView* v) { return !v.hiddenOrHasHiddenAncestor; }\n',
        )),
        "{}",
    )


    if failures:
        print(f"{failures} self-test failure(s)")
        return 1
    print("selftest: ok")
    return 0


# ------------------------------------------------------------------ main


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", default="check", choices=("check", "apply", "verify", "selftest"))
    ap.add_argument("--root", default=".")
    ap.add_argument("--json", default=None)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--allow-orphans", action="store_true",
                    help="do not fail on reported orphans (review the list first)")
    ap.add_argument("--build-cmd", default=None)
    args = ap.parse_args()

    if args.command == "selftest":
        return selftest()

    root = Path(args.root).resolve()
    if not (root / "rpcs3").exists():
        print(f"error: {root} does not look like an rpcs3 checkout", file=sys.stderr)
        return 2

    if args.command == "check":
        return cmd_check(root, args.json, args.allow_orphans)
    if args.command == "apply":
        return cmd_apply(root, args.force, args.json, args.allow_orphans)
    return cmd_verify(root, args.build_cmd)


if __name__ == "__main__":
    raise SystemExit(main())
