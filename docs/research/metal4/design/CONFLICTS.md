# Conflict register

A conflict is a place where a design doc and the RSX tree disagree about a dial.
The rule: record the dial, both references, and move on.  Never rewrite either
side silently.  The RSX tree wins on what the RSX does.  A design doc wins on how
we re-express the RSX in Metal 4.

A citation conflict is a place where a design doc points at the wrong line of the
tree or of the inventory.  The register records it the same way, under the `X`
prefix.  A citation conflict changes no design decision.

---

## C1 — the clear region

**Dials.** `M_NV4097_SET_CLEAR_RECT_HORIZONTAL` (0x1d98),
`M_NV4097_SET_CLEAR_RECT_VERTICAL` (0x1d9c).

**Design doc says.** `LLD-surface.md:137-138`: "`SET_CLEAR_RECT_HORIZONTAL`/
`_VERTICAL` are unbound but read by `clear_surface`; they scope the clear to a
sub-rectangle."  `LLD-surface.md:257-259` and `:288-289` repeat that the clear
rect is the sub-rectangle source.

**The tree says.** The two registers have no reader anywhere:

- They are bound to `nullptr` in the method table
  (`rsx_methods.cpp:1476-1477`).
- No accessor exists in `rsx_methods.h` (no `clear_rect` symbol).
- Both backends scope the clear with the **scissor**, not the clear-rect
  registers: `VKGSRender.cpp:1441-1454` (`m_scissor`, then `rsx::clip_region`),
  `GLGSRender.cpp:668-671` (`scissor_origin_x`/`scissor_width`/...).
- The framebuffer layout reads neither register (`RSXThread.cpp:1472-1660`).

**Resolution.** Implement the clear region from the scissor state
(`scissor_origin_x`/`scissor_width`/`scissor_origin_y`/`scissor_height`,
`rsx_methods.h:204-222`), exactly as VK and GL do.  `MTLSurfaceState` reads the
scissor and records the conflict here.  The LLD text is left unchanged for its
author to correct; the disposition of the two dials stays as the shard records
it, because the registers are still unbound guest state.

**Open for the LLD author.** If a title is later found that writes the clear-rect
registers and expects them to take effect, the RSX would need a reader that does
not exist today.  No such reader is added by this port.

---

## C2 — a disabled depth test and depth writes

**Dials.** `M_NV4097_SET_DEPTH_TEST_ENABLE` (0xa74), `M_NV4097_SET_DEPTH_MASK`
(0xa70).

**The RSX says.** Two independent whole-word booleans (`rsx_decode.h`), both bound
to `set_surface_options_dirty_bit` (`nv4097.cpp:333-340`).  The decode states no
interaction, so the RSX primary sources do not say what a write does with the
test off.

**Design doc says.** `LLD-depth-rop.md:100` (disposition note): "Metal has no
separate enable; with the test off, writes stay governed by the depth mask."
`LLD-depth-rop.md:164-168` (§2.3) reshapes a disabled test to
`depthCompareFunction = Always` and is silent on the write.

**Both shipping backends suppress the write.**  `VKGSRender.cpp:282-286` calls
`set_depth_mask` only inside `if (depth_test_enabled())`, and
`graphics_pipeline_state.hpp:97` is the only writer of `depthWriteEnable`, so the
field stays false when the branch is skipped.  `GLDraw.cpp:86,98` sets the mask
unconditionally but toggles `GL_DEPTH_TEST`, and a disabled test leaves the depth
buffer unupdated.  The RSX is NV47-class, where a disabled test bypasses the
depth buffer; that is the hardware reading both backends encode.

**Resolution.** Encode a disabled test as `depthCompareFunction = Always` *and*
`depthWriteEnabled = false`.  `MTLDepthStencil` reads the mask only inside the
test-enabled branch and records the conflict here.  The LLD text is left
unchanged for its author to correct.

**Open.** Unverified against a title.  If the guest does write depth with the
test off, the guard is dropped and the mask applies unconditionally.

---

## C3 — the colour write mask and the host channel order

**Dials.** `M_NV4097_SET_COLOR_MASK` (0x324), `M_NV4097_SET_COLOR_MASK_MRT`
(0x370).

**The RSX says.** One byte per channel (A/R/G/B, `rsx_decode.h:3009`) for
attachment 0, and a 4-bit group per render target for the rest
(`rsx_decode.h:3064`).  Which channels a surface actually stores comes from the
surface colour format, not from the mask register.

**Design doc says.** `LLD-depth-rop.md:76,93` mark both dials `direct`, and
`LLD-depth-rop.md:170-178` (§2.4) states "The per-channel mask maps exactly onto
`MTLColorWriteMask`".  `LLD-depth-rop.md:259-265` (§3.2) repeats the direct
nibble fan-out and never mentions the format.

