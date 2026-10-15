# 8. RSX backend contract & the `Emu/RSX/VK/**` port surface

Owner/scope: Scope 8 (cross-cutting) | Status: draft | Date: 2026-10-15

> **Read-only research.** No RPCS3 source, CMake, test or `.ci` file was modified to produce
> this document and no build was run. `rpcs3/Emu/RSX/VK/**` may be edited concurrently by other
> agents, so **symbol names are the stable reference and `file:line` values are advisory**.
>
> **Tags.** `[V]` = verified against this tree or a primary document; `[I]` = inference/synthesis;
> `[U]` = could not verify (claimed but unchecked). Line numbers cite the branch `metal4` tree at
> the time of writing.
>
> **Relationship to scopes 01–05.** This is the cross-cutting companion to
> [`01-core-memory.md`](01-core-memory.md), [`02-submission-sync.md`](02-submission-sync.md),
> [`03-shaders-pipelines-descriptors.md`](03-shaders-pipelines-descriptors.md),
> [`04-images-formats-samplers-rtt.md`](04-images-formats-samplers-rtt.md) and
> [`05-presentation-and-impossible-register.md`](05-presentation-and-impossible-register.md), and
> is subordinate to the master [`README.md`](README.md). Scopes 01–05 own the **per-API Vulkan →
> Metal 4 construct mappings**; this document deliberately does **not** repeat them. It answers a
> different question: *given RPCS3's own backend contract, exactly which files move, how hard is
> each, and what does a honest native `MTL/` backend cost?*

---

## 1. Scope & relationship

### 1.1 What this scope owns

1. **The RSX backend contract** — the virtual interface a backend must implement, verified from
   the tree (§2). This is the fixed boundary the port must satisfy.
2. **A complete, file-by-file port inventory of `rpcs3/Emu/RSX/VK/**`** — all **112 files /
   34,213 LOC** `[V]`, each with role, difficulty (1–5) and disposition (§3–§6). This is the
   centrepiece.
3. **Coexistence & selection** — how an `MTL/` backend lives beside Vulkan/MoltenVK/KosmicKrisp,
   and the exact config/enum/CMake seams it plugs into (§7).
4. **Realistic effort & phasing** — a bottom-up LOC estimate and a minimum-viable-backend
   definition, reconciled with the top-down figures in `../05-metal-backend-northstar.md` and
   `README.md` §7 (§8).
5. **Evidence from comparable ports** — Dolphin, Godot, PPSSPP, DuckStation, Ryujinx, and
   RPCS3's own upstream position (§9).

### 1.2 What this scope explicitly does **not** own

- Construct-level `Vk* → MTL4*` mappings (scopes 01–05).
- The impossibility register (master `README.md` §6, superseding `05` §9).
- The dependency graph and phasing narrative (master `README.md` §5, §7) — §8 here is an
  independent bottom-up cross-check, not a replacement.
- Any source/build change. This is a scoping document only.

### 1.3 Why a separate scope exists

Docs 01–05 are organised **by Vulkan subsystem** (instance, submission, shaders, images,
present). That framing is necessary but not sufficient: a backend is not a bag of API calls, it
is an implementation of one C++ contract (`rsx::thread`) plus a set of trait specialisations over
shared code. The porting question that actually determines schedule is *"which of the 112 files
survive, which are deleted, and which are the three or four that eat the year"* — which is this
scope. §2 and §3–§6 are the two halves of that answer.

---

## 2. The RSX backend contract (what a backend must implement)

Scope 8's first job is to state the fixed target. Everything here is `[V]` against the tree.

### 2.1 The virtual base: `rsx::thread`

`class thread : public cpu_thread, public GCM_context, public GRAPH_backend`
(`rpcs3/Emu/RSX/RSXThread.h:107`) is the base of every backend. Construction is templated on the
concrete type and always goes through `rsx::thread` (`gui_application.cpp:787-808`). The
**pure-virtual / must-override** surface a new backend must supply is small and stable:

| Requirement | Declaration | Notes |
|---|---|---|
| cycle timer | `virtual u64 get_cycles() = 0` (`RSXThread.h:337`) | host monotonic clock |
| display refresh | `virtual f64 get_display_refresh_rate() const = 0` (`:155`) | from the window/present layer |
| thread bring-up | `virtual void on_init_thread() = 0` (`:370`) | device/queue/swapchain setup |
| flip handling | `virtual void flip(const display_flip_info_t&) = 0` (`:372`) | `display_flip_info_t` at `:113` |

Non-pure hooks with default (no-op) bodies that a full backend overrides: `on_exit` (`:348`),
`do_local_task` (`:353`), `emit_geometry` (`:355`), `clear_surface` (`:364`), `begin`/`end`
(`:365-366`), `execute_nop_draw` (`:367`), `on_frame_end` (`:371`), `timestamp` (`:373`),
`on_access_violation` (`:374`), `on_invalidate_memory_range` (`:375`), `notify_tile_unbound`
(`:376`), `renderctl` (`:379`), `begin_conditional_rendering`/`end_conditional_rendering`
(`:390-391`), `write_barrier` (`:396`), `sync_hint` (`:397`), `release_GCM_label` (`:398`),
`on_semaphore_acquire_wait` (`:444`), `scaled_image_from_memory` (`:450`), `get_programs`
(`:454`), `is_current_program_interpreted` (`:458`), `patch_transform_constants`
(`VKGSRender.h:289`). `[V]`

### 2.2 The capability struct: `rsx::thread::backend_configuration`

The single most important contract artefact is `struct backend_configuration` (`RSXThread.h:80-98`),
a plain struct of 16 booleans the shared RSX code consults instead of querying the driver. It is
held as a member `backend_configuration backend_config{}` (`:123`) and exposed read-only via
`get_backend_config()` (`:230`) / `get_renderer_backend_config()` (`:489-491`). A backend fills it
at init. `[V]`

