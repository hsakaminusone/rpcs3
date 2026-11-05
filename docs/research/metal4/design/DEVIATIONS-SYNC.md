# DEVIATIONS-SYNC — synchronization, resource lifetime and command recording

Scope: the RSX Metal 4 backend (`rpcs3/Emu/RSX/MTL`). This file audits the barrier and hazard
path, render-pass load/store and resolve actions, resource residency and heap lifetime, the
command recorder and its frame ring, the pass scheduler, argument-table staleness, and the
presenter-to-RSX handoff. It compares the Metal path with Vulkan (`rpcs3/Emu/RSX/VK`), OpenGL
(`rpcs3/Emu/RSX/GL`) and the shared code (`rpcs3/Emu/RSX/Common`). It also mines KosmicKrisp
(`.kk-mesa/src/kosmickrisp`, read-only) for Metal 4 techniques.

## 1. Method

1. I read the Metal path in full: `MTLBarrier.{h,cpp}`, `MTLResourceManager.{h,cpp}`,
   `MTLCommandRecorder.{h,cpp}`, `MTLPassScheduler.{h,cpp}`, `MTLArgumentBinds.{h,cpp}`,
   `MTLFramebuffer.{h,cpp}`, `MTLSurface.{h,cpp}`, `MTLTextureCache.{h,cpp}`,
   `MTLConstants.{h,cpp}`, `MTLZCULL.{h,cpp}`, `MTLPresent.{h,cpp}`,
   `MTLPresenter.{h,cpp}`, `MTLDevice.{h,cpp}`, and `MTLGSRender.cpp`
   (`open_frame`/`begin`, `load_texture_env`, `record_pass_hazards`, `end`,
   `get_present_source`, `flip`, `on_semaphore_acquire_wait`, `do_local_task`).
2. I read the reference path: `VKPresent.cpp`, `VKGSRender.cpp`, `VKGSRenderTypes.hpp`,
   `VKTextureCache.cpp`, `VKDraw.cpp`, `VKRenderPass.cpp`, `VKRenderTargets.h`,
   `GLGSRender.cpp`, `GLTextureCache.h`, `GLRenderTargets.cpp`, `glutils/sync.hpp`.
3. I read KosmicKrisp as a Metal API reference only: `kk_bo.c`, `kk_queue.c`,
   `kk_device.c`, `kk_cmd_buffer.c`, `kk_sync.c`, `kk_wsi.c`, `kk_device_memory.c`,
   `kk_cmd_pool.c`, `kk_image_layout.c`.
4. I did not build, run, commit or push. I changed one file: this one. I did not edit
   `.kk-mesa`, `filesdump/`, Vulkan code or OpenGL code.
5. Mesa policy note: I only read `.kk-mesa`. I did not edit it, and I did not produce Mesa
   prose or Mesa commit messages. Marker: biblioklept.
6. Confirmation rule:
   - `Yes` = the difference is certain from the code.
   - `Partial` = the code path is certain, but the visible effect needs a run.
   - `No` = the claim needs an on-device measurement; it is a hypothesis, not a fact.
7. Rows already marked `fixed` in `METAL4-TODO.md` are not repeated. `DR10` (pass per clause)
   and `PF3` (bounded `begin_frame`) are treated as prior work. This file adds only the
   synchronization, lifetime and recording defects that are not on that list.

## 2. Findings table

