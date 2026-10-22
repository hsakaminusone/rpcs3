# Handoff — the RSX to Metal 4 backend

This document hands over the Metal 4 backend. Read it first. Then read `METAL4-TODO.md` for the live
work list, `REALISATION.md` for the per-cluster status, `REVIEW-FINDINGS.md` for the itemised
defects, and `CONFLICTS.md` for design/tree disagreements.

**This revision.** The 2026-10-21/22 window is recorded in `INVESTIGATION-2026-10-22.md` (the
datapoints) and in `LOG-FINDINGS.md` as L36–L55. Nine runs (R1–R9) were made. The **FIFO desync
reproduces with no probe at all**, so it is not a diagnostic artefact, and the desync capture that
`RR7-FREEZE-FORENSICS.md` §8.1 prescribes shows why: the word the FIFO rejects is a **data word**,
and the words around it name method indices that do not exist, so **the host is executing a data
block as commands**. The label write is not the corrupting event (the label address is outside the
FIFO's guest window), and the freeze is a **recovery loop that makes no progress** (`restore_point`
equals `get`, so the recovery returns the host to the same word) that ends when the GET runs off
the end of the mapped region. The black video and the black race rectangle are also a data-word
problem of the same family: the guest's `NV3089` blit had every geometry register at zero while the
same module's context registers were set, so the setup commands were consumed as part of a bulk
data block. **The next measurement is the §8 companion control on Vulkan**, which decides whether
the fault is shared or Metal-specific.

Three fixes landed: a zero `DS_DX`/`DT_DY` is the power-on value and is no longer divided by
(`nv3089.cpp:34-66`); a reused Metal texture is cleared when no upload covers it, which is the one
real divergence from the OpenGL reference and the cause of the `0xff` video surface; and a reused
image is discarded unless its format, extent and mip count match. The desync capture is a
**backend hook**: the shared FIFO gathers the data and calls `on_fifo_desync`, and the Metal
backend prints it through the probe facility, so the diagnostic belongs to the backend and the
other backends pay nothing. The 2026-10-21 design audit found 26 confirmed defects (LLD §20); the
implementation then forced five more corrections and four optimisations (LLD §21).

**The 20:55 probe-2 run** is the first clean run of the final binary (L25–L33 in `LOG-FINDINGS.md`).
It exits `rc=0` after 2:19, so the graceful-shutdown and archive-flush paths ran. It reaches the
race with no `Dead FIFO`, no `should_skip_draw` refusal, no failed link and no skipped draw
(`draw/skipped pipeline pending == 0`, `draw/skipped pipeline failed == 0`), and all 97 links are
asynchronous. So the remaining visual failures are not the skip path, not a failed compile and not
the FIFO path. What the log does show is: DXT45 textures dominate (396,544 of 708 k bound formats),
the RSX thread is GPU-bound at 21.4 ms a frame and drains the queue once a frame
(`recorder/wait idle` 25.5 ms avg), the semaphore wait is a spin worth 12.6 % of wall, and 10.1 % of
flips compose with no display buffer and present a clear. The interpreter-eligibility, texture-format,
readback and depth probes that this pass adds are the next evidence step; they were not in the
binary under test.

**The diagnostics this pass.** The bulk probe text (the perf report, the coverage table and the
level-3/5 verbose family) now goes to its own file, `$TMPDIR/rpcs3-mtl-probe.log`, so a deep run can
be read without the HLE channels on top; warnings and errors stay in the shared log. While a probe
level is set, `sys_spu`, `sys_event`, `cellDmux` and `cellVdec` drop to `error` for the run, because
they were 93 % of a 397 k-line log; that block is marked `TODO(revert)` and is not part of the
backend. The new families are `draw/interpreter` vs `draw/recompiler`, five
`interpreter/reject …` reason latches, `texture host format`, `texture upload context`,
`texture/bind null fragment` and `texture/bind null vertex`, `texture readback format` and
`texture readback context`, `surface/depth plane decode` and `surface/depth plane encode`, and the
`draw vertex program` and `draw fragment program` hashes at level 3. The CPU tests are green; the
interpreter run gate (`interpreter_only`, the standalone MSL probe) still needs an approved run. The
remaining known visual failures are the black Xevious panel, the black race-selection background
video and the black FMVs.

---

## 0. Start here

**The state.** The backend builds, boots, renders the guest frame upright and reaches real gameplay
screens on RR7. The present/flip freeze and the cube descriptor abort are fixed. The **FIFO desync
is open and is the blocker**: it reproduces with no probe, and the desync capture shows the host
executing a data block as commands (L52), with the label write exonerated (L53) and the freeze
classified as a non-advancing recovery loop (L54). The remaining known visual failures are the
black Xevious panel, the black race-selection background video and the black FMVs; the video
surface is sampled as a `framebuffer_storage` texture and was never written, and it read `0xff`
because a Metal image that was never rendered into is not zeroed — the OpenGL reference clears a
reused image and Metal did not, which is now fixed but unverified. The black rectangle on the
title screen is a separate symptom: the texture it samples is opaque white in the guest and
converts byte-identically, so that blackness is in the shading or the ROP. `INVESTIGATION-2026-10-22.md`
holds the datapoints.

