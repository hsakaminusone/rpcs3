# 4. Images, formats, samplers, render targets, MSAA/resolve & copy/blit

Owner/scope: Scope 4 | Status: draft | Date: 2026-10-15

Scope-4 mapping research for a hypothetical **native Apple Metal 4** RSX backend for RPCS3.
Method: read-only inventory of the in-scope RPCS3 Vulkan path (branch `kosmickrisp-integration`),
cross-checked against three pristine reference implementations (Mesa **KosmicKrisp** `kk_*`,
Godot's Metal `pixel_formats.cpp`, Apple **metal-cpp** `MTL4*` headers) and Apple's published
Metal 4 / MTLPixelFormat documentation. **No RPCS3 source, CMake, or tests were modified.**

Legend used throughout: **[V]** verified fact (read in source / doc), **[I]** inference,
**[?]** could-not-verify / open. Difficulty scale: **1** trivial rename · **2** mechanical ·
**3** real work but well-trodden · **4** hard / needs design · **5** unknown or impossible.

---

## 1. Scope & RPCS3 inventory

### 1.1 In-scope subsystem

Everything that turns RSX guest texture/surface state into GPU image resources and moves bytes
between them: pixel-format translation, image/texture creation & views, sampler objects, render
targets (color + depth/stencil), MSAA + resolve, and the copy/clear/blit family.

### 1.2 Files and entry points actually used by RPCS3 (exact symbols + `file:line`)

Format translation — `rpcs3/Emu/RSX/VK/VKFormats.cpp`:
- `get_compatible_depth_surface_format(const gpu_formats_support&, rsx::surface_depth_format2)` — `:9` **[V]**
- `get_compatible_sampler_format(const gpu_formats_support&, u32 format)` — `:195` **[V]**
- `get_compatible_srgb_format(VkFormat)` — `:243` **[V]**
- `get_compatible_snorm_format(VkFormat)` — `:268` **[V]**
- `get_format_texel_width(VkFormat)` / `get_format_element_size(VkFormat)` — `:291` / `:338` **[V]**
- `get_format_convert_flags(VkFormat)` / `formats_are_bitcast_compatible(...)` — `:399` / `:452,480` **[V]**
- `get_compatible_surface_format(rsx::surface_color_format)` → `{VkFormat, VkComponentMapping}` — `rpcs3/Emu/RSX/VK/VKGSRender.cpp:37` **[V]**
- `gpu_formats_support { d24_unorm_s8, d32_sfloat_s8, bgra8_linear, argb8_linear }` — `rpcs3/Emu/RSX/VK/vkutils/device.h:17` **[V]**

Images / views / layouts — `vkutils/image.h`, `vkutils/image_helpers.h`, `vkutils/image_impl.h`:
- `image` class (owns `VkImage`, `VkImageView` sets), `image_view` subresource ranges, `view_type` **[V]**
- `vk::image_view_create_info`, `vk::as_rtt`, `vk::as_input_attachment` helpers **[V]**

Samplers — `vkutils/sampler.h`, `vkutils/sampler.cpp`:
- `struct sampler` ctor/matches signature (clamp U/V/W, `unnormalized_coordinates`, `mipLodBias`,
  `max_anisotropy`, `min_lod`, `max_lod`, `min_filter`, `mag_filter`, `mipmap_mode`, `border_color`,
  `depth_compare`, `depth_compare_mode`) — `vkutils/sampler.h:42-53` **[V]**
- `sampler_pool_key_t` (packed key: clamp modes, unnormalized, filters, mipmap, border, compare,
  min/max LOD fx12, LOD bias fx12+sign, aniso) — `vkutils/sampler.h:64+` **[V]**

Render targets / resolve — `VKRenderTargets.{h,cpp}`, `VKResolveHelper.{h,cpp}`:
- `vk::resolve_image` / `vk::unresolve_image` — `VKRenderTargets.h:21-22` **[V]**
- MSAA ops `resolve` / `unresolve` / `spill` / `unspill` / `clear_memory` — `VKRenderTargets.h:82-124` **[V]**
- `surface_cache_traits::get_attachment_create_flags` — `VKRenderTargets.h:169` **[V]**
- `cs_resolve_base` / `depth_resolve_base` compute helpers — `VKResolveHelper.cpp` **[V]**

Texture cache / uploads — `VKTexture.cpp`, `VKTextureCache.cpp`:
- `copy_scaled_image(...)`, `copy_transfer_regions_impl(...)` — `VKTextureCache.cpp:414` **[V]**
- `blitter::scale_image(...)` — `VKTexture.cpp:1488`, `VKRenderTargets.cpp:1115` **[V]**

Allocation budget — `VKResourceManager.cpp:56-72` (`max_unique_samplers` / `maxSamplerAllocationCount`) **[V]**.

### 1.3 Complete inventory of copy / clear / blit / resolve / fill call sites (in scope)

**[V] grep-derived.** `vkCmd*` usage in the texture/RT path:

| Family | Symbol | Call sites |
|---|---|---|
| Image→buffer | `vkCmdCopyImageToBuffer` | `VKTexture.cpp:141,172,230,792,820`; `VKTextureCache.cpp:337`; `VKRenderTargets.cpp:644` |
| Buffer→image | `vkCmdCopyBufferToImage` | `VKTexture.cpp:306,347,411,798,823,1391,1398,1404`; `VKRenderTargets.cpp:681` |
| Image→image | `vkCmdCopyImage` | `VKTexture.cpp:625`; `VKTextureCache.cpp:540,587` |
| Blit | `vkCmdBlitImage` | `VKTexture.cpp:708` |
| Clear color | `vkCmdClearColorImage` | `VKTextureCache.cpp:816,865,915,965,1092`; `VKRenderTargets.cpp:510` |
| Clear depth/stencil | `vkCmdClearDepthStencilImage` | `VKTextureCache.cpp:821,870,920,970,1097`; `VKRenderTargets.cpp:515`; `VKResolveHelper.cpp:113,182` |
| Fill buffer | `vkCmdFillBuffer` | `VKTexture.cpp:367` |
| Scaled copy | `copy_scaled_image` | `VKTexture.cpp:631,722,756,810,1622`; `VKTextureCache.cpp:575`; `VKRenderTargets.cpp:789` |
| Scaled blit | `blitter::scale_image` | `VKTexture.cpp:1488`; `VKRenderTargets.cpp:1115` |

**Observation (inference):** the Blit family is used *only* for the guest's "scaled image" path
(RSX texture transfers that need filtering / format change); everything else is plain copy or clear.
This means a Metal backend can implement ~90% of scope-4 transfer traffic with same-format
copies and render-pass clears, and can relegate scaling to a compute pass. **[I]**

---

## 2. Vulkan/RSX → Metal 4 mapping table

### 2.1 Construct mapping

| # | Vulkan / RSX construct (exact API or symbol) | Metal 4 equivalent | Metal 4 objects/methods | Difficulty (1-5) | Notes |
|---|---|---|---|---|---|
| 1 | `VkImage` + `VkImageView` (`vkutils/image`) | `MTL4::Texture` (`MTL::Texture`) + view | `device.newTexture(descriptor)`; `texture.newTextureView(pixelFormat:)` | 2 | Views need `MTLTextureUsagePixelFormatView`; see §2.5. **[V]** |
| 2 | `VkImageView` subresource range (base mip/layer, count) | texture view subrange | `newTextureView(pixelFormat:textureType:levels:slices:swizzle:)` | 2 | Mip/layer subranges supported; format reinterpretation only within documented groups. **[V]** |
| 3 | `VK_IMAGE_TYPE_1D` | 2D texture (height=1) | `MTLTextureTypeType2D` | 1 | KK emulates 1D as 2D (`kk_image.c`). **[V]** |
| 4 | `VK_IMAGE_TYPE_3D` + `2D_ARRAY_COMPATIBLE` | 3D texture + auxiliary 2D-array view texture | `newTexture(type=3D)` + `mtl_handle_array` aux texture | 4 | KK allocates a **separate** array-compat texture. **[V]** |
| 5 | `VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT` | block-texel view | (no direct flag) — requires subresource aliasing / separate texture | 5 | KK: disables layout optimization when this bit is set (`kk_image_layout_can_optimize`) so views are legal; still needs care. **[V]** |
| 6 | `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` | pixel-format-view capable texture | `MTLTextureUsagePixelFormatView` | 2 | KK maps this bit straight to that usage. **[V]** |
| 7 | Linear / `VK_IMAGE_TILING_LINEAR` | `MTLTextureTypeTextureBuffer` / `buffer.makeTexture(descriptor:offset:bytesPerRow:)` | 3 | KK routes linear textures through buffers; rejects 3D/compressed/depth/attachments. **[V]** |
| 8 | `VkSampler` (`vkutils/sampler`) | `MTLSamplerState` + `MTL4ArgumentTable` slot | `device.newSamplerState(descriptor)`; `argumentTable.setSamplerState(id, index)` | 3 | Descriptor has separate min/mag/address modes. §2.4. **[V]** |
| 9 | `VkSamplerCreateInfo.addressModeU/V/W` | `sAddressMode`/`tAddressMode`/`rAddressMode` | 1:1 except custom border (§2.4) | 2 | **[V]** |
| 10 | `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER` custom color | **only** TransparentBlack/OpaqueBlack/OpaqueWhite | `descriptor.borderColor` | 4 | Custom color unsupported → RPCS3 fallback already exists (`get_closest_border_color_enum`). **[V]** |
| 11 | `VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE` | `MTLSamplerAddressModeMirrorClampToEdge` | enum exists | 1 | **[V]** |
| 12 | `unnormalizedCoordinates` | `MTLSamplerDescriptor.normalizedCoordinates` | 2 | Mirrors Vulkan semantically. **[V]** |
| 13 | `mipmapMode` (nearest/linear) | `mipFilter` (Nearest/Linear/NotMipmapped) | 1 | **[V]** |
| 14 | `minFilter`/`magFilter` | `minFilter`/`magFilter` | 1 | Metal **does** have separate min/mag (earlier assumption was wrong). **[V]** |
| 15 | `maxAnisotropy` | `maxAnisotropy` | 2 | Clamp to device max; KK rounds to power-of-two. **[V]** |
| 16 | `minLod`/`maxLod` | `lodMinClamp`/`lodMaxClamp` | 2 | **[V]** |
| 17 | `mipLodBias` | `lodBias` **or** shader-side uniform | 4 | Metal `lodBias` is S4.6 fixed-point (`[-16,15.999]`); KK passes it as descriptor-set half instead. **Disagreement — see §5/§7.** **[V]** |
| 18 | `compareEnable`/`compareOp` | `compareFunction` | 2 | **[V]** |
| 19 | `VkSamplerReductionMode` | `reductionMode` | 2 | Metal supports Min/Max/WeightedAverage. **[V]** |
| 20 | `VkRenderPass`/`VkFramebuffer` | `MTL4RenderPassDescriptor` | `MTL4::RenderPassDescriptor`, `cmdBuf.makeRenderCommandEncoder(desc, options)` | 3 | No framebuffer object; attachments carry resolve + load/store. **[V]** |
| 21 | `VkAttachmentDescription.loadOp=CLEAR` | `loadAction = Clear` | attachment desc | 1 | **[V]** |
| 22 | `VkAttachmentDescription.storeOp` | `storeAction = Store` / `DontCare` | attachment desc | 1 | **[V]** |
| 23 | `VkSubpassDescription.resolveAttachments` | `resolveTexture` + `storeAction=MultisampleResolve`/`StoreAndMultisampleResolve` | `RenderPassColorAttachmentDescriptor.resolveTexture` | 2 | **MSAA resolve is native via store action.** **[V]** |
| 24 | `VkAttachmentStoreOp` depth/stencil resolve | `depthResolveFilter` / `stencilResolveFilter` | attachment desc | 3 | KK forces depth/stencil resolve to meta passes due to a Metal sync bug. **[V]** |
| 25 | `vkCmdCopyImageToBuffer` | `copyFromTexture(...toBuffer...)` | `MTL4ComputeCommandEncoder` | 2 | **No blit encoder in M4** — copies live on the compute encoder. **[V]** |
| 26 | `vkCmdCopyBufferToImage` | `copyFromBuffer(...toTexture...)` (+`options:`) | compute encoder | 2 | **[V]** |
| 27 | `vkCmdCopyImage` | `copyFromTexture(...toTexture...)` | compute encoder | 2 | Same-format/same-sample only; else KK goes through a buffer. **[V]** |
| 28 | `vkCmdBlitImage` | **no direct equivalent** → compute/graphics pass | meta draw / compute shader | 4 | KK: `vk_meta_blit_image2` graphics pass. **[V]** |
| 29 | `vkCmdResolveImage` | (native store-action resolve) or compute pass | pass resolve / meta | 3 | **[V]** |
| 30 | `vkCmdClearColorImage` | render-pass `loadAction=Clear` (or compute fill) | render pass | 2 | KK uses load-op clear. **[V]** |
| 31 | `vkCmdClearDepthStencilImage` | pass `loadAction=Clear` (depth/stencil) | render pass | 2 | **[V]** |
| 32 | `vkCmdFillBuffer` | `fillBuffer(buffer, range, value)` | compute encoder | 1 | Byte-pattern fill only. **[V]** |
| 33 | `vkCmdUpdateBuffer` | `copyFromBuffer` device-side, or host write | compute encoder | 2 | Small host writes preferred. **[V]** |
| 34 | `vkCmdGenerateMipmaps` (blitter) | `generateMipmaps(texture)` | compute encoder | 2 | Requires mipmapped + appropriate usage. **[V]** |
| 35 | `VkImageMemoryBarrier` / layout transitions | **none** — Metal has no image layouts | — | 1 | Layout tracking becomes a no-op; synchronization via `MTL4CommandBuffer` ordering/fences. **[V]** |
| 36 | `VkBufferImageCopy` regions | `MTL::Origin`/`MTL::Size` + slice/level | compute encoder copies | 2 | **[V]** |
| 37 | `VkComponentMapping` (swizzle) | `MTLTextureSwizzleChannels` | `texture.swizzle` / view swizzle | 2 | Also used to fix depth→(d,d,d,1). **[V]** |
| 38 | `MTL_STORE_ACTION` for MSAA color | `MTLStoreActionMultisampleResolve` / `StoreAndMultisampleResolve` | attachment desc | 2 | **[V]** |
| 39 | Memoryless / tile-only surfaces | `MTLStorageModeMemoryless` | `resourceOptions` | 4 | Not a drop-in for PS3's "exploded" model — see §5. **[V]** |
| 40 | `VkFormat` (all) | `MTLPixelFormat` | `texture.pixelFormat` | 2 | Full table §2.3. **[V]** |
| 41 | `VkImage` usage flags | `MTLTextureUsage` (ShaderRead/Write/RenderTarget/PixelFormatView/ShaderAtomic) | desc `usage` | 2 | **[V]** |
| 42 | Sparse / `VK_IMAGE_CREATE_SPARSE_*` | `MTLHeapTypeSparse` | sparse heaps | 5 | **KK has no sparse** (`kk_image.c:288`); PS3 does not require it. **[V]** |
| 43 | `VK_EXT_custom_border_color` | unsupported (3 fixed colors) | borderColor | 4 | §2.4. **[V]** |
| 44 | `VK_EXT_image_2d_view_of_3d` | aux array texture | `mtl_handle_array` | 4 | **[V]** |
| 45 | `MTLLogicalToPhysicalColorAttachmentMap` (MRT remap) | `supportColorAttachmentMapping` on `MTL4RenderPassDescriptor` | `MTLLogicalToPhysicalColorAttachmentMap` | 3 | Useful for RSX MRT output remapping. **[V]** |
| 46 | Texture↔buffer texel layout | `MTLTextureTypeTextureBuffer` | `buffer.makeTexture(descriptor:offset:bytesPerRow:)` | 3 | Linear-texture constraints apply. **[V]** |

### 2.2 How RPCS3 currently maps PS3 sampler formats (`VKFormats.cpp:195-241`) — verified excerpt

| PS3 format | RPCS3 `get_compatible_sampler_format` | Apple-specific note |
|---|---|---|
| `B8` | `VK_FORMAT_R8_UNORM` | — |
| `A8R8G8B8`, `D8R8G8B8` | `VK_FORMAT_B8G8R8A8_UNORM` | not among Apple's linear drawable set, but valid sampled format |
| `R5G6B5` / `R6G5B5` | `VK_FORMAT_R5G6B5_UNORM_PACK16` | on `__APPLE__ && ARCH_X64`: falls back to `B8G8R8A8_UNORM` (`#if !defined(__APPLE__) || !defined(ARCH_X64)` block, `VKFormats.cpp:200-215`) **[V]** |
| `R5G5B5A1` | `VK_FORMAT_R5G5B5A1_UNORM_PACK16` | — |
| `D1R5G5B5`, `A1R5G5B5` | `VK_FORMAT_A1R5G5B5_UNORM_PACK16` | the **A1B5G5R5** MTL enum is disabled in KK (§5) |
| `A4R4G4B4` | `VK_FORMAT_R4G4B4A4_UNORM_PACK16` | emulated with swizzle (§2.3) |
| `DXT1`/`DXT23`/`DXT45` | `VK_FORMAT_BC1/2/3_*` (or BGRA8 fallback) | — |
| `G8B8` | `VK_FORMAT_R8G8_UNORM` | — |
| `W16_Z16_Y16_X16_FLOAT` | `VK_FORMAT_R16G16B16A16_SFLOAT` | — |
| `Y16_X16` | `VK_FORMAT_R16G16_UNORM` | — |
| `X16` | `VK_FORMAT_R16_UNORM` | — |
| `DEPTH16` | `VK_FORMAT_D16_UNORM` | Godot substitutes `Depth32Float` on iOS/tvOS |
| `DEPTH16_FLOAT` | `VK_FORMAT_D32_SFLOAT` | — |
| `DEPTH24_D8` | `D24_UNORM_S8_UINT` **or** `D32_SFLOAT_S8_UINT` | **no `Depth24Unorm_Stencil8` on Apple Silicon** → must pick D32 (§5) |
| `DEPTH24_D8_FLOAT` | `VK_FORMAT_D32_SFLOAT_S8_UINT` | — |
| `COMPRESSED_B8R8_G8R8` / `COMPRESSED_R8B8_R8G8` | `VK_FORMAT_B8G8R8A8_UNORM` | — |

### 2.3 PS3 / VkFormat → MTLPixelFormat mapping (ANSWER to deliverable (a))

Cross-checked `kk_format.c` (`kk_vf_formats[]`, lines ~160-340) vs Godot
`drivers/metal/pixel_formats.cpp` vs `MTLPixelFormat`. **Verdicts:** `direct` = native MTL enum;
`emul` = same enum + swizzle/fallback; `sub` = substitute a different enum; `reject` = impossible.

| PS3 format | VkFormat (RPCS3) | MTLPixelFormat | KK verdict | Godot verdict | Agree? | Diff |
|---|---|---|---|---|---|---|
| `B8` | `R8_UNORM` | `R8Unorm` | direct | direct | ✅ | — |
| `A8R8G8B8`/`D8R8G8B8` | `B8G8R8A8_UNORM` | `BGRA8Unorm` | direct | direct | ✅ | — |
| `R5G6B5`/`R6G5B5` | `R5G6B5_UNORM_PACK16` | `B5G6R5Unorm` | direct | direct (`B5G6R5Unorm`) | ✅ | RPCS3 X64-Apple path avoids it → BGRA8 |
| `R5G5B5A1` | `R5G5B5A1_UNORM_PACK16` | `B5G5R5A1Unorm` | direct | n/a (uses `BGR5A1Unorm` for A1R5G5B5) | ~ | component order differs per format |
| `A1R5G5B5`/`D1R5G5B5` | `A1R5G5B5_UNORM_PACK16` | `BGR5A1Unorm` | **KK enum `A1B5G5R5_UNORM` DISABLED** | `BGR5A1Unorm` (direct) | ⚠️ | **disagreement**: KK disables A1B5G5R5 (border-color HW bug); Godot maps RGB-swapped `BGR5A1Unorm` |
| `A4R4G4B4` | `R4G4B4A4_UNORM_PACK16` | `ABGR4Unorm` | `A4B4G4R4Unorm` + swizzle **ABGR** | n/a | ~ | KK `is_native=false` |
| `A8R8G8B8` (sRGB) | `B8G8R8A8_SRGB` | `BGRA8Unorm_sRGB` | direct | direct | ✅ | — |
| `R8G8B8` (22-bit) | (not natively; usually `B8G8R8A8_UNORM`) | — | — | `B8G8R8_*` = **Invalid** | ✅ | 24-bit packed RGB unsupported on Metal (Godot `pixel_formats.cpp:340-346`) |
| `G8B8` | `R8G8_UNORM` | `RG8Unorm` | direct | direct | ✅ | — |
| `X8R8G8B8` | `B8G8R8A8_UNORM` | `BGRA8Unorm` | direct | direct | ✅ | — |
| `R8G8B8A8` | `R8G8B8A8_UNORM` | `RGBA8Unorm` | direct | direct | ✅ | — |
| `W16Z16Y16X16` | `R16G16B16A16_SFLOAT` | `RGBA16Float` | direct | direct | ✅ | — |
| `Y16_X16` | `R16G16_UNORM` | `RG16Unorm` | direct | direct | ✅ | — |
| `X16` | `R16_UNORM` | `R16Unorm` | direct | direct | ✅ | — |
| `DXT1` | `BC1_RGBA_UNORM` (or RGB) | `BC1_RGBA` | `DXT1_RGB→DXT1_RGBA + swizzle RGB1` | direct (`BC1_RGBA`) | ~ | KK: RGB1 swizzle; `is_native` true |
| `DXT23` | `BC2_RGBA_UNORM` | `BC2_RGBA` | direct | direct | ✅ | — |
| `DXT45` | `BC3_RGBA_UNORM` | `BC3_RGBA` | direct | direct | ✅ | — |
| `DXT1` (sRGB) | `BC1_RGBA_SRGB` | `BC1_RGBA_sRGB` | `DXT1_SRGB→DXT1_SRGBA +RGB1` | direct | ~ | — |
| `DEPTH16` | `D16_UNORM` | `Depth16Unorm` | direct (`Z16_UNORM`) | `Depth16Unorm`; **`Depth32Float` on iOS/tvOS** | ~ | Godot substitutes on mobile |
| `DEPTH16_FLOAT` | `D32_SFLOAT` | `Depth32Float` | direct (`Z32_FLOAT`) | direct | ✅ | — |
| `DEPTH24_D8` | `D24_UNORM_S8_UINT` | **`Depth24Unorm_Stencil8`** | **KK has NO z24s8 enum** (Mesa routes to Z32/rejects) | `Depth24Unorm_Stencil8` primary, `Depth32Float_Stencil8` sub | ⚠️ | **disagreement**; Apple Silicon: `Depth24Unorm_Stencil8` unsupported → sub D32 |
| `DEPTH24_D8_FLOAT` | `D32_SFLOAT_S8_UINT` | `Depth32Float_Stencil8` | direct (`Z32_FLOAT_S8X24_UINT`) | direct | ✅ | needs `PixelFormatView` for depth views |
| — (internal) | `X8_D24_UNORM_PACK32` | — | none | primary Invalid, sub `Depth24Unorm_Stencil8` | — | depth-only 24-bit unsupported |
| — | `S8_UINT` | `Stencil8` | direct | direct | ✅ | — |
| `A4B4G4R4` internal | `A4B4G4R4_UNORM` | `ABGR4Unorm` | direct | n/a | ~ | — |
| others (internal) | `R10G10B10A2_UNORM` | `RGB10A2Unorm` | direct | direct | ✅ | — |
| others | `B10G10R10A2_UNORM` | `BGR10A2Unorm` | direct | direct | ✅ | — |
| others | `R11G11B10_FLOAT` | `RG11B10Float` | direct | direct | ✅ | — |
| others | `E5B9G9R9_UFLOAT_PACK32` | `RGB9E5Float` | `R9G9B9E5_FLOAT` direct | direct (**blend disabled on macOS**, `modifyMTLFormatCapabilities`) | ~ | — |
| others | `R32G32B32_*` | **`Invalid`** (Godot) | not in KK table | Invalid | ✅ | 96-bit packed unsupported |
| others | `R8G8B8A8_SNORM` | `RGBA8Snorm` | direct | `B8G8R8A8_SNORM→RGBA8Snorm + swizzle B,G,R,A` | ~ | Godot swaps |
| others | `R32_SFLOAT`, `R32_UINT`, `R32G32_SFLOAT`, `R32G32B32A32_*`, `RGBA16*`, `RG16*`, all ETC2/ASTC/BC/RGTC/BPTC | matching enums | direct | direct | ✅ | KK tag: Apple7 caps (`MTL_FMT_*`) |

**Key disagreement to flag: `D24_UNORM_S8`.** Godot maps it to `Depth24Unorm_Stencil8` with a
`Depth32Float_Stencil8` substitute, whereas KK simply has no 24-bit-depth enum and Mesa routes it
elsewhere. Apple's `isDepth24Stencil8PixelFormatSupported` is **"Never supported on Apple Silicon"**
— so on any M-series Mac the *only* correct behaviour is `D24_UNORM_S8_UINT → Depth32Float_Stencil8`.
RPCS3 already has this fallback path via `get_compatible_depth_surface_format`
(`gpu_formats_support.d24_unorm_s8=false`). **[V]/[I]**

**Disagreement to flag: `A1B5G5R5_UNORM`.** KK disables the native enum because of a border-color
HW bug and expects the driver to emulate it; Godot provides a native `BGR5A1Unorm`. For a native
backend we would need to decide whether the PS3 `A1R5G5B5` texture path uses `BGR5A1Unorm`
(RGB-swapped) or an emulated path. **[V]**

### 2.4 Sampler feature coverage (ANSWER to deliverable (d))

| Vulkan sampler feature RPCS3 uses | Metal 4 support | Verdict | Difficulty |
|---|---|---|---|
| separate min/mag filter | `MTLSamplerDescriptor.minFilter`/`magFilter` | ✅ | 1 |
| mip filter (nearest/linear/none) | `mipFilter` | ✅ | 1 |
| address U/V/W | `sAddressMode`/`tAddressMode`/`rAddressMode` | ✅ | 1 |
| ClampToEdge / Repeat / MirroredRepeat / MirrorClampToEdge | all present | ✅ | 1 |
| ClampToBorder (fixed colors) | `borderColor` = TransparentBlack/OpaqueBlack/OpaqueWhite | ⚠️ partial | 2 |
| custom border color (`VK_EXT_custom_border_color`) | **unsupported** | ❌ → RPCS3 fallback `get_closest_border_color_enum` | 4 |
| unnormalized coordinates | `normalizedCoordinates` | ✅ | 2 |
| anisotropy | `maxAnisotropy` | ✅ (clamp to device max) | 2 |
| LOD bias (float) | `lodBias` **S4.6 fixed-point** | ⚠️ precision loss | 4 |
| LOD min/max clamp | `lodMinClamp`/`lodMaxClamp` | ✅ | 2 |
| compare sampler (shadow) | `compareFunction` | ✅ | 2 |
| reduction mode (min/max/avg) | `reductionMode` | ✅ | 2 |
| **1024 unique-sampler budget** | `maxArgumentBufferSamplerCount` | ⚠️ hard cap | 4 |

### 2.5 Format reinterpretation / `BLOCK_TEXEL_VIEW_COMPATIBLE` (ANSWER to deliverable (e))

- Metal texture **pixel-format views** are legal only within documented groups: all color formats
  of the same bit length; sRGB↔non-sRGB of the same compressed format; combined depth-stencil ↔
  `x24_stencil8`/`x32_stencil8`. `pixelFormatView` usage is required. **[V]**
- `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` → set `MTLTextureUsagePixelFormatView` (KK does exactly this). **[V]**
- `VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT`: KK handles it by *disabling* layout
  optimization (`kk_image_layout_can_optimize()` returns false; `kk_image_layout_level_offsets_defined()`
  returns false) so that a block-texel view is representable. A native backend can either mirror
  this (large memory cost) or create a **separate** texture and re-upload. **[V]/[I]**
- `Z32_FLOAT_S8X24_UINT` requires `PixelFormatView` for the depth-only view — KK sets it explicitly. **[V]**
- Metal→Vulkan depth sampling returns `(d,d,d,1)`; KK fixes it with a swizzle in
  `kk_image_view.c:129-145`. RPCS3's `VkComponentMapping` path (`get_compatible_surface_format`)
  must do the equivalent. **[V]**

### 2.6 Copy/blit/clear mapping incl. "does a blit encoder still exist?" (ANSWER to deliverable (b))

**Verdict: Metal 4 has NO blit encoder.** **[V]**
- `MTL4::ComputeCommandEncoder` exposes all copy/fill/mipmap entry points:
  `copyFromBuffer(_:sourceOffset:toBuffer:destinationOffset:size:)`,
  `copyFromBuffer(...sourceBytesPerRow:...toTexture:...)` (+`options:`),
  `copyFromTexture(_:toTexture:)`,
  `copyFromTexture(...sourceSlice:...toTexture:...)`,
  `copyFromTexture(...toBuffer:...)` (+`options:`),
  `fillBuffer(_:range:value:)`, `generateMipmaps(texture:)`
  (`metal-cpp/Metal/MTL4ComputeCommandEncoder.hpp:67-93`). **[V]**
- The KosmicKrisp bridge's `mtl_blit_encoder` is a **dead `typedef void`**
  (`bridge/mtl_types.h:24`), confirming the Mesa port already dropped the blit encoder concept. **[V]**
- WWDC25 confirmed blit+compute+ML consolidated into one unified compute encoder. **[V]**

Mapping strategy (KK-derived, recommended for RSX):
- Same-format + same-sample-count image→image → `copyFromTexture(toTexture:)`.
- Otherwise → copy **through a buffer** (`copy_through_buffer`), including depth↔stencil aspect
  (via `MTLBlitOptionDepthFromDepthStencil` / `StencilFromDepthStencil`). **[V]**
- 1D compressed and linear-tiling images → route through a buffer. **[V]**
- Scaled blit (`vkCmdBlitImage`) → graphics meta pass (`vk_meta_blit_image2`). **[V]**
- `vkCmdClear*Image` → render-pass `loadAction=Clear` (or a compute fill for buffer views). **[V]**
- `vkCmdFillBuffer` → `fillBuffer`. **[V]**

### 2.7 MSAA + resolve + tile memory (ANSWER to deliverable (c))

- **Native MSAA resolve via store actions.** `MTL4RenderPassDescriptor` uses the MTL attachment
  descriptor types (`MTL::RenderPassColorAttachmentDescriptorArray` / Depth / Stencil) that expose
  `resolveTexture`, `resolveLevel/Slice/DepthPlane`, `storeAction` (`MultisampleResolve`,
  `StoreAndMultisampleResolve`), `depthResolveFilter`, `stencilResolveFilter`, `clearColor`. **[V]**
- KK uses `MTLStoreActionStoreAndMultisampleResolve` / `MultisampleResolve`
  (`kk_cmd_buffer.c:896-922`, `kk_get_attachment_store_op`) and drops the separate resolve API. **[V]**
- KK **skips / meta-routes** resolve unless: rendering to the whole framebuffer, formats match the
  resolve target, and `kk_va_format->resolve` is set. Depth/stencil resolve is forced to meta passes
  because of a Metal synchronization bug combining `MultisampleResolve` with a depth/stencil pass. **[V]**
- **Memoryless/tile** (`MTLStorageModeMemoryless`, value 3): Apple-family only, macOS 11+; contents
  valid only within a pass; **cannot** use `loadAction=load` nor `storeAction=store`/
  `storeAndMultisampleResolve`. KK **never uses memoryless** (only the enum exists in
  `mtl_types.h:61-64`). **[V]**
- **PS3 "exploded" surface model vs memoryless:** RPCS3 keeps the MSAA surface and a *separate*
  resolve target in memory and performs explicit resolves/unresolves/spills (`VKRenderTargets.h:82-124`),
  because RSX can sample the multisample surface directly and swap surfaces mid-frame. Memoryless
  storage cannot be a drop-in for that model — the resolve target must persist and be samplable. **[I]**
- **Sample counts:** 1 & 4 always; 2 on all iOS/tvOS + some macOS; 8 only some macOS. Max sample
  count in an MSAA render pass = 4 for GPU families Apple2..Apple8, **8 for Apple9+**. Must query
  `device.supportsTextureSampleCount(_:)`. **[V]**
- **Sparse:** not supported by KK (`kk_image.c:288`); requires `MTLHeapTypeSparse` + private storage;
  `supportsPlacementSparse` is iOS 26.4+. PS3 does not require sparse residency. **[V]**

### 2.8 1024 unique-sampler limit — root cause & implications (ANSWER to deliverable (f))

**Root cause (verified):** macOS caps an app at ~1024 *unique* `MTLSamplerState` objects
(iOS/tvOS: 96). The cap applies only to samplers created with `supportArgumentBuffers = true`,
i.e. samplers usable inside argument buffers, and is reported by
`MTLDevice.maxArgumentBufferSamplerCount` (`bridge/mtl_sampler.m:18` sets the flag). **[V]**

KosmicKrisp: `pdev->info.max_sampler_count = mtl_device_max_argument_buffer_sampler_count(...)`
(`kk_physical_device.c:1054-1055`); `maxSamplerAllocationCount = MSL_MAX_SAMPLERS` if
`max_sampler_count >= MSL_MAX_SAMPLERS`, else 4000 (`kk_physical_device.c:541-566`). The sampler
heap returns OOM past the count (`kk_device.c:174`). **[V]**

RPCS3 budget: `max_allowed_samplers = min(limits.maxSamplerAllocationCount * 3/4, 2048,
driver_selection.max_unique_samplers)` (`VKResourceManager.cpp:56-59`); the KosmicKrisp workaround
profile sets `max_unique_samplers = 1024` (`vkutils/vk_driver_selection.h:230`). When the pool
exceeds the budget, RPCS3 evicts unreferenced samplers (`collect(filter_expr)`). **[V]**

**Implications for a native Metal 4 backend:** the limit is intrinsic to argument-buffer samplers
and cannot be raised. A native backend must (1) keep the same bounded-arena +
eviction strategy, (2) deduplicate aggressively via the packed `sampler_pool_key_t`, and (3)
prefer **direct `setSamplerState` on the argument table** (Metal 4 `MTL4ArgumentTable.setSamplerState`)
over stuffing samplers into argument buffers, which sidesteps the argument-buffer accounting
entirely. **[I]** The 1024 comment in RPCS3 is a *driver-side* cap passed through, not an RSX limit. **[V]**

---

## 3. Pristine reference code

All references are read-only clones under `/tmp/metal4-refs/`. "Pristine" = cannot be tainted by
our RPCS3 edits.

### 3.1 Mesa KosmicKrisp (`mesa/src/kosmickrisp/`)

- **`vulkan/kk_format.c`** — canonical `pipe_format → MTLPixelFormat` table `kk_vf_formats[]`
  (lines ~160-340) plus per-format caps. Native enums listed; `A8_UNORM` and `A1B5G5R5_UNORM`
  **commented out** (border-color HW bug); `R4G4B4A4/A4R4G4B4/B4G4R4A4` emulated via
  `A4B4G4R4_UNORM` + `MTL_SWIZZLE_ABGR/BGRA/GBAR`; `DXT1_RGB/SRGB` via `DXT1_RGBA/SRGBA + RGB1`.
  No `Z24_UNORM_S8` enum. Depth/stencil: `Z16_UNORM`, `Z32_FLOAT`, `S8_UINT`,
  `Z32_FLOAT_S8X24_UINT`, `X32_S8X24_UINT`.
- **`vulkan/kk_sampler.c`** — sampler packing; **sets `lodMinClamp`/`lodMaxClamp` but NOT `lodBias`**;
  instead `sampler->lod_bias_fp16 = _mesa_float_to_half(pCreateInfo->mipLodBias)` (`:236`), passed
  via descriptor set (avoids Metal's S4.6 precision loss). Aniso rounded to power-of-two (`:74`).
- **`bridge/mtl_sampler.m`** — `descriptor.supportArgumentBuffers = true` (`:18`); `borderColor`
  set (`:51`); `lodMinClamp`/`lodMaxClamp` set (`:76-77`); **`lodBias` never set**.
- **`vulkan/kk_cmd_buffer.c:896-922`** — `kk_get_attachment_store_op` choosing
  `MTLStoreActionStoreAndMultisampleResolve` / `MultisampleResolve`; resolve forced to meta passes
  when depth/stencil (Metal sync bug), `:962-966`.
- **`vulkan/kk_cmd_copy.c`** — image→image same-format fast path; otherwise `copy_through_buffer`;
  D↔S aspect via `MTLBlitOptionDepthFromDepthStencil`/`StencilFromDepthStencil`.
- **`vulkan/kk_cmd_clear.c` / `kk_cmd_meta.c`** — clear via load-op; `vkCmdClearAttachments`/fill/update
  via meta passes; `vkCmdBlitImage2`→graphics meta, `vkCmdResolveImage2`→`vk_meta_resolve_image2`.
- **`vulkan/kk_image.c`** — 1D→2D emulation; 3D + `2D_ARRAY_COMPATIBLE`/`VIEW_COMPATIBLE` →
  aux 2D-array texture (`mtl_handle_array`); linear tiling via buffer; **no sparse** (`:288`);
  host copies.
- **`vulkan/kk_image_view.c:129-145`** — depth→`(d,d,d,1)` swizzle fix.
- **`vulkan/kk_image_layout.{c,h}`** — block-texel offset rules; `kk_image_layout_can_optimize`
  false when `BLOCK_TEXEL_VIEW_COMPATIBLE_BIT`; `kk_image_layout_level_offsets_defined` false.
- **`vulkan/kk_physical_device.c:541-566, 1054-1055`** — sampler-count derivation.
- **`bridge/mtl_render_state.m`** — `mtl_new_render_pass_descriptor()` returns
  `MTL4RenderPassDescriptor` (confirms M4 pass object).
- **`bridge/mtl_types.h:24`** — `typedef void mtl_blit_encoder;` (dead blit encoder).
- **`bridge/mtl_types.h:61-64`** — memoryless enum definition only (never used).

### 3.2 Godot Metal (`godot/drivers/metal/pixel_formats.cpp`)

- Authoritative `VkFormat → MTLPixelFormat` table + `modifyMTLFormatCapabilities` (`:899`).
- `Depth16Unorm_Stencil8 := Depth32Float_Stencil8` (`:73`); `Depth24Unorm_Stencil8 := Invalid`
  on the platform macro path (`:74`); real mapping `D24_UNORM_S8_UINT → Depth24Unorm_Stencil8`
  primary, `Depth32Float_Stencil8` substitute (`:460`); caps disabled when
  `!p_feat.supportsDepth24Stencil8` (`:1012`) and `Depth32Float_Stencil8` restricted on
  iOS/tvOS (`:1013`).
- `R32G32B32_*` = Invalid (`:426-428`); `B8G8R8_*` = Invalid (`:340-346`); `E5B9G9R9_UFLOAT_PACK32`
  → `RGB9E5Float` (`:451`, blend disabled on macOS); `D16_UNORM` → `Depth16Unorm` with `Depth32Float`
  substitute (`:458`); `X8_D24_UNORM_PACK32` primary Invalid, sub `Depth24Unorm_Stencil8` (`:462`).

### 3.3 Apple metal-cpp (`metal-cpp/Metal/`)

- `MTL4ComputeCommandEncoder.hpp:67-93` — the **entire** copy/fill/mipmap surface (no blit encoder).
- `MTL4RenderPass.hpp` — `MTL4::RenderPassDescriptor`, `setSupportColorAttachmentMapping`,
  `makeRenderPassDescriptor`.
- `MTLRenderPass.hpp` — attachment types with `resolveTexture`, store actions, resolve filters.
- `MTLTexture.hpp` / `MTLResource.hpp` — storage modes (Memoryless=3), usage flags, swizzle.
- `MTLSampler.hpp` — `minFilter`/`magFilter`/`mipFilter`, address modes, `borderColor`,
  `normalizedCoordinates`, `maxAnisotropy`, `lodMinClamp`/`lodMaxClamp`, `lodBias`, `compareFunction`,
  `reductionMode`.
- `MTL4ArgumentTable.hpp` — `setTexture`/`setSamplerState`/`setResource`/`setAddress` by `ResourceID`.

---

## 4. Impossible / not-recommended

| Item | Verdict | Why |
|---|---|---|
| Metal 4 **blit encoder** | ❌ does not exist | All copies/fills/mipmaps are on `MTL4ComputeCommandEncoder`. **[V]** |
| Custom-border sampler color | ❌ unsupported | Only TransparentBlack/OpaqueBlack/OpaqueWhite. RPCS3 has fallback. **[V]** |
| `Depth24Unorm_Stencil8` on Apple Silicon | ❌ never supported | Must substitute `Depth32Float_Stencil8`. **[V]** |
| Native `A8Unorm` / `A1B5G5R5Unorm` | ❌ disabled in KK (HW border-color bug) | Emulate or substitute. **[V]** |
| `R32G32B32_*`, `B8G8R8_*` (96/24-bit packed) | ❌ Invalid | Godot marks Invalid. **[V]** |
| Sparse residency | ❌ not supported (KK) / not needed | `kk_image.c:288`. **[V]** |
| Memoryless for the PS3 exploded MSAA model | ⚠️ not a drop-in | Resolve target must persist & be samplable. **[I]** |
| 1:1 `VkImageMemoryBarrier`/layouts | ❌ n/a | Metal has no image layouts. **[V]** |
| `VK_IMAGE_TILING_LINEAR` for 3D/compressed/depth/attachments | ❌ rejected | KK rejects linear tiling for these. **[V]** |
| Raising the 1024 unique-sampler cap | ❌ hard macOS limit | `maxArgumentBufferSamplerCount`. **[V]** |
| Exact float LOD bias via `lodBias` | ⚠️ precision loss (S4.6) | KK passes it shader-side instead. **[V]** |

---

## 5. Tricky (ranked)

1. **PS3 "exploded" MSAA surfaces vs Metal pass-scoped resolve (5).** RPCS3 models MSAA + a
   separate resolve texture with explicit resolve/unresolve/spill/unspill and mid-frame surface
   swaps. Metal 4 resolve is a *store action at pass end*. Reconciling sampling-a-multisample-surface
   with Metal's resolve model requires a concrete design (likely: always resolve on store, keep an
   explicit persistent resolve target, emulate `unresolve` by re-rendering). **[I]**
2. **LOD bias fidelity (4).** Metal `lodBias` is S4.6 fixed-point; KK deliberately avoids it and
   passes half-float bias through the descriptor set. A native backend must decide between
   per-sampler shader-injected bias (needs shader-side plumbing) and accepting Metal's quantization. **[V]**
3. **Format views / `BLOCK_TEXEL_VIEW_COMPATIBLE` (4).** Views only within same-bit-length / sRGB /
   combined-depth groups. Block-texel views force KK to disable layout optimization (memory blow-up);
   a native backend needs either the same or separate-texture re-upload. **[V]**
4. **1024 unique-sampler budget (4).** Hard cap; requires bounded arena + eviction or argument-table
   `setSamplerState` to bypass argument-buffer accounting. **[V]**
5. **1D / 3D-array-compatible / linear textures (4).** Each needs an emulation strategy
   (1D→2D, 3D→aux array texture, linear→buffer). KK provides the blueprint but it is non-trivial. **[V]**
6. **Scaled blit / resolve helpers (4).** `vkCmdBlitImage`, `vkCmdResolveImage`, and depth-resolve must
   become graphics/compute meta passes; depth/stencil resolve specifically forced to meta by a Metal
   sync bug. **[V]**
7. **sRGB + drawable colorspace mismatch (3).** `_sRGB` formats auto-convert; `CAMetalLayer.colorspace`
   defaults to no colour matching. RSX gamma paths need care. **[V]**
8. **Sampler dedup key parity (3).** RPCS3's packed key must be reproduced exactly to preserve
   hit-rate under the 1024 cap. **[V]** 

---

## 6. Minimal Metal 4 code sketch (metal-cpp)

Research sketch only — illustrative, not compiled. Shows image/view creation, sampler binding via
`MTL4ArgumentTable`, MSAA resolve via store action, and a buffer→texture copy on the compute encoder.

```cpp
// --- Texture + format-view-capable image -----------------------------------
MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
    MTL::PixelFormatBGRA8Unorm, w, h, /*mipmapped=*/true);
td->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget
           | MTL::TextureUsagePixelFormatView);      // mutable-format / views
td->setStorageMode(MTL::StorageModePrivate);
MTL::Texture* tex = device->newTexture(td);

// Reinterpret within a legal group (same bit length) — e.g. depth-only view of Z32F_S8:
MTL::Texture* depthView = tex->newTextureView(MTL::PixelFormatDepth32Float);

// 1D emulated as 2D (height = 1); 3D 2D-array-compat needs a separate aux array texture.

// --- Sampler ---------------------------------------------------------------
MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
sd->setMinFilter(MTL::SamplerMinMagFilterLinear);
sd->setMagFilter(MTL::SamplerMinMagFilterNearest);   // separate min/mag: supported
sd->setMipFilter(MTL::SamplerMipFilterLinear);
sd->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
sd->setTAddressMode(MTL::SamplerAddressModeRepeat);
sd->setRAddressMode(MTL::SamplerAddressModeRepeat);
sd->setBorderColor(MTL::SamplerBorderColorTransparentBlack); // only 3 fixed colors
sd->setNormalizedCoordinates(true);
sd->setMaxAnisotropy(8);
sd->setLodMinClamp(0.0f);
sd->setLodMaxClamp(15.0f);
// NOTE: prefer shader-side LOD bias over sd->setLodBias() (S4.6 fixed-point loses precision).
MTL::SamplerState* samp = device->newSamplerState(sd);

// --- Bind via MTL4ArgumentTable (bypasses argument-buffer sampler accounting)
MTL4::ArgumentTableDescriptor* atd = MTL4::ArgumentTableDescriptor::alloc()->init();
atd->setMaxTextureCount(64); atd->setMaxSamplerStateCount(16);
MTL4::ArgumentTable* table = device->newArgumentTable(atd);
table->setTexture(tex->gpuResourceID(), /*index=*/0);
table->setSamplerState(samp->gpuResourceID(), /*index=*/0);
encoder->setArgumentTable(table, MTL::RenderStageFragment);

// --- MSAA render pass with native resolve ----------------------------------
MTL4::RenderPassDescriptor* rp = MTL4::RenderPassDescriptor::alloc()->init();
auto* color = rp->colorAttachments()->object(0);
color->setTexture(msaaTex);                 // sampleCount = 4
color->setResolveTexture(resolveTex);       // persistent resolve target (PS3 model)
color->setLoadAction(MTL::LoadActionClear);
color->setStoreAction(MTL::StoreActionMultisampleResolve); // native resolve on store
color->setClearColor(MTL::ClearColor::Make(0,0,0,1));
auto* depth = rp->depthAttachment();
depth->setTexture(depthView);
depth->setLoadAction(MTL::LoadActionClear);
depth->setStoreAction(MTL::StoreActionStore);
depth->setClearDepth(1.0);

// --- Copy: no blit encoder — use the compute encoder -----------------------
MTL4::ComputeCommandEncoder* ce = cmd->computeCommandEncoder();
ce->copyFromBuffer(srcBuffer, 0, bytesPerRow, bytesPerImage, size,
                   dstTexture, /*slice=*/0, /*level=*/0, {0,0,0});
ce->fillBuffer(dstBuffer, NS::Range::Make(0, size), 0);
ce->generateMipmaps(dstTexture);
ce->endEncoding();
```

---

## 7. Open questions for the human

1. **LOD bias strategy.** Adopt KK's shader-injected `lod_bias_fp16` (needs NIR/shader plumbing in
   the native backend) or accept Metal's S4.6 `lodBias` quantization? This is a correctness/perf
   tradeoff and the two pristine refs disagree. **[V]**
