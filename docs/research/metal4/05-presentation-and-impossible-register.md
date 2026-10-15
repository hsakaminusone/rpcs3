# 5. Presentation, swapchain, host/window integration + master "impossible/tricky" register
Owner/scope: Scope 5 | Status: draft | Date: 2026-10-15

> Scope: (A) the present/swapchain/windowing seam of a native Metal 4 RSX backend, and (B) the
> consolidated cross-scope "impossible / tricky" register for the whole backend.
>
> **Availability of peer scope docs at time of writing:** the directory `docs/research/metal4/` did
> **not exist** when this doc was started, and `01-core-memory.md`, `02-submission-sync.md`,
> `03-shaders-pipelines-descriptors.md`, `04-images-formats-samplers-rtt.md` were **not present**.
> §9 is therefore built from **my own verification** (RPCS3 source, the four pristine ref trees, and
> web sources) plus the *background* docs `docs/research/01,03,04,05,07,08` and the Mesa
> `KK_WORKAROUND_*` list. Provenance is marked per row; nothing is attributed to a peer scope doc
> that I did not actually read.
>
> **Read-only discipline:** no RPCS3 source/CMake/test was modified and no build was run. All
> `file:line` citations are point-in-time on branch `kosmickrisp-integration`. Another agent is
> concurrently editing `rpcs3/Emu/RSX/VK/**`; line numbers there may drift.

---

## 1. Scope & RPCS3 inventory

### 1.1 What "presentation" means here
A native backend must implement the `rsx::thread::flip()` contract (`Emu/RSX/RSXThread.h`) and the
window-handle seam `GSFrameBase` (`Emu/RSX/GSFrameBase.h`), which on macOS hands the renderer an
`NSView*` (`display_handle_t = void*`, `Emu/RSX/display.h:22`). Today that path is:
`Qt gs_frame (QWindow)` → `winId()` → `NSView*` → `CAMetalLayer` → `VK_EXT_metal_surface` → Vulkan
WSI. A native backend keeps everything up to the `CAMetalLayer` and replaces the Vulkan WSI and
`vkQueuePresentKHR` with `CAMetalLayer` + `MTL4CommandQueue.signalDrawable`/`waitForDrawable` +
`MTLDrawable.present`.

### 1.2 Exact RPCS3 inventory (verified from tree)

| Concern | Symbol / file:line | Notes |
|---|---|---|
| Flip entry point | `VKGSRender::flip(const rsx::display_flip_info_t&)` — `VK/VKPresent.cpp:426` | 590-line body: swapchain health, acquire, aspect fit, upscale, overlays, submit+present, stats. |
| Present call | `m_swapchain->present(...)` — `VK/VKPresent.cpp:159`; impl `vk::swapchain_WSI::present` `vkutils/swapchain.cpp:344`; `vkQueuePresentKHR` at `vkutils/swapchain.cpp:359` | Returns `VK_SUCCESS`/`VK_SUBOPTIMAL_KHR`/`VK_ERROR_OUT_OF_DATE_KHR`; handled `VKPresent.cpp:161-177`. |
| Swapchain recreate | `VKGSRender::reinitialize_swapchain()` — `VK/VKPresent.cpp:38-148` | **Hard sync point**: drains queues (`vkDeviceWaitIdle`, `VKPresent.cpp:85`), clears FBO caches, rebuilds. Called from `flip()` retry loop `VKPresent.cpp:443-465` and from `OUT_OF_DATE` at `:621`. |
| Acquire | `vkAcquireNextImageKHR` — `vkutils/swapchain_core.h:202-205`; loop/spin + `VK_TIMEOUT`/`NOT_READY` handling `VKPresent.cpp:596-633` | Timeout `0` when image count ≤ 2 else 100 ms (`VKPresent.cpp:595`). |
| Present modes | `VkPresentModeKHR` preference chain — `vkutils/swapchain.cpp:236-272` | maps `vsync_mode` → IMMEDIATE/MAILBOX/FIFO_RELAXED/FIFO. |
| Image count | `nb_swap_images = minImageCount+2` (triple-buffer target) — `vkutils/swapchain.cpp:274-286` | becomes `m_max_async_frames` (`VKPresent.cpp:112`, `VKGSRender.cpp:686`). |
| Format/color-space pick | `vkGetPhysicalDeviceSurfaceFormatsKHR` — `vkutils/instance.cpp:374-400` | software present fallback `instance.cpp:366-370`. |
| WSI surface (Apple) | `make_WSI_surface` uses `VkMetalSurfaceCreateInfoEXT` + `GetCAMetalLayerFromMetalView` — `vkutils/swapchain_macos.hpp:12-22` | `#include "metal_layer.h"`. |
| **Metal layer shim** | `GetCAMetalLayerFromMetalView` — `vkutils/metal_layer.mm:9` (header `metal_layer.h:2`) | **2-line header / 10-line `.mm`.** Body: `return ((NSView*)view).layer;` — the seed of the native path; assumes the `NSView` already has a backing `CALayer`. |
| macOS swapchain alias | `using swapchain_MacOS = native_swapchain_base` — `vkutils/swapchain_macos.hpp:9` | the *software-present fallback* (`instance.cpp:367`), not the primary path. Primary is `swapchain_WSI` (`instance.cpp:404`). `native_swapchain_base::init/create/present/destroy` all `throw_exception("Native swapchain is not implemented yet!")` (`swapchain_core.h:125-143`). |
| Present semaphores | `vk::frame_context_t::acquire_signal_semaphore`/`present_wait_semaphore` — `VK/VKGSRenderTypes.hpp:177-200` | created in `frame_context_t::init` (`:188`), one pair per swap image. |
| Submit/signal | `close_and_submit_command_buffer(...)` — `VK/VKGSRender.cpp:2510`, called `VKPresent.cpp:227-230` with `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT` | `wait_on`/`queue_signal` at `VKGSRender.cpp:2585-2608`. |
| Frame advance | `advance_queued_frames()` `VKPresent.cpp:184`; `queue_swap_request()` `:215`; `frame_context_cleanup()` `:245` | ring of `m_max_async_frames` contexts. |
| Frame pacing | `m_vsync_mode` compared to `g_cfg.video.vsync` forces recreate — `VKPresent.cpp:438-441`; vsync enum `Emu/system_config_types.h:390` | |
| Display refresh rate | `GSFrameBase::client_display_rate()` → `QScreen::refreshRate()` — `rpcs3qt/gs_frame.cpp:805-820`; consumed by `RSXThread` timing | |
| Window handle | `gs_frame::handle()` returns `(void*)winId()` NSView — `rpcs3qt/gs_frame.cpp:737-747` | |
| Swapchain init | `m_swapchain->init(w,h)` — `VKGSRender.cpp:577-583`; initial image clear-to-black `VKGSRender.cpp:723-734` | |
| Present-source resolution | `get_present_source()` — `VKPresent.cpp:306-424` (RTT overlap, format cast, texture-cache upload) | expects `vk::viewable_image*`. |
| Upscaling | `vk::upscaler` iface `upscalers/upscaling.h:24`; `nearest_upscale_pass` (`nearest_pass.hpp:7`), `bilinear_upscale_pass` (`bilinear_pass.hpp:7`), `fsr_upscale_pass` (`fsr_pass.h:49`); driver `VKPresent.cpp:806-822, 824-887` | FSR = **FSR1 spatial**, no motion vectors. |
| Overlays | `vk::overlay_pass` `VK/VKOverlays.h:42`; `ui_overlay_renderer` `:122`; `video_out_calibration_pass` `:208`; `get_overlay_pass<T>()` `:250`; drive from `VKPresent.cpp:666-680, 889-976` | shared UI markup lives in `Emu/RSX/Overlays/`; only the renderer is backend-specific. |
| Screenshot / recording | forced GPU→CPU readback `VKPresent.cpp:684-779` (`vk::buffer` + `vkCmdCopyImageToBuffer` + `map`) | |
| Occlusion / conditional | `vk::query_pool_manager` `VK/VKQueryPool.h:13`, `VK_QUERY_TYPE_OCCLUSION` `:46`; `begin/end_conditional_rendering` `VKGSRender.cpp:3011`; `_vkCmdBeginConditionalRenderingEXT` `VKDraw.cpp:1168` | `supports_hw_conditional_render = (vendor != MVK)` `VKGSRender.cpp:795`. |
| Instance / surface extensions | `VK_KHR_SURFACE` `instance.cpp:122`; `VK_KHR_PORTABILITY_ENUMERATION` `:131,196`; `VK_EXT_METAL_SURFACE` `:164` | |
| Driver vendor | `enum class driver_vendor { ..., MVK, HONEYKRISP, KOSMICKRISP }` — `vkutils/chip_class.h:45-59` | also `APPLE_MVK` `:36`. |

