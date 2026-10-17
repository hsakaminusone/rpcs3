# Review findings register

Three review passes read the Metal 4 implementation against the RSX behaviour.
The method is fixed: the RSX is the source of truth, and the Vulkan backend, the
OpenGL backend, the shared `Emu/RSX/Common` and `Emu/RSX/Core` code and the RSX
headers are evidence for what the RSX does. A finding is recorded when the tree
does not reproduce the behaviour the evidence shows.

The passes were:

- **A — the shader and ABI contract.** `MTLConstants.{h,cpp}`,
  `MTLArgumentBinds.{h,cpp}`, `MTLVertexProgram.{h,cpp}`,
  `MTLFragmentProgram.{h,cpp}`, `MTLProgramPipeline.{h,cpp}`, `MTLShader.{h,cpp}`.
- **B — the surface and texture cache.** `MTLSurface.{h,cpp}`,
  `MTLSurfaceCache.{h,cpp}`, `MTLSurfaceState.{h,cpp}`,
  `MTLResourceManager.{h,cpp}`, `MTLTextureCache.{h,cpp}`, `MTLHandles.{h,cpp}`,
  `MTLFormats.{h,cpp}`, `MTLFramebuffer.{h,cpp}`, `MTLFill.{h,cpp}`.
- **C — the state latches and the clear path.** `MTLRasterState.{h,cpp}`,
  `MTLDepthStencil.{h,cpp}`, `MTLBlendState.{h,cpp}`, `MTLVertexState.{h,cpp}`,
  `MTLDrawState.{h,cpp}`, `MTLEncoderState.{h,cpp}`, `MTLPipeline.{h,cpp}`.
- **D — synchronisation, DMA, images and presentation.**
  `MTLCommandRecorder.{h,cpp}`, `MTLBarrier.{h,cpp}`, `MTLPassScheduler.{h,cpp}`,
  `MTLPresenter.{h,cpp}`, `MTLSyncFlip.{h,cpp}`, `MTLNv0039Dma.{h,cpp}`,
  `MTLNv2dSurface.{h,cpp}`, `MTLNv3089Image.{h,cpp}`,
  `MTLNv308aImage.{h,cpp}`, `MTLFill.{h,cpp}`, `MTLDevice.h`,
  `MTLAvailability.h`, `MTLMetalLayer.{h,mm}`, and the sync, flip and
  `open_frame` region of `MTLGSRender.{h,cpp}`.
- **E — the state-latching cluster, a second reading.** `MTLPipeline`,
  `MTLBlendState`, `MTLDepthStencil`, `MTLRasterState`, `MTLSamplerState`,
  `MTLDrawState`, `MTLEncoderState`, `MTLVertexState`, `MTLSurfaceState`,
  `MTLFormats`, `MTLFramebuffer`. The pass numbered its findings `F1..F12`, which
  collides with pass D, so they are recorded as `E1..E12`; `E5` and `E6` are the
  pass's duplicates of `C3` and `C1`.

Line references are as of the review pass; every entry also names the function,
which does not move. A finding that a fix closed is marked **Fixed**; a finding
that a subsystem a fix needs is missing is marked **Blocked**; a finding a fix
reduced but did not close is marked **Partial**; a finding that records an
accepted deviation is marked **Limitation**.

## Summary

| ID | Severity | Area | Status |
|---|---|---|---|
| A1 | High | vertex transform constants | Fixed |
| A2 | High | instanced-constant stride | Fixed |
| A3 | High | depth/stencil mirror bind | Fixed |
| A4 | Medium | depth-compare input bind | Fixed |
| A5 | Medium | transform-constant ring size | Fixed |
| A6 | Medium | `block_ring` growth | Fixed |
| A7 | Medium | `emulate_shadow_compare` | Fixed |
| A8 | Low | `conditional_render_predicate` | Limitation |
| B1 | High | depth/stencil mirror | Fixed |
| B2 | High | depth-compare input | Fixed |
| B3 | High | transfer-read barrier | Fixed |
| B4 | High | CPU readback sync | Fixed |
| B5 | High | `copy_texture` AA region | Fixed |
| B6 | High | depth resolve path | Fixed |
| B7 | Medium | `flush_old_contents` region | Fixed |
| B8 | Medium | `load_background` texel size | Open |
| B9 | Medium | `erase_bkgnd` init | Fixed |
| B10 | Medium | `insert_texture_barrier` | Fixed |
| B11 | Medium | `DEPTH16_FLOAT` map | Fixed |
| B12 | Medium | border colour SNORM | Fixed |
| B13 | Medium | deferred-release slot | Fixed |
| B14 | Low | MSAA swizzle view | Fixed |
| B15 | High | combined-format copy no-op | Open |
| C1 | High | logic op | Open |
| C2 | High | two-sided stencil ref | Fixed |
| C3 | High | extended depth range | Fixed |
| C4 | Medium | point polygon mode | Limitation |
| C5 | Medium | PSO key alpha fields | Fixed |
| C6 | Medium | dead vertex latch | Fixed |
| C7 | Low | polygon-offset enables | Limitation |
| C8 | Low | duplicate scissor build | Fixed |
| C9 | Low | `capture_draw_state` | Limitation |
| C10 | Low | dither | Limitation |
| F1 | Critical | present source lookup | Fixed |
| F2 | Critical | pass hazards | Fixed |
| F3 | Critical | fill values buffer | Fixed |
| F4 | High | multisampled present source | Fixed |
| F5 | High | flip preamble | Partial |
| F6 | High | split barrier loss | Fixed |
| F7 | High | cross-encoder barriers | Fixed |
| F8 | High | access-violation result | Fixed |
| F9 | Medium | drawable layer setup | Partial |
| F10 | Medium | present output pipeline | Fixed |
| F11 | Medium | DMA/image clusters | Fixed |
| F12 | Medium | drawable acquire block | Fixed |
| E1 | High | cull-both point/line | Fixed |
| E2 | Critical | `surface_b` attachment | Fixed |
| E3 | Low | PSO attachment-count key | Won't fix |
| E4 | High | `z16_float` depth format | Fixed |
| E5 | High | extended depth range | Duplicate of C3 |
| E6 | High | logic op | Duplicate of C1 |
| E7 | Medium | depth bounds test | Fixed |
| E8 | Medium | sampler LOD bias | Partial |
| E9 | Medium | A2C at 1 spp | Fixed |
| E10 | Low | 2-sample capability | Fixed |
| E11 | Low | PSO key hash | Fixed |
| E12 | Low | key completeness | Limitation |
| E13 | Low | latch dataflow and dead fields | Open |
| E14 | Medium | state-latch hash truncation | Fixed |