| id | deviation | Metal file:line | VK/GL reference file:line | class | severity | confirmed? | proposed fix |
|---|---|---|---|---|---|---|---|
| SY1 | The texture-cache readback waits for the commit with the recorder timeout. `flush_for_readback` closes the encoder, commits the frame, then waits on the slot's completion event. A signal that never arrives still stalls the guest readback path for the bound. | `MTLCommandRecorder.cpp` `flush_for_readback`, `await_value`; caller `MTLTextureCache.cpp:246-261` (flush at `:257`); also `MTLGSRender.cpp:1236` | `VKTextureCache.cpp:1472-1473` (`wait_for_fence(..., GENERAL_WAIT_TIMEOUT)`); `GLTextureCache.h:431-438` and `glutils/sync.hpp:79-105` (poll `glClientWaitSync(..., 0)` in a loop) | missing feature | H | Yes | Fixed: the wait is bounded by `recorder_feedback_timeout`. On timeout, log, do not reset the allocator, and take fresh slot objects plus a new generation. Never reuse the command buffer whose work is unfinished. |
| SY2 | On a feedback timeout, `begin_frame` still calls `on_commit_complete` for the slot. That frees the slot's deferred allocations and returns their heap spans, while the abandoned command buffer may still be running. | `MTLCommandRecorder.cpp:125-142` (timeout at `:125-137`, unconditional release at `:142`); `MTLResourceManager.cpp:341-352`, `:406-421` (free span and release) | `VKPresent.cpp:245-254` (a timed-out frame marks the swapchain unavailable; it does not recycle the command buffer's resources); `VKGSRenderTypes.hpp:68-77`, `:110-129` (a pending command buffer is not reset without a successful wait) | wrong result | H | Yes | Release the deferred list only when `reported` is true. Quarantine a timed-out slot's list until a real feedback arrives, or until shutdown with the ring idle. |
| SY3 | The timeout path rebuilds a slot without releasing the old objects. `allocate_slot` overwrites four Metal objects. The shutdown path has the same root: it waits five seconds for the drain and then releases the allocators and command buffers even when the ring is not idle. | `MTLCommandRecorder.cpp:43-63` (overwrite), `:134` (rebuild), `:65-97` (drain at `:73-83`, release at `:86-92`) | `VKGSRenderTypes.hpp:110-129` (a pending command buffer and its resources outlive a failed wait); `VKPresent.cpp:250-276` (cleanup does not reset a pending command buffer) | diagnostic | M | Yes | Release the old slot objects before rebuilding. On shutdown, if the drain times out, keep the objects alive or leak them deliberately; do not release an allocator with work in flight. |
| SY4 | The argument binder never writes a zero value. A texture or buffer slot that was bound in an earlier draw and is unbound in a later draw keeps the earlier entry. The table is re-bound every draw, but only changed non-zero slots are written. | `MTLArgumentBinds.cpp:43-59` (`address`/`stream`), `:68-80` (`unit`), `MTLArgumentBinds.h:174-179` (unused slots may stay unbound); re-bind at `MTLGSRender.cpp:2581-2582` | `VKDraw.cpp:727`, `:758`, `:781` (Vulkan writes a null image view and a null sampler for an unbound unit every draw) | wrong result | M | Partial | On a non-zero-to-zero transition, write a shared null texture and null sampler. Keep the per-stage cached set in sync. If Metal accepts a null `ResourceID`, write it; otherwise use the 1x1 null image that Vulkan uses. |
| SY5 | The persistent residency set has no removal path. `make_persistent` adds and commits, but `release_list` only frees the heap span and releases the object. The set therefore retains every published allocation forever. Present-scratch and capture resizes leave the old texture in the set. | `MTLResourceManager.h:64-78` (only add and query); `MTLResourceManager.cpp:206-216` (add + commit), `:380-394` (defer), `:406-421` (free + release); users at `MTLGSRender.cpp:2816-2836`, `MTLPresent.cpp:190-222` | KosmicKrisp `kk_bo.c:82-89` and `kk_device.c:438-459` (remove on destroy); the backend's own contract at `MTLResourceManager.h:59-63` ("removed only when it is released") | performance | M | Yes | Add `residency_manager::remove(MTL::Allocation*)` (`removeAllocation`, `commit`, erase `m_published`) and call it from `release_list` for published allocations. Remove each heap before releasing it at shutdown. |
| SY6 | Dedicated placement heaps are never destroyed, and the pool never shrinks. A request over half a block gets its own heap; `free` returns the span but keeps the heap in `m_blocks` until shutdown. | `MTLResourceManager.cpp:62-77` (dedicated heap), `:127-160` (span only), `:24-33` (release only at shutdown), `:15-16` (block sizes) | `VKPresent.cpp:189-197` (`rebalance_memory_type_usage`, `vmm_check_memory_usage`, `m_rtts.trim` and `m_texture_cache.on_frame_end` every frame) | performance | L | Yes | Track free bytes per block. Release a fully free dedicated block after the frame ring can no longer reference it. At minimum, report the pool high-water mark so a leak is visible. |
| SY7 | No hazard record survives a frame boundary. `begin_frame` clears every record, the heaps are untracked, and `begin_frame` waits only for the same slot, three frames old. Three frames can be in flight. Whether frame N+2 may write a surface frame N still reads is not proven. | `MTLPassScheduler.cpp:30-37`; `MTLResourceManager.cpp:45-46` (untracked heaps); `MTLCommandRecorder.cpp:99-123`, `:141-142` | `VKPresent.cpp:187-209`, `:245-254` (queued frames are fenced and trimmed only after completion); `VKGSRender.cpp:1282-1294`; `VKPresent.cpp:193-197` (double-buffered caches) | wrong result | H | No | Hypothesis. Test two frames that write and read the same surface with no CPU wait under a GPU capture. If the frames overlap, keep cross-frame records for surfaces that persist across frames, or wait for frame N-1 before recording frame N+1. |
| SY8 | The scheduler models only RAW and WAR. A second write to the same resource in a later pass consumes no earlier write, so no WAW edge is emitted. A render-pass `LoadActionLoad` is also not recorded as a read, and `surface::clear_memory` does not call `note_write`. | `MTLPassScheduler.cpp:49-80` (`note_write` consumes only `read_stages`), `MTLFramebuffer.cpp:148`, `:170` (load actions), `MTLSurface.cpp:319-353` (clear pass) | `VKGSRender.cpp:1511`, `:1587` (`write_barrier` on a descriptor and a bound target); `GLRenderTargets.cpp:507` (`memory_barrier`); `VKRenderPass.cpp:345-363` (explicit attachment store actions) | wrong result | H | No | Hypothesis. Add a WAW edge when `note_write` finds an earlier `write_stages` in a different pass. Record each `LoadActionLoad` attachment as a read. Measure before changing, because a render-pass boundary may already order the attachment set. |
| SY9 | The slot generation is read on the feedback thread and written on the RSX thread. It must be atomic because the two run concurrently; a plain `u32` races. | `MTLCommandRecorder.h` `frame_slot::generation`; `MTLCommandRecorder.cpp` `commit_slot`, `on_feedback` | `VKGSRenderTypes.hpp:55-77`, `:110-129` (`reset_id` and the pending state change under `guard_mutex`) | wrong result | M | Yes | Fixed: `generation` is `std::atomic<u32>`; the recorder no longer has a mutex, so the generation compare is the only cross-thread state in the feedback handler. |
| SY10 | Every MSAA clause pass resolves the color attachment. `open_pass` sets `StoreActionStoreAndMultisampleResolve` whenever a resolve surface exists. Depth stores only and resolves in its own pass. An N-clause draw pays N full color resolves, then `surface::resolve` can resolve again on a transfer read. | `MTLFramebuffer.cpp:154-158`; contrast `:166-174`; `MTLSurface.cpp:403-461` (`resolve`, gated by `require_resolve`) | KosmicKrisp `kk_cmd_buffer.c:895-922` (resolve only when the resolve mode asks), `:989-1042` (meta resolve for depth/stencil); Vulkan resolves on demand through `VKResolveHelper.cpp` | performance | M | Yes | Store color only in `open_pass`, as depth does, and resolve once in `surface::resolve` when `require_resolve` is set. The on-demand path already covers a later transfer read. |
| SY11 | Residency publication is one `commit()` per allocation and never calls `requestResidency`. KosmicKrisp adds and removes under a mutex and issues `commit` + `requestResidency` as one operation. | `MTLResourceManager.cpp:206-216` (commit at `:214`), `:54-57` (heap publish); no persistent-set `requestResidency` (only the layer set at `MTLPresenter.cpp:69`) | KosmicKrisp `kk_device.c:430-468` (`commit` + `requestResidency` under a mutex), `kk_bo.c:68`; `MTLResourceManager.h:55-63` (the zero-read failure this set exists to prevent) | performance | M | No | Batch additions per frame and commit once before the frame's command buffer commit. Call `requestResidency` after the commit, as KK does. Measure with a GPU capture before changing. |
| SY12 | The presenter waits on the drawable with an unbounded queue call. This matches Vulkan's deliberate unbounded acquire, so it is not a deviation. The handoff itself is on the right thread and the drawable is signalled on the commit path. | `MTLCommandRecorder.cpp` `set_present_drawable`, `submit`; `MTLPresenter.cpp:141-148`, `:200` | `VKPresent.cpp:591-595` ("set no timeout here so that we wait for the next image if need be"), `:596-633` (spin and service queued frames) | not a deviation | L | Yes | No change. Keep the acquire and the signal on the RSX thread. Re-check only if PF8 (nil drawable) or a window-close stall appears. |
| SY13 | The guest label release is written on the GPU, not by the CPU. `write_gcm_label` calls the backend hook and returns without a CPU store; the recorder stages the four bytes per frame slot and emits them as one compute-encoder `copyFromBuffer` at the next encoder boundary, under one queue-scope `All → Blit` barrier per batch (VK writes per label with `vkCmdUpdateBuffer`, `VKGSRender.cpp:1710-1770`). The destination is a 64 KiB no-copy import of the label's own page, created after the page exists and cached for the process, because an import is fixed at creation and the application-lifetime window predates the RSX reports mapping (L136/L137). A semaphore acquire that finds its address among the labels recorded in a command buffer commits that buffer and waits only for its slot's feedback; it then re-reads the word and takes its normal spin. A label with no acquire waiter pays no wait. A label outside the guest window takes the old drain and CPU store. | `MTLGSRender.cpp` `release_GCM_label`, `sync_label`, `on_semaphore_acquire_wait`; `MTLCommandRecorder.cpp` `write_label`/`commit_pending_labels`/`flush_label_writes`/`sync_label`/`reenter_slot`; `MTLResourceManager.cpp` `label_buffer`; `nv406e.cpp` `semaphore_acquire` | `VKGSRender.cpp:1710-1770`; `GLGSRender.cpp:1423` (`glBufferSubData` after a fence) | missing feature | H | Partial | Device run: confirm the acquire waits for the boot label, the FIFO link at `0xcb09d4` is consumed, the frame time stops tracking the drain, and no guest wait times out. Run K (2026-10-26) confirmed the hole: the boot release is committed but the acquire reads the old `0` and parks (L135). Run L (2026-10-26) confirmed the wait: the slot is committed and its feedback arrives (2 waits; re-enter max 235.7 ms), but the guest word still reads `0`; the device import predates the label page (L136), so the write now targets a per-page import (L137). The residual window is that a label not waited on becomes visible at the next commit, so a guest that resets the same label on the CPU without waiting could be overwritten; that is already a race on real hardware. **2026-10-27:** the setting is honoured again, so this path is opt-in; the default is the CPU store (`L139`). **Run T (L145) separated the label from the boot park:** the label landed, but GET had already licensed the guest to reuse A, so the link at `0xcb09d4` was next-chunk data when the parser arrived. Run U showed the live link at `0xcb09d4`, the A→B handover, and 2,892 handovers with no `Dead FIFO`; the label wait is not the remaining cause. |
| SY14 | Completion was a condition variable fed by the commit-feedback handler: every wait took a mutex and the RSX thread slept until that handler ran, so the GPU's completion point was reported through a second thread's scheduling (run L measured a 235.7 ms re-enter wait against 18.7 ms of feedback lag). | `MTLCommandRecorder.cpp` `commit_slot`, `await_value`, `slot_idle`; `MTLCommandRecorder.h` `frame_slot::completion_value`, `m_completion_event` | KosmicKrisp `kk_queue.c:230` (`signalEvent` after the commit); `bridge/mtl_sync.m:41-52` (`waitUntilSignaledValue`); metal-cpp `MTL4CommandQueue.hpp:142` | performance | M | Yes | Fixed 2026-10-27: one `MTL::SharedEvent`, a monotone value per commit, `signalEvent` after `commit`, and every wait polls `signaledValue` with the existing 10 s bound. The feedback handler keeps only the commit diagnostics and the ZCULL `completed_commits` count. |