### 1.3 What is trivially reusable for a Metal backend
`flip()`'s **non-GPU** logic — buffer scan, `avconf` handling, aspect/inset math
(`avconfig.aspect_convert_region`, `VKPresent.cpp:642`), stereo eye split (`:560-580`), resolution
scale, screenshot encode (`gs_frame::take_screenshot`), overlay *content* (`Overlays/`) — is
API-neutral. Only the ~15 Vulkan calls around it are backend-specific.

---

## 2. Vulkan/RSX → Metal 4 mapping table

Difficulty: 1 trivial rename · 2 mechanical · 3 real work but well-trodden · 4 hard/needs design · 5 unknown/impossible.

| # | Vulkan / RSX construct (exact API or symbol) | Metal 4 equivalent | Metal 4 objects/methods | Diff | Notes |
|---|---|---|---|---|---|
| 1 | Window handle `NSView*` + `CAMetalLayer` (`metal_layer.mm:9`) | same `CAMetalLayer` | `CA::MetalLayer` | 1 | Shim already returns the layer; must also **create/attach** a `CAMetalLayer` if the view has none. |
| 2 | `VK_EXT_metal_surface` / `VkMetalSurfaceCreateInfoEXT` (`swapchain_macos.hpp:16-20`) | (no surface object) | — | 1 | Native backend talks to the layer directly; extension and loader vanish. |
| 3 | `VK_KHR_portability_enumeration` (`instance.cpp:131`) | (none) | — | 1 | Only needed to *hide/show* MVK; meaningless natively. |
| 4 | `VkSwapchainKHR` + `vkCreateSwapchainKHR` (`swapchain.cpp:292-328`) | drawable ring owned by `CAMetalLayer` | `setDrawableSize:`, `setPixelFormat:`, `setMaximumDrawableCount:`, `setFramebufferOnly:`, `setDevice:`, `setColorspace:` | 3 | Reimplement image bookkeeping; see Godot `SurfaceLayer` (`rendering_context_driver_metal.cpp:149-284`). |
| 5 | `vkAcquireNextImageKHR` (`swapchain_core.h:204`) | `nextDrawable` | `CA::MetalLayer::nextDrawable()` | 3 | **Blocking**. `allowsNextDrawableTimeout` (Godot sets `true`) governs timeout; `false` can deadlock the caller (cf. zed #53390). |
| 6 | Acquire/present semaphores (`frame_context_t`, `VKGSRenderTypes.hpp:177-193`) | drawable ordering + shared events | `MTL4::CommandQueue::waitForDrawable:` / `signalDrawable:` / `signalEvent:value:` / `waitForEvent:value:` (`MTL4CommandQueue.hpp:140,142,277,280`) | 4 | Metal 4 does the wait/commit/signal dance on the queue, not via `VkSemaphore`. Needs a re-model of the frame ring. |
| 7 | `vkQueuePresentKHR` (`swapchain.cpp:359`) | drawable present | `MTL::Drawable::present()` (`MTLDrawable.hpp:47`), `presentAfterMinimumDuration:` (`:48`), `presentAtTime:` (`:50`) | 2 | |
| 8 | `VkPresentModeKHR` FIFO/IMMEDIATE/MAILBOX/RELAXED (`swapchain.cpp:236-272`) | bool + min-duration | `setDisplaySyncEnabled:` (MetalLayer), `presentAfterMinimumDuration:` | 3 | Metal has no true MAILBOX; FIFO↔`displaySync=true`, IMMEDIATE↔`false`; RELAXED/MAILBOX approximated by min-duration. |
| 9 | `nb_swap_images = minImageCount+2` (`swapchain.cpp:279`) | `maximumDrawableCount` (≤3) | `CA::MetalLayer::setMaximumDrawableCount:` | 2 | Metal caps at 3 (Godot clamps `MIN(3U, n)`), so `m_max_async_frames≤3`. |
| 10 | `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` (`swapchain_core.h:219`) | no layout; drawable texture | `CA::MetalDrawable::texture()` | 2 | Acquire/present are the only transitions. |
| 11 | `vkCmdClearColorImage` on swap image (`VKPresent.cpp:130, 788`) | render-pass clear | `MTL4RenderPassDescriptor` attachment `loadAction = Clear` | 2 | |
| 12 | `VkImageBlit` / `m_upscaler->scale_output` (`VKPresent.cpp:885`) | blit / draw | `MTL4::ComputeCommandEncoder` blit, or full-screen render | 3 | |
| 13 | FSR1 spatial upscale (`upscalers/fsr_pass.h:49`) | MetalFX | `MTLFX::SpatialScaler` / `MTL4FX::SpatialScaler` (`MetalFX/MTL4FXSpatialScaler.hpp`) | 3 | MetalFX spatial is a usable drop-in; **temporal** scaler needs depth+motion+jitter RPCS3 doesn't have. |
| 14 | Overlay compositing (`overlay_pass`, `VKOverlays.h:42`) | MTL4 render draws | `MTL4RenderCommandEncoder` + `MTL4ArgumentTable` | 3 | Shared `compiled_resource` reused. |
| 15 | `video_out_calibration_pass` gamma/range (`VKOverlays.h:208`) | render + layer colorspace | `setColorspace:`, `setWantsExtendedDynamicRangeContent:` | 3 | EDR only meaningful with float16 drawable. |
| 16 | sRGB/format backpressure (`instance.cpp:392-400` prefers BGRA8_UNORM) | drawable format | `setPixelFormat:` BGRA8Unorm / RGBA16Float | 2 | Colorspace is a `CGColorSpaceRef` on the layer, not a Vk color space. |
| 17 | Refresh rate (`gs_frame.cpp:805`, `client_display_rate`) | display link / screen | `CAMetalDisplayLink`, `NSScreen.maximumFramesPerSecond` | 3 | Needed for correct emulated timing + pacing. |
| 18 | Frame pacing / vsync config (`system_config_types.h:390`) | display link / sync | `CAMetalDisplayLink` `preferredFrameRateRange`, `displaySyncEnabled` | 4 | Tri-state vsync has no 1:1 Metal equivalent (see §5). |
| 19 | Screenshot readback (`VKPresent.cpp:684-779`) | blit to shared buffer | `MTL4::ComputeCommandEncoder` blit + `MTLStorageModeShared` | 2 | `framebufferOnly` must be `false` to read the drawable texture. |
| 20 | `VkQueryPool` occlusion (`VKQueryPool.h:46`) | visibility result buffer | `MTLRenderCommandEncoder::setVisibilityResultMode:offset:` (+ resolve) | 4 | Count-of-samples model, not "any-passed"; see register. |
| 21 | `VK_EXT_conditional_rendering` (`VKDraw.cpp:1168`) | **none** | — | 4 | No Metal equivalent; MVK already disables it for perf reasons. |
| 22 | Swapchain recreate on resize (`VKPresent.cpp:38`) | resize drawable | `setDrawableSize:` on the layer | 2 | Must coordinate in-flight drawables; no `vkDeviceWaitIdle`. |
| 23 | Stereo present (`VKPresent.cpp:560-580`) | two-eyes present | two views / layered drawable | 4 | RSX stereo is edge-case; no Metal "stereo swapchain". |
| 24 | `MTKView` (not used by RPCS3 today) | alternative host view | `MTKView.currentDrawable`/`currentMTL4RenderPassDescriptor` | 2 | RPCS3 uses `QWindow`; `MTKView` would be intrusive. |

**Verified fact** (metal-cpp headers): `MTL::Device::newMTL4CommandQueue()` (`Metal/MTLDevice.hpp:477`),
`newCommandAllocator()` `:416`, `newCommandBuffer()` `:419`, `newArgumentTable(descriptor:error:)` `:407`,
`newResidencySet(descriptor:error:)` `:496`, `newCompiler(descriptor:error:)` `:425`.
`MTL4::CommandBuffer::beginCommandBuffer(allocator:)` `MTL4CommandBuffer.hpp:64`, `useResidencySet:` `:88`.
`MTL4::CommandQueue::commit(...)` `MTL4CommandQueue.hpp:126`, `signalDrawable:` `:140`, `signalEvent:` `:142`.

---

## 3. Pristine reference code

All paths under `/tmp/metal4-refs/`. **Verified from tree.**

### 3.1 Godot — closest host/surface analog
- `godot/drivers/metal/rendering_context_driver_metal.h` — `RenderingContextDriverMetal::Surface` abstraction
  (`:86-128`): `resize()`, `acquire_next_frame_buffer()`, `present(MDCommandBuffer*)`, `next_drawable()`, HDR fields.
  `WindowPlatformData { CA::MetalLayer *layer; }` (`:82-84`).
- `godot/drivers/metal/rendering_context_driver_metal.cpp`:
  - `MDFrameBufferLayer` — **deferred drawable acquisition** (calls `layer->nextDrawable()` only when the
    texture is first needed) `:116-147`.
  - `SurfaceLayer::SurfaceLayer` — `setAllowsNextDrawableTimeout(true)`, `setFramebufferOnly(true)`,
    `setOpaque`, `setPixelFormat`, `setDevice` `:157-164`.
  - `SurfaceLayer::resize` — `setDrawableSize`, `setMaximumDrawableCount(MIN(3U, n))`, `setDisplaySyncEnabled`
    per vsync `:170-224`; HDR branch: `setWantsExtendedDynamicRangeContent(true)` +
    `kCGColorSpaceExtendedLinearSRGB` + `PixelFormatRGBA16Float` `:204-224`, SDR uses `kCGColorSpaceSRGB` + BGRA8 `:214-223`.
  - `present()` — `presentDrawableAfterMinimumDuration` when vsync != disabled, else `presentDrawable` `:256-262`.
  - `get_residency_set()` returns `layer->residencySet()` (macOS 26) `:280-283`.
  - `SurfaceOffscreen` — software-present fallback with a 3-deep ring `:286-408`.
- `godot/drivers/vulkan/rendering_context_driver_vulkan.cpp` — the Vulkan side, for the same four methods.
- `godot/drivers/metal/README.md` — "Future work" explicitly lists *placement heaps*, *explicit hazard
  tracking*, *MetalFX upscaling*.

### 3.2 Mesa KosmicKrisp — canonical Vulkan→Metal 4 WSI
- **Naming correction (verified):** there is **no `kk_swapchain.c`** and no `mtl_layer.*`/`mtl_swapchain.*`
  in this tree. KK does *not* own a swapchain; it plugs into **Mesa shared `wsi_common`** and only supplies
  Metal hooks:
  - `mesa/src/kosmickrisp/vulkan/kk_wsi.c` — `wsi_device_init(...)` (`:120`), then sets
    `wsi->metal.bind_drawable_to_vkimage = kk_bind_drawable_to_vkimage` (`:129`),
    `wsi->metal.encode_drawable_present = kk_encode_drawable_present` (`:130`),
    `wsi->metal.cmd_blit_image` (`:131`), `wsi->metal.get_mtl4_command_queue` (`:132`), `wsi->get_blit_queue` (`:133`).
  - `kk_bind_drawable_to_vkimage` `:51-69` — binds a `CAMetalDrawable`'s texture into the VkImage and takes
    its **`gpuResourceID`** (`mtl_texture_get_gpu_resource_id`).
  - `kk_encode_drawable_present` `:85-91` — stashes the drawable on the `kk_cmd_buffer` for present.
  - `kk_get_mtl4_command_queue` `:93-102` — the swapchain present uses the device's **Metal 4** queue.
- Bridge (the analog of what a native backend must write): `mesa/src/kosmickrisp/bridge/mtl_*.{h,m}`,
  incl. `mtl_command_queue.m`, `mtl_residency_set.m`, `mtl_sync.m`, `mtl_texture.m`; only
  `mtl_bridge.m:21` imports `<QuartzCore/CAMetalLayer.h>`.
- Frameworks actually linked (verified): `bridge/meson.build` links **`Foundation` only**; the Vulkan
  ICD `vulkan/meson.build` links `-Wl,-undefined,dynamic_lookup` (Metal/QuartzCore resolved at runtime).
- **`mesa/docs/drivers/kosmickrisp/workarounds.rst`** — the 18 `KK_WORKAROUND_*` entries (read in full;
  see §9 for the ones a native backend also inherits).

### 3.3 Apple metal-cpp bindings
- `metal-cpp/QuartzCore/CAMetalLayer.hpp`: `nextDrawable()` `:58`, `maximumDrawableCount` `:60`,
  `displaySyncEnabled` `:63`, `colorspace/setColorspace` `:66-67`, `allowsNextDrawableTimeout` `:69`,
  `wantsExtendedDynamicRangeContent` `:72`, `residencySet()` `:75`.
- `metal-cpp/QuartzCore/CAMetalDrawable.hpp`: `layer()` `:38`, `texture()` `:39`.
- `metal-cpp/Metal/MTLDrawable.hpp`: `present()` `:47`, `presentAfterMinimumDuration:` `:48`,
  `presentAtTime:` `:50`, `presentedTime()` `:52`, `addPresentedHandler` `:42`.
- `metal-cpp/Metal/MTL4CommandQueue.hpp`: `commit` `:126`, `signalDrawable` `:140`, `signalEvent` `:142`,
  `waitForEvent` `:275`, `wait(drawable)` `:280`.
- `metal-cpp/MetalFX/MTL4FX*` and `MetalFX/MTLFX*` — spatial/temporal/denoised scalers + frame interpolator.
- No `CAMetalDisplayLink.hpp` wrapper exists in this metal-cpp snapshot (only `CAMetalLayer`,
  `CAMetalDrawable`) — **could not verify** a C++ binding for `CAMetalDisplayLink`; must be called from Obj-C.

### 3.4 Metal 4 sample app (present flow, authoritative)
- `metal-4-basics/Metal4Basics/Sources/Utility/BufferUtilities.swift` etc.; present is in
  `Engine/Metal4Renderer.swift:452-458`:
  `commandQueue.waitForDrawable(drawable)` → `commandQueue.commit([commandBuffer])` →
  `commandQueue.signalDrawable(drawable)` → `drawable.present()`; frame-completion via
  `commandQueue.signalEvent(frameCompletionEvent, value:)` (`:458`) and the layer's residency set is
  attached to the queue (`:292`). `MetalView.swift` is an `MTKView` wrapper.

### 3.5 MoltenVK — the vocabulary of "no Metal equivalent"
- `MoltenVK/README.md:396-407` — the three compliance categories; "**Non-compliance** … no practical,
  or reasonably performant, mechanism"; points to `Docs/MoltenVK_Runtime_UserGuide.md#limitations`.
- `MoltenVK/Docs/MoltenVK_Runtime_UserGuide.md:681-700` — Known limitations: `VK_QUERY_TYPE_PIPELINE_STATISTICS`
  unsupported; `VkAllocationCallbacks` ignored; PVRTC loading.
- `MoltenVK/Docs/Whats_New.md` — portability-subset feature tweaks (`:476`, `:1847`).

---

## 4. Impossible / not-recommended

*Verdict "impossible" = no Metal API path exists; "not-recommended" = expressible but a known trap.*

1. **Geometry-shader stage — impossible; low RSX impact.** Metal has no geometry stage; only
   vertex+fragment (+ mesh/object/tile). *Verified:* Metal mesh-shader docs; Unity: "Metal doesn't
   support Hull, Domain, or Geometry shader stages … uses a compute shader". RSX never exposes
   geometry shaders to games (fixed VP/FP model), so this is a non-issue **for RSX emulation** — but it
   is why MoltenVK fails conformance, and it does bite *helper* geometry passes.
2. **Tessellation stages (hull/domain) — impossible in Metal 4; low RSX impact.** `MTL4RenderCommandEncoder`
   has **no tessellation methods** (metal-4-basics comment thread confirms; Apple's guide to Metal 4
   doesn't list tessellation). Metal 3 had a tessellation path; Metal 4 routes to mesh shaders. RSX has
   no tessellation → not a functional loss for RPCS3.
3. **Transform feedback (`VK_EXT_transform_feedback`) — impossible.** No Metal equivalent; MVK does not
   implement it. RSX games cannot invoke it. Only relevant if a future feature (e.g. some compute
   emulation) wanted it.
4. **`VkRenderPass` / `VkFramebuffer` objects — impossible (as objects).** Metal has no pass/framebuffer
   object; use per-encoder `MTL4RenderPassDescriptor` + `loadAction`/`storeAction`. Not "impossible" to
   *do*, but the design **must** change (biggest conceptual gap, doc 03:108 / doc 05:53).
5. **`VK_EXT_external_memory_host` (host-pointer import) — impossible.** Apple exposes no API to import an
   arbitrary host pointer as a `MTLBuffer`; `newBufferWithBytesNoCopy:` requires page-aligned,
   well-behaved memory. RPCS3's `supports_passthrough_dma` fast path (`VKGSRender.cpp:798`) has no native
   equivalent (doc 03:101). *Inference:* guest backing on unified memory can still be *copied* cheaply.
6. **Positional sparse binding (`VkSparseImageMemoryBind` at an exact offset) — impossible.**
   Metal sparse heaps don't let you place tiles at specific locations (MoltenVK #1700: "Metal doesn't
   let you place sparse tiles at specific locations inside of sparse heaps, whereas Vulkan and D3D12 do").
   Metal 4 "placement sparse" is heap-relative, not address-relative.
7. **`VK_EXT_conditional_rendering` — no Metal equivalent.** MVK synthesizes it. RPCS3 already sets
   `supports_hw_conditional_render=false` on MVK (`VKGSRender.cpp:795`) and emulates via CPU sync; a native
   backend inherits that.
8. **`VK_QUERY_TYPE_PIPELINE_STATISTICS` — impossible.** Explicitly unsupported by MVK; no Metal analog.
9. **Explicit Vulkan memory model (`VK_KHR_vulkan_memory_model`, `Coherent`/`Volatile` semantics) — not
   recommended to rely on.** Metal 4 makes **all resources untracked** and expects explicit
   stage-to-stage barriers; the coherency semantics differ and the MSL compiler has documented
   miscompiles (KK_WORKAROUND_6/15).
10. **`VkAllocationCallbacks` — impossible.** MVK ignores them; Metal has no allocator hooks.

---

## 5. Tricky (ranked)

Ranked by (risk × likelihood of biting) for the **presentation scope**; cross-scope items are in §9.

1. **Frame pacing / VRR / tri-state vsync has no faithful Metal mapping (diff 4).**
   RPCS3's `vsync_mode` (`system_config_types.h:390`) maps to FIFO/IMMEDIATE/MAILBOX/FIFO_RELAXED.
   `CAMetalLayer` only offers `displaySyncEnabled: Bool` plus `presentAfterMinimumDuration:` /
   `presentAtTime:`. There is no true MAILBOX/tear-free-uncapped mode. Also `CAMetalDisplayLink`
   (the macOS `CADisplayLink`) is the modern way to pace; `MTKView` is not used. **Verified** Godot maps
   MAILBOX/ADAPTIVE/ENABLED all to `displaySyncEnabled=true` and DISABLED to `false`
   (`rendering_context_driver_metal.cpp:188-198`). *Could not verify* a `CAMetalDisplayLink` C++ binding
   in the shipped metal-cpp.
2. **`nextDrawable` blocks; resize/minimize/pause can deadlock the flip thread (diff 3).**
   The RPCS3 loop already knows "minimized window → `VK_ERROR_OUT_OF_DATE_KHR`" (`VKPresent.cpp:43-50`).
   On Metal, `nextDrawable` blocks if no drawable is free and `allowsNextDrawableTimeout=false`
   (zed #53390 hang); Godot sets it `true`. RPCS3's `flip()` runs on the RSX thread; blocking there
   stalls emulation. Need a timeout path + a "window minimized" early-out mirroring `VKPresent.cpp:46`.
3. **Swapchain recreate is a hard-sync drain today, absent natively (diff 3).**
   `reinitialize_swapchain()` calls `vkDeviceWaitIdle` (`VKPresent.cpp:85`) and rebuilds FBO caches.
   Metal has no swapchain object to recreate; you resize the layer and must ensure **in-flight drawables
   are released** before changing `drawableSize`/`pixelFormat`. This is the trickiest *state machine*.
4. **`maximumDrawableCount ≤ 3` collides with RPCS3's triple-buffer assumption (diff 2-3).**
   `nb_swap_images = minImageCount + 2` (`swapchain.cpp:279`) becomes `m_max_async_frames`
   (`VKPresent.cpp:112`). Metal caps at 3 (Godot clamps). RPCS3's `m_queued_frames`/`m_aux_frame_context`
   logic (`VKGSRender.h:167-175`) works but the async depth is reduced.
5. **Screenshot/recording readback vs `framebufferOnly` (diff 3).**
   RPCS3 reads back the presented image (`VKPresent.cpp:752-753`). `CALayer.framebufferOnly=true` (Godot's
   choice) forbids reading the drawable texture → screenshots/recording must blit from a non-framebuffer-only
   path (e.g. render to an owned texture first, then blit to the drawable) or set `framebufferOnly=false`.
6. **Overlay & calibration compositing into a float16/EDR drawable (diff 3).**
   `video_out_calibration_pass` gamma/range (`VKOverlays.h:208`) and overlays (`VKPresent.cpp:889-976`)
   must respect the drawable colorspace. If EDR is enabled the pixel format is RGBA16Float and the
   overlay pipeline must match; if not, BGRA8Unorm + sRGB.
7. **`CAMetalLayer` creation/ownership from `winId()` (diff 3).**
   `metal_layer.mm:9` returns `((NSView*)view).layer` and **assumes the view already has a layer**. RPCS3's
   `gs_frame` is a bare `QWindow`; Qt 6 may or may not create a backing layer, and it may be shared with
   other Qt content. The native backend likely needs `[view setWantsLayer:YES]` + a dedicated `CAMetalLayer`
   (and to disable Qt's own rendering into that region).
8. **MetalFX temporal scaler is not a drop-in for FSR1 (diff 3-4).**
   RPCS3 uses **FSR1 spatial** (`fsr_pass.h:49`, no motion vectors/depth). `MTLFXSpatialScaler` is the
   drop-in. `MTLFXTemporalScaler`/`MTL4FXTemporalScaler` require **color + depth + motion vectors + jitter**
   (WWDC22 10103, WWDC25 211) which RSX does not produce. Frame interpolation is even further out.
9. **Headless / native software-present path is unimplemented (diff 3).**
   `native_swapchain_base` (`swapchain_core.h:117`) throws for `init/create/present/destroy`
   (`:125-143`) and is aliased as `swapchain_MacOS` (`swapchain_macos.hpp:9`). A native Metal backend
   needs a real implementation for the "present not possible" case (`instance.cpp:366-370`) and for
   `is_headless()` / DMA readback (`swapchain.cpp:21-49`).
10. **Stereo/multi-view present (diff 4).** RSX stereo (`VKPresent.cpp:560-580`) has no Metal "stereo
    swapchain"; requires two drawables or a layered texture + viewport split, and a matching output
    shader. Low frequency, high effort.
11. **Display timing for emulated vsync (diff 3).** `client_display_rate()` (`gs_frame.cpp:805`) uses
    `QScreen::refreshRate()`; VRR/ProMotion means the rate is dynamic. Emulated RSX flip timing should
    use `CAMetalDisplayLink`/`presentedTime` rather than a static refresh rate.
12. **`presentAtTime:` / `presentAfterMinimumDuration:` vs the game's own frame limiter (diff 2-3).**
    RPCS3 already throttles; double-pacing (Metal min-duration + RPCS3 limiter) causes stutter. Must pick
    one owner of pacing.

---

## 6. Minimal Metal 4 code sketch (metal-cpp)

Objective-C++ (`.mm`), metal-cpp, macOS 26+, Apple-Silicon. **Inference** (compiled mentally from the
metal-cpp headers + WWDC sample); not built.

```objc++
// mtl_swapchain.h  — the presentation seam a native backend needs.
#import <Metal/Metal.hpp>          // MTL4::CommandQueue, MTL::Device, MTL::Drawable
#import <QuartzCore/QuartzCore.hpp> // CA::MetalLayer, CA::MetalDrawable

struct mtl_frame {
    CA::MetalDrawable* drawable = nullptr;   // acquired via layer->nextDrawable()
    MTL::SharedEvent*  frame_done = nullptr; // replaces the VkSemaphore pair
    uint64_t           frame_id   = 0;
};

class mtl_swapchain {
    CA::MetalLayer*        layer      = nullptr;  // attached to the QWindow's NSView
    MTL::Device*           device     = nullptr;
    MTL4::CommandQueue*    queue      = nullptr;  // device->newMTL4CommandQueue()
    MTL4::CommandAllocator* allocator = nullptr;  // device->newCommandAllocator()
    MTL::ResidencySet*     residency  = nullptr;  // layer->residencySet() + owned textures
    uint32_t width = 0, height = 0;

public:
    void create(void* nsview, MTL::Device* dev, MTL::PixelFormat fmt) {
        device  = dev;
        queue   = device->newMTL4CommandQueue();
        allocator = device->newCommandAllocator();

        layer = (CA::MetalLayer*)nsview;              // or [[NSView alloc] init]->layer after setWantsLayer
        layer->setDevice(device);
        layer->setPixelFormat(fmt);                   // BGRA8Unorm | RGBA16Float (EDR)
        layer->setFramebufferOnly(false);             // RPCS3 reads back for screenshots/recording
        layer->setMaximumDrawableCount(3);            // Metal max; RPCS3 wanted minImageCount+2
        layer->setAllowsNextDrawableTimeout(true);    // do NOT deadlock the RSX thread
        layer->setDisplaySyncEnabled(true);           // FIFO ↔ vsync on
        layer->setColorspace(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
        // EDR variant: setWantsExtendedDynamicRangeContent(true); kCGColorSpaceExtendedLinearSRGB;

        residency = layer->residencySet();            // macOS 26: layer's dynamic residency set
        queue->addResidencySet(residency);            // make drawables resident on the queue
    }

    void resize(uint32_t w, uint32_t h) {
        width = w; height = h;
        layer->setDrawableSize(CGSizeMake(w, h));     // replaces destroy/create swapchain
    }

    // Acquire, mimicking VKPresent.cpp:596's acquire loop (returns nullptr on timeout).
    CA::MetalDrawable* acquire() { return layer->nextDrawable(); }

    // Present, mimicking VKPresent.cpp:159 + the Metal 4 sample's 4-step dance.
    void present(MTL4::CommandBuffer* cb, CA::MetalDrawable* drawable, MTL::SharedEvent* done, uint64_t id) {
        queue->waitForDrawable(drawable);             // MTL4CommandQueue.hpp:280
        queue->commit(&cb, 1);                        // MTL4CommandQueue.hpp:126
        queue->signalDrawable(drawable);              // MTL4CommandQueue.hpp:140
        drawable->present();                          // MTLDrawable.hpp:47
        queue->signalEvent(done, id);                 // MTL4CommandQueue.hpp:142 (frame ring recycle)
    }
};
```

Notes for the human: (a) `CA::MetalDrawable::texture()` (`CAMetalDrawable.hpp:39`) is the present target;
(b) `MTL4::CommandBuffer` is created once per frame from the allocator and reset via
`beginCommandBuffer(allocator:)` (`MTL4CommandBuffer.hpp:64`); (c) allocators must not be reused while
in flight (metalbyexample); (d) `framebufferOnly=false` costs bandwidth — consider a dedicated owned
texture + blit-to-drawable only when screenshots are active.

---

## 7. Open questions for the human

1. **`CAMetalLayer` ownership from Qt.** Does the RPCS3 `QWindow`'s `NSView` already have a usable
   `CALayer` (`metal_layer.mm:9` reads `.layer`), or must the native backend create one? Does Qt 6 render
   into that view (conflict)?
2. **Pacing owner.** Should the backend own pacing (`presentAfterMinimumDuration`/`CAMetalDisplayLink`)
   or keep RPCS3's limiter as the single owner (risk of double-pacing)?
3. **`CAMetalDisplayLink` vs `MTKView` vs plain `nextDrawable` loop.** RPCS3 has no display-link concept;
   which is in scope for the spike? (No C++ wrapper verified.)
4. **Sync model for presents.** Use `waitForDrawable`/`signalDrawable` only, or add a `MTLSharedEvent`
   frame ring (as the Metal 4 sample does for the last N frames)? This interacts with scope-2 (submission).
5. **EDR/HDR in scope?** RSX output is SDR with a gamma calibration pass; do we expose EDR at all, or
   keep BGRA8Unorm + sRGB only?
6. **Upscaler target.** MetalFX spatial as the FSR1 replacement, or keep a hand-rolled FS-style pass? Is
   temporal (needs depth+motion) ever feasible for RSX?
7. **Screenshots/recording.** Accept `framebufferOnly=false` always, or path through an owned texture?
8. **Stereo present.** Is RSX stereo worth any design effort, or drop to a "unsupported, fall back"?
9. **Peer scopes.** `01-core-memory.md`…`04-images-formats-samplers-rtt.md` were not present; re-verify §9
   row provenance once they land.
10. **`MTL4Compiler` concurrency** (KK_WORKAROUND_11): one compiler per `MTLDevice` — does that constrain
   RPCS3's async pipeline-compile pool (`VKGSRender.cpp:700-705` uses N threads)?

---

## 8. Sources

**Apple / WWDC**
- Understanding the Metal 4 core API — <https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api>
- `MTL4CommandQueue.signalDrawable(_:)` — <https://developer.apple.com/documentation/metal/mtl4commandqueue/signaldrawable(_:)>
- `MTL4CommandQueue.waitForDrawable(_:)` — <https://developer.apple.com/documentation/metal/mtl4commandqueue/waitfordrawable(_:)>
- `MTL4CommandEncoder.barrier(afterQueueStages:beforeStages:visibilityOptions:)` — <https://developer.apple.com/documentation/metal/mtl4commandencoder/barrier(afterqueuestages:beforestages:visibilityoptions:)>
- `MTLDrawable` — <https://developer.apple.com/documentation/metal/mtldrawable>
- `CAMetalLayer` — <https://developer.apple.com/documentation/quartzcore/cametallayer>
- `CAMetalLayer.maximumDrawableCount` — <https://developer.apple.com/documentation/quartzcore/cametallayer/maximumdrawablecount>
- `CAMetalLayer.presentsWithTransaction` — <https://developer.apple.com/documentation/quartzcore/cametallayer/presentswithtransaction>
- `CAMetalDisplayLink`/`CADisplayLink` — <https://developer.apple.com/documentation/quartzcore/cadisplaylink>
- "Optimize for variable refresh rate displays" (WWDC21 10147) — <https://developer.apple.com/videos/play/wwdc2021/10147>
- "Metal Game Performance Optimization" (WWDC18 612) — <https://developer.apple.com/videos/play/wwdc2018/612>
- "Discover Metal 4" (WWDC25 205) — <https://developer.apple.com/videos/play/wwdc2025/205>
- "Explore Metal 4 games" (WWDC25 254) — <https://developer.apple.com/videos/play/wwdc2025/254>
- "Go further with Metal 4 games" (WWDC25 211) — <https://developer.apple.com/videos/play/wwdc2025/211>
- "Boost performance with MetalFX Upscaling" (WWDC22 10103) — <https://developer.apple.com/videos/play/wwdc2022/10103>
- MetalFX — <https://developer.apple.com/documentation/metalfx>
- "Explore HDR rendering with EDR" (WWDC21 10161) — <https://developer.apple.com/videos/play/wwdc2021/10161>
- "Using color spaces to display HDR content" — <https://developer.apple.com/documentation/metal/using-color-spaces-to-display-hdr-content>
- "Culling occluded geometry using the visibility result buffer" — <https://developer.apple.com/documentation/metal/culling-occluded-geometry-using-the-visibility-result-buffer>
- "Creating sparse heaps and sparse textures" — <https://developer.apple.com/documentation/metal/creating-sparse-heaps-and-sparse-textures>
- "Improving CPU performance by using argument buffers" — <https://developer.apple.com/documentation/metal/improving-cpu-performance-by-using-argument-buffers>
- Metal Feature Set Tables (PDF) — <https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf>
- "Capturing a Metal workload in Xcode" — <https://developer.apple.com/documentation/xcode/capturing-a-metal-workload-in-xcode>
- "Metal Best Practices Guide: Drawables" — <https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/Drawables.html>

**Third-party write-ups / issues**
- Metal by Example, "Getting Started with Metal 4" — <https://metalbyexample.com/metal-4/>
- Metal 4 Essentials (blakecrosley) — <https://blakecrosley.com/blog/metal-4-essentials>
- dev.to "WWDC 2025 – Discover Metal 4" — <https://dev.to/arshtechpro/wwdc-2025-discover-metal-4-23f2>
- MetalFX quality discussion — <https://www.reddit.com/r/macgaming/comments/1m4ysmo/metal_fx_the_worst_upscaler_in_2025/>
- Metal argument-buffer sampler limit (Chromium) — <https://issues.chromium.org/477317117>
- wgpu sampled-texture-limit issue — <https://github.com/gfx-rs/wgpu/issues/2898>
- "Tier 2 argument buffers above 500000" (limit discussion) — <https://www.reddit.com/r/macgaming/comments/v98n1r/apple_engineer_tier_2_argument_buffers_above/>
- Godot Metal backend (source, in refs) — <https://github.com/godotengine/godot/tree/master/drivers/metal>
- zed main-thread `nextDrawable` hang — <https://github.com/zed-industries/zed/issues/53390>
- MoltenVK README ("Non-compliance") — <https://github.com/KhronosGroup/MoltenVK/blob/main/README.md>
- MoltenVK user guide limitations — <https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md#limitations>
- MoltenVK sparse textures #1700 — <https://github.com/KhronosGroup/MoltenVK/issues/1700>
- MoltenVK geometry shaders #1524 — <https://github.com/KhronosGroup/MoltenVK/issues/1524>
- MoltenVK Metal 4 features #2560 — <https://github.com/KhronosGroup/MoltenVK/issues/2560>
- MoltenVK use survey #1085 — <https://github.com/KhronosGroup/MoltenVK/issues/1085>
- Metal mesh shaders (Apple WWDC22 10162) — <https://developer.apple.com/videos/play/wwdc2022/10162>
- Mesa KosmicKrisp docs — <https://docs.mesa3d.org/drivers/kosmickrisp.html>
- Mesa KosmicKrisp workarounds — <https://docs.mesa3d.org/drivers/kosmickrisp/workarounds.html>
- Cemu #444 (vertex stride) — <https://github.com/cemu-project/Cemu/issues/444>
- VK_EXT_transform_feedback — <https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_transform_feedback.html>
- VK_EXT_conditional_rendering sample — <https://docs.vulkan.org/samples/latest/samples/extensions/conditional_rendering/README.html>
- LunarG "The State of Vulkan on Apple – Oct. 2026" — <https://www.lunarg.com/the-state-of-vulkan-on-apple-oct-2026/>
- Dolphin Progress Report Jul–Aug 2022 — <https://dolphin-emu.org/blog/6I/>

**Local pristine trees** (`/tmp/metal4-refs/`): `mesa/`, `godot/`, `metal-cpp/`, `MoltenVK/`,
`metal-4-basics/` (paths cited inline).

**RPCS3 in-tree** (background): `docs/research/01-drivers-kosmickrisp-moltenvk.md`,
`03-rpcs3-backend-architecture.md`, `04-backend-selection-design.md`, `05-metal-backend-northstar.md`,
`07-metal4-and-api-constraints.md`, `08-prior-art-metal-backends.md`.

---

## 9. MASTER REGISTER — superseded

**The authoritative, peer-provenanced master register now lives in
[`README.md` §6 "Consolidated impossibility / not-recommended register"](README.md#6-consolidated-impossibility--not-recommended-register).**

The register that previously occupied this section was written **before** the peer scope docs
`01-core-memory.md`…`04-images-formats-samplers-rtt.md` existed, so **zero of its rows carried peer
provenance** and several of its claims (notably the blanket "external host-pointer import —
impossible", the render-pass→encoder ownership, and the sparse/host-mem grouping) are now known to
be imprecise or superseded. It has been removed from this doc to keep a single source of truth.

**What moved where** (old §9 rank → new `README.md` §6 row):

- Ranks **1, 2, 4, 5, 8, 9, 10, 14, 15** (render-pass/encoder, MSL codegen, memory model/barriers,
  argument tables/sampler cap, no-Metal-equivalent features, occlusion/conditional, sparse/external
  memory, shader interpreter, compute passes) → merged with peer evidence into rows **R1, R2, R3,
  R4, R5, R6, R12, R13, R32**.
- Ranks **6, 7, 20, 21, 22, 24, 25, 26, 27, 28** (compiler/driver bugs, per-title quirks,
  `MTL4Compiler` concurrency, TBDR tuning, fast-math, heap snapshot, pipeline/shader cache, MSAA
  sample-mask, Xcode capture, ML encoder) → rows **R10, R11, R37, R30** and the hygiene list.
- **Presentation/window-system rows that were genuinely 05-specific were moved too**, not dropped:
  old rank **11** (swapchain → `CAMetalLayer`, `maximumDrawableCount ≤ 3`, blocking `nextDrawable`)
  → row **R21**; rank **12** (present/sync model) → rows **R8/R42-mapping**; rank **13** (frame
  pacing / VRR) → row **R23**; rank **16** (screenshot readback vs `framebufferOnly`) → row
  **R28**; rank **17** (`CAMetalLayer` creation/ownership from Qt) → row **R22**; rank **18**
  (MetalFX is not a drop-in for FSR1) → row **R27**; rank **19** (headless/software-present
  swapchain) → **§5 rank 9** of this doc; rank **23** (stereo present) → row **R31**.

The detailed presentation analysis (§§1–8 of this doc) is unaffected and remains the per-scope
reference for scope 05.
