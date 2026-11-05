# Review findings register

Eight passes read the Metal 4 implementation against the RSX behaviour.
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
- **G — the capability parity pass.** The backend against
  `rsx::thread::backend_configuration` and against the two shipping backends.
- **H — the first-light bring-up.** The evidence of the first frame the backend
  presented, read against the command recorder, the binding layer, the texture cache
  and the format map. Section 7.1 of `HANDOFF.md` holds the frame measurements.
- **J — the parity sweep.** The index subrange, the two byte rings, the 16-bit strip
  restart, the no-attribute drain, the shader-decoded sampler filter, the typeless
  copy, the live resolution-scale change, the fp16 setting and the skip diagnostics.
  The rows are J1-J10.

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
| A10 | Critical | overlay sampler index | Fixed |
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
| B16 | High | depth read-back encode | Fixed |
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
| C11 | High | pipeline topology class | Fixed |
| F1 | Critical | present source lookup | Fixed |
| F2 | Critical | pass hazards | Fixed |
| F3 | Critical | fill values buffer | Fixed |
| F4 | High | multisampled present source | Fixed |
| F5 | High | flip preamble | Fixed |
| F6 | High | split barrier loss | Fixed |
| F7 | High | cross-encoder barriers | Fixed |
| F8 | High | access-violation result | Fixed |
| F9 | Medium | drawable layer setup | Fixed |
| F10 | Medium | present output pipeline | Fixed |
| F11 | Medium | DMA/image clusters | Fixed |
| F12 | Medium | drawable acquire block | Fixed |
| F13 | Critical | present source format space | Fixed |
| F14 | Critical | the composited layer | Fixed |
| F15 | High | the stranded drawable | Fixed |
| F16 | Critical | the layer residency gate | Fixed |
| F17 | Critical | the commit-options lifespan | Fixed |
| F18 | High | the savedata dialog crash | Won't fix (upstream #15092) |
| F19 | High | the argument table per encoder | Fixed |
| F20 | Critical | the packed texture formats | Fixed |
| E1 | High | cull-both point/line | Fixed |
| E2 | Critical | `surface_b` attachment | Fixed |
| E3 | Low | PSO attachment-count key | Won't fix |
| E4 | High | `z16_float` depth format | Fixed |
| E5 | High | extended depth range | Duplicate of C3 |
| E6 | High | logic op | Duplicate of C1 |
| E7 | Medium | depth bounds test | Partial |
| E8 | Medium | sampler LOD bias | Partial |
| E9 | Medium | A2C at 1 spp | Fixed |
| E10 | Low | 2-sample capability | Fixed |
| E11 | Low | PSO key hash | Fixed |
| E12 | Low | key completeness | Limitation |
| E13 | Low | latch dataflow and dead fields | Fixed |
| E14 | Medium | state-latch hash truncation | Fixed |
| G1 | High | flat shading | Limitation |
| G2 | Low | `supports_multidraw` | Limitation |
| G3 | Medium | the boot preload dialog | Fixed |
| F21 | Critical | the missing base flip | Fixed |
| F22 | High | the fault wait with no flush work | Fixed |
| F23 | High | the pipeline archive flush configuration | Fixed |
| F24 | Critical | the RSX draw path omitted the clip-space Y flip | Fixed |
| I1 | High | the upload blit write record | Fixed |
| I2 | High | the unbounded shutdown drain | Fixed |
| I3 | High | the residency set drop order | Fixed |
| I4 | Medium | `insert_texture_barrier` | Fixed |
| I5 | Medium | the blit-engine fast path | Limitation |
| I6 | High | the teardown abort | Fixed |
| I7 | Medium | the flicker content | Open |
| I8 | Medium | the residency set lifespan | Fixed |
| I9 | High | the transfer-copy write ordering | Fixed |
| J1 | High | indexed subrange address | Fixed |
| J2 | High | ring growth to the block size | Fixed |
| J3 | Medium | ring block alignment | Fixed |
| J4 | Medium | 16-bit strip restart | Fixed |
| J5 | Low | no-attribute clause drain | Fixed |
| J6 | Medium | shader-decoded sampler filter | Fixed |
| J7 | High | typeless texture copy | Fixed |
| J8 | Medium | live resolution-scale change | Fixed |
| J9 | Medium | native fp16 selection | Fixed |
| J10 | Medium | skip-reason diagnostics | Fixed |

Totals: 97 findings, 80 fixed, 3 partial, 0 blocked, 1 open, 9 limitations,
2 duplicates, 2 won't fix. The table is the whole register: the seven passes
(A to E, G and H), the live defects the black-frame investigation found (I1-I9),
the boot observation (G3), the present-path and draw-path investigation
(F21-F24), and the parity sweep (J1-J10).

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

### A10 — the overlay samplers shared one sampler index. Fixed.
The overlay passes are not RSX draws, but their shaders go through the same
`spv_to_msl`, and their combined image samplers sit at the argument-table
indices, 0 and 1, below the RSX texture-range base of 12. The A9 rule guarded
the sampler index with `slot_has_sampler`, which is true only for the
twelve-to-twenty-seven texture units, so a sampler below the base kept no
index. SPIRV-Cross keeps its default index 0 for a binding that carries none,
so both overlay samplers reached MSL at `[[sampler(0)]]` and the Metal compiler
rejected the fragment library of the first overlay pass, `present_scale_pass`,
verbatim: "cannot reserve 'sampler' resource location at index 0". Both overlay
fragment shaders declare two samplers (`VideoOutCalibrationPass.glsl:6-7` and
`OverlayRenderFS.glsl`), and the cross-compiler pushes a sampler argument for
every declared sampled image even when the shader only fetches it, so no
overlay fragment library could build and the output pass died with the
translation. The status quo is measured on Apple A18 Pro through
`MTL4::Compiler::newLibrary` with the backend's own `CompilerDescriptor`
(`/tmp/mtlsampler`): two samplers at `[[sampler(0)]]` are rejected with that
message, `[[sampler(0)]]` with `[[sampler(1)]]` compiles, and `[[sampler(0)]]`
with `[[sampler(31)]]` fails with the other message, "must be between 0 and
15", which rules out the reading that the failing module kept the RSX
bindings of 30 and 31.

