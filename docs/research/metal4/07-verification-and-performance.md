# 7. Verification, conformance, determinism, debug tooling & the performance model
Owner/scope: Scope 7 (cross-cutting) | Status: draft | Date: 2026-10-15

> **Scope:** the cross-cutting question "**how would we know the backend is correct, and know it is
> fast enough**" for a hypothetical native Apple Metal 4 RSX backend for RPCS3. This document is
> deliberately orthogonal to the sibling scopes: it does **not** map `Vk*` constructs to `MTL4*`
> (README §3, docs 01–05) and it does **not** re-litigate the impossibility register (README §6).
> It maps (a) the **correctness oracle** and an automatable conformance ladder, (b) **determinism**
> and a divergence taxonomy, (c) the **debug-tooling inventory** and where Metal is *worse* than the
> Vulkan path RPCS3 ships today, (d) the **performance model** and a benchmarking plan, and (e) a
> **CI strategy** for a macOS code-hosting platform that cannot run Metal on its hosted runners.
>
> **Read-only research.** No RPCS3 source, CMake, test or `.ci` file was modified and no build was
> run to produce this note. All `file:line` citations were read on 2026-10-15; the tree is
> point-in-time and another agent is editing `rpcs3/Emu/RSX/VK/**` concurrently, so **symbol names
> are the stable reference; line numbers are advisory**.
>
> **Legend:** `[V]` = verified against a primary source (RPCS3 tree or cited vendor doc);
> `[I]` = inference / engineering judgement; `[U]` = could not verify. Effort ratings reuse the
> scale fixed by the sibling docs and README §3: **1** trivial rename · **2** mechanical · **3** real
> work, well-trodden · **4** hard / needs design · **5** unknown or impossible.

---

## 1. Scope & what the sibling docs already settled

This doc answers six concrete questions that the five per-scope docs deliberately left open:

1. **Correctness oracle & conformance** — what is "right", and what can be tested automatically
   (§2–§3).
2. **Determinism** — how much divergence is acceptable, and how to classify it (§4).
3. **Debug tooling** — what exists on Metal, and what is *worse* than Vulkan (§5–§6).
4. **Performance model & benchmarking** — where time goes and how to measure (§7–§9).
5. **CI strategy** — what can actually run on GitHub macOS runners (§9).
6. **Effort/risk** — the same 1–5 scale as the sibling docs (§10).

**Facts already established by the sibling docs and the README** (do not re-derive here; these are
inputs, not findings):

- A native backend deletes the MoltenVK/KosmicKrisp layer and talks to
  `MTL4CommandQueue`/`MTL4CommandBuffer`/`MTL4CommandEncoder` directly; the ~55k LOC of shared RSX
  emulation is unchanged behind `rsx::thread` and `backend_configuration` (README §2).
- RPCS3's shipping Vulkan path is **effectively single-queue, one-submit-fence-per-command-buffer**,
  with one `vk::fence m_submit_fence` per CB (`02-submission-sync.md` §1.1, `vkutils/commands.cpp`).
