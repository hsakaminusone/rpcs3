# Metal 4 review — applicability and implementation

**Status:** live implementation log. Companion to
`docs/research/review/METAL4-BACKEND-REVIEW.md` (v1, 2026-11-27).
**Tree:** `metal4-dev`, rebased onto `upstream/master` `0ca5eb0ef`
(2026-11-25), base of this work `ddee3c969`.
**Rule:** the review's line numbers are for `release/metal4` `e6fd9ca7b`.
Every finding below was re-read against `metal4-dev`; a finding is only
"present" when the code path still exists *after* the rebase.

The Demon's Souls handoff stays in `DS-STROBE-INVESTIGATION.md`. This file
does not replace it and does not repeat its measurements.

---

## 1. Why this file exists

The review is a static read of a different commit. The rebase brought in
upstream fixes that retire several findings, and `metal4-dev` already carries
work (F60–F62, the F10/ROP epilogue) that the review did not see. The useful
output is therefore not "the review says X" but "of X, these still exist here,
these explain a Demon's Souls symptom, and these are worth changing first".

Two findings were already fixed by the rebase and must not be re-fixed:

- **F7** (`sys_rsx` vblank/flip deadlock): upstream `7ef112187`.
- **C23 / C1 layer 1** (NaN guard): upstream `#20078` rewrote the clamp with
  integer bit tests; `FragmentProgramDecompiler.cpp` now uses
  `floatBitsToUint` / `_select`.

Two findings are already implemented in `metal4-dev` although the review
listed them as missing:

- **C10** (depth-bounds test): emitted in the fragment epilogue from
  `MTLFragmentProgram.cpp` `mtl_rasteriser_state.x/.y`.
- **D2** (MSAA sample mask): emitted as `gl_SampleMask[0]` from
  `mtl_rasteriser_state.z`.

---

## 2. Applicability table

`✔` = still present here; `✘` = retired/fixed; `~` = present but not on the
Demon's Souls path.

| ID | Finding | Here | Evidence / note |
|---|---|---|---|
| S1 | Reused compute encoder has no intra-encoder ordering | ✔ | `MTLCommandRecorder.cpp:754-761` returns `m_compute_encoder` with no `barrierAfterEncoderStages`; `MTLSurface.cpp:743-754` is `copyFromTexture` → `copyFromBuffer` in one encoder. |
| C4 | Blit render area ignores destination origin | ✘ | `MTLBlit.cpp:292-293` uses `dst_x1/dst_y1`; commit `160d310bf5`. |
| C1 | Fast math defeats NaN handling | ~ | Layer 1 fixed by rebase. Default is still `MathModeFast` (`MTLShader.cpp:261,321`). |
| D1 | Blend-constant block is 36 bytes | ✔ | `MTLConstants.h:77-86`; MSL block is 48 bytes (`MTLFragmentProgram.cpp:290-297`). |
| T5 | In-place blit is dropped | ✔ | `MTLBlit.cpp:527-529`. |
| C8 | Scissor is mirrored | ✔ | `MTLRasterState.cpp:137-145` computes `y = clip_height - y2`. |
| C2 | Clip size ignores resolution scale | ~ | `clip_width/height` stay raw (`MTLSurfaceState.cpp:53-54`), but F60 added scaled `raster_width/height` and the viewport/pass use those. Only scale ≠ 100 % is affected. |
| C19 | Viewport depth range double-applies | ✔ | `MTLEncoderState.cpp:104-106` uses `clip_min/clip_max`; the vertex shader already emits window z (`_EMULATE_ZCLIP_XFORM_*`). |
| T1 | A4R4G4B4 swizzle is wrong | ✔ | `MTLFormats.cpp:734-739` returns `{R,A,B,G}`; the correct permutation is the A1R5G5B5 one, `{A,B,G,R}`. |
| T4 | 3D upload strides a whole volume per slice | ✔ | `MTLTextureCache.cpp:195,200` set `rows = height * depth`; `:509` passes that as `sourceBytesPerImage`. |
| C27 | Failed link/latch never retries | ✔ | `MTLProgramPipeline.cpp:850` still requires `work->error[0] == '\0'`. |
| C30 | Compiler concurrency left at default | ✔ | No `setShouldMaximizeConcurrentCompilation` in `MTL/`. |
| C11 | Samplers do not support argument buffers | ✔ | No `setSupportArgumentBuffers` in `MTLSamplerState.cpp`. |
| T2 | Surface upload/readback skips the endian swap | ~ | `MTLTextureCache.cpp:132` sets `supports_byteswap = false`. Only observable with Read/Write Color Buffers, which the DS probe disables. |
| T3 | MSAA resolve is hardware averaging; unresolve is a no-op | ~ | `MTLSurface.cpp:507-566`; `unresolve()` only clears the flag. Only titles that read back MSAA. |
| L5 | Overlay image rewritten while a frame may sample it | ✔ | `MTLOverlays.cpp:553` `replaceRegion`. |
| C13 | Present fallback reads guest memory raw | ✔ | `MTLGSRender.cpp:8156` `vm::get_super_ptr`, no pre-flush, no format cast. |
| C17 | Interpreter resolved before the real pipeline | ✔ | Not on the DS produce-a-frame path; stutter only. |
| A1 | One command buffer per slot; mid-frame commit waits | ✔ | `MTLCommandRecorder.h:18-21`; performance, not pixels. |
| C14 | Cache keys miss code-changing inputs | ✔ | Archive name keys device + OS; the per-program key still misses the MSL options. Mostly a stale-binary hazard. |
| C5 | No position invariance | ✔ | `require_explicit_invariance` exists in `GLSLTypes.h` but is never set by the Metal emitters. |
| L1–L3 | Timeout/teardown/feedback | ~ | The recorder already abandons a stalled slot instead of resetting it (`MTLCommandRecorder.cpp:416-429`), so the review's L1 description is weaker here. Not on the pixel path. |
| R1 | Guest window is a snapshot at import | ✔ | `MTLDevice.cpp` no-copy import. Broad; needs its own measurement. |