The rule is now one expression: `slot_has_sampler(slot) ? texture_sampler_index
(slot) : slot`. A texture unit keeps its position inside the stage's range,
which is A9, and a combined image sampler outside the units is an overlay
sampler, whose index is its binding, because the overlay argument table takes
exactly that binding as its own index (`MTLOverlays.cpp` binds the sampler
state at the shader's binding and the table declares two sampler entries). No
signature moved: the two callers of `spv_to_msl` already pass the slot they
mean.

The RSX programs are untouched by the second half of the rule, read from the
declarations rather than assumed. The units are the only combined image
samplers a stage declares between 12 and 27, so they take the unit branch. The
stencil mirror at 30 and the depth input at 50 are separate samplerless
textures (A9), which the cross-compiler reports as separate images, so the
rule is not reached for them. The vertex texel buffers (`usamplerBuffer`,
bindings 5 and 6) are SPIR-V images of dimension `Buffer`, and the vendored
cross-compiler emits no sampler resource for those
(`spirv_msl.cpp:15557`, the sampler is pushed only when the dimension is not
`Buffer`), so the index the new rule assigns there is inert and the emitted
MSL is unchanged.

One hypothesis this register must not carry forward: the overlay snippets'
`#ifdef VULKAN` branches are live in this backend, not dead. glslang defines
`VULKAN` itself whenever it compiles GLSL for Vulkan, in the preamble it
generates (`Versions.cpp:708-712`, `#define VULKAN <spvVersion.vulkanGlsl>`),
so the backend must not define it, and an attempt to do so fails the vertex
stage with "Macro redefined; different substitutions: VULKAN" instead of
selecting anything (measured, run 3). The `%preprocessor` placeholder the
overlay sources carry is the backend's own preamble seam, and the snippets'
Vulkan branch is already the branch the argument table was built for.

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

**Resolution.** Both sites now use the encode. `surface::write_to_memory` and
`cached_texture_section::copy_texture` copy the host depth and stencil planes into shared
staging buffers, call `command_recorder::flush_for_readback()` once, and then run
`encode_depth_plane` into the guest-pitch row. The stencil plane is copied with
`get_depth_plane_option(..., true)` for a combined format, so a z24s8 read-back keeps its
stencil byte. The bit-exact unscaled path is bounded by the host extent and the guest
row. TX12 is the texture-cache site and is fixed with the same code.

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

### B10 — `insert_texture_barrier` is empty. Fixed by the pass scheduler, then by I4.
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
loop needs. VK's `strong_ordering` flag and its barrier-per-loop tracker are its
own deduplication, which the per-frame record supplies here.

The scheduler's records only match when the sampled unit resolves to the same
`MTL::ResourceID` as the attachment, which is not always the case, so I4 later
gave the hook a real edge: it now flags the request the renderer turns into one
queue-scope barrier for the next pass. The two mechanisms are complementary and
the counter stays as the evidence channel.

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

### C11 — the pipeline's topology class contradicted the point-size program. Fixed.
The pipeline's class came from the draw's primitive family alone
(`get_primitive_topology_class`). The RSX vertex program declares the point-size
output in its output mask, and the MSL vertex shader then writes `[[point_size]]`
(`MTLVertexProgram.cpp:408`), so a triangle draw of such a program asked the
Metal compiler for a triangle-class pipeline over a point-size vertex function
and the compiler rejected it with "Vertex shader writes point size but
inputPrimitiveTopology is MTLPrimitiveTopologyClassTriangle". The verdict is
measured on the target device through the backend's own build path
(`MTL4::Compiler::newRenderPipelineState`, `/tmp/mtlpointsize`): a point-size
vertex function is accepted with the `unspecified` and the `point` classes and
rejected with the `line` and the `triangle` classes, each with its own message,
while a vertex function that does not write the output is accepted with all
four. The rejected build left the pipeline null, so the draw fell to
`execute_nop_draw` and its geometry was dropped; run 4's log carries the
rejection at `0:01:16.992121` and again at `0:01:42.916699`.

The class is now decided by the draw family and the program together. A draw of
the point family keeps the point class, which the point-size output belongs to.
A draw of any other family whose program writes that output takes
`MTLPrimitiveTopologyClassUnspecified`, the class that constrains nothing. The
substitution preserves RSX behaviour exactly, because a non-point primitive
ignores that output, so nothing downstream of the rasteriser depends on it.

The placement of the rule is load-bearing. It sits in
`capture_pipeline_properties`, not in `validate_pipeline_properties`, because
the shared program cache calls the validate hook only for a pipeline it already
holds (`MTLProgramPipeline.cpp:159`): a substitution applied there would never
reach the build of a new pipeline, and a cache hit and a miss would disagree on
the class. The capture runs on every draw, before the lookup. The class is part
of the pipeline key (`MTLPipeline.h:102`; `operator==` includes
`input_primitive_topology`), so the two forms are two entries and neither can
serve the other.

One neighbouring rule must not read the substituted value. The cull-both rule
(E1) drops the rasteriser for a triangle-class draw only, because the RSX culls
triangles only, so it reads the draw's family and never the class the point-size
rule may have put in the key.

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

### F5 — the flip ignores the guest present state. Fixed.
`MTLGSRender.cpp:977` never read `info.skip_frame` (`VKPresent.cpp:495`), never
read `display_buffers[info.buffer].width/height/pitch` (`VKPresent.cpp:513-515`),
never consulted `avconfig` for the format, the gamma or the stereo mode
(`VKPresent.cpp:532-541`), and never handled the stereo second eye
(`VKPresent.cpp:560-580`). A skipped frame was still presented and the wrong
extent could be used. The flip now drops a skipped frame and takes the extent
from the display buffer, with the AV configuration as the fallback and the cap.

**The remaining half is closed and re-checked against the landed output tier.**
The earlier reading was that the present resolves its source by address and the
drawable takes the source size, so the display buffer's pitch and format and the
stereo second eye had nothing to land on. The tree now consumes all three: the
flip reads the display buffer's pitch into the source lookup
(`MTLGSRender.cpp:2664`, passed to `get_present_source`), resolves the source with
the AV configuration's compatible GCM format (`av_format`,
`get_compatible_gcm_format`), and resolves a second image for the stereo eye below
the blanking interval and hands both to `present_source{ image, image2, ... }`
(`:2733-2748`), which the output path composes with the stereo flag. The drawable
is no longer the source size either (F9): it is the client area, so the pitch and
the format cannot dictate it. Nothing in the preamble is left unread; the stereo
second eye keeps one deviation, that it is composed only when the AV
configuration reports stereo, which is VK's own condition
(`VKPresent.cpp:560-580`).

### F6 — `flush` loses a barrier that requests a split. Fixed.
The pre-fix `flush` set `m_split` when `emit_barrier` returned false, but still ran
`m_pending.clear()`, so the edge was discarded; `split_requested()`
(`MTLPassScheduler.h:50`) had no caller. A barrier that cannot be emitted inside
an encoder must survive as a cross-pass edge. `flush` now retains the specs it
cannot emit (`MTLPassScheduler.cpp:89-104`), `begin_pass` no longer clears them, and
`emit_split_barriers` emits them on the encoder the caller opens after the split
(`MTLPassScheduler.cpp:106-122`).

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

### F9 — the drawable layer is configured incompletely and sized from the source. Fixed.
`MTLMetalLayer.mm:31-36` and `:44-54` never set `maximumDrawableCount`,
`displaySyncEnabled`, `allowsNextDrawableTimeout` or `colorspace`, and
`drawableSize` is set from the source resolution (HLD-06 §2.2). The drawable must
track the client area scaled by the backing scale factor, and the timeout, vsync
and colour-space properties govern the present. The layer now sets
`maximumDrawableCount = 3` (one per ring slot), `displaySyncEnabled`,
`allowsNextDrawableTimeout` and the sRGB colour space. The drawable size follows
the client area in device pixels (`gs_frame::client_width` multiplies by the
device pixel ratio), which is the size the output path scales into, so this half
is closed.

**The present-mode half is closed too, with one framework limitation.**
`presenter::update_vsync` re-reads `g_cfg.video.vsync` on every acquire and maps
all three modes: `full` sets `displaySyncEnabled`, `off` clears it, and `adaptive`
clears it and paces the present with `presentAfterMinimumDuration` at the display's
refresh interval, which `metal_layer_refresh_interval` reads from
`NSScreen.maximumFramesPerSecond`. The layer, the drawable pool and the sync
setting are therefore all driven. The one thing Metal cannot express is Vulkan's
mailbox mode: the framework has no queue-ahead present mode, so `adaptive` is a
pacing approximation rather than an exact yield. That is a property of the
framework, not a gap in the tree, and the run-4 telemetry shows the mapping in
effect (`sync 0`, `present interval 0.0 ms` with VSync off, display latency 11-15
ms at the 60 Hz panel rate).

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

### F13 — the present source was resolved with the surface-colour format. Fixed.
`get_present_source` holds a display buffer's format as `u32` and reinterpreted
it as a surface-colour format (`static_cast<rsx::surface_color_format>`) before
the call that sizes its block, so that call resolved to the surface-colour
overload of `get_format_block_size_in_bytes` (`TextureUtils.cpp:1436-1461`). The
display format is a GCM *texture* format, not a surface-colour format: the AV
configuration reports it in the texture space (`rsx_utils.cpp:151`), the image
lookup and the pixel format below it take it in that space, and the Vulkan
present path passes the same value to the same function with no cast
(`VKPresent.cpp:314`). `CELL_GCM_TEXTURE_A8R8G8B8` is `0x85`, which is not a
surface-colour format, and that overload throws "Invalid color format 0x85"
(`TextureUtils.cpp:1461`).

