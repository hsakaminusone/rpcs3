# Metal4 vs Vulkan/OpenGL behavioral parity audit — 2026-11-17

Question: is there any difference in behavior between the Vulkan/OpenGL backends
and the Metal 4 backend? Find everything that can change game-visible behavior:
every added line, every function call, every workflow, plus the nuanced tricks
in the mature backends that Metal4 might lack. Scrub commit/PR/issue history.

## Method

Six parallel read-only audits were dispatched, plus direct investigation of the
cross-cutting areas no subagent owned:

| Domain | Source |
|---|---|
| Surface / texture / blit / write-back | subagent (pending at time of writing) |
| Sync / ZCULL / FIFO / frame pacing | subagent (received) |
| Shader / program / interpreter / FSR | subagent (pending at time of writing) |
| Presentation / flip / overlays | subagent (received) |
| Config / capabilities / draw path | subagent (pending) + direct investigation |
| Upstream VK/GL history mining | subagent (received) |

All code was read from `git show metal4-dev:<path>` (authoritative) and
`git show upstream/master:<path>` for VK/GL. The working tree is on `metal4-dev`
with unrelated in-flight edits (`MTLConstants.*`, `MTLGSRender.cpp`,
`MTLProgramPipeline.cpp`) that were treated as read-only. Confidence tags:
**[V]** verified by reading code end-to-end, **[I]** inference, **[U]** unverified.

---

## Part 1 — CONFIRMED behavior gaps (verified in code)

### G1. Present-source display-format correction is missing — HIGH
- **Metal4:** `MTLGSRender::get_present_source` (`MTLGSRender.cpp:6924-7095`) uses a
  found surface's texture directly (`:6977-6987`) or a cached texture directly
  (`:6991-7001`), with **no comparison against the AV-configured display format**
  and no conversion path for the found-image case. The expected format is only
  computed in the CPU-upload fallback (`:7006`).
- **VK:** `VKPresent.cpp:404-430` — when `image_to_flip->format() != expected_format`
  it creates a temporary subresource in the expected format and bitcasts
  (`copy_image`) or typeless-copies (`copy_image_typeless`) into it. Commit
  `acd3fd0c3` "vk: Implement display output format correction" (GL twin
  `bc141831f`): *"99.9% of the time this matches … but in some cases, let's just
  say some devs are creative."*
- **Impact:** a title that renders the display buffer in a format differing from
  its `CellVideoOutConfigure` format presents with wrong channel order (red/blue
  swap) on Metal4; VK/GL correct it. Rare trigger, whole-frame color error.
- **Not covered** by the existing deviation docs (F13 is a different
  wrong-format-*space* crash). Confidence **[V]**.

### G2. Texture upload row-length clamp is missing — MEDIUM
- **Metal4:** `upload_texture_data` (`MTLTextureCache.cpp:192-196`) sets
  `span.row_pitch = align(width_in_block * block_size, 4)` with **no
  `max(..., width_in_texel)` clamp**.
