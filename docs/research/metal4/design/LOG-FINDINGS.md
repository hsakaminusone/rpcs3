# Metal 4 run-log findings — Ridge Racer 7 (`BLUS30001`)

**What this document is.** The objective results of the Metal 4 investigation, open items first.
`L<n>` ids are stable because other documents cite them; resolved rows are compressed, not deleted.

**Current state at a glance.**

- The backend renders the game end to end. The intro FMV, the Xevious panel, the road, the sky,
  the HUD and the early upload/present paths are correct (L116, L95, L96, L22).
- **The remaining visual defect is intermittent black patches on cars and environment during races,
  plus per-glyph loss in menu text and a smeared rear window.** None is root-caused yet. The menu
  text is the sharpest lead. A pixel diff between the F12 surface at frame 2493 and the next
  presented frame shows the loss is **real and one frame wide**, not a readback tear; the F12 path
  reproduces a good frame exactly (shot 1521 = presents 1522–1526, 2009 bright px each). The left
  list panel **is** drawn — a 9-slice plus atlas glyph quads; the earlier "no draw covers the panel"
  was a decoder y-flip, not a missing draw. The loss is *inside* three glyph quads: the surviving
  pixels are a subset of the correct glyph, and the affected glyphs **move between frames and
  between cells** (present 2494 loses a right-panel glyph that frame 2493 draws correctly), so this
  is a sampling or visibility fault, not a lost draw (L153, L155).
- **The L145 FIFO class is closed** (GET is an acknowledgement; per-stream frontier; run V: zero
  `Dead FIFO`, zero desync, zero `FIFO error`, six redirects).
- **A second, distinct FIFO class is closed (L149):** the guest's semaphore-wait trampoline is entered by
  `CALL`, then `JUMP`s into a command chunk that itself `CALL`s a static routine. Hardware has one
  return-address register and overwrites it; RPCS3 rejected the nested `CALL`, replayed the stale
  outer return, and tripped the Dead-FIFO guard (21 recoveries in 2 s → main-thread fault). A `CALL`
  now overwrites `fifo_ret_addr` and `stream_tracker::on_call` saves the newest caller's stream, so
  `add()` cannot recycle the stream the parser is in. Upstream `master` has the same single-level
  restriction; the fix is a deliberate, spec-correct divergence.
- **The level-5 capture now reaches back in time** (in-RAM ring, per-draw records, all 18 rings,
  texture bytes at bind). The second probe run produced captures at frames 182/1416/1814/1932/2956.
  The 3950 F12 press hit the five-window limit, so the race defect frame itself is not captured
  (L151).
- **Current status (2026-11-07).** The ordering class is measured out: `MTL_PROBE_SERIALISE=1` +
  `MTL_PROBE_TRACKED_HEAPS=1` at ~4 fps still glitched, so frame overlap, missing GPU edges, TBDR
  visibility and cross-frame hazards are exonerated. Metal API validation (load/store actions,
  unretained resources) reported zero errors; `MTL_SHADER_VALIDATION=1` still crashes at launch
  (L78/L79), so GPU validation is unavailable. The red-texture run is **requalified**: it proves the
  bind path reaches the shader, not that content, UV, LOD, wrap, filter or aliasing are innocent
  (L160). The nested-CALL crash (L149) is fixed, so a normal race run can proceed; the teardown
  fault (L159/B25) still ends every run [fixed 2026-11-08, L165]. Most captures and logs were cleared on 2026-11-07
  (27 GB); `dumps-capture-20261107-132727` is the surviving level-5 capture. A code audit of the
  texture path found three real defects (C1-C3, §1.1.1); all three are fixed with GPU-free tests
  (L161-L163). None touches the CPU-uploaded BC atlases that carry the visible defect, so the next
  work is measurement: the replacement-texture probe now has UV and block-checker patterns, and
  `MTL_PROBE_FORCE_LOD0` pins every sampler to level zero (§1.1.2 P1).

**Method.** Probe log, RPCS3 log and crash reports; bounded captures. Probe levels: 2 = counters,
3 = per event, 5 = per draw + dumps. A window is armed by a cubemap bind, a large B8 bind, a
present-gap watchdog, a content trigger, or `MTL_PROBE_AT_SEC`, and probes at most 64 draws (L113).
Every number is measured unless the row says *hypothesis*.

**Artifacts.** `build/probe/run.log` and `build/probe/run-flicker.log` (probe),
`~/Library/Caches/rpcs3/RPCS3.log` (emulator). **`build/probe/dumps-capture-20261107-132727/` is
the surviving level-5 capture** (F12 windows at frames 4166/6150/6189; 6189 carries the draw
records); the older `dump/` and `dump-flicker/` captures and most raw logs were cleared on
2026-11-07. Decoded evidence is kept in `build/probe/tex/`, `crops/`, `shots/` and `reference/`.
Validation launchers: `build/probe/run-validate.sh` (`MTL_VALIDATE=1` is API only; `MTL_VALIDATE=full`
adds GPU validation, which crashes at launch) and `run-capture.sh`, `run-red.sh`, `run-red-units.sh`,
`run-noalpha.sh`, `run-dumppipes.sh`.
Analysis: `build/probe/capture.py`, `diff.py`, `texture.py`, `png.py`, `fifo-ring-decode.py`,
`flicker.py`, `transient.py`, `montage.py`.

**Projection convention.** `capture.py:project()` returns **viewport-space** coordinates. The UI
viewport is `[0,720,1280,-720]`, so a dumped image row is `image_y = height − projected_y`
(L155). Panel and glyph intersections must use that mapping.

**States.** `open` · `fixed` (code changed) · `fixed (device-confirmed)` · `settled` (measured, no
action) · `superseded` · `removed`.

---

# 1. Open

## 1.1 The remaining visual defects

The FMV, the Xevious panel and the early car silhouette are closed (L116, L95, L96). What remains is
a family of black or missing pixels that appears in every scene:

| id | finding | state |
|---|---|---|
| L147 | **Menu text loses individual glyphs; the title-screen line is correct.** The 2026-10-31 screenshot reads `S art Race`, `P e-race Confirmation`, `ON_INE BATTLE`, `R dg e St te G AND PRIX`: uppercase glyphs survive and single letters disappear inside words. A null or invalid bind would black the whole string, and a stale atlas would still show glyphs, so the atlas is bound and mostly correct; individual cells are black. Candidates, in order: the glyph atlas is partially uploaded and the missing cells were never written; a per-frame atlas update races the sample; the atlas texture is aliased by another guest surface at the same address. **First measurement:** at a failing menu draw, dump the bound atlas (level-5 `guest-*`/`host-sample-*`) and compare the missing cells with guest memory. **Measured 2026-11-06 (L155g):** the cells that lose ink decode full in the guest, and all six host copies are byte-identical, so a partial upload and a stale atlas are excluded; the surviving candidates are per-frame visibility or a sampler-read hazard (L160). | open |
| L146 | **The rear window is smeared.** Reported by the user; not yet captured. Consistent with a content-transfer hazard on a render target that is resolved or copied before the write is visible (the unconfirmed `MTLSurface.cpp` barrier fix is the first candidate), but no draw, pass or surface has been identified. Needs a level-5 capture at the car/rear-window scene and a pass/surface census. | open |
| L98 | **The car is drawn and then shaded black.** Mid-frame dumps at draws 207 000/207 200/207 400 showed the menu, text and background normal and the car as a correct **silhouette** filled with black plus a few coloured speckles. Geometry, cull and depth accepted it; the colour is lost in the fragment inputs or their math. The current report is intermittent black patches rather than a full silhouette, and the car has not been re-captured since L116; L97/L105/L99 remain the last measurements of the colour path. **Measured 2026-11-07:** `MTL_PROBE_REPLACE_UNITS=0` removes the car and environment patches, so unit 0 carries them; the uniform-red run cannot separate UV/mip from content (L160). | open |
| L97 | **The car's paint texture is one BC1 block repeated** (`lf 84 1f 84 00 00 00 00`, `c0 == c1 == 0x841f`, indices 0) in guest and conversion alike. Faithful copy; whether that colour is the intended paint is a shader/constant question. | open |
| L105 | **The car-select probes are not on the car.** The patch goes 115,78,44 → 108,74,44 → 102,71,43 under the car draws, then `fp 21 vp 15` darkens it to 48,32,20 and `fp 67 vp 31` to 40,28,18: it is background that a later draw overwrites. L99 is a prerequisite. | open |
| L99 | **The presented surface holds three panels while the screenshot holds one.** `host-present-1800-*` is 1280×720 with three menus at x = 0/427/853 (77 % of bytes differ, so three renders); the F12 screenshot is one full-size panel. The Metal viewport and scissor are the full 1280×720, so the thirding is guest-side. Decides where a car probe must sit. | open |
| L89 | **No referenced unit is lost and the numbering agrees** (`texture/resolved` 471 712, `retained` 616 234; `bind null`/`dropped` never fire; `[[texture(12+i)]]` = `texture_slot_base + i`). Unmeasured: the content of the image bound at the car draw, and whether the car program samples that unit. | open |
| L49 | **A texture is bound at guest address `0x00000000`** — DXT1 8×8, all-zero content, and `texture/bind null` stayed 0, so the descriptor carried a handle. | open |
| L50 | **The video surface is allocated at screen size and sampled at video size** (host 1280×720 at `0xc0880000`, guest descriptor 1408×768). Unclear whether L41/L43 closed it. | open |
| L153 | **The per-glyph loss is a real one-frame surface defect.** Shot 2493 (the F12 surface) and present 2494 differ in the left list panel; the largest component is the `a` of `Transmission`, where 2493 shows the panel background and 2494 shows the glyph. `flush_for_readback()` commits and re-enters the slot, so this is not a readback tear, and the screenshot path reproduces a good frame exactly (shot 1521 and presents 1522–1526 all measure 2009 bright px in the panel), so the 2493 loss is in the frame. **Correction:** the earlier "no recorded draw covers the list panel" was a decoder y-flip artifact — the panel *is* drawn, 487209–487235, and the loss is a partial quad rather than a missing draw. See L155 for the corrected census. | corrected (L155) |

