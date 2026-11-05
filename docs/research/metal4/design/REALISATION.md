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
| `sync-flip` | 17 | none | reuse only; the guest labels stay CPU-side (`supports_host_gpu_labels` is false) |
| `nv0039-dma` | 10 | none | reuse only; the byte copy is the shared CPU handler |
| `nv2d-surface` | 7 | none | reuse only; the destination is the shared CPU blit |
| `nv3089-image` | 15 | none | reuse only; the scaled copy is the shared CPU handler |
| `nv308a-image` | 6 | none | reuse only; the host store is the shared CPU handler |

## Reuse-only clusters

`fixed-function`, `misc`, `sync-flip`, `nv0039-dma`, `nv2d-surface`,
`nv3089-image` and `nv308a-image` need no new Metal code. Every live row is
realised by shared `Core/`, `Common/` or `Program/` code that the Metal backend
calls the same way the Vulkan and OpenGL backends do. The transfer and sync
engines run the shared CPU handlers, because the guest window is a shared buffer
and the backend declines host GPU labels and passthrough DMA; a Metal-specific
decode of the same registers would only drift.

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

The remaining clusters and the latched-but-unconsumed rows need the same three
missing layers: the surface store, the pass and encoder scheduler, and the pipeline
and descriptor layer. No dial can be exercised end to end before those exist.

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

The output mode is the display-sync setting. The backend floor has one sync state
and no swap interval, so `full` sets the display sync, `off` clears it, and
`adaptive` clears it and paces the present with a minimum present duration read
from the screen's refresh rate.

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
follows the bind is invisible to the draw (`CONFLICTS.md` `X5`). The order is
therefore mutate-then-bind-then-draw, and `argument_binder::bind` returns whether
it wrote any slot so the draw path re-binds the table only when it must. One table
per stage per frame slot is enough: no table ring exists.

**The texture units resolve before the programs.** A draw clause resolves its
referenced units once, in `load_texture_env`, before
`get_current_vertex_program` and `get_current_fragment_program`, because the
builders dereference the resolved descriptors (`RSXThread.cpp:2488`). A unit the
program does not reference is skipped. The unit's image and its sampler are two
entries, because MSL has no combined image sampler.

## Open work

The findings register (`REVIEW-FINDINGS.md`) holds the itemised list of places
where the tree does not yet reproduce the RSX behaviour, with the evidence for
each. Seven passes feed it: the review passes A to E and G, and the first-light
bring-up pass H. Pass E is a second reading of the state-latching cluster, recorded
as the `E` entries.

The blocked set shares one cause: **the pass scheduler was not wired**, so no
hazard was recorded and `emit_barrier` had no caller. The scheduler is now driven
(pass D's F2), and the two items it unblocked have since landed: the
depth-as-texture stencil mirror and the `EQUAL` depth-test input (A3/B1, A4/B2),
and the MSAA depth resolve (B6, see the depth/stencil cluster below).

The unfinished set is the five partials the register lists (E7, E8, E13, F5 and F9);
the register carries each one's remaining half. The depth read-back encode (B16, the
inverse of the B8 decode and its z16 half-to-float step) is now fixed at both the
surface and texture-cache sites, so it is no longer in that set. One finding is open outright: the flat-shading path (G1), because Metal has no
provoking-vertex state, so the RSX last-vertex convention has no native expression.
The items the `REVIEW-FINDINGS.md` "Parked for review" table lists also wait, because
none can be settled from the tree alone: they wait on a hardware measurement, a
Metal 4 behaviour check, or a decision the acceptance harness owns. The logic op
(C1/E6) and the binding-cap set (A9) have both closed and are described below. The PSO
attachment-count key (E3) is closed without a change: the proposed colour-attachment
map bridges a gap the RSX renumbering does not leave. The DMA, image and sync units
(F11) closed by deletion: they were decode-only duplicates of the shared NV47
handlers, and the device reports no passthrough DMA and no host GPU labels.

B16 has its decision, and the wiring still has to wait. The inverse of the depth
decode belongs on the CPU, as `decode_depth_plane` does on the upload, because the
two must stay exact inverses and the encode has no second consumer; a GPU pass
would put the codec in GLSL for one path and would still need the CPU for the
stencil half. The read-back therefore has to reach host memory: the plane copy goes
to a shared staging buffer, the frame commits and waits with the existing readback
wait (B4), and the encode writes the guest pack into the target buffer. Nothing
reaches this path today, so the item is a decision recorded rather than a defect
reachable in the tree.