## 3. Highest-risk findings

1. **SY2** is the only row that can corrupt memory. It needs a feedback timeout, which is
   abnormal, but the failure mode is a heap span that is reused while the GPU still reads it.
   The fix is one guard (`if (reported)`) plus a quarantine list. Do it with SY1, because a
   bounded readback wait creates exactly the timeout that SY2 mishandles.
2. **SY7 and SY8** are the rows that decide whether the whole hazard design is sound. The
   scheduler is correct inside one frame for the pairs it records. It deliberately drops all
   records at the frame boundary and never records a WAW pair. Both rows are hypotheses, not
   facts. A GPU capture with a two-frame write/read test is the cheapest way to settle them.
3. **SY4** is a silent wrong result. The stale entry is certain. The visible effect needs a
   shader that reads an unbound unit, which RSX leaves undefined, so the row is `Partial`.
   The fix is small and it mirrors `VKDraw.cpp:727`.
4. **SY5 and SY11** are not visible in one frame. They grow the residency set and its commit
   cost over a session, and SY5 leaves a released object aliasing a reused heap span. That
   aliasing is what turns SY4 from a stale read into a read of another surface.

## 4. Checked and not a deviation

- **Bounded `begin_frame`.** The 10 s wait and the generation guard are present
  (`MTLCommandRecorder.cpp`, `await_value`) and match `VKPresent.cpp:250`. `PF3` is fixed. Since
  2026-10-27 the predicate is the commit's completion event, not the feedback handler's flag
  (SY14).
