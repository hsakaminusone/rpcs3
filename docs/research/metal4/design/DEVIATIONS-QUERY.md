# DEVIATIONS-QUERY — occlusion queries, ZCULL, conditional render, visibility results and GPU time

Scope: the query path of the RSX Metal 4 backend (`rpcs3/Emu/RSX/MTL`). This file audits the
visibility-result buffer, the five shared ZCULL hooks, the counter pool and its completion gate,
conditional rendering, and GPU timing. It compares the Metal path with Vulkan
(`rpcs3/Emu/RSX/VK`), OpenGL (`rpcs3/Emu/RSX/GL`) and the shared report machinery
(`rpcs3/Emu/RSX/RSXZCULL.{h,cpp}`, `rpcs3/Emu/RSX/RSXThread.cpp`,
`rpcs3/Emu/RSX/rsx_methods.cpp`). It also mines KosmicKrisp
(`.kk-mesa/src/kosmickrisp`, read-only) for Metal 4 query techniques.

## 1. Method

1. I read the Metal query path in full: `MTLZCULL.{h,cpp}`, the visibility-result setup in
   `MTLFramebuffer.{h,cpp}` (`open_pass` 100-198) and `MTLCommandRecorder.{h,cpp}`
   (`begin_frame`, `flush_for_readback`, `open_pass`, `on_feedback`), and every query site in
   `MTLGSRender.cpp` (init 634-635, `open_frame` 736-742, `bind_framebuffer` 826,
   `should_skip_draw` 1571, `end`/`emit_geometry` 1939, 1955, 2658, `on_access_violation` 3458).
   `rgba16f` appears nowhere in the Metal backend; there is no such query path to audit.
2. I read the shared code that the backend implements: `RSXZCULL.h` (the five virtual hooks,
   the 2048-slot pool, the report queue) and `RSXZCULL.cpp` (`set_active`, `read_report`,
   `on_draw`, `update` 529-655, the two `read_barrier` overloads 660-753, `write` 337-380), plus
   `RSXThread.cpp` 928-958 (CPU conditional render), 1012-1025 (`should_skip_draw`), 3233-3237
   (guest report read) and `rsx_methods.cpp:1687-1690` (the ZCULL/ZPASS method bindings). The
   path named in the brief, `rpcs3/Emu/RSX/rsx_method_registers`, does not exist in this tree.
3. I read the reference path: `VKGSRender.cpp` 602-605, 2889-2996, `VKQueryPool.{h,cpp}`
   (`check_query_status`, `get_query_result` 163-192), `VKGSRenderTypes.hpp` 148-173,
   `GLGSRender.cpp` 144, 1489-1530.
4. I read KosmicKrisp as a Metal API reference only: `vulkan/kk_query_pool.c`,
   `vulkan/kk_query_table.c`, `vulkan/kk_cmd_draw.c` (432-435, 1400-1440),
   `vulkan/kk_cmd_meta.c` (75-120), `bridge/mtl_encoder.{h,m}`, `libkk/kk_query.cl`, and the
   Metal 4 SDK headers (`MTL4RenderPass.h`, `MTLRenderPass.h`, `MTL4RenderCommandEncoder.h`,
   `MTL4CommandEncoder.h`) for the visibility-result and barrier contracts.
5. I did not build, run, commit or push. I changed one file: this one. I did not edit
   `.kk-mesa`, `filesdump/`, Vulkan code or OpenGL code.
6. Mesa policy note: I only read `.kk-mesa`. I did not edit it, and I did not produce Mesa
   prose or Mesa commit messages. Marker: biblioklept.
7. Confirmation rule: `Yes` = the difference is certain from the code; `Partial` = the code
   path is certain but the visible effect needs a run; `No` = the claim needs an on-device
   measurement. No row below is `No`; the rows that would have been are in §4 with the
   measurement that would settle them.
8. Line numbers are as read on 2026-10-20. The Metal sources are being edited concurrently
   (`git status` shows them modified), so a citation may drift by a few lines.
9. Not repeated here: `DR8` (conditional-render slot dead ABI), `SY7`/`SY8` (frame-boundary
   hazards), and the settings rows `A37` (`relaxed_zcull_sync`) / `A43` (`precise_zpass_count`),
   which already own the "self-declared ignored" finding. This file references them instead.