The throw is fatal to the RSX thread. Run 4 died of it twice in one session, at
`0:01:16.992871` and at `0:01:42.917262`, and each death followed the flip after
a `CLEAR_SURFACE` command dump; the process then ran on with its RSX thread dead
until the watchdog's cap. The cast is dropped, so the argument selects the
texture overload, which is the space the value belongs to.

### F14 — the backend presented to a layer the display does not composite. Fixed.
`presenter::init` built its own `CAMetalLayer` and assigned it to the Qt view.
A window carries a CAMetalLayer only when its surface type asks for one: Qt
returns a Metal layer from `makeBackingLayer` for `QSurface::MetalSurface` and
`QSurface::VulkanSurface` and a plain `CALayer` otherwise (Qt,
`qnsview_drawing.mm:80-87` and `:109-121`). The surface type is `gs_frame`'s,
and it was set for Vulkan alone (`gs_frame.cpp:96-100`), so the Metal renderer
ran on a raster window. Its content layer was then a plain `CALayer`, Qt's
`setLayer:` had already wrapped it in a container layer and made the view its
delegate, and the layer the backend assigned was a second layer on a
layer-backed view. The draws of that layer had no display behind them.

The fix is the half the VK path already had. `gs_frame` sets the Metal-capable
surface type for the Metal renderer too, and `attach_metal_layer` adopts the
`CAMetalLayer` the view carries, below Qt's container layer when it is there,
and builds one only for a view that has none. Only a layer the backend built is
the backend's to release (`detach_metal_layer`). Qt keeps the plain
`CAMetalLayer` flavour for a Vulkan surface, because a client that presents
outside a Core Animation transaction cannot drive a `QMetalLayer`
(`qnsview_drawing.mm:116-121`), and both backends are such a client.

### F15 — an acquired drawable could be left without a present. Fixed.
Two paths acquired a drawable and then dropped it. `presenter::drop()` cleared
`m_drawable` when the output pass failed to open, while the queue still held
that drawable from `set_present_drawable` and signalled it, so the drawable was
signalled and never presented. `acquire` cleared the recorder's drawable and
returned when the drawable carried no texture, with the same result. A metal
layer returns a drawable to its pool only after the display has shown it, so
each path stranded one of the three drawables the ring keeps.

`drop()` is deleted. The failed-pass path logs the failure and presents the
drawable empty, and `acquire` keeps the drawable of a textureless frame so
`present` presents it. The contract is then one line: every acquired drawable is
presented.

The acquire takes its own reference to the drawable, and the acquire and the
present run inside an `NS::AutoreleasePool`, because the RSX thread runs no pool
of its own and the framework returns autoreleased objects on it (Apple,
presenting metal drawables).

### F16 — the layer residency set was read below its availability. Fixed.
`metal_layer_residency_set` read `CAMetalLayer.residencySet`, which is macOS 26
and later, with a 15.0 deployment target. The read is now behind
`@available(macOS 26.0, *)`, so a 15.0 build cannot reach it.

### F18 — the savedata message dialog crashes on macOS. Upstream, not this backend.
Mega Man 9 (NPUB30036) reaches its autosave warning at 16.7 s of emulation and RPCS3 opens a message
dialog. Dismissing that dialog reads null on the Qt main thread and freezes emulation:

```
·! 0:00:16.671878 cellSysutil: cellMsgDialogOpen2(type=0xa0, msgString=“This game saves data automatically ... indicates the game is being saved.”)
·! 0:00:16.674709 Input: SetIntercepted: pads=1, keyboards=1, mice=1, src=open_msg_dialog
·! 0:00:22.455184 Input: SetIntercepted: pads=0, keyboards=0, mice=0, src=operator()
·F 0:00:22.456263 SYS:
Segfault reading location 0000000000000000 at 00000001008bb1dc.
Thread: Main Thread.
```

The faulting instruction loads a null member and calls through it (`ldr x0, [x19, #0x48]` /
`ldr x8, [x0]`), in a file-local lambda of the Qt UI, at imageOffset `0x29b1dc` of the `rpcs3` image
(crash report `rpcs3-2026-10-18-025735.ips`, slice UUID `1F5C1153-FA89-3902-B9DC-6858BB408FD2`, which
matches the build). This is upstream issue **#15092, "macOS: Crash on game savedata dialogs"** (open;
labels *Bug*, *OS: macOS*). That report names Mega Man 9, Mega Man 10, Sonic 4 Episode 1/2, Sonic The
Fighters and Sonic Generations, gives the trigger as "when game prompts to create a save file or warn
to not exit the game while is saving", and comes from RPCS3 0.0.30-16001 on macOS 14.3, two years
before this backend existed. No file of the Metal 4 backend takes part: `msg_dialog_frame.cpp`
carries no renderer reference.

Which path this backend takes: `open_msg_dialog` uses the overlay dialog when
`g_fxo->try_get<rsx::overlays::display_manager>()` succeeds (`cellMsgDialog.cpp:177`, `:133`), and
`rsx::thread` first created that manager for `video_renderer::opengl` and `video_renderer::vulkan`
alone (`RSXThread.cpp:858`). The Metal renderer was added to that condition, so this backend now
carries an overlay tier, the dialog is drawn inside the game window, it opens and closes with the
pad, and the crash does not reproduce. The Qt widgets (`cellMsgDialog.cpp:278`) stay the fallback for
a renderer without an overlay tier, and the upstream crash stays open for that path. `[V]`

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

### E7 — the depth-bounds test has no usable native path. Partial. (pass label F7)
The RSX has `depth_bounds_test_enabled`/`depth_bounds_min`/`depth_bounds_max`
(`rsx_methods.h:428-441`) and VK applies them (`VKDraw.cpp:241-271`). The latch
this entry added stands and is right: `capture_depth_bounds` reads the three
registers, saturates the range into the depth format
(`clamp_depth_bounds_value`), and, when the draw's
`RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE` is set, encodes it the same way the VK
path does (`encode_emulated_depth`), then clamps to the Metal `[0,1]` viewport
range. The flag is read at the call site, because
`requires_depth_range_emulation` and `current_fragment_program` are protected
`rsx::thread` members and the latch is a free function.

The apply does not stand. This entry read the presence of
`MTL4::RenderCommandEncoder::setDepthTestBounds` as a native path and the
cluster called it on every encoder state application, but the call reaches the
driver's `-[AGXG17FamilyRenderContext_mtlnext setDepthTestMinBound:maxBound:]`
and traps there. The first draw that a working pipeline carried died of it:
`EXC_BREAKPOINT`, SIGTRAP, exit 133, faulting thread `rsx::thread`, frame 0 of
the crash report of 2026-10-18 01:07:17, with `apply_encoder_state` ←
`MTLGSRender::end()` ← `rsx::nv4097::set_begin_end` below it. No earlier run
reached the encoder, so first light was the first exercise of the call. The
argument is not the cause: `capture_depth_bounds` clamps both bounds into
`[0,1]` after the format saturation and the emulated encode, so no value outside
the range can reach the setter, and the disabled test passes `[0,1]`.

The design had already ruled the native path out, and this entry contradicted it:
`LLD-depth-rop` §3.4 states that `MTL::DepthStencilDescriptor` has no bounds test
and no encoder setter adds one, maps the dial to a fragment-shader compare that
`discard_fragment()`s outside `[min, max]` with the bounds fed as fragment
constants (`HLD-05 §3.3`), and records the silent drop as the bring-up fallback,
the fallback the VK path takes on a device without support
(`VKPipelineCompiler.cpp:118-125`). The framework does carry the selector; this
driver traps on it. The call is therefore dropped for bring-up, which is the
design's fallback, and the latch stays for the emulation epilogue, which is what
the register now waits on. Until that epilogue lands, a depth-bounds title is
under-culled on Metal.

