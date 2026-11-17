# Handoff — Demon's Souls world literal (2026-11-16, updated 2026-11-17)

For whoever picks this up next, including me tomorrow.  Read §3 and §8 first if
you only have two minutes: the byte-order mechanism is now measured, but the
producer of the bytes is still unidentified and the causal link to the dark
world is still unproven.

> **Update 2026-11-17 — adversarial review.**  The W2 runs (ds15–ds18) were
> taken.  They resolve the §7 measurement in favour of **H-guest**: the copy is
> byte-faithful (`src == after`), so the reversed word is already in the staging
> buffer.  Two claims in the 11-16 version are now known to be wrong or
> overstated, and are corrected below: (a) this cannot be "Metal-only", because
> the fill and the staging path are shared with VK/GL; (b) the W4 oracle is
> partial and was never observed to light the world.  The new §3.6 and §8.4
> say what the log actually proves.  The transient "previous screen" ghost the
> user saw is recorded in §2.1 and is a separate artefact from the dark world.

---

## 1. TL;DR

* Demon's Souls (BLUS30443) renders the 3D world black/flat on the Metal 4
  backend.  Geometry, depth and HUD are present; the shading is near black
  (screenshots in §2.1).
* The strongest candidate remains one 4-byte value in the trailing
  inline-literal blocks of the five "world" fragment programs.  The animated
  lanes arrive at the shader as a transposed word: `T(M)` is junk
  (`0.000155`, `-232225`, `1.38e14`, …), while `bswap32(T(M))` is a smooth
  exposure-style ramp (`1.48148 → 1.81783`).
* **Measured (ds18):** the `nv308a` copy is byte-faithful.  The staging bytes
  are already transposed before the blit.  So the defect is **upstream of the
  RSX writers**, in whatever produces the staging block, or the literals are
  not the cause of the darkness.
* **Not Metal-only.**  `write_fragment_constants_to_buffer` and the staging
  path are backend-neutral.  If the guest bytes are transposed, VK/GL must be
  equally dark.  Do not present this as a Metal backend defect.
* **Still unproven:** that `fp-69` reads `fc[67]`/`fc[68]` into the colour
  output, and that correcting the word lights the world.  The oracle fired, but
  only on the `|v| > 1e6` frames and never with a verified visual result.
* Next action: identify the producer of the staging block (§8, open question 1)
  and dump the MSL/disassembly of `fp-69` to show the constant's use.  Do not
  fix the literal layer until one of those two answers is in hand.

---

## 2. Context you need

### 2.1 The visible symptom (user report + screenshots)

Screenshots are in
`~/Library/Application Support/rpcs3/screenshots/BLUS30443/`:

| file | what it shows |
|---|---|
| `BLUS30443_screenshot_2026_11_15_09_25_43.png` | a **correctly lit** world (earlier session) |
| `..._2026_11_16_18_18_56.png`, `..._18_18_58.png` | dark/flat world, HUD and character faintly visible |
| `..._2026_11_16_18_19_02.png` | dark/flat world, title/loading overlay visible |
| `..._2026_11_16_18_19_20.png`, `..._18_19_23.png` | dark/flat world, geometry visible but unlit |

The user's description (2026-11-17):

* the world is **dark/flat**, but geometry is faintly present — not a missing
  draw, not a blank framebuffer;
* during a **flicker**, the *previous Demon's Souls screen* (title/loading)
  appeared briefly in the world;
* the wrong image was present only for a short time during the flicker.

Interpretation: the persistent defect is a **shading/constant** failure — the
world is rasterised and depth-tested, then multiplied by something near zero.
The brief appearance of a previous screen is a **stale surface** (uncleared or
reused render target) and is probably a *separate* defect; it is not explained
by the literal and should be tracked independently.  Do not let the ghost
distract from the constant.

This is the `metal4` line of work: a native Metal 4 backend for RPCS3, held to
RSX behaviour as the spec.  A parity audit (`METAL4-VK-GL-PARITY.md`) is
complete through D1–D16.