The working hypothesis is no longer a single mechanism. The menu-text loss is per-cell and the race
patches are block-shaped and vary per frame, so the two may share a surface-cache or partial-upload
cause rather than a shader cause. The glyph evidence narrows it further: the quad is rasterised and
the atlas is bound, but part of the sample comes back transparent, and the affected cells change
from frame to frame. That is the signature of a **read hazard on data the draw depends on**, not of
a wrong draw or a wrong texture. Both discriminators have now run: serialise plus tracked heaps
still glitched, so the hazard is not frame overlap and not a missing GPU edge.
L148 (scissored clear) was a real correctness defect but its visual
attribution is **measured false**: the device run after the fix kept the same patches, and the
`clear/scissored fill` latch stayed at zero. L148 stays a correctness fix only.

### 1.1.1 Code-audit status and plan (2026-11-07)

The audit covered `MTLTextureCache`, `MTLHandles`, `MTLSamplerState`, `MTLArgumentBinds`,
`MTLPassScheduler`, `MTLSurface`, `MTLResourceManager`, `MTLFormats` and the shared
`TextureUtils` upload. It found three real defects, and all three are fixed (`L161`-`L163`) with
GPU-free tests in `rpcs3/tests/test_rsx_metal4_state.cpp`. None is shown to be the visible defect:
all three sit on paths that the race patches and the glyph loss do not obviously use (C1 is the
reused, non-shader-read clear path; C2 is the GPU-to-CPU readback; C3 is 3-D reuse). The rows keep
their original hypotheses and now record the fix and the test that covers it.

| id | defect | mechanism | fit to the symptom | next test |
|---|---|---|---|---|
| C1 | **`clear_texture` computes compressed mip extents with floor division.** `MTLTextureCache.cpp:310-322`: `row_bytes = (level_width / 4) * block_size`, `rows = level_height / 4`, and a level whose computed size is zero is skipped. | A level below 4 texels, or a non-multiple-of-4 extent, is under-covered or not written at all. The texture is `StorageModePrivate`, so a skipped level keeps undefined contents; the `copyFromBuffer` size is the texel extent while the staging row is the floored block extent, so the copy can read past the staging allocation. | The host chain is only cleared for a reused section whose context is not `shader_read`. The race atlases and the glyph atlas are uploaded once and never take that path, so this is not the primary path; it would matter for a framebuffer-storage BC section and for any `max_lod` that reaches an undefined level. | **Fixed (L161).** `get_texture_level_extent` rounds up to whole blocks with a minimum of one, shrinks 3-D depth per level, and drives both the staging size and the copy size; the clear also takes its base extent from the host image and its face count from the texture type. Tests: `MTLTextureLevelExtent.*`. |
| C2 | **`copy_texture` uses a block index as a texel origin for compressed formats.** `MTLTextureCache.cpp:410-415`: `transfer_x = (offset % rsx_pitch) / block_size`, then `Origin::Make(transfer_x, …)` and `transfer_width -= transfer_x`. | Byte offset divided by block size yields blocks, but the origin and the width are in texels, so a partial compressed readback starts and ends at the wrong place. | Readback only (F12, screenshots, CPU reads). It cannot corrupt on-screen sampling; it can corrupt the evidence used to diagnose it. | **Fixed (L162).** `get_texture_readback_region` converts the byte offset to a block origin and scales by the block edge, keeps the raw byte offset as the staging destination, and scales the tail row count the same way. Tests: `MTLTextureReadbackRegion.*`. |
| C3 | **The reuse test ignores depth and array length.** `MTLTextureCache.h:487-493` compares type, pixel format, width, height and `mipmapLevelCount() == mipmaps`, but not `depth`. | A 3-D section re-created at the same address with another depth reuses the old allocation, and the upload/copy then uses the new depth/slices against it. It also compares the raw guest mip count with the clamped host count, so a legal request whose chain was capped is rebuilt every time. | The unit-0 objects seen so far are 2-D or cube, so this is not the race path; it is a correctness and performance issue for 3-D titles. | **Fixed (L163).** The reuse test compares against `normalize_texture_extent(...)`: host depth is checked and a capped chain is accepted. Tests: the `MTLTextureExtent.*` cases cover the normalisation the predicate now uses. |
| C4 | **Aliasing is ordered but not invalidated.** `MTLPassScheduler` inserts the barriers; `MTLGSRender.cpp:1831`/`1915` call `notify_surface_changed(range)` at a framebuffer bind. | If the range handed to the texture cache is narrower than what the GPU will later write (AA factors, depth range, pitch), a sampled section keeps stale bytes while the barrier only orders access. | Matches "moves per frame", but the captured atlas samples were byte-identical to the guest, so any staleness would have to live in the texture object or in a missed *later* write, not in the bytes at bind. | **Checked; dismissed as a code defect.** The read range is the texture's guest footprint (`get_texture_size`, all mips and faces); the write range is the surface's level-0 footprint. Both cover every guest byte the draw can touch, and the same Metal object matches by identity even when the footprints differ. A read of mip levels beyond a surface write is stale by contract (the guest never wrote them through that surface), not an ordering miss. The remaining evidence need is the capture intersection, not a code change. |
| C5 | **The fragment alpha test is a per-pixel kill and the glyph loss is a subset of the glyph.** `MTLFragmentProgram.cpp:363-415`, `612-615`. | A kill is exactly the shape of "the quad rasterised, only some pixels survive". The glyph draws share program, constants and sampler, so a moving loss would need a per-draw state difference or a race. | The one run with the test compiled out was inconclusive (black from boot, audio and logic alive). | A menu-only run with the alpha test off, booted to the menu before the switch takes effect, and the alpha function/ref logged per subdraw. |
| C6 | **`max_lod` is clamped to the guest mip count, not the host count.** `MTLSamplerState.cpp:168-176` uses `texture.get_exact_mipmap_count()`. | If `normalize_texture_extent` capped the host chain, `max_lod` can name a level the image does not have. Metal clamps to the last resident level, so this is only unsafe when that level is undefined (C1). | Not a cause on its own; it is the route by which C1 could reach the sampler. | Open hypothesis. C1 now defines every level it clears, and `MTL_PROBE_FORCE_LOD0` (state-only, no archive suffix) pins the sampler to level zero so a run can tell a mip/LOD fault from a post-sample one. |
| C7 | **The readback and upload copy only slice 0 and level 0.** `MTLTextureCache.cpp:copy_texture` copies one `copyFromTexture` with slice 0 and level 0; `upload_texture_data` loops the shared subresource layouts. | A section with several faces or levels would read back only the first subresource. | Not a defect against the reference: the shared cache flushes one section at a time and the Vulkan backend also copies only the base image/level (`VKTextureCache.h:212-300`). A section that the GPU wrote is a surface and has one level; guest-mip textures are uploaded, not read back. | **Checked; dismissed.** The cube clear and upload already repeat per face/level; readback keeps the reference's single-subresource contract. |

The suspects the audit did **not** find are as important as the ones it did: the bind path
writes every changed argument-table slot and resets both binders per frame (L107), the view cache
keys on the composed swizzle, and the format map matches VK/GL for every guest format in the
census. C4 and C7 were re-read against the core and the Vulkan reference and dismissed as code
defects (`L164`). The live hypotheses are a sampling-side fault that the red texture masked (L160)
and the visibility of a later write that only a capture intersection can settle.

