# DEVIATIONS-TEXTURE — the Metal 4 texture cache, sampler, format and MSAA paths

Scope: the RSX Metal 4 backend (`rpcs3/Emu/RSX/MTL`). This file audits the texture cache,
the sampler state, the format conversion and the multisample resolve. It compares them with
the Vulkan backend (`rpcs3/Emu/RSX/VK`), the OpenGL backend (`rpcs3/Emu/RSX/GL`) and the
shared code (`rpcs3/Emu/RSX/Common`). It also mines the KosmicKrisp Metal driver for valid
Metal 4 techniques.

The file holds four parts: the method, the deviation table, the KosmicKrisp technique table,
and the prose commentary with a 15-line summary.

## Method

The RSX is the source of truth. Vulkan and OpenGL are the reference implementations. A row
enters the deviation table when the Metal path does not reproduce the behaviour the reference
shows. Each row names the exact `file:line` on both sides. When the reference has no
equivalent, the row says so.

Read-only. No build, no run, no commit. One file is created. The Mesa reference tree
(`.kk-mesa`) is read only.

The Metal scope files are `MTLTextureCache.{cpp,h}`, `MTLSamplerState.{cpp,h}`,
`MTLFormats.{cpp,h}`, `MTLHandles.{cpp,h}`, `MTLSurface.{cpp,h}` and the texture sections of
`MTLGSRender.cpp`. The reference files are `VKTextureCache.{cpp,h}`, `VKFormats.cpp`,
`VKDraw.cpp`, `GLTexture.cpp`, `GLTextureCache.h`, `GLDraw.cpp`, `glutils/sampler.cpp`,
`glutils/image.cpp`, `Common/texture_cache.h`, `Common/texture_cache_helpers.h`,
`Common/surface_utils.h` and `RSXThread.cpp`.

The class of a row is one of: `wrong result`, `missing feature`, `performance`, `diagnostic`
or `not a deviation`. The confidence is `yes (code)` for a static proof, `yes (TODO <id>)`
for a defect the live work list (`METAL4-TODO.md`) already records, and `no (needs run)` for
a claim that needs a render test to confirm.

The severity is `H`, `M` or `L`. Severity reflects the effect on the rendered image and the
cost to fix, not the size of the code change.

The KosmicKrisp source is a Vulkan driver on Metal. It is read-only reference material for
the Metal 4 API. A technique enters the second table only when it is valid for the RSX. A
technique that only satisfies Vulkan semantics is rejected, with the reason stated.

## Deviation table