Fields (`RSXThread.h:82-97`): `supports_multidraw`, `supports_hw_a2c`, `supports_hw_a2c_1spp`,
`supports_hw_renormalization`, `supports_hw_msaa`, `supports_hw_a2one`,
`supports_hw_conditional_render`, `supports_hw_instanced_rendering`, `supports_passthrough_dma`,
`supports_asynchronous_compute`, `supports_host_gpu_labels`, `supports_normalized_barycentrics`,
`supports_last_provoking_vertex`, `supports_programmable_blending`,
`supports_extended_depth_range`, `supports_framebufferless_rendering`. `[V]`

The Vulkan backend fills it in `VKGSRender.cpp:746-909` and consumes it throughout the draw path
(e.g. alpha-to-coverage at `VKGSRender.cpp:394-396`, host-GPU-label gating at `:1712`,`:1767`). `[V]`
**This struct is the highest-value port target**: get it right and ~55k LOC of shared
`Common/`/`Program/`/`NV47/` emulation is reused unchanged. `[I]`

### 2.3 The intermediate base and the report/query contract

`class GSRender : public rsx::thread` (`rpcs3/Emu/RSX/GSRender.h:20`) adds the architecture-neutral
render scaffolding; `VKGSRender : public GSRender, public ::rsx::reports::ZCULL_control`
(`VKGSRender.h:38`). The ZCULL/query contract is a second virtual surface: `ZCULL_control` declares
`begin_occlusion_query`, `end_occlusion_query`, `check_occlusion_query_status`,
`get_occlusion_query_result`, `discard_occlusion_query` (`RSXZCULL.h:212-216`) plus `~ZCULL_control`
(`:157`), overridden by `VKGSRender.h:265-269`. `[V]`

### 2.4 The trait/split points that make reuse possible

The port is not "rewrite the emulator": the heavy caches are already generic and parameterised on
backend traits. Verified specialisation points:

- `vk::surface_cache : public rsx::surface_store<vk::surface_cache_traits>`
  (`VKRenderTargets.h:747`; traits at `:159`) — generic surface store in `Common/surface_store.h`
  (`:4`). `[V]`
- `vk::texture_cache : rsx::texture_cache<texture_cache_type, texture_cache_traits>`
  (`VKTextureCache.h:21-26`; `cached_texture_section` at `:34-36`) — generic cache in
  `Common/texture_cache.h` (`:9`). `[V]`
- `struct program_cache : public program_state_cache<VKTraits>` with `VKTraits` at
  `VKProgramBuffer.h:9`/`:72`. `[V]`

Shared, backend-agnostic code in the current tree (all `[V]`, for scale): `Emu/RSX/Common/` 34
files / 17,164 LOC; `Emu/RSX/Program/` 72 files / 17,879 LOC; `Emu/RSX/NV47/` 23 files / 3,246
LOC. `Emu/RSX/GL/` (the second full backend) is 66 files / 18,193 LOC and is the best local
precedent for "a second backend over the same traits". `[V]`

### 2.5 Contract-size verdict

The **required** interface is ~4 pure virtuals + ~25 overridable hooks + a 16-bool struct + 5
ZCULL virtuals. That is ~50 signatures. The backend is large not because the contract is large but
because **the 112-file Vulkan implementation is one particular way to satisfy ~50 signatures**,
and the three generic caches (`surface_store`, `texture_cache`, `program_state_cache`) are where
almost all of the backend-specific complexity is injected. `[I]`

---

## 3. Port-surface method & legend

### 3.1 Measurement

- Full tree enumerated with `find`; LOC via `wc -l` on `.cpp/.h/.hpp/.mm`. `[V]`
- Totals: **112 files / 34,213 LOC** — top-level `VK/` 56 files / 23,633 LOC; `vkutils/` 51
  files / 10,003 LOC; `upscalers/` 5 files / 577 LOC. These three sum exactly. `[V]`
- Comparison points measured identically: `GL/` 66 files / 18,193 LOC; `Common/` 34 / 17,164;
  `Program/` 72 / 17,879; `NV47/` 23 / 3,246; `Overlays/` 98 / 20,151. `[V]`

> Note: doc 03's "111 files / 33,089 LOC" figure was measured on `kosmickrisp-integration`; the
> current `metal4` tree is **112 files / 34,213 LOC**. This document uses the current tree. `[V]`

### 3.2 Difficulty scale (identical to scopes 01–05)

**1** trivial rename · **2** mechanical · **3** real work, well-trodden · **4** hard / needs design
· **5** unknown or impossible. `[V]`

### 3.3 Disposition vocabulary (exactly four values)

- **ported** — the responsibility survives into a new `MTL/` subsystem; the code is retargeted
  (internally it may be a near-total rewrite; the *role* is carried).
- **replaced** — the file's role is taken over by a structurally different Metal 4 mechanism
  (e.g. `VkRenderPass`+`VkFramebuffer` → per-encoder `MTL4RenderPassDescriptor`); there is no 1:1
  successor file.
- **deleted** — nothing in `MTL/` corresponds; drop it for the native backend (the Vulkan backend
  may of course keep it).
- **untouched** — backend-agnostic shared code; stays exactly as-is outside `VK/` and is not part
  of the port surface.

### 3.4 Row honesty

Header/implementation pairs are collapsed into one row for readability but **both files are
counted**. Trivial files (e.g. `vkutils/metal_layer.h`, 2 lines) are listed explicitly rather than
hidden. Every one of the 112 files appears in exactly one row of §4–§6.

---

## 4. Inventory A — top-level `Emu/RSX/VK/` (56 files / 23,633 LOC)

