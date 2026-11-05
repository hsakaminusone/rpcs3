# PRESENT-FIFO-PARITY — Metal 4 present, flip, sync, and flip completion against Vulkan and OpenGL

Owner/Scope: RSX Metal 4 backend, the present and flip path (`rpcs3/Emu/RSX/MTL`) | Status: read-only audit | Date: 2026-10-19
Branch: `metal4`. No file under `rpcs3/**` is changed. No build, no emulator run, no commit.

`file:line` is advisory, because the branch moves. Symbol names are the stable reference.

**Divergence class.** One word per row.

| Class | Meaning |
|---|---|
| stall | The Metal path waits where the reference does not, so the RSX thread stops. |
| dropped completion | The frame reaches the guest as acknowledged, but the image is not presented. |
| lock | A lock, a wait, or a GPU flush-readback appears on the flip path. |
| extra work | The Metal path does work on the flip path that the reference does not do. |
| missing call | A call that Vulkan or OpenGL makes on the flip path is absent in Metal. |
| behavioural | The same call exists, but the observable result differs. |

**Risk.** H = a title can stall, freeze, or corrupt the output. M = a title can drop frames, leak, or
mis-time. L = a bounded defect, or a defect that needs a specific configuration.

---

## 1. Method

1. Read the Metal present path end to end: `MTLGSRender::flip` (`MTLGSRender.cpp:2813`), `open_frame`
   (`:724`), `begin` (`:746`), `end` (`:1528`), `on_semaphore_acquire_wait` (`:3084`),
   `do_local_task` (`:3123`), and `bind_framebuffer` (`:768`). `[V]`
2. Read the Metal output tier: `MTLPresenter::init/acquire/update_vsync/present`
   (`MTLPresenter.cpp:38`, `:102`, `:224`, `:245`), `command_recorder::begin_frame/submit/
   flush_for_readback` (`MTLCommandRecorder.cpp:88`, `:151`, `:195`), and the layer setup in
   `MTLMetalLayer.mm:121-165`. `[V]`
3. Read the shared contract that Metal and the reference backends both call:
   `rsx::thread::flip` (`RSXThread.cpp:2788`), `rsx::thread::do_local_task` (`:1390`),
   `request_emu_flip` (`:3617`), `handle_emu_flip` (`:3647`), and `on_frame_end` (`:3469`). `[V]`
4. Read `VKGSRender::flip` (`VKPresent.cpp:426`) and `GLGSRender::flip` (`GLPresent.cpp:162`) line by
   line, plus the acquire wait and local-task overrides (`VKGSRender.cpp:1136`, `GLGSRender.cpp:1328`)
   and the swapchain acquire retry (`VKPresent.cpp:596-633`). `[V]`
5. Compare the two paths at every seam the flip crosses: the skip-frame branch, the emu-flip branch,
   the drawable/swapchain acquire, the black clear, the present call, the completion signal, the
   overlay retire, the cache frame-end, and the resolution-scaling re-sync. `[V]`
6. Record one row per divergence, with the Metal site and the reference site. Rank by expected effect
   on a real title. `[I]`

Commands were read-only (`grep`, file reads, `git status`). No build and no emulator run.

---

## 2. Present-path summary

### 2.1 Metal 4

- `begin()` calls `open_frame()`. `open_frame` calls `recorder.begin_frame()`, which waits for the
  commit feedback of the ring slot before it resets the allocator. It then sets `m_frame_open = true`
  (`MTLGSRender.cpp:724-744`).
- Draws encode into the slot's command buffer while the frame is open.
- A guest flip reaches `handle_emu_flip` (`RSXThread.cpp:3647`). That function applies the frame
  limit and the vsync gate, then calls `MTLGSRender::flip` (`MTLGSRender.cpp:2813`).
- `flip` presents in one place: `presenter::present` (`MTLPresenter.cpp:245`) calls
  `recorder.submit()` and then `drawable->present()`. The compose pass reads the display-buffer
  surface and writes the drawable (`MTLGSRender.cpp:2998-3039`).
- `GSRender::flip` calls `m_frame->flip` for the host window bookkeeping (`GSRender.cpp:55`).
  `rsx::thread::flip` clears the display interrupt and the flip request (`RSXThread.cpp:2788-2816`).
- A mid-frame semaphore wait calls `on_semaphore_acquire_wait` (`MTLGSRender.cpp:3084`). That path
  can submit the frame and clear `m_frame_open` before the flip arrives (`:3112-3116`).

### 2.2 Vulkan

- `flip` re-initialises the swapchain when needed, handles skip-frame and `swapchain_unavailable`
  with a mini-flip, then computes the present source (`VKPresent.cpp:426-511`).