| id | deviation | Metal file:line | VK/GL reference file:line | class | severity | confirmed? | proposed fix |
|---|---|---|---|---|---|---|---|
| TX1 | A vertex texture keeps the guest min, mag and mip filters. Vulkan and OpenGL force nearest and drop mipmaps for a vertex texture. | `MTLSamplerState.cpp:186-197` (capture), `:217-219` (apply) | `VKDraw.cpp:606-619`; `glutils/sampler.cpp:194-198` | wrong result | M | yes (code) | In `capture_vertex_sampler`, set nearest min and mag filters and `SamplerMipFilterNotMipmapped`. |
| TX2 | A vertex texture never sets `normalizedCoordinates = false`. An unnormalized vertex texture samples the wrong texels. | `MTLSamplerState.cpp:210-237` (no call), `MTLSamplerState.h:20-41` (no field) | `VKDraw.cpp:594,608,617`; KK `kk_sampler.c:96,108-110` | wrong result | M | yes (code) | Add a `normalized` field. Set it from `format() & CELL_GCM_TEXTURE_UN` for vertex units. Call `setNormalizedCoordinates`, force clamp-to-edge on R, and disable the mip filter, as Metal requires. |
| TX3 | The fragment sampler does not force nearest filtering when the format needs a shader-side SNORM, sRGB or redirected-depth decode. The filter runs on the encoded texels and the shader converts after the filter. Vulkan and OpenGL force nearest for the same cases and decode in hardware when the format allows it. | `MTLSamplerState.cpp:170-184` (no format test); `MTLDevice.cpp:89` (`disable_hardware_texel_remapping` ignored); `MTLDevice.cpp:285-315` (no `supports_hw_texel_remapping`) | `VKDraw.cpp:376-400,444-479`; `GLDraw.cpp:394-425,439-470`; `RSXThread.cpp:2662-2669` | wrong result | M | yes (code) | Pass `format_ex` into the sampler capture. Use nearest when a SEXT or GAMMA decode is active. Add the sRGB and SNORM view override when Metal accepts the view. |
| TX4 | `setMaxAnisotropy` receives the guest value. Metal requires a power of two, and the guest values include 6, 10 and 12. | `MTLSamplerState.cpp:91-106`, `:223` | `VKDraw.cpp:520-534` (float anisotropy); KK `kk_sampler.c:72-74,124-125` | wrong result | M | yes (code) | Round up to the next power of two and clamp to 16, as KosmicKrisp does. |
| TX5 | A custom border colour is reduced to the three Metal border colours. A guest value that is not black, white or transparent black is lost. | `MTLSamplerState.cpp:119-134`, `:224` | `VKDraw.cpp:404-441,602-604`; `VKFormats.cpp:64-85`; KK `kk_sampler.c:159-232` | missing feature | L | yes (code) | Adopt KK K3, or keep the nearest-value fallback and record the approximation in the user documentation. |
| TX6 | `convolution_min` becomes a mipmapped linear filter. Vulkan and OpenGL do not enable mipmapping for that filter. | `MTLSamplerState.cpp:81-85`, `:156-161` | `VKFormats.cpp:44`; `VKDraw.cpp:481-518` | wrong result | L | yes (code) | Return `SamplerMipFilterNotMipmapped` for `convolution_min`, or pass `sample_mipmaps = false`. |
| TX7 | A 3D texture returns the base image from `get_view`. The native channel layout and the unit remap are never applied. | `MTLHandles.cpp:19-22` | `VKTextureCache.cpp:843` (3D view); `glutils/image.cpp:450-479` | wrong result | M | yes (code) | Apply the map in the shader for 3D units, or reject a non-identity layout for a 3D texture and decode it in the shader. |
| TX8 | A cube texture that needs a view gets a 2D view with six slices. A 2D view accepts one slice, and a cube shader slot needs a cube resource. | `MTLHandles.cpp:43-54` (type at `:47`) | `VKTextureCache.cpp:728-738` (cube view and six layers) | wrong result | M | no (needs run) | Keep `TextureTypeCube` when the source is a cube. Use a 2D view only for a 2D source. |
| TX9 | A format-changing (typeless) texture copy is logged and skipped. The destination temporary keeps uninitialised contents. | `MTLTextureCache.cpp:352-356` | `VKTextureCache.cpp:467-532`; KK `kk_cmd_copy.c:263-293,327-405` | wrong result | H | yes (code) | Add a compute conversion pass, or route the copy through a buffer in the destination format (KK K7). |
| TX10 | A scaled texture copy is logged and skipped. | `MTLTextureCache.cpp:358-362` | `VKTextureCache.cpp:537-589`; `GLTextureCache.h:374-393` | missing feature | M | yes (code) | Add a scaled blit or a compute sampler pass (KK K7). |
| TX11 | The surface compatibility test requires the exact host pixel format. Vulkan and OpenGL accept a set of host formats for one guest format, including depth formats for `A8R8G8B8`. | `MTLTextureCache.h:517-520` | `VKTextureCache.cpp:1379-1417`; `GLTextureCache.h:880-925` | performance | M | yes (code) | Implement the per-format set. Include the depth-as-colour cases. |
| TX12 | The texture-cache read-back copies the host depth plane into the guest pack. `Depth32Float` writes four bytes per texel into a two-byte row, and a combined format loses the stencil. | `MTLTextureCache.cpp` | `MTLSurface.cpp` (the same defect, SF9); `VKTexture.cpp:118-229` | wrong result | M | fixed | Reuse `encode_depth_plane` and copy both planes. This is the texture-cache site of SF9. Both sites are fixed. |
| TX13 | An unsupported 2-sample request folds up to 4 samples. The guest lattice and `get_spp()` stay at 2. | `MTLResourceManager.cpp:285-295`; `MTLSurface.cpp:36-40` | `VKRenderTargets.h:241-249,290-292` | performance | L | yes (code) | Keep the guest lattice for the address paths and use the host count only for the Metal descriptor. Record the mismatch. |
| TX14 | An unknown guest texture format logs an error and becomes `BGRA8Unorm`. Vulkan and OpenGL fail the lookup. | `MTLFormats.cpp:308-311` | `VKFormats.cpp:237-240` | diagnostic | L | yes (code) | Fail the lookup and skip the draw with a clear message. Do not sample a wrong format. |
| TX15 | Not a deviation. The texture component layout matches the OpenGL map for every supported guest format. | `MTLFormats.cpp:314-364` | `GLTexture.cpp:214-269` | not a deviation | L | yes (code) | None. |
| TX16 | Not a deviation. The LOD range, the bias quantisation and the clamp match Vulkan. | `MTLSamplerState.cpp:108-117,155-161` | `VKDraw.cpp:499-517` | not a deviation | L | yes (code) | None. |
| TX17 | Not a deviation. The resolve uses `StoreAndMultisampleResolve` for colour, depth and stencil. The unresolve no-op follows the Metal rule that an image copy needs equal sample counts. | `MTLSurface.cpp:403-461`, `:463-469` | KK `kk_cmd_buffer.c:895-985`; `kk_cmd_copy.c:263-293` | not a deviation | L | yes (code) | None. Keep the depth-resolve sync check in the surface audit (SF K7). |
| TX18 | Not a deviation. The 2D engine does not sample a texture. It is a memory copy, and it falls back to the CPU path. | `MTLGSRender.cpp:3302-3330` | `NV47/HW/nv3089.cpp:592-598`; SF16 | not a deviation | L | yes (code) | None. SF8 tracks the missing GPU blit. |