| # | File(s) | LOC | What it does | Metal 4 embodiment | Diff | Disp |
|---|---|---|---|---|---|---|
| A1 | `VulkanAPI.h`/`.cpp` | 88 | Hand-declares the Vulkan extension structs/functions RPCS3 uses beyond the loader headers. | No analogue; metal-cpp / Metal headers replace it. | 1 | deleted |
| A2 | `VKProcTable.h` | 44 | Manual `vkGet*ProcAddr` loader table (used when not linked to the loader). | No analogue — Metal links directly. | 1 | deleted |
| A3 | `VKCommonDecompiler.h`/`.cpp` | 79 | Shared decompiler capability helpers used by VP/FP translators. | Folded into the shader front-end (SPIR-V caps). | 2 | replaced |
| A4 | `VKFormats.h`/`.cpp` | 525 | `VkFormat` ↔ RSX/GCM surface-format tables; format-class and swizzle queries. | New `MTLPixelFormat` mapping tables (scope 04 owns the format rules). | 3 | ported |
| A5 | `VKCommonPipelineLayout.h`/`.cpp` | 39 | Shared descriptor-set/push-constant layout built once per program. | One `MTL4ArgumentTable` per encoder; layout object disappears (scope 03). | 2 | replaced |
| A6 | `VKProgramBuffer.h` | 111 | `VKTraits` + `program_state_cache<VKTraits>` specialisation. | `MTLTraits` over the same generic cache. | 2 | ported |
| A7 | `VKPipelineCompiler.h`/`.cpp` | 617 | glslang → SPIR-V compile, pipeline cache, program-hash lookup. | `glslang` unchanged; insert SPIRV-Cross `CompilerMSL` → `MTL4Compiler` (scope 03). | 4 | ported |
| A8 | `VKProgramPipeline.h`/`.cpp` | 963 | Assembles graphics/compute pipeline objects from RSX state + shaders. | `MTL4RenderPipeline`/compute pipeline descriptors (scope 03). | 4 | ported |
| A9 | `VKVertexProgram.h`/`.cpp` | 638 | RSX VP ucode → GLSL, constant binding, `VkPipeline` vertex stage. | Same GLSL, MSL via SPIRV-Cross; stage = `MTL4LibraryFunction`. | 3 | ported |
| A10 | `VKFragmentProgram.h`/`.cpp` | 759 | RSX FP ucode → GLSL, ROP/alpha-test, fragment stage. | Same GLSL → MSL; blending via encoder state. | 3 | ported |
| A11 | `VKShaderInterpreter.h`/`.cpp` | 959 | Software shader interpreter driven by compute (fallback when a program can't compile). | Compute-kernel equivalent on `MTL4ComputeCommandEncoder`. | 4 | ported |
| A12 | `VKVertexBuffers.cpp` | 379 | Vertex-input assembly, attribute divisors, index buffers. | `MTL4` vertex descriptor + argument table. | 2 | ported |
| A13 | `VKDraw.cpp` | 1427 | The draw-encoding path: state binding, multi-draw, primitive restart, instancing. | Draw calls on a render encoder; explicit barrier bookkeeping (scope 02). | 4 | ported |
| A14 | `VKDMA.h`/`.cpp` | 570 | Host DMA downloads/reads back to guest memory (label reads, `cellGcm` queries). | Blit/compute readback via shared/private buffers (scope 01/04). | 3 | ported |
| A15 | `VKCompute.h`/`.cpp` | 1055 | Compute framework + kernels (deswizzle, format convert, image ops). | Compute encoders; kernels compiled to MSL (scope 03). | 4 | ported |
| A16 | `VKTextureCache.h`/`.cpp` | 2293 | Texture cache: traits, invalidation, upload, view/format reconciliation. | New traits over `rsx::texture_cache`; MTL textures + residency (scope 04). | 5 | ported |
| A17 | `VKTexture.cpp` | 1631 | Per-texture upload, format conversion, deswizzle, mip handling. | MTL texture creation + copy/convert encoders (scope 04). | 5 | ported |
| A18 | `VKRenderTargets.h`/`.cpp` | 1906 | Surface cache traits/impl, render-target management, tile memory. | New traits over `rsx::surface_store`; `MTL4RenderPassDescriptor` attachments. | 5 | ported |
| A19 | `VKResolveHelper.h`/`.cpp` | 733 | MSAA resolve, blit, and resolve-on-access helpers. | `MTL4` resolve store-action / blit encoder. | 3 | ported |
| A20 | `VKQueryPool.h`/`.cpp` | 342 | Occlusion/timestamp query pools. | `MTL4CounterHeap` / query handling (scope 02). | 3 | ported |
| A21 | `VKRenderPass.h`/`.cpp` | 523 | `VkRenderPass` + `VkFramebuffer` construction and caching. | **No analogue**: per-encoder `MTL4RenderPassDescriptor` (README §3 #57). | 3 | replaced |
| A22 | `VKFramebuffer.h`/`.cpp` | 187 | Framebuffer attachment binding wrapper. | Collapsed into the encoder's render-pass descriptor. | 3 | replaced |
| A23 | `VKCommandStream.h`/`.cpp` | 219 | Command-buffer pool/ring and submit bookkeeping. | `MTL4CommandBuffer` + allocator lifecycle (scope 02). | 3 | ported |
| A24 | `VKAsyncScheduler.h`/`.cpp` | 249 | Async-compute scheduling and overlap with the render queue. | Second `MTL4CommandQueue` + explicit sync (scope 02). | 4 | ported |
| A25 | `VKDataHeapManager.h`/`.cpp` | 111 | Ring-heap snapshot/restore for state rollback. | Same idea over `MTLBuffer` sub-allocations. | 2 | ported |
| A26 | `VKMemAlloc.cpp` | 63 | `#define VMA_IMPLEMENTATION` (single TU for VMA). | No VMA; `MTLHeap`/allocator replaces it (scope 01). | 1 | replaced |
| A27 | `VKResourceManager.h`/`.cpp` | 531 | Scratch/`null` resources, sampler pool, memory-pressure handling. | MTL scratch/null textures, argument-table sampler pool. | 4 | ported |
| A28 | `VKHelpers.h`/`.cpp` | 472 | Miscellaneous helpers (image layout, small utilities). | Re-cast onto Metal helpers. | 2 | ported |
| A29 | `VKOverlays.h`/`.cpp` | 1232 | In-emulator overlay UI (FPS, messages, OSD) drawn by the backend. | Same overlay geometry over an MTL pipeline (scope 05). | 3 | ported |
| A30 | `VKPresent.cpp` | 1017 | Swapchain acquire/present, frame flip, screenshot path. | `CAMetalLayer`/`CAMetalDrawable` present (scope 05). | 4 | ported |
| A31 | `VKGSRenderTypes.hpp` | 347 | Shared types: `heap_snapshot`, frame context, submit info. | MTL equivalents for frame/submit state. | 2 | ported |
| A32 | `VKGSRender.h`/`.cpp` | 3524 | **The backend class**: init, capability fill (`:746-909`), frame loop, all `rsx::thread` overrides. | `MTLGSRender` — the spine of the new backend. | 5 | ported |

**Top-level roll-up:** 56 files / 23,633 LOC. Diffs: 1×A1,A2 (2); 2×A3,A5,A6,A12,A25,A28,A31
(7); 3×A4,A9,A10,A14,A19,A20,A21,A22,A23,A29 (10); 4×A7,A8,A11,A13,A15,A24,A27,A30 (8);
5×A16,A17,A18,A32 (4). Dispositions: deleted 2, replaced 5 (A3,A5,A21,A22,A26), ported 49. `[V]`

---

## 5. Inventory B — `vkutils/` (51 files / 10,003 LOC)

| # | File(s) | LOC | What it does | Metal 4 embodiment | Diff | Disp |
|---|---|---|---|---|---|---|
| B1 | `instance.h`/`.cpp` | 470 | `VkInstance`, extension/metal-surface setup, device enumeration. | **No instance object**; `CreateSystemDefaultDevice()` (scope 01). | 2 | replaced |
| B2 | `device.h`/`.cpp` | 1427 | Physical/logical device, feature negotiation, limits, memory-type discovery, queue config. | `MTLDevice` probe; `MetalFeatures`/`MetalLimits` (scope 01). | 4 | ported |
| B3 | `chip_class.h`/`.cpp` | 233 | Vendor/chip classification (`MVK`,`HONEYKRISP`,`KOSMICKRISP`). | `supportsFamily:` / registry ID classification (scope 01). | 2 | ported |
| B4 | `vk_driver_selection.h` | 1010 | Dependency-free MVK/KK/Honeykrisp coexistence seams, adapter labels, chip families. | Not needed by a native backend; stays a Vulkan-backend concern. | 1 | deleted |
| B5 | `memory.h`/`.cpp` | 758 | `mem_allocator_*`, `memory_block`, host-pointer/external memory, VMM pools. | `MTLHeap`/allocator, external-buffer support (scope 01). | 4 | ported |
| B6 | `image.h`/`.cpp` | 716 | `VkImage` wrapper + creation. | `MTLTexture` wrapper (scope 04). | 3 | ported |
| B7 | `image_helpers.h`/`.cpp` | 259 | Layout transitions, view/format helpers. | Views via `MTLTexture`; layouts are implicit (scope 04). | 2 | replaced |
| B8 | `buffer_object.h`/`.cpp` | 232 | `VkBuffer` wrapper + backing memory. | `MTLBuffer` wrapper (scope 01). | 2 | ported |
| B9 | `descriptors.h`/`.cpp` | 785 | Descriptor sets, layouts, pools, caching. | `MTL4ArgumentTable` (scope 03). | 4 | replaced |
| B10 | `graphics_pipeline_state.hpp` | 220 | Pipeline-state struct + hash/equality for caching. | `MTL4RenderPipelineDescriptor` state + hash. | 3 | ported |
| B11 | `pipeline_binding_table.h` | 23 | Fixed binding indices for the pipeline layout. | Constant argument-table slots. | 1 | replaced |
| B12 | `framebuffer_object.hpp` | 112 | `VkFramebuffer` RAII wrapper. | Collapsed into render-pass descriptor. | 2 | replaced |
| B13 | `commands.h`/`.cpp` | 327 | Command-pool/buffer wrappers and one-time-submit helpers. | `MTL4CommandBuffer`/allocator (scope 02). | 3 | replaced |
| B14 | `barriers.h`/`.cpp` | 164 | Pipeline/image/memory barrier helpers. | Explicit `MTL4` barrier encoder (scope 02). | 3 | replaced |
| B15 | `sync.h`/`.cpp` | 804 | Fences, semaphores, events, timeline. | `MTLSharedEvent` + command-buffer completion (scope 02). | 4 | ported |
| B16 | `sampler.h`/`.cpp` | 339 | Sampler cache/creation. | `MTLSamplerState` cache (scope 04). | 3 | ported |
| B17 | `data_heap.h`/`.cpp` | 382 | Host-visible ring heap (`data_heap`), dirty ranges, heap pool flags. | `MTLBuffer` ring + `didModifyRange` (scope 01). | 3 | ported |
| B18 | `scratch.h`/`.cpp` | 236 | Null/scratch sampler, image view, buffers. | MTL scratch/null resources. | 2 | ported |
| B19 | `garbage_collector.h` | 58 | Deferred-destruction list for GPU-touched objects. | Same pattern over MTL resources. | 1 | ported |
| B20 | `unique_resource.h`/`.cpp` | 34 | Small RAII unique-handle wrapper. | metal-cpp `NS::RetainPtr` idiom. | 1 | ported |
| B21 | `shared.h`/`.cpp` | 271 | Ref-counted base + shared utilities. | Generic; retargetable unchanged. | 1 | ported |
| B22 | `debug_print.h`/`.cpp` | 49 | Vulkan object debug printing. | Replaced by Metal validation/Xcode tooling. | 1 | replaced |
| B23 | `ex.h`/`.cpp` | 101 | `vk::exception` type and throw helper. | Same pattern, MTL error strings. | 1 | ported |
| B24 | `metal_layer.h`/`.mm` | 12 | `VK_EXT_metal_surface` shim: `GetCAMetalLayerFromMetalView` (`metal_layer.mm:9`, `return ((NSView*)view).layer;`). | **The seed of the native present path** (scope 05) — becomes first-class. | 1 | ported |
| B25 | `swapchain.h`, `swapchain_core.h`, `swapchain.cpp`, `swapchain_macos.hpp`, `swapchain_unix.hpp`, `swapchain_win32.hpp`, `swapchain_android.hpp` | 931 | Multi-platform swapchain abstraction (`_core` + per-OS headers); macOS path is the relevant one. | `CAMetalLayer` + drawable management (scope 05). | 3 | ported |
| B26 | `query_pool.hpp` | 50 | `VkQueryPool` RAII wrapper. | `MTL4CounterHeap` wrapper (scope 02). | 3 | ported |

**`vkutils/` roll-up:** 51 files / 10,003 LOC. Diffs: 1×B4,B11,B19,B20,B21,B22,B23,B24 (8);
2×B1,B3,B7,B8,B12,B18 (6); 3×B6,B10,B13,B14,B16,B17,B25,B26 (8); 4×B2,B5,B9,B15 (4).
Dispositions: deleted 1 (B4); replaced 7 (B1,B7,B9,B11,B12,B13,B14,B22 — note B22 counted once,
so 8 rows replaced: B1,B7,B9,B11,B12,B13,B14,B22); ported 42. `[V]`

---

## 6. Inventory C — `upscalers/` (5 files / 577 LOC) + full-tree roll-up

| # | File(s) | LOC | What it does | Metal 4 embodiment | Diff | Disp |
|---|---|---|---|---|---|---|
| C1 | `fsr_pass.h`, `fsr1/fsr_pass.cpp` | 479 | FSR1 upscaling pass used by the backend post-chain. | Same shader (GLSL→MSL) on a compute/fragment pass; or map to MetalFX. | 3 | ported |
| C2 | `upscaling.h`, `bilinear_pass.hpp`, `nearest_pass.hpp` | 98 | Upscaling mode enum + bilinear/nearest fallback passes. | Same passes over MTL pipelines. | 2 | ported |

**`upscalers/` roll-up:** 5 files / 577 LOC, 2 rows, diff 3/2, all ported. `[V]`

### 6.1 Complete-tree roll-up (the headline table)

| Bucket | Files | LOC | Rows | Diff 1 | 2 | 3 | 4 | 5 | ported | replaced | deleted |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `VK/` top-level | 56 | 23,633 | 32 | 2 | 7 | 10 | 8 | 4 | 49 | 5 | 2 |
| `vkutils/` | 51 | 10,003 | 26 | 8 | 6 | 8 | 4 | 0 | 42 | 8 | 1 |
| `upscalers/` | 5 | 577 | 2 | 0 | 1 | 1 | 0 | 0 | 5 | 0 | 0 |
| **Total** | **112** | **34,213** | **60** | **10** | **14** | **19** | **12** | **4** | **96** | **13** | **3** |

> Row counts exceed file counts because `.h`/`.cpp` pairs are collapsed; the file column above
> reconciles to 112 exactly. `[V]`

**Reading the roll-up.**

- **Only 3 files are clean "delete".** `VulkanAPI.*` (extension declarations), `VKProcTable.h`
  (manual loader) and `vkutils/vk_driver_selection.h` (MVK/KK coexistence) exist *only* because
  there is a Vulkan driver underneath. A native backend needs none of them. `[V]`
- **13 rows are "replaced", not "ported"** — and they cluster exactly where the master README
  predicts the structural shifts: instance/loader (B1), descriptors (B9), pipeline layout (A5),
  binding table (B11), render pass + framebuffer (A21/A22/B12), command pools (B13), barriers
  (B14), VMA (A26), layout/view helpers (B7), debug print (B22), decompiler caps (A3). These are
  *"object disappears"* events, not API translations. `[I]`
- **Only 4 rows rate difficulty 5**, and they are precisely the three heavy caches plus the
  backend class: `VKTextureCache` (A16), `VKTexture` (A17), `VKRenderTargets` (A18),
  `VKGSRender` (A32). Everything else is 1–4. `[V]`
- **The long tail is small.** 24 of 60 rows are difficulty 1–2 (≈40%), i.e. mechanical or
  trivial; they are the cheapest early wins and should be front-loaded. `[I]`

---

## 7. Coexistence & backend selection

A native backend must not remove or destabilise the Vulkan path. The tree already demonstrates
the required pattern with MoltenVK/KosmicKrisp coexistence, so the native backend should reuse the
same seams. All `[V]` unless noted.

### 7.1 The renderer enum and config

- `enum class video_renderer { null, opengl, vulkan };` (`rpcs3/Emu/system_config_types.h:44-49`).
  **Add a `metal` enumerator.** `[I]` (proposed change, not present).
- `cfg::_enum<video_renderer> renderer{ ..., video_renderer::vulkan }` when `HAVE_VULKAN`, else
  `video_renderer::opengl` (`rpcs3/Emu/system_config.h:122-126`). On Apple an `MTL`-capable build
  would want `metal` as the default while keeping `vulkan` selectable. `[I]`
- Apple driver preference already exists for the *Vulkan* backend:
  `enum class vulkan_apple_driver { auto_, moltenvk, kosmickrisp }`
  (`system_config_types.h:322-327`), consumed in `VKGSRender.cpp:497-503`. This is the precedent
  for a future `renderer = metal` sitting cleanly beside `renderer = vulkan`. `[V]`

### 7.2 The construction switch

`g_emu_callbacks.init_gs_render` (`rpcs3/rpcs3qt/gui_application.cpp:787-808`) switches on
`g_cfg.video.renderer.get()` and does `g_fxo->init<rsx::thread, named_thread<VKGSRender>>(ar)`
(`:806`), guarded by `#if defined(HAVE_VULKAN)`. A native backend adds:

```cpp
case video_renderer::metal:
{
#if defined(HAVE_METAL)
    g_fxo->init<rsx::thread, named_thread<MTLGSRender>>(ar);
#endif
    break;
}
```

This is the *entire* runtime selection surface: one enum value, one config branch, one switch
arm — because everything downstream goes through `rsx::thread`. `[V]`

### 7.3 The Qt adapter-list / labels

`render_creator.cpp` enumerates devices on a worker thread via `vk::instance`
(`render_creator.cpp:33-71`) and appends adapter labels, tagging duplicate Apple drivers with
`vk::driver_selection::is_apple_gpu_driver(...)` / `make_adapter_label(...)` (`:57-68`). A Metal
backend needs its own (much simpler) enumeration — one device, one name — and its own label
branch. `[V]`

### 7.4 Build-system seam

- The Vulkan backend is compiled only when `TARGET 3rdparty_vulkan` and is `-DHAVE_VULKAN`
  (`rpcs3/Emu/CMakeLists.txt:627-684`; define set at `3rdparty/CMakeLists.txt:188`). `[V]`
- The single Apple-specific source, `vkutils/metal_layer.mm`, is added under `if(APPLE)`
  (`rpcs3/Emu/CMakeLists.txt:679-683`). `[V]`
- A native backend therefore adds an `if(APPLE)` block listing `RSX/MTL/*.cpp/.mm`, links
  Metal/MetalKit/QuartzCore/AppKit, and defines `HAVE_METAL`. No existing target needs editing
  beyond the additive block. `[I]`

### 7.5 Coexistence model to copy

Godot ships exactly this model: native Metal is used on Apple Silicon, and **MoltenVK remains the
fallback** where native Metal is unavailable (x86 macOS) — "Godot also supports Metal rendering
via MoltenVK, which is used as a fallback when native Metal support is not available" ([Godot
docs][gdocs]). Dolphin explicitly keeps both backends: "MoltenVK is here to stay" ([Dolphin
report][dolphin22]). RPCS3 should do the same — ship `MTL/` as an **additional** renderer, not a
replacement. `[V]`/`[I]`

