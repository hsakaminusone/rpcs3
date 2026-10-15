# HLD-01 — RSX-native Metal 4 backend architecture

Owner/scope: `design/` cluster `HLD-01` (top-level architecture & module decomposition) | Status: draft | Date: 2026-10-16

> **Read-only research.** No file under `rpcs3/**` was modified to produce this document. `file:line`
> citations were read on branch `metal4`; the tree is a moving branch, so **symbol names are the
> stable reference and line numbers are advisory**. The RSX register inventory in
> `inventory/out/` is the ground truth for which registers exist; this document introduces no new
> register.
>
> **Legend.** `[V]` verified against a primary source (this tree, the reference checkouts under
> `/tmp/metal4-refs/`, or a cited Apple/vendor doc); `[I]` inference/engineering judgement; `[U]`
> could not verify. Difficulty scale (identical to the rest of the set): **1** trivial rename ·
> **2** mechanical · **3** real work, well-trodden · **4** hard/needs design · **5** unknown.
> Metal-4 API names below are read from `metal-cpp` under `/tmp/metal4-refs/metal-cpp/Metal/`
> (headers cited inline), not paraphrased from Apple prose.

---

## 1. Purpose & non-goals

### 1.1 Purpose

Specify the **top-level architecture and module decomposition** of a native Apple Metal 4 backend
for RSX, designed from the RSX guest contract outward (`10` §1.1). This document:

1. fixes the component set and each component's contract (§3.2 table);
2. states the **RSX semantics → Metal 4 semantics** shape change, with the load-bearing boundary
   decision (the **hazard-derived pass scheduler**) made explicit (§2);
3. decides **reuse vs replace** against the two shipping backends (`VK/`, `GL/`) and justifies it
   (§2.3–§2.4), naming the abstractions that survive and those that are deleted;
4. fixes the interfaces, lifetimes and ownership that the per-cluster LLDs (`design/LLD-*.md`) must
   respect (§3).

It is deliberately **not** a per-register mapping: that is the inventory disposition layer and the
LLDs. The 156-row `README.md` §3 construct table is used here only as a completeness checklist
(`10` §1.2, §8).

### 1.2 Non-goals

- No source, CMake, test or `.ci` change; no build; no measurement (measurement is `07`/`09`).
- No re-derivation of `Vk* → MTL4*` per-construct mappings (owned by `01`–`05`), and no per-cluster
  register disposition (owned by `design/LLD-<cluster>.md`).
- No Metal 3 fallback. A native backend targets **Metal 4 only**: `MTL4*` objects exist on M1+ with
  macOS 26+ and never on Intel (`README.md` §2 verdict 5; `06` §3.1). Stronger: `metal-cpp` ships
  no `API_AVAILABLE` annotations, so an unguarded `MTL4*` symbol compiles silently and fails at
  runtime on macOS 15–25 (`README.md` §2 risk 8; `06` §3.4) — gating is a runtime discipline, not a
  compile-time one (§4.7).
- No decision on the strategic question *whether* to build a native backend at all; that is an open
  question (§6 Q1) with a documented honest default (`10` §10.1, `08` §9.1).

---

## 2. Model

### 2.1 The design target is the RSX contract, not the Vulkan API

The artefact a backend satisfies is `rsx::thread`
(`class thread : public cpu_thread, public GCM_context, public GRAPH_backend`,
`rpcs3/Emu/RSX/RSXThread.h:107`; `GRAPH_backend` at `NV47/FW/GRAPH_backend.h:10`). Its required
surface is small and fixed:

| Contract element | Declaration | Conf. |
|---|---|---|
| 4 pure virtuals: `get_cycles`, `get_display_refresh_rate`, `on_init_thread`, `flip` | `RSXThread.h:337,155,370,372` | `[V]` |
| ~25 no-op-default hooks: `on_exit`, `do_local_task`, `emit_geometry`, `clear_surface`, `begin`/`end`, `on_frame_end`, `on_access_violation`, `on_invalidate_memory_range`, `notify_tile_unbound`, `renderctl`, `begin`/`end_conditional_rendering`, `write_barrier`, `sync_hint`, `release_GCM_label`, `on_semaphore_acquire_wait`, `scaled_image_from_memory`, `get_programs`, `is_current_program_interpreted` | `RSXThread.h:348-458`; `VKGSRender.h:261-312` | `[V]` |
| 16-bool `backend_configuration` capability struct | `RSXThread.h:80-98` (member `:123`; accessor `:230`) | `[V]` |
| 5 ZCULL virtuals + dtor | `RSXZCULL.h:157,212-216` | `[V]` |
| 3 generic-cache trait specialisation points | `VKRenderTargets.h:747`, `VKTextureCache.h:412`, `VKProgramBuffer.h:9,72` | `[V]` |

Everything else in the 34,213-LOC `VK/` tree (`08` §6.1) is *one way* to satisfy that surface. The
architecture is therefore organised around **(a)** the shared lifecycle/dispatch/caches that already
exist, **(b)** a new encoder spine, and **(c)** the two Metal-4 mechanisms the contract has no
vocabulary for (argument tables, residency). `[I]`

### 2.2 The shape change

RSX is a **fixed VP/FP, software-vertex-pulling, surface-and-draw machine** (`10` §3.1). It has no
render passes, no subpasses, no descriptor sets, no pipeline layouts, no queue families, no image
layouts and no push constants. Metal 4 is likewise an object-light API: the render-pass and
framebuffer *objects* are gone, descriptor sets collapse to one mutable argument table, the blit
encoder is gone, and hazard tracking is the application's job. The mapping is therefore dominated
by **"object disappears"** events plus one genuinely new component (the scheduler).