## KosmicKrisp technique table

| id | KK file:line | what KK does | valid for RSX? | why | how we would apply it |
|---|---|---|---|---|---|
| K1 | `kk_sampler.c:72-74,124-125` | Rounds `maxAnisotropy` up to the next power of two and applies it to the Metal descriptor. | valid | Metal accepts only the powers of two 1, 2, 4, 8 and 16. The guest values 6, 10 and 12 are not powers of two. | Use it in `get_max_anisotropy` before `setMaxAnisotropy`. It closes TX4. |
| K2 | `kk_sampler.c:52-60,96,108-110` | Sets `normalizedCoordinates` from the Vulkan flag. Forces clamp-to-edge on W and disables the mip filter when the coordinates are unnormalized. | valid | It is the exact rule for the RSX `CELL_GCM_TEXTURE_UN` vertex path. Metal validates the W address mode and the mip filter for an unnormalized sampler. | Add the field and the call to `create_sampler_state`. It closes TX2. |
| K3 | `kk_sampler.c:24-42,67-70,201-232`; `kk_nir_lower_custom_border.c:24-90` | Emulates a custom border colour with two clamped samples and a lerp. The border value comes from a texture query. | valid with condition | The RSX border colour is a single ARGB value, so the technique applies. It costs a second sample on a border texel, and the MSL lowering of the Apple query op needs a test. | Use it only for a border-clamped unit with a non-standard colour. It closes TX5. |
| K4 | `kk_image_view.c:120-150` | Rewrites the view swizzle because a Metal depth sample returns `(d, d, d, 1)`. The G and B channels that read Y or Z become 0. | valid | The RSX depth path reads the depth in all four channels. The Metal map `{R,R,R,R}` already does this. | Informational. Keep the depth layout in `MTLFormats.cpp:338-343`. |
| K5 | `kk_image_view.c:122-127` | Composes the format `unswizzle` map with the view swizzle before it creates the view. | valid | The RSX has a host layout and a per-unit remap. The two maps must compose in that order. | Informational. `MTLHandles.cpp:7-10,27` already composes `m_native_layout` with the decoded remap. |
| K6 | `kk_image_view.c:195-232` | Records that a Metal render target cannot carry a swizzle. It creates a separate input view for sampling. | valid | It supports SF7. A surface that is also sampled needs a view, not a swizzled render target. | Keep the render target unswizzled. Create the sampled view on demand. No texture-cache change. |
| K7 | `kk_cmd_copy.c:263-293,327-405` | Copies image to image only when the format and the sample count match. Any other copy goes through a buffer. | valid | It states the Metal rule that the current texture cache ignores. `MTLTextureCache.cpp:352-362` skips the copy instead. | Add the buffer or compute path for a typeless and a scaled copy. It closes TX9 and TX10. |
| K8 | `kk_cmd_buffer.c:895-985` | Chooses the store action from the resolve mode. Metal forbids `MULTISAMPLE_RESOLVE` for one depth or stencil aspect while the other uses plain `STORE`. | valid | The RSX resolve uses `StoreAndMultisampleResolve` for both aspects. That form complies. | Informational. Keep both store actions equal. TX17 records the current state. |
| K9 | `kk_nir_lower_image_view_min_lod.c` | Lowers a per-image-view minimum LOD clamp for Vulkan. | rejected | The RSX has a per-unit `min_lod` and `max_lod`, not a per-view clamp. The sampler already carries them. | None. |
| K10 | `kk_cmd_copy.c:263-293` | Rejects a compressed 1D image-to-image copy and routes it through a buffer. | not applicable | The RSX 1D texture is a 2D texture with height 1. The compressed block rule still applies, but the 1D case does not occur. | None. Use the K7 buffer path for a compressed typeless copy. |

