# Plan — closing the Demon's Souls world literal (2026-11-16)

Status: **one run to certainty.**  Supersedes
`PLAN-2026-11-14-fp-literal-rca.md`.  The object is still one 4-byte number, but
the question has changed.  It is no longer "is the shader getting the right
value" — that is measured and answered.  It is "**who put those four bytes in
that order, and is the guest's source already in that order**".

## 0. What is already proven, and how

| # | Fact | Evidence |
|---|---|---|
| F1 | The fill reads the program image with a per-16-bit-lane byte swap: `T(w) = ((w & 0x00ff00ff) << 8) \| ((w & 0xff00ff00) >> 8)`. | `write_fragment_constants_to_buffer_sse2`, `ProgramStateCache.cpp:646`; the ARM fallback at `:676` is the same function. |
| F2 | The authored words in the same quad are correct under `T`: `00400000`→2.0, `803f0000`→1.0, `993e9999`→0.3. | every `fill.log` line, lanes 1 and 3. |
| F3 | The patched `.x`/`.z` words arrive at the shader as the full byte reversal of a sane float.  `T(word)` is junk (NaN or ±1e38); `bswap32(T(word))` is a smooth 1.308–2.222 across 164 distinct words. | `run-20261115-092255-ds12/fill.log` (614 fills, 5 world programs), `run-20261115-091131-ds11/fill.log`. |
| F4 | The image already contains the bad bytes at prefetch, so the patch is a memory write that lands before the program is analysed. | `ds11/prefetch.log`: the tail of `reg 014a3c81` is `a1273fbd 00400000 a1273fbd 803f0000`, identical to the fill. |
| F5 | The world programs live in **local memory** (`loc 0`) at fixed offsets, and the same offset is re-patched across frames (same `off`, different `hash`). | `ds12/fill.log` groups by `off`; `prefetch.log` `loc 0`. |
| F6 | Only three sites in the RSX write guest-visible bytes: `nv308a::color`, `nv0039::buffer_notify`, `nv3089::image_in`. | `grep -n "vm::_ptr<u8>" rpcs3/Emu/RSX/NV47/HW` returns exactly those three. |

F2 and F3 together are the whole diagnosis: **one code path writes four bytes in
the wrong order, and it is not the fill, the constant ring, the shader or the
backend.**  Everything else in the image survives the same transform.

## 1. Why the last run could not attribute the write

`MTL_PROBE_WRITE_LOG` is emitted from `graph_engine::invalidate_fragment_program`
(`RSXGraphEngine.cpp:1508`).  Three properties make the world patch invisible to
it, and all three are visible in the code:

1. **Wrong address.**  `nv308a` calls
   `invalidate_fragment_program(dst_dma, dst_offset, count * 4, ...)` with the
   *blit destination base*, but writes at
   `dst_offset + x * 4 + out_pitch * y` (`nv308a.cpp:84`, `:95`).  Both the
   overlap test and the `relative >= tail_start` filter therefore test a
   position where the bytes did not go.  `nv3089` has the same shape: it passes
   `dst_offset` and writes at `dst_offset + out_offset` (`nv3089.cpp:213`,
   `:276`).
2. **Wrong program.**  The hook compares against
   `m_ctx->program.fragment_program`, the *currently bound* program.  A driver
   that patches a batch of programs and then binds them is invisible to it.
   That is exactly what F5 shows: five world programs, all re-patched, one of
   them bound.
3. **Wrong time.**  For `nv0039` and `nv3089` the hook runs *before* the copy
   (the comment at `RSXGraphEngine.cpp:1540` says so).  The `tail` it prints is
   the pre-copy image, so even a hit would show the old bytes.

The eleven lines in `ds11/writes.log` are the proof that this is a gap and not a
total absence: all eleven are `nv308a`, all land at `rel` 896–1472, and all
carry instruction words (`4002820e`, `9d1c04c9`, …), never the literal quad.  The
probe was working; it was pointed at the wrong byte.

## 2. The fork

Two worlds are consistent with F1–F6.  Exactly one is true.

**H-rpcs3 — the writer reorders.**  The guest hands the blit engine the word in
the layout the hardware's ucode fetch wants, RPCS3 stores it verbatim, and the
bytes in memory are correct.  Then `T` would return the intended float, as it
does for every authored word.  It does not.  So for H-rpcs3 to be false and the
game to work on hardware, RPCS3 must be transforming the bytes on the way in.

**H-guest — the guest writes the bad order.**  The guest's source buffer already
contains the reversed bytes, and RPCS3 copies them faithfully.  For this to be
consistent with a working game, the hardware's ucode layout would have to differ
from the layout `T` assumes — which F2 refutes for the authored words in the same
quad.

The two are distinguished by **one measurement: the source bytes at the writer.**
If `src` is already `bswap32(T(word))`, H-guest; if `src` is `T(word)` and `dst`
is not, H-rpcs3.  The writer is one of three functions (F6).  This is a closed
question, not a search.

## 3. Instruments

All are default-off, `EXCISE`-wrapped, bounded, and none is load-bearing.

