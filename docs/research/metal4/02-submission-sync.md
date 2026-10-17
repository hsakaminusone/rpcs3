# 2. Command encoding, submission, synchronization, render passes & queries

Owner/scope: Scope 2 | Status: draft | Date: 2026-10-15

> Read-only research note. No RPCS3 source, CMake or tests were modified to produce it. All RPCS3 symbols were read on 2026-10-15; another agent is editing `rpcs3/Emu/RSX/VK/**` concurrently, so **line numbers may drift — the symbol names are the stable reference**. Facts and inference are labelled where it matters. Sources are in §8.

## 1. Scope & RPCS3 inventory

**In scope:** how RPCS3's Vulkan renderer encodes GPU work into command buffers, submits it to queues, synchronizes it (pipeline barriers, fences, binary semaphores, events, queue-family handling, conditional rendering), how it models render passes and framebuffers, and how it implements GPU queries (occlusion). **Out of scope:** shader/NIR→MSL translation, descriptor/pipeline-layout design, image/buffer allocation, and swapchain construction (only acquire/present *synchronization* is touched here).

RPCS3's Vulkan backend is effectively a **single-queue, single-subpass, one-submit-fence-per-command-buffer** design. Everything below was found by grep in `rpcs3/Emu/RSX/VK/**` and is genuinely used (nothing invented).

### 1.1 Command buffers & pools
- `vkCreateCommandPool` with `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` — `vkutils/commands.cpp:17-22`.
- `vkAllocateCommandBuffers`, strictly `VK_COMMAND_BUFFER_LEVEL_PRIMARY` — `vkutils/commands.cpp:51-56`.
- `vkBeginCommandBuffer` + `VkCommandBufferInheritanceInfo` + `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT` — `vkutils/commands.cpp:87-113`.
- `vkResetCommandBuffer` / `vkEndCommandBuffer` — `vkutils/commands.cpp:74-78, 115-125`.
- One `vk::fence m_submit_fence` per command buffer, waited in `command_buffer::begin()` — `vkutils/commands.cpp:58, 89-97`.
- `vkCmdBindPipeline` / `vkCmdBindDescriptorSets` behind a two-entry state cache (`m_bound_pipelines`, `m_bound_descriptor_sets`) — `vkutils/commands.cpp:149-196`.
- `command_buffer::submit()` — `vkutils/commands.cpp:127-147`; asserts no open query and no hanging conditional render (`cb_has_open_query`, `cb_has_conditional_render`).

### 1.2 Queue submission
- `vkQueueSubmit(queue, 1, &info, fence)` under a global `g_submit_mutex` — `VKCommandStream.cpp:15-50`.
- `VkSubmitInfo` with `waitSemaphoreCount`/`pWaitSemaphores`/`pWaitDstStageMask`/`signalSemaphoreCount`/`pSignalSemaphores` — `VKCommandStream.cpp:32-43`.
- Deferred (MTRSX) path: `rsx::dma_manager().backend_ctrl(rctrl_queue_submit, packet)` when `g_cfg.video.multithreaded_rsx` — `VKCommandStream.cpp:52-69`; `submit_packet` deep-copies the `VkSubmitInfo` and retargets semaphore pointers — `VKCommandStream.h:15-49`.
- `driver_manager_t` EID-completion thread — `VKCommandStream.cpp:77-146`, `VKCommandStream.h:51-71`.
- Frame submit entry point `VKGSRender::close_and_submit_command_buffer()` — `VKGSRender.cpp:2510-2614`: closes any renderpass, ends any open query, optional `vkCmdUpdateBuffer` label release, `command_buffer::end()` + `tag()`, then submits **primary** (graphics queue) and, when async streaming is recording, a **secondary** (transfer queue) chained with a semaphore waited at `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` — `VKGSRender.cpp:2582-2611`.
- Swapchain: `vkAcquireNextImageKHR` — `vkutils/swapchain.cpp:204`; `vkQueuePresentKHR` — `vkutils/swapchain.cpp:359`. Frame slots `m_max_async_frames` / `m_queued_frames` — `VKGSRender.h:169-175`, `VKPresent.cpp:112-113`.

### 1.3 Synchronization objects
- `vk::fence`: `vkCreateFence` / `vkResetFences` / `vkDestroyFence` (`vkutils/sync.cpp:151-166, 168-172`), `vkWaitForFences` (`vkutils/sync.cpp:569`), plus an atomic `flushed` spin flag (`sync.h:21-36`).
- `vk::semaphore`: `vkCreateSemaphore` / `vkDestroySemaphore`, **binary only** (`vkutils/sync.cpp:192-208`). Frame acquire/present semaphores — `VKGSRenderTypes.hpp:177-194`.
- `vk::event` with three backends (`sync_backend::events_v1 | events_v2 | gpu_label`) — `vkutils/sync.h:38-56`, `sync.cpp:210-383`:
  - created with `VK_EVENT_CREATE_DEVICE_ONLY_BIT_KHR` for GPU-domain events_v2 — `sync.cpp:235-240`;
  - signal: `vkCmdSetEvent` / `_vkCmdSetEvent2KHR` — `sync.cpp:288-306`;
  - wait: `vkCmdWaitEvents` / `_vkCmdWaitEvents2KHR` — `sync.cpp:344-361`;
  - host: `vkSetEvent` / `vkResetEvent` / `vkGetEventStatus` — `sync.cpp:329-383`;
  - `gpu_label` fallback (AMD pre-Navi host events): a host-visible-coherent buffer written with `vkCmdFillBuffer` after a barrier — `sync.cpp:385-479`.
- **No timeline semaphores** anywhere (grep: no `VkSemaphoreType` / `VK_SEMAPHORE_TYPE_TIMELINE`).