Totals: 59 findings, 43 fixed, 3 partial, 0 blocked, 4 open, 6 limitations, 2 duplicates, 1 won't fix.

## A — the shader and ABI contract

### A1 — the vertex transform constants are read unrelocated. Fixed.
The decompiler rewrites every constant read to a compact index
(`Program/VertexProgramDecompiler.cpp:414-435`), and `_fetch_constant` reads
`vc[x + xform_constants_offset]` (`Program/GLSLSnippets/RSXProg/
RSXVertexPrologue.glsl:93-97`), so GL and VK upload the compacted array
(`GLGSRender.cpp:1159-1171`, `VKGSRender.cpp:2370-2380`). The tree discarded the
relocation table and uploaded the full 468-vec4 file, so a non-indexed program
sampled the wrong register. `mtl::vertex_program` now derives from
`rsx::VertexProgramBase`, carries `constant_ids` and `has_indexed_constants`, and
the upload applies the table.

### A2 — the instanced-constant table stride did not match the shader. Fixed.
The shared helper strides the instance tables by
`CONSTANTS_ARRAY_LENGTH = has_indexed_constants ? 468 : size32(constant_ids)`
(`RSXDrawCommands.cpp:754-756`), and VK passes the compiled program so it matches
(`VKGSRender.cpp:2168`). The tree passed `nullptr`, which forces a full re-upload
and the wrong stride for a non-indexed program. The draw path now passes
`frame.compiled_vertex`.

### A3 — the depth-texture stencil mirror is never bound. Fixed. Duplicate of B1.
A redirected depth fragment unit also binds the stencil aspect
(`VKDraw.cpp:702-721`), which the shared decode reads (`Program/GLSLSnippets/
RSXProg/RSXFragmentTextureOps.glsl:106-108`). The tree declared the second
sampler but nothing produced or bound it, so a redirected depth texture decoded
garbage. A device probe confirmed the fix is a `MTLPixelFormatX32_Stencil8`
texture view of the depth surface, shader-read as `texture2d<uint>`, which is what
MoltenVK uses (`MVKImage.mm:2146-2161`); a copy is not needed. `MTLPixelFormatStencil8`
is not a castable view and aborts. `mtl::texture::stencil_view` now produces the
view and `load_texture_env` binds it (see B1).

### A4 — the depth-compare input is never bound. Fixed. Duplicate of B2.
The front-end sets `RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE` for an `EQUAL`
depth test (`RSXThread.cpp:2200-2204`) and the epilogue does
`texelFetch(frag_depth, ...)` (`Program/GLSLSnippets/RSXProg/
RSXROPEpilogue.glsl:77-82`). VK binds the depth-aspect view at
`frag_depth_input_location` (`VKDraw.cpp:795-800`). The tree declared the input
under the same flag but never wrote the slot. A device probe confirmed a
same-format depth→depth copy is bit-exact, so the fix is a mirror taken at the
clause boundary and bound at `fragment_depth_input_slot`; `surface::mirror_depth`
and `MTLGSRender::end` now do it (see B2).

### A5 — the transform-constant ring was sized in elements, not blocks. Fixed.
The header defines `transform_capacity` as the number of changes a frame may hold,
and one change is a 468-vec4 file, but `init` passed the capacity as a count of
vec4, so one block filled the ring and every change grew and retired a buffer.
`init` now passes `transform_capacity * vertex_constant_count`.

### A6 — `block_ring::alloc` could write past its capacity. Fixed.
Growth allocated a replacement buffer of the same capacity, so a block larger
than the capacity wrote past the end. Growth now takes
`max(m_capacity, count)` and the ring grows to fit the block.

### A7 — `emulate_shadow_compare` is forced false. Fixed.
GL and VK derive it from device depth-format support
(`VKFragmentProgram.cpp:405,636`). The tree forced it false because the native
compare sampler was not measured against the RSX. It is now measured, and
`emulate_shadow_compare` stays false: the native path is correct once the operand
order is compensated.

Metal's `sample_compare` evaluates `reference OP texel`; the RSX evaluates
`texel OP ref` (`comparison_passes` is called as `comparison_passes(texel, ref)`,
`RSXFragmentTextureOps.glsl:106`, `RSXFragmentPrologue.glsl:137-149`). Native
nearest returns exactly 0.0 or 1.0 for all eight functions, so the four directed
functions invert and the four directionless ones do not: with a stored depth of
0.5 and a reference of 0.25, native `less` is 1.0 where the RSX is 0.0. That is 32
mismatches over 72 samples, and 0 of 72 once the sampler swaps the directed
functions — which is the compensation `VKDraw.cpp:32-45,409` already applies. The
sampler path therefore calls `get_compare_function(zfunc, true)` and the depth and
stencil tests keep the direct order, because they compare the incoming value
against the stored one.

Three further measurements settle the surrounding behaviour. A linear compare
sampler is a per-texel compare followed by the filter, not a filtered 0/1: a 2x2
depth of {0.2, 0.4, 0.6, 0.8} sampled at uv (0.5, 0.5) returns exactly 1.0, 0.75,
0.5, 0.25 or 0.0 for four, three, two, one or zero passing texels. The compare
runs on the texels of the selected mip level, not on level 0. Equality is
inclusive for `_or_equal` and exclusive for the strict functions, and the reversal
does not change that: each side's pass set is the other's complement.

### A8 — the hardware conditional-render path is not implemented. Limitation.
`conditional_render_predicate_slot` is reserved in the binding map but has no
producer and no consumer: `argument_binds::conditional_render_predicate`
(`MTLArgumentBinds.h:93`) is never assigned by the draw path, so the binder's
`address(...)` call (`MTLArgumentBinds.cpp:82`) never writes it (a zero value is
skipped), and no MSL declares a read at the slot. That is consistent with
`cfg.supports_hw_conditional_render = false` (`MTLDevice.cpp:137`), which makes
the shared front end take the CPU skip (`RSXThread.cpp:939`), so the RSX
conditional render is reproduced without the hardware path. Enabling the flag
needs both ends: a producer for the predicate word and a shader that tests it.

## B — the surface and texture cache