- **RPCS3 has no GPU timestamp queries today** — no `VK_QUERY_TYPE_TIMESTAMP`, no
  `vkCmdWriteTimestamp` anywhere in the tree `[V]` (grep of `rpcs3/Emu/RSX` returns zero). GPU-side
  timing is therefore *new* instrumentation a Metal backend must add via `MTL4CounterHeap` /
  `writeTimestamp` (README §9 links these; MSL/counter detail is scope 02's).
- RSX has no geometry/tessellation/transform-feedback/pipeline-statistics stage; PS3 games cannot
  reach them (README §6, R32). `frame_statistics_t` is therefore the only built-in "pipeline
  statistics" RPCS3 has, and it is **CPU-side** (§7).
- The RSX per-title quirk surface (RTT layouts, MSAA resolve, spilling, formats) is where emulator
  correctness lives (README §6, R11), and MSAA is an "exploded" model that Metal's pass-scoped
  resolve cannot mirror drop-in (README §6, R16).
- The shader path is `RSX ucode → GLSL → glslang → SPIR-V`, then (for Metal)
  **SPIRV-Cross `CompilerMSL`** — i.e. the SPIR-V *is* available at build time and can be emitted
  with debug info (README §2; R2).
- The MSL/Metal compiler + driver bug surface is inherited and re-hit by a native backend; KK ships
  **18** `KK_WORKAROUND_*` entries (README §6, R10).
- `rpcs3_test` (CMake target, `rpcs3/CMakeLists.txt:180-225`, `gtest_discover_tests`) is today a
  **GPU-free** CPU suite (`rpcs3/tests/`: `test_address_range`, `test_rsx_mm_queue`,
  `test_rsx_fp_asm`, `test_vk_driver_selection`, …) `[V]`. `test_vk_driver_selection.cpp` is the
  working template for "pure logic that runs anywhere" tests.
- The RSX capture/replay machinery is the closest thing RPCS3 has to a reproducible workload
  (§3); the frame-debug trace and screenshot paths are the closest thing to an oracle (§2).

**Out of scope here:** the actual `Vulkan → Metal 4` construct mapping, phasing, and the
impossibility register — see README §3, §6, §7 and docs 01–05.

---

## 2. The correctness oracle: what "right" means for an RSX frame

### 2.1 The fundamental problem: there is no local reference

RPCS3 emulates a fixed-function/partially-programmable RSX pipeline, but the *reference
implementation* is a physical PS3 that cannot be instrumented at the frame level. In practice RPCS3
"defines" correctness by **agreement between its own backends** (Vulkan/KK, Vulkan/MoltenVK,
OpenGL, and the software-renderer fallback where applicable) plus **human visual review** plus the
**compatibility database** (per-title playability/status). None of these is a pixel oracle, and none
is available for Metal on day one unless a Metal backend exists to compare against. `[I]`

This has a hard consequence for the new backend: **a native Metal backend cannot be validated by
differential testing against itself.** The first meaningful oracle is "does Metal agree with the
Vulkan/KK path that is already shipped for the same title on the same machine" — a *differential*
oracle that is useful but not authoritative.

### 2.2 The oracle ladder (each rung is a weaker but more automatable statement)

Proposed, layered from cheapest/most-automatable to most-expensive/human:

| Rung | Statement | Oracle | Automatable? |
|---|---|---|---|
| 0 | **API-legal**: the backend never calls Metal incorrectly | `MTL_DEBUG_LAYER=1` (§5) | yes (on a GPU host) |
| 1 | **Shader-legal**: no OOB/`nil` access/stack issues | `MTL_SHADER_VALIDATION=1` (§5) | yes (expensive, §7) |
| 2 | **Structural**: same draws/passes/bindings/flip sequencing as the reference backend | `frame_trace_data`/`frame_debug` (§2.3), capture replay | partly |
| 3 | **Guest-observable**: the GPU results RSX can *read back* (render-target memory, occlusion counts, ZCULL stats) match | PS3-visible semantics; RSX readback paths | hard, high value |
| 4 | **Host-frame equivalence**: the presented image matches the reference backend within tolerance | screenshots + golden images (§3) | yes, with tolerance |
| 5 | **Title behaviour**: the game runs, the UI/frame pacing behaves, no regressions | compatibility DB + human | no |

The critical insight is that **rung 3 is the only rung that is "objectively about RSX"**; rungs 4–5
are about *host* rendering and inherit TBDR/MSAA/rounding differences (README §6, R16; §4 below).
Rung 2 is where a Metal backend can be made *structurally* comparable to the Vulkan backend without
demanding pixel equality, and it is cheap because RPCS3 already records it.

### 2.3 RPCS3's built-in structural oracles (verified)

**Frame-debug trace.** `rsx::frame_trace_data` (`rpcs3/Emu/RSX/Capture/rsx_trace.h:11-32`) records
a `command_queue` of `(reg, value)` pairs and a `draw_calls` vector of `draw_state{name, programs,
rsx_state state, 4×color_buffer, 2×depth_stencil, index, vertex_count}`. It is populated while
`capture_current_frame` is set (`RSXThread.cpp:921`, `RSXFIFO.cpp:819`) and reset at frame start
(`RSXThread.cpp:3468`). The global instance is `rsx::frame_trace_data frame_debug` (`RSXThread.cpp:46`,
declared `RSXThread.h:36`) `[V]`. This is an **RSX-level, not pixel-level** trace: it can assert
"the backend issued the same draw call with the same VP/FP program keys and the same RSX state" —
exactly the kind of statement that survives backend-specific rasterization differences.

**Frame capture / replay.** `rsx::frame_capture_data` (magic `"RRC"`, `rsx_replay.h:13`) records the
decoded NV4097 command stream plus memory blocks, display-buffer state and tile state; it is saved as
`.rrc.gz` under `config/captures/` (`RSXThread.cpp:3484`) and booted with `--rsx-capture <path>`
(`rpcs3.cpp:414,843,1228-1240`), including `--headless`/`--no-gui` (`rpcs3.cpp:391,448`) `[V]`.
`rsx_replay_thread` (`rsx_replay.h:161`) fakes a `sys_rsx_context`, applies the captured state and
requests an emu flip (`rsx_replay.cpp:174+`). This is the single most valuable tool for Metal
bring-up: **it turns "run a whole game" into "replay one frame's worth of RSX commands, deterministically, headless."**

**Screenshots.** `g_user_asked_for_screenshot` (`RSXThread.cpp:43`) drives a readback in
`VKPresent.cpp:662-779`/`GLPresent.cpp:331-387` into the virtual
`take_screenshot(std::vector<u8>&&, w, h, is_bgra)` (`GSFrameBase.h:35`) `[V]`. A native backend
must implement the same seam, which also gives the conformance harness a pixel output to hash.

**CPU-side frame statistics.** `frame_statistics_t` (`RSXDisplay.h:50-68`) carries `draw_calls`,
`submit_count`, `setup_time`, `vertex_upload_time`, `textures_upload_time`, `draw_exec_time`,
`flip_time`, `vertex_cache_request_count/miss_count`, `program_cache_lookups_total/ellided`, and a
`framebuffer_statistics_t`. `thread::m_frame_stats` (`RSXThread.h:162`) is reset each frame
(`RSXThread.cpp:3568`) and queued with the flip (`RSXThread.cpp:3524`) `[V]`. These fields are the
front-end of the performance model (§8) and can double as a **structural regression oracle** (e.g.
"draw call count and framebuffer layout per frame are unchanged").

### 2.4 What follows for the Metal backend

- Build the conformance harness on **rung 2 + rung 4**, because both are cheap and reproducible;
  treat rung 0/1 validation as a separate "developer gate" (§3.2) and rung 3 as a targeted
  correctness spike for the specific paths RSX reads back (render-target memory, ZCULL).
- Do **not** promise bit-exact frame equality against Vulkan; the honest claim is "equal within a
  documented tolerance, with an explicit, classified list of known divergences" (§4).
- The `frame_debug` name/program pair is the natural key to attach **Metal encoder labels**
  (§7.2), which is what makes a `.gputrace` of a failing replay legible.
- `[U]` Whether the RSX capture format records enough state for a *pixel-complete* replay of every
  title (spilled surfaces, MSAA resolve history) — this is scope 04/R11 territory and should be
  re-checked before the harness depends on it.

---

## 3. Automatable conformance: the ladder in practice

### 3.1 Level 0 — GPU-free unit tests (works in any CI, today)

The existing `rpcs3_test` target (`CMakeLists.txt:180-225`) already proves the pattern: pure logic
tests with no `MTLDevice`/`VkDevice` in sight (`test_vk_driver_selection.cpp` exercises KK driver-id
parsing, label uniqueness, chip-class boundaries and workaround-profile selection as plain function
tests) `[V]`. For the Metal backend the equivalent GPU-free surfaces are:

- **Workaround/feature profiles** — mirror the KK `KK_WORKAROUND_*` selection (README §6, R10) as a
  pure table tested the way `test_vk_driver_selection.cpp` tests the KK profile `[V]`.
- **Format-table logic** — the `VkFormat`→`MTLPixelFormat` mapping, capability fallbacks, and
  "is this format natively samplable/renderable" decisions (scope 04) are pure functions.
- **Binding/argument-table index math** — deriving `MTL4ArgumentTable` sizes and slot indices from
  reflected bindings (scope 03) is pure and easy to get wrong.
- **Cache-key logic** — shader-cache namespace (`"metal"`, README §6 R37), pipeline-cache keys,
  texture-cache keys, sampler-pool keys.

This is the **only tier that runs on GitHub-hosted macOS runners** (§9). It should be grown
aggressively precisely because the higher tiers cannot run there.

### 3.2 Level 1 — validation gates (developer machine / self-hosted only)

Run the backend under `MTL_DEBUG_LAYER=1` and, for shader-touching changes, `MTL_SHADER_VALIDATION=1`
(§5). Both are **process-global and read before `MTLDevice` creation** `[V]` (Apple
`MetalValidation(1)` man page), so they are environment settings, not code. Practical recipe
(mirrors the real-world `NoGraphicsAPI` project, which runs its CTest suite under
`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --output-on-failure`) `[V]`:

```
export MTL_DEBUG_LAYER=1
export MTL_DEBUG_LAYER_ERROR_MODE=assert          # assert | ignore | nslog
export MTL_DEBUG_LAYER_WARNING_MODE=nslog
export MTL_SHADER_VALIDATION=1
export MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING=1
export MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1   # otherwise errors only reach the system log
```

Caveat: shader validation instruments every pipeline by default
(`MTL_SHADER_VALIDATION_DEFAULT_STATE=all`) and has a **significant** GPU-time and compile-time
cost, so it belongs in a separate "validation" job, not the perf job; per-pipeline scoping
(`MTL4PipelineOptions.shaderValidation`, `MTL_SHADER_VALIDATION_ENABLE/DISABLE_PIPELINES`) exists
for ship-grade builds `[V]`.

### 3.3 Level 2 — RSX capture replay as the reproducible workload

The capture/replay path is the key enabler: a `.rrc.gz` plus `--rsx-capture` gives a bounded,
headless, repeatable workload. Proposed harness:

1. **Corpus.** Curate a small set of captures per class: (a) trivial 2D/overlay, (b) a
   VP/FP-heavy scene, (c) RTT + MSAA-heavy, (d) a format/deswizzle stress scene, (e) a ZCULL-heavy
   scene. Each capture carries metadata: title id, capture version (`0x6`), GPU family, OS build.
2. **Driver.** `rpcs3 --headless --rsx-capture corpus/N.rrc.gz` → render N frames → request a
   screenshot via `g_user_asked_for_screenshot` → write RGBA + a `frame_statistics_t` dump.
3. **Compare.** Against a reference image produced by the Vulkan/KK backend on the *same* machine
   (differential), using a **tolerance** metric (§4), not equality.
4. **Structure check.** Additionally diff the `frame_debug` draw/program/state sequence against the
   reference; this catches "looks right but issued the wrong pipeline" failures that a tolerance
   metric can mask.

Because RPCS3 already ships every piece of this (capture load/store, headless boot, screenshot
seam, frame debug), the *new* work is glue and a comparison tool, not infrastructure `[I]`.

### 3.4 Level 3 — golden-image tests with an explicit tolerance model

RPCS3 has **no** golden-image GPU test suite today `[V]` (the test list in §1 contains no rendering
test). Adding one is a design decision, not a small patch:

- **Store images, not hashes, for the reference**, but gate on a tolerance metric (e.g. per-pixel
  max/mean absolute error, or a perceptual metric) to survive driver-version and GPU-family drift.
  The GitHub mobile team had to **re-record snapshot images when moving Intel→M1** because UI
  rendering differed `[V]` — the same effect will bite a Metal backend across M1→M4 and across
  macOS releases.
- **Bucket tolerance by region**: RSX-observable regions (readback targets) get tight tolerance;
  purely host-presented regions get loose tolerance.
- **Record the environment** (device family, OS, Xcode/macOS build) in the manifest; treat a
  mismatch as "re-baseline required", not a failure.

### 3.5 Level 4 — capture the failing case, debug offline

When a replay fails, the harness should emit a **`.gputrace`** (programmatic capture, §5.5) and a
frame-debug dump as CI artifacts. `gpudebug` can then be driven **non-interactively** from a script
or an agent against the trace (`gpudebug -s <session> -c "go ..." -c "info pipeline"`), and `man
gpudebug` is explicitly recommended by Apple as sufficient context for autonomous investigation
`[V]`. This is how a failing replay becomes debuggable *without* the original GPU in the loop.

---

## 4. Determinism & the divergence taxonomy

### 4.1 Three axes of (non-)determinism

| Axis | Same backend, same machine | Same backend, different Mac | Different backend, same machine |
|---|---|---|---|
| Examples | run-to-run FP/thread drift, cache state, shader-compile races | GPU family / OS / thermal state | Vulkan/KK vs native Metal |
| Can it be bit-exact? | sometimes (with care) | no | no |
| Acceptable for tests? | yes if pinned | no (re-baseline) | only with tolerance |

### 4.2 Sources of divergence a Metal backend will hit

RPCS3-side (present in *any* backend, must not be blamed on Metal):

- Multithreaded RSX (`g_cfg.video.multithreaded_rsx`) and the async scheduler reorder work
  (`02-submission-sync.md` §1.2; README §6, R13).
- FIFO reordering / flattener (`m_flattener.evaluate_performance(m_frame_stats.draw_calls)`,
  `RSXThread.cpp:3534`) `[V]` and frame-skip logic.
- Texture-cache spilling and surface reuse (scope 04/R11).
- Shader compilation order / program-cache hits (`program_cache_lookups_*`,
  `RSXDisplay.h:64-65`) `[V]`.

Metal-side (new, must be *classified*, not fixed blindly):

- **TBDR rasterization order** and tile-memory residency (README §6, R11) — changes blending/ROV
  edge cases and any order-dependent read.
- **MSAA model mismatch** — RSX's exploded MSAA surface + separate resolve target vs Metal's
  pass-scoped resolve (README §6, R16). Resolve numeric content and sample pattern are not
  guaranteed identical.
- **ROP output rounding** — RSX quantizes colour output (`round_to_8bit` in
  `RSXFragmentPrologue.glsl:30,36,44`; applied per-MRT in `RSXROPEpilogue.glsl:19-22`) and
  quantizes fragment depth by 24/16-bit (`RSXROPEpilogue.glsl:84`). Any mismatch here is a *known,
  bounded* divergence, not a bug.
- **LOD bias quantization** — Metal `lodBias` is S4.6; KK injects an `lod_bias_fp16` shader-side to
  avoid the precision loss (README §6, R19).
- **Depth/stencil format forcing** — RPCS3 already forces `D24→D32` on Apple silicon (README §6,
  R16/§4 shift 6).
- **Sampler border colour** — only three border colours exist (README §6, R18).
- **MSL/Metal compiler precision & codegen bugs** — the KK W14/W16/W17 precision/LOD and
  W6/W15/W18 coherency workarounds (README §6, R10) are *documented* divergence sources.
- **`-ffast-math` / `fastMathEnabled`** — RPCS3 has `disable_msl_fast_math` (README §3 row 4);
  fast-math is a classic silent-divergence switch.

### 4.3 Divergence taxonomy (for triage and for the harness)

Classify every observed difference into exactly one bucket; only bucket **C** fails a build:

- **A — Exact.** Identical bytes. Expected for shader-cache/pipeline-cache keys, argument-table
  layout, capture round-trips.
- **B — Toleranced.** Bounded numeric difference with a known cause from §4.2 (rounding, MSAA
  resolve, LOD, precision). Passes with an image tolerance; the cause is *recorded*, not hidden.
- **C — Incorrect.** Structural difference (wrong draw/pipeline/binding), or a numeric difference
  outside the tolerance, or a crash/validation error. Fails.
- **D — Pending.** Difference whose cause is unknown → promote to an investigation, never silently
  widen the tolerance.

### 4.4 Determinism policy (recommendation)

1. **Never assert bit-exact frame equality across backends.** Assert per-backend reproducibility
   (same backend twice on the same machine → identical, modulo a documented allow-list) and
   cross-backend tolerance.
2. **Make the workload deterministic** for tests: single-threaded RSX, no frame-skip, fixed
   resolution, fixed vsync, headless, warmed caches.
3. **Treat "run twice, same result" as a first-class test.** A backend that is *not* reproducible
   against itself is untestable; this is the cheapest determinism guard and should exist before any
   pixel comparison.
4. **Pin the environment** (device family + OS build + GPU) in every baseline; treat environment
   change as re-baseline, not regression.

---

## 5. Debug tooling inventory (Metal side)

All items below are Apple-supported and verified against Apple documentation unless marked. This is
the raw inventory; §6 is the *gap analysis*.

### 5.1 Metal debugger (Xcode) — the GPU trace UI

- Capture the workload for **multiple frames** and "jump back and forth in time"; inspect passes,
  dependencies, memory, pipeline states, samplers, textures, bound resources, geometry, attachments.
  `[V]`
- **Insights** (auto recommendations: Memory, Bandwidth, …), **Performance timeline** (overlap vs
  serialization), **Memory viewer**. `[V]`
- **Shader Debugger** — step through vertex/fragment/compute shader execution and inspect variable
  values *on real GPU data* (since Xcode 10 / WWDC18); highlights NaN/Inf. 1 `[V]`
- **Shader profiler / cost graph / performance heat maps** — per-line shader cost, SIMD-group
  performance. `[V]`
- Requires GPU Frame Capture enabled in the scheme (auto-enabled when the target links Metal).

### 5.2 Command-line tools (new in the Xcode 26 era) — the automation story

- **`gpucapture`** — attach to a running process and capture a Metal workload on demand. `[V]`
- **`gpudebug`** — a text-mode, **self-discoverable** interactive debugger over a `.gputrace`. The
  root has `commands`, `performance`, `api_calls`, `resources`; `go`/`list` navigate, `info
  pipeline`/`fetch color0` inspect, `--oneshot` and `-s <session> -c "..."` script it. Apple
  explicitly documents it as **well-suited for AI agents / scripted workflows** and recommends
  handing an agent `man gpudebug` + a `.gputrace`. `[V]`
- **`metalperftrace`** — collect performance traces and emit human-readable summaries. `[V]`
- `gpudebug` is the single biggest *win* over the Vulkan path (no equivalent headless trace-debug
  script exists today).

### 5.3 Runtime validation

- **API validation** (`MTL_DEBUG_LAYER`): CPU-side misuse — creating resources wrong, encoding
  commands wrong, "draw without a pipeline set". Modes: `MTL_DEBUG_LAYER_ERROR_MODE`
  (`assert|ignore|nslog`), `MTL_DEBUG_LAYER_WARNING_MODE`, `MTL_DEBUG_LAYER_VALIDATE_LOAD_ACTIONS`/
  `STORE_ACTIONS`, `..._VALIDATE_UNRETAINED_RESOURCES`. Cheap enough to enable broadly. `[V]`
- **Shader validation** (`MTL_SHADER_VALIDATION`): GPU-side — OOB access, `nil` textures, missing
  `useResource`, stack overflow. Knobs include `MTL_SHADER_VALIDATION_DEFAULT_STATE`
  (`all`/`none`), `..._ENABLE/DISABLE_PIPELINES`, `..._FAIL_MODE` (`zerofill|allow`),
  `..._COMPILER_INLINING` (`default|full`), `..._ABORT_ON_FAULT`, `..._REPORT_TO_STDERR`. Per-
  pipeline via `MTL4PipelineOptions.shaderValidation`. **Significant** runtime/compile cost. `[V]`
- Reference: `man MetalValidation` (also published as `METALVALIDATION(1)`); **all variables must be
  set before the first `MTLDevice` is created**. `[V]`

### 5.4 Metal Performance HUD + Instruments

- **Metal Performance HUD** (`MTL_HUD_ENABLED=1`, logging via `MTL_HUD_LOG_ENABLED=1`): live FPS,
  GPU time, frame interval, memory, thermal state, present mode; ~once/second it writes
  machine-readable `metal-HUD: <frame>,<mem>,<present-interval>,<gpu-time>,…` lines to the system
  log. Rich metric/env surface (`MTL_HUD_ENCODER_TIMING_ENABLED`,
  `..._GPU_TIMELINE_FRAME_COUNT`, `MTL_HUD_INSIGHTS_ENABLED`, `MTL_HUD_REPORT_URL`, …). `[V]`
- **Instruments**: `Metal System Trace` + the `Game Performance` template (Points of Interest,
  Time Profiler, Thermal State, Metal Application/GPU/Display/Resource Events…), with **performance
  limiter + utilization counters** and **occupancy**. `[V]`
- **`os_signpost`/`OSSignposter`** — CPU-phase intervals visible in Instruments; pair with Metal
  System Trace for a unified CPU+GPU timeline. `[V]`
- Practical note from practitioners: prefer Instruments for repeatable timing because frame-capture
  timings vary with thermal/other-load conditions; **never profile on the Simulator**. `[V]`

### 5.5 GPU capture plumbing (needed to get a `.gputrace` from RPCS3)

- Programmatic: `MTLCaptureManager.sharedCaptureManager`, `MTLCaptureDescriptor` (destination
  `MTLCaptureDestinationGPUTraceDocument` → `.gputrace`), `MTLCaptureScope` (`begin`/`end`,
  `label`, `device`, `commandQueue`, and **`mtl4CommandQueue`** for Metal 4). Scope records only
  command buffers created after start and committed before stop; can span multiple frames. `[V]`
- Enabling capture: from Xcode, or `MTL_CAPTURE_ENABLED=1`, or an `Info.plist` `MetalCaptureEnabled`
  key. **Command-line binaries need the Info.plist key** (a minimal `Info.plist` next to the binary
  works per a widely-cited report); a raw CLI app otherwise reports
  `supportsDestination:GPUTraceDocument == NO`. `[V]`
- Known ordering gotcha: `gpudebug`'s replayer loads in the background; `info pipeline`/`fetch`
  block until ready. `[V]`
- Prior art to mirror: MoltenVK exposes `MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE` /
  `..._OUTPUT_FILE` / `..._PERFORMANCE_TRACKING`; KK exposes `MESA_KK_GPU_CAPTURE` /
  `MESA_KK_GPU_CAPTURE_DIRECTORY` and `MESA_KK_DEBUG`. A native backend should offer an equivalent
  env-var capture trigger (README §9, 09). `[V]`

### 5.6 Labels & groups

- `MTLCommandEncoder`/`MTLCommandBuffer` exposes `label`, `pushDebugGroup`/`popDebugGroup`,
  `insertDebugSignpost` (metal-cpp `MTLCommandEncoder.hpp`); Metal 4 encoders keep this surface.
  `[V]`
- These names are what make a `.gputrace`/`gpudebug` session readable; the natural source is
  `frame_trace_data::draw_state::name` / `programs` (§2.3) and the RSX event name.

### 5.7 Counter / timestamp API

- Classic: `MTLCounterSampleBuffer` + `MTLRenderPassSampleBufferAttachmentDescriptor` +
  `sampleCounters(sampleBuffer:sampleIndex:barrier:)`, counter sets (`MTLCommonCounterSetTimestamp`),
  `resolveCounterRange`, `sampleTimestamps`. `[V]`
- Metal 4: `MTL4CounterHeap` + `MTL4CommandBuffer.writeTimestamp(counterHeap:index:)` and
  `MTL4RenderCommandEncoder/MTL4ComputeCommandEncoder.writeTimestamp(granularity:after:…)`; a
  timestamp is captured after prior work in the command buffer completes. `[V]`
- **This is net-new for RPCS3** (no timestamp queries today, §1) — see §7.3.

---

## 6. What is worse on Metal (the debug-ergonomics gap)

This section is the honest counterweight to §5: the Metal toolchain is excellent *inside Xcode on a
local Mac* and materially weaker at everything RPCS3's Vulkan workflow takes for granted.

### 6.1 No validation-layer callback / no `VK_EXT_debug_utils`

- Vulkan gives RPCS3 a programmable validation layer (`VK_LAYER_KHRONOS_validation`) with
  stateless/object-lifetime/GPU-assisted/thread-safety/synchronization/best-practices coverage,
  **Debug Printf** (`VK_KHR_shader_non_semantic_info`), and a **messenger callback**
  (`VK_EXT_debug_utils`) that can filter by severity/type/message-id, route to stdout, and be
  configured per-instance (`VK_EXT_layer_settings`, `vk_layer_settings.txt`, `VkConfig`). `[V]`
- Metal has **no analogous programmable layer**. Validation is a set of **process-global environment
  variables** read before `MTLDevice` creation, with no callback, no severity filtering API, no
  per-object configuration, and no in-process routing beyond "assert / ignore / nslog / stderr". To
  change what is validated you must restart with a different environment. `[V]`
- There is **no Debug Printf equivalent** with format strings + filtering/verbose controls; Metal's
  "shader logging" (`MTLFunctionLog`/`MTLLogContainer`, "Logging shader debug messages") is a
  coarser facility `[V]` `[U for exact feature parity]`.
- Consequence: RPCS3's existing Vulkan-based CI/debug ergonomics (custom callback, message-id
  filters, printf) do **not** port; the Metal backend loses a whole diagnostic channel.

