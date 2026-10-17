# 10. RSX-native Metal 4 backend architecture

Owner/scope: Scope 10 (cross-cutting, design) | Status: draft | Date: 2026-10-15

**Verification legend.** `[V]` = verified this session against the RPCS3 tree, a pristine reference
under `/tmp/metal4-refs/`, or a primary source fetched over HTTP (URL in §11). `[I]` = inference from
`[V]` facts. `[U]` = unverified / no primary source found. `file:line` is advisory (the tree is a moving
branch); **symbol names are the stable reference**. Read-only task: no RPCS3 source, CMake, test or
`.ci` file was modified; only this file was created. Difficulty scale (as elsewhere): **1** trivial
rename · **2** mechanical · **3** real work, well-trodden · **4** hard / needs design · **5** unknown.

---

## 1. Scope, method & relationship to `01`–`09`

### 1.1 The reorientation thesis

`01`–`09` are a **Vulkan → Metal 4 translation inventory**: they enumerate every `Vk*` construct RPCS3
touches and map it to a Metal 4 equivalent (156 merged rows, `README.md` §3; 37-row register, §6).
That was the correct first act — you cannot port what you have not enumerated — but it is the wrong
frame for *design*. Three facts from our own set force the change:

1. Scope `08` puts the port at **~20,000–30,000 LOC** (`08` §8.1) against a 34,213-LOC Vulkan
   backend. **60–90% is rewritten anyway**; that is not a port, it is a new backend.
2. Scope `09` §7.2 concludes a **naive 1:1 port of the VK draw path is a wash** — the win appears
   only with argument tables + indirect batching.
3. The 156 rows are dominated by **Vulkan's invented scaffolding**: raw `vkCmd*` entry points,
   instance/device/loader/extension plumbing, render-pass/subpass, descriptor sets/layouts, barriers.
   RSX has **none of these concepts** (`README.md` §2.3, §4). RPCS3 synthesises them — and so would a
   1:1 Metal port — purely to satisfy the source API.

**Design thesis: derive the backend from the RSX guest contract outward, and let the 156 rows be a
checklist ("does the RSX-native design need this at all?"), not a work breakdown.**

### 1.2 What this supersedes, refines, and leaves standing

| Artefact | Status under this doc |
|---|---|
| `README.md` §3 / `01`–`05` construct mappings | **Reframed**: input checklist; the *mapping targets* remain correct, the *row list* is not a backlog. |
| `README.md` §6 / `05` §9 register (37 rows) | **Stands where framing-independent**; §9 records the rows whose verdict changes. |
| `08` §2 contract, §3 port surface, §8 effort | **Adopted as ground truth**; §6 revises the effort delta. |
| `09` draw-path analysis | **Adopted**; becomes §4.5. |
| `06` build/toolchain/deploy, `07` verification/perf | **Unchanged and still binding** — they are framing-independent engineering constraints. |
| This doc | The **design**; it does not re-map constructs. |

### 1.3 Method

1. Start from the RSX contract (~50 signatures + the 16-bool `backend_configuration` + three generic
   caches) — §2.
2. Constrain by what the PS3 guest can actually do — §3. This is the scope *reducer*.
3. Ask, per contract element, "what is the natural Metal 4 primitive?" — §4.
4. Diff against the VK-derived plan — §5 (dispositions) and §8 (per-category).
5. Re-estimate honestly — §6, §7.

### 1.4 Non-goals

No source/CMake/test changes; no build; no measurement (see `07` §9). No re-derivation of per-construct
`Vk → MTL4` mappings (`01`–`05` own those).

---

## 2. The RSX contract as the design target

### 2.1 The contract, restated (`08` §2, all `[V]`)

- Base: `class thread : public cpu_thread, public GCM_context, public GRAPH_backend`
  (`rpcs3/Emu/RSX/RSXThread.h:107`).
- **4 pure virtuals**: `get_cycles()` (`:337`), `get_display_refresh_rate() const` (`:155`),
  `on_init_thread()` (`:370`), `flip(const display_flip_info_t&)` (`:372`).
- **~25 no-op-default hooks** (override surface): notably `write_barrier`, `sync_hint`, `get_programs`,
  `begin`/`end_conditional_rendering`, `on_access_violation`, `notify_tile_unbound`
  (`RSXThread.h:348-458`; `VKGSRender.h:289`).
- **16-bool `backend_configuration`** (`RSXThread.h:80-98`; member `:123`; accessors `:230`, `:489`).
  Fields at `:82-97`.
- **ZCULL virtuals**: `begin_occlusion_query`, `end_occlusion_query`,
  `check_occlusion_query_status`, `get_occlusion_query_result`, `discard_occlusion_query`
  (`RSXZCULL.h:212-216`), overridden at `VKGSRender.h:265-269`.
- **Trait split points** (reuse surface): `vk::surface_cache : rsx::surface_store<vk::surface_cache_traits>`
  (`VKRenderTargets.h:747`; traits `:159`); `vk::texture_cache : rsx::texture_cache<vk::texture_cache,
  vk::texture_cache_traits>` (`VKTextureCache.h:412`; traits `:21`); `program_cache :
  program_state_cache<VKTraits>` (`VKProgramBuffer.h:9`,`:72`). Generics `Common/{surface_store,texture_cache}.h:22`.

**Interpretation.** The contract is not "a Vulkan backend". It is: *(a)* a lifecycle (init → frame →
flip → exit); *(b)* a capability descriptor the shared emulation reads instead of probing the driver;
*(c)* three cache specialisations; *(d)* two event surfaces (draw encoding and ZCULL). Everything
else in the 34k-line `VK/` tree is **one way to satisfy that**. `08` §2.5 names this exactly: ~50
signatures, not 112 files.

### 2.2 `backend_configuration` → a Metal 4 capability plan

This table is the concrete conversion of the highest-value artefact. **Verdict vocabulary:** *free*
(inherent), *work* (implementable, real effort), *false* (must be forced off), *deleted* (not
applicable). `[V]` unless noted.

