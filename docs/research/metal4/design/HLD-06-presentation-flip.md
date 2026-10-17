# HLD-06 — Presentation & flip: the drawable-presentation layer

Owner/scope: `design/` cluster `HLD-06` (presentation / flip / swapchain-equivalent) | Status: draft | Date: 2026-10-16

> **Scope of this document.** The presentation and flip layer for a native Apple Metal 4 backend for
> RSX: how the RSX flip contract (`rsx::thread::flip`) becomes a `CAMetalLayer` drawable present; what
> replaces the `VkSwapchainKHR`; the frame-boundary/pacing model; the copy-engine and resize paths that
> touch the presented image; and the failure modes that are specific to the drawable model. It is
> **design only**: **no file under `rpcs3/**` was modified**; only this document was created.
>
> **Verification legend.** `[V]` = verified this session against the RPCS3 tree or the metal-cpp
> snapshot under `/tmp/metal4-refs/` (headers cited inline); `[I]` = inference from `[V]` facts;
> `[U]` = could not verify / needs a device spike. Difficulty scale (identical to the set): **1**
> trivial rename · **2** mechanical · **3** real work, well-trodden · **4** hard / needs design · **5**
> unknown. Every `MTL*`/`MTL4*`/`CA*` symbol below exists in the referenced header; nothing is invented.
> `file:line` citations were read on branch `metal4`; the tree is a moving branch, so **symbol names
> are the stable reference and line numbers are advisory**.

---

## 1. Purpose & non-goals

### 1.1 Purpose

Specify the Metal 4 presentation layer that `HLD-01` names component **C7 `mtl::presenter`**
(`HLD-01` §3.2) and that `HLD-04` §1.2 hands "Flip, `CAMetalLayer`, drawable presentation, frame
pacing" to. It answers five questions in one place so the frame tail cannot drift between the
scheduler, the recorder and the window:

1. **What is the RSX flip, exactly?** The FIFO flip registers, the `m_queued_flip` protocol, the
   completed-frame semaphore and the `SET_REFERENCE` word — and which of these is a *presentation*
   boundary versus a *FIFO-consumption* marker (§2.1, §2.4).
2. **What replaces the swapchain?** The `CA::MetalLayer` drawable ring, its acquisition/back-pressure,
   and the Metal 4 present sequence (§2.2, §2.3).
3. **How is the frame boundary ordered?** The `wait`→`commit`→`signalDrawable`→`present` sequence and
   the `MTLSharedEvent` frame ring that replaces the `VkFence`/`VkSemaphore` pair (§2.4, §2.5).
4. **What touches the presented image besides draws?** The NV0039/NV3089/NV308A/NV3062 copy engines at
   the frame boundary, the upscale/calibration/overlay passes, and screenshot readback (§2.6).
5. **What happens on resize, minimize, HDR and multi-display?** The window/backing-scale path with no
   `vkDeviceWaitIdle` drain, and the colorspace/EDR decision (§2.7, §2.8).

### 1.2 Non-goals

- **No source, CMake, test or `.ci` change; no build; no measurement** (matches `10` §1.4, `11`).
- **Not the barrier/hazard model.** `HLD-03` owns pass boundaries, barriers, events and the TBDR
  fragment constraint; this document only *uses* the frame-tail ordering they define (`HLD-03` §2.3
  Edge D, §3.3).
- **Not the command recording or the allocator ring.** `HLD-04` owns the `MTL4::CommandAllocator` /
  `MTL4::CommandBuffer` ring and `commit`; this document owns the drawable ring and the `present` call
  that consumes the committed buffer.
- **Not the shader/binding/format design.** `HLD-05` owns those; the overlay and calibration *passes*
  are backend-agnostic geometry (`Overlays/`) with new pipelines.
- **Not a decision to build the backend.** The strategic gate is `HLD-01` §6 Q1 / `10` §10.1.
- **Not Metal 3.** The present path here is the Metal 4 `MTL4::CommandQueue` path; the Metal 3
  `presentDrawable:` family is cited only as the contrast that no longer applies (§2.3).

---

## 2. Model — RSX semantics → Metal 4 semantics

### 2.1 The RSX flip contract

The presentation obligation is exactly the `rsx::thread` contract: `flip(const display_flip_info_t&)`
is pure-virtual (`RSXThread.h:372`) and every backend implements it; the architecture-neutral
lifecycle is `GSRender : rsx::thread` (`GSRender.h:20`), whose base `GSRender::flip` just forwards to
the window (`GSRender.cpp:55-61`). The RSX-side frame boundary is built from four moving parts:

| RSX construct | Symbol / handler | What it means | Cite |
|---|---|---|---|
| Flip head (2-word range) | `M_GCM_FLIP_HEAD` → `gcm::driver_flip` → `sys_rsx_context_attribute(..., 0x102, ...)` | the guest submitted a display buffer | `rsx_methods.cpp:96-102` `[V]` |
| Driver queue (8-word range) | `M_GCM_DRIVER_QUEUE` → `gcm::queue_flip` → `0x103` | the guest queued the next buffer; `lastQueuedBufferId` + `on_frame_end` + queue event | `rsx_methods.cpp:104-116` `[V]` |
| HLE flip command | `M_GCM_FLIP_COMMAND` → `flip_command` (`ensure(isHLE)`) | bump flip count, guest `queue_handler`, `reset()`, `on_frame_end`, `request_emu_flip`, zero the flip semaphore | `rsx_methods.cpp:40-66` `[V]` |
| Emulated flip | `request_emu_flip` → `handle_emu_flip` → `flip()` | the frame tail: pop the queued frame, apply the frame limiter, call the backend `flip()` | `RSXThread.cpp:3605`, `:3635`, `:3725` `[V]` |