### 1.1.2 Next steps and plan

**P0 — make a long run possible.**
1. **Done (L165; 2026-11-08).** The `0x98` fault was not a device-lifetime bug: it was an
   `on_exit` invalidation readback encoded on a command buffer that had already been ended. Audit
   the release order produced nothing; the backtrace did. `frame_slot` now tracks `buffer_open`
   and the encoder-open paths re-enter a closed slot first. Two consecutive clean quits, no fault.
2. **Done.** The uncommitted range-scheduler redesign was kept: it is what §1.1.1 C4 relies on
   (aliased guest ranges ordered through the hazard records), it passed the state tests, and it was
   committed in `144e57624` rather than reverted. The instrumentation and the aliasing tests stay.
3. Every run follows the one-instance rule (`build/probe/rpcs3.pid`, never `pkill -f rpcs3`).

**P1 — discriminate sampling from content from visibility (cheap, decisive).**
4. **Ready to run.** `MTL_PROBE_REPLACE_TEXTURES` now takes `1` (one-texel red), `uv` (a red/green
   coordinate gradient with a blue parity grid) or `checker` (a four-by-four-block index encoded
   in colour). Run one race window with `MTL_PROBE_REPLACE_TEXTURES=uv MTL_PROBE_REPLACE_UNITS=0`.
   If the patches become the *wrong part of the pattern* in place, the fault is UV/mip/sampling;
   if the patches stay black and rectangular regardless of the pattern, the fault is after the
   sample (blend, ROP, visibility).
5. **Ready to run.** `MTL_PROBE_FORCE_LOD0=1` is a state-only switch (point filter,
   `max_lod = min_lod = 0`, bias 0) that pins every sampler to level zero; it needs no archive
   suffix. If the splotches vanish, the mip/LOD/filter path is implicated and C1/C6 move up; if
   they remain, mips are exonerated.
6. Decode the surviving capture (`dumps-capture-20261107-132727`, window at frame 6189): compare
   the unit-0 atlas guest bytes with the host upload, read the per-draw `texture_id`/`sampler_id`,
   and intersect every sampled range with the colour/depth target ranges of the glitch frames.
   Acceptance: a row stating whether any overlap exists and whether the Metal texture is shared.

**P2 — fix the confirmed code defects and add tests.**
7. **Done (L161-L163).** `get_texture_level_extent` computes compressed extents in whole blocks
   with `ceil` and a minimum of one block, shrinks 3-D depth per level, and drives both the staging
   size and the copy size; the cube clear repeats over all six faces. `get_texture_readback_region`
   converts a byte offset to a block origin and keeps the raw byte offset for the staging
   destination. `fits` compares depth and the clamped mip count through `normalize_texture_extent`.
   GPU-free tests are in `rpcs3/tests/test_rsx_metal4_state.cpp`; the filtered suites pass.
8. Re-run API validation and one race window after C1-C3. Expected: no visual change unless C1 is
   on the path; record the result either way, because a null result raises the sampling-side
   rank.

**P3 — only if P1 and P2 do not close it.**
9. Split the menu defect from the race defect: menu-only run, alpha test off (C5), then single-mip
   and point sampling. A menu-only fix is a separate finding from the race patches.
10. Audit the shared `texture_cache` upload, partial-flush and section-invalidation path against
    VK/GL using the capture's actual sections rather than a static read. A metal-cpp harness can
    drive `copy_transfer_regions` and the compressed sizing without the emulator.

**Docs and commit.**
11. Append each device result to §1.1 and §1.1.1, changing a state rather than deleting a row; keep
    `L` ids permanent.
12. Commit the L149 fix, tests and docs with `hsaka -1.0 <340957754+hsakaminusone@users.noreply.github.com>`;
    keep C1-C3 in their own commit if they change behaviour.

## 1.2 FIFO: the nested-CALL desync

The L145 class — GET publishing a prefetched block end, so the guest recycled a chunk the parser was
still in — is closed and device-confirmed. The capture run exposed a **different** desync at 6:54.9,
after 21 recoveries in 2 s tripped the `Dead FIFO` guard and the main thread segfaulted.

| id | finding | state |
|---|---|---|
| L149 | **A nested `CALL` is a legitimate hardware tail call; RPCS3 rejected it.** The guest enters the wait trampoline with `CALL 0x1544` from `0x10d1ffc` (return address `0x10d2000`); `0x1544` is `NV406E_SEMAPHORE_ACQUIRE`; `0x154c` is `JUMP 0xcb0000`. The parser walks command buffer A (18-command dispatch history `0xcb05c8…0xcb07dc`) to the genuinely aligned header `0x00c9a982` = `CALL 0xc9a980`, a mapped static routine. Hardware has one return-address register: the inner `CALL` overwrites the outer return, which is dead because A ends in a `JUMP`, not a `RET`. RPCS3 (and upstream) rejected the second `CALL` with `CALL found inside a subroutine`; `recover_fifo` restored the stale outer return and failed identically (21 recoveries in 2 s → `Dead FIFO` → fault at `IOGPUDeviceGetNextTraceID`, and the same desync halted the all-probes run at 139 s in the race). **Fix:** `RSXFIFO.cpp` — a `CALL` overwrites `fifo_ret_addr` (nested calls are legal, as on hardware), and `stream_tracker::on_call` saves the newest caller's stream unconditionally, so `on_return` restores the chunk the parser actually occupies and `add()` cannot recycle it mid-parse. The outer frame is dead in both models. Verified by `FifoStreams.NestedCallKeepsTheChunkItReturnsInto` (fails pre-fix, passes with it). Deliberate divergence from upstream's single-level guard. | fixed |
| L150 | **The 507 ms acquire stall was the timing trigger, not a cause.** `acquire stall 483` was logged at `0:06:54.702` (507.3 ms, one drawable in flight); the producer then rewound PUT `0x10d1ffc -> 0xce3434` at `0:06:54.733`, the trampoline jump ran at `0:06:54.765`, and the nested `CALL` failed at `0:06:54.765`. Larger stalls (654.831 ms, 630+ ms) ran without a desync, so the stall alone was not sufficient: it only selected the `CALL`-to-trampoline path. The class is closed by the L149 fix; the parser state is correct either way. | superseded (L149) |
| L115 | **The two early FIFO fixes did not remove the old desync.** Run 2 still hit one at 53.451 s: `get=0x1490000 put=0x10a588c get_guest=0x51590000 restore_point=0x10a5814 code_start=0x10a5478`. Superseded by L145 for the old class; kept as the pre-L145 baseline. | superseded (L145) |
| L20 | The 0:00:52 desync re-walks one word (`0x2fd53cc`, then `0x3f800000`) and exhausts the 20-in-2-s limit: GET is reading data, not commands. | superseded (L145) |
| L35 | The FMV handoff stops the RSX thread: `stop_audio`, `NV3089_IMAGE_IN` NOPed, `possible desync (0xbf800000)`, then no further work (recovery absorbs it and waits on an empty FIFO). The FMV itself now renders (L116); the handoff desync is folded into the L145 class. | superseded (L145) |
| L52 | The words in the error window are **all data**: as headers they name undefined methods 0x69A–0x69F. | superseded (L145) |
| L54 | **Recovery makes no progress**: `restore_point == get`, so `set_get` re-runs the same word; after seven retries GET reaches the end of the iomap and the error becomes fatal. | settled (see L121) |
| L57 | The data words are the guest's vertex attribute data and format registers (`10030303`, four undefined 0x1A00–0x1BFF indices). | superseded (L145) |
| L58 | In 3 of 4 captures the error is exactly `0x39c` bytes (231 words) after `code_start`, the last jump target — the same block is mis-parsed at the same word. | superseded (L145) |
| L64 | `cmd=0x3f48c8ca` passes the call test (`& 3 == 2`, target `0x1f48c8c8`), 300 MB past PUT and outside the ring → `get_guest=0xffffffff`; recovery walks into it again. | superseded (L145) |
| L82 | `get=0x10b4e00 cmd=0x1d3`: neither method, call, return nor legal jump (`ret_addr=0x3`); the stream resumes five words later. | superseded (L145) |
| L86 | **Self-sustaining loop, then a self-call**: `restore_point == get` for five captures, then a call to its own address; `restore_point` is only refreshed at sync points. | settled (see L121) |
| L118 | **GET is one word past a correct boundary, always `0x39C` (231 words) after `code_start`.** Both episodes of the last `Atomic` run walk exactly 231 words from the last jump and die on the 232nd: A `get=0xcb0e20 code_start=0xcb0a84 cmd=0x10030303`, B `get=0x10a28cc code_start=0x10a2530 cmd=0x00010000`. The last six dispatches are the same block in both (`reg 0x19=0x10`, `0x1A=0`, `0x75B=0x800`, `0x75C=0x10000`, `0x19=0x800`, `0x1A=0`), and the parser's 256-entry history is internally consistent throughout (`d=+8` for one-arg commands, `d=+4` inside multi-argument groups), so the slip is upstream of the bookkeeping. | superseded (L145) |
| L120 | **`fetch_u32` had no guard for GET at or past PUT.** Only `put == addr` was treated as EMPTY; the `addr < put` trim never applied when GET was ahead, so the cache was filled to the end of the 128-byte line holding PUT and every word past PUT was served from unpublished memory. One mis-parse that stepped over PUT was therefore unrecoverable, and 4.07 MB of walking is what L83 saw. The consumer of the same state is visible in `nv4097.cpp`: `fifo_read_limit` says "if put is behind get it will result in a number ignored by min". The per-stream frontier (L132) and honest GET (L145) close the state. | fixed (L145) |