## Sampler state

The Metal sampler matches Vulkan on the address modes, the LOD quantisation and the compare
swap (`MTLSamplerState.cpp:14-36,108-117,226-231`). The gaps are in the cases the reference
treats specially.

The largest gap is TX3. Metal declares no hardware texel remap:
`supports_hw_texel_remapping` is never set in `MTLDevice.cpp:285-315`, and
`disable_hardware_texel_remapping` is listed as an ignored setting at `MTLDevice.cpp:89`.
The shader therefore always decodes SEXT, EXPAND and GAMMA
(`RSXThread.cpp:2662-2669`). Vulkan and OpenGL decode in hardware when the format allows it
(`VKDraw.cpp:376-400`; `GLDraw.cpp:406-424`), and they force nearest filtering when a decode
must stay in the shader (`VKDraw.cpp:444-479`; `GLDraw.cpp:439-470`). Metal filters first and
decodes second. A linear-filtered SNORM or redirected-depth texture therefore differs at
texel edges.

TX4 is a Metal API rule, not a preference. KosmicKrisp rounds the value up
(`kk_sampler.c:72-74`). Metal rejects 6, 10 and 12. The guest max-anisotropy register offers
those values, so the current code can fail validation.

TX1 and TX2 are the vertex texture path. Vulkan forces nearest and sets the unnormalized
flag (`VKDraw.cpp:594-619`). OpenGL forces nearest (`glutils/sampler.cpp:194-195`). Metal
keeps the guest filters (`MTLSamplerState.cpp:186-197`) and has no unnormalized field
(`MTLSamplerState.h:20-41`). A vertex texture that uses an unnormalized coordinate or a
linear filter reads different texels.

TX5 and TX6 are small. Metal has three border colours, so a custom border value cannot be
exact (`MTLSamplerState.cpp:119-134`). Vulkan and OpenGL pass the guest colour through
(`VKDraw.cpp:404-441`; `GLDraw.cpp:394-425`). KosmicKrisp shows the two-sample emulation
(K3). A `convolution_min` texture is mipmapped in Metal and not in Vulkan or OpenGL
(`VKFormats.cpp:44`).

## Formats and texture views

The texture format map is correct for the common path. The host component layout in
`MTLFormats.cpp:314-364` matches the OpenGL map in `GLTexture.cpp:214-269` for every
supported guest format: `B8`, `X16`, `G8B8`, `Y16_X16`, `HILO`, the packed colour formats,
the depth formats and the DXT formats. The packed formats stay packed on Apple silicon
(`MTLFormats.cpp:249-274`), which is valid and avoids the `BGRA8` expansion that Vulkan
uses on the same host (`VKFormats.cpp:200-215`).

The depth map is also correct. `z16_float` becomes `Depth32Float` and `z24s8` becomes
`Depth32Float_Stencil8` (`MTLFormats.cpp:73-90`). The depth upload decodes the guest pack
before the Metal copy (`MTLTextureCache.cpp:99-117`). The component layout `{R,R,R,R}`
(`MTLFormats.cpp:338-343`) supplies the depth in all four channels, as the reference does.