### 7.6 Constraint on macOS version

Because the target is Metal 4 (`MTL4CommandQueue`/`MTL4CommandBuffer`), the native backend implies
a macOS 26+ floor for the `metal` renderer, while `vulkan` keeps the existing floor. This is a
selection/UX constraint, not a port blocker; encode it in the config UI, not in shared code. `[I]`
(corroborated by `README.md` §3 and `../05-metal-backend-northstar.md` §5).

---

## 8. Realistic effort & phasing

### 8.1 Bottom-up LOC estimate

The port surface is **34,213 LOC**. A native backend is not a line-for-line translation: the
deleted/replaced ceremony (Vulkan loader, VMA, descriptor layouts, render-pass objects, barriers)
is offset by new Metal-specific glue (argument tables, residency, render-pass descriptors, MSL
binding, metal-cpp boilerplate). Comparable ports confirm a compression ratio rather than 1:1:

- Dolphin's native Metal backend is **~25 files / ~161 KB** — far smaller than a full Vulkan
  backend, because Dolphin's Vulkan backend is also large ([Dolphin Metal dir][dolphinapi]). `[V]`
- Godot's Metal driver (master) is **~24 files / ~600 KB** of source, with a single
  `rendering_device_driver_metal.cpp` at **127,610 bytes** and `metal3_objects.cpp` at **79,870
  bytes** ([Godot metal dir][godotapi]). `[V]`
