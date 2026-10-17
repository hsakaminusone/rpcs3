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

**The argument table was too small for its own map.** The first shape was 16
buffers, 28 textures and 28 samplers, taken from the fixed RSX map's own range.
It did not cover the map's highest indices: the substituted slots are buffers 28
and 29, and the derived fragment range reaches texture 50. A bind past the
table's maximum is not a diagnostic — the slot is simply not there. The shape is
now derived from the map (`argument_buffer_count = fragment_blend_slot + 1`,
`argument_texture_count = fragment_depth_input_slot + 1`), so a slot added to the
map can no longer overflow the table in silence. The map also gained the three
derived fragment slots that the fragment decompiler had been keeping as private
constants, so the ABI has one home.

## The shader layer

C6 is the RSX ucode to MSL seam, and it is one chain (HLD-05 §2.3 route A):
RSX ucode -> GLSL text -> glslang -> SPIR-V -> SPIRV-Cross `CompilerMSL` -> MSL
-> `MTL4::Compiler`. `MTLShader` holds the second half, `MTLProgramPipeline`
holds the seam and the pipeline cache.

The chain stays as HLD-05 §2.3 decides, because the asset sits at the GLSL level
and survives through SPIR-V: the emulation library (`GLSLSnippets/**`, 1,743
lines), `GLSLCommon` (886) and the two decompilers (2,356). Emitting MSL directly
would fork all of it. glslang is already vendored and already serves Vulkan, so
SPIR-V costs the Metal path nothing new, and it is the only stable, diffable seam
the verification loop has.

`HLD-05-shader-binding-contract.md:78` calls SPIRV-Cross "a pinned vendored
revision", but it was not in the tree. It is vendored now, as plain files on the
model 06 used for metal-cpp, at commit `300681bbcb8365a8d1eacdacdbdd0249bbf1d6b1`
(Apache-2.0), with its provenance in `3rdparty/spirv-cross/RPCS3_VENDORING.md`.
The six translation units build into `3rdparty_spirv_cross_impl` under
`-w -Wno-error`, because the upstream code is not ours and it is not built with
the project's warning set. This closes `CONFLICTS.md` `X3`.

**HLD-05 §2.4 asks for three MSL post-passes. None is needed.** The section is
written against the assumption that SPIRV-Cross cannot express three RSX features
in MSL. It can, and the seam does not have to guess at MSL text:

1. Programmable blending. SPIRV-Cross has the native path, and it is one option:
   `CompilerMSL::Options::use_framebuffer_fetch_subpasses` turns a subpass input
   into a `[[color(i)]]` fragment input and its read into the value itself
   (`spirv_msl.cpp:15699-15718`, `:16522-16526`). The option defaults to false,
   which is why the first draft of this unit tried to rewrite the MSL by text.
   The seam sets the option and the rewrite is gone. A text pass here would also
   have been fragile: it keyed on `texture2d<float>` and missed the
   multisampled form, and it found the input attachment by searching for the
   first `.read(` in the module.
2. Push-constant substitution. `add_msl_resource_binding` already sets the MSL
   buffer index from the substituted slot, so the emitted declaration is
   `[[buffer(28)]]` or `[[buffer(29)]]` directly (HLD-05 §3.3). A text pass on the
   block name is a second mechanism for one job.
3. Border colour and LOD bias. The LOD bias is already handled where it belongs,
   in the emulation library: the decompiler reads the per-unit `sampler_info` and
   the `TEX*_BIAS` ops apply it (`GLSLSnippets/RSXProg/RSXFragmentTextureOps.glsl`).
   The border colour is a real gap and stays open: RSX takes an arbitrary RGBA
   border, and Metal's sampler offers only four fixed border colours
   (`MTL::SamplerBorderColor`). Baking the colour into the MSL would also be
   wrong, because the program cache is keyed by the program and the border colour
   is per texture unit and per draw. The fix belongs in the shared
   `sampler_info` buffer, next to the LOD bias, and it changes a header the
   Vulkan backend also reads, so it is recorded rather than taken here.

