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

<!-- doc-end -->