- **Deferred-release slot choice.** `defer_release` queues on the open slot
  (`MTLResourceManager.cpp:380-394`). The open slot is the youngest of the three, so its next
  completion is after every frame that was in flight at release time. This is conservative and
  correct while the slot is not abandoned.
- **Present handoff.** The drawable is acquired on the RSX thread, waited before encoding,
  signalled after the commit, and released after `present` (`MTLPresenter.cpp:102-215`,
  `MTLCommandRecorder.cpp:213-221`, `:263-275`). This matches KK `kk_queue.c:201-222`.
- **Drawable residency.** The layer's read-only residency set is attached and made resident
  (`MTLPresenter.cpp:64-70`), as KK does. The set is removed before the layer is detached.
- **Shutdown order.** The ring drains before the presenter's residency set is dropped
  (`MTLGSRender.cpp:693-699`), so the `PF3` drain can still receive feedback.
- **ZCULL counters.** The visibility buffer is persistent, the slot generation gates a query
  until its frame completes, and a counted-but-ungated query is deferred
  (`MTLZCULL.cpp:89-110`, `on_slot_complete`; `:208-246`, `end_occlusion_query`). No new
  defect.
- **Flush queue and readback.** `do_local_task` drains requests on the RSX thread and the
  `map_synchronized` flush is the only readback commit (`MTLGSRender.cpp:3212-3260`,
  `MTLTextureCache.cpp:246-261`). The thread placement matches `GLGSRender.cpp:1329-1350`.