The logic op (C1/E6) closed by emulation. Metal has no logic-op state, so the
fragment shader reads the destination colour through the framebuffer fetch the
programmable-blend path already owns (`[[color(n)]]`, the `subpassInput` the shared
ROP prologue declares) and applies the op in the epilogue. The capability
`supports_native_logic_op` defaults to true, so Vulkan and OpenGL keep their native
op, and only Metal folds the new `RSX_SHADER_CONTROL_LOGIC_OP` bit and declares
`_ENABLE_LOGIC_OP`. The destination load is unconditional under the op, because the
ROP blend-target bits are written only under programmable blending and the op
replaces blending. The op itself travels in the fragment push-constant block, so a
change of op does not recompile the shader, and the emulation quantizes both colours
to the guest's 8-bit unorm encoding, which is the only encoding in which the op is
defined.

The texture initialisation path (B8) and the combined-format copy (B15) closed. B8
now decodes the guest depth pack into the host plane with
`rsx::get_depth_clear_value`, the transform VK already uses, so no second codec
exists; the combined-format stencil stays marked unknown because the upload
direction has no plane option for a colour source. B15 is a call defect and not a
driver defect: an un-named copy of a combined format is invalid API, and the named
plane form writes and reads back bit-exact on private and on shared storage, so no
Metal 3 blit and no render pass is needed.

A9 blocked first light and is now closed. The Metal 4 shader compiler
caps an MSL `[[buffer(n)]]` index at 30, a `[[sampler(n)]]` index at 15 and a
`[[texture(n)]]` index at 127, and no language version from 3.1 to 4.0 moves the
caps; the argument table descriptor caps its sampler space at 16, the same value.
Four sites sat above a cap. The fill values sat at buffer 51, so the fill library
could not be created and every clear and every output pass was dropped; the slot is
now 30, the last index the buffer cap allows, and the header asserts it. The
texture units took their texture index as their sampler index, so no fragment
shader that sampled a unit and no vertex shader that used a VTF unit could be
compiled; the sampler index is now the unit's position inside its stage's texture
range, and the table writer takes both indices per unit. The table's sampler count
followed the texture count, so the descriptor asked for 51 against the cap of 16
and the validation layer aborted the process on it. The depth mirror and the depth
input also exceeded the cap: Metal has no combined image sampler, so their entries
could not share the 16 unit entries, and the fix is a separate texture fetched
without a sampler. Both are now samplerless — the mirror an integer `utexture2D`
read with `texelFetch`, the depth input a `texture2D` the shared epilogue already
fetched — so the sampler space holds the 16 units and nothing else. The in-tree
GLSL-to-MSL hop has no test, so first light is where the emitted declarations are
first verified.

The device-level ladder outside the tree is green end to end. It reproduces the
caps, and then covers, in order: a clear; a full-screen fill through the argument
table, which is what the new fill slot unblocked; one sampled texture; a depth test
and a depth write; two draws in one pass with a scissored table re-bind, which shows
the encoder captures the argument table when it is bound; a pass split whose surface
is read back through a sampler and carries a pattern no other rung produces, so a
broken read cannot look like a working one; the presenter's copy into a shared
buffer; and a matrix of read-back edges, every row of which is correct. The
ladder's one hard lesson is about residency, not barriers: a resource the queue has
not been told about is not resident, a compute-encoder copy from it writes nothing
while the commit still reports success, and the queue then wedges with a timeout.
Every read-back failure the ladder first showed was that, and the false blame fell
on the barrier form until the residency set went in. The backend's residency
manager is what makes the same shapes work in the tree.

The ladder's cap measurement fixed the target form for the two over-cap
declarations, and a second device probe then ran the whole shape of a playable 2D
frame in one command buffer (`/tmp/rsxframe`): a sprite upload verified with
`getBytes` before rendering, an alpha-blended pass whose arithmetic is exact at three
sample points (`Cs * 0.625`), a depth surface copied to a mirror, a second pass that
reads the samplerless stencil and depth mirrors and re-samples the RTT at a
half-scale offset so its output is distinguishable from the first, a read-back of
both surfaces, and a present. Every check held. Two device constraints came out of
it: a depth-only view of a combined Depth32Float_Stencil8 texture is rejected
(`texture view pixelFormat ... not castable`), so the depth input binds the combined
texture directly and only the stencil view is castable; and `[[position]]` cannot be
declared both in the stage-in struct and as a fragment parameter. The probe also
measured that the explicit producer-to-consumer barrier changed no pixel on this
device for this encoder structure, which is recorded as a measurement and not as a
rule that the barrier can be dropped.

