# 9. The draw path: encoding, batching, indirect commands & per-draw CPU cost

Owner/scope: Scope 9 (cross-cutting) | Status: draft | Date: 2026-10-15

**Verification legend used throughout:**
- `[V]` = verified this session by reading a file in-repo, a pristine reference checkout under `/tmp/metal4-refs/`, or an immutable raw source fetched over HTTP (URL listed in §11).
- `[I]` = inference / reasoned conclusion from `[V]` facts — not directly stated by any source.
- `[U]` = unverified / could not be confirmed (Apple HTML docs are JS-rendered; some pages returned only summaries).

Repo is strictly read-only for this task; only this file was created. No build was run. No file under `rpcs3/**` was touched. Another agent is editing `rpcs3/Emu/RSX/VK/**` concurrently, so **line numbers may drift — symbol names are the stable reference**.

---

## 1. Scope & boundary with scopes 02/03

**This document owns the draw-call path itself:** one RSX draw (a *draw clause*) → the sequence of Metal 4 encoder calls that issue it, the CPU cost of that sequence, and every mechanism for issuing *fewer/cheaper* encoder calls (native instancing, multi-draw, indirect batches, GPU-driven command generation). It also owns command-buffer *construction* at the encoder level — allocators, command-buffer reuse and where encoding can run on parallel threads.

**Boundary with scope 02 (submission/sync).** Scope 02 owns the queue: `MTL4CommandQueue`, `commit`, semaphores/fences/barriers, the render-pass and framebuffer model at pass granularity, the occlusion query pool, and swapchain present. This doc stops at `MTL4RenderCommandEncoder`; it consumes 02's decisions and does **not** re-derive them. §5 (allocators/commit grouping) and §6 (counter heaps) intentionally overlap 02 and are written to stay consistent with `02-submission-sync.md` rows 1–6 and §1.7 — cross-references marked inline.

**Boundary with scope 03 (shaders/pipelines/descriptors).** Scope 03 owns the argument-table *model*, pipeline objects and the push-constant story. This doc consumes `setArgumentTable`, `setRenderPipelineState` and the argument-table layout, and focuses only on **how often and in what order** those are re-emitted per draw, plus how batching interacts with the fact that `MTL4ArgumentTable` has **no `setBytes`** (03 row 16) and that RPCS3's vertex path is already **software vertex pulling** via `usamplerBuffer` (03 §1.6; `RSXVertexFetch.glsl:75`). The dynamic-state re-emission (`update_draw_state` → viewport/scissor/blend/stencil) is 03's; this doc notes it as a per-draw cost but does not re-table it.

**Out of scope / not attempted:** no source, CMake, test or CI changes; no build; no on-hardware measurement (see §7 for how to measure).

---

## 2. RPCS3's draw path, traced from source

### 2.1 The chain, end to end

1. **FIFO flattening (draw-call reduction).** `FIFO::flattening_helper` collapses the raw RSX command stream (mostly `BEGIN_END` + ignorable register writes) before draw clauses reach the backend. `evaluate_performance(u32 total_draw_count)` — `rpcs3/Emu/RSX/RSXFIFO.cpp:522` — decides whether flattening stays on: `if (total_draw_count <= 2000)` ⇒ "low pressure", flattening disabled (`RSXFIFO.cpp:532`); if `num_collapsed < 500` it is disabled (`RSXFIFO.cpp:549`); `ensure(total_draw_count > 2000)` once enabled (`:569`). `num_collapsed` accumulates in the handler (`:657`). `[V]` **This is RPCS3's own, API-agnostic batching pass** — it produces fewer, larger draw clauses; the Metal backend inherits it unchanged.
2. **Draw clause construction.** NV47 handlers (`rpcs3/Emu/RSX/NV47/HW/nv4097.cpp:421-443`) build a `draw_clause` (`rsx::draw_clause`, `rpcs3/Emu/RSX/NV47/FW/draw_call.hpp`) containing `draw_command_ranges`, `draw_command_barriers`, and classification flags. `[V]`
3. **Draw-clause classification** (all in `draw_call.hpp`): `is_single_draw()` (`:177`), `pass_count()` (`:206`), `get_subranges()` (`:311`), the barrier array `draw_command_barriers` (`:25`), `is_trivial_instanced_draw` set from `check_trivially_instanced()` (`:69`,`:95`), `is_immediate_draw` / `inline_vertex_array` for push-register geometry (`:75`,`:81`), and `min_index()` (`:166`). `[V]`
4. **Vertex/index upload + topology emulation.** `VKGSRender::upload_vertex_data()` — `rpcs3/Emu/RSX/VK/VKVertexBuffers.cpp:218` — runs `draw_command_visitor` (`:88`): `get_appropriate_topology` maps RSX topology to a VK topology and reports whether the primitive is emulated (`:98`,`:121`); index emulation for quads/fans/etc. goes through `emulate_as_indexed` (`:70`) and `write_index_array_data_to_buffer` (`:156`); primitive restart is emulated by doubling the upload (`:131`,`:135`). It returns `vertex_upload_info { native_primitive_type, vertex_draw_count, vertex_count, vertex_base, index_base, vertex_index_offset, persistent_range_base, volatile_range_base, index_info }` (`:371-379`). **The vertex streams are software-pulled**: the source data lives in `m_persistent_attribute_storage` / `m_volatile_attribute_storage`, which are texel-buffer *views* (`VK_FORMAT_R8_UINT`) over the attribute ring, re-created in 64 MB blocks (`:351-353`, `:362-366`). `[V]`
5. **Per-draw encoding.** `VKGSRender::emit_geometry(u32 sub_index)` — `VKDraw.cpp:992-1256`; called from `VKGSRender::end()` (`VKDraw.cpp:1408`). See §2.2.
6. **Per-frame counter.** `m_frame_stats.draw_calls++` in `rsx::thread::end()` — `RSXThread.cpp:979`; the struct is `frame_statistics_t` (`Core/RSXDisplay.h:50-68`, fields `draw_calls`, `submit_count`, `setup_time`, `vertex_upload_time`, `textures_upload_time`, `draw_exec_time`, `flip_time`, …). The FIFO flattener is fed this counter once per frame: `m_flattener.evaluate_performance(m_frame_stats.draw_calls)` — `RSXThread.cpp:3534`. It is surfaced in the on-screen stats as `"draw calls"`, `"draw call setup"`, `"vertex upload time"`, `"draw call execution"` — `VKPresent.cpp:950-965`. `[V]`

