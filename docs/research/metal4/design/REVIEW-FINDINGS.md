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

Line references are as of the review pass; every entry also names the function,
which does not move. A finding that a fix closed is marked **Fixed**; a finding
that needs a subsystem the tree does not have is marked **Blocked** and names the
subsystem; a finding that records an accepted deviation is marked **Limitation**.

## Summary

| ID | Severity | Area | Status |
|---|---|---|---|
| A1 | High | vertex transform constants | Fixed |
| A2 | High | instanced-constant stride | Fixed |
| A3 | High | depth/stencil mirror bind | Blocked |
| A4 | Medium | depth-compare input bind | Blocked |
| A5 | Medium | transform-constant ring size | Fixed |
| A6 | Medium | `block_ring` growth | Fixed |
| A7 | Medium | `emulate_shadow_compare` | Open |
| A8 | Low | `conditional_render_predicate` | Open |
| B1 | High | depth/stencil mirror | Blocked |
| B2 | High | depth-compare input | Blocked |
| B3 | High | transfer-read barrier | Fixed |
| B4 | High | CPU readback sync | Open |
| B5 | High | `copy_texture` AA region | Fixed |
| B6 | High | depth resolve path | Blocked |
| B7 | Medium | `flush_old_contents` region | Open |
| B8 | Medium | `load_background` texel size | Open |
| B9 | Medium | `erase_bkgnd` init | Open |
| B10 | Medium | `insert_texture_barrier` | Blocked |
| B11 | Medium | `DEPTH16_FLOAT` map | Fixed |
| B12 | Medium | border colour SNORM | Fixed |
| B13 | Medium | deferred-release slot | Fixed |
| B14 | Low | MSAA swizzle view | Open |
| C1 | High | logic op | Open |
| C2 | High | two-sided stencil ref | Fixed |
| C3 | High | extended depth range | Open |
| C4 | Medium | point polygon mode | Limitation |
| C5 | Medium | PSO key alpha fields | Fixed |
| C6 | Medium | dead vertex latch | Fixed |
| C7 | Low | polygon-offset enables | Limitation |
| C8 | Low | duplicate scissor build | Fixed |
| C9 | Low | `capture_draw_state` | Limitation |
| C10 | Low | dither | Limitation |

Totals: 29 findings, 13 fixed, 6 blocked, 6 open, 4 limitations.

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

### A3 — the depth-texture stencil mirror is never bound. Blocked on the pass scheduler.
A redirected depth fragment unit also binds the stencil aspect
(`VKDraw.cpp:702-721`), which the shared decode reads (`Program/GLSLSnippets/
RSXProg/RSXFragmentTextureOps.glsl:106-108`). The tree declares the second
sampler but nothing produces or binds it, so a redirected depth texture decodes
garbage. Metal has no stencil view of a depth-stencil texture, so the fix needs a
copied `R32Uint` mirror, and Metal forbids sampling a bound attachment, so the
draw must also leave the depth attachment's pass. Both need the pass scheduler.

### A4 — the depth-compare input is never bound. Blocked on the pass scheduler.
The front-end sets `RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE` for an `EQUAL`
depth test (`RSXThread.cpp:2200-2204`) and the epilogue does
`texelFetch(frag_depth, ...)` (`Program/GLSLSnippets/RSXProg/
RSXROPEpilogue.glsl:77-82`). VK binds the depth-aspect view at
`frag_depth_input_location` (`VKDraw.cpp:795-800`). The tree declares the input
under the same flag but never writes the slot. The fix is the same pass split as
A3.

### A5 — the transform-constant ring was sized in elements, not blocks. Fixed.
The header defines `transform_capacity` as the number of changes a frame may hold,
and one change is a 468-vec4 file, but `init` passed the capacity as a count of
vec4, so one block filled the ring and every change grew and retired a buffer.
`init` now passes `transform_capacity * vertex_constant_count`.

### A6 — `block_ring::alloc` could write past its capacity. Fixed.
Growth allocated a replacement buffer of the same capacity, so a block larger
than the capacity wrote past the end. Growth now takes
`max(m_capacity, count)` and the ring grows to fit the block.

### A7 — `emulate_shadow_compare` is forced false. Open.
GL and VK derive it from device depth-format support
(`VKFragmentProgram.cpp:405,636`). The tree forces it false because the native
compare sampler was not measured against the RSX. A native `sampler2DShadow` may
differ from the emulation, so the flag needs a hardware measurement before it can
be enabled.

### A8 — `conditional_render_predicate` is bound but never declared. Open.
The binder writes the slot, but no MSL declares it, which is correct only while
`mtl::backend_configuration::supports_hw_conditional_render` is false and the
conditional draw is skipped on the CPU. If the hardware path is ever enabled, the
shader must declare and test the predicate.

## B — the surface and texture cache

### B1 — the depth-texture stencil mirror. Blocked on the pass scheduler. Duplicate of A3.
Recorded separately because the cache side owns it: `get_view` makes only a
colour view, `resolve_texture_bind` returns one `texture_bind`, and
`argument_binds` has no stencil field. The fix is the copied `R32Uint` stencil
mirror carried in `texture_bind` and bound at `fragment_stencil_slot_base + id`
(30..45).