The **completed-frame queue** is the `m_queued_flip` (`RSXThread.h:113`) protocol: `on_frame_end`
saves the frame stats and pushes the buffer (`RSXThread.cpp:3524-3526`); `handle_emu_flip` pops it
(`:3643`), sets `m_queued_flip.emu_flip`/`in_progress` (`:3721-3722`), calls `flip(m_queued_flip)`
(`:3725`), then sets `flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_DONE` (`:3728`). `request_emu_flip`
called off-thread coalesces into `async_flip_requested`/`async_flip_buffer` (`:3614-3621`;
`RSXThread.h:224-225`) and `flip()` clears the request (`RSXThread.cpp:2780-2791`). `[V]`

`display_flip_info_t` carries the `buffer_queue`, `buffer`, `skip_frame`, `emu_flip`, `in_progress`
and the `frame_statistics_t` (`RSXDisplay.h:77-114`). `[V]` The frame limiter and vblank coupling live
in `handle_emu_flip` (`:3650-3716`), and the emulated timing source is
`get_cached_display_refresh_rate()` (`:3572-3603`, cached from `get_display_refresh_rate()`,
`RSXThread.h:155`). `[V]`

**The flip semaphore.** The `label_addr + 0x10` word is the guest-visible flip word. It is written by
`nv406e::semaphore_release` on the flip DMA context (offset `0x10`, `CELL_GCM_CONTEXT_DMA_SEMAPHORE_R`)
where it *bypasses* the `sync_point_request` raise (`nv406e.cpp:121-125`) and maps to the presentation
hand-off, not to a draw-stream label (`LLD-sync-flip` §3.2). `flip_command` zeroes it each HLE flip
(`rsx_methods.cpp:65`). `[V]`

### 2.2 The swapchain-equivalent under Metal 4

There is **no `VkSwapchainKHR` object and nothing to recreate** (`README.md` §4 shift 3). The
swapchain becomes a `CA::MetalLayer` attached to the window's `NSView`; the presentable image is a
`CA::MetalDrawable` whose `texture()` is the render target (`CAMetalDrawable.hpp:39`). `[V]`

The existing macOS seam already returns the layer: `gs_frame::handle()` returns
`reinterpret_cast<void*>(winId())` as the `NSView` (`gs_frame.cpp:737-742`), `display_handle_t` is
`void* // NSView` (`display.h:20-21`), and the Vulkan backend's shim
`GetCAMetalLayerFromMetalView` is `return ((NSView*)view).layer;` (`metal_layer.mm:9`). `[V]` That
shim **assumes the view already has a backing `CALayer`**; whether Qt 6 guarantees one is `[U]` (§6
Q1; R22).

| VK construct | Metal 4 object / method | Shape change | Conf. |
|---|---|---|---|
| `VkSwapchainKHR` + `vkCreateSwapchainKHR` | `CA::MetalLayer` (device/pixelFormat/drawableSize) | object dissolves into a layer | `[V]` |
| `vkAcquireNextImageKHR` | `CA::MetalLayer::nextDrawable()` (`CAMetalLayer.hpp:58`) | acquire returns a drawable, not an index | `[V]` |
| swap image count `minImageCount+2` (`swapchain.cpp:274-286`) | `setMaximumDrawableCount:` (`CAMetalLayer.hpp:60-61`) | **Metal caps at 3** (R21) | `[V]` |
| `VkImage` present layout `PRESENT_SRC_KHR` | `CA::MetalDrawable::texture()` | no layout; acquire/present are the transitions | `[V]` |
| `vkQueuePresentKHR` (`swapchain.cpp:344`) | `signalDrawable` + `MTL::Drawable::present()` | present is a queue method + a drawable method | `[V]` |
| acquire/present `VkSemaphore` pair (`VKGSRenderTypes.hpp:177-178`) | `MTLSharedEvent` value + `wait(drawable)` | binary semaphores → one monotonic event | `[V]` |
| resize recreate + `vkDeviceWaitIdle` (`VKPresent.cpp:85`) | `setDrawableSize:` + in-flight coordination | no drain; a resize race (R29) | `[I]` |

`mtl::presenter` sets the layer up once from the `NSView`: `setDevice` (`:46-47`), `setPixelFormat`
(`:49-50`), `setFramebufferOnly` (`:52-53`), `setDrawableSize` (`:55-56`),
`setMaximumDrawableCount` (`:60-61`), `setDisplaySyncEnabled` (`:63-64`), `setColorspace` (`:66-67`),
`setAllowsNextDrawableTimeout` (`:69-70`), `setWantsExtendedDynamicRangeContent` (`:72-73`), and
`layer->residencySet()` (`:75`) attached with `CommandQueue::addResidencySet` (`MTL4CommandQueue.hpp:123`). `[V]`

**`maximumDrawableCount ≤ 3` is the hard constraint.** The VK backend negotiated
`minImageCount+2`/`≥3` (`swapchain.cpp:274-286`) and set `m_max_async_frames = get_swap_image_count()`
(`VKGSRender.cpp:686`; `VKPresent.cpp:112`). Metal caps the layer at three drawables (R21), so the
presenter must clamp the frame ring to `min(3, desired)` and the async depth is reduced. `[V]`

### 2.3 The present path: Metal 4 vs Metal 3

Metal 3 attached the drawable to the command buffer: `MTL::CommandBuffer::presentDrawable` (`:177`),
`presentDrawableAfterMinimumDuration` (`:178`), `presentDrawableAtTime` (`:180`). **Metal 4 has none of
these.** `MTL4::CommandBuffer` carries no drawable reference at all (no `Drawable` symbol in
`MTL4CommandBuffer.hpp`), and `MTL4::CommandQueue` exposes no `presentDrawable`; present is a
four-step dance on the queue and the drawable:

| Step | Metal 4 call | Cite |
|---|---|---|
| 1. Gate the queue on the acquired drawable | `MTL4::CommandQueue::wait(const MTL::Drawable*)` (`waitForDrawable:`) | `MTL4CommandQueue.hpp:149`, selector `:282` |
| 2. Submit the frame's command buffers | `MTL4::CommandQueue::commit(commandBuffers, count[, options])` | `MTL4CommandQueue.hpp:126-127` |
| 3. Mark the drawable as produced by the commit | `MTL4::CommandQueue::signalDrawable(const MTL::Drawable*)` | `MTL4CommandQueue.hpp:140` |
| 4. Hand the drawable to the display | `MTL::Drawable::present()` | `MTLDrawable.hpp:47` |

This sequence is the Metal 4 sample's authoritative flow and is what `HLD-03` §2.3 Edge D fixes. `[V]`
The drawable's `texture()` (`CAMetalDrawable.hpp:39`) is the colour attachment the scheduler opens the
final pass against; the recorder's commit is the same `commit` that step 2 performs, so the presenter
and `HLD-04` share one commit call. `[I]`

Pacing variants: `MTL::Drawable::presentAfterMinimumDuration(_:)` (`:48`) and `presentAtTime(_:)`
(`:50`) exist for min-duration/pinned presentation; `present()` is the plain form. `[V]` The
`MTL::Drawable::presentedTime()` accessor (`:52`) and `addPresentedHandler` (`:42-43`) are the only
presentation *feedback* Metal exposes; there is no `VkPresent`-style result code and no
`VK_SUBOPTIMAL_KHR` analogue. `[V]`

### 2.4 The frame boundary, pacing, and the `SET_REFERENCE` confusion

**The flip is the presentation boundary; `SET_REFERENCE` is not.** This must be stated once because
the topic couples them. `M_NV406E_SET_REFERENCE` (the real register; **`NV4097_SET_REFERENCE` does not
exist** — `HLD-03` §2.2, `LLD-sync-flip` §2.1) is bound to `nv406e::set_reference`, which calls
`thread::sync()` and stores the argument into `RsxDmaControl::ref` (`nv406e.cpp:14-22`). `ref` is the
guest's **FIFO-consumption reference channel** — it says "the FIFO has been read up to here", not "a
frame was presented". `thread::sync()` is a CPU-side flush and does **not** wait for the GPU
(`RSXThread.cpp:2917-2937`). `[V]`

Therefore the mapping is:

| Guest notion | Source | Metal 4 realisation | Conf. |
|---|---|---|---|
| FIFO consumed to a point | `M_NV406E_SET_REFERENCE` → `RsxDmaControl::ref` | host write into a shared `MTL::Buffer` (`mtl::label_sink`), ordered after the covering commit's feedback (`HLD-03` §2.3 Edge B) | `[I]` |
| Frame queued / completed | `on_frame_end` → `m_queued_flip.push` | frame-ring slot tagged with an `MTLSharedEvent` value | `[I]` |
| Frame presented | flip → backend `flip()` | `signalDrawable` + `present()`; completion via `presentedTime`/handler | `[I]` |
| Flip word (`label_addr + 0x10`) | `flip_command` zeroing | shared-buffer write, ordered after the presented frame's GPU work | `[I]` |

The presenter must not conflate "commit completed" (an `MTL4CommitFeedback` or a `MTLSharedEvent`
value firing) with "presented" (`presentedTime` non-zero). A frame can be GPU-complete and still
queued behind the display; the guest's flip timing depends on the latter. `[I]`

**Pacing.** The emulated frame limiter is already computed host-side in `handle_emu_flip`
(`RSXThread.cpp:3650-3716`), driven by `get_cached_display_refresh_rate()` (`:3572`) and
`frame_limit_type` (`:3651-3666`). `GSRender::get_display_refresh_rate()` returns
`m_frame->client_display_rate()` (`GSRender.cpp:63-72`), which is `QScreen::refreshRate()` maximised
over all screens (`gs_frame.cpp:805-820`). `[V]` Metal offers only `displaySyncEnabled: bool`
(`CAMetalLayer.hpp:63-64`) plus `presentAfterMinimumDuration:`; there is **no true MAILBOX** and the
tri-state `vsync_mode` (`off`/`adaptive`/`full`, `swapchain.cpp:239-252`) has no 1:1 mapping (R23). `[V]`
Two owners of pacing (RPCS3's limiter and Metal's min-duration) will fight; the design keeps **RPCS3
as the single pacing owner** and uses `displaySyncEnabled` only to select FIFO-vs-immediate (§6 Q2). `[I]`

### 2.5 Frame ring and drawable back-pressure

The frame ring is `HLD-03` §2.3 Edge D's "#frames-in-flight `MTL4CommandAllocator`s + one
`MTLSharedEvent` value" and `HLD-04` §3.2's `mtl::frame_ring`. HLD-06 owns the **drawable** half and
the recycling rule:

- **Acquire.** `nextDrawable()` (`CAMetalLayer.hpp:58`) blocks when no drawable is free.
  `allowsNextDrawableTimeout` (`:69-70`) governs whether it times out (Godot sets it `true`); RPCS3's
  `flip()` runs on the RSX thread, so a blocking acquire stalls emulation (R21; `05` §5 rank 2). The
  presenter sets the timeout `true` and treats a nil drawable as a **dropped present** (see below). `[V]`
- **In-flight tracking.** Each frame slot carries a monotonically increasing `MTLSharedEvent` value;
  the queue signals it after the commit (`signalEvent`, `MTL4CommandQueue.hpp:142`), and the presenter
  gates slot reuse on it. The host wait is `MTLSharedEvent::waitUntilSignaledValue(_:timeoutMS:)`
  (`MTLEvent.hpp:73`) or a poll of `signaledValue()` (`:72`); the queue-side gate is
  `MTL4CommandQueue::wait(const MTL::Event*, value)` (`:148`, selector `waitForEvent_value_` `:277`). `[V]`