- `../05-metal-backend-northstar.md` and `README.md` independently estimate **~25–30k LOC new
  `MTL/`**. `[V]`

**Estimate:** expect **≈60–90% of the Vulkan LOC** for functional equivalence, i.e.
**~20,000–30,000 LOC of new `MTL/`**, centred on **~25,000**. The high end is justified by RPCS3's
per-title quirk surface and the three generic caches, which are larger and more stateful than the
equivalent areas in Dolphin or Godot. `[I]`

### 8.2 Minimum viable backend (MVP) definition

The smallest thing that boots a title, presents, and draws is a strict subset of the inventory.
Defined concretely by row:

| MVP component | Rows | Why minimal |
|---|---|---|
| Backend class + contract | A32, A31 | `MTLGSRender`, capability struct, frame loop |
| Device/instance/features | B1, B2, B3 | one `MTLDevice`, `MetalFeatures`/`MetalLimits` |
| Memory + buffers | B5, B8, B17 | allocator, `MTLBuffer`, upload ring |
| Images + format map | B6, A4 | `MTLTexture`, format tables |
| Samplers | B16 | one sampler cache |
| Commands + sync | B13, B15 | `MTL4CommandBuffer` loop + `MTLSharedEvent` |
| Shader front-end | A7, A9, A10 | GLSL → SPIR-V → MSL → `MTL4Compiler` (scope 03) |
| Pipelines + state | A8, B10 | `MTL4RenderPipeline` + state hash |
| Vertex + draw | A12, A13 | vertex descriptor + draw encoding |
| Pass/attachment | A21/A22 (replaced) | per-encoder `MTL4RenderPassDescriptor` only |
| Program cache | A6 | `MTLTraits` over generic cache |
| Present/overlay | A30, B25, A29, B24 | `CAMetalLayer` present + OSD |
| Helpers/RAII | A28, B19–B23 | scaffolding |
| *Minimal* textures/RTs | A16, A17, A18 (**reduced scope**) | BGRA8 + D32 only; no MSAA/resolve/format zoo |