**The first moves, in order.**

1. Read `METAL4-TODO.md`, then `INVESTIGATION-2026-10-22.md` for the datapoints and
   `LOG-FINDINGS.md` for L36–L55. The priority table is P0.5 label confirmation, P1 async shader
   acceptance, P2 shader interpreter. P0.5 is **reopened**: the desync is not closed.
2. **Run the `RR7-FREEZE-FORENSICS.md` §8 companion control: the identical scene on Vulkan.** One
   config line (`Renderer: Vulkan` in `config_BLUS30001.yml`) and one run. If Vulkan freezes
   identically, the fault is in the shared FIFO or in the bulk-write argument accounting and Metal
   inherits it; if only Metal freezes, the fault is in the Metal backend's FIFO interaction. No
   other measurement reduces the search space this much.
3. Read the new capture when it fires. It prints the offending word's method index and whether the
   method table knows it, the guest address of the GET, the last label write and the last eight
   dispatched commands. The bulk commands are the only ones that carry a variable-length data
   block: `NV4097_SET_TRANSFORM_PROGRAM` and `NV4097_SET_TRANSFORM_CONSTANT`
   (`nv4097.cpp:130`, `:144`, `:168`, `:202`, `:244`).
4. Re-check the level-2 and level-3 behaviour. R7 (level 3) produced 399 frames and **0 draws**
   while R8 (level 2, the same binary) reached the race loading screen. The changed texture-cache
   code did not execute in R7 (`texture/create` 0), so the difference is the probe level or the
   intermittent hang landing earlier; it is not explained yet.
5. The interpreter is implemented and the CPU tests are green (LLD §21). What remains is one
   approved run to confirm the default `async_with_interpreter` path reaches
   `draw/skipped pipeline pending == 0` (seen in the 20:55 run at real-pipeline coverage) and the
   `interpreter_only` path, and to re-check the visual failures above (LLD §15.3).
6. Do not add a startup precompile bar. The Vulkan bar is interpreter variants, not game-shader
   precompilation (P2); Metal now has an asynchronous warm preload instead.
7. Make the label release a **pipelined fence** instead of a full `wait_idle`. It costs 57.9 ms
   a frame in an 82.8 ms frame while the GPU is busy 8.68 ms of it. The native form is a GPU-side
   label write into shared memory, which unified memory allows, and Vulkan's form is
   `vkCmdUpdateBuffer` on the GPU timeline.
8. Excise the diagnostics, then commit as `hsaka -1.0` and push `origin/metal4`. The new
   diagnostic code carries `TODO(excise)` markers: the `on_fifo_desync` hook and the capture
   (`RSXThread.h`, `RSXThread.cpp`, `RSXFIFO.cpp`, `MTLGSRender.cpp`), the label record
   (`nv47_sync.hpp`) and the new texture probes (`MTLTextureCache.h`, `MTLTextureCache.cpp`).

**The context that saves a day.**