| Field | RSX meaning | Metal 4 mechanism | Verdict |
|---|---|---|---|
| `supports_multidraw` | batch N subranges in one call | **no native multi-draw**; host-built `MTLDraw*IndirectArguments` + `draw*(indirectBuffer:)`. Set true only once the batcher lands, else fall back to loops. | **work** (`09` §4-C) |
| `supports_hw_a2c` | alpha-to-coverage | `MTLRenderPipelineDescriptor.alphaToCoverageEnabled` | **free** |
| `supports_hw_a2c_1spp` | A2C at 1 sample | same flag, sample count 1 | **free** `[I]` |
| `supports_hw_renormalization` | NV compressed-format normalisation quirk | Apple GPUs are not NV → behaviour absent | **false** |
| `supports_hw_msaa` | MSAA | `setRasterSampleCount` + store-action resolve | **free** |
| `supports_hw_a2one` | alpha-to-one | `MTLRenderPipelineDescriptor.alphaToOneEnabled` | **free** |
| `supports_hw_conditional_render` | `hw_cond` predicate | **no Metal command**; CPU skip (bring-up) or compute predicate-patch (`09` §6.2). The GL backend also leaves this `false` (`GLGSRender.cpp` never sets it). | **work** / **false** |
| `supports_hw_instanced_rendering` | instanced draws | `instanceCount` on every draw | **free** |
| `supports_passthrough_dma` | guest memory as device memory | no host-pointer *device* path; `newBufferWithBytesNoCopy` is shared only | **false** (CPU copy) |
| `supports_asynchronous_compute` | separate transfer/compute stream | one `MTL4CommandQueue`, many CBs; a second queue + event if truly needed | **work** |
| `supports_host_gpu_labels` | label readback via host memory | unified memory makes labels cheap, but RSX-first may not need the DMA workaround at all | **reshape** |
| `supports_normalized_barycentrics` | interpolation normalisation | Metal barycentrics are normalised | **free** |
| `supports_last_provoking_vertex` | RSX flat-shading last-vertex convention | **Metal exposes no public provoking-vertex mode**; MoltenVK gates `provokingVertexLast` on `useMetalPrivateAPI` (`MVKPipeline.mm:843-844`, `MVKDevice.mm:782-783`) | **false** (smooth fallback; same warning as VK `VKGSRender.cpp:754`) |
| `supports_programmable_blending` | ROP reads current RT | TBDR framebuffer fetch `[[color(N)]]` — Apple **prefers** this over pass splits | **work** (natural fit) |
| `supports_extended_depth_range` | float depth > 1 | Metal stores float depth without a documented [0,1] clamp toggle | **free** `[I]`/`[U]` |
| `supports_framebufferless_rendering` | raster with no attachments (occlusion) | render encoder with a visibility buffer and no colour attachment | **free** `[I]` |

**Net picture.** 16 fields → **~7 free**, **~4 work**, **4 forced false** (`renormalization`,
`passthrough_dma`, `last_provoking_vertex`, `conditional_render` on bring-up), 1 reshaped. The VK fill
(`VKGSRender.cpp:746-909`) spends ~160 lines on vendor/extension negotiation; one vendor, one GPU family
collapses it to a small `MetalFeatures` struct. `[I]`

### 2.3 The three caches are the real design surface

Almost all backend complexity is injected through the traits, not the ~50 signatures (`08` §2.5
`[I]`). The RSX-native plan therefore keeps the **generic caches verbatim** and rewrites only the
traits:

- `rsx::surface_store<Traits>` already computes **R/W hazards, cyclic references, and DMA-range
  tracking** (`surface_store.h:229`, `:1114`, `:1388`, `:1566`) — the exact information Metal 4
  barriers need (§4.4).
- `rsx::texture_cache<Self, Traits>` already owns invalidation, protection, and format
  reconciliation (`texture_cache.h:22`) — backend-agnostic search, Metal-specific storage.
- `program_state_cache<Traits>` owns program-hash lookup; only the pipeline object differs.

**Load-bearing claim: the RSX-native design is ~90% "write new traits for the generic caches + write a
surface-hazard-driven encoder", not "translate VK objects".** `[I]`

---

## 3. The guest feature set (what the backend must support)

The 156 rows map a superset of *Vulkan*. RSX is a much smaller fixed-function model. Separating
"reachable by a PS3 game" from "feature-dumped and unused" is the single most effective scope
reducer. `[V]` unless noted.

### 3.1 Reachable (the backend must implement it)

