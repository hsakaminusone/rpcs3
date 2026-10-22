# DEVIATIONS-DRAW

Audit of the Metal 4 DRAW / GEOMETRY / VERTEX / INDEX / PROGRAM / ARGUMENT-BINDING path.
The audit compares the Metal 4 backend against the Vulkan backend and the OpenGL backend.

## 1. Method

1. I read the Metal 4 draw path in full: `MTLGSRender.cpp` (`end()` 1528-2038,
   `emit_geometry()` 2040-2640, `draw_command_visitor` 138-309), `MTLDrawState.cpp`,
   `MTLVertexState.h`, `MTLVertexProgram.cpp`, `MTLProgramPipeline.cpp`,
   `MTLArgumentBinds.cpp/.h`, `MTLPipeline.cpp/.h`, `MTLRasterState.cpp`,
   `MTLEncoderState.cpp`, `MTLConstants.cpp`, `MTLCommandRecorder.cpp`, `MTLFramebuffer.cpp`.
2. I read the reference path: `VKDraw.cpp` (full), `VKVertexBuffers.cpp`, `VKVertexProgram.cpp`,
   `VKProgramBuffer.h`, `GLDraw.cpp`, `GLVertexBuffers.cpp`, `GLGSRender.cpp`,
   `ProgramStateCache.h`, `BufferUtils.cpp`, `draw_call.cpp/.h`, `RSXThread.cpp`.
3. I read KosmicKrisp (KK) as a Metal API reference only. I did not treat KK as an RSX
   semantics reference. I read `kk_cmd_draw.c` 760-819, 1080-1219, 1280-1334, 1540-1869,
   `kk_draws.cl` 30-90, `vk_to_mtl_map.c` 220-254.
4. I did not build, run, commit, or push. I changed one file: this one.
5. I followed the Mesa repository policy for `.kk-mesa`. I only read it. I did not edit it,
   and I did not produce Mesa prose or Mesa commit messages.
6. Confirmation rule for the `confirmed?` column:
   - `Yes` = the code path is read and the difference is certain from the code.
   - `Partial` = the code path is certain, but the visible effect needs a run.
   - `No` = the claim needs an on-device measurement.

## 2. Main table