### 6.2 Validation is global and expensive; shader validation especially

- Shader validation instruments pipelines (all by default), increases runtime compile time, and has
  a "significant" GPU-time cost; the practical guidance is to enable it selectively/per-pipeline. `[V]`
- API validation is cheaper but still process-global and must precede device creation. `[V]`
- RPCS3's Vulkan path can turn on/off specific checks per run with a settings file; Metal cannot. `[V]`

### 6.3 `.gputrace` replay is not fully robust (real, documented failures)

- **Residency-set mutation cap during replay**: opening a `.gputrace` produced on macOS 26.x could
  fail with `command queue residency set limit of 32 exceeded` / `Metal object creation failed` /
  replayer validation errors; the working hypothesis is a hard (hardware-dependent) limit on
  residency-set mutations during gputrace replay (56 adds triggered 24 errors). This is directly
  relevant because Metal 4 makes **residency sets** the binding-visibility mechanism (README §6,
  R2/R8; §8). `[V]`
- **MetalTools limitations** (from a real Metal-4-adjacent project): concurrent sampler
  creation/destruction can crash API validation in `MTLSamplerDescriptorHashMap`; shader validation
  cannot reliably enumerate **placed resources through heap residency** (the workaround is to
  register individual resources and serialise residency enumeration); shader instrumentation of
  native indirect mesh commands crashes or leaves readbacks unwritten (direct commands are fine). `[V]`
