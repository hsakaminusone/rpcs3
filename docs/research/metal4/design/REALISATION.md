# Realisation register — RSX → Metal 4

The design layer says what each dial becomes. This register says what code realises
it today, and why some clusters need no new Metal code.

`live` counts the rows whose disposition is `reshape`, `emulate` or `direct`. The
inert rows (`moot`, `impossible`, `na`) need no code.

| cluster | live | C++ unit | status |
|---|---|---|---|
| `surface` | 25 | `MTLSurfaceState` | latched |
| `depth-rop` | 30 | `MTLDepthStencil` | latched, 23 of 30; the depth bounds, ZCULL and `SET_ZSTENCIL_CLEAR_VALUE` need the pass substrate |
| `raster` | 23 | `MTLRasterState` | latched |
| `vertex` | 25 | `MTLVertexState` | slot map and shader key; the streams, the transform constants and the instancing operands are reused |
| `texture` | 13 | `MTLSamplerState` | the sampler; the cache, the format and the program rows need the texture cache and the compiler |
| `blend` | 13 | `MTLBlendState` | latched; the alpha test, the logic op and the shade mode key the shader |
| `draw` | 13 | `MTLDrawState` | the primitive map, the index map and the value state; the lowering is reused |
| `fixed-function` | 3 | none | reuse only |
| `misc` | 3 | none | reuse only |
| `sync-flip` | 17 | `MTLSyncFlip` | the selector state, the alignment rules, the report decode and the release-barrier shape; the label sink and the present need the command queue |
| `nv0039-dma` | 10 | `MTLNv0039Dma` | the transfer descriptor, the validity rule and the visibility point; the byte copy is the shared handler |
| `nv2d-surface` | 7 | `MTLNv2dSurface` | the destination descriptor, the format map and the pitch rule; the copy and the swizzle kernel need the compute encoder |
| `nv3089-image` | 15 | `MTLNv3089Image` | the source state, the format map, the clip rule, the interpolation and the mirror test; the copy needs the compute encoder |
| `nv308a-image` | 6 | `MTLNv308aImage` | the upload state, the store kind, the offset rule and the RGB565 rounding; the host store needs the guest window and the compute encoder |

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

The barrier chokepoint (C3, first half) is the third layer. `MTLBarrier` carries the
producer, consumer and visibility of one edge, decides whether the two sides are
legal inside one encoder, and emits the intra-pass form. A `false` return is a
pass-split request, which the pass scheduler resolves.

The pass scheduler (C3, second half) is its pair. `MTLPassScheduler` derives the
encoder boundaries from the hazards: it tracks the writer and the reader of each
resource in the open pass, queues the barrier that a WAR or RAW edge needs, and
raises a split request when the edge is not legal inside one encoder. It never
opens or closes an encoder. A pass boundary orders every resource, so the touch
sets clear with the pass and not with the frame.

The surface store (C5) is the fourth layer. `MTLSurface` is the Metal projection of
one guest range: an owned, polymorphic `mtl::texture` handle, a resolve target when
the surface is multisampled, and the deferred data movement the generic store asks
for before an access. `MTLSurfaceCache` supplies the trait set of the generic
`rsx::surface_store`; the store itself is not forked (HLD-02 I10).

Two realisations of the surface store are worth recording.

**The surface type derives from the image type.** The `mtl_surface` sketch in
`HLD-02-resource-model.md:411-415` implies containment. Containment cannot work:
`surface_store.h:391` casts an image pointer back to the surface pointer, and
`surface_utils.h:504` cross-casts an image to `rsx::ref_counted`. So `mtl::surface`
derives from `mtl::texture` and from
`rsx::render_target_descriptor<mtl::texture*>`, which is the shape the Vulkan
backend also has. Recorded as `CONFLICTS.md` `X2`.