### B1 — the depth-texture stencil mirror. Fixed. Duplicate of A3.
The cache side owns it: `get_view` makes only a colour view and
`resolve_texture_bind` returns one `texture_bind`. `mtl::texture::stencil_view` now
returns the `X32_Stencil8` view (the base delegates a sampled view to its root), and
`surface` overrides it to build and cache the view. `load_texture_env` fills
`fragment_stencil[id]` for each redirected unit, with the dedicated
`stencil_mirror_sampler` (the unit's sampler may be a comparison sampler), and
`argument_binder` binds it at `fragment_stencil_slot_base + id` (30..45) in both the
texture and sampler spaces, because the MSL layout is a combined image sampler.

### B2 — the depth-compare input. Fixed. Duplicate of A4.
`resolve_texture_bind` carries no depth view. The source is the bound depth surface
(`framebuffer::depth()`), which the clause-boundary mirror makes samplable:
`surface::mirror_depth` copies the surface into `depth_mirror()` in its own encoder
with a queue-scope edge on both sides, `end()` binds the result at
`fragment_depth_input_slot` (50), and the pass opens with a
`StageBlit`→`StageFragment` queue barrier for the read.

### B3 — `write_render_target_to_memory` skipped the read barrier. Fixed.
VK calls `surface->read_barrier(cmd)` before the copy (`VKRenderTargets.h:643`).
Without it an MSAA surface is read before its resolve and a pending
`old_contents` flush is skipped, so the DMA write-back returned stale bytes. The
barrier is now taken first.

### B4 — the CPU readback has no completion wait. Fixed.
`cached_texture_section::map_synchronized` returned the staging pointer as soon as
the copy was encoded, but the copy executes at commit, so every readback was
undefined. GL waits a fence (`GLTextureCache.h:432`); VK's `map_synchronized`
returns null, so its readback is dead and is no precedent. A Metal 4 command buffer
is the unit of submission, so the copy can be observed only after a commit:
`copy_texture` records the recorder in the section, and `map_synchronized` calls
`command_recorder::flush_for_readback`, which ends the command buffer, commits it,
waits for that slot's commit feedback, then resets the allocator and re-enters the
**same** slot. The slot is re-entered rather than advanced because the frame owns
exactly one slot, and the present drawable is not consumed, so only the frame's own
submit presents.

### B5 — `copy_texture` over-ran the resolve image for an AA surface. Fixed.
The region was scaled by `samples_x`/`samples_y` before the blit, but a Metal
multisample image is resolved in place and is pixel-sized. VK scales because its
image really is sample-sized and it has a typeless resize when the sizes differ
(`VKTextureCache.h:241-261`); Metal has no such path, so the source region
exceeded the texture. The scaling is removed.

### B6 — an MSAA depth surface is resolved through a colour attachment. Fixed.
`surface::resolve` set the depth texture as `colorAttachments()->object(0)` with a
resolve texture, and the framebuffer layer never gave the depth attachment a
resolve target. VK resolves the depth aspect directly (`VKRenderTargets.cpp:1034`).
A device probe confirms an MSAA→1-sample blit is rejected (`sampleCount` must
match), so a blit cannot resolve it either.

KosmicKrisp forces meta resolves for depth and stencil from the outset, on the
premise that a Metal producer barrier does not order a depth or stencil
render-pass resolve. It introduced that bail-out in `db5ab8de` (2026-07-23) with
the comment "There appears to be a synchronization bug in Metal", has not
revisited it since, and files no Metal ticket for it: it is a baked-in decision,
not a tracked Apple bug. The project's own 48-round measurement on the macOS 27
floor (Apple A18 Pro, macOS 27.0.1) does not reproduce it. A depth render-pass
resolve is 48 of 48 reliable with a **producer** barrier and 46 of 48 with the
consumer form, so the producer form is what the edge needs; a stencil resolve
needs no barrier at all, and a combined depth-and-stencil resolve is 32 of 32.
The measurement is the basis of the macOS 27 floor decision (CONFLICTS.md).

`surface::resolve` now builds the resolve in its own pass: the depth attachment
and, for a combined format, the stencil attachment, both `LoadActionLoad` +
`StoreActionStoreAndMultisampleResolve` with the 1-sample image as the target. The
pass emits a producer barrier (`emit_producer_barrier`), so later queue work
observes the resolved image. `StoreActionStore` keeps the multisampled image, so a
later write needs no unresolve — which retires the illegal 1-sample-to-N-sample
blit that `unresolve` used to do. The `StoreActionStore` on the depth and stencil
attachments in `framebuffer::open_pass` stays, because a clause pass is not the
last writer of the multisampled image.

### B7 — `flush_old_contents` used the raw region. Fixed.
VK transforms both rectangles with `transform_pixels_to_samples` and skips a
source that fails `test()` when the copy is not accept-all
(`VKRenderTargets.cpp:1049-1054,:1073-1077`). Metal has no sample transform to
apply (B5), so only the second half was live: the tree copied the raw rectangles
and never called `test()`, so a dirty source was blended into the target. The
transfer now computes `accept_all = last_use_tag && test()` and skips a source
whose `test()` fails, and it detects a covering copy so the background is neither
loaded under it nor restored over it. A run in which every source was skipped
re-initialises the surface instead of keeping stale contents
(`VKRenderTargets.cpp:1118-1125`), and the write is recorded with
`on_write_copy(newest_tag, optimize_copy)`.

### B8 — `load_background` uses the guest texel size. Open: the mechanism is measured.
The guest z24s8 surface is 4 bytes per texel and its host image is
`Depth32Float_Stencil8`, 8 bytes per texel, so a raw `copyFromBuffer` cannot be
correct: the destination row exceeds the source row stride, and Metal validates
`sourceBytesPerRow >= destination row bytes`. The mismatch is confined to depth:
`get_surface_color_format` keeps the guest block size for every colour format, so
colour rows match. The depth formats split three ways — `z16_uint` →
`Depth16Unorm` (2 bytes, matches), `z16_float` → `Depth32Float` (4 vs 2), and
`z24s8_uint`/`z24s8_float` → `Depth32Float_Stencil8` (8 vs 4)
(`MTLFormats.cpp:44-61`; `TextureUtils.cpp:1376-1388`).

The device answered all three questions, so the fix has a determined mechanism.

1. A blit may read and write a depth texture, but a combined format is
   conditional. `Depth16Unorm` and `Depth32Float` accept a plain copy in both
   directions. `Depth32Float_Stencil8` rejects `MTLBlitOptionNone` in both
   directions — and rejects the no-option overload as well — with
   `option (MTLBlitOptionNone) is not a valid MTLBlitOption for … pixel format
   MTLPixelFormatDepth32Float_Stencil8`, so a combined copy must name its plane.
2. One copy carries exactly one plane. `MTLBlitOptionDepthFromDepthStencil`
   transfers the depth plane at 4 bytes per texel and
   `MTLBlitOptionStencilFromDepthStencil` the stencil plane at 1 byte per texel;
   a depth copy and a stencil copy in one command buffer place each plane in its
   own buffer with no cross-contamination. The depth plane keeps a colour row
   rule (`>= width * 4`, a multiple of 4), the stencil plane takes
   `>= width * 1`, and `Depth16Unorm` takes `>= width * 2` at a multiple of 2.
   The depth read-back is bit-exact: the option returns `0000003f` at every texel
   for a clear depth of 0.5.
3. The stencil plane of a combined image cannot be uploaded. It is addressable
   in both directions only as an explicit `StencilFromDepthStencil` copy, and the
   upload direction has no plane option for a colour source, so the guest stencil
   cannot reach the image. The stencil therefore stays marked unknown
   (`stencil_init_flags |= (1 << 9)`, the marker `on_write_copy` already sets)
   rather than left cleared.

The fix is therefore a depth-plane decode into a staging buffer (24-bit unorm or
IEEE half to float32) followed by a blit that names the plane, gated to the
formats whose byte sizes differ.  It cannot reuse the format-aware colour upload
VK uses, because VK's host depth format keeps the guest texel size
(`VKRenderTargets.cpp:695-800`).  The write path must also avoid the Metal 4
compute-encoder copy, which does not perform a combined-format write at all (B15).

Reachability: the path runs only when `needs_cpu_upload()` holds, that is with
`read_depth_buffer` enabled or `force_data_load` set, so it is off by default.

### B9 — `erase_bkgnd` was never initialised. Fixed.
VK clears the surface and marks it ready when the read-buffers configuration is
off (`VKRenderTargets.cpp:817-828`) and calls `initialize_memory` on the
`erase_bkgnd` flag (`VKRenderTargets.cpp:1000-1009`), with the `ready` and
`on_write` bookkeeping. The tree acted only when `needs_cpu_upload()` was true,
which it is not with the config off, so a partially written surface kept
undefined contents and stayed permanently dirty. `memory_barrier` now calls
`initialize_memory` whenever `erase_bkgnd` is set; that function loads the
background when `needs_cpu_upload()` holds and otherwise opens a clear-load pass
over the surface (and over the resolve target when it is read), marks the surface
ready, and the caller records the write with `on_write`.

### B10 — `insert_texture_barrier` is empty. Fixed by the pass scheduler.
VK emits an image barrier on the render target (`VKTextureCache.cpp:1368-1377` →
`VKRenderTargets.cpp:882`) and GL a texture barrier
(`GLTextureCache.h:875-877`); the base calls the hook whenever a sampled texture
overlaps a written surface (`Common/texture_cache.h:2555,2582,2773`). The tree
leaves the hook empty. The requirement is met elsewhere: the hazard it expresses
is the cyclic reference, the same resource being written as an attachment and
read as a texture, and the F2 scheduler records exactly that pair
(`MTLGSRender::record_pass_hazards` notes the referenced units as reads and the
attachment set as writes, keyed by `MTL::ResourceID`) and emits the ordering at
the encoder boundary, before the sampling pass encodes its draws. An edge that
cannot be emitted inside the open encoder is carried to the next one
(`split_requested`/`emit_split_barriers`), which is the treatment a TBDR feedback
loop needs. The hook is therefore left empty by design; VK's `strong_ordering`
flag and its barrier-per-loop tracker are its own deduplication, which the
per-frame record supplies here.

### B11 — `DEPTH16_FLOAT` mapped to a unorm depth. Fixed.
VK maps it to `VK_FORMAT_D32_SFLOAT` (`VKFormats.cpp:225`) and GL to
`GL_DEPTH_COMPONENT32F` (`GLTexture.cpp:98`). The tree mapped it to
`PixelFormatDepth16Unorm`, so a float depth was reinterpreted and compared wrong.
It now maps to `PixelFormatDepth32Float`.

### B12 — the border colour ignored the SNORM expansion. Fixed.
VK calls `remapped_border_color(sext_conv_required)` where
`sext_conv_required = (texel_remap_control & SEXT_MASK) != 0`
(`VKDraw.cpp:412-413`), which applies the RSX SNORM re-expansion
(`RSXTexture.cpp:368-446`). The tree passed no flag. The fragment sampler now
passes `argb_signed() != 0`.

### B13 — the deferred release was one slot early. Fixed.
A release is queued on the slot of the frame that asks for it, and
`on_commit_complete(index)` released `m_deferred[(index + 1) % N]`. The frame on
slot `index + 1` may still be in flight, so objects it still references were
freed. The release now drains `m_deferred[index]`, the slot whose frame just
completed.

### B14 — an MSAA texture drops the sample swizzle. Fixed.
`get_view` returned the texture itself when `sample_count() > 1`, so a multisample
texture with a non-identity remap sampled in native order. A device probe confirmed
a texture view made from a multisampled source can carry a channel swizzle
(`newTextureView(pixelFormat, type, levelRange, sliceRange, MTL::TextureSwizzleChannels)`),
and that a view cannot change the texture type. `get_view` now builds the swizzled
view for a multisample texture as well, with the source's own
`TextureType2DMultisample`, so the remap stays at the view.

### B15 — the Metal 4 compute encoder does not write a combined depth-stencil image. Open.
A `copyFromBuffer` into a `Depth32Float_Stencil8` texture on
`MTL4::ComputeCommandEncoder` is accepted by the validation layer and reports no
command-buffer error, and it writes nothing: the destination keeps its cleared
value with an explicit `MTL4` barrier, in private and in shared storage, read back
by both a Metal 4 and a Metal 3 path. The same copy on `MTL::BlitCommandEncoder`
performs the write and is bit-exact. Copies into `Depth32Float` and `Depth16Unorm`
are performed by the compute encoder, so the defect is confined to the combined
format. `surface::load_background` (`MTLSurface.cpp:357`), the texture upload
(`MTLTextureCache.cpp:61`) and the read-back (`MTLTextureCache.cpp:142`) all use
the compute encoder, so every combined-format transfer the backend encodes is
unaffected today only because no such transfer is reached yet. The workaround is a
render pass or a Metal 3 blit encoder for a combined-format copy; the choice waits
on whether an ordering rule between the two encoder families is needed.

## C — the state latches and the clear path

### C1 — the logic op has no implementation. Open: mechanism identified, design decided.
The RSX logic op is a real operation: VK bakes it into the pipeline
(`VKGSRender.cpp:322-326`) and GL enables `GL_COLOR_LOGIC_OP`
(`GLDraw.cpp:199-201`). Metal has no logic-op state — neither
`MTL4::RenderPipelineDescriptor` nor `MTL4::RenderCommandEncoder` exposes one — so a
logic-op draw is currently a plain write. Pass E6 re-confirms it: the operation is
latched and keyed and then never consumed, and `MTLBlendState.cpp:111` disables
blending when it wins, so the draw writes plainly with no diagnostic.

The mechanism that reads the destination colour in a Metal 4 fragment shader is the
`[[color(n)]]` input, and the tree already owns it: the programmable-blending path
declares `layout(input_attachment_index = n, set = 0, binding = 46 + n) uniform
subpassInput frag_src_n` and reads it with `subpassLoad`
(`RSXROPPrologue.glsl:53-77`; `MTLFragmentProgram.cpp:124-132`), and
`MTLShader.cpp:78` sets `use_framebuffer_fetch_subpasses`, so SPIRV-Cross lowers the
subpass input to an MSL `[[color(n)]]` parameter. The logic op needs no encoder
state and no binding of its own: the same declaration, plus the op applied in the
epilogue.

Design, not landed:
- A capability `backend_configuration::supports_native_logic_op` (default true), set
  false in `MTLDevice.cpp` beside the other capability flags.
- `rsx::thread` folds a synthesized `RSX_SHADER_CONTROL_LOGIC_OP` into
  `current_fragment_program.ctrl` when `logic_op_enabled()` holds and the capability
  is false — the `RSX_SHADER_CONTROL_ALPHA_TEST` precedent (`RSXThread.cpp:2442`).
  The bit goes in the enum's "Custom" range (`gcm_enums.h:457`).
- The shared ROP prologue declares and loads `mrt_colorN` under `_ENABLE_LOGIC_OP`,
  and the epilogue applies the op where it applies blending. The load must **not**
  reuse the programmable-blending gate `IS_MRT_BLEND_ENABLED(n)`, because the ROP
  blend-target bits are written only under `RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING`
  (`Core/RSXDrawCommands.cpp:693-696`) and a logic-op guest need not enable blending.
- The op is a uniform in the fragment push-constant block, so changing the op does
  not recompile the shader.
- Scope: the emulation quantizes source and destination to the guest's 8-bit unorm
  encoding. Vulkan forbids `logicOpEnable` on a floating-point attachment and GL
  leaves the result undefined for a non-fixed-point framebuffer, so a float target
  has no logic op to reproduce.

Not landed in this pass: no test in the tree reaches the GLSL-to-MSL hop, so the
emulation cannot be verified before first light, and it would otherwise land into a
programmable-blending path that has never executed. The fix follows first light.

### C2 — the stencil reference ignored the single-sided case. Fixed.
A single-sided draw applies the front reference to both faces
(`GLDraw.cpp:148-156`; VK uses `VK_STENCIL_FRONT_AND_BACK` with
`stencil_func_ref()`, `VKGSRender.cpp:351-355`). The tree always passed
`back_stencil_func_ref()` as the back reference. `capture_stencil_references` now
returns the front reference for the back face when the two-sided test is off.

### C3 — the extended-depth-range guard forces a `[0,1]` viewport. Fixed.
The backend advertised `supports_extended_depth_range`, so
`requires_depth_range_emulation()` was false (`RSXThread.cpp:2218-2226`) and the
vertex shader compressed Z. The viewport must then re-expand to `[clip_min,
clip_max]`, but the tree fell back to `[0,1]`, which stores the compressed Z on a
float target. The pass's second option — pass an out-of-range `znear`/`zfar`
through — is impossible: Metal validates `MTLViewport.znear/zfar` to [0,1] and
has no unrestricted depth range, which the project's own `LLD-raster` §3.2 and
invariant 4 already state ("Out-of-[0,1] clip values never reach
`MTLViewport.znear/zfar`; the shader transform owns the general case"). The
capability is now false, so the shader zclip/depth-range transform runs and the
`[0,1]` viewport is the mapping the shader produces, matching VK without
`unrestrictedDepthRange` (`VKGSRender.cpp:1305-1320`; pass E5 reached the same
conclusion from the other direction).

### C4 — the point polygon mode collapses to a fill. Limitation.
GL expands points (`GLDraw.cpp:70-73,:314-316`); Metal has no point polygon mode,
so the tree collapses `polygon_mode::point` to `TriangleFillModeFill` and a
point-mode draw renders filled. Reproducing it needs a point-expansion pass.

### C5 — the PSO key carried runtime alpha state. Fixed.
`alpha_func` and `alpha_ref` are fragment-context uniforms read at run time
(`Core/RSXDrawCommands.cpp:684-685,718`; `MTLFragmentProgram.cpp:433-435`), so
keying the PSO on them created an unbounded number of pipeline objects for an
animated reference. Both are removed from the hash and from the equality; VK does
not key on them either. The latch still decodes them, because the dial test
asserts that decode.

### C6 — the vertex latch and the attribute check were dead. Fixed.
`capture_vertex_state` and `attribute_sourced` had no caller, and the real
attribute decode is the shared `fill_vertex_layout_state`
(`Core/RSXDrawCommands.cpp:290`). The struct and both functions are removed, so
the unit holds only the slot map its consumers read.

### C7 — the polygon-offset enables are collapsed. Limitation.
The RSX has independent point, line and fill offset enables
(`rsx_methods.h:501-512`). The tree gates the bias on the fill enable alone.
Metal applies the bias per primitive class and has no separate point or line
enable, so the line case can only be approximated.

### C8 — two divergent scissor constructions. Fixed.
`build_scissor_rect` had no caller and the draw path built the scissor inline
from the same source. The draw path now calls `build_scissor_rect`, so the
`x2 - x1` contract has one owner.

### C9 — `capture_draw_state` is not consumed by the draw path. Limitation.
The dial test asserts it (`tests/test_rsx_metal4_dials.cpp`), so the decode is
verified, but the draw path reads the shared `draw_command_processor` visitor
instead. This matches GL and VK, so the latch is the decode proof rather than
hot-path state.

### C10 — the guest dither is dropped. Limitation.
`SET_DITHER_ENABLE` has no Metal state, and VK drops it too; GL reproduces it
(`GLDraw.cpp:81`). The deviation is accepted and recorded here.

## D — synchronisation, DMA, images and presentation

### F1 — the present source is looked up with a display-buffer index as a guest address. Fixed.
`MTLGSRender.cpp:988` calls `cache.get_surface_at(info.buffer)`, but
`get_surface_at` takes an RSX address (`Common/surface_store.h:1164`), and
`info.buffer` is the display-buffer index `0..7`. VK and GL resolve the flip
source through `rsx::get_address(display_buffers[info.buffer].offset,
CELL_GCM_LOCATION_LOCAL)` (`VKPresent.cpp:551`, `GLPresent.cpp:217`). No surface
was ever found, so `presented` stayed false and nothing reached the drawable: the
present path could not work at all. `flip` now bounds-checks
`info.buffer < display_buffers_count`, computes the address with
`rsx::get_address(display_buffers[info.buffer].offset, CELL_GCM_LOCATION_LOCAL)`,
and looks that up.

### F2 — no hazard barriers are emitted anywhere. Fixed.
`MTLPassScheduler.{h,cpp}` and `MTLBarrier.{h,cpp}` are included only by each
other, so `note_write`/`note_read`/`flush` are never called; the heaps are
`HazardTrackingModeUntracked` (`MTLResourceManager.cpp:40`), so the driver
inserts nothing either. The RSX needs RAW/WAR/WAW ordering between passes and
before the presenter's copy (HLD-03 §2.4, §3.4). The fix is to have the draw path
record every resource it touches and call `flush` at each encoder boundary. This
is the same structural gap as the earlier blocked set: the scheduler is written
but has no caller. The scheduler is now driven: the record survives across passes
within a frame keyed by `MTL::ResourceID`, `begin_frame` is called from
`open_frame`, `MTLGSRender::record_pass_hazards(bool)` is called from `end()` and
from both `clear_surface` pass sites, and `MTLPresenter::present` emits the
queue-scope barrier before the copy, so `note_read`/`note_write`/`flush` all have
callers.

### F3 — one shared fill-values buffer serves every fill. Fixed.
`MTLFill.cpp:122` allocated one persistent `m_values`; `:252` wrote the CPU
values into it and `:263` bound it, but a Metal command buffer captures the
buffer, so every fill in the frame read the values of the last fill written. VK
and GL carry the clear values per command (`VKPresent.cpp` draws with
`vkCmdClearAttachments`; GL with `glClear*`), so each fill is independent. The
fill values are now one block of a per-frame-slot ring (`MTLConstants.h`
`fill_values`, `alloc_fill_values`), and each fill packs its own block, so no two
fills share storage.

### F4 — a multisampled source is requested and then rejected. Fixed.
`MTLGSRender.cpp:990` used `get_surface(memory_read)`; `is_transfer()` is false,
so `MTLSurface.cpp:9-11` returned the multisampled surface itself, and
`MTLPresenter.cpp:59` dropped anything whose `sampleCount() != 1`. VK and GL
resolve the render target and hand the single-sample image to the present
(`VKRenderTargets.cpp:839`, `VKPresent.cpp:349`, `GLPresent.cpp:95`). An MSAA
frame presented nothing. The flip now takes a transfer access, which resolves the
surface first, so the present always reads one sample per pixel.

### F5 — the flip ignores the guest present state. Partial.
`MTLGSRender.cpp:977` never read `info.skip_frame` (`VKPresent.cpp:495`), never
read `display_buffers[info.buffer].width/height/pitch` (`VKPresent.cpp:513-515`),
never consulted `avconfig` for the format, the gamma or the stereo mode
(`VKPresent.cpp:532-541`), and never handled the stereo second eye
(`VKPresent.cpp:560-580`). A skipped frame was still presented and the wrong
extent could be used. The flip now drops a skipped frame and takes the extent
from the display buffer, with the AV configuration as the fallback and the cap.
The pitch, the format and the stereo eye are still not consumed, because the
present resolves the source by address and the drawable takes the source size;
the stereo second eye stays a deviation.

### F6 — `flush` loses a barrier that requests a split. Fixed.
`MTLPassScheduler.cpp:147-155` set `m_split` when `emit_barrier` returned false,
but still ran `m_pending.clear()`, so the edge was discarded; `split_requested()`
(`MTLPassScheduler.h:44`) had no caller. A barrier that cannot be emitted inside
an encoder must survive as a cross-pass edge. `flush` now retains the specs it
cannot emit, `begin_pass` no longer clears them, and `emit_split_barriers` emits
them on the encoder the caller opens after the split.

### F7 — the barrier chokepoint cannot express a cross-encoder edge. Fixed.
`MTLBarrier.cpp:19` only ever called `barrierAfterEncoderStages`, which orders two
sides of one encoder, and `barrier_spec` (`MTLBarrier.h:13-18`) had no
before-queue or after-queue stage fields. A release/acquire edge that must span
encoders — and `MTLSyncFlip.h:78`'s `before_queue_stages` — had nowhere to go
(HLD-03 §3.2). `MTL4::CommandEncoder` carries the queue-scope form, so the
chokepoint now also exposes `emit_queue_barrier`, which calls
`barrierAfterQueueStages`. `MTLPassScheduler` uses it through
`emit_split_barriers`. The wiring that produces the edges is F2.

### F8 — `on_access_violation` drops the flush and sampler result. Fixed.
`MTLGSRender.cpp:1053-1061` inspected only `violation_handled`; `num_flushable`
and `invalidate_samplers` were ignored. VK and GL flush the dirty sections back
to guest memory and mark the samplers dirty (`VKGSRender.cpp:1028,1041-1102`,
`GLGSRender.cpp:1289-1307`).

The dropped flush was worse than stale bytes. The fault runs on the guest thread
that touched the page (`Utilities/Thread.cpp:1596`), and the invalidation leaves the
page protected until the flush releases it: `unprotect_set` runs only on the path
that `flush_all` takes, and a deferred cause returns before it. The handler
returned true, so the faulting instruction was retried on a page that was still
read-only, faulted again, and took the same path — a livelock for any guest write
into a page holding a clean, flushable section.

The tree now mirrors the GL mechanism. A fault records the invalidation and posts
the result to a queue; the waiting thread raises `backend_interrupt`,
`vm::temporary_unlock()` and spins on the request's `processed` flag
(`GLGSRender.cpp:1298-1307,1385-1396`). The RSX thread drains the queue in
`do_local_task`, which is also the release point the base calls on exit, opens the
frame when the fault arrived between frames, and calls
`texture_cache::flush_all`. `flush_for_readback` (B4) makes the read-back observe
its own copy, and `unprotect_set` then releases the page.

A request is posted even when nothing is flushable, because the sampler flags are
the RSX thread's to write: `m_textures_dirty` is a plain `std::array<bool, 16>`
(`RSXThread.h:249`) that the draw path reads, so the faulting thread must not
write it. That also removes a data race the previous handler had. The deadlock
guard is the same one GL uses: a semaphore wait drains the queue first, because the
RSX thread can otherwise sleep on a label while the faulting thread waits for a
flush only the RSX thread can run.

### F9 — the drawable layer is configured incompletely and sized from the source. Partial.
`MTLMetalLayer.mm:31-36` and `:44-54` never set `maximumDrawableCount`,
`displaySyncEnabled`, `allowsNextDrawableTimeout` or `colorspace`, and
`drawableSize` is set from the source resolution (HLD-06 §2.2). The drawable must
track the client area scaled by the backing scale factor, and the timeout, vsync
and colour-space properties govern the present. The layer now sets
`maximumDrawableCount = 3` (one per ring slot), `displaySyncEnabled`,
`allowsNextDrawableTimeout` and the sRGB colour space. The drawable size still
follows the source, because a view-sized drawable needs the scale pass F10.

### F10 — the present output pipeline. Fixed.
The original text is stale: the output tier exists. `mtl::frame_output` owns the
scale, calibration and UI passes and the frame capture (`MTLPresent.cpp`), and
`MTLGSRender::flip` drives it. The flip requests the capture before the compose
(`MTLGSRender.cpp:1416`), composes into the drawable's pass with the aspect, the
display area, the gamma, the range flag and the overlay manager (`:1450`), and the
calibration pass applies the gamma, the range conversion and the stereo merge
(`MTLPresent.cpp:73-88`). The capture runs before the output path scales, the order
VK uses (`VKPresent.cpp:684`). The letterbox is the black clear outside the aspect
rectangle (`MTLGSRender.cpp:1436-1440`).

The remaining gap was the output mode. The layer hardcoded `displaySyncEnabled = YES`
(`MTLMetalLayer.mm`), so `g_cfg.video.vsync` was ignored. `presenter::update_vsync`
now re-reads the setting on every acquire and maps it per HLD-06 §2.8: `full` sets
the display sync, `off` clears it, and `adaptive` clears it and paces the present
with `presentAfterMinimumDuration` at the display's refresh interval. The refresh
interval has no metal-cpp binding, so `metal_layer_refresh_interval` reads
`NSScreen.maximumFramesPerSecond` in the Objective-C++ unit. `adaptive` is an
approximation: Metal has no mailbox present mode, so it cannot yield exactly.

### F11 — the DMA, image and sync clusters are dead code. Fixed: deleted.
The five units only included themselves, so nothing reached them. The live path
is the shared CPU implementation in `NV47/HW/*.cpp` (`nv0039.cpp:69-177` and the
rest), and the backend already declines both GPU paths a Metal-side unit would
serve: `supports_passthrough_dma` is false (`MTLDevice.cpp:135`), so a transfer
is a host byte copy over the shared guest window, and `supports_host_gpu_labels`
is false (`MTLDevice.cpp:138`), so the guest labels stay CPU-side. A Metal-side
decode of the same registers would only drift from the shared handlers, so the
five units and their headers are deleted and the clusters move to reuse-only
(`REALISATION.md`).

### F12 — the drawable acquire can block forever. Fixed.
`MTLPresenter.cpp:78-83` called `nextDrawable()` while
`allowsNextDrawableTimeout` was unset (`MTLMetalLayer.mm:31-36`), so the call
blocked on the RSX thread when the drawable pool was empty. VK bounds the
equivalent acquire with a timeout (`VKPresent.cpp:595-596`). The layer now sets
`allowsNextDrawableTimeout`, so a nil drawable is a dropped frame, which the
presenter already handles.

## E — the state-latching cluster, second reading

The same cluster was read a second time against the VK and GL evidence. The pass
numbered its findings `F1..F12`, which collides with pass D, so they are recorded
as `E1..E12` and each entry names the pass's label. `E5` and `E6` are the pass's
duplicates of `C3` and `C1`.

### E1 — a cull-both draw dropped the rasteriser for points and lines. Fixed. (pass label F1)
`MTLGSRender.cpp` set `raster_enabled = !culls_both_faces(raster)` with no
primitive test, and `MTLPipeline.cpp:81` folds it into
`setRasterizationEnabled(false)`. The RSX culls triangles only; Vulkan maps
`front_and_back` to `VK_CULL_MODE_FRONT_AND_BACK`, which spares points and lines
(`VKGSRender.cpp:212-218`), so a textured point-sprite or line draw with cull-both
latched rasterised nothing. The key already carries `input_primitive_topology`, so
the drop is now conditioned on the triangle class.

### E2 — a `surface_b`-only draw bound a null attachment. Fixed. (pass label F2)
`surface_color_target_mask()` returns 1 for both `surface_a` and `surface_b`
(`rsx_methods.h:772-774`), so `color_attachment_count()` is 1, but the store fills
`m_bound_render_targets[1]` for `surface_b` (`surface_store.cpp:16`,
`get_rtt_indexes`). `framebuffer::bind` copied slot *i* to slot *i* and took the
count as the highest bound index plus one, so slot 0 stayed null and
`m_color_count` became 2: `m_sample_count = m_color[0]->samples()` dereferenced
null and `open_pass` bound no texture at slot 0. The attachments are now compacted
into slots 0..n-1, which is the RSX's own model — it renumbers the enabled
targets, GL maps `surface_b` to draw buffer 0 (`GLRenderTargets.cpp:291-294`) and
VK builds `m_fbo_images` in `get_rtt_indexes` order and asserts
`mrt_buffers_count == m_draw_buffers.size()` (`VKGSRender.cpp:2697-2712,804`). The
pass rated this a mis-render; it is a crash.

### E3 — `attachment_count` in the PSO key. Won't fix: the decision stands. (pass label F3)
The pass proposed `MTL4RenderPipelineColorAttachmentDescriptor
.setColorAttachmentMap` to keep one PSO across attachment counts. Recorded, not
taken: the map bridges a *logical-to-physical* index gap, and E2's fix leaves no
gap — the RSX renumbers, so draw slot *i* is always the *i*-th enabled target and
the logical and physical indices agree. The map would cost a descriptor write per
draw for nothing, so `attachment_count` stays in the key.

### E4 — `z16_float` used a unorm depth target. Fixed. (pass label F4)
`get_surface_depth_format` returned `PixelFormatDepth16Unorm` for both `z16_uint`
and `z16_float`. A unorm target cannot hold the float range; VK maps `z16_float`
to `VK_FORMAT_D32_SFLOAT` (`VKFormats.cpp:15-16`) and GL to `depth32f`
(`GLRenderTargets.cpp:78-79`). The float variant now returns
`PixelFormatDepth32Float`. This is the surface-format half of the same mistake as
B11, which was the texture format.

### E5 — the extended-depth-range claim. Duplicate of C3; C3 is now Fixed. (pass label F5)
The pass reached C3's conclusion from the other direction: the code promised
`supports_extended_depth_range` and then clamped the viewport to [0,1]. C3 is
closed by evidence the pass did not have — `LLD-raster` §3.2 and invariant 4
("Out-of-[0,1] clip values never reach `MTLViewport.znear/zfar`; the shader
transform owns the general case") and Apple's [0,1] validation of
`MTLViewport.znear/zfar`, which rules out the pass's second option.

### E6 — the logic op is a no-op. Duplicate of C1. (pass label F6)
Confirms C1 exactly: `logic_operation` is latched, keyed and then never consumed,
and `MTLBlendState.cpp:111` disables blending when the logic op wins, so the draw
writes plainly. C1 stays Open (it needs the MSL ROP emulation).

### E7 — the depth-bounds test was absent. Fixed. (pass label F7)
The RSX has `depth_bounds_test_enabled`/`depth_bounds_min`/`depth_bounds_max`
(`rsx_methods.h:428-441`) and VK applies them (`VKDraw.cpp:241-271`), but the
cluster had no latch and never called `setDepthTestBounds`, which
`MTL4::RenderCommandEncoder` does carry, so a depth-bounds title was
under-culled. `capture_depth_bounds` now latches the registers, saturates the
range into the depth format (`clamp_depth_bounds_value`) and, when the draw's
`RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE` is set, encodes it the same way the VK
path does (`encode_emulated_depth`), then clamps to the Metal [0,1] viewport
range. The disabled range collapses to `[0,1]`, which is the VK form after the
same clamp. The flag is read at the call site, because
`requires_depth_range_emulation` and `current_fragment_program` are protected
`rsx::thread` members and the latch is a free function.

### E8 — the sampler dropped the LOD bias and did not clamp the LOD range. Partial. (pass label F8)
`capture_sampler` copied `min_lod`/`max_lod` straight from the registers
(`MTLSamplerState.cpp:139-140`) and `create_sampler_state` bound them unclamped
(`:188-189`). The range is now clamped to `texture.get_exact_mipmap_count() - 1`
when the minify filter samples mipmaps, matching VK (`VKDraw.cpp:467-518`). The
bias stays unimplemented: `MTLSamplerDescriptor` has no `lodBias`, so the guest
bias must be injected in the fragment shader (research 04 R19, open question 23),
which changes the fragment program's texture-state contract. Recorded as a
limitation so the drop is explicit rather than silent. VK's 0.5 bias rounding
for a nearest mip filter goes with it.

### E9 — A2C at one sample was claimed. Fixed. (pass label F9)
`supports_hw_a2c_1spp = true` suppressed the shader emulation
(`RSXThread.cpp:2445-2452`) with no measurement, and Metal's behaviour at one
sample is unverified. GL disables it explicitly ("In OGL A2C is implicitly
disabled at 1spp", `GLGSRender.cpp:61`). The flag is now false, so the verified
shader path runs; `supports_hw_a2one` stays true because Metal's `AlphaToOneState`
is well defined.

### E10 — no gate for a two-sample surface. Fixed. (pass label F10)
`surface::create` took `host_samples` from the guest AA mode, and
`diagonal_centered_2_samples` is 2; Apple's `supportsTextureSampleCount` table
lists 2 as "some devices" and 4 as "all", and the device init checked only the GPU
family (`MTLDevice.cpp:71`), so a 2-sample surface on a device that rejects it
failed descriptor validation with no explanation. The request now passes through
`resource_manager::supported_sample_count`, which folds an unsupported request up
to 4 and keeps the antialiasing; the guest 2-sample layout is `ps3` either way, so
the pitch and the resolve are unaffected.

### E11 — the PSO key hash combined its fields commutatively. Fixed. (pass label F11)
`hash_struct<mtl::pipeline_properties>` folded its fields with `^=`, so any two
fields that swap produce the same seed and the map probes extra buckets.
`operator==` kept it correct, but it costs CPU on a cache that holds thousands of
objects. The fields now fold through the non-commutative `hash64` (FNV). The
shared `pipeline_key_hash` (`ProgramStateCache.h:181-190`) has the same XOR and is
left alone: it is not our file.

### E12 — the key is complete but carries inert fields. Limitation. (pass label F12)
`blend.alpha_test_enabled` and `blend.shade_mode` travel in the key but the
fragment program already varies with them through its program id, so they are
redundant rather than wrong; `blend.logic_op_*` is inert for the same reason as
C1/E6. `write_mask` at or above `attachment_count` is always zero and is now a
constant term. No behaviour change; recorded so a later PSO-reuse audit does not
read the key as all-live state.

### E13 — the latch dataflow is not single-read and holds unconsumed state. Open. (pass §3)
Three perf and robustness items from the same pass. First, the headers say the
latch is "read once per draw", but `capture_encoder_state` composes
`capture_raster_state`/`capture_surface_state` and `capture_pipeline_properties`
then re-runs `capture_blend_state`/`capture_raster_state`/
`capture_anti_aliasing_state`, so the register file is traversed two to three
times per clause. Second, `point_size`, `line_width`, `line_smooth`,
`point_sprite_*`, `stipple_*` and `user_clip_control` are captured and read only
by the dial harness, and `stipple_pattern` is a raw `const void*` into register
space (`MTLRasterState.cpp:31`) where every other field copies its value, so a
later consumer that holds the latch across a register write reads stale or
retired data. Third, `apply_encoder_state` re-issues viewport, scissor, stencil
references, blend colour, bias and cull every clause although the struct it
already holds permits an equality cheap-out. Not folded into a fix: the harness
asserts the decode (the C5 and C6 precedent), so pruning the fields is a harness
decision.

### E14 — the state-latch hashes truncated their enum fields. Fixed. (found by the state tests)
`hash_depth_stencil_state` packed each stencil face into a `u32` with three bits per
operation (`MTLDepthStencil.cpp`), but `rsx::stencil_op` is a 16-bit GL constant:
`keep` is `0x1E00` and `zero` is `0`, so three bits could not separate them and the
operation bits also overlapped the mask fields. `hash_sampler_state` had the same
defect in a milder form — a four-bit slot for the 16-bit `comparison_function`,
overlapping `depth_compare` in the highest bits, which survived only because the
current comparison values are `0x0200..0x0207`. An `unordered_map` stays correct on a
collision (`operator==` still decides), so the cost was extra probes on a cache that
holds thousands of objects, and the risk was that a later value set would collapse
more states. Both now fold each field at its own width through `hash64`. The state
tests' "hash tracks every field" cases caught the depth/stencil one.

## Parked for review

These are the findings a fix cannot settle from the tree alone. Each needs either
a hardware measurement, a Metal-4 behaviour check, or a decision the acceptance
harness owns. They are parked, not closed.

| ID | The question to answer first |
|---|---|
| B8 | The mechanism is measured (see the entry). What remains is the implementation: a depth-plane decode into a staging buffer, a blit that names the plane, and the stencil-unknown marker. |
| B15 | Whether the Metal 4 compute-encoder copy of a combined depth-stencil image is a platform defect or a probe artefact, and whether a Metal 3 blit encoder needs an ordering rule against a Metal 4 command buffer. |
| C1 | The mechanism is answered (see the entry): the destination read is the existing `subpassInput`/`[[color(n)]]` framebuffer-fetch path. What remains is the implementation, which waits on first light because no test reaches the GLSL-to-MSL hop. |
| E13 | Which of the harness-only latch fields the harness will release, and whether the per-clause state re-issue is worth a cheap-out. |
| F5 / F9 | The partial halves wait transitively on the present output mode. |

<!-- doc-end -->