- **Capture on command-line apps** requires the Info.plist/`MTL_CAPTURE_ENABLED` dance (§5.5), and
  **Rosetta breaks it**: KK W8 documents a Rosetta heap-size issue that breaks GPU capture. `[V]`
- `[U]` Whether the Metal 4 runtime fixes the depth/stencil resolve sync bug that forces KK to use
  meta passes (README §6, R16) — until resolved, D/S resolve artifacts will be harder to triage.

### 6.4 Tooling is macOS-only and cannot be run headless on CI

- The whole toolchain (Xcode debugger, `gpudebug`, `gpucapture`, Instruments) is macOS-only; there
  is no RenderDoc/cross-platform analog and no NVIDIA/AMD counter tooling. `[V]`
- The **Simulator is not a fallback**: it exposes a *family-2-class* device, translates calls to the
  host GPU, supports fewer features and different limits, and is explicitly documented as unsuitable
  for realistic profiling ("never profile on Simulator if you want real numbers"). A Metal 4 backend
  cannot be meaningfully validated in the Simulator. `[V]`
- §9 shows the hosted-CI situation compounds this.

### 6.5 Shader symbolization

- A `.gputrace` is far more useful with source. RPCS3's MSL comes from SPIRV-Cross, so the backend
  must emit SPIRV-Cross debug options / embed sources and keep the mapping from SPIR-V → MSL; Apple
  notes debug info must be included when compiling and that GPUTK4/Metal shader converter workflows
  rely on it. `[V]` Without this, shader debugging degrades to disassembly.