2. **Exploded MSAA model.** Do we commit to "always resolve on store + persistent resolve texture",
   or invest in a tile-memory/memoryless path for surfaces RSX never samples? Requires an RSX-level
   audit of which surfaces are actually sampled. **[I]**
3. **`A1B5G5R5` / `A1R5G5B5`.** Follow KK (emulate, because native is buggy) or Godot (native
   `BGR5A1Unorm`)? Does RSX even exercise this path? **[V]**
4. **`D24_UNORM_S8`.** Confirm we always force `D32_SFLOAT_S8_UINT` on Apple Silicon (RPCS3 already
   does via `gpu_formats_support`); verify no guest title depends on 24-bit depth precision. **[I]**
5. **Block-texel views.** Mirror KK's disable-layout-optimization (memory cost) or use separate
   textures + re-upload? **[V]**
6. **Sampler count.** Confirm `setSamplerState`-on-argument-table bypasses the 1024 argument-buffer
   cap in Metal 4; if so we may drop the aggressive eviction. **[?]**
7. **1D / 3D-array-compat / linear tiling.** How many real titles hit each? Determines whether these
   need first-class support or can be best-effort. **[?]**
8. **Depth/stencil resolve.** KK forces it to meta because of a Metal sync bug — is that bug still
   present in the Metal 4 runtime, or can we use native `depthResolveFilter`/`stencilResolveFilter`? **[?]**