**Why the nested call is not a parser bug.** The two command buffers are `0xcb0000` and `0x10a0000`,
exactly `0x3F0000` apart (L122). In the normal cycle the parser walks one buffer from its base to a
`JUMP` at the tail, follows it to the other base, and repeats (L123). The `CALL`-to-trampoline path is
the guest's wait idiom: it is used when the producer must wait for a label before continuing. The
parser's position bookkeeping is exact throughout (L124); the only thing that changes is that the
return-address register is already occupied when the chunk's own `CALL` arrives.

**Probe audit (2026-11-03 run).** `RPCS3.log`: 1778 parked, 38 FIFO redirect, 1733 PUT move, 1729 PUT
rewind, 3444 jump cross-buffer, 3 jump high-water, 4 chunk tail, 142 label, 16 watchdog, 484 present
gap, 16 cubemap, 125 failed, 1 skipped, 487 stall, 268 device, 5 archive, 6 timeout, 40 hang, 15 dead.
`build/probe/run.log`: 252 desync lines (21 captures × 12), 4096 `redirect` are the per-draw geometry
field (not FIFO), 21 label, 1 watchdog, 135 present gap, 27 refused, 2 skipped, 162 stall, 27 device,
53 archive, 1 dead. The 38 redirects are the producer abandoning chunks mid-command; the word at the
frontier is often a valid method or `0`, not a link. They do not by themselves explain the nested
call, but they are the same producer-restart behaviour that makes the `CALL` path reachable.

## 1.3 Presentation and performance

| id | finding | state |
|---|---|---|
| L14 | **Cold boot stalls at the title video** (frame 306, 75 pipeline misses; 302 of 306 flips refuse the compose because no display buffer is configured). Warm boot renders. | open |
| L31 | **10.1 % of flips present a clear** (`compose ran` 3 420 + `refused` 383). Run 1: 7 042 flips, 6 219 composed, 823 refused; run 2: 535 refused. | open |
| L40 | **The video plane comes from a path that almost never runs**: the 1408×768 A8R8G8B8 surface is sampled with `upload_context = framebuffer_storage` on 223 of 669 binds, but only 2 readbacks in 300 frames, so the sampler sees whatever the last readback left. | open |
| L47 | **836 guest faults per run**, all handled, 600 µs average, **136.5 ms maximum**; the RSX-thread case flushes in place. Run 1: 1 059 "writing 1" lines in the 100 s pause. | open |
| L48 / L30 | **The semaphore wait is a spin**: 65 297 798 iterations in 300 frames (218 k/s, 1.93 s CPU), ~190 k per frame. | open |
| L39 | **One texture readback blocked the RSX thread for 134.8 ms** to move 6 291 456 bytes. | open |
| L143 | **`nextDrawable` is a tail, not a 20 ms average.** Run Q's `present/acquire/next drawable` is 4.30 ms over 300 calls, but 922 ms of that is one call; without it the mean is ~1.2 ms. `present/flip` is 7.32 ms avg / 923 ms max and `recorder/present drawable wait` is 267 µs avg / 71.9 ms max. The recurring RSX-thread cost is `frame/gpu wait` 9.14 ms avg and `recorder/begin frame/wait feedback` 2.50 ms avg / 750 ms max. The 922 ms sample is the CAMetalLayer drawable timeout: `nextDrawable` returns nil and the frame is dropped (`MTLPresenter.cpp:172`). VK does not block here: `VKGSRender::present` is a non-blocking `vkQueuePresentKHR`, and its acquire is a 100 ms bounded wait followed by a 0-timeout spin and drop (`VKPresent.cpp:592`). Metal's acquire and `MTL4CommandQueue::wait(drawable)` are both CPU blocks on the FIFO thread, so a dry pool stops the parser for the whole timeout. | open |

The correct comparison is not "Metal acquire is slow"; it is "Metal has an unbounded CPU wait on the
FIFO thread and VK does not". The parser is the FIFO consumer, so any window-server stall on the RSX
thread shows up as a guest command-buffer stall, and in RR7 a stalled parser is also what turns a
normal buffer switch into a producer restart. The fix is to take the window server off that thread,
not to make `nextDrawable` faster. Two designs preserve the current renderer:

1. **Offscreen compose plus a present thread (recommended).** The RSX thread composes the frame into
   a small ring of offscreen textures, signals a shared event, and returns from `flip` immediately.
   A dedicated present thread owns `nextDrawable`, `waitForDrawable`, the final blit and `present`;
   if the drawable pool is dry it drops the oldest composed frame instead of stalling. The RSX
   thread's only new cost is an atomic publish, and the FIFO never waits on the window server. This
   is the same split VK gets for free from `vkQueuePresentKHR`, made explicit.
2. **Bounded acquire on the RSX thread.** Set `allowsNextDrawableTimeout` and skip the acquire when
   the in-flight drawable count is at `maximumDrawableCount`, so `nextDrawable` is never called on a
   dry pool; move `m_queue->wait(drawable)` to a GPU timeline event or to the present path. This is a
   much smaller change but still couples the parser to the pool, and the frame-drop policy becomes
   visible as judder.

Until either lands, the measured steady-state cost is dominated by `frame/gpu wait` and the feedback
wait, not by the acquire; optimizing `nextDrawable` alone would not move the parser.

## 1.4 Instrumentation and capture coverage

