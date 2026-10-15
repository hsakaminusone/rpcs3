# HLD-04 — Command encoding: recording, argument binding, submission

Cluster coverage: `draw`, `vertex`, `texture`, `blend`, `raster`, `surface` (the clusters whose state
is consumed *at draw time* by the command stream). This document owns the shape of the recording and
submission path — how the RSX FIFO is turned into `MTL4::CommandBuffer`s, how a draw binds state, how
pipelines are selected and created, how multi-draw/inline-array batches are issued, and how the
recorder hands control to the barrier machinery of `HLD-03`. It is **design only**; no file under
`rpcs3/**` is modified.

---

## 1. Purpose & non-goals

### 1.1 Purpose

Specify the Metal 4 command-encoding contract for the RSX-first backend:

1. **The record/submit pipeline.** `MTL4::CommandAllocator` → `MTL4::CommandBuffer` →
   `MTL4::RenderCommandEncoder` / `MTL4::ComputeCommandEncoder` → `MTL4::CommandQueue::commit`, and
   where the recorder sits relative to the RSX/FIFO thread.
2. **Argument binding.** The `MTL4::ArgumentTable` lifecycle and the RSX
   `pipeline_binding_table` → argument-table index mapping, aligned with `HLD-05` §3.2.
3. **Pipeline state.** Which pipeline objects exist (`MTL::RenderPipelineState`,
   `MTL::ComputePipelineState`, mesh pipelines), how they are created asynchronously
   (`MTL4::Compiler` + cache), and the mesh-shader decision.
4. **Indirect command buffers / multi-draw.** Whether RSX inline-array and multi-range draws can
   batch into `MTL::IndirectCommandBuffer`s, and the fallback actually chosen.
5. **Draws and vertex fetch.** The concrete `MTL4::RenderCommandEncoder` draw methods, the RSX draw
   model, and the vertex-fetch decision (software pulling, no `MTLVertexDescriptor`).
6. **The hand-off to `HLD-03`.** The exact seam between "record a draw" and "insert a barrier / open
   a pass".

### 1.2 Non-goals

| Out of scope | Owned by |
|---|---|
| Barrier derivation, pass-split policy, event/fence objects, TBDR fragment constraint | `HLD-03` |
| Guest memory → buffer/texture, tiling/swizzle, residency (`MTLResidencySet`), format table | `HLD-02` |
| CgBinary → MSL translation, the shader facet surface, PSO *keying*, the canonical slot map's provenance | `HLD-05` |
| Flip, `CAMetalLayer`, drawable presentation, frame pacing | `HLD-06` |
| Per-cluster register decode tables and dispositions | `LLD-draw`, `LLD-vertex`, `LLD-texture`, `LLD-blend`, `LLD-raster`, `LLD-surface` |
| Implementation, headers, or build changes | — (design only) |

---

## 2. Model (shape change)

### 2.1 RSX vs Metal 4: what changes shape

RSX is a **FIFO of register writes and draw triggers** consumed by a single `rsx::thread`; state is
implicit (the last-written value in `rsx_state`), and a draw is a trigger that snapshots whatever is
current (`RSXThread.h:254` `m_graphics_state`; `RSXThread.h:357` `run_FIFO`). Metal 4 is an
**explicit encoder stream**: every state change, bind, and draw is an ordered method call on an open
encoder, and the encoder is the only state carrier.

| RSX concept | Metal 4 concept | Shape change | Conf. |
|---|---|---|---|
| FIFO register stream, `methods[reg]` dispatch (`RSXFIFO.cpp:875-931`) | ordered encoder method calls | state is pushed at encode time, not read from a register file at trigger time | `[V]` |
| draw trigger = "flush current state" | `drawPrimitives` / `drawIndexedPrimitives` on an open encoder | a draw is a method, not a state transition | `[V]` |
| implicit hazard ordering (guest timeline) | explicit `barrier*` + pass boundaries | ordering must be *emitted*; the driver does not infer it | `[V]` |
| one big mutable register file | per-encoder dynamic state + per-stage `MTL4::ArgumentTable` | the mutable indirection moves into argument tables | `[V]` |
| `SET_BEGIN_END` begin/end draw state machine (`LLD-draw` §2.3) | start/stop of a draw-emitting region | the machine becomes a recorder region, not a register pair | `[I]` |

### 2.2 The record/submit pipeline

`MTL4::CommandBuffer` holds **no strong references** and is reusable; it does not track hazards
(`README.md` §4; `HLD-01` §3.3). The verified sequence:

1. **Allocate.** One `MTL4::CommandAllocator` per encoding thread / frame-in-flight via
   `Device::newCommandAllocator()` (`MTLDevice.hpp:416`). `MTL4::CommandAllocator` exposes only
   `reset()` / `allocatedSize()` / `label()` (`MTL4CommandAllocator.hpp:51,57`) and is **not
   thread-safe** — one per encoding thread (`HLD-01` §4.7). `Device::newCommandBuffer()`
   (`MTLDevice.hpp:419`) creates the reusable buffer.
2. **Begin.** `CommandBuffer::beginCommandBuffer(allocator:)` (`MTL4CommandBuffer.hpp:64`), or the
   options form `beginCommandBuffer(allocator:options:)` (`:65`; `CommandBufferOptions`
   `MTL4CommandBuffer.hpp:51`).
3. **Encode.** `renderCommandEncoder(descriptor:options:)` (`MTL4CommandBuffer.hpp:82`) — this is the
   **verified factory**; there is no Metal-3-style `makeRenderCommandEncoder` on the MTL4 command
   buffer. The no-options overload is `renderCommandEncoder(descriptor:)` (`:81`). Compute work uses
   `computeCommandEncoder()` (`:67`).