- **Pipeline compiler and archive.** The on-disk pipeline archive is loaded at init and
  flushed at exit (`MTLGSRender.cpp:656-679`, `:717`). The first-use build is still
  synchronous, which is `DR9`/`S5` in `METAL4-TODO.md`; this file does not re-report it.
- **Allocator hardening (no row).** `heap_pool::allocate` can insert a zero-length free span
  when `padding > 0` and the remaining size is zero (`MTLResourceManager.cpp:90-96`). The span
  is harmless because every later request needs a non-zero size, but it is noise. Add an
  assertion for a zero-length insert, and for a double free, without changing behaviour.

## 5. Missing features and settings

- The persistent residency set has no removal API (SY5) and no `requestResidency` handshake
  (SY11). These are the two residency features KK has that this backend lacks. Both are
  backend-local; no shared setting gates them.
- `host_label_synchronization` is honoured (`MTLDevice.cpp` `fill_backend_config`). Off is the
  default and VK's default: the release stores the word on the CPU (`nv47_sync.hpp:83`) and the FIFO
  reader never waits. On is the GPU write with the per-page import; its acquire now waits on the
  commit's completion event (SY14), not on a feedback condition variable. The cost of the CPU path
  is that the guest can see a release before the GPU has finished with the range it releases, which
  is why the setting is opt-in.
