# METAL4-TODO — confirmed Metal 4 backend defects and gaps

Scope: the RSX Metal 4 backend (`rpcs3/Emu/RSX/MTL`). This is the live work list. Every row is
confirmed by a run log or by a static comparison against `rpcs3/Emu/RSX/VK` and
`rpcs3/Emu/RSX/GL`. A row leaves the list only when the fix is built, tested, and validated.

Rule for every fix: RSX is the specification. Vulkan and OpenGL are evidence of the RSX behaviour;
the implementation is the native Metal 4 mechanism (Apple documentation, KosmicKrisp). No hacks,
no workarounds, no wrapper-on-wrapper, no compatibility aliases. The review that enforces this is
`REVIEW-RSX-NATIVE.md`.

Sources: `PRESENT-FIFO-PARITY.md` (PF), `DEVIATIONS-DRAW.md` (DR), `DEVIATIONS-SURFACE.md` (SF),
`VIDEO-TEXTURE-PARITY.md` (V1), `RR7-FREEZE-FORENSICS.md` (freeze evidence), `DEVIATIONS-QUERY.md`
(Q), `DEVIATIONS-TEXTURE.md` (TX), `DEVIATIONS-VERTEX.md` (VX), `SHADER-PIPELINE-PARITY.md` (S),
`LOG-FINDINGS.md` (L).

## Status legend

- `fixed` = changed, built, and covered by the unit tests (203/203 MTL, 499/499 full).
- `landed` = existing infrastructure verified in this pass.
- `priority` = next work, ahead of the ordinary open list; still unfixed.
- `open` = confirmed, not yet changed.
- `partial` = part of the row is fixed; the remainder is named.
- `needs run` = confirmed in code, but the visible effect needs an on-device run.
- `needs A/B` = confirmed in code, but the visible effect needs an on-device comparison.
- `needs capture` = hypothesis that needs a GPU capture.
- `planned` = design decision recorded; the work is not started.
- `parked` = a design decision (G1) blocks the change.
- `no action` = checked and correct as is.

## Completed and open

The priority table and the two status tables are the index. The sections below hold the evidence
and the detail; a row changes state there and in this index together. At this revision the Metal
list holds **65 fixed or landed rows** and **38 pending rows**: 20 open, 3 priority, 10 needing a
run or probe, 2 partial, 2 planned and 1 parked. Eight rows take no action, and the shared HLE
findings are in `NON-METAL-FINDINGS.md`.