Two view cases fail. A 3D texture returns the base image, so the native layout and the unit
remap are lost (TX7). A cube texture that needs a view gets a 2D view with six slices (TX8).
The cube case is a type mismatch: the shader slot is a cube. The 3D case is a lost map:
this path creates no 3D view, so the only correct option is a shader-side decode.

An unknown format silently becomes `BGRA8Unorm` (TX14). That turns a data error into a
wrong image. The reference fails the lookup instead.

The surface format map is separate from the texture map. It is correct for the common
formats, but a surface never sets a native layout. That is SF7 and this file does not repeat
it. The same fix must cover the resolve target, because the resolve target is sampled.

The sRGB path is a performance gap, not a result gap. Metal has no sRGB surface view and the
ROP packs in the shader (`MTLFragmentProgram.cpp:291`). Vulkan and OpenGL can switch the view
to an sRGB format (`VKDraw.cpp:382-395`; `GLDraw.cpp:412-417`). The image is correct. The
shader work is extra.

## Copy and conversion

The texture cache can copy only when the source and the destination have the same pixel
format and the same extent (`MTLTextureCache.cpp:352-362`). Vulkan converts a typeless copy
with `vk::copy_image_typeless` and scales with `vk::copy_scaled_image`
(`VKTextureCache.cpp:467-589`). OpenGL scales through the hardware blitter
(`GLTextureCache.h:374-393`).

Metal logs an error and skips the copy. The temporary image then keeps uninitialised
contents. TX9 is therefore worse than a missing feature: the draw samples that temporary.
TX10 is the same defect on the scaled path. KosmicKrisp states the correct rule: copy image
to image only when the format and the sample count match, otherwise use a buffer
(`kk_cmd_copy.c:263-293,327-405`, K7).

TX11 makes the problem more likely. The cache asks whether a surface can serve a guest
format. Metal requires exact host equality (`MTLTextureCache.h:517-520`). Vulkan and OpenGL
accept several host formats for one guest format, including a depth surface for
`A8R8G8B8` (`VKTextureCache.cpp:1402-1415`; `GLTextureCache.h:897-901`). The strict test
forces a conversion that TX9 cannot perform.

## Read-back and flush

The flush path is correct in structure. `copy_texture` encodes the GPU copy into a shared
staging buffer and `map_synchronized` waits on `flush_for_readback`
(`MTLTextureCache.cpp:246-262`). The base `imp_flush` then copies the staging bytes to guest
memory. `finish_flush` is empty by design.

The depth case is not correct (TX12). The copy handles only `Depth32Float_Stencil8` and it
copies the depth plane with no pack (`MTLTextureCache.cpp:220-234`). A `Depth32Float` section
writes four bytes per texel into a row whose guest pitch is two bytes per texel. A
`z24s8` section loses the stencil byte. This is the same defect as SF9 at the texture-cache
site. SF9 covers `surface::write_to_memory`; this row covers the section flush. Both sites
need the `encode_depth_plane` fix.

The read-back also uses `rsx_pitch` as the Metal row stride. That is correct only when the
host extent equals the guest extent. SF10 records the scaled-surface case. This file does
not repeat it.

## Multisample resolve

The resolve structure follows Metal. The surface creates a private multisample image and a
private single-sample resolve target (`MTLSurface.cpp:51-70`). The resolve pass uses
`LoadActionLoad` and `StoreActionStoreAndMultisampleResolve` for colour, depth and stencil
(`MTLSurface.cpp:420-446`). The multisample image survives, so `unresolve` is a no-op
(`MTLSurface.cpp:463-469`). KosmicKrisp confirms that an image copy needs equal sample counts
(`kk_cmd_copy.c:263-293`) and that both depth aspects must use the same store action
(`kk_cmd_buffer.c:961-985`).

The sample-count fold is the one divergence (TX13). An unsupported 2-sample request becomes
4 (`MTLResourceManager.cpp:285-295`). The guest lattice still says 2 samples
(`MTLSurface.cpp:36-40`). The resolve target is pixel-sized, so the image is correct. The
host count is what `samples()` reports, and the shader path uses that count. Record the
mismatch so a later MSAA test does not read it as a bug.

The depth-resolve synchronisation risk is not repeated here. The surface audit records it as
SF K7, with the KosmicKrisp note that a depth render-pass resolve needs a producer barrier
(`MTLSurface.cpp:451-458`; `kk_cmd_buffer.c:1000-1005`).