4. **End.** `endCommandBuffer()` (`:71`).
5. **Submit.** `MTL4::CommandQueue::commit(commandBuffers:count:)`
   (`MTL4CommandQueue.hpp:126`) or `commit(commandBuffers:count:options:)` (`:127`). Completion is
   observed through `MTL4::CommitOptions::addFeedbackHandler` (`MTL4CommandQueue.hpp:98`) →
   `MTL4::CommitFeedback` (`MTL4CommitFeedback.hpp:38`; `GPUStartTime`/`GPUEndTime`/`error`).

`MTL4::CommandQueue` is created with `Device::newMTL4CommandQueue()` (`MTLDevice.hpp:477`) — **not**
the Metal-3 `MTLCommandQueue`. The queue also carries `signalEvent`/`wait` (`MTL4CommandQueue.hpp:142,148`),
`signalDrawable` (`:140`), and residency attach (`addResidencySet`, `:123`).

### 2.3 Recorder placement relative to the RSX/FIFO thread

The recorder (`mtl::command_recorder`, component **C4** of `HLD-01` §2.2) is a **synchronous
consumer of the flattened FIFO**. `RSXThread` declares the thread topology and the backend's
capabilities through `backend_configuration` (`RSXThread.h:80-98`, member `:123`, accessor `:230`);
`GSRender : rsx::thread` (`GSRender.h:20`) supplies the architecture-neutral lifecycle. Under
RSX-first:

- The FIFO is flattened by the `flattening_helper` (`RSXFIFO.h:63-127`) before the backend sees draw
  clauses; thresholds `>2000`/`≥500` (`RSXFIFO.cpp:522-579`). The recorder therefore never re-reads
  raw FIFO — it consumes `draw_clause`s.
- **Encoding stays on the FIFO/RSX thread** in the first cut, mirroring the VK backend's single
  `close_and_submit_command_buffer` (`VKGSRender.cpp:2510`). This keeps the one-allocator-per-thread
  invariant trivially true and avoids a second synchronization surface.
- A second encoding thread is *not* precluded by the API (`MTL4::CommandAllocator` is per-thread),
  but it would need its own allocator, its own argument tables, and a cross-thread command-buffer
  handoff. It is an open question (§6 Q9).

### 2.4 Shape changes that constrain everything else

| Property (verified) | Consequence for the encoder | Conf. |
|---|---|---|
| `MTL4::CommandBuffer` has no strong refs and is reusable (`HLD-01` §3.3) | every bound resource, argument table, and transient must outlive the matching `MTL4CommitFeedback` | `[V]` |
| No driver hazard tracking; barriers are explicit (`HLD-03` §2; `MTL4CommandEncoder.hpp:50-54`) | correctness cannot cite residency or "the driver serializes it" | `[V]` |
| **No blit encoder** — copies live on the compute encoder (`MTL4ComputeCommandEncoder.hpp:67-93`) | `surface`/`texture` copies, fills, and mip generation are compute-encoder commands | `[V]` |
| `MTL4::ArgumentTable` has **no `setBytes`** (`MTL4ArgumentTable.hpp:73-80`) | constants are always memory-backed (`setAddress`) | `[V]`/`[U]` |
| No native multi-draw | multi-range draws lower to indirect argument buffers or a loop (§3.5) | `[V]` |
| No conditional-render command (`README` R6) | conditional draws are CPU-skipped or predicate-patched (§3.6) | `[V]` |

---

## 3. Interfaces / data structures

### 3.1 The command recorder (C4)

`mtl::command_recorder` is the single chokepoint through which the whole frame's decisions are
recorded (`HLD-01` §2.2, §3.3; `11` §9). It owns:

| Member | Type | Created from | Lifetime |
|---|---|---|---|
| `m_allocator` | `MTL4::CommandAllocator*` | `Device::newCommandAllocator()` (`MTLDevice.hpp:416`) | frame slot; `reset()` after feedback |
| `m_cmd` | `MTL4::CommandBuffer*` | `Device::newCommandBuffer()` (`MTLDevice.hpp:419`) | frame slot; reusable |
| `m_encoder` | `MTL4::CommandEncoder*` (render or compute) | `renderCommandEncoder(descriptor:options:)` / `computeCommandEncoder()` | one open at a time |
| `m_tables[stage]` | `MTL4::ArgumentTable*` | `Device::newArgumentTable(descriptor:error:)` (`MTLDevice.hpp:407`) | frame ring |
| `m_decision_trace` | `std::vector<decision_record>` | local | frame; the H2/H3 seam |

The recorder exposes exactly one way to open a pass and one way to emit a barrier, so that every
boundary is diffable (`HLD-01` §4.2; `HLD-03` §3.4):

| Call | Effect |
|---|---|
| `mtl::command_recorder::open_pass(const pass_plan&)` | `renderCommandEncoder(descriptor:options:)`; the descriptor comes from `HLD-03`'s plan; `options` carries `RenderEncoderOptionSuspending`/`Resuming` for merge |
| `mtl::command_recorder::open_compute()` | `computeCommandEncoder()` for copy/fill/mip/predicate-patch work |
| `mtl::command_recorder::close_encoder()` | `endEncoding()` (`MTL4CommandEncoder.hpp:58`) exactly once |
| `mtl::command_recorder::emit(barrier_spec)` | forwards to `mtl::emit_barrier` (`HLD-03` §3.4) on the open encoder |
| `mtl::command_recorder::bind_draw_state(const draw_state&)` | sets pipeline state, argument table(s), dynamic state |
| `mtl::command_recorder::draw(const draw_clause&, const draw_range_t&)` | issues the draw(s) |