- On an emu flip it calls `evaluate_cpu_usage_reduction_limits` (`VKPresent.cpp:586-589`).
- It acquires a swapchain image in a bounded retry loop that never returns without an image
  (`VKPresent.cpp:595-636`).
- It clears black when the source is missing or the aspect has a border (`VKPresent.cpp:781-802`),
  then queues the swap request and calls `m_frame->flip` and `rsx::thread::flip`
  (`VKPresent.cpp:983-988`).
- After the flip it re-syncs the resolution-scaling configuration (`VKPresent.cpp:991-1016`).
- `frame_context_cleanup` waits `FRAME_PRESENT_TIMEOUT` for the queued swap
  (`VKPresent.cpp:245-254`). The texture cache frame-end runs in `advance_queued_frames`
  (`VKPresent.cpp:197`).
- `on_semaphore_acquire_wait` runs the local task when a flush is pending, when a guest flip is
  requested, or when the queue is deadlocked (`VKGSRender.cpp:1136-1144`).

### 2.3 OpenGL

- `flip` returns early on `info.skip_frame` (`GLPresent.cpp:164-169`).
- On an emu flip it calls `evaluate_cpu_usage_reduction_limits` and `update_swap_interval`
  (`GLPresent.cpp:253-257`).
- It clears black when the source is missing or the aspect has a border (`GLPresent.cpp:276-281`),
  and retires the dirty overlays (`GLPresent.cpp:283-298`).
- It blits or scales into the default framebuffer, then calls `m_frame->flip` and
  `rsx::thread::flip` (`GLPresent.cpp:518-519`).
- After the flip it re-syncs the resolution-scaling configuration and runs the cache frame-end
  (`GLPresent.cpp:522-535`). `SwapBuffers` blocks the RSX thread.
- `on_semaphore_acquire_wait` runs the local task when the work queue is not empty or when a guest
  flip is requested (`GLGSRender.cpp:1328-1335`).

---

## 3. Gap register

Rows are ordered by expected impact on a real title.