The partial set is the flip preamble (F5), the drawable layer setup (F9) and the
sampler LOD clamp (E8, which implements the clamp and records the bias as a
limitation because Metal has no sampler bias): the fixes closed the present-critical
half and the remainder waits on the present pipeline or the fragment program's
texture contract.

The access-violation result (F8) closed with the transfer pass's readback wait.
The dropped flush was a livelock rather than stale bytes: the page keeps its
protection until the flush releases it, so a fault that was answered without a
flush retried on the same page forever. The handler now queues the flush for the
RSX thread and waits, which is the GL mechanism, and the queue is also where the
sampler flags are raised.

The limitation set is the Metal state the RSX has and Metal does not: point
polygon mode (C4), the per-primitive polygon-offset enables (C7), the line width,
the dither (C10) and the inert PSO-key fields (E12), plus the hardware
conditional-render path (A8), which stays on the CPU skip with its argument slot
reserved and unbound.

The second state pass also closed the cull-both point and line regression (E1,
which rasterised nothing), the `surface_b` null-attachment crash (E2), the
`z16_float` depth target (E4), the extended-depth-range claim (C3/E5), the A2C at
one sample claim (E9), the missing depth-bounds test (E7) and the missing
two-sample gate (E10), and replaced the PSO key's commutative hash (E11).

The surface transfer pass closed the source validation and the covering-copy fast
path in the old-contents drain (B7), the `erase_bkgnd` initialisation (B9, which
was never done with the read-buffers configuration off) and the cyclic-reference
barrier (B10, whose requirement the resource-level hazard record already meets).
It then closed the CPU readback wait (B4): a readback must observe the copy it
encoded, so `command_recorder::flush_for_readback` ends the command buffer, commits
it, waits for that slot's commit feedback and re-enters the same slot, because the
frame owns exactly one slot and the present drawable belongs to the frame's own
submit.

Running the state tests found one more defect, recorded as E14: the depth/stencil
and sampler hashes packed 16-bit GL enum values into three-bit and four-bit fields,
so `keep` and `zero` hashed alike and a distinct sampler state could collide. Both
now fold each field at its own width.

The shadow-compare flag (A7) also closed, on a measurement rather than a code
defect. The native `sampler2DShadow` path can serve the RSX, but only with the
operand order compensated, because Metal compares the reference against the texel
and the RSX compares the texel against the reference. The sampler path now swaps
the directed functions, which is what Vulkan already does. The measurement is in
the register, and the native path is therefore kept.

## Performance testing owed

The review accepted three diagnostics and none is in the tree, on purpose: a
counter on the draw path costs the path it measures, and the first numbers have to
come from a frame with real content rather than from a synthetic probe. They are
recorded here so the decision is not lost.

- **The pass-split count.** How often a hazard cannot be emitted inside the open
  encoder, which is the TBDR feedback case the scheduler carries to the next
  encoder. It is the direct measure of how much the design's barrier placement
  costs, and the scheduler already latches the fact.
- **The heap high-water mark.** The committed shared and private heap bytes, one
  line per heap block. The pools are 32 MiB (shared) and 128 MiB (private), heaps
  are never released until shutdown, and the target machine has 8 GB, so the
  number says whether a long session is close to its budget.
- **A trim policy for the pools.** Once the high-water mark is known: release a
  pool that stayed idle for a whole frame ring, or mark its blocks purgeable. Only
  worth a policy once the mark says the memory is a problem.

The measurements come first. `HANDOFF.md` carries them as an open question for the
first session that runs real content.

## The depth/stencil-as-texture cluster

This was the last blocked group.  The redirected depth fragment unit (A3/B1) and the
emulated depth compare (A4/B2) both read the depth surface from a fragment shader.
A probe on the target device settled every mechanism, so the group is unblocked.

The shared decode fixes the contract.  `ZS_READ(index, coord)` reads the depth plane
and the stencil plane as
`vec2(texture(texN, coord).r, float(texture(texN_stencil, coord).x))`
(`Program/GLSLSnippets/RSXProg/RSXFragmentTextureDepthConversion.glsl:2`,
`...RSXFragmentTextureMSAAOps.glsl:3`), and `TEX_NAME_STENCIL` is a second sampler
per TIU (`RSXFragmentTextureOps.glsl:56`).  The stencil read is unsigned-integer, so
the mirror is a `texture2d<uint>`.  The emulated compare fetches the depth plane with
`texelFetch(frag_depth, ...)` and needs no sampler (`RSXROPEpilogue.glsl:77-82`).