| fact | detail |
|---|---|
| the backend renders | fixed-function, surface, clear, 2-D fill, blit, present and 3-D geometry run end to end |
| the image is upright | draw viewport negates height, scissor mirrors, winding inverts (F24) |
| the present architecture is correct | drawable is the present image, output tier blits into it, layer presents; KK does the same |
| the freeze is fixed | a flip opens the frame and presents; acquire does not submit; feedback wait is bounded |
| the cube abort is fixed | `make_descriptor` sets `arrayLength = 1`; the six RSX cube faces are addressed through the upload slice. |
| the race-start fatal is fixed in code | the probe-2 log dies after 21 FIFO desync recoveries; the label value `0x10000` is read as the next command. `MTLGSRender::release_GCM_label` now drains the queue before the CPU writes. **The 20:55 run is clean over 2:19 with no `Dead FIFO` (L25).** |
| the race-start fatal is **open again** | the desync reproduces in every run since, **including with no probe** (R4). The capture shows the host executing a data block as commands, not a bad label write (L52, L53), and the freeze is a recovery loop that never advances (L54). The open question is the shared FIFO versus the Metal backend, which the Vulkan control decides. |
| the black video is a data-word problem | the video surface at `0xc0880000` is sampled as a `framebuffer_storage` texture (223 binds) and read `0xff`, so it was never written; the guest's `NV3089` blit had clip, output and scale all zero while the same module's context registers were set. The OpenGL reference clears a reused image and Metal did not (T1, now fixed, unverified). |
| the black rectangle's texture is correct | the 8×8 DXT45 it samples is opaque white in the guest and converts byte-identically, and the draw's write mask is `0xf` into an A8R8G8B8 target. The blackness is in the shading or the ROP, not the upload. |
| the async link crash is fixed | Metal 4's completion-handler `newRenderPipelineState` corrupts the pipeline data set serializer (L18). Links now run through one synchronous worker; the 10:21 scan reports 66/66 ready, 0 `MDB_*`, 0 failures. All 97 links in the 20:55 run are asynchronous and none fails (L25). |
| the probe-2 fatal | `0:05:04.547`: `Dead FIFO commands queue state has been detected!`; 21 desyncs from `0:05:04.381`; the PPU main thread then waits in `sys_semaphore_wait`. Not reproduced in the 20:55 run. |
| the probe-5 fatal | `0:00:52.766`: the same `Dead FIFO`; the same command words (`0x2fd53cc`, then `0x3f800000`) repeat; the 105-second capture window never arms. The desync is intermittent, not closed by L13. |
| the black video is not proven core | the earlier “no picture, shared HLE” reading is obsolete; the 20:55 run decodes frames and presents black, with 13 % of the log in the PAMF/vdec path and no draw error (L32) |
| the texture blame is DXT | DXT45 396,544 + DXT1 171,588 + DXT23 8,530 of 708 k bound format instances, against A8R8G8B8 129,696 and B8 2,852 (L28) |
| TX9 is built, not proven | typeless copies stage through a shared buffer; the RR7 path has not been confirmed to use it |
| the diagnostics are switchable | `MTL_PROBE` levels 1–5; a probe run writes its bulk text to `$TMPDIR/rpcs3-mtl-probe.log`; `MTL_DEBUG_LOGS=0` / `MTL_PERF_ENABLED=0` remove all of it |
| Vulkan with KosmicKrisp loads the title | the guest stream is sound; a divergence is in this backend |
| the commit identity | `hsaka -1.0 <340957754+hsakaminusone@users.noreply.github.com>` |
| the language | ASD-STE100 Simplified Technical English |
| the scope | `metal4` only, pushed to `origin/metal4`, without breaking Vulkan or OpenGL |
| one instance, never detached | kill-before, one tracked child, a cap, kill-after |

---

## 1. The backend in one page

### 1.1 What it is

RPCS3 emulates the RSX. Vulkan reaches Metal through MoltenVK or the KosmicKrisp driver. The Metal 4
backend removes the translator and drives the Metal 4 queue, command buffer and encoders directly.
It keeps the shared RSX emulation behind `rsx::thread` and the backend capability struct. The shared
part is about 55,000 lines; the backend is a second implementation of the graphics resource layer.

### 1.2 Scope and the RSX-first rule

Scope: parity with the Vulkan backend's RSX feature set. There is no Metal 3 fallback; MoltenVK is
the pre-Metal-4 fallback. The RSX is the source of truth. The design asks what the RSX does and what
the smallest Metal 4 expression is. Performance centres are the encoder boundary, the pipeline
cache, per-draw CPU work and real hazard barriers.

### 1.3 Target and environment

| item | value |
|---|---|
| SoC | Apple A18 Pro |
| OS | macOS 27.0.1 |
| memory | 8 GB |
| deployment target | macOS 15.0 |
| Metal 4 API floor | macOS 26 |
| backend runtime floor | macOS 27 |
| build | `./build`, Ninja, one build at a time, `-j4` |

The deployment target stays 15.0 so the binary loads on older systems and the backend declines at
run time. Metal 4 exists on Apple silicon only.

### 1.4 Where the code is

| role | location | note |
|---|---|---|
| Metal subsystem | `rpcs3/Emu/RSX/MTL/` | about 16k lines, 67 files |
| Metal tests | `rpcs3/tests/test_rsx_metal4_*.cpp` | about 4k lines, 150 MTL cases |
| shared RSX | `rpcs3/Emu/RSX/Common`, `Core`, `Program` | unchanged; all backends call it |
| design docs | `docs/research/metal4/design/` | this file is the master state |

The five exception sites are real error paths, not stubs. The temporary diagnostics are named by
`TODO(excise)` and are removed before the commit.

---

## 2. The rules of the work

**Evidence.** RSX behaviour is the source of truth. Evidence order: the RSX state inventory and
headers, the two shipping backends, the shared emulation, the RSX documentation. A design/tree
disagreement goes into `CONFLICTS.md`; neither side is edited in silence.

**Completeness spine.** The inventory holds 282 rows, 203 live, 178 work dials (133 `reshape`, 45
`emulate`). A coverage test fails when a live row has no disposition. “Wired” means a C++ test
writes the register, reads the latch and checks the field; it proves decode, not correct use.