Do not run the Vulkan or OpenGL backend as a differential test.  That was
explicitly ruled out; the RSX behaviour, not another backend, is the reference.
That does **not** make the defect Metal-specific: the logical consequence of a
shared fill and a faithful copy still has to be resolved.

---

## 3. What is proven, and with what

### 3.1 The fill transform

`write_fragment_constants_to_buffer_sse2` (`ProgramStateCache.cpp`) does

```cpp
shuffled = _mm_or_si128(_mm_slli_epi16(vector, 8), _mm_srli_epi16(vector, 8));
```

which is a byte swap **within each 16-bit halfword**:

```
T(w) = ((w & 0x00ff00ff) << 8) | ((w & 0xff00ff00) >> 8)
```

(The 11-16 version of this note claimed the formula was mis-stated.  It is
correct: `0x803f0000 → 0x3f800000 = 1.0`.)  The ARM fallback is the same
function, so this is backend- and arch-neutral.  This is the correct convention
for RSX ucode: a 32-bit word is stored as two 16-bit halves and the fetch swaps
each half.

### 3.2 Authored words are correct

From every `fill.log` line, the same quad:

| image word (host u32) | `T(word)` | value |
|---|---|---|
| `00400000` | `40000000` | 2.0 |
| `803f0000` | `3f800000` | 1.0 |
| `993e9999` | `3e999999` | 0.3 |

So `T` is right, the image layout is right, and the `.y`/`.w` lanes of the very
quad under suspicion are right.

### 3.3 The patched lanes are transposed

World program `fp-69` (`len 3312`, quad at `0xcc0`), live images over eight
frames (`ds18/watch.log`):

```
[W, 2.0, W, 1.0] with W = 8da73fbd, 22393fc3, 62c83fc8, 54453fcd,
                       fb563fd1, 72d93fe5, aecd3fe8, ...
```

For every W word:

| | value |
|---|---|
| `T(W)` | `-3.93e-15`, `0.000155`, `-232225`, `3.4e3`, `1.38e14`, … (junk) |
| `bswap32(T(W))` | `1.48148`, `1.52448`, `1.56551`, `1.60413`, `1.64048`, `1.7656`, `1.79257`, `1.81783` |

The `bswap32(T(W))` column is smooth and monotone across frames.  That is an
exposure/scale ramp, not noise, and it is the strongest single piece of
evidence for the transposed interpretation.

Two honest caveats:

1. **Range alone does not uniquely identify the permutation.**  Re-running the
   permutation search over the captured words, six byte permutations put every
   value in a plausible float range; the tightest is the full reversal
   `(2,3,0,1)`.
2. The smooth ramp is a statement about the *value*, not about *who* produced
   it.  The W2 log now answers the "who copied it" half (§3.6) but not the
   "who wrote it" half.

### 3.4 The patch is a memory write, before analysis

`ds11/prefetch.log`:

```
prefetch fp reg 014a3c81 loc 0 base off 21642368 data off 21642368 len 3312
  tail ... a1273fbd 00400000 a1273fbd 803f0000 00ba810e ...
```

`loc 0` is `CELL_GCM_LOCATION_LOCAL`, so the world programs live in RSX local
memory, and the image already holds the bad bytes when it is analysed.  The
`fill.log` for the same program prints the same words, so nothing between
prefetch and fill alters them.

### 3.5 The mechanism, stated precisely

Let `X` be the authored float, `W = bits(X)`, and `T` the fill transform.

* what the shader must receive: `W`
* what the shader receives: `T(M)`, where `M` is the word in memory
* measured: `T(M) = bswap32(W)`
* therefore: `M = bswap32(T(W))`

In words: **the four bytes at the patched lane are in reverse order relative to
the byte sequence the program image expects.**  The authored lanes have
`M = T(W)`; the patched lanes have `M = bswap32(T(W))`.

Equivalently, in the T layout the two 16-bit halves of the value are in the
wrong order.  This matters for reading the logs: `2.0` and `1.0` have a zero
low half, so a half transposition is **invisible** on them.  An earlier
argument that "mixed encodings in one 16-byte packet prove a copy-order bug is
impossible" is therefore wrong: the mixture is exactly what a uniform half
transposition produces for lanes whose low half is zero.

