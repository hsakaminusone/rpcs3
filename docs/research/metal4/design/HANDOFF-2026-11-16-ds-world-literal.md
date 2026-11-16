# Handoff — Demon's Souls world literal (2026-11-16)

For whoever picks this up next, including me tomorrow.  Read §3 and §7 first if
you only have two minutes: the defect is understood to within one measurement,
and the measurement is instrumented but not yet taken.

---

## 1. TL;DR

* Demon's Souls (BLUS30443) renders the 3D world black/flat on the Metal 4
  backend.  Everything else in the frame is correct.
* The cause is **not** the Metal backend, the shader, the constant ring, the
  MetalFX path or the FSR port.  It is one 4-byte number in the trailing
  inline-literal block of the five "world" fragment programs.
* The shader receives `bswap32(X)` where `X` is the value the game authored.
  `X` is a smooth 1.31–2.22 exposure/scale factor; `bswap32(X)` is junk
  (NaN or ±1e38).  The fill replaces the lane with whatever the program image
  contains, and the image contains the reversed bytes.
* The authored lanes of the same quad (`2.0`, `1.0`) are correct in the same
  image, so the image layout convention is right and only the **per-draw patch**
  is wrong.
* The patch is a memory write that lands before the program is analysed
  (proven by the prefetch log).  Only three code sites can perform such a
  write.
* The previous write probe could not have seen it.  Three independent,
  code-level reasons (§6).  New instruments W1–W4 fix all three and add a
  functional oracle that proves the interpretation on screen.
* Next action: one launch, two runs (with and without the oracle), then read
  `watch.log`.  The decision table in §10 maps the result to the fix layer in
  one step.

---

## 2. Context you need

This is the `metal4` line of work: a native Metal 4 backend for RPCS3, held to
RSX behaviour as the spec.  A parity audit (`METAL4-VK-GL-PARITY.md`) is
complete through D1–D16.  The last commit is `8ef12357f`
("metal4: add the GPU blit engine and close the D1/D5/D6 parity gaps") on
`origin/metal4-dev`.  A release worktree sits at `/Users/abhakash/PersonalWS/rpcs3-release`,
detached at that commit, not built, not pushed.

The Demon's Souls defect is a **guest/RSX-layer** bug that the Metal backend
merely makes visible.  Do not "fix" it in MSL, in the ring, or in MetalFX.

Do not run the Vulkan or OpenGL backend as a differential test.  That was
explicitly ruled out; the RSX behaviour, not another backend, is the reference.

---

## 3. What is proven, and with what

### 3.1 The fill transform

`write_fragment_constants_to_buffer_sse2` (`ProgramStateCache.cpp`, the loop
starting with `_mm_loadu_si128`) does

```cpp
shuffled = _mm_or_si128(_mm_slli_epi16(vector, 8), _mm_srli_epi16(vector, 8));
```

which is a byte swap **within each 16-bit halfword**:

```
T(w) = ((w & 0x00ff00ff) << 8) | ((w & 0xff00ff00) >> 8)
```

The ARM fallback two lines below is the same function, so this is
backend- and arch-neutral.  This is the correct convention for RSX ucode: a
32-bit word is stored as two 16-bit halves and the fetch swaps each half.

### 3.2 Authored words are correct

From every `fill.log` line, the same quad:

| image word (host u32) | `T(word)` | value |
|---|---|---|
| `00400000` | `40000000` | 2.0 |
| `803f0000` | `3f800000` | 1.0 |
| `993e9999` | `3e999999` | 0.3 |

So `T` is right, the image layout is right, and the `.y`/`.w` lanes of the very
quad under suspicion are right.

### 3.3 The patched lanes are byte-reversed

World program `fp-69` (`len 3312`, quad at `0xcc0`), three live images captured
in `ds11`:

```
live-1  bd 3f 27 a1   00 00 40 00   bd 3f 27 a1   00 00 3f 80
live-2  ...           00 00 40 00   ...           00 00 3f 80   (different X)
live-3  ...           00 00 40 00   ...           00 00 3f 80   (different X)
```

The `.x`/`.z` words change per draw; `.y`/`.w` do not.  For every X word:

| | value |
|---|---|
| `T(word)` | 3.107e38 (junk; `0x7f69c43f`) |
| `bswap32(T(word))` | 1.534 (sane; `0x3fc4697f`) |