**The tree says.** The mask is not a direct image for the single- and
two-channel formats, because the host stores the RSX blue channel in the red
slot:

- `color_utils.hpp:267` (`get_b8_colormask`) and `:275`
  (`get_g8b8_r8g8_colormask`): `red = blue`.
- `color_utils.hpp:300` (`get_write_output_mask`): the channels the format stores
  (`b8` -> red only, `g8b8` -> red and green).
- `MTLFormats.cpp:15` maps `b8` -> `R8Unorm` and `g8b8` -> `RG8Unorm`, so the
  same red-slot storage applies to Metal.

**Resolution.** `capture_color_write_mask` takes the surface colour format and
applies the two shared helpers before building the `MTLColorWriteMask`.  The two
dials are therefore `reshape`, not `direct`, for `b8` and `g8b8`.  The LLD text
is left unchanged for its author to correct.

---

## C4 — the mip filter source

**Dials.** `M_NV4097_SET_TEXTURE_FILTER` (0x1a14, `min_filter`),
`M_NV4097_SET_TEXTURE_FORMAT` (0x1a00, the mip level count in bits 16-31).

**Design doc says.** `LLD-texture.md:211-212`: "`min_filter`/`mag_filter` →
`minFilter`/`magFilter`; `mipmap` (format bits 16-31) → `mipFilter`".  The LLD
assigns the minify filter to `minFilter` alone, and assigns `mipFilter` to the mip
level count.  It does not say how the count chooses Nearest or Linear.

**The RSX says.** The minify filter is a compound value.  Its two halves name the
minification filter and the mip treatment: `gcm_enums.h:1524-1533` reads "mipmap
base level" for `nearest`/`linear`, "closest mipmap level" for the `*_nearest`
values, and "linear mix between closest mipmap levels" for the `*_linear` values.
The mip level count is a property of the texture, not of the sampler; Metal takes
the mip count from the texture size.

`VKFormats.cpp:38-44` is the shipping backend's corroboration: it returns a
`VkSamplerMipmapMode` per minify value.

**Resolution.** `get_mip_filter` derives `MTL::SamplerMipFilter` from the minify
filter: `NotMipmapped` for `nearest`/`linear`, `Nearest` for the `*_nearest`
values, `Linear` for the `*_linear` values.  The mip level count does not choose
the filter.  The LLD text is left unchanged for its author to correct.

**Open.** Unverified against a title.  A title that samples a mip chain with
`min_filter == linear` takes the base level under this reading.

---

## C5 — the quad strip topology

**Dial.** `M_NV4097_SET_BEGIN_END` (0x1808, the `primitive_type` field).

**Design doc says.** `LLD-draw.md:224`: "`quads` / `quad_strip` | — | emulated (→
triangles)".  The LLD groups the quad strip with the quad list and marks both
emulated.

**The tree says.** The shared helper `is_primitive_native`
(`Common/BufferUtils.cpp:816-837`) returns true for `quad_strip` (line 825) and
false for `quads`.  The shared expander `write_index_array_data_to_buffer_impl`
(`Common/BufferUtils.cpp:940-960`) expands `quads` through `expand_indexed_quads`
and has no `quad_strip` case; the non-indexed expander rejects it with "Native
primitive type doesn't require expansion" (line 914).  Both shipping backends
agree: `GLHelpers.cpp:30` returns `GL_TRIANGLE_STRIP` and `VKVertexBuffers.cpp:26`
returns `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP`.

**Resolution.** Reuse the shared helper.  A quad strip maps to
`MTL::PrimitiveTypeTriangleStrip` and uploads untouched.  The LLD's emulation
claim needs an expander that does not exist, so this port does not implement it.
The LLD text is left unchanged for its author to correct.

**Open.** Unverified against a title.  A quad strip is not a triangle strip in
vertex order, so a title that draws one may show wrong triangles.  The tree's
reading is consistent across both shipping backends, so this port follows it.

---

## X1 — the `nv308a.cpp` inventory citation

**Design doc says.** `LLD-nv308a-image.md:32` and `:301` cite
`inventory/out/SUMMARY.md:518` for the `nv308a.cpp` "0 functions" row.

**The tree says.** The row is at `inventory/out/SUMMARY.md:520`.  Line 518 is
`nv0039.cpp`.

**Resolution.** The claim is correct: the generator does not count
`nv308a::color::impl`, a nested static member, so `nv308a.cpp` reports 0.  Only
the citation is off by two lines.  The LLD text is left unchanged for its author
to correct.

## C6 — the native pitch of an exploded surface

**Dials.** `surface` cluster: `M_NV4097_SET_SURFACE_FORMAT` (0x0208, the
antialias field) and `M_NV4097_SET_SURFACE_PITCH_A` (0x020c).