## Missing features and settings

`disable_hardware_texel_remapping` has no effect (`MTLDevice.cpp:89`). The backend has no
hardware texel remap to disable. The shader path always runs. This is consistent, but the
setting cannot change the result.

`read_color_buffers` and `read_depth_buffer` are listed as ignored (`MTLDevice.cpp:93-94`).
The texture cache still reads back a section when the guest requests it. The user setting
cannot turn that off. The result is correct. The control is missing.

`strict_rendering_mode` is listed as ignored (`MTLDevice.cpp:97`). The RSX texture path has
no strict variant. The common code always uses the accurate path.

`force_hw_MSAA_resolve` is honoured through the shared code
(`Common/texture_cache_helpers.h:78-93`). A multisample surface is resolved before it is
sampled when the setting is on. The default path samples the multisample image.

The 2D engine has no sampler and no texture view. `scaled_image_from_memory` returns false
and the CPU fallback in `nv3089.cpp:592-598` runs (`MTLGSRender.cpp`). The source sections are
flushed and the destination sections are invalidated before the CPU copy, so the result is
correct. The GPU path is SF8.

## Highest-risk deviations

TX9 is the highest risk. A typeless copy leaves a temporary uninitialised, and the draw then
samples it. TX11 makes that path more likely because the compatibility test is stricter than
the reference. The fix is the KosmicKrisp buffer rule (K7), or a compute conversion pass.

TX3 is the next. A SNORM or redirected-depth texture with a linear filter is decoded after
the filter. The result changes at every texel edge. The reference avoids the case with a
nearest filter.

TX12 used to corrupt a depth read-back. A `z16_float` section overran its guest row. A
`z24s8` section lost the stencil. Both the surface site (SF9) and the texture-cache site
now use `encode_depth_plane` after a bounded read-back wait.

## Summary

- The Metal upload, deswizzle and mip path is correct for the common texture.
- The sampler loses the Vulkan and OpenGL nearest-filter guard for SNORM and redirected-depth textures.
- The sampler passes guest anisotropy values that Metal rejects.
- A vertex texture keeps the guest filters and drops the unnormalized-coordinate flag.
- A custom border colour is reduced to the three Metal values.
- A `convolution_min` texture is mipmapped, unlike the reference.
- A 3D texture gets no view, so the native layout and the remap are lost.
- A cube texture that needs a view gets a 2D view with six slices.
- A format-changing texture copy is skipped and leaves the temporary uninitialised.
- A scaled texture copy is skipped.
- The surface compatibility test is stricter than the reference and forces those copies.
- The texture-cache depth read-back is the second site of SF9.
- The MSAA sample-count fold keeps the image correct but changes the host count.
- KosmicKrisp confirms the anisotropy, normalized-coordinate and format-copy rules.
- No change is needed for the common component map, the LOD quantisation or the resolve store action.

---

## Resolution (2026-10-20)

| id | resolution |
|---|---|
| TX1 | Fixed. `capture_vertex_sampler` forces nearest min and mag filters and no mip chain, as `VKDraw.cpp:606-619` does. |
| TX2 | Fixed. `sampler_state` carries `unnormalized_coords`, filled from `CELL_GCM_TEXTURE_UN`. `create_sampler_state` calls `setNormalizedCoordinates`, and R is clamp-to-edge in that mode. The field is in the equality and the hash. |
| TX4 | Fixed. `get_max_anisotropy` rounds up to the Metal powers of two: 6 becomes 8, and 10 and 12 become 16. |
| TX6 | Fixed. `convolution_min` returns `SamplerMipFilterNotMipmapped`. |
| TX19 | Fixed earlier; see `REVIEW-RSX-NATIVE.md` R1. |
| TX3, TX5, TX7–TX14 | Open. |
| TX20 | Open (planned: the `R6G5B5` expansion). |

Build and tests: 143/143 MTL, 490/490 full.

---

Compliance note: this document quotes KosmicKrisp (Mesa) source for analysis only. It is not a
Mesa contribution, and it is not published to the Mesa project. The Mesa `AGENTS.md` in the
reference clone asks for a marker when prose is generated; biblioklept.

<!-- doc-end -->