- **Back-pressure.** The VK acquire loop spins on `VK_TIMEOUT`/`VK_NOT_READY` and calls
  `check_present_status()` to recycle (`VKPresent.cpp:596-633`; `VKGSRender.cpp:1282-1294`). The Metal
  analogue is: try `nextDrawable()`; on nil, drain completed frame slots (feedback/event), then retry;
  if the window is minimized (0×0), skip the present entirely, mirroring the VK early-out
  (`VKPresent.cpp:46-50`). `[I]`
- **`skip_frame`.** `info.skip_frame` short-circuits the present in both backends (`VKPresent.cpp:495`;
  `GLPresent.cpp:164-169`). Under Metal a skipped frame commits its work (or drops the buffer) and
  recycles the slot **without acquiring a drawable**. `[I]`

### 2.6 Copy engines at the frame boundary

The guest can produce the display buffer with a blit-engine transfer rather than a draw. The inventory
clusters are `nv0039-dma` (12), `nv3089-image` (21), `nv308a-image` (16) and `nv2d-surface` (13); the
handlers are `nv0039::buffer_notify` (`rsx_methods.cpp:1729`), `nv3089::image_in` (`:1726`),
`nv308a::color` (`:1723`), and the NV3062 surface-2D cluster is largely unbound. `[V]` These map to a
**compute meta pass** (there is no Metal blit encoder; `HLD-01` §3.5, `HLD-04` §2.4). `[V]`

Presentation-relevant consequences:

1. **Ordering.** A blit that writes the display buffer must be ordered before the drawable's present;
   it is part of the frame's command buffer, so the `commit`→`signalDrawable`→`present` sequence
   covers it — but the scheduler must treat the blit as a writer of the present source (`HLD-03` §2.4
   "transfer alias" row). `[I]`
2. **Present-source resolution.** `get_present_source` (`VKPresent.cpp:306-424`; GL analogue
   `GLPresent.cpp:51-160`) resolves the display buffer to a surface, a cached texture, or a CPU
   upload; the Vulkan path may `copy_texture` from guest memory (`VKPresent.cpp:358-393`). The Metal
   analogue is a compute-encoder copy from a shared buffer into the present texture. `[I]`
3. **Upscale / calibration / overlay.** After the source is resolved, the VK path runs an upscaler and
   the `video_out_calibration_pass` then overlays (`VKPresent.cpp:824-976`; `GLPresent.cpp:329-444`).
   On Metal these are render/compute passes whose final target is the drawable texture; the overlay
   *content* is backend-agnostic (`Overlays/`). `[I]`

### 2.7 Window, backing scale, resize

`client_width()`/`client_height()` return `width() * devicePixelRatio()` (`gs_frame.cpp:781-803`), so
the drawable size is in **physical** pixels; `setDrawableSize:` (`CAMetalLayer.hpp:55-56`) must use that
size, and the viewport/aspect math (`avconfig.aspect_convert_region`, `VKPresent.cpp:642`) operates on
it unchanged. `[V]`

The resize path is the **trickiest state machine** (`05` §5 rank 3; R29). Today
`reinitialize_swapchain()` drains with `vkDeviceWaitIdle` (`VKPresent.cpp:85`) and rebuilds the FBO
caches; Metal has no swapchain object, so the presenter:

- re-reads `client_width/height` at the top of `flip()` and compares to the last drawable size,
  mirroring the VK dimension check (`VKPresent.cpp:431-435`);
- on change, calls `setDrawableSize:` **after** the in-flight drawables of the old size are released
  (their `MTLSharedEvent` values have fired); a drawable still held by the display must not be resized
  under it. `[I]`
- treats a 0×0 window as "present unavailable" (drop presents, keep committing) rather than recreating
  anything, mirroring `VKPresent.cpp:46-50`. `[I]`

### 2.8 What has no Metal path

Recorded so the gaps are explicit, not hidden (the "impossible register" story of `05` §4/§9, now
consolidated in `README.md` §6):

| Need | Status | Chosen fallback | Conf. |
|---|---|---|---|
| True MAILBOX / tear-free-uncapped present mode | **no 1:1 path** (R23) | `displaySyncEnabled=true` (FIFO) or `false` (immediate); approximate adaptive with `presentAfterMinimumDuration:` | `[V]` |
| `VkSwapchainKHR` recreate | **object does not exist** (R29) | `setDrawableSize:` + in-flight coordination | `[V]` |
| `vkDeviceWaitIdle` drain on resize | **not-recommended** (no queue-idle wait) | per-slot `MTLSharedEvent` feedback; resize only after release | `[I]` |
| `CAMetalDisplayLink` for pacing | **unverified C++ binding** | no `DisplayLink`/`maximumFramesPerSecond` symbol in metal-cpp; must be called from Obj-C or omitted | `[U]` |
| Stereo/multi-view swapchain | **no Metal "stereo swapchain"** (R31) | two drawables / layered texture + output shader; else unsupported fallback | `[V]` |
| Reading the presented image (screenshots/recording) | **conflicts with `framebufferOnly`** (R28) | owned colour texture + blit to drawable, or `framebufferOnly=false` only while capturing | `[V]` |

`CAMetalDisplayLink` and `NSScreen.maximumFramesPerSecond` have **no C++ binding** in this metal-cpp
snapshot (grep for `DisplayLink`/`CADisplayLink`/`maximumFramesPerSecond` finds nothing) → `[U]`;
the display-link pacing question is `05` §7 Q3 / §6 Q4 below. `[V]`

---

## 3. Interfaces / data structures

### 3.1 The presenter (C7)

`mtl::presenter` is the single owner of the layer, the drawable ring and the present call. It sits
between the recorder (`HLD-04` C4) and the window (`GSFrameBase`).