**Probes.** When a mechanism is not derivable, write a standalone device probe, run it with the
validation layer on and off, keep the probe and the log, and record the measurement. A result that
changes between the two runs is a tool artefact.

**Subagents.** Read-only, no build/run/commit/push, no edits to `.kk-mesa` or `filesdump/`, exactly
one report document, report `file:line` + severity + confirmed/hypothesis.

**Build and test.** One build, four jobs. Change; build app and tests; run tests; run the document
linter and the inventory validator. A change is complete only when both pass.

**One emulator instance, never detached.** Kill before, track one child, cap the run, kill after,
confirm none is left. Concurrent or orphaned instances freeze this 8 GB machine. The local patch
replaces the `macdeployqt` post-build step with `/usr/bin/true`; keep it, and never set
`-DMACDEPLOYQT_EXECUTABLE` to get the same effect.

**Diagnostics.** `MTL_PROBE=1` is the counters and periodic report; `2` (`trace`/`verbose`) also
raises the `MTL` channel. `MTL_PROBE_INTERVAL` and `MTL_PROBE_STALL_MS` tune two probes. Every site
is `TODO(excise)`. `MTL_DBG`/`MTL_DBG_ON` must not exist, and no trace line may sit on a path that
can spin.

**Kill cover.** `pkill -9 -f` self-matches. Use `pgrep -f 'rpcs3.app/Contents/MacOS/rpcs3'` and
`ps -p <pid>`. Never `pkill -f rpcs3`.

**Config.** `Renderer:` in `~/Library/Application Support/rpcs3/config.yml` selects the backend.
Copy and restore the file for a temporary Vulkan reference run. KosmicKrisp needs no environment
variable; the loader points at the bundle's own ICD.

**Capture.** Display capture and keystroke injection are refused by TCC. The user presses keys and
reports. F12 screenshots land in `~/Library/Application Support/rpcs3/screenshots/<title id>/`; a
black frame is a few KB, a content frame tens of KB.

---

## 3. What is built

**Substrate.** A placement-heap pool per storage class, a free-list suballocator, explicit
residency sets and a per-slot deferred-release list. The guest window is one no-copy buffer over
`vm::g_sudo_addr`. The command recorder has three frame slots, one allocator, one reusable command
buffer and one argument table per stage; it is the only way to open a pass, close an encoder or
submit. The barrier chokepoint carries producer, consumer and visibility; a `false` return asks the
pass scheduler to split. The surface store projects one guest range to an owned texture, a resolve
target and deferred data movement.

**Pipeline and binding.** The key is the shared properties struct. The argument table replaces
descriptor sets, layouts and push constants; its order is mutate, bind, draw. Cull, front face,
fill, depth clip, bias, viewport, scissor, stencil references, blend colour and the depth-stencil
object are encoder state and key nothing.

**Shaders.** Guest ucode → decompiled GLSL (Vulkan rules) → SPIR-V (vendored glslang) → MSL
(vendored SPIRV-Cross) → Metal library. The chain keeps about 5,000 lines of shared emulation. No
text post-passes. Programmable blending uses the subpass-input form. Both SPIRV-Cross and the shader
TU build with `-fexceptions`, so a malformed guest shader fails its pipeline instead of aborting.

**State, clear and draw.** Full clear is a load action; partial clear is a scissored fill. Each latch
field hashes at its own width. Attachments are compacted. Cull-both drops rasterisation only for
triangles. A two-sample guest request folds to four host samples, but the guest lattice and
`get_spp()` stay at two.

**Presenter and flip.** The drawable is the client area; one blit scales the RSX image into it. The
recorder signals the drawable on the frame's own submit. The flip calls both `GSRender::flip` and
`rsx::thread::flip`. Vsync off is no sync, adaptive is paced, full is display sync. The draw path
carries the same Y flip as the presenter: viewport height negation, mirrored scissor, inverted
winding.

**Reuse-only clusters.** `fixed-function`, `misc`, `sync-flip`, `nv0039-dma`, `nv2d-surface`,
`nv3089-image` and `nv308a-image` need no new Metal code; the shared CPU handlers do the work.

**Landed batches.** Batch 1: PF1–PF7, PF10, PF11, DR1, DR2, DR7, SF1, SF2, SF6, SF11–SF13, V1, F1.
Batch 2: SY1–SY3, SY5, SY9, S3–S5, S7, L1, TX1, TX2, TX4, TX6, F4, F6, TX9. Batch 3: VX1–VX3,
DR3, DR6, L3, TX3, F13, PF9.

---

## 4. The measurements

These are device facts on the floor device. The probes are not in the tree.

### 4.1 Shader limits

| limit | value |
|---|---|
| MSL buffer index | 0–30 (31 fails) |
| MSL sampler index | 0–15 (16 fails) |
| MSL texture index | 0–127 (128 fails) |
| argument table buffer bind count | accepts 100000 |
| argument-buffer escape hatch | no ceiling found up to 8000 entries |
| MSL language version | does not move the three limits |