### 2.2 `emit_geometry`, the exact per-draw sequence

`emit_geometry` is called once per RSX subdraw. The hot path (`VKDraw.cpp:1087-1255`):

- **Per-subdraw bookkeeping / first-subdraw setup.** `if (m_current_draw.subdraw_id == 0)` allocates a stream-layout buffer sized `pass_count() * 168` (`:1091-1097`). `update_vertex_env(sub_index, upload_info)` (`:1101`; impl `VKGSRender.cpp:2384`). `[V]`
- **Descriptor churn.** `m_program->bind_uniform(persistent_buffer, …, vertex_buffers_location)` and `+1` for volatile (`:1103-1107`) — the software-pulled stream table. `[V]`
- **Render-pass / framebuffer transition.** `vk::renderpass_op(...)` (`:1110-1128`): if the pass or FBO differs it ends the old renderpass and sets `reload_state`, which forces `update_draw_state()` + `begin_render_pass()` (`:1155-1158`). Pass boundaries are driven by RT/FBO identity, not by draw count. `[V]`
- **Programmable blending.** When `RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING` is set, an intra-pass image barrier is inserted per MRT (`insert_image_memory_barrier`, `:1130-1149`) — i.e. a barrier *between draws inside one pass* for input-attachment ROP read-back. `[V]`
- **Pipeline + descriptor bind.** `m_program->bind(*m_current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS)` (`:1153`). A code comment explicitly notes "FIXME: We only need to rebind the pipeline when reload state is set" — i.e. today it rebinds every subdraw. On Vulkan this is cheap because the bind is behind the two-entry state cache in `vkutils/commands.cpp:149-196` (`vkCmdBindPipeline`/`vkCmdBindDescriptorSets`; `bind_descriptor_sets` asserts `num_sets <= 2`). `[V]`
- **Conditional rendering.** Host-gated: `_vkCmdBeginConditionalRenderingEXT(buffer = m_cond_render_buffer)` on reload (`:1160-1170`); the predicate buffer is built in `VKGSRender.cpp` (`:3039-3195`, scratch fallback `:2313`). `[V]`
- **Occlusion begin.** `m_occlusion_query_manager->allocate_query(...)` / `begin_query(...)` (`:1051-1084`), gated on `current_fragment_program.ctrl & RSX_SHADER_CONTROL_...`. `[V]`

### 2.3 The draw dispatch itself (`VKDraw.cpp:1176-1253`)

| Condition | Non-indexed (`!upload_info.index_info`) | Indexed |
|---|---|---|
| `is_trivial_instanced_draw` | `vkCmdDraw(count, pass_count(), 0, 0)` `:1180` | `vkCmdDrawIndexed(count, pass_count(), 0,0,0)` `:1212` |
| `is_single_draw()` | `vkCmdDraw(count, 1, 0, 0)` `:1184` | `vkCmdDrawIndexed(count, 1, 0,0,0)` `:1216` |
| HW multi-draw (`get_multidraw_support()`) | `_vkCmdDrawMultiEXT(subranges, …, sizeof(rsx::draw_range_t))` `:1190` | `_vkCmdDrawMultiIndexedEXT(subranges_count, …, sizeof(VkMultiDrawIndexedInfoEXT))` `:1240` |
| fallback | loop `vkCmdDraw(range.count, 1, vertex_offset, 0)` `:1196-1200` | loop `vkCmdDrawIndexed(...)` `:1246-1251` |

Indexed draws also `vkCmdBindIndexBuffer(m_index_buffer_ring_info.heap, offset, index_type)` first (`:1208`). The indexed multi-draw path builds a host `m_multidraw_parameters_buffer` of `VkMultiDrawIndexedInfoEXT` (`:1224-1239`) — direct evidence RPCS3 already hoists per-subrange arguments into a host buffer for batch submission. `[V]`

HW multi-draw capability: `multidraw_support.supported` from `VkPhysicalDeviceMultiDrawFeaturesEXT::multiDraw`, `max_batch_size` default 65536 (`vkutils/device.cpp:110`), refreshed from `maxMultiDrawCount` (`:231`); disabled if too small (`:236`). `descriptor_max_draw_calls` default plus 8192 floor (`device.h:86`, `device.cpp:225`) bounds descriptor-pool pressure per frame. `[V]`

### 2.4 Per-frame draw-count evidence

- **Verified:** the counter exists, is per RSX draw scope, is sampled every frame and drives the flattener (`RSXThread.cpp:979`, `:3534`; `RSXDisplay.h:52`). `[V]`
- **Verified:** the only in-tree quantitative statements about magnitude are the flattener thresholds — RPCS3 treats `> 2000` raw commands/frame as the point where flattening matters, and requires `≥ 500` collapses to keep it (`RSXFIFO.cpp:532`,`:549`,`:569`). `[V]`
- **Unverified:** an absolute per-game draw-call number (e.g. "game X does N draws/frame"). No such figure is hardcoded in-tree and none was measured here. Any figure quoted in a port plan must come from running with the overlay (`VKPresent.cpp:950`) on target titles. `[U]`