### 3.2 Frame ring and allocator discipline

| Type | Contents | Reset point | Conf. |
|---|---|---|---|
| `mtl::frame_ring` | N slots of `{allocator, command buffer, argument tables, feedback completion}` | slot reused only after its `MTL4CommitFeedback` fires | `[I]` |
| `mtl::completion` | `MTL4::CommitOptions::addFeedbackHandler` (`MTL4CommandQueue.hpp:98`) → `MTL4::CommitFeedback` (`MTL4CommitFeedback.hpp:38`) | — | `[V]` |

One allocator per encoding thread/frame-in-flight; `reset()` never while its GPU work is in flight
(`HLD-01` §4.7). Frame count is bounded by the allocator ring and `maximumDrawableCount ≤ 3`
(`HLD-03` §4.9; **R21**).

### 3.3 Argument binding

**Creation.** `Device::newArgumentTable(descriptor:error:)` (`MTLDevice.hpp:407`) from an
`MTL4::ArgumentTableDescriptor` (`MTL4ArgumentTable.hpp:37`). The descriptor fixes the table's shape
and must be sized to the fixed RSX map (`HLD-05` §3.2):

| Descriptor field | Purpose | Value implied by the RSX map | Conf. |
|---|---|---|---|
| `setMaxBufferBindCount(_:)` (`:57`) | buffer-slot count | at least 16 (slots 0–11 + substituted) | `[I]` |
| `setMaxTextureBindCount(_:)` (`:59`) | texture-slot count | base 12 + up to 16 FS textures | `[I]` |
| `setMaxSamplerStateBindCount(_:)` (`:61`) | sampler-slot count | matches texture count | `[I]` |
| `setInitializeBindings(_:)` (`:53`) | deterministic null binding for unbound slots | `true` (`HLD-05` §4.10) | `[I]` |
| `setSupportAttributeStrides(_:)` (`:63`) | enables `setAddress` with stride | unused under software pulling (`HLD-05` §3.5) | `[U]` |

**Binding calls.** `MTL4::ArgumentTable` exposes exactly four bind methods
(`MTL4ArgumentTable.hpp:73-80`):

| Method | Selector (metal-cpp) | Slot kind | RSX use |
|---|---|---|---|
| `setAddress(gpuAddress, index)` (`:73`) | `setAddress:atIndex:` | **buffer** | constant/state buffers sub-allocated in a heap (slots 0–4, 7–11) |
| `setAddress(gpuAddress, stride, index)` (`:74`) | `setAddress:attributeStride:atIndex:` | **buffer** | attribute-strided; unused (pulling) |
| `setResource(resourceID, index)` (`:76`) | `setResource:atBufferIndex:` | **buffer** | a whole `MTLBuffer` by `gpuResourceID` |
| `setTexture(resourceID, index)` (`:80`) | `setTexture:atIndex:` | **texture** | vertex streams (5, 6) and textures (12+i) |
| `setSamplerState(resourceID, index)` (`:78`) | `setSamplerState:atIndex:` | **sampler** | texture samplers (12+i) |

**There is no `setBytes` and no Metal-3 `setBuffer:offset:index:`.** The MTL4 argument table binds a
buffer either by GPU address (`setAddress:atIndex:`) or by resource (`setResource:atBufferIndex:`);
the Metal-3 `setBuffer:offset:atIndex:` form does not exist on `MTL4::ArgumentTable`. Every constant,
including the two substituted push-constant blocks, is a resident `MTL::Buffer` bound by `setAddress`
(`HLD-05` §3.3, §4.3). Absent in metal-cpp `[V]`; the target SDK is `[U]` (`HLD-01` §6 Q5). `[V]`/`[U]`

**Encoder attach.** `RenderCommandEncoder::setArgumentTable(table:stages:)`
(`MTL4RenderCommandEncoder.hpp:84`) selects one table per stage
(`MTL::RenderStageVertex`/`RenderStageFragment`); the compute encoder uses
`ComputeCommandEncoder::setArgumentTable(_:)` (`MTL4ComputeCommandEncoder.hpp:108`). Because the
stage selects the table, the **same index space** is used for both stages (`HLD-05` §3.1). `[V]`

**Snapshot semantics.** A table mutation is visible only to draws encoded *after* it; the table is
snapshotted at draw time. This is what makes per-draw mutation safe and is the precondition for
state-keyed batching (`HLD-01` §4.8; `09` §4). `[V]`

**Slot alignment.** The concrete slot → index → MSL-declaration table lives in `HLD-05` §3.2 and is
**not duplicated here**; the command-encoding-relevant fact is the *kind* of bind per slot:

| Slot range | Source (`pipeline_binding_table.h:9-20`) | Bind kind | Conf. |
|---|---|---|---|
| 0–4 | vertex params, vertex constants, fragment constants, fragment state, fragment texture params | `setAddress` | `[V]` |
| 5–6 | vertex streams (persistent, volatile) | `setTexture` (buffer-texture) | `[V]` |
| 7 | draw-parameters heap | `setAddress` | `[V]` |
| 8 | conditional-render predicate | `setAddress` | `[V]` |
| 9 | rasterizer env (stipple) | `setAddress` | `[V]` |
| 10–11 | instancing LUT, instancing constants | `setAddress` | `[V]` |
| 12+i | fragment/vertex textures | `setTexture` + `setSamplerState` | `[V]` |
| `S_vs_draw_params`, `S_fs_blend` | substituted push constants (`HLD-05` §3.2 †) | `setAddress` | `[I]` |