- **VK:** `VKTexture.cpp:157` clamps `bufferRowLength = max(declared_pitch, width_in_texel)`
  (commit `c415578e7` "Clamp buffer row length to never be less than declared
  width — Fixes some games with broken textures", 2019-11-13).
- **Impact:** games that submit an upload pitch narrower than the declared texture
  width get garbled textures on Metal4; VK is robust. Which games do this is
  **[U]** from the commit message. Confidence **[V]** on the code difference.

### G3. Overlays vanish when the present source cannot be resolved — MEDIUM
- **Metal4:** `frame_output::compose` (`MTLPresent.cpp:157-162`) returns early on
  `!source.image` **before** the overlay loop (`:194-207`). The caller passes a
  null source on frames where the display buffer can't be resolved
  (`MTLGSRender.cpp:7388-7392` zeroes the buffer dims).
- **VK/GL:** overlay/debug rendering sits **outside** the `if (image_to_flip)`
  block — VK `VKPresent.cpp:911-998`, GL `GLPresent.cpp:464` — so the UI renders
  over the black frame.
- **Impact:** on a frame with no resolveable source (video playback holes,
  resolution-scale changes, uncached surfaces — exactly the "video hole" case the
  Metal4 diagnostics were added to investigate), the RSX image is correctly
  black-cleared but the **home menu, perf overlay and debug overlay all vanish**.
  If the source stays unresolved for many frames the menu appears to disappear.
- No comment justifies skipping UI here. Confidence **[V]** (static control flow).

### G4. Stereo + non-100% resolution scale: second eye scaled twice — MEDIUM-LOW
- **Metal4:** the first `get_present_source(..., eye=0)` at `MTLGSRender.cpp:7243`
  modifies `buffer_width/height` by reference via `apply_resolution_scale<true>`
  (`:6984-6985`). The stereo block (`:7395-7407`) passes those **already-scaled**
  values into the second eye's `get_present_source` (`:7405`), which scales again
  if the second eye resolves to a surface. The final `present_source{...}` (`:7410`)
  carries double-scaled dims into `aspect_convert_region` (`:7508`).
- **VK:** `VKPresent.cpp:584-585` resets `present_info.width/height` to the
  unscaled guest dims before the second-eye call, then reads the single-scaled
  result back at `:602-603`.
- **Impact:** stereo titles with `resolution_scale_percent != 100` get a wrong
  aspect rectangle / cropping and wrong FSR gating (the upscaler reads
  `source.width/height`). Latent at 100% (scale is a no-op). Confidence **[V]** on
  the code difference; **[U]** on real-world frequency.

### G5. Stereo "combined surface / inset" branch missing — MEDIUM-LOW
- **Metal4:** `MTLGSRender.cpp:7395-7407` has only the
  `if (image->height() < min_expected_height)` branch; **no else branch**.
- **VK:** `VKPresent.cpp:593-599` has the symmetric `else`: when the first-eye
  image is already tall enough to contain both, it crops `buffer_height` to
  `min(image_height - min_expected_height, scaled_buffer_height)` to account for
  insets. GL has the same at `GLPresent.cpp:237-258`.
- **Impact:** stereo games whose first-eye surface already contains both eyes
  (with an inset) compute the aspect rectangle from the full double-height
  surface → wrong vertical scaling / letterbox. Confidence **[V]**.

### G6. No retry on a nil drawable (PF8) — MEDIUM-LOW
- **Metal4:** `present.acquire(...)` returns bool; on failure the frame is
  submitted with no compose and `present_frame` returns false
  (`MTLGSRender.cpp:7491-7492, 7577-7590`). Now documented as intentional
  (`MTLPresenter.cpp:113-125`).
- **VK:** `VKPresent.cpp:595-636` retries `acquire_next_swapchain_image` in a
  bounded loop and never returns without an image.
- **Impact:** dropped frames on resize/occlusion. Confidence **[V]**.

### G7. Resolution-scale change has no memory-pressure reclaim — LOW
- **Metal4:** `sync_scaling_config` is called at the **start** of `present_frame`
  (`MTLGSRender.cpp:7127-7140`) with no memory-pressure handling.
- **VK:** done in `post_present` (`VKPresent.cpp:1013-1041`) with
  `vmm_determine_memory_load_severity` + `handle_memory_pressure` before/after
  (commit `51ea735cb`). GL: `GLPresent.cpp:542-557`.
- **Impact:** (i) one-frame timing difference for live scale changes (Metal
  applies to current flip, VK/GL to next — Metal's comment says intentional);
  (ii) under memory pressure a scale-up can OOM/stall on Metal4 where VK
  reclaims first. Confidence **[V]** on code, **[U]** on visible impact.

---

## Part 2 — Sync / ZCULL / FIFO (subagent report)

### Intentional, not gaps (verified)
- **Conditional-render / sync-hint no-ops:** MTL inherits empty defaults for
  `on_begin_conditional_rendering`, `on_end_conditional_rendering`, `on_sync_hint`
  (`RSXRenderBackend.h:184-185,208`) because `supports_hw_conditional_render=false`
  (`MTLDevice.cpp:411`). The core only calls the hardware path when that flag is
  true (`RSXCoreRenderer.cpp:355-363`); MTL uses a synchronous
  `zcull_ctrl->read_barrier` (`:367`) and skips the whole clause on the CPU
  (`:386,749`). GL has no override either. ZCULL results are served by MTL's own
  `occlusion_queries()` provider (`MTLGSRender.cpp:7690`, `MTLZCULL.cpp:283-322`).
  Result correctness preserved — it is a **performance** difference (VK
  pre-flushes on `on_sync_hint`, `VKGSRender.cpp:1820-1886`; MTL/GL pay a bounded
  flush+SharedEvent wait at eval time). Confidence **[V]**.
- `on_renderctl` is VK-internal (`vk::rctrl_queue_submit`, VKCommandStream),
  irrelevant to MTL — not a gap. MTL-only `on_fifo_desync` is diagnostic-only
  (gated at probe level 2) — no game-visible effect. MTL-only `sync_label`
  correctly waits for in-flight GPU label writes before a semaphore acquire — a
  correctness positive.
- **ZCULL Q1–Q6** (forced read flushes, no encoder-pointer cache, `reported`
  threading, commit-count gate, boolean mode, exhausted pool) all verified fixed
  in code. Confidence **[V]**.

### Open / hypotheses
- **Pass-boundary barrier producer-side:** `close_encoder()` raises a
  `StageAll→StageAll` barrier before `endEncoding()`
  (`MTLCommandRecorder.cpp:1057-1060`, `MTLBarrier.cpp:48-51`); KK/VK raise it
  after the pass ends. The project's own docs first called this a black-screen
  cause, then **retracted** (LOG-FINDINGS L233) and measured the shipping model
  rendering correctly (L234). Low/unproven impact.
- **Pass scheduler emits no WAW edge:** `note_write()` consumes only read_stages
  (`MTLPassScheduler.cpp:166-239`); two writes to the same attachment in different
  passes are unordered, but the unconditional StageAll backstop at every encoder
  close masks it. Confidence **[V]** on code, low on impact.
- **No cross-frame hazard records:** `scheduler.begin_frame()` clears records;
  only the same-slot wait orders frames, with three in flight. VK fences frames
  and trims caches after completion (`VKPresent.cpp:187-254`). Unproven.
- **Forced ZPASS read can return partial count on timeout**
  (`MTLZCULL.cpp:310-320`) — bounded fallback; rare.
- **FIFO recovery wedge on jump out of iomap** (`LOG-FINDINGS` L241, DS wedge at
  1:12) — rollback can land on the bad jump target and loop with Atomic accuracy;
  full recovery path not line-by-line traced. Confidence **[I]**.
- **Whole-packet gate unconditional** (`RSXFIFO.cpp:1077-1084`) — an intentional
  local deviation, still unproven. Note: docs claiming a runtime A/B switch
  (`m_upstream_semantics`) are **stale** — no such switch exists in the tree.
- **Renderer replacement after a second game kills presenting** — open item
  (docs §22.17 / L235).

### Docs staleness
`DEVIATIONS-SYNC.md` SY1/2/5/9/14 fixed; SY6/7/8/10/11 open. `DEVIATIONS-QUERY.md`
Q1–Q6 fixed; Q7 diagnostic only. `PRESENT-FIFO-PARITY.md` PF1–7/9–11 largely fixed
in the refactored tree; PF8 open. `METAL4-FIFO-PARK-HANDOFF` §234/L236 "switchable"
claim stale; L233 hazard theory retracted.

---

## Part 3 — Presentation (subagent report, beyond Part 1)

### Intentional / documented
- **Debug overlay shows fewer counters** (`MTLGSRender.cpp:7532-7550`, 10 lines
  vs VK 15) — Metal has no cache counters; documented in `METAL4-VK-GL-PARITY.md:647`.
- **Debug overlay text one frame earlier** than VK/GL (set before compose at
  `:7532` vs after at VKPresent.cpp:969) — at most a one-frame skew on static text.
- **Frame capture async** (`request_capture` → `on_slot_complete` → `deliver`),
  ~3-slot delivery latency; a capture immediately before shutdown can be lost.
  Content is still the requested frame; documented in `MTLPresent.h` and HLD-06.
- **No MAILBOX present mode** — `off` uses `present()`, `adaptive` uses
  `presentAfterMinimumDuration()` (`MTLPresenter.cpp:232-251`); VK prefers
  IMMEDIATE/MAILBOX/FIFO_RELAXED (`swapchain.cpp:238-252`). Metal can tear where
  VK's MAILBOX is tear-free. Intentional platform limitation (no Metal MAILBOX;
  RPCS3's limiter stays the single pacing owner).
- **FSR output texture hard-coded BGRA8Unorm** (`MTLUpscaler.cpp:628-635`) vs VK's
  source format (`fsr_pass.cpp:210-225`) — `texture.write(float4)` writes logical
  RGBA into BGRA storage correctly; no color shift expected. Documented.
- **Calibration sampler LINEAR** matches VK (`VKOverlays.h:47`); GL uses NEAREST
  for `output_scaling=nearest` — a pre-existing VK/GL divergence Metal follows VK on.
- **Bilinear/nearest upscalers eager at init** (`MTLPresent.cpp:39-61`) vs VK's
  fully lazy per-mode creation — an init failure in a trivial blit pass would
  fail all presentation on Metal, but this is very unlikely. Low.

### Verified at parity
FSR mode gating, enlargement gating, RCAS attenuation, stereo exclusion, lazy FSR
init, fallback filter, calibration/blend math, Y-flip via viewport, alpha blend
factors, window-space overlay layout, overlay lock, `record_with_overlays`, BGRA
screenshot flag, gamma/full-range inputs, letterbox black clear,
drawable-size-based aspect, `skip_frame` handling — all match VK/GL.

### Docs staleness
`METAL4-VK-GL-PARITY.md` §14 is **stale**: claims FSR/RCAS absent (D4) and
`rcas_sharpening_intensity` inert, but current code implements FSR
(`MTLUpscaler.cpp`, commit `cda1e39e1`) and reads that setting at `:708`.

---

## Part 4 — Upstream history mining (subagent report)

### Already present in Metal4 (do not re-litigate)
Verified equivalents exist for: early-Z loopback (`441e7e048`,
`MTLFragmentProgram.cpp:631`), single-index points (`18f18b305`,
`MTLGSRender.cpp:1156`), programmable-blend MSAA (`d55e9fcf2`,
`MTLFragmentProgram.cpp:230,420`), two-sided stencil (`d10658b81`,
`MTLDepthStencil.cpp:76-102`), clip-plane inversion (`f3e34e1d4`, shared
`GLSLCommon.cpp` + `MTLVertexProgram.cpp:278`), invalid-mip skip (`d277003ef`,
`MTLTextureCache.cpp:183`), exact mip count (`7e35f5999`), RGB565 byte order
(`78cb66ecf`, `MTLFormats.cpp:417-500`), early discard (`0b2f9f0f1`),
format-space crash F13, clear logic (`Core/RSXCoreRenderer.cpp:203-284`),
unresolve (`StoreAndMultisampleResolve`), post-cyclic-Z
(`RSX_SHADER_CONTROL_DISABLE_EARLY_Z`), and most ROP/shader shared-code fixes.

### Gaps
G1 and G2 above are the two confirmed missing generic fixes. Rank 3 (readback
byte-range, `6d52415d5`) partly overlaps the already-documented H1 audit item
(`CODE-AUDIT-2026-11-13.md` — `surface::write_to_memory` drops partial rows and
destination column, no scaled path). Rank 4 (line width & point size scaling by
resolution, `47e784d5d`) is a platform limitation: Metal scales point size
(`MTLGSRender.cpp:5609`) but has no `setLineWidth` API (`MTLRasterState.cpp:33`
copies raw width) — thin lines at non-100% scale, low severity.

---

## Part 5 — Direct investigation (cross-cutting, no subagent)

### Virtual interface (`RSXRenderBackend.h`)
- VK overrides but MTL does not: `on_sync_hint`, `on_begin/end_conditional_rendering`,
  `on_renderctl`, 5 occlusion-query methods. All resolved in Part 2 (intentional or
  VK-internal).
- MTL-only: `on_fifo_desync` (diagnostic), `sync_label` (correctness positive).

### Shared-core renderer branching — no gap
`rsx_cache.h:301` treats Metal like Vulkan for threaded shader preload;
`RSXThread.cpp:592-593` includes Metal in native-interface handling.
`System.cpp:1885` Vulkan-only log is cosmetic.

### Config surface — no gap
`read/write_color_buffers`, `strict_rendering_mode`, `relaxed_zcull_sync`,
`emulate_extended_depth_range` are all read in shared code
(`Common/texture_cache.h`, `RSXGraphEngine.cpp`, `RSXZCULL.cpp`).
MTL reports `supports_extended_depth_range=false` and wires the emulation at
`MTLGSRender.cpp:4803` via `capture_encoder_state(..., ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE)`.

### Capability choices (`MTLDevice.cpp:373-419`) — all fallback-covered
| Flag | MTL | VK/GL | Fallback |
|---|---|---|---|
| `supports_multidraw` | false | true | separate draws, identical output |
| `supports_hw_conditional_render` | false | true (VK) | CPU skip + read_barrier |
| `supports_native_logic_op` | false | true | ROP emulation via `RSX_SHADER_CONTROL_LOGIC_OP` + MSL |
| `supports_extended_depth_range` | false | config-dependent | shader `depth_range` varying |
| `supports_asynchronous_compute` | false | true (VK) | backend-internal, no shared-core effect |
| `supports_passthrough_dma` | false | config-dependent | backend-internal |
| `supports_hw_msaa/a2c` | from config | from device | config-driven |

No new confirmed gap from these areas.

---

## Ranked summary of confirmed gaps

1. **G1** present-source display-format correction missing — HIGH (whole-frame color)
2. **G6** no drawable acquire retry — MEDIUM-LOW (dropped frames)
3. **G3** overlays vanish on unresolved source — MEDIUM
4. **G2** upload row-length clamp missing — MEDIUM
5. **G4** stereo double-scale — MEDIUM-LOW
6. **G5** stereo inset branch missing — MEDIUM-LOW
7. **G7** resolution-scale change without memory reclaim — LOW

Intentional/platform limitations (not bugs): MAILBOX absence, FSR output format,
async capture, eager bilinear/nearest init, debug overlay omissions, line-width
scaling, conditional-render CPU skip, pass-boundary barrier placement.

## Part 6 — Surface / texture / blit (subagent report, verified)

### New confirmed gaps
- **G8. Stencil plane never uploaded on depth-texture upload — MEDIUM-HIGH**
  `upload_texture_data()` (`MTLTextureCache.cpp:449-475`) decodes the guest depth
  pack into a float plane and uploads with only `get_depth_plane_option(..., false)`
  (depth); there is no `StencilFromDepthStencil` upload. Readback *does* handle both
  planes (`MTLTextureCache.cpp:723-736`). VK/GL upload `DEPTH24_D8` directly into
  D24S8/D32S8 preserving depth+stencil (`VKFormats.cpp:222-225`). Impact: a game
  that uploads a depth-stencil texture then samples/writes-back stencil gets
  undefined/zero stencil. Confidence **[V]** on presence/absence; **[U]** on title exposure.
- **G9. Render-target format compatibility: strict equality vs. compatible set — MEDIUM**
  MTL `MTLTextureCache.h:868-871` accepts a surface only if its host format is
  *exactly* `get_texture_format(gcm_format)`; VK (`VKTextureCache.cpp:1379-1417`)
  and GL accept a per-format **set** (e.g. A8R8G8B8 → BGRA8 **or** D24/D32S8;
  depth-as-colour). Impact: depth sampled through a colour format forces a copy
  on MTL; with MTL's blitter declining depth destinations this can route to CPU
  fallback or an undefined temporary. Documented open (TX11). Confidence **[V]** code,
  medium severity.
- **G10. `force_high_precision_z_buffer` ignored — LOW**
  MTL always maps z24s8→Depth32Float_Stencil8 (`MTLFormats.cpp:202-204`); GL
  respects the setting (`GLRenderTargets.cpp:84`). Documented open (F9). **[V]**

### Likely gaps (need runtime test, lower confidence)
- **B1.** MSAA source coordinate transform missing in `copy_transfer_regions_impl`
  (`MTLTextureCache.cpp:989-1096`) — VK calls `transform_samples_to_pixels`
  (`VKTextureCache.cpp:497-500`). If deferred gathers can source MSAA surfaces,
  wrong texels. **[U]** reachability.
- **B2.** Surface background load / packed16 and depth encode bypass the shared
  `be_t` byteswap (`MTLSurface.cpp:467-474`, `MTLFormats.cpp`), using field
  permutations instead. SF14 claims fixed; equivalence for g8b8/x1r5g5b5 not proven
  statically. **[U]** — flag for a focused write-back test.
- **B3.** Custom border colour reduced to black/white/transparent-black (TX5). **[V]** difference, low severity.

### Important doc staleness (surface domain)
The three docs are materially stale. Verified already-fixed in current code (do
not re-chase): S1/S2/S12 target-lock guards, S3/S4/S5/S16 clear, S7 native layout,
S9 depth readback, **S10 scaled-surface readback (actually handled)**, **S8 full GPU
blit path now exists**, SF14/TX20 packed16 writeback, **TX7/TX8 3D/cube view fixed**,
**TX9/TX10 typeless/scaled copy fixed**, TX21 gather temporaries.

## Part 7 — Shader / program / interpreter / FSR (subagent report, verified)

### New confirmed gap
- **G11. EMULATE_DEPTH_RANGE async first-use draw skip — MEDIUM-HIGH**
  MTL `compile_async = !interpreter_only && shadermode != recompiler`
  (`MTLGSRender.cpp:4429`) has **no** depth-range exception, yet the interpreter
  rejects `RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE` programs
  (`MTLShaderInterpreter.cpp:655-660`). On an async first-use miss in the default
  `async_with_interpreter` mode the draw yields and returns false
  (`MTLGSRender.cpp:4484-4487`) — a transient dropped draw. VK forces synchronous
  compile for these programs (`VKGSRender.cpp:1953-1954`, "the interpreter does not
  emulate depth range"). Easy fix: mirror VK's `allow_async` gate. Confidence **[V]**.
- **G12. Interpreter stipple on non-polygon draws — LOW**
  MTL keys stipple on live `ctx.draw_state.stipple.enabled`
  (`MTLShaderInterpreter.cpp:725`) with no primitive-class gate; VK/GL use
  `fp_ctrl & POLYGON_STIPPLE`, latched only for polygon draws. A points/lines draw
  while polygon-stipple is enabled compiles a stippling variant and discards
  fragments on MTL. Rare. **[V]** code, medium-high confidence.
- **G13. Interpreter rejection set / vertex textures — LOW**
  MTL's interpreter rejects vertex-texture fetch and several ctrl bits, running
  the recompiler where VK's interpreter handles them (`VKShaderInterpreter.cpp:554-592`).
  Divergence only in interpreter modes. **[V]**.

### Verified at parity (no finding)
Fragment/vertex constant upload (shared T()/sanitize, sizing, update frequency —
PR #19669 present in **both** branches); shader cache key composition; program
logging + compile hint; async pipeline state machine (docs claiming "synchronous"
are stale); FSR upscaler (constants, enlargement gating, RCAS clamp, all 12 EASU
taps — faithful line-for-line port, prior gather/BGRA concerns do not apply);
interpreter decode/flow/constants, header layouts, block lifetime.

### Doc staleness (shader domain)
Stale claims: S03 half-support hardcoded false (now config-driven); S06/A7
synchronous pipeline/no interpreter (now async + interpreter + shadermode read);
A15/A63 no program log/compile hint (now implemented); A57/A61/B12/B14 no
FSR/RCAS / slider unused (now implemented and consumed).

---

## Updated ranked summary of confirmed gaps (all domains)

1. **G1** present-source display-format correction missing — HIGH (whole-frame color)
2. **G11** EMULATE_DEPTH_RANGE async first-use draw skip — MEDIUM-HIGH (dropped draw)
3. **G8** stencil plane never uploaded on depth-texture upload — MEDIUM-HIGH
4. **G6** no drawable acquire retry — MEDIUM-LOW (dropped frames)
5. **G3** overlays vanish on unresolved source — MEDIUM
6. **G2** upload row-length clamp missing — MEDIUM
7. **G9** render-target format compatible-set — MEDIUM
8. **G4/G5** stereo double-scale / inset branch — MEDIUM-LOW
9. **G7** resolution-scale change without memory reclaim — LOW
10. **G10/G12/G13** force-high-precision-z ignored, stipple/vertex-texture interpreter divergences — LOW
11. **G14** (from sync) FIFO recovery wedge on jump-out-of-iomap — MEDIUM, unproven
12. **G15** (from sync) no WAW scheduler edge / no cross-frame hazard records — low, masked by StageAll backstop

## Part 8 — Config / capabilities / draw path (subagent report, verified)

### New confirmed gaps
- **G16. Depth-bounds test silently ignored — MEDIUM (rare titles)**
  `capture_depth_bounds` computes min/max into `encoder_state.depth_bounds_min/max`
  (`MTLDepthStencil.cpp:107-126`, `MTLEncoderState.cpp:38-39`), but **no encoder
  setter or MSL epilogue ever consumes them** — verified by grep: the only writes
  are the captures and a reset to `[0,1]` at `MTLGSRender.cpp:4814-4815`. Metal
  has no `setDepthBounds` API. VK uses `vkCmdSetDepthBounds` (`VKDraw.cpp:244-272`);
  GL `GL_DEPTH_BOUNDS_TEST_EXT`. Impact: a guest enabling depth bounds is
  **under-culled** — wrong pixels survive. Documented open (E7/F14). **[V]** code;
  **[U]** title reach (no known title measured).
- **G17. Sample mask ignored — MEDIUM**
  `capture_anti_aliasing_state` (`MTLDepthStencil.cpp:133-137`) keeps only
  `alpha_to_coverage`/`alpha_to_one`; `multisample.sample_mask` never reaches the
  encoder or MSL (the header comment at `MTLDepthStencil.h:77` claims it "travels
  in the MSL as `[[sample_mask]]`" — unfulfilled). VK/GL use the pipeline sample
  mask / `glSampleMask`. Impact: a non-full guest sample mask with MSAA draws with
  full coverage on Metal. Documented open (F12). **[V]**.
- **G18. Line width ignored — LOW-MEDIUM**
  `line_width`/`line_smooth` are captured/hashed but never applied
  (`MTLRasterState.cpp:33`, `MTLGSRender.cpp:214` hash only); Metal line width is
  always 1 px. VK/GL set real line width (`VKDraw.cpp:175-182`, `GLDraw.cpp:252`).
  On Apple VK likely also lacks wide-line support, so the difference may be moot.
  **[V]** code, medium confidence.

### Verified fixes / at parity (docs understate the code)
Flat shading (`supports_last_provoking_vertex=true`, index rotation), framebufferless
rendering (now genuinely works with scratch depth attachment), FSR/RCAS, GPU blit,
pass batching, async pipeline, shader-compile hint, debug output, host labels —
all fixed in committed code. Subrange splitting, 16-bit strip widening, winding,
no-attribute dependency drain, instancing, `line_loop` OOB fix — all at parity.
Render-pass policy (batching key, load/store actions, feedback split, mid-draw
restart) is sound — no lost writes found. Vertex/constant setup has no offset or
alignment gap.

### Intentional / API limitations
Sampler approximations (anisotropy rounded, LOD bias quantized, 3 border colours),
texel remap forces nearest (quality, not correctness), surface compat exact-format
(extra copies), flat-shaded quad residual, async-compute false (inert),
`force_cpu_blit_processing` meaningful again. `renderdoc_compatiblity` has no
reader (tooling only, not game-visible).

### [in-flight] uncommitted edits are diagnostic-only
`MTLConstants.cpp/.h`, `MTLGSRender.cpp` (+321), `MTLProgramPipeline.cpp` (+63),
`MTLInstrumentation.h` (-58): F12-press-extended capture windows, image-byte/present
dump budgets, world logging, probe-level-5 MSL `fc[]` logging, and removal of
`MTL_PROBE_ARM_FRAME`. **Zero rendering-behavior delta** — no pass policy, draw
issuing, vertex upload, or capability changes.

---

## Final ranked summary of confirmed gaps (all 6 domains complete)

1. **G1** present-source display-format correction missing — HIGH (whole-frame color)
2. **G11** EMULATE_DEPTH_RANGE async first-use draw skip — MED-HIGH (dropped draw)
3. **G8** stencil plane never uploaded on depth-texture upload — MED-HIGH
4. **G16** depth-bounds test silently ignored — MED (under-culling, rare titles)
5. **G17** sample mask ignored — MED (wrong coverage with MSAA)
6. **G6** no drawable acquire retry — MED-LOW (dropped frames)
7. **G3** overlays vanish on unresolved source — MED
8. **G2** upload row-length clamp missing — MED
9. **G9/G6surf** render-target/surface format compatible-set — MED
10. **G4/G5** stereo double-scale / inset branch — MED-LOW
11. **G18** line width ignored — LOW-MED
12. **G7** scale change without memory reclaim — LOW
13. **G10/G12/G13** force-high-precision-z, stipple/vertex-texture interpreter — LOW
14. **G14** FIFO recovery wedge on jump-out-of-iomap — MED, unproven
15. **G15** no WAW scheduler edge / cross-frame hazard records — low, masked

Intentional/platform limitations (not bugs): no MAILBOX, FSR output format, async
capture, eager bilinear/nearest init, debug overlay omissions, conditional-render
CPU skip, pass-boundary barrier placement, strict format fallback, 2-sample→4 AA
fold, uniform Depth32Float, sampler approximations, flat-shaded quad residual.

**Strongest meta-finding:** all four parity/deviation docs are materially stale —
dozens of rows marked "open" are fixed in committed code (FSR/RCAS, GPU blit,
async pipeline, flat shading, framebufferless, 3D/cube views, typeless copies,
scaled readback, program log, debug output, host labels). The docs should be
refreshed before anyone re-investigates those areas.

This document is read-only research; no code changes were made.