- RPCS3's shader cache keys on `"vulkan"` (README §6, R37); a `"metal"` namespace plus
  `MTL4PipelineDataSetSerializer` is needed for reproducible shader-cache behaviour. `[V]/[I]`

### 6.6 Gap summary

| Capability | Vulkan path today | Native Metal | Verdict |
|---|---|---|---|
| Programmable validation callback / message filtering | yes | no | **worse** |
| Debug Printf in shaders | yes | limited ("shader logging") | **worse** |
| Cross-platform GPU debugger (RenderDoc) | yes | no | **worse** |
| Headless/scriptable trace debugger | no | **`gpudebug`** | **better** |
| Shader source-level debugger | via RenderDoc | **Xcode shader debugger** | better |
| Live perf HUD | external tools | **`MTL_HUD_ENABLED`** | better |
| Per-line shader profiling / cost graph | limited | **yes** | better |
| Frame capture replay robustness | good | **known replay bugs** (§6.3) | worse |
| Simulator/software fallback | n/a | not usable for M4 | worse |
| CI-hosted execution | yes (Linux GPU runners exist) | **no** (GitHub macOS has no Metal) | **worse** |

---

## 7. Instrumentation: labels, counters, timestamps, HUD

### 7.1 What RPCS3 already exposes (verified, CPU-side)

- `frame_statistics_t` (`RSXDisplay.h:50-68`): `draw_calls`, `submit_count`, the four `*_time`
  buckets, vertex-cache and program-cache counters, `framebuffer_stats`. `thread::m_frame_stats` is
  reset per frame (`RSXThread.cpp:3568`) and attached to the flip (`RSXThread.cpp:3524`). `[V]`
- RSX load: `thread::get_load()` averages idle/elapsed over ~30 frames into
  `performance_counters.approximate_load` (`RSXThread.cpp:3427-3455`) `[V]`.
- Overlays: `perf_metrics_overlay` (`overlay_perf_metrics.h:11`) with `m_fps`, `m_frametime`,
  `m_rsx_load` (`:64/:65/:81`), FPS/frametime graphs; the debug overlay enables `m_profiler`
  (`m_profiler.enabled = !!g_cfg.video.debug_overlay`, `RSXThread.cpp:3569`) `[V]`.
- The per-draw timing buckets are written by the backends (`VKDraw.cpp` `setup_time`,
  `vertex_upload_time`, `textures_upload_time`, `draw_exec_time`; `VKPresent.cpp:985` `flip_time`). `[V]`

**Key fact for the Metal backend:** every timing number RPCS3 displays today is a **CPU-side wall
clock** (`m_profiler.duration()` around host work). There is **no GPU time**, no per-pass GPU cost,
and no GPU timestamp path at all. A Metal backend that only re-implements `frame_statistics_t`
inherits exactly the same blind spot.

### 7.2 Labels and groups (cheap, high value)

- Attach `label` to every `MTL4CommandBuffer` and encoder, and wrap passes in
  `pushDebugGroup`/`popDebugGroup`. Source the strings from `frame_trace_data` draw names/programs
  (`rsx_trace.h:14-22`) and RSX event names. `[V]/[I]`
- Emit `insertDebugSignpost` at RSX-observable boundaries (flip, resolve, cache upload) so a Metal
  System Trace lines up with `frame_statistics_t`.
- This is what makes the §3.4/§3.5 artifacts debuggable; it is nearly free and should be in the
  Phase-0 bring-up (README §7).

### 7.3 Timestamps & counters (net-new plumbing)

Proposed instrumentation, mapped to RPCS3 phases:

| RPCS3 phase | Source today | Metal instrumentation |
|---|---|---|
| Frame begin/end | flip/on_frame_end | `MTL4CommandBuffer.writeTimestamp(counterHeap:index:)` |
| Per-pass GPU cost | none | encoder `writeTimestamp(granularity:after:counterHeap:index:)` |
| CPU encode time | `m_profiler.duration()` | `os_signpost` intervals |
| GPU frame time | none | `MTLCommandBuffer.gpuStartTime/gpuEndTime` / HUD `GPU Time` |
| Per-draw GPU | none | not directly available; use debug groups + shader profiler |
| Counters | none | `MTLCounterSampleBuffer`/counter sets; Instruments limiter/utilization/occupancy |

- Use `MTL4CounterHeap` (+ `resolveCounterRange`/`sampleTimestamps`) and convert GPU timestamps to a
  CPU timeline (Apple documents this conversion). `[V]`
- Keep this **opt-in and low-overhead**: a timestamp write per pass is cheap; per-draw timestamps
  and instrumentation change the workload.
- Beware the residency-replay interaction (§6.3): heavy counter/ residency churn can make a captured
  trace un-replayable on some machines `[V]`.

### 7.4 HUD vs RPCS3 overlay

The Metal Performance HUD (`MTL_HUD_ENABLED=1`) is the direct analog of RPCS3's perf overlay and its
log line `metal-HUD: …` is machine-parseable — useful as a **second, independent** measurement in
the benchmark harness (§8.6) and as a zero-code sanity check during bring-up. `[V]`

---

## 8. The performance model

### 8.1 The frame-time identity

A single number is not a model. The useful decomposition, grounded in RPCS3's own fields:

```
frame_time = max(
    guest_CPU   = Cell/SPU/PPU work                (not backend-specific)
    rsx_CPU     = encode + uploads + setup         ≈ setup_time + vertex_upload_time
                                                   + textures_upload_time + draw_exec_time + flip_time
    gpu         = RSX work on the GPU             (new: need Metal timestamps)
) + bubbles     = sync stalls + pipeline drains + encoder-boundary flushes + swapchain waits
```

`rsx_CPU` is exactly `frame_statistics_t` today; `gpu` is invisible until §7.3 exists; `bubbles` is
where a Metal backend can *lose* (TBDR tile flushes at encoder boundaries; one encoder per draw is
documented as a trap, README §6 "not-recommended hygiene"). `[V]/[I]`

### 8.2 CPU side (where RPCS3 usually lives)

- RPCS3 is widely characterised as **CPU-limited** (its own blog calls memory latency and CPU
  threads the dominant factors; a deep-dive is dedicated to CPU/RAM scaling) `[V]`. The GPU backend
  choice therefore often changes *bubbles* and *stalls*, not the headline FPS.
- Metal 4's CPU story is genuinely better *if* used correctly: reused command buffers + explicit
  `MTL4CommandAllocator` (reset once GPU work completes; multiple allocators to avoid blocking
  encoding), argument tables that move binding off the critical path, and multi-threaded encoding
  with one allocator per thread (`MTL4CommandAllocator` is **not** thread-safe). `[V]`
- **Argument-table / residency costs:** Apple states residency-set updates are "a much lower cost at
  runtime", that populating a set can happen at startup, that rarely-changing sets should attach to
  the **queue** (frequently-changing to the command buffer), and that **fewer, larger** sets are
  preferred. `[V]` The hard cap is **32 residency sets per queue and per buffer** (Apple GPU family
  6+; "Not available" below family 6) and **32 counter heaps per process**. `[V]` RPCS3's texture
  cache will want to churn residency; this is the concrete tuning constraint.
- **Submission cost is not free:** an independent M4 Max measurement puts a command-buffer round trip
  at ~65 µs, of which `waitUntilCompleted` is ~63 µs median while the GPU runs ~1.7 µs — i.e.
  per-frame CPU/GPU coordination dominates for small work. RPCS3's own flip/submit path and the
  parity gap between "one CB per draw" and "one CB per frame" matter a lot. `[V]` (measured on a
  specific workload; treat as order-of-magnitude `[I]`).

### 8.3 Memory model (a real change, mostly in the backend's favour)

- Apple silicon is **unified memory** (`hasUnifiedMemory()`), so the CPU/GPU DMA and PCIe-latency
  reasoning in RPCS3's own performance blog (upload/download, transfer lag, GPU sync stalls) is
  largely replaced by "shared buffers" `[V]/[I]`. There is no host→device copy for a shared
  `MTLBuffer`.