### 3.4 Pipeline state

**Verified correction to the naming.** In metal-cpp (commit `27c4382b`, 2026-06-08) there is **no**
`MTL4::RenderPipelineState`, `MTL4::ComputePipelineState`, or `MTL4::MeshRenderPipelineState` class.
`MTL4::Compiler::newRenderPipelineState(...)` and `newComputePipelineState(...)` return the shared
**`MTL::RenderPipelineState`** / **`MTL::ComputePipelineState`** types (`MTL4Compiler.hpp:106,127`);
mesh pipelines are described by `MTL4::MeshRenderPipelineDescriptor`
(`MTL4MeshRenderPipeline.hpp:6`) but also compile to `MTL::RenderPipelineState`. The encoder setters
are `setRenderPipelineState(const MTL::RenderPipelineState*)` (`MTL4RenderCommandEncoder.hpp:108`) and
`setComputePipelineState(const MTL::ComputePipelineState*)` (`MTL4ComputeCommandEncoder.hpp:110`).
`[V]` (Whether a future SDK renames these to `MTL4*PipelineState` is `[U]`.)

| Pipeline object | Descriptor | Factory | Conf. |
|---|---|---|---|
| `MTL::RenderPipelineState` | `MTL4::RenderPipelineDescriptor` (`MTL4RenderPipeline.hpp:107`) | `Compiler::newRenderPipelineState(descriptor:options:error:)` (`MTL4Compiler.hpp:127`) | `[V]` |
| `MTL::ComputePipelineState` | `MTL4::ComputePipelineDescriptor` (`MTL4ComputePipeline.hpp:5`) | `Compiler::newComputePipelineState(descriptor:options:error:)` (`MTL4Compiler.hpp:106`) | `[V]` |
| mesh pipeline → `MTL::RenderPipelineState` | `MTL4::MeshRenderPipelineDescriptor` (`MTL4MeshRenderPipeline.hpp:6`) | `Compiler::newRenderPipelineState(...)` | `[I]` |

**Creation is asynchronous.** The `CompilerTask*` overloads
(`MTL4Compiler.hpp:108-110,129-131`) return immediately and deliver the state through a
`NewRenderPipelineStateCompletionHandler`; the synchronous overloads block. The RSX-first design uses
the async form and a keyed cache so a compile stall never blocks the FIFO thread (`HLD-05` §3.5).

**One `MTL4::Compiler` per device.** `Device::newCompiler(descriptor:error:)`
(`MTLDevice.hpp:425`); the KosmicKrisp workaround (W11) requires a single compiler because concurrent
compilers corrupt the heap (`HLD-05` §3.4). This serializes pipeline creation and shapes the async
pool topology (§6 Q7). `[V]`

**PSO vs encoder state.** Per `HLD-05` §3.5, the PSO carries the two shader functions, input
topology, per-attachment blend, attachment formats, sample count, and rasterization/cull/front-face.
Depth-stencil is a **separate** `MTL::DepthStencilState` set with
`setDepthStencilState(_:)` (`MTL4RenderCommandEncoder.hpp:98`). Viewport, scissor, blend colour,
depth bias, depth-clip, stencil reference, triangle fill, and vertex amplification are **encoder
dynamic state** (`MTL4RenderCommandEncoder.hpp:86-125`). `[V]`

**Cache key.** Composed from the two guest program ids plus the PSO-relevant subset of
`pipeline_props`, excluding depth-stencil (`HLD-05` §3.5). Cache persistence uses
`MTL4::PipelineDataSetSerializer` → `MTL::Archive` (`HLD-01` §3.4).

### 3.5 Indirect command buffers and multi-draw

**RSX model.** A non-trivial draw clause materialises as one sub-draw per primitive-restart barrier
(`draw_clause::get_subranges`, `draw_call.cpp:141-181`; `is_single_draw`, `draw_call.hpp:177-199`).
`draw_clause` is `draw_call.hpp:19`; `pass_count()` is `draw_call.hpp:311`. The draw handlers are
`draw_arrays` (`nv4097.cpp:421`), `draw_index_array` (`:429`), `draw_inline_array` (`:437`), and the
begin/end state machine `set_begin_end` (`:485`).

**Metal 4 has no native multi-draw.** The two available mechanisms:

| Mechanism | Metal 4 objects | Verdict | Conf. |
|---|---|---|---|
| Host-built indirect argument buffer | `MTL::DrawPrimitivesIndirectArguments` / `MTL::DrawIndexedPrimitivesIndirectArguments` (`MTLRenderCommandEncoder.hpp:113,121`); `drawPrimitives(type:indirectBuffer:)` (`MTL4RenderCommandEncoder.hpp:79`); `drawIndexedPrimitives(type:indexType:indexBuffer:indexBufferLength:indirectBuffer:)` (`:69`) | **chosen** for multi-range batches; `MTLDraw*IndirectArguments` written into a host buffer, chunked by `multidraw_support.max_batch_size` 65536 | `[V]` |
| `MTL::IndirectCommandBuffer` (ICB) | `Device::newIndirectCommandBuffer(descriptor:maxCommandCount:options:)` (`MTLDevice.hpp:463`); `executeCommandsInBuffer(_:withRange:)` (`MTL4RenderCommandEncoder.hpp:81`) / `(_:indirectBuffer:)` (`:82`) | **not used for draws** — an ICB command cannot bind an `MTL4::ArgumentTable`; argument binding is encoder state, so a pre-recorded draw cannot carry its bindings | `[V]`/`[U]` |

