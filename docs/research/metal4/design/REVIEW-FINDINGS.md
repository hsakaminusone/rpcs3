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
| A9 | Critical | MSL binding index limits | Fixed |
| B1 | High | depth/stencil mirror | Fixed |
| B2 | High | depth-compare input | Fixed |
| B3 | High | transfer-read barrier | Fixed |
| B4 | High | CPU readback sync | Fixed |
| B5 | High | `copy_texture` AA region | Fixed |
| B6 | High | depth resolve path | Fixed |
| B7 | Medium | `flush_old_contents` region | Fixed |
| B8 | Medium | `load_background` texel size | Fixed |
| B9 | Medium | `erase_bkgnd` init | Fixed |
| B10 | Medium | `insert_texture_barrier` | Fixed |
| B11 | Medium | `DEPTH16_FLOAT` map | Fixed |
| B12 | Medium | border colour SNORM | Fixed |
| B13 | Medium | deferred-release slot | Fixed |
| B14 | Low | MSAA swizzle view | Fixed |
| B15 | High | combined-format copy no-op | Fixed |
| B16 | High | depth read-back encode | Partial |
| C1 | High | logic op | Fixed |
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
| E13 | Low | latch dataflow and dead fields | Partial |
| E14 | Medium | state-latch hash truncation | Fixed |
| G1 | High | flat shading | Open |
| G2 | Low | `supports_multidraw` | Limitation |

Totals: 63 findings, 47 fixed, 5 partial, 0 blocked, 1 open, 7 limitations, 2 duplicates, 1 won't fix.

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

### A9 — the MSL binding indices exceeded the compiler limits. Fixed.
The Metal 4 shader compiler rejects the index map in `MTLArgumentBinds.h`. An MSL
`[[buffer(n)]]` index must be at most 30, a `[[sampler(n)]]` index at most 15 and a
`[[texture(n)]]` index at most 127. The measurement is on Apple A18 Pro, macOS
27.0.1, through `MTL4::Compiler::newLibrary` with the same `CompilerDescriptor`
the backend builds (`MTLDevice.cpp:86`): index 30 compiles and index 31 fails with
"must be between 0 and 30"; index 15 compiles and index 16 fails with "must be
between 0 and 15"; index 127 compiles and index 128 fails. The language version
does not move the three limits: 3.1, 3.2 and 4.0 give the same result. The
argument table is not the limit: `setMaxBufferBindCount` accepts 100000.

Four sites broke.

**The fill values, fixed.** `fill_values_slot` was 51, so the fill library could
not be created: the fill MSL declared the values at `[[buffer(51)]]`, so
`fill_pass::init` logged "fill library creation failed", left `m_library` null, and
every clear and every output pass was then dropped. The slot is now 30, the last
index the buffer cap allows, and `argument_buffer_count` follows it to 31. Index 30
is also the first stencil-mirror texture, which is no collision: a buffer and a
texture are separate spaces, and no buffer slot of either stage reaches 30. The
header asserts the two caps, so a later edit cannot silently re-break them.