### 1.4 Barriers & transitions
- `vkCmdPipelineBarrier` — `vkutils/barriers.cpp:35, 61, 79, 128`; `vkutils/image_helpers.cpp:216`; `sync.cpp:267, 472`; `VKPresent.cpp:904`.
- `_vkCmdPipelineBarrier2KHR` — `sync.cpp:257`.
- Helpers: `insert_image_memory_barrier`, `insert_buffer_memory_barrier`, `insert_global_memory_barrier`, `insert_texture_barrier` — `vkutils/barriers.cpp:10-135`; **all four call `vk::end_renderpass(cmd)` first** unless `preserve_renderpass` (`barriers.cpp:19-22, 46-49, 70-73, 93-96`).
- Types: `VkImageMemoryBarrier` / `VkBufferMemoryBarrier` / `VkMemoryBarrier`; sync2: `VkDependencyInfoKHR` / `VkMemoryBarrier2KHR` / `VkBufferMemoryBarrier2KHR` (`sync.cpp:311-326`, `VKTextureCache.cpp:350-355`).
- Image-layout state machine: `vk::image::current_layout`, `current_queue_family` — `vkutils/image.h:48-49`. Layouts actually used: `GENERAL`, `COLOR_ATTACHMENT_OPTIMAL`, `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`, `SHADER_READ_ONLY_OPTIMAL`, `TRANSFER_SRC/DST_OPTIMAL`, `PREINITIALIZED`, `ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT` (`VKRenderTargets.cpp:866`, `VKDraw.cpp:69`).
- `VK_QUEUE_FAMILY_IGNORED` on every barrier; queue families are tracked but there are **no ownership transfers** (grep: only `IGNORED`).
- `VK_DEPENDENCY_BY_REGION_BIT` — `VKPresent.cpp:904`, `VKDraw.cpp:1146` (programmable blending), `VKRenderPass.cpp:391` (input attachments).

### 1.5 Conditional rendering (host-gated)
- `_vkCmdBeginConditionalRenderingEXT` / `_vkCmdEndConditionalRenderingEXT`, `VkConditionalRenderingBeginInfoEXT`, predicate buffer `m_cond_render_buffer` — `VKDraw.cpp:1164-1168, 1420`; `VKGSRender.cpp:2548, 3039-3195`; scratch predicate fallback `VKGSRender.cpp:2313`.

### 1.6 Render passes & framebuffers
- `vkCreateRenderPass` with **one** subpass; attachments `loadOp = LOAD` / `storeOp = STORE` (color stencil load/store `DONT_CARE`; depth+stencil LOAD/STORE) — `VKRenderPass.cpp:341-405`.
- Renderpass cache keyed by a packed `u64` (`renderpass_key_blob`: color format, depth format, sample count, per-attachment layout blob, input-attachment mask) — `VKRenderPass.cpp`.
- One `VkSubpassDependency` **only when input attachments are present** (`COLOR_ATTACHMENT_OUTPUT → FRAGMENT_SHADER`, `COLOR_ATTACHMENT_WRITE → INPUT_ATTACHMENT_READ`, `VK_DEPENDENCY_BY_REGION_BIT`) — `VKRenderPass.cpp:377-393`.
- `vkCmdBeginRenderPass(..., VK_SUBPASS_CONTENTS_INLINE)` / `vkCmdEndRenderPass` — `VKRenderPass.cpp:460, 477`; **no `vkCmdNextSubpass`** (grep: zero hits).
- `vkCreateFramebuffer` — `vkutils/framebuffer_object.hpp:42`; `vkCreateImageView` — `vkutils/image.cpp:446`. FBO cache keyed on width/height/`ia_ref` in `VKFramebuffer.cpp`; renderpass opened/ended lazily (`begin_renderpass`/`end_renderpass`, `VKRenderPass.cpp:439+`).

### 1.7 Queries (occlusion)
- `VkQueryPool` of type `VK_QUERY_TYPE_OCCLUSION` — `VKQueryPool.h:46`; `vkCreateQueryPool` / `vkDestroyQueryPool` — `vkutils/query_pool.hpp:20-32`.
- `vkCmdResetQueryPool` — `VKQueryPool.cpp:96`; `vkCmdBeginQuery` / `vkCmdEndQuery` — `VKQueryPool.cpp:155, 160`.
- `vkGetQueryPoolResults` with `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT` — `VKQueryPool.cpp:19`; `vkCmdCopyQueryPoolResults` with `VK_QUERY_RESULT_WAIT_BIT` (indirect predicate) — `VKQueryPool.cpp:191`.
- Flags: `VK_QUERY_RESULT_PARTIAL_BIT` (`VKQueryPool.h:47`), `VK_QUERY_CONTROL_PRECISE_BIT` (`VKGSRender.cpp:604`).
- Pool manager with refcounted slots, reallocation and a "close renderpass before reading" note (TBDR) — `VKQueryPool.h:13-85`, `VKQueryPool.cpp`.
- **No timestamp queries** (grep: no `VK_QUERY_TYPE_TIMESTAMP`, no `vkCmdWriteTimestamp`), though `VKAsyncScheduler`/profiling could add them later.

### 1.8 Ring buffers & deferred submission
- `vk::data_heap : rsx::data_heap` — host-visible-coherent ring buffers, optional VRAM shadow, dirty-range tracking — `vkutils/data_heap.h:23-110` (base in `Emu/RSX/Common/ring_buffer_helper.h`).
- `vk::data_heap_manager` register/snapshot/restore/reset — `VKDataHeapManager.cpp:19-73`; per-frame snapshot in `frame_context_t::tag_frame_end` — `VKGSRenderTypes.hpp:185, 211-221`.
- `vk::AsyncTaskScheduler` (transfer-queue async texture streaming): `VK_MAX_ASYNC_COMPUTE_QUEUES=256`, own `command_pool`, event pool (16384) for GPU sync or semaphore pool for host-safe mode — `VKAsyncScheduler.h:8-61`, `VKAsyncScheduler.cpp:31-172`.

## 2. Vulkan/RSX → Metal 4 mapping table