The ICB verdict matches `HLD-01` §3.5 ("ICB rapid draw (strategy D) … out of scope for draws") and
`README` R44. The `MTL::IndirectRenderCommand` surface carries no argument-table method, so an ICB
batch would have to share one table across all commands and could not express per-draw RSX bindings.
`executeCommandsInBuffer` **is** used on the **compute** encoder for compute batches such as the
conditional-render predicate-patch (`MTL4ComputeCommandEncoder.hpp:88-89`). `[V]`/`[U]`

**Inline arrays / immediate mode** (`INLINE_ARRAY`, `ARRAY_ELEMENT16/32`; `LLD-draw` §3.4) are host
words in the command stream. There is no Metal path to source vertex data from the stream: the words
are staged into an `MTL::Buffer` and issued as an ordinary draw. No ICB participates. `[V]`

**Chunking and addresses.** Because `drawIndexedPrimitives` takes the index buffer as an
`MTL::GPUAddress` + length (not bound state), the index buffer must have a **stable GPU address** for
the lifetime of the commit — a heap sub-allocation must not be moved or freed before feedback (§4.6).
`[V]`

### 3.6 Draws and the conditional-render gap

**Draw methods** (all on `MTL4::RenderCommandEncoder`, verified):

| RSX draw form | Metal 4 method | Notes | Conf. |
|---|---|---|---|
| non-indexed | `drawPrimitives(type:vertexStart:vertexCount:)` (`:76`) | +`instanceCount` (`:77`), +`instanceCount:baseInstance:` (`:78`) | `[V]` |
| indexed | `drawIndexedPrimitives(type:indexCount:indexType:indexBuffer:indexBufferLength:)` (`:66`) | +`instanceCount` (`:67`), +`instanceCount:baseVertex:baseInstance:` (`:68`) | `[V]` |
| multi-range | `drawPrimitives(type:indirectBuffer:)` (`:79`), `drawIndexedPrimitives(…indirectBuffer:)` (`:69`) | args in a host buffer (§3.5) | `[V]` |

`MTL::IndexType` is `UInt16`/`UInt32`. Primitive types are a strict subset of RSX's; line-loop, fan,
quad, quad-strip, and polygon are **host-emulated** by index-buffer/topology rewriting
(`get_appropriate_topology`, `is_primitive_native`, `generate_emulating_index_buffer`;
`VKVertexBuffers.cpp:103-115/208`; `LLD-draw` §3.5). `[V]`

**Instancing.** `instanceCount`/`baseInstance` map the RSX instanced draw; the instancing LUT and
constant array are slots 10/11 (`HLD-05` §3.2). `draw_clause::is_trivial_instanced_draw` distinguishes
the trivial (hardware-expressible) case from the general case that needs the LUT. `[V]`

**Conditional render.** `SET_RENDER_ENABLE` (`set_render_mode`, `nv4097.cpp:692-726`) has no Metal 4
conditional-render command (`README` R6). Two shapes chosen by `backend_configuration`
(`RSXThread.h:80-98`): **bring-up** CPU skip via `cond_render_ctrl.disable_rendering()`
(`RSXZCULL.cpp:976-979`, consulted at `VKDraw.cpp:1266`); **parity** a compute predicate-patch that
zeroes the indirect arguments, plus the predicate bound at slot 8
(`conditional_render_predicate_slot = 8`, `pipeline_binding_table.h:15`) for a shader early-out
(`LLD-draw` §3.7). `[I]`

### 3.7 Vertex fetch

**Decision: software pulling; no `MTLVertexDescriptor`.** RSX has no host vertex-input layout; the VS
reads attributes from the `texture_buffer` streams by `[[vertex_id]]` (`VKVertexProgram.cpp:162-184`;
`RSXVertexFetch.glsl`; `HLD-05` §3.5). The PSO therefore sets **no** `MTL::VertexDescriptor`
(`RenderPipelineDescriptor::setVertexDescriptor` exists at `MTL4RenderPipeline.hpp:163` but is left
null) and binds **no** vertex buffers for attributes. The persistent/volatile streams arrive as
`[[texture(5)]]`/`[[texture(6)]]` via `setTexture`, the draw-parameters heap as `[[buffer(7)]]` via
`setAddress` (`LLD-vertex` §3.0). Attribute format decode, masks, and register-value attributes live in
`LLD-vertex` §3.1–§3.3. `[V]`/`[I]`

### 3.8 Hand-off to `HLD-03`

The recorder is the *only* place barriers and pass boundaries are emitted. The seam:

| Hand-off | Provider | Consumer | Metal 4 object | Conf. |
|---|---|---|---|---|
| pass plan → encoder open | `HLD-03` `pass_scheduler` | `mtl::command_recorder::open_pass` | `MTL4::RenderPassDescriptor` → `renderCommandEncoder(descriptor:options:)` | `[I]` |
| merge decision → encoder options | `HLD-03` | `open_pass` | `RenderEncoderOptionSuspending`/`Resuming` (`MTL4RenderCommandEncoder.hpp:55-59`) | `[V]` |
| barrier spec → encoder | `HLD-03` `mtl::emit_barrier` | open encoder | `barrierAfterEncoderStages:beforeEncoderStages:` (`MTL4CommandEncoder.hpp:50`), `barrierAfterQueueStages:beforeStages:` (`:52`), `barrierAfterStages:beforeQueueStages:` (`:54`) | `[V]` |
| cross-encoder fence (same CB) | `HLD-03` | encoder | `updateFence(_:afterEncoderStages:)` (`MTL4CommandEncoder.hpp:70`) / `waitForFence(_:beforeEncoderStages:)` (`:72`) | `[V]` |
| queue ordering | `HLD-03` | `MTL4::CommandQueue` | `signalEvent`/`wait` (`MTL4CommandQueue.hpp:142,148`) | `[V]` |
| timestamps | `HLD-03` | CB/encoder | `writeTimestampIntoHeap(_:index:)` (`MTL4CommandBuffer.hpp:91`), `writeTimestamp(granularity:stage:counterHeap:index:)` (render/compute), `MTL4::CounterHeap` (`MTL4Counters.hpp:63`) | `[V]` |
| occlusion | `HLD-03` | render encoder | `setVisibilityResultMode(_:offset:)` (`MTL4RenderCommandEncoder.hpp:127`); `RenderPassDescriptor::setVisibilityResultBuffer` (`MTL4RenderPass.hpp:97`) | `[V]` |
| completion | recorder | `HLD-03`/frame ring | `MTL4CommitFeedback` (`MTL4CommitFeedback.hpp:38`) | `[V]` |