| id | deviation | Metal file:line | VK/GL reference file:line | class | severity | confirmed? | proposed fix |
|---|---|---|---|---|---|---|---|
| D1 | A non-indexed draw is never split into the clause subranges. The whole subdraw goes out as one strip or fan. A `primitive_restart_barrier` inside one clause therefore connects the two parts. | `MTLGSRender.cpp:1983-1994` (subdraw loop), `MTLGSRender.cpp:2635` (`drawPrimitives`, one call) | `VKDraw.cpp:1186-1201` (`get_subranges`, one `vkCmdDraw` per range), `GLDraw.cpp:806-855` (`glMultiDrawArrays`/loop) | wrong result | H | Yes | Read `draw_call.get_subranges()` for every non-single, non-instanced draw. Issue one `drawPrimitives` per range, and add `range.count` to the first vertex each step, as VK does. |
| D2 | An indexed draw is never split into the clause subranges. The whole subdraw goes out as one `drawIndexedPrimitives`. A `primitive_restart_barrier` inside one clause therefore connects the two parts. | `MTLGSRender.cpp:1983-1994` (subdraw loop), `MTLGSRender.cpp:2621-2632` (`drawIndexedPrimitives`, one call) | `VKDraw.cpp:1242-1252` (`get_subranges`, one `vkCmdDrawIndexed` per range), `VKDraw.cpp:1218-1241`, `GLDraw.cpp:878-900` (`glMultiDrawElements`) | wrong result | H | Yes | Read `draw_call.get_subranges()`. Issue one `drawIndexedPrimitives` per range. Advance the index-ring byte offset by `get_index_count(primitive, range.count) * type_size` each step, as GL does at `GLDraw.cpp:890-897`. |
| D3 | For a strip topology with primitive restart disabled and a 16-bit index array, the `0xFFFF` value is a valid vertex index in RSX, but Metal treats it as a strip restart. Metal has no restart-enable state, so the draw cannot turn it off. | `MTLGSRender.cpp:221-228` (index write keeps the guest type), `MTLGSRender.cpp:2621-2632` (`get_index_type` passes `MTL::IndexTypeUInt16`) | `BufferUtils.cpp:546-549` (restart disabled keeps the indices untouched), `BufferUtils.cpp:206`, `BufferUtils.cpp:337-338` (restart enabled rewrites to `index_limit<T>()`) | wrong result | M | Partial | When restart is disabled and the topology is a strip or fan and the index size is below 4 bytes, promote the indices to `u32` in the host index array. Then Metal restarts only at `0xFFFFFFFF`, which RSX cannot reach. KK proves this rule at `kk_cmd_draw.c:1708-1733`. |
| D4 | RSX flat shading is not emulated. `RSX_SHADER_CONTROL_FLAT_SHADING` is never set, so the shader compiles to smooth interpolation. The Metal vertex program already contains the flat path, but nothing reaches it. | `MTLDevice.cpp:312` (`supports_last_provoking_vertex = false`), `RSXThread.cpp:2372-2377` and `2419-2423` (the flag gates the control bit), `MTLVertexProgram.cpp:243-246` (the unreachable flat declaration) | `VKProgramBuffer.h:54-57` and `VKPipelineCompiler.cpp:167-174` (`USE_LAST_PROVOKING_VERTEX`), `GLGSRender.cpp:53-54` (GL defaults to last vertex) | missing feature | M | Yes | Keep the smooth fallback for bring-up, because Metal's provoking vertex is the first and RSX uses the last. A correct fix needs geometry reordering. KK shows the shape at `kk_cmd_draw.c:1821-1830`. This is parked as G1 in `REVIEW-FINDINGS.md:1765`. |
| D5 | The front-face winding is inverted. RSX `front_face::cw` becomes `MTL::WindingCounterClockwise`. Metal uses a top-left window origin, the same as Vulkan, so Vulkan does not invert and Metal should not invert either. KK uses the same negative-height viewport as this backend and does not invert. | `MTLRasterState.cpp:66-74` (inversion), applied at `MTLEncoderState.cpp:63`, viewport at `MTLRasterState.cpp:104-113` | `VKGSRender.cpp:201-210` (no inversion), `GLDraw.cpp:56-63` (inverts, but GL uses a bottom-left origin), KK `kk_cmd_draw.c:1313-1323` with viewport `kk_cmd_draw.c:787-793` | wrong result | H | Partial | Map `front_face::cw` to `MTL::WindingClockwise` and `front_face::ccw` to `MTL::WindingCounterClockwise`. Then run a cull A/B on a title with known back-face culling. The F24 reasoning at `REVIEW-FINDINGS.md:1746-1751` compares against a native rectangle instead of against the Vulkan viewport, so it may count the Y flip twice. |
| D6 | A draw with no enabled vertex attribute returns without draining the clause pipeline dependencies. GL drains them through a NOP draw loop before it returns. | `MTLGSRender.cpp:2095-2099` | `GLDraw.cpp:755-766` (`execute_pipeline_dependencies` loop, then `draw_call.end()`) | diagnostic | L | Yes | Run the GL drain loop before the return, or call `execute_nop_draw()`. |
| D7 | A draw whose render pass cannot open returns without a NOP draw. Every other failure path in the same function calls `execute_nop_draw()`. | `MTLGSRender.cpp:1894-1905` (no NOP draw) | `MTLGSRender.cpp:1546`, `:1559`, `:1827`, `:1870` (all call `execute_nop_draw()`), `VKDraw.cpp:1283-1288` | diagnostic | L | Yes | Call `execute_nop_draw()` before `rsx::thread::end()` on the pass-open failure path. |
| D8 | The conditional-render predicate is declared as argument-table slot 8 and the binder writes it, but no draw sets it and the vertex program has no predicate prologue. | `MTLArgumentBinds.h:26`, `:144`, `MTLArgumentBinds.cpp:89`, `MTLGSRender.cpp:2480-2492` (the field is absent), `MTLVertexProgram.cpp:371-374` (no check) | `VKVertexProgram.cpp:435-442` (predicate prologue), `VKGSRender.cpp:2311-2315` (predicate bind) | not a deviation | L | Yes | No action for correctness. `MTLDevice.cpp:313` sets `supports_hw_conditional_render = false`, so `RSXThread.cpp:952-957` drains the query on the CPU and `should_skip_draw()` skips the draw. The slot is dead ABI. Remove it or wire it if a hardware predicate path lands. |
| D9 | Pipeline creation runs on the RSX thread and blocks. The `compile_async` flag is ignored. | `MTLProgramPipeline.cpp:288` (`(void)compile_async`), `MTLGSRender.cpp:1852-1859` (`compile_async = false`) | `VKProgramBuffer.h:51` (`COMPILE_DEFERRED`), `ProgramStateCache.h:438-446` (the callback model) | performance | M | Yes | Build the pipeline on the worker path and return the placeholder until the callback lands, as VK does. |
| D10 | Every draw clause opens and closes its own render pass. The pass loads and stores the attachments each time. | `MTLGSRender.cpp:1894` (open), `MTLGSRender.cpp:1996-1997` (close), `MTLCommandRecorder.cpp:246-255` (`open_pass` closes the previous encoder) | `VKDraw.cpp:1155-1171` (one render pass, reused across draws) | performance | H | Yes | This is a documented reshape (`REALISATION.md:635`). Keep the pass open across draws that share attachments. Re-open only when the attachment set or the load/store action changes. |
| D11 | An inlined array sets `max_index` to `vertex_count`, not `vertex_count - 1`. The vertex range is then one vertex larger than the stream. | `MTLGSRender.cpp:284` (`result.max_index = vertex_count`) | `VKVertexBuffers.cpp:203`, `:209` (`vertex_count - 1`), `GLVertexBuffers.cpp:141`, `:144` (`vertex_count`) | not a deviation | L | Yes | Metal matches GL. Only VK differs. Keep as is. Note it if the shared code is ever unified. |