**MVP definition (one paragraph):** an `MTLGSRender` that opens a `CAMetalLayer`, presents the
overlay, and renders vertex+fragment draws for a handful of bootable titles by compiling RSX GLSL
→ SPIR-V → MSL through SPIRV-Cross `CompilerMSL`, binding resources through a single
`MTL4ArgumentTable`, obeying a minimal explicit-barrier encoder loop, and supporting only BGRA8
colour and D32 depth targets (no MSAA, no exotic formats, no async compute). This is **~15
subsystems / roughly 8,000–12,000 LOC**, consistent with the `README.md` §7 "MVP one draw path"
row (~6–12 person-months). `[I]`

### 8.3 Phase mapping (bottom-up cross-check of `README.md` §7)

| Phase | Inventory rows brought online | New LOC (est.) |
|---|---|---|
| 0 bring-up | A32, A31, B1–B3, B5, B6, B8, B13, B15, A21(replaced), A30, B25, B24, A29 | ~3–5k |
| 1 MVP one draw path | + A7, A9, A10, A8, B10, A12, A13, A4, B16, A6, A28 | ~5–7k |
| 2 memory + sync | + A24, B17, B14, A25, A23 | +2–4k |
| 3 RTT/MSAA/formats/copies | + A18, A19, A17, A16, A15 | +6–10k |
| 4 queries/conditional/async | + A20, B26, A14, A11 | +2–4k |
| 5 parity (upscalers, interpreter, DMA edges) | + C1, C2, A27 | +3–5k |

