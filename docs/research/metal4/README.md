# Native Apple Metal 4 RSX backend — master synthesis

> **This is the master document for the `docs/research/metal4/` set.** It rolls up eleven
> independently-produced scoping documents into one coherent reference. It is a *merge*, not a
> concatenation: overlapping rows are deduplicated, contradictions are reconciled or flagged,
> and every claim carries provenance to the scope doc + section that supports it.
>
> The set is **two tiers**. **`01`–`05`** are the per-API *translation inventory* (Vulkan → Metal 4):
> they own the unified mapping (§3) and most of the impossibility register (§6). **`06`–`11`** are
> cross-cutting — build/deploy (`06`), verification, conformance & performance (`07`), the
> `Emu/RSX/VK/**` port surface and effort (`08`), the draw path (`09`), the **RSX-native
> architecture** (`10`), and the executable verification loop (`11`). They add draw-path mapping
> rows (§3.7), a reconciled effort/phasing model (§7), and register deltas (§6.1–§6.2); their
> headline conclusions are folded into §2 and §4 rather than re-listed.
>
> **Read-only research.** No RPCS3 source, CMake, test or `.ci` file was modified to produce this
> set and no build was run. Peer docs were written in parallel and mostly blind to each other;
> `file:line` references inside them are point-in-time on branch `kosmickrisp-integration` (01–05)
> and `metal4` (06–11), and an agent may have concurrently edited `rpcs3/Emu/RSX/VK/**`, so
> **symbol names are the stable reference; line numbers are advisory.** `[V]` = verified against a
> primary source; `[I]` = inference; `[U]` = could not verify.

---

## 1. Purpose & how to read this set

| Doc | Scope |
|---|---|
| **this `README.md`** | Master: executive summary, unified mapping, architectural shifts, dependency graph, consolidated impossibility register, phasing, open questions, sources. Authoritative for cross-scope statements. |
| [`01-core-memory.md`](01-core-memory.md) | Instance/device/adapter, feature+limit negotiation, queues, memory heaps & allocation, sub-allocation, VMM accounting, residency. |
| [`02-submission-sync.md`](02-submission-sync.md) | Command encoding, submission, synchronization, barriers, fences, render passes & queries. |
| [`03-shaders-pipelines-descriptors.md`](03-shaders-pipelines-descriptors.md) | Shaders (SPIR-V→MSL), pipeline objects, descriptors/argument tables, dynamic state. |
| [`04-images-formats-samplers-rtt.md`](04-images-formats-samplers-rtt.md) | Images, pixel formats, samplers, render targets, MSAA/resolve, copy/clear/blit. |
| [`05-presentation-and-impossible-register.md`](05-presentation-and-impossible-register.md) | Presentation/window/swapchain; its original master register is now superseded by §6 here (see §6 and 05 §9). |
| [`06-build-toolchain-deployment.md`](06-build-toolchain-deployment.md) | Cross-cutting: SPIRV-Cross vendoring, runtime vs offline MSL, the **macOS 26 / M1+ wall**, availability gating, the Metal-3-fallback question, CMake wiring, packaging, and what CI can/cannot verify. |
| [`07-verification-and-performance.md`](07-verification-and-performance.md) | Cross-cutting: the correctness **oracle ladder**, the conformance levels, determinism & the divergence taxonomy, Metal debug tooling, the **performance model**, and the three-tier CI strategy. |
| [`08-backend-port-surface.md`](08-backend-port-surface.md) | Cross-cutting: the **RSX backend contract** (`rsx::thread` + `backend_configuration`), the 112-file / 34,213-LOC `Emu/RSX/VK/**` port surface, the LOC/effort estimate, cost centres, and comparable-port evidence. |
| [`09-draw-submission-and-batching.md`](09-draw-submission-and-batching.md) | Cross-cutting: the draw path traced end to end, the draw→Metal-4 mapping (§3.7 here), batching strategies (A–F), command-buffer construction, and the "is explicit encoding a win?" verdict. |
| [`10-rsx-native-metal4-architecture.md`](10-rsx-native-metal4-architecture.md) | Cross-cutting: the **RSX-first** target architecture (resource/binding/pass/sync/draw/shader/present models), the delete/keep/reshape diff vs the Vulkan-derived plan, a revised effort estimate, and register deltas (§6.1). |
| [`11-verification-loop.md`](11-verification-loop.md) | Cross-cutting: the **executable verification loop** — the RRC replay substrate, the H0–H7 harness ladder, the recording seam, differential goldens, per-command localisation, and the fast/slow iteration loop. |
| [`12-rsx-state-space-inventory.md`](12-rsx-state-space-inventory.md) | The **completeness spine**: the mechanically-generated enumeration of the RSX state space (282 method registers in 14 clusters, 158 decoders, 768 value constants, 275 shader facets), the curated disposition layer, and the coverage gate. Backed by [`inventory/`](inventory/) (generator + tests + validator). |
| [`design/`](design/) | The **design frame built on the spine**: `HLD-*` (architecture, resources, synchronization, command encoding, shaders/binding, presentation) and `LLD-<cluster>` (one per cluster, each carrying that cluster's disposition shard). |

**How to read:** use this document for *what it means, what conflicts, what to build first, and
what is impossible*; use the detail docs for the full `file:line` inventories and per-scope
reasoning. `01`–`05` are the translation inventory; `06`–`11` are cross-cutting, and their
conclusions are **folded into** §2 (headline verdicts), §4 (shifts), §5 (dependency graph), §6
(register deltas), §7 (reconciled phasing) and §8 (questions) rather than duplicated. `12` +
`design/` are the *RSX-first* working set: `12` is the completeness spine and the `HLD`/`LLD`
docs are where each cluster's mapping is decided against it. Where this
document and a detail doc disagree, **this document is authoritative** and the disagreement is
called out inline. Background context (not part of this set, referenced):
[`../03-rpcs3-backend-architecture.md`](../03-rpcs3-backend-architecture.md),
[`../05-metal-backend-northstar.md`](../05-metal-backend-northstar.md).

---

## 2. Executive summary

**What a native Metal 4 backend is.** RPCS3's Vulkan backend (`rpcs3/Emu/RSX/VK/**`, **112 files /
34,213 LOC** — `08` §6.1) currently reaches the GPU through MoltenVK or KosmicKrisp (a Vulkan
driver whose backend *is* Metal 4). A native backend deletes that layer and talks to
`MTL4CommandQueue`/`MTL4CommandBuffer`/`MTL4CommandEncoder` directly, keeping the shared
~55k LOC of RSX emulation unchanged behind the `rsx::thread` virtual contract and the
`backend_configuration` capability struct. The new `MTL/` subsystem is estimated at **~20–30k LOC**
(bottom-up `08` §8.1: ~20–30k, centre ~25k; RSX-first `10` §6.1: ~18–25k, centre ~21k) —
effectively a second `VK/`.

**Headline verdicts.**

1. **Feasible, but large.** No shared code must change; the GPU-resource layer is rewritten.
   Multi-person, multi-year for parity; the measured upside over a healthy KK/MVK is *modest*
   (`../05-metal-backend-northstar.md`). It is a research northstar, not the recommended next ship.
2. **The shader path is the make-or-break, and it is already decided in principle:** keep
   `RSX ucode → GLSL → glslang → SPIR-V` unchanged and insert **SPIRV-Cross `CompilerMSL`** before
   the GPU (`GLSL → SPIR-V → MSL → MTL4Compiler`). Metal 4 did **not** change the shader-binary
   model: the argument table indexes the *classic* `[[buffer(N)]]/[[texture(N)]]/[[sampler(N)]]`
   spaces, so SPIRV-Cross needs no Metal-4 awareness (03 §3.4, §1.2). No NIR→MSL backend is needed
   (KK only needed one because it starts from NIR).
3. **The hardest structural shifts are all "object disappears" events**, not new APIs:
   `VkRenderPass`+`VkFramebuffer` → per-encoder `MTL4RenderPassDescriptor`; descriptor
   sets/layouts/push-constants → one argument table; the blit encoder → the compute encoder;
   implicit hazard tracking → explicit barriers; implicit residency → explicit residency sets.
4. **Every difficulty rating across the set is 1–5 on the same scale** (1 rename → 5
   unknown/impossible). The single most common rating is **3–4**; genuinely impossible items are
   few and (with two exceptions) have **low RSX impact** because PS3 games cannot reach the
   missing features (geometry/tessellation/transform-feedback/pipeline-stats).
5. **It is an Apple-silicon-only backend with a hard OS floor.** Metal 4 exists on **M1+ and
   macOS 26+ only — never on Intel**; that is a hardware+OS wall, not a switch (`06` §3.1). The
   recommended posture keeps `CMAKE_OSX_DEPLOYMENT_TARGET`/`LSMinimumSystemVersion` at **15.0**,
   builds against the macOS 26 SDK (weak-linking later symbols), and runtime-gates on
   `arm64 && macOS_major >= 26 && supportsFamily(MTLGPUFamilyMetal4)` (`06` §3.3–§3.4).
   **A Metal 3 fallback is a second backend, not a build flag — do not build it; MoltenVK already
   *is* the pre-26 fallback** (`06` §4). Intel keeps the MoltenVK-only build with the `MTL`
   sources arch-excluded, mirroring `bundle_should_include(kosmickrisp)=arm64`.
6. **The contract is tiny; the implementation is not.** A backend must satisfy **~4 pure virtuals +
   ~25 hooks + a 16-bool `backend_configuration` + 5 ZCULL virtuals (~50 signatures)** (`08` §2.5).
   The size lives in the three generic caches — `rsx::surface_store`, `rsx::texture_cache`,
   `program_state_cache` — which the ~55k LOC of shared `Common/`/`Program/`/`NV47/` emulation
   already drives.
7. **The draw path is the conditional win.** A naive 1:1 port of the VK draw path is **roughly a
   wash**; the win appears only if argument tables and indirect batching are exploited (`09` §7.2).
   It is measurable today via `m_frame_stats.setup_time`/`draw_exec_time` (`09` §7.3).
8. **RSX-first reshapes the estimate, it does not halve it.** Designing from the RSX contract
   deletes ~5–7k LOC of Vulkan-only *middle scaffolding* (render-pass objects + packed `u64` key,
   descriptor-set/layout machinery, loader/instance/extension layer, push-constant workaround) — a
   **~15–20%** reduction (`10` §5–§6), not a re-architecture of the hard parts (texture-cache
   traits, MSL correctness, per-title quirks all survive). Its make-or-break is a **hazard-derived
   pass scheduler** (`10` §10).
9. **Verification is the real project risk.** RPCS3 has **no golden-image GPU suite**, and
   **GitHub-hosted macOS runners expose no dependable Metal device** (`07` §3.4, §9.4). The only
   RSX-*objective* oracle is guest-observable readback (`07` §2.2 rung 3); everything else is
   *differential* against the shipping backend and needs a self-hosted Apple-silicon runner.
10. **The capture/replay substrate already *is* the harness.** Capture and replay live in
    **backend-agnostic core** (`RSXThread.cpp:975,3478`; `RSXFIFO.cpp:833,836`) and the `.rrc`
    format is **per-command annotated** — so a future `MTL` backend inherits a deterministic,
    command-localisable test loop unchanged (`11` §3).

**Overall feasibility / effort picture.** Two independent bottom-up estimates now exist and
corroborate each other:

| Milestone | Scope | Rough effort |
|---|---|---|
| Bring-up | device/queue/swapchain, clear + present, overlay UI | weeks–low months |
| MVP | VP/FP pipeline, textures, vertex upload, basic draws, a few bootable titles | **~6–12 person-months**; **8–12k LOC** (`08` §8.2) → RSX-first **~7–10k** (`10` §6.1) |
| Playable subset | render targets, MSAA resolve, texture-cache correctness, format/deswizzle compute | +12–24 person-months |
| Parity with VK backend | all RSX quirks, async compute, ZCULL, savestate, FSR/upscalers, edge formats | multi-person-year |

`08` §8.1 estimates **~20,000–30,000 LOC** of new `MTL/` (centre ~25,000) by bottom-up row count;
`10` §6 revises that to **~18–25k (centre ~21k)** after the RSX-native deletions; `08` §8.3's
phase mapping (~21–35k) brackets the same range. All three are order-of-magnitude, not schedules.

**Biggest risks (consolidated):** (1) the RSX per-title quirk surface; (2) shader correctness via
SPIR-V→MSL; (3) the render-pass→encoder redesign; (4) TBDR performance tuning; (5) long-term
maintenance by few people — plus the inherited MSL/Metal compiler bug surface (§6, row R10).
Added by the cross-cutting scopes: (6) **no local pixel oracle** — correctness is defined by
agreement with another backend on a different rasteriser, so both false positives and false
negatives are expected (`07` §2.1); (7) **CI cannot test the thing that matters** — no dependable
Metal on hosted runners, so without a self-hosted Mac all GPU verification is manual (`07` §9.4);
(8) **the availability gate cannot be enforced by the compiler under metal-cpp** — metal-cpp ships
no `API_AVAILABLE`, so an unguarded `MTL4*` call compiles silently and crashes on macOS 15–25
(`06` §3.4); (9) **the strategic gate itself** — the upside over KosmicKrisp, which already runs
directly on Metal 4, is unmeasured, and Dolphin's precedent shows native Metal beating a *healthy*
translator only narrowly (+23%/+30% after fixing a 45% MVK regression) (`10` §10.1).

---

## 3. Unified Vulkan → Metal 4 mapping

One merged, deduplicated table. The **Scope** column attributes each row to its owner doc; where
two docs mapped the same construct the row lists both and the notes carry the discrepancy. Where
two docs **rated the same construct differently**, both ratings appear as `a / b` and the row is
marked **(diff disagree)**. Difficulty scale (identical across the set): **1** trivial rename ·
**2** mechanical · **3** real work, well-trodden · **4** hard / needs design · **5** unknown or
impossible.