**Design doc says.** `HLD-02-resource-model.md:155-160` and `:485`: "`native_pitch`
— the Metal texture row stride … The invariant (§4) is `native_pitch ≥ rsx_pitch`
and `native_pitch ≥ bpp × width`."

**The tree says.** `surface_utils.h:256` defines
`get_bpp()` as `native_pitch / get_surface_width<rsx::surface_metrics::samples>()`,
and `surface_utils.h:205-208` defines that width as `surface_width * samples_x`.
So `get_bpp()` returns the format block size only when
`native_pitch == width × bpp × samples_x`.  The doc states a lower bound with no
upper bound, so a value that only satisfies the bound breaks `get_bpp()`, which
the generic store reads (`surface_utils.h:570`, `surface_utils.h:577`).

**Resolution.** `native_pitch = width × bpp × samples_x`, the exact value
`get_bpp()` requires.  It satisfies the doc's bound when `samples_x == 1` and
exceeds it otherwise.  The `≥ rsx_pitch` clause has no consumer: the host texture
row is not a guest row, and the guest row is `rsx_pitch`, which drives
`memory_range`.  The doc text is left unchanged for its author to correct.

---

## X2 — the `mtl_surface` field types

**Design doc says.** `HLD-02-resource-model.md:411-412` writes the `mtl_surface`
sketch with `MTL::Texture* texture;` and `MTL::Texture* resolve;`.

**The tree says.** `surface_utils.h:160` declares `resolve_surface` as
`std::unique_ptr<remove_pointer_t<image_storage_type>>`, so the storage type must
be an owning handle with a destructor.  A `metal-cpp` object is not destructible:
`NS::Object` declares `~Object() = delete` (`Foundation/NSObject.hpp:95`).
Further, `surface_utils.h:504` cross-casts an image to `rsx::ref_counted`, so the
storage type must be polymorphic, and `surface_store.h:391` casts an image back to
the surface type, so the surface type must derive from the image type.

**Resolution.** `image_storage_type = mtl::texture*` — the type
`HLD-01-architecture.md:240` names — `mtl::texture` is an owning, polymorphic
handle, and `mtl::surface` derives from it.  HLD-02 §3.2's sketch field types are
not implementable as written; the sketch is left unchanged for its author to
correct.

---

## C7 — cull face, front face, polygon mode and the depth/stencil formats

**Dials.** `raster` cluster: `M_NV4097_SET_CULL_FACE` (0x1830),
`M_NV4097_SET_CULL_FACE_ENABLE` (0x183c), `M_NV4097_SET_FRONT_FACE` (0x1834),
`M_NV4097_SET_FRONT_POLYGON_MODE` (0x1828), `M_NV4097_SET_BACK_POLYGON_MODE`
(0x182c) and `M_NV4097_SET_POLY_OFFSET_FILL_ENABLE` (0x0a68); plus the depth and
stencil fields of `M_NV4097_SET_SURFACE_FORMAT` (0x0208).

**Design doc says.** `HLD-05-shader-binding-contract.md:331` places the raster
cluster's cull face and front face in the **pipeline**: `setRasterizationEnabled`,
`setCullMode`, `setFrontFacingWinding`.  `:333` places the depth and stencil pixel
formats in the pipeline too.  `:166` repeats the assignment.

**The SDK says.** `3rdparty/metal-cpp/Metal/MTL4RenderPipeline.hpp` declares
`MTL4::RenderPipelineDescriptor` with `setRasterSampleCount` (`:189`),
`setRasterizationEnabled` (`:191`), `setInputPrimitiveTopology` (`:185`),
`setAlphaToCoverageState` (`:175`) and `setAlphaToOneState` (`:177`), and per
colour attachment `setPixelFormat` (`:82`), `setWriteMask` (`:90`) and the blend
setters (`:76`).  It declares no `setCullMode`, no `setFrontFacingWinding`, no
`setTriangleFillMode` and no depth/stencil attachment pixel format setter; its
base `MTL4::PipelineDescriptor` adds only `setLabel` and `setOptions`.  The older
Metal 3 `MTL::RenderPipelineDescriptor` does carry the format setters, so a
Metal 3 translation reads correctly here and a Metal 4 one does not.  The missing
state is on `MTL4::RenderCommandEncoder` instead: `setCullMode` (`:92`),
`setDepthBias` (`:94`), `setDepthClipMode` (`:96`), `setFrontFacingWinding`
(`:104`), `setScissorRect` (`:110`), `setStencilReferenceValues` (`:114`),
`setTriangleFillMode` (`:120`), `setViewport` (`:124`) and `setBlendColor`
(`:86`).  The depth and stencil attachment formats belong to the render pass.