The TBDR constraint is `HLD-03`'s: no `.fragment`/`.tile` in `afterEncoderStages` on a render encoder
(`HLD-01` §4.3; `HLD-03` §4.4); the recorder honours it by forwarding a pass-split request rather than
emitting an illegal intra-pass barrier.

---

## 4. Invariants

1. **One allocator per encoding thread / frame-in-flight.** `MTL4::CommandAllocator` is not
   thread-safe; no `reset()` while its GPU work is in flight (`MTL4CommandAllocator.hpp:57`;
   `HLD-01` §4.7). `[V]`
2. **Resource lifetime ≥ commit completion.** The command buffer holds no strong references; no
   resource, argument table, transient, or index buffer may be freed or overwritten before the
   matching `MTL4CommitFeedback` (`HLD-01` §4.6). `[V]`
3. **Everything bound is resident.** Every argument-table-bound resource is in a committed
   `MTLResidencySet` before the commit that uses it (`HLD-01` §4.5). Residency is not ordering. `[V]`
4. **Argument-table snapshot semantics.** A table mutation is visible only to draws encoded after it
   (`HLD-01` §4.8). `[V]`
5. **No `setBytes`.** Every constant is memory-backed and bound by `setAddress`
   (`MTL4ArgumentTable.hpp:73-80`; `HLD-05` §4.3). `[V]`/`[U]`
6. **Stable index-buffer address.** An index buffer passed as `MTL::GPUAddress` must not move or be
   freed for the commit's lifetime (`MTL4RenderCommandEncoder.hpp:66-69`). `[V]`
7. **No vertex descriptor, no attribute vertex buffers.** Vertex fetch is software pulling
   (`HLD-05` §3.5). `[V]`/`[I]`
8. **No ICB for draws.** A pre-recorded `MTLIndirectRenderCommand` cannot carry an argument-table
   binding; draws use indirect argument buffers or a loop (`HLD-01` §3.5; `README` R44). `[V]`/`[U]`
9. **Pipeline state set before draw; depth-stencil is encoder state, not PSO.** `HLD-05` §3.5. `[V]`
10. **Every barrier/boundary through the single chokepoint.** No inline `barrier*` call elsewhere
    (`HLD-01` §4.2; `HLD-03` §4.3). `[I]`
11. **Every opened encoder is closed exactly once.** An `endEncoding()`-less encoder invalidates the
    command buffer (`MTL4CommandEncoder.hpp:58`). `[V]`
12. **One commit per frame (first cut), matching `close_and_submit_command_buffer`**
    (`VKGSRender.cpp:2510`); grouped multi-command-buffer commit is permitted by
    `commit(commandBuffers:count:options:)` (`HLD-01` §6 Q10). `[V]`/`[I]`

---

## 5. Failure modes & limits

| Failure | Mechanism | Detection | Conf. |
|---|---|---|---|
| **Dangling bind** | a slot a compiled MSL references is never bound, or bound to a freed resource | `setInitializeBindings(true)` gives a deterministic null binding for *unbound*, not for *freed*; guest-memory differential (H5) | `[I]` |
| **Argument-table reuse across frames** | a table still referenced by in-flight work is mutated for the next frame | correctness/pixel corruption; H2/H3 decision-trace diff | `[I]` |
| **Encoder leak** | an encoder is opened and never `endEncoding()`d | command buffer invalid at `endCommandBuffer()`/commit | `[V]` |
| **Allocator reset in flight** | `reset()` before feedback | GPU reads recycled command memory; crash or garbage | `[V]` |
| **Moved index buffer** | a heap sub-allocation holding an index buffer is relocated before commit | wrong indices, no error | `[V]` |
| **ICB-for-draws attempted** | an ICB is used to batch draws, but the argument table cannot be carried | all commands read one stale table | `[U]` |
| **No multi-draw** | a long clause lowers to N draw calls instead of one indirect batch | per-draw CPU overhead; `m_frame_stats` | `[V]` |
| **No conditional-render command** | bring-up CPU skip diverges from GPU predicate timing; parity needs the compute patch | guest-observable readback (H5) | `[V]` |
| **No stage-gated queue wait** | `MTL4::CommandQueue::wait` gates the queue, not a stage (**R7**; `HLD-03` §2) | re-anchor on the surface hazard; not expressible exactly | `[V]` |
| **One compiler per device** | async pipeline creation serializes (KK W11) | compile stall under a shader-heavy frame | `[V]` |
| **`setBytes` absent** | a port that assumes push constants fails to bind | compile-time (symbol absent in metal-cpp); non-blocking under RSX-first | `[V]` |
| **Primitive emulation cost** | fan/quad/polygon rewrite builds an index buffer per draw | CPU time; `LLD-draw` §3.5 | `[V]` |