- Measured base M4 bandwidth is ~100 GB/s (STREAM, matching the ~120 GB/s spec), and the M-series
  GPU outperforms the CPU in FP32 from M2 onward; treat these as ballpark, not constants. `[V]`
- **TBDR economics are the flip side:** load/store actions and memoryless storage determine whether
  a pass touches system memory, and RSX's exploded MSAA model (R16) means RPCS3 may be forced to
  resolve-on-store and keep persistent resolve textures, costing bandwidth Metal users would
  normally avoid. `[V]/[I]`

### 8.4 What the native backend can win vs. what it can lose

**Win (vs MoltenVK/KosmicKrisp):**

- Delete a layered implementation. MoltenVK itself documents "unavoidable memory and performance
  overhead", that Metal lacks some fine-tuning controls, and that SPIR-V→MSL can introduce shader
  inefficiencies vs hand-tuned code; it also loops on the CPU for `drawCount` indirect draws
  (a real 5.8 ms→0.017 ms example in their issue). Vulkan Portability conformance was ~3.2% failing
  on 1.2+ext (2023). `[V]` — this bounds the *upside*: the layer is not free, but it is also not the
  bottleneck for a CPU-limited emulator.
- Metal 4 primitives (argument tables, residency sets, command allocators, encoder suspend/resume +
  pass merging) are designed to cut CPU encode cost. `[V]`
- MetalFX spatial upscaling as an FSR1 replacement (README §6, R27). `[V]/[I]`
- Unified memory removes a class of transfer stalls. `[I]`

**Lose:**

- TBDR tuning surface: getting load/store/memoryless right per RSX surface is a new discipline
  (R11/R16). `[I]`
- `vkCmdBlitImage` becomes a compute/graphics meta pass (R25); a scaled blit is no longer one call. `[I]`
- Dynamic `LINE_WIDTH`, custom border colours, exact LOD bias are lost or approximated (R18/R19/R24). `[I]`
- One encoder per draw is catastrophic (tile-memory flush); RPCS3's per-draw state churn must be
  regrouped (README §6 NR1). `[I]`

**Bottom line (inherited, README §2):** the measured upside over a *healthy* KK/MVK is
"modest"; the native backend is a research northstar, not a promised speed-up. §8 is a model for
*measuring* whether that changes, not a claim that it will.

### 8.5 Non-goals for the perf model

- Do not model Cell/SPU/PPU (unchanged by the backend).
- Do not assume MoltenVK is "slow" without measuring; MVK's own maintainer calls its overhead "low".
  `[V]`
- Do not compare across GPU families as if the number were a constant.

### 8.6 Predictions to falsify (all `[I]`)

| Hypothesis | How to test |
|---|---|
| `rsx_CPU` (encode) is *lower* natively than via KK for the same capture | Δ`setup_time+draw_exec_time` per draw in the replay harness |
| `gpu` time is *higher* natively at first due to TBDR pass flushes | GPU timestamps per pass; count encoder boundaries |
| Frame time is dominated by `bubbles` (sync/swapchain), not compute | Instruments Metal System Trace overlap analysis |
| Residency churn is a measurable `rsx_CPU` cost | vary residency-set granularity; measure (32-set cap) |
| MoltenVK's MSL codegen costs shader time | compare shader-bytecode/cost graph; MVK perf trackers |

---

## 9. Benchmarking plan & CI strategy

### 9.1 Benchmark methodology

1. **Fixed workload:** one or more `.rrc.gz` captures (§3.3), fixed resolution, fixed vsync, fixed
   draw count; headless where the capture allows it.
2. **Warm-up:** discard the first N frames (shader/pipeline compile, cache fill).
3. **Statistics, not averages:** report median and p1/p99 frame time, not a single mean; a single
   mean hides the stalls that matter for an emulator. RPCS3's overlay already tracks 1%-low
   concepts (issue #9153 requested exactly this). `[V]`
4. **Two independent instruments:** the backend's own `frame_statistics_t` + Metal timestamps
   (§7.3), and the HUD `metal-HUD:` log / Instruments (`[V]`).
5. **Control the environment:** record device family, OS, thermal state (HUD exposes it), and avoid
   profiling under other GPU load; forced performance state exists in Instruments for repeatability. `[V]`
6. **Beware residual state:** a study on an M4 Pro shows a reproducible post-GPU **system-level-cache
   displacement** effect (a 16 MiB SLC traversal slowed 31–63% after a completed GPU footprint; a
   second traversal removes part of it). Run-to-run comparisons need warm-up/traversal control if
   they approach cache-sensitive workloads. `[V]`

### 9.2 Metrics to track

Frame time (p1/p50/p99), GPU time (per pass + total), CPU encode time (signposts), `frame_statistics_t`
buckets, encoder/command-buffer counts, residency-set update count, peak allocated size
(`MTLDevice.currentAllocatedSize`), draw calls, and (qualitative) validation-error count.

### 9.3 Regression gates

- Compare against a **per-machine baseline**, not a global constant; gate on a **noise band**
  (e.g. fail only beyond k× the observed run-to-run σ), and never compare across GPU families. `[I]`
- Correctness gate = §4.3 buckets A/B; perf gate = p50/p99 within band. Keep them separate: a
  correctness re-baseline (driver update) must not block a perf change and vice versa.

### 9.4 CI strategy — the constraint is real and verified

**GitHub-hosted macOS runners cannot run Metal `[V]`.** Evidence, strongest first:

- `actions/runner-images#1779` (2020, closed): inside a GitHub-hosted macOS runner
  `MTLCreateSystemDefaultDevice()` **returns nil** and `MTLCopyAllDevices()` is empty; the reporter
  identifies GPU configuration in the guest. GitHub's maintainer replied that their macOS
  virtualisation approach cannot do GPU passthrough and there were "no plans to make GPU functions
  available for macOS runners in the nearest future". `[V]`
- `actions/runner-images#7085` (2023, closed without Metal): a request to add Metal/GPU passthrough
  for `macos-latest`, with a commenter stating it is "currently impossible due to runners' design".
  `[V]`
- Community discussion `#160669`: "Metal is not currently available on Github Actions macOS images".
  `[V]`
- Note the *misleading* marketing: GitHub's 2023 M1-runner blog says the runner "comes with GPU
  hardware acceleration enabled by default" — but that describes the **host**/build speedup, not a
  `MTLDevice` exposed to the guest. Trust #1779, not the blog. `[V]`
- Caveat/`[U]`: `actions/runner-images#14836` reports **macOS-15-*intel*** images where Metal work on
  multisampled textures hangs/returns wrong results, implying *some* Metal device exists on that
  image. The situation is image- and architecture-dependent and changing; **do not design tests
  around the possibility.** Treat "no reproducible Metal on hosted runners" as the working
  assumption and re-verify periodically.

**Proposed three-tier CI:**

| Tier | Where | What runs | Gate |
|---|---|---|---|
| **A (hosted, every PR)** | GitHub `macos-*` hosted | Android/desktop builds + **GPU-free** `rpcs3_test` (`CMakeLists.txt:180-225`) + static checks + `[I]` an `mtl`-tagged ctest that **skips** when `MTLCreateSystemDefaultDevice()==nil` | required |
| **B (self-hosted Apple silicon, opt-in)** | a bare-metal Mac (M1–M4) runner | API-validation smoke (`MTL_DEBUG_LAYER=1`), capture replay + golden image (§3.3–3.4), determinism "run twice" (§4.4) | required for `MTL/**` changes |
| **C (self-hosted, nightly)** | same Mac(s) | full capture corpus, perf tracking + baselines, shader-validation job, `.gputrace` artifacts on failure | non-blocking, tracked |

**Concrete mechanisms:**

- The skip predicate is trivial and honest: if no `MTLDevice`, `GTEST_SKIP()`/`return` with a clear
  message; this is why Tier A stays green while GPU work is developed (`[I]`, mirrors the Go/graphics
  pattern of probing `MTLCreateSystemDefaultDevice` at runtime).
- Use `gpudebug`/`gpucapture` and `.gputrace`/frame-debug dumps as **CI artifacts** so failures can
  be diagnosed offline (§3.5) even when the CI host has no GPU `[V for the tools, I for the plan]`.
- Keep shader validation in its own job (it is expensive, §6.2) `[V]`.
- If GitHub ever exposes `ParavirtualizedGraphics` in the guest (Apple ships a paravirtualised Metal
  adapter, and VMware/Parallels support it on Apple silicon), revisit Tier B as a hosted tier; until
  then, a self-hosted bare-metal Mac is the only way to run Metal in CI. `[V]/[U]`

---

## 10. Effort, risk & open questions

### 10.1 Effort/risk table (difficulty 1–5, sibling scale)

| # | Work item | Diff | Risk | Notes |
|---|---|---|---|---|
| V1 | GPU-free unit tests (format table, binding math, workaround profile, cache keys) | 2 | low | Mirrors `test_vk_driver_selection.cpp`; runs in Tier A. |
| V2 | Metal encoder labels / debug groups / signposts from `frame_trace_data` | 2 | low | Cheap; do in bring-up (README §7 Phase 0). |
| V3 | GPU timestamp + counter plumbing (`MTL4CounterHeap`/`writeTimestamp`) | 4 | med | Net-new (no timestamps today); residency-replay interaction (§6.3). |
| V4 | Capture-replay conformance harness (`--rsx-capture` + screenshot + compare) | 4 | med | Pieces exist; glue + tolerance model + corpus are the work. |
| V5 | Golden-image tolerance model + manifests + re-baseline policy | 4 | med | No golden-image suite exists today; driver/GPU-family drift. |
| V6 | Determinism test ("same backend twice") + divergence classification | 3 | med | Cheapest high-value guard; blocks pixel testing otherwise. |
| V7 | Validation integration (API + shader env profiles, per-pipeline scoping) | 3 | med | Process-global env; shader validation expensive; CI host needed. |
| V8 | `.gputrace` capture trigger + artifact plumbing (env-var equivalent) | 3 | low | Mirror `MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE` / `MESA_KK_GPU_CAPTURE`. |
| V9 | Perf model instrumentation (per-pass GPU, CPU signposts, HUD log parse) | 4 | med | Depends on V3; needs stable baseline methodology. |
| V10 | Benchmark corpus + regression gates + baseline store | 4 | med | Per-machine baselines; noise-band gates. |
| V11 | Tier-B/C self-hosted CI runners + nightly perf job | 4 | high | Ops + hardware; no hosted alternative (§9.4). |
| V12 | Shader debug-info emission through SPIRV-Cross + `"metal"` cache namespace | 3 | med | Needed for shader symbolization; ties to scope 03 (R37). |
| V13 | Rung-3 (guest-observable) correctness spike: readback/ZCULL/MSAA resolve | 5 | high | The only "objectively RSX" oracle; unknown scope. |
| V14 | Cross-backend differential tool (MTL vs VK/KK image + `frame_debug` diff) | 4 | med | The practical oracle until rung 3 exists. |

### 10.2 Top risks (ranked)

1. **No local pixel oracle.** Correctness is defined by agreement with another backend on a
   TBDR-vs-not rasterizer; false positives/negatives are likely. Mitigation: V6+V14+V5 with a
   *classified* tolerance model (§4.3), and invest in rung 3 (V13).
2. **CI cannot test the thing that matters.** GitHub macOS has no Metal (§9.4). Without a
   self-hosted Mac, all GPU verification is manual. This is a *project* risk, not a backend bug.
3. **Tooling gaps delay diagnosis.** No validation callback / Debug Printf; `.gputrace` replay
   bugs; heap-residency validation blind spots. Mitigation: V7+V8, and treat `gpudebug` as the
   primary offline tool.
4. **TBDR performance may not beat the layered path** (README §2: "modest" upside). Mitigation: V9
   + §8.6 falsifiable hypotheses; do not promise a speed-up.
5. **False determinism failures** wreck the harness if a workload isn't pinned (multithreaded RSX,
   frame-skip, cache state). Mitigation: §4.4 policy, V6.