**Resolution.** The cull face, the front face, the polygon mode, the depth bias
and the clip mode are encoder state and key nothing (`MTLEncoderState.h`).  The
depth and stencil formats are render-pass state.  `MTLPipeline.h` therefore keeps
only `raster_enabled` — a cull-both draw has no Metal cull mode, so it drops
rasterisation — plus the colour format, the attachment count, the sample count,
alpha-to-coverage, alpha-to-one, the blend latch and the write masks.  The effect
is a *smaller* PSO key and a higher cache hit rate than the doc implies.  The
`raster` dispositions (`raster.csv:21-26`) are already right: they name the
`MTL4RenderCommandEncoder` setters.  Only the HLD-05 stage assignment is wrong,
and it is left unchanged for its author to correct.

**One further correction, found while wiring the clear.**  The depth test and the
depth write are **not** individual encoder fields.  `MTL4RenderCommandEncoder`
declares `setDepthStencilState(const MTL::DepthStencilState*)` (`:98`) and no
`setDepthCompareFunction` and no `setDepthWriteEnabled`; the compare function and
the write flag are fields of `MTL::DepthStencilDescriptor`, consumed when the
immutable `MTL::DepthStencilState` is built.  So the `depth-rop` latch
(`MTLDepthStencil.h`) is the key of a cached object, and that object is the only
way the depth state reaches the encoder.  `create_depth_stencil_state`
(`MTLDepthStencil.cpp:154`) builds it; the *cache* and the `setDepthStencilState`
call are part of the draw path, so a backend that builds the object without
applying it has no depth test at all.  The built-in fill applies its own two
objects directly, because it must not be rejected by the state it is clearing.

---

## X3 — SPIRV-Cross is assumed but not vendored

**Design doc says.** `HLD-05-shader-binding-contract.md:78` describes SPIRV-Cross as
"battle-tested (MoltenVK, Godot) and is a pinned vendored revision", and `:58`
instructs the reader to "insert **SPIRV-Cross `CompilerMSL`** before
`MTL4Compiler`".

**The tree says.** There is no SPIRV-Cross in the tree. A search for any path or
file matching `*spirv*cross*` or `spirv_cross*` under `3rdparty/` and under the
whole repository returns nothing, and `.gitmodules` lists every third-party
submodule (ffmpeg, asmjit, llvm, glslang, zlib, hidapi, pugixml and the rest) with
no SPIRV-Cross among them. glslang **is** vendored (`3rdparty/glslang`), so
`GLSL → SPIR-V` is available; the `SPIR-V → MSL` hop is not.

**Resolution.** No design decision changes. The chain stays
`RSX → GLSL → glslang → SPIR-V → CompilerMSL → MSL` (HLD-05 §2.3) and the three MSL
post-passes (`:91-100`) stand. This is a prerequisite rather than a contradiction,
and it is recorded because it gates the whole shader layer and because the doc's
wording invites the reader to assume the tool is already present. Vendoring
SPIRV-Cross as plain files, on the model `06` uses for metal-cpp, is the open
action. **Closed:** SPIRV-Cross is vendored at
`300681bbcb8365a8d1eacdacdbdd0249bbf1d6b1` (Apache-2.0), as plain files under
`3rdparty/spirv-cross`, with its provenance in
`3rdparty/spirv-cross/RPCS3_VENDORING.md`.

## X4 — the shader object names in `HLD-05` §3.4-§3.6

**Design doc says.** `HLD-05-shader-binding-contract.md` §3.4 names
`MTL4::Library` and `MTL4::RenderPipelineState` as the objects the shader layer
produces; §3.5-§3.6 name a `build_program` entry point and an `ExtraData` block;
and `pipeline_binding_table.h` is named as the binding table the backend fills.

**The tree says.** `MTL4::Compiler::newLibrary` and
`MTL4::Compiler::newRenderPipelineState` return `MTL::Library*` and
`MTL::RenderPipelineState*` (metal-cpp `Metal/MTL4Compiler.hpp`), because Metal 4
compiles the Metal 3 objects and then consumes them. The pipeline entry point in
`MTLProgramPipeline` is `build_pipeline`, and there is no `ExtraData` block. The
pipeline key is the generic `program_state_cache` `pipeline_properties`
(`ProgramStateCache.h:165,174-179`). `pipeline_binding_table.h` is not used:
`VKPipelineCompiler` fills the table at run time (`prepareBindingTable()`), and
the Metal argument table is fixed and baked into the MSL at compile time.

**Resolution.** No design decision changes. The doc names the API by its intent
and the tree names it by what metal-cpp returns; the two are the same object. The
Metal path declares its argument indices at compile time, which is why the
binding table is inert for this backend and why the PSO key stays small.

<!-- doc-end -->