- `shader_compiler_threads_count` (`F6`) and the synchronous first-use compile (`DR9`/`S5`)
  remain the only compiler-side gaps. The archive path itself is complete.
- There is no cross-frame hazard tracking (SY7) and no WAW/load edge (SY8). No setting turns
  either on. If the tests confirm a problem, the fix belongs in `pass_scheduler` and costs one
  frame of overlap in the worst case.
- `PF8` (nil drawable drops a frame) and `PF9` (resolution-scaling re-sync) remain open.
  They are present-path rows, not sync/lifetime rows, and are not repeated here.

## 6. KosmicKrisp techniques

| id | KK file:line | what KK does | valid for RSX? | why | how we would apply it |
|---|---|---|---|---|---|
| K1 | `kk_bo.c:82-89`; `kk_device.c:438-459` | Removes a heap or buffer from the residency set when the BO is destroyed. Add and remove are serialized by the set mutex. | Yes | An RSX resource also has a definite release point, and a Metal residency set does not drop an allocation when the application releases it. | Adopt. Add `residency_manager::remove`, call it from `release_list`, and remove heaps in `heap_pool::shutdown`. Fixes SY5. |
| K2 | `kk_cmd_buffer.c:895-922`, `:989-1042` | Picks the store action from the resolve mode. Uses `STORE_AND_MULTISAMPLE_RESOLVE` only when a render-pass resolve is requested, and falls back to a meta resolve for depth/stencil. | Yes | RSX resolves only when a transfer reads the surface or the MSAA state requires it. Resolving every clause is not an RSX rule. | Adopt. Store color only in `open_pass`, resolve once in `surface::resolve`. Fixes SY10. |
| K3 | `kk_queue.c:93-101`, `:201-222` | Builds one commit-options object per commit, waits for the drawable before the commit, signals it after, and releases it after the signal. | Yes | The Metal 4 contract is the same. The backend already follows it. | Already adopted at `MTLCommandRecorder.cpp:320-346`, `:213-221`, `:263-275`. No action. |
| K4 | `kk_queue.c:26-41`, `:74-75` | Counts `commits_in_flight` and resets the command pool only on the last completion. | Partial | The rule ("do not reset while pending") is valid. KK's global counter is coarser than the per-slot feedback ring. | Keep the per-slot ring. Use KK's rule as the reference for SY2 and SY3: a pending buffer is never reset or released. |
| K5 | `kk_device.c:430-468` | Adds and removes residency allocations under a mutex and calls `commit` + `requestResidency` as one operation. | Yes | The persistent set is single-threaded today, but the commit/request handshake is the documented residency contract. | Adopt the batched commit and `requestResidency`. The mutex is optional while the set stays on the RSX thread. Relates to SY11. |
| K6 | `kk_sync.c:13-25`; `kk_queue.c:226-231` | Emulates Vulkan timeline semaphores with `MTLSharedEvent` and signals them after the commit. | No, Vulkan only | RSX has no timeline semaphores. Commit feedback is the native Metal 4 completion signal and it already carries the slot generation. | Reject. Keep commit feedback. |
| K7 | `kk_wsi.c:51-68`, `:85-91` | Binds the drawable's texture into the swapchain image and carries the drawable in the command buffer until the present. | Yes | The backend acquires the drawable and signals it on the same queue. | Already adopted at `MTLPresenter.cpp:102-215` and `MTLCommandRecorder.cpp:213-221`. No action. |

## 7. Fifteen-line summary