| RSX semantic | Existing (VK-shaped) synthesis | Native Metal 4 shape | Shape change | Conf. |
|---|---|---|---|---|
| Surfaces + draws; RPCS3 decides load/clear/resolve/reuse | `VkRenderPass`+`VkFramebuffer`+packed `u64` key | per-encoder `MTL4::RenderPassDescriptor`, boundary derived from surface hazard events; suspend/resume to merge | **the encoder *is* the pass**; boundary source moves from attachment identity to `surface_store` hazard | `[V]`/`[I]` |
| Fixed shader-source set (vertex/constants/textures/predicate/env/instancing) | descriptor sets + layouts + pools + binding table | one `MTL4::ArgumentTable`, slots from the RSX shader ABI | descriptor subsystem deleted; table keyed by RSX slot enum | `[V]`/`[I]` |
| Constant-buffer region; VS 4 B / FS 28 B blocks | `vkCmdPushConstants` | entries in the constant buffer (RSX has no push constants) | push-constant machinery deleted; `setBytes` gap moot | `[V]`/`[I]` |
| Order RTT-write before texture-read; resolves; programmable-blend feedback | `vkCmdPipelineBarrier` on every barrier end-of-renderpass | explicit stage-pair barriers, access-mask half dropped | hazard *knowledge* already exists in `surface_store`; only the emit shape changes | `[V]` |
| Draw clauses over a FIFO; flattener + `draw_clause` classify batches | `vkCmdDraw*` + `_vkCmdDrawMulti*EXT` | `drawPrimitives`/`drawIndexedPrimitives` + host-built indirect args | batching intelligence is upstream and transfers unchanged | `[V]` |
| ucode → GLSL | GLSL → SPIR-V (glslang) | GLSL → SPIR-V → MSL (SPIRV-Cross) → `MTL4::Compiler` | add one cross-compile stage; front-end untouched | `[V]` |
| `flip(display_flip_info_t)` at frame end | `VkSwapchainKHR` acquire/present/fence/semaphore | `CA::MetalLayer` drawable ring + `MTL4::CommandQueue` + `MTLSharedEvent` | swapchain object dissolves; frame ring = allocators + event value | `[V]` |

Two facts drive the whole design and are stated once here:

- **Metal 4 has no driver hazard tracking.** `MTLHazardTrackingMode` has no effect on work submitted
  to an `MTL4CommandQueue` (`README.md` §4.1; `01` §1.3). Every dependency must be an explicit
  `barrierAfterStages:beforeQueueStages:`, `barrierAfterQueueStages:beforeStages:` or
  `barrierAfterEncoderStages:beforeEncoderStages:` call, each taking an
  `MTL4::VisibilityOptions { None, Device, ResourceAlias }` — verified in
  `MTL4CommandEncoder.hpp:50-54,55-63`. **The access-mask half of every RPCS3 Vulkan barrier is
  dropped; only the stage half survives.** `[V]`
- **Residency ≠ hazard tracking.** `MTLResidencySet` (`MTLResidencySet.hpp:52,61,76`) makes memory
  *reachable*, never *ordered*; it is a separate mandatory subsystem attached via
  `MTL4::CommandQueue::addResidencySet` (`MTL4CommandQueue.hpp:123`). `[V]`

### 2.3 The load-bearing decision: a hazard-derived pass scheduler (RN1)

The alternative to a scheduler is the VK-shaped plan: rebuild `VkRenderPass`+`VkFramebuffer` and
cache them by a packed `u64` key. That plan is 710 LOC describing a *Vulkan* object RSX never had
(`10` §4.3, §5). The RSX-native plan replaces it with a scheduler that derives encoder boundaries
and barriers from **hazard events RPCS3 already computes**:

- `rsx::surface_store<Traits>` already computes R/W hazards and calls `read_barrier` /
  `memory_barrier(cmd, access)` / `clear_rw_barrier` (`Common/surface_store.h:229,1114,1388,1566`;
  `10` §4.4 `[V]`), and `rsx::thread::write_barrier(u32,u32)` is already a virtual hook
  (`RSXThread.h:396`; VK impl `VKGSRender.cpp:1778`).
- The scheduler opens a new `MTL4::RenderCommandEncoder` when a draw targets a different surface
  set **or** when a hazard is illegal intra-pass, and links consecutive encoders that share
  attachments with `MTL4::RenderEncoderOptionSuspending` / `RenderEncoderOptionResuming`
  (`MTL4RenderCommandEncoder.hpp:55-58`) so the GPU merges them into one TBDR pass.

**Why this is make-or-break and not a micro-optimisation.** RSX rebinds surfaces rapidly; the VK
render-pass layer coalesces that spam, which is exactly what keeps tile memory hot on TBDR (`10`
§7.2). A *naive* scheduler that opens an encoder at every surface change is **worse** than the VK
plan: each boundary is a full attachment store+load. Suspend/resume merging is therefore mandatory
from the first scheduler revision, not a later optimisation. `[I]`