## 2. Findings table

| id | deviation | Metal file:line | VK/GL reference file:line | class | severity | confirmed? | proposed fix |
|---|---|---|---|---|---|---|---|
| Q1 | Every forced ZPASS read is taken from the CPU without submitting the open command buffer and without waiting for commit feedback. The forced path bypasses `check_occlusion_query_status` entirely (`force_read or ...`), so the value is whatever the last completed commit left, which is 0 after the CPU clear at begin, or a partial count from an earlier frame of the query. Vulkan submits, busy-waits and then reads; GL blocks inside `glGetQueryObjectiv(GL_QUERY_RESULT)`. This fires on every conditional-render evaluation (Metal reports no hardware predicate, `DR8`) and on every guest read of a ZPASS report address. | `MTLZCULL.cpp:249-263` (read at `:260`), `:230-247`; forced path `RSXZCULL.cpp:616-620`, `:734-737`, `:747-750`; conditional render `RSXThread.cpp:952-956` | `VKGSRender.cpp:2952-2966` (`flush_command_queue`, `busy_wait`, `data.sync`), `VKQueryPool.cpp:168-182` (polls until ready); `GLGSRender.cpp:1512-1519` | wrong result | H | Yes | Give `zcull` the recorder handle and, when the end slot is unreported, call the bounded `flush_for_readback` (`MTLCommandRecorder.cpp:277-331`, the primitive the texture readback already uses) before reading `contents()`. Never read a counter whose producing slot is unreported. |
| Q2 | `apply` caches the emitted mode on the raw encoder pointer and never invalidates it at a pass or frame boundary. Every draw opens a new pass (`framebuffer::open_pass`), and the previous encoder was ended just before, so Metal can hand the next encoder the same address; `apply` then returns early and the whole pass runs with visibility mode Disabled, so its draws silently do not count without any error. `begin_frame` clears the recorder's `m_encoder` but not this cache. | `MTLZCULL.cpp:145-148` (cache), `:159-161`; `MTLCommandRecorder.cpp:347-356` (new encoder per pass), `:216`; `MTLGSRender.cpp:1955`, `:2658` (the only call site) | KosmicKrisp re-emits on a dirty flag, not encoder identity (`kk_cmd_draw.c:1435-1437`, `kk_cmd_meta.c:110-117`); VK has no such cache at all (`VKGSRender.cpp:2889-2900`) | wrong result | H | Partial | Invalidate from `command_recorder::open_pass` or `begin_frame` (a pass is a new encoder, so the mode must be re-emitted), or drop the cache and emit per draw (one state call). A GPU capture of a four-pass query whose per-pass counter is read back settles whether address reuse happens today. |
| Q3 | A timed-out slot is still reported as complete to the ZCULL module. `open_frame` calls `on_slot_complete` unconditionally after `begin_frame`, so the abandoned frame's generation is bumped, counters deferred on that slot are returned to the free list, and the next `begin_occlusion_query` zeroes and reuses one while the abandoned command buffer can still accumulate into it. The new query then also reports the abandoned frame's fragments. (SY2/SY3 own the memory; this is the query consequence.) | `MTLGSRender.cpp:738`; `MTLCommandRecorder.cpp:164-176` (abandon, no `reported` result exported); `MTLZCULL.cpp:96`, `:103-114`, `:205` | No equivalent: VK never recycles a query whose command buffer is pending (`VKGSRenderTypes.hpp:154-172`), and `VKQueryPool.cpp:194-210` frees only unreferenced pools | wrong result | H | Yes | Return `reported` from `begin_frame` and pass it to `on_slot_complete`; on abandonment, move the slot's deferred counters to the quarantine list with the slot's resources instead of freeing them. |
| Q4 | The completion gate only advances when the slot is reused three frames later, so a query is never readable before frame N+3 even though its own frame's commit reported at the end of frame N. `update` also breaks at the first unready writer, so one late query holds the whole report queue (head-of-line) and the guest's report writes behind it. Vulkan ties availability to the command buffer that holds the query, not to a ring position. | `MTLZCULL.cpp:225-227`, `:230-247` (gate at `:246`); `MTLGSRender.cpp:738`; `RSXZCULL.cpp:616-625` | `VKGSRenderTypes.hpp:154-172`; `VKGSRender.cpp:2928-2942` | performance | M | Yes | Bump a per-slot ZCULL epoch from `command_recorder::on_feedback` (`MTLCommandRecorder.cpp:422-474`) and gate on that, so a query is ready once its own frame reports. Keep the slot-reuse bump as the fallback for an unreported slot. |
| Q5 | Every occlusion query uses `VisibilityResultModeCounting`. The boolean fast path is never used, so with `precise_zpass_count` off (the default) the backend still pays a per-fragment count where VK, GL and KK only test for a first hit. | `MTLZCULL.cpp:150` | `VKGSRender.cpp:602-605`, `VKQueryPool.cpp:141`; `GLGSRender.cpp:144` (`GL_ANY_SAMPLES_PASSED`); KosmicKrisp `kk_query_pool.c:423-429` | performance | M | Yes | Pick the mode from `g_cfg.video.precise_zpass_count`, as KK does. `write` already collapses any non-zero to `0xFFFF` when the setting is off (`RSXZCULL.cpp:350-356`), so boolean mode's 1 is sufficient. A43 owns the reported-value side; this row is the hardware cost. |
| Q6 | When the counter pool is empty, `begin` marks the query unowned and `get_result` returns the base class's -1 sentinel ("unknown, treat as fully visible") while VK leaves the result at 0 and GL returns 0, so a conditional render runs the block it should skip. The free list can fall below the 2048-query ceiling because deferred counters are held until their slot completes. | `MTLZCULL.cpp:185-191`, `:249-258` (`:256`), `:276-313` | `VKGSRender.cpp:2944-2948` (result stays 0), `GLGSRender.cpp:1512-1520` | wrong result | L | Partial | Return 0, not -1, for a query that had draws but no counter, and log once. Or size the buffer to `occlusion_query_count + 2 * max_reports_per_frame` so the deferred window cannot empty the free list. |
| Q7 | There is no GPU timestamp path: no `MTL4::CounterHeap`, so per-pass GPU time is unavailable and all timing is CPU-side (`MTL_PERF_SCOPE`). The only GPU time is the commit feedback's start/end pair, one per frame. RSX has no guest timestamp query, so this is diagnostic, not semantic. | `MTLCommandRecorder.cpp:438-453`; no `CounterHeap` anywhere in `rpcs3/Emu/RSX/MTL` (grep) | Vulkan has no guest timestamp query either (`VKQueryPool.cpp:53`, only `VK_QUERY_TYPE_OCCLUSION` is created); KosmicKrisp `kk_query_pool.c:198-207`, `:353-415` | missing feature | L | Yes | Optional: sample a counter heap at pass boundaries behind the profiler switch. No guest behaviour depends on it. |