### 3.6 The copy is faithful (ds18) — the new result

`ds18/watch.log` has 234 reports, all `nv308a`, including 8 on
`program=21642368+3312` (the world program).  In every report:

```
before=8da73fbd 00400000 8da73fbd 803f0000
after =22393fc3 00400000 22393fc3 803f0000
src   =22393fc3 00400000 22393fc3 803f0000
```

`src == after`, byte for byte.  The blit copies what it is given; the staging
buffer already contains the transposed word.  The §7 measurement therefore
selects **H-guest**: the reversal is upstream of the RSX writers.

This also means the `nv308a`/`nv3089` invalidation-range work did not fix this
defect (it was still worth fixing; see §9).  The write-ring proves only that
the FIFO words were already `22393fc3`/`62c83fc8`/`54453fcd`/`fb563fd1`/…, not
that any backend reordered them.

### 3.7 The producer is lane-local

Within the same 16-byte block, the animated lanes are transposed and the static
lanes are not (`2.0`, `1.0`; a nearby quad's `0.3` is normal under `T`).  So
this is **not** a whole-image convention difference and **not** a per-copy byte
swap: it is whatever writes the animated lanes into the staging ring.  That is
the signature to chase.

---

## 4. The mechanism, re-derived (kept for the algebra)

`T` is an involution.  `bswap32` is a four-byte reversal.  `bswap32 ∘ T` is a
swap of the two 16-bit halves:

```
bswap32(T(0x3fbd8da7)) = 0x8da73fbd
```

so the observed image word `0x8da73fbd` is `bswap32(T(W))` for `W = 1.48148`.
The log's `rev=` column is `f32(bswap32(T(after)))`, which is exactly `W`; the
`read=` column is `f32(T(after))`, which is what the shader actually sees.  Do
not confuse the two columns.

---

## 5. Code map — where the number lives

| Stage | Site |
|---|---|
| guest writes ucode + patches | RSX command stream (`NV308A`, `NV0039`, `NV3089`) |
| program analysed, image latched | `graph_engine::prefetch_fragment_program`, `RSXGraphEngine.cpp` (~`:990`) |
| image → shader array | `write_fragment_constants_to_buffer*`, `ProgramStateCache.cpp` (~`:638`) |
| shader reads `fc[]` | `RSXFragmentProgram`, MSL from SPIRV-Cross |
| invalidation on write | `graph_engine::invalidate_fragment_program`, `RSXGraphEngine.cpp` (~`:1508`) |

Writers, and only these three write guest-visible bytes
(`grep -n "vm::_ptr<u8>" rpcs3/Emu/RSX/NV47/HW`):

| Writer | Entry | Destination |
|---|---|---|
| `nv308a::color::impl` | FIFO inline data → local memory | `dst_offset + x*4 + out_pitch*y` (`dst_offset + x*2 + ...` for r5g6b5) |
| `nv0039::buffer_notify` | 2D copy with pitch/stride | `dst_offset`, span `write_length` |
| `nv3089::image_in` | "image from CPU", incl. scaled and swizzled | `dst_offset + out_offset` |

The market now says: none of the three is producing the transposition.  They
are consumers of an already-transposed staging buffer.

---

## 6. Why the previous run could not answer it

`MTL_PROBE_WRITE_LOG` is emitted from `invalidate_fragment_program`.  Three
properties make the world patch structurally invisible, all readable in the
code:

1. **Wrong address.**  `nv308a` called
   `invalidate_fragment_program(dst_dma, dst_offset, count * 4, ...)` with the
   blit's *destination base*, but writes at
   `dst_offset + x*4 + out_pitch*y`.  This is fixed in the tree (§9), but it is
   not what produced the reversal.
2. **Wrong program.**  It compares only against
   `m_ctx->program.fragment_program`, the currently bound program.
3. **Wrong time.**  For `nv0039` and `nv3089` the hook ran *before* the copy.

The new W1/W2 instrument fixed all three.  It is what produced the ds18 result:
the copy is faithful.

---

## 7. The two surviving hypotheses

**H-rpcs3 — a writer reorders.**  The guest hands the copy the bytes in the
layout hardware wants; RPCS3 stores them in the wrong order.

**H-guest — the source is already reversed.**  The guest's staging bytes are
already `bswap32(T(W))` and RPCS3 copies faithfully.

**Result (ds18): H-guest.**  `src == after` in all 234 reports.  The remaining
question is *which* producer wrote the staging block, and whether the shader
actually consumes it (§8).

---

## 8. Instruments in the tree

All default-off, `EXCISE`-wrapped, bounded, none load-bearing.

### 8.1 W1/W2 — `rsx::program_write_watch`

New header `rpcs3/Emu/RSX/Core/RSXProgramWriteWatch.h`.

* `claim(addr, offset, len, image)` at every fragment prefetch records the image
  with a **384-byte shadow** of its tail.  Up to **64** entries, round-robin
  eviction.
* Keyed on the **host pointer the copy wrote through**.
* `note(writer, addr, size, src, src_len, detail)` is called **after** the copy
  by all three writers, and by `nv3089-gpu` for the HW-accelerated route.
* `watch drift` when the shadow changes without a writer claim.
* Report shape:

```
watch nv308a writer=nv308a program=<off>+<len> at=<abs> waddr=<host> wsize=<n> src=<host> x=.. y=.. pitch=.. fmt=.. count=..
  before=........ ........ ........ ........ after=........ ........ ........ ........ src=........ ........ ........ ........
  read=<4 floats T(after)> rev=<4 floats bswap32(T(after))>
```

### 8.2 W3 — FIFO cursor on the fill

```
fill fp off <o> len <l> hash <h> count <c> sanitize <s> fifo <pos> fix <n> | [..] @.. .... -> ....
```

That orders a patch against a bind.

### 8.3 W4 — functional oracle (partial — read §8.4)

`RSX_FP_LITERAL_FIX=bswap32` (or `halves`), logged to `RSX_FP_LITERAL_FIX_LOG`.

In `write_fragment_constants_to_buffer`, after the fill, for the **last two
quads** of an image with `ucode_length >= 3000`: a lane whose value is not a
plausible float (`!isfinite` or `|v| > 1e6`) but whose `bswap32` is plausible is
replaced by the reversal.

The predicate is the diagnosis.  `2.0` and `1.0` are plausible and are never
touched.  W4 is a probe, not the fix.

### 8.4 What the oracle did *not* prove

The predicate `|v| > 1e6` is too narrow.  The animated lanes alternate between
tiny values (`0.000155`, `2.9e-14`) and huge values (`-232225`, `1.38e14`), and
only the huge ones are "broken" by the oracle's own test.  Three of the four
consecutive frames above were never corrected.  Consequently:

* the oracle firing proves the *reversal* on the frames it touched, not that
  the transposition is the defect on every frame;
* no run has been checked frame-by-frame for "run B's world is lit".  The
  handoff must not claim a visual confirmation it does not have.

If the functional test is retried, make it all-or-nothing: correct every
`.x`/`.z` lane of the world family unconditionally (not gated on `|v| > 1e6`),
and capture a frame with `MTL_PROBE_ARM_FRAME` so the result is comparable to
run A.  A partial oracle cannot settle the question.

### 8.5 New — `MTL_PROBE_ARM_FRAME`

Comma list of up to 32 frame numbers.  Each value arms the same manual window
the F12 key arms, and the window starts on the frame *after* the number (as a
press does).  This is what lets a headless run capture the defect; F12 cannot
be pressed from a shell.  `MTL_PROBE_FRAME_COUNT` still sets the window length.

---

## 9. Code changes now on `metal4-dev` (2026-11-17 review)

Verified correct and kept:

* `BufferUtils.cpp` `line_loop` OOB: `upload_untouched` returns `count`, and
  `get_index_count(line_loop, n, false)` reserves `n+1`, so `dst[written]` is in
  bounds.  The old `dst[dst.size_bytes()/sizeof(T)]` was always one past the
  end.
* `MTLSurface.cpp` read-back: `rows` must stay guest rows; clamping it to host
  rows corrupted `span = rows * rsx_pitch` and the guest→host row mapping.
* `MTLShader.cpp` `NS::Error*` retain/drain; the two `MTLTextureCache.cpp` OOM
  guards; the FIFO `umax` comment; `supports_asynchronous_compute = false`.

Two changes from the 11-16 batch were wrong and are corrected on
`metal4-dev`:

* **Reverted: the "blit far-corner extent" change in `MTLBlit.cpp`.**  The SDK
  documents `renderTargetWidth`/`Height` as constraining the render target,
  anchored at the attachment's top-left.  The viewport and scissor still start
  at `(x0, y0)`, so the constrained area must span `dst_x1`/`dst_y1` (the far
  corner).  The original code was correct; constraining it to `w × h` puts an
  offset destination rectangle outside the render target.
* **Completed: the `nv3089` invalidation range.**  The 11-16 fix corrected the
  offset (`dst_offset + out_offset`) but kept the source span.  A scaled-up
  destination writes `out_pitch*(h-1) + w*out_bpp`, so the invalidation now
  uses the destination extent (the same `pitch*(clip_height-1)+bpp*clip_width`
  the reservation path already uses), floored by the source span.

Tests on `metal4-dev`: `build-tests/bin/rpcs3_test` → 696/696 pass.
Scrubber `.metal4-release-scrub.py check` → 0 residuals, 0 orphans, 0 review.
(The 19 dial-test failures one sees when the binary is run against the *release*
checkout are the missing `docs/` inventory, not the code.)

---

## 10. Decision table

| observation | conclusion | next |
|---|---|---|
| `src == after`, both transposed | producer is upstream of the copy | instrument the staging ring (§11.1) |
| `watch drift`, no `watch <writer>` | writer outside the three sites | instrument the next candidate; do not guess |
| nothing at all | the change is not a write | re-audit `offsets_cache` and the fill |
| oracle run B lights the world | the literal is causal | fix the producer, not the fill |
| oracle run B does **not** light the world | the literal is not causal (or the oracle is still partial) | inspect `fp-69`'s use first (§11.2) |

---

## 11. Open questions and next actions

1. **Who writes the staging block?**  `nv308a` reads a 16-byte ring block at
   `0x79e219c…` and copies it faithfully.  The block already contains
   `[W, 2.0, W, 1.0]` with only the W lanes transposed.  Candidates: a guest
   store path that is 16-bit-wide (or reorders halves), a transfer that writes
   the ring, or the game's own runtime patcher.  Instrument the ring address
   range for CPU/guest writes, or dump the block per frame and diff the writer.
   This is the only measurement that identifies the layer.
2. **Does `fp-69` actually consume `fc[67]`/`fc[68]`?**  The offsets are in
   `offsets_cache`, but no one has shown the constants reach the colour output.
   Dump the MSL (or disassemble the ucode) and trace `fc[67]`/`fc[68]` to the
   fragment colour.  If they are written to a temp and discarded, the whole
   literal model is a red herring and the darkness has another cause.
3. **Why only the animated lanes?**  That is the producer's signature.  Answer
   it together with (1).
4. **The flicker ghost (§2.1).**  The previous screen appearing briefly is a
   stale-surface artefact (uncleared/reused target), not a constant.  Track it
   separately; it should be reproducible with the surface reuse/cache probes,
   not with the literal instruments.
5. **The shared-fill contradiction.**  If the guest bytes are transposed, VK/GL
   must be equally dark.  Resolve it logically, not by running a differential:
   either the staging producer is shared (most likely) or the literal is not
   the cause.

Next action, in order: (2) before (1).  If `fp-69` does not use the constants,
stop and re-open the shading path; everything downstream of that is wasted.

---

## 12. Repo, build and release state

* Branch `metal4-dev` at `47ac27cef` plus the two 2026-11-17 fixes (§9).
* Dev build: `build-tests`.  `rpcs3` and `rpcs3_test` green after the fixes;
  re-run `ninja rpcs3 rpcs3_test`, then `build-tests/bin/rpcs3_test`, then
  `python3 .metal4-release-scrub.py check`.
* The release commit `e6fd9ca7b` on `release/metal4` was cut **before** the two
  §9 corrections, so it contains the blit regression and the incomplete
  `nv3089` span.  Re-cut the squashed release after the dev tree settles; do not
  force-push `origin/metal4`.
* Dev-build speedup: `-DMACDEPLOYQT_EXECUTABLE=/usr/bin/true`.

---

## 13. Docs

* `docs/research/metal4/design/PLAN-2026-11-16-ds-world-literal-rca.md` — the
  live plan.
* `docs/research/metal4/design/PLAN-2026-11-14-fp-literal-rca.md` — superseded.
* `docs/research/metal4/design/METAL4-VK-GL-PARITY.md` — D1–D16 audit.

---

## 14. Reproducing the offline analysis

```sh
cd /Users/abhakash/PersonalWS/rpcs3/build/probe
python3 - <<'EOF'
import re, struct, glob, itertools, math
f32 = lambda u: struct.unpack('>f', struct.pack('>I', u))[0]
def perm(w, p):
    b = [(w >> (8*i)) & 0xff for i in range(4)]
    o = [b[p[i]] for i in range(4)]
    return o[0] | o[1] << 8 | o[2] << 16 | o[3] << 24
words = []
for path in glob.glob('run-*/fill.log') + glob.glob('run-*/dumps-keep/*.const.txt'):
    for line in open(path, errors='replace'):
        for m in re.finditer(r'\[6[78]\] (?:@\d+ )?([0-9a-f]{8}) ', line):
            w = int(m.group(1), 16)
            if w != 0x803f0000:
                words.append(w)
words = list(dict.fromkeys(words))
print('distinct', len(words))
for p in itertools.permutations(range(4)):
    v = [f32(perm(w, p)) for w in words]
    if not all(math.isfinite(x) for x in v):
        continue
    ok = sum(1 for x in v if 0.002 <= abs(x) <= 1000)
    if ok == len(v):
        print(p, round(min(v), 3), round(max(v), 3))
EOF
```

The `(2,3,0,1)` row is the full reversal; it is the tightest, and it is the
hypothesis the oracle tests.

---

## 15. Launch template (headless, with a capture window)

```sh
cd /Users/abhakash/PersonalWS/rpcs3/build-tests/bin
EBOOT="/Users/abhakash/PersonalWS/rpcs3/filesdump/Demon's Souls (USA)/Demon's Souls (USA)/PS3_GAME/USRDIR/EBOOT.BIN"
RUN=/Users/abhakash/PersonalWS/rpcs3/build/probe/run-$(date +%Y%m%d-%H%M%S)-ds20
mkdir -p "$RUN"
MTL_PROBE=2 MTL_PROBE_LOG="$RUN/mtl-probe.log" MTL_PROBE_FRAME_COUNT=60 MTL_PROBE_TAPE=0 \
MTL_PROBE_ARM_FRAME=600,1200,1800,2400,3000 \
MTL_PROBE_PREFETCH_LOG="$RUN/prefetch.log" RSX_FP_LITERAL_LOG="$RUN/fill.log" \
RSX_FP_WATCH_LOG="$RUN/watch.log" RPCS3_TRACE_ON_SIGNAL=1 \
nohup ./rpcs3.app/Contents/MacOS/rpcs3 --no-gui "$EBOOT" > "$RUN/stdout.log" 2>&1 &
```

Notes:

* `--no-gui` must come **before** the path.
* `RPCS3: RSX` messages at `error` level do not reach stdout in `--no-gui`;
  read `~/Library/Caches/rpcs3/RPCS3.log` (`·E`/`·F`).  It is truncated per run
  and overwritten by any `--help` invocation.
* `MTL_PROBE=5` dumps go to `MTL_PROBE_DUMP_DIR` (default `/tmp/mtl4-dump`).
* Never `pkill -f rpcs3`; multiple launches are fine.
* The oracle run is run B of §8.3, but see §8.4 before drawing conclusions from
  it.

<!-- doc-end -->