VK satisfies both by binding a second *view* of the same image: the stencil aspect
view with a dedicated nearest/clamp-to-border sampler (`VKDraw.cpp:702-721`) and the
depth aspect view with a null sampler (`VKDraw.cpp:795-800`).  A hardware probe on the
target device (Apple A18 Pro, macOS 27) measured the Metal equivalents.

- **The stencil mirror is a texture view.**  `newTextureView(MTLPixelFormatX32_Stencil8)`
  of the `Depth32Float_Stencil8` surface is a shader-readable `texture2d<uint>` that
  returns the exact stencil bytes, and it is the mechanism MoltenVK uses for a Vulkan
  stencil aspect (`MVKImage.mm:2146-2161`).  `MTLPixelFormatStencil8` is not a
  castable view format and aborts the process, so it must never be requested.  The
  parent needs `MTLTextureUsagePixelFormatView`, which `surface::create` already sets.
- **The depth input is a same-format copy.**  Depth→depth `copyFromTexture` is
  bit-exact for `Depth32Float_Stencil8`, `Depth32Float` and `Depth16Unorm`, so a
  mirror taken at the clause boundary gives the pre-pass snapshot the emulated compare
  needs.  Every clause already gets its own pass (`command_recorder::open_pass` closes
  the open encoder first, `MTLCommandRecorder.cpp:138-140`).
- **The MSL read form is `.read()`.**  `texture2d<float, access::read>` with
  `.read(uint2)` compiles and returns the depth; `texelFetch` is not a valid MSL
  identifier in this toolchain.  The cross-compiler emits `.read()` for the guest
  `texelFetch` (`OpImageFetch`), so the guest form needs no change.
- **A stencil read-back uses the blit option.**  `copyFromTexture … toBuffer …
  MTLBlitOptionStencilFromDepthStencil` yields one byte per texel with the exact
  stencil values, and the depth option yields the depth plane (4 bytes per texel for
  the 32-bit formats, 2 for `Depth16Unorm`).  A combined format requires a non-`None`
  option.  MoltenVK sizes the read-back the same way (`MVKCmdTransfer.mm:1163-1185`).

The same-pass attachment read is **not** rejected by API validation on this device,
for depth or colour, read-only or write-enabled, but it stays formally undefined, so
the depth input keeps the copy.  The stencil view is safe in the redirect case
because the sampled surface is normally not the bound depth attachment.

KosmicKrisp (the Mesa Vulkan-on-Metal-4 driver) independently reaches the same
conclusions, which is the strongest corroboration available because it targets the same
API.  It maps the stencil aspect to `PIPE_FORMAT_X32_S8X24_UINT`
(`kk_image_view.c:103-105`), casts that to `MTLPixelFormatX32_Stencil8`
(`kk_format.c:338`), builds the view with `newTextureViewWithPixelFormat:`
(`mtl_texture.m:77-88`), and reserves `MTLPixelFormatStencil8` for blits only
(`kk_cmd_copy.c:56`) — the same split.  Its read-back uses
`MTLBlitOptionDepthFromDepthStencil` / `MTLBlitOptionStencilFromDepthStencil`
(`kk_cmd_copy.c:50-57`) with `util_format_get_stride` sizing, byte-for-byte the contract
measured here.  Its same-pass split predicate (`kk_cmd_buffer.c:394-408`) splits on a
depth/stencil write→read edge and never on a colour write→read edge, exactly the measured
rule, and it always splits under multisampling (`kk_cmd_buffer.c:467-477`).

The depth input keeps the **copy** and does not adopt KosmicKrisp's pass split.  The split
makes a depth read defined by moving it to a later encoder, which works when the read and
the write are separate commands.  The emulated compare reads and writes the same surface
inside one draw, so a split cannot express it; the pre-pass snapshot is what the RSX
depth test needs.  The copy is the mechanism the split cannot replace.

Multisampling: an MS→MS depth copy is clean, an MS→1-sample blit is rejected
(`sampleCount` must match), and a texture view made from a multisampled source can carry a
channel swizzle, which answers B14.