| Capability | Evidence | Backend consequence |
|---|---|---|
| Fixed VP/FP pipeline only (no GS/TS) | `nv4097.cpp` draw handlers; `Program/` is only VP+FP | No geometry/tessellation stages to build (`README.md` R32). |
| Draw clauses over a FIFO; software vertex pulling | `NV47/FW/draw_call.hpp`; `RSXVertexFetch.glsl:75` | Draw path is already RSX-shaped; no vertex-input descriptor (`09` §3 #9). |
| 10 primitive types incl. quads/fans/polygon | `gcm_enums.h:1253-1265` | Host-side index emulation already exists; Metal topology subset. |
| Surfaces (colour + depth), up to 4 MRT | `gcm_enums.h:1275-1295`, `:1364-1380` | MRT remap via `MTLLogicalToPhysicalColorAttachmentMap` (`README.md` #139). |
| Depth formats Z16 / Z24S8, uint+float variants | `gcm_enums.h:1297-1309` | `D24_UNORM_S8` → `Depth32Float_Stencil8` (`README.md` §4.6). |
| MSAA 1/2/4 samples (explicitly resolved) | `gcm_enums.h:1347-1353` | Store-action resolve; exploded resolve textures (R16). |
| ~13 colour surface formats | `gcm_enums.h:1364-1380` | Format table (`04` §2.3). |
| Programmable blending / ROP read-back | `RSXThread.cpp:299-420`; `supports_programmable_blending` | Framebuffer fetch. |
| Per-texture border colour; LOD bias; sample mask | `gcm_enums.h:700`; `VKPipelineCompiler.cpp:151` | Border fallback; S4.6 LOD bias; sample-mask caveat (R18/R19/R34). |
| Line width register | `gcm_enums.h:619`; `VKDraw.cpp:175-177` | Clamp to 1.0 (R24). |
| Blit engine (swizzled transfers, format convert) | `nv3089.cpp` `decode_transfer_registers`; `nv0039.cpp`; `nv308a.cpp`; `nv406e.cpp` | Semantics matter, not `vkCmdBlitImage` — compute/meta pass (§4.1). |
| ZCULL / occlusion; conditional render | `RSXZCULL.h:212-216`; `RSXThread.cpp:2877-2912` | Visibility buffer; predicate patch (§4.5). |
| Async texture streaming | `VKAsyncScheduler`; `system_config.h` async flag | Second queue or batched CBs (§4.5). |

### 3.2 Feature-dumped and unused (do not build it)

| Feature | Why it is dead for RSX |
|---|---|
| Sparse binding | Feature-dumped only, never used in the draw path (`README.md` R5). |
| Geometry / tessellation / transform feedback / pipeline statistics | Fixed VP/FP model cannot reach them (R32). |
| fp64 / 64-bit atomics | RSX shaders do not emit them (R33). |
| `vkAllocationCallbacks`, queue-family ownership | No Metal analog; single queue family (R36). |
| Wide lines, provoking vertex, custom border colour | Not expressible in public Metal; RSX use is peripheral (R24/R18; `MVKPipeline.mm:843`). |
| Stereo present | Reachable in principle but rare; not a milestone (`05` R31). |
| 1D textures / 3D-array compat / linear 3D | Rare; best-effort (`05` Q28; `README.md` R20/R26). |

### 3.3 What this buys the schedule

The reduction is not in the *shader* path (RSX VP/FP is a large translator: `Program/` 72 files /
17,879 LOC, `[V]`) but in the **API surface**: no render-pass graph, no descriptor model, no subpass
dependencies, no extendable pipeline stages — exactly the categories that dominate the 156 rows (§8).

---

## 4. Target architecture

Each subsection states the **RSX requirement**, the **Metal 4 shape**, and **why it differs from the
VK-derived plan**.

### 4.1 Resource & memory model

**RSX requirement.** A single contiguous guest memory pool (main + local ~512 MB) plus host-managed
buffers (attribute ring, index ring, constant uploads) and textures uploaded from guest RAM.
Guest-visible memory is *unified*; the RSX thread writes to it. `[V]`

**Metal 4 shape.** One `MTLDevice`; one placement `MTLHeap` per storage class (`StorageModeShared`
for guest-visible/host-written, `StorageModePrivate` for GPU-only); `newBuffer(offset:)` /
`newTexture(offset:)` sub-allocation; `MTLResidencySet` attached to the queue, updated per frame.
Metal's `hasUnifiedMemory()` collapses the Vulkan `host_visible_coherent` / `device_local` /
`device_bar` trichotomy to **two** storage modes (`README.md` §4; `01` #9/#10). `[V]`

**Why it differs.** The VK plan carries `mem_allocator_vma` + `mem_allocator_vk` +
`get_compatible_memory_type` + type-index search (`01` #11/#15/#16, diff 3–4). RSX-first asks one
question — *guest-visible or GPU-only?* — and picks Shared or Private; VMA, both allocators, the type
index, and the coherence "dance" (`02` #24) are **Vulkan surface, not RSX requirements**. Survivors: a
placement heap, an upload ring, residency; the `data_heap` shadow (`01` #24) likely collapses to shared
rings on unified memory. `[I]`

### 4.2 Binding model — argument tables from RSX bindings, not descriptors

**RSX requirement.** The shader reads from a fixed, small set of *sources*, not "descriptor sets":
transform/vertex constants, fragment constants, vertex streams (software-pulled), textures, vertex
textures, a per-draw `draw_parameters` block, the ROP/fragment-state block, the rasteriser env, an
instancing lookup table, and the conditional-render predicate. `[V]`

**Metal 4 shape.** One `MTL4ArgumentTable` sized from the RSX shader ABI. Critically, RPCS3 **already
defines the slot map**: `vk::pipeline_binding_table` reserves **12 fixed slots**
(`pipeline_binding_table.h`: vertex params 0, vertex constants 1, fragment constants 2, fragment
state 3, fragment texture params 4, vertex buffers 5, conditional predicate 8, rasteriser env 9,
instancing table 10, instancing constants 11, textures from 12). `[V]` Under RSX-first this is not a
"descriptor layout" — it is **the RSX shader's argument register file**, re-expressed with
`setAddress`/`setTexture`/`setSamplerState`. The table snapshots at draw time, so it is safe to mutate
between draws (`09` §4). `[V]`

**Why it differs from the VK plan.** The VK-derived plan owns an entire descriptor-set/layout/pool
subsystem (`vkutils/descriptors.{h,cpp}` 785 LOC, `VKCommonPipelineLayout` 39, `pipeline_binding_table`
23) and a 3–4-difficulty translation (`03` #5/#17, `02` #6, R3). RSX has **no descriptor sets** —
that vocabulary is Vulkan's. RSX-first deletes the descriptor subsystem and the pipeline-layout object
outright and replaces them with a **single table keyed by an RSX-derived slot enum**. `[I]`

**The one real problem: constants.** RSX has *no push constants* — it has a constant-buffer region
(UBO-shaped). `vkCmdPushConstants` exists only because RPCS3 chose it for a 4-byte VS block and a
28-byte FS programmable-blend block (`03` #16, #87, R15). Under RSX-first those are **small entries in
the constant buffer** — sidestepping the `MTL4ArgumentTable` `setBytes` gap entirely. **Verified:**
metal-cpp `MTL4::ArgumentTable` exposes only `setAddress`/`setResource`/`setTexture`/`setSamplerState`
(`MTL4ArgumentTable.hpp:73-80`) and objc2-metal matches (no `setBytes`); Apple's cross-API table still
lists `[table setBytes:length:atIndex:]` for `vkCmdPushConstants` — a documentation gap `[U]` the
RSX-native design does not need to resolve. `[V]`/`[I]`

### 4.3 Pass / attachment model — hazard-derived boundaries, no `VkRenderPass`

**RSX requirement.** RSX has **surfaces and draws, not passes**. A draw targets a colour surface (or
several) plus a depth surface; RPCS3 decides when surfaces are loaded, cleared, resolved, unbound, and
reused. `[V]`

**Metal 4 shape.** Each encoder is created from a fresh `MTL4RenderPassDescriptor`; the *encoder is the
pass*. Attachment load/store state is set per attachment. Boundaries are derived from **RSX surface
events**, not from an attachment *identity key*: a new encoder is opened when a draw targets a
different surface set, or when a hazard cannot be satisfied intra-pass (§4.4). Where the next encoder
shares attachments, link it with `MTL4RenderEncoderOptionSuspending` / `Resuming` so the GPU merges
them into **one pass** (`WWDC25 254`). `[V]`

**Why it differs.** The VK plan rebuilds `VkRenderPass` + `VkFramebuffer` + a packed-`u64` render-pass
cache (`08` A21/A22/B12) — 710 LOC whose entire job describes a *Vulkan* object RSX never had. RSX-first
deletes them for a **surface-store-driven encoder scheduler** — the load-bearing change of this document,
and what makes TBDR-correct pass merging possible where the VK-shaped break at every `vk::renderpass_op`
boundary would destroy it (`09` §9). `[I]`

**Free win.** `MTLLogicalToPhysicalColorAttachmentMap` lets one encoder configure a superset of colour
attachments and remap shader outputs per pipeline (`WWDC25 254`) — a fit for RSX MRT remapping
(`README.md` #139), avoiding pass splits the VK plan cannot. `[V]`

### 4.4 Synchronization model — explicit barriers on existing surface-hazard tracking

**RSX requirement.** Order surface writes before reads (RTT→texture), order resolves, and order
programmable-blending feedback. RPCS3 already computes all of this: `surface_store` calls
`read_barrier` / `memory_barrier(cmd, access)` / `clear_rw_barrier` (`surface_store.h:229`, `:1114`,
`:1388`, `:1566`, `:1618`), and `rsx::thread` exposes `write_barrier` (`RSXThread.h:396`). `[V]`

**Metal 4 shape.** Metal 4 has **no driver hazard tracking** — every dependency is explicit
(`README.md` §4.1). Three barrier forms: `barrierAfterStages:beforeQueueStages:`,
`barrierAfterQueueStages:beforeStages:`, `barrierAfterEncoderStages:beforeEncoderStages:`, each with
an `MTL4VisibilityOption`. **The access-mask half of every RPCS3 Vulkan barrier is dropped; only the
stage half survives** (`01` §1.5). The RSX-native scheduler emits: intra-pass barriers where the stage
pair is legal, and **encoder/pass boundaries where the TBDR fragment constraint forbids an intra-pass
wait on fragment/tile stages** (`tbdr-architecture.md`, "TBDR hard constraints"). `[V]`

**Why it differs.** `02` #48–#55 ranks the barrier redesign as the single hardest item, because it
starts from `vkCmdPipelineBarrier` call sites and translates masks. RSX-first starts from **knowledge
RPCS3 already computes** (`surface_access`, cyclic refs, RW barriers) and asks only "does this
dependency cross a tile?" The TBDR constraint becomes a *scheduler rule* (feedback ⇒ split pass), not a
mapping failure. `[I]`

### 4.5 Draw & submission model — draw clauses → indirect / ICB batching

**RSX requirement.** `FIFO::flattening_helper::evaluate_performance` (`RSXFIFO.cpp:522`; thresholds
`>2000` `:532`, `≥500` `:549`) collapses the command stream; NV47 builds a `draw_clause` with
`draw_command_ranges` and barriers (`draw_call.hpp:19-86`); `pass_count()` (`:206`) and
`is_trivial_instanced_draw` (`:79`) already classify batcheable runs. `[V]`

**Metal 4 shape.**
- Default: `drawPrimitives` / `drawIndexedPrimitives` (index as a **GPU address + length argument**,
  `09` §3 #3).
- Trivial instancing: map `pass_count()` → `instanceCount` (`09` §3 #2/#7) — **free once the flag is
  set**.
- Batches: build `MTLDraw*IndirectArguments` (RPCS3 already builds `m_multidraw_parameters_buffer`,
  `VKDraw.cpp:1224`) and issue `draw*(indirectBuffer:)` (`09` §3 #5/#6).
- Conditional render: compute predicate-patch that zeros the indirect args when false (`09` §6.2) —
  the one genuinely GPU-driven need RSX has.
- Submission: one `MTL4CommandAllocator` per frame-in-flight; multiple CBs (draws on the RSX thread,
  transfer/compute on another) committed together; **drop the MoltenVK-style deep-copy submit packet**
  (`09` §5.2). `[V]`

**Why it differs.** The VK plan starts at `vkCmdDraw*` (`VKDraw.cpp:1176-1253`) and derives "batch =
multi-draw extension". RSX-first starts at the **flattener + draw-clause**, which already *are* the
batching layer. `09` §7.2's "a wash unless argument tables + indirect batching are exploited" is the
reason the RSX frame is right: the batching intelligence is upstream of the API and transfers unchanged. `[I]`

### 4.6 Shader / pipeline model — ucode → GLSL → SPIR-V → MSL

**RSX requirement.** RSX ucode is decompiled to GLSL by `Program/` (VP: `VKVertexProgram`, FP:
`VKFragmentProgram` — the *GLSL* is backend-agnostic; only the `Vk*` wrapper differs). Constants are
read from the constant-buffer region; interpolation, ROP, alpha-test, and programmable blending are
encoded in the GLSL. `[V]`

**Metal 4 shape.** Keep the front-end **unchanged**: `RSX ucode → GLSL → glslang → SPIR-V`. Insert
**SPIRV-Cross `CompilerMSL`** before `MTL4Compiler`. Metal 4 did **not** change the shader-binary
model: the argument table indexes the classic `[[buffer(N)]]`/`[[texture(N)]]`/`[[sampler(N)]]`
spaces, so SPIRV-Cross needs **no Metal-4 awareness**. A `MTL4Compiler` (one per device, KK W11)
compiles MSL → pipeline state; `MTL4PipelineDataSetSerializer` backs the binary archive (`06` §2.5).
`[V]`

**Recommendation: SPIRV-Cross**, not direct GLSL→MSL: it costs ~0 to reuse `Program/` (17,879 LOC),
is battle-tested (MVK, Godot), needs only a one-time binding remap (`msl_resource_binding`, spec
constants), is maintained as a pinned revision, and trades full emission control for far less
correctness risk. Add a **thin MSL post-pass** only for the three RSX-specific
rewrites the cross-compiler cannot know about: programmable blending → `[[color(N)]]` fetch, the
push-constant → constant-buffer substitution (§4.2), and the border-colour / LOD-bias shader
injection (`04` R18/R19). `[I]`

**Why it differs.** It does not — the VK plan is already RSX-shaped here (`06` §2.1; `03` §1.2). The
difference is that RSX-first makes explicit that the **descriptor/pipeline-layout wrapper**
(`VKProgramPipeline`, `VKCommonPipelineLayout`) is *not* part of the shader model (§4.2). `[I]`

### 4.7 Present / frame model

**RSX requirement.** `flip(display_flip_info_t)` (`RSXThread.h:372`) at end of frame;
`get_display_refresh_rate()` (`:155`). RSX output is SDR BGRA-class with a gamma/range calibration
pass; stereo is a rare option. `[V]`

**Metal 4 shape.** `CAMetalLayer` on the Qt `QWindow`'s `NSView` (attaching a layer if none,
`README.md` R22); one `MTL4CommandQueue`; `waitForDrawable` → encode → `signalDrawable` →
`drawable.present`. `maximumDrawableCount ≤ 3`. `CAMetalDisplayLink` / `NSScreen.maximumFramesPerSecond`
for refresh and pacing. Pipeline-cache namespace `"metal"` (`06` §2.3). `[V]`

**Why it differs from the VK plan.** The `VkSwapchainKHR` object **dissolves**: there is no swapchain
to recreate on resize (`05` R29). The frame ring becomes `#frames-in-flight` allocators + a
`MTLSharedEvent` value, not a fence/semaphore/timeline trio (`02` #44/#45). The residual hard parts —
`nextDrawable` blocking, `maximumDrawableCount ≤ 3`, Qt layer ownership, tri-state vsync — are
**unchanged** by the reorientation; they are genuine, not Vulkan artefacts. `[V]`

---

## 5. Delete / keep / reshape vs the Vulkan-derived plan

LOC deltas are estimates against the `08` §4–§6 row LOC. Dispositions: **delete** (no RSX need),
**keep** (RSX requirement survives), **reshape** (same need, different shape). `[I]` unless marked.

| Subsystem (08 rows) | VK-derived plan | RSX-native plan | Disposition | ΔLOC | Conf. |
|---|---|---|---|---|---|
| Render-pass + framebuffer objects + packed `u64` key (A21/A22/B12) | rebuild `VkRenderPass`/`VkFramebuffer` + cache | hazard-derived `MTL4RenderPassDescriptor` per encoder; no object | **delete** | −710 | high |
| Descriptor sets/layouts/pools (B9, A5, B11) | translate descriptor model | one `MTL4ArgumentTable` from RSX slot enum | **delete** | −847 | high |
| Push-constant machinery (part of A5/B9) | transient buffer / spec constants | constant-buffer entries (RSX has none) | **delete** | −150 | med |
| Instance/loader/portability/extensions (A1/A2/B1/B4) | Metal "instance" shim | `CreateSystemDefaultDevice` + `MetalFeatures` | **delete** | −1,600 | high |
| Device feature/limit/vendor negotiation (B2/B3) | translate `VkPhysicalDevice*` | one vendor, one family → small probe | **reshape** | −900 | med |
| VMA + second allocator + memory types (A26/B5 part) | two allocators, type search | placement heap, Shared-vs-Private | **reshape** | −800 | med |
| Barriers (B14) | translate `vkCmdPipelineBarrier` | hazard-driven stage barriers from `surface_store` | **reshape** | ~0 | med |
| Command pool/buffer (A23/B13) | translate pools + deep-copy submit | allocator-per-frame, batched commit | **reshape** | −300 | med |
| Sync (B15) fences/semaphores/events | translate all three | `MTLSharedEvent` + commit feedback | **reshape** | −400 | med |
| Texture cache traits (A16) | retarget traits | retarget traits over generic cache | **keep** (reshape traits) | ~0 | med |
| `VKTexture` upload/deswizzle/convert (A17) | port per-format logic | same logic, Metal storage/format table | **keep** | ~0 | med |
| Surface cache traits (A18) | retarget traits | retarget + hazard scheduler | **reshape** | +400 | low |
| Resolve helper (A19) | port resolve helpers | store-action resolve + meta for D/S | **reshape** | −200 | med |
| Query pool (A20/B26) | port `VkQueryPool` | visibility buffer + counter heap | **reshape** | −100 | med |
| Blit-engine VK plumbing (part of A17/A19/A15) | `vkCmdBlitImage`/copy call sites | compute/meta pass driven by NV3089/NV0039 semantics | **delete** (VK plumbing) | −400 | med |
| Async scheduler (A24) | port `VKAsyncScheduler` | batched CBs / second queue | **reshape** | −100 | low |
| Compute framework (A15) | port kernels | compute encoders; kernels to MSL | **keep** | ~0 | med |
| DMA (A14) | host-pointer fast path | CPU copy / shared buffers | **reshape** | −200 | low |
| Shader front-end (A7/A9/A10) | + SPIRV-Cross | + SPIRV-Cross (unchanged) | **keep** | ~0 | high |
| Pipelines (A8/B10) | translate pipeline objects | `MTL4RenderPipeline` + state hash | **keep** | ~0 | high |
| Vertex/draw (A12/A13) | port draw encoding | draw-clause → indirect batcher | **reshape** | +300 | med |
| Backend class (A32) | port 3.2k LOC spine | new spine (contract-driven) | **keep** (rewrite) | ~0 | high |
| Present/overlay (A30/B25/A29/B24) | port swapchain | `CAMetalLayer` + drawable ring | **reshape** | −300 | med |
| Upscalers (C1/C2) | port FSR passes | MetalFX spatial or compute pass | **reshape** | −200 | med |
| Overlays (A29) | port overlay pipeline | same geometry, MTL pipeline | **keep** | ~0 | med |

**Net: ~−5,000 to −7,000 LOC of deletions/compressions, offset by ~+700 LOC of RSX-native additions
(hazard scheduler + indirect batcher).** See §6 for the reconciled total. `[I]`

---

## 6. Revised effort estimate

### 6.1 Bottom-up

`08` §8.1: port surface 34,213 LOC → **~20,000–30,000 LOC new `MTL/`, centred ~25,000**; MVP
**8,000–12,000 LOC / ~6–12 person-months**. Applying §5's net deltas:

| | VK-derived (§5/`08`) | RSX-native (this doc) |
|---|---|---|
| Deletions unlocked | — | **~5–7k LOC** never written |
| RSX-native additions | — | **~0.7k LOC** (scheduler + batcher) |
| Net new `MTL/` | 20–30k (centre 25k) | **~18–25k (centre ~21k)** |
| MVP | 8–12k | **~7–10k** |

### 6.2 Honest verdict

**RSX-first mostly *reshapes*, reducing the number by roughly 15–20% — not by half.** Structurally, the
~55k LOC of shared emulation is untouched either way, and the *expensive* parts — texture-cache traits,
`MTLTexture` upload/deswizzle/convert, MSL correctness, the per-title quirk surface — are identical
under both frames (`08` §8.4 cost centres survive). What the reorientation removes is the **middle
scaffolding**: render-pass object model, descriptor model, loader/instance/extension layer, blit-engine
VK plumbing, and the push-constant workaround (§5) — real but bounded (~5–7k LOC), and mostly about
**schedule and shape**, not the two hard years of texture-cache and quirk work.

**Moves the number *down*:** (1) the render-pass layer is replaced by a scheduler that RPCS3's existing
hazard tracking already informs — converting a `diff-4/5` "needs design" item (`README.md` R1) into a
`diff-3` boundary rule; (2) descriptor/push-constant items (`R3`, `R15`) go **moot** (§9); (3) the
barrier redesign (`02` §5 rank 1) becomes an RSX dependency question, not a memory-mask translation.

**Moves the number *up*:** two genuinely new components — the surface-hazard encoder scheduler
(§4.3/§4.4) and the draw-clause indirect batcher (§4.5) — not scheduled as first-class by the VK plan. `[I]`

---

## 7. What the reorientation makes WORSE

Deliberately unsentimental.

1. **You throw away per-title knowledge encoded in the VK structure.** The render-pass cache, descriptor
   layout, and format/view rules are not *pure* Vulkan ceremony — they carry years of per-game fixes.
   `08` §10 flags this: if RSX quirks leak into the MTL traits, cost grows. Mitigation: keep the generic
   caches and `Program/` untouched; port *traits* file-by-file with the VK comments as a checklist.
2. **The render-pass layer coalesces RSX surface spam — it is not only a Vulkan workaround.** RSX
   rebinds surfaces rapidly; the existing pass logic groups draws that share attachments, which is
   *exactly* what keeps tile memory hot on TBDR. A naive hazard scheduler opens **more** encoders, each a
   store+load (`tbdr-architecture.md`). Mitigation: suspend/resume pass merging is mandatory from the
   first scheduler iteration; reusing one encoder while attachments are unchanged is the default.
3. **Under-scoping risk.** The 156-row inventory was a completeness guarantee; a *derived* design can
   silently miss a capability no RSX-side analysis surfaced. Mitigation: §8 walks the whole set by
   category; §10 keeps an explicit "resolve before committing" list.
4. **No local precedent.** KosmicKrisp is a *Vulkan* driver; Godot is Metal 3 (`03` §4 NR2). Mitigation:
   treat the second in-tree backend (`GL/`, 37 files / 17,064 LOC, same trait split) as the structural
   template, KK as the Metal-4 behaviour reference.
5. **Diffing against a known-good oracle gets harder.** The VK plan diffed construct-by-construct
   against VK; RSX-native has no such symmetry. Mitigation: `07` §2's oracle ladder must be stood up
   *before* the scheduler.
6. **The `backend_configuration` fill becomes a design decision.** Several fields go false/work (§2.2),
   *changing emulation behaviour* (flat shading→smooth; conditional render→CPU) — a visible-quality
   regression to own, not hide; document each as a caveat mirroring `VKGSRender.cpp:754`.

---

## 8. Completeness checklist: the 156 rows under the RSX frame

Summarised by category (the six `README.md` §3 sub-tables). Legend: **collapse** = not needed once the
Vulkan vocabulary is dropped; **required** = genuine RSX need; **reshape** = same need, different
implementation; **changed** = a row whose *verdict* differs from the VK frame (called out below).

| Category (`README.md` §3) | Rows | Collapse | Required | Reshape | Changed |
|---|---|---|---|---|---|
| 3.1 instance/device/adapter/features/limits | 14 | 6 | 2 | 6 | — |
| 3.2 memory/heaps/residency | 18 | 3 | 4 | 11 | — |
| 3.3 command/submission/sync/passes/queries | 39 | 9 | 13 | 17 | R7, R9 |
| 3.4 shaders/pipelines/descriptors/dyn-state | 35 | 12 | 14 | 9 | R3, R15 |
| 3.5 images/formats/samplers/RTT/copies | 34 | 2 | 22 | 10 | R14 |
| 3.6 presentation/window/swapchain | 16 | 1 | 9 | 6 | — |

**Collapse highlights** (subject is Vulkan-only): 2, 5 (enumeration); 3 (metal surface is a
`CAMetalLayer` shim, already present — `metal_layer.mm:9`); 7 (`chip_class`); 12, 52 (queue families);
32 (`VkAllocationCallbacks`); 36, 76 (inheritance/layout); 38 (descriptor sets); 57–62 (render pass,
load/store, subpass, framebuffer, image view — the *objects* vanish); 106 (tess stub); 118, 145 (layouts).

**Required highlights** (unavoidably real): 15–23 (allocation/buffers/textures), 28 (residency), 39–44
(drawable, commit), 73–75 (SPIRV-Cross + library), 78–79 (pipelines), 107–140 (texture/format/RTT/
sampler body), 141–156 (present, overlay, pacing).

**Reshape highlights**: 16 (allocators→heap), 48–55 (barriers→stage scheduler), 64–67 (queries→
visibility buffer), 86 (input attachment→framebuffer fetch), 119–128 (copies→compute encoder),
135–136 (format/usage table).

### 8.1 The disposition rule (for any future row)

> **If the construct exists because Vulkan (or the loader/MVK) requires an object RSX has no concept
> of — a pass, a subpass, a descriptor set, a layout, a queue-family, an instance, an extension —
> collapse it. If it expresses a genuine RSX requirement (a surface, a draw, a texture, a format, a
> hazard, a query, a present) — keep or reshape it. If it is a Metal-4 *mechanism* (argument table,
> residency set, barrier, encoder, allocator) — it is part of the target shape and gets sized, not
> mapped.**

### 8.2 Rows whose verdict changes under this frame

- **#38 descriptor sets / #76 pipeline layout:** "hard" → **collapse+delete** (the object never exists).
- **#87 push constants:** "impossible" → **moot** — RSX has none (constant-buffer entries).
- **#55 `ATTACHMENT_FEEDBACK_LOOP` / #86 input attachment:** "split the pass" → **reshape** (framebuffer
  fetch, Apple-preferred).
- **#57 `VkRenderPass` (R1):** "hard" → **reshape** — the encoder *is* the pass.
- **#83 `usamplerBuffer` (R14):** "impossible" → **reshape** — RSX already software-pulls vertices; only
  the texel-buffer binding remains (`09` §3 #9). Full cross-map: §9.

---

## 9. Register deltas (the 37-row register, `README.md` §6)

**Unchanged (framing-independent):** R2, R10, R11, R16, R18–R23, R25–R31, R34, R35, R37 — MSL
correctness, per-title quirks, exploded MSAA, border/LOD/texel views, drawable ring, `CAMetalLayer`
from Qt, pacing, blit/tiling, MetalFX, screenshots, savestate, stereo, sample mask, memory model,
pipeline cache. These are RSX requirements or Metal facts; the frame does not touch them.

**Become moot (the construct never appears).** R5 sparse binding (never used); R32 geometry/tess/TF/
pipeline-stats (VP/FP model cannot reach); R33 fp64/64-bit atomics (never emitted); R36
`VkAllocationCallbacks`/queue-family (no analog); R3 descriptor sets/layouts (no descriptors, §4.2); R15
push constants (constant-buffer entries, §4.2); R9 `DEPENDENCY_BY_REGION`/subpass self-deps (no subpasses);
R24 dynamic `LINE_WIDTH` (kept only as a clamp-to-1.0 loss, not a design item).

**Change verdict / severity.**

| Row | VK-frame verdict | RSX-frame verdict |
|---|---|---|
| **R1** render-pass → `MTL4RenderPassDescriptor` | hard (design) | **reshape** — encoder *is* the pass; the hardness was the Vulkan object. |
| **R6** conditional rendering | impossible (no command) | **reshape** — CPU skip for bring-up, compute predicate-patch for parity; RSX requirement survives. |
| **R7** stage-gated semaphore waits | impossible | **reshape** — RSX-first anchors sync on surface hazards; the stage-gated *wait* need not be reproduced at all. |
| **R8** binary semaphores / `VkEvent` objects | impossible | **reshape** — one monotonic `MTLSharedEvent` + commit feedback; object-level vocabulary collapses. |
| **R12** occlusion query model | hard | **reshape** — visibility buffer; the pass-spanning problem is a *scheduler* concern, not a mapping one. |
| **R13** async streaming topology | not-recommended to port | **reshape** — RSX has one serial FIFO; async is upload-only and can be batched CBs. |
| **R14** `usamplerBuffer` | impossible (bit-exact) | **reshape (reconfirmed)** — RSX already software-pulls; only the texel-buffer binding remains. |
| **R17** 1024-sampler cap | hard (intrinsic) | **likely moot** — the RSX-native binding model binds samplers via `setSamplerState` on the argument table, not argument-buffer accounting. `[U]` (needs the SDK spike). |
| **R4** host-pointer import / `passthrough_dma` | impossible for textures, constrained for buffers | **reshape** — RSX-first sets `supports_passthrough_dma=false` and uses CPU copy; the buffer path is a shared `MTLBuffer`. |

**New rows (not in the register).**
- **RN1 — surface-hazard encoder scheduler** (surface_store R/W/cyclic → encoder boundaries + barriers).
  `diff 4`; make-or-break of §4.3/§4.4. `[I]`
- **RN2 — draw-clause indirect batcher** (`MTLDraw*IndirectArguments` + predicate patch). `diff 3–4`;
  needed for the `09` §7.2 win. `[I]`
- **RN3 — constant-buffer substitution** for the VS/FS push-constant blocks; removes R15; must stay
  coherent with the `draw_parameters` SSBO lifetime. `diff 2`. `[I]`

---

## 10. Risks & open questions for the human

Prioritised. Each is a decision, not a task.

1. **The strategic gate: is a native backend worth it at all?** KosmicKrisp already runs directly on
   Metal 4 (`README.md` §4), and Dolphin's win over a *healthy* MoltenVK was **+23% / +30%** after
   isolating a 45% MVK regression — authors concluding "Metal is still better, but by only a little bit"
   and "MoltenVK is here to stay" (`[V]`). **For:** a native backend deletes a layer (no Vulkan loader;
   argument tables without descriptor translation) and RSX-first removes ~5–7k LOC of scaffolding.
   **Against:** RPCS3 is CPU-limited (`07` §8.2), the upside is unmeasured versus KK, and maintenance is
   a multi-person-year commitment. **Recommendation:** decide *before* any scheduler work; honest default
   is "research northstar, not next ship."
2. **The single most important architectural decision: commit to a hazard-derived pass scheduler
   (RSX-first) rather than a VK-style render-pass cache.** Everything in §4.3–§4.5 — pass merging,
   TBDR-correct boundaries, barrier placement — depends on it. If declined, §5's savings evaporate and
   the doc degenerates to the `08` port.
3. **Binding-model ceiling: one frame-wide argument table mutated per draw, or per-state tables?**
   `09` Q1. Strategy-C indirect batching (§4.5) needs a batch to share one table — a hard dependency of
   RN2.
4. **Does the target SDK's `MTL4::ArgumentTable` actually lack `setBytes`?** `[U]` — metal-cpp and
   objc2-metal lack it (`MTL4ArgumentTable.hpp:73-80`); Apple's cross-API table lists it. RSX-first makes
   this **non-blocking** (constant buffers).
5. **Does `nextDrawable` blocking / `maximumDrawableCount ≤ 3` force a frame-ring redesign?** `R21`.
   Unchanged by the reorientation; still needs a spike.
6. **`supports_last_provoking_vertex` = false** (no public mode; MVK gates it on private API): which
   titles regress under smooth-interpolation fallback? `[V]`/`[I]`
7. **How much of `A16`/`A17`/`A18` is backend-specific vs absorbed by the generic caches?** `08` §10
   item 1 — a compile of `rsx::surface_store`/`rsx::texture_cache` against a stub `MTLTraits` was never
   attempted. `[U]` This spike decides whether RN1 is `diff 4` or `diff 5`.
8. **Conditional-render first cut: CPU skip or GPU predicate-patch from day one?** `09` Q2.
9. **Does `setSamplerState` bypass the 1024 argument-buffer sampler cap?** `[U]` (R17); if yes, drop
   sampler eviction.

---

## 11. Sources

### `[V]` Primary — this tree (symbols stable; line refs advisory; paths under `rpcs3/Emu/RSX/`)

- `RSXThread.h` — `backend_configuration` `:80-98` (fields `:82-97`), member `:123`, accessors
  `:230`/`:489`; base `:107`; pure virtuals `:155`/`:337`/`:370`/`:372`; hooks `:348-458`;
  `write_barrier` `:396`. `GSRender.h:20`; `RSXZCULL.h:157`,`:212-216`; `Core/RSXDisplay.h:50-68`.
- `VK/` — `VKGSRender.cpp:746-909` (capability fill; `:754` warning), `VKGSRender.h:38`,`:265-269`,`:289`;
  `VKRenderTargets.h:159`,`:747`; `VKTextureCache.h:21-32`,`:412`; `VKProgramBuffer.h:9`,`:72`;
  `vkutils/pipeline_binding_table.h` (12 slots); `vkutils/metal_layer.mm:9`; `vkutils/sampler.cpp:10-38`;
  `vkutils/vk_driver_selection.h:801-805` (`kosmickrisp_min_macos_major = 27`); `VKDraw.cpp` (draw
  dispatch, `vk::renderpass_op`, programmable-blend barrier, `m_multidraw_parameters_buffer`);
  `VKVertexBuffers.cpp` (software vertex pulling).
- `Common/surface_store.h:22`,`:229`,`:1114`,`:1388`,`:1566`,`:1618`; `Common/texture_cache.h:22`.
- `NV47/FW/draw_call.hpp:19-86`,`:166`,`:206`; `NV47/FW/draw_call.inc.h`;
  `NV47/HW/{nv3089,nv0039,nv308a,nv406e,nv4097}.cpp`.
- `RSXFIFO.cpp:522-575` (flattener thresholds); `RSXThread.cpp:299-420` (programmable blending),
  `:2877-2912` (conditional render), `:3534` (flattener feed).
- `gcm_enums.h:619` (line width), `:700` (border colour), `:1253-1265` (primitives), `:1297-1309`
  (depth formats), `:1347-1353` (AA), `:1364-1380` (colour formats).
- `GL/GLGSRender.cpp:50-63`,`:197`,`:224` (second-backend capability fill); `GL/GLTextureCache.h:21-34`;
  GL tree 37 files / 17,064 LOC (measured).
- `docs/research/metal4/{README,01..09}.md` — adopted ground truth for contract, register, effort,
  build and verification constraints (each carries its own §8/§11 `file:line` set).

### `[V]` Primary — reference checkouts under `/tmp/metal4-refs/`

- metal-cpp `Metal/MTL4ArgumentTable.hpp:73-80` (only `setAddress`/`setResource`/`setTexture`/
  `setSamplerState`; **no `setBytes`**). MoltenVK `MoltenVK/GPUObjects/MVKPipeline.mm:843-844`,
  `MVKDevice.mm:782-783` (`provokingVertexLast` gated on `useMetalPrivateAPI`).
- Mesa KosmicKrisp `docs/drivers/kosmickrisp/workarounds.rst` (W11 one-`MTL4Compiler`-per-device heap
  corruption; W1/W2 crashes; W3 ballot; W4/W5 discard), `docs/drivers/kosmickrisp.rst` (macOS 26+ /
  Apple silicon), `src/kosmickrisp/vulkan/kk_cmd_meta.c` (`vk_meta_blit_image2`), `kk_cmd_draw.c`
  (indirect draw, predicate patch — per `09` §11).

### `[V]` Primary — fetched over HTTP

- Apple GPTK "Cross-API Translation" — <https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/translating-to-metal4-api/references/cross-api-translation.md>
  — residency mandatory for argument-table-bound resources; `[table setBytes:length:atIndex:]` listed for
  `vkCmdPushConstants`; "prefer programmable blending over splitting"; "collapse consecutive render
  encoders when attachment configurations match". "TBDR Architecture" — <https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/translating-to-metal4-api/references/tbdr-architecture.md>
  — TBDR fragment constraint; per-pass store/load cost model; memoryless; programmable blending.
- docs.rs `objc2-metal` `MTL4ArgumentTable.rs` — <https://docs.rs/objc2-metal/latest/aarch64-apple-visionos/src/objc2_metal/generated/MTL4ArgumentTable.rs.html>
  — limits (`maxBufferBindCount ≤ 31`, `maxTextureBindCount ≤ 128`, `maxSamplerStateBindCount ≤ 16`);
  protocol has no `setBytes`.
- Dolphin Progress Report Jul–Aug 2022 — <https://dolphin-emu.org/blog/2022/09/13/dolphin-progress-report-july-and-august-2022/>
  — +23%/+30%; 45% MoltenVK regression; "Metal is still better, but by only a little bit"; "MoltenVK is
  here to stay".

### `[V]` Primary — Apple documentation and WWDC (fetched or returned by search)

- Understanding the Metal 4 core API <https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api>;
  `MTL4ArgumentTable` <https://developer.apple.com/documentation/metal/mtl4argumenttable>;
  `MTL4RenderPassDescriptor` <https://developer.apple.com/documentation/metal/mtl4renderpassdescriptor>.
- `MTLLogicalToPhysicalColorAttachmentMap` <https://developer.apple.com/documentation/metal/mtllogicaltophysicalcolorattachmentmap>;
  `MTLResidencySet` <https://developer.apple.com/documentation/metal/mtlresidencyset>; residency sets
  <https://developer.apple.com/documentation/metal/simplifying-gpu-resource-management-with-residency-sets>;
  argument buffers <https://developer.apple.com/documentation/metal/improving-cpu-performance-by-using-argument-buffers>.
- Barriers <https://developer.apple.com/documentation/metal/mtl4commandencoder/barrierafterstages:beforequeuestages:visibilityoptions:>;
  producer <https://developer.apple.com/documentation/metal/synchronizing-passes-with-producer-barriers>,
  consumer <https://developer.apple.com/documentation/metal/synchronizing-passes-with-consumer-barriers>.
- WWDC25 205 <https://developer.apple.com/videos/play/wwdc2025/205/>; WWDC25 254
  <https://developer.apple.com/videos/play/wwdc2025/254/> (suspend/resume pass merging, colour-attachment
  map, parallel encoding, allocator-per-frame); WWDC20 10602 <https://developer.apple.com/videos/play/wwdc2020/10602/>
  (programmable blending, tile shading).
- MSL Specification <https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf>; Feature
  Set Tables <https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf>.

### `[I]` Secondary / context

- metalbyexample "Getting Started with Metal 4" <https://metalbyexample.com/metal-4/> (reusable command
  buffers; suspend/resume). blakecrosley "Metal 4 Essentials" <https://blakecrosley.com/blog/metal-4-essentials>
  (all resources untracked; explicit barriers). Godot internal rendering architecture
  <https://docs.godotengine.org/en/4.5/engine_details/architecture/internal_rendering_architecture.html>
  (Metal backend, SPIRV-Cross, MoltenVK fallback). SPIRV-Cross `spirv_msl.hpp`
  <https://github.com/KhronosGroup/SPIRV-Cross/blob/main/spirv_msl.hpp> (`argument_buffers`,
  `ArgumentBuffersTier`, `msl_version`). MoltenVK `README.md`
  <https://github.com/KhronosGroup/MoltenVK/blob/main/README.md> (deprecation/non-compliance).