**The epilogue's shape is known.** The fragment stage already receives per-draw
push-constant blocks besides its constant file (`MTLArgumentBinds.h`: the flat
fragment constants at [2], the fragment context at [3], the per-unit texture
parameters at [4] and the substituted push constant at [29]), and
`MTLFragmentProgram.cpp:541` ends every generated fragment with the shared ROP
epilogue (`glsl::insert_fragment_epilogue`). The emulation is therefore a third
piece of that epilogue: two fragment constants carrying the clamped bounds, written
per draw only when `depth_bounds_test_enabled()` holds, and a
`gl_FragCoord.z < min || gl_FragCoord.z > max` early `discard_fragment()`. The
pipeline key needs the flag, because the epilogue changes the generated MSL, and the
test must run after the depth write is decided, which means the depth write cannot
stay early. It is not written, because no title on this machine enables the test
(the latch reads the disabled default in every measured run) and an epilogue that
disables early-z for every draw of a title would be a real cost taken blind.

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

**Why it is a shared-code change and not a Metal patch.** MSL takes a bias only as
a lod option on the sample call itself (`texture.sample(sampler, coord, bias(b))`),
so the value has to be an argument of the generated call, not a property of the
sampler object. The sample calls come from the shared GLSL snippets, which emit
`texture2D(sampler, coord)` for a plain RSX sample; the decompiler would have to
emit the biased form, with the bias reaching it as a uniform from the per-unit
texture-parameter block (`MTLArgumentBinds.h`, fragment_texture_params at [4]),
which the sampler capture already fills. That is a change to `Emu/RSX/Program` and
to every backend's snippet, so a Metal-only patch would duplicate the shared
snippet, which the design rejects. VK expresses the same dial in the sampler and
pays the 0.5 rounding for a nearest mip filter, which this backend would inherit
with the change.

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

### E13 — the latch dataflow is not single-read and holds unconsumed state. Fixed, one deferred item. (pass §3)
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
The decision the entry left open is made here: they are **kept**. They are the
regression coverage for the decode, they cost a capture of five scalars per clause,
and releasing them would remove the only reader that pins those decoder rows. A
harness that no longer wants them can prune them with the row that asserts them, in
one change.

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

### G1 — the flat-shading path is disabled on Metal. Limitation, with the expression costed.

RSX flat shading takes its value from the provoking vertex, which for the RSX is the
last vertex of the primitive. Vulkan and OpenGL select that vertex with a
last-provoking-vertex state and set
`backend_configuration::supports_last_provoking_vertex`; Metal exposes no such state,
so `MTLDevice.cpp` leaves the capability false. The shared code then clears
`RSX_SHADER_CONTROL_FLAT_SHADING` (`RSXThread.cpp:2368-2373`), so a flat-shaded
attribute is interpolated smoothly instead of taking one vertex's value, and the Metal
vertex program's own flat path (`MTLVertexProgram.cpp:246`) is dead code.

**The capability value is correct, and the evidence is external.** Metal's `flat`
interpolation takes the **first** vertex of the primitive; the RSX takes the last.
The metal-cpp headers of this SDK carry no provoking-vertex state at all
(`Metal/MTLRenderPipeline.hpp`, `Metal4/MTLRenderCommandEncoder.hpp`: no
`ProvokingVertex` symbol), so no assignment can express the RSX convention. Two
independent statements agree: ANGLE's Metal backend records that "Metal uses first
vertex as provoking vertex while OpenGL uses last vertex, thus it's very difficult to
implement flat shading conforming to OpenGL" (angleproject issue 40096506), and
MoltenVK does not expose `VK_EXT_provoking_vertex` at all (MoltenVK issue 2237), which
is the same fact seen from the Vulkan side. Setting the capability true would
therefore be wrong, not a fix.

**The expressions, costed.** The emulation must move the RSX's last vertex to Metal's
first slot, or carry its attribute forward, per primitive:

- *An index rotation.* The indexed draw already copies the guest indices into the
  index ring (`MTLGSRender.cpp`, `drawIndexedPrimitives` against
  `frame.constants.index_ring()`), so a flat draw can rotate each primitive's triple
  `(i0,i1,i2)` to `(i2,i0,i1)` inside that copy at no extra upload. An inline
  (non-indexed) draw has no indices and would need the same rotation applied to the
  vertex stream, which is a full per-vertex permutation. A triangle strip or fan
  cannot be rotated in place at all: each of its triangles must become three list
  vertices, which multiplies the vertex count and the index span.
- *A provoking-attribute replicate.* The vertex program cannot know which vertex is
  last; Metal has no geometry shader, and a mesh-shader path (`MTL4::MeshRenderPipeline`)
  is a whole new pipeline class, not a state fix.

Both are a real feature with a real cost, they change pixels by design, and no title
on this machine flat-shades, so no measured run can decide between them. The backend
therefore records the drop, which is exactly the fallback Vulkan takes on a device
without the extension (`VKGSRender.cpp:752-754`, a warning and smooth shading). The
first flat-shading title reopens it; the index rotation is then the cheaper expression
for a triangle list, and the strip/fan conversion its cost.

### G2 — `supports_multidraw` is inert. Limitation.

`rsx::thread` declares `supports_multidraw` (`RSXThread.h:82`); all three backends
assign it; a repository-wide search finds no reader. The field is a capability the
shared code never consults, so it is inert in the same way as the E12 key fields. On
Metal the `false` costs nothing, because the flag changes no code path, but the field
should be either consumed or recorded as inert.

### G3 — the boot preload showed no dialog. Fixed.
The first observed run reported "I didn't see any shader precompilation stage": the
warm start rebuilt the pipeline set with no progress shown. The cause is a parity
miss, not a missing load. `MTLGSRender::on_init_thread` called
`shaders->load(nullptr, ...)`, and the fallback the shared loader builds when the
dialog argument is null is the no-op base class (`rsx_cache.h:288-296`), so the
"Preloading cached shaders from disk.\nPlease wait..." dialog that Vulkan and OpenGL
post never existed.

Vulkan and OpenGL choose the dialog by the overlay manager
(`VKGSRender.cpp:1370-1391`, `GLGSRender.cpp:441-460`): a real
`rsx::shader_loading_dialog_native` when `m_overlay_manager` is set, otherwise the
frame is hidden and the load runs with a null dialog. Metal now takes the same two
paths. The native dialog draws through the renderer's own flip, which the Metal
present path already serves for every overlay (`MTLGSRender.cpp`, the overlay tier of
the output pass), and the progress updates reach it from the RSX thread, which is the
thread the load runs on. One boot confirms the dialog and the warm load together.

## H — the first-light bring-up

The first frame the backend presented was black. This pass read the bring-up evidence
against the code and recorded the three findings below. All three are fixed.

### F17 — the commit options were reused between commits. Fixed.
`command_recorder::commit_slot` built one `MTL4::CommitOptions` for the whole run and
passed it to every `commit`. Metal keeps the options object until the commit's feedback
is delivered; the code comment records the rule and the reference driver's behaviour
(`MTLCommandRecorder.cpp:213-217`, Apple `MTL4CommitOptions` and
`MTL4CommandQueue::commit`, and KosmicKrisp building one per submit in
`kk_queue_commit`). A checkpoint that reused the instance delivered feedback for its
first commit alone, so the second checkpoint stayed in flight, and the frame ring
stopped once its three slots were in flight. This is the stall runs 11, 12 and 16
measured. `commit_slot` now allocates one options object per commit and releases it
after the call (`MTLCommandRecorder.cpp:211-225`).

### F19 — the argument table is captured by the encoder, not by the table. Fixed.
Metal 4 snapshots an argument table when the encoder binds it, and `open_pass` closes
and recreates the encoder for every pass ("Metal 4:" the recorder opens one encoder per
pass, `MTLCommandRecorder.cpp:174`). The binder writes only the slots whose value
changed, so a slot written outside the current encoder was absent from the table that
encoder used, and the first draw of a pass could run against a table that lacked what
the shader reads. Every draw now re-binds both tables into the encoder it opens, and
the return of the binder stays a write signal, not a bind signal
(`MTLGSRender.cpp:1489-1497`). The measurement that closed the search: a full run
reported a write for both stages on 3,001 of 3,001 binds, so the table content was
never the cause of the black frame. The finding is kept because the defect is real.