| # | Vulkan / RSX construct (exact API or symbol) | Metal 4 equivalent | Metal 4 objects/methods | Difficulty (1-5) | Notes |
|---|---|---|---|---|---|
| 1 | `VkCommandPool` (`vkCreateCommandPool`, `TRANSIENT`\|`RESET_COMMAND_BUFFER`) | `MTL4CommandAllocator` | `device.newCommandAllocator()`, `reset()`, `allocatedSize()`, `beginCommandBuffer(allocator:)` | 2 | Pool → allocator that owns *encoding* memory. Reset per frame (triangle sample resets the allocator each frame). Flags have no equivalent. |
| 2 | `VkCommandBuffer` primary, one-time-submit | `MTL4CommandBuffer` (device factory, **reusable**) | `device.newCommandBuffer()`, `beginCommandBuffer(allocator:)`, `endCommandBuffer()` | 2 | 1:1 but CB is reusable and **unretained** (resources must outlive the CB). No queue ownership. |
| 3 | `vkBeginCommandBuffer` + `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT` | `MTL4CommandBufferOptions` (or none) | `beginCommandBuffer(allocator:options:)` | 2 | No usage flags; no inheritance. |
| 4 | `VkCommandBufferInheritanceInfo` | none | — | 1 | Set but unused (primary only). Drop. |
| 5 | `vkCmdBindPipeline` (state-cached) | per-encoder state set | `MTL4RenderCommandEncoder.setRenderPipelineState()`, `MTL4ComputeCommandEncoder.setComputePipelineState()` | 2 | Encoders are per-pass, so the cache buys little; re-bind per pass. |
| 6 | `vkCmdBindDescriptorSets` | `MTL4ArgumentTable` | `argumentTable.setAddress()/setTexture()/setSamplerState()`, encoder `setArgumentTable(_:atStages:)` | 4 | Different model (bindless-ish; set on table, not encoder). Design item, out of this scope. |
| 7 | `vkQueueSubmit(queue,1,&info,fence)` | `MTL4CommandQueue` commit | `queue.commit([cb], options:)`, `MTL4CommitOptions.addFeedbackHandler()` | 3 | Commit **is** the submit. Many CBs per commit; `fence` → feedback handler (or shared event). |
| 8 | `VkSubmitInfo.pWaitSemaphores` + `pWaitDstStageMask` | queue event wait | `MTL4CommandQueue.waitForEvent(event, value:)` (`wait(_:value:)`) | 3 | **Stage-gated waits are not expressible** — queue-level event only. |
| 9 | `VkSubmitInfo.pSignalSemaphores` | queue event signal | `MTL4CommandQueue.signalEvent(event, value:)` | 3 | Binary semaphore → monotonic event value. |
| 10 | `vkQueuePresentKHR` | drawable present | `MTL4CommandQueue.signalDrawable(drawable)`, `drawable.present()` | 3 | Present is drawable-centric; moves to the present subsystem but changes the tail of a frame's sync. |
| 11 | `vkAcquireNextImageKHR` | drawable acquire/wait | `MTL4CommandQueue.waitForDrawable(drawable)` (`wait(_:drawable:)`) + `CAMetalLayer.nextDrawable` | 4 | Swapchain subsystem, but the acquire semaphore disappears into the drawable. |
| 12 | `VkFence` (`vkCreateFence`/`vkResetFences`/`vkWaitForFences`) | commit feedback + host semaphore, or `MTLSharedEvent` poll | `MTL4CommitFeedback` (GPUStartTime/GPUEndTime/error), `MTLSharedEvent.signaledValue` | 3 | No CPU-waitable fence object. Per-CB `m_submit_fence` maps to a feedback handler or an event value. |
| 13 | `VkSemaphore` (binary) | `MTLEvent` (same device) / `MTLSharedEvent` (cross-device/CPU) | `signalEvent:value:`, `waitForEvent:value:`, `signaledValue` | 3 | Binary not native; use monotonic 64-bit values. |
| 14 | `VkEvent` (`vkCmdSetEvent[2KHR]`, `vkCmdWaitEvents[2KHR]`, `vkSetEvent`, `vkGetEventStatus`) | producer/consumer barriers + `MTLSharedEvent` for host | `barrier(afterStages:beforeQueueStages:...)`, `barrier(afterQueueStages:beforeStages:...)`, shared-event host signal/poll | 4 | GPU-domain split into barriers (Mesa does exactly this); host-domain via shared event. |
| 15 | `gpu_label` fallback (`vkCmdFillBuffer` + mapped buffer) | `MTLSharedEvent` / shared `MTLBuffer` | `setSignaledValue()`, `notify(_:atValue:)`, shared-storage buffer | 2 | Metal shares memory with the CPU natively (unified memory); the workaround is unneeded. |
| 16 | `vkCmdPipelineBarrier` (image/buffer/global) | queue + intra-pass barriers | `barrierAfterStages(_:beforeQueueStages:visibilityOptions:)`, `barrierAfterQueueStages(_:beforeStages:...)`, `barrierAfterEncoderStages(_:beforeEncoderStages:...)` | 4 | **The core redesign.** RPCS3 ends the renderpass for *every* barrier (`barriers.cpp`); Metal 4 expects producer/consumer pairs or intra-pass barriers. |
| 17 | `_vkCmdPipelineBarrier2KHR` + `VkDependencyInfoKHR` | same barrier API | same three methods + `MTL4VisibilityOptions` | 4 | Sync2 maps more cleanly than sync1 (explicit dep info). |
| 18 | `VkImageMemoryBarrier` (layout transition) | visibility option + load/store action | `MTL4VisibilityOptionDevice`, `MTLLoadAction`/`MTLStoreAction` | 3 | Most `VK_IMAGE_LAYOUT_*` transitions dissolve; "attachment ⇄ shader-read" becomes barrier + store/load actions. |
| 19 | `VkBufferMemoryBarrier` | same barrier calls | as #16 | 3 | No layouts; purely visibility. |
| 20 | `VkMemoryBarrier` | broad queue barrier | `barrierAfterStages(..., .all, ...)` / `barrierAfterQueueStages` with `MTLStages.all` | 3 | Correct but over-broad; narrow using the stage mapping. |
| 21 | `VK_QUEUE_FAMILY_IGNORED`, `image::current_queue_family`, ownership transfers | no-op | — | 1 | Single queue family; ownership is meaningless. Delete the tracking. |
| 22 | `VK_DEPENDENCY_BY_REGION_BIT` | no equivalent (per-tile TBDR) | — | 3 | Meaning changes; TBDR makes it implicit. |
| 23 | `insert_texture_barrier` feedback loop (`barriers.cpp:82-135`) | intra-pass barrier, else split pass + barrier | `barrierAfterEncoderStages(...)` / two encoders + producer+consumer barrier | 4 | TBDR **fragment constraint**: cannot wait on fragment/tile stages intra-pass. |
| 24 | `VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT` (programmable blending) | programmable blending via tile memory | shader `[[color(N)]]` read + same-pass store action | 4 | Or split the pass; changes RSX read-write-alias handling. |
| 25 | `_vkCmdBeginConditionalRenderingEXT` / `_vkCmdEnd...` | **none** — emulate | occlusion `MTLVisibilityResultMode` + `executeCommands(buffer:indirectBuffer:)`, or GPU compute skip | 5 | Metal has **no conditional render command**. Host predicate + occlusion result must be lowered. |
| 26 | `vkCreateRenderPass` + one subpass | `MTL4RenderPassDescriptor` per encoder | `commandBuffer.makeRenderCommandEncoder(descriptor:options:)`, `endEncoding()` | 3 | The `VkRenderPass` object disappears; the **encoder is the pass**. |
| 27 | `VkAttachmentDescription.loadOp`/`storeOp` | `MTLLoadAction` / `MTLStoreAction` | descriptor attachment `loadAction`/`storeAction`; dynamic `setColorStoreAction(_:index:)`, `setDepthStoreAction()`, `setStencilStoreAction()` | 2 | Near 1:1. RPCS3 always does LOAD/STORE; `dontCare` where output is unused. |
| 28 | `VkSubpassDependency` (input attachments, `VKRenderPass.cpp:384-392`) | intra-pass barrier | `barrierAfterEncoderStages(.fragment? ...)` — invalid for fragment on TBDR | 3 | Input-attachment dependency maps to an intra-pass barrier *unless* it is a fragment→fragment/DS read-write, which mandates a pass split. |
| 29 | `vkCmdBeginRenderPass` / `vkCmdEndRenderPass` (`VK_SUBPASS_CONTENTS_INLINE`) | encoder create / end | `makeRenderCommandEncoder(...)`, `endEncoding()` | 2 | Encode the pass inline; `VK_SUBPASS_CONTENTS_SECONDARY` is unused. |
| 30 | `vkCmdNextSubpass` | not used | — | 1 | RPCS3 never calls it; nothing to map. |
| 31 | `VkFramebuffer` (`vkCreateFramebuffer`) | none | `MTL4RenderPassDescriptor.colorAttachments[i].texture`, `depthAttachment`, `stencilAttachment` | 2 | FBO → descriptor attachments. Render targets are textures/texture-views. |
| 32 | `vkCreateImageView` | `MTLTextureViewPool` | `newTextureViewWithDescriptor:` | 2 | Lightweight views replace `VkImageView`. |
| 33 | `VK_QUERY_TYPE_OCCLUSION` + `vkCmdBeginQuery`/`vkCmdEndQuery` | visibility result buffer + per-draw mode | `renderPassDescriptor.visibilityResultBuffer`, `setVisibilityResultMode(_:offset:)` | 4 | Pass-scoped. A query that spans draws/passes must accumulate via `MTLVisibilityResultType` or manual sum. |
| 34 | `vkCmdResetQueryPool` | buffer reset + pass-start reset | visibility buffer is reset by the GPU at pass start; else `blit.fillBuffer` | 5 | Reset semantics differ — must be verified against real device behaviour. |
| 35 | `vkGetQueryPoolResults` (+ `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT`) | read mapped shared `MTLBuffer` after completion | `MTLBuffer.contents()` / `didModifyRange` (managed) | 3 | No async result struct; poll memory after the commit feedback fires. |
| 36 | `vkCmdCopyQueryPoolResults` (`WAIT_BIT`) → indirect predicate | GPU copy into predicate buffer | `MTL4ComputeCommandEncoder.copy(...)` or blit | 4 | Feeds the conditional-render emulation (#25). |
| 37 | `VkQueryResultFlags`, `VK_QUERY_CONTROL_PRECISE_BIT` | none | — | 2 | Flags are meaningless on Metal. |
| 38 | Timestamp queries (`VK_QUERY_TYPE_TIMESTAMP`) | `MTL4CounterHeap` (Timestamp) | `writeTimestamp(granularity:after:counterHeap:index:)`, `resolveCounterRange`, `invalidateCounterRange` | 2 | Not currently used by RPCS3; available if profiling is added. |
| 39 | `vkCreateQueryPool` pool + refcounted slot lifecycle | counter heap (timestamps) / plain `MTLBuffer` (occlusion) | `device.newCounterHeap(descriptor:)`, `MTLBuffer` shared storage | 2 | Manager logic (lifetime, reallocation) largely portable. |
| 40 | Ring buffers (`vk::data_heap`, host-visible-coherent) + `data_heap_manager` snapshot/restore | `MTLBuffer` with `MTLResourceStorageModeShared` | shared buffers on unified memory; CPU writes visible to GPU | 2 | Carry-over. On Apple silicon there is no staging/coherence dance; snapshot/restore logic mostly survives. |
| 41 | `vk::AsyncTaskScheduler` (transfer queue, event/semaphore pools) | second `MTL4CommandQueue` or compute encoders on the same queue | `MTL4CommandQueue` × N, `commit([cb])` batches | 4 | Metal 4 pushes toward batching many CBs into one queue; the async-streaming design should be re-thought, not ported. |

## 3. Pristine reference code

These are existing, working implementations of the *same* mapping — they are the strongest evidence for the table above.

### 3.1 Mesa KosmicKrisp — the canonical Vulkan→Metal **4** driver
`src/kosmickrisp/` implements a full Vulkan driver on Metal 4 and is the direct analogue of what an RPCS3 Metal-4 backend must do for this scope (read in the earlier session from a temp checkout of Mesa `main`; the workarounds doc was also captured then):

- `vulkan/kk_queue.c` — queue submission maps to `[MTL4CommandQueue commit:count:options:]`; Vulkan binary/timeline semaphores map to a monotonic `MTLSharedEvent` with `signalEvent:value:` / `waitForEvent:value:`; `VkFence` is implemented with `MTL4CommitOptions addFeedbackHandler:`.
- `vulkan/kk_event.c` — Vulkan events are modelled as a GPU-writable buffer holding a `VkResult`; `vkCmdSetEvent2` is an immediate write, `vkCmdWaitEvents2` is a **no-op** (ordering is carried by barriers, not by the event).
- `vulkan/kk_sync.c`, `vulkan/kk_cmd_pool.c` — semaphore/fence and command-pool equivalents.
- `vulkan/kk_cmd_buffer.c` — barrier translation and store-action selection; renderpass splits are inserted for MSAA, texture-write→attachment-read, depth/stencil-write→read, and storage-write→input-read.
- `bridge/mtl_encoder.m` — the exact Metal 4 calls: `barrierAfterStages:beforeQueueStages:visibilityOptions:` / `barrierAfterEncoderStages:...` with `MTL4VisibilityOptionResourceAlias` where needed; `setVisibilityResultMode:offset:` for occlusion; `writeTimestampWithGranularity:afterStage:intoHeap:atIndex:`; dynamic `setColorStoreAction:atIndex:` / `setDepthStoreAction:` / `setStencilStoreAction:`.
- `bridge/mtl_command_queue.m` — `commit:count:options:` with the feedback handler (GPUStartTime / GPUEndTime / error).
- `docs/drivers/kosmickrisp/workarounds.rst` — **18 mandatory workarounds** (cache coherency, early-discard, subgroup ballots, `MTL4Compiler` concurrency heap corruption, robustness, …). A native Metal backend inherits this known-broken list verbatim.

> Caveat: the Mesa host (`gitlab.freedesktop.org`) now sits behind an Anubis bot filter, so these files could not be re-fetched this session. Behaviour above is from the earlier read; **exact Mesa line numbers are not reproduced** for that reason.

### 3.2 Godot `drivers/metal` — same abstraction, Metal 3 *and* Metal 4
- `rendering_device_driver_metal.h/.cpp`: `SyncMode { HazardTracking, Barriers }`. Default **`Barriers`** uses untracked resources + explicit barriers + per-level **fences** (`_fences[2]` alternating by render-graph level); hazard-tracking mode uses `MTLResourceHazardTrackingModeTracked` and skips level fences. `render_pass_create` builds `MDSubpass`/`MDAttachment` and derives load/store actions from `firstUseSubpassIndex` / `lastUseSubpassIndex`. Timestamp queries are stubbed as a no-op in that path (contrast with Metal 4 counter heaps).
- `metal_objects_shared.h`: `MDRenderPass` / `MDAttachment` / `MDCommandBufferBase`, level-fence arrays, `MDRingBuffer` for transient allocations.
- Lesson: the *same* render-graph abstraction can back both an implicit-hazard-tracking API and an explicit-barrier API — useful precedent for hiding the Metal 4 barrier model behind RPCS3's existing `vk::` helpers.

### 3.3 metal-cpp MTL4 headers — authoritative signatures (re-fetched 2026-10-15)
- `Metal/MTL4CommandEncoder.hpp`: `MTL4::VisibilityOptions { None=0, Device=1, ResourceAlias=1<<1 }`; `barrierAfterEncoderStages`, `barrierAfterQueueStages`, `barrierAfterStages` (all `(MTL::Stages, MTL::Stages, VisibilityOptions)`); `updateFence(_:afterEncoderStages:)`, `waitForFence(_:beforeEncoderStages:)`.
- `Metal/MTL4CommandQueue.hpp`: `commit(commandBuffers, count[, options])`; `signalEvent`/`wait(event,value)`; `signalDrawable`/`wait(drawable)`; `MTL4::CommitOptions::addFeedbackHandler(...)`; `CommandQueueDescriptor.feedbackQueue`.
- `Metal/MTL4RenderCommandEncoder.hpp`: `setVisibilityResultMode(mode, offset)`, `writeTimestamp(granularity, stage, counterHeap, index)`, `setColorStoreAction`, `setDepthStoreAction`, `setStencilStoreAction`, `setColorAttachmentMap`, `setArgumentTable`, `drawPrimitives(..., indirectBuffer)`, `executeCommandsInBuffer(...)`.
- `Metal/MTL4RenderPass.hpp`: `RenderPassDescriptor.visibilityResultBuffer` / `visibilityResultType`, `colorAttachments` (an `MTL::RenderPassColorAttachmentDescriptorArray`), `depthAttachment`/`stencilAttachment`, `setSupportColorAttachmentMapping`, `tileWidth`/`tileHeight`.

### 3.4 Apple "Drawing a triangle with Metal 4" sample
Canonical lifecycle and the one fact that shapes §1: **one `MTL4CommandAllocator` and one reusable `MTL4CommandBuffer` per in-flight frame**; each frame does `[frameAllocator reset]; [commandBuffer beginCommandBufferWithAllocator:frameAllocator]` → encode → `endCommandBuffer` → `commit`. `[device supportsFamily:MTLGPUFamilyMetal4]` selects the path.

### 3.5 MoltenVK #2560 (maintainer notes)
Explicit statements that Metal 4 **removed automatic hazard tracking for MTL4 pipelines/command encoders**, that explicit barrier methods replace it, and that **residency sets are the only residency mechanism** for MTL4 work.

## 4. Impossible / not-recommended

**Impossible (no Metal 4 equivalent):**
1. **Conditional rendering commands.** There is no `vkCmdBeginConditionalRenderingEXT` analogue (#25). The only GPU-side predicate primitive is `executeCommandsInBuffer(_:indirectBuffer:)` (indirect command buffer) plus the occlusion visibility buffer. RPCS3's host-written predicate buffer `m_cond_render_buffer` (`VKGSRender.cpp:3039-3195`) must be lowered to a compute that writes an indirect-command range, or to host-side skipping.
2. **Stage-gated semaphore waits** (`VkSubmitInfo.pWaitDstStageMask`, #8). `MTL4CommandQueue` events gate the queue, not a stage. The RPCS3 async-streaming wait at `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` (`VKGSRender.cpp:2597`) has no 1:1 form.
3. **Binary semaphores** (#13) and **`VkEvent` as an object** (#14). Only monotonic `MTLEvent`/`MTLSharedEvent` exist.
4. **`VK_DEPENDENCY_BY_REGION_BIT` semantics** (#22) and subpass self-dependencies. TBDR forbids intra-pass fragment→fragment waits, so these must become pass splits.
5. **Queue-family ownership transfers.** Meaningless — single queue family (#21).

**Not recommended:**
- **Encoding one Metal render encoder per RPCS3 draw.** RPCS3 already treats begin/end renderpass as cheap state (`VKRenderPass.cpp:439-477`); on TBDR each encoder boundary flushes tile memory. Keep passes open and emit barriers only where RPCS3 already ends a pass.
- **`MTL4VisibilityOptionResourceAlias` for ordinary aliasing.** Apple states `Device` suffices for normal placement-heap aliasing; `ResourceAlias` is only for simultaneously-aliased virtual addresses and may flush to system memory.
- **Porting `VKAsyncScheduler`'s transfer-queue + 16384-event pool verbatim.** Metal 4 wants many command buffers batched into few commits on one queue; the async-streaming model should be re-derived (see §5, item 6).
- **Emulating `VK_QUERY_CONTROL_PRECISE_BIT`.** It has no effect on Metal; drop it.

## 5. Tricky (ranked)

Ranked hardest → easiest; difficulty tag in the same 1-5 scale.

1. **Barrier model redesign** *(5)*. RPCS3 has three barrier shapes (`image`, `buffer`, `global`) all lowered to a single `vkCmdPipelineBarrier`, and **every one of them ends the open renderpass** (`vkutils/barriers.cpp:19-96`). Metal 4 requires choosing between *producer* (`barrierAfterStages:beforeQueueStages:`), *consumer* (`barrierAfterQueueStages:beforeStages:`), and *intra-pass* (`barrierAfterEncoderStages:beforeEncoderStages:`) barriers, plus a `MTL4VisibilityOption`. The source stage determines the producer mask, the consumer usage determines the consumer mask, and there is no "shader resource" umbrella stage (Apple's mapping tables in §8 must be followed literally). The TBDR fragment constraint furthermore forbids `afterEncoderStages` containing `.fragment`/`.tile` on a render encoder, which forces pass splits — exactly the case RPCS3's feedback-loop barrier (`insert_texture_barrier`) hits.
2. **Conditional rendering / occlusion predicates** *(5, tied)*. See §4 items 1–2. This is a semantic redesign, not a translation.
3. **Occlusion query emulation** *(4)*. `vkCmdBeginQuery`/`EndQuery` are draw-range operations with a reset/pool lifecycle; Metal's visibility buffer is **render-pass-scoped**, reset by the GPU at pass start, and selected per-draw with `setVisibilityResultMode(_:offset:)`. A query spanning draws is fine; a query spanning passes needs `MTLVisibilityResultType` accumulation or a manual sum. Reading the result (`vkGetQueryPoolResults` + availability) becomes reading a shared `MTLBuffer` after the commit feedback fires. The manager's reallocation/refcount logic (`VKQueryPool.h:13-85`) is the portable part.
4. **Semaphore / fence / event model** *(4)*. Binary `VkSemaphore`, `VkFence`, and `VkEvent` all collapse onto `MTLEvent`/`MTLSharedEvent` with monotonic values plus the **commit feedback handler** for CPU completion. RPCS3's per-command-buffer `m_submit_fence` (`vkutils/commands.cpp:58`) and the frame acquire/present semaphores (`VKGSRenderTypes.hpp:177-194`) must be re-expressed as event values + feedback handlers. The `gpu_label`/events_v1/v2 machinery (`vkutils/sync.cpp`) is deletable.
5. **Render pass & subpass dissolution** *(4)*. `VkRenderPass` + `VkFramebuffer` become one `MTL4RenderPassDescriptor` per encoder. Input-attachment dependencies (`VKRenderPass.cpp:384-392`) become intra-pass barriers — **unless** they are fragment→fragment or depth/stencil read-write, which require a pass split. `VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT` (programmable blending, `VKRenderTargets.cpp:866`) maps to Metal programmable blending or a split. The packed `u64` renderpass key remains useful as the descriptor cache key.
6. **Async streaming / queue topology** *(4)*. `VKAsyncScheduler` uses a second (transfer) queue and a large event/semaphore pool; Metal 4 encourages batching many command buffers into one `commit`. Decide whether the async stream becomes a second `MTL4CommandQueue` joined by an event, or compute encoders on the same queue with producer/consumer barriers.
7. **Ring buffers & deferred submission carry-over** *(2)*. `vk::data_heap`'s snapshot/restore (`VKDataHeapManager.cpp`, `frame_context_t::tag_frame_end`) is control logic that survives; the *coherence* handling is unnecessary on unified memory (`MTLResourceStorageModeShared`). The real change is that `MTL4CommandAllocator::reset()` + reusable command buffers replace `vkResetCommandBuffer`/one-time-submit (`vkutils/commands.cpp:74-113`), and MTL4 command buffers hold **unretained** references — resource lifetime must be managed by the backend.
8. **Command buffer lifecycle plumbing** *(2)*. Mechanical: `MTL4CommandAllocator` ↔ `command_pool`, reusable `MTL4CommandBuffer` ↔ `vk::command_buffer`, `begin/end` ↔ same. Centralize it and the rest of §1 ports cleanly.

## 6. Minimal Metal 4 code sketch (metal-cpp)

Illustrative only — shows the shape of the mapping for one frame (allocator → CB → encoder → commit/feedback event → barriers → renderpass descriptor with load/store + visibility buffer → counter-heap timestamp). Names follow `metal-cpp` MTL4 headers.

```cpp
#include <Metal/Metal.hpp>
#include <Metal/MTL4.hpp>

// ---- persistent objects -------------------------------------------------
MTL::Device*            dev   = MTL::CreateSystemDefaultDevice();
MTL4::CommandQueue*     queue = dev->newCommandQueue();          // or newMTL4CommandQueue
MTL::SharedEvent*       frameEvent = dev->newSharedEvent();      // CPU<->GPU handoff
uint64_t                frameValue = 0;

// one allocator + one reusable CB per in-flight frame (triangle-sample lifecycle)
MTL4::CommandAllocator* alloc = dev->newCommandAllocator();
MTL4::CommandBuffer*    cb    = dev->newCommandBuffer();

// occlusion: one shared visibility buffer (sample counts), queried by offset
MTL::Buffer* visBuf = dev->newBuffer(8 * kMaxOcclusionQueries,
                                     MTL::ResourceStorageModeShared);

// timestamps: opaque counter heap, resolves to an MTLBuffer
MTL4::CounterHeapDescriptor* chd = MTL4::CounterHeapDescriptor::alloc()->init();
chd->setType(MTL4::CounterHeapTypeTimestamp);
chd->setCount(kTimestampSlots);
MTL4::CounterHeap* tsHeap = dev->newCounterHeap(chd, nullptr);

// ---- per frame ----------------------------------------------------------
void encode_frame(MTL::Texture* color, MTL::Texture* depth /*, ...*/)
{
    alloc->reset();
    cb->beginCommandBuffer(alloc);

    // 1) Render pass descriptor: the "VkRenderPass + VkFramebuffer" replacement.
    MTL4::RenderPassDescriptor* rp = MTL4::RenderPassDescriptor::alloc()->init();
    auto* ca = rp->colorAttachments()->object(0);
    ca->setTexture(color);
    ca->setLoadAction(MTL::LoadActionLoad);      // matches VK_ATTACHMENT_LOAD_OP_LOAD
    ca->setStoreAction(MTL::StoreActionStore);   // matches VK_ATTACHMENT_STORE_OP_STORE
    rp->depthAttachment()->setTexture(depth);
    rp->depthAttachment()->setLoadAction(MTL::LoadActionLoad);
    rp->depthAttachment()->setStoreAction(MTL::StoreActionStore);
    rp->setVisibilityResultBuffer(visBuf);       // occlusion accumulator

    MTL4::RenderCommandEncoder* enc =
        cb->renderCommandEncoder(rp, MTL4::RenderEncoderOptionNone);

    // Consumer barrier: earlier passes' writes visible before we fragment.
    enc->barrierAfterQueueStages(MTL::StageFragment, MTL::StageFragment,
                                 MTL4::VisibilityOptionDevice);

    // Occlusion query "begin": counting into visBuf at slot*8.
    enc->setVisibilityResultMode(MTL::VisibilityResultModeCounting, /*offset*/ 0);
    enc->setRenderPipelineState(/* pso */);
    // enc->drawPrimitives(...);
    enc->setVisibilityResultMode(MTL::VisibilityResultModeDisabled, 0); // "end query"

    // Intra-pass barrier example (valid stages only; fragment waits are illegal on TBDR).
    enc->barrierAfterEncoderStages(MTL::StageDispatch, MTL::StageDispatch,
                                   MTL4::VisibilityOptionDevice);

    // Optional timestamp (RPCS3 doesn't use these today).
    enc->writeTimestamp(MTL4::TimestampGranularityPrecise, MTL::RenderStageFragment,
                        tsHeap, 0);

    // "vkCmdCopyQueryPoolResults WAIT_BIT" stand-in: resolve timestamps to a buffer.
    cb->resolveCounterHeap(tsHeap, {0, 2}, MTL4::BufferRange{ tsOut, 0, 2*8 },
                           nullptr, nullptr);

    enc->endEncoding();          // == vkCmdEndRenderPass

    // Producer barrier: this pass's fragment output visible to later passes.
    // (Emit on the outgoing encoder before endEncoding in real code.)

    cb->endCommandBuffer();

    // ---- submit: vkQueueSubmit + VkFence in one call --------------------
    MTL4::CommitOptions* opts = MTL4::CommitOptions::alloc()->init();
    opts->addFeedbackHandler([&](MTL4::CommitFeedback* fb) {
        // IMTL4CommitFeedback: fb->gpuStartTime(), fb->gpuEndTime(), fb->error()
        uint64_t* vis = (uint64_t*)visBuf->contents();     // occlusion results
        (void)vis; (void)fb;
        frameEvent->setSignaledValue(frameValue);          // replaces VkFence signal
    });

    MTL4::CommandBuffer* cbs[] = { cb };
    queue->commit(cbs, 1, opts);       // == vkQueueSubmit(..., fence)

    // Cross-queue / CPU wait: timeline event instead of a binary semaphore.
    frameValue++;
    queue->signalEvent(frameEvent, frameValue);
    // queue->wait(frameEvent, value)  ==  VkSubmitInfo.pWaitSemaphores (queue-level only)
}
```

Key differences from the Vulkan path worth noting in the sketch: the render pass is a *descriptor*, store actions are set on the descriptor (and optionally dynamically), the occlusion "query" is an offset into a shared buffer with per-draw mode switching, and the fence is the **feedback handler** rather than a `VkFence`.

## 7. Open questions for the human

1. **Hazard-tracking strategy.** Do we port RPCS3's explicit `vk::` barrier helpers 1:1 (translate each `vkCmdPipelineBarrier` call site), or do we build a deferred, resource-state-tracking barrier pass (as Apple's skill and D3D12 ports recommend)? The latter is more work but matches Metal 4's model and the async/threaded command generation. *Recommendation to discuss, not a decision.*
2. **How much to preserve `VK_IMAGE_LAYOUT_*`.** Since layouts mostly dissolve, do we keep `vk::image::current_layout` as a *logical* state used only to choose barriers/store actions, or delete it — and if we delete it, what replaces the decision logic in `VKRenderTargets.cpp` / `VKDraw.cpp`?
3. **Conditional rendering.** Is host-side skipping acceptable for bring-up (cheap, correctness-first) with a follow-up indirect-command-buffer path, or must the first cut be GPU-side? RPCS3's predicate is sometimes an occlusion-query result (`VKGSRender.cpp:3098`), which forces a GPU path for exact behaviour.
4. **Occlusion reset semantics.** Confirm on real hardware whether `visibilityResultBuffer` is zeroed at pass start (Apple/ANGLE docs say yes) and how RPCS3's pool-reuse/`vkCmdResetQueryPool` pattern maps when a query spans multiple render passes. This is not fully verified here.
5. **Async streaming topology.** One `MTL4CommandQueue` with many batched CBs, or a dedicated second queue for the async stream? This affects `VKAsyncScheduler`, `VKDataHeapManager`, and the semaphore→event change together.
6. **Fence replacement policy.** Per-command-buffer feedback handler vs. a per-frame `MTLSharedEvent` value. Feedback handlers are per-commit; RPCS3 currently has one fence per command buffer (`vkutils/commands.cpp:58`) and waits on it in `begin()`.
7. **`MTL4CommandAllocator` reset cadence.** The triangle sample resets one allocator per frame; RPCS3 creates many command buffers per frame (primary + secondary + async). Is one allocator per frame still correct, or one per "submit scope"?
8. **Where does `vk::AsyncScheduler`'s 16384-event pool go?** Metal events are cheap object handles; is a pool still wanted, or do we allocate per-stream events?

## 8. Sources

Apple documentation (Metal 4 core API, sync, passes, queries) — fetched 2026-10-15:
- Understanding the Metal 4 core API — <https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api>
- `MTL4CommandQueue` — <https://developer.apple.com/documentation/metal/mtl4commandqueue>
- `MTL4CommandBuffer` — <https://developer.apple.com/documentation/metal/mtl4commandbuffer>
- `MTL4CommandAllocator` — <https://developer.apple.com/documentation/metal/mtl4commandallocator>
- `MTL4CommandEncoder` — <https://developer.apple.com/documentation/metal/mtl4commandencoder>
- `barrier(afterEncoderStages:beforeEncoderStages:visibilityOptions:)` — <https://developer.apple.com/documentation/metal/mtl4commandencoder/barrier(afterencoderstages:beforeencoderstages:visibilityoptions:)>
- `barrier(afterQueueStages:beforeStages:visibilityOptions:)` — <https://developer.apple.com/documentation/metal/mtl4commandencoder/barrier(afterqueuestages:beforestages:visibilityoptions:)>
- `barrier(afterStages:beforeQueueStages:visibilityOptions:)` — <https://developer.apple.com/documentation/metal/mtl4commandencoder/barrier(afterstages:beforequeuestages:visibilityoptions:)>
- `MTL4VisibilityOptions` — <https://developer.apple.com/documentation/metal/mtl4visibilityoptions>
- `MTL4CommitOptions` — <https://developer.apple.com/documentation/metal/mtl4commitoptions>
- `MTL4CommitFeedback` — <https://developer.apple.com/documentation/metal/mtl4commitfeedback>
- `MTL4RenderCommandEncoder` — <https://developer.apple.com/documentation/metal/mtl4rendercommandencoder>
- `MTL4RenderPassDescriptor` — <https://developer.apple.com/documentation/metal/mtl4renderpassdescriptor>
- `MTL4CounterHeap` — <https://developer.apple.com/documentation/metal/mtl4counterheap>
- `writeTimestamp(granularity:after:counterHeap:index:)` — <https://developer.apple.com/documentation/metal/mtl4rendercommandencoder/writetimestamp(granularity:after:counterheap:index:)>
- Resource synchronization — <https://developer.apple.com/documentation/metal/resource-synchronization>
- Synchronizing stages within a pass — <https://developer.apple.com/documentation/metal/synchronizing-stages-within-a-pass>
- Synchronizing passes with producer barriers — <https://developer.apple.com/documentation/metal/synchronizing-passes-with-producer-barriers>
- Synchronizing passes with consumer barriers — <https://developer.apple.com/documentation/metal/synchronizing-passes-with-consumer-barriers>
- `MTLStages` — <https://developer.apple.com/documentation/metal/mtlstages>
- `MTLStoreAction` / `MTLLoadAction` — <https://developer.apple.com/documentation/metal/mtlstoreaction>, <https://developer.apple.com/documentation/metal/mtlloadaction>
- Setting load and store actions — <https://developer.apple.com/documentation/metal/setting-load-and-store-actions>
- `MTLStorageMode.memoryless` — <https://developer.apple.com/documentation/metal/mtlstoragemode/memoryless>
- `MTLEvent` / `MTLSharedEvent` — <https://developer.apple.com/documentation/metal/mtlevent>, <https://developer.apple.com/documentation/metal/mtlsharedevent>
- `MTLResidencySet` — <https://developer.apple.com/documentation/metal/mtlresidencyset>
- Render passes (API collection) — <https://developer.apple.com/documentation/metal/render-passes>
- Drawing a triangle with Metal 4 (sample) — <https://developer.apple.com/documentation/metal/drawing-a-triangle-with-metal-4>

Apple game-porting-toolkit skills (excellent, concrete cross-API mapping tables):
- `managing-metal4-synchronization/SKILL.md` — <https://github.com/apple/game-porting-toolkit/blob/main/game-porting-skills/skills/managing-metal4-synchronization/SKILL.md>
- `translating-to-metal4-api/references/gpu-queries-and-capabilities.md` — <https://github.com/apple/game-porting-toolkit/blob/main/game-porting-skills/skills/translating-to-metal4-api/references/gpu-queries-and-capabilities.md>
- `translating-to-metal4-api/references/tbdr-architecture.md` — <https://github.com/apple/game-porting-toolkit/blob/main/game-porting-skills/skills/translating-to-metal4-api/references/tbdr-architecture.md>

WWDC25:
- Discover Metal 4 (205) — <https://developer.apple.com/videos/play/wwdc2025/205>
- Explore Metal 4 games (254) — <https://developer.apple.com/videos/play/wwdc2025/254>
- Go further with Metal 4 games (211) — <https://developer.apple.com/videos/play/wwdc2025/211>
- WWDC25 overview / "Drawing a triangle with Metal 4" listed under updates — <https://developer.apple.com/documentation/updates/wwdc2025>

metal-cpp MTL4 headers (fetched 2026-10-15):
- `MTL4CommandEncoder.hpp` — <https://raw.githubusercontent.com/bkaradzic/metal-cpp/metal-cpp_27/Metal/MTL4CommandEncoder.hpp>
- `MTL4CommandQueue.hpp` — <https://raw.githubusercontent.com/bkaradzic/metal-cpp/metal-cpp_27/Metal/MTL4CommandQueue.hpp>
- `MTL4RenderCommandEncoder.hpp` — <https://raw.githubusercontent.com/bkaradzic/metal-cpp/metal-cpp_27/Metal/MTL4RenderCommandEncoder.hpp>
- `MTL4RenderPass.hpp` — <https://raw.githubusercontent.com/bkaradzic/metal-cpp/metal-cpp_27/Metal/MTL4RenderPass.hpp>

Prior art & cross-references:
- Mesa KosmicKrisp (Vulkan→Metal 4 driver) — <https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/kosmickrisp> (host blocked automated fetching on 2026-10-15; findings carried from an earlier read).
- KosmicKrisp workarounds doc — <https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/docs/drivers/kosmickrisp/workarounds.rst>
- Godot Metal driver — <https://github.com/godotengine/godot/tree/master/drivers/metal>
- MoltenVK "New features available in Metal 4" (#2560) — <https://github.com/KhronosGroup/MoltenVK/issues/2560>
- ANGLE occlusion queries in the Metal backend — <https://chromium.googlesource.com/angle/angle/+/main/src/libANGLE/renderer/metal/doc/OcclusionQueries.md>
- Blender Metal command-buffer manager (barrier/skip strategy) — <https://github.com/blender/blender/blob/main/source/blender/gpu/metal/mtl_command_buffer.mm>
- `MTL4RenderCommandEncoder` mirror (objc struct docs, encoder options) — <https://docs.rs/objc2-metal/0.3.2/aarch64-apple-visionos/src/objc2_metal/generated/MTL4RenderCommandEncoder.rs.html>
- `MTL4Counters.rs` mirror (counter heap / timestamp granularity enums) — <https://docs.rs/objc2-metal/0.3.2/aarch64-apple-ios-macabi/src/objc2_metal/generated/MTL4Counters.rs.html>
- Meta Horizon: loads/stores/passes & Vulkan subpass↔tile-shading — <https://developers.meta.com/horizon/documentation/unreal/po-advanced-gpu-pipelines/>
- Khronos `VK_KHR_dynamic_rendering` (suspend/resume pass analogy) — <https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_dynamic_rendering.html>
- `MTLResidencySet` mirror — <https://github.com/livingston/apple-docs/blob/main/documentation/Metal/MTLResidencySet.md>