**A surface has two sample counts.** `get_spp()` is the guest sample count and must
equal `get_format_sample_count(antialias)` (`surface_store.h:646`). The Metal image
sample count follows the video configuration, so an override to one sample leaves
`get_spp()` unchanged. `samples()` is the image sample count, and it decides whether
a resolve target exists.

**Deferred release is per frame slot.** A release is queued on the frame slot that
asks for it and runs when the frame after the next one completes. The frames that
follow a release on the queue may still reference the object, so freeing at the
requesting slot's own completion is not enough.

## The pipeline and binding layer

The pipeline key (C6, first half) is the fifth layer. `MTLPipeline` holds the Metal
backend's `pipeline_properties` — the exact material of one
`MTL4::RenderPipelineDescriptor` — with its equality and its `hash_struct`
specialisation, and assembles the descriptor from the landed state units. The
shared `program_state_cache` owns the cache, so there is no second cache
(HLD-02 I10).

The encoder state (C7) is its companion. `MTLEncoderState` composes the landed
raster, depth-stencil and blend captures into the per-draw dynamic set and writes
it to the open encoder.

The argument binds (C6, second half) are the sixth layer. `MTLArgumentBinds` holds
the RSX shader ABI index map and the per-draw bind values, and `argument_binder`
writes them into the per-stage `MTL4::ArgumentTable`, skipping whatever did not
change. Metal 4 binds by GPU address and resource ID, so this unit needs no
compiled shader.

Three corrections are worth recording.

**The Metal 4 pipeline carries less state than the design assumed.**
`HLD-05-shader-binding-contract.md:331` and `:333` place the cull face, the front
face and the depth and stencil attachment formats in the pipeline.
`MTL4::RenderPipelineDescriptor` has none of them: the cull face, the front face,
the polygon mode, the depth bias and the clip mode are
`MTL4::RenderCommandEncoder` state, and the depth and stencil formats belong to the
render pass. The key therefore holds only `raster_enabled` — a cull-both draw has
no Metal cull mode, so it drops rasterisation — the colour format, the attachment
count, the sample count, alpha-to-coverage, alpha-to-one, the blend latch and the
write masks. The effect is a smaller key and a higher hit rate than the doc
implies. Recorded as `CONFLICTS.md` `C7`.

**The AA control was latched nowhere.** `M_NV4097_SET_ANTI_ALIASING_CONTROL` is
`reshape` in `depth-rop.csv:32` and names `setAlphaToCoverageState` and
`setAlphaToOneState`, yet no unit read it, while `MTLDevice` already advertises
`supports_hw_a2c`. `capture_anti_aliasing_state` now latches both bits and the key
carries them. The per-draw sample mask stays open: it has no Metal encoder setter
and travels in the MSL as `[[sample_mask]]`.

**The two substituted push-constant slots have no settled index.** `HLD-05` §3.2 †
and §6 Q2 leave them open. `MTLArgumentBinds` uses the doc's own stated safe
default, 28 and 29 above the texture range, and says so at the definition.

## The shader layer is blocked on one tool

C6's third part is the program pipeline, and one dependency is missing. The shared
frontend (`Program/`) emits GLSL text and then SPIR-V
(`spirv::compile_glsl_to_spv`, `Program/SPIRVCommon.h:9`). The Vulkan backend
consumes the SPIR-V directly; Metal consumes MSL, and the SPIR-V to MSL step is
SPIRV-Cross. `HLD-05-shader-binding-contract.md:78` calls SPIRV-Cross "a pinned
vendored revision", but it is not in the tree. Recorded as `CONFLICTS.md` `X3`.

The chain stays as HLD-05 §2.3 decides, because the asset sits at the GLSL level
and survives through SPIR-V: the emulation library (`GLSLSnippets/**`, 1,743
lines), `GLSLCommon` (886) and the two decompilers (2,356). Emitting MSL directly
would fork all of it. glslang is already vendored and already serves Vulkan, so
SPIR-V costs the Metal path nothing new, and it is the only stable, diffable seam
the verification loop would have.

<!-- doc-end -->
