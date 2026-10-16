# Realisation register — RSX → Metal 4

The design layer says what each dial becomes. This register says what code realises
it today, and why some clusters need no new Metal code.

`live` counts the rows whose disposition is `reshape`, `emulate` or `direct`. The
inert rows (`moot`, `impossible`, `na`) need no code.

| cluster | live | C++ unit | status |
|---|---|---|---|
| `surface` | 25 | `MTLSurfaceState` | latched |
| `depth-rop` | 30 | `MTLDepthStencil` | latched, 22 of 30; the AA control, the depth bounds, ZCULL and `SET_ZSTENCIL_CLEAR_VALUE` need the pass substrate |
| `raster` | 23 | `MTLRasterState` | latched |
| `vertex` | 25 | `MTLVertexState` | slot map and shader key; the streams, the transform constants and the instancing operands are reused |
| `texture` | 13 | `MTLSamplerState` | the sampler; the cache, the format and the program rows need the texture cache and the compiler |
| `blend` | 13 | `MTLBlendState` | latched; the alpha test, the logic op and the shade mode key the shader |
| `draw` | 13 | `MTLDrawState` | the primitive map, the index map and the value state; the lowering is reused |
| `fixed-function` | 3 | none | reuse only |
| `misc` | 3 | none | reuse only |
| `sync-flip` | 17 | `MTLSyncFlip` | the selector state, the alignment rules, the report decode and the release-barrier shape; the label sink and the present need the command queue |
| `nv0039-dma` | 10 | none | not started |
| `nv2d-surface` | 7 | none | not started |
| `nv3089-image` | 15 | none | not started |
| `nv308a-image` | 6 | none | not started |

## Reuse-only clusters

`fixed-function` and `misc` need no new Metal code. Every live row is realised by
shared `Core/`, `Common/` or `Program/` code that the Metal backend calls the same
way the Vulkan and OpenGL backends do.

**`fixed-function`.**

- `SET_FOG_MODE`, `SET_FOG_PARAMS` → the `fragment_context_t` payload of
  `draw_command_processor::fill_fragment_state_buffer` (`Core/RSXDrawCommands.cpp:665-716`),
  bound at slot 3. The equation is MSL text.
- `SET_TWO_SIDE_LIGHT_EN` → the fragment-program cache key
  (`Program/ProgramStateCache.cpp:727`), which already separates the two variants.
- `SET_SPECULAR_ENABLE` is `moot`; nothing reads it.

**`misc`.**

- `SET_CONTROL0` → the depth format of `MTLSurfaceState`
  (`MTLSurfaceState.cpp:49`, `surface_depth_fmt()`), so the `surface` unit owns it.
- `SET_FREQUENCY_DIVIDER_OPERATION` → the vertex-layout modulo bit and index mask of
  `draw_command_processor::analyse_inputs_interleaved` (`Core/RSXDrawCommands.cpp:62,122`).
- `INVALIDATE_L2` → the shared `pipeline_state::fragment_program_ucode_dirty` bit
  (`Core/RSXDriverState.h:9`, consumed at `RSXThread.cpp:2230-2235`).

## Substrate

The remaining clusters and the latched-but-unconsumed rows need the same four
missing layers: the surface store, the pass and encoder scheduler, the pipeline and
descriptor layer, and the label sink with its command queue. No dial can be
exercised end to end before those exist.

The resource layer (C5) is the first layer in place. `MTLResourceManager` holds a
placement heap pool per storage class, a suballocator with a free list, the explicit
residency sets, and the deferred-release list. It is the gate for the surface store
and for the texture cache. The device layer also imports the guest window as one
no-copy buffer, so a guest address is a buffer offset.

The command recorder (C4) is the second layer. `MTLCommandRecorder` owns a ring of
three frame slots, each with one allocator, one reusable command buffer, and one
argument table per stage. It is the only way to open a pass, close an encoder, or
submit, so the recorded stream is one chokepoint.

<!-- doc-end -->