### 10.3 Open questions for the human

1. **Is a self-hosted Apple silicon runner acceptable** to unlock Tier B/C, or must everything be
   hosted (in which case GPU verification is permanently manual)? (§9.4)
2. **Do we adopt golden-image tests at all**, given driver/GPU-family drift and the re-baseline
   burden, or keep verification to capture replay + `frame_debug` diffs + human review? (§3.4)
3. **What is the tolerance definition** for rung 4 (exact metric + thresholds + per-region buckets)?
   (§3.4, §4.3)
4. **Do we invest in rung 3** (guest-observable readback/MSAA/ZCULL correctness)? It is the only
   objective oracle but its scope is unknown. (§2.2, V13)
5. **Which capture corpus** represents "enough" coverage, and where do the `.rrc.gz` files live
   (repo size, licensing)? (§3.3)
6. **Do we ship a Metal Performance HUD/timestamp-based overlay** in addition to the existing
   `perf_metrics_overlay`, or keep the overlay CPU-only and use the HUD for GPU numbers? (§7.4)
7. **Is `MTL_SHADER_VALIDATION` cheap enough** to leave on for Tier B, or per-pipeline only? (§6.2)
8. **How do we handle the residency-set cap (32)** in the texture cache without churning residency
   per frame? (§8.2; ties to README §6 R2/R8 and scope 04.)

---

## 11. Sources

**RPCS3 source (verified in-tree, 2026-10-15; line numbers advisory)**
- `rpcs3/Emu/RSX/Core/RSXDisplay.h:50-68` — `frame_statistics_t`.
- `rpcs3/Emu/RSX/RSXThread.h:34,36,162,317,475` — `g_user_asked_for_frame_capture`, `frame_debug`, `m_frame_stats`, `capture_current_frame`.
- `rpcs3/Emu/RSX/RSXThread.cpp:43,44,46,921,979,3465-3511,3484,3524,3568,3427-3455` — screenshot flag, capture save, frame-debug, stats reset, `get_load()`.
- `rpcs3/Emu/RSX/Capture/rsx_replay.h:13,161,188` / `rsx_replay.cpp:16-174` — `frame_capture_data`, magic `RRC`, `rsx_replay_thread`.
- `rpcs3/Emu/RSX/Capture/rsx_trace.h:11-32` — `frame_trace_data`.
- `rpcs3/Emu/RSX/GSFrameBase.h:35` — `take_screenshot`.
- `rpcs3/Emu/RSX/VK/VKPresent.cpp:662-779,985` / `GL/GLPresent.cpp:331-387` — screenshot readback, flip time.
- `rpcs3/Emu/RSX/Program/GLSLSnippets/RSXProg/RSXFragmentPrologue.glsl:30,36,44` and `RSXROPEpilogue.glsl:19-22,84` — `round_to_8bit`, depth quantization.
- `rpcs3/Emu/RSX/Overlays/overlay_perf_metrics.h:11,29-31,64-65,81` / `.cpp` — perf overlay.
- `rpcs3/rpcs3.cpp:391,414,448,843,1228-1240` — `--headless`/`--no-gui`, `--rsx-capture`.
- `rpcs3/CMakeLists.txt:180-225` — `rpcs3_test` + `gtest_discover_tests`; `rpcs3/tests/` listing (GPU-free).
- `rpcs3/tests/test_vk_driver_selection.cpp` — GPU-free logic-test pattern.
- Grep result: **no** `VK_QUERY_TYPE_TIMESTAMP` / `vkCmdWriteTimestamp` in `rpcs3/Emu/RSX`.

**Apple — validation**
- Validating your app's Metal API usage — <https://developer.apple.com/documentation/xcode/validating-your-apps-metal-api-usage>
- Validating your app's Metal shader usage — <https://developer.apple.com/documentation/xcode/validating-your-apps-metal-shader-usage>
- `MetalValidation(1)` man page — <https://keith.github.io/xcode-man-pages/MetalValidation.1.html>
- `MTLShaderValidation` — <https://developer.apple.com/documentation/metal/mtlshadervalidation>