| id | finding | state |
|---|---|---|
| L151 | **The level-5 capture reaches back in time, but the defect frame was not captured.** The recorder keeps a 12-frame in-RAM ring of every CPU ring plus one `draw_record` per subdraw, and dumps on F12, a present-gap watchdog, a content trigger, a cubemap bind, a large B8 bind or `MTL_PROBE_AT_SEC`. The 2026-11-03 run captured frames 182/1416/1814/1932/2956 (6/230/234/234/234 files) and `host-shot-3950`. The 3950 F12 press hit `diagnostic_capture_window_limit = 5`, so only the shot image was written; the race defect frame has no ring/draw capture. Raise the limit or reset the budget before the next defect run. `MTL_PROBE_SERIALISE=1` (`wait_idle()` after `open_frame()`) separates a data defect from an overlapping-frame hazard. | open |
| L152 | **The capture decoder was out of sync with `draw_record`.** `capture.py` zipped 62 field names onto the 298 scalars `struct.unpack` returns, so every array field after `pipeline_key` desynchronized and the tool failed with `'int' object is not iterable`; an earlier phantom `_pad` entry had hidden the same class of error. The decoder now carries an explicit `(name, count)` layout, asserts the counts sum to the unpacked scalar count, and regroups the arrays. Record size (`1200`) and the C++ `static_assert` are unchanged. All five captures decode (78 frame/age entries). | fixed |
| L103 | **(a)** the unit trace printed the format's declared layout, not the view swizzle; it now prints the composed swizzle. **(b)** the MSL declares each ring at block size (`vc[1]`, `fc[7]`, `fs_contexts[1]`) but indexes it with the ring index; real UB, proven benign (744 working draws, GPU validation silent), deferred until a shader-codegen change is justified. | (a) fixed / (b) deferred |
| L154 | **The defect frame's inputs are not in the capture.** `host-target-*` is written only at `diagnostic_draws < 24` or every 200 draws while a window is active; the window is armed at the present of 2493 and covers 2494..2509, so frame 2493 has no intermediate target and `host-sample-*` starts at 249400. The defect frame's sampled textures cannot be recovered from this run: a press must arm before the frame, or the window must include one pre-roll frame with samples. Also `record.global_id` is `diagnostic_global_draw_id` (per subdraw), not the log's `diagnostic_draws`, so log draw numbers and record ids are not directly comparable. | open |
| L155 | **The per-glyph loss is a partial quad, and it moves between frames.** **(a) The decoder's y is flipped.** `capture.py:project()` returns viewport-space y; the UI viewport is `[0,720,1280,-720]`, so `image_y = height − projected_y`. Two independent anchors confirm it: the `a` quad of `Transmission` projects to y 519..551 = image 169..201, and the `Machine Select` row 487255–487268 projects to 620..653 = image 67..99. This is why the earlier census concluded "no draw covers the panel". **(b) The panel is drawn.** Frame 2493 p1: 487209–487211 are the panel 9-slice (`c5b4a480`, `c5b46480`, `c5b36480`), 487212–487235 are atlas glyph quads (`c441e080`), and the `Transmission` row is 487215–487226 (12 quads). The only later draws over the `a` cell are 487244/487245 (`c5b5b480`, x 162..272), which reach x 271–272 of the 271..288 cell — they cannot explain the loss. **(c) Three of the 12 quads lose pixels, and only pixels the glyph covers.** Bright-pixel counts (>200 on every channel) per cell, shot 2493 vs present 2494: `a` 18/93, first `s` 47/82, first `i` 4/30, `T` 64/68, and the other eight cells exact. The surviving pixels are a **subset** of the correct glyph — `a` keeps only its bottom serif (rows 192–193), `s` keeps rows 179–183 and 192–193, `i` keeps the dot (174–175) and rows 192–193. A lost draw, a wrong transform or a wrong UV would not produce a subset, so the quad rasterised and the atlas sample returned alpha 0 for part of it. **(d) It is not the cell, the texture or the sampler.** All 12 quads carry identical recorded state (command 1, primitive 8, 4 verts / 6 indices, flags, blend/depth/raster hashes, `pipeline_state` 518034583552, `pipeline_key` 723067336, fragment constants, sampler hash 122444642, atlas `c441e080` 2048×2048 BC2, mip 1, lod 0..15); only `global_id`, the ring offsets (`attrib_offset`, `vs_context_offset`, `fs_context_offset`, `fs_texture_base_index` 1920..2096 step 16, `fs_stipple_offset`) and `hash_persistent` differ. The same `s` and `i` cells render correctly at 487222/487223/487224 in the same frame. 487217's vertex bytes and transform are identical between m1 (486890) and p1 (487217); only `color_target` (`0xC0000000` vs `0xC0880000`) and `hash_sampler` differ. **(e) The loss moves, and it is one frame wide.** The right-panel cell 487290 (x 990..1010, y 523..556) measures 127 bright px in shot 2493 and in presents 2495..2509, and **32 in present 2494** — a single-frame loss in a cell that is otherwise identical in the other 16 samples (shot 2493 and presents 2495..2509). The failing frame is captured: present 2494 is `p2` (frame 2494), and its corresponding quad 487617 differs from p1's 487290 and p3's 487943 only in `global_id`, `frame_id`, `color_target` and `hash_sampler`. `hash_sampler` hashes the whole 48-byte `sampler_info` array for *every* bound unit (`MTLGSRender.cpp:4351`), so it moves whenever any unit's sampler registers move and is not a cross-frame discriminator. **(f) A raw shot-vs-present diff is not a defect mask.** The panel is translucent over an animated glow/noise blob (the 2493-vs-2494 density map differs across an ellipse of roughly x 175..355, y 90..195), so only the static glyph cells are comparable. The left-panel cells are constant at their full count in presents 2494..2508 and drop to 0 at 2509, which is the scene transition, not a loss. **(g) The frame-1506 census reproduces (c) exactly and proves the inputs are identical.** Shot 20:15:07 reads `Tr n m.ssion`; host-present-1507 is correct. The missing draws are `200888` (`a`, x271), `200890` (first `s`, x303) and `200892` (first `i`, x343, dot only); the other nine glyphs are present. All twelve glyph draws record identical state; p1:200888 and p2:201248 are identical in vertex context, fragment context, stipple, fragment constants, transform constants, persistent vertex bytes and index bytes — **only `color_target` differs**. The atlas is correct: `0xc441e080` decoded BC2 cells for `a` (x758..782), `i` (x950..960) and `s` (x1156..1179) contain full ink, and all six `host-sample-*` copies of the atlas are byte-identical. The atlas was uploaded once at t=27.251 and never again. **Next:** the final run records the Metal `texture_id`/`sampler_id` per unit per draw, per-frame sampled images, and a 150-frame forward window (`MTL_PROBE_FRAME_COUNT`), so the recorded CPU state and the bound objects can be compared across every frame that shows the defect. | open |
| L158 | **The forward capture was one frame wide while the ring burst was six.** `MTL_PROBE_FRAME_COUNT` set only `diagnostic_capture_last_frame`, so the per-frame sampled images and the present readback were captured for the single frame after the press, while `continue_dump()` wrote a hard-coded six-frame burst. The 2026-11-06 run therefore has 21 trigger windows but only 28 `host-present-*` files: 21 active frames plus seven periodic ones. The forward burst is now `dump_frame_count()` = `max(6, capture_frame_count() + 1)`, so `MTL_PROBE_FRAME_COUNT=150` keeps 151 forward ring frames and 150 active frame reads, joined to the trigger frame as `p1`. Each `draw_record` also carries the Metal `texture_id[16]`/`sampler_id[16]` the argument table held (record 1200 → 1456 bytes; `capture.py` decodes and prints them). The atlas/environment images are captured once per frame per address, because `diagnostic_reset_frame_state()` already cleared the dedupe list per frame; this is what makes a multi-frame window meaningful. | fixed |
| L159 | **The RSX thread faults inside IOGPU during teardown.** Every run ends with `Segfault reading location 0x98` on the RSX thread, PC in `IOGPU.framework` at `IOGPUDeviceGetNextTraceID + 40` (`__TEXT 0x1adb45000–0x1adb81000`). A clean run with the probes off (`build/probe/run-clean-3.log`) reproduces it, so it is not probe-induced. The main-thread `.ips` fault at `0x18` is secondary Qt teardown after that. The fault is after `Stopping emulator...`, so it does not affect a captured frame, but it is a real backend defect. | fixed (L165) |
| L160 | **The red-texture run exonerates the bind path, not the content or the sampler.** `MTL_PROBE_REPLACE_TEXTURES=1` turned the whole run red and removed the glitches, but a one-texel uniform red image also removes UV, mip/LOD, wrap, filter, format and aliasing, so the result only proves that the resolved unit reaches the shader. With `MTL_PROBE_REPLACE_UNITS=0` the car and environment corruption is gone while the menu glyph loss and the black minimap remain: unit 0 carries the race patches, the menu glyph unit is distinct, and the minimap is a separate null or zero bind (L49). A patterned replacement (UV gradient, block-indexed checker) and a forced LOD-0/point sampler on unit 0 are the discriminators. | open |
| L161 | **C1 fixed: the clear sized compressed levels with floor division.** `clear_texture` now calls `get_texture_level_extent`, which rounds a DXT level up to whole four-by-four blocks with a minimum of one, shrinks a 3-D level in depth, and drives the staging allocation, the zeroing and the `copyFromBuffer` size from one extent. The clear takes its base extent from the host image and repeats over all six faces of a cube. The old code floored a 2×2 or 1×1 level to zero and skipped it, leaving it undefined in a private texture, and cleared only face 0. Tests: `MTLTextureLevelExtent.*`. | fixed |
| L162 | **C2 fixed: the readback used a block index as a texel origin.** `get_texture_readback_region` converts the guest byte offset into a block row and block column, multiplies both by four for a compressed format, sizes the texel region from that origin, and keeps the raw byte offset as the staging destination; the tail row count is scaled by the block edge too. The old arithmetic started a partial readback at the wrong texel and could shorten it twice. Tests: `MTLTextureReadbackRegion.*`. Readback only, so it changes evidence, not on-screen sampling. | fixed |
| L163 | **C3 fixed: the reuse test ignored depth and compared the raw mip count.** `create_new_texture` now compares the host width, height, depth, pixel format and level count against `normalize_texture_extent(...)`, so a 3-D section re-created with another depth is rebuilt, a cube face stays square, and a legal request whose chain was capped is reused rather than rebuilt every time. Tests: the existing `MTLTextureExtent.*` cases cover the normalisation the predicate now uses. | fixed |
| L164 | **C4 and C7 are not code defects.** C4: the read note carries the texture's full guest footprint (`get_texture_size`, every mip and face) and the write note the surface's level-0 footprint; both cover the bytes the draw touches, the same Metal object matches by identity regardless of range, and mip bytes a surface never wrote are stale by contract, not by a missed barrier. C7: `copy_texture` reads one subresource because the shared cache flushes one section and the Vulkan backend does the same (`VKTextureCache.h:212-300`); the cube and 3-D cases are covered by the upload and clear loops, not by readback. Both stay in §1.1.1 with their dismissal so a later run does not re-open them by accident. | closed |
| L165 | **L159 root cause: the exit-time invalidation was encoded on an ended command buffer.** The fault chain was `on_exit → do_local_task(lock_wait) → handle_invalidated_memory_range → invalidate_range → copy_texture → command_recorder::open_compute → command_buffer->computeCommandEncoder → IOGPUMetal4ComputeCommandEncoder initWithCommandAllocator:+0x58`, fault reading `0x98`. `on_exit` drains the pending XDR-unmap invalidation after the frame was submitted, so `open_compute` touched a slot whose command buffer had already been `endCommandBuffer`-ed; the readback was the crash, not teardown of a released device. Fix: `frame_slot` tracks `buffer_open`; `begin_frame`/`reenter_slot` set it and the three `endCommandBuffer` paths clear it; `open_copy_encoder` and `open_pass` re-enter the slot first when its buffer is closed. Two consecutive clean quits, no fault. `Utilities/Thread.cpp` also gained the env-gated fatal-fault backtrace (`RPCS3_TRACE_ON_SIGNAL=1`) that made the chain readable. | fixed |