The `Here` column is the state at the review re-read. Change set 1
(`49553cd6e`) implements S1, C1 layer 2, D1, T1, T4, C8, C19, C27, C30 and
C11; the C13 pre-flush follows in `b3075bc6a`. The remaining `✔` rows are
still open.

---

## 3. Demon's Souls mapping

The three symptoms and the findings that can produce them on this tree:

| Symptom | Primary | Secondary | Why |
|---|---|---|---|
| Whole-image black/grey flicker | **S1** | C13, T2 (only with read buffers), C19 | An unordered clear→upload or texture→buffer pair inside one encoder produces partially written images that change per frame. The present fallback reads raw guest bytes with no flush. |
| White water / floor | S1, D1 | C1 (Fast math), C19 | S1 can leave a texture half-written. D1 misaligns the blend constants after the first draw, so an alpha-blended water pass can take a wrong constant colour. C1 is the review's original explanation; its strongest layer is already fixed by the rebase, so it is now a residual. |
| Left-half blur / seam at x=640 | **T5** | C8/C2 (scale), C4 (already fixed) | C4 is in the tree, so the review's seam explanation is spent. An in-place scaled/filtered copy is dropped, which is exactly the shape of a screen-space blur that writes the scene target. C8 only matters when the game uses a partial scissor. |

The order below is value ÷ risk for *these* symptoms. Larger correctness work
that needs its own capture (T5 through a temporary, T2 endian, T3 mosaic) is
listed but not mixed into the first change set.

---

## 4. Change set 1 (commit `49553cd6e`)

Each item is small, local, and independently revertable.

1. **S1** — `open_copy_encoder()` emits an encoder-scope
   `barrierAfterEncoderStages(Blit|Dispatch, Blit|Dispatch)` when it returns an
   already-open encoder; `surface::write_to_memory` emits the same edge between
   its texture copy and its buffer copy. This is the Metal 4 rule: queue and
   producer barriers order *other* encoders, not commands inside one.
2. **D1** — pad `blend_values` to the 48 bytes the MSL block actually declares,
   so block *n* starts where the shader expects.
3. **T1** — A4R4G4B4 uses the same channel permutation as A1R5G5B5.
4. **T4** — per-slice source stride: `rows` stays the whole volume for the
   allocation, `image_rows` is one slice, and `sourceBytesPerImage` uses
   `image_rows`.
5. **C19** — the viewport depth range is always `[0, 1]`; the vertex shader
   already produced window z.
6. **C8** — the scissor is not mirrored and is clamped to the pass extent.
7. **C27** — a failed link is retried up to two times even when the failure
   carries an error string, then latched. This restores the branch the review
   found dead without retrying hard syntax errors forever.