**The seam is wired.** `MTLProgramPipeline` calls the two decompiler subclasses
(`MTLVertexProgram`, `MTLFragmentProgram`) for the RSX ucode -> GLSL hop, then
`spirv::compile_glsl_to_spv`, then `spv_to_msl`, then `compile_msl`. The
decompilers emit Vulkan-rule GLSL 450 and take their argument indices from
`MTLArgumentBinds.h`, so a program's MSL indices are the RSX ABI numbers.

**The four doc-vs-tree conflicts.** These change no design decision, so they are
citation conflicts (`CONFLICTS.md` `X4`):

1. `HLD-05` §3.4 names `MTL4::Library` as the shader object. metal-cpp returns
   `MTL::Library*`, because `MTL4::Compiler` produces the Metal 3 object.
2. The same paragraph names `MTL4::RenderPipelineState`; the tree returns
   `MTL::RenderPipelineState*`, for the same reason.
3. `HLD-05` §3.5-§3.6 name `build_program` and an `ExtraData` block. The tree has
   `build_pipeline` and no such block, and the pipeline key is the
   `program_state_cache` `pipeline_properties` (`ProgramStateCache.h:165`), so no
   second cache exists (HLD-02 I10).
4. `pipeline_binding_table.h` is inert for this backend. The Vulkan backend fills
   the binding table at run time (`prepareBindingTable()`); the Metal argument
   table is fixed and baked into the MSL, so the Metal path declares its indices at
   compile time and needs no table.

**The remaining risk.** SPIRV-Cross is built with
`SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS`, and the project builds with
`-fno-exceptions`. A malformed guest shader therefore aborts RPCS3 instead of
failing one pipeline. A pre-validate or a subprocess guard is owed before the
backend is offered to users.

## The presenter

C7. No pixel reaches the screen without it. `MTLPresenter` owns the CAMetalLayer
and the present path; `MTLMetalLayer` is the AppKit half, and it is the only
Objective-C++ unit of the backend. The renderer attaches it to the Qt NSView that
`GSFrameBase::handle()` returns.

**The drawable carries the source resolution.** A CAMetalLayer scales its drawable
to its own bounds, so the drawable is sized to the RSX output and the present is
one copy and no scale pass. A drawable the size of the client area would need a
GPU scale or a cropped copy. The presenter sets the size on the first present and
on every size change, so the layer needs no resize call. The cost is that the
display pipe, not the GPU, does the scaling, and that `CAMetalLayer`'s default
`contentsGravity` is `resize`: a source whose aspect ratio differs from the window
is stretched rather than letterboxed, and a Retina window upscales the drawable.
Both are the overlay composite pass's job (the aspect mode and integer scaling
live there), not the presenter's.

**The present stays on the one submit path.** Metal 4 requires a wait for the
drawable before it is written and a signal before it is presented, and both are
queue operations. The queue belongs to the recorder, so
`command_recorder::set_present_drawable` does the wait immediately and `submit()`
does the signal after the commit. The presenter does not commit; a second commit
path would break the frame ring.

**The copy goes through a compute encoder.** Metal 4 has no separate blit
encoder. `MTL4::ComputeCommandEncoder::copyFromTexture` is the copy, so the
present needs no new encoder kind.

**The flip source is the surface at the flip address.** `MTLGSRender::flip` looks
it up with `rsx::surface_store::get_surface_at(info.buffer)` and asks for the
memory-read form. A frame whose surface is absent, or whose drawable pool is
empty, presents nothing: the frame is dropped and not presented late.

**A copy needs a matching destination.** `copyFromTexture` is a straight copy, so
both sides must agree on the sample count and the format. The presenter checks
both and drops the frame when they do not match, and it takes the layer's pixel
format from the source when the layer can scan it out. A multisampled display
surface therefore needs its resolve target, and an exotic format needs a
conversion pass; neither is a copy.

**The presenter is not the frame owner.** `begin_frame` and `submit` stay in
`flip`, so a dropped present still advances the ring and the recorder stays the
only owner of the frame lifecycle.