### B2 — the depth-compare input. Blocked on the pass scheduler. Duplicate of A4.
`resolve_texture_bind` carries no depth view, so the draw path has nothing to
bind at `fragment_depth_input_slot` (50). The natural source is the bound depth
surface (`framebuffer::depth()->value()`), which needs the pass split.

### B3 — `write_render_target_to_memory` skipped the read barrier. Fixed.
VK calls `surface->read_barrier(cmd)` before the copy (`VKRenderTargets.h:643`).
Without it an MSAA surface is read before its resolve and a pending
`old_contents` flush is skipped, so the DMA write-back returned stale bytes. The
barrier is now taken first.

### B4 — the CPU readback has no completion wait. Open.
`cached_texture_section::map_synchronized` returns the staging pointer as soon as
the copy is encoded; the copy executes at commit, so every `finish_flush`
readback is undefined. GL waits a fence (`GLTextureCache.h:432`) and `finish_flush`
here is empty. The fix is a shared-event or `waitUntilCompleted` wait on the
frame slot the copy was encoded into.

### B5 — `copy_texture` over-ran the resolve image for an AA surface. Fixed.
The region was scaled by `samples_x`/`samples_y` before the blit, but a Metal
multisample image is resolved in place and is pixel-sized. VK scales because its
image really is sample-sized and it has a typeless resize when the sizes differ
(`VKTextureCache.h:241-261`); Metal has no such path, so the source region
exceeded the texture. The scaling is removed.

### B6 — an MSAA depth surface is resolved through a colour attachment. Blocked on a depth resolve.
`surface::resolve` sets the depth texture as `colorAttachments()->object(0)` with a
resolve texture, and the framebuffer layer never gives the depth attachment a
resolve target. VK resolves the depth aspect directly
(`VKRenderTargets.cpp:1034`). Metal 4 resolves a depth attachment through the
depth attachment's own store action.

### B7 — `flush_old_contents` uses the raw region. Open.
VK transforms both rectangles with `transform_pixels_to_samples` and skips a
source that fails `test()` when it is not an accept-all copy
(`VKRenderTargets.cpp:1049-1054,:1073-1077`). The tree copies the raw rectangles,
so an AA region is misplaced and a dirty source is blended into the target.

### B8 — `load_background` uses the guest texel size. Open.
The guest z24s8 surface is 4 bytes per texel and its host image is
`Depth32Float_Stencil8`, 8 bytes per texel, so a raw `copyFromBuffer` cannot be
correct: the destination row length exceeds the source row stride. VK uploads
through the format-aware path. The fix is a format-aware background upload (a
compute unpack of the guest texels).

### B9 — `erase_bkgnd` is never initialised. Open.
VK clears the surface and marks it ready when the read-buffers config is off
(`VKRenderTargets.cpp:817-828`) and calls `initialize_memory` on the
`erase_bkgnd` flag (`VKRenderTargets.cpp:1000-1009`), with the `ready` and
`on_write` bookkeeping. The tree acts only when `needs_cpu_upload()` is true,
which it is not with the config off, so a partially written surface keeps
undefined contents and stays permanently dirty.

### B10 — `insert_texture_barrier` is a no-op. Blocked on the pass scheduler.
VK emits an image barrier (`VKTextureCache.cpp:1368`) and GL a texture barrier
(`GLTextureCache.h:875-877`); the base calls the hook whenever a sampled texture
overlaps a written surface (`Common/texture_cache.h:2555,2774,2894`). The tree
does nothing, so a feedback loop (programmable blending, a cyclic reference)
samples without a barrier.

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

### B14 — an MSAA texture drops the sample swizzle. Open.
`get_view` returns the texture itself when `sample_count() > 1`, so a multisample
texture with a non-identity remap samples in native order. Whether Metal can
carry a channel swizzle on a multisample view was not measured; if it cannot, the
remap must move to the read site.

## C — the state latches and the clear path

### C1 — the logic op has no implementation. Open.
The RSX logic op is a real operation: VK bakes it into the pipeline
(`VKGSRender.cpp:322-326`) and GL enables `GL_COLOR_LOGIC_OP`
(`GLDraw.cpp:199-201`). Metal has no logic-op state and no MSL performs it, so a
logic-op draw is a plain write. The fix is an MSL emulation that reads the
destination, or a recorded deviation.

### C2 — the stencil reference ignored the single-sided case. Fixed.
A single-sided draw applies the front reference to both faces
(`GLDraw.cpp:148-156`; VK uses `VK_STENCIL_FRONT_AND_BACK` with
`stencil_func_ref()`, `VKGSRender.cpp:351-355`). The tree always passed
`back_stencil_func_ref()` as the back reference. `capture_stencil_references` now
returns the front reference for the back face when the two-sided test is off.

### C3 — the extended-depth-range guard forces a `[0,1]` viewport. Open.
The backend advertises `supports_extended_depth_range`, so
`requires_depth_range_emulation()` is false (`RSXThread.cpp:2218-2226`) and the
vertex shader compresses Z with `apply_zclip_xform`. The viewport must then
re-expand to `[clip_min, clip_max]`, but the tree falls back to `[0,1]` whenever
the clip range leaves it, which stores the compressed Z on a float target. The
fix is either to stop advertising the capability, so the shader emulation runs,
or to drop the guard if Metal accepts an out-of-range `znear`/`zfar`.

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

<!-- doc-end -->