---

## 8. Sources

Pristine reference repositories (read-only):

- Mesa KosmicKrisp Vulkan driver — <https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/kosmickrisp>
  (files `vulkan/kk_format.c`, `kk_sampler.c`, `kk_cmd_copy.c`, `kk_cmd_clear.c`, `kk_cmd_meta.c`,
  `kk_cmd_buffer.c`, `kk_image.c`, `kk_image_view.c`, `kk_image_layout.c`, `kk_physical_device.c`,
  `bridge/mtl_sampler.m`, `bridge/mtl_types.h`, `bridge/mtl_render_state.m`).
- Godot Metal `pixel_formats.cpp` — <https://github.com/godotengine/godot/blob/master/drivers/metal/pixel_formats.cpp>
- Apple metal-cpp headers — <https://github.com/brackeen/gltf-viewer> / official
  <https://developer.apple.com/metal/cpp/> (`MTL4ComputeCommandEncoder.hpp`, `MTL4RenderPass.hpp`,
  `MTLRenderPass.hpp`, `MTLTexture.hpp`, `MTLResource.hpp`, `MTLSampler.hpp`, `MTL4ArgumentTable.hpp`).

RPCS3 in-repo (read-only, branch `kosmickrisp-integration`):

- `rpcs3/Emu/RSX/VK/VKFormats.cpp`, `VKRenderTargets.{h,cpp}`, `VKResolveHelper.{h,cpp}`,
  `VKTexture.cpp`, `VKTextureCache.cpp`, `VKResourceManager.cpp`, `VKGSRender.cpp`,
  `vkutils/image.h`, `vkutils/image_helpers.h`, `vkutils/sampler.{h,cpp}`,
  `vkutils/vk_driver_selection.h`.