The mechanism is complete and wired. It cannot yet produce a content pixel,
because no draw is executed and the surface store is therefore empty: that needs
the execution spine (C1).

## The dial tests

The Python suite checks the inventory contract. It does not run C++ and it does
not touch a dial. The C++ suite does: `rpcs3/tests/test_rsx_metal4_dials.cpp`.

**The mapper layer.** Seven property tests. A mapper from an RSX enum to a Metal
enum must be injective on the values RSX can produce, and the collapsed
equations must collapse only where the design says. The assertions are
properties, not restated tables.

**The latch layer.** One test for each of the 51 live dials that a capture unit
reads. Each test writes the register, reads the latch and checks the field. Each
test registers itself as it is defined, so the registry cannot drift from the
test set, and a per-cluster ratchet asserts that the covered set is a subset of
the live set and never falls below a floor.

**The exhaustive layer.** One test for each of the 1,337 dials of the inventory:

| list | dials | what the test proves |
| --- | --- | --- |
| registers | 282 | the id names a real constant, and the constant's word is the recorded address |
| values | 768 | the constant's value is the recorded value |
| facet enums | 58 | the same, for the shader enums |
| facet opcodes | 70 | `rsx_fp_op_names[i]` spells the recorded mnemonic |
| facet bitfields | 129 | the width from an all-ones word, and the shift from a one-bit word |
| facet fields | 18 | the member exists, at compile time |
| binding slots | 12 | the Metal index is the RSX ABI number |

The list is `rpcs3/tests/rsx_metal4_dial_list.inc`, generated by
`inventory/tools/gen_dial_list.py`. The generator verifies each name against the
header that the inventory cites, and the compile verifies it again, so a name
that the header does not define is a build error and not a silent gap. Each list
also has a coverage test that reads its CSV and fails if an inventory row is
absent from the list. A dial cannot drop out in silence.

The fixture starts from `rsx_state::reset()` (`rsx_methods.cpp:797`). An all-zero
register file is not a legal RSX state: a getter that maps a word to a GCM enum
goes through `gcm_enum_cast`, which returns an `expected`, and the conversion to
the enum verifies (`gcm_enums.h:1188`). `NV4097_SET_FRONT_FACE` zero is one such
word, so a zeroed fixture stops on a valid dial.

## The execution spine

C1 is the `rsx::thread` execution path. The backend objects come up in
`on_init_thread` and go down in `on_exit`, in reverse order, and `flip` presents
the frame. This unit lands the frame lifetime, the clear path and the draw path.

**The framebuffer is the pass descriptor.** Metal has no framebuffer object, so
`MTLFramebuffer` holds the bound attachment set — the colour targets and the depth
stencil the store bound — and builds the `MTL4::RenderPassDescriptor` for each
pass. The store still owns the surfaces: it reuses a match, splits an overlap and
allocates when there is none. The set is re-walked only when the layout changes,
and an unchanged layout is recognised by the address, pitch, format and target
fields, not by the serialization tag, which changes with every store. A
multisampled colour attachment resolves on store, so the resolved image is the
samplable one.

**The clear is a load action, or a fill.** A clear that covers its whole
attachment is `MTLLoadActionClear` and costs no draw. Metal has no scissored
clear, so a clear that does not cover its attachment is a full-screen triangle
confined by the scissor: `MTLFill` is that pipeline, and it is the Metal form of
`vkCmdClearAttachments`. Its values reach the fragment stage through the argument
table, because Metal 4 has no `setBytes`, and the ABI carries one slot for it
(`fill_values_slot`). `clear_covers_attachment()` decides which form applies, so
the decision stays the surface cluster's and not this unit's.

**The depth-stencil object is the only carrier of the depth state.**
`MTL4RenderCommandEncoder` has `setDepthStencilState` and no compare-function and
no write-flag setter, so the latch must be built into a cached object and applied
(`CONFLICTS.md` C7). The fill applies its own two objects — a write one and a read
one, both with an always-passing compare, because a clear must not be rejected by
the state it is clearing. The draw path applies the latched object through the
same cache.