The MSAA depth resolve **does** use a render-pass store.  KosmicKrisp forces a
meta resolve for depth and stencil because it measured a synchronization bug in
Metal (`kk_cmd_buffer.c:1000-1005`), but the backend floor is macOS 27 and the bug
does not reproduce there.  The 48-round measurement on the floor device is in
CONFLICTS.md X10: a depth render-pass resolve is 48 of 48 reliable with a
**producer** barrier and 46 of 48 with the consumer form, a stencil resolve needs
no barrier, and a combined depth-and-stencil resolve is 32 of 32.  `surface::resolve`
therefore builds the resolve in its own pass — the depth attachment and, for a
combined format, the stencil attachment, both `LoadActionLoad` +
`StoreActionStoreAndMultisampleResolve` with the 1-sample image as the target — and
emits `emit_producer_barrier` (`MTLBarrier.cpp:34`).  The barrier form is
load-bearing: the consumer form leaves a stale window on the clear-only and
draw-written resolve cases, and no barrier is worse.

`StoreActionStore` keeps the multisampled image, so a later write needs no
unresolve, and `framebuffer::open_pass` keeps `StoreActionStore` on the depth and
stencil attachments because a clause pass is not the last writer of that image.

The two depth-as-texture binds are separate, samplerless textures in the MSL layout.
The generated GLSL declares an integer `utexture2D` for the stencil mirror and a
`texture2D` for the depth input, and the Metal resource map gives each a texture with
no sampler, so neither spends a sampler index.  The shared snippets express the
mirror read as `texture()` on a combined sampler, so the Metal side re-defines the
two macros that read it and uses `texelFetch`.  The mirror's own state is
`stencil_mirror_sampler`: nearest, clamp-to-border, opaque black
(`MTLSamplerState.cpp:187-196`), and the out-of-range result of the fetch is zero,
which is what the border gives.

## The macOS 27 floor and the KosmicKrisp workarounds

The backend floor is macOS 27 (`mtl::metal4_api_floor` 26, `offer_floor` 27).  The UI
offers the renderer as "Metal (experimental)" (`rpcs3qt/emu_settings.cpp`, the
`video_renderer::metal` display name), and the renderer list gate hides it on a host
below the floor (`rpcs3qt/settings_dialog.cpp`, `metal4_available`), so an
unsupported host never sees the option.  KosmicKrisp's workaround ledger
(`docs.mesa3d.org/drivers/kosmickrisp/workarounds.html`) is the best available
prior art for what breaks in Metal 4. Every entry that resolved at or before the
floor needs no reproduction here, so the ledger is a filter rather than a
checklist:

| KosmicKrisp workaround | Status on the floor | Applies here |
|---|---|---|
| W6 device coherency | fixed in macOS 27 beta | no |
| W7 stencil `sample_mask` under a depth-only MSAA pass | macOS 26.0.1 | unconfirmed for the RSX; measure before relying on it |
| W8 Rosetta heap-size alignment | fixed as of macOS 27 beta 5 | no |
| W9/W10 loop reconvergence and `bcsel` miscompiles | macOS 26.4.1 | no |
| W11 multiple `MTL4Compiler` heap corruption | resolved in macOS 27 beta 5 | no, and the one-compiler-per-device rule holds anyway |
| W12 index-buffer length register math | resolved in macOS 27 | no |
| W13 16-bit index-buffer out-of-bounds guarantees | open, FB23291220 | only if index-buffer robustness is enabled |
| W14/W17 MSL float-controls NaN folding | macOS 26.5 / 27.0 beta | no |
| W15 volatile / coherent device miscompile | present on macOS 27 only when W6 is disabled | no: the `coherent` qualifier is kept |
| W16 out-of-bounds LOD clamp | macOS 27 beta, M5 only | no: the floor device is not M5 |
| W18 stale reads after a barrier | fixed in macOS 27 | no |

The depth-resolve restriction is not in the ledger and is a KosmicKrisp design
decision rather than a tracked bug; the floor measurement does not reproduce it
(CONFLICTS.md X10). Only W13 and W7 stay live, and each is conditional: W13 needs
index-buffer robustness to be enabled, and W7 needs a measurement against the RSX
before anything relies on the stencil `sample_mask`.

One defect on the floor is neither in the ledger nor a KosmicKrisp decision, and
it is ours to record: the Metal 4 compute encoder accepts a copy into a combined
depth-stencil image and writes nothing, while the Metal 3 blit encoder performs it
(REVIEW-FINDINGS.md, B15). The ledger cannot be a complete filter, because it
covers one driver's reach.

<!-- doc-end -->