**The texture units, fixed.** `spv_to_msl` set `msl_sampler` to the binding, and
the decompiler declares each texture unit at `texture_slot_base + index`, that is
12 to 27 for the fragment units and 12 to 15 for the four VTF units. Every one of
those indices exceeds the sampler cap, so **no fragment shader that sampled any
texture unit could be compiled**, and no vertex shader that used a VTF unit. The
sampler index is now decoupled from the texture index: a unit takes its position
inside its stage's texture range, `texture_sampler_index(slot) = slot -
texture_slot_base`, so the fragment units take 0 to 15 and the VTF units take 0 to
3. `MSLResourceBinding` carries the two indices separately, the texture index
stays the RSX ABI number, and the table writer now takes both indices per unit. A
stage has at most `fragment_texture_count` sampler-bearing units, so the whole
range fits the cap, asserted in the header. This half is what first light needed:
an ordinary RSX draw samples textures, so no draw rendered without it.

**The argument table, fixed.** The table's sampler space shares the cap: the
argument table descriptor must not ask for more than 16 sampler entries, and
`argument_sampler_count` was `argument_texture_count`, so every frame slot's
descriptor asked for 51. The release driver tolerates it, which is why the probe's
own tables worked while the validation layer aborted the process on the assertion
"max sampler state bind count must not be more than 16". The count is now 16, and
the static assertions tie it to the unit counts in both directions: the table must
be wide enough for one sampler per texture unit, and no wider than the cap. The
mirror and the depth input still ask for sampler indices the table cannot address,
which is inert while their shaders already fail to build.

**The depth mirrors, landed.** The stencil mirror of a redirected depth unit is
declared at `fragment_stencil_slot_base + index`, that is 30 to 45
(`MTLFragmentProgram.cpp`), and the emulated depth compare declares `frag_depth` at
50. Both were combined image samplers, so both took the binding as the sampler index
and both exceeded the cap, and a fragment shader that used either feature could not
be compiled. Both are now separate, samplerless textures: the mirror is an integer
`utexture2D` (`utexture2DMS` when multisampled), which is the exact type for an
unfiltered texel read, and the depth input is a `texture2D` (`texture2DMS`), which
the shared ROP epilogue already reads with `texelFetch`. The extension
`GL_EXT_samplerless_texture_functions` is required when either feature is on. The
non-integer mirror types are unreachable but log an error rather than declaring a
float read. The table writer binds no sampler for either entry — a bind past the
declared sampler count aborts the process on the validation layer (measured,
`/tmp/flprobe/tbl.mm`, exit 134) — so the sampler the two producers used to build is
gone with the declaration.

**The form is now measured.** A probe measured the target MSL on the floor device
through the same `MTL4::Compiler::newLibrary` call the backend uses (`/tmp/a9probe`,
Apple A18 Pro, macOS 27.0.1). The 16-unit form plus a samplerless `texture2d<uint>`
at `[[texture(30)]]` and a samplerless `texture2d<float>` at `[[texture(50)]]`, both
read with `read()`, compiles: the library builds, and a pipeline and an argument
table build on top of it. The current form fails on the sampler index, verbatim:
`'sampler' attribute parameter is out of bounds: must be between 0 and 15`. Three
facts bound the fix. The mirror and the depth input keep their texture indices — 30
and 50 are legal with or without a sampler. The reads must be `read()` and never
`sample()`, because `sample()` forces a sampler declaration and re-breaks the cap.
And the form sits exactly on the limit: 16 samplers for 16 units alongside 18
textures, nothing over a cap. The escape hatch measured too: an argument buffer
holding the whole set compiled with no ceiling found up to 8000 entries, so the
fallback is real if a guest program ever needs more sampler states than the form
allows.

The remaining half is a real constraint, not a slot that moved. Metal has no
combined image sampler: a GLSL `sampler2D` becomes an MSL texture plus an MSL
sampler, one sampler index each, and the compiler rejects two samplers that share
an index ("cannot reserve 'sampler' resource location at index 0"; measured). The
declarations the tree emits today need 16 unit samplers, one per mirror (up to 16
more) and one for the depth input, so a program that uses both features needs up
to 33 sampler entries against a cap of 16. The states are fewer than the entries:
every mirror and the depth input share one state (`stencil_mirror_sampler()`, read
by `load_texture_env` and by the depth-input resolve). Declaring a mirror as a
separate texture instead of a combined sampler lets several of them share one
sampler entry, which is what brings the count down, but the arithmetic still ends
at 16 units plus one shared entry, one over the cap.

Two forms remove the constraint, and the first is now measured:

- **Preferred: fetch the mirrors without a sampler.** The stencil half is read as
  an integer, so an unfiltered texel read is the exact operation, and `frag_depth`
  is already read with `texelFetch` in the shared ROP epilogue. Declaring the
  mirror as a separate `utexture2D` and the depth input as a separate `texture2D`
  removes both from the sampler space, so all 16 indices stay with the units and
  nothing is over the cap. The two macros that express the fetch are defined by the
  shared GLSL snippets and are re-definable per backend, which is the same seam the
  Metal wrapper already uses for its own header. The coordinate must be clamped,
  because a texel read returns zero out of range where the mirror's state clamps,
  and `GL_EXT_samplerless_texture_functions` is owed for the depth input.

The shortcut of making only the two reads samplerless is ruled out, and that is a
measurement rather than an inference. A probe compiled the six vendored SPIRV-Cross
translation units and drove `CompilerMSL` with the backend's own options and resource
couplings (`/tmp/scprobe`). The vendored cross-compiler emits an MSL `[[sampler(n)]]`
argument for a combined image sampler that is only ever fetched — declared but never
referenced — because it pushes a sampler for every sampled-image type and has no
fetch-only check (`spirv_msl.cpp:15557-15661`). It emits the argument even when no
binding mapping is supplied, so no choice of mapping avoids it. A separate texture
(SPIR-V `Sampled = 2`) fetched with `read()` emits no sampler argument, which is why
the preferred form above is the form that works. The one alternative that keeps the
combined declaration is a constexpr sampler, which does suppress the argument but
bakes sampler state into the shader as constants.
- **Escape hatch: an argument buffer.** A resource struct bound at one buffer
  index lifts all three caps at once: 200 textures and 200 samplers in one struct
  compile (measured on the same device). It is the standard Metal answer and it is
  what to reach for if a guest program ever needs more sampler states than the
  unions above can express. The cost is that the shader must index arrays instead
  of naming declarations, which changes the decompiler's texture contract.

The preferred form is landed, and it leaves no sampler over the cap, including for a
program that declares all 16 units: the mirror and the depth input leave the sampler
space entirely. The two macros that express the fetch are re-defined after
`insert_glsl_legacy_function`, so the mirror is read with `texelFetch` while the
depth half of the read still comes from the unit, which keeps its sampler. The fetch
clamps to the texture size and returns zero outside `[0, 1]`, which is the mirror's
state: `stencil_mirror_sampler()` is clamp-to-border with an opaque-black border and
nearest (`MTLSamplerState.cpp:187-196`), so an out-of-range coordinate reads zero
either way. One correction to the register: the earlier claim that the mirror's
state clamps is wrong, and the samplerless form agrees with the border rather than
with a clamp.

What no test in the tree reaches is the GLSL-to-MSL hop, so the first real
verification of the emitted declarations is first light.

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

### B8 — `load_background` uses the guest texel size. Fixed.
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

The fix is a depth-plane decode into a staging buffer followed by a blit that
names the plane (B15), gated to the formats whose byte sizes differ.  It cannot
reuse the format-aware colour upload VK uses, because VK's host depth format keeps
the guest texel size (`VKRenderTargets.cpp:695-800`).  `decode_depth_plane`
(`MTLFormats.cpp`) reproduces the VK transform exactly: `rsx::get_depth_clear_value`
on the bits the RSX pack puts the depth in, that is bits 31:8 of a z24s8 word and
bits 15:0 of a z16 word.  No second codec exists.  `load_background` decodes when
the depth format needs it (`z16_float`, `z24s8_uint`, `z24s8_float`) and copies as
before otherwise, and `upload_texture_data` applies the same decode to a sampled
depth texture.  `z16_uint` maps to `Depth16Unorm`, whose texel size already matches
the guest, so it takes no decode.

The combined-format stencil stays marked unknown (`stencil_init_flags |= (1 << 9)`,
the marker `on_write_copy` already sets), because the upload direction has no plane
option for a colour source.  The corrected B15 measurement shows the plane is
addressable, so the marker is an open item and not a permanent limit.

Reachability: the path runs only when `needs_cpu_upload()` holds, that is with
`read_depth_buffer` enabled or `force_data_load` set, so it is off by default.

### B16 — the depth read-back writes the host plane, not the guest pack. Partial.
`write_to_memory` and `copy_texture` now name the depth plane of a combined source
(B15), so the copy no longer fails, but the bytes written are the host depth plane:
a little-endian binary32 at 4 bytes per texel. The guest expects the RSX pack
instead — a z24s8 destination wants the 24-bit depth in bits 31:8 and the stencil
byte in bits 7:0 of each 4-byte word, and a z16 destination wants the depth in bits
15:0 of a 2-byte word.

**The inverse is written and tested.** `encode_depth_plane` (MTLFormats.cpp) inverts
`decode_depth_plane`: it takes the host plane, one f32 per texel, and writes the
guest pack at the destination stride, per format. A unorm pack rounds the depth back
(z16, z24s8); a z16 float pack re-encodes E4M12 as the scalar form of
`pack_e4m12_pack16` (`E4M12Conversion.glsl:5-12`); a z24s8 float pack takes the
float's own top 24 bits. The VK `cs_fconvert_task<f32,f16>` path does not apply here,
because this backend's plane is `Depth32Float` and its decode is E4M12, not f16. A
null stencil plane writes a zero stencil byte instead of leaving whatever the guest
buffer held. Two state-suite rows cover it: a round trip back through
`decode_depth_plane` for ten words across all four formats, and the zero-stencil case.

**Two encode defects fell out of the new tests and were fixed.** `encode_unorm`
computed `value * range + 0.5` in f32; `65535 + 0.5` rounds up to 2^16 and
`16777215 + 0.5` rounds up to 2^24, and the z24s8 pack then shifts the overflow out
of the word, so a depth of 1.0 encoded as zero. The arithmetic is now f64 and the
result is clamped to the range. The byte order is confirmed against the decode tables
rather than assumed: z24s8 puts the stencil in bits 7:0 and the depth in bits 31:8
(`rsx_decode.h:3404`, `clear_z24` and `clear_stencil`), and z16 puts the depth in bits
15:0. A third defect was in the test itself: the zero-stencil case asserted the wrong
byte order and was corrected to `packed[0] = 0x00`, `packed[1..3] = 0xFF`.

**The encode applies exactly when `depth_format_needs_decode` holds.** That is the
same predicate the upload uses, and it agrees with the host format table: a z16_uint
surface is `Depth16Unorm`, whose plane is already the guest pack, while z16_float is
`Depth32Float` and z24s8 is `Depth32Float_Stencil8`, whose planes are not.

**What remains is the completion point.** The encode is a CPU pass, so it must run
after the copy that fills the guest buffer has completed. The read-back path
(`MTLSurfaceCache.cpp:203-213` → `MTLSurface::write_to_memory`) records that copy into
the open compute encoder, and the transform then needs a synchronisation point after
the frame slot commits — in Metal 4 the form is `signalEvent` plus
`waitUntilSignaledValue` (HLD-03). That point is not written, because nothing reaches
this path today and a synchronisation point is not something to add unprofiled. The
stencil plane is still not written for a combined surface, so a stencil read-back of a
combined surface is still lost.

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

### B15 — the un-named combined-format copy is invalid API. Fixed.
A `copyFromBuffer` or `copyFromTexture` into a `Depth32Float_Stencil8` texture on
`MTL4::ComputeCommandEncoder` with `MTLBlitOptionNone` is **invalid API**: the
validation layer rejects the option for a combined format. The earlier reading that
the driver "writes nothing" was a read-back artefact. On Apple A18 Pro, macOS
27.0.1, the un-named call did perform the write — with an explicit `MTL4` barrier,
in private and in shared storage — so the defect is the call and not the driver, and
there is no Apple bug to work around. Every combined-format copy must name its
plane with `BlitOptionDepthFromDepthStencil` or `BlitOptionStencilFromDepthStencil`,
and one call carries one plane. The named form writes and reads back bit-exact
(`0000003f` at every texel for a clear depth of 0.5) on private and on shared
storage, so no Metal 3 blit encoder and no render pass is needed, and the
two-encoder ordering rule this item was waiting on does not apply. A
`texture→texture` copy of a combined format is the exception: the API exposes no
option overload for it, and the plain form performs both planes, so those sites
stay as they are. `texture->texture` copies of `Depth32Float` and `Depth16Unorm`
are unaffected in both forms.

`get_depth_plane_option` (`MTLFormats.cpp`) is the one place that names a plane. The
four buffer↔texture sites use it: `surface::load_background`
(`MTLSurface.cpp:368`), the texture upload (`MTLTextureCache.cpp:75`),
`surface::write_to_memory` (`MTLSurface.cpp:476`) and
`cached_texture_section::copy_texture` (`MTLTextureCache.cpp:173`). Each site takes
the option branch only for `PixelFormatDepth32Float_Stencil8`, so a plane option is
never passed for a non-combined format, and a combined format is never copied
without one.

## C — the state latches and the clear path

### C1 — the logic op has no implementation. Fixed.
The RSX logic op is a real operation: VK bakes it into the pipeline
(`VKGSRender.cpp:322-326`) and GL enables `GL_COLOR_LOGIC_OP`
(`GLDraw.cpp:199-201`). Metal has no logic-op state — neither
`MTL4::RenderPipelineDescriptor` nor `MTL4::RenderCommandEncoder` exposes one — so a
logic-op draw was a plain write. Pass E6 re-confirmed it: the operation was latched
and keyed and then never consumed, and `MTLBlendState.cpp:111` disables blending
when it wins, so the draw wrote plainly with no diagnostic.

The mechanism that reads the destination colour in a Metal 4 fragment shader is the
`[[color(n)]]` input, and the tree already owns it: the programmable-blending path
declares `layout(input_attachment_index = n, set = 0, binding = 46 + n) uniform
subpassInput frag_src_n` and reads it with `subpassLoad`
(`RSXROPPrologue.glsl:53-77`), and `MTLShader.cpp:78` sets
`use_framebuffer_fetch_subpasses`, so SPIRV-Cross lowers the subpass input to an MSL
`[[color(n)]]` parameter. The logic op needs no encoder state and no binding of its
own: the same declaration, plus the op applied in the epilogue.

Landed:
- A capability `backend_configuration::supports_native_logic_op`, true through a
  default member initializer, so Vulkan and OpenGL — which never set it — keep their
  native logic op, and `MTLDevice.cpp` sets it false.
- `RSX_SHADER_CONTROL_LOGIC_OP` (`0x00001000`, the enum's "Custom" range,
  `gcm_enums.h:459`). `rsx::thread` folds it into `current_fragment_program.ctrl`
  when `logic_op_enabled()` holds and the capability is false — the
  `RSX_SHADER_CONTROL_ALPHA_TEST` precedent (`RSXThread.cpp:2445-2452`).
- `GLSLTypes.h` `ROP_logic_op` drives `_ENABLE_LOGIC_OP` (`GLSLCommon.cpp:384-387`),
  and only the Metal fragment program sets the property, so no other backend's shader
  changes.
- The shared ROP prologue declares and loads `mrt_colorN` under `_ENABLE_LOGIC_OP`
  through `MRTn_LOAD_DO`. The load must **not** reuse the programmable-blending gate
  `IS_MRT_BLEND_ENABLED(n)`, because the ROP blend-target bits are written only under
  `RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING` (`Core/RSXDrawCommands.cpp:693-696`)
  and a logic-op guest need not enable blending; under the logic op the load is
  unconditional, and `MRTn_BLEND_DO` is left for the blend apply alone.
- `RSXFragmentPrologue.glsl` gains `_logic_op_quantize`, `_logic_op_byte` and
  `_logic_op_apply`, and the epilogue applies `_logic_op_apply` to every output
  between the blend block and the channel remap. The op is the guest enum value
  (`gcm_enums.h:1627-1645`); the apply is a 16-entry switch on one byte.
- The op travels in the fragment push-constant block (`MTLConstants.h`
  `blend_values.logic_op`, `sizeof` 32 to 36), and the `subpassInput`s are declared
  under `PROGRAMMABLE_BLENDING | LOGIC_OP`, so a change of op does not recompile the
  shader. `MTLGSRender.cpp` allocates and fills the block when either bit is set.
- Scope: the emulation quantizes source and destination to the guest's 8-bit unorm
  encoding with `round_to_8bit`. Vulkan forbids `logicOpEnable` on a floating-point
  attachment and GL leaves the result undefined for a non-fixed-point framebuffer, so
  a float target has no logic op to reproduce. The PSO key still carries the inert
  `blend.logic_op_*` fields (E12), and they stay inert.

The blend block grew from 32 to 36 bytes, so
`MTLConstantsLayout.StructSizesMatchTheShaderAbi` was updated to expect the new size
and to pin `offsetof(blend_values, logic_op) == 32`; the old assertion still expected
32 and failed.

The emulation is not executed end to end: no test in the tree reaches the
GLSL-to-MSL hop, so first light is the first verification of it.

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
writes plainly. C1 now carries the MSL ROP emulation and is Fixed.

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

### E13 — the latch dataflow is not single-read and holds unconsumed state. Partial. (pass §3)
Four items from the same pass.

**The aliased stipple pattern, fixed.** `stipple_pattern` was a raw `const void*`
into register space (`MTLRasterState.cpp:31`) while every other field of
`raster_state` copies its value, so a consumer that held the latch across a register
write would read stale or retired data. The latch now copies the 32-word pattern
(`raster_state::stipple_word_count`), and the harness row asserts the copy instead of
the pointer.

**The harness-only fields, kept and marked.** `point_size`, `line_width`,
`line_smooth`, `point_sprite_*` and `user_clip_control` are captured and read only by
the dial harness: the real consumer reads the register file at the point of use, so
point size reaches the shader through `vertex_context::point_size`
(`MTLGSRender.cpp:1007`). The header now marks them as captured for the harness rather
than pruning them, because they are what pins the decode (the C5 and C6 precedent).
Pruning them is a harness decision and stays open below.

**The repeated traversal, already resolved.** The entry's first item no longer matches
the tree. `capture_encoder_state` calls `capture_raster_state` and
`capture_surface_state` once each (`MTLEncoderState.cpp:11-12`) and re-runs nothing.
The other two captures belong to `capture_pipeline_properties`
(`MTLGSRender.cpp:218-220`), which is a separate latch for a separate consumer — the
pipeline key, not the encoder state. Each register file is therefore read once per
cluster per clause by design, and the duplicated traversal the entry described does
not exist in the current revision.

**The per-clause re-issue, deferred.** `apply_encoder_state`
(`MTLEncoderState.cpp:53-82`) writes cull mode, winding, fill mode, depth clip mode,
depth bounds, depth bias, stencil references, blend colour, viewport and scissor on
every clause, although the state it already holds permits an equality cheap-out. It
would need `operator==(const encoder_state&, const encoder_state&)`, a held previous
state, and a key on the encoder pointer so the held state is dropped when a pass opens
a new encoder. Not written now: it is a performance change on a path that has never
executed, so it cannot be measured, and this backend does not add unmeasurable work.
The shape is recorded here so the change is mechanical when a frame can be profiled.

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

## G — the capability parity pass

This pass read the backend against the capability struct the shared code consults, and
against the two shipping backends.

### G1 — the flat-shading path is disabled on Metal. Open.

RSX flat shading takes its value from the provoking vertex, which for the RSX is the
last vertex of the primitive. Vulkan and OpenGL select that vertex with a
last-provoking-vertex state and set
`backend_configuration::supports_last_provoking_vertex`; Metal exposes no such state,
so `MTLDevice.cpp` leaves the capability false. The shared code then clears
`RSX_SHADER_CONTROL_FLAT_SHADING` (`RSXThread.cpp:2368-2373`), so a flat-shaded
attribute is interpolated smoothly instead of taking one vertex's value, and the Metal
vertex program's own flat path (`MTLVertexProgram.cpp:246`) is dead code.

Metal has no native expression of the RSX last-vertex convention. Two candidates
exist and neither is written or measured: an index rotation that moves the provoking
vertex to the first slot, and an explicit replicate of the provoking attribute in the
vertex program. Settle it with first light against a title that flat-shades, or record
it as an accepted limitation.

### G2 — `supports_multidraw` is inert. Limitation.

`rsx::thread` declares `supports_multidraw` (`RSXThread.h:82`); all three backends
assign it; a repository-wide search finds no reader. The field is a capability the
shared code never consults, so it is inert in the same way as the E12 key fields. On
Metal the `false` costs nothing, because the flag changes no code path, but the field
should be either consumed or recorded as inert.

## Parked for review

These are the findings a fix cannot settle from the tree alone. Each needs either
a hardware measurement, a Metal-4 behaviour check, or a decision the acceptance
harness owns. They are parked, not closed.

| ID | The question to answer first |
|---|---|
| A9 | The indices, the fill slot, the units, the sampler table and the two samplerless declarations are all landed and all under their caps. The depth-mirror form is measured (a samplerless `read()`, `read()` never `sample()`, indices 30/50 kept) and the sampler-count question is answered: the argument-buffer escape hatch compiled with no ceiling found up to 8000 entries. What remains is the in-tree GLSL-to-MSL hop, which no test reaches, so first light is the first verification. |
| B16 | Only the completion point: the encode is written and tested, and the predicate is settled (`depth_format_needs_decode`). What is unfinished is where the CPU pass runs, which needs a synchronisation point after the frame slot commits, and the stencil plane of a combined read-back. |
| E13 | Which of the harness-only latch fields the harness will release. The per-clause re-issue is deferred with its shape recorded in the entry. |
| F5 / F9 | The present output mode landed with F10, so neither partial half waits on it any more: F5 still does not consume the display-buffer pitch and format or the stereo second eye, and F9 still sizes the drawable from the source rather than the view. Both need a re-check against the landed output tier. |
| G1 | Which expression is the cheaper — an index rotation or a provoking-attribute replicate — and does first light show a title that flat-shades at all? |

<!-- doc-end -->