Sum ≈ 21–35k LOC, bracketing the §8.1 central estimate. `[I]`

### 8.4 The three cost centres

Ranked by `LOC × difficulty` (and by how many other rows depend on them):

1. **`VKGSRender.cpp`** (A32, 3,211 LOC, diff 5) — the spine: every contract override, capability
   fill, and the frame loop. Nothing works without it; it has no smaller port.
2. **`VKTextureCache` + `VKRenderTargets`** (A16+A18, 4,199 LOC, diff 5) — the two trait
   specialisations over the generic caches; where per-title correctness lives and where the
   format/swizzle/RTT rule surface (scope 04) is actually applied.
3. **`VKTexture.cpp`** (A17, 1,631 LOC, diff 5) — upload, deswizzle, format conversion; the
   single densest blob of format-specific behaviour.

Honourable mention: `vkutils/device.cpp` (B2, 1,207 LOC, diff 4) — feature/limit/memory-type
negotiation (scope 01), the gate for everything else. `[I]`

---

## 9. Evidence from comparable Metal ports

| Project | Approach | Measured outcome / lesson | Source |
|---|---|---|---|
| **Dolphin** | Native Metal backend by *tellowkrinkle* (5.0-16965); an earlier *stenzek* attempt stalled. ~25 files in `Source/Core/VideoBackends/Metal/`. | **+23% (Skyward Sword) / +30% (Rogue Leader)** vs MoltenVK after isolating a **45% MoltenVK regression**; native Metal is "still better, but by only a little bit" in fair fights. Kept MoltenVK. | [blog][dolphin22], [dir][dolphinapi], [Vulkan.org][vulkanorg] |
| **Godot** | Native Metal driver (PR GH-88199, Stuart Carnie), merged for 4.4; **Apple-Silicon-only**, MoltenVK fallback otherwise; SPIR-V→MSL via SPIRV-Cross; later refactor toward Metal 4. | ~24 files / ~600 KB; `rendering_device_driver_metal.cpp` is 127,610 bytes alone. Pain points: dynamic uniform offsets, stable argument buffers, startup shader-compile stalls. `metal3_*` + `metal_objects_shared.*` split shows the Metal 3→4 transition cost. | [dev snap][gdev44], [dir][godotapi], [docs][gdocs] |
| **PPSSPP** | **Native Metal** backend for macOS/iOS (Vulkan, OpenGL, D3D11 and Metal). | Confirms a mature emulator can maintain five backends; Metal chosen for "efficient rendering on M-series and A-series chips." | [features][ppsspp] |
| **DuckStation** | Hardware renderer supports D3D11/D3D12/OpenGL/Vulkan/**Metal** (author = stenzek, i.e. the Dolphin Metal author). | A second existence proof that one maintainer can ship a Metal backend alongside four others; also the source of the "starter backend is small" intuition. | [repo][duck] |
| **Ryujinx** | **No native Metal** — Vulkan via MoltenVK, using a *patched MoltenVK fork*. | Patches needed: `Use Metal Private API`, `Use MSL 3.1`, `Disable residency sets`, `Maximize concurrent compilation`, `Do not end current encoder before encodeWait`. Shows the friction MoltenVK imposes that a native backend would avoid. | [fork][ryufork], [patch list][ryupatches] |
| **RPCS3 upstream** | No native Metal; MoltenVK is the supported macOS path. A maintainer position recorded on the macOS/Metal request thread: *"No one wanted a Native metal version. MoltenVK is doing perfectly fine."* | Sets the burden of proof: a native backend must beat a *healthy* MoltenVK/KosmicKrisp, exactly as Dolphin discovered. | [issue 11922][rpcs3issue] |
| **Apple** | Metal 4 introduces `MTL4CommandQueue`/`MTL4CommandBuffer`/`MTL4Compiler`/argument tables; there is a dedicated "Using the Metal 4 compilation API". | Confirms the target API and that the shader-binary model is unchanged (argument tables index classic binding spaces). | [core API][apple4core], [compilation API][apple4compile] |

### 9.1 The single most useful lesson

**A native Metal backend's win over a healthy MoltenVK is modest, and the first honest benchmark
can be misleading.** Dolphin's initial "+big" result was actually the native backend *exposing a
45% MoltenVK regression*; once the regression was fixed the true margin was +23%/+30% in the best
cases ([Dolphin report][dolphin22], [Vulkan.org][vulkanorg]). For RPCS3 — where KosmicKrisp
already sits directly on Metal 4 — the same caution applies doubly: **scope the backend as a
research northstar and a MoltenVK-quality escape hatch, not as a performance headline**, and
measure against a *fixed, healthy* translation layer before committing. `[V]`/`[I]`

---

## 10. Risks, unknowns & what would change the estimate

1. **The trait injections, not the API.** The estimate hinges on how much of A16/A17/A18 is
   genuinely backend-specific versus expressible through `rsx::surface_store` /
   `rsx::texture_cache`. If the generic caches absorb more than expected, the cost centres shrink;
   if RSX's format/swizzle quirks leak into the MTL traits, they grow. `[I]` **Unverified (`[U]`):
   an actual compile of the generic caches against a stub `MTLTraits` has not been attempted.**
2. **MSL correctness surface.** Dolphin's and Godot's pain (dynamic uniform offsets, argument
   buffers, shader-compile stalls) predicts a steady bug tail even with a working pipeline. Scope
   03 owns the mapping; this scope flags it as the dominant *schedule* risk after §8.4. `[V]`/`[I]`
3. **macOS 26+ floor** for the `metal` renderer (§7.6) restricts the test matrix. `[I]`
4. **Coexistence hygiene.** Adding `video_renderer::metal` touches a shared enum and a shared
   switch; the change is tiny but lives in code every backend depends on, so it must be additive
   and feature-gated (`HAVE_METAL`). `[I]`
5. **Maintenance.** Godot explicitly notes it supports native Metal *and* MoltenVK; Ryujinx chose
   patched MoltenVK over a native backend. The long-term cost of a second backend for a small
   team is the same argument that stalled stenzek's Dolphin attempt. `[V]`/`[I]`

**What would change the estimate down:** proving (via a spike) that `MTLTraits` can bind the
generic caches with <2k LOC; reusing a permissive MSL backend; MetalFX replacing FSR1.
**Up:** discovering that per-game RSX quirks require MTL-specific workarounds in the traits, or
that `MTL4RenderPassDescriptor` cannot express some RSX tile/clear semantics without extra passes.

---

## 11. Sources

### Primary / verified against this tree `[V]`

- `rpcs3/Emu/RSX/RSXThread.h` — `rsx::thread` virtual contract (`:107`, `:337`, `:155`, `:370`,
  `:372`), `backend_configuration` (`:80-98`, member `:123`, accessor `:230`, `:489-491`).
- `rpcs3/Emu/RSX/GSRender.h` (`:20`), `rpcs3/Emu/RSX/RSXZCULL.h` (`:157`, `:212-216`).
- `rpcs3/Emu/RSX/VK/VKGSRender.h` (`:38`, overrides `:211-312`), `VK/VKGSRender.cpp`
  (capability fill `:746-909`; use sites `:394-396`, `:1712`, `:1767`).
- `rpcs3/Emu/RSX/VK/VKRenderTargets.h` (`:159`, `:747`), `VK/VKTextureCache.h` (`:21-36`),
  `VK/VKProgramBuffer.h` (`:9`, `:72`), `VK/vkutils/metal_layer.mm` (`:9`).
- `rpcs3/Emu/system_config_types.h` (`:44-49`, `:322-327`), `rpcs3/Emu/system_config.h`
  (`:122-126`), `rpcs3/rpcs3qt/gui_application.cpp` (`:787-808`),
  `rpcs3/rpcs3qt/render_creator.cpp` (`:33-71`),
  `rpcs3/Emu/RSX/VK/vkutils/vk_driver_selection.h`.
- `rpcs3/Emu/CMakeLists.txt` (`:627-684`), `3rdparty/CMakeLists.txt` (`:188`).
- Full-tree enumeration + `wc -l` on `rpcs3/Emu/RSX/VK/**` (112 files / 34,213 LOC) and
  comparison backends (`GL/`, `Common/`, `Program/`, `NV47/`, `Overlays/`).

### External

- [dolphin22] Dolphin Progress Report, Jul–Aug 2022 — Metal backend, +23%/+30%, 45% MoltenVK
  regression: https://dolphin-emu.org/blog/2022/09/13/dolphin-progress-report-july-and-august-2022/
- [dolphinapi] Dolphin Metal backend directory listing (GitHub API):
  https://api.github.com/repos/dolphin-emu/dolphin/contents/Source/Core/VideoBackends/Metal
- [vulkanorg] Vulkan.org news, "MoltenVK Added to Two Emulators":
  https://www.vulkan.org/news/auto-21326-48a2348075f77e07d7d06593806657fa
- [gdev44] Godot 4.4 dev 1 snapshot — native Metal backend (GH-88199, Stuart Carnie), Apple
  Silicon only, MoltenVK fallback: https://godotengine.org/article/dev-snapshot-godot-4-4-dev-1
- [godotapi] Godot `drivers/metal` directory listing (GitHub API), incl.
  `rendering_device_driver_metal.cpp` (127,610 B), `metal3_objects.cpp` (79,870 B):
  https://api.github.com/repos/godotengine/godot/contents/drivers/metal
- [gdocs] Godot internal rendering architecture — Metal driver, SPIRV-Cross, MoltenVK fallback:
  https://docs.godotengine.org/en/4.5/engine_details/architecture/internal_rendering_architecture.html
- Godot Metal driver README:
  https://github.com/godotengine/godot/blob/master/drivers/metal/README.md
- [ppsspp] PPSSPP features — "Vulkan, OpenGL, D3D11 & Metal backends":
  https://ppsspp.co/features.html
- [duck] DuckStation — hardware renderer D3D11/D3D12/OpenGL/Vulkan/Metal:
  https://github.com/stenzek/duckstation
- [ryufork] Ryujinx.MoltenVK patched fork:
  https://github.com/V380-Ori/Ryujinx.MoltenVK
- [ryupatches] Ryujinx MoltenVK patch set (`0001-Use-Metal-Private-API-by-default.patch`,
  `0002-Use-MSL-3.1-by-default-on-macOS.patch`, `0003-Disable-residency-sets.patch`, …):
  https://github.com/V380-Ori/Ryujinx.MoltenVK
- [rpcs3issue] RPCS3 issue #11922 (native Metal request thread; "MoltenVK is doing perfectly
  fine"): https://github.com/RPCS3/rpcs3/issues/11922
- MoltenVK Runtime User Guide (extension limitations, private-API caveats):
  https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md
- [apple4core] Apple, "Understanding the Metal 4 core API":
  https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api
- [apple4compile] Apple, "Using the Metal 4 compilation API":
  https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api
- Apple, "Porting your Metal code to Apple silicon":
  https://developer.apple.com/documentation/apple-silicon/porting-your-metal-code-to-apple-silicon
- Apple, "Managing your game window for Metal in macOS":
  https://developer.apple.com/documentation/metal/managing-your-game-window-for-metal-in-macos

### Sibling scopes (authoritative for per-API mappings; referenced, not duplicated)

- [`01-core-memory.md`](01-core-memory.md) · [`02-submission-sync.md`](02-submission-sync.md) ·
  [`03-shaders-pipelines-descriptors.md`](03-shaders-pipelines-descriptors.md) ·
  [`04-images-formats-samplers-rtt.md`](04-images-formats-samplers-rtt.md) ·
  [`05-presentation-and-impossible-register.md`](05-presentation-and-impossible-register.md) ·
  [`README.md`](README.md) · [`../03-rpcs3-backend-architecture.md`](../03-rpcs3-backend-architecture.md) ·
  [`../05-metal-backend-northstar.md`](../05-metal-backend-northstar.md)