### F20 — the packed GCM formats were mapped to `BGRA8Unorm`. Fixed.
On Apple silicon the shared uploader keeps a packed GCM format packed, and the BGRA8
expansion below the guard is Intel-Mac only (`TextureUtils.cpp:950`); Vulkan carries
the same guard and picks the native packed formats (`VKFormats.cpp:200-215`). The Metal
format map sent all six packed formats to `BGRA8Unorm` anyway, so a packed upload was
read as BGRA8 and the source row covered half the destination row. The map now returns
the native packed formats with the measured channel order, and `A4R4G4B4` needs a
`{Green, Blue, Alpha, Red}` rotation (`MTLFormats.cpp:47-52`, `:255-262`; the
measurement is `HANDOFF.md` 4.7). A live run reports host format 41 for guest `0xb7`.

## Live defects from the black-frame investigation

The investigation that chased the black frame found these on the way. They are
recorded because each is a real defect in the tree, independent of whether the
black frame is theirs.

### I1 — the upload blit held no write record, so its edge was never emitted. Fixed.
`pass_scheduler::note_read` emits a read-after-write edge only when the resource
has a live write record (`MTLPassScheduler.cpp:56-60`), and the CPU upload wrote a
texture through a compute encoder without recording it. A unit that resolved from
the CPU therefore had no `StageBlit → StageFragment` edge on the draw that read it.
`load_texture_env` now records the write for a unit it just resolved from the CPU,
in both stages (`MTLGSRender.cpp`, the two `note_write` calls after
`resolve_texture_bind`). The record is conservative: a reused cache section also
records a write, which costs a redundant edge but never misses a required one.

### I2 — the shutdown drain had no bound. Fixed.
`command_recorder::shutdown` waited on `ring_idle()` with no timeout, so a commit
whose feedback never arrives held the window close for ever. Run 25 ended with
exit code 143, which is this watchdog's TERM, not the game's own exit: the window
close hung. The wait is now bounded at 5 s and reports the slots still in flight
(`MTLCommandRecorder.cpp:49-70`).

### I3 — the layer's residency set was dropped before the ring drained. Fixed.
`on_exit` removed the presenter's residency set first and drained the ring last
(`MTLGSRender.cpp:581-592`). A frame still in flight refers to the drawable's set,
so dropping the set first can fault the GPU and lose the commit feedback the drain
waits for. The drain now precedes the presenter's teardown.