**Capture contents.** Per captured frame: all 18 rings (`persistent`, `volatile`, `index`,
`instructions`, `overlay-vertices`, `overlay-uniforms`, `vertex-context`, `draw-parameters`,
`fragment-context`, `sampler-info`, `stipple`, `blend-values`, `fill-values`, `vertex-draw-params`,
`fragment-constants`, `transform-constants`, `instancing-constants`, `instancing-lookup`), the
`draws.bin` records, ring generations and `grew_mask`, retired buffers, target/present/shot images,
and texture bytes at bind. `capture.py` projects vertices with the same frequency/modulo rules as
`RSXVertexFetch.glsl`; `diff.py` diffs two frames; `texture.py` decodes the Metal format constants
(`0x50` BGRA8, `0x46` RGBA8, `0x0a` R8, `0x82` BC1, `0x84` BC2, `0x86` BC3).

**Capture safety.** `MTL_PROBE_DUMP_DIR` must point inside the repo, never `/tmp`. Run at most one
emulator instance; track it through `build/probe/rpcs3.pid`, never `pkill -f rpcs3`. Diagnostic
shader variants carry an archive suffix and never touch the real archive; state-only switches do not.

---

# 2. Fixed by this investigation

| id | defect | fix |
|---|---|---|
| L10 / L1 | Cube descriptor set `arrayLength = 6`; Metal requires 1, aborting inside `heapTextureSizeAndAlign`. | Legal extent (one texel minimum, square face, capped mips); the six faces ride the upload slice. |
| L13 | The label release raced the FIFO. | `supports_host_gpu_labels = true`; the release waits its slot, then writes. |
| L19 | The sampled-texture dump computed a DXT span 16× too large. | Block dimensions. |
| L41 / L43 / L44 | **Metal never cleared a reused image**, and neither Metal nor GL checked extent/mip/format of the reused image. | Metal clears a reused image that was not fully uploaded and discards a non-fitting one. |
| L60 / L66 / L71 | **63 consecutive draws were refused** because `begin()` never prepared the framebuffer. | `begin()` binds `context_draw`; the bind fast path tests context, `rtt_config_dirty`, `attachments.valid()` and `ignore_change`, like VK/GL. |
| L70 / L55 / L58 | **The accurate FIFO retry was off by one**: it left the position on the argument, so the retry read the second argument as the first. | On the empty path the position rewinds to the header and `m_remaining_commands` clears. |
| L72 | **Metal 4 captures the argument table per draw at encode time**; an unwritten slot keeps the previous draw's binding. | `argument_binder::address` writes whenever the value changes, zero included. |
| L108 | **The FIFO word cache could serve a stale word**: the control-command shortcut set the cache bounds without storing the word. | The shortcut writes the fetched word back in guest byte order. |
| L90 | The probe file opened with `"w"`, truncating every run. | Follows `MTL_PROBE_LOG`. |
| L109 | **No diagnostic separated "discarded" from "never ran".** | `MTL_PROBE_NO_ALPHA_TEST` compiles the alpha test out (own archive). Its depth half was withdrawn (L112); the surviving discriminator is L114. |
| L92 | **The diagnostics perturbed the frame they measured**: four sites read guest memory with `vm::read32`, faulting the `PROT_NONE` target and flushing it. | All four read through `vm::get_super_ptr`. |
| L93 | **The pipeline archive suppressed every shader dump.** | The archive is moved aside before a capture run; dump limit 512. |
| L33 / L34 | The probe could not see correctness; fourteen latches used a ternary name. | Per-draw id/format/readback records; plain latches. |
| L78 | `MTL_SHADER_VALIDATION=1` crashes Apple's Metal 4 debug compiler. | Keep `MTL_DEBUG_LAYER=1`, drop shader validation (L79). |
| L3 / L15 | `should_skip_draw` hid its reason. | Predicates logged for the first 64 refusals. |
| L16 | Four `Resource was destroyed whilst holding a resource reference!` at shutdown. | Teardown only; the clean `rc=0` run proves it is not the fatal path. |
| L107 | The binder's stage-only cache | **not a bug**: tables are per recorder slot and both binders reset in `begin()`. |
| L116 | **The video quad was culled, not skipped.** All traced quads are CCW in NDC and `capture_front_face` inverted the guest's `cw` on top of the negated viewport height, so the winding test was inverted twice (DEVIATIONS-DRAW D5). | `MTL_PROBE_NO_CULL` proved the cull; the front-face derivation now matches the guest. The FMV, Xevious panel and early car silhouette render after the fix. |
| L12 / L11 | **The intro FMV, course-select video, in-race FMVs and the Xevious panel were black.** | Fixed by L116 (cull); decode/upload were already faithful (L95, L96). |
| L37 | **`0x00010000` is a non-method word and is reported.** It is the argument of the preceding `reg 0x75C` write; in every capture the breaking word is the last argument of the block before it, not a header. The earlier "not malformed" claim came from reading `0xa0030003` as if bits 16–17 were part of the method index. | Corrected; folded into the L145 class. |
| L128 | **`sync_get()` could publish GET ahead of the parsed position.** | Closed by L145: `sync_get()` publishes `m_fifo_pos` alone. |
| L129 | **VK/GL write the label GPU-side and keep work in flight; Metal drained the GPU on the RSX thread.** The park/catch-up cadence is normal on all three backends (L125); the drain was Metal-only. | L131 removes the drain. |
| L131 | **The label release no longer drains the GPU.** `release_GCM_label` stages the guest word and emits one compute-encoder `copyFromBuffer` at the next encoder boundary, under one queue-scope `All → Blit` barrier per batch. The shared code skips its CPU store. A label outside the guest window, or one released before a frame exists, still takes the old drain + CPU store. | Fixed; run M confirmed the boot verify and 5,060 chunks. |
| L134 | **The back-end label batch was not submitted.** `flush_label_writes` ran only at the next encoder boundary and `commit_pending_labels` only from `on_semaphore_acquire_wait`, which this guest never reaches. | `release_GCM_label` commits the batch immediately, without waiting. |
| L135 | **A semaphore acquire could accept the pre-release word while the GPU label write was in flight.** | `sync_label(address)` commits the buffer carrying the write and waits only for that slot's feedback before the acquire re-reads the word. |
| L136 | **A no-copy import over the guest window is fixed at creation; a page the CPU maps later is invisible to the GPU.** The Metal device imports `vm::g_sudo_addr` at 4.58 s, but the RSX reports block is mapped at 18.96 s; the GPU write landed in the old physical page. | Per-page import created after the page exists (L137). |
| L137 | **Label writes get a per-page import created after the page exists.** `resource_manager::label_buffer` caches one 64 KiB no-copy import per label page, created at the first release and published to the residency set. | Fixed; run M confirmed `sudo=0x1000000 guest=0x1000000` and the boot park gone. |
| L139 | **The label path is the setting's choice again, and the default has no FIFO reader wait.** | `MTLDevice::fill_backend_config` honours `g_cfg.video.host_label_synchronization`: off (default) is the host store, on is the GPU write with the per-page import (L137) and the acquire wait (L135). The enabled path polls a `MTLSharedEvent` timeline (`signaledValue`), so a completed commit costs one atomic load. |
| L132 | **The desync was a cached word at or past PUT; the parser now follows the published frontier of its own stream.** `stream_tracker` keeps `{base, frontier, linear}` per stream; a PUT is attributed to the stream that contains it; a word at the frontier is consumed only if it is a jump/call/return. The cache is capped at the frontier and a method packet is dispatched only when fully published. | Fixed; tests, run U/V. |
| L133 | **The `put == addr` early return bypassed the frontier rule.** | The equality now enters the frontier path; a link is consumed as one word and a data word returns `EMPTY`. |
| L140 | **The L138 park was the guest's park area, and the frontier rule parked the parser on its first word.** `0x3020300` holds a count-0 NOP; the guest patches it later. A jump target above every stream that PUT does not name is now added unbounded, exactly as static code below the streams. | Fixed; test `FifoStreams.ParkAreaAboveStreamsIsReadUnbounded`. |
| L141 / L144 | **The link a chunk publishes could be gone before the parser read it; the publish sample was one move too late.** | Superseded by L145: honest GET means the producer cannot recycle the link, so the sample layer is deleted. |
| L142 | **A stream abandoned mid-command.** When the parser stands on a strict stream's frontier, the word is not a link, and PUT names a different known stream with a published chunk, `stream_tracker::redirect` marks the chunk stale and moves the parser to the published stream's base. `recover_fifo` keeps the known streams (`reset_at`). | Fixed; device-confirmed run S, six redirects in run V. |
| L145 | **GET is an acknowledgement, not a read-ahead hint.** `sync_get()` published the prefetched block end, licensing the guest to recycle a chunk the parser was still in. Run T: PUT moved to `A+0x9d4`, the parser spent 188 ms in the label acquire, the guest reused the link with the next chunk's data, and the parser arrived on data. | `sync_get()` publishes `m_fifo_pos` only; `cellGcmCallback` writes the jump before publishing PUT; the frontier is enforced in fast mode too. Tests: 12/12 `FifoStreams`, 516/516 full suite. **Device-confirmed run U** (2,892 handovers, 0 desync) and **run V** (zero `Dead FIFO`, zero desync, zero `FIFO error`, six redirects). |
| L148 | **A scissored colour clear became a whole-attachment clear.** `MTLFramebuffer::open_pass` chose the colour load action from `state.clear_color_fast` alone and ignored `clear_mask`; the partial path passes `clear_mask = 0` because Metal has no scissored clear and the fill shader draws the scissor, so the whole attachment became the clear colour. VK (`vkCmdClearAttachments`) and GL scissor the clear. | The colour load action now also requires `(clear_mask & RSX_GCM_CLEAR_COLOR_RGBA_MASK)`. Device run: `clear/scissored fill` zero. **Visual attribution measured false** — the patches survived the fix; this is a correctness fix only. |
| L152 | **The capture decoder was out of sync with `draw_record`.** | The phantom `_pad` field is removed; `capture.py` now decodes the 1200-byte record correctly. |
| L156 | **Mega Man 9 crashed the RSX thread on its first interleaved draw**, two minutes into boot. Symbolication put the fault at `rsx::interleaved_range_info::calculate_required_range` reading address `0x8`; `rsx::context::register_state` sits at offset 8, so the block's `ctx` was null. The rebase moved this shared function from `rsx::method_registers` to `REGS(ctx)` and `MTLGSRender::on_bind_context` never bound `frame_resources::vertex_layout`; the pre-port Metal tree did not need the binding because the function was thread-global. | `on_bind_context` now calls `vertex_layout.set_context(ctx)`. Test `MTLContract.VertexLayoutMustCarryTheBoundContext`. |
| L157 | **The renderer was replaced after every flip, so the shader preload ran in an endless loop.** `create_renderer` and `reset_renderer` stored `m_renderer_info = logical_id()` *before* `on_init()`. Metal opens its device in `on_init`, so the stored id was `Metal | NULL` while `configured_logical_id()` became `Metal | Apple A18 Pro`; `display_engine::flip` saw the mismatch and requested a replacement on every frame. Each replacement re-ran the full preload, rewrote the archive from a partial set, and showed the loading dialog again (run log: replace → init → replaced every ~1.5 s; 12 cycles in 18 s). VK was immune because it creates its device in the constructor. | `m_renderer_info` is assigned after `on_init()` in both `on_task` and `reset_renderer`. |