## 3. Highest-risk findings

1. **Q1 is the row that changes what a game sees.** Conditional rendering on this backend is a
   CPU predicate, and the predicate is read from a buffer the GPU may not have been given yet.
   The failure is one-sided and visible: a block that should render is skipped. It is not a
   rare path either, because `DR8` means every conditional render takes it. The fix is bounded
   and reuses `flush_for_readback`, which already implements "submit and wait with a timeout".
2. **Q2 is the same kind of silent failure with an allocator-shaped trigger.** A pass that
   should count reports nothing, and nothing logs it. The code cannot prove reuse; a capture
   can. Until it is settled, invalidating the cache costs one state call per pass.
3. **Q3 is the only row that corrupts a *correct* query.** It needs a feedback timeout, which
   is abnormal, but that is exactly the window SY1 creates when the readback wait is bounded.
   Fix Q1 and Q3 together, the way SYNC pairs SY1 and SY2.
4. **Q4 is not a wrong result, but it is the one a title will feel.** Three frames of added
   latency on every ZPASS report, plus head-of-line blocking of the report queue, is enough to
   make visibility-driven rendering lag. The feedback handler already knows the answer one
   frame earlier; using it costs an epoch counter.

## 4. Checked and not a deviation

- **`VisibilityResultTypeAccumulate`.** The SDK defines `Reset` as "reset visibility result data
  when you create a render command encoder" and `Accumulate` as "accumulate across multiple
  render passes" (`MTLRenderPass.h:57-64`). Pass-per-clause (`D10`) and queries that span frames
  both need accumulate, and the descriptor sets it on every pass (`MTLFramebuffer.cpp:131-138`).
  Correct, and it also means the CPU clear at begin (`MTLZCULL.cpp:205`) is mandatory rather
  than redundant.