Across `ds11` + `ds12`: **164 distinct X words, 164/164 sane after
`bswap32(T(word))`, range 1.308–2.222**, smoothly ordered.  That is an
exposure/scale ramp, not noise.

Two honest caveats:

1. **Range alone does not uniquely identify the permutation.**  Re-running the
   permutation search over the 164 words, six byte permutations put every value
   in a plausible float range; the tightest is the full reversal
   `(2,3,0,1)` → 1.308–2.222, the next are 1.312–2.234 and 0.51–7.98.  The
   clustering is strong evidence, not proof.
2. 164/164 is a statement about the *value*, not about *who* produced it.

That is exactly why the next run captures the writer (§8, W2) instead of
reasoning from values alone.

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

`ds12/fill.log` has 614 fills over 5 distinct `(offset, length)` pairs with
different hashes per pair: each world program is re-patched in place, frame
after frame.

---

## 4. The mechanism, stated precisely

Let `X` be the authored float, `W = bits(X)`, and `T` the fill transform.

* what the shader must receive: `W`
* what the shader receives: `T(M)`, where `M` is the word in memory
* measured: `T(M) = bswap32(W)`
* therefore: `M = bswap32(T(W))`

In words: **the four bytes at the patched lane are in reverse order relative to
the byte sequence the program image expects.**  The authored lanes have
`M = T(W)`; the patched lanes have `M = bswap32(T(W))`.

`bswap32(T(W))` is byte-reversal of `T(W)`, which — because `T` is itself a
swap of two 16-bit halves — is the same as swapping the two 16-bit halves of
`W` and storing them big-endian.  Both descriptions are true; only the
byte-reversal one is stated in terms of the image, and that is the one the log
prints.

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

`nv308a` is the only one proven active in the world-program region, from
`ds11/writes.log` (§6).

---

## 6. Why the previous run could not answer it

`MTL_PROBE_WRITE_LOG` is emitted from `invalidate_fragment_program`.  Three
properties make the world patch structurally invisible, all readable in the
code:

1. **Wrong address.**  `nv308a` calls
   `invalidate_fragment_program(dst_dma, dst_offset, count * 4, ...)` with the
   blit's *destination base*, but writes at
   `dst_offset + x*4 + out_pitch*y`.  Both the overlap test and the
   `relative >= tail_start` filter therefore test a position where the bytes did
   not go.  `nv3089` has the same shape: passes `dst_offset`, writes at
   `dst_offset + out_offset`.
2. **Wrong program.**  It compares only against
   `m_ctx->program.fragment_program`, the currently bound program.  The guest
   patches a set of programs and then binds them; five world programs are
   patched and one is bound.
3. **Wrong time.**  For `nv0039` and `nv3089` the hook runs *before* the copy —
   the comment above it says so.  The `tail` it prints is the pre-copy image.

The eleven lines in `writes.log` are the proof it is a gap, not an absence: all
eleven are `nv308a`, all at `rel` 896–1472, all carrying instruction words
(`4002820e`, `9d1c04c9`, …), never the literal quad.  The probe worked; it was
pointed at the wrong byte.

---

## 7. The two surviving hypotheses

**H-rpcs3 — a writer reorders.**  The guest hands the copy the bytes in the
layout hardware wants; RPCS3 stores them in the wrong order.  Fix: in the
convicted writer, for program-image destinations.

**H-guest — the source is already reversed.**  The guest's staging bytes are
already `bswap32(T(W))` and RPCS3 copies faithfully.  Fix: upstream of the
copy — the patch is produced or addressed wrongly, and `at`/`x`/`y`/`pitch`
from the W2 line name the layer.

They are distinguished by **one measurement: the source bytes at the writer.**
`src == T(W)` and `dst != src` ⇒ H-rpcs3.  `src == dst == bswap32(T(W))` ⇒
H-guest.

---

## 8. Instruments now in the tree

All default-off, `EXCISE`-wrapped, bounded, none load-bearing.

### W1/W2 — `rsx::program_write_watch`

New header `rpcs3/Emu/RSX/Core/RSXProgramWriteWatch.h`.

* `claim(addr, offset, len, image)` at every fragment prefetch records the image
  with a **384-byte shadow** of its tail.  Up to **64** entries, round-robin
  eviction, because the loading screen prefetches more than 16 distinct programs
  before the world ones.
* Keyed on the **host pointer the copy wrote through**, not the guest offset.
  That is what defeats blind spot 1: a copy that writes at an element offset
  inside its destination still matches.