A combined image sampler that is only fetched still emits a sampler argument. A bind past the
declared sampler count aborts on the validation layer.

### 4.2 Depth and stencil

| measurement | result |
|---|---|
| depth resolve, producer barrier, 12 configurations | 576/576 |
| depth resolve, consumer barrier | 46/48 |
| depth resolve, no barrier | 4/48 and 3/48 |
| combined depth-and-stencil resolve | 32/32 |
| stencil resolve, all barrier forms | 48/48 |
| named-plane combined-format copy | bit-exact, private and shared |
| un-named combined-format copy | invalid API |
| depth-only view of `Depth32Float_Stencil8` | rejected; only the stencil view casts |
| guest z24s8 | stencil bits 7:0, depth 31:8 |
| guest z16 | depth bits 15:0 |
| read-back encode | bit-exact at every texel |

### 4.3 Shadow compare

Directed functions: 32/72 mismatches. Swapped functions: 0/72. Linear compare gives
1.0/0.75/0.5/0.25/0.0. The native comparison sampler is correct with the directed functions swapped.

### 4.4 Presentation and frame probe

Drawable count is 3. An explicit producer-to-consumer barrier between two encoders changed no pixel
on this device for this encoder structure; this is a measurement, not a licence to drop barriers.
The full 2-D frame probe (upload, alpha blend, depth mirror, RTT resample, read-back, present)
passed every check. It proves encoder shapes, not the RSX translation.

### 4.5 Residency

A resource not in an `MTL::ResidencySet` on the queue is not resident. A compute copy to it writes
nothing, the commit reports success, and the queue wedges with `MTL4CommandQueueTimeout`. Residency
and barriers are separate.

### 4.6 Packed texture formats

The shared uploader keeps a packed GCM format packed on Apple silicon. Metal names a packed format
from the most significant field, so the read permutes it. KK proves the direction
(`kk_format.c:231-234`).

| guest format | native Metal format | read |
|---|---|---|
| `R5G6B5` | `B5G6R5Unorm` | `{One, Blue, Green, Red}` |
| `R5G5B5A1` | `BGR5A1Unorm` | `{Alpha, Blue, Green, Red}` |
| `A1R5G5B5` | `A1BGR5Unorm` | `{Alpha, Blue, Green, Red}` |
| `D1R5G5B5` | `A1BGR5Unorm` | `{One, Blue, Green, Red}` |
| `A4R4G4B4` (stored `R4G4B4A4`) | `ABGR4Unorm` | `{Red, Alpha, Blue, Green}` |
| `R6G5B5` | expansion planned | REVIEW R3 |

The earlier “the probe measured the channel order” claim was wrong: a shader write/read of the same
channel is invariant under any permutation.

### 4.7 Copy rules

A buffer-to-texture copy accepts a source row shorter than the destination row: a 512-byte source
row into a 1024-byte destination row wrote 1020 of 1024 bytes and put two source rows in the first
destination row. `bytesPerRow` is required for a linear texture. A `uchar` view is illegal. A wrong
source pitch fails silently, so the pitch is measured, never assumed.

---

## 5. The backend against Vulkan and OpenGL

### 5.1 Size

| backend | .cpp files | .cpp lines | headers |
|---|---|---|---|
| Metal 4 | 31 | ~11k | ~4.3k |
| Vulkan | 29 | ~18.5k | ~4.8k |
| OpenGL | 18 | ~9.1k | ~3.5k |

Line count is a weak proxy. The feature list is the real comparison.

### 5.2 Capability differences

| capability | Vulkan | OpenGL | Metal | consequence |
|---|---|---|---|---|
| `supports_last_provoking_vertex` | device | true | **false** | flat shading falls back to smooth |
| `supports_hw_conditional_render` | true | not set | **false** | CPU skip; correct but slower |
| `supports_native_logic_op` | true | true | **false** | emulated in the ROP |
| `supports_host_gpu_labels` | setting | true | **false** | guest writes stay CPU-side |
| `supports_extended_depth_range` | device | device | **false** | shared float-depth emulation |
| `supports_passthrough_dma` | device | not set | **false** | shared CPU handlers |
| `supports_multidraw` | true | true | **false** | inert; no reader |
| `supports_asynchronous_compute` | true | not set | true | — |
| `supports_programmable_blending` | true | not set | true | — |

### 5.3 Gaps

| feature | Vulkan | OpenGL | Metal |
|---|---|---|---|
| shader interpreter | yes | yes | yes (`LLD-shader-interpreter.md`; runtime gate pending) |
| on-disk pipeline cache | yes | yes | yes (`"metal"/"v1"`, driver archive) |
| multi-threaded pipeline build | pool | one | preload pool; draw-time build synchronous |
| async scheduler | yes | no | no (deliberate) |
| occlusion query pool | yes | yes | none (ZCULL exists) |
| upscalers (FSR1, RCAS, bilinear, nearest) | yes | yes | **none** |