**The TBDR constraint becomes a scheduler rule, not a mapping failure.** Apple's TBDR notes forbid an
`afterEncoderStages` mask containing `.fragment`/`.tile` on a render encoder (`README.md` §4.1;
`02` §5 rank 1); the scheduler must therefore split a pass whenever a feedback dependency touches
fragment/tile stages. This is the same condition that makes RSX programmable-blending read-back
(ROP reads current RT) a **framebuffer fetch** (`[[color(N)]]`) rather than a pass split
(`10` §4.3; `09` §3 #13). `[V]`/`[I]`

Confidence: the *shape* is `[I]` (engineering judgement on `[V]` facts). Whether it is `diff 4` or
`diff 5` is gated on the trait-compile spike in §6 Q3.

### 2.4 Reuse vs replace — decision against `VK/` and `GL/`

Both shipping backends are already thin skins over one shared, backend-agnostic emulation core. That
core does not change; only its trait parameterisations and the ~50-signature contract do.

**Measured LOC (this tree, `wc -l` on `*.cpp/*.h/*.hpp/*.mm`):**

| Subtree | Files | LOC | Role | Disposition for `MTL/` |
|---|---|---|---|---|
| `Common/` | 34 | 17,164 | generic `surface_store`, `texture_cache`, blit/search logic | **reuse verbatim** |
| `Core/` | 12 | 1,874 | `RSXContext`, `RSXDrawCommands`, `RSXDisplay`, `RSXFrameBuffer` | **reuse verbatim** |
| `Program/` | 72 (36 `.glsl`) | 17,879 | ucode → GLSL front-end + `ProgramStateCache` | **reuse verbatim** |
| `NV47/` | 23 | 3,246 | HW handlers, `draw_clause`, `GRAPH_backend` | **reuse verbatim** |
| root RSX sources | 28 | 20,822 | `RSXThread.*`, `rsx_methods.*`, `gcm_enums.h`, `rsx_decode.h` | **reuse verbatim** |
| `Overlays/` | 98 | 20,151 | backend-agnostic overlay UI | **reuse geometry**; new MTL pipelines |
| `VK/` | 112 | 34,213 | the Vulkan backend (port surface, `08` §6.1) | **the map; mostly replaced** |
| `GL/` | 66 | 18,193 | second backend; structural template (`10` §7.4) | **template, not code** |

The brief's "~51.5k LOC shared `Common/`+`Core/`" does not reconcile to a single measured subtree
(`Common/`+`Core/` = **19,038**; `Common/`+`Core/`+`Program/`+`NV47/`+`Utils/`+`Host/`+`Capture/` =
**42,967**). The decision does not depend on the exact figure: **the whole backend-agnostic core
survives, because it is already parameterised on traits, not on `Vk*`.** `[V]`

**Decision: reuse the shared core verbatim; replace only the three trait specialisations, the
~50-signature contract implementation, and the Vulkan-only middle scaffolding.** Justification:

1. The trait split already exists and is proven by a second backend: `GL/` is 66 files / 18,193 LOC
   over the same `rsx::surface_store`/`rsx::texture_cache` generics (`08` §2.4, §3.1). `[V]`
2. The shared core is where correctness lives (per-title quirks, format/deswizzle, MSAA resolve); it
   is `~43k` LOC and touching it multiplies risk for zero benefit (`10` §7.1). `[I]`
3. The `VK/` tree is dominated by Vulkan-invented ceremony RSX does not need (instance/loader,
   VMA + memory types, descriptor sets/layouts/pools, pipeline layout, push-constant workaround,
   render-pass/framebuffer objects, command pools + deep-copy submit, the `VkSemaphore`/`VkFence`/
   `VkEvent` trio). Reusing it would import the 156-row mapping problem the reorientation exists to
   delete (`10` §5, §1.1). `[I]`

**Abstractions that survive (Tier 1 — reused unchanged):** the `rsx::thread` base and contract; the
method dispatch tables `methods[]` / `state_signals[]` (`rsx_methods.cpp:17-18`); `rsx_state` decode
(`rsx_methods.h:78`); `FIFO::FIFO_control` and `FIFO::flattening_helper` (`RSXFIFO.h:63,129`); the
NV47 handlers; `rsx::context`/`GCM_context` (`Core/RSXContext.h:32`); `draw_command_processor` and
`draw_clause` (`Core/RSXDrawCommands.h:20`; `NV47/FW/draw_call.hpp:19`); `rsx::surface_store`/
`rsx::texture_cache`/`program_state_cache`; `Program/`; `Capture/`; `Host/MM`; `Utils/`; `GSRender`.

**Abstractions that get a native re-specialisation (Tier 2 — replace the traits, not the cache):**
`vk::surface_cache_traits` (`VKRenderTargets.h:159,747`) → `mtl::surface_cache_traits`;
`vk::texture_cache_traits` (`VKTextureCache.h:412`) → `mtl::texture_cache_traits`; `VKTraits`
over `program_state_cache` (`VKProgramBuffer.h:9,72`) → `MTLTraits`; the backend spine
`VKGSRender` (`VKGSRender.h:38`) → `MTLGSRender`; the capability fill `VKGSRender.cpp:746-909` →
a small `MetalFeatures` probe.

**Abstractions deleted (Tier 3 — never written for `MTL/`):** the Vulkan loader/instance/extension
layer (`VulkanAPI.*`, `VKProcTable.h`, `vkutils/instance.*`); `vk_driver_selection.h`; VMA and the
two allocators and the memory-type index search; descriptor sets/layouts/pools
(`vkutils/descriptors.*`); `VKCommonPipelineLayout`; the push-constant workaround; `VkRenderPass` +
`VkFramebuffer` + packed-`u64` key (`VKRenderPass.*`, `VKFramebuffer.*`,
`vkutils/framebuffer_object.hpp`); the command-pool/deep-copy submit packet
(`VKCommandStream.*`); the `VkFence`/`VkSemaphore`/`VkEvent` triad (`vkutils/sync.*`); the
blit-engine Vulkan plumbing. Net ≈ **−5,000 to −7,000 LOC never written**, offset by ≈ **+700 LOC**
of RSX-native additions (the scheduler RN1 and the indirect batcher RN2) (`10` §5, §6.1). `[I]`

**Verdict on the reorientation:** it *reshapes* the estimate by ~15–20% (≈20–30k → ≈18–25k new
`MTL/` LOC, `10` §6.1), it does **not** halve it. The expensive parts — texture-cache traits,
`MTLTexture` upload/deswizzle/convert, MSL correctness, the per-title quirk surface — are identical
under both frames. What the reorientation removes is **middle scaffolding**. `[I]`

---

## 3. Interfaces / data structures

### 3.1 Namespaces and the device seam

All new code lives in `rpcs3/Emu/RSX/MTL/**` under namespace `mtl` (mirroring `vk::`), behind
`#if defined(HAVE_METAL)`, arch-excluded on Intel (`06` §3.3; `08` §7.4). A thin `mtl::device`
wrapper owns the single `MTL::Device` and the single `MTL4::CommandQueue`, and is the **only** place
device symbols are touched — the precondition for the null/recording device that makes the
structural verification loop hosted-CI-capable (`11` §5.2, §9.4). `[I]`

A `video_renderer::metal` enumerator (`system_config_types.h:44-49` currently has only `null`,
`opengl`, `vulkan`) selects `MTLGSRender` at construction (`gui_application.cpp:787-808`, the same
seam that already selects `VKGSRender`). This is the entire runtime-selection surface (`08` §7.2).
`[V]`/`[I]`

### 3.2 Component table

| # | Component (type) | Responsibility | Inputs | Outputs | Depends-on |
|---|---|---|---|---|---|
| C1 | **RSX thread / FIFO consumer** — `MTLGSRender : GSRender, reports::ZCULL_control` | Implement the `rsx::thread` contract; own the per-frame lifecycle (init → frame → flip → exit); fill `backend_configuration`; consume FIFO commands | guest FIFO (`FIFO_control::read`), `display_flip_info_t`, ZCULL hooks | `backend_configuration`; recorded encoder stream; flipped frame; screenshots | `gs_frame`/`GSFrameBase`, C2, C3, C4, C5, C7 |
| C2 | **Method dispatcher** — existing `methods[]` / `state_signals[]` + `rsx_state` | Decode each `(reg,value)`; invoke the hardware handler; set dirty signals; feed the flattener; build the `draw_clause` | FIFO register pairs (`RSXFIFO.cpp:910-931`) | mutated `rsx_state`, `m_graphics_state`, `draw_clause`, draw/dirty events to C1 | reused verbatim; no backend coupling |
| C3 | **Hazard-derived pass scheduler (RN1)** — `mtl::pass_scheduler` | Derive encoder boundaries + barrier placement from surface hazard events; enforce the TBDR rule (feedback ⇒ split pass); drive suspend/resume pass merging | surface R/W/cyclic events from C5's `surface_store`; the current `draw_clause`; `framebuffer_layout` | `render_pass_plan { attachments, load/store per attachment, sample count, visibility buffer }`; ordered `barrier_op` list; pass-split/suspend decisions | `rsx::surface_store`, C4, C5 |
| C4 | **Command encoder / recorder** — `mtl::command_recorder` | Own one `MTL4::CommandAllocator` per frame-in-flight; begin/commit `MTL4::CommandBuffer`s; open `MTL4::RenderCommandEncoder`/`ComputeCommandEncoder`; emit the whole frame's decisions through one chokepoint and record them | C3 plans + C6 draws/binds + C5 resources | `MTL4::CommandBuffer`s committed via `CommandQueue::commit`; `decision_trace` (H2 seam) | C1, C5, C6, `mtl::device` |
| C5 | **Resource manager** — `mtl::resource_manager` | One placement `MTLHeap` per storage class (Shared/Private); `newBuffer`/`newTexture` sub-allocation; host-pointer buffer import; `MTLResidencySet` lifecycle; scratch/null resources; deferred release (garbage collector) | guest address ranges, `framebuffer_layout`, upload/deswizzle requests, memory-pressure signals | `mtl::buffer`/`mtl::texture` handles, residency set updates, `gpuResourceID`s | `mtl::device`, C3 (hazard input), C4 (lifetime) |
| C6 | **Shader translation + binding layer** — `mtl::shader_cache`, `mtl::pipeline_cache`, `mtl::argument_table` | GLSL → SPIR-V (glslang, unchanged) → MSL (SPIRV-Cross `CompilerMSL`) → `MTL4::Compiler`; build `MTL4::RenderPipelineDescriptor`/compute pipelines; own the one `MTL4::ArgumentTable` keyed by the RSX slot enum and its per-draw snapshots | `draw_clause`, shader ucode (reused front-end), RSX slot bindings, `MetalFeatures` | `MTL::Library`, `MTL::RenderPipelineState`, argument-table snapshots, `MTL4::PipelineDataSetSerializer` archive | C5, `Program/`, `mtl::device` |
| C7 | **Presenter / flip path** — `mtl::presenter` | `CA::MetalLayer` on the Qt `QWindow`'s `NSView`; `waitForDrawable` → commit → `signalDrawable` → `drawable.present`; overlay composite; screenshot readback; refresh/pacing | `display_flip_info_t`, `client_width/height`, overlay passes, screenshot requests | presented frame, `get_display_refresh_rate()`, `take_screenshot(...)` | C1, C4, C6 |

### 3.3 Key types, lifetimes, ownership

**Trait types (Tier 2).** `mtl::surface_cache_traits`, `mtl::texture_cache_traits` and `MTLTraits`
must satisfy the generic-cache contracts by substituting the resource/handle types the VK traits
declare (`VKRenderTargets.h:159-167`; `VKTextureCache.h:21-36`; `texture_cache.h:26-32`). Concretely:
`command_list_type → mtl::command_recorder&`, `buffer_object_type → mtl::buffer*`,
`image_resource_type → mtl::texture*`. **This is the single largest unknown in the design** (§6 Q3):
the generic caches were never compiled against a stub `MTLTraits` (`08` §10 item 1). `[V]`/`[U]`

**`MTLTraits` for `program_state_cache`** owns the program hash lookup; only the pipeline object
differs (mirrors `VKProgramBuffer.h:9,72`). `[V]`

**Capability plan.** `backend_configuration` (`RSXThread.h:80-98`) is filled from a `MetalFeatures`
probe. Net: ~7 fields free, ~4 work, 4 forced `false` (`supports_hw_renormalization`,
`supports_passthrough_dma`, `supports_last_provoking_vertex`, `supports_hw_conditional_render` on
bring-up), 1 reshaped (`supports_host_gpu_labels`) — derived in `10` §2.2, re-verified against the
VK fill (`VKGSRender.cpp:746-909`). `[V]`/`[I]`

**Argument table.** One long-lived `MTL4::ArgumentTable` created via `Device::newArgumentTable`
(`MTLDevice.hpp:407`), sized by `MTL4::ArgumentTableDescriptor` (`MTL4ArgumentTable.hpp:57-61`:
`setMaxBufferBindCount`, `setMaxTextureBindCount`, `setMaxSamplerStateBindCount`). Slot enum is the
existing RSX ABI: `vk::pipeline_binding_table` reserves 12 fixed slots (`pipeline_binding_table.h:9-19`;
inventory `S_*`, `rsx_binding_slots.csv`). The encoder snapshots the table at draw time, so it is
safe to mutate between draws (`09` §4; `10` §4.2). **metal-cpp `MTL4::ArgumentTable` exposes only
`setAddress`/`setResource`/`setTexture`/`setSamplerState` — there is no `setBytes`**
(`MTL4ArgumentTable.hpp:73-80`, confirmed: no `setBytes` symbol). `[V]`

**Command buffers and allocators.** `Device::newCommandAllocator()` (`MTLDevice.hpp:416`) and
`Device::newCommandBuffer()` (`MTLDevice.hpp:419`). One allocator per frame-in-flight, reset after
completion (`MTL4CommandAllocator` `reset()`/`allocatedSize()`). `MTL4::CommandBuffer` holds **no
strong references** and is reusable (`README.md` §4; `02`); resources referenced must outlive the
commit. `[V]`

**Residency.** `Device::newResidencySet` (`MTLDevice.hpp:496`); `addAllocation`/`commit`/
`requestResidency` (`MTLResidencySet.hpp:52,61,76`); attached with
`CommandQueue::addResidencySet` (`MTL4CommandQueue.hpp:123`). The garbage collector must not release
a resource before the matching residency removal (`README.md` §4; `01` §1.5 item 4). `[V]`

**Recording seam (verifiability requirement).** Every hazard decision, pass boundary and binding goes
through a single chokepoint that emits a structured `decision_record` (`11` §5.1, §9). This is a
design requirement, not a test afterthought: a barrier or boundary computed inline and handed
straight to an encoder is invisible to the H2/H3 loop (`11` §9 items 1–4). `[I]`

**`draw_range_t`/`barrier_t`/`draw_clause`** are reused unchanged (`NV47/FW/draw_call.inc.h:39,64`;
`draw_call.hpp:19`); the flattener collapses the FIFO before the backend sees draw clauses
(`RSXFIFO.cpp:522-579`, thresholds `>2000`/`≥500`). The batcher (RN2) consumes
`draw_clause::get_subranges()`/`pass_count()` and emits `MTLDraw*IndirectArguments` into a host
buffer, then `draw*(indirectBuffer:)` (`09` §3 #5–#6; `draw_call.hpp:206,311`). `[V]`

### 3.4 Metal 4 object inventory (concrete)

| Purpose | Metal 4 symbol (verified in metal-cpp) |
|---|---|
| device / queue | `MTL::CreateSystemDefaultDevice()`; `Device::newMTL4CommandQueue()` (`MTLDevice.hpp:477`) |
| commit + feedback | `MTL4::CommandQueue::commit(cbs,count,options)`; `MTL4::CommitOptions::addFeedbackHandler` (`MTL4CommandQueue.hpp:126,98`) |
| allocator / buffer | `Device::newCommandAllocator()`; `CommandBuffer::beginCommandBuffer(allocator:)`, `endCommandBuffer()` (`MTL4CommandBuffer.hpp:64,71`) |
| render pass | `MTL4::RenderPassDescriptor` (`colorAttachments`, `depthAttachment`, `stencilAttachment`, `setVisibilityResultBuffer`, `setRenderTargetWidth/Height`, `setDefaultRasterSampleCount`) (`MTL4RenderPass.hpp:51,55,87,97,81,83,71`) |
| encoders | `CommandBuffer::renderCommandEncoder(descriptor,options)`, `computeCommandEncoder()` (`MTL4CommandBuffer.hpp:81,67`) |
| pass merge | `MTL4::RenderEncoderOptionSuspending` / `RenderEncoderOptionResuming` (`MTL4RenderCommandEncoder.hpp:57-58`) |
| barriers | `barrierAfterStages:beforeQueueStages:`, `barrierAfterQueueStages:beforeStages:`, `barrierAfterEncoderStages:beforeEncoderStages:` + `MTL4::VisibilityOptions {None,Device,ResourceAlias}` (`MTL4CommandEncoder.hpp:50-54,55-63`) |
| binding | `ArgumentTable::setAddress/setTexture/setSamplerState/setResource`; encoder `setArgumentTable(table,stages)` (`MTL4ArgumentTable.hpp:73-80`; `MTL4RenderCommandEncoder.hpp:84`) |
| draw | `drawPrimitives`, `drawIndexedPrimitives`, `drawPrimitives(indirectBuffer:)`, `drawIndexedPrimitives(…indirectBuffer:)`, `executeCommandsInBuffer` (`MTL4RenderCommandEncoder.hpp:66-82`) |
| occlusion | `setVisibilityResultMode(mode,offset)` (`MTL4RenderCommandEncoder.hpp:127`) |
| copy/fill/mips | compute encoder `copyFromBuffer`, `copyFromTexture`, `fillBuffer`, `generateMipmaps` (`MTL4ComputeCommandEncoder.hpp:67-93`) |
| shaders / pipelines | SPIRV-Cross `CompilerMSL` → `MTL4::Compiler::newLibrary`, `newRenderPipelineState`, `newComputePipelineState` (`MTL4Compiler.hpp:119,127,106`); `MTL4::LibraryFunctionDescriptor{setLibrary,setName}` (`MTL4LibraryFunctionDescriptor.hpp:52,54`); `MTL4::RenderPipelineDescriptor` (`MTL4RenderPipeline.hpp:143`) |
| pipeline cache | `MTL4::PipelineDataSetSerializer` → `MTL::Archive` |
| residency | `MTL::ResidencySet` + `CommandQueue::addResidencySet` |
| present | `CA::MetalLayer`, `waitForDrawable`, `signalDrawable(drawable)`, `drawable.present()`, `CAMetalDisplayLink` |
| timestamps | `MTL4::CounterHeap` + `CommandBuffer::writeTimestampIntoHeap` (`MTL4CommandBuffer.hpp:91`) |

### 3.5 Where Metal 4 lacks a facility — with the fallback actually chosen

| RSX need | Metal 4 gap | Fallback chosen | Conf. |
|---|---|---|---|
| push constants (VS 4 B / FS 28 B) | `MTL4::ArgumentTable` has no `setBytes` | **constant-buffer entries** (RSX has no push constants) — removes the gap entirely; alternatively transient buffer + `setAddress` | `[V]`/`[I]` |
| HW multi-draw | no native multi-draw | host-built `MTLDraw*IndirectArguments` + `draw*(indirectBuffer:)`; else per-subrange loop (KK precedent: `kk_convert_to_indirect_draw`, `kk_cmd_draw.c:1464`) | `[V]` |
| conditional render | no conditional-render command | **bring-up: CPU skip**; **parity: compute predicate-patch** zeroing the indirect args (KK `kk_predicate_draws`, `kk_cmd_draw.c:1487`) | `[V]`/`[I]` |
| last-vertex provoking (flat shading) | no public provoking-vertex mode | force `supports_last_provoking_vertex=false`; smooth-interpolation fallback + warning (mirrors `VKGSRender.cpp:752-754`) | `[V]` |
| custom border color | only TransparentBlack/OpaqueBlack/OpaqueWhite | nearest-enum fallback | `[V]` |
| `mipLodBias` | `lodBias` is S4.6 fixed-point | S4.6 or shader-side uniform (KK passes it shader-side) | `[V]` |
| `usamplerBuffer` (software vertex pull) | no texel-buffer sampler in the Vulkan sense | RSX already software-pulls; bind via `MTLTextureTypeTextureBuffer` / `buffer.makeTexture(...)` | `[V]` |
| `D24_UNORM_S8` surface | unsupported on Apple silicon | `D32Float_Stencil8`; `gpu_formats_support.d24_unorm_s8=false` | `[V]`/`[I]` |
| depth/stencil MSAA resolve | store-action resolve covers color; D/S forces meta passes on Metal sync bug | store-action resolve for color; compute/graphics meta pass for D/S | `[V]` |
| blit (`vkCmdBlitImage`) | no blit encoder; no scaled blit | compute meta pass driven by NV3089/NV0039 semantics (`09` §3; `nv3089`/`nv0039` handlers) | `[V]` |
| ICB rapid draw (strategy D) | `MTLIndirectRenderCommand` carries no argument-table method | **out of scope for draws**; indirect draws only (`09` §4-D) | `[V]`/`[U]` |

---

## 4. Invariants

1. **No implicit hazards.** No correctness argument may rely on driver hazard tracking; every
   dependency is an explicit barrier or pass boundary (`README.md` §4.1; `MTL4CommandEncoder.hpp:50-54`).
2. **Barriers and pass boundaries are derived, not emergent.** Both are computed by C3 from C5's
   `surface_store` events and emitted through C4's single chokepoint, so H2/H3 can diff them
   (`11` §9 items 1–2).
3. **TBDR fragment constraint.** No render-encoder `barrierAfterEncoderStages` may include
   `.fragment`/`.tile`; the scheduler splits the pass instead (`02` §5 rank 1; `09` §3 #13).
4. **Pass merging is the default.** Consecutive encoders that share attachments are linked with
   `RenderEncoderOptionSuspending`/`Resuming`; a boundary without merging is a pessimisation and
   requires justification (`10` §7.2).
5. **Everything bound is resident.** Every resource referenced from an argument table or attachment
   is in a committed `MTLResidencySet` before the commit that uses it (`README.md` §4.2). Residency is
   orthogonal to ordering and does not substitute for a barrier.
6. **Resource lifetime ≥ commit completion.** `MTL4::CommandBuffer` holds no strong references
   (`README.md` §4); no resource, argument table, or transient buffer may be released or overwritten
   before the matching `MTL4CommitFeedback`.
7. **Allocator discipline.** Exactly one `MTL4::CommandAllocator` per encoding thread/frame-in-flight
   (`MTL4CommandAllocator` is not thread-safe; `09` §5.1); no reset while its GPU work is in flight.
8. **Argument-table snapshot semantics.** A table mutation is only visible to draws encoded *after*
   it; state-keyed batching (strategy F) and indirect batches (strategy C) depend on this (`09` §4).
9. **The capability struct is the only place emulation behaviour branches on the backend** — never a
   driver probe from shared code (`RSXThread.h:80-98`; `08` §2.2).
10. **Metal 4 is gated at runtime, not compile time.** Availability checks wrap every `MTL4*` entry
    point (metal-cpp ships no `API_AVAILABLE`) (`06` §3.4; `README.md` §2 risk 8).
11. **Determinism in replay mode.** Thread scheduling, clock/vsync and allocator behaviour are pinned
    so H2/H3 are run-to-run stable (`11` §9 item 5).

---

## 5. Failure modes & limits

- **Scheduler over-splitting (highest-severity perf failure).** A boundary per surface change stores
  and reloads every attachment; this *regresses* the VK backend it replaces (`10` §7.2). Detected by
  `m_frame_stats`/Instruments pass counts; mitigated by mandatory suspend/resume merging (§2.3).
- **Trait leakage (highest-severity correctness failure).** Per-title RSX quirks leaking into
  `MTLTraits` grows cost and bug surface; the phase-1 mitigation is to port traits file-by-file with
  the VK comments as a checklist and keep the generic caches and `Program/` untouched (`10` §7.1;
  `08` §10 item 1).
- **MSL correctness tail.** Dolphin/Godot precedent predicts a steady bug tail (dynamic uniform
  offsets, argument buffers, shader-compile stalls) even with a working pipeline (`08` §9, §10 item 2;
  `09` §7.2). This is the schedule risk after the traits.
- **Metal-compiler / driver bug surface.** The KosmicKrisp workaround register (one `MTL4Compiler`
  per device, W1/W2/W3/W4/W5) is inherited (`README.md` §6 R10; `10` §11). Not a correctness failure
  the loop can see — it surfaces as wrong pixels or crashes.
- **No local pixel oracle / CI blind spot.** RPCS3 has no golden-image GPU suite, hosted macOS
  runners expose no `MTLDevice`, and the only RSX-objective oracle is guest-observable readback
  (`README.md` §2 risks 6–7; `11` §8). Without a self-hosted Apple-silicon runner and the H2/H3 seam,
  GPU verification is manual.
- **Strategic downside risk.** The upside over KosmicKrisp — which already runs directly on Metal 4 —
  is unmeasured; Dolphin's native-Metal win over a *healthy* translator was only +23%/+30%
  (`README.md` §2 risk 9; `08` §9.1). The honest posture is research northstar, not next ship.
- **`nextDrawable` blocking / `maximumDrawableCount ≤ 3`.** A genuine constraint the reorientation
  does not remove; forces a frame-ring redesign (`10` §4.7; `05` R21). Unresolved (§6 Q7).
- **Per-title regressions owned, not hidden.** Forcing `supports_last_provoking_vertex=false` and the
  bring-up CPU conditional-render path are visible-quality changes; each must be documented as a
  caveat mirroring `VKGSRender.cpp:754` (`10` §7.6).
- **Under-scoping.** A *derived* design can silently miss a capability the inventory caught; the
  mitigation is the §2.2 checklist and the §6 question list, not confidence (`10` §7.3).

---

## 6. Open questions

Each is a decision for a human, tagged with confidence.

1. **[V]/[I] Strategic gate — is a native backend worth building at all?** KosmicKrisp already runs
   directly on Metal 4; the native upside is unmeasured and RPCS3 is often CPU-limited
   (`README.md` §2 risk 9; `10` §10.1). Resolve **before** any scheduler work.
2. **[I] Commit to a hazard-derived pass scheduler (RN1) rather than a VK-style render-pass cache?**
   Everything in §2.3–§2.4, §5's savings, and TBDR-correct pass merging depend on it (`10` §10.2).
   If declined, this design degenerates to the `08` port.
3. **[U] Can `rsx::surface_store` / `rsx::texture_cache` / `program_state_cache` compile against a
   stub `MTLTraits` in <2k LOC?** Never attempted (`08` §10 item 1; `10` §10.7). This spike decides
   whether RN1 is `diff 4` or `diff 5` and whether the ~43k-LOC reuse claim holds.
4. **[I] Binding-model ceiling — one frame-wide argument table mutated per draw, or per-state tables?**
   Strategy-C indirect batching (RN2) needs a batch to share one table; a per-draw table swap kills it
   (`09` §10 Q1).
5. **[U] Does the target SDK's `MTL4::ArgumentTable` actually lack `setBytes`?** metal-cpp and
   objc2-metal lack it (`MTL4ArgumentTable.hpp:73-80`); Apple's cross-API table lists it. RSX-first
   makes this non-blocking (constant buffers), but it affects the constant path.
6. **[U] One `MTL4Compiler` per device constrains the async pipeline-compile pool.** KK's W11
   workaround implies serialisation; the pool topology is undesigned (`README.md` §4; `10` §4.6).
7. **[U] Does `nextDrawable` blocking / `maximumDrawableCount ≤ 3` force a frame-ring redesign?**
   Unchanged by the reorientation; still needs a spike (`05` R21; `10` §4.7).
8. **[I] Conditional-render first cut: CPU skip or compute predicate-patch from day one?**
   (`09` §10 Q2; §3.5 above). The GPU-path barrier/ordering details need a spike.
9. **[U] Residency-set granularity and ordering vs the garbage collector.** Device-level vs transitional
   per-frame set, and the removal-after-completion ordering, are unspecified here.
10. **[U] Do we keep one commit per frame (matching `close_and_submit_command_buffer`,
    `VKGSRender.cpp:2510`), or move to per-frame allocators + grouped commit of multiple
    command buffers?** This changes both C4 and the draw path (`09` §10 Q4).
11. **[V] Which titles regress under the `supports_last_provoking_vertex=false` smooth fallback?**
    (`10` §10.6). Needs a title survey.
12. **[U] Does `setSamplerState` bypass the 1024 argument-buffer sampler cap?** If yes, drop sampler
    eviction; if no, the cap returns (`README.md` §6 R17; `10` §10.9).
13. **[V] Runtime availability gating cannot be enforced by the compiler under metal-cpp.** A lint or
    the null-device seam must enforce it (`06` §3.4).
14. **[I] Null/recording device (effort ~3) to bring H2/H3 into hosted CI?** Recommended; it is the
    difference between a loop that runs everywhere and one that needs a specific Mac (`11` §10 Q1).

**Highest-value (report-back set):** Q1 (strategic gate), Q2 (commit to the scheduler), Q3 (trait
spike), Q4 (argument-table ceiling), Q8 (conditional-render first cut), Q10 (commit model), Q6/Q9
(compiler + residency lifetimes), Q14 (null device).

---

## 7. Provenance

### 7.1 This tree `[V]` (paths under `rpcs3/Emu/` unless noted)

- `RSX/RSXThread.h` — `thread : cpu_thread, GCM_context, GRAPH_backend` `:107`;
  `backend_configuration` `:80-98`, member `:123`, accessor `:230`; pure virtuals `:155,337,370,372`;
  hooks `:348-458`; `write_barrier` `:396`; `read_barrier` `:395`; `m_frame_stats` `:162`;
  `m_graphics_state` `:254`; `run_FIFO` `:357`; `m_flattener` `:133`.
- `RSX/GSRender.h:20` — `GSRender : rsx::thread` (architecture-neutral scaffolding).
- `RSX/RSXZCULL.h:157,212-216` — `ZCULL_control` 5 backend virtuals.
- `RSX/RSXFIFO.h:63-127,129` — `flattening_helper`, `FIFO_control`; `RSXFIFO.cpp:522-579`
  (`evaluate_performance` thresholds `>2000`/`≥500`), `:668` (`run_FIFO`), `:875-931` (flattener +
  `methods[reg]` dispatch + `state_signals[reg]`).
- `RSX/rsx_methods.h:78` — `rsx_state`; `:1370-1372` — `method_registers`, `methods`, `state_signals`;
  `RSX/rsx_methods.cpp:15-18` — table definitions.
- `RSX/NV47/FW/draw_call.hpp:19,69,75,79,81,177,206,296,311` — `draw_clause` classification;
  `draw_call.inc.h:39,64` — `barrier_t`, `draw_range_t`; `NV47/FW/GRAPH_backend.h:10` — `GRAPH_backend`.
- `RSX/Core/RSXDrawCommands.h:20` — `draw_command_processor`; `Core/RSXContext.h:32` — `GCM_context`.
- `RSX/Common/surface_store.h:210-279` — surface R/W/cyclic/clipped-region handling;
  `Common/texture_cache.h:20-40` — generic `texture_cache<derived,traits>`.
- `RSX/VK/VKGSRender.h:38` — `VKGSRender : GSRender, ZCULL_control`; `:261-312` — overrides;
  `RSX/VK/VKGSRender.cpp:746-909` — capability fill; `:752-754` — provoking-vertex warning;
  `:1778` — `write_barrier`; `:2510` — `close_and_submit_command_buffer`.
- `RSX/VK/VKRenderTargets.h:159-167,747` — `surface_cache_traits`;
  `RSX/VK/VKTextureCache.h:21-36,412`; `RSX/VK/VKProgramBuffer.h:9,72`;
  `RSX/VK/vkutils/pipeline_binding_table.h:9-19` — 12 argument slots.
- `Emu/system_config_types.h:44-49` — `video_renderer { null, opengl, vulkan }`;
  `rpcs3qt/gui_application.cpp:787-808` — construction switch.
- Measured LOC: `Common/` 34/17,164; `Core/` 12/1,874; `Program/` 72/17,879; `NV47/` 23/3,246;
  `Overlays/` 98/20,151; `VK/` 112/34,213; `GL/` 66/18,193 (agrees with `08` §3.1).
- `docs/research/metal4/inventory/out/SUMMARY.md` — 282 registers / 14 clusters;
  `out/rsx_binding_slots.csv`, `out/clusters.json` (ground truth for registers).

### 7.2 Reference checkouts under `/tmp/metal4-refs/` `[V]`

- `metal-cpp/Metal/MTL4ArgumentTable.hpp:57-61,73-80` (no `setBytes`);
  `MTL4CommandQueue.hpp:98,123,126-127`; `MTL4CommandBuffer.hpp:64,67,71,81-82,91`;
  `MTL4CommandEncoder.hpp:50-54,55-63`; `MTL4RenderCommandEncoder.hpp:55-58,66-82,84,108,127`;
  `MTL4RenderPass.hpp:51,55,71,81,83,87,97`; `MTL4ComputeCommandEncoder.hpp:67-93,108`;
  `MTL4CommandAllocator.hpp:51,57`; `MTL4Compiler.hpp:106,119,127`;
  `MTL4LibraryFunctionDescriptor.hpp:52,54`; `MTL4RenderPipeline.hpp:143,179,185,189,201`;
  `MTLDevice.hpp:407,416,419,477,496,541`; `MTLResidencySet.hpp:52,61,76`.
- Mesa KosmicKrisp `src/kosmickrisp/vulkan/kk_cmd_draw.c:1464` (`kk_convert_to_indirect_draw`),
  `:1487` (`kk_predicate_draws`); `docs/drivers/kosmickrisp/workarounds.rst`,
  `docs/drivers/kosmickrisp.rst`.

### 7.3 Sibling scopes (authoritative; adopted, not re-derived) `[V]`

`docs/research/metal4/`: `README.md` (§2 verdicts, §4 shifts, §6 register); `01`–`05` (construct
mappings); `06` (§3 availability gate, §4 no Metal-3 fallback); `07` (oracles, CI tiers);
`08` (§2 contract, §6.1 LOC, §7 coexistence, §8 effort, §9 comparables); `09` (§2 draw path, §3
draw mapping, §4 batching A–F, §5 allocators, §6 counters, §7.2 verdict); `10` (§2 capability plan,
§3 guest feature set, §4 target architecture, §5 delete/keep/reshape, §6 effort, §7 what gets worse,
§9 register deltas, §10 risks); `11` (§2 testable contract, §3 RRC substrate, §5 recording seam,
§9 design-for-verifiability). Inventory: `inventory/SCHEMA.md` §6 (HLD template), `inventory/README.md`.

<!-- doc-end -->