| id | gap | Metal site (file:line) | Vulkan/GL reference (file:line) | divergence class | risk | proposed fix |
|---|---|---|---|---|---|---|
| PF1 | A flip that arrives while the frame is closed returns without a present. The guard `!m_frame_resources or !m_frame_open` calls the base flip and the shared flip, then returns, so the display buffer is not composed and the drawable is not presented. The completion is still signalled, so the guest does not hang; the screen keeps the previous image. | `MTLGSRender.cpp:2817-2824` | `VKPresent.cpp:495-511`, `VKPresent.cpp:983-988`, `GLPresent.cpp:164-169`, `GLPresent.cpp:518-519` | dropped completion | H | Reopen the frame (`open_frame`) and present the display buffer when the guard finds `m_frame_open == false` but a valid resource set exists, as Vulkan re-queues and presents. Reserve the bare guard return for a missing resource set. |
| PF2 | `on_semaphore_acquire_wait` submits the frame and clears `m_frame_open` in the middle of a frame. The next guest flip then takes the PF1 guard, so the frame is never composed. Vulkan and OpenGL keep the frame open and service the wait without closing the frame. | `MTLGSRender.cpp:3112-3116` | `VKGSRender.cpp:1136-1144`, `GLGSRender.cpp:1328-1335` | extra work | H | Do not call `recorder.submit()` and clear `m_frame_open` on this path. Service the wait the way the reference does: drain the flush queue and return, and leave the frame open for its flip. |
| PF3 | `command_recorder::begin_frame` waits on the commit-feedback condition with no timeout. A slot whose GPU feedback never arrives stops the RSX thread inside `open_frame`. Vulkan bounds the equivalent wait with `FRAME_PRESENT_TIMEOUT`. | `MTLCommandRecorder.cpp:98-107` | `VKPresent.cpp:245-254` | stall | H | Give the wait a timeout, or add a watchdog that logs and breaks the wait. A drawable pool that ran dry and a GPU fault must not stop the RSX thread. |
| PF4 | The PF1 guard returns before `retire_overlays` and `textures.on_frame_end`. The overlay temporary images and the texture-cache frame state are not retired. Vulkan and OpenGL always retire the overlays and end the cache frame. | `MTLGSRender.cpp:2817-2824` vs `MTLGSRender.cpp:3054-3064` | `VKPresent.cpp:256-274`, `GLPresent.cpp:283-298`, `GLPresent.cpp:535`, `VKPresent.cpp:197` | missing call | M | Run `retire_overlays` and `textures.on_frame_end` on the guard path too, or move both into a single frame-end helper that every exit from `flip` calls. |
| PF5 | `on_semaphore_acquire_wait` tests only the flush queue. It ignores `flip_request::emu_requested`. Vulkan and OpenGL run the local task when a guest flip is requested, so the flip is serviced at the wait. Metal services it later, on the main-loop interrupt check. | `MTLGSRender.cpp:3104-3110` | `VKGSRender.cpp:1138-1141`, `GLGSRender.cpp:1330-1331` | missing call | M | Add `(async_flip_requested & flip_request::emu_requested)` to the local-task condition, so the flip latency at a guest spin matches the reference. |
| PF6 | `flip` never calls `evaluate_cpu_usage_reduction_limits` on an emu flip. Vulkan and OpenGL call it on every emu flip, so the CPU-usage reduction limits react to the present cadence. | `MTLGSRender.cpp:2813` | `VKPresent.cpp:586-589`, `GLPresent.cpp:253-257` | missing call | M | Call `evaluate_cpu_usage_reduction_limits()` from `flip` when `info.emu_flip` is set, before the acquire. |
| PF7 | `bind_framebuffer` does not call `check_zcull_status(true)`. Vulkan calls it after the framebuffer bind, and OpenGL calls it from the render-target bind. The Metal zcull subsystem is real (`MTLZCULL.cpp`), so its surface-active flag is not updated on a framebuffer swap. | `MTLGSRender.cpp:768-795` | `VKGSRender.cpp:2851`, `GLRenderTargets.cpp:321` | missing call | M | Call `check_zcull_status(true)` at the end of `bind_framebuffer`, after the layout is committed. |
| PF8 | A nil drawable or a drawable with no texture drops the frame. `presenter::acquire` returns false, so `flip` skips the compose and presents nothing. Vulkan retries the acquire in a bounded loop and never returns without an image. | `MTLPresenter.cpp:178-184`, `MTLPresenter.cpp:212-219` | `VKPresent.cpp:596-633` | dropped completion | M | Treat a nil drawable as a retryable event: retry the acquire a bounded number of times, or defer the present to the next flip instead of returning with no present. |
| PF9 | `flip` never re-syncs `resolution_scaling_config` from the video configuration, and never tells the surface store that the configuration changed. Vulkan and OpenGL re-sync after the flip and call `sync_scaling_config`. Metal latches the configuration once at thread start. | `MTLGSRender.cpp:3071-3078` | `VKPresent.cpp:991-1016`, `GLPresent.cpp:522-532` | missing call | L | Rebuild the active scaling configuration after the flip, compare it with `resolution_scaling_config`, and apply the same re-sync the reference uses when the two differ. |
| PF10 | `presenter::acquire` calls `update_vsync` on every acquire, not only on an emu flip. Vulkan re-reads the vsync mode at flip and re-initialises the swapchain on a change; OpenGL calls `update_swap_interval` only on an emu flip. | `MTLPresenter.cpp:129`, `MTLPresenter.cpp:224-243` | `VKPresent.cpp:438-441`, `GLPresent.cpp:253-257` | extra work | L | Move the `update_vsync` call behind the emu-flip branch, so the interval is re-read at the same cadence as the reference. |
| PF11 | An unresolved present source presents nothing and does not clear the drawable. The compose is skipped when `source.image` is null, and no black clear replaces it. Vulkan and OpenGL clear the window to black when the source is missing. | `MTLGSRender.cpp:2998-2999`, `MTLGSRender.cpp:3051` | `VKPresent.cpp:781-802`, `GLPresent.cpp:276-281` | behavioural | L | When the source is missing but a drawable was acquired, encode a clear-to-black pass before the present, so a missing source does not leave a stale image on screen. |
| PF12 | A diagnostic read-back runs inside `flip` for the first presents when `MTL_DEBUG_LOGS` is defined. It calls `flush_for_readback`, which commits and waits on the GPU in the middle of the present path. Vulkan and OpenGL read back only for a user-requested screenshot or a recording. | `MTLGSRender.cpp:2927-2930`, `MTLCommandRecorder.cpp:195-230` | `VKPresent.cpp:757-768`, `GLPresent.cpp:373-377` | extra work | L | Keep the diagnostic read-back, because it is debug-gated, but move it off the present path, or count it as a stall in the present diagnostic so it cannot be mistaken for a present stall. |

---

## 4. Ranked risks

### Risk H

1. **PF3 — the unbounded commit-feedback wait.** This is the only gap that stops the RSX thread
   inside `open_frame`. When the wait does not return, the RSX thread stops, the FIFO does not
   advance, and the guest draw thread stops with it. The other gaps only spoil the image. A
   GPU-side fault or a starved drawable pool is enough to reach the wait.