8. **C30** — `setShouldMaximizeConcurrentCompilation(true)`.
9. **C11** — `setSupportArgumentBuffers(true)` on sampler descriptors.
10. **C1 layer 2** — the default MSL math mode becomes `Relaxed`; the
    `disable_msl_fast_math` option still selects `Safe`.

### Change set 2 (commit `b3075bc6a`)

11. **C13** (pre-flush only) — before the guest-memory present fallback reads
    `vm::get_super_ptr`, find the `blit_engine_dst` / `framebuffer_storage`
    sections overlapping the display range, `copy_texture` each unsynchronised
    section, `flush_for_readback()`, then `invalidate_range(..., read)`. This is
    the Vulkan sequence (`VKPresent.cpp:366-388`). The format-cast half of C13
    stays deferred with the T6 typeless copy path.

### Change set 3 (commit `cc7157d43`)

12. **T5** — an in-place scaled/filtered blit copies the normalised source
    region into a private single-sample 2D staging image, rebases the source
    rectangle (keeping a flip signed), then runs the existing scaled pass from
    the staging view. MSAA, 3D/array and out-of-bounds sources are rejected
    before allocation.

## 5. Deferred (needs its own change and capture)

The full remaining list, including the large redesigns, is tracked in
`METAL4-FULL-REMEDIATION-PLAN.md`.

- **T2** surface upload/readback endian swap (kernel + format cast).
- **T3** MSAA sample mosaic resolve/unresolve.
- **L5** overlay reallocation instead of `replaceRegion`.
- **C13 format cast** when the display format differs from the source surface;
  the pre-flush half is done.
- **C2** scaled clip extent everywhere (only matters at scale ≠ 100 %).
- **C17/A1** submission and compile-pipeline rework.
- **R1** per-block guest window imports.
- **C14/C29** cache keys and periodic archive flush.

---

## 6. Log

| Date | Change | Build | Tests |
|---|---|---|---|
| 2026-11-27 | file created; S1/D1/T1/T4/C19/C8/C27/C30/C11/C1 implemented (`49553cd6e`) | `build-tests`, arm64 Release: clean | `rpcs3_test`: 690/690, 2 disabled; `.metal4-release-scrub.py check`: clean |
| 2026-11-27 | C13 pre-flush in the guest-memory present fallback (`b3075bc6a`) | `build-tests`, arm64 Release: clean | `rpcs3_test`: 690/690, 2 disabled; `.metal4-release-scrub.py check`: clean |
| 2026-11-27 | T5 in-place blit staging (`cc7157d43`); full remediation plan created | `build-tests`, arm64 Release: clean | `rpcs3_test`: 690/690, 2 disabled; `.metal4-release-scrub.py check`: clean |

### Notes from this pass

- **Rebase fallout fixed in passing.** `MTLProgramPipeline` still named
  `rsx::shader_loading_dialog`, which upstream renamed to
  `shader_loading_dialog_native`; the file did not compile until the two
  declarations were updated. This was present before the change set and is not
  a review finding.
- **S1 cost.** Every reuse of the shared compute encoder now emits an
  encoder-scope edge. Uploads in one encoder serialise, which is the review's
  documented cost; the alternative (tracked resource edges) is redesign R3.
- **C19 evidence.** `RSXVertexPrologue.glsl` takes the fallback
  `apply_zclip_xform`, whose own comment says the value is "the final depth
  that should be stored in the Z buffer"; the viewport must therefore stay
  `[0, 1]`. The guest clip range is still captured for the state dump.
- **Tests updated, not deleted.** The scissor test asserted the mirror and was
  inverted; a clamping case was added. The A4R4G4B4 table row and the
  `blend_values` size/offset assertions moved with the code.
- **C13 cost.** The flush only runs on the fallback path (no surface or cached
  texture covers the display range), so it does not add a readback to the
  common present. When it does run, it waits for the copies with
  `flush_for_readback()` before the CPU read, exactly as the VK path waits on
  its command buffer. `invalidate_range(read)` runs even when nothing was
  flushed, so a stale section cannot survive into the next frame.
- **Next measurement.** Run the Demon's Souls capture with the same probe
  configuration (`MTL_PROBE_FRAME_COUNT` raised for a ~5 s window) and compare
  `classify-present.py` against the `ds24e` baseline (`stable 125 / absent 78 /
  floor-white 22 / flash 7 / blur-left 0`). The seam is still expected to
  survive: T5 is not in this change set.