## 3. Highest-risk deviations

1. **D5, the winding inversion.** This is the only row that can flip face culling for every
   draw on every title. The Metal window origin is top-left, the same as Vulkan. VK (Vulkan)
   and KK (Vulkan on Metal) confirm that the negative-height viewport does not need a winding
   change. The current code follows GL, which needs a change only because GL uses a bottom-left
   origin. Run the A/B before any other fix.
2. **D1 and D2, the missing subrange split.** These rows break any strip that the guest splits
   with a `primitive_restart_barrier`. The two parts connect across the barrier. The fix is small
   and it mirrors the reference code exactly.
3. **D3, the 16-bit strip restart.** This row breaks a draw only when a valid vertex index equals
   `0xFFFF` and restart is off. It is rare, but it is a silent wrong result. The fix is a host-side
   promotion and it costs one copy of the index array.
4. **D10, the pass per clause.** This row is not a correctness fault, but it is the largest
   performance cost in the path. Each clause pays a full attachment load and store.

## 4. KosmicKrisp table

| id | KK file:line | what KK does | valid for RSX? | why | how we would apply it |
|---|---|---|---|---|---|
| K1 | `kk_cmd_draw.c:1708-1733` (`requires_unroll_index_promotion`) | If restart is off, the topology is a strip or fan, and the index size is below 4 bytes, it unrolls and thereby promotes the indices to `u32`. This stops a valid index from being read as a restart. | Yes | RSX uses the same strip restart rule, and Metal has the same fixed restart index. The RSX semantic is exactly the KK case. | Adopt. Promote the index array to `u32` in the host writer when `!restart_index_enabled()` and the topology is a strip or fan and the index size is below 4 bytes. This is the fix for D3. |
| K2 | `kk_cmd_draw.c:1790-1819` (`requires_unroll_restart`) | If restart is on and the guest restart index is not the native all-ones value, it unrolls. | Yes | The rule is valid for RSX, but RSX already satisfies it. `BufferUtils.cpp:546-563` rewrites every enabled restart to `index_limit<T>()`, which is the Metal all-ones value. RSX lists never restart, so the list case does not apply. | No action. Keep the shared writer as the single place that normalizes the restart index. Add a test that pins this contract. |
| K3 | `kk_cmd_draw.c:1821-1830` (`requires_unroll_flatshade`), source at `kk_draws.cl:75-90` | If the last vertex is provoking and the fragment shader uses flat inputs, it unrolls the geometry to move the provoking vertex first. | Yes | The RSX provoking vertex is the last vertex, and Metal's is the first. The semantic is valid for RSX. | Defer. The unroll needs a full geometry expansion, which the design parks as G1 (`REVIEW-FINDINGS.md:1765`). Adopt it only for a title that flat-shades, and start with a triangle list. |
| K4 | `kk_cmd_draw.c:1645-1670` (`kk_sanitize_index_range`) | Metal rejects an index address or length of zero. KK substitutes a cached zero index buffer. | No, only Vulkan | This is a Vulkan binding-model problem. The Metal 4 path passes the index array as a draw argument and allocates it from a ring with a nonzero address (`MTLGSRender.cpp:2627`). An empty draw returns early at `MTLGSRender.cpp:2110-2113`. | Reject. No RSX case reaches a null index buffer. |
| K5 | `kk_cmd_draw.c:1853-1866` (`upload_base_vertex`) | It uploads `baseVertex` to a buffer for a shader that reads it. | No, only Vulkan | This is a KK shader-ABI choice for tessellation. The Metal 4 path keeps the base index in the draw-parameter block and the shared vertex fetch applies it (`MTLGSRender.cpp:2241`), which is also what VK does. | Reject. It duplicates a contract the shared vertex fetch already owns. |
| K6 | `kk_cmd_draw.c:1550-1600` (`kk_unroll_geometry`) | It converts a draw to an indirect draw and rewrites it with a compute kernel. It is the vehicle for all the unroll rules. | No, only Vulkan | RSX has no indirect draw and no tessellation. The technique needs an indirect command stream that this backend does not build. | Reject for now. Reconsider only if K3 is adopted, because K3 needs the same rewrite engine. |
| K7 | `kk_cmd_draw.c:1672-1706` (`kk_dispatch_draw`) | It guards an empty draw, because Metal rejects a zero vertex or instance count. | Yes | The rule is correct for Metal, but the Metal 4 path already returns early on an empty draw. | Accept as already covered. `MTLGSRender.cpp:2110-2113` returns when `vertex_draw_count == 0`. |