* `note(writer, addr, size, src, src_len, detail)` is called **after** the copy
  by all three writers, so blind spot 3 is gone.  It compares the shadow of
  every overlapping entry, so blind spot 2 is gone.
* If the shadow changed between two prefetches of the same image and no writer
  claimed it, `claim` logs `watch drift`.  That is the net: it proves the
  mutation happened outside the three instrumented copies.
* Report shape (only the first changed 4-byte lane):

```
watch nv308a writer=nv308a program=<off>+<len> at=<abs> waddr=<host> wsize=<n> src=<host> x=.. y=.. pitch=.. fmt=.. count=..
  before=........ ........ ........ ........ after=........ ........ ........ ........ src=........ ........ ........ ........
  read=<4 floats T(after)> rev=<4 floats bswap32(T(after))>
```

`read` is what the shader would get; `rev` is the suspected correction.  If
`rev` is the smooth 1.3–2.2 value, the defect is confirmed at that instruction.

Call sites: `nv308a` bulk, `nv308a` non-increment, `nv308a-r5g6b5`, `nv0039`
(via `utils::deferred_op`, so it runs on every exit), `nv3089` at the end of
`image_in` (after linear and swizzled copies).

### W3 — FIFO cursor on the fill

The fill line gains `fifo <get_pos>` and `fix <count>`:

```
fill fp off <o> len <l> hash <h> count <c> sanitize <s> fifo <pos> fix <n> | [..] @.. .... -> ....
```

That orders a patch against a bind, and lets a writer line be matched to the
fill that consumed it without a FIFO dump.

### W4 — functional oracle

`RSX_FP_LITERAL_FIX=bswap32` (or `halves`), logged to `RSX_FP_LITERAL_FIX_LOG`.

In `write_fragment_constants_to_buffer`, after the fill, for the **last two
quads** of an image with `ucode_length >= 3000`: a lane whose value is not a
plausible float (`!isfinite` or `|v| > 1e6`) but whose `bswap32` is plausible is
replaced by the reversal.

The predicate is the diagnosis.  `2.0` and `1.0` are plausible and are never
touched, so a correct scene cannot be produced by damaging them.  If the world
draws with this, the value, the lane, the direction and the size of the defect
are all confirmed on screen, independently of W1/W2.

W4 is a probe, not the fix.  The fix belongs where W2 points.

---

## 9. Run protocol

One launch, two runs, same save and camera, `MTL_PROBE=2` (level 5 dumps are not
needed and cost ~13 GB).

```sh
cd /Users/abhakash/PersonalWS/rpcs3/build-tests/bin
EBOOT="/Users/abhakash/PersonalWS/rpcs3/filesdump/Demon's Souls (USA)/Demon's Souls (USA)/PS3_GAME/USRDIR/EBOOT.BIN"
RUN=/Users/abhakash/PersonalWS/rpcs3/build/probe/run-$(date +%Y%m%d-%H%M%S)-ds14
mkdir -p "$RUN"

# run A: attribution only
MTL_PROBE=2 MTL_PROBE_LOG="$RUN/mtl-probe.log" MTL_PROBE_FRAME_COUNT=60 MTL_PROBE_TAPE=0 \
MTL_PROBE_PREFETCH_LOG="$RUN/prefetch.log" RSX_FP_LITERAL_LOG="$RUN/fill.log" \
RSX_FP_WATCH_LOG="$RUN/watch.log" RPCS3_TRACE_ON_SIGNAL=1 \
nohup ./rpcs3.app/Contents/MacOS/rpcs3 --no-gui "$EBOOT" > "$RUN/stdout.log" 2>&1 &

# run B: same, plus the oracle
RUNB="${RUN}-fix"
mkdir -p "$RUNB"
MTL_PROBE=2 MTL_PROBE_LOG="$RUNB/mtl-probe.log" MTL_PROBE_FRAME_COUNT=60 MTL_PROBE_TAPE=0 \
MTL_PROBE_PREFETCH_LOG="$RUNB/prefetch.log" RSX_FP_LITERAL_LOG="$RUNB/fill.log" \
RSX_FP_WATCH_LOG="$RUNB/watch.log" RSX_FP_LITERAL_FIX=bswap32 \
RSX_FP_LITERAL_FIX_LOG="$RUNB/fix.log" RPCS3_TRACE_ON_SIGNAL=1 \
nohup ./rpcs3.app/Contents/MacOS/rpcs3 --no-gui "$EBOOT" > "$RUNB/stdout.log" 2>&1 &
```