**Apple — debugging, capture, profiling, counters, HUD**
- Metal debugger — <https://developer.apple.com/documentation/xcode/metal-debugger>
- Metal developer workflows — <https://developer.apple.com/documentation/xcode/metal-developer-workflows>
- Debugging with interactive command-line tools (`gpudebug`) — <https://developer.apple.com/documentation/xcode/debugging-with-interactive-command-line-tools>
- Investigating GPU issues with AI agents — <https://developer.apple.com/documentation/xcode/investigating-gpu-issues-with-ai-agents>
- Metal developer tools (`gpucapture`, `gpudebug`, `metalperftrace`) — <https://developer.apple.com/metal/tools>
- Capturing a Metal workload in Xcode — <https://developer.apple.com/documentation/xcode/capturing-a-metal-workload-in-xcode>
- Capturing a Metal workload programmatically — <https://developer.apple.com/documentation/xcode/capturing-a-metal-workload-programmatically>
- `MTLCaptureManager` — <https://developer.apple.com/documentation/metal/mtlcapturemanager>
- `MTLCaptureScope` — <https://developer.apple.com/documentation/metal/mtlcapturescope>
- Metal debugging types (frame capture, shader logs) — <https://developer.apple.com/documentation/metal/metal-debugging-types>
- Analyzing the performance of your Metal app (Instruments Game Performance) — <https://developer.apple.com/documentation/xcode/analyzing-the-performance-of-your-metal-app>
- Reducing shader bottlenecks (limiter/utilization counters) — <https://developer.apple.com/documentation/xcode/reducing-shader-bottlenecks>
- Finding your Metal app's GPU occupancy — <https://developer.apple.com/documentation/xcode/finding-your-metal-apps-gpu-occupancy>
- Monitoring your Metal app's graphics performance (HUD) — <https://developer.apple.com/documentation/xcode/monitoring-your-metal-apps-graphics-performance>
- Customizing the Metal Performance HUD — <https://developer.apple.com/documentation/xcode/customizing-metal-performance-hud>
- Understanding the Metal Performance HUD metrics — <https://developer.apple.com/documentation/xcode/understanding-metal-performance-hud-metrics>
- `MTL4CommandBuffer.writeTimestamp(counterHeap:index:)` — <https://developer.apple.com/documentation/metal/mtl4commandbuffer/writetimestamp(counterheap:index:)>
- `MTL4RenderCommandEncoder.writeTimestamp` — <https://developer.apple.com/documentation/metal/mtl4rendercommandencoder>
- Recording Performance Data (`os_signpost`) — <https://developer.apple.com/documentation/os/recording-performance-data>
- Metal Shader Debugging and Profiling (WWDC18) — <https://developer.apple.com/videos/play/wwdc2018/608>

**Apple — Metal 4 performance, tooling, virtualisation**
- Discover Metal 4 (WWDC25) — <https://developer.apple.com/videos/play/wwdc2025/205/>
- Explore Metal 4 games (WWDC25) — <https://developer.apple.com/videos/play/wwdc2025/254/>
- Understanding the Metal 4 core API — <https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api>
- Metal Feature Set Tables (PDF) — <https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf>
- Developing Metal apps that run in Simulator — <https://developer.apple.com/documentation/metal/developing-metal-apps-that-run-in-simulator>
- `MTLCreateSystemDefaultDevice()` — <https://developer.apple.com/documentation/metal/mtlcreatesystemdefaultdevice()>
- Paravirtualized Graphics — <https://developer.apple.com/documentation/paravirtualizedgraphics>
- `MTLResidencySet` (Rust binding mirror of the Apple header) — <https://docs.rs/objc2-metal/latest/aarch64-apple-tvos/src/objc2_metal/generated/MTLResidencySet.rs.html>

**Vulkan / comparison tooling**
- `VK_LAYER_KHRONOS_validation` — <https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/khronos_validation_layer.md>
- Debug Printf — <https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/debug_printf.md>
- Vulkan Debug Utilities (`VK_EXT_debug_utils`) tutorial — <https://docs.vulkan.org/samples/latest/samples/extensions/debug_utils/README.html>
- Validation layers tutorial — <https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/00_Setup/02_Validation_layers.html>

**MoltenVK (layered-path baseline)**
- MoltenVK Runtime User Guide (portability + performance caveats) — <https://android.googlesource.com/platform/external/moltenvk/+/show/HEAD/Docs/MoltenVK_Runtime_UserGuide.md>
- "Performance vs raw Metal" (issue #825) — <https://github.com/KhronosGroup/MoltenVK/issues/825>
- "Apple M1 Max MoltenVK CPU performance is slow" (indirect drawCount; issue #1749) — <https://github.com/KhronosGroup/MoltenVK/issues/1749>
- Vulkanised 2023 — MoltenVK application portability (conformance/perf) — <https://vulkan.org/user/pages/09.events/vulkanised-2023/vulkanised_2023_moltenvk_application_portability_with_vulkan_on_metal.pdf>

**CI / hosted runners**
- `actions/runner-images#1779` — MTLCreateSystemDefaultDevice returns nil — <https://github.com/actions/runner-images/issues/1779>
- `actions/runner-images#7085` — Add GPU Passthrough (Metal) — <https://github.com/actions/runner-images/issues/7085>
- `actions/runner-images#14836` — macos-15-intel Metal hang/wrong results — <https://github.com/actions/runner-images/issues/14836>
- `actions/runner-images` discussion #6138 — Add support for Metal in macOS images — <https://github.com/actions/runner-images/discussions/6138>
- GitHub community discussion #160669 — Metal unavailable on macOS images — <https://github.com/orgs/community/discussions/160669>
- GitHub blog: M1 macOS larger runner — <https://github.blog/news-insights/product-news/introducing-the-new-apple-silicon-powered-m1-macos-larger-runner-for-github-actions>
- GitHub Docs: larger runners reference — <https://docs.github.com/en/actions/reference/runners/larger-runners>

**Performance data & measured behaviour**
- RPCS3 blog — "A Deep-Dive into Hardware and Performance Scaling" — <https://blog.rpcs3.net/2020/08/21/hardware-performance-scaling>
- RPCS3 issue #9153 — Improve Performance overlays (min/max, 1% low, avg) — <https://github.com/RPCS3/rpcs3/issues/9153>
- Apple vs. Oranges: Apple Silicon M-series for HPC (STREAM/GEMM/powermetrics) — <https://arxiv.org/pdf/2502.05317v1>
- Residual GPU Cache State on Apple M4 Pro (post-GPU SLC displacement) — <https://arxiv.org/pdf/2606.27098>
- ArrowMetal `RESIDENT.md` — measured M4 Max command-buffer round-trip cost — <https://github.com/singhpratech/ArrowMetal/blob/main/docs/RESIDENT.md>
- metal-gpu-bench (M4 memory/FLOPS/latency) — <https://github.com/fangde/metal-gpu-bench>
- MLX issue #2846 — residency-set mutation limit breaks `.gputrace` replay — <https://github.com/ml-explore/mlx/issues/2846>
- `sebbbi/NoGraphicsAPI` — Metal 4 validation notes & MetalTools limitations — <https://github.com/sebbbi/NoGraphicsAPI/blob/main/docs/metal-validation.md>
- Apple Game Porting Toolkit skill — using Metal validation (perf cost, per-pipeline scoping) — <https://github.com/apple/game-porting-toolkit/blob/main/game-porting-skills/skills/using-metal-validation/SKILL.md>
- flutter/impeller — Metal validation & HUD without Xcode — <https://github.com/flutter/flutter/blob/master/docs/engine/impeller/docs/metal_validation.md>
- MLX Metal debugger / `MTL_CAPTURE_ENABLED` workflow — <https://ml-explore.github.io/mlx/build/html/dev/metal_debugger.html>

**In-set (inputs, not re-derived)**
- `docs/research/metal4/README.md` §2, §3, §6 (R2/R8/R10/R11/R13/R16/R18/R19/R24/R25/R27/R32/R37), §7, §9.
- `docs/research/metal4/01-core-memory.md`, `02-submission-sync.md`, `03-shaders-pipelines-descriptors.md`, `04-images-formats-samplers-rtt.md`, `05-presentation-and-impossible-register.md`.
- `docs/research/03-rpcs3-backend-architecture.md`.