| Member | Type | Created from | Lifetime |
|---|---|---|---|
| `m_layer` | `CA::MetalLayer*` | the `NSView` from `GSFrameBase::handle()` (`GSFrameBase.h:31`), created if absent | window |
| `m_drawable` | `CA::MetalDrawable*` | `m_layer->nextDrawable()` (`CAMetalLayer.hpp:58`) | one frame |
| `m_frame_event` | `MTL::SharedEvent*` | `Device::newSharedEvent()` (`MTLDevice.hpp:500-501`) | device |
| `m_frame_value` | `u64` monotonic | incremented per frame slot | presenter |
| `m_drawable_size` | `CGSize` | `client_width()/client_height()` (`GSFrameBase.h:26-27`) | until resize |
| `m_last_presented_time` | `CFTimeInterval` | `MTL::Drawable::presentedTime()` (`MTLDrawable.hpp:52`) | per present |

### 3.2 Presenter types

| Type | Fields | Notes |
|---|---|---|
| `mtl::presenter_config` | `pixel_format`, `colorspace`, `edr`, `display_sync`, `max_drawables`, `allow_timeout` | the layer's static configuration; `[I]` |
| `mtl::present_frame` | `drawable`, `frame_value`, `skip`, `in_progress` | the per-frame present record; mirrors `display_flip_info_t` (`RSXDisplay.h:77-114`) |
| `mtl::present_result` | `presented`, `dropped_no_drawable`, `resized`, `unavailable` | replaces `VkResult`/`VK_SUBOPTIMAL_KHR` (`VKPresent.cpp:159-177`); `[I]` |

### 3.3 Frame ring (presenter half)

| Concern | Provider | Metal 4 object |
|---|---|---|
| Allocator / command buffer ring | `HLD-04` C4 `mtl::command_recorder` | `MTL4::CommandAllocator` + `MTL4::CommandBuffer` (`HLD-04` §3.2) |
| Drawable ring | HLD-06 `mtl::presenter` | `CA::MetalLayer` drawable pool, `maximumDrawableCount ≤ 3` |
| Frame completion | shared | one `MTLSharedEvent` value per slot (`MTL4CommandQueue.hpp:142`) |
| CPU recycle gate | HLD-06 | `waitUntilSignaledValue(_:timeoutMS:)` (`MTLEvent.hpp:73`) |
| Completion callback | shared | `CommitOptions::addFeedbackHandler` → `MTL4CommitFeedback` (`MTL4CommandQueue.hpp:98`; `MTL4CommitFeedback.hpp:38`) |

The two rings are coupled by the frame slot: a slot is reusable only when both its command allocator
is free (`HLD-04` invariant 2) **and** its drawable has been released by the display. `[I]`

### 3.4 Ownership

The presenter owns `m_layer`, the drawable and the present sequence; the recorder owns the allocator,
the command buffer and the `commit`; the RSX thread owns the `m_queued_flip` protocol and the flip
semaphore write. Overlay and calibration passes are recorded through C4 but target the drawable
texture the presenter acquired, so the presenter must hand the drawable texture to the recorder before
the final pass is opened. `[I]`

---

## 4. Invariants

1. **The flip is the presentation boundary; `SET_REFERENCE` is a FIFO-consumption marker.** No frame
   timing or present decision may be derived from `RsxDmaControl::ref` (`nv406e.cpp:19-21`). `[V]`
2. **Present is the four-step Metal 4 sequence.** `wait(drawable)` → `commit` → `signalDrawable` →
   `present()`; there is no `presentDrawable:` on an `MTL4::CommandBuffer`. `[V]`
3. **`maximumDrawableCount ≤ 3`.** The frame ring is clamped; no code may assume `minImageCount+2`
   drawables (R21; `swapchain.cpp:274-286`). `[V]`
4. **The acquire never blocks the RSX thread unboundedly.** `allowsNextDrawableTimeout=true` plus a
   dropped-present path; the TDR-style escape of `nv406e::semaphore_acquire` (`nv406e.cpp:68-88`) is
   the pattern for any host wait (`HLD-03` invariant 8). `[I]`
5. **A frame slot is recycled only after both its commit feedback and its drawable release.** `[I]`
6. **Nothing is resized while an in-flight drawable of the old size is held.** `setDrawableSize:` is
   ordered after release (R29). `[I]`
7. **Every presented image's GPU work is committed before `signalDrawable`.** The present source,
   upscale, calibration and overlay passes are in the same commit array. `[I]`
8. **The guest-visible flip word is written only after the presented frame's GPU work is visible.**
   `label_addr + 0x10` is a shared-buffer write gated on completion (`rsx_methods.cpp:65`;
   `LLD-sync-flip` invariant 6). `[I]`
9. **RPCS3 owns pacing.** `displaySyncEnabled` selects FIFO/immediate only; the frame limiter is not
   re-implemented in Metal (avoids double-pacing; R23). `[I]`
10. **Runtime gating.** Every `MTL4*`/`CA*` entry point is availability-checked (metal-cpp ships no
    `API_AVAILABLE`; R42, `HLD-01` invariant 10). `[V]`

---

## 5. Failure modes & limits