**Cost model that follows.** Per RSX draw RPCS3 currently emits: 0–1 `begin_render_pass`, 1 pipeline bind + 1 descriptor-set bind (cached, but re-issued per subdraw), 0–N `bind_uniform` table mutations, and 1–K `vkCmdDraw*`. The Metal port's job is to make that last row cheapest.

---

## 3. Vulkan → Metal 4 draw mapping table

Columns: `# | Vulkan / RSX construct (exact API or symbol) | Metal 4 equivalent | Metal 4 objects/methods | Difficulty (1-5) | Notes`.
Difficulty: 1 trivial rename · 2 mechanical · 3 real work but well-trodden · 4 hard/needs design · 5 unknown/impossible. Methods verified in `/tmp/metal4-refs/metal-cpp/Metal/MTL4RenderCommandEncoder.hpp`.

| # | Vulkan / RSX construct (exact API or symbol) | Metal 4 equivalent | Metal 4 objects/methods | Difficulty (1-5) | Notes |
|---|---|---|---|---|---|
| 1 | `vkCmdDraw(cmd, count,1,0,0)` (`VKDraw.cpp:1184`) | direct draw | `Metal 4 RenderCommandEncoder::drawPrimitives(primitiveType,vertexStart,vertexCount)` | 1 | `metal-cpp:76`. `vertexStart` = `upload_info.vertex_base` (`VKVertexBuffers.cpp:378`). `[V]` |
| 2 | `vkCmdDraw(..., pass_count())` (`:1180`) | native instancing | `drawPrimitives(...,vertexCount,instanceCount)` | 1 | `metal-cpp:77`. RSX "trivial instanced" maps 1:1. `[V]` |
| 3 | `vkCmdDrawIndexed(...)` (`:1216`) + `vkCmdBindIndexBuffer` (`:1208`) | indexed draw, **index as GPU address+length arg** | `drawIndexedPrimitives(primitiveType,indexCount,indexType,indexBuffer,indexBufferLength)` | 2 | `metal-cpp:66`. No bound index-buffer state; the address is an argument. Metal requires ≥2/4-byte alignment and a matching `MTL::IndexType`. `[V]` |
| 4 | `vkCmdDrawIndexed(...,baseVertex,baseInstance)` | same params | `drawIndexedPrimitives(...,instanceCount,baseVertex:baseInstance:)` | 1 | `metal-cpp:68`. `baseVertex` usable because RSX performs index emulation host-side. `[V]` |
| 5 | `_vkCmdDrawMultiEXT(subranges, sizeof(rsx::draw_range_t))` (`:1190`) | **no native multi-draw** → indirect or loop | `drawPrimitives(primitiveType, indirectBuffer: GPUAddress)` (`:79`) from a host-built `MTLDrawPrimitivesIndirectArguments` array; else N× `drawPrimitives` | 3 | KosmicKrisp sets precedent: it fakes multi-draw with indirect (`kk_cmd_draw.c:1464-1480`). `[V]` |
| 6 | `_vkCmdDrawMultiIndexedEXT(...)` (`:1240`) + `m_multidraw_parameters_buffer` (`:1224`) | **no native multi-draw** → indirect or loop | `drawIndexedPrimitives(primitiveType, indexType, indexBuffer, indexBufferLength, indirectBuffer:)` (`metal-cpp:69`) | 3 | Arg layout = `MTLDrawIndexedPrimitivesIndirectArguments` (indexCount/instanceCount/firstIndex/vertexOffset/firstInstance). Host already builds the array. `[V]` |
| 7 | `draw_clause::is_trivial_instanced_draw` + `pass_count()` (`draw_call.hpp:79`,`:206`) | one instanced draw | see #2/#4 | 1 | — `[V]` |
| 8 | `draw_command_visitor` index/topology emulation, `emulate_as_indexed` (`VKVertexBuffers.cpp:70`,`:121`) | unchanged; output index GPU buffer + `MTL::IndexType` | `drawIndexedPrimitives(...,indexType,...)` | 2 | Fan/quads/restart already host-side; nothing Metal-specific. `[V]` |
| 9 | Software-pulled vertex streams: `m_persistent_attribute_storage` / `m_volatile_attribute_storage` texel views (`VKVertexBuffers.cpp:353`,`:366`) | argument-table buffer slots; stream fetched via `texture_buffer` in MSL | `ArgumentTable::setAddress(gpuAddress, idx)` | **4** | Same core hard problem as scope 03 row 12 (no `usamplerBuffer`). **No vertex-buffer bind stage exists on either side** — that is a structural simplification for the Metal port. `[V]` |
| 10 | `m_program->bind(...)` pipeline (`VKDraw.cpp:1153`) | set pipeline | `MTL4RenderCommandEncoder::setRenderPipelineState(MTL::RenderPipelineState)` | 1 | — `[V]` |
| 11 | `m_program->bind(...)` descriptor sets; `m_program->bind_uniform` (`:1103`,`:1153`) | set argument table + bind | `setArgumentTable(table, stages)` (`metal-cpp:84`), `ArgumentTable::setAddress/setTexture/setSamplerState` | 3 | Table snapshots at draw time ⇒ safe to mutate between draws (WWDC25 254). `[V]` |
| 12 | Render-pass boundary on RT/FBO change: `vk::renderpass_op` (`:1110`), `begin_render_pass` (`:1158`), `vk::end_renderpass` (`:1121`) | new encoder / pass merge | `MTL4CommandBuffer::makeRenderCommandEncoder(descriptor:options:)` + `MTL4RenderEncoderOptionSuspending/Resuming`, `endEncoding()` | 4 | Suspend/Resume merges encoders into one GPU pass (WWDC25 254) → avoids TBDR store/load; but RPCS3 must then keep one logical pass across command buffers. `[V]`/`[I]` |
| 13 | Programmable-blending intra-pass `insert_image_memory_barrier` (`:1130-1149`) | framebuffer fetch + encoder barrier | MSL `[[color(n)]]` + `MTL4CommandEncoder::barrier(afterStages:beforeStages:)` | **4** | No subpasses; read-back = tile framebuffer fetch (03 row 15). Barrier must order prior draw's color writes before next draw's fetch. `[V]`/`[I]` |
| 14 | `_vkCmdBeginConditionalRenderingEXT` (`:1168`), predicate `m_cond_render_buffer` | **no Metal conditional render** → indirect patching | compute kernel patches `MTLDraw*IndirectArguments`; then `draw*(indirectBuffer:)` | **4** | KosmicKrisp's model (see §6). RSX hw-cond maps to a predicate compare; patch args to zero when false. `[V]` |
| 15 | Occlusion begin/end query (`:1051-1077`; scope 02 §1.7) | visibility result buffer | `MTL4RenderPassDescriptor.visibilityResultBuffer` + `setVisibilityResultMode(mode, offset)` | 2 | Scope 02 owns pool modelling; KK `:1435-1437`. `[V]` |
| 16 | `draw_clause::is_immediate_draw` / `inline_vertex_array` (`draw_call.hpp:75`,`:81`) | unchanged; materialise stream then draw | as #1/#3 | 2 | Inline array is already copied into the attribute ring host-side. `[V]` |
| 17 | `get_appropriate_topology` (`VKVertexBuffers.cpp:98`) | `MTL::PrimitiveType` | `drawPrimitives(primitiveType,…)` | 2 | Triangle-fan has no Metal topology → emulate as indexed (KK & MoltenVK both do). `[V]` |
| 18 | `draw_command_barriers` / primitive-restart barrier (`draw_call.hpp:25`,`:113`) | host-side index emulation (no GPU construct) | — | 2 | Barriers are already resolved on the CPU into index runs. `[V]` |
| 19 | `multidraw_support.max_batch_size` 65536 (`device.cpp:110`) | chunk indirect array | split `set of MTLDraw*IndirectArguments` into batches | 2 | Metal indirect draw has no explicit per-call count cap; chunk for cache/arg-buffer sizing. `[V]`/`[I]` |
| 20 | `update_draw_state()` dynamic state (`:1157`) | encoder setters | per scope 03 rows 24–28 | 2 | Owned by 03; noted here as a per-draw emit cost. `[V]` |
| 21 | Push constants: `vkCmdPushConstants` (VS 4B / FS 28B) | **no `setBytes`** → transient buffer | `ArgumentTable::setAddress` to `MTL::Buffer` | **4** | 03 row 16; also KK writes per-draw data to argument-table slot 2 (`kk_cmd_draw.c:1914`). `[V]` |
| 22 | MT `vkCmdDraw` from `draw_parameters` SSBO index (`VKVertexProgram.cpp:87`,`:131`) | **stays a buffer** — enabler for indirect batching | shader reads `draw_parameters`; table set once | 3 | The single most important affordance: per-draw data is *not* in bind slots. `[V]`/`[I]` |