- **Counter layout and alignment.** 2048 counters of 8 bytes is exactly 16 KiB, the last valid
  offset is 2047*8, and `setVisibilityResultMode` requires a multiple of 8
  (`MTL4RenderCommandEncoder.h:225-229`). `counter_stride` and `counter_capacity`
  (`MTLZCULL.cpp:15`, `MTLZCULL.h:38`) satisfy both; `occlusion_query_count` is 2048
  (`RSXZCULL.h:93`) and the base pool is the same size (`:118`), so the driver-handle mapping
  in `init` (`MTLZCULL.cpp:46-49`) matches `VKGSRender.cpp:597-600`.
- **Per-fragment vs per-sample counting.** Metal counts "fragments (pixels) passing the depth
  and stencil tests" (`MTL4RenderPass.h:96`); VK occlusion queries and `GL_SAMPLES_PASSED`
  count samples. On a 4x MSAA surface VK/GL therefore report about four times the Metal value,
  and RSX ZPASS is a pixel count, so Metal is the closer match. This is a VK/GL over-count on
  MSAA, not a Metal defect. Measurement if it ever matters: one 4x MSAA pass over a known
  coverage, compared across all three backends.
- **Segment summation.** VK must sum one `VkQuery` per begin/end segment
  (`VKGSRender.cpp:2969-2977`); Metal's single counter accumulates the segments in hardware,
  so `get_occlusion_query_result` assigning rather than `+=` is correct
  (`MTLZCULL.cpp:260-261`).
- **Host read coherence.** The buffer is `StorageModeShared` (`MTLResourceManager.cpp:12-15`)
  and persistently resident (`:349-351`), so a read after commit feedback needs no
  `synchronizeResource`. The missing piece is the wait (Q1), not a flush.
- **Barrier visibility scope.** Every barrier uses `MTL4::VisibilityOptionDevice`
  (`MTLBarrier.h:18`), which is the GPU coherence point. KosmicKrisp passes
  `MTL4VisibilityOptionResourceAlias` on every barrier (`bridge/mtl_encoder.m:28-34`), but its
  own TODO says that is only needed when occlusion queries are in use and it is the *aliased
  virtual address* option (`MTL4CommandEncoder.h:28-32`). This backend does not alias the
  counter buffer, so `Device` is the correct scope. Not adopted.
- **Pass coverage.** Only `framebuffer::open_pass` attaches the visibility buffer
  (`MTLFramebuffer.cpp:136`). The clear/`fill_pass` descriptor (`MTLSurface.cpp:362`), the
  depth-resolve pass (`:459`), the output pass (`MTLPresent.cpp:234`) and the overlay encoders
  (`MTLOverlays.cpp:312`) do not, so clears, resolves, the presenter and the OSD cannot inflate
  a count.
- **Threading.** All five hooks, `apply` and `on_slot_complete` run on the RSX thread
  (`RSXZCULL.cpp:52/59/140/150/276`, `RSXThread.cpp:955`, `MTLGSRender.cpp:738`, `:2658`), so
  the lock-free state in `MTLZCULL.h` is sound. The commit feedback thread only touches the
  recorder's slot state.
- **Framebuffer-bind invalidation.** `check_zcull_status(true)` on a bind (`MTLGSRender.cpp:826`)
  ends and restarts the query through the shared path, which is what
  `VKGSRender.cpp:2851`/`GLRenderTargets.cpp:321` require. Not a deviation.
- **Report-page protection.** `on_access_violation` falls through to the base implementation
  (`MTLGSRender.cpp:3458`), so protected report pages work as in VK/GL.
- **`should_skip_draw` and the predicate slot.** Covered by `DR8`; the CPU skip path means a
  skipped draw never reaches `apply`, which is the correct behaviour (a skipped draw must not
  count). `on_draw` still increments `num_draws` (`RSXThread.cpp:993`, `RSXZCULL.cpp:323-330`)
  but VK has the same property, so the `num_draws`-only query is not a Metal difference.
- **DEVIATIONS-SYNC §4, "ZCULL counters".** That note ("no new defect") is superseded: the
  generation mechanism itself is sound, but Q1-Q4 are defects in how it is driven.

