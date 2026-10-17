# LLD — `draw` cluster (RSX draw submission → Metal 4)

Owner/Scope: RSX-first Metal 4 backend, the `draw` cluster | Status: draft | Date: 2026-10-16 (Fri)
Branch: `metal4`. Design only — nothing under `rpcs3/**` is modified.

**Legend.** `[V]` verified by reading a file in this repo (or a pristine reference under
`/tmp/metal4-refs/`); `[I]` inference from `[V]` facts; `[U]` could not verify. `file:line` is
advisory (the branch moves); **symbol names are the stable reference**. Difficulty 1–5 (1 trivial
rename · 2 mechanical · 3 real work, well-trodden · 4 hard/needs design · 5 unknown) and
disposition vocabulary are those of `inventory/SCHEMA.md` §4. Every id below is taken verbatim from
`inventory/out/clusters/draw.csv` (13 ids); this document invents no register.

---

## 1. Scope

The `draw` cluster is the RSX **draw-submission** state: the begin/end primitive-mode state machine,
the non-indexed and indexed draw commands, inline/immediate vertex and index data pushed through the
FIFO, the index-array descriptor (base address + DMA location + index width), primitive restart, and
the render-enable register that gates conditional rendering. It is the half of the command space
that *issues work* — as opposed to the `surface`/`raster`/`vertex` clusters that *parameterise*
work. `inventory/out/SUMMARY.md:106-122`; `12-rsx-state-space-inventory.md:166` ("begin/end, draw
calls, inline arrays").

In scope (13 ids, `out/clusters/draw.csv`): `M_NV4097_SET_BEGIN_END`, `M_NV4097_DRAW_ARRAYS`,
`M_NV4097_DRAW_INDEX_ARRAY`, `M_NV4097_INLINE_ARRAY`, `M_NV4097_ARRAY_ELEMENT16`,
`M_NV4097_ARRAY_ELEMENT32`, `M_NV4097_SET_INDEX_ARRAY_ADDRESS`, `M_NV4097_SET_INDEX_ARRAY_DMA`,
`M_NV4097_SET_RESTART_INDEX_ENABLE`, `M_NV4097_SET_RESTART_INDEX`, `M_NV4097_SET_RENDER_ENABLE`,
`M_NV4097_SET_VERTEX_DATA_BASE_OFFSET`, `M_NV4097_SET_VERTEX_DATA_BASE_INDEX`.

Out of scope, owned elsewhere: vertex attribute decode/format and the software-pull upload
(`vertex` cluster → `LLD-vertex`), transform constants and instancing tables (`vertex`), the
render-target/pass-descriptor synthesis that an encoder opens against (`surface`, `HLD-02`), the
argument-table index space (`HLD-05`), the ZCULL/occlusion query that produces the conditional
predicate (`depth-rop` → `LLD-depth-rop`), and barriers/residency (`HLD-03`). This document fixes
only how the 13 draw registers lower onto `MTL4RenderCommandEncoder` draw calls.

**The shape change.** RSX submission is *stateful*: `SET_BEGIN_END := primitive` opens a draw
clause, subsequent draw commands append ranges into it, and `SET_BEGIN_END := 0` closes the clause
and executes it (`nv4097.cpp:485-559`). Metal has no such state: a draw is a discrete
`drawPrimitives`/`drawIndexedPrimitives` call on an already-open `MTL4RenderCommandEncoder`. The
begin/end machine is therefore collapsed **entirely into host-side draw-clause lowering** — RPCS3
already does this — and the Metal backend sees only a vector of `(first,count)` ranges plus a
primitive and an index mode. `draw_call.hpp:19-86`, `[V]`.

---

## 2. Register model

### 2.1 The shard

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_VERTEX_DATA_BASE_OFFSET` | 0x1738 | whole word (`vertex_data_base_offset`) | `nv4097::set_vertex_base_offset` | reshape | `MTL4ArgumentTable.setAddress` (vertex_buffers slot 5), base-added offsets |
| `M_NV4097_SET_VERTEX_DATA_BASE_INDEX` | 0x173c | whole word (`index_base`) | `nv4097::set_index_base_offset` | reshape | `drawIndexedPrimitives(…baseVertex:)` |
| `M_NV4097_SET_BEGIN_END` | 0x1808 | low u8 = `primitive_type` | `nv4097::set_begin_end` | reshape | `MTL4RenderCommandEncoder` open per pass + one draw call per lowered range |
| `M_NV4097_ARRAY_ELEMENT16` | 0x180c | none (two `u16`) | `nv4097::set_array_element16` | emulate | `drawIndexedPrimitives(…indexBuffer:)` from a host-staged index buffer |
| `M_NV4097_ARRAY_ELEMENT32` | 0x1810 | none (one `u32`) | `nv4097::set_array_element32` | emulate | `drawIndexedPrimitives(…indexBuffer:)` from a host-staged index buffer |
| `M_NV4097_DRAW_ARRAYS` | 0x1814 | `start` 0:23, `count_raw` 24:31 | `nv4097::draw_arrays` | direct | `drawPrimitives(…vertexStart:vertexCount:)` |
| `M_NV4097_INLINE_ARRAY` | 0x1818 | none (raw vertex word) | `nv4097::draw_inline_array` | emulate | transient shared `MTLBuffer` (attribute ring) → `drawPrimitives` |
| `M_NV4097_SET_INDEX_ARRAY_ADDRESS` | 0x181c | `index_array_offset` 0:28 | — (unbound) | reshape | `drawIndexedPrimitives(…indexBuffer:)` (`GPUAddress`) |
| `M_NV4097_SET_INDEX_ARRAY_DMA` | 0x1820 | `index_dma` 0:3, `type_raw` 4:11 | `nv4097::check_index_array_dma` | reshape | `MTL::IndexType` `UInt16`/`UInt32` on `drawIndexedPrimitives` |
| `M_NV4097_DRAW_INDEX_ARRAY` | 0x1824 | `start` 0:23, `count` 24:31 | `nv4097::draw_index_array` | direct | `drawIndexedPrimitives(…indexCount:indexType:indexBuffer:indexBufferLength:)` |
| `M_NV4097_SET_RESTART_INDEX_ENABLE` | 0x1dac | whole word → bool | — (state signal) | emulate | host index filter → `drawIndexedPrimitives` split runs |
| `M_NV4097_SET_RESTART_INDEX` | 0x1db0 | whole word | — (unbound) | emulate | host index rewrite before `drawIndexedPrimitives` |
| `M_NV4097_SET_RENDER_ENABLE` | 0x1e98 | `mode` = `arg>>24`, `offset` = `arg & 0xffffff` | `nv4097::set_render_mode` | reshape | `MTL4ArgumentTable.setAddress` (conditional_predicate slot 8) + indirect-args zero / CPU skip |

Decode field offsets are ground truth from `out/rsx_decode_fields.csv` (`NV4097_DRAW_ARRAYS`,
`NV4097_DRAW_INDEX_ARRAY`, `NV4097_SET_INDEX_ARRAY_ADDRESS`, `NV4097_SET_INDEX_ARRAY_DMA`);
`inventory/out/SUMMARY.md:451-457`. `[V]`.

### 2.2 Value vs dirty-bit vs clause-transient state

Three lifetimes coexist in this cluster and must not be conflated. `[V]`

1. **Value state (in `rsx_state::registers`).** `SET_INDEX_ARRAY_ADDRESS`
   (`rsx_methods.h:977`), `SET_VERTEX_DATA_BASE_OFFSET` (`:972`), `SET_VERTEX_DATA_BASE_INDEX`
   (read back in `capture/rsx_capture.cpp:209/223`), `SET_RESTART_INDEX` (`rsx_methods.h:301`), and
   `SET_RESTART_INDEX_ENABLE` (`:306`). These persist across draws and are read when the draw is
   lowered. Defaults on reset: base offset/index `0` (`rsx_methods.cpp:962-963`), restart enable
   `0`, restart index `0xffffffff` (`:1005-1006`).
2. **Clause-transient state (in `draw_clause`).** `DRAW_ARRAYS`/`DRAW_INDEX_ARRAY`/`INLINE_ARRAY`
   inflate `draw_command_ranges` / `inline_vertex_array` inside the open clause
   (`draw_call.hpp:22/81`), and `ARRAY_ELEMENT16/32` push into
   `draw_command_processor::m_element_push_buffer` (`RSXDrawCommands.cpp:259-264`). All of this is
   cleared at `reset()`/`end()` (`draw_call.cpp:123-139`; `RSXThread.cpp:987`).
3. **Dirty-bit / signal state.** `SET_RESTART_INDEX_ENABLE` is a *signal* register
   (`state_signals[...] = pipeline_config_dirty`, `rsx_methods.cpp:784`) with no bound handler; the
   clause also carries its own execution barriers (`draw_call.inc.h:15-23`).

### 2.3 The begin/end state machine (`SET_BEGIN_END`)

`set_begin_end` (`nv4097.cpp:485-559`) is the only place a primitive type enters the system:
`SET_BEGIN_END := p` with `p != 0` decodes `to_primitive_type(p)` (`gcm_enums.h:1267`), calls
`draw_clause::reset(p)` (`draw_call.cpp:123`) and `thread::begin()` (`RSXThread.cpp:924`); a
subsequent `SET_BEGIN_END := 0` calls `compile()` (`draw_call.hpp:90`), which decides
`is_trivial_instanced_draw` (`draw_call.cpp:94-121`), then `thread::end()`
(`RSXThread.cpp:971-996`). Intermediate commands mutate the clause; **execution-barrier** commands
(`SET_VERTEX_DATA_BASE_*`, `SET_VERTEX_DATA_ARRAY_OFFSET`, transform-constant load/update) insert a
`barrier_t` and *split the clause into a new sub-draw* (`draw_call.cpp:23-92`, `:67-91`). Invalid
primitive values set `in_begin_end` without resetting the clause and log (`nv4097.cpp:491-497`).

**Why the clause is the right lowering unit.** It already encodes the three things Metal needs per
call: the primitive, the ordered `(first,count)` ranges *with their barrier splits*, and the
batchability decision. Base offsets/index and vertex-array/constant changes arrive as barriers and
are replayed in order by `execute_pipeline_dependencies` (`draw_call.cpp:183-252`) before each
range is issued. `[V]`

---

## 3. Metal 4 mapping

### 3.1 The lowering pipeline (host-side, backend-agnostic, reused)

```
FIFO ──▶ set_begin_end / draw_arrays / draw_index_array / inline_array / array_element*
        └─▶ draw_clause { primitive, ranges[], barriers[], inline_vertex_array[] }
              │  thread::end() ── compile() ── get_subranges()
              ▼
        draw_command_processor::get_draw_command()   (RSXDrawCommands.cpp:214)
              ├─ draw_array_command        → drawPrimitives
              ├─ draw_indexed_array_command→ write_index_array_data_to_buffer → drawIndexedPrimitives
              └─ draw_inlined_array        → materialise into attribute ring  → drawPrimitives
```

Nothing in this pipeline is Vulkan-specific; it is RSX-shape (`09 §3 #16`, `README §3.7 D7/D8`).
The Metal backend consumes the same `draw_array_command` / `draw_indexed_array_command` /
`draw_inlined_array` variant that the VK backend consumes (`RSXDrawCommands.h:52-54`). This is the
central reason the `draw` cluster is *reshape*, not *emulate*: the semantics already live upstream
of the API. `[V]`/`[I]`

### 3.2 Non-indexed draws (`DRAW_ARRAYS`) and instancing

`DRAW_ARRAYS` decodes `start` (24b) and `count_raw` (8b) and appends `(start,count)`
(`nv4097.cpp:421-427`). It maps 1:1 onto
`MTL4RenderCommandEncoder::drawPrimitives(primitiveType, vertexStart, vertexCount)`
(`MTL4RenderCommandEncoder.hpp:76`, metal-cpp). `vertexStart` is the software-pull base (the RSX
backend fetches vertices in the shader; there is no hardware vertex-buffer binding), so the start
indexes the *fetched* stream rather than a bound VBO (`README §3.7 D1`; `VKVertexBuffers.cpp:374`).
`[V]`

**Trivial instancing.** When the clause contains `N>1` identical ranges separated only by
transform-constant barriers, `check_trivially_instanced()` returns true
(`draw_call.cpp:94-121`; `draw_call.hpp:79`) and the draw folds onto the `instanceCount` overload:
`drawPrimitives(primitiveType, vertexStart, vertexCount, instanceCount)`
(`MTL4RenderCommandEncoder.hpp:77`). This is `pass_count()` → `instanceCount`
(`draw_call.hpp:206`; `README §3.7 D2`, `09 §3 #2`). The transition is signalled to the pipeline by
`xform_instancing_state_dirty` (`nv4097.cpp:541-544`). `[V]`

### 3.3 Indexed draws and the index-array descriptor

`DRAW_INDEX_ARRAY` (`start` 24b, `count` 24:8) sets `command = indexed` and appends a range
(`nv4097.cpp:429-435`). The materialised index data comes from `get_raw_index_array`
(`RSXDrawCommands.cpp:193-212`), which resolves the *guest* address from
`SET_INDEX_ARRAY_ADDRESS` + the DMA location, forces alignment to the index size
(`(0 - type_size) & get_address(...)`, `:205`), and returns a byte span at `first*type_size`. The
index width/location come from `SET_INDEX_ARRAY_DMA`
(`index_dma` 0:3 → `CellGcmLocation`, `type_raw` 4:11 → `index_array_type`, `rsx_decode.h:3442-3451`).
That span is uploaded/expanded by `write_index_array_data_to_buffer`
(`VKVertexBuffers.cpp:156-162`), then the draw issues
`drawIndexedPrimitives(primitiveType, indexCount, indexType, indexBuffer, indexBufferLength)`
(`MTL4RenderCommandEncoder.hpp:66`). `[V]`

Two facts fix the Metal form:

- **The index buffer is a GPU-address + length *argument*, not bound state** (metal-cpp takes
  `MTL::GPUAddress indexBuffer, NS::UInteger indexBufferLength`,
  `MTL4RenderCommandEncoder.hpp:66`; `README §3.7 D3`). `SET_INDEX_ARRAY_ADDRESS` therefore does not
  become bound state; it becomes the pointer arithmetic that produces `indexBuffer`. `[V]`
- **`SET_INDEX_ARRAY_DMA` maps to `MTL::IndexType`** — `u16 → IndexTypeUInt16`,
  `u32 → IndexTypeUInt32` (`gcm_enums.h:1247-1251`). Invalid DMA values are rejected upstream and
  roll back to the latch with `recover_fifo()` (`nv4097.cpp:470-481`). `[V]`

`baseVertex` (`MTL4RenderCommandEncoder.hpp:68`) is the natural home for
`SET_VERTEX_DATA_BASE_INDEX` *if* a hardware index buffer is materialised. RSX applies the base to
the index host-side and masks to 20 bits (`get_index_from_base`, `rsx_utils.h:523-526`), exactly the
role of `baseVertex`; whether the RSX-native backend uses `baseVertex` or folds it into the
software-pull fetch is an open question (§6 Q5). `[I]`/`[U]`

`SET_VERTEX_DATA_BASE_OFFSET` is different: it is added to each vertex-array base address, not to
the index, and masked to 28 bits (`get_vertex_offset_from_base`, `rsx_utils.h:514-519`); it is
consumed when the software-pull fetch offsets are written (`VKVertexBuffers.cpp:272-286`,
`GLDraw.cpp:742-743`). Under Metal it lands in the vertex-stream argument-table address
(`HLD-05`, slot 5), not on the draw call. Both base registers are *execution-barrier* commands
(`nv4097.cpp:460-468` → `draw_call.cpp:197-210`), i.e. they force a clause split. `[V]`

### 3.4 Inline arrays and immediate mode (`INLINE_ARRAY`, `ARRAY_ELEMENT16/32`)

There is no Metal path that consumes vertex or index data *from the command stream*. Two RSX
mechanisms feed data through the FIFO and both are host-staged:

- **`INLINE_ARRAY`** pushes one raw vertex word at a time into `inline_vertex_array`
  (`nv4097.cpp:437-442`; `draw_call.hpp:81`), with `command = inlined_array`
  (`get_elements_count`/`min_index` special-case the empty-ranges inline clause,
  `draw_call.hpp:155-175`). The materialised words are staged through the attribute ring and issued
  as a normal `drawPrimitives` (`README §3.7 D8`, `09 §3 #16`). Disposition **emulate** (host
  staging); target is the shared `MTLBuffer` + `drawPrimitives`. `[V]`
- **`ARRAY_ELEMENT16/32`** push indices into `m_element_push_buffer`
  (`nv4097.cpp:406-419`; `RSXDrawCommands.cpp:259-264`). `get_raw_index_array` short-circuits to
  that buffer when non-empty (`RSXDrawCommands.cpp:195-199`), so the immediate-index path and the
  guest-index-array path converge on the same upload → `drawIndexedPrimitives`. `[V]`

Immediate-mode *vertex* data written through the `SET_VERTEX_DATA*_M` registers is the `vertex`
cluster's push-buffer machinery (`append_to_push_buffer`, `RSXDrawCommands.cpp:238-250`); it reaches
this cluster only as `is_immediate_draw` on the clause (`draw_call.hpp:75`; end-of-clause synthesis at
`nv4097.cpp:504-527`). `[V]`

### 3.5 Primitive-type mapping and host emulation

`SET_BEGIN_END` carries a `primitive_type` (`gcm_enums.h:1253-1265`). Metal's `MTLPrimitiveType` is
a strict subset; RSX already emulates the rest by generating an index buffer and/or rewriting the
topology (`get_appropriate_topology`, `is_primitive_native`, `generate_emulating_index_buffer`;
`VKVertexBuffers.cpp:103-115/208`; `get_index_count` `:132`). `[V]`

| RSX `primitive_type` | Metal `MTLPrimitiveType` | path |
|---|---|---|
| `points` | `Point` | native |
| `lines` | `Line` | native |
| `line_strip` | `LineStrip` | native |
| `line_loop` | — | emulated (strip + closing segment) |
| `triangles` | `Triangle` | native |
| `triangle_strip` | `TriangleStrip` | native |
| `triangle_fan` | — | emulated (fan → strip/list) |
| `quads` / `quad_strip` | — | emulated (→ triangles) |
| `polygon` | — | emulated (triangulated) |

The emulation is topology arithmetic on the index buffer (`expand_indexed_triangle_fan`,
`expand_indexed_quads`, `Common/BufferUtils.cpp:567/619`), not a Metal feature gap the backend must
close. `[V]`

### 3.6 Multi-range draws: subranges and indirect

A non-trivial clause materialises as one sub-draw per `primitive_restart_barrier`
(`draw_clause::get_subranges`, `draw_call.cpp:141-181`; `is_single_draw`, `draw_call.hpp:177-199`).
Metal has no multi-draw entry point, so this lowers either to a loop of draw calls or to a
host-built argument array — `MTL::DrawPrimitivesIndirectArguments` /
`MTL::DrawIndexedPrimitivesIndirectArguments` (`MTLRenderCommandEncoder.hpp:113/121`) plus
`drawPrimitives(primitiveType, indirectBuffer)` /
`drawIndexedPrimitives(primitiveType, indexType, indexBuffer, indexBufferLength, indirectBuffer)`
(`:79`/`:69`). This is `README §3.7 D5/D6` and `09 §3 #5/#6`; RPCS3 already builds the VK
equivalent (`m_multidraw_parameters_buffer`, `VKDraw.cpp:1224-1240`). Chunking is bounded by
`multidraw_support.max_batch_size` 65536 (`README §3.7 D10`). Difficulty 3, no per-call Windows cap
on Apple. `[V]`/`[I]`

### 3.7 Render-enable / conditional rendering (`SET_RENDER_ENABLE`)

`set_render_mode` (`nv4097.cpp:692-726`) interprets `mode = arg>>24`: `1` disables conditional
rendering (`disable_conditional_rendering`, `RSXThread.cpp:2901`); `2` treats `arg & 0xffffff` as the
guest report address and enables conditional rendering (`enable_conditional_rendering`,
`RSXThread.cpp:2877`), which arms `cond_render_ctrl` against a ZCULL/occlusion report
(`RSXZCULL.cpp:986-1035`). The predicate is a guest `CellGcmReportData` word whose value-to-zero
means "skip". `[V]`

Metal 4 has **no conditional-render command**: the VK path's
`_vkCmdBeginConditionalRenderingEXT` (`VKDraw.cpp:1160-1169`) has no analog
(`README §6 R6`; `05 #21`). Two shapes, chosen by `backend_configuration`:

- **Bring-up — CPU skip.** `cond_render_ctrl.disable_rendering()` (`RSXZCULL.cpp:976-979`) is
  already consulted at `begin()` (`VKDraw.cpp:1266`) to drop the encoding entirely. This matches
  the GL backend, which leaves `supports_hw_conditional_render = false` (`10 §2.2`). No Metal
  object participates.
- **Parity — GPU predicate-patch.** A compute encoder reads the report predicate and zeroes the
  indirect argument(s) when false, so the draw still issues but writes nothing; the predicate is
  also bound through `MTL4ArgumentTable.setAddress(...,8)` at
  `pipeline_binding_table.h:15` (`conditional_render_predicate_slot = 8`) for a shader-side
  early-out. This is the `09 §6.2` design and the "one genuinely GPU-driven need RSX has"
  (`10 §4.5`). `[I]`

Disposition **reshape** (verdict changed from `impossible` by `README §6 R6` / `10 §9`). The Metal
targets are `MTL4ArgumentTable.setAddress` (slot 8) and the indirect-args zeroing; the CPU-skip
fallback is host code, not a Metal object. Difficulty 4, risk high. `[I]`

---

## 4. Impossible / not-recommended

| Item | Status | Fallback actually chosen |
|---|---|---|
| Native *conditional-render command* | **no Metal API path** (`README §6 R6`) | reshape: CPU skip for bring-up, GPU predicate-patch for parity (§3.7). Not `impossible` because the RSX *requirement* survives. `[I]` |
| Native *multi-draw* command | **no Metal API path** (`README §3.7 D5/D6`) | indirect argument buffer + `draw*(indirectBuffer:)`, else N× draw (§3.6). `[V]` |
| Data *from the command stream* (inline vertex words, FIFO indices) | **no Metal API path** | host staging into an `MTLBuffer` then a normal draw (§3.4). `[V]` |
| Metal-side *primitive restart* control | **no exposed control** in the MTL4 encoder surface | host index filter (`remove_restart_index` / `upload_untouched_skip_restart`, `BufferUtils.cpp:445`/`:330`) and barrier-driven clause splits (`draw_call.cpp:49-65`). Not-recommended to rely on any driver restart behaviour. `[V]` |
| `GL_LINE_LOOP` / fans / quads / polygon topologies | not expressible as `MTLPrimitiveType` | host topology expansion (§3.5). `[V]` |
| Wide lines / polygon mode from the draw path | expressible only via private API | clamp/replace; owned by `raster`/`blend`, not here. `[U]` |

---

## 5. Invariants & failure modes

**Invariants (must hold at every emitted draw).**

1. **An open encoder.** A lowered draw is only legal inside an open `MTL4RenderCommandEncoder` whose
   `MTL4RenderPassDescriptor` matches the RSX surfaces current at that clause. The begin/end machine
   does *not* own the encoder: the pass scheduler (`HLD-02`/`HLD-03`) owns it, and many clauses may
   share one encoder (`10 §4.3`). `[I]`
2. **Barrier-ordered lowering.** `execute_pipeline_dependencies` must run before each range so that
   `SET_VERTEX_DATA_BASE_*`, vertex-array offsets, and transform-constant updates are applied in
   clause order (`draw_call.cpp:183-252`). Reordering violates RSX ordering. `[V]`
3. **Index residency and length.** Before `drawIndexedPrimitives`, the index range must be resident
   (`MTLResidencySet`; Metal 4 has no implicit residency, `README §4.2`) and
   `indexBufferLength >= indexCount * indexSize`, with the `GPUAddress` aligned to the index size
   (`RSXDrawCommands.cpp:205`). `[V]`
4. **No cross-run merging under restart.** Runs separated by a `primitive_restart_barrier` must not
   be fused into one draw (`draw_call.cpp:119-129`, `draw_call.hpp:177-199`). `[V]`
5. **Instancing only when legal.** `instanceCount` may be used only when
   `check_trivially_instanced()` holds — all ranges identical and only transform-constant barriers
   present (`draw_call.cpp:94-121`). `[V]`
6. **Inline-array staging completeness.** The inline vertex words must be uploaded and visible (a
   producer barrier) before the `drawPrimitives` that reads them. `[I]`

**Failure modes.**

- **FIFO desync.** Invalid `SET_INDEX_ARRAY_DMA` (`nv4097.cpp:470-481`) or an invalid
  `SET_BEGIN_END` primitive (`:491-497`) is a desync source; the emulator logs and `recover_fifo()`s.
  A backend that assumes decoded values are valid will mis-draw silently. `[V]`
- **Empty clause / draw-without-begin.** `SET_BEGIN_END := 0` with an empty clause leaves
  `in_begin_end` set false and emits nothing (`nv4097.cpp:529-551`); the backend must not synthesise
  a draw. `[V]`
- **Address mask omission.** Dropping the 28-bit vertex-base mask (`rsx_utils.h:518`) or the 20-bit
  index-base mask (`:525`) silently wraps addresses; these masks are load-bearing (comment:
  "Seen happening heavily in R&C games"). `[V]`
- **`vertexStart` / `baseVertex` confusion.** Using `baseVertex` when the backend still software-pulls
  (or vice versa) double-applies or omits the base; see §6 Q5. `[U]`
- **Missing residency / barrier.** Any draw whose argument-table resources are not in a committed
  residency set, or whose prior writer is unordered, is UB under Metal 4 (`README §4.1/§4.2`). `[V]`
- **Ring reuse.** Index/attribute/indirect rings must be per-frame-in-flight (allocator-scoped);
  reuse before the consuming commit completes is a read-after-write hazard. `[I]`
- **Instancing misclassification.** A clause with a non-transform barrier that is nonetheless folded
  into `instanceCount` produces a wrong image, not a crash — only the invariant 5 check catches it.
  `[V]`

---

## 6. Open questions

- **Q1 `[U]`** — Do indexed draws require a *materialised* `MTLBuffer` for the index data (the metal-cpp
  `drawIndexedPrimitives` overloads all take `GPUAddress indexBuffer`), or can the RSX software-pull
  consume indices from the same buffer and avoid a dedicated index ring? This decides whether
  `SET_INDEX_ARRAY_ADDRESS` is pure pointer math or a residency-bearing object. Experiment: H0 index
  math + a device spike (§`11` H4).
- **Q2 `[U]`** — Is there *any* Metal primitive-restart control (fixed max-index restart) that would
  let the GPU restart strips directly and avoid the host re-upload in
  `emulate_restart` (`VKVertexBuffers.cpp:122/172-176`)? The MTL4 surface exposes none; confirm on
  the target SDK before committing to host emulation.
- **Q3 `[I]`** — Multi-draw shape: indirect args (`MTL::DrawIndexedPrimitivesIndirectArguments`)
  versus an N× loop, and the chunk size. `max_batch_size` 65536 is a *VK* cap
  (`README §3.7 D10`); the Metal argument-buffer sizing rule needs its own derivation (`09 §4-C`).
- **Q4 `[U]`** — Conditional-render first cut (`README §6 R6`; `09 Q2`): CPU skip or GPU
  predicate-patch from day one? The predicate slot 8 (`pipeline_binding_table.h:15`) binding
  semantics under MTL4 (address of the guest report word) are unverified.
- **Q5 `[U]`** — `SET_VERTEX_DATA_BASE_INDEX` → `baseVertex` vs software-pull fetch fold (§3.3). Does
  Metal `baseVertex` add to each index *before* fetch, matching `get_index_from_base`'s masked
  modular add, and is `baseInstance` ever non-zero (RSX instancing uses the lookup table, not
  `baseInstance`)? `[I]`
- **Q6 `[I]`** — Inline-array staging: reuse the existing attribute ring, or a dedicated transient
  ring? Either way the producer/consumer barrier and the ring's per-frame-in-flight scope must be
  stated. `[U]`
- **Q7 `[U]`** — Does a draw-clause boundary always coincide with an encoder boundary, or can
  consecutive clauses share one encoder? This is a pass-scheduler question (`HLD-02`/`HLD-03`) with
  direct TBDR consequences (`10 §7.2`), not a draw-cluster decision.

---

## 7. Provenance

**In-repo RPCS3 (read-only this session)** `[V]`
- `rpcs3/Emu/RSX/NV47/HW/nv4097.cpp` — `set_array_element16:406`, `set_array_element32:415`,
  `draw_arrays:421`, `draw_index_array:429`, `draw_inline_array:437`, `set_vertex_base_offset:460`,
  `set_index_base_offset:465`, `check_index_array_dma:470`, `set_begin_end:485`, `set_render_mode:692`.
- `rpcs3/Emu/RSX/NV47/FW/draw_call.hpp` — `draw_clause:19`, `is_immediate_draw:75`,
  `is_disjoint_primitive:76`, `is_trivial_instanced_draw:79`, `inline_vertex_array:81`,
  `append:102`, `get_elements_count:155`, `min_index:166`, `is_single_draw:177`, `pass_count:206`,
  `begin/end:234-244`, `execute_pipeline_dependencies:296`, `get_subranges:311`.
- `rpcs3/Emu/RSX/NV47/FW/draw_call.cpp` — `insert_command_barrier:23`, `check_trivially_instanced:94`,
  `reset:123`, `get_subranges:141`, `execute_pipeline_dependencies:183`; `draw_call.inc.h:15` barriers.
- `rpcs3/Emu/RSX/Core/RSXDrawCommands.cpp` — `get_raw_index_array:193`, `get_draw_command:214`,
  `append_to_push_buffer:238`, `get_push_buffer_vertex_count:252`, `append_array_element:259`,
  `get_push_buffer_index_count:266`; `RSXDrawCommands.h:50-63`.
- `rpcs3/Emu/RSX/RSXThread.cpp` — `begin:924`, `end:971`, `execute_nop_draw:998`,
  `enable_conditional_rendering:2877`, `disable_conditional_rendering:2901`.
- `rpcs3/Emu/RSX/RSXZCULL.cpp:967-1035` — `conditional_render_eval` (predicate lifecycle).
- `rpcs3/Emu/RSX/rsx_methods.cpp` — dispatch binds `:1627-1633`, `:1686`, `:1713-1716`; defaults
  `:962-963`, `:1005-1006`, `:1147`; restart signal `:784`; parse-only `:1426-1440`, `:1479-1485`.
- `rpcs3/Emu/RSX/rsx_methods.h` — `index_array_location:291`, `index_type:296`, `restart_index:301`,
  `restart_index_enabled:311`, `vertex_data_base_offset:972`, `index_array_address:977`.
- `rpcs3/Emu/RSX/rsx_decode.h:703-723` (`SET_RESTART_INDEX_ENABLE`), `:1370-1390`
  (`SET_RESTART_INDEX`), `:3430-3458` (`SET_INDEX_ARRAY_DMA`).
- `rpcs3/Emu/RSX/gcm_enums.h:1247-1265` (`index_array_type`, `primitive_type`), `:1267`
  (`to_primitive_type`), `:118-127` (`CELL_GCM_PRIMITIVE_*`).
- `rpcs3/Emu/RSX/Utils/rsx_utils.h:514-526` — `get_vertex_offset_from_base`, `get_index_from_base`
  (mask semantics).
- `rpcs3/Emu/RSX/Common/BufferUtils.cpp:330/445/535/567/619` — restart skip + topology expansion.
- `rpcs3/Emu/RSX/VK/VKVertexBuffers.cpp` — index upload/expansion `:103-190`, vertex-base `:224-234`,
  draw-command result `:371-378`; `VK/VKDraw.cpp` — draw dispatch `:1176-1253`, conditional-render
  begin `:1160-1170`, `begin()` guard `:1266`; `VK/VKVertexBuffers.cpp:122` restart emulation.
- `rpcs3/Emu/RSX/VK/vkutils/pipeline_binding_table.h:15` — `conditional_render_predicate_slot = 8`.

**Reference checkouts under `/tmp/metal4-refs/`** `[V]`
- metal-cpp `Metal/MTL4RenderCommandEncoder.hpp:66-79` — `drawIndexedPrimitives` (GPU-address +
  length; `+instanceCount`; `+instanceCount,baseVertex,baseInstance`; indirect) and `drawPrimitives`
  (`vertexStart,vertexCount`; `+instanceCount`; `+instanceCount,baseInstance`; indirect);
  `:84` `setArgumentTable`; `:127` `setVisibilityResultMode`; `Metal/MTLRenderCommandEncoder.hpp:113/121`
  — `DrawPrimitivesIndirectArguments` / `DrawIndexedPrimitivesIndirectArguments`.
- metal-cpp `Metal/MTL4ArgumentTable.hpp:73-80` — `setAddress`/`setResource`/`setTexture`/
  `setSamplerState` (no `setBytes`).

**Sibling research docs (in-repo)** `[V]`
- `docs/research/metal4/10-rsx-native-metal4-architecture.md` §4.5 (draw & submission model),
  §4.3/§4.4 (pass/barrier scheduler), §2.2 (`backend_configuration`), §9 (register deltas, R6).
- `docs/research/metal4/README.md` §3.7 D1–D11 (draw-path mapping), §4 (architectural shifts),
  §6 R6 (conditional render).
- `docs/research/metal4/09-draw-submission-and-batching.md` §3 (draw-path constructs #1–#22), §4
  (batching strategies A–F), §6.2 (predicate patch), §7.2 (the "wash unless batched" verdict).
- `docs/research/metal4/inventory/SCHEMA.md` §4–§6; `inventory/out/SUMMARY.md:106-122`;
  `inventory/out/clusters/draw.csv`; `12-rsx-state-space-inventory.md:166`.
- `design/HLD-05-shader-binding-contract.md` (argument-table slots), `HLD-03` (barriers/residency),
  `HLD-02` (pass scheduler).

<!-- doc-end -->