Never `pkill -f rpcs3`; multiple launches are fine.

First reads, in order:

1. `watch.log` — `watch <writer>` lines and their `rev=` column (§8).
2. `fix.log` — should contain roughly 2 lanes per quad per frame, i.e. the
   `.x` and `.z` lanes, and nothing else.
3. `fill.log` with `fix 2` on the family lines.
4. Visual: run B's world versus run A's.

If `watch.log` is empty but `fix.log` fired and run B renders, the mutation is
outside the three writers and `prefetch.log`/`fill.log` `fifo` ordering narrows
it; `watch drift` will say so explicitly.

---

## 10. Decision table

| run A `watch` says | run B | Conclusion | Fix |
|---|---|---|---|
| `src` is the correct order, `after` is reversed | world draws | writer reorders on the way to local memory | in that writer: restore source byte order for a program-image destination |
| `src == after`, both reversed | world draws | guest staging bytes already reversed; copy faithful | upstream of the copy; `x`/`y`/`pitch`/`at` name the layer |
| `watch drift`, no `watch <writer>` | world draws | writer outside the three sites | use `fifo` ordering; instrument the next candidate, do not guess |
| nothing at all | world draws | the change is not a write at all | re-audit `offsets_cache` and the fill |
| anything | world does **not** draw | the interpretation of §3.3 is wrong | discard the model; W1–W3 data is still valid for the next hypothesis |

Any outcome above other than the last closes this bug; the last one invalidates
the model rather than the instruments, which is why both runs are worth taking.

---

## 11. Fix requirements when the layer is known

* General: no game constants, no literal offsets, no lane positions, no
  `if (is_demons_souls)`.
* At the copy/format layer if H-rpcs3; at the producer if H-guest.  Not in MSL,
  not in the ring, not in the fill.
* Remove W4's reversal and the probe variants when the real fix lands; keep the
  watch/diagnostic discipline (default-off, bounded, `EXCISE`).
* Consider whether `invalidate_fragment_program` should be handed the **real**
  written range rather than the blit base (§6.1).  That is a latent correctness
  issue independent of this bug; it is not confirmed to matter today, so do not
  change it on speculation.

---

## 12. Repo, build and release state

* Branch `metal4-dev` at `8ef12357f`; parity D1–D16 implemented, D4 FSR port
  (`MTLUpscaler`/`MTLUpscaleFSR.h`) and its tests in place.
* Dev build: `build-tests`.  `rpcs3` and `rpcs3_test` were green before this
  round of instrumentation; the instrumentation rebuild was in flight when this
  report was written.  Re-run `ninja rpcs3 rpcs3_test` in `build-tests`, then
  `build-tests/bin/rpcs3_test`, then
  `python3 scripts/metal4-release-scrub.py check`.
* The scrubber counts 607 edits untracked / 609 with `MTLBlit.cpp`.  New files
  must be `git add -N`'d before scrubbing or the release tree will miss them:
  `RSXProgramWriteWatch.h`, `MTLUpscaleFSR.h`, `MTLUpscaler.*`,
  `test_rsx_metal4_fsr.cpp`.
* Release worktree `/Users/abhakash/PersonalWS/rpcs3-release` is detached at
  `8ef12357f`, submodules initialised, **no build dir**.  Do not push
  `release/metal4` until the world renders.  `origin/metal4` must not be
  force-pushed.
* Dev-build speedup: `-DMACDEPLOYQT_EXECUTABLE=/usr/bin/true`.

---

## 13. Docs

* `docs/research/metal4/design/PLAN-2026-11-16-ds-world-literal-rca.md` — the
  live plan (this report is its narrative form).
* `docs/research/metal4/design/PLAN-2026-11-14-fp-literal-rca.md` — superseded;
  kept for the P1–P15 probe history.
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

## 15. Open questions

1. Which of the three writers performs the patch, and is the source already
   reversed?  (W2 answers both.)
2. Why do the five world programs share the *same* X per frame but differ
   between frames?  Consistent with one exposure value per frame written into
   each program; if W2 shows five independent writes with different `x`/`y`,
   the producer is a loop, which helps locate it.
3. Does the same reversal affect the **instruction** words that `writes.log`
   already caught (`rel` 896–1472)?  If yes, the defect is broader than the
   literal block and the fix is definitely at the copy layer.  W2's `before`/
   `after` on those writes answers this for free — check them in the same log.

<!-- doc-end -->