## 5. KosmicKrisp adoption shortlist

1. **K1, adopt now.** It is the direct fix for D3. It is a host-side change in the index writer,
   and it needs no shader change.
2. **K2, keep as a pinned contract.** No code change. Add a test that asserts that an enabled
   restart becomes the all-ones value for every strip topology.
3. **K7, no action.** The guard already exists.
4. **K3, conditional.** Adopt only for a flat-shaded title, and only behind a capability flag.
   The cost is a full geometry expansion. This matches the parked item G1.

## 6. Rejected KosmicKrisp ideas

1. **K4, the null-index sanitizer.** Reject. It is a Vulkan binding-model workaround. The Metal 4
   path never binds a null index buffer, and it guards an empty draw.
2. **K5, the base-vertex upload buffer.** Reject. It is a KK shader-ABI choice. The Metal 4 path
   already applies the base index in the shared vertex fetch, the same as VK.
3. **K6, the indirect unroll engine.** Reject for now. RSX has no indirect draw and no tessellation.
   Adopt it only as the vehicle for K3, if K3 is adopted.

## 7. Notes that are not deviations

1. **D8, the conditional-render slot.** Metal uses the CPU skip path, so the missing predicate
   bind is not a correctness fault. The slot is dead ABI.
2. **D11, the inlined-array `max_index`.** Metal matches GL. Only VK subtracts one.
3. **The primitive expansion set.** `MTLDrawState.cpp:25-54` maps fan, quad, polygon, and line loop
   to native Metal types through the shared writer. This matches `VKVertexBuffers.cpp:15-43` and
   `GLGSRender.cpp` through `BufferUtils.cpp:816-871`.
4. **The vertex base and the index rebase.** The Metal path passes `baseVertex = 0` and keeps the
   base in the draw-parameter block (`MTLGSRender.cpp:2241`, `:2630`). This matches VK
   (`VKDraw.cpp:1216`, `vkCmdDrawIndexed` with vertex offset 0).
5. **The pipeline-properties key.** `MTLPipeline.h:26-59` keys the color format, the attachment
   count, the sample count, alpha-to-coverage, alpha-to-one, the raster-enable flag, the blend
   cluster, the write mask, and the topology class. `MTLProgramPipeline.cpp:233-274` masks the
   write mask by the fragment output mask, the same as `VKProgramBuffer.h:34-41`. I found no
   missing key field.

<!-- doc-end -->