---

## 6. Open questions

Each is a decision for a human, tagged with confidence.

1. **[V]/[I] Commit model — one command buffer per frame, or grouped commit?** Keep the VK-shaped
   single commit (`VKGSRender.cpp:2510`) or move to per-frame allocators with grouped
   `commit(commandBuffers:count:options:)`. Changes C4 and the draw path (`HLD-01` §6 Q10). `[V]`
2. **[I] Argument-table granularity — one frame-wide table mutated per draw, or per-stage /
   per-state tables?** Strategy-C indirect batching needs a batch to share one table; a per-draw
   table swap kills it (`HLD-01` §6 Q4; `HLD-05` §6 Q3). Directly constrains §3.3. `[I]`
3. **[U] Does the target SDK's `MTL4::ArgumentTable` lack `setBytes`?** metal-cpp exposes only
   `setAddress/setResource/setTexture/setSamplerState` (`MTL4ArgumentTable.hpp:73-80`); Apple's
   cross-API table lists `setBytes`. RSX-first is non-blocking (constant buffers), but it decides the
   substituted-push-constant path (`HLD-05` §3.3). `[U]`
4. **[U] Is ICB batching for draws feasible under any shared-table scheme?** §3.5 rejects it today
   because ICB commands cannot bind tables; confirm whether a frame-wide table + ICB can recover the
   rapid-draw strategy D (`HLD-01` §3.5; `README` R44). `[U]`
5. **[I] Conditional-render first cut — CPU skip or compute predicate-patch from day one?** `HLD-01`
   §6 Q8; `LLD-draw` §3.7. The GPU-path barrier/ordering details need a spike. `[I]`
6. **[U] Mesh shaders for RSX geometry/transform-feedback emulation?** `MTL4::MeshRenderPipelineDescriptor`
   (`MTL4MeshRenderPipeline.hpp:6`) and `drawMeshThreadgroups`/`drawMeshThreads`
   (`MTL4RenderCommandEncoder.hpp:71-74`) could host the vertex-expansion pass; never attempted in the
   VK path. `[U]`
7. **[U] Async pipeline-compile pool topology given one `MTL4::Compiler` per device.** KK's W11
   workaround implies serialization (`HLD-05` §3.4); the pool and its priority queue are undesigned
   (`HLD-01` §6 Q6). `[U]`
8. **[U] Indirect argument-buffer ring sizing and chunking policy.** The `max_batch_size` 65536 chunk
   bound and the per-frame argument-buffer ring depth are unspecified here (`LLD-draw` §3.6). `[U]`
9. **[I] Does encoding stay on the RSX/FIFO thread?** §2.3 assumes yes for the first cut; a second
   encoding thread needs its own allocator, tables, and a CB handoff. `[I]`
10. **[U] Is the vertex-descriptor path truly unused for every title?** Software pulling is the
    decision, but `setSupportAttributeStrides`/`setAddress(stride:)` remain available if a title needs
    hardware-strided input (`HLD-05` §3.5). `[U]`
11. **[U] Does `setSamplerState` bypass the argument-buffer sampler cap?** If yes, the sampler
    eviction path disappears (`HLD-01` §6 Q12; `README` R17). `[U]`

**Highest-value (report-back set):** Q1 (commit model), Q2 (argument-table granularity), Q3
(`setBytes`), Q4 (ICB-for-draws), Q5 (conditional-render first cut), Q6 (mesh-shader decision).

---

## 7. Provenance

### 7.1 This tree `[V]` (paths under `rpcs3/Emu/` unless noted)

- `RSX/RSXThread.h` — `backend_configuration` `:80-98`, member `:123`, accessor `:230`;
  `m_graphics_state` `:254`; `run_FIFO` `:357`.
- `RSX/GSRender.h:20` — `GSRender : rsx::thread`.
- `RSX/RSXFIFO.h:63-127` — `flattening_helper`, `FIFO_control`; `RSX/RSXFIFO.cpp:522-579`
  (thresholds `>2000`/`≥500`), `:875-931` (flattener + `methods[reg]` dispatch).
- `RSX/NV47/FW/draw_call.hpp:19` (`draw_clause`), `:177-199` (`is_single_draw`), `:206,311`
  (`get_subranges`/`pass_count`); `draw_call.cpp:141-181` (subranges).
- `RSX/NV47/HW/nv4097.cpp:421` (`draw_arrays`), `:429` (`draw_index_array`), `:437`
  (`draw_inline_array`), `:485` (`set_begin_end`), `:692-726` (`set_render_mode`).
- `RSX/VK/vkutils/pipeline_binding_table.h:7-22` — canonical slot map (slots 0–12).
- `RSX/VK/VKGSRender.cpp:2510` (`close_and_submit_command_buffer`), `:2290-2322` (bind slots),
  `:2429-2458` (push constants), `:1266` (conditional-render consult), `:752-754` (provoking vertex).