2. **PF1 — the closed-frame flip drops the present.** The guest receives the flip completion and keeps
   running, so the fault looks like a freeze of the image while the title is still alive. This is the
   most likely on-screen symptom.
3. **PF2 — the mid-frame submit closes the frame.** This is the direct cause of PF1. Every frame with a
   mid-frame semaphore acquire wait is a candidate, and a semaphore wait is common.

### Risk M

4. **PF8 — the nil drawable drops the present.** A drop is intended when the pool is empty, but the
   current code also drops a drawable that carries no texture, and it never retries. This can drop
   several consecutive frames on a resize or an occlusion.
5. **PF4 — the guard path skips the frame-end work.** The overlay temporaries and the texture-cache
   frame state accumulate on every dropped frame. The effect grows with the run length.
6. **PF5 — the emu flip is not serviced at the acquire wait.** The flip still happens on the next
   main-loop interrupt check, so this is a latency difference, not a lost flip.
7. **PF6 — the missing CPU-usage reduction update.** The reduction limits do not react to the present
   cadence, so the pacing differs from the reference on a slow title.
8. **PF7 — the missing zcull status update.** The occlusion counters can use the wrong
   surface-active flag after a framebuffer swap, so an occlusion query can report a wrong value.

### Risk L

9. **PF9 — the missing resolution-scaling re-sync.** Only a live change to the resolution-scale
   setting is affected.
10. **PF10 — the per-acquire vsync update.** A small amount of extra work, and a possible re-read of
    the mode at a time the reference does not use.
11. **PF11 — no black clear on a missing source.** A stale image can remain after a source that does
    not resolve.
12. **PF12 — the diagnostic read-back in the present path.** Debug-gated, so it cannot affect a
    shipping build.

---

## 5. Not a gap

These items were checked and are at parity, or are intended behaviour.

- **The present is asynchronous in a different place, not in a different way.** Metal presents on the
  RSX thread right after `recorder.submit()` (`MTLPresenter.cpp:292-314`). Vulkan queues the swap and
  presents from the queue thread, and OpenGL blocks the RSX thread in `SwapBuffers`. The three reach
  the same result.
- **The per-slot commit feedback is equivalent to the HLD-06 shared-event design.** The recorder
  signals after the commit and the slot returns only when the GPU work completed
  (`MTLCommandRecorder.cpp:151-193`). This matches the effect HLD-06 requires for the slot return.
- **The guard path still acknowledges the flip.** `GSRender::flip` and `rsx::thread::flip` both run on
  the guard path (`MTLGSRender.cpp:2821-2822`), so the display interrupt clears and
  `request_emu_flip` returns. PF1 is a lost present, not a lost completion.
- **The drawable acquire is bounded on purpose.** `allowsNextDrawableTimeout = YES` and a nil
  drawable is a drop, not a block (`MTLMetalLayer.mm:135`, `MTLPresenter.cpp:178-184`). This is the
  intended pacing, and it is not the stall of PF3.
- **`skip_frame` is honoured.** The source block is gated by `!info.skip_frame`
  (`MTLGSRender.cpp:2870`), so a skipped frame presents nothing, as in OpenGL.
- **The black letterbox clear matches.** The drawable pass uses `LoadActionClear` with opaque black
  (`MTLGSRender.cpp:3013-3015`), as Vulkan and OpenGL clear the border area.
- **The overlay lock matches.** `frame_output::compose` holds the overlay-manager lock while it walks
  the visible list (`MTLPresent.cpp:98-111`), as the reference paths do.
- **`m_frame->flip` is host-window bookkeeping.** `GSRender::flip` calls it, and both Vulkan and Metal
  call `GSRender::flip` (`GSRender.cpp:55`, `VKPresent.cpp:987`, `MTLGSRender.cpp:3071`).
- **The drawable ring and the frame ring agree.** `maximumDrawableCount = 3`
  (`MTLMetalLayer.mm:133`) equals `frame_slot_count = 3` (`MTLDevice.h:21`), so the ring cannot
  outrun the pool.
- **The flip request, the frame limit, and the vsync gate are shared code.** `request_emu_flip`,
  `handle_emu_flip`, and the frame-limit maths live in `rsx::thread`
  (`RSXThread.cpp:3617`, `:3647`), so the Metal flip timing and the guest flip cadence match the
  reference backends by construction.
- **The vsync modes themselves match.** `presenter::update_vsync` maps the three guest modes the same
  way Vulkan and OpenGL do (`MTLPresenter.cpp:224-243`). Only the call site differs, which is PF10.
- **The occlusion counter subsystem is implemented.** `MTLZCULL.cpp` allocates real visibility
  counters, so PF7 is a missing update call and not a stub.

<!-- doc-end -->