The pipeline archive (`MTL4PipelineDataSetSerializer`) is the `VkPipelineCache` analogue. The
shader interpreter is implemented (`LLD-shader-interpreter.md` §21); only its runtime gate (an
approved draw run and the standalone MSL probe) is outstanding. A draw the chain cannot express on
the interpreter falls back to a synchronous real build, not a drop.

### 5.4 Settings Metal ignores

| setting | Metal |
|---|---|
| `read_color_buffers`, `write_color_buffers`, `read_depth_buffer`, `write_depth_buffer` | shared half inherited; backend half absent |
| `strict_rendering_mode`, `disable_zcull_queries`, `relaxed_zcull_sync` | inherited |
| `disable_hardware_texel_remapping` | **missing** |
| `disable_vertex_cache` | no-op by construction |
| `force_high_precision_z_buffer` | effectively always on |
| `precise_zpass_count` | shared half inherited; backend half absent |
| `host_label_synchronization` | matches false capability |
| `disable_msl_fast_math` | **missing**; F4 now selects safe math |
| `log_programs`, `debug_output` | debug-only; absent |
| `output_scaling`, `rcas_sharpening_intensity`, `min_scalable_dimension`, `resolution_scale_percent` | **missing** |

### 5.5 Workarounds and debug surface

Class A (vendor driver workarounds) mostly does not apply, except the Apple/Kosmickrisp entries:
KK unrolls loops, so Vulkan emulates primitive restart; Metal must check its own index path. Class B
(shared guest quirks) is inherited. Class C (OpenGL-only hacks) encodes guest behaviour Metal must
reproduce; it lives in the shared vertex prologue. Vulkan's debug callback, RenderDoc and GPU labels
are absent; Xcode GPU capture and `MTL_HUD_ENABLED` are available.

---

## 6. The findings register

`REVIEW-FINDINGS.md` holds 97 findings: **80 fixed, 3 partial, 0 blocked, 1 open, 9 limitations, 2
duplicates, 2 won't fix**. The parity sweep added J1–J10 (VX1–VX3, DR3, DR6, TX3, TX9, PF9, F13,
L3). The open finding is I7, the content of the two present buffers; it needs a reproducer. The
partials are B16, E7 and E8.

The register sweep closed I4 and I6 and added I9 and G3. G1 (flat shading) is a limitation, not an
open defect. G2 (`supports_multidraw`) is inert.

**Current high-severity state.** The descriptor abort is open and reproduced. The black mini-game,
the black background video and the black FMV are open visual defects. SF9/TX12 (depth read-back),
TX11 (depth-as-colour view), TX14 (null texture), SY4 (null resource unbind) and PF8 (drawable
without texture) are deferred with a recorded reason. DR5 (winding) needs an on-device cull A/B.

---

## 7. The pending work

### 7.1 The descriptor abort (blocker)

The 22:05 run aborted in `rsx::thread` with SIGABRT. The stack is `validateWithDevice` →
`heapTextureSizeAndAlignWithDescriptor` → `new_texture` → `create_new_texture` →
`upload_image_from_cpu` → `upload_texture` → `load_texture_env` → `end`. This is the L1 class again.
The last log line is a normalized cube descriptor at `0:03:35.278933`, but the crash may be a later
descriptor that bypassed normalization.

Do:

1. Find every caller of `new_texture` and identify which ones bypass `normalize_texture_extent`.
2. Validate every field at the allocation choke point: type, format, width, height, depth, mips,
   sample count, array length, storage mode, usage, swizzle, compression.
3. Log the full descriptor before the Metal call, with the guest address and the upload context.
4. Clamp or reject with a safe fallback, and unit-test every illegal case.
5. Re-run one bounded RR7 session at `MTL_PROBE=2` after the fix. Do not run without approval.

### 7.2 The black Xevious mini-game

The panel is the Xevious arcade window on the startup/loading screen. Input and audio work, so the
guest logic runs and the failure is in the render or sample path. The panel may be a separate RTT
sampled by a quad, or a surface presented directly. Determine which, then check:

1. Is the draw refused by `should_skip_draw`? L3 now reports all four predicates.
2. Is the RTT cleared black after it is drawn?
3. Is it resolved and written back before the sample?
4. Is the sampled texture invalidated when the guest writes it?
5. Is the surface format/layout/permutation correct at 16 bits?
6. Is there a barrier and a residency entry between the producer and the sampler?

### 7.3 The black video and FMV

The earlier reading that this is shared HLE code with no Metal path is not proven. This run decodes
frames and presents black. Trace `cellVdec` into guest buffers, then the RSX sample path:

1. `cellVdecGetPictureExt` writes through `outBuff.get_ptr()`; the pitch alignment is a TODO.
2. Check the guest texture format, component order, swizzle, sRGB and row pitch at 1280×720.
3. Check `on_access_violation` and `invalidate_range` for a raw CPU write.
4. Check whether the path uses a typeless copy, an MSAA source, a scaled copy or a CPU upload.
5. Check TX9's staged copy and its blit-to-blit barrier.
6. Check the barrier and residency between the decode write and the fragment sample.

The race-selection background and the FMV can share a cause. Do not fix one without checking the
other.

### 7.4 The open TODO rows

Order: SF8, SF9, TX5, TX7, TX10, TX11, TX12, TX14, DR9, DR10, PF8, PF12, S5, S6, F2, F3, F5, F7–F12,
F14, SY4, SY6–SY8, SY10, SY11, Q7, VX4, SF14, TX20. The live list and severity are in
`METAL4-TODO.md`; this section does not duplicate them.

### 7.5 Coverage and dials

203 live dials, 51 wired. The four latch clusters are depth-rop, raster, blend and draw; the other
ten write straight into a resource and need a device observation. 65 live dials have no decoder;
`vertex` (4 of 25) and `texture` (3 of 13) are the sharpest. 71 dials are difficulty 3 or 4 and need
a design decision.

### 7.6 Code that has never executed

The samplerless declarations (A9), the C1 logic-op shader, the shadow-compare fallback, the
cyclic-reference texture barrier (I4), the adaptive-vsync approximation, the stereo second eye, the
CPU read-back wait and the Metal 3 blit encoder inside a Metal 4 command buffer. Each needs a title
or a probe; none is a confirmed defect today.

### 7.7 Gap list

1. Flat shading: limitation (G1). No native provoking vertex; index rotation or attribute replicate
   are the costed expressions. No title on this install uses it.
2. No shader interpreter (Gap 2).
3. No occlusion query pool (Gap 3).
4. No upscalers (Gap 4).
5. `disable_hardware_texel_remapping` ignored (Gap 5).
6. Backend half of surface read/write flags absent (Gap 6).
7. `precise_zpass_count` backend half absent (Gap 7).
8. `disable_vertex_cache` and `force_high_precision_z_buffer` are silent no-ops (Gap 8).
9. Primitive restart against the Apple driver (Gap 9) — DR3 widens 16-bit strips.
10. `disable_msl_fast_math` (Gap 10) — F4 selects safe math.
11. On-disk shader cache landed (Gap 11).
12. Pipeline build is asynchronous at draw time (Gap 12 closed); the warm preload submits and drains one bounded queue (F6).

### 7.8 Guest quirks

Pitch conversion is unimplemented (`surface_store.h`), so overlapping surfaces of differing pitch
are truncated. The depth-to-colour `force_convert` bitcast needs the resample path. ZCULL titles that
never drain zpass and the `fdiv` subpixel bias are shared behaviour. The three Class C OpenGL hacks
encode guest behaviour; reproduce the behaviour, not the hack.

### 7.9 Performance

The run-4 steady window (300 frames, `MTL_PROBE=1`, F21/F22 in): 60 fps, `frame/gpu` 2,016 µs with
840 µs GPU wait, `present/flip` 413 µs, `present/acquire` 231 µs, `present/display` 59 µs,
`frame/end` 58 µs, `texture/upload` 39 µs, display gap 16.89 ms, latency 15.75 ms, 1.38 drawables in
flight. The boot window is the one to watch: `present/flip` 4,876 µs average with 879 ms maximum,
`present/acquire` 2,021 µs average with 361.8 ms maximum, `program/pipeline` 5,108 µs over four
builds. The 3-D title has no level-1 window yet. Owed: encoder boundaries per frame, pipeline cache
hit rate, per-draw CPU time, hazards per frame and a Vulkan differential.

### 7.10 Hygiene and the commit

1. Excise every `TODO(excise)` site, `MTLInstrumentation.h`, the acquire-stall probe and the
   main-thread heartbeat. The markers span `MTLCommandRecorder.cpp`, `MTLDevice.cpp`,
   `MTLInstrumentation.h`, `MTLMetalLayer.h`, `MTLMetalLayer.mm` and `MTLPresenter.cpp`.
2. Run the tests and the linter after excision.
3. Commit as `hsaka -1.0 <340957754+hsakaminusone@users.noreply.github.com>`, author and committer.
4. Push `origin/metal4`. Do not touch Vulkan or OpenGL.
5. Split the commits where a test exists: present/flip, F24, barrier/copy ordering, frontend exit,
   shader preload, batch 1–3, excision.

---

## 8. Accepted limitations