- `RSX/VK/VKDraw.cpp:1160-1169` (VK conditional render), `:1224-1240` (multi-draw args).
- `RSX/VK/VKVertexBuffers.cpp:103-115/208` (topology emulation); `RSX/Common/BufferUtils.cpp:330/445/567/619`.
- `RSX/VK/VKVertexProgram.cpp:162-184` (persistent/volatile streams), `:128-142`, `:144-159`.
- `RSX/RSXZCULL.cpp:976-979` (`disable_rendering`), `:986-1035` (cond render ctrl).
- `docs/research/metal4/design/HLD-01-architecture.md` §2.2 (C4), §3.3–§3.5, §4, §6.
- `docs/research/metal4/design/HLD-03-synchronization-model.md` §2, §3.3–§3.4, §4.
- `docs/research/metal4/design/HLD-05-shader-binding-contract.md` §3.1–§3.5, §4, §6.
- `docs/research/metal4/design/LLD-draw.md` §3.4–§3.7; `LLD-vertex.md` §3.0–§3.3.
- `docs/research/metal4/inventory/SCHEMA.md` §6 (HLD template), §4 (disposition vocabulary);
  `docs/research/metal4/inventory/out/rsx_binding_slots.csv`.

### 7.2 metal-cpp `[V]` (paths under `metal-cpp/Metal/`, commit `27c4382b`, 2026-06-08)

- `MTL4CommandBuffer.hpp` — `beginCommandBuffer(allocator:)` `:64`, `(allocator:options:)` `:65`;
  `computeCommandEncoder()` `:67`; `endCommandBuffer()` `:71`; `renderCommandEncoder(descriptor:)`
  `:81`; `renderCommandEncoder(descriptor:options:)` `:82`; `useResidencySet(_:)` `:88`;
  `writeTimestampIntoHeap(_:index:)` `:91`; `CommandBufferOptions` `:51`.
- `MTL4RenderCommandEncoder.hpp` — `drawIndexedPrimitives` `:66-69`; `drawMeshThreadgroups` `:71-72`;
  `drawMeshThreads` `:74`; `drawPrimitives` `:76-79`; `executeCommandsInBuffer` `:81-82`;
  `setArgumentTable(_:stages:)` `:84`; dynamic-state setters `:86-125`;
  `setVisibilityResultMode(_:offset:)` `:127`; `RenderEncoderOptions` `:55-59`.
- `MTL4ComputeCommandEncoder.hpp` — `copyFromBuffer` `:67-69`; `dispatchThreadgroups` `:82-83`;
  `dispatchThreads` `:85-86`; `executeCommandsInBuffer` `:88-89`; `fillBuffer` `:91`;
  `generateMipmaps` `:93`; `setArgumentTable(_:)` `:108`; `setComputePipelineState(_:)` `:110`;
  `setThreadgroupMemoryLength` `:114`; `writeTimestamp` `:120`.
- `MTL4CommandEncoder.hpp` — `barrierAfterEncoderStages` `:50`; `barrierAfterQueueStages` `:52`;
  `barrierAfterStages` `:54`; `endEncoding()` `:58`; `updateFence(_:afterEncoderStages:)` `:70`;
  `waitForFence(_:beforeEncoderStages:)` `:72`.
- `MTL4ArgumentTable.hpp` — `ArgumentTableDescriptor` `:37`; `setMaxBufferBindCount` `:57`;
  `setMaxTextureBindCount` `:59`; `setMaxSamplerStateBindCount` `:61`; `setInitializeBindings` `:53`;
  `setSupportAttributeStrides` `:63`; `setAddress` `:73-74`; `setResource` `:76`;
  `setSamplerState` `:78`; `setTexture` `:80`.
- `MTL4CommandQueue.hpp` — `CommandQueue` `:120`; `commit` `:126-127`; `addResidencySet` `:123`;
  `signalEvent` `:142`; `wait` `:148`; `signalDrawable` `:140`; `CommitOptions::addFeedbackHandler`
  `:98`.
- `MTL4CommandAllocator.hpp` — `reset()` `:57`; `allocatedSize()` `:51`.
- `MTL4Compiler.hpp` — `newComputePipelineState` `:106`; `newRenderPipelineState` `:127`;
  `newLibrary` `:119`; async `CompilerTask` overloads `:108-110,129-131`.
- `MTL4RenderPipeline.hpp` — `RenderPipelineDescriptor` `:107`; `setVertexDescriptor` `:163`;
  `setSupportIndirectCommandBuffers` `:159`.
- `MTL4ComputePipeline.hpp:5` — `ComputePipelineDescriptor`; `MTL4MeshRenderPipeline.hpp:6` —
  `MeshRenderPipelineDescriptor`.
- `MTL4RenderPass.hpp` — `RenderPassDescriptor` `:46`; `setVisibilityResultBuffer` `:97`;
  `setRenderTargetWidth/Height` `:83/81`; `setDefaultRasterSampleCount` `:71`.
- `MTL4Counters.hpp:63` — `CounterHeap`; `MTL4CommitFeedback.hpp:38` — `CommitFeedback`.
- `MTLDevice.hpp` — `newArgumentTable` `:407`; `newCommandAllocator` `:416-417`; `newCommandBuffer`
  `:419`; `newCompiler` `:425`; `newCounterHeap` `:437`; `newIndirectCommandBuffer` `:463`;
  `newMTL4CommandQueue` `:477-478`; `newResidencySet` `:496`.
- `MTLRenderCommandEncoder.hpp:113,121` — `DrawPrimitivesIndirectArguments` /
  `DrawIndexedPrimitivesIndirectArguments` (argument structs, shared with MTL4).

### 7.3 Cross-document consistency

- Slot → argument-table index → MSL declaration: `HLD-05` §3.2 (authoritative; not duplicated).
- Barrier/pass-split policy and the TBDR fragment constraint: `HLD-03` §2, §4.
- Component decomposition (C4 `mtl::command_recorder`) and object inventory: `HLD-01` §2.2, §3.4.
- Disposition vocabulary and template: `inventory/SCHEMA.md` §4, §6.

<!-- doc-end -->