### I4 — `insert_texture_barrier` is a counter with no barrier. Fixed.
`MTLTextureCache.h:511` incremented a diagnostic counter and emitted nothing, while
the reference backends emit a barrier, because a pass that samples the surface it is
writing needs one. B10 had left the hook empty on the reasoning that the F2 scheduler
records the same pair; the stricter reading is the one that holds, because the
scheduler keys its records by `MTL::ResourceID`, and the surface a unit samples is not
always the attachment object the pass writes (B10's own note). The hook is now the
channel that closes that gap.

The cache cannot reach the scheduler: it resolves a descriptor while the previous
pass's encoder is already closed, and it holds no frame state. The request therefore
crosses the pass boundary as one flag on the cache
(`texture_cache::consume_texture_barrier_request`), and the renderer turns it into an
edge in `record_pass_hazards`, which is the one place that owns the encoding order.
`pass_scheduler::request_texture_barrier` queues it in the carried list, because
`begin_pass` clears the per-pass queue and the sampler resolves before the consuming
pass opens. `emit_split_barriers` then emits it on the new encoder with the queue
form: the producer is the fragment and tile stage of the pass that has flushed, and
the consumer is the vertex and fragment stage of the pass that samples. That is the
only form a render encoder cannot express itself (HLD-03 §2.1 item 5), and it is
correct in both call orders, because the deferral makes the writer's encoder closed
either way.

The counter stays as the evidence channel: a title that reaches the hook now emits a
real barrier and reports it. No measured run has reached it, so the fix costs nothing
where it is not needed. A GPU-free row pins the contract
(`test_rsx_metal4_state.cpp`, the single-read flag and the no-split invariant).

### I5 — the blit-engine fast path is declined. Limitation, with the shape recorded.
`MTLGSRender::scaled_image_from_memory` returns false (`:2410`), so every blit falls
to the CPU transfer. The measurement (texture-resolution audit §3) shows no byte loss,
because the CPU fallback does write guest memory, and no `scaled_image_from_memory`
transfer occurs in the measured runs. It is a performance and capability gap, not the
black frame.

The shape is known and small, which is why this is a recorded drop rather than an open
question. The shared cache already owns the whole path: `upload_scaled_image`
(`Common/texture_cache.h:3349`) resolves the source, the destination and the region
overlap, and calls exactly one backend primitive, a `blitter`
(`VKHelpers.h:134`, `scale_image(cmd, src, dst, src_area, dst_area, interpolate,
xfer_info)`). Vulkan's `texture_cache::blit` is a 17-line wrapper around it. Metal
would add the `blitter_type` trait, a `blit` on the cache, and one `scale_image` that
reuses the compute pass the present compose already uses for a scaled, filtered copy
(`MTLPresent.cpp`, the compose pass with a linear or nearest sampler). It is not
written, because no title on this machine uses a scaled blit, and the rule for this
backend is that unmeasurable work does not land: a wrong scaled blit corrupts guest
memory that today is correct. The first title whose transfer audit shows a
non-identity blit reopens it.

### I6 — the teardown abort is attributed and fixed. Fixed.
A SIGABRT in the main thread, after "All threads have been stopped", appeared
intermittently. The release binary is stripped, so the first reading named nothing.
The run-4 crash report (`~/Library/Logs/DiagnosticReports/rpcs3-2026-10-19-151439.ips`,
`bug_type 309`, `exception EXC_CRASH SIGABRT`, `termination code 6 SIGNAL "Abort
trap: 6"`, `asi libsystem_c.dylib ["abort() called"]`) names every frame:

```
abort <- report_fatal_error <- thread_base::initialize()::$_0::__invoke   (the handler)
      <- std::terminate <- unique_ptr<thread>::~unique_ptr
      <- __cxa_finalize_ranges <- exit <- -[NSApplication terminate:] <- libqcocoa
      <- gui_application::create_callbacks()::$_0 -> Emulator::Quit(bool)
```

The chain is the window's X: AppKit's `terminate:` runs `exit()`, which runs the
static destructors and no Qt object destructor. The object that dies joinable is
`midi_creator::m_midi_init_thread` (`rpcs3/rpcs3qt/midi_creator.cpp:15`), the async
rtmidi warm-up. Its only join site was `~midi_creator`, and that destructor runs only
when a `midi_creator` is destroyed: the `midi_creator m_midi_creator` member of
`emu_settings` never is on this path, and the local `midi_creator mc {}` of
`config_checker.cpp:299` is a different session's business. A joinable `std::thread`
destroyed by the exit handler calls `std::terminate`, and the installed handler
reports and aborts. The Metal backend takes no part; the defect is in the frontend and
predates it.

The fix makes the join independent of any destructor that `exit()` skips: a static
guard object defined after the thread pointer in the same unit joins it at static
destruction (`midi_creator::init_thread_guard`), and the three join sites share one
`join_init_thread()`. The guard is destroyed before the pointer it owns and after the
mutex, both defined before it in the same unit. The join is instant in every ordinary
session, because the thread is a one-shot rtmidi create that has long finished; it can
only wait where the alternative is the abort.

### I7 — the flicker is not buffer misrouting. Open.
Every present was paired with the flip that selected it: 32,829 pairs, the
presented address equal to the flipped buffer in all 32,829, and a mismatch in
none. The display does not present the wrong buffer. What remains is the content
of the two buffers, which the host readback now measures.

### Correction — the "CPU upload" reading was overstated.
`upload_texture` is the resolution entry point, not the CPU path: it searches the
surface store and the texture cache and only falls back to the CPU on a miss, so a
line logged at that call is a re-resolution, not a CPU upload. The run-25 lookup
settles the store side: `store lookup for 0xc1480000, bound 0, overlaps 1`, so the
store does hold the scene surface and the search does find it. The black frame is
therefore not a store miss, and the provenance of the bound image is the open
question, which the fragment-unit line now answers.

### I8 — every resource left residency one frame after its creation. Fixed.
This is the black frame. `resource_manager::new_buffer` and `new_texture` register each
sub-allocation with `residency_manager::on_bind`, which adds it to the set of the frame
slot that is recording. `residency_manager::on_commit_complete` then removes and
re-commits every allocation that slot tracked, when that slot comes round again. A
resource was therefore resident for at most one frame. Every resource this backend uses is
cache-owned and outlives its frame, so the vertex rings, the index ring, the stream views,
the constants and the surfaces were non-resident for the whole run.

A non-resident buffer read through an argument-table address returns zeros. The vertex
context matrix became zero, every attribute descriptor became zero, every vertex collapsed
to the origin, and no primitive covered a pixel. Metal reports nothing for this: the debug
layer and shader validation accept the read in silence, so run 31's clean log is
consistent with the failure. The render target kept the value that its first,
still-resident frame wrote, which is why every presented image held one word.

Measured with the run-31 shaders, state and bytes (`m4heap`): the same draw gives 230400
px magenta and 230400 px green when the vertex buffers are in the residency set, and no
coverage at all when they were registered at creation and then removed and re-committed,
which is the sequence the backend ran.

The fix follows HLD-02 §2.6 Tier 1, which puts the long-lived placement heaps in the
persistent set: a heap outlives its sub-allocations, so heap residency covers each of them
for its whole life. The per-frame sets had no member left and are gone
(`MTLResourceManager.cpp`, `MTLCommandRecorder.cpp`). `resource_manager::check_residency`
logs once if a sub-allocation's heap is not in the persistent set, which is the detection
HLD-02 F1 asks for.

### I9 — the transfer copy wrote without ordering the reads it overwrote. Fixed.
The placement heaps are untracked on purpose, because the pass scheduler inserts the
barriers (`MTLResourceManager.cpp:46`). Every copy that follows earlier work therefore
has to name its own edge, and one site did not. `copy_transfer_regions_impl` is the
texture cache's transfer: it opens a compute encoder and blits a resolved unit into a
destination image that comes from the temporary pool, so an earlier draw of the same
frame may still be sampling the very image the blit overwrites. Nothing ordered that
pair, and a pooled temporary is reused precisely because a title resolves many units, so
the path is reachable by any title with temporaries.

The scheduler's own record of the pair is not where the edge belongs.
`load_texture_env` notes the transfer write before the pass it belongs to opens
(`MTLGSRender.cpp`, the two `note_write` calls after `resolve_texture_bind`, I1), and
`begin_pass` clears the per-pass queue. The write half of that note is already encoded by
then, so a barrier in the draw's pass would come after the copy it must precede: the
clear drops the spec, and dropping it is right. What is not right is the copy having no
edge at all. B3 fixed exactly this class for the surface write-back
(`surface->read_barrier` first); the transfer now takes the same form every other copy in
this backend takes, a queue-scope barrier before it writes:

```
emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageBlit, MTL4::VisibilityOptionDevice, true });
```

The producer is every stage, which covers the read half whether the earlier sample was a
vertex or a fragment read, and `StageAll` includes the blit stage, so the transfers order
among themselves too. The measured runs never take this path (the transfer row is absent
from run 4's report), so the change costs nothing there, and where the path is taken the
ordering is required. `mirror_depth` (`MTLSurface.cpp:155`) and the host readback
(`MTLGSRender.cpp:1147`) already emit the identical spec on a compute encoder.

## The present-path investigation

The instrumented runs of the present path found two defects. Both are fixed.

### F21 — the Metal flip never ran the base flip. Fixed.
`MTLGSRender::flip` ended with `GSRender::flip(info)` and never called
`rsx::thread::flip(info)`. `GSRender::flip` only calls `m_frame->flip(m_context)`
(`GSRender.cpp:55-61`), so the base bookkeeping never ran. The Vulkan and the
OpenGL flips call both (`VKPresent.cpp:987`, `GLPresent.cpp:519`).

The base flip clears `async_flip_requested`, clears the display interrupt, and
stamps `last_host_flip_timestamp` (`RSXThread.cpp:2788-2816`). Without the clear,
the overlay request stayed set for the rest of the run. The gate in
`overlay::refresh` then passed on every call (`overlays.cpp:522-534`), so the
overlay asked for a flip on every local task. The RSX thread drained the drawable
ring and blocked in `nextDrawable`. A window of 150 frames measured 150 overlay
flips, and one `present/acquire/next drawable` cost 4299 us on average.

The flip now calls `rsx::thread::flip(info)` on the guard return and on the main
return (`MTLGSRender.cpp`). The same window measures a gated overlay flip rate and
one `present/acquire/next drawable` at 38 us on average. The run now paces to the
guest, not to the local task rate.

### F22 — the fault waited for an RSX flush that did not exist. Fixed.
A guest fault deferred to the RSX thread and then waited for the flush
(`fault/producer wait`). But `invalidate_address` unprotects the page in place for
every deferred fault that keeps no section to flush
(`texture_cache.h:1336-1351`), so the wait carried no work. It only delivered the
sampler flags. The faulting thread blocked for about one frame: a window of 150
frames measured 16886 us on average in the wait, and the guest thread lost 2.5 s
of the 2.68 s window.

The handler now waits only when `sections_to_flush` is not empty
(`MTLGSRender.cpp`). The queue removes a request after the RSX thread services it
and the owner stops waiting (`MTLGSRender.cpp`). The same window measures the
fault at 22 us on average, and the frame rate rose from 56 fps to 72 fps.

### F23 — the serializer configuration made the archive flush fail. Fixed.
The pipeline data set serializer was created with
`CaptureDescriptors | CaptureBinaries`. On this OS build that mask makes
`serializeAsArchiveAndFlushToURL` return NO with a null error object, so the L3
archive was never written and the log read "was not written: unknown". The header
documents `CaptureBinaries` as an addition to `CaptureDescriptors`, which is what
made the losing mask look correct.

`CaptureBinaries` alone writes the archive. The mask, the two API properties the
layer rests on (a flush is destructive; a warm boot re-captures what it looks up)
and the evidence are in HLD-07 §4.2. A standalone metal-cpp program settled it:
four runs per configuration, the combination failed in all four and
`CaptureBinaries` alone wrote a 39,568-byte archive in all four. Both archive paths
now log the error domain and code (`MTLDevice.cpp:19-31`), because a failed flush
can carry no error object at all.

### F24 — the RSX draw path omitted the clip-space Y flip. Fixed.
The translated shaders emit Vulkan-convention clip space, whose NDC Y points down,
and Metal's NDC points up. SPIRV-Cross does not add the flip: `Options::vertex.flip_vert_y`
defaults to `false` (`spirv_glsl.hpp:183`) and `build_msl` leaves it there
(`MTLShader.cpp:103-116`). KosmicKrisp negates the viewport height for every
Vulkan viewport for this reason (`kk_cmd_draw.c:778-793`). The Metal 4 tree
negated it only in the present passes (`m_vulkan_clip_space`, `MTLOverlays.cpp:381`),
so the RSX draw path rendered with a native rectangle (`MTLRasterState.cpp`).

A native rectangle stores every surface mirrored: `scale_offset_mat` maps the
guest's top row to `ndc.y = -1`, and Metal puts `ndc.y = -1` at the target's last
row. An uploaded texture keeps the guest row order (no cache flips rows), so a
surface's `v = 0` held the guest's bottom while a texture's `v = 0` held its top.
The present pass draws `pos(-1,-1)` with `uv(0,0)` and its height negation places
`ndc.y = -1` at the drawable's top, so the drawable's top sampled `v = 0`. That
single flip is right for an uploaded texture and wrong for a surface.

The image evidence pairs a 3-D title with a 2-D title on one build: Ridge Racer 7
presents from a surface on 1689 of 1690 flips (`present source from the surface`)
and showed an inverted frame, while Mega Man 9, which presents from the texture
cache, was upright. A vertical flip of the Ridge Racer 7 screenshot is perfectly
readable, so the defect was one Y flip, not a rotation and not a colour fault.

The draw path now carries the flip (`MTLRasterState.cpp`): the viewport rectangle is
`{0, clip_height, clip_width, -clip_height}`, the scissor mirrors about `clip_height`,
and the winding inverts because a Y mirror reverses triangle orientation. The
render-to-texture path had the same fault: a sampled surface read the image
upside-down, which a 2-D title does not exercise. The fix is verified on Ridge Racer 7
and on Mega Man 9.

## J — the parity sweep

A tenth pass re-read the draw, index, sampler, copy and present paths that the first nine passes
touched only in part. It found eight confirmed defects and two missing diagnostics. All ten are
fixed in the same build as this register.

| ID | Severity | Area | Status |
|---|---|---|---|
| J1 | High | indexed subrange address | Fixed |
| J2 | High | ring growth to the block size | Fixed |
| J3 | Medium | ring block alignment | Fixed |
| J4 | Medium | 16-bit strip restart | Fixed |
| J5 | Low | no-attribute clause drain | Fixed |
| J6 | Medium | shader-decoded sampler filter | Fixed |
| J7 | High | typeless texture copy | Fixed |
| J8 | Medium | live resolution-scale change | Fixed |
| J9 | Medium | native fp16 selection | Fixed |
| J10 | Medium | skip-reason diagnostics | Fixed |

### J1 — an index subrange put the byte offset in `baseVertex`

The Metal 4 indexed call is `drawIndexedPrimitives(type, count, indexType, address, length,
instanceCount, baseVertex, baseInstance)` (`MTL4RenderCommandEncoder.hpp:68`). It has no
index-buffer offset argument. The subrange loop passed `index_offset * type_size` as the seventh
argument, so Metal read it as a base vertex: subrange two drew vertex 4 instead of index 4.
The loop now advances the GPU address and shrinks the declared length, with `baseVertex = 0`.
The single-draw path was already correct.

### J2 and J3 — both byte rings could overrun or misalign

`stream_ring::alloc` and `byte_ring::alloc` compared `m_count + size` with `m_capacity`, then
allocated `m_capacity` for the replacement. One block larger than the ring therefore kept the old
capacity and the copy ran past the buffer end. `byte_ring` also advanced by the unaligned size.
Both now use `allocate_ring_block`: the cursor starts on a four-byte boundary, the reservation
asks for `max(capacity, aligned_size)`, and the next block starts at the aligned end. The helper is
Metal-free and unit-tested (four cases).

### J4 — Metal restarts every 16-bit strip at `0xFFFF`

Metal has no primitive-restart toggle; a strip restarts at the largest index value. RSX with
restart disabled treats `0xFFFF` as a vertex index, so a 16-bit strip cannot be passed through.
The index visitor widens such an array to `u32`; `0xFFFFFFFF` is the only restart value there and
no RSX title can address that vertex. Vulkan avoids the widening because it can disable restart.

### J5 — a no-attribute draw did not drain its clause

`GLDraw.cpp:755-766` runs the dependency loop and `draw_call.end()` before it returns. The Metal
path returned with neither. It now calls `execute_nop_draw()` and `end()`, which is the same pair
the other refusal paths use.

### J6 — the sampler filtered the encoded value

The fragment shader re-expands signed channels (SEXT), applies gamma and decodes the depth pack.
Linear filtering before that work filters the stored, encoded value, not the value the guest
defined. Vulkan forces nearest when `sext_conv_required` or a depth redirect is active
(`VKDraw.cpp:419-437`). The Metal `sampler_state` now carries `force_nearest`, set from
`texture.format_ex()` (SEXT or gamma) and the depth class; the builder overrides min, mag and mip
to nearest. The field joins equality and the hash.

### J7 — a typeless copy skipped and left a black temporary

`copy_transfer_regions_impl` rejected every copy whose source and destination formats differed,
so the destination temporary kept uninitialised contents. It now stages the raw texel bits through
a shared buffer: texture-to-buffer, a blit-to-blit barrier, then buffer-to-texture. Compressed
formats and unequal texel sizes still fail with a message because there is no bit-preserving route
for them. This is the likely mechanism behind a black sampled render target; it is built and
unit-tested, not yet confirmed on RR7.

### J8 — a live resolution-scale change was not applied

`resolution_scaling_config` is set at thread start. Vulkan re-reads the setting at every flip and
clones the resident surfaces when it changes (`VKPresent.cpp:1000-1020`). Metal never re-read it,
so a change at runtime left surfaces at the old salt. `flip` now compares and calls
`sync_scaling_config` before the output pass.

### J9 — the low-precision setting never reached the fragment decompiler

The decompiler forced `has_native_half_support = false`, so the f16 register path was dead on a
device that executes fp16 natively. It now follows `shader_precision == low`, as Vulkan does
(`VKFragmentProgram.cpp:630-638`). The two depth emulation bits stay false because Metal has
native depth compare and depth storage.

### J10 — the skip log could not name its cause

`should_skip_draw` has four independent predicates. The path logged a once-only line with no bits.
It now reports all four for the first 64 refusals at probe level 2.

## K — the shader-interpreter design audit

The interpreter was not implemented when this pass ran, so it reviewed the design
(`LLD-shader-interpreter.md`) against the current `MTL*` sources, the shared GLSL bodies and the
Metal 4 contract. It is not an implementation finding and is not counted in the A-J summary. The
full ledger is LLD §20; these are the source-confirmed blockers that would have produced a broken
first implementation:

- **K1** — the FS instruction block was placed at `set=1, binding=12`, which collides with the
  first compact texture array in the same GLSL descriptor set. GLSL/SPIR-V require unique
  `(set, binding)` pairs even though MSL's buffer and texture spaces are separate. The block moves
  to `set=0, binding=12`.
- **K2** — `IS_TEXTURE_RESIDENT` referenced `textures_resident`, but the block declares
  `reserved1`. The design now emits an alias.
- **K3** — the builder redefined `TEX_PARAM` differently and redefined `texture_base_index`;
  `MTLFragmentDecompilerThread::insertConstants` already emits both (`MTLFragmentProgram.cpp:174-177,
  328`). Only the fog/wpos macros are added, and `TEX_PARAM` is undefined first if it is touched.
- **K4** — the compact-array guard removed the sampler array but left the `case` label reachable.
  The guard must wrap the whole case and the switch needs a `default`.
- **K5** — the shared body exports four `ocol` outputs unconditionally. The interpreter must trim
  to `min(4, mrt_buffers_count)` and mask `write_mask` in `interpreter_only`, which has no real
  program validation.
- **K6** — one `argument_binder` caches per stage, not per table. Separate tables alone do not
  isolate the interpreter; a second binder and stage-specific instruction fields are required.
- **K7** — the byte ring aligns to 4, not 256, and resets every frame. The blocks must be aligned
  and forced on the first interpreter draw of each frame.
- **K8** — an async placeholder cannot be replaced by a later synchronous build. `can_interpret`
  and `allow_async` must be decided before the first `get_graphics_pipeline` call.
- **K9** — `MTLTraits::build_pipeline` already honours `compile_async`
  (`MTLProgramPipeline.cpp:1057-1059`); the draw path passes `true` unconditionally. Synchronous
  real builds and interpreter links must also share a compile mutex around the L18-sensitive
  `newRenderPipelineState`.
- **K10** — the rejection list omitted texture alpha kill, emulated depth compare, programmable
  blending, logic op and shadow/redirected/multisampled textures. `ATTRIBUTE_INTERPOLATION` is a
  normal triangle flag and must not be rejected.
- **K11** — the design called `MTL4::RenderPipelineState` a type; the compiler returns
  `MTL::RenderPipelineState*`. The descriptor is still `MTL4::RenderPipelineDescriptor`.
- **K12** — the full 468-vector vertex constant file and the existing `compiled_fragment` null
  guard are confirmed in current code, not open questions. The interpreter forces
  `frame.compiled_vertex`/`compiled_fragment` to null and clears `transform_constants_valid` on a
  kind change.
- **K13** — prebuild uses `compile_msl_async` for libraries; the link worker only links pipelines.
  The first-use wait can sit behind an in-flight guest link and is not bounded by priority.
- **K14** — MSL 1D samplers, runtime sampler-array indexing and `[[user(locn16)]]` remain
  unvalidated without a Metal CLI. The standalone probe is the gate.
- **K15** — the interpreter binder omitted the push-constant substitute at buffer 28 and the
  conditional stipple/instancing slots. The VS reads `draw_parameters_offset` from slot 28, so an
  unbound slot makes every context lookup wrong. The table binds `binds.vertex_draw_params` there
  and binds 8/9/10/11 when the draw uses them.
- **K16** — the single-type cache key still carried the referenced mask, so the four prebuilt
  variants would never be reused. The key clears the mask/dimensions for a single type and stores
  only the type; the live mask is in the per-draw header and the binder.

### K — implementation record (2026-10-21)

The interpreter landed. The compiler and the tests confirmed five further corrections; the first
four would have failed silently. The full record is LLD §21.

- **K17** — the FS builder's `#define VULKAN` collided with glslang's own `VULKAN` define for the
  Vulkan rule set (`'#define' : Macro redefined; different substitutions: VULKAN`). The interpreter
  sources must not define it.
- **K18** — the VS prologue omitted `_builtin_lit` (`no matching overloaded function found`) until
  `properties.has_lit_op` is set; the shared body emulates LIT.
- **K19** — forcing `in_register_mask` for wpos/fogc made the body read `fetch_fog_value()` /
  `get_wpos()`, which reference the named varying `fog_c` the interpreter never declares. The FS
  emits the `fog_*` / `wpos_*` context macros instead and does not set the register bits.
- **K20** — `empty()` tested `referenced_mask == 0`, but `canonicalize()` clears the mask for the
  single-type fast path, so a textured single-type program was treated as empty and lost
  `COMPILER_OPT_ENABLE_TEXTURES`. `empty()` is now `!is_single() && referenced_mask == 0`.
- **K21** — the first cut reused the regular argument tables. Beyond K6's isolation issue, a shared
  table let an interpreter bind clear the regular 12..27 slots while the regular binder's cache
  still held them, so an interleaved regular draw in the same frame read null/stale textures.
  `frame_slot` now owns `interpreter_vertex_table` / `interpreter_fragment_table` as the design
  required.

Verification: five `MTLInterpreter.*` tests pass and the suite is green (504/504 enabled). The perf
work (LLD §21.2, P1-P4) removed the per-draw `argument_binds` copy, the regular unit walks, the
per-draw ucode re-hash and the fixed 16-unit single-type bind. Runtime gates R24/K14 and the §17
questions are unchanged.

## M — the diagnostic facility (2026-10-21)

The 20:55 probe-2 run showed that the probe could measure only performance, not correctness: it had
no interpreter-versus-recompiler counter, no host-format record, no per-draw program identity and no
readback or depth record. The run evidence is L25–L33 in `LOG-FINDINGS.md`. This pass adds the
missing families and splits the output.

- **M1** — `MTL_PROBE_LATCH` registers a name once per call site, in a function-local `static`. A
  latch whose name varies per call (a ternary, a computed string) silently registers only the branch
  that ran first and then attributes every later event to it; the second name never appears in the
  coverage table. **Fourteen sites in the tree used a ternary name**, so every split family in the
  2026-10-21 probe-2 and probe-5 coverage tables is a branch total, not a branch count
  (`LOG-FINDINGS.md`, "Correction — the split latches of this run are unreliable"). All fourteen are
  now one macro per branch in an `if`/`else`, and the macro comment carries the rule. Any future
  conditional latch must split the same way.
- **M2** — the coverage table stores the key name by pointer. A family that builds its name at run
  time must intern it. The families added here use fixed names, so they do not need the intern step;
  a family that names a program or a format per event does, and that helper does not exist yet.
- **M3** — the bulk probe text (the perf report, the coverage table and the level-3/5 verbose family)
  now goes to `$TMPDIR/rpcs3-mtl-probe.log`, and a warning and an error stay in the shared log. The
  `MTL` channel therefore no longer needs the level-3 trace raise, and that raise is removed. The
  source of the split is one small `file_sink` in `MTLInstrumentation.h`, so it goes with the
  facility.
- **M4** — while a probe level is set, `sys_spu`, `sys_event`, `cellDmux` and `cellVdec` drop to
  `error`, because they were 80 % and 13 % of a 397 k-line log (`NON-METAL-FINDINGS.md` N11). The
  four-line block sits next to `probe::apply_log_level()` and is marked `TODO(revert)`. It is not
  part of the backend and must not survive the commit.
- **M5** — the run itself found three performance facts that the earlier probe could not separate
  because the fixtures were missing: the L13 label drain is a once-a-frame full GPU wait (L29), the
  semaphore wait is a spin at 12.6 % of wall (L30), and 10.1 % of flips compose with no display
  buffer (L31). The first two need a native mechanism, not a threshold tweak; the third needs the
  compose predicate.

## Parked for review

These are the findings a fix cannot settle from the tree alone. Each needs either
a hardware measurement, a Metal-4 behaviour check, or a decision the acceptance
harness owns. They are parked, not closed.

| ID | The question to answer first |
|---|---|
| A9 | The indices, the fill slot, the units, the sampler table and the two samplerless declarations are all landed and all under their caps. The depth-mirror form is measured (a samplerless `read()`, `read()` never `sample()`, indices 30/50 kept) and the sampler-count question is answered: the argument-buffer escape hatch compiled with no ceiling found up to 8000 entries. What remains is the in-tree GLSL-to-MSL hop, which no test reaches, so first light is the first verification. |
| B16 | Only the completion point: the encode is written and tested, and the predicate is settled (`depth_format_needs_decode`). What is unfinished is where the CPU pass runs, which needs a synchronisation point after the frame slot commits, and the stencil plane of a combined read-back. |
| E7 | Whether to write the fragment-shader depth-bounds epilogue or to keep the drop. The question LLD-depth-rop §6 Q3 left open is the input: does a live title enable the test at all? The native path is closed by the driver trap. |
| E8 | The sampler LOD bias needs a biased sample in the generated MSL, which is a change to the shared decompiler's snippet contract, not a Metal-side patch. The question is whether the per-unit texture-parameter block can carry the bias without a second snippet for Metal. |
| G1 | The capability value is settled by evidence: Metal's provoking vertex is the first, the RSX's is the last. What is parked is the emulation — an index rotation for a triangle list, and a strip or fan conversion that multiplies its vertices — until a title flat-shades. |
| I5 | Whether any title uses a scaled blit at all. The CPU fallback is correct and no measured transfer is non-identity, so the GPU path (the shared `upload_scaled_image` plus one `blitter`) waits for a title whose audit shows one. |
| I7 | The content of the two present buffers. The routing is proven (32,829 pairs, no mismatch) and the host readback is wired, so this needs the flicker to reproduce on a title; none on this machine has since the pairing was proven. |

<!-- doc-end -->