1. Point polygon mode collapses to fill.
2. Per-primitive polygon-offset enables collapse to the fill enable.
3. Guest dither is dropped.
4. Hardware conditional render is not implemented; CPU skip is used.
5. Level-of-detail bias needs a fragment injection.
6. Draw-state latch is a decode proof, not hot-path state.
7. Pipeline key carries inert fields.
8. Border colour is one of four fixed colours.
9. The drawable stretches a source whose aspect ratio differs from the window.
10. Stereo second eye is a deviation.
11. The stencil plane of a combined image cannot be uploaded.
12. Pitch conversion is unimplemented.
13. Flat shading falls back to smooth (G1).
14. Typeless/scaled copies are refused rather than emulated (I5); TX9 now stages the equal-texel
    typeless case.
15. `supports_multidraw` is inert (G2).
16. `R6G5B5` needs a format expansion (TX20).
17. A 16-bit surface uses an identity layout; raw write-back swaps R/B. An output permutation or a
    32-bit staging path is planned (R2).

---

## 9. Roadblocks

**Blocking.** The descriptor abort. The black mini-game and black video are visual blockers for
RR7; they do not stop the process.

**Environment.** No new dependencies; 8 GB memory; four-job builds; one emulator instance; about
39 GB free disk.

**Tooling.** `ripgrep` and `timeout` are absent. zsh; BSD `sed`. `MoltenVKShaderConverter` takes
SPIR-V only. `glslangValidator`/`glslc` are absent; SPIRV-Tools is present. The unit suite stays
GPU-free because a `verify()` dialog can kill it.

**Platform.** Vendored Metal headers carry no availability annotations; the runtime gate is the only
guard. A depth-only view of `Depth32Float_Stencil8` is rejected. `[[position]]` cannot be both a
stage-in and a fragment parameter. A bind past the declared sampler count aborts on validation.

**Measurement.** A probe that ignores residency reports a false defect. A result that changes with
validation on/off is a tool artefact. Only an on-screen present measures.

---

## 10. The verification ladder

1. GPU-free unit tests. These run anywhere.
2. Shader-translate check. Compiles the emitted shader without a GPU.
3. Guest-observable read-back. The only objective oracle.
4. Differential comparison against the shipping backend.
5. Golden images.

RPCS3 has no golden-image suite and the hosted runners have no dependable GPU, so the GPU half is
manual. A differential run compares a tile-based deferred renderer with a non-tile-based one; use a
classified tolerance model, not an exact match. Rung 2 is the cheapest unused rung: decompile a
logic-op and a redirected-depth program and compile the result.

---

## 11. How to continue

1. Read `METAL4-TODO.md` and this file.
2. Read `SUBAGENT-RR7-2026-10-20.md` when it lands; then fix the abort at the allocation choke
   point.
3. Ask for one bounded RR7 run at `MTL_PROBE=2` and a Vulkan/KosmicKrisp reference run. Copy the
   log before each run.
4. Fix the black paths from evidence: panel surface, video upload, invalidation, barrier, residency.
5. Work the open TODO rows by severity; do not re-open fixed rows.
6. Keep the build/test loop, the probe rule and the one-instance rule.
7. Excise, test, commit as `hsaka -1.0`, push `origin/metal4`.

---

## 12. Datapoint index

| datapoint | value |
|---|---|
| branch | `metal4` |
| commits ahead of `origin/metal4` | 55 |
| Metal subsystem files | 67 |
| Metal subsystem lines | ~16k |
| Metal test cases | 150 |
| full test cases | 498 |
| Metal test suites | 38 |
| TODO rows | 60 fixed, 40 pending |
| review findings | 97 (80 fixed, 3 partial, 1 open, 9 limitations, 2 duplicates, 2 won't fix) |
| inventory rows | 282 |
| live rows | 203 |
| work dials | 178 (133 reshape, 45 emulate) |
| live dials without a decoder | 65 |
| dials at difficulty 3 or 4 | 71 |
| design documents | 26 |
| commits | uncommitted since `17c3c332e` |
| last build | green, with the asynchronous shader path |
| last RR7 run | 2026-10-21 00:01–00:06, probe 2; label race at race start |
| crash thread | `rsx::thread` (probe-2 fatal) |
| crash site | `RSXThread::recover_fifo` after dead FIFO |
| MSL buffer cap | 30 |
| MSL sampler cap | 15 |
| MSL texture cap | 127 |
| argument-buffer highest test | 8000 entries |
| depth resolve, producer barrier | 576/576 |
| depth resolve, consumer barrier | 46/48 |
| combined depth/stencil resolve | 32/32 |
| stencil resolve, all forms | 48/48 |
| shadow compare, swapped | 0/72 |
| drawable count | 3 |
| Metal 4 API floor | macOS 26 |
| runtime floor | macOS 27 |
| deployment target | 15.0 |
| code written (estimate) | ~75% |
| code verified (estimate) | ~70% |

<!-- doc-end -->