> Deduplicated from 32 (01) + 41 (02) + 47 (03) + 46 (04) + 24 (05) = **190 raw rows** to
> **156 merged rows** in §3.1–§3.6 (plus the **11 draw-path rows** in §3.7 from `09`). Rows for the
> same construct appearing in multiple docs were collapsed
> — and further bundled (e.g. the ~9 individual builtins in 03 #29-37 become one row; the ~11
> sampler sub-features in 04 #9-19 become one row) — so 156 over-counts the true construct count but
> under-counts the raw rows. `VkRenderPass`/`VkFramebuffer` collapsed from 02 #26, 04 #20 and 05's
> ranks/sketch into a single row (#57).

### 3.1 Instance / device / adapter / features / limits

| # | Construct (exact symbol) | Metal 4 objects / methods | Diff | Scope | Notes |
|---|---|---|---|---|---|
| 1 | `VkInstance` / `vkCreateInstance` (`instance.cpp:90,224`) | `MTL::CreateSystemDefaultDevice()`, `MTL::CopyAllDevices()` | 1 | 01 #1 | No instance object, no validation layers, no loader. |
| 2 | `VK_KHR_portability_enumeration` + enumerate bit (`instance.cpp:129,221`) | — | 1 | 01 #2, 05 #3 | Exists only to unmask MVK; meaningless natively. |
| 3 | `VK_EXT_metal_surface` → layer (`instance.cpp:164`; `metal_layer.mm:9`) | `CA::MetalLayer` | 1 | 01 #3, 05 #2 | Already a 1-line shim (`GetCAMetalLayerFromMetalView`). See 05. |
| 4 | `VK_EXT_layer_settings` + `MVK_CONFIG_*` (`instance.cpp:134-148`) | `MTLCompileOptions::fastMathEnabled` / `MTL4Compiler` options | 2 | 01 #4 | `disable_msl_fast_math` moves to the shader compiler (scope 03). |
| 5 | `vkEnumeratePhysicalDevices` (`instance.cpp:251`) | `MTL::CopyAllDevices()` / `CreateSystemDefaultDevice()` | 1 | 01 #5 | RPCS3 picks one GPU; Metal likewise. |
| 6 | `vk::physical_device` + `get_physical_device_properties_0/1` (`device.cpp:241,163,184`) | `hasUnifiedMemory()`, `supportsFamily:`, `registryID` | 2 | 01 #6 | Godot `MetalDeviceProperties` is the canonical probe. |
| 7 | `driver_vendor` / `chip_class` (`chip_class.h:45-64`; `device.cpp:286,427`) | `supportsFamily(GPUFamilyApple3…Apple10, Metal3)` | 2 | 01 #7 | Apple vendorID 0x106B → needs its own native enum. |
| 8 | Feature negotiation `TEST_VK_FEATURE(...)` (`device.cpp:15-160`) | `MetalFeatures` struct | 3 | 01 #28 | Replace `VkPhysicalDeviceFeatures`. |
| 9 | `VkPhysicalDeviceMemoryProperties` heaps (`device.cpp:459`; `:1034`) | `recommendedMaxWorkingSetSize()`, `currentAllocatedSize()`, `hasUnifiedMemory()` | 3 | 01 #8 | KK reports **exactly one** heap; it is a budget, not an array. |
| 10 | Memory classes `host_visible_coherent`/`device_local`/`device_bar` (`device.h:32-42`) | `StorageModeShared`, `StorageModePrivate` (Managed on Intel) | 4 | 01 #9 | `device_bar` (ReBAR) has no Metal distinction; all three collapse to Shared-vs-Private. |
| 11 | `get_compatible_memory_type` / type-index search (`device.cpp:916`; loops `memory.cpp:229-245`) | `MTLResourceOptions` / `MTLTextureDescriptor.storageMode` | 3 | 01 #10 | No "memory type index"; fixed at resource creation. |
| 12 | Queue family/count/properties (`device.cpp:432-458`) | one `MTL4CommandQueue` + allocators | 3 | 01 #29 | RPCS3 is effectively single-queue; Metal has no queue families. |
| 13 | `get_limits` consumers (`VKGSRender.cpp:663-2491`; `VKCompute.cpp:97`; `image.cpp:22-30`) | `MetalLimits` table | 2 | 01 #30 | Godot `MetalLimits` is a ready-made mapping. |
| 14 | Sampler allocation cap (`VKResourceManager.cpp:55-64`) | `maxArgumentBufferSamplerCount` | **3 / 4 (diff disagree)** | 01 #25, 04 §2.4/§2.8 | 01 rates 3 (allocator concern); 04 rates 4 (hard cap + eviction). See §6 R17. |

### 3.2 Memory allocation / heaps / residency

| # | Construct (exact symbol) | Metal 4 objects / methods | Diff | Scope | Notes |
|---|---|---|---|---|---|
| 15 | `mem_allocator_vma` (VMA) (`memory.h:109`; `memory.cpp:163-358`) | `MTLHeapDescriptor(HeapTypePlacement)` + `newBuffer/newTexture(offset:)` | 4 | 01 #11 | Godot `MetalHeapAllocator` is the reference. |
| 16 | `mem_allocator_vk` raw `vkAllocateMemory` (`memory.h:139`; `memory.cpp:360-450`) | `newBuffer/newTexture`, or `newHeap` | 4 | 01 #12 | Two allocator backends collapse into one. |
| 17 | `memory_block` + `vkAllocateMemory` (`memory.cpp:457-470`) | `MTL::Allocation` (buffer/texture/heap) | 3 | 01 #13 | Thin handle over `MTLBuffer`/`MTLTexture`/`MTLHeap`. |
| 18 | `memory_block_host` host-pointer import (`memory.cpp:472-513`; `buffer_object.cpp:97-143`) | `newBufferWithBytesNoCopy:length:options:deallocator:` | 4 | 01 #14 | **Buffers only**, page-aligned, whole-allocation; no texture analog. §6 R4. |
| 19 | `VK_MEMORY_PROPERTY_HOST_COHERENT` reliance (no flush/invalidate) | `StorageModeShared`; `StorageModeManaged` + `didModifyRange:` on Intel | 4 | 01 #15 | Shared is coherent on Apple silicon (KK flush/invalidate are no-ops). |
| 20 | Budget query `vmaGetHeapBudgets` (`memory.cpp:325-348`) | `currentAllocatedSize()` (used), `recommendedMaxWorkingSetSize()` (budget) | 3 | 01 #16 | KK: budget = `0.9 × available`. |
| 21 | VMM pools + `vmm_notify_memory_allocated` / `vmm_handle_memory_pressure` (`memory.h:19-33,205`; `VKResourceManager.cpp:269`) | wrap `MTLHeap`/`allocatedSize()`; `setPurgeableState:` | 3 | 01 #17 | Pool taxonomy is backend-agnostic; only the byte source changes. |
| 22 | `vk::buffer` alloc + `ALLOW_NULL`/`IGNORE_VMEM_PRESSURE` (`buffer_object.cpp:46-95`; `buffer_object.h:12-15`) | `newBufferWithLength:options:`; app-level null handle | 3 | 01 #19 | Nullable buffer is an app concept Metal doesn't model. |
| 23 | `vk::image` alloc + `ALLOW_NULL`/`SHAREABLE` (`image.h:25-28`; `image.cpp:122-160`) | `newTextureWithDescriptor:` (from heap: `offset:`) | 3 | 01 #20; 04 #1 | Format/usage detail → scope 04. |
| 24 | `vk::data_heap` host-coherent ring + VRAM shadow (`data_heap.h:23-110`) | `StorageModeShared` `MTLBuffer` (persistent `contents()`) + optional private shadow + blit | **4 / 2 (diff disagree)** | 01 #21, 02 #40 | 01 rates the heap+shadow 4; 02 rates ring carry-over 2 ("coherence dance unneeded"). Both true: the *coherence* is trivial, the *VRAM shadow* is not. |
| 25 | `data_heap_manager::restore_snapshot`/`get_heap_snapshot` (`VKDataHeapManager.h:14-26`; `VKGSRenderTypes.hpp:185`) | app-side offset map over persistent buffers | 3 | 01 #22 | No Metal heap snapshot API; bookkeeping reimplemented. §6 R30. |
| 26 | `null_sampler`/`null_image_view`/scratch pool (`scratch.h:6-15`; `scratch.cpp:100`) | null `MTLTexture`/`MTLSamplerState`; scratch `MTLBuffer` | 2 | 01 #23 | Detail in scopes 03/04. |
| 27 | `vk::garbage_collector` / `disposable_t` (`garbage_collector.h:7-57`) | `NS::TransferPtr`/`RetainPtr`; deferred `->release()` | 2 | 01 #24 | Must respect MTL4 no-strong-ref rule + residency removal ordering (§4, §5). |
| 28 | **Implicit residency** on bind (no RPCS3 code) | `MTLResidencySet` + `addAllocation`/`commit`/`requestResidency`; queue `addResidencySet` | 3 | 01 #26 | New mandatory subsystem; KK uses one device-level set. Residency ≠ hazards (§4). |
| 29 | Sparse binding (feature-dumped only, `device.cpp:1018`; never used) | `HeapTypeSparse`/`HeapTypePlacement` + `updateBufferMappings` | 5 | 01 #27; 04 #42 | Positional binding impossible. §6 R5. |
| 30 | `vkMapMemory`/`vkUnmapMemory` (`memory.cpp:425-435,289-303`) | `MTLBuffer::contents()` (shared only) | 3 | 01 #31 | Shared buffers need no map/unmap; private buffers are CPU-unreachable. |
| 31 | `supports_passthrough_dma` (`VKGSRender.cpp:798`; `device.h:191`) | fallback: CPU copy | 5 | 01 #32 | Must force `false`. §6 R4. |
| 32 | `VkAllocationCallbacks` | — | 5 | 05 #10 | Metal has no allocator hooks; MVK ignores them. |

### 3.3 Command buffers / submission / synchronization / passes / queries

| # | Construct (exact symbol) | Metal 4 objects / methods | Diff | Scope | Notes |
|---|---|---|---|---|---|
| 33 | `VkCommandPool` (`commands.cpp:17-22`) | `MTL4CommandAllocator` | 2 | 02 #1 | Pool → allocator that owns *encoding* memory; reset per frame. |
| 34 | `VkCommandBuffer` primary, one-time-submit | `MTL4CommandBuffer` (**reusable, unretained**) | 2 | 02 #2 | No queue ownership; resources must outlive the CB. |
| 35 | `vkBeginCommandBuffer` + `ONE_TIME_SUBMIT` (`commands.cpp:87-113`) | `beginCommandBuffer(allocator:options:)` | 2 | 02 #3 | No usage flags; no inheritance. |
| 36 | `VkCommandBufferInheritanceInfo` | — | 1 | 02 #4 | Set but unused (primary only). Drop. |
| 37 | `vkCmdBindPipeline` (state-cached, `commands.cpp:149-196`) | `setRenderPipelineState()` / `setComputePipelineState()` | 2 | 02 #5; 03 #7/#8 | Encoders are per-pass; cache buys little. |
| 38 | `vkCmdBindDescriptorSets` (`commands.cpp`) | `MTL4ArgumentTable` + `encoder.setArgumentTable(_:atStages:)` | **4 / 3 (diff disagree)** | 02 #6, 03 #17 | 02 rates 4 (model change); 03 rates the table bind 3. Design item, scope 03. |
| 39 | `vkQueueSubmit(queue,1,&info,fence)` (`VKCommandStream.cpp:15-50`) | `queue.commit([cb], options:)`, `MTL4CommitOptions.addFeedbackHandler()` | 3 | 02 #7 | Commit **is** the submit; many CBs per commit. |
| 40 | `VkSubmitInfo.pWaitSemaphores` + `pWaitDstStageMask` (`VKCommandStream.cpp:32-43`) | `MTL4CommandQueue.waitForEvent(event, value:)` | 3 | 02 #8 | **Stage-gated waits are not expressible** — queue-level event only. §6 R7. |
| 41 | `VkSubmitInfo.pSignalSemaphores` | `MTL4CommandQueue.signalEvent(event, value:)` | 3 | 02 #9 | Binary semaphore → monotonic event value. |
| 42 | `vkQueuePresentKHR` (`swapchain.cpp:359`) | `signalDrawable(drawable)` + `drawable.present()` | **3 / 2 (diff disagree)** | 02 #10, 05 #7 | 02 rates the queue tail 3; 05 rates the present call 2. |
| 43 | `vkAcquireNextImageKHR` (`swapchain_core.h:204`) | `waitForDrawable(drawable)` + `CAMetalLayer.nextDrawable` | **4 / 3 (diff disagree)** | 02 #11, 05 #5 | 02 rates the drawable model 4; 05 rates acquire 3. §6 R21. |
| 44 | `VkFence` (`sync.cpp:151-172`, `:569`) | `MTL4CommitFeedback` + `MTLSharedEvent.signaledValue` | 3 | 02 #12; 05 #6 | No CPU-waitable fence object. |
| 45 | `VkSemaphore` (binary) (`sync.cpp:192-208`) | `MTLEvent`/`MTLSharedEvent` (monotonic 64-bit) | 3 | 02 #13 | Binary not native. §6 R8. |
| 46 | `VkEvent` (`sync.cpp:210-383`) | producer/consumer barriers + `MTLSharedEvent` for host | 4 | 02 #14 | GPU-domain split into barriers (Mesa does exactly this). |
| 47 | `gpu_label` fallback (`sync.cpp:385-479`) | `MTLSharedEvent` / shared `MTLBuffer` | 2 | 02 #15 | Unified memory makes the workaround unnecessary. |
| 48 | `vkCmdPipelineBarrier` (`barriers.cpp:35,61,79,128`) | `barrierAfterStages:beforeQueueStages:`, `barrierAfterQueueStages:beforeStages:`, `barrierAfterEncoderStages:beforeEncoderStages:` + `MTL4VisibilityOption` | **4 / 5 (diff disagree)** | 02 #16 | 02's own table rates 4 but its ranked list (#1) rates 5. §4 shift #1. |
| 49 | `_vkCmdPipelineBarrier2KHR` + `VkDependencyInfoKHR` (`sync.cpp:257`) | same three barrier methods + `MTL4VisibilityOptions` | 4 | 02 #17 | Sync2 maps more cleanly. |
| 50 | `VkImageMemoryBarrier` (layout transition) (`image.h:48-49`) | visibility option + `MTLLoadAction`/`MTLStoreAction` | **3 / 1 (diff disagree)** | 02 #18, 04 #35 | 02 rates the transition semantics 3; 04 rates "no image layouts" 1. Both hold: layouts dissolve, but attachment⇄shader-read still needs a barrier + load/store. |
| 51 | `VkBufferMemoryBarrier` / `VkMemoryBarrier` | same barrier calls (`MTLStages.all` for global) | 3 | 02 #19, #20 | No layouts; purely visibility. |
| 52 | `VK_QUEUE_FAMILY_IGNORED`, `image::current_queue_family` | — | 1 | 02 #21 | Single queue family; ownership meaningless. Delete tracking. |
| 53 | `VK_DEPENDENCY_BY_REGION_BIT` (`VKPresent.cpp:904`; `VKDraw.cpp:1146`) | no equivalent (per-tile TBDR) | 3 | 02 #22 | Meaning changes; TBDR makes it implicit. §6 R9. |
| 54 | `insert_texture_barrier` feedback loop (`barriers.cpp:82-135`) | intra-pass barrier, else split pass + barrier | 4 | 02 #23 | TBDR **fragment constraint**: cannot wait on fragment/tile stages intra-pass. |
| 55 | `VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT` (`VKRenderTargets.cpp:866`) | tile `[[color(N)]]` programmable blending + same-pass store | 4 | 02 #24; 03 #15 | Or split the pass; changes RSX read-write-alias handling. |
| 56 | `_vkCmdBeginConditionalRenderingEXT`/`End` (`VKDraw.cpp:1164-1168,1420`) | **none** — emulate via visibility buffer + `executeCommands(buffer:indirectBuffer:)` or compute skip | **5 / 4 (diff disagree)** | 02 #25, 05 #21 | 02 rates 5 (no command); 05 rates 4. §6 R6. |
| 57 | `vkCreateRenderPass` + one subpass (`VKRenderPass.cpp:341-405`) | `MTL4RenderPassDescriptor` per encoder + `makeRenderCommandEncoder(descriptor:options:)` | 3 | 02 #26; 04 #20; 05 §9 rank1 | The `VkRenderPass` object disappears; **the encoder is the pass**. Architectural. |
| 58 | `VkAttachmentDescription.loadOp`/`storeOp` | `MTLLoadAction`/`MTLStoreAction` + dynamic `setColorStoreAction`/`setDepthStoreAction`/`setStencilStoreAction` | **2 / 1 (diff disagree)** | 02 #27; 04 #21/#22 | 02 rates 2 (near 1:1); 04 rates 1 (direct). |
| 59 | `VkSubpassDependency` (input attachments, `VKRenderPass.cpp:384-392`) | intra-pass barrier — invalid for fragment on TBDR | 3 | 02 #28 | Fragment→fragment / DS read-write mandates a pass split. |
| 60 | `vkCmdBeginRenderPass`/`vkCmdEndRenderPass` (`VKRenderPass.cpp:460,477`) | `makeRenderCommandEncoder(...)` / `endEncoding()` | 2 | 02 #29 | `VK_SUBPASS_CONTENTS_SECONDARY` unused. |
| 61 | `vkCmdNextSubpass` | not used | 1 | 02 #30 | RPCS3 never calls it. |
| 62 | `VkFramebuffer` (`framebuffer_object.hpp:42`) | attachment textures on `MTL4RenderPassDescriptor` | 2 | 02 #31; 04 #20 | FBO → descriptor attachments. |
| 63 | `vkCreateImageView` (`image.cpp:446`) | `MTLTextureViewPool` / `newTextureViewWithDescriptor:` | 2 | 02 #32; 04 #1/#2 | Lightweight views replace `VkImageView`. |
| 64 | `VK_QUERY_TYPE_OCCLUSION` + `vkCmdBeginQuery`/`vkCmdEndQuery` (`VKQueryPool.cpp:155,160`) | `renderPassDescriptor.visibilityResultBuffer` + `setVisibilityResultMode(_:offset:)` | 4 | 02 #33; 05 #20 | Pass-scoped; a query spanning passes needs accumulation. §6 R12. |
| 65 | `vkCmdResetQueryPool` (`VKQueryPool.cpp:96`) | visibility buffer reset at pass start; else `fillBuffer` | 5 | 02 #34 | Reset semantics differ — **must be verified on device**. |
| 66 | `vkGetQueryPoolResults` (+ availability) (`VKQueryPool.cpp:19`) | read mapped shared `MTLBuffer` after commit feedback | 3 | 02 #35 | No async result struct. |
| 67 | `vkCmdCopyQueryPoolResults` (`WAIT_BIT`) (`VKQueryPool.cpp:191`) | compute copy into predicate buffer | 4 | 02 #36 | Feeds conditional-render emulation. |
| 68 | `VkQueryResultFlags`, `VK_QUERY_CONTROL_PRECISE_BIT` | none | 2 | 02 #37 | Flags meaningless on Metal. |
| 69 | Timestamp queries `VK_QUERY_TYPE_TIMESTAMP` (unused today) | `MTL4CounterHeap`, `writeTimestamp(granularity:after:counterHeap:index:)` | 2 | 02 #38 | Available if profiling is added. |
| 70 | `vkCreateQueryPool` pool/slot lifecycle (`VKQueryPool.h:13-85`) | counter heap / plain shared `MTLBuffer` | 2 | 02 #39 | Manager logic (lifetime/reallocation) largely portable. |
| 71 | `vk::AsyncTaskScheduler` (`VKAsyncScheduler.h:8-61`) | second `MTL4CommandQueue` or compute encoders on one queue | 4 | 02 #41 | Metal 4 pushes toward batching; re-derive, don't port. §6 R13. |

### 3.4 Shaders / pipelines / descriptors / dynamic state

| # | Construct (exact symbol) | Metal 4 objects / methods | Diff | Scope | Notes |
|---|---|---|---|---|---|
| 72 | GLSL→SPIR-V `compile_glsl_to_spv` (glslang) (`SPIRVCommon.cpp:128`) | **unchanged** | 1 | 03 #1 | SPIR-V stays the intermediate; do not touch glslang. |
| 73 | `VkShaderModule` from SPIR-V | MSL via SPIRV-Cross → `MTL4::Compiler::newLibrary(LibraryDescriptor)` | 3 | 03 #2 | SPIR-V→MSL is the real work; needs binding remap. |
| 74 | `VkPipelineShaderStageCreateInfo` (VS/FS) | `MTL4::LibraryFunctionDescriptor{setLibrary,setName}` | 2 | 03 #3 | Metal 4 uses descriptors, not `MTLFunction`. |
| 75 | `VkSpecializationInfo` | `MTL4::SpecializedFunctionDescriptor` + `MTLFunctionConstantValues` | 3 | 03 #4 | SPIRV-Cross maps spec constants to macros or function constants. |
| 76 | `VkPipelineLayout` / `vkCreatePipelineLayout` (`VKProgramPipeline.cpp:361`) | **no object**; implicit in argument table + shader | 4 | 03 #5 | Table sizes derived from reflected bindings. |
| 77 | `VkPipelineCache` (`VKPipelineCompiler`) | `MTL4::PipelineDataSetSerializer` → `MTL::Archive` | 3 | 03 #6; 05 §9 rank25 | Binary archives compatible across Metal 3/4. |
| 78 | `vkCreateGraphicsPipelines` | `MTL4::Compiler::newRenderPipelineState(...)` | 2 | 03 #7 | Polymorphic with mesh/tile. |
| 79 | `vkCreateComputePipelines` | `newComputePipelineState(...)` | 2 | 03 #8 | — |
| 80 | `VkPipeline` destruction | ARC `release` | 1 | 03 #9 | — |
| 81 | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | `ArgumentTable::setAddress(buffer->gpuAddress(), idx)` | 2 | 03 #10 | GPU address supports pointer arithmetic. |
| 82 | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | `setAddress(gpuAddress, idx)` | 2 | 03 #11 | Same space as UBO. |
| 83 | `VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER` (`usamplerBuffer`, VS streams) | `texture_buffer_native` MSL type; `MTLTextureViewPool::setTextureViewFromBuffer` | 4 | 03 #12; 04 #46 | **Hardest binding item.** Metal has no `usamplerBuffer`. §6 R14. |
| 84 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | `setTexture(gpuResourceID, tIdx)` + `setSamplerState(gpuResourceID, sIdx)` | 3 | 03 #13; 04 #8 | Metal separates texture/sampler; SPIRV-Cross gives separate msl_texture/msl_sampler. |
| 85 | `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` | `setTexture(gpuResourceID, idx)` | 3 | 03 #14 | Needs `access::read_write`; MSL 3.2+ fences. |
| 86 | `VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT` / `subpassInput(MS)` (`VKFragmentProgram.cpp:277`) | MSL `[[color(n)]]` + load actions | 4 | 03 #15; 02 #24 | No subpasses; read-back = tile framebuffer fetch. `.loadAction` must be `Load` or read is UB. |
| 87 | `vkCmdPushConstants` (VS 4B; FS 28B, `VKFragmentProgram.cpp:341`) | **emulated** — transient `MTLBuffer` + `setAddress`; or function constants | 4 | 03 #16 | metal-cpp `MTL4::ArgumentTable` has **no `setBytes`**. §4 shift #5, §6 R15, §8 Q. |
| 88 | `VkDescriptorSet` dynamic offsets | pointer offset on GPU address (`setAddress(base+offset, idx)`) | 3 | 03 #18 | Table snapshots at each draw → cheap. |
| 89 | `UPDATE_AFTER_BIND` bindless textures | re-`setTexture` between draws | 3 | 03 #19 | No descriptor-indexing emulation; the table is live/mutable. |
| 90 | `VkPipelineRasterizationStateCreateInfo` | `setInputPrimitiveTopology`, `setRasterizationEnabled`, `setMaxVertexAmplificationCount` | 2 | 03 #20 | Polygon mode point unsupported. |
| 91 | `VkPipelineColorBlendStateCreateInfo` | per-attachment `setBlendingState/factors/ops/writeMask` | 2 | 03 #21 | — |
| 92 | `VkPipelineDepthStencilStateCreateInfo` | separate `MTL::DepthStencilState` + `setDepthStencilState` | 3 | 03 #22 | Not part of PSO; set on encoder. |
| 93 | MSAA `rasterizationSamples` | `setRasterSampleCount` | 1 | 03 #23 | — |
| 94 | Dynamic state `VIEWPORT`/`SCISSOR`/`BLEND_CONSTANTS`/`STENCIL_*`/`DEPTH_BIAS` (`VKShaderInterpreter.cpp:37-77`) | encoder setters (`setViewports`, `setScissorRects`, `setBlendColor…`, `setStencilReferenceValue`, `setDepthBias:slopeScale:clamp:`) | 2 | 03 #24-27 | — |
| 95 | Dynamic `LINE_WIDTH` | not dynamic without private API | 4 | 03 #28 | MVK needs `MVK_USE_METAL_PRIVATE_API` for `wideLines`. Clamp to 1.0. §6 R24. |
| 96 | Builtins `gl_Position`/`gl_PointSize`/`gl_ClipDistance[0..5]`/`gl_FragCoord`/`gl_FrontFacing`/`gl_FragDepth`/`gl_SampleID`/`gl_FragStencilRefARB`/`gl_VertexIndex`/`gl_BaryCoord{EXT,AMD}` (`VKVertexProgram.cpp:286-292,289`; `GLSLCommon.cpp:760-771`) | MSL `[[position]]`/`[[point_size]]`/`[[clip_distance]]`/`[[front_facing]]`/`[[depth(any)]]`/`[[sample_id]]`/`[[stencil]]`/`[[vertex_id]]`/`[[barycentric_coord]]` | 1–3 | 03 #29-37 | `gl_CullDistance` and `gl_Layer`/`gl_ViewportIndex` absent from RPCS3. Barycentrics `[U]`. |
| 97 | `discard` (`RSXROPEpilogue.glsl:4,37,44`) | `discard_fragment()` | 2 | 03 #38 | Plus KK W4/W5 workarounds. |
| 98 | Varyings (RSX outputs 0..~22) | MSL `[[user(locnN)]]` struct in/out | 2 | 03 #39 | SPIRV-Cross handles; names/structs must match (macOS). |
| 99 | VS `textureLod`/`texelFetch` (VTF) (`GLSLCommon.cpp:642-651`) | MSL vertex texture sample | 2 | 03 #40 | — |
| 100 | `sampler2DMS`/`usampler2DMS` | `texture2d_ms` | 2 | 03 #41 | SPIRV-Cross maps. |
| 101 | `sampler2DShadow` shadow compare (`GLSLCommon.cpp:454`) | `depth2d::sample_compare` (or emulated) | 3 | 03 #42 | Native `sample_compare` exists. |
| 102 | Programmable blending ROP push-constant (28B) | function constant / MSL `[[buffer(n)]]` | 4 | 03 #43 | Interacts with #86. |
| 103 | `subgroupShuffleXor` etc. (FSR only, `fsr_ffx_a_flattened.inc:330`) | `simd_shuffle_xor`, `simd_ballot`, `simd_*` | 3 | 03 #44 | Native but KK W3 (ballot in conditional misbehaves). |
| 104 | `VK_EXT_shader_explicit_arithmetic_types_float16` | MSL `half` | 2 | 03 #45 | Watch fp16 transcendentals. |
| 105 | `SEPARATE_SHADER_OBJECTS` | n/a | 2 | 03 #46 | Metal links VS/FS at PSO creation. |
| 106 | `VkPipelineTessellationStateCreateInfo` stub (`VKShaderInterpreter.cpp:35`) | n/a — no TS/GS | 1 | 03 #47 | Dead; delete. |

### 3.5 Images / formats / samplers / render targets / copies

| # | Construct (exact symbol) | Metal 4 objects / methods | Diff | Scope | Notes |
|---|---|---|---|---|---|
| 107 | `VkImage` + `VkImageView` (`vkutils/image`) | `MTL4::Texture` (`MTL::Texture`) + view | 2 | 04 #1 | Views need `MTLTextureUsagePixelFormatView`. |
| 108 | `VkImageView` subresource range (`image.h`) | `newTextureView(pixelFormat:textureType:levels:slices:swizzle:)` | 2 | 04 #2 | Format reinterpretation only within documented groups. |
| 109 | `VK_IMAGE_TYPE_1D` | 2D texture (height=1) | 1 | 04 #3 | KK emulates 1D as 2D. |
| 110 | `VK_IMAGE_TYPE_3D` + `2D_ARRAY_COMPATIBLE` | 3D texture + auxiliary 2D-array view texture | 4 | 04 #4 | KK allocates a separate array-compat texture. |
| 111 | `VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT` | subresource aliasing / separate texture | 5 | 04 #5 | KK disables layout optimization; else re-upload. §6 R20. |
| 112 | `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` | `MTLTextureUsagePixelFormatView` | 2 | 04 #6 | KK maps the bit straight to that usage. |
| 113 | Linear / `VK_IMAGE_TILING_LINEAR` | `MTLTextureTypeTextureBuffer` / `buffer.makeTexture(...)` | 3 | 04 #7 | KK routes linear textures through buffers; rejects 3D/compressed/depth/attachments. §6 R26. |
| 114 | `VkSampler` (`vkutils/sampler`) | `MTLSamplerState` + `MTL4ArgumentTable` slot | 3 | 04 #8 | Descriptor has separate min/mag/address modes. |
| 115 | `VkSamplerCreateInfo` filters/address/LOD/compare/reduction (`sampler.h:42-53`) | `minFilter`/`magFilter`/`mipFilter`/`sAddressMode`…/`lodMinClamp`/`lodMaxClamp`/`compareFunction`/`reductionMode` | 1–2 | 04 #9-16, #18-19 | Metal **does** have separate min/mag (earlier assumption was wrong). |
| 116 | `mipLodBias` | `lodBias` **S4.6 fixed-point** or shader-side uniform | 4 | 04 #17 | Precision loss; KK passes bias shader-side. §6 R19. |
| 117 | `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER` custom color | **only** TransparentBlack/OpaqueBlack/OpaqueWhite | 4 | 04 #10; 04 §2.4 | Custom color unsupported → `get_closest_border_color_enum` fallback. §6 R18. |
| 118 | `VK_IMAGE_LAYOUT_*` transitions | **none** — no image layouts | 1 | 04 #35 | Sync via encoder ordering/barriers; see #50. |
| 119 | `vkCmdCopyImageToBuffer` (`VKTexture.cpp:141,172,…`) | `copyFromTexture(...toBuffer...)` on compute encoder | 2 | 04 #25 | **No blit encoder**; copies on compute encoder. §4 shift #4. |
| 120 | `vkCmdCopyBufferToImage` (`VKTexture.cpp:306,347,…`) | `copyFromBuffer(...toTexture...)` (+options:) | 2 | 04 #26 | — |
| 121 | `vkCmdCopyImage` (`VKTexture.cpp:625`) | `copyFromTexture(...toTexture...)` | 2 | 04 #27 | Same-format/same-sample only; else through a buffer. |
| 122 | `vkCmdBlitImage` (`VKTexture.cpp:708`) | **no direct equivalent** → compute/graphics meta pass | **4 / 3 (diff disagree)** | 04 #28, 05 #12 | 04 rates 4 (meta pass); 05 rates 3 (`m_upscaler->scale_output`). §6 R25. |
| 123 | `vkCmdResolveImage` | native store-action resolve or compute pass | 3 | 04 #29 | — |
| 124 | `vkCmdClearColorImage` (`VKTextureCache.cpp:816,…`) | render-pass `loadAction=Clear` | 2 | 04 #30; 05 #11 | KK uses load-op clear. |
| 125 | `vkCmdClearDepthStencilImage` (`VKTextureCache.cpp:821,…`) | pass `loadAction=Clear` | 2 | 04 #31 | — |
| 126 | `vkCmdFillBuffer` (`VKTexture.cpp:367`) | `fillBuffer(buffer, range, value)` | 1 | 04 #32 | Byte-pattern fill only. |
| 127 | `vkCmdUpdateBuffer` | `copyFromBuffer` device-side, or host write | 2 | 04 #33 | Small host writes preferred. |
| 128 | `vkCmdGenerateMipmaps` (blitter) | `generateMipmaps(texture)` | 2 | 04 #34 | Requires mipmapped + usage. |
| 129 | `VkBufferImageCopy` regions | `MTL::Origin`/`MTL::Size` + slice/level | 2 | 04 #36 | — |
| 130 | `VkComponentMapping` (swizzle) | `MTLTextureSwizzleChannels` | 2 | 04 #37 | Also fixes depth→(d,d,d,1). |
| 131 | `VkSubpassDescription.resolveAttachments` | `resolveTexture` + `storeAction=MultisampleResolve`/`StoreAndMultisampleResolve` | 2 | 04 #23; 02 #28 | **MSAA resolve is native via store action.** |
| 132 | `VkAttachmentStoreOp` depth/stencil resolve | `depthResolveFilter` / `stencilResolveFilter` | 3 | 04 #24 | KK forces D/S resolve to meta passes (Metal sync bug). §6 R16. |
| 133 | `MTL_STORE_ACTION` for MSAA color | `MultisampleResolve` / `StoreAndMultisampleResolve` | 2 | 04 #38 | — |
| 134 | Memoryless / tile-only surfaces | `MTLStorageModeMemoryless` | 4 | 04 #39; 01 §4 item5 | Not a drop-in for PS3's exploded model. §6 R16. |
| 135 | `VkFormat` (all) | `MTLPixelFormat` | 2 | 04 #40 | Full per-format table 04 §2.3. §4 shift #6. |
| 136 | `VkImage` usage flags | `MTLTextureUsage` (ShaderRead/Write/RenderTarget/PixelFormatView/ShaderAtomic) | 2 | 04 #41 | — |
| 137 | `VK_EXT_custom_border_color` | unsupported (3 fixed colors) | 4 | 04 #43 | See #117. |
| 138 | `VK_EXT_image_2d_view_of_3d` | `mtl_handle_array` aux array texture | 4 | 04 #44 | — |
| 139 | MRT remap | `MTLLogicalToPhysicalColorAttachmentMap` + `setSupportColorAttachmentMapping` | 3 | 04 #45 | Useful for RSX MRT output remapping. |
| 140 | Texture↔buffer texel layout | `MTLTextureTypeTextureBuffer`, `buffer.makeTexture(...)` | 3 | 04 #46 | Linear-texture constraints apply. |

### 3.6 Presentation / window / swapchain

| # | Construct (exact symbol) | Metal 4 objects / methods | Diff | Scope | Notes |
|---|---|---|---|---|---|
| 141 | Window handle `NSView*` + `CAMetalLayer` (`metal_layer.mm:9`) | `CA::MetalLayer` | 1 | 05 #1 | Must also **create/attach** a layer if the view has none. §6 R22. |
| 142 | `VkSwapchainKHR` + `vkCreateSwapchainKHR` (`swapchain.cpp:292-328`) | drawable ring owned by `CAMetalLayer` (`setDrawableSize:`, `setPixelFormat:`, `setMaximumDrawableCount:`, `setFramebufferOnly:`, `setDevice:`, `setColorspace:`) | 3 | 05 #4 | Reimplement image bookkeeping; cf. Godot `SurfaceLayer`. |
| 143 | `VkPresentModeKHR` FIFO/IMMEDIATE/MAILBOX/RELAXED (`swapchain.cpp:236-272`) | `setDisplaySyncEnabled:` + `presentAfterMinimumDuration:` | **3 / 4 (diff disagree)** | 05 #8 | 05's own table rates 3, its ranked list (#1) rates 4. No true MAILBOX. §6 R23. |
| 144 | `nb_swap_images = minImageCount+2` (`swapchain.cpp:279`) | `setMaximumDrawableCount:` (≤3) | 2 | 05 #9 | Metal caps at 3; `m_max_async_frames ≤ 3`. |
| 145 | `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` (`swapchain_core.h:219`) | no layout; `CA::MetalDrawable::texture()` | 2 | 05 #10 | Acquire/present are the only transitions. |
| 146 | `VkImageBlit` / `m_upscaler->scale_output` (`VKPresent.cpp:885`) | compute blit or full-screen render | 3 | 05 #12 | See #122. |
| 147 | FSR1 spatial upscale (`fsr_pass.h:49`) | `MTLFX::SpatialScaler` / `MTL4FX::SpatialScaler` | 3 | 05 #13 | Spatial is a drop-in; **temporal** needs depth+motion+jitter RSX lacks. §6 R27. |
| 148 | Overlay compositing (`overlay_pass`, `VKOverlays.h:42`) | `MTL4RenderCommandEncoder` + `MTL4ArgumentTable` | 3 | 05 #14 | Shared `compiled_resource` reused. |
| 149 | `video_out_calibration_pass` gamma/range (`VKOverlays.h:208`) | render + layer colorspace (`setColorspace:`, `setWantsExtendedDynamicRangeContent:`) | 3 | 05 #15 | EDR only with a float16 drawable. |
| 150 | sRGB/format backpressure (`instance.cpp:392-400`) | `setPixelFormat:` BGRA8Unorm / RGBA16Float | 2 | 05 #16 | Colorspace is a `CGColorSpaceRef`, not a Vk color space. |
| 151 | Refresh rate (`gs_frame.cpp:805`) | `CAMetalDisplayLink`, `NSScreen.maximumFramesPerSecond` | 3 | 05 #17 | Needed for emulated timing + pacing. |
| 152 | Frame pacing / vsync config (`system_config_types.h:390`) | `CAMetalDisplayLink.preferredFrameRateRange`, `displaySyncEnabled` | 4 | 05 #18 | Tri-state vsync has no 1:1 equivalent. §6 R23. |
| 153 | Screenshot readback (`VKPresent.cpp:684-779`) | compute-encoder blit + `MTLStorageModeShared` | 2 | 05 #19 | `framebufferOnly` must be `false`. §6 R28. |
| 154 | Swapchain recreate on resize (`VKPresent.cpp:38`) | `setDrawableSize:` on the layer | 2 | 05 #22 | Must coordinate in-flight drawables; no `vkDeviceWaitIdle`. §6 R29. |
| 155 | Stereo present (`VKPresent.cpp:560-580`) | two views / layered drawable | 4 | 05 #23 | No Metal "stereo swapchain". §6 R31. |
| 156 | `MTKView` (not used today) | alternative host view (`currentDrawable`, `currentMTL4RenderPassDescriptor`) | 2 | 05 #24 | RPCS3 uses `QWindow`; intrusive. |

**Count: 156 merged rows** (from 190 raw; duplicates collapsed and several bundled rows, e.g.
sampler sub-features #115 and builtins #96, absorb multiple raw rows — so the raw count is higher
than the sum of the six table lengths above suggests.). *(Rows are numbered continuously 1–156.)*

### 3.7 Draw-path rows (`09`)

`09` re-traverses the **draw path** at *draw-call* granularity (22 rows). **Eleven of them are not
new constructs** — they re-map rows already above, so they are cross-referenced here rather than
duplicated (`09` §3):

| `09` row | Merges into | `09` row | Merges into |
|---|---|---|---|
| #7 trivial-instanced draw + `pass_count()` | §3 #90 + D2 below | #14 conditional rendering | §3 #56 (R6) |
| #9 software-pulled vertex streams | §3 #83 (`usamplerBuffer`) | #15 occlusion query | §3 #64 |
| #10 bind pipeline | §3 #37 | #17 primitive topology | §3 #90 |
| #11 descriptor sets / argument table | §3 #38, #88 | #20 `update_draw_state` | §3 #94 |
| #12 render-pass boundary | §3 #57, #60 | #21 push constants | §3 #87 (R15) |
| #13 programmable blending | §3 #55, #86 | | |

The remaining **eleven are genuinely new** — the draw-*dispatch* and batching constructs that
§3.1–§3.6 never enumerated, because they are calls rather than objects:

| # | Construct (exact symbol) | Metal 4 objects / methods | Diff | Scope | Notes |
|---|---|---|---|---|---|
| D1 | `vkCmdDraw(cmd,count,1,0,0)` (`VKDraw.cpp:1184`) | `drawPrimitives(type,vertexStart,vertexCount)` | 1 | 09 #1 | `vertexStart` = `upload_info.vertex_base` (`VKVertexBuffers.cpp:378`). |
| D2 | `vkCmdDraw(...,pass_count())` (`:1180`) | `drawPrimitives(...,instanceCount)` | 1 | 09 #2, #7 | RSX "trivial instanced" maps 1:1 to native instancing — the cheapest batching win (strategy B). |
| D3 | `vkCmdDrawIndexed(...)` (`:1216`) + `vkCmdBindIndexBuffer` (`:1208`) | `drawIndexedPrimitives(...,indexBuffer:indexBufferLength:)` | 2 | 09 #3 | The index buffer is an **argument (GPU address + length)**, not bound state; needs ≥2/4-byte alignment and a matching `MTL::IndexType`. |
| D4 | `vkCmdDrawIndexed(...,baseVertex,baseInstance)` | `drawIndexedPrimitives(...,baseVertex:baseInstance:)` | 1 | 09 #4 | Usable because RSX index emulation is host-side. |
| D5 | `_vkCmdDrawMultiEXT(subranges,...)` (`:1190`) | **no native multi-draw** → `drawPrimitives(indirectBuffer:)` else N× draw | 3 | 09 #5 | KK sets the precedent (fake multi-draw with indirect, `kk_cmd_draw.c:1464-1480`). |
| D6 | `_vkCmdDrawMultiIndexedEXT(...)` (`:1240`) + `m_multidraw_parameters_buffer` (`:1224`) | `drawIndexedPrimitives(...,indirectBuffer:)` | 3 | 09 #6 | Arg layout = `MTLDrawIndexedPrimitivesIndirectArguments`; the host already builds the array. |
| D7 | `draw_command_visitor` quad/fan/restart emulation (`VKVertexBuffers.cpp:70`,`:121`) | unchanged — output index GPU buffer + `MTL::IndexType` | 2 | 09 #8 | Fan/quads/restart are already host-side; nothing Metal-specific. |
| D8 | `draw_clause::is_immediate_draw` / `inline_vertex_array` (`draw_call.hpp:75`,`:81`) | materialise stream, then D1/D3 | 2 | 09 #16 | The inline array is already copied into the attribute ring host-side. |
| D9 | `draw_command_barriers` / primitive-restart barrier (`draw_call.hpp:25`,`:113`) | host-side index emulation (no GPU construct) | 2 | 09 #18 | Barriers are resolved on the CPU into index runs. |
| D10 | `multidraw_support.max_batch_size` 65536 (`device.cpp:110`) | chunk the indirect-args array | 2 | 09 #19 | No explicit per-call cap; chunk for cache/argument-buffer sizing. |
| D11 | MT `vkCmdDraw` sources its data from the `draw_parameters` SSBO index (`VKVertexProgram.cpp:87`,`:131`) | **stays a buffer** | 3 | 09 #22 | **The single most important affordance:** per-draw data is *not* in bind slots, so a batch can share one argument table — this is what makes indirect batching (strategy C) feasible at all. |

> With §3.7 the merged mapping is **167 construct rows** (156 + 11). The draw-path **batching
> strategies** (A–F) and the "explicit encoding is a wash unless batched" verdict live in `09`
> §4/§7 and are summarised in §2 verdict 7 and §4 below; they are not mapping rows.

---

## 4. The core architectural shifts (true across all scopes)

These are the handful of facts that recur in every scope and drive the design. Each is verified in
the cited docs.

1. **Metal 4 has no driver hazard tracking. Every dependency is explicit.** `MTLHazardTrackingMode`
   is irrelevant to `MTL4CommandQueue`; "the value of an `MTLResource`'s `hazardTrackingMode` has no
   effect on the work you submit to an `MTL4CommandQueue`" (01 §1.3 [V]). Correctness therefore
   rests on `barrierAfterStages:beforeQueueStages:`, `barrierAfterQueueStages:beforeStages:`, and
   `barrierAfterEncoderStages:beforeEncoderStages:`, each taking an `MTL4VisibilityOption`
   (`None`/`Device`/`ResourceAlias`) (02 §3.3, §5 rank1 [V]). **The access-mask half of every
   RPCS3 barrier is dropped; only the stage half survives** (01 §1.5 [V]). The TBDR fragment
   constraint forbids `afterEncoderStages` containing `.fragment`/`.tile` on a render encoder →
   feedback loops force pass splits (02 §5 rank1).

2. **Residency ≠ hazard tracking.** Apple states verbatim: *"Residency sets don't track hazards,
   so you need to account for hazards with fences and events."* (01 §1.4 [V]). Residency
   (`MTLResidencySet` + `addAllocation`/`commit`/`requestResidency`, attached to queue or command
   buffer) is a **separate mandatory subsystem**; it makes memory *reachable*, never *ordered*.
   "Bound ⇒ visible" and "coherent ⇒ ordered" are both false under Metal 4 (01 §1.5 [V]/[I]).

3. **`VkRenderPass` + `VkFramebuffer` → per-encoder `MTL4RenderPassDescriptor`.** The pass/framebuffer
   *objects* disappear; the encoder *is* the pass, and attachment load/store state is set on the
   descriptor (02 #26/#31, 04 #20, 05 §9 rank1 [V]). The packed `u64` renderpass key remains useful
   as a descriptor cache key (02 §5 rank5).

4. **The blit encoder is gone — copies/fills/mipmaps ride on `MTL4ComputeCommandEncoder`.**
   `MTL4::ComputeCommandEncoder` exposes all copy/fill/mipmap entry points; the KosmicKrisp bridge
   even defines `typedef void mtl_blit_encoder;` (04 §2.6 [V]). Scaled blits and D/S-aspect copies
   become compute/graphics meta passes (04 §2.6). *(Note the one sloppy counter-mention: 02 §2 #34
   and #36 say "blit.fillBuffer"/"or blit"; that is superseded by 04's explicit finding.)*

5. **`MTL4ArgumentTable` has no `setBytes` → push constants need transient buffers.** metal-cpp's
   `MTL4::ArgumentTable` exposes only `setAddress`/`setTexture`/`setSamplerState`/`setResource`
   (03 §3.5, #16 [V]), yet Apple's own cross-API table lists `[table setBytes:length:atIndex:]`.
   Until that surface gap is resolved on the target SDK, RPCS3's push constants (VS 4B
   `draw_parameters_offset`; FS 28B programmable-blending block) are bound through a transient
   buffer by GPU address, or compiled as function constants (03 §7 Q1). **This is a genuine
   unresolved `[U]` item**, not a verified impossibility.

6. **Apple-Silicon format gaps.** `D24_UNORM_S8_UINT` has no supportable native pixel format on
   Apple silicon (`isDepth24Stencil8PixelFormatSupported` is "Never supported on Apple Silicon"),
   so `D24_UNORM_S8_UINT → Depth32Float_Stencil8` is the only correct behaviour; RPCS3 already has
   this fallback via `gpu_formats_support.d24_unorm_s8=false` (04 §2.3, §7 Q4 [V]/[I]). Related:
   96/24-bit packed formats are `Invalid`; `A1B5G5R5Unorm`/`A8Unorm` are disabled in KK for a
   border-color HW bug; memoryless is textures-only and not a Vulkan memory type (01 §4 item5).

**Additional cross-scope shifts worth carrying:**
- **`MTL4CommandBuffer` holds no strong references** and is reusable; resource *retention* is the
  app's job, and the `garbage_collector` must not release before the matching residency removal
  (01 §1.5 item4, 02 §5 rank8 [V]).
- **Memory collapses to one heap / two storage modes.** KK registers exactly one heap and one
  memory type on unified memory (01 §3.1 [V]); `host_visible_coherent`/`device_local`/`device_bar`
  collapse to Shared-vs-Private, and `device_bar` is deleted (01 #9, §4 item3).
- **One `MTL4Compiler` per `MTLDevice`** (KK W11 heap corruption) constrains RPCS3's async
  pipeline-compile pool (03 §3.1 W11, 05 §7 Q10 [V]).

**Cross-cutting shifts from the design scopes (`06`–`11`).** The inventory above is *Vulkan→Metal*;
these are equally load-bearing and come from the engineering/architecture scopes:

- **Metal 4 is arm64 + macOS 26+ only — never Intel.** The wall is hardware **and** OS, not a flag.
  Recommended gate: keep app target/`LSMinimumSystemVersion` at **15.0**, build against the macOS 26
  SDK (weak-linking later symbols), and runtime-gate on `arm64 && macOS_major >= 26 &&
  supportsFamily(MTLGPUFamilyMetal4)`; arch-exclude the `MTL` sources on Intel, mirroring
  `bundle_should_include(kosmickrisp)=arm64` (`06` §3.2–§3.5).
- **Availability is not compiler-enforced.** metal-cpp carries **no `API_AVAILABLE` annotations** on
  its `MTL4*` headers (verified: 0 files), so a `MTL4*` reference outside an explicit
  `@available`/`__builtin_available` guard **compiles** and crashes at load on macOS 15–25. Every
  entry point needs a manual guard (`06` §3.4).
- **There is no Metal 3 fallback *build*.** Metal 3 and Metal 4 differ in the *command model*, not a
  capability toggle; a fallback is a **second backend**. MoltenVK already serves pre-26, so do not
  build a Metal 3 `MTL/` (`06` §4).
- **The shader path inserts one vendored component, not a new backend.** `GLSL → SPIR-V →
  SPIRV-Cross CompilerMSL → MSL → MTL4Compiler`; SPIRV-Cross is MIT, ~diff 2 to vendor like glslang,
  and needs **no** Metal-4 awareness because the argument table indexes the classic
  `[[buffer/texture/sampler(N)]]` spaces (`06` §2). Runtime MSL compilation is mandatory; binary
  archives/`MTL4Archive` are **not** shipped (`06` §2.2–§2.4).
- **Correctness is *relative*, not absolute, until guest readback exists.** With no golden-image
  suite, the only RSX-*objective* oracle is guest-observable memory (RSX/`cellGcm` readback); every
  pixel comparison is *differential* against KK/MVK and therefore inherits their bugs — both false
  positives and false negatives are expected (`07` §2.1–§2.2).
- **The draw path is a wash unless batched.** Explicit `MTL4` encoding only wins with argument tables
  (shift 5 above) **plus** indirect batching; it is measurable today with `frame_statistics_t`
  (`setup_time`/`draw_exec_time`), and RSX's `draw_parameters`-in-an-SSBO design is what makes
  batch-shared argument tables possible (`09` §7, D11).
- **The render-pass *scheduler* is the RSX-native design problem.** RSX has no render-pass object;
  the backend must *synthesise* `MTL4RenderPassDescriptor`s from hazards while preserving TBDR tile
  locality, rather than porting a VK-style render-pass cache. This is the make-or-break of the
  RSX-first plan (`10` §5, §10).
- **The verification harness is already in the tree.** Capture/replay is backend-agnostic core and
  `.rrc` is per-command annotated, so a `MTL` backend inherits a deterministic, command-localisable
  test loop for free once it is a `video_renderer` (`11` §3).

---

## 5. Cross-scope dependency graph

**Foundation → dependants.** Scope **01** (device, queues, allocator, residency) is the base:
nothing renders without a `MTL::Device`, an allocation policy, and a committed residency set.
Scope **03** (compiler → MSL, argument tables, PSOs) is the next gate: encoders cannot exist
without a pipeline and a table. Scope **02** (command buffers, barriers, fences, pass encoders)
is where 01's residency contract and 03's pipelines meet. Scope **04** (textures, formats,
samplers, RTT) binds *into* 03's argument table and rides on 02's compute-encoder copies and
barriers. Scope **05** (present) closes the loop and feeds 01's residency ownership via the
layer's dynamic residency set.

```
01 device/alloc/residency ──┬─► 03 compiler/MSL/PSO/argument-table ──┬─► 02 encoders/barriers/fences/passes ──┬─► 05 present/swapchain
                            │                                        │                                        │
                            └────────────────────────────────────────┴─► 04 textures/formats/samplers/RTT ───┘
```

**Hard ordering constraints (must land before):**
- 01 device + allocator + **one residency set** before *any* 03/04/05 work (01 §1.4, §5; 05 §6 [V]).
- 03 MSL/PSO + argument-table sizing before 02 can encode a draw and before 04 can bind textures.
- 02's **barrier model** and **resource-retention contract** before 04's copies/resolves and 05's
  present are correct (01 §1.5 hands 02 the retention contract; 02 §5 rank1).
- 02's compute-encoder copy path (no blit encoder) before 04's copy/clear/mipmap work (04 §2.6).

**Shared state created at scope boundaries (the risky seams):**
- **Residency ownership.** 01 owns the single device-level `MTLResidencySet` (+ commit/request
  ordering); 05 owns the `CAMetalLayer`'s dynamic residency set (`layer->residencySet()`) and must
  attach it to the same queue; 02 governs when removal is safe relative to in-flight CBs
  (01 §5 item6 + §1.5; 05 §6). **All three must agree on one queue-attachment policy.**
- **The argument table.** 03 owns creation/sizing (`maxBufferBindCount` etc.) from reflected
  bindings; **01** supplies buffers by `gpuAddress`, **04** supplies textures/samplers by
  `gpuResourceID`, **02** sets the table on the encoder per stage (03 #5/#17, #10-16; 04 #8). The
  12+ fixed slots in `pipeline_binding_table.h` are the shared contract (03 §1.4).
- **The format table.** 04 owns `VkFormat→MTLPixelFormat` and storage-mode/usage; **01** consumes it
  for allocation/storage mode, **03** for PSO `pixelFormat` and texture-buffer alignment, **02** for
  resolve/store-action legality (04 §2.3; 01 #10; 03 #23).
- **The render-pass descriptor.** 02 encodes it, 04 populates attachments/resolve/load-store, 05
  consumes it for the drawable (02 #26, 04 #20, 05 §6).
- **Push-constant emulation.** 03 owns the transient-buffer/function-constant decision; 01 must
  allocate the bump buffer; 02 must emit the bind (03 #16/#43).

**Cross-cutting dependencies (`06`–`11`).**
- **`06` (build/toolchain) gates everything and is a *prerequisite*, not a peer.** No `MTL` target
  compiles, and no runtime gate exists, until the macOS 26 SDK wiring, the SPIRV-Cross vendoring,
  and the availability guards are in place. It is a build-layer prerequisite for `03` and a
  packaging prerequisite for `05`.
- **`10` (RSX-native architecture) sits *above* 01–05.** It consumes their inventories but re-orders
  the implementation (hazard-derived scheduler replaces the render-pass cache; argument table
  replaces descriptor sets), and should be read *before* Phase 1 is committed.
- **`11` (verification loop) is orthogonal and parallel.** It depends only on the backend-agnostic
  capture/replay substrate that already exists and should be built *with* Phases 0–1, not after,
  because it is what makes Phase 1 measurable.
- **`07` and `08` are cross-references.** `07` supplies the oracle ladder that `11` implements; `08`
  supplies the row-level LOC that `10` re-baselines. Neither adds a build-order constraint.

---

## 6. Consolidated impossibility / not-recommended register

**This is the authoritative merged register.** It replaces 05 §9 (which was written before 01–04
existed, carried zero peer provenance, and whose claims are superseded where noted). Ranked by
severity × likelihood of blocking an MVP/parity. **Provenance** = scope doc + section + original
rank where applicable. Verdicts: **impossible** = no Metal API path; **not-recommended** =
expressible but a known trap; **hard** = real design needed; **opportunity** = optional upside.

> Deduped from **77 raw register entries** — 01 §4 (7) + 02 §4 (9) + 03 §4 (12) + 04 §4 (11) +
> 05 §4 (10) + 05 §9 (28 ranks) — to **37 merged rows** (R1–R37) below. Where two docs disagreed
> the corrected position is stated explicitly. **§6.1** records how the RSX-native frame (`10` §9)
> re-scores those rows; **§6.2** adds the rows contributed by the engineering scopes (`06`–`11`:
> R38–R47, RN1–RN3).

| # | Item | Verdict | Owning scope | Corrected position / why | Best-known workaround | Provenance |
|---|---|---|---|---|---|---|
| R1 | **Render-pass → per-encoder `MTL4RenderPassDescriptor` (no `VkRenderPass`/`VkFramebuffer` objects)** | hard | **02** (was mis-owned to 03 in 05 §9 rank1) | Metal captures attachment load/store per encoder. Touches every draw, MSAA, RTT. | `MTL4RenderPassDescriptor` + explicit `loadAction`/`storeAction`; attachment map; pass suspend/resume. | 02 §1.6, #26/#31, §5 rank5; 04 #20; 05 §9 rank1 |
| R2 | **No MSL codegen in RSX; shader translation correctness** | hard | 03 | Pipeline is `ucode→GLSL→SPIR-V`; no MSL target. Metal 4 changed the *command* model, not MSL. | Option A: SPIR-V→SPIRV-Cross `CompilerMSL` (recommended start). | 03 §1.2, §3.4; 05 §9 rank2 |
| R3 | **Descriptor sets/layouts/push-constants → argument tables** | hard | 03 (+01 buffers, +04 samplers) | No object analog; table *sizes* derived from reflected bindings; no `setBytes` for push constants. | `MTL4ArgumentTable` per stage; transient buffer / function constants for push constants. | 03 T1, #5/#16/#17; 02 #6; 05 §9 rank5 |
| R4 | **`VK_EXT_external_memory_host` / host-pointer import / `supports_passthrough_dma`** | **impossible for textures; feasible-but-constrained for buffers** (05's blanket "impossible" is **superseded**) | 01 | `newBufferWithBytesNoCopy:` **does** exist — buffers, page-aligned, whole-allocation, `deallocator` lifetime managed. RPCS3 only uses `memory_block_host` from `buffer::buffer(...host_pointer...)`, so the real use is feasible. But the DMA fast path (`VKDMA.cpp:306`) has no equivalent → `supports_passthrough_dma` forced `false`, CPU-copy fallback. | `newBufferWithBytesNoCopy:options:MTLStorageModeShared` + manual deallocator; CPU copy for DMA. | 01 §4 item2 + item7, #14/#32; 05 §4 item5 + §9 rank10 (corrected) |
| R5 | **Positional sparse binding at an exact offset** | impossible | 01 (+04) | Metal placement-sparse is **heap-relative** (`updateBufferMappings`), not address-relative. RPCS3 never uses sparse (feature-dump only). | Metal 4 heap-relative placement sparse; or accept-and-document. | 01 §4 item1, #27; 04 #42, §2.7; 05 §4 item6 + §9 rank10 |
| R6 | **`VK_EXT_conditional_rendering` — no command present** | impossible | 02 (+05) | No `vkCmdBeginConditionalRenderingEXT` analog. Host predicate `m_cond_render_buffer` must lower to indirect-command-buffer + visibility buffer, or host skipping. | `executeCommands(buffer:indirectBuffer:)` + occlusion visibility buffer; or CPU-side skip (RPCS3 already does on MVK). | 02 §4 item1 + #25; 05 §4 item7 + §9 rank9 |
| R7 | **Stage-gated semaphore waits (`pWaitDstStageMask`)** | impossible | 02 | `MTL4CommandQueue` events gate the queue, not a stage. RPCS3's async wait at `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` (`VKGSRender.cpp:2597`) has no 1:1 form. | Queue-level `waitForEvent`; re-derive the async wait. | 02 §4 item2, #8 |
| R8 | **Binary semaphores & `VkEvent` as objects** | impossible | 02 | Only monotonic `MTLEvent`/`MTLSharedEvent`. `VkFence`→commit feedback; events_v1/v2/gpu_label machinery deletable. | Monotonic 64-bit event values + commit feedback handler. | 02 §4 item3, #13/#14; 05 §9 rank12 |
| R9 | **`VK_DEPENDENCY_BY_REGION_BIT` + subpass self-dependencies** | impossible | 02 | TBDR forbids intra-pass fragment→fragment waits; these must become pass splits. | Pass split + producer/consumer barrier. | 02 §4 item4, #22/#28 |
| R10 | **MSL/Metal compiler & driver bug surface (maintenance)** | hard | cross-cutting | KK documents **18** `KK_WORKAROUND_*` (W1 uninit-scratch crash, W2 `while(true)`, W3 `simd_ballot`, W4/W5 discard, W6/W15/W18 coherency, W7 stencil-only sample_mask, W9 reconvergence, W10 bcsel, W11 compiler concurrency, W12/W13 index robustness, W14/W16/W17 precision/LOD). A native backend re-hits them. | KK-style register + per-OS gates; single `MTL4Compiler` per device; SPIRV-Cross mitigation flags (`manual_helper_invocation_updates`, `check_discarded_frag_stores`, `force_fragment_with_side_effects_execution`). | 01 §3.1; 03 §3.1, T4/T5; 05 §9 rank6, rank20, rank22 |
| R11 | **RSX per-title quirk surface (RTT layouts, MSAA resolve, spilling, formats)** | hard | 04 | The texture/RT cache is where emulator correctness lives; TBDR changes load/store economics. | Reuse `Common/texture_cache.h` + `surface_store.h` with new traits; re-derive per-title fixes. | 05 §9 rank7; 04 §5 |
| R12 | **Occlusion query model (count-of-samples, pass-scoped)** | hard | 02 (+05) | Metal has only a visibility result buffer (sample count), reset at pass start, selected per-draw. A query spanning passes needs accumulation. Reset semantics unverified. | `setVisibilityResultMode(_:offset:)` + resolve; read shared `MTLBuffer` after commit feedback. | 02 §1.7, §4 item1, #33/#34; 05 #20 + §9 rank9 |
| R13 | **Async streaming topology / `VKAsyncScheduler`** | not-recommended to port verbatim | 02 | Metal 4 wants many CBs batched into few commits on one queue; the transfer-queue + 16384-event pool should be re-derived. | Second `MTL4CommandQueue` joined by an event, or compute encoders on one queue. | 02 §4 NR3, §5 rank6, #41 |
| R14 | **True `VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER` (`usamplerBuffer`)** — subsumes the "vertex stride ×4 / vertex pulling" wall (05 §9 rank3) | impossible (bit-exact) | 03 (+01/04) | Metal has no 1D/samplerBuffer alias. RPCS3's whole vertex path already reads RSX streams via `usamplerBuffer` (`RSXVertexFetch.glsl:75`), i.e. **software vertex pulling is already how RPCS3 works** — so 05 §9 rank3's "need vertex pulling" is largely pre-satisfied; the real remaining work is emulating the texel-buffer binding, not designing a new vertex path. | `texture_buffer<T>` (2D, width 4096, `r32ui` alignment 4) or manual SSBO indexing; `texture_buffer_native=true`. | 03 §1.6, §4 item3, #12, T2; 05 §9 rank3 (reconciled) |
| R15 | **Push constants as a first-class register-fast inline blob** | impossible (no `setBytes`) | 03 | metal-cpp `MTL4::ArgumentTable` lacks `setBytes`; Apple's ObjC table lists it → **surface gap unresolved `[U]`**. | Transient bump-allocated `MTLBuffer` bound by GPU address, or function constants. | 03 §4 item4, #16, §7 Q1 |
| R16 | **PS3 "exploded" MSAA surfaces vs Metal pass-scoped resolve** | not a drop-in | 04 | RPCS3 keeps MSAA + a separate samplable resolve target with explicit resolve/unresolve/spill; Metal resolve is a store action at pass end. Memoryless cannot substitute. | Always resolve-on-store + persistent resolve texture; emulate `unresolve` by re-render. **D/S resolve** forced to meta passes (Metal sync bug); verify in Metal 4 runtime. | 04 §2.7, §5 rank1, §4 rows; 01 §4 item5 |
| R17 | **1024-unique-sampler cap (`maxArgumentBufferSamplerCount`)** | hard (intrinsic) | 04 (+01) | macOS caps argument-buffer samplers at ~1024 (iOS/tvOS 96). RPCS3's `max_unique_samplers` is 1024 under KK. **Rating differs: 01 #25 = 3, 04 §2.4 = 4.** | Bounded arena + eviction via packed `sampler_pool_key_t`; prefer argument-table `setSamplerState` to bypass argument-buffer accounting (unverified). | 04 §2.8, #/§2.4; 01 #25 |
| R18 | **Custom-border sampler color (`VK_EXT_custom_border_color`)** | impossible | 04 | Only TransparentBlack/OpaqueBlack/OpaqueWhite exist. | RPCS3 fallback `get_closest_border_color_enum` already exists. | 04 §2.4, #10/#43 |
| R19 | **Exact float LOD bias via `lodBias`** | not-recommended (precision loss) | 04 | Metal `lodBias` is S4.6 fixed-point; KK deliberately passes half-float bias shader-side. | Per-sampler shader-injected `lod_bias_fp16`, or accept quantization. | 04 §2.4, #17, §7 Q1 |
| R20 | **Block-texel views (`BLOCK_TEXEL_VIEW_COMPATIBLE`) + format reinterpretation groups** | hard | 04 | Views legal only within documented groups; KK disables layout optimization (memory blow-up) when the bit is set. | Mirror KK (disable optimization) or separate texture + re-upload. | 04 §2.5, #5, §5 rank3 |
| R21 | **Swapchain → `CAMetalLayer` drawable ring; `maximumDrawableCount ≤ 3`; blocking `nextDrawable`** | hard | 05 | RPCS3 assumed `minImageCount+2` images and non-blocking acquire; Metal caps drawables at 3 and `nextDrawable` can block/deadlock the RSX thread. | `setMaximumDrawableCount(3)` + `allowsNextDrawableTimeout(true)` + timeout path; "minimized" early-out. | 05 §5 rank2/rank4, #5/#9; 02 #11 |
| R22 | **`CAMetalLayer` creation/ownership from Qt `winId()` NSView** | hard | 05 | `metal_layer.mm:9` reads `((NSView*)view).layer` and **assumes a layer exists**; Qt 6 may not attach one / may share it. | `setWantsLayer:YES` + dedicated `CAMetalLayer`; coordinate with Qt. | 05 §5 rank7, #1; 01 #3 |
| R23 | **Frame pacing / VRR / tri-state vsync mismatch** | hard | 05 | `vsync_mode` FIFO/IMMEDIATE/MAILBOX/RELAXED has no faithful Metal mapping (bool + min-duration). **Rating differs: 05 table #8 = 3, ranked #1 = 4.** | FIFO↔`displaySyncEnabled=true`, IMMEDIATE↔`false`; approximate MAILBOX via `presentAfterMinimumDuration:`/`CAMetalDisplayLink`. | 05 §5 rank1, #8/#18 |
| R24 | **Dynamic `LINE_WIDTH`** | impossible (public API) | 03 | `wideLines` needs `MVK_USE_METAL_PRIVATE_API` in MoltenVK; disqualifies App-Store distribution. | Clamp to 1.0. | 03 §4 NR6, #28 |
| R25 | **`vkCmdBlitImage` / scaled blit** | not a drop-in | 04 (+05) | No direct equivalent; must become a graphics/compute meta pass. **Rating differs: 04 #28 = 4, 05 #12 = 3.** | KK `vk_meta_blit_image2` graphics pass. | 04 §2.6, #28; 05 #12 |
| R26 | **`VK_IMAGE_TILING_LINEAR` for 3D/compressed/depth/attachment textures** | impossible (KK rejects) | 04 | Linear tiling only routes through buffers for simple cases. | Route through a buffer; reject the unsupported cases. | 04 §4, #7 |
| R27 | **MetalFX is not a drop-in for FSR1** | not-recommended (temporal) | 05 (+04) | RPCS3 uses FSR1 **spatial** (no motion vectors/depth/jitter). | `MTLFXSpatialScaler` for FSR1 parity; temporal/frame-interpolation out of scope. | 05 §5 rank8, §9 rank18, #13 |
| R28 | **Screenshots/recording readback vs `framebufferOnly`** | hard | 05 | Reading the drawable texture requires `framebufferOnly=false` (bandwidth) or an owned intermediate. | Owned color texture + blit to drawable; enable readback only when capturing. | 05 §5 rank5, #19 |
| R29 | **Swapchain recreate is a hard-sync drain today, absent natively** | hard | 05 | `reinitialize_swapchain()` calls `vkDeviceWaitIdle` (`VKPresent.cpp:85`); Metal has no swapchain object — resize the layer and release in-flight drawables. | `setDrawableSize:` + in-flight drawable coordination. | 05 §5 rank3, #22 |
| R30 | **Savestate / heap snapshot & `data_heap` ring semantics** | hard (portable) | 01 | No Metal heap snapshot API; `managed_heap_snapshot_t` is a cursor map → ports, but interacts with residency commit ordering + the async scheduler. | Reimplement ring/heap snapshot over persistently-mapped shared `MTLBuffer`. | 01 §5 item7; 05 §9 rank24 |
| R31 | **Stereo/multi-view present** | not-recommended (low frequency) | 05 | No Metal "stereo swapchain"; needs two drawables/viewport split + output shader. | Two drawables or layered texture; drop to unsupported fallback. | 05 §5 rank10, §9 rank23 |
| R32 | **Geometry / tessellation / transform-feedback / pipeline-statistics** | impossible (no API) but **low RSX impact** | 02/03/05 | No geometry stage, no `MTL4RenderCommandEncoder` tessellation methods, no transform-feedback, no pipeline-stats. **PS3 games cannot reach any of these** (fixed VP/FP model, no GS/TS). | Not needed for RSX; only bites helper geometry passes. Do **not** port KK's GS/tess compute-emulation stack. | 03 §1.3, §4 item6/NR4; 05 §4 items1-3/item8, §9 rank8 |
| R33 | **fp64 / 64-bit atomics** | impossible on Apple GPUs | 03 | No fp64; SPIRV-Cross/MVK report 64-bit atomics unsupported. Not used by RSX shaders. | None needed. | 03 §4 items1-2 |
| R34 | **Per-sample coverage from a static pipeline sample mask without private API; stencil `sample_mask`** | impossible / hard | 03 (+04) | `MTLRenderPipelineDescriptor.sampleMask` needs `MVK_USE_METAL_PRIVATE_API`; public `[[sample_mask]]` hits KK W7 (ignored for stencil-only MS depth/stencil w/o color). | Conditional `discard_fragment()` at shader end; avoid depth-only MSAA stencil writes. | 03 §4 item5, T5; 05 §9 rank26 |
| R35 | **Explicit Vulkan memory model (`Coherent`/`Volatile`) reliance** | not-recommended | 02 (+01) | Metal 4 treats all resources untracked; coherency semantics differ; MSL has documented miscompiles (KK W6/W15). | Explicit barriers; don't rely on Vulkan memory-model guarantees. | 05 §4 item9, §9 rank4; 01 §1.5 |
| R36 | **`VkAllocationCallbacks` / queue-family ownership transfers** | impossible / meaningless | 05/02 | Metal has no allocator hooks (MVK ignores); single queue family makes ownership transfers meaningless. | Delete. | 05 §4 item10; 02 §4 item5 |
| R37 | **Pipeline-cache & shader-cache forward-compat; Xcode GPU capture; `MTL4Compiler` concurrency; ML encoder** | tricky / opportunity | 03 (+tooling) | `vk::shader_cache` keys on `"vulkan"`; needs a `"metal"` namespace. KK W8 Rosetta heap-size breaks capture. `MTL4MachineLearningCommandEncoder` is an unused opportunity. | New `"metal"` cache namespace + `MTL4PipelineDataSetSerializer`; one compiler per device; Metal capture in scheme. | 05 §9 rank20/rank25/rank27/rank28; 03 T7 |
| R38 | **Metal 4 on Intel/x86_64, or on macOS ≤ 25** | impossible | 06 | Metal 4 was built Apple-silicon-first; the supported-hardware set excludes Intel entirely. A hardware+OS wall, not a switch. | Keep Intel on MoltenVK; arch-exclude the `MTL` sources (mirror `bundle_should_include(kosmickrisp)`). | 06 §3.1, §9.1 |
| R39 | **Build-time precompilation of game shaders** | impossible | 06 | The shader set is generated at runtime from RSX ucode — per-title and unbounded. There is nothing to precompile ahead of time. | Runtime `MTL4Compiler` + the `"metal"` shader-cache namespace. | 06 §2.3, §9.2 |
| R40 | **A "Metal 3 fallback" as a build flag** | not-recommended | 06 | Metal 3 and 4 differ in the *command model* (submission/sync/encoders/binding/compiler), not a capability toggle — a fallback is a **second backend**. | MoltenVK already *is* the pre-26 fallback; ship MVK on ≤ 25. | 06 §4, §9.3 |
| R41 | **Dependable Metal execution on standard GitHub-hosted macOS runners** | impossible | 06/07 | Hosted runners expose no dependable `MTLDevice`; the whole GPU half of the verification loop is unavailable there. | Self-hosted Apple-silicon runner for the GPU tiers (07 §9.4); GPU-free rungs (H0/H1/H2 + null device) stay in hosted CI. | 06 §7.3; 07 §9.4 |
| R42 | **Compiler-enforced availability gating under metal-cpp** | impossible | 06 | metal-cpp's `MTL4*` headers carry **no `API_AVAILABLE`** (verified: 0 files), so an unguarded `MTL4*` reference compiles and crashes at load on macOS 15–25. | A manual `@available`/`__builtin_available` guard on every entry point + one central `supportsFamily(MTLGPUFamilyMetal4)` check. | 06 §3.4 |
| R43 | **A local pixel oracle / golden-image suite** | hard | 07 | RPCS3 has no golden-image GPU suite; cross-backend agreement is on a *different* rasteriser (TBDR vs not), so false positives **and** negatives are expected. | Differential goldens + a *classified* tolerance model + the rung-3 guest-readback spike. | 07 §2.1, §2.2, §3.4 |
| R44 | **One call issuing N draws *with* per-draw argument-table binds (ICB for draws)** | impossible / not-recommended | 09 | ICBs carry no argument table (`MTLIndirectCommandEncoder.hpp`); indirect draws carry only draw args, not binds. Any "one call, N fully-different draws" design is not expressible. | Host-built indirect batches sharing **one** argument table (RN2 / strategy C). | 09 §9, §4 |
| R45 | **Parallel per-draw encoding within a pass** | not-recommended | 09 | The encoder is stateful and order-sensitive, and RSX state is a serial FIFO; only *independent* command buffers may encode in parallel. | Parallelise only across independent CBs (transfer/compute), one allocator per thread. | 09 §9, §5.2 |
| R46 | **GPU-driven culling / compute-generated draws as a first milestone** | not-recommended | 09 | No RSX scene-graph analogue; KK leaves it a TODO. No correctness payoff for MVP. | Defer; use GPU work only for the conditional-render predicate (R6). | 09 §9, §6.2 |
| R47 | **Naively breaking the pass at every `vk::renderpass_op` boundary** | not-recommended | 09 | On TBDR each boundary is a store+reload of **all** attachments (a tile-memory flush). | Pass merge with suspend/resume, driven by the hazard scheduler (RN1). | 09 §9, §3 #12 |
| RN1 | **Surface-hazard encoder scheduler** | hard | 10 | RSX has no render-pass object; the backend must synthesise `MTL4RenderPassDescriptor`s from `surface_store` R/W/cyclic hazards while preserving TBDR tile locality. `diff 4` — the plan's make-or-break. | Hazard-derived pass boundaries + barriers; explicitly **not** a VK-style render-pass cache. | 10 §4.3, §5, §9 |
| RN2 | **Draw-clause indirect batcher** | hard | 10/09 | Needed for the `09` §7.2 win (`diff 3–4`). Requires a batch to share one argument table — a hard dependency on the binding-model decision. | `MTLDraw*IndirectArguments` + predicate patch + a shared frame-wide argument table. | 10 §9; 09 §4, §7 |
| RN3 | **Constant-buffer substitution for the VS/FS push-constant blocks** | hard | 10 | Removes R15 (no `setBytes`): the VS 4B `draw_parameters_offset` and the FS 28B blend block move to constant-buffer entries. `diff 2`. | Constant-buffer entries in the argument table; must stay coherent with the `draw_parameters` SSBO lifetime. | 10 §9, §4.2 |

**Not-recommended hygiene (kept out of the ranked table to avoid noise):** one render encoder per
RPCS3 draw (02 §4 NR1 — TBDR flushes tile memory at each encoder boundary); `MTL4VisibilityOptionResourceAlias`
for ordinary aliasing (02 §4 NR2 — `Device` suffices; `ResourceAlias` can flush to system memory);
emulating `VK_QUERY_CONTROL_PRECISE_BIT` (02 §4 NR4); reusing MoltenVK's shipped MSL (03 §4 NR1 —
deprecated by Khronos, Metal-3-era); using Godot's Metal driver as a **Metal 4** template (03 §4 NR2 —
it is a Metal 3 driver, `MTL4::MDCommandBuffer` is only forward-declared); hand-writing a NIR→MSL or
SPIR-V→MSL backend (03 §4 NR3); relying on `SEPARATE_SHADER_OBJECTS` (03 §4 NR5).

**Impossible rows with low RSX impact** (R5, R32, R33): they matter for *helper* passes and for
sharing code with MoltenVK, not for MVP.

### 6.1 Register deltas under the RSX-native frame (`10` §9)

Designing from the RSX contract rather than from Vulkan **creates no new API impossibilities, but it
re-scores several R-rows**, because their difficulty was a property of the *Vulkan object*, not of
RSX:

- **Moot under RSX-first** (the construct never appears): **R5** (sparse binding), **R32**
  (geometry/tess/TF/pipeline-stats), **R33** (fp64/atomics), **R36** (`VkAllocationCallbacks`/
  queue-family), **R3** (descriptor sets/layouts → argument table), **R15** (push constants →
  constant buffers, see RN3), **R9** (`DEPENDENCY_BY_REGION`/subpass self-deps), **R24** (dynamic
  `LINE_WIDTH` — kept only as a clamp-to-1.0 loss, not a design item).
- **Reshaped** (the RSX requirement survives; the Vulkan *shape* was the hard part): **R1** (the
  encoder *is* the pass), **R6** (CPU skip → compute predicate-patch), **R7** (sync anchored on
  surface hazards; the stage-gated wait need not be reproduced), **R8** (one monotonic
  `MTLSharedEvent` + commit feedback; object-level vocabulary collapses), **R12** (visibility
  buffer; the pass-spanning problem becomes a *scheduler* concern), **R13** (one serial FIFO; async
  is upload-only, batched as CBs), **R14** (already software-pulling; only the texel-buffer binding
  remains), **R4** (`passthrough_dma=false` + a shared `MTLBuffer`). **R17** is **likely moot** —
  `setSamplerState` binds samplers outside argument-buffer accounting (`[U]`, needs the SDK spike).
- **Unchanged** (framing-independent): R2, R10, R11, R16, R18–R23, R25–R31, R34, R35, R37 — MSL
  correctness, per-title quirks, exploded MSAA, border/LOD/texel-views, drawable ring,
  `CAMetalLayer`-from-Qt, pacing, blit/tiling, MetalFX, screenshots, savestate, stereo, sample mask,
  memory model, pipeline/shader cache.

**Net:** the register loses 8 rows wholesale and re-labels ~9, but **the rows that remain hard —
R1/R6/R11/R12/R16, R10's workaround surface, R43/R44/R47 and RN1/RN2 — are exactly the ones the
reorientation does *not* make easier.** That is why §2 verdict 8 calls RSX-first a ~15–20% *reshape*,
not a halving.

### 6.2 New rows from the engineering scopes (`06`–`11`)

R38–R47 and RN1–RN3 are in the table above; grouped by kind:

- **Platform walls (`06`):** R38 (no Metal 4 on Intel/≤ 25), R41 (no dependable Metal on hosted CI),
  R42 (availability not compiler-enforced under metal-cpp). These are *constraints*, not defects —
  they shape packaging and CI, not the backend body.
- **Product posture (`06`):** R39 (no build-time shader precompile), R40 (no Metal 3 build).
- **Verification (`07`):** R43 (no local pixel oracle) — the project risk.
- **Encode / dispatch (`09`):** R44 (no ICB-for-draws), R45 (no parallel per-draw encoding), R46
  (no GPU-driven first milestone), R47 (no naive pass breaks).
- **Net-new RSX-native work (`10`):** RN1 (hazard scheduler), RN2 (indirect batcher), RN3 (constant
  buffers) — the items the RSX-first plan *introduces* in exchange for the deletions in §4.

---

## 7. Suggested implementation phasing

Derived from the difficulty ratings and §5's dependency graph, and **reconciled with `08` §8.3 and
`10` §6.1**. Effort estimates are order-of-magnitude; they are not schedules.

**Phase 0 — Bring-up (weeks–low months).** Device (`CreateSystemDefaultDevice`), one
`MTL4CommandQueue` + allocator, `CAMetalLayer` creation on the `QWindow` (`setWantsLayer:YES`),
residency set, `waitForDrawable`/`commit`/`signalDrawable`/`present`, clear + present, and the
**overlay UI** renderer. No RSX draw. Proves scopes 01+05 seams and the Metal build. Difficulty
1–2 rows: 01 #1-7/#26, 05 #1-10/#14-16.

**Phase 1 — MVP: one draw path (~6–12 person-months).** The make-or-break: SPIRV-Cross
(`GLSL→SPIR-V→MSL`, 03 #1-2), `MTL4Compiler`/library, `MTL4ArgumentTable` sized from
`pipeline_binding_table.h` (03 #5/#17), **push-constant transient buffer** (03 #16), basic
`RenderPipelineState` (03 #7), the `MTL4RenderPassDescriptor` path (02 #26/#57), a minimal
explicit-barrier encoder loop (02 #48), vertex pulling via **texel buffer** (03 #83/R14), a
texture-cache subset (BGRA8 + `D32` fallback, 04 #107/#135), and present. **MVP definition:** a
`MTLGSRender` that renders VP/FP draws for a handful of bootable titles, correct enough to compare
frame output against KK/MVK. This is the northstar's "spec spike then one draw" (`../05` §10).

**Phase 2 — Memory + synchronization correctness.** Placement-heap allocator (01 #15-17), the
one-heap/two-storage-mode collapse (01 #10), **residency sets** and the retention contract
(01 #28 / R2/R8), the full barrier redesign (02 #48-55), fences/events (02 #44-47), `data_heap`
rings (01 #24), VMM accounting + purge (01 #21). Difficulty 4 rows dominate here.

**Phase 3 — Render targets, MSAA, formats, copies.** RTT + MSAA resolve (04 #131-133/R16),
format table (04 #135), samplers (04 #114-117), copies/fills/mipmaps on the **compute encoder**
(04 #119-128/R4-shift), render-pass clears (04 #124-125).

**Phase 4 — Queries, conditional rendering, async.** Occlusion (02 #64-67/R12), conditional-render
emulation (R6), async scheduler topology (R13), ZCULL.

**Phase 5 — Parity.** Savestate (R30), FSR/MetalFX (R27), stereo (R31), edge formats (R20/R26),
per-title quirks (R11), and the **KK workaround register** (R10).

**Critical path:** 01 (device/alloc/residency) → 03 (MSL/PSO/table) → 02 (barrier model) →
04 (RTT/MSAA/copies). 05 can proceed in parallel after 01. §4 shifts #1 (barriers) and #5 (push
constants) are the two items most likely to stall Phase 1–2.

**Reconciliation with the cross-cutting scopes (`06`–`11`).** Three independent bottoms-up estimates
now exist and bracket the same range:

| Source | Phase-1 / MVP figure | Whole-backend figure | Basis |
|---|---|---|---|
| `05` (northstar) | ~6–12 person-months | — | capability framing |
| `08` §8.1–§8.2 (bottom-up rows) | **8–12k LOC** | **20–35k LOC** (centre ~25k) | 60 rows / 112 files / 34,213 VK LOC |
| `10` §6.1 (RSX-first revision) | **~7–10k LOC** | **18–25k LOC** (centre ~21k) | `08` minus ~5–7k of deleted scaffolding |

Three adjustments the cross-cutting scopes force on the phases above:

1. **Phase 0 grows by `06`.** macOS 26 SDK wiring, SPIRV-Cross vendoring, availability guards, and
   packaging/signing are a *prerequisite*, not a Phase-1 detail (`06` §5–§6).
2. **The verification loop (`11`) is not a final phase — it starts in Phase 0.** The H0/H1 rungs
   (GPU-free unit tests + `xcrun metal` shader-translate) and the recording seam should land with
   Phases 0–1, because they are what makes Phase 1 measurable; the RRC-driven rungs (H3–H7) attach as
   soon as the backend is a `video_renderer` (`11` §8).
3. **If `10`'s RSX-first plan is adopted, the *shape* of Phases 1–3 changes** even though the order
   does not: the render-pass cache is replaced by the hazard scheduler (RN1), descriptor-set work by
   argument-table work, and the push-constant transient buffer by constant-buffer entries (RN3).

---

## 8. Open questions for the human

Merged and deduplicated from all eleven docs' `## … Open questions` sections; only decisions a human
genuinely must make are kept. *(05's "re-verify §9 provenance once 01–04 land" is now resolved by
this merge and is dropped; questions `06`–`11` raise that are *already answered* above — the
push-constant `setBytes` surface (Q16) and the sampler-cap bypass (Q27) — are not repeated.)*

**Product / target-scope decisions**
1. **Intel/AMD Mac support.** Support the `Managed` storage-mode path (and `didModifyRange:`), or
   target Apple silicon only (01 §7 Q2)? RPCS3's `device_bar` logic suggests discrete GPUs were
   once cared about.
2. **`supports_passthrough_dma`.** Confirm forcing it `false` (CPU-copy DMA fallback) is accepted,
   removing a perf feature on `VKDMA.cpp:306` — any title-regression budget? (01 §7 Q3; R4.)
3. **EDR/HDR in scope?** RSX output is SDR with a gamma-calibration pass; expose EDR (float16
   drawable) or keep BGRA8Unorm + sRGB only? (05 §7 Q5.)
4. **Stereo present.** Worth design effort, or "unsupported, fall back"? (05 §7 Q8.)
5. **Upscaler target.** MetalFX spatial as the FSR1 replacement, or keep a hand-rolled pass; is
   temporal (needs depth+motion) ever feasible for RSX? (05 §7 Q6.)
6. **Screenshots/recording.** Accept `framebufferOnly=false` always, or path through an owned
   texture? (05 §7 Q7.)
7. **Vertex pulling / texel-buffer width.** Do we accept the 2D-width tiling
   (`texel_buffer_texture_width`=4096) for RSX streams that may exceed 4096 u32, or read through a
   raw SSBO (which edits `RSXVertexFetch.glsl`)? (03 §7 Q4.)

**Sync / topology / pacing decisions**
8. **Frame-ring & queue topology.** One `MTL4CommandQueue` with many batched CBs, or a dedicated
   second queue for the async stream? This jointly decides `VKAsyncScheduler` (`VKDataHeapManager`)
   and the semaphore→event change, and the present sync model (`waitForDrawable`/`signalDrawable`
   only vs a `MTLSharedEvent` frame ring). (02 §7 Q5/Q8; 05 §7 Q4; R13.)
9. **Fence replacement policy.** Per-command-buffer feedback handler vs a per-frame
   `MTLSharedEvent` value (RPCS3 currently has one `VkFence` per CB). (02 §7 Q6.)
10. **`MTL4CommandAllocator` reset cadence.** One allocator per frame (triangle sample) vs per
    "submit scope", given RPCS3 creates primary + secondary + async CBs per frame. (02 §7 Q7.)
11. **Pacing owner.** Backend owns pacing (`presentAfterMinimumDuration`/`CAMetalDisplayLink`) or
    RPCS3's limiter stays the single owner (double-pacing risk)? (05 §7 Q2.)
12. **`CAMetalDisplayLink` vs `MTKView` vs plain `nextDrawable` loop** for the spike (no verified
    C++ binding for `CAMetalDisplayLink`). (05 §7 Q3.)
13. **Hazard-tracking strategy.** Translate each `vkCmdPipelineBarrier` call site 1:1, or build a
    deferred resource-state-tracking barrier pass (matches Metal 4 + threaded command generation)?
    (02 §7 Q1.)
14. **`VK_IMAGE_LAYOUT_*` disposition.** Keep `image::current_layout` as *logical* state to choose
    barriers/store actions, or delete it — and what replaces the decision logic in
    `VKRenderTargets.cpp`/`VKDraw.cpp`? (02 §7 Q2.)
15. **Conditional rendering first cut.** Host-side skipping for bring-up (cheap) with a later
    indirect-command-buffer path, or GPU-side from the start (some predicates are occlusion
    results, `VKGSRender.cpp:3098`)? (02 §7 Q3.)

**Shader / binding decisions**
16. **Push constants.** Confirm the target SDK's `MTL4::ArgumentTable` surface (`setBytes` absent in
    metal-cpp): per-draw bump-allocated constant buffer, or function-constant specialisation for the
    FS blend block? Blocks T1/T8. (03 §7 Q1; R15.)
17. **MSL version.** Target MSL 3.1 vs 4.0 — and does the pinned SPIRV-Cross revision emit it
    correctly? (03 §7 Q2.)
18. **Argument-buffer tiers vs argument tables.** Flatten set/binding to one flat slot space and
    bind classic `[[buffer(N)]]…`, or keep SPIRV-Cross argument buffers (Tier 1/2) and bind one
    argument-buffer address per set? Affects slot-limit headroom. (03 §7 Q3.)
19. **Barycentrics.** Does the target MSL expose `[[barycentric_coord]]`, and is the EXT-vs-AMD
    flavor distinction (`GLSLCommon.cpp:760-771`) preserved? (03 §7 Q5 [U].)
20. **ROP/programmable-blend ownership.** Does scope 03 own the input-attachment→framebuffer-fetch
    shader rewrite (it straddles binding #15 and shader body #43)? (03 §7 Q7.)
21. **Workaround policy.** Adopt KK's cosmetic workarounds unconditionally (init all scratch, wrap
    discards) even where Apple fixed the bug in macOS 27, or gate on OS version? Depends on
    RPCS3's min-OS target. (03 §7 Q6; R10.)

**Image / format decisions**
22. **Exploded MSAA model.** Commit to "always resolve-on-store + persistent resolve texture", or
    invest in a tile-memory/memoryless path for surfaces RSX never samples (needs an RSX-level
    audit)? (04 §7 Q2; R16.)
23. **LOD bias.** KK's shader-injected `lod_bias_fp16` (needs shader plumbing) or accept Metal's
    S4.6 quantization? (04 §7 Q1; R19.)
24. **`A1B5G5R5Unorm`/`A1R5G5B5`.** Follow KK (emulate; native is buggy) or Godot (native
    `BGR5A1Unorm`)? Does RSX exercise it? (04 §7 Q3.)
25. **`D24_UNORM_S8`.** Confirm always forcing `D32_SFLOAT_S8_UINT` on Apple silicon (RPCS3 already
    does); verify no title depends on 24-bit depth precision. (04 §7 Q4; §4 shift #6.)
26. **Block-texel views.** Mirror KK's disable-layout-optimization (memory cost) or separate
    textures + re-upload? (04 §7 Q5; R20.)
27. **Sampler count.** Confirm `setSamplerState`-on-argument-table bypasses the 1024
    argument-buffer cap; if so, drop aggressive eviction. (04 §7 Q6 [U]; R17.)
28. **1D / 3D-array-compat / linear tiling.** How many real titles hit each — first-class support or
    best-effort? (04 §7 Q7.)
29. **Depth/stencil resolve bug.** Does the Metal sync bug that forces KK to meta passes persist in
    the Metal 4 runtime, or can native `depthResolveFilter`/`stencilResolveFilter` be used?
    (04 §7 Q8 [U]; R16.)

**Platform / maintenance decisions**
30. **`CAMetalLayer` ownership from Qt.** Does the RPCS3 `QWindow`'s `NSView` already have a usable
    `CALayer`, and does Qt 6 render into that view (conflict)? (05 §7 Q1; R22.)
31. **`MTL4Compiler` concurrency.** One compiler per `MTLDevice` (KK W11) — does that constrain
    RPCS3's async pipeline-compile pool (`VKGSRender.cpp:700-705`, N threads)? (05 §7 Q10; R10.)
32. **Buffer device address / `MTLGPUAddress`.** RPCS3 doesn't use it today; is there a future
    bindless plan (scopes 03/04) that changes 01's allocation layout? (01 §7 Q4.)
33. **Purgeable memory for OOM pressure.** Keep freeing caches, or mark texture-cache resources
    `volatile` (`setPurgeableState`) as Apple recommends? Changes ownership semantics.
    (01 §7 Q5.)
34. **`data_heap` shadow.** Keep the VRAM-shadow + blit design, or use persistent shared rings
    everywhere given unified memory (TBDR economics may differ)? (01 §7 Q6.)

**Cross-cutting: build, platform & CI decisions (`06`–`07`)**

35. **Native renderer floor: macOS 26 or 27?** The Metal 4 API floor is 26; the existing *KK* gate is
    27. Pick one gate for both the native backend and KK. (06 §3.6)
36. **Keep shipping the Intel build?** MoltenVK-only with the `MTL` sources arch-excluded, or drop
    Intel entirely (macOS 27 reportedly ends Intel support)? (06 §3.5)
37. **metal-cpp vs direct Obj-C++** — and the **license posture**: metal-cpp is Apache-2.0 compiled
    *into* a GPLv2 binary, vs MoltenVK's *separately-loaded* dylib. SPIRV-Cross (MIT) is fine. (06 §5.1)
38. **Runtime-only MSL, or offline-precompile the fixed helper shaders?** The latter pulls the ~700 MB
    Metal toolchain into the build. (06 §2.3)
39. **Should the macOS build be able to build *without* Vulkan** (`USE_VULKAN=OFF`) once a native
    backend exists? (06 §5.3)
40. **CI budget:** compile + MSL validation only, a self-hosted arm64/macOS-26 nightly, or a paid GPU
    runner? (06 §7.4; 07 §9.4)

**Cross-cutting: verification decisions (`07` + `11`)**

41. **Is a self-hosted Apple-silicon runner acceptable** to unlock GPU CI, or must everything be
    hosted (in which case GPU verification is permanently manual)? — the single biggest lever. (07 §9.4)
42. **Do we adopt golden-image tests at all**, given driver/GPU-family drift and the re-baseline
    burden, or keep verification to capture-replay + `frame_debug` diffs + human review? (07 §3.4)
43. **Tolerance definition for rung 4:** exact metric, thresholds, per-region buckets? (07 §4.3)
44. **Do we invest in rung 3** (guest-observable readback/MSAA/ZCULL correctness)? It is the only
    objective oracle, but its scope is unknown. (07 §2.2; 11 Q3)
45. **Capture corpus:** what set represents "enough" coverage, and where do the `.rrc.gz` files live
    (repo size, licensing)? Who owns it, and what minimum gates a merge? (07 §3.3; 11 Q2)
46. **Does the RRC format record enough for a *pixel-complete* replay** of every title (spilled
    surfaces, MSAA resolve history)? H5/H6 depend on it. (11 Q3)
47. **Is the null device worth building** to bring H2/H3 into hosted CI? (11 Q1)
48. **Ship a Metal Performance HUD / timestamp overlay** in addition to the existing CPU overlay, or
    keep the overlay CPU-only and use the HUD for GPU numbers? (07 §7.4)
49. **`MTL_SHADER_VALIDATION` in Tier B:** always-on or per-pipeline? (07 §6.2)
50. **Residency-set cap (32)** in the texture cache without per-frame residency churn? (07 §8.2)

**Cross-cutting: architecture decisions (`08`–`10`)**

51. **The strategic gate: is a native backend worth it at all**, versus a healthy KosmicKrisp (which
    already runs directly on Metal 4), given that RPCS3 is CPU-limited and Dolphin's native-Metal win
    over *healthy* MoltenVK was only +23%/+30%? Decide **before** any scheduler work. (10 §10.1) —
    **the top decision in the whole set.**
52. **Commit to a hazard-derived pass scheduler** (RSX-first) rather than a VK-style render-pass
    cache? If declined, `10`'s savings evaporate and the plan degenerates to the `08` port. (10 §10.2; RN1)
53. **Binding-model ceiling: one frame-wide argument table mutated per draw, or per-state tables?**
    Strategy-C indirect batching needs a *batch* to share one table. (09 Q1; 10 Q3)
54. **Uniformity vs fast path:** model *every* RSX draw as indirect (KK-style), or keep direct /
    instanced on the fast path and go indirect only for subrange batches? (09 Q2)
55. **Shading contract:** may we extend it with extra per-draw indirection (beyond the
    `draw_parameters` SSBO) so indirect batches can share one argument table? (09 Q5)
56. **`supports_last_provoking_vertex = false`:** which titles regress under the smooth-interpolation
    fallback? (10 §10.6)
57. **ICB research budget:** spike how ICB binds resolve under Metal 4, or declare ICBs out of scope
    for draws and revisit only if a GPU-driven need appears? (09 Q7)
58. **Profiling in milestone 1:** require `MTL4CounterHeap` timestamps, or is the CPU
    `setup_time`/`draw_exec_time` overlay enough to judge the win/loss? (09 Q6)
59. **Can the generic caches bind under ~2k LOC of `MTLTraits`?** A stub-compile spike decides whether
    RN1 is diff-4 or diff-5. (08 §10.1)

---

## 9. Sources

Merged and deduplicated from all eleven docs' `## … Sources` sections. *(Detail docs carry
additional per-scope citations inline in their own sources section; this is the consolidated list.)*

**Apple — Metal 4 & core API**
- Understanding the Metal 4 core API — <https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api>
- Metal 4 — <https://developer.apple.com/documentation/metal/metal-4>
- Using the Metal 4 compilation API — <https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api>
- Drawing a triangle with Metal 4 (sample) — <https://developer.apple.com/documentation/metal/drawing-a-triangle-with-metal-4>
- `MTL4CommandQueue` — <https://developer.apple.com/documentation/metal/mtl4commandqueue>
- `MTL4CommandBuffer` — <https://developer.apple.com/documentation/metal/mtl4commandbuffer>
- `MTL4CommandAllocator` — <https://developer.apple.com/documentation/metal/mtl4commandallocator>
- `MTL4CommandEncoder` — <https://developer.apple.com/documentation/metal/mtl4commandencoder>
- `MTL4ArgumentTable` — <https://developer.apple.com/documentation/metal/mtl4argumenttable>
- `MTL4RenderCommandEncoder` — <https://developer.apple.com/documentation/metal/mtl4rendercommandencoder>
- `MTL4RenderPassDescriptor` — <https://developer.apple.com/documentation/metal/mtl4renderpassdescriptor>
- `MTL4ComputeCommandEncoder` — <https://developer.apple.com/documentation/metal/mtl4computecommandencoder>
- `MTL4CounterHeap` — <https://developer.apple.com/documentation/metal/mtl4counterheap>
- `MTL4VisibilityOptions` — <https://developer.apple.com/documentation/metal/mtl4visibilityoptions>
- `MTL4CommitOptions` — <https://developer.apple.com/documentation/metal/mtl4commitoptions>
- `MTL4CommitFeedback` — <https://developer.apple.com/documentation/metal/mtl4commitfeedback>
- `barrier(afterEncoderStages:beforeEncoderStages:visibilityOptions:)` — <https://developer.apple.com/documentation/metal/mtl4commandencoder/barrier(afterencoderstages:beforeencoderstages:visibilityoptions:)>
- `barrier(afterQueueStages:beforeStages:visibilityOptions:)` — <https://developer.apple.com/documentation/metal/mtl4commandencoder/barrier(afterqueuestages:beforestages:visibilityoptions:)>
- `barrier(afterStages:beforeQueueStages:visibilityOptions:)` — <https://developer.apple.com/documentation/metal/mtl4commandencoder/barrier(afterstages:beforequeuestages:visibilityoptions:)>
- `writeTimestamp(granularity:after:counterHeap:index:)` — <https://developer.apple.com/documentation/metal/mtl4rendercommandencoder/writetimestamp(granularity:after:counterheap:index:)>
- `MTL4CommandBuffer.useResidencySet(_:)` — <https://developer.apple.com/documentation/metal/mtl4commandbuffer/useresidencyset(_:)>
- `MTL4CommandQueue.updateMappings(buffer:heap:operations:)` — <https://developer.apple.com/documentation/metal/mtl4commandqueue/updatemappings(buffer:heap:operations:)>
- `MTL4CommandQueue.signalDrawable(_:)` — <https://developer.apple.com/documentation/metal/mtl4commandqueue/signaldrawable(_:)>
- `MTL4CommandQueue.waitForDrawable(_:)` — <https://developer.apple.com/documentation/metal/mtl4commandqueue/waitfordrawable(_:)>

**Apple — resources, memory, residency, sync, formats, passes, queries**
- `MTLResidencySet` — <https://developer.apple.com/documentation/metal/mtlresidencyset>
- `MTLResidencySetDescriptor` — <https://developer.apple.com/documentation/metal/mtlresidencysetdescriptor>
- Simplifying GPU resource management with residency sets — <https://developer.apple.com/documentation/metal/simplifying-gpu-resource-management-with-residency-sets>
- `MTLHeap` — <https://developer.apple.com/documentation/metal/mtlheap>
- `MTLHeapDescriptor` — <https://developer.apple.com/documentation/metal/mtlheapdescriptor>
- `MTLHeapType` — <https://developer.apple.com/documentation/metal/mtlheaptype>
- Memory heaps — <https://developer.apple.com/documentation/metal/memory-heaps>
- `MTLAllocation` — <https://developer.apple.com/documentation/metal/mtlallocation>
- `MTLStorageMode` — <https://developer.apple.com/documentation/metal/mtlstoragemode>
- `MTLStorageMode.memoryless` — <https://developer.apple.com/documentation/metal/mtlstoragemode/memoryless>
- Setting resource storage modes — <https://developer.apple.com/documentation/metal/setting-resource-storage-modes>
- Choosing a resource storage mode for Apple GPUs — <https://developer.apple.com/documentation/metal/choosing-a-resource-storage-mode-for-apple-gpus>
- `MTLHazardTrackingMode` — <https://developer.apple.com/documentation/metal/mtlhazardtrackingmode>
- `MTLResource.hazardTrackingMode` — <https://developer.apple.com/documentation/metal/mtlresource/hazardtrackingmode>
- `MTLResource.storageMode` — <https://developer.apple.com/documentation/metal/mtlresource/storagemode>
- `MTLPurgeableState` — <https://developer.apple.com/documentation/metal/mtlpurgeablestate>
- Resource fundamentals — <https://developer.apple.com/documentation/metal/resource-fundamentals>
- Reducing the memory footprint of Metal apps — <https://developer.apple.com/documentation/metal/reducing-the-memory-footprint-of-metal-apps>
- `MTLDevice.makeHeap(descriptor:)` — <https://developer.apple.com/documentation/metal/mtldevice/makeheap(descriptor:)>
- `MTLDevice.makeResidencySet(descriptor:)` — <https://developer.apple.com/documentation/metal/mtldevice/makeresidencyset(descriptor:)>
- `MTLDevice.recommendedMaxWorkingSetSize` — <https://developer.apple.com/documentation/metal/mtldevice/recommendedmaxworkingsetsize>
- `MTLDevice.currentAllocatedSize` — <https://developer.apple.com/documentation/metal/mtldevice/currentallocatedsize>
- `MTLDevice.hasUnifiedMemory` — <https://developer.apple.com/documentation/metal/mtldevice/hasunifiedmemory>
- `MTLDevice.maxArgumentBufferSamplerCount` — <https://developer.apple.com/documentation/metal/mtldevice/maxargumentbuffersamplercount>
- `MTLDevice.supportsTextureSampleCount(_:)` — <https://developer.apple.com/documentation/metal/mtldevice/supportstexturesamplecount(_:)>
- `isDepth24Stencil8PixelFormatSupported` — <https://developer.apple.com/documentation/metal/mtldevice/isdepth24stencil8pixelformatsupported>
- Resource synchronization — <https://developer.apple.com/documentation/metal/resource-synchronization>
- Synchronizing stages within a pass — <https://developer.apple.com/documentation/metal/synchronizing-stages-within-a-pass>
- Synchronizing passes with producer barriers — <https://developer.apple.com/documentation/metal/synchronizing-passes-with-producer-barriers>
- Synchronizing passes with consumer barriers — <https://developer.apple.com/documentation/metal/synchronizing-passes-with-consumer-barriers>
- `MTLStages` — <https://developer.apple.com/documentation/metal/mtlstages>
- `MTLStoreAction` — <https://developer.apple.com/documentation/metal/mtlstoreaction>
- `MTLLoadAction` — <https://developer.apple.com/documentation/metal/mtlloadaction>
- Setting load and store actions — <https://developer.apple.com/documentation/metal/setting-load-and-store-actions>
- `MTLEvent` — <https://developer.apple.com/documentation/metal/mtlevent>
- `MTLSharedEvent` — <https://developer.apple.com/documentation/metal/mtlsharedevent>
- Render passes (API collection) — <https://developer.apple.com/documentation/metal/render-passes>
- `MTLRenderPassAttachmentDescriptor` (resolve, store actions) — <https://developer.apple.com/documentation/metal/mtlrenderpassattachmentdescriptor>
- `MTLPixelFormat` — <https://developer.apple.com/documentation/metal/mtlpixelformat>
- Choosing a pixel format for a texture — <https://developer.apple.com/documentation/metal/choosing-a-pixel-format-for-a-metal-texture>
- `MTLSamplerDescriptor` — <https://developer.apple.com/documentation/metal/mtlsamplerdescriptor>
- `MTLSamplerAddressMode` / border colors — <https://developer.apple.com/documentation/metal/mtlsampleraddressmode>
- `MTLTexture` — <https://developer.apple.com/documentation/metal/mtltexture>
- Texture views (`newTextureView`) — <https://developer.apple.com/documentation/metal/mtltexture/newtextureview(pixelformat:)>
- `MTLTextureUsage` — <https://developer.apple.com/documentation/metal/mtltextureusage>
- Sparse resources — <https://developer.apple.com/documentation/metal/sparse-resources>
- Creating sparse heaps and sparse textures — <https://developer.apple.com/documentation/metal/creating-sparse-heaps-and-sparse-textures>
- Culling occluded geometry using the visibility result buffer — <https://developer.apple.com/documentation/metal/culling-occluded-geometry-using-the-visibility-result-buffer>
- Improving CPU performance by using argument buffers — <https://developer.apple.com/documentation/metal/improving-cpu-performance-by-using-argument-buffers>
- `MTLDrawable` — <https://developer.apple.com/documentation/metal/mtldrawable>
- `MTL4MachineLearningCommandEncoder` / `MTLTensor` (Metal 4 ML) — <https://developer.apple.com/documentation/metal/metal-4>
- Metal Feature Set Tables (PDF) — <https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf>
- Metal Shading Language Specification v4.1 (2026-06-04) — <https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf>
- MSL Specification v4 (2025-10-23) — <https://developer.apple.com/metal/metal-shading-language-specification.pdf>
- Apple metal-cpp — <https://developer.apple.com/metal/cpp/>
- WWDC25 updates — <https://developer.apple.com/documentation/updates/wwdc2025>

**Apple — QuartzCore / display / capture**
- `CAMetalLayer` — <https://developer.apple.com/documentation/quartzcore/cametallayer>
- `CAMetalLayer.maximumDrawableCount` — <https://developer.apple.com/documentation/quartzcore/cametallayer/maximumdrawablecount>
- `CAMetalLayer.presentsWithTransaction` — <https://developer.apple.com/documentation/quartzcore/cametallayer/presentswithtransaction>
- `CAMetalLayer.colorspace` — <https://developer.apple.com/documentation/quartzcore/cametallayer/colorspace>
- `CAMetalDisplayLink`/`CADisplayLink` — <https://developer.apple.com/documentation/quartzcore/cadisplaylink>
- Using color spaces to display HDR content — <https://developer.apple.com/documentation/metal/using-color-spaces-to-display-hdr-content>
- Capturing a Metal workload in Xcode — <https://developer.apple.com/documentation/xcode/capturing-a-metal-workload-in-xcode>
- Metal Best Practices Guide: Drawables — <https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/Drawables.html>
- MetalFX — <https://developer.apple.com/documentation/metalfx>

**WWDC sessions**
- 205 Discover Metal 4 — <https://developer.apple.com/videos/play/wwdc2025/205/>
- 254 Explore Metal 4 games — <https://developer.apple.com/videos/play/wwdc2025/254/>
- 211 Go further with Metal 4 games — <https://developer.apple.com/videos/play/wwdc2025/211/>
- 262 Combine Metal 4 machine learning and graphics — <https://developer.apple.com/videos/play/wwdc2025/262/>
- 10103 Boost performance with MetalFX Upscaling (WWDC22) — <https://developer.apple.com/videos/play/wwdc2022/10103>
- 10162 Metal mesh shaders (WWDC22) — <https://developer.apple.com/videos/play/wwdc2022/10162>
- 10147 Optimize for variable refresh rate displays (WWDC21) — <https://developer.apple.com/videos/play/wwdc2021/10147>
- 10161 Explore HDR rendering with EDR (WWDC21) — <https://developer.apple.com/videos/play/wwdc2021/10161>
- 612 Metal Game Performance Optimization (WWDC18) — <https://developer.apple.com/videos/play/wwdc2018/612>

**Apple — game-porting-toolkit skills (canonical Metal 4 cross-API maps)** `[V]`
- `translating-to-metal4-api/SKILL.md` — <https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/translating-to-metal4-api/SKILL.md>
- `translating-to-metal4-api/references/cross-api-translation.md` — <https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/translating-to-metal4-api/references/cross-api-translation.md>
- `translating-to-metal4-api/references/gpu-queries-and-capabilities.md` — <https://github.com/apple/game-porting-toolkit/blob/main/game-porting-skills/skills/translating-to-metal4-api/references/gpu-queries-and-capabilities.md>
- `translating-to-metal4-api/references/tbdr-architecture.md` — <https://github.com/apple/game-porting-toolkit/blob/main/game-porting-skills/skills/translating-to-metal4-api/references/tbdr-architecture.md>
- `managing-metal4-synchronization/SKILL.md` — <https://github.com/apple/game-porting-toolkit/blob/main/game-porting-skills/skills/managing-metal4-synchronization/SKILL.md>
- `creating-metal4-shader-pipelines/SKILL.md` — <https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/creating-metal4-shader-pipelines/SKILL.md>
- `integrating-metal-shaderconverter-shaders/SKILL.md` — <https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/integrating-metal-shaderconverter-shaders/SKILL.md>
- `integrating-metal-shaderconverter-shaders/references/binding-model.md` — <https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/integrating-metal-shaderconverter-shaders/references/binding-model.md>
- `integrating-metal-shaderconverter-shaders/references/geometry-tessellation.md` — <https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/integrating-metal-shaderconverter-shaders/references/geometry-tessellation.md>
- `compiling-with-metal-shaderconverter/SKILL.md` — <https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/compiling-with-metal-shaderconverter/SKILL.md>

**SPIRV-Cross / MoltenVK**
- SPIRV-Cross README — <https://raw.githubusercontent.com/KhronosGroup/SPIRV-Cross/main/README.md>
- `spirv_msl.hpp` — <https://raw.githubusercontent.com/KhronosGroup/SPIRV-Cross/main/spirv_msl.hpp>
- `spirv_common.hpp` — <https://raw.githubusercontent.com/KhronosGroup/SPIRV-Cross/main/spirv_common.hpp>
- SPIRV-Cross wiki — <https://github.com/KhronosGroup/SPIRV-Cross/wiki>
- SPIRV-Cross MSL backend (DeepWiki) — <https://deepwiki.com/KhronosGroup/SPIRV-Cross/3.1-metal-shading-language-(msl)>
- SPIRV-Cross issue #2229 — <https://github.com/KhronosGroup/SPIRV-Cross/issues/2229>
- SPIRV-Cross issue #1840 (`gl_CullDistance`) — <https://github.com/KhronosGroup/SPIRV-Cross/issues/1840>
- SPIRV-Cross issue #1004 — <https://github.com/KhronosGroup/SPIRV-Cross/issues/1004>
- MoltenVK README (deprecation; "Non-compliance") — <https://github.com/KhronosGroup/MoltenVK/blob/main/README.md>
- MoltenVK `Docs/Whats_New.md` — <https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/Docs/Whats_New.md>
- MoltenVK runtime user guide limitations — <https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md#limitations>
- MoltenVK issue #2560 (New features in Metal 4) — <https://github.com/KhronosGroup/MoltenVK/issues/2560>
- MoltenVK issue #2638 — <https://github.com/KhronosGroup/MoltenVK/issues/2638>
- MoltenVK issue #1700 (sparse textures) — <https://github.com/KhronosGroup/MoltenVK/issues/1700>
- MoltenVK issue #1524 (geometry shaders) — <https://github.com/KhronosGroup/MoltenVK/issues/1524>
- MoltenVK issue #1085 (use survey) — <https://github.com/KhronosGroup/MoltenVK/issues/1085>

**Mesa KosmicKrisp (Vulkan→Metal 4 driver)**
- Source tree — <https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/kosmickrisp>
- KosmicKrisp docs — <https://docs.mesa3d.org/drivers/kosmickrisp.html>
- KosmicKrisp workarounds (W1–W18) — <https://docs.mesa3d.org/drivers/kosmickrisp/workarounds.html> / <https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/docs/drivers/kosmickrisp/workarounds.rst>

**Godot (Metal driver)**
- `drivers/metal` — <https://github.com/godotengine/godot/tree/master/drivers/metal>
- `pixel_formats.cpp` — <https://github.com/godotengine/godot/blob/master/drivers/metal/pixel_formats.cpp>
- `rendering_shader_container_metal.cpp`, `rendering_device_driver_metal.cpp`, `rendering_context_driver_metal.cpp` (in the `drivers/metal` tree above; **Metal 3, not Metal 4**)

**metal-cpp headers** `[V]` (local `/tmp/metal4-refs/metal-cpp/Metal`)
- `MTL4ArgumentTable.hpp`, `MTL4Compiler.hpp`, `MTL4RenderPipeline.hpp`, `MTL4LibraryDescriptor.hpp`, `MTL4LibraryFunctionDescriptor.hpp`, `MTL4SpecializedFunctionDescriptor.hpp`, `MTL4PipelineDataSetSerializer.hpp`, `MTL4Archive.hpp`, `MTL4BinaryFunction.hpp`, `MTL4LinkingDescriptor.hpp`
- `MTL4CommandEncoder.hpp`, `MTL4CommandQueue.hpp`, `MTL4RenderCommandEncoder.hpp`, `MTL4RenderPass.hpp`, `MTL4CommandBuffer.hpp`, `MTL4CommandAllocator.hpp`, `MTL4ComputeCommandEncoder.hpp`
- `MTLDevice.hpp`, `MTLHeap.hpp`, `MTLResource.hpp`, `MTLResidencySet.hpp`, `MTLTexture.hpp`, `MTLSampler.hpp`, `MTLRenderPass.hpp`
- `QuartzCore/CAMetalLayer.hpp`, `QuartzCore/CAMetalDrawable.hpp`, `Metal/MTLDrawable.hpp`, `MetalFX/MTL4FX*.hpp`
- bkaradzic metal-cpp mirrors — <https://raw.githubusercontent.com/bkaradzic/metal-cpp/metal-cpp_27/Metal/MTL4CommandEncoder.hpp>, `.../MTL4CommandQueue.hpp`, `.../MTL4RenderCommandEncoder.hpp`, `.../MTL4RenderPass.hpp`
- docs.rs mirrors — <https://docs.rs/objc2-metal/0.3.2/aarch64-apple-visionos/src/objc2_metal/generated/MTL4RenderCommandEncoder.rs.html>, <https://docs.rs/objc2-metal/0.3.2/aarch64-apple-ios-macabi/src/objc2_metal/generated/MTL4Counters.rs.html>
- livingston apple-docs `MTLResidencySet.md` — <https://github.com/livingston/apple-docs/blob/main/documentation/Metal/MTLResidencySet.md>

**Apple metal-4-basics sample** `[V]` (local `/tmp/metal4-refs/metal-4-basics`)
- `Metal4Basics/Sources/Engine/Metal4Renderer.swift`, `Sources/Shaders/PBR.metal`, `Sources/Utility/BufferUtilities.swift`

**Third-party / prior art / issues**
- Metal by Example, "Getting Started with Metal 4" — <https://metalbyexample.com/metal-4/>
- Metal 4 Essentials (blakecrosley) — <https://blakecrosley.com/blog/metal-4-essentials>
- dev.to "WWDC 2025 – Discover Metal 4" — <https://dev.to/arshtechpro/wwdc-2025-discover-metal-4-23f2>
- ANGLE occlusion queries in the Metal backend — <https://chromium.googlesource.com/angle/angle/+/main/src/libANGLE/renderer/metal/doc/OcclusionQueries.md>
- Blender Metal command-buffer manager — <https://github.com/blender/blender/blob/main/source/blender/gpu/metal/mtl_command_buffer.mm>
- Meta Horizon: loads/stores/passes & subpass↔tile-shading — <https://developers.meta.com/horizon/documentation/unreal/po-advanced-gpu-pipelines/>
- `VK_KHR_dynamic_rendering` — <https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_dynamic_rendering.html>
- `VK_EXT_transform_feedback` — <https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_transform_feedback.html>
- `VK_EXT_conditional_rendering` sample — <https://docs.vulkan.org/samples/latest/samples/extensions/conditional_rendering/README.html>
- Metal argument-buffer sampler limit (Chromium) — <https://issues.chromium.org/477317117>
- wgpu sampled-texture-limit issue — <https://github.com/gfx-rs/wgpu/issues/2898>
- "Tier 2 argument buffers above 500000" — <https://www.reddit.com/r/macgaming/comments/v98n1r/apple_engineer_tier_2_argument_buffers_above/>
- MetalFX quality discussion — <https://www.reddit.com/r/macgaming/comments/1m4ysmo/metal_fx_the_worst_upscaler_in_2025/>
- zed main-thread `nextDrawable` hang — <https://github.com/zed-industries/zed/issues/53390>
- Cemu #444 (vertex stride) — <https://github.com/cemu-project/Cemu/issues/444>
- LunarG "The State of Vulkan on Apple – Oct. 2026" — <https://www.lunarg.com/the-state-of-vulkan-on-apple-oct-2026/>
- Dolphin Progress Report Jul–Aug 2022 — <https://dolphin-emu.org/blog/6I/>
- brackeen/gltf-viewer (metal-cpp) — <https://github.com/brackeen/gltf-viewer>

**Added by the cross-cutting scopes (`06`–`11`)**
- Apple — `MTL4Compiler` — <https://developer.apple.com/documentation/metal/mtl4compiler>
- Apple — `MTL4Archive` — <https://developer.apple.com/documentation/metal/mtl4archive>
- Apple — `MTLIndirectCommandBuffer` — <https://developer.apple.com/documentation/metal/mtlindirectcommandbuffer>
- Apple — `MTLDrawPrimitivesIndirectArguments` — <https://developer.apple.com/documentation/metal/mtldrawprimitivesindirectarguments>
- Apple — `MTLDevice.supportsFamily(_:)` / `MTLGPUFamily` — <https://developer.apple.com/documentation/metal/mtldevice/supportsfamily(_:)>
- Apple — Monitoring your Metal app's graphics performance (HUD) — <https://developer.apple.com/documentation/xcode/monitoring-your-metal-apps-graphics-performance>
- Apple — Metal developer tools: `MTL_DEBUG_LAYER`, `MTL_SHADER_VALIDATION`, `MTL_HUD_ENABLED`, `.gputrace` capture (see `06` §7, `07` §6–§7)
- Apple — `MTL4PipelineDataSetSerializer`, `MTL4BinaryFunction`, `MTL4StaticLinkingDescriptor`, `MTL4SpecializedFunctionDescriptor` (metal-cpp headers, listed above; see `06` §2.5)
- RPCS3 in-repo (build / packaging / CI) — `.ci/build-mac-homebrew.sh`, `.ci/deploy-mac-homebrew.sh`, `.github/workflows/rpcs3.yml`, `rpcs3/rpcs3.plist.in`, `3rdparty/CMakeLists.txt` (`06` §2–§7)
- RPCS3 in-repo (capture / replay) — `rpcs3/Emu/RSX/Capture/rsx_replay.h`, `rsx_replay.cpp`; `rpcs3/Emu/RSX/RSXThread.cpp`; `RSXFIFO.cpp` (`11` §3)
- Mesa KosmicKrisp — `src/kosmickrisp/kk_cmd_draw.c` (multi-draw-with-indirect precedent; `09` §3)
- Comparable ports & scope discussions (`08` §9, `10` §10.1) — Dolphin native-Metal progress reports and Godot `drivers/metal` (both listed above); **PPSSPP**, **DuckStation**, **Ryujinx** (chose *patched MoltenVK* over a native backend); RPCS3 issue #11922
- `actions/runner-images` (macOS runner GPU availability) — <https://github.com/actions/runner-images> (`07` §9.4)

**RPCS3 in-repo (read-only)** — see each detail doc's §8 for the full `VK/` file list; the load-bearing
subsystem inventory is in [`../03-rpcs3-backend-architecture.md`](../03-rpcs3-backend-architecture.md)
and the northstar/effort framing in [`../05-metal-backend-northstar.md`](../05-metal-backend-northstar.md).

---

*End of master synthesis. Per-scope detail, full inventories, code sketches and per-scope
references remain in the eleven `NN-*.md` docs. Unverified items are marked `[U]`; the highest-risk
ones are 03 §7 Q1 (push-constant `setBytes` — made *non-blocking* by the RSX-first constant buffers,
RN3), 03 §7 Q2 (MSL version), 04 §7 Q6 (`setSamplerState` vs the 1024-sampler cap), 08 §10.1 /
10 §10.7 (whether `MTLTraits` binds the generic caches under ~2k LOC — decides RN1's difficulty),
09 §4-D (how ICB binds resolve under Metal 4), and 11 Q3 (whether RRC records enough for a
pixel-complete replay — H5/H6 depend on it).*