### W1 — persistent program watch list (`RSX_FP_WATCH_LOG`)

`graph_engine::prefetch_fragment_program` records `{dma, offset, len, tail
pointer, 384-byte shadow}` for every fragment program it sees, up to 16 entries.
This is the fix for blind spots 2 and 3: a write is checked against every
program that has ever been prefetched, not against the bound one.

On re-record of a known `(dma, offset)` the shadow is compared first; a change
found there is logged as `watch drift`, which means *some* writer mutated the
tail and no instrumented site claimed it.  That is the safety net: if the
writer is outside W2, the drift line still proves the write happened and
brackets it with a FIFO position.

### W2 — byte-level capture at the three writers (`RSX_FP_WATCH_LOG`)

`nv308a`, `nv0039`, `nv3089` call `program_write_watch::note(...)` with the
**real** write offset (`dst_offset + x*4 + out_pitch*y` for `nv308a`,
`dst_offset + out_offset` for `nv3089`), the size, the source pointer and its
length, and the FIFO position.  The call is after the copy for all three.

A hit logs, for the first changed 4-byte lane:

```
watch hit writer=nv308a dma=0 off=<abs> size=<n> program=<off>+<len> fifo=<pos> x=.. y=.. pitch=.. fmt=.. count=..
  at=<abs> before=<4 words> after=<4 words>
  read=<4 floats> rev=<4 floats> src=<4 words>
```

`read` is `T(after)`, `rev` is `bswap32(T(after))`.  If `rev` is the smooth value
and `src` equals `T(word)`, the writer reordered and H-rpcs3 is confirmed at the
exact instruction.  If `src` already equals `after`, H-guest is confirmed.

The overlap prefilter is a range compare; the 384-byte `memcmp` runs only on an
overlap.  Cost on a non-hit is four integer compares.

### W3 — fifo position on the fill (`RSX_FP_LITERAL_LOG`)

One extra field, `fifo=<get_pos>`, on the existing fill line.  It orders the
patch against the bind and lets the writer line be matched to the fill that
consumed it without a FIFO dump.

### W4 — functional oracle (`RSX_FP_LITERAL_FIX=bswap32`)

In `write_fragment_constants_to_buffer`, after the fill, for the last two quads
of the world family only: a lane whose `T(word)` is not a plausible float
(`!isfinite` or `|v| > 1e6`) but whose `bswap32(T(word))` is, is replaced by the
reversal.  Each decision is logged.

This is the 100% proof and it does not depend on W1/W2 being right.  It applies
the *suspected* correction to exactly the lanes that are broken by F3's test and
to nothing else.  If the world draws with it, then the value, the lane, the
direction and the magnitude of the defect are all confirmed on screen; if it
does not, F3's interpretation is wrong and the whole model is discarded before
any fix is written.  The authored lanes (2.0, 1.0) are untouched by the
predicate, so a correct scene cannot be produced by breaking them.

W4 is a probe, not the fix.  The fix belongs at the layer W2 convicts.

## 4. Run protocol — one launch, two runs

```
runA  RSX_FP_WATCH_LOG=<A>/watch.log RSX_FP_LITERAL_LOG=<A>/fill.log      (no fix)
runB  RSX_FP_WATCH_LOG=<B>/watch.log RSX_FP_LITERAL_LOG=<B>/fill.log
      RSX_FP_LITERAL_FIX=bswap32                                          (fix)
```

Both at `MTL_PROBE=2` (level 5 dumps are not needed and cost 13 GB), capture
window as before, same save, same camera.

`runA` answers "who and in what order".  `runB` answers "is that the whole
defect".  They are independent; a failure of either is informative.

## 5. Decision table

| runA `watch hit` says | runB says | Conclusion | Fix |
|---|---|---|---|
| `src` == `after`, `after` == reversal of `read` | world renders | the guest's staging buffer already holds the reversed word; RPCS3 copied faithfully | the image layout for this patch differs from `T`'s model — the defect is upstream of the copy, in how the patch is produced or addressed; W2's `at`/`x`/`y`/`pitch` name the layer |
| `src` == `read` (correct), `after` reversed | world renders | the writer reorders on the way to local memory | in that writer: restore the source byte order for program-image destinations |
| `watch drift` and no `watch hit` | world renders | the writer is outside the three sites | use W3's fifo bracket and the drift line to locate it; do not guess |
| no hit, no drift | world renders | the patch is not a write at all — the observed change is produced later | the value is being altered between the image and the shader; re-audit `offsets_cache` and the fill |
| any | world black | F3's interpretation is wrong | discard the model; the defect is not this word, and the probe data from W1/W2/W3 is still valid for the next hypothesis |

## 6. What "resolved" means

1. `runA` names a concrete function and byte order, or proves the write is not a
   write.
2. `runB` renders the world with the reversed lanes.
3. The fix is at the convicted layer, general (no game constants, no lane
   positions, no offsets), and the reverse variant is removed.
4. `rpcs3_test` green, `metal4-release-scrub.py check` clean, scrubbed tree
   builds.

<!-- doc-end -->