Apple documentation / WWDC25 (fetched during research; canonical URLs):

- Metal 4 — <https://developer.apple.com/documentation/metal/metal-4>
- `MTL4ComputeCommandEncoder` — <https://developer.apple.com/documentation/metal/mtl4computecommandencoder>
- `MTL4RenderPassDescriptor` — <https://developer.apple.com/documentation/metal/mtl4renderpassdescriptor>
- `MTL4ArgumentTable` — <https://developer.apple.com/documentation/metal/mtl4argumenttable>
- `MTL4CommandBuffer` — <https://developer.apple.com/documentation/metal/mtl4commandbuffer>
- `MTLPixelFormat` — <https://developer.apple.com/documentation/metal/mtlpixelformat>
- Choosing a pixel format for a texture — <https://developer.apple.com/documentation/metal/choosing-a-pixel-format-for-a-metal-texture>
- `MTLSamplerDescriptor` — <https://developer.apple.com/documentation/metal/mtlsamplerdescriptor>
- Sampler addressing / border colors — <https://developer.apple.com/documentation/metal/mtlsampleraddressmode>
- `MTLTexture` — <https://developer.apple.com/documentation/metal/mtltexture>
- Texture views (`newTextureView`) — <https://developer.apple.com/documentation/metal/mtltexture/newtextureview(pixelformat:)>
- `MTLTextureUsage` — <https://developer.apple.com/documentation/metal/mtltextureusage>
- `MTLStorageMode` (memoryless) — <https://developer.apple.com/documentation/metal/mtlstoragemode>
- `MTLRenderPassAttachmentDescriptor` (resolve, store actions) — <https://developer.apple.com/documentation/metal/mtlrenderpassattachmentdescriptor>
- `MTLStoreAction` — <https://developer.apple.com/documentation/metal/mtlstoreaction>
- `maxArgumentBufferSamplerCount` — <https://developer.apple.com/documentation/metal/mtldevice/maxargumentbuffersamplercount>
- `supportsTextureSampleCount(_:)` — <https://developer.apple.com/documentation/metal/mtldevice/supportstexturesamplecount(_:)>
- `isDepth24Stencil8PixelFormatSupported` — <https://developer.apple.com/documentation/metal/mtldevice/isdepth24stencil8pixelformatsupported>
- Metal Feature Set Tables — <https://developer.apple.com/documentation/metal/metal-feature-set-tables>
- WWDC25 "Discover Metal 4" (session 205) — <https://developer.apple.com/videos/play/wwdc2025/205/>
- WWDC25 Metal 4 compute/ML consolidation (session 254) — <https://developer.apple.com/videos/play/wwdc2025/254/>
- `CAMetalLayer` colorspace — <https://developer.apple.com/documentation/quartzcore/cametallayer/colorspace>
- Metal sparse resources — <https://developer.apple.com/documentation/metal/sparse-resources>

**Could not verify:** exact behaviour of `MTL4ArgumentTable.setSamplerState` regarding the
argument-buffer sampler accounting cap (open question 6); whether the Metal depth/stencil-resolve
sync bug persists in the Metal 4 runtime (open question 8); the precise Metal 4 minimum-OS/HW
matrix beyond "macOS 26 / M1+" (documented for `MTL4*` APIs).