---

# 3. Settled — measured, not a cause

| id | result |
|---|---|
| L87 | **The device samples BC exactly** (BC1/BC3/cubemap, with and without mips). |
| L88 / L94 | **Every upload is faithful**: `guest-*`/`conv-*` byte-identical for 156 DXT1 and 263 DXT45; no large upload all-zero across 2 026 + 4 108. |
| L95 | **The video is three B8 planes and they carry picture**: 1280×720 luma + two 640×360 chroma, luma up to 210 distinct bytes, only 7 of 378 luma uploads flat (fades). |
| L96 | **B8 sampling agrees with Vulkan**: `B8` → `R8Unorm` with `{ONE, RED, RED, RED}` as a view swizzle, same as VK. |
| L22 / L21 | The upload and present paths are correct early (BC3 logo, Xevious panel, ESRB notice). |
| L25 / L26 / L27 | Compilation is not the visible-bug path: 97 async links, none failed; `skipped pipeline pending / failed` and `bind dropped` never fire. |
| L28 / L24 | Format census: guest DXT45 396 544, DXT1 171 588, A8R8G8B8 129 696, DXT23 8 530, B8 2 852; host BC3 802, BC1 528, BGRA8 259, R8 13, D32S8 12, BC2 1. |
| L51 | Level-2 texture probes clean: `create` 3, `reuse` 0, `partial upload` 0. |
| L73 | A **null texture** samples 0; a **null sampler** falls back to a default; `setAddress(0)`/`setTexture(null)` are legal. |
| L74 | `setRenderTargetWidth/Height` is a hard clip: an oversized viewport drops the whole draw, an oversized target is safe. |
| L75 | The 1920×1080 scissor with a 4096×4096 guest viewport is not a mismatch: the host viewport is the surface clip; the RSX transform lives in the vertex env. |
| L76 | The texture binding contract agrees on both sides, so a black object is not a slot-map mismatch. |
| L23 | Superseded by L95: the "dark flat luma" of the 52 s window was a fade. |
| L67 / L69 | Run-depth and log-census baselines; nothing Metal-specific in the shared log (54 `lock sudo`, 21 desync reports, 819 SPU `MFC_*`, 71 `ROP reads from rN`). |
| L68 | All 65 faults in the 02:5x run were on guest threads and handled. |
| L43a / L45 / L46 | Metal ignoring Vulkan's create flags, `create_nul_section` arguments and `on_section_destroyed` **matches OpenGL**. |
| L53 / L56 / L63 | The label address `0x60300800` is **outside** the ring (`0x50100000..0x53200000`), so the label write cannot land in command data. |
| L59 / L55 | Folded into L64 / L70. |
| L4 / L5 / L7 | Upstream noise (ROP read-before-write, SPU channel loop, MFC warnings) with no wrong output. |
| L77 | **Validation is the only debugger-class tool**: `MTL_DEBUG_LAYER=1` (with `MTL_DEBUG_LAYER_ERROR_MODE=nslog`) catches real errors; there is no Xcode 26, so no Metal Debugger, `.gputrace`, `xctrace` or offline `metal` compiler. |
| L80 | **The post-draw colour probe works and is exercised**: it copies the patch from colour attachment 0 after every draw of a window and logs mean BGRA, per-channel non-zero counts, distinct bytes and the first pixel. Run 2: 3 984 probes, 3 568 with non-zero content. |
| L81 | **The pixel probe works and the early video path writes**: at 25 s, draw 1242 is opaque white in all 4096 pixels and 1243 is textured (mean ~205, alpha 235). |
| L100 | **Run 1 reached all four scenes**: frame 1067/32.1 s video (`vp 43 fp 93`), 2517/68.3 s car select (`fp 52` ×524, `fp 53` ×7), 4374/125.4 s video (`fp 54`), 5796/313.6 s video (`fp 91`), 6996/406.5 s (`fp 25/9/94`). 520 host dumps. |
| L101 | **The video quad was encoded with every state field correct and still left the target untouched.** Draw 18896 (frame 1067): `primitive 8 count 4`, clip −0.9992/1.0008/−1.0014/0.9986, `scissor 0 0 1280x720`, `cull 2 winding 1`, `depth 515` LEQUAL `z=0` vs a 1.0 clear, `write mask 0xf`, `layout color 0xc0880000`, clear `(0,0,0,0)`. No `skipped` latch; `draw/record` = `recompiler` + `interpreter`. The probe read `(0,0,0,0)` after the draw and every later draw of the frame. **Resolved by L116:** the quad was culled. |
| L102 | **The CPU-side constant path is not the discriminator.** `fp 93`'s output alpha is exactly `diff_color.a` (`r1.w = fc[6].y = 1.0`); its only discard is the alpha test (GREATER, ref 0); `diff_color` is `vs 43`'s `vc[0 + xform_constants_offset]`, the CPU block at index 468 reads `(1,1,1,1)`, and the ring index math has no double offset. |
| L110 | **The alpha test is not the video's cause.** Run 2 compiled the alpha test out of every program and captured the same draw (frame 879, draw 12262, `fp 100`): full-screen quad, units 0/1/2 = B8 1280×720 + 2×640×360, guest planes non-empty (luma `230400/230400` nonzero, 71 distinct bytes), blend `770/771` = SRC_COLOR / ONE_MINUS_SRC_COLOR, clear `(0,0,0,0)`. All four colour patches stayed exactly `0x00000000`. Resolved by L116. |
| L111 | **The colour probe cannot separate the two survivors.** "Fragment ran and wrote `(0,0,0,0)`" and "no fragment ran" are the same reading on a `(0,0,0,0)` target. L114's forced output and L116's cull answered it for the video. |
| L114 | **The forced-output discriminator works and separates "never wrote" from "wrote black".** The selector is content-based (`MTL_PROBE_FORCE_FP_TEXTURES=<n>`, counting enabled 2-bit groups in `program.texture_state.texture_dimensions`) because fragment ids move with play order; the archive name carries the value (`…-forcefpt3.bin`). In run 5 the forced programs painted (tyres and track railings turned magenta) while the video quad did not, which pointed at the raster side and produced L116. |
| L42 | **A black rectangle is not a texture-content problem.** The 8×8 DXT45 it samples is opaque white in the guest and converts byte-identically. The Xevious panel's blackness was the cull (L116). |
| L32 | **The boot black screen was the intro FMV, not a missing shader.** Settled by L116. |
| L91 | **The texture-replacement bisection is not decisive for one draw inside a frame.** `MTL_PROBE_REPLACE_TEXTURES=1` binds a one-texel red image to every referenced unit; every draw sampling that unit then turns red, so the patch may already be red before the draw under test. It answers "does sampling work at all", not "did *this* draw write". L114 answers it. |
| L38 / L29 | **The label drain was the frame rate**: 57.9 ms average, 470 ms max, one per frame, against `frame/gpu us` 8.68 ms and `present/display gap` 82.8 ms — the GPU was idle 74 ms of every 83 ms. Fixed by L131. |
| L104 | **The 100 s pause after the second video is the guest, not a GPU hang**: ~1 131 presents and ~1 059 guest "access violation … writing 1" with no new draws; the RSX thread works in `release_GCM_label`/`wait_idle`; the game resumes at 406 s. |
| L36 | **The probe is exonerated**: a run with `MTL_PROBE` unset reproduced the desyncs and the `Dead FIFO`. |
| L117 | **The desync came from the malformed-command branch, not the unmapped-page path.** Proof is the capture's own cache line: `addr=0x10a28cc size=0x4 next=0xffffffff`. Only the control-command shortcut in `fetch_u32` stores `m_cache_addr = addr` unaligned with `m_cache_size = 4`, and `next=umax` is written by the malformed branch. `sys_rsx_context_iomap(io=0x50100000, size=0x3100000)` happens once at init and there is no `iounmap`. |
| L119 | **The producer rewrites the region while the parser walks it.** Episode A's history recorded `reg 0x19`/`0x6AB`-family commands at `0xcb0a84…0xcb0e1c`; the ring captured at those same addresses microseconds later holds the vertex-attribute block (`00041aac`, `00041ab4`, `00041ac8`, …). Episode B's dump changed between the 20 retries. This explains L85. |
| L121 | **Recovery is a livelock and the 2 ms retry is what makes it fatal.** `recover_fifo` restores to `restore_point == get`, so the same malformed word is read again; the retry sleeps 2 ms, so 20 recoveries take ~50 ms and trip `Dead FIFO`. The 44.587 s episode repeats 7 times and the 85.450 s episode 21 times, each at one fixed address. |
| L83 | The host executed 4.07 MB **ahead of PUT** for 0x39c bytes, then `0x3f800000` was a legal *old jump* off the iomap. See L120. |
| L84 | **GET sat on an argument — confirmed at word level.** 256 words before it were a well-formed header/argument cadence; episode B landed on the argument `0x10000` of `reg 0x75C` at `0x10a28c8`, and episode A on the vertex-attribute value `0x10030303`. The parser stopped between a header and its argument in both. |
| L85 | **The dispatch history did not match the ring at those offsets — because the guest rewrites the buffer while the parser walks it (L119)**, not because the 1 KB cache was parsed differently and not because a page was remapped: the iomap is created once and there is no `iounmap`. |
| L122 | **The producer uses two command buffers, `0xcb0000` and `0x10a0000`, `0x3F0000` apart.** The only two jump targets are those bases. Every `put` lies above one of them, and the observed spans are at least `0x1F9DC` (A, to `0xccf9dc`) and `0x21BE0` (B, to `0x10c1be0`). `put` alternates between them, normally naming the buffer the parser is *not* reading. The `0x3F0000` spacing is allocation placement, not a ring. |
| L123 | **`put` normally names the buffer the parser is *not* reading, at the same offset.** Every recorded sample shows the other buffer: `0x10a13bc`/`0xcb13bc`, `0x10a3d08`/`0xcb3d08`, `0x10a6264`/`0xcb6264`, with `get - put == +0x3F0000` and the low 16 bits equal. Numeric comparison across the pair is meaningless — the buffer being filled has the lower address. |
| L124 | **The parser's bookkeeping never drifted in the whole run: no `header phase slip`, no `jump past PUT`.** Every header was where the previous command predicted. The desync was the parser reading a *word* that is not a command while its position bookkeeping stayed perfectly consistent. |
| L125 | **The parser parks on `put` equality, and the app then moves `put` to the other buffer: 50 times in 6 idle seconds.** `prev=0x10a13bc put=0xcb13bc get=0x10a13bc` repeats from 36.26 to 42.58 s. It is benign because the word at the resume point is the app's own last command or a jump that re-parks the parser. |
| L126 | **The failing frame: the resume led into the other buffer, and the parser walked `0x2B08` past anything ever published there.** After resuming at `A+0x6264` the parser consumed the chunk to `A+0xF0DC` (`0x8E78` bytes, no overrun), then went into B and walked to `B+0xF57C`; the fetch that broke it was `B+0xF580`, `0x2B0C` past the furthest B was published (`B+0xCA74`). The 11 KB it consumed in B was the buffer's pre-run content. The per-stream frontier stops both walks (L132). |
| L127 | **The parser reached the bad word through zeros.** `last_cmd=0x0` with `next=0xcbf580` puts the previous header at `0xcbf57c` and makes it `0x00000000` — a count-0 NOP. The NOP path had no frontier check, so zeroed space parsed as an unbounded NOP stream. The frontier check now runs before every fetch (L132). |
| L130 | **The probes hid the event, because every test in them assumes the frontier is numerically ahead.** `addr > put`, `target > put` and `prev_put - put > 0x10000` are all numeric, and here the buffer being read has the *lower* address. A run with 21 errors therefore produced 104 clean-looking diag lines and no chain. The discriminating test is a slab comparison, `(addr ^ put) & ~0xffff`, logged as `jump cross-buffer`. |
| L138 | **The producer stall at a later frontier was the L140/L142 class, not the label path.** Run M parked at `addr=0xcbe890 frontier=0xcbe890 put=0x10ae994 word=0x0`; the guest kept running. Run O reproduced it in gameplay and showed the word at the parked frontier changing (`0x10000` → `0x4025`); the frontier link test then accepted a data word. Run P excluded the label path (host-store labels, same freeze). The park-area rule (L140) and `redirect` (L142) close it. Device-confirmed run V: zero desync. |
| L141 | **The link a chunk publishes could be gone before the parser read it.** Superseded by L145. |
| L144 | **The boot handover lost the link before PUT moved away.** Superseded by L145. |

---

# 4. Environment and non-Metal

| id | result |
|---|---|
| L2 | LLVM JIT teardown SIGSEGV on quit — recorded and fixed as I6 (`REVIEW-FINDINGS.md:1564`). Run 2 quit with a main-thread segfault in the same teardown path (`SYS: Segfault … Deleting old game window`). |
| L6 / L17 / L62 | 54 guest ranges fail `mlock` (first: the FIFO ring). Benign with unified memory; would matter on a discrete GPU. |
| L8 | The guest asks for `BLJS10001/…/RR7.DAT`, gets `CELL_ENOENT`, and falls back to the 4.872 GB `/dev_bdvd` copy. |
| L9 / L61 | **Low Power Mode was on and the machine was on battery** for every measurement in this window; the §1.3 numbers are lower bounds. |