## 5. Missing settings and features

- **No hardware boolean occlusion mode.** `precise_zpass_count` has no effect on the Metal
  visibility mode (Q5); the shared `write` path still honours it for the reported value (A43),
  which is why the setting looks "ignored" and is not.
- **No GPU timestamps.** No counter heap, so there is no per-pass GPU time and no way to
  attribute a frame to a pass without a capture (Q7). Nothing in the guest needs it.
- **No hardware conditional render** (`MTLDevice.cpp:313`). With `DR8` this is a deliberate
  bring-up choice, but it converts every conditional render into the forced read of Q1, so the
  two rows must be judged together. There is no setting to fall back to a correct slow path,
  because the only path is the slow one.
- **No query-availability signal.** The shared code has no "not ready" answer for a forced read
  (`RSXZCULL.cpp:616-625` asserts that a forced read succeeds), so the backend has no way to
  defer; it must wait internally. KK solves this with an availability word written on the GPU
  (K2). This is the structural gap behind Q1.
- **No tunable query latency.** The three-frame gate is fixed by `frame_slot_count`
  (`MTLDevice.h:21`) and the slot-reuse point; there is no setting or capability that trades
  latency for safety, and Q4 shows the safety is not needed once feedback is used.
- **No pool sizing or exhaustion policy.** The pool is fixed at `occlusion_query_count`
  (`MTLZCULL.h:37-38`), never grows, and degrades to the -1 sentinel (Q6). No log, no setting.
- **No query-path test.** `rpcs3/tests` has no ZCULL/occlusion coverage (grep), so none of
  Q1-Q6 is pinned by a test. A unit test over the free list, the deferred list and the
  generation gate would catch Q3 and Q6 without a GPU.
- **ZCULL statistics remain stubbed in shared code** (`RSXZCULL.cpp:358-367`), so only ZPASS
  reaches any backend. That is not Metal's gap and is not counted here, but it bounds what a
  "query" audit can cover.

## 6. KosmicKrisp techniques

| id | KK file:line | what KK does | valid for RSX? | why | how we would apply it |
|---|---|---|---|---|---|
| K1 | `kk_query_pool.c:423-429` | Maps `VK_QUERY_CONTROL_PRECISE_BIT` to `COUNTING` and its absence to `BOOLEAN`. | Yes | RSX has the same precise/thrifty split, and `precise_zpass_count` is exactly that bit. | Adopt. Choose the mode from the setting. This is the fix for Q5. |
| K2 | `kk_query_pool.c:431-457`, `:459-493` | `EndQuery` writes an availability word on the GPU; the CPU polls availability with a 2 s timeout and a device-status check instead of reading blind. | Yes, with a condition | The rule ("do not read a value the GPU has not published") is exactly Q1. RSX has no query API, so the availability word is internal only. | Adopt the mechanism, not the constant polling: one availability word per counter, written by a small compute pass after the query's last draw, and wait on it only for a forced read. |
| K3 | `kk_query_pool.c:105-128`, `:252-271`; `kk_query.c` reset kernels | Resets queries with a compute dispatch in stream order; the CPU never zeroes a live counter. | Partial | The backend's free-list gate already guarantees the index is idle, so the CPU store at begin is safe in the common case. It is *not* safe for the abandoned slot (Q3), where the GPU may still write the counter. | Use the GPU-side clear only for the quarantine path: zero the counter with one dispatch at the next begin when the index came back from an abandoned slot. |
| K4 | `kk_query_pool.c:165-197`; `kk_query_table.c:96-150` | One global occlusion heap shared by every pool, with a 16-bit remap table and an in-use bitset; add/remove are serialized. | Partial | RSX has a single fixed 2048-slot pool, so the remap table buys nothing. The in-use bitset is still worth having. | Reject the remap layer. Adopt the `in_use` check as a debug assertion in `begin`/`release_counter` to catch a double begin or a double free. |
| K5 | `kk_cmd_draw.c:1435-1437`; `kk_cmd_meta.c:110-117`, `:78-82` | Re-emits the visibility mode when the occlusion state is dirty, and forces `DISABLED` for meta passes while saving/restoring the outer mode. | Yes | Dynamic state is dirty per encoder, not keyed on an object address, which is precisely what Q2 needs; meta passes must never count. | Adopt. Invalidate on pass open / meta pass, or simply always emit. |
| K6 | `kk_cmd_draw.c:432-435`; `bridge/mtl_encoder.m:28-34` | Attaches the global visibility buffer to every render-pass descriptor and passes `MTL4VisibilityOptionResourceAlias` on every barrier. | Partial | The descriptor attachment matches this backend already. The barrier option is the *resource-alias* coherence point for aliased virtual addresses and KK's own TODO says it should be conditional. | Keep the descriptor attachment. Do not copy the blanket barrier option; keep `VisibilityOptionDevice` (see §4). |
| K7 | `kk_query_pool.c:198-207`, `:353-415`, `:567-576` | Timestamps go to an `MTL4::CounterHeap`, with the resolve deferred to a compute/blit stage and a barrier before the copy. | Yes for diagnostics, no for guest semantics | RSX has no guest timestamp query, so only the profiler benefits. | Conditional adopt for Q7: counter heap sampled at pass boundaries, resolved once per frame. No guest path. |
| K8 | `kk_query_pool.c:475-493` | Any wait is bounded and checks the device status instead of spinning forever. | Yes | This is the same rule as SY1 and the recorder's 10 s feedback timeout. | Constrain the Q1 fix with it: a forced read must time out, log, and return the last known value rather than block the RSX thread. |