**The 2026-10-21/22 window** is recorded in `INVESTIGATION-2026-10-22.md` and in `LOG-FINDINGS.md`
as L36–L55. It adds nine runs (R1–R9 in the investigation's run inventory) and these changes of
state:

| row | state | evidence |
|---|---|---|
| L13 (the label race) | **reopened** | the FIFO desync reproduces with no probe at all (R4) and in every run since. The desync capture (L52) shows the host executing a **data block as commands**: the word at the error names method index 0x69A–0x69F, none of which is a defined method, and the same window holds `0x10030303` and `0x80078000` (register values) and eight IEEE-754 floats alternating −1.0 and +1.0. The label write is **not** the corrupting event: the label address `0x60300800` is outside the FIFO's guest window `0x50100000..0x53200000` (L53). |
| L35 / L39 (the freeze) | **reclassified** | the freeze is a **recovery loop that makes no progress**: every capture has `restore_point == get`, so `recover_fifo()` returns the host to the same word, and the GET eventually reaches `0x3100000`, the exact end of the iomap window, where the address is unmapped and the error becomes fatal (L54). |
| L26 (no skipped draw) | confirmed | `draw/recompiler` 116 503, `draw/interpreter` 0: with the asynchronous variant prebuild the interpreter is a first-use fallback and was never needed. The interpreter's *rendering* is still unproven. |
| L43 / L45 / L46 | **closed as not-Metal** | OpenGL behaves the same way in all three cases (`GLTextureCache.h:724`, `:833`, `:446-466`). The real divergence is **T1**: OpenGL clears a reused image that is not uploaded wholly from the CPU (`GLTextureCache.h:751-761`) and Metal did not. |
| new: T1 | **fixed, unverified** | `texture_cache::clear_texture` clears a reused image and any level that no upload covers. The code did not execute in the verification run (`texture/create` 0), so the effect is unmeasured. |
| new: T2 | **fixed, unverified** | a reused image is discarded unless its format, extent and mip count match the new section. |
| new: the NV3089 scale | **fixed** | a zero `DS_DX`/`DT_DY` is the power-on value, not a scale of zero; the old code divided by it (`nv3089.cpp:180`, `:186`). The scale is now 1.0 and the empty-region warning names the geometry. |
| new: the FIFO desync capture | **landed** | the shared FIFO gathers the data and calls the backend hook `on_fifo_desync`; the Metal backend prints it through the probe facility at level 2. The capture names the offending method index, whether the method table knows it, the guest address of the GET, the last label write and the last eight dispatched commands. |

**The next measurement** is the `RR7-FREEZE-FORENSICS.md` §8 companion control: the identical
scene on Vulkan. If Vulkan freezes identically, the fault is in the shared FIFO or in the bulk-write
argument accounting and Metal inherits it; if only Metal freezes, the fault is in the Metal
backend's FIFO interaction. The bulk commands are the only ones that carry a variable-length data
block, and they are the only ones that derive a skip count from something other than the header's
count (`nv4097.cpp:130`, `:144`, `:168`, `:202`, `:244`).

The 12:54 probe-5 run confirms normal BC3 upload and ESRB compositing, adds the compressed
diagnostic-length fix (L19), and reproduces the FIFO desync at 0:00:52 before the capture window
arms; the black panel and video remain unproven. SF9 and its texture-cache twin TX12 are now
fixed; SF8 keeps a CPU fallback that invalidates the cache, with the GPU blit still open.

The **20:55 probe-2 run** is the first clean run of the final binary and adds L25–L33. It exits
`rc=0` after 2:19 and reaches the race with no `Dead FIFO`, so P0.5 is closed on device. It reports
`draw/skipped pipeline pending == 0`, `draw/skipped pipeline failed == 0` and 97/97 asynchronous
links, so P1 is closed on device. It does not show the visual failures: the draw is GPU-bound and
error-free, DXT45 dominates the bound texture formats, and 10.1 % of flips compose with no display
buffer. This run also adds the correctness probes (batch 7) and moves the bulk probe text to its own
file, `$TMPDIR/rpcs3-mtl-probe.log`; the flooding shared HLE channels drop to `error` for a probe run
(N5).

### Priority queue

P0 (L10) is closed: `make_descriptor` now sets `arrayLength = 1` for every type, the build is green,
and a local Metal probe reproduced the old `MTLTextureTypeCube requires that arrayLength is 1`
assertion. The remaining priority work is:

| order | rows | work |
|---|---|---|
| P0.5 | L13 | **Reopened.** The 20:55 run was clean, but the desync reproduces in every run since, including with no probe at all (R4). The capture (L52) shows the host executing a data block as commands, and the label write is not the corrupting event (L53). The remaining work is the §8 companion control on Vulkan and the capture's command history. Separately, the drain that P0.5 introduced is on the frame-critical path (`sync/label release` 57.9 ms a frame, 470 ms max, in a 82.8 ms frame; the GPU is busy 8.68 ms of it), so its native form — a GPU-side label write into shared memory, which unified memory allows — is worth doing on its own. |
| P1 | DR9, S5, L18 | **Closed on device.** Asynchronous `newLibrary`, a serialized synchronous link worker, placeholders, draw-path skip, begin/end drains, warm preload and bounded teardown. The 20:55 run reports 97/97 links asynchronous, `draw/skipped pipeline pending == 0` and no serializer error. The interpreter-eligibility counter that this pass adds says how much of the draw stream the interpreter covers; it is not yet in the binary. |
| P2 | F5 | Implemented (LLD §21): shared GLSL through the existing SPIR-V → MSL route, compact per-type sampler arrays inside the 16-sampler cap, separate Metal 4 argument tables and binder, set=0 instruction block, trimmed outputs, compile-mutex serialization, and a no-skip mode policy. The CPU tests are green; the `interpreter_only` run and the four optimisations' effects are pending. Do not add a progress bar without the interpreter. |

The Vulkan and OpenGL startup bar (`VKShaderInterpreter.cpp:709-711`,
`GLShaderInterpreter.cpp:17-36`) says “Precompiling shader interpreter variants.” It is not
game-shader precompilation. Metal already has the on-disk pipeline cache (`rsx_cache.h:235-300`);
it replays its own `metal/v1` entries. Async first-use compilation now exists: libraries use
`MTL4::Compiler` completion handlers and links use one synchronous compiler worker. Metal 4's
completion-handler link path crashes the driver when a pipeline data set serializer is attached
(L18), so the worker is required, not a tuning choice. The remaining gap is the interpreter
fallback.

### Completed (built and green)

| group | rows |
|---|---|
| A. Present and flip | PF1–PF7, PF9–PF11 |
| B. Draw | DR1, DR2, DR3, DR6, DR7 |
| C. Surface | SF1–SF7, SF9, SF11–SF13, SF15 |
| D. Video texture | V1 |
| E. Shader and pipeline | S1–S5 |
| F. Features | F1, F4, F6, F13 |
| G. Synchronization | SY1, SY2, SY3, SY5, SY9 |
| H. Texture and sampler | TX1–TX4, TX6, TX9, TX12, TX19 |
| J. Queries | Q1–Q6 |
| K. Run log | L1, L3, L10, L13, L18, L19 |
| L. Vertex and index | VX1–VX3 |

### Open (not yet changed, or not yet verified)

| group | rows |
|---|---|
| A. Present and flip | PF8, PF12 |
| B. Draw | DR4, DR5, DR10 |
| C. Surface | SF8, SF10, SF14 |
| E. Shader and pipeline | S6 |
| F. Features | F2, F3, F7–F12, F14 |
| G. Synchronization | SY4, SY6–SY8, SY10, SY11 |
| H. Texture and sampler | TX5, TX7, TX8, TX10–TX11, TX14, TX20 |
| J. Queries | Q7 |
| K. Run log | L11–L12 (B2–B4 landed; needs run) |
| L. Vertex and index | VX4 |

## Batch 1 — landed (built, green)

| id | change |
|---|---|
| PF1, PF4 | `MTLGSRender::flip` opens the frame when it is not open, so a flip always composes and presents. The silent guard-return is gone. |
| PF2, PF5 | `on_semaphore_acquire_wait` no longer submits or closes the frame; it runs the local task only on a pending flush or an emu flip, as Vulkan and OpenGL do. |
| PF3 | `command_recorder::begin_frame` bounds the feedback wait at 10 s (Vulkan `FRAME_PRESENT_TIMEOUT`). A stale feedback is ignored by a new slot generation. |
| PF6, PF10 | `evaluate_cpu_usage_reduction_limits` and the vsync re-apply run on an emu flip, not on every acquire. |
| PF7, SF13 | `bind_framebuffer` calls `check_zcull_status(true)`. |
| PF11 | The drawable is acquired and cleared black even when the present source is missing. |
| SF1 | The bound-target lock has the `address && pitch` guard. |
| SF2 | The orphan write-back re-checks `is_locked()`. |
| SF12 | The superseded/orphan release runs before the bound-target lock, as Vulkan orders it. |
| SF6 | Surface match compares the guest sample count, not the host count. |
| SF11 | `is_compatible_surface` compares the host pixel format. |
| DR1, DR2 | A clause whose `primitive_restart_barrier` splits a strip draws one Metal draw per subrange. |
| DR7 | The pass-open failure path runs a NOP draw. |
| V1 | The sample scan builds the effective pitch, so a swizzled texture no longer prints a false `Invalid 2D region descriptor`. |
| F1 | Output scaling: `nearest` now selects the nearest sampling pass; bilinear stays the default. |

## Batch 5 — landed this turn (probe-2 follow-up)

| id | change |
|---|---|
| L13 | `MTLGSRender::release_GCM_label` drains the current slot (or the ring when the frame is closed) before the CPU writes a back-end label.  `supports_host_gpu_labels` now routes labels through it.  `command_recorder::wait_idle` bounds the closed-frame wait. |
| L15 | A six-face cube depth is the expected RSX form, so `make_descriptor` no longer reports it as an illegal descriptor. |
| B2 | `texture_cache::resolve_texture_bind` materializes a deferred subresource before the render pass opens.  The draw path records the transfer write for the pass scheduler. |
| B3 | The argument writer writes null texture and sampler transitions instead of skipping zero values.  Address slots keep the non-zero guard until `setAddress(0)` is probe-verified. |
| B4 | A disabled fragment or vertex texture unit binds null and clears its stencil slot; it cannot sample the previous draw's image. |

## Batch 6 — probe-5 follow-up

| id | change |
|---|---|
| SF9 | Depth read-back now stages the host depth and stencil planes, waits once, and CPU-encodes the guest `z16_float` / `z24s8` pack. The bit-exact unscaled path is bounded by the host extent and guest pitch. |
| TX12 | The texture-cache flush uses the same encode as SF9, so a depth texture section no longer writes the host float plane into the guest pack or loses stencil at the second site. |
| SF8 | The CPU fallback now materializes the source sections and drops the destination sections before `nv3089` writes guest memory. The stale host copy was a correctness bug, not only a missing GPU blit; the GPU blit remains open. |
| L19 | The probe level-5 sampled-texture dump computed a compressed length as `width * height * block_size`, which is 16x the real byte span. It now uses block dimensions for DXT1/23/45. |

## Batch 7 — 20:55 probe-2 follow-up (correctness probes)

| id | change |
|---|---|
| L25, L26 | The draw path records `draw/interpreter` and `draw/recompiler` as two separate latches, so the interpreter's coverage of the draw stream is visible without text. `MTL_PROBE_LATCH` registers its name once per call site, so the branch is split into two macros; a ternary name silently records only the branch that ran first. **Fourteen pre-existing sites had that ternary defect** (the `feature/*`, `bind/*`, `draw/not indexed`, `draw/nonempty`, `texture/resolved`, `sync/barrier *`, `present/flip *`, `present/source *`, `sync/semaphore wait/frame *`, `overlay/clip space *` and `transfer/*` splits); all are fixed the same way, and the earlier coverage data for them is a branch total (L34). |
| L33 | The interpreter records the reason it stands aside as five latches: `interpreter/reject vertex textures`, `…/fragment control`, `…/texture state`, `…/no output` and `…/alpha never`, plus `interpreter/pipeline not ready` when the source is eligible but its pipeline is not linked yet. |
| L28 | The upload sites record the resolved host format (`texture host format`) and the upload context (`texture upload context`); a blit-engine context is the FMV/video route. |
| L33 | The bind sites record `texture/bind null fragment` and `texture/bind null vertex` when a bound unit fails `validate()`. |
| L33 | `cached_texture_section::copy_texture` records `texture/readback on miss` / `on flush` with the guest format (`texture readback format`) and the upload context (`texture readback context`). |
| L33 | `decode_depth_plane` and `encode_depth_plane` record `surface/depth plane decode` / `encode` with the guest depth format (`surface depth plane format`). |
| L33 | The draw path records `draw vertex program` and `draw fragment program` by ucode hash at level 3, so the level-2 numbers stay clean. |
| N11 | The bulk probe text (the perf report, the coverage table and the level-3/5 verbose family) goes to `$TMPDIR/rpcs3-mtl-probe.log`; a warning and an error stay in the shared log. While a probe level is set, `sys_spu`, `sys_event`, `cellDmux` and `cellVdec` drop to `error`, because they were 93 % of a 397 k-line log. That block is `TODO(revert)` and is not part of the backend. |

## Batch 2 — landed this turn

| id | change |
|---|---|
| L1 | A guest texture with a zero extent, a non-square cube or an over-long mip chain no longer reaches Metal as an illegal descriptor. `normalize_texture_extent` produces the legal host extent and `make_descriptor` reports the first eight substitutions. L10 later found the remaining cube `arrayLength` field; it is now 1. |
| TX1 | A vertex texture uses nearest min and mag filters and does not sample the mip chain. |
| TX2 | A vertex texture carries `CELL_GCM_TEXTURE_UN`; `setNormalizedCoordinates` receives it and R is clamp-to-edge in that mode. |
| TX4 | Anisotropy rounds up to the Metal powers of two, so the guest 6, 10 and 12 become 8, 16 and 16. |
| TX6 | `convolution_min` no longer samples the mip chain. |
| F4 | `disable_msl_fast_math` selects `MTL::MathModeSafe` at MSL compile time. |
| F6 | The shader preload uses `shader_compiler_threads_count` when the setting is non-zero. |
| TX9 | A typeless texture copy stages through a shared buffer with two Metal blits and a blit-to-blit barrier. Compressed or unequal-texel copies still fail with a message. |

## Batch 3 — landed this turn

| id | change |
|---|---|
| VX1 | A multi-subrange indexed draw now advances the Metal index address and shrinks the declared length; the byte offset no longer lands in `baseVertex`. |
| VX2 | `stream_ring` and `byte_ring` grow to the block size when one allocation exceeds the ring capacity, instead of allocating the old capacity and overrunning. |
| VX3 | Both rings reserve on a four-byte boundary, so an index, vertex or uniform block stays aligned. |
| DR3 | A 16-bit strip with restart disabled is widened to `u32`, so `0xFFFF` stays a vertex index instead of a Metal restart. |
| DR6 | A no-attribute draw drains its clause dependencies through `execute_nop_draw()`, as GL does. |
| L3 | The skip path reports the four `should_skip_draw` predicates for the first 64 refusals. |
| TX3 | A fragment texture whose format needs a shader-side SEXT, gamma or depth decode is sampled with nearest filters, so the filter never runs on the encoded value. |
| F13 | `shader_precision = low` selects native fp16 fragment registers. |
| PF9 | A live resolution-scale change is applied to the resident surfaces at flip. |

## A. Present and flip

| id | defect | Metal site | reference | state |
|---|---|---|---|---|
| PF1 | A flip that finds the frame closed returned with no present and still signalled the completion. | `MTLGSRender.cpp` flip | `VKPresent.cpp:495-511`, `:983-988` | fixed |
| PF2 | `on_semaphore_acquire_wait` submitted and cleared `m_frame_open` mid-frame. | `MTLGSRender.cpp` | `VKGSRender.cpp:1136-1144` | fixed |
| PF3 | `begin_frame` waited on the slot feedback with no timeout. | `MTLCommandRecorder.cpp` | `VKPresent.cpp:250` | fixed |
| PF4 | The PF1 guard skipped `retire_overlays` and `textures.on_frame_end`. | `MTLGSRender.cpp` | `VKPresent.cpp:256-274` | fixed |
| PF5 | `on_semaphore_acquire_wait` ignored `emu_requested`. | `MTLGSRender.cpp` | `VKGSRender.cpp:1138-1141` | fixed |
| PF6 | `flip` never called `evaluate_cpu_usage_reduction_limits`. | `MTLGSRender.cpp` | `VKPresent.cpp:586-589` | fixed |
| PF7 | `bind_framebuffer` never called `check_zcull_status(true)`. | `MTLGSRender.cpp` | `VKGSRender.cpp:2851` | fixed |
| PF8 | A nil drawable, or a drawable with no texture, drops the frame; the next flip re-acquires. | `MTLPresenter.cpp` | `VKPresent.cpp:596-633` | open |
| PF9 | `flip` never re-synced `resolution_scaling_config`. Slot tags carry the salt, so a mismatch re-allocates. The flip now clones the resident surfaces before the output pass. | `MTLGSRender.cpp` | `VKPresent.cpp:991-1016` | fixed |
| PF10 | `presenter::acquire` called `update_vsync` on every acquire. | `MTLPresenter.cpp` | `VKPresent.cpp:438-441` | fixed |
| PF11 | A missing present source presented nothing and left a stale image. | `MTLGSRender.cpp` | `GLPresent.cpp:276-281` | fixed |
| PF12 | The two present buffers are routed and read back, but their content is unverified; the old review found no flicker on this machine. | `MTLPresent.cpp` | `VKPresent.cpp` | needs run (REVIEW I7) |

## B. Draw, geometry, vertex, index, and binding

| id | defect | Metal site | reference | class | sev | state |
|---|---|---|---|---|---|---|
| DR1 | A non-indexed clause was never split at `primitive_restart_barrier`. | `MTLGSRender.cpp` dispatch | `VKDraw.cpp:1186-1201` | wrong result | H | fixed |
| DR2 | An indexed clause was never split at `primitive_restart_barrier`. | `MTLGSRender.cpp` dispatch | `VKDraw.cpp:1242-1252` | wrong result | H | fixed |
| DR3 | A 16-bit strip with restart disabled passes `0xFFFF`, which Metal reads as a restart. The index visitor widens the array to `u32` (KK `kk_cmd_draw.c:1708-1733`). | `MTLGSRender.cpp` index visitor | `BufferUtils.cpp:546-549` | wrong result | M | fixed |
| DR4 | RSX flat shading is unreachable. Needs geometry reordering (G1). | `MTLDevice.cpp`; `MTLVertexProgram.cpp` | `VKProgramBuffer.h:54-57` | missing feature | M | parked |
| DR5 | Front-face winding is inverted. Metal has a top-left origin like Vulkan, so it must not invert. | `MTLRasterState.cpp:66-74` | `VKGSRender.cpp:201-210`; KK | wrong result | H | needs A/B |
| DR6 | A no-attribute draw returned without draining clause dependencies. It now runs `execute_nop_draw()` and `end()`. | `MTLGSRender.cpp` | `GLDraw.cpp:755-766` | diagnostic | L | fixed |
| DR7 | A pass-open failure returned without a NOP draw. | `MTLGSRender.cpp` | `VKDraw.cpp:1283-1288` | diagnostic | L | fixed |
| DR8 | The conditional-render slot is dead ABI; the CPU skip path covers correctness. | `MTLArgumentBinds.h` | `VKVertexProgram.cpp:435-442` | not a deviation | L | no action |
| DR9 | Pipeline creation blocks on the RSX thread; `compile_async` is ignored. The library and link now use completion handlers; a miss skips and drains later. | `MTLProgramPipeline.cpp` | `VKProgramBuffer.h:51` | performance | M | fixed |
| DR10 | Every draw clause opens and closes its own render pass. | `MTLGSRender.cpp` | `VKDraw.cpp:1155-1171` | performance | H | open |
| DR11 | Inlined-array `max_index` matches GL, not VK. | `MTLGSRender.cpp` | `GLVertexBuffers.cpp:141` | not a deviation | L | no action |

## C. Surface, clear, blit, and 2D engine

| id | defect | Metal site | reference | class | sev | state |
|---|---|---|---|---|---|---|
| SF1 | The bound-target lock loop had no `address && pitch` guard. | `MTLGSRender.cpp` | `VKGSRender.cpp:2780` | wrong result | M | fixed |
| SF2 | The orphan write-back loop had no `is_locked()` re-check. | `MTLGSRender.cpp` | `VKGSRender.cpp:2749-2761` | wrong result | M | fixed |
| SF3 | A partial clear mask is treated as a full RGBA clear. | `MTLFramebuffer.cpp` | `VKGSRender.cpp:1516-1620` | wrong result | M | fixed |
| SF4 | The clear colour ignores the format's component map. Needs a KK K1-style map. | `MTLSurfaceState.cpp`; `MTLFramebuffer.cpp` | `VKGSRender.cpp:1526-1578` | wrong result | H | fixed |
| SF5 | A partial stencil clear ignores `stencil_mask()`. | `MTLGSRender.cpp`; `MTLFramebuffer.cpp`; `MTLFill.cpp` | `VKGSRender.cpp:1624-1638` | wrong result | M | fixed |
| SF6 | Surface match compared the guest spp to the host count. | `MTLSurfaceCache.cpp` | `VKRenderTargets.h:595` | performance | M | fixed |
| SF7 | A surface never sets a native component layout; `b8`, `g8b8`, `x32` reads are wrong. | `MTLSurface.cpp` | `VKRenderTargets.h:290` | wrong result | H | fixed |
| SF8 | No GPU blit path; `scaled_image_from_memory` still returns false. The CPU fallback now flushes the source and invalidates the destination, so the result is correct but slower. | `MTLGSRender.cpp` | `VKTextureCache.cpp:1686` | performance | L | partial (correctness fixed; GPU blit open) |
| SF9 | Depth read-back writes the host plane; `z16_float` overruns, `z24s8` loses stencil. | `MTLSurface.cpp` | `VKTexture.cpp:118-229` | wrong result | H | fixed |
| SF10 | Read-back mixes host extent with guest pitch when resolution scaling is on. | `MTLSurface.cpp` | `VKRenderTargets.h:645-655` | wrong result | M | needs run |
| SF11 | `is_compatible_surface` compared the guest GCM format. | `MTLSurfaceCache.cpp` | `VKRenderTargets.h:480` | performance | L | fixed |
| SF12 | Bound targets were locked before the superseded/orphan release. | `MTLGSRender.cpp` | `VKGSRender.cpp:2732-2810` | wrong result | M | fixed |
| SF13 | Same as PF7. | `MTLGSRender.cpp` | `VKGSRender.cpp:2851` | wrong result | M | fixed |
| SF14 | A packed 16-bit surface is self-consistent inside Metal but the raw write-back moves the red and blue fields, and the `x1r5g5b5` alpha bit, because Metal has no host format in the guest bit order. | `MTLSurface.cpp:482-517`; `MTLFormats.cpp:47-53` | `VKGSRender.cpp:44-53` uses the guest format natively | wrong result | M | planned (REVIEW R2) |
| SF15 | `is_compatible_surface` compares the host pixel format but not the component layout, so `x8r8g8b8_z`, `x8r8g8b8_o` and `a8r8g8b8` are interchangeable although their alpha reads differ. | `MTLSurfaceCache.cpp:36-42` | RSX surface identity | wrong result | M | fixed |

## D. Video texture diagnostics

| id | defect | Metal site | reference | state |
|---|---|---|---|---|
| V1 | The draw sample scan passed `tex.pitch()` (0 for swizzled) and warned falsely. | `MTLGSRender.cpp` | `texture_cache.h:3110-3127` | fixed |

## E. Shader and pipeline precompilation

Verified landed this turn: the on-disk pipeline set loads on init with the threaded preload
(`rsx_cache.h:280` includes `video_renderer::metal`), the draw site passes `allow_notification =
true`, the compiled pipeline feeds `shader_cache::store`, and `device::flush_pipeline_archive`
writes the driver archive on exit (`MTLGSRender.cpp:656-679`, `:717`, `:1887-1894`). The first-use build is now asynchronous (`DR9`): the MSL library and the render pipeline link both
use completion handlers and a `CompilerTask`. The draw path submits and skips; `begin()` and `end()`
drain; the warm preload drains one bounded queue; teardown waits once and gates the archive. The
remaining work is the device acceptance run, not the API wiring.

| id | item | state |
|---|---|---|
| S1 | L2 on-disk shader set: load, store, `allow_notification = true`. | landed |
| S2 | L3 driver pipeline archive: load, flush at exit. | landed |
| S3 | Parallel preload includes Metal; worker count is the queue depth. | landed |
| S4 | Shader context passes `pipeline_task_options()`. | landed |
| S5, DR9 | First-use build is asynchronous: both the MSL library and the render pipeline link use completion handlers. Draw misses skip; begin/end drain; teardown is bounded. | landed |
| S6 | The in-tree GLSL-to-MSL hop has no test reach, so the binding contract is verified only through SPIRV-Cross. | needs run (REVIEW A9) |

## F. Backend feature and settings parity

`DEVIATIONS-FEATURES.md` holds the full audit: 63 video-setting rows, 22 capability rows, and 14
KosmicKrisp technique rows. The self-declared ignored list at `MTLDevice.cpp:79-113` over-reports:
six of its entries are honoured through shared code. The rows below are the confirmed gaps, in
impact order.

| id | item | site | reference | state |
|---|---|---|---|---|
| F1 | `output_scaling` is never read; the nearest pass is built but never run, so Metal always samples bilinear. | `MTLPresent.cpp` | `VKPresent.cpp:804-821, :885` | fixed (nearest); FSR open |
| F2 | RCAS sharpening: `rcas_sharpening_intensity` is never read. | `MTLPresent.cpp` | `VKPresent.cpp` upscale chain | open |
| F3 | FidelityFX Super Resolution is absent. A faithful port of the FSR compute pass is a separate unit of work. | `MTLPresent.cpp` | `upscalers/fsr_pass.h` | open |
| F4 | `disable_msl_fast_math` is shown in the GUI and never read by Metal. | `MTLShader.cpp` | Metal compiler options | fixed |
| F5 | `shadermode` is never read; there is no interpreter fallback for a shader stall. The Vulkan/GL boot bar is interpreter variants, not game-shader precompilation. Design complete and audited: `LLD-shader-interpreter.md` (compact 16-sampler map, separate Metal 4 tables, no-skip policy, set=0 instruction block, output trimming, separate binder, compile-mutex serialization). The 2026-10-21 audit found 26 confirmed defects; all are corrected in the design and listed in LLD §20. | `MTLGSRender.cpp` | `VKGSRender.cpp` | design complete and audited; implementation pending (P2) |
| F6 | The preload ignored `shader_compiler_threads_count`. The worker count follows the setting when it is non-zero. | `rsx_cache.h:280-281` | same | fixed |
| F7 | `host_label_synchronization` is never read; the capability is forced false. | `MTLDevice.cpp:314` | `VKGSRender.cpp` | open |
| F8 | `disable_vertex_cache` has no effect because Metal uploads vertex data per draw. | `MTLGSRender.cpp` | `VKVertexBuffers.cpp` | open |
| F9 | `force_high_precision_z_buffer` has no effect; depth is always `Depth32Float`. | `MTLFormats.cpp:82` | `VKRenderTargets.h` | open |
| F10 | The debug overlay is created but never given text, so the RSX statistics pane is blank. | `MTLOverlays.cpp` | `RSXOverlay.cpp` | open |
| F11 | No GPU blit path; `scaled_image_from_memory` still returns false, so two blit settings are no-ops. The CPU fallback is now correct (same as SF8). | `MTLGSRender.cpp:3291` | `VKTextureCache.cpp:1686` | partial (same as SF8) |
| F12 | The RSX sample mask is not applied: no encoder setter exists and the MSL emits no `[[sample_mask]]` test. | `MTLDepthStencil.h:65-70` | `VKPipelineCompiler.cpp:151`; KK `kk_shader.c:552-567` | open (SHADER S02) |
| F13 | The fragment decompiler hardcoded native fp16 off. `shader_precision = low` now selects the f16 register path; the two depth emulation bits stay off because both paths are native. | `MTLFragmentProgram.cpp` | `VKFragmentProgram.cpp:632-638` | fixed (SHADER S03) |
| F14 | Depth-bounds emulation is not implemented; the drop is safe but the guest effect is lost. | `MTLDepthStencil.cpp` | `VKDraw.cpp:240-271`; LLD-depth-rop §6 Q3 | open, needs title (REVIEW E7) |

The remaining audit rows are lower-impact and recorded in `DEVIATIONS-FEATURES.md`; each is either a
shared-code path that already honours the setting or a setting with no Metal-visible effect.

## G. Synchronization, resource lifetime and command recording

`DEVIATIONS-SYNC.md` holds the full audit (SY1–SY12). The sync path is sound inside one frame for
RAW and WAR pairs. The gaps are on the recovery paths and in the cross-frame model. The rows below
are the confirmed gaps, in impact order.

| id | item | site | reference | severity | state |
|---|---|---|---|---|---|
| SY1 | The texture-cache read-back waits on the commit feedback with no timeout, so a lost feedback hangs the RSX thread. | `MTLCommandRecorder.cpp:277-331` | `VKTextureCache.cpp:1472-1473` | H | fixed |
| SY2 | On a feedback timeout, `begin_frame` still calls `on_commit_complete`, freeing deferred allocations the abandoned command buffer may still read. The only memory-corruption row. | `MTLCommandRecorder.cpp:164-192` | `VKPresent.cpp:245-254` | H | fixed |
| SY3 | The timeout path rebuilds a slot without releasing the old objects; shutdown releases allocators with work in flight. | `MTLCommandRecorder.cpp:45-70, :164-176` | `VKGSRenderTypes.hpp:110-129` | M | fixed |
| SY4 | The binder never writes a zero value, so an unbound texture or buffer slot keeps its earlier entry. | `MTLArgumentBinds.cpp:43-80` | `VKDraw.cpp:727` | M | partial |
| SY5 | The persistent residency set has no removal path; every published allocation is retained forever. | `MTLResourceManager.cpp:206-233, :461` | KK `kk_bo.c:82-89` | M | fixed |
| SY6 | Dedicated placement heaps are never destroyed and the pool keeps its high-water mark. | `MTLResourceManager.cpp:24-33, :62-77` | `VKPresent.cpp:189-197` | L | open |
| SY7 | No hazard record survives a frame boundary, and three frames can be in flight. Hypothesis. | `MTLPassScheduler.cpp:30-37` | `VKPresent.cpp:187-209` | H | needs capture |
| SY8 | The scheduler has no WAW edge and no read edge for a render-pass load. Hypothesis. | `MTLPassScheduler.cpp:49-80` | `VKGSRender.cpp:1511, :1587` | H | needs capture |
| SY9 | The slot generation is read under `m_mutex` but written without it. | `MTLCommandRecorder.h:30` | `VKGSRenderTypes.hpp:55-77` | M | fixed |
| SY10 | Every MSAA colour clause resolves, then the on-demand resolve can run again. | `MTLFramebuffer.cpp:154-158` | KK `kk_cmd_buffer.c:895-922` | M | open |
| SY11 | Residency publication is one `commit()` per allocation and never calls `requestResidency`. Hypothesis. | `MTLResourceManager.cpp:206-216` | KK `kk_device.c:430-468` | M | needs capture |
| SY12 | The unbounded drawable wait matches Vulkan and is not a deviation. | `MTLCommandRecorder.cpp:263-275` | `VKPresent.cpp:591-595` | L | no action |

## H. Texture, sampler and copy parity

`DEVIATIONS-TEXTURE.md` holds the full audit (TX1–TX20). The upload, deswizzle and mip path is
correct for the common texture. The gaps are in the filter rules, the view construction, the
packed-format field order and the format-changing copies.

| id | item | site | reference | severity | state |
|---|---|---|---|---|---|
| TX1 | A vertex texture keeps the guest filters; Vulkan and OpenGL force nearest and drop mipmaps. | `MTLSamplerState.cpp:186-197` | `VKDraw.cpp:606-619` | M | fixed |
| TX2 | A vertex texture never sets `normalizedCoordinates = false`. | `MTLSamplerState.cpp:210-237` | `VKDraw.cpp:594,608,617` | M | fixed |
| TX3 | The fragment sampler did not force nearest for a shader-side SNORM, sRGB or depth decode. The sampler now carries `force_nearest` from `format_ex` and the depth class. | `MTLSamplerState.cpp` | `VKDraw.cpp:376-479` | M | fixed |
| TX4 | `setMaxAnisotropy` receives the guest value; Metal requires a power of two. | `MTLSamplerState.cpp:91-106` | `VKDraw.cpp:520-534` | M | fixed |
| TX5 | A custom border colour is lost. | `MTLSamplerState.cpp:119-134` | `VKDraw.cpp:404-441` | L | open |
| TX6 | `convolution_min` becomes mipmapped linear. | `MTLSamplerState.cpp:81-85` | `VKFormats.cpp:44` | L | fixed |
| TX7 | A 3D texture returns the base image; its layout and unit remap are lost. | `MTLHandles.cpp:19-22` | `VKTextureCache.cpp:843` | M | open |
| TX8 | A cube that needs a view gets a 2D six-slice view. | `MTLHandles.cpp:43-54` | `VKTextureCache.cpp:728-738` | M | needs run |
| TX9 | A typeless texture copy is skipped; the temporary keeps uninitialised contents. It now stages through a shared buffer with two blits and a blit barrier. | `MTLTextureCache.cpp` | `VKTextureCache.cpp:467-532` | H | fixed (unit-tested texel rule) |
| TX10 | A scaled texture copy is skipped. | `MTLTextureCache.cpp:358-362` | `VKTextureCache.cpp:537-589` | M | open |
| TX11 | The surface compatibility test requires the exact host pixel format. | `MTLTextureCache.h:517-520` | `VKTextureCache.cpp:1379-1417` | M | open |
| TX12 | The texture-cache depth read-back copies the host plane (the second site of SF9). | `MTLTextureCache.cpp` | `VKTexture.cpp:118-229` | M | fixed |
| TX13 | A 2-sample request folds up to 4. This is the chosen Metal mapping: the guest lattice and `get_spp()` stay at 2, and only the host descriptor uses 4. No guest-visible mismatch. | `MTLResourceManager.cpp:303-309`; `MTLSurface.cpp:36-40` | `VKRenderTargets.h:241-249` | L | no action |
| TX14 | An unknown guest texture format becomes `BGRA8Unorm` instead of failing the lookup. | `MTLFormats.cpp:308-311` | `VKFormats.cpp:237-240` | L | open |
| TX15–TX18 | Not a deviation: component map, LOD range, resolve store actions, 2D engine. | — | — | L | no action |
| TX19 | The packed 16-bit textures used the GL identity swizzle on a Metal host format with the opposite field order, and swapped the two alpha-bearing formats. | `MTLFormats.cpp:389-406, :467-490` | `VKFormats.cpp:200-205`; KK `kk_format.c:224-234` | H | fixed (REVIEW R1) |
| TX20 | `R6G5B5` stores six bits of red; `B5G6R5Unorm` stores six bits of green, so no permutation recovers the guest field. | `MTLFormats.cpp:392-394` | `TextureUtils.cpp:99-111` | M | planned (REVIEW R3) |

## L. Vertex and index path

Full audit: `DEVIATIONS-VERTEX.md`. The vertex upload is correct in layout and semantics. The gaps
were the Metal 4 argument order for an index subrange and the growth rule of the two byte rings.

| id | item | site | reference | severity | state |
|---|---|---|---|---|---|
| VX1 | One subrange draw passed `index_offset * type_size` as `baseVertex`. Metal 4 has no index-buffer offset argument, so every later subrange drew from the wrong vertices. The address now advances and the declared length shrinks. | `MTLGSRender.cpp` indexed subrange loop | `VKDraw.cpp:1242-1252`; `MTL4RenderCommandEncoder.hpp:68` | H | fixed |
| VX2 | A single allocation larger than the ring capacity allocated the old capacity and then copied past the buffer end. The reservation grows to the block size. | `MTLConstants.cpp` `stream_ring::alloc`, `byte_ring::alloc` | `VKVertexBuffers.cpp` ring reserve | H | fixed (unit-tested) |
| VX3 | `byte_ring` placed each block at the previous end with no alignment. The index, overlay vertex and overlay uniform blocks now reserve on a four-byte boundary. | `MTLConstants.cpp` `byte_ring::alloc` | `VKVertexBuffers.cpp`; `GLVertexBuffers.cpp` | M | fixed (unit-tested) |
| VX4 | The vertex upload writes the whole `[min_index, max_index]` range every subdraw. GL keeps `vertex_range.second` and uploads only the missing tail; Metal has no equivalent cache. | `MTLGSRender.cpp` vertex upload | `GLVertexBuffers.cpp:57-150`; `VKVertexBuffers.cpp` | M | open (performance) |

The remaining checks are no action: the base-index rebase, the inline-array extent, the expanded
index output and the `vertex_index_offset` handoff reproduce the reference.

## I. Recorded defects that are not Metal-backend defects

| id | item |
|---|---|
| N1 | RR7 mode-select freeze follows a shared FIFO desync and a guest label spin; the Metal mechanism was PF1/PF2. |
| N2 | `cellDmux` EPERM and `CELL_DMUX_ERROR_SEQ` are upstream-correct. |
| N3 | Cross-title HLE video-state contamination (Mega Man 9 then RR7 in one process). |
| N4 | Audio restart, second input press, shutdown teardown noise: core, not Metal. |

## J. Occlusion queries, ZCULL and conditional render

The full audit is `DEVIATIONS-QUERY.md`; these rows are the actionable list. The gate now
uses delivered commit feedback, so a result is readable in the frame that produced it.

| id | defect | Metal site | reference | class | sev | state |
|---|---|---|---|---|---|---|
| Q1 | A forced ZPASS read never submitted or waited, so it returned 0 or a partial count. | `MTLZCULL.cpp` | `VKGSRender.cpp:2952-2966` | wrong result | H | fixed |
| Q2 | The visibility mode was cached on a raw encoder pointer and never invalidated, so an address-reusing pass counted nothing. | `MTLZCULL.cpp` | KK `kk_cmd_draw.c:1435-1437` | wrong result | H | fixed |
| Q3 | A timed-out slot was still reported complete, so a counter could be reused while the abandoned frame wrote it. | `MTLCommandRecorder.cpp`; `MTLGSRender.cpp`; `MTLZCULL.cpp` | `VKGSRenderTypes.hpp:154-172` | wrong result | H | fixed |
| Q4 | A query was readable only at slot reuse, three frames late. | `MTLZCULL.cpp`; `MTLCommandRecorder.cpp` | `VKQueryPool.cpp:168-182` | performance | M | fixed |
| Q5 | The boolean occlusion mode was never used, so `precise_zpass_count` did not reduce the hardware cost. | `MTLZCULL.cpp` | `VKGSRender.cpp:602-605`; KK `kk_query_pool.c:423-429` | performance | M | fixed |
| Q6 | An exhausted counter pool answered "fully visible" (-1) where VK and GL answer 0. | `MTLZCULL.cpp` | `VKGSRender.cpp:2944-2948` | wrong result | L | fixed |
| Q7 | No counter heap, so no per-pass GPU time. | `MTLCommandRecorder.cpp` | KK `kk_query_pool.c:198-207` | missing feature | L | open (diagnostic only) |

## K. Run-log findings

Source: `LOG-FINDINGS.md`. The earlier run is RR7 (`BLUS30001`), 2026-10-19 23:26:06 to 23:28:00,
133,106 log lines, and four `.ips` reports. The following run is 2026-10-20 22:01:37 to 22:05:17,
22.7 MB, one `.ips`. The current run is 2026-10-21 20:55, `MTL_PROBE=2`, 397,107 lines, `rc=0`
after 2:19. L1 and L10 are Metal crashes. L2 and L4–L9 are shared, guest or host findings;
they are recorded here so every log observation has an owner, and they are not Metal-backend
defects. The L25–L33 rows are the 20:55 run.

### K.1 Metal, open

| id | finding | site | reference | sev | state |
|---|---|---|---|---|---|
| L11 | The Xevious mini-game panel on the startup/loading screen is completely black, but the X button produces the correct audio feedback. Input and guest logic run; the panel is not rendered or not sampled. The panel's render path, skip reason, surface format and copy route are not proven. | `MTLGSRender.cpp` skip path; `MTLFramebuffer.cpp` clear; `MTLTextureCache.cpp` copy | 22:05 screenshot; `SUBAGENT-RR7-2026-10-20.md` | H | needs run/probe |
| L12 | The race-selection background video and the FMVs are completely black. The 22:05 run decodes and samples video, so the earlier “shared HLE only, no picture” reading is obsolete. The decode-to-sample path, invalidation, format/pitch and barrier are not proven. | `MTLGSRender.cpp` `load_texture_env`; `MTLTextureCache.cpp` upload/invalidate | `cellVdec.cpp`; `VIDEO-TEXTURE-PARITY.md` | H | needs run/probe |

### K.2 Metal, fixed

| id | finding | site | reference | sev | state |
|---|---|---|---|---|---|
| L1 | Metal aborted in `validateWithDevice` inside `heapTextureSizeAndAlign` during a fragment-texture CPU upload from `MTLGSRender::end`. A guest descriptor could carry a zero extent, a non-square cube or a zero mip count, and nothing logged which one. The L1 fix covers those fields; L10 found the remaining cube `arrayLength` field, and that is now 1. | `MTLTextureCache.h` (`make_descriptor`); `MTLTextureCache.cpp` (`normalize_texture_extent`) | `MTLTextureCache.cpp` `create_new_texture`; `MTLResourceManager.cpp` `new_texture` | H | fixed (unit-tested) |
| L3 | `should_skip_draw` refused a draw and substituted a NOP with no reason bits. The skip path now reports the four predicates for the first 64 refusals at probe level 2. | `MTLGSRender.cpp` skip path | `RSXThread.cpp:1012-1025`; DR8 | M | fixed |
| L10 | The 22:05 RR7 run aborted with SIGABRT in `MTLTextureDescriptor validateWithDevice`, inside `heapTextureSizeAndAlign`. Root cause: `make_descriptor` set `arrayLength = 6` for `TextureTypeCube`; Metal requires 1. The last flushed line was a 256x256 cube with 7 mips and format 134. A local Metal probe reproduced the exact assertion with 6 and passed with 1; `depth` must also be 1, and the uploader still addresses the six faces by slice. The fix is built and the suites are green; on-device RR7 remains. | `MTLTextureCache.h` (`make_descriptor`) | crash report `rpcs3-2026-10-20-220517.ips`, thread 34; local cube probe | H | fixed (probe-verified; RR7 run pending) |

### K.3 Not Metal

| id | finding | owner | state |
|---|---|---|---|
| L2 | LLVM JIT teardown SIGSEGV on quit (`llvm::Module::~Module`). Already recorded as I6. | shared timing/JIT | fixed (I6) |
| L4 | The fragment decompiler reads ROP output r0 before a write; four shader workers hit it. The log does not name the shader. | shared decompiler | open (needs the shader dump) |
| L5 | SPU JIT detects a channel loop and asks for a report. No hang in the log. | SPU | open (guest image) |
| L6 | The VM cannot lock 15 guest ranges; the run continues. | host wired-memory limit | open (environment) |
| L7 | The SPU recompiler warns 671 times about MFC_EAH and 85 times about MFC_Cmd. | SPU | open (needs a dynamic-MFC test) |
| L8 | The guest asks for `/dev_hdd0/game/BLJS10001/USRDIR/RR7.DAT` and falls back to the disc copy. | guest data | open (environment) |
| L9 | Low Power Mode is on; timing results are lower bounds. | host | open (environment) |

### K.4 The 20:55 probe-2 run (L25–L33)

| id | finding | site | sev | state |
|---|---|---|---|---|
| L25 | Compilation is not the visible-bug path. All `program/*` timers land in the first interval (VS 54 calls / 1.16 s, FS 71 calls / 0.32 s, `program/pipeline` 97 calls / 6.45 µs avg); later reports are empty. All 97 links are asynchronous and none fails. | `MTLProgramPipeline.cpp` | L | evidence |
| L26 | No draw is skipped and no pipeline or bind fails: `draw/skipped pipeline pending`, `draw/skipped pipeline failed` and `texture/bind dropped` are zero. The visual failures are not the skip path. | `MTLGSRender.cpp` skip path | L | evidence |
| L27 | The run is clean of Metal faults; the only RSX warnings are the benign `ROP reads from rN` lines. | — | L | evidence |
| L28 | DXT45 (136) 396,544 dominates the bound formats, then DXT1 171,588, A8R8G8B8 129,696, DXT23 8,530, B8 2,852. The 1,615 host resources resolve to BC3 802, BC1 528, BGRA8 259, R8 13, `Depth32Float_Stencil8` 12, BC2 1. The BC route is the first texture suspect; the depth-format resources are the first depth-as-colour suspect. | `MTLGSRender.cpp` upload; `MTLTextureCache.h` `create_image` | M | evidence |
| L29 | The RSX thread is GPU-bound and drains once a frame: `recorder/wait idle` and `sync/label release` are 5,197 ms / 204 calls (25.5 ms avg, 342 ms max) against `frame/gpu us` 21.4 ms avg. The L13 drain is on the frame-critical path; the native form is a GPU-side label write. | `MTLGSRender::release_GCM_label`; `MTLCommandRecorder::wait_idle` | M | open |
| L30 | The semaphore wait is a spin: 1,259 ms over 38,793,305 entries in 10.0 s (12.6 % of wall), and the `sync/semaphore waits` latch reaches 584 M over the run, about 190 k entries a frame. | `MTLCommandRecorder` semaphore wait | M | open |
| L31 | 383 of 3,803 flips (10.1 %) take `present/compose refused` and present a clear: `present/flip delivered` = `present/compose ran` + `present/compose refused`. Part of a missing picture is present-side. | present compose | M | open |
| L32 | The boot black screen is the intro FMV: `cellVdec` / `cellDmux` traffic fills the same window and no draw error accompanies it. The FMV blank is an upload, bind or compose issue. | `MTLTextureCache.cpp` blit upload; present compose | M | needs run (probes added) |
| L33 | The probe could not see correctness. This pass adds the interpreter, host-format, null-bind, readback and depth-plane families, and moves the bulk text to `$TMPDIR/rpcs3-mtl-probe.log`. | the probe facility | M | fixed |
| L34 | Fourteen `MTL_PROBE_LATCH` sites used a ternary name, so each site counted every event under the variant that ran first. Every split family in the 20:55 coverage table is a branch total (`LOG-FINDINGS.md`, correction). All fourteen are now one macro per branch. | `MTLInstrumentation.h` macro contract; fourteen sites | M | fixed |

The L11 and L12 checks now have named evidence to collect: `draw/interpreter` vs `draw/recompiler`
and the `interpreter/reject …` reasons for L11, and `texture host format`, `texture upload context`
and the null-bind latches for L12. Both still need the run.

The non-Metal rows are indexed in `LOG-FINDINGS.md`; they do not change the Metal exit criteria.
The benign and already-known log lines (access violation, `cellDmux`, I6) are listed there under
"Already known".

<!-- doc-end -->