1. The Metal 4 sync path is sound inside one frame for RAW and WAR pairs.
2. SY1: the texture readback wait has no timeout, so a lost feedback freezes the RSX thread.
3. SY2: a feedback timeout frees deferred heap spans while the abandoned command buffer may run.
4. SY3: the timeout rebuilds a slot without releasing the old objects; shutdown can release live ones.
5. SY4: a texture unit that goes unbound keeps its previous binding; Vulkan binds a null image.
6. SY5: the persistent residency set has no removal, so present and capture resizes leak entries.
7. SY6: dedicated heaps are never destroyed; the pool keeps its high-water mark until exit.
8. SY7: no hazard record survives a frame boundary, and three frames can be in flight (hypothesis).
9. SY8: the scheduler has no WAW edge and no read edge for a render-pass load (hypothesis).
10. SY9 and SY14: the slot generation is atomic, and completion is polled from the commit's event.
11. SY10: every MSAA color clause resolves, then the on-demand resolve can run again.
12. SY11: residency commits once per allocation and never calls `requestResidency`.
13. SY12: the unbounded drawable wait matches Vulkan and is not a deviation.
14. The drawable handoff, deferred-release slot choice, shutdown order and ZCULL gate are correct.
15. Fix SY2 with SY1, settle SY7 and SY8 with a GPU capture, and adopt K1 and K2 for SY5 and SY10.

## Resolution (2026-10-25)

SY1, SY2, SY3, SY5 and SY9 are fixed. SY4 is fixed in the binder (the null texture and sampler are
written on a zero transition) and needs a device confirmation. SY13 is the new row: the guest
label release is written on the GPU in command-stream order, so the RSX thread no longer drains
per release (L131). SY6, SY7, SY8, SY10 and SY11 remain: SY10 is the double MSAA resolve (M), SY6
is pool growth (L), and SY7/SY8/SY11 stay hypotheses until a GPU capture settles them. See
`GAP-PLAN.md`.

## Resolution (2026-10-27)

**The label path now follows the setting.** `MTLDevice::fill_backend_config` reads
`g_cfg.video.host_label_synchronization` instead of forcing `supports_host_gpu_labels` true, which
is what VK does (`VKGSRender.cpp:801`) and what GL does for its host DMA writer
(`GLGSRender.cpp:222`). The default is off, so `write_gcm_label` stores the word on the CPU
(`nv47_sync.hpp:83`) and the FIFO reader never waits for a label. The GPU path (L131/L134/L137)
runs only when the setting is on, and then the acquire still waits for the slot that carries the
word, because the parser reads the label on the CPU and would otherwise accept the pre-release
value (L135).

**The enabled path now waits on a Metal 4 completion event (SY14).** The recorder creates one
`MTL::SharedEvent` and every commit takes the next value of a monotone `u64`;
`MTL4CommandQueue::signalEvent` marks that commit's work done (metal-cpp
`Metal/MTL4CommandQueue.hpp:142`), and `MTL::SharedEvent::signaledValue` is the CPU's read of it
(`Metal/MTLEvent.hpp:71-73`). KosmicKrisp signals the same way (`kk_queue.c:230`). A slot is
complete when `signaledValue >= completion_value`, so every wait is one atomic load when the ring
is already idle and a bounded poll otherwise:

1. `begin_frame`, `reenter_slot`, `wait_for_slot`, `wait_idle` and `shutdown` all use the same
   predicate (`command_recorder::await_value`), bounded by the same 10 s as before;
2. the commit-feedback handler stays, but only for its commit diagnostics and the ZCULL query
   gate's `completed_commits` count; it no longer owns the completion state and is not on any wait
   path;
3. the mutex and the condition variable are gone. That removes a lock and a wake-up from a check
   that is almost always already true, and it removes the only place the RSX thread slept on the
   feedback thread's scheduling.

The recorder deliberately does **not** pump `on_semaphore_acquire_wait()` from inside the wait.
The FIFO loop already does that on its own spin (`nv406e.cpp:121`), and a nested
`do_local_task` → `flip()` would commit the open frame and move `m_slot` under the caller. The GPU
needs no CPU help to finish an already-committed buffer, so a plain poll is sufficient.

`MTL4CommandQueue::wait(event, value)` remains available for a later change that keeps the
following work on the GPU: it would order later commands behind the label without the CPU reading
`signaledValue` at all. That needs proof that no command after an acquire depends on the CPU-side
read, so it is not part of this change.

The gain is that a late commit feedback no longer delays the acquire (run L measured a 235.7 ms
re-enter wait against 18.7 ms of feedback lag) and the wait ends at the label, not at the slot's
recycle. The CPU-store default stays the fastest path and needs none of this.

<!-- doc-end -->