## 7. Fifteen-line summary

1. The Metal query design is structurally sound: one persistent visibility buffer, accumulate
   type, one counter per live query, free list plus a deferred-release window.
2. Q1: a forced ZPASS read — every conditional-render evaluation and every guest report read —
   never submits or waits, so it returns 0 or a partial count. H, certain.
3. Q2: `apply` caches the visibility mode on a raw encoder pointer that is never invalidated, so
   a pass whose encoder reuses an address draws uncounted. H, needs a capture.
4. Q3: a timed-out slot still bumps the ZCULL generation, so a deferred counter is reused while
   the abandoned frame can write it. H, needs a timeout.
5. Q4: the gate waits for the slot's next reuse, three frames later, and one late query blocks
   the whole report queue. M, certain.
6. Q5: `precise_zpass_count` has no hardware boolean mode, unlike VK, GL and KK. M.
7. Q6: an exhausted counter pool answers "fully visible" (-1) where VK answers 0. L.
8. Q7: no counter heap, so no per-pass GPU time; only commit feedback exists. L.
9. Accumulate across passes and frames is the documented contract and the right choice.
10. The counter layout, alignment, driver-handle mapping and 2048-slot ceiling are correct.
11. Metal counts fragments and VK counts samples, so Metal is closer to RSX on MSAA.
12. Only draw passes carry the visibility buffer; clears, resolves, the presenter and overlays
    cannot pollute a count.
13. The barrier visibility scope (`Device`) is correct; KK's blanket `ResourceAlias` is for
    aliased addresses and is rejected.
14. No query test exists; a free-list/deferred/gate unit test would pin Q3 and Q6 without a GPU.
15. Fix Q1 and Q3 together, invalidate Q2's cache, and move Q4's gate to commit feedback.

## Resolution (2026-10-20)

Q1–Q6 are fixed; the code is `MTLZCULL.{h,cpp}`, `MTLCommandRecorder.{h,cpp}` and
`MTLGSRender.cpp`.

- Q1: `get_occlusion_query_result()` calls the bounded `flush_for_readback()` when the slot has
  not reported, so a forced read sees the count of the frame that produced it.
- Q2: the visibility-mode encoder-pointer cache is gone; the mode is emitted per pass.
- Q3: `begin_frame()` returns `reported`; a timed-out slot calls `on_slot_complete(slot, false)`,
  which skips the generation bump and the deferred release.
- Q4: a per-slot delivered-commit counter gates `completed_commits(end_slot) > end_commit`, with
  the slot generation as the fallback.
- Q5: the boolean mode is selected when `precise_zpass_count` is off.
- Q6: an exhausted pool reports 0 and logs once, as VK and GL do.
- Q7 stays open; it is diagnostic only.

Build and tests: 143/143 MTL, 490/490 full.

Compliance note: this file only records what I read in `.kk-mesa`. I did not modify it, and no
text here is intended for publication upstream in Mesa. Marker: biblioklept.

<!-- doc-end -->