| Failure | Mechanism | Detection | Conf. |
|---|---|---|---|
| **Drawable starvation / RSX-thread stall** | `nextDrawable()` blocks when all 3 drawables are held; `flip()` is on the RSX thread | desync counter; `is_fifo_idle()` (`RSXThread.cpp:2944`); timeout path (R21) | `[V]` |
| **Dropped present** | acquire times out → present skipped but frame advanced | guest-visible frame timing drift; H3 on a flip-heavy capture | `[I]` |
| **Frame latency / queue depth** | drawables held longer than the allocator ring expects; `m_max_async_frames` no longer equals the drawable count | `m_frame_stats.flip_time` (`RSXDisplay.h:59`); H7 perf band | `[I]` |
| **Tear / pace mismatch** | `displaySyncEnabled` + RPCS3 limiter both pacing; no true MAILBOX (R23) | visual tear; `presentedTime` jitter | `[V]` |
| **Resize race** | `setDrawableSize:` while a drawable is presented; old-size texture reused | wrong-size frame / crash; resize is the trickiest state machine (R29) | `[I]` |
| **Minimized window** | 0×0 drawable; acquire fails | mirror `VKPresent.cpp:46-50` early-out | `[V]` |
| **HDR / colorspace mismatch** | drawable `pixelFormat`/`colorspace` do not match the calibration pass output (`avconfig.format` may be `R16G16B16X16_FLOAT`) | wrong gamma/range; calibration pass divergence | `[I]` |
| **Screenshot readback blocked** | `framebufferOnly=true` forbids reading the drawable texture (R28) | screenshot/recording black or failing | `[V]` |
| **Multi-display pacing** | `client_display_rate()` returns the max over all screens (`gs_frame.cpp:811-816`); VRR/ProMotion is dynamic | frame-limiter drift when the window moves screens | `[V]` |
| **Stereo present** | no Metal "stereo swapchain" (R31) | second eye missing | `[V]` |
| **Layer ownership conflict** | Qt 6 has no backing layer, or shares it (`metal_layer.mm:9` assumes one) (R22) | no drawable / black window | `[U]` |
| **No display-link binding** | `CAMetalDisplayLink` has no metal-cpp wrapper | must call Obj-C or omit pacing refinement | `[U]` |

**Hard limits carried forward.** `maximumDrawableCount ≤ 3` (R21); no true MAILBOX (R23); no
`VkSwapchainKHR`/`vkDeviceWaitIdle` (R29); `framebufferOnly` conflicts with readback (R28); no
verified `CAMetalDisplayLink` C++ binding; no local pixel oracle, so presentation correctness is
verified *differentially* until rung-3 guest readback exists (R43; `11` §6). `[V]`/`[U]`

---

## 6. Open questions

Each is a decision for a human, tagged with confidence.

1. **[U] `CAMetalLayer` ownership from the Qt `QWindow`.** Does the `NSView` from `gs_frame::handle()`
   already have a usable `CALayer` (`metal_layer.mm:9` reads `.layer`), or must the presenter call
   `setWantsLayer:YES` and attach a dedicated `CAMetalLayer`? Does Qt render into that view (conflict)?
   (`05` §7 Q1; R22.) **Highest-value** — the whole present path is blocked on it. `[U]`
2. **[I] Pacing owner — RPCS3 limiter, Metal min-duration, or a display link?** §2.4 chooses RPCS3 as
   the single owner; confirm that `displaySyncEnabled` alone is enough and that
   `presentAfterMinimumDuration:` is not needed (double-pacing risk) (`05` §7 Q2). `[I]`
3. **[U] Is `CAMetalDisplayLink` reachable at all?** No C++ binding in this metal-cpp snapshot; is an
   Obj-C call acceptable, and is a display link worth it versus `presentedTime`-based pacing?
   (`05` §7 Q3.) `[U]`