---

## 4. Batching strategies and their trade-offs

The design question is not "can we batch" but "what does the argument table force". On Metal 4 the encoder snapshots the argument table **at draw time** (WWDC25 254; Apple docs), so two consecutive draws can share one table *only if every resource they read is either unchanged or addressed through a buffer the shader indexes itself*. RPCS3 already satisfies the latter for vertex data (`draw_parameters[draw_parameters_offset]`, `VKVertexProgram.cpp:87`) and for software-pulled streams. **That is the structural reason a batch is feasible at all here.** `[V]`/`[I]`

| Strategy | What it removes | What it costs / risks | Realistic for RSX? |
|---|---|---|---|
| **A. Per-draw direct** (`drawIndexedPrimitives` × N; the metal-4-basics baseline, `Metal4Renderer.swift:407-444`) | nothing | 1 encoder entry + 1 argument-table mutation per draw; simple, correct | Yes — default, ship first |
| **B. Native instancing** (map `pass_count()` → `instanceCount`, `is_trivial_instanced_draw`) | N−1 encoder entries for trivial subdraws | none; already detected by `check_trivially_instanced()` | Yes — cheap, immediate win |
| **C. Host-built indirect batch** (`drawIndexedPrimitives(indirectBuffer:)`) — KosmicKrisp's `kk_convert_to_indirect_draw` (`kk_cmd_draw.c:1464-1480`) | N−1 encoder entries; one entry issues a whole subrange set | requires per-subrange args in a GPU buffer (RPCS3 already builds `m_multidraw_parameters_buffer`); requires the whole set to share pipeline + argument table | **Yes** — best fit; subranges already share state by construction |
| **D. Indirect command buffer (ICB)** (`MTL::IndirectCommandBuffer` + `executeCommandsInBuffer`, `metal-cpp:81`) | encoder re-entry for reused/static command sets; enables GPU-side generation | ICB commands carry `setRenderPipelineState`/`setVertexBuffer`/`setFragmentBuffer` but **no `setArgumentTable`** (`MTLIndirectCommandEncoder.hpp:11-81`) → cannot express MTL4 argument-table binds; created from `MTL::Device` (Metal 3 object); GPU access only via an argument buffer | **Marginal** — only pays off with GPU-driven generation, which RSX has no analogue for |
| **E. Conditional-render predicate patch** (compute writes/zeroes indirect args, KK `kk_cmd_draw.c:1508-1531`) | host round-trip for `hw_cond`; keeps one entry point | needs a compute dispatch before the draw batch; ordering/barrier | Yes — required to emulate RSX conditional render at all |
| **F. State-keyed batching** (don't re-emit identical pipeline/table/dynamic state across consecutive draws) | redundant `setRenderPipelineState`/`setArgumentTable`/viewport calls | needs a small "last emitted" cache; correctness risk on missed state | Yes — mirrors the existing VK two-entry cache (`vkutils/commands.cpp:149-196`) |

**Recommended first-cut order:** B → F → C → E → (D only if a GPU-driven need appears). Note D's blocker is real and verified: `MTLIndirectRenderCommand` exposes `setRenderPipelineState`, `setVertexBuffer:offset:atIndex:`, `setFragmentBuffer:...` and index-buffer setters, but **no argument-table method** (`MTLIndirectCommandEncoder.hpp:11-81`; Apple `MTLIndirectRenderCommand`). How binds resolve inside an ICB under Metal 4 is `[U]`; it is not a viable MTL4 argument-table path today.

**The ID/descriptor subtlety.** Strategy C only works when consecutive subranges share the argument table. RPCS3's per-subdraw texture rebinds (`VKProgramPipeline.cpp` `bind_uniform`, `VKDraw.cpp:698-819`) break that. So C batches **vertex-only / same-material** runs; mixed-material runs fall back to A. This is exactly the granularity the `draw_clause` subrange machinery already produces, so the host side needs no new partitioning logic. `[I]`

---

## 5. Command-buffer construction & parallel encoding

### 5.1 Metal 4 primitives (verified)

- `MTL4CommandAllocator` — owns the memory backing command encoding. `allocatedSize()` to observe growth, `reset()` to reuse after the GPU finishes. **Allocators are not thread-safe** → one per encoding thread; the canonical pattern is one allocator per frame-in-flight (`metal-4-basics/Metal4Renderer.swift:74`,`:331`; WWDC25 254; `MTL4CommandAllocator` docs). `[V]`
- `MTL4CommandBuffer` — requested from the **device**, decoupled from the queue, **reusable**, and **unretained** (referenced resources must outlive it). `beginCommandBuffer(allocator:)` / `endCommandBuffer()`; render encoders via `makeRenderCommandEncoder(descriptor:options:)`. `[V]`
- **Parallel encoding + grouped commit:** "begin multiple command buffers on multiple threads, each with a different command allocator … submit them all with a single commit call" (WWDC25 254). `MTL4CommandQueue::commit` takes an array of command buffers. `[V]`
- **Pass merging:** `MTL4RenderEncoderOptionSuspending` / `Resuming` merge multiple render encoders (possibly in different command buffers) into **one GPU pass**; commit all buffers in one call (WWDC25 254). `[V]`

### 5.2 Where RPCS3 maps — and where it must diverge from `VKAsyncScheduler`

Today RPCS3 records one primary command buffer for the graphics queue, submits under a global mutex, and (optionally) a secondary transfer-queue submit chained by semaphore — `close_and_submit_command_buffer()` (`VKGSRender.cpp:2510-2614`; scope 02 §1.2). Async texture streaming runs on a separate thread via `VKAsyncScheduler` / `vk::AsyncTaskScheduler` with its own command pool and event/semaphore pool (`VKAsyncScheduler.h:8-61`; init at `VKGSRender.cpp:882`; flush at `:2590-2603`). `[V]`

**Where the Metal design should diverging from porting `VKAsyncScheduler` (scope 02 row 41 explicitly says "re-think, not port"):**

1. **Match parallelism to the real dependency graph, not to the queue count.** RSX draw state is a single serial FIFO — *per-draw* encoding cannot be split across threads. Only **pass/framebuffer-independent** work (texture upload/streaming, resolve/desync compute, mip-gen) is parallelisable. So: main RSX thread encodes the frame's draw command buffers; a second encoder thread handles transfer/compute buffers with its **own allocator** and they are committed together in one `commit([...])`. `[I]`
2. **Replace "one command pool + fence per submit"** (scope 02 §1.1) with **one allocator per frame-in-flight**, reset at frame end — this is the Metal 4 idiom, not the VK idiom. `[V]`/`[I]`
3. **Exploit suspend/resume instead of queue-level barriers.** RPCS3 already splits work at RT/FBO boundaries (`vk::renderpass_op`, `VKDraw.cpp:1110`). Encoding those splits into separate command buffers is only free if they are linked with `Resuming`/`Suspending` so TBDR store/load at the boundary is avoided; otherwise the split is a pessimisation. This is the concrete reason not to copy the VK `renderpass_op` break points naively. `[I]`
4. **Drop the MTRSX deep-copy submit packet machinery** (`VKCommandStream.h:15-49`, scope 02 §1.2): `MTL4CommandBuffer` is decoupled from the queue and can be committed from any thread, so there is nothing to deep-copy. This is a *simplification* the explicit model buys. `[I]`

**Caution (verified):** allocators must not be reset while their GPU work is in flight, and command buffers are unretained — so every resource referenced (buffers, textures, argument tables) must stay alive until completion. RPCS3's ring buffers (scope 02 §1.8) already have per-frame lifetime; the argument tables and transient push-constant buffers from scope 03 must join that lifetime model.

---

## 6. Counters, queries and GPU-driven options

### 6.1 Queries

- **Occlusion** — RPCS3 has a `VkQueryPool` of `VK_QUERY_TYPE_OCCLUSION` behind `VKQueryPool.h` / `vkutils/query_pool.hpp` (scope 02 §1.7). Metal equivalent is a **visibility result buffer** referenced from `MTL4RenderPassDescriptor` plus `setVisibilityResultMode(mode, offset)` on the encoder; KosmicKrisp uses exactly this (`kk_cmd_draw.c:1435-1437`). No counter heap is involved. Scope 02 owns the pool/slot modelling and the "close renderpass before reading" TBDR rule. `[V]`
- **Timestamps** — RPCS3 has **no** timestamp queries (scope 02 §1.7: no `VK_QUERY_TYPE_TIMESTAMP`, no `vkCmdWriteTimestamp`). Metal 4 adds `MTL4CounterHeap` (`MTL4CounterHeapTypeTimestamp`) + `writeTimestamp(granularity:after:counterHeap:index:)` on the render encoder / `writeTimestamp(counterHeap:index:)` on the command buffer, resolved to a buffer by `MTL4CommandBuffer::resolveCounterHeap` (and `invalidateCounterRange` for CPU reads). This is a **new capability**, useful for per-pass GPU timing; it is a scope-02 concern (heap lifetime, resolve fencing) that this scope only *requests*. `[V]`
- **Counter ranges** must be invalidated before CPU read unless resolved on the GPU timeline (`invalidateCounterRange` docs). `[V]`

### 6.2 GPU-driven

- **ICB path:** `executeCommandsInBuffer(...)` exists on `MTL4RenderCommandEncoder` (`metal-cpp:81-82`), including the indirect *range* variant (`executeCommands(buffer:indirectBuffer:)`) so the GPU can choose how many commands run. Pipelines opt in via `setSupportIndirectCommandBuffers` (`MTL4RenderPipeline.hpp:195`). The blocker remains §4-D: no argument table inside an ICB. `[V]`
- **Indirect draw path (no ICB):** `drawPrimitives(indirectBuffer:)` / `drawIndexedPrimitives(...indirectBuffer:)` are already GPU-driven in the sense that the *arguments* may be produced by compute. This is the realistic GPU-driven surface for RSX. `[V]`
- **What GPU-driven is actually *for* in this port:** not culling (RSX has no scene graph to cull). It is **conditional rendering**: KosmicKrisp converts predicates into an indirect draw and lets a compute kernel zero the args when the predicate is false (`kk_cmd_draw.c:1508-1531`, `libkk_predicate_indirect_*`), with an explicit TODO to move to device-generated commands later. RPCS3's `hw_cond` (`VKDraw.cpp:1160`) is the direct analogue, and it is the strongest candidate for the GPU-driven machinery. `[V]`

---

## 7. Is Metal 4's explicit encoding a win or a loss vs MoltenVK?

### 7.1 The two models

- **MoltenVK (deferred):** every `vkCmd*` records a pooled `MVKCommand` object; the command buffer is *encoded at commit time* by a visitor (`MVKCommandEncoder::encode`/`encodeCommandsImpl`, `MVKCommandBuffer.mm`). Secondary command buffers and `vkCmdExecuteCommands` are re-encoded **inline** into the same `MTLRenderCommandEncoder` (host-side indirection, no GPU parallelism). Prefill timing is configurable (`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` 0–3: prefill nothing vs at `vkEndCommandBuffer` vs per-command). MoltenVK also does redundant-state reduction and CPU-side triangle-fan→indexed conversion (`MVKCmdDraw.mm`). `[V]`
- **Metal 4 (explicit):** commands are written **directly** into encoder memory; there is no per-command pooled object and no deferred visitor; per-draw resource binding goes through **argument tables** instead of individually-tracked bind points (WWDC25 205: "as draw call counts increased … the impact of managing bind points per draw added to the CPU time"); allocators give direct memory control; command buffers are independent and parallel-encodable; multiple can be committed in one call. `[V]`

### 7.2 Verdict

**Net win — conditional on exploiting argument tables and indirect batching. A naive 1:1 port of the VK draw path would be roughly a wash.**

Win drivers: `[I]` (built on `[V]` facts above)
1. **Argument tables remove the per-bind driver state capture** that WWDC25 205 names as the CPU cost that grows with draw count. RPCS3's per-subdraw `bind_uniform` churn (`VKDraw.cpp:698-819`, `:1103`) is precisely this cost; with a table snapshot semantics it becomes cheap and safe to mutate between draws.
2. **No deferred command objects.** MoltenVK allocates and later walks one object per Vulkan command per frame; MTL4 encodes inline. Fewer allocations, no visitor indirection.
3. **Explicit allocator + grouped commit + parallel encoding + suspend/resume** give control MoltenVK's model cannot expose.

Loss/risk drivers: `[I]`
1. **No `setBytes`** → push constants need transient buffers (03 row 16). *Not a regression* — MoltenVK has the same Metal limitation.
2. **No native multi-draw** → emulation required. *Not a regression* — MoltenVK loops indirect draws too.
3. **More explicit lifetime management** (allocators, unretained command buffers, residency sets) replaces MoltenVK's hidden bookkeeping. This is the main way a careless port ends up *slower and buggier*.
4. **Metal 4 is young.** KosmicKrisp itself still fakes multi-draw and conditional render and leaves ICB/device-generated commands as a TODO (`kk_cmd_draw.c:1508`), which is a warning about how much is on the port to build.

### 7.3 How to measure this (concretely, using existing hooks)

- RPCS3 already times exactly the right things: `m_frame_stats.setup_time` (descriptor/state setup, `VKDraw.cpp:1174`) and `draw_exec_time` (draw dispatch, `:1255`), reported as "draw call setup" / "draw call execution" alongside "draw calls" in the overlay (`VKPresent.cpp:950-956`). Port these counters unchanged; the *ratio* update-time-to-draw-time per frame is the headline metric. `[V]`
- `MTL4CommandAllocator::allocatedSize()` as a proxy for encoding volume. `[V]`
- **Metal System Trace / Instruments** for per-encoder CPU and GPU timeline; GPU-side per-pass time via `MTL4CounterHeap` timestamps (§6.1) once scope 02 wires them. `[V]`
- **A/B protocol:** run the same titles through (a) the existing MoltenVK backend, (b) Metal 4 per-draw (strategy A/B/F), (c) Metal 4 indirect-batched (strategy C). The claim "explicit is a win" is only supported if (b)/(c) beat (a) on CPU-frame-time at equal GPU work, ideally on draw-call-heavy scenes (the ones the flattener flags at `> 2000`/frame, `RSXFIFO.cpp:532`). `[I]`

---

## 8. Effort/risk table

Difficulty from the 1–5 scale. Effort = relative implementation size. Risk = chance of subtle correctness/perf failure.

| Item | Difficulty | Effort | Risk | Notes |
|---|---|---|---|---|
| Direct + indexed draws (`drawPrimitives`/`drawIndexedPrimitives`) | 1–2 | S | Low | Core mapping §3 #1–#4. `[V]` |
| Native instancing from `pass_count()` | 1 | S | Low | §3 #7. `[V]` |
| Index buffer as GPU address+length + `MTL::IndexType` | 2 | S | Low | Host emulation already done (`VKVertexBuffers.cpp`). `[V]` |
| Software-pulled vertex streams in argument table | 4 | L | **High** | Shares scope 03 row 12 (no `usamplerBuffer`); texel-buffer emulation. `[V]` |
| State-keyed batching (skip redundant emits) | 2 | M | Medium | Mirror `vkutils/commands.cpp:149-196`; missed-state bugs. `[I]` |
| Host-built indirect batch (`draw*(indirectBuffer:)`) | 3 | M | Medium | §3 #5–#6; arg layout must match `MTLDraw*IndirectArguments`. `[V]` |
| Conditional render via compute arg-patching | 4 | L | **High** | §3 #14; ordering/barrier + predicate semantics; new compute pass. `[V]` |
| Render-pass merge with suspend/resume | 4 | L | High | §3 #12; interacts with scope 02 pass model; must not regress TBDR. `[I]` |
| Programmable-blend framebuffer-fetch + encoder barrier | 4 | M | High | §3 #13; correctness of input-attachment read-back. `[V]`/`[I]` |
| Transient push-constant buffers (no `setBytes`) | 4 | M | Medium | §3 #21; ring-buffer lifetime with unretained CB. `[V]` |
| Command allocator / CB lifetime per frame-in-flight | 2 | M | Medium | §5.1; reset-after-completion rule. `[V]` |
| Parallel encoding of transfer/compute buffers | 3 | M | Medium | §5.2; one allocator per thread. `[V]` |
| Occlusion via visibility result buffer | 2 | S | Low | §6.1; scope 02 owns pool. `[V]` |
| Counter-heap timestamps | 2 | S | Low | §6.1; new capability, scope 02 lifetime. `[V]` |
| ICB / `executeCommandsInBuffer` for draws | 5 | L | **High** | §4-D: no argument table inside an ICB → likely a dead end for MTL4 binds. `[V]`/`[U]` |

---

## 9. Impossible / not-recommended

- **`setBytes`-style push constants.** No such method exists on `MTL4ArgumentTable` (03 row 16); the only path is a transient buffer + `setAddress`. Impossible via the API. `[V]`
- **A single call that issues N draws *and* carries per-draw argument-table binds.** ICBs carry no argument table (`MTLIndirectCommandEncoder.hpp`); indirect draws carry only draw args. Any "one call, N fully-different draws" design is not expressible. Not recommended to pursue. `[V]`
- **Parallelising per-draw encoding within a pass.** RSX state is a serial FIFO; the encoder is stateful and order-sensitive. Only independent command buffers may encode in parallel (§5.2). Not recommended. `[I]`
- **GPU-driven culling / compute-generated draws as a first milestone.** No RSX scene-graph analogue; KosmicKrisp leaves it as a TODO. Defer; use GPU-driven only for the conditional-render predicate (§6.2). `[V]`
- **Naively breaking the pass at every `vk::renderpass_op` boundary** (`VKDraw.cpp:1110`) without suspend/resume. On TBDR each boundary is a store+load of all attachments (Apple game-porting TBDR notes; WWDC25 254). Not recommended. `[V]`
- **Triangle-fan / primitive-restart native topologies.** Already emulated host-side; nothing to gain. `[V]`
- **Wide lines / polygon-mode-point** — needs private API (scope 03 row 28); out of this scope. `[V]`

---

## 10. Open questions for the human

1. **Binding model ceiling:** do we commit to a *single frame-wide argument table* mutated per draw (bindless), or per-state tables? Everything in §4 hinges on this; a per-draw table swap would kill strategy C.
2. **Uniformity vs fast path:** do we model *every* RSX draw as an indirect draw for uniformity (KK-style), or keep direct `drawPrimitives`/`drawIndexedPrimitives` on the single/instanced fast path and only go indirect for subrange batches? The latter is likely faster; the former is simpler.
3. **Conditional rendering:** is adopting a compute predicate-patch pass acceptable, or must the first milestone keep host-side predicate evaluation (skip the draw on the CPU)? The `[U]` GPU-path details (barrier placement, predicate semantics parity with `VK_EXT_conditional_rendering`) need a spike.
4. **Frame model:** does the Metal port keep one-commit-per-frame (matching `close_and_submit_command_buffer`), or move to per-frame allocators with grouped commit and possibly multiple command buffers? This changes scope 02's design as much as scope 09's.
5. **Shading contract:** may we extend the RSX shader contract with extra per-draw indirection (beyond the existing `draw_parameters` SSBO, `VKVertexProgram.cpp:131`) to make indirect batches share one argument table? This touches scope 03 and is a hard dependency of strategy C.
6. **Profiling:** do we require `MTL4CounterHeap` timestamp instrumentation in milestone 1 (new capability, scope 02), or is the existing CPU `setup_time`/`draw_exec_time` overlay (`VKPresent.cpp:950`) sufficient to judge the win/loss in §7?
7. **ICB research budget:** is it worth a spike to determine how ICB binds resolve under Metal 4 (currently `[U]`), or do we declare ICBs out of scope for draws and revisit only if a GPU-driven need appears?

---

## 11. Sources

**In-repo (read-only, `[V]`):** `rpcs3/Emu/RSX/VK/VKDraw.cpp` (§2.2–2.3); `rpcs3/Emu/RSX/VK/VKVertexBuffers.cpp` (§2.1); `rpcs3/Emu/RSX/NV47/FW/draw_call.hpp`; `rpcs3/Emu/RSX/RSXFIFO.cpp`; `rpcs3/Emu/RSX/Core/RSXDisplay.h`; `rpcs3/Emu/RSX/RSXThread.cpp`; `rpcs3/Emu/RSX/VK/VKPresent.cpp`; `rpcs3/Emu/RSX/VK/vkutils/device.h` / `device.cpp`; `rpcs3/Emu/RSX/VK/VKGSRender.cpp` / `VKGSRender.h`; `rpcs3/Emu/RSX/VK/VKProgramPipeline.cpp`; `rpcs3/Emu/RSX/VK/vkutils/commands.cpp`; sibling scopes `docs/research/metal4/02-submission-sync.md`, `03-shaders-pipelines-descriptors.md`.

**Reference checkouts under `/tmp/metal4-refs/` (`[V]`):**
- Mesa KosmicKrisp: `mesa/src/kosmickrisp/vulkan/kk_cmd_draw.c` (`kk_flush_gfx_state` `:1442`, `kk_convert_to_indirect_draw` `:1464`, predicate patch `:1508-1531`, `kk_dispatch_draw` `:1672`, `kk_draw` `:2057`).
- metal-cpp: `Metal/MTL4RenderCommandEncoder.hpp` (`:66-84`), `Metal/MTL4ArgumentTable.hpp`, `Metal/MTLIndirectCommandEncoder.hpp`, `Metal/MTLIndirectCommandBuffer.hpp`, `Metal/MTL4Counters.hpp`, `Metal/MTL4RenderPipeline.hpp`.
- MoltenVK: `MoltenVK/Commands/MVKCmdDraw.mm`, `MVKCommandBuffer.mm`.
- Godot: `godot/drivers/metal/metal3_objects.cpp` (`render_draw_indexed` `:1483`, indirect loops `:1517`).
- `metal-4-basics/Metal4Basics/Sources/Engine/Metal4Renderer.swift` (allocators `:74`,`:331`; per-draw table mutation + `drawIndexedPrimitives` `:407-444`; `commit([cb])` `:453`).

**Web (`[V]` for the quoted content):**
1. https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api
2. https://developer.apple.com/documentation/metal/mtl4commandallocator
3. https://developer.apple.com/documentation/metal/mtl4argumenttable
4. https://developer.apple.com/documentation/metal/mtl4rendercommandencoder
5. https://developer.apple.com/videos/play/wwdc2025/205 (Discover Metal 4)
6. https://developer.apple.com/videos/play/wwdc2025/254 (Explore Metal 4 games — allocators, parallel encoding, suspend/resume, color-attachment map)
7. https://developer.apple.com/documentation/metal/mtlindirectcommandbuffer
8. https://developer.apple.com/documentation/metal/mtlindirectrendercommand
9. https://developer.apple.com/documentation/metal/mtlindirectcommandbufferdescriptor
10. https://developer.apple.com/documentation/metal/indirect-command-encoding
11. https://developer.apple.com/documentation/metal/improving-cpu-performance-by-using-argument-buffers
12. https://developer.apple.com/documentation/metal/mtl4commandbuffer
13. https://developer.apple.com/documentation/metal/mtl4rendercommandencoder/writetimestamp(granularity:after:counterheap:index:)
14. https://developer.apple.com/documentation/metal/mtl4commandbuffer/writetimestamp(counterheap:index:)
15. https://docs.rs/objc2-metal/latest/aarch64-apple-visionos/src/objc2_metal/generated/MTL4Counters.rs.html (`MTL4CounterHeapType`, `MTL4TimestampHeapEntry`)
16. https://docs.rs/objc2-metal/0.3.2/x86_64-apple-darwin/src/objc2_metal/generated/MTLIndirectCommandEncoder.rs.html (no argument-table method)
17. https://docs.rs/objc2-metal/0.3.2/aarch64-apple-visionos/src/objc2_metal/generated/MTL4ArgumentTable.rs.html (`maxBufferBindCount` ≤ 31, `maxTextureBindCount` ≤ 128)
18. https://github.com/bkaradzic/metal-cpp/blob/metal-cpp_27/Metal/MTL4ArgumentTable.hpp
19. https://github.com/tmc/apple/blob/main/metal/mtl4_render_command_encoder_protocol.gen.go (draw + argument-table selectors)
20. https://github.com/livingston/apple-docs/blob/main/documentation/Metal/MTL4ComputeCommandEncoder.md (`writeTimestamp`, unified encoder)
21. https://learn.microsoft.com/en-us/dotnet/api/metal.imtl4argumenttable.setaddress (Metal `IMTL4ArgumentTable.SetAddress` signature)
22. https://github.com/apple/game-porting-toolkit/blob/main/game-porting-skills/skills/translating-to-metal4-api/references/tbdr-architecture.md (TBDR pass-boundary cost model)
23. https://skia.googlesource.com/external/github.com/KhronosGroup/MoltenVK/+/refs/heads/mesh-shader/Docs/Whats_New.md (redundant-state reduction, triangle-fan)
24. https://github.com/KhronosGroup/MoltenVK (deferred command model, private-API note)
25. https://android.googlesource.com/platform/external/moltenvk/+/refs/heads/main/Docs/MoltenVK_Configuration_Parameters.md (`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` 0–3)
26. https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawMultiEXT.html and .../VK_EXT_multi_draw.html (multi-draw = "avoid repeated state checks")
27. https://developer.apple.com/documentation/metal/improving-rendering-performance-with-vertex-amplification (vertex amplification)
28. https://developer.apple.com/videos/play/wwdc2023/10125 (Bring your game to Mac pt.3 — ICBs are a superset of indirect draw args; encode `executeCommandsInBuffer`)
29. https://developer.apple.com/documentation/metal/mtlrendercommandencoder (classic render encoder, `setBytes` exists on Metal 3 but not on MTL4 argument tables)
30. https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/VK/VKPresent.cpp (upstream stats overlay strings)