**The scissor is cached.** `rsx::thread::get_scissor` returns false both for an
unchanged region and for an invalid one, and it fills the region only in the first
case, so the caller caches the rectangle and reads the flag to tell the two apart.
The clear asks for the unclipped form, which is the rule the draw context uses in
the Vulkan backend (`VKGSRender.cpp:2618`).

**The draw path.** `end()` runs before `rsx::thread::end()`, which owns the
post-execute cleanup, and it drives the whole clause: the programs, the pipeline,
the pass and the encoder state are per clause, and each subdraw uploads its own
vertex streams, fills its own ABI blocks and dispatches. `emit_geometry` is the
per-subdraw half, so the split matches the clause's own iteration
(`draw_call.begin()` / `next()` / `end()`).

**The pipeline is the shared program cache's object.** The Metal PSO key *is*
`pipeline_properties`, so no second cache exists and the key is exactly the
pipeline's inputs. The key holds the colour format, the attachment count, the
sample count, the A2C/A2one pair, the collapsed `MTL::PrimitiveTopologyClass`, the
whole `blend` latch and the per-attachment write masks — and nothing else. Cull
mode, front face, fill mode, depth clip mode, depth bias, the viewport, the
scissor, the stencil references, the blend colour and the depth-stencil object all
live on the encoder (`CONFLICTS.md` C7), so they cost no pipeline object.

**The vertex streams are `R8Uint` texture buffers.** One view spans a whole
per-slot ring, so a draw's data is a starting offset inside `attrib_data`, not a
new view, and the view is rebuilt only when the ring grows. The shared GLSL's byte
addressing therefore works unchanged: one texel is one byte, so no MSL post-pass
and no byte expansion exists. The ring is per frame slot, and a growth within a
frame retires the old pair to the deferred-release sink.

**Every per-draw value lives in its own ring block.** A Metal 4 command buffer
does not snapshot a buffer's contents at encode time, so the draw-parameter block,
the vertex context, the fragment context, the texture parameters, the stipple
pattern, the fragment literals and the substituted push-constant words are all
written into the slot's rings and never read back. A block the shader indexes by
offset travels as an index; a block the shader reads at offset zero (the two
substituted push constants) is bound at its own address. The transform-constant
file is the one exception: it is gated on the shared
`transform_constants_dirty` flag and its 468-vec4 upload happens only when the
file changes.

**Argument tables are snapshotted at bind time.** A probe confirmed that
`MTL4RenderCommandEncoder::setArgumentTable` captures the table, so a write that
follows the bind is invisible to the draw. The order is therefore
mutate-then-bind-then-draw, and `argument_binder::bind` returns whether it wrote
any slot so the draw path re-binds the table only when it must. One table per
stage per frame slot is enough: no table ring exists.

**The texture units resolve before the programs.** A draw clause resolves its
referenced units once, in `load_texture_env`, before
`get_current_vertex_program` and `get_current_fragment_program`, because the
builders dereference the resolved descriptors (`RSXThread.cpp:2488`). A unit the
program does not reference is skipped. The unit's image and its sampler are two
entries, because MSL has no combined image sampler.

## Open work

The findings register (`REVIEW-FINDINGS.md`) holds the itemised list of places
where the tree does not yet reproduce the RSX behaviour, with the evidence for
each. The blocked set shares one cause: **the pass scheduler is not wired**, so
`emit_barrier`'s pass-split request has no resolver, and the depth-as-texture
stencil mirror (A3/B1), the `EQUAL` depth-test input (A4/B2), the texture
barrier (B10) and the MSAA depth resolve (B6) all wait on it. The open set is the
surface initialisation and transfer path (B4, B7, B8, B9, B14), the logic op
(C1), the extended depth range (C3) and the two shader-feature flags (A7, A8).
The limitation set is the Metal state the RSX has and Metal does not: point
polygon mode (C4), the per-primitive polygon-offset enables (C7), the line width,
the depth bounds and the dither (C10).

<!-- doc-end -->