4. **[U] Frame-ring shape — drawable-only, or drawable + `MTLSharedEvent`?** §2.5 assumes one event
   value per slot (the Metal 4 sample's model); confirm the depth and whether the allocator ring and
   the drawable ring must be the same size (`HLD-03` §2.3 Edge D; `HLD-04` §3.2). `[U]`
5. **[U] EDR/HDR in scope?** RSX output is SDR with a gamma calibration pass; expose
   `wantsExtendedDynamicRangeContent` + `RGBA16Float` drawable, or keep `BGRA8Unorm` + sRGB only?
   (`05` §7 Q5; R16.) `[U]`
6. **[U] Screenshots/recording — always `framebufferOnly=false`, or an owned intermediate?** The VK
   path reads the presented image (`VKPresent.cpp:684-779`); the GL path uses `copy_to`
   (`GLPresent.cpp:331-393`). `framebufferOnly=false` costs bandwidth; an owned texture + blit costs a
   pass (R28; `05` §7 Q7). `[U]`
7. **[I] Stereo present — design or drop?** Two drawables or a layered texture + viewport split; RSX
   stereo is edge-case (`VKPresent.cpp:560-580`; R31). `[I]`
8. **[U] Resize protocol — how much of `reinitialize_swapchain` survives?** The Metal path must not
   drain (`VKPresent.cpp:85`); which caches (FBO/texture) still need invalidation on a size change?
   (R29.) `[U]`
9. **[I] Present-source resolution — port `get_present_source` or rewrite it?** It has an explicit
   `@FIXME` in both backends (`VKPresent.cpp:310`; `GLPresent.cpp:55`); the Metal version is a compute
   copy. Decide whether to reuse the surface/texture-cache lookup unchanged. `[I]`
10. **[U] Does `presentedTime`/`addPresentedHandler` give a usable emulated-vsync signal?** If yes, the
    guest's flip timing could track real presentation instead of the static refresh rate
    (`gs_frame.cpp:805`). `[U]`

**Highest-value open questions (for the human), in order:** **Q1** (layer ownership — blocks the whole
path), **Q2/Q3** (pacing owner and display-link reachability), **Q4** (frame-ring shape, which locks
the allocator ring and present sync), **Q6** (readback vs `framebufferOnly`, which decides the present
target), **Q5** (HDR/colorspace, which decides pixel format and the calibration pass).

---

## 7. Provenance

### 7.1 Primary — verified against this tree `[V]`

- `rpcs3/Emu/RSX/RSXThread.cpp` — `post_vblank_event` `:1062`; `init` `:2731` (`device_addr + 0x30`
  `:2738`); `flip` `:2776` (async clear `:2780-2791`); `sync()` `:2917`; `is_fifo_idle` `:2944`;
  `on_frame_end` `:3457` (`m_queued_flip.push` `:3525`); `get_cached_display_refresh_rate` `:3572`;
  `request_emu_flip` `:3605`; `handle_emu_flip` `:3635` (frame limiter `:3650-3716`, `flip()` call
  `:3725`, `flip_status` `:3728`); `evaluate_cpu_usage_reduction_limits` `:3761`.
- `rpcs3/Emu/RSX/RSXThread.h` — `m_queued_flip` `:113`; `m_display_rate_fetch_count` `:152`;
  `m_cached_display_rate` `:153`; `get_display_refresh_rate` `:155`; `m_frame_stats` `:162`;
  `async_flip_requested` `:224`; `async_flip_buffer` `:225`; `flip_status` `:241`; `requested_vsync`
  `:244`; `enable_second_vhandler` `:245`; `on_frame_end` `:371`; `flip` `:372`.
- `rpcs3/Emu/RSX/Core/RSXDisplay.h` — `frame_statistics_t` `:50-68`; `display_flip_info_t` `:77-114`
  (`buffer_queue`/`buffer`/`skip_frame`/`emu_flip`/`in_progress`/`stats`); `vblank_thread` `:116`.
- `rpcs3/Emu/RSX/NV47/HW/nv406e.cpp` — `set_reference` `:14-22` (`sync()` `:16`, `ref.store` `:21`);
  `semaphore_acquire` `:24` (flip-sema `:44`, TDR `:68-88`); `semaphore_release` `:107` (flip-sema
  bypass `:121-125`, device flip word `:137-141`, `write_gcm_label` `:143`).
- `rpcs3/Emu/RSX/rsx_methods.cpp` — `flip_command` `:40-66` (`ensure(isHLE)` `:42`, zero
  `label_addr + 0x10` `:65`); `user_command` `:68`; `driver_flip` `:96-102`; `queue_flip` `:104-116`;
  `fifo::draw_barrier` `:121`; NV308A bind `:1723`; NV3089 `:1726`; NV0039 `:1729`.
- `rpcs3/Emu/RSX/VK/VKPresent.cpp` — `reinitialize_swapchain` `:38` (minimized `:46-50`,
  `vkDeviceWaitIdle` `:85`, `m_max_async_frames` `:112`); `present` `:150-182`; `advance_queued_frames`
  `:184`; `queue_swap_request` `:215`; `frame_context_cleanup` `:245`; `get_present_source` `:306-424`
  (`@FIXME` `:310`, copy-from-guest `:358-393`); `flip` `:426` (vsync recreate `:438`, skip `:495`,
  acquire loop `:595-633`, aspect `:642`, screenshot `:684-779`, upscale/calibration `:824-887`,
  overlay `:889-976`, `queue_swap_request()` `:983`, `m_frame->flip` `:987`).
- `rpcs3/Emu/RSX/GL/GLPresent.cpp` — `get_present_source` `:51-160` (`@FIXME` `:55`); `flip` `:162`
  (skip `:164-169`, `update_swap_interval` `:256`, screenshot `:329-393`, overlay `:444`,
  `m_frame->flip` `:518`).
- `rpcs3/Emu/RSX/GSFrameBase.h` — `flip` `:25`; `client_width` `:26`; `client_height` `:27`;
  `client_display_rate` `:28`; `handle` `:31`; `present_frame` `:34`; `take_screenshot` `:35`.
- `rpcs3/Emu/RSX/display.h` — `display_handle_t = void* // NSView` `:20-21`; `draw_context_t` `:37`.
- `rpcs3/Emu/RSX/GSRender.h` — `GSRender : rsx::thread` `:20`; `m_vsync_mode` `:27`; `flip` `:39`;
  `get_display_refresh_rate` `:40`.
- `rpcs3/Emu/RSX/GSRender.cpp` — ctor vsync `:18`; `flip` `:55-61`; `get_display_refresh_rate`
  `:63-72`.
- `rpcs3/rpcs3qt/gs_frame.cpp` — `handle` `:737-764` (NSView `:742`); `client_width` `:781-791`;
  `client_height` `:793-803`; `client_display_rate` `:805-820`; `flip` `:822`.
- `rpcs3/Emu/RSX/VK/VKGSRender.cpp` — swapchain dims `:577-580`; `m_max_async_frames` `:686`;
  `check_present_status` `:1282-1294`.
- `rpcs3/Emu/RSX/VK/VKGSRender.h` — `m_swapchain` `:97`; `m_swapchain_dims` `:118`;
  `swapchain_unavailable` `:119`; `m_frame_context_storage` `:168`; `m_max_async_frames` `:169`;
  `m_aux_frame_context` `:171`; `m_current_frame` `:174`; `m_queued_frames` `:175`.
- `rpcs3/Emu/RSX/VK/VKGSRenderTypes.hpp` — `frame_context_t` `:175`; acquire/present semaphores
  `:177-178`; `present_image` `:182`; `swap_command_buffer` `:183`.
- `rpcs3/Emu/RSX/VK/vkutils/swapchain.cpp` — `init` `:196`; present-mode chain `:236-272`;
  `nb_swap_images` `:274-286`; `present` `:344`.
- `rpcs3/Emu/RSX/VK/vkutils/metal_layer.mm:9` — `GetCAMetalLayerFromMetalView` (header `metal_layer.h:2`).
- `docs/research/metal4/inventory/out/clusters/sync-flip.csv` — 18 IDs, incl. `M_NV406E_SET_REFERENCE`
  `:2`, `M_GCM_FLIP_HEAD` `:16`, `M_GCM_DRIVER_QUEUE` `:17`, `M_GCM_FLIP_COMMAND` `:19`;
  `inventory/out/clusters.json` — `sync-flip` 18, `nv0039-dma` 12, `nv3089-image` 21, `nv308a-image`
  16, `nv2d-surface` 13.

### 7.2 metal-cpp (commit `27c4382b`, 2026-06-08) `[V]`

- `QuartzCore/CAMetalLayer.hpp` — `device` `:46-47`; `pixelFormat` `:49-50`; `framebufferOnly`
  `:52-53`; `drawableSize` `:55-56`; `nextDrawable` `:58`; `maximumDrawableCount` `:60-61`;
  `displaySyncEnabled` `:63-64`; `colorspace` `:66-67`; `allowsNextDrawableTimeout` `:69-70`;
  `wantsExtendedDynamicRangeContent` `:72-73`; `residencySet` `:75`.
- `QuartzCore/CAMetalDrawable.hpp` — `layer` `:38`; `texture` `:39`.
- `Metal/MTLDrawable.hpp` — `addPresentedHandler` `:42-43`; `drawableID` `:45`; `present` `:47`;
  `presentAfterMinimumDuration` `:48`; `presentAtTime` `:50`; `presentedTime` `:52`.
- `Metal/MTL4CommandQueue.hpp` — `CommandQueueErrorTimeout` `:55`; `CommitOptions::addFeedbackHandler`
  `:98`; `CommandQueue` `:120`; `addResidencySet` `:123`; `commit` `:126-127`; `signalDrawable` `:140`;
  `signalEvent` `:142`; `wait(event,value)` `:148`; `wait(drawable)` `:149` (selectors
  `waitForEvent_value_` `:277`, `waitForDrawable_` `:282`).
- `Metal/MTL4CommitFeedback.hpp` — `CommitFeedback` `:38`; `GPUEndTime` `:41`; `GPUStartTime` `:43`;
  `error` `:45`.
- `Metal/MTLCommandBuffer.hpp` — **Metal 3 contrast**: `presentDrawable` `:177`;
  `presentDrawableAfterMinimumDuration` `:178`; `presentDrawableAtTime` `:180`.
- `Metal/MTLDevice.hpp` — `CreateSystemDefaultDevice` `:262`/`:600`; `newCommandAllocator` `:416-417`;
  `newCommandBuffer` `:419`; `newMTL4CommandQueue` `:477-478`; `newResidencySet` `:496`;
  `newSharedEvent` `:500-501`.
- `Metal/MTLEvent.hpp` — `SharedEvent` `:63`; `setSignaledValue` `:71`; `signaledValue` `:72`;
  `waitUntilSignaledValue(_:timeoutMS:)` `:73`.
- `Metal/MTLResidencySet.hpp` — `ResidencySet` `:49`; `addAllocation` `:52`; `commit` `:61`;
  `requestResidency` `:76`.
- **Absent**: no `CAMetalDisplayLink`, `CADisplayLink`, `maximumFramesPerSecond` or
  `preferredFrameRateRange` symbol anywhere in the snapshot (grep) `[V]`; `MTL4CommandBuffer` carries
  no drawable reference (no `Drawable` symbol in `MTL4CommandBuffer.hpp`) `[V]`.

### 7.3 Sibling scope `[V]`

- `docs/research/metal4/design/HLD-01-architecture.md` — §2.2 (shape change row `flip`), §3.2 (C7
  `mtl::presenter`), §3.4 (present object row), §3.5, §5 (`nextDrawable`/`maximumDrawableCount`), §6
  (Q7), §7.
- `docs/research/metal4/design/HLD-03-synchronization-model.md` — §2.1 (order primitives, commit-array
  `[U]`), §2.2 (register corrections: `NV4097_SET_REFERENCE` absent), §2.3 Edge D (frame/flip
  boundary), §3.3 (`mtl::` objects), §4 (invariants 8/9), §6 (Q8/Q10).
- `docs/research/metal4/design/HLD-04-command-encoding.md` — §1.2 (HLD-06 ownership), §2.2
  (record/submit, `commit`), §3.2 (`mtl::frame_ring`), §3.4 (pipeline state), §3.8 (hand-off).
- `docs/research/metal4/design/LLD-sync-flip.md` — §3.2 (semaphore/reference protocol, flip word),
  §3.5 (flip semaphore), §3.6 (flip/display hand-off), §5 (invariant 6).
- `docs/research/metal4/05-presentation-and-impossible-register.md` — §1.2 (inventory), §2 (mapping
  rows #1–#24), §3.3–§3.5 (reference code), §5 (ranked tricky list), §7 (open questions); the master
  register was superseded by `README.md` §6.
- `docs/research/metal4/README.md` — §3.3 rows #43/#141–#155; §4 shifts 3–4; §6 R21/R22/R23/R28/R29/
  R31/R42/R43; §7 (phase 5 parity: stereo, FSR/MetalFX).
- `docs/research/metal4/11-verification-loop.md` — §4 (H0–H7), §6 (no local pixel oracle), §9 (flip-
  heavy corpus item, determinism).
- `docs/research/metal4/inventory/SCHEMA.md` §4/§6; `inventory/out/SUMMARY.md` (282 registers / 14
  clusters).

### 7.4 Apple / external (as fixed by `05`; not re-fetched here) `[V]` via `05`

- `CAMetalLayer` / `MTLDrawable` / `MTL4CommandQueue.signalDrawable`/`waitForDrawable`; Metal 4 core
  API; `MTL4CommandQueue` present flow — `05` §8 (developer.apple.com documentation links).
- Godot `rendering_context_driver_metal.cpp` (deferred drawable acquisition, `setAllowsNextDrawableTimeout(true)`,
  `setMaximumDrawableCount(MIN(3U, n))`, HDR branch); Mesa KosmicKrisp `kk_wsi.c`; Metal 4 sample
  `Metal4Renderer.swift` (`waitForDrawable`→`commit`→`signalDrawable`→`present`); MoltenVK limitations
  — `05` §3.
- WWDC25 205/254/211 (Metal 4 games); "Optimize for variable refresh rate displays" (WWDC21 10147);
  MetalFX — `05` §8.

<!-- doc-end -->
