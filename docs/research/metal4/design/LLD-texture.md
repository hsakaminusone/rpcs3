# LLD-texture — Fragment texture cluster: format decode, samplers, swizzle, program pointer

Owner/Scope: RSX-first Metal 4 backend, fragment-texture-cluster boundary | Status: draft | Date: 2026-10-16
Branch: `metal4`. Design only — nothing under `rpcs3/**` is modified.

**Legend.** `[V]` verified against a primary source in this repo; `[I]` inference from `[V]`
facts; `[U]` could not verify. `file:line` is advisory (the branch moves); **symbol names are the
stable reference**. Difficulty scale: **1** trivial rename · **2** mechanical · **3** real work,
well-trodden · **4** hard / needs design · **5** unknown.

---

## 1. Scope

The **texture** cluster is the 16 registers at NV4097 addresses `0x08e4`–`0x1fe8`
(`inventory/out/clusters/texture.csv`). It is everything the guest writes to configure **fragment
texture sampling**, plus the fragment program pointer, the fragment shader control/window words, the
texture-coordinate control array, and three fixed-function registers that carry no reader.

**In scope:** the 16 `M_NV4097_*` ids listed in §2.1: the 16-unit fragment texture register file
(offset, format, address, control0/1/2/3, filter, image-rect, border colour), the fragment program
pointer, texture-coordinate control, the colour-key block, shader control, and shader window.

**Out of scope (owned elsewhere):**
- Vertex texture units (VTF) — the `VERTEX_TEXTURE_*` registers live in the **vertex** shard
  (`LLD-vertex` §3.5); this document fixes the *fragment* path only. The two share the sampler and
  format model. `[V]`
- Texture **resolution, format substitution, deswizzle/tiling, upload, flush** — `HLD-02`
  §2.4/§2.5/§2.7 and scope `04`. This document owns the *state → cache entry* seam, not the cache
  internals. `[V]`
- Sampler **object lifetime, arena, eviction** — `04` §2.8; this document fixes only the
  `MTLSamplerState` fields and the argument-table slot. `[V]`
- Pass/encoder synthesis, MSAA, ROP — `HLD-03`, `04` §2.7; referenced only where a texture result
  is an attachment view. `[V]`
- Fragment **program decompilation** (ucode→GLSL) — `Program/` + `HLD-05`. This document fixes only
  how the *program pointer* and *shader control/window* map. `[V]`

This document is the single owner of: the fragment texture argument-table slots (§3.0), the
`CELL_GCM_TEXTURE_*` decode→`MTLPixelFormat`/`MTLTextureType` seam (§3.1), the CONTROL1
channel-swizzle fork (§3.3), and the dead-register dispositions (§3.7).

---

## 2. Register model

### 2.1 Master decode table (every id in `clusters/texture.csv`)

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_SHADER_PROGRAM` | 0x08e4 | `shader_program_address()` bits 0-30 | bind | reshape | `MTL4::Library` + `LibraryFunctionDescriptor` |
| `M_NV4097_SET_TEXTURE_CONTROL2` | 0x0b00 | — (no decoder) | bind_range x16 | na | none (undecoded; no reader) |
| `M_NV4097_SET_TEX_COORD_CONTROL` | 0x0b40 | bit0 per index 0-9 | bind_array x10 | reshape | MSL texcoord expression; shader key |
| `M_NV4097_SET_TEXTURE_CONTROL3` | 0x1840 | `texture_control3_helper` x16 (pitch/depth) | bind_range x16 | reshape | `MTLTextureDescriptor` depth; cache pitch |
| `M_NV4097_SET_TEXTURE_OFFSET` | 0x1a00 | `texture_offset_helper` x16 (stride 8) | bind_range x16 | reshape | `MTL::Texture` cache entry; `setTexture(12+i)` |
| `M_NV4097_SET_TEXTURE_FORMAT` | 0x1a04 | `texture_format_helper` x16 | bind_range x16 | reshape | `MTLPixelFormat` + `MTLTextureType` |
| `M_NV4097_SET_TEXTURE_ADDRESS` | 0x1a08 | wrap s/t/r + remap flags + zfunc | bind_range x16 | reshape | `MTLSamplerState` + shader decode flags |
| `M_NV4097_SET_TEXTURE_CONTROL0` | 0x1a0c | `texture_control0_helper` x16 (LOD/aniso/enable) | bind_range x16 | reshape | `MTLSamplerDescriptor` LOD/aniso; bind gate |
| `M_NV4097_SET_TEXTURE_CONTROL1` | 0x1a10 | raw channel remap | bind_range x16 | reshape | `MTLTextureSwizzle` or shader remap |
| `M_NV4097_SET_TEXTURE_FILTER` | 0x1a14 | bias/convolution/min/mag/argb_signed | bind_range x16 | reshape | `MTLSamplerDescriptor` filters + shader bias |
| `M_NV4097_SET_TEXTURE_IMAGE_RECT` | 0x1a18 | `texture_image_rect_helper` x16 (w/h) | bind_range x16 | reshape | `MTLTextureDescriptor` width/height |
| `M_NV4097_SET_TEXTURE_BORDER_COLOR` | 0x1a1c | RGBA8 border word | bind_range x16 | reshape | `MTLSamplerDescriptor::borderColor` |
| `M_NV4097_SET_COLOR_KEY_COLOR` | 0x1d00 | — (no decoder) | bind_array x16 | na | none (colour-key unimplemented) |
| `M_NV4097_SET_SHADER_CONTROL` | 0x1d60 | `shader_ctrl()` raw u32 | unbound | reshape | fragment MSL + PSO/shader key |
| `M_NV4097_SET_SHADER_WINDOW` | 0x1d88 | height/origin/pixel_center | unbound | reshape | fragment context wpos (slot 3) |
| `M_NV4097_SET_NO_PARANOID_TEXTURE_FETCHES` | 0x1fe8 | — (no decoder) | unbound | na | none (no reader) |

### 2.2 Decode

**Guest address and location (`fragment_texture::offset()`/`location()`, `RSXTexture.cpp:26-34`).**
`offset()` is the raw word masked with `0x7FFFFFFF` (bit 31 is the memory location); `location()` is
`FORMAT & 0x3` minus one → `CellGcmLocation`. The pair `(location, offset)` is the guest identity
that `rsx::get_address` resolves. `[V]`

**Format word (`fragment_texture::format()`, `RSXTexture.cpp:63-141`).** One `u32`: `location`
(bits 0-1), `cubemap` (bit 2), `border_type` (bit 3), `dimension` (bits 4-7 → `texture_dimension`),
`format` (bits 8-15 → `CellGcmTexture`), `mipmap` (bits 16-31). `get_extended_texture_dimension()`
folds `cubemap` into the 2D case (`:51-61`); `is_compressed_format()` tests DXT1/23/45 (`:128-136`);
`format_ex()` derives the SEXT/EXPAND/GAMMA flags from `format`, `unsigned_remap()`, `gamma()` and
`argb_signed()` with the documented precedence SNORM > GAMMA > UNSIGNED_REMAP/BX2 (`:68-126`). `[V]`

**Address word (`fragment_texture::wrap_s/t/r()` etc., `RSXTexture.cpp:158-198`).** `wrap_s`
(bits 0-3), `aniso_bias` (4-7), `wrap_t` (8-11), `unsigned_remap` (12-15), `wrap_r` (16-19),
`gamma` (20-23, RGBA→ARGB re-mapped at `:183-188`), `signed_remap` (24-27), `zfunc` (28-31). The
wrap enums are `rsx::texture_wrap_mode` (`gcm_enums.h:1484-1502`); `zfunc` is a
`comparison_function`. `[V]`

**Control0 (`RSXTexture.cpp:200-236`; decoder `rsx_decode.h:4715-4754`).** `enabled` (bit 31),
`max_lod` (bits 7-18), `min_lod` (bits 19-30), both `4.8` fixed-point (`decode_fxp<4,8,false>`);
`max_aniso` (bits 4-6, overridable by config at `:217-228`); `alpha_kill_enabled` (bit 2). `[V]`

**Control1 (`fragment_texture::remap()`/`decoded_remap()`, `RSXTexture.cpp:238-310`).** `remap()` is
the raw `u32`. `decoded_remap()` rewrites the low word for the special formats — float formats force
`0xE4` (`:250-267`), 16-bit-block formats (`X16`, `Y16_X16`, `HILO8`, `HILO_S8`) map
`0xE4/0x4E/0xEE/0x44` to `0x66/0x99/0xAA/0x55` under the high-word override (`:268-304`) — then
`decode_remap_encoding` produces the channel-select vector. The shader-side remap is applied by
`remap_vector` (`RSXFragmentPrologue.glsl:154-181`), and the *raw* remap is copied into the slot-4
`sampler_info.remap` (`RSXThread.cpp:2498`). `[V]`

**Filter (`fragment_texture::bias()` etc., `RSXTexture.cpp:312-356`).** `bias` (bits 0-12,
`decode_fxp<4,8>` plus the config `texture_lod_bias`, clamped to `[-16, 15.996]`),
`convolution_filter` (13-15), `min_filter` (16-18), `mag_filter` (24-26), `argb_signed`/per-channel
`*_signed` (28-31). Enums: `texture_minify_filter`/`texture_magnify_filter`
(`gcm_enums.h:1524-1553`). `[V]`

**Image rect and control3 (`RSXTexture.cpp:358-366,448-456`).** `width` (bits 16-31), `height`
(bits 0-15, forced `1` for 1D at `:365`); `pitch` (control3 bits 0-15) and `depth` (control3 bits
20-31, read only when `dimension3d` at `:450`). These feed `image_section_attributes_t` (`:458-471`),
the upload descriptor. `[V]`

**Border colour (`fragment_texture::border_color()`, `RSXTexture.cpp:368-436`).** Raw RGBA8. When
`argb_signed` is set, RPCS3 re-expands and bias-compensates the border because the PS3 SNORM border
path is broken (`:382-435`). `remapped_border_color()` then applies the channel remap (`:438-446`). `[V]`

**Shader program pointer (`shader_program_address()`, `rsx_decode.h:1800-1821`).** Bits 0-30; the
dump splits location and `& ~3` offset. The handler `set_shader_program_dirty` only raises
`fragment_program_ucode_dirty` (`nv4097.cpp:19-22`). `[V]`

**Shader control (`shader_ctrl()`, `rsx_decode.h:1683-1705`; accessor `rsx_methods.h:924-927`).** Raw
`u32`; the meaningful flags are `RSX_SHADER_CONTROL_*` (`gcm_enums.h:450-483`) and the GCM flags
`CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT`/`_32_BITS_EXPORTS`. It has a decoder for dumping but a
`nullptr` handler (`rsx_methods.cpp:1463`). `[V]`

**Shader window (`rsx_decode.h:3117-3151`).** `window_shader_height` (bits 0-11),
`window_shader_origin` (12-15 → `window_origin`), `pixel_center` (16-19 → `window_pixel_center`).
Consumed by the fragment context as `wpos_scale`/`wpos_bias` (`RSXDrawCommands.cpp:708-738`). `[V]`

**Texture-coordinate control (`texcoord_control_mask()`, `rsx_methods.h:1332-1342`).** Ten words;
bit 0 of index `i` selects 1D vs 2D coordinates for texture `i`, consumed by
`RSXFragmentProgram::texcoord_is_2d` (`RSXFragmentProgram.h:283`). `[V]`

**Dead registers.** `SET_TEXTURE_CONTROL2` has no decoder and no reader; only the cellGcmInit default
`0x2dc8` (`rsx_methods.cpp:253-268`) and the register printer (`gcm_printing.cpp:265-280`).
`SET_COLOR_KEY_COLOR` has a `nullptr` handler (`rsx_methods.cpp:1461`) and no reader (a comment only
at `GLDraw.cpp:324`). `SET_NO_PARANOID_TEXTURE_FETCHES` has a `nullptr` handler
(`rsx_methods.cpp:1505`), default `0x0` (`:534`) and no reader. `[V]`

### 2.3 Lifetime: value vs dirty-bit

Two lifetimes, and the backend must honour both:

1. **Value state** — all eight per-unit fragment texture registers, the program pointer, shader
   control and shader window. Held in `rsx::method_registers` (`rsx_methods.h:144-145`) and read at
   draw/compile time. `[V]`
2. **Dirty-bit state** — the eight `SET_TEXTURE_*` registers (stride 8) plus `SET_TEXTURE_CONTROL2`
   and `SET_TEXTURE_CONTROL3` (stride 1) route through `nv4097::set_fragment_texture_dirty_bit`
   (`rsx_methods.cpp:1668-1677`), which sets `m_textures_dirty[index]` and, if the fragment program
   references that unit, `fragment_program_state_dirty` (`common.cpp:65-78`). `SET_SHADER_PROGRAM`
   sets `fragment_program_ucode_dirty` (`nv4097.cpp:19-22`). `SET_TEX_COORD_CONTROL` and
   `SET_SHADER_WINDOW` are unbound but carry `state_signals` of
   `fragment_program_state_dirty`/`fragment_state_dirty` (`rsx_methods.cpp:703-712,724`). `[V]`

The Metal backend does not own these bits: `rsx::thread`, `texture_cache` and the sampler pool own
them. The LLD's obligation is that every consumer in §3 keys off the same value (`rsx_state`) so a
dirty-bit that is *not* set cannot leave a stale `MTL::Texture`/`MTLSamplerState` bound. `[I]`

---

## 3. Metal 4 mapping

### 3.0 The fragment texture argument-table slot map

The RSX shader ABI is fixed by `vkutils/pipeline_binding_table.h:7-22` (`out/rsx_binding_slots.csv`).
Under RSX-first this is the **fragment-stage `MTL4ArgumentTable` index map** (`HLD-05` §3.1/§3.2);
the texture cluster owns the slots marked ★.

| slot | source | this cluster owns? | `MTL4::ArgumentTable` call |
|---|---|---|---|
| 2 | fragment constants | no | `setAddress(fc->gpuAddress(), 2)` |
| 3 | fragment state (context) | ★ (`SET_SHADER_WINDOW` wpos) | `setAddress(gpuAddress + fs_context_offset, 3)` |
| 4 | fragment texture params | ★ (per-TIU decode: remap/control) | `setAddress(gpuAddress + fs_texture_base_index, 4)` |
| 9 | rasteriser env (stipple) | no | `setAddress(…, 9)` |
| 12+i | fragment textures T0..T15 | ★ (all 8 per-unit registers, i = 0-15) | `setTexture(view, 12+i)` + `setSamplerState(sampler, 12+i)` |

Slot 4 is the `TextureParametersBuffer` (`sampler_info texture_parameters[]`,
`VKFragmentProgram.cpp:328-331`); the shader reads `texture_parameters[texture_base_index + i]`
(`:544`). Slot 12 is shared with the vertex-texture base in the *vertex* table without collision
because argument tables are bound per stage (`HLD-05` §3.1). MSL has no combined image-sampler, so
each unit is a `texture`/`sampler` pair (`HLD-05` §3.2). `[V]`

### 3.1 Format and dimension decode → `MTLPixelFormat` / `MTLTextureType`

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_TEXTURE_FORMAT` | 0x1a04 | location/cubemap/dimension/format/mipmap | bind_range x16 | reshape | `MTLPixelFormat` + `MTLTextureType` |
| `M_NV4097_SET_TEXTURE_OFFSET` | 0x1a00 | addr + location | bind_range x16 | reshape | `MTL::Texture` (cache); `setTexture(12+i)` |

The format word selects the cache-entry key (`HLD-02` §2.5) and its `MTLPixelFormat` per the
`HLD-02` §2.7 table: DXT1/23/45 → `BC1/2/3_RGBA`; `DEPTH24_D8` → `Depth32Float_Stencil8` (no
`Depth24Unorm_Stencil8` on Apple silicon); `A1R5G5B5`/`D1R5G5B5` → `BGR5A1Unorm` (KK's native enum
is disabled for the border-colour bug); `A4R4G4B4` → `ABGR4Unorm` + swizzle;
`COMPRESSED_B8R8_G8R8`/`R8B8_R8G8` → `BGRA8Unorm` substitute; `HILO8`/`HILO_S8` →
`RG8Unorm`/`RG8Snorm`; packed 24-bit → no enum, must be emulated. `MTLTextureType` follows
`get_extended_texture_dimension()`: 1D → `MTLTextureType2D` with height 1 (`04` #3), 2D → `Type2D`,
cubemap → `TypeCube`, 3D → `Type3D`. `[V]`/`[I]`

### 3.2 Addressing, filtering, border, LOD

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_TEXTURE_ADDRESS` | 0x1a08 | wrap s/t/r, remap flags, zfunc | bind_range x16 | reshape | `MTLSamplerState` + shader decode flags |
| `M_NV4097_SET_TEXTURE_CONTROL0` | 0x1a0c | LOD clamp/aniso/enable/alpha-kill | bind_range x16 | reshape | `MTLSamplerDescriptor` LOD/aniso; bind gate |
| `M_NV4097_SET_TEXTURE_FILTER` | 0x1a14 | bias/convolution/min/mag/argb_signed | bind_range x16 | reshape | `MTLSamplerDescriptor` filters + shader bias |
| `M_NV4097_SET_TEXTURE_BORDER_COLOR` | 0x1a1c | RGBA8 border | bind_range x16 | reshape | `MTLSamplerDescriptor::borderColor` |

**Addressing.** `wrap_s/t/r` → `sAddressMode`/`tAddressMode`/`rAddressMode` (`04` §2.1 #9); `zfunc` →
`compareFunction` when the unit is a shadow sampler (`04` §2.1 #18; `RSXThread.cpp:2599-2605`). The
remaining bits of the address word are **not** sampler state: `unsigned_remap`, `gamma` and
`signed_remap` are format-decode flags folded into `format_ex().texel_remap_control`
(`RSXTexture.cpp:68-126`) and applied shader-side via `RSX_SHADER_CONTROL_TEXTURE_FORMAT_CONVERT`. `[V]`

**Filtering and LOD.** `min_filter`/`mag_filter` → `minFilter`/`magFilter` (Metal has separate
min/mag — earlier assumptions were wrong, `04` §2.1 #14); `mipmap` (format bits 16-31) → `mipFilter`;
`min_lod`/`max_lod` → `lodMinClamp`/`lodMaxClamp` (`04` §2.1 #16); `max_aniso` → `maxAnisotropy`
(`#15`). `bias` is `4.8` fixed-point but Metal's `lodBias` is `S4.6` (`04` #17/R19), so exact bias
is **shader-injected**; `convolution_filter` has no Metal equivalent and is dropped. `[V]`/`[I]`

**Border colour.** Only `TransparentBlack`/`OpaqueBlack`/`OpaqueWhite` exist (`04` R18); the custom
colour falls back to `get_closest_border_color_enum`. RPCS3's SNORM re-expansion
(`RSXTexture.cpp:368-436`) must run *before* the closest-colour selection. `[V]`

### 3.3 Channel swizzle (CONTROL1)

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_TEXTURE_CONTROL1` | 0x1a10 | raw channel remap | bind_range x16 | reshape | `MTLTextureSwizzle` or shader-side remap |

Two candidate realisations, and this is the cluster's main fork (Q1):

- **Hardware `MTLTextureSwizzle`** — set on the texture view (`04` #37). Cheap, but can only express
  a static channel select and cannot reproduce the SEXT/EXPAND/GAMMA decode or the format-specific
  `decoded_remap()` rewrites (`RSXTexture.cpp:243-310`). `[I]`
- **Shader-side remap** — the current RPCS3 path: the raw remap goes into `sampler_info.remap`
  (slot 4) and `remap_vector` applies it (`RSXThread.cpp:2498`;
  `RSXFragmentPrologue.glsl:154-181`). This keeps the remap next to the per-format decode flags
  (`format_ex`), at the cost of a shader branch. `[V]`

Under RSX-first the safe default is to **keep the shader-side remap** (it is what the VK/GL backends
already do and it composes with `RSX_SHADER_CONTROL_TEXTURE_FORMAT_CONVERT`), and use
`MTLTextureSwizzle` only for the depth→`(d,d,d,1)` and `A4R4G4B4` fixed cases where no decode is
involved. `[I]`

### 3.4 Image rect, pitch and depth (CONTROL3)

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_TEXTURE_IMAGE_RECT` | 0x1a18 | width/height | bind_range x16 | reshape | `MTLTextureDescriptor` width/height |
| `M_NV4097_SET_TEXTURE_CONTROL3` | 0x1840 | pitch (0-15), depth (20-31) | bind_range x16 | reshape | `MTLTextureDescriptor` depth; cache pitch |

`width`/`height` become the `MTLTextureDescriptor` dimensions; `height` is 1 for 1D
(`RSXTexture.cpp:363`). `pitch` is the guest bytes-per-row and feeds the cache's
`memory_range`/`native_pitch` decision exactly as for surfaces (`HLD-02` §2.3, invariant I4); `depth`
is the 3D texture depth (`:450`). Both are part of the upload descriptor
`image_section_attributes_t` (`:458-471`) and never a standalone Metal field beyond the descriptor.
`[V]`/`[I]`

### 3.5 Fragment program pointer and shader control

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_SHADER_PROGRAM` | 0x08e4 | `shader_program_address()` bits 0-30 | bind | reshape | `MTL4::Library` + `LibraryFunctionDescriptor` |
| `M_NV4097_SET_SHADER_CONTROL` | 0x1d60 | `shader_ctrl()` raw u32 | unbound | reshape | fragment MSL + shader/PSO key |

The program pointer is an upload/cache identity, not GPU state: the guest address selects the
fragment ucode, the decompiler emits GLSL, glslang emits SPIR-V, SPIRV-Cross emits MSL and
`MTL4::Compiler::newLibrary` builds a `MTL4::Library` (`HLD-05` §2.2/§3.4). The handler only raises
`fragment_program_ucode_dirty` (`nv4097.cpp:19-22`); the address itself is consumed by the
`ProgramStateCache` ucode lookup, so the Metal object is the *cached library*, keyed by the ucode
hash, not by this register. `[V]`

`SET_SHADER_CONTROL` is the fragment shader's control word:
`CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT`/`_32_BITS_EXPORTS` and the `RSX_SHADER_CONTROL_*` bits
(`gcm_enums.h:450-483`) select MSL code paths (precision, depth export, flat shading, instanced
constants, interpreter model, sRGB, alpha test, programmable blending, depth-range emulation).
Because it changes the emitted MSL it is part of the **shader cache key** and, transitively, the PSO
(`HLD-05` §3.5 invariant 8); the depth-stencil subset that is not baked into the MSL becomes a
separate `MTL::DepthStencilState` (`HLD-05` §2.6). `[V]`

### 3.6 Shader window

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_SHADER_WINDOW` | 0x1d88 | height/origin/pixel_center | unbound | reshape | fragment context wpos (slot 3) |

The window height and origin define the window-space Y mapping the fragment shader uses for `wpos`
(gl_FragCoord emulation): the draw-parameter filler writes `wpos_scale` and `wpos_bias` into the
fragment context from `shader_window_origin()`/`shader_window_height()`
(`RSXDrawCommands.cpp:720-738`), and `pixel_center == integer` is emulated by subtracting `0.5` from
both biases (`:731-738`, because MSL/SPIR-V only permit a half pixel centre). This is a field of the
slot-3 `FragmentStateBuffer`, not a viewport object; the actual Metal viewport comes from the
rasteriser cluster. `[V]`/`[I]`

### 3.7 Texture-coordinate control and dead registers

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_TEX_COORD_CONTROL` | 0x0b40 | bit0 per index 0-9 | bind_array x10 | reshape | MSL texcoord expression; shader key |
| `M_NV4097_SET_TEXTURE_CONTROL2` | 0x0b00 | — | bind_range x16 | na | none |
| `M_NV4097_SET_COLOR_KEY_COLOR` | 0x1d00 | — | bind_array x16 | na | none |
| `M_NV4097_SET_NO_PARANOID_TEXTURE_FETCHES` | 0x1fe8 | — | unbound | na | none |

`SET_TEX_COORD_CONTROL` is `nullptr`-handled but read through `texcoord_control_mask()`
(`rsx_methods.h:1332-1342`); bit 0 of unit `i` tells the decompiler to emit a 2D coordinate
expression instead of 1D (`RSXFragmentProgram.h:283-292`). Because it changes the emitted MSL it
belongs in the **shader cache key** (`HLD-05` §3.5). The other three have no decoder and no reader:
`SET_TEXTURE_CONTROL2` is the `0x2dc8` cellGcmInit default only (`rsx_methods.cpp:253`),
`SET_COLOR_KEY_COLOR` is unimplemented (`GLDraw.cpp:324` is a comment), and
`SET_NO_PARANOID_TEXTURE_FETCHES` is default-zero with no reader. `[V]`

---

## 4. Impossible / not-recommended

- **Custom border colour — impossible.** Metal exposes only `TransparentBlack`/`OpaqueBlack`/
  `OpaqueWhite` (`04` R18). The chosen fallback is RPCS3's existing `get_closest_border_color_enum`,
  applied *after* the PS3 SNORM re-expansion (`RSXTexture.cpp:368-436`). `[V]`
- **Exact float LOD bias via `lodBias` — not-recommended.** `lodBias` is `S4.6` fixed-point and
  quantises the `4.8` RSX bias (`04` #17/R19). Choose shader-side injection over the descriptor
  field, or accept the quantisation. `[V]`
- **Combined image-sampler — impossible.** MSL has no combined sampler; every unit is a
  `texture`/`sampler` pair (`HLD-05` §3.2). Not a mapping failure — it is a reshape of one RSX unit
  into two argument-table slots. `[V]`
- **Convolution min/mag filter — not-recommended.** `CELL_GCM_TEXTURE_CONVOLUTION_MIN`/`_MAG`
  (`gcm_enums.h:1532,1547`) have no Metal filter mode; treat as the nearest linear mode and record
  the divergence. `[I]`
- **`DEPTH24_D8` → native 24-bit depth-stencil — impossible on Apple silicon.** Only
  `Depth32Float_Stencil8` exists; the fallback is already `HLD-02` §2.7. `[V]`
- **`A1R5G5B5`/`D1R5G5B5` native enum — not-recommended.** KK disables `A1B5G5R5Unorm` for a
  border-colour hardware bug; use `BGR5A1Unorm` (RGB-swapped) or shader emulation (`HLD-02` §2.7,
  `04` §2.3). `[V]`
- **Argument-buffer sampler count — hard limit, avoid the path.** macOS caps argument-buffer
  samplers at ~1024 (`04` §2.8/R17); bind via `MTL4ArgumentTable::setSamplerState` rather than
  stuffing samplers into an argument buffer. `[I]`
- **`SET_TEXTURE_CONTROL2`, `SET_COLOR_KEY_COLOR`, `SET_NO_PARANOID_TEXTURE_FETCHES` — `na`.** No
  decoder and no reader; no Metal construct should be invented for them. `[V]`

---

## 5. Invariants & failure modes

**Invariants.**
1. **Unit/format agreement.** For every unit `i` with `fragment_textures[i].enabled()` and a defined
   `format_class`, the texture cache holds one `MTL::Texture` whose `MTLPixelFormat`/`MTLTextureType`
   match the format word, and a `MTLSamplerState` whose address/filter/LOD fields match the
   address/filter/control0 words (`HLD-02` I1). `[V]`
2. **Slot pairing.** For every live unit `i`, `setTexture(…, 12+i)` and `setSamplerState(…, 12+i)` are
   both bound before the draw; an unpaired unit is a bug (`HLD-05` invariant 3). `[I]`
3. **Remap/decode agreement.** The channel select the shader applies (`sampler_info.remap`) equals
   `decoded_remap()` for the bound format, and the per-format SEXT/EXPAND/GAMMA flags in
   `sampler_info.control` are consistent with `format_ex()` (`RSXTexture.cpp:68-126`;
   `RSXThread.cpp:2650-2670`). `[V]`
4. **Shader-key completeness.** `SET_SHADER_CONTROL`'s MSL-affecting bits, `SET_TEX_COORD_CONTROL`
   and the per-unit format flags are all in the shader cache key; two draws with equal ucode but
   different control words must not share an MSL (`HLD-05` invariant 8). `[V]`
5. **Program identity.** The MSL/`MTL4::Library` for a draw is keyed by the fragment ucode the
   program pointer selects; `fragment_program_ucode_dirty` alone does not imply a new ucode
   (`nv4097.cpp:19`). `[I]`
6. **Border after re-expansion.** The closest-colour fallback is chosen from the SNORM-corrected
   border, not the raw word. `[V]`

**Failure modes.**

| # | Symptom | Cause | Detection / mitigation |
|---|---|---|---|
| F1 | Wrong channels (R/B swap, alpha misplaced) | `decoded_remap()` mismatch, or `MTLTextureSwizzle`/shader remap double-applied | invariant 3; H2 decision trace on the remap word |
| F2 | Washed-out / double-gamma texture | GAMMA flag applied both in the format decode and via an sRGB view | `format_ex` GAMMA → `RSX_SHADER_CONTROL_TEXTURE_FORMAT_CONVERT`; keep the view linear (`04` §2.3) |
| F3 | Stale texture after a re-bind | `m_textures_dirty[i]` not propagated to the cache/bind | `set_fragment_texture_dirty_bit` (`common.cpp:65`); H2 binding trace |
| F4 | Slightly wrong LOD at mip transitions | `lodBias` `S4.6` quantisation | shader-injected bias (R19); assert bias is injected, not in the descriptor |
| F5 | Wrong border value | custom border colour mapped to the nearest fixed colour, or SNORM not re-expanded | §3.2/§4; compare against a KK reference on a border-sampling title (HLD-02 Q5) |
| F6 | `BGRA8` used where `BC1` was intended (or vice versa) | DXT format mis-detected (`is_compressed_format`, `RSXTexture.cpp:128`) | invariant 1; HLD-02 §2.7 format table |
| F7 | 3D texture collapses to a single slice | `depth` read for a non-3D dimension, or ignored for a 3D one | `RSXTexture.cpp:448-450`; invariant 1 |
| F8 | Sampler pool OOM | > 1024 argument-buffer samplers | prefer `setSamplerState` (`04` §2.8/R17); dedup by `sampler_pool_key_t` |
| F9 | PSO/MSL reused across differing shader control | `SET_SHADER_CONTROL`/texcoord-control omitted from the shader key | invariant 4 |
| F10 | wpos off by half a pixel | `pixel_center == integer` not emulated | `RSXDrawCommands.cpp:731-738`; invariant of §3.6 |

---

## 6. Open questions

- **Q1 — CONTROL1 realisation.** Hardware `MTLTextureSwizzle` vs shader-side `sampler_info.remap`
  (§3.3). The shader path composes with the per-format decode; the hardware path avoids a branch but
  cannot express SEXT/EXPAND/GAMMA. Decide per format, or keep the shader path wholesale. `[I]`
- **Q2 — LOD bias placement.** Descriptor `lodBias` (`S4.6`) vs shader-injected half-float bias
  (`04` R19). Confirm which the shader epilogue can carry without a new slot. `[I]`
- **Q3 — Sampler binding mechanism.** Does `MTL4ArgumentTable::setSamplerState` sidestep the 1024
  argument-buffer cap (`04` §2.8/R17)? Unverified on device. `[U]`
- **Q4 — Border-colour fidelity.** How close is `get_closest_border_color_enum` to the guest value
  across the SNORM/BIASED cases (`RSXTexture.cpp:368-436`)? Needs a hardware-reference render. `[U]`
- **Q5 — Gamma path.** `format_ex` GAMMA could be realised either in the shader (current) or by an
  `…_sRGB` texture view; the latter risks double-encoding with the ROP sRGB packer (`LLD-vertex`
  §3.8). Confirm the single-encode path. `[I]`
- **Q6 — `MTLTextureType2D` vs `Type1D` for 1D textures.** KK emulates 1D as 2D (`04` #3); confirm
  no MSL sampling difference under the pinned SPIRV-Cross. `[I]`
- **Q7 — DXT sRGB variants.** `COMPRESSED_DXT1` with the UN/LN feature bits may need
  `BC1_RGBA_sRGB` vs `BC1_RGBA`; the choice interacts with the GAMMA flag (`04` §2.3). `[I]`
- **Q8 — `SET_TEXTURE_CONTROL2` semantics.** No decoder/reader in the tree; confirm on hardware
  whether the `0x2dc8` block ever changes behaviour, before leaving it `na`. `[U]`
- **Q9 — Depth-texture `compareFunction` and `depth2d`.** Which formats take the native
  `sample_compare` path vs the emulated `require_tex_shadow_ops` (`HLD-05` §2.5;
  `RSXThread.cpp:2593-2607`)? `[I]`

---

## 7. Provenance

**In-repo — decoders & accessors** `[V]`
- `rpcs3/Emu/RSX/RSXTexture.cpp:26-34` (offset/location), `:36-61` (cubemap/dimension), `:63-141`
  (format/format_ex/mipmap), `:158-198` (address), `:200-236` (control0), `:238-310`
  (control1/decoded_remap), `:312-356` (filter), `:358-366` (image rect), `:368-446` (border),
  `:448-471` (control3/attributes).
- `rpcs3/Emu/RSX/rsx_decode.h:4619-4637` (`texture_offset_helper`), `:4639-4686`
  (`texture_format_helper`), `:4688-4712` (`texture_image_rect_helper`), `:4714-4754`
  (`texture_control0_helper`), `:4756-4780` (`texture_control3_helper`), `:1683-1705` (shader
  control), `:1800-1821` (shader program), `:3117-3151` (shader window).
- `rpcs3/Emu/RSX/rsx_methods.h:924-927` (`shader_control`), `:1332-1342` (`texcoord_control_mask`),
  `:144-145` (texture state arrays).
- `rpcs3/Emu/RSX/rsx_methods.cpp:230-239,253-268,534` (defaults), `:703-712,724` (state signals),
  `:1409-1505` (unbound ids), `:1596-1611` (bind_array), `:1668-1685` (bind_range texture/VTF),
  `:1707-1708` (`SET_SHADER_PROGRAM` bind).
- `rpcs3/Emu/RSX/NV47/HW/common.cpp:65-78` (`set_fragment_texture_dirty_bit`);
  `NV47/HW/nv4097.cpp:19-22` (`set_shader_program_dirty`).
- `rpcs3/Emu/RSX/gcm_enums.h:444-484` (`RSX_SHADER_CONTROL_*`), `:1484-1553`
  (`texture_wrap_mode`/`texture_max_anisotropy`/minify/magnify filters).

**In-repo — consumers & shader seam** `[V]`
- `rpcs3/Emu/RSX/RSXThread.cpp:2481-2670` (per-unit texture state fill: `texture_params[i].remap`
  `:2498`, `.control` `:2670`, shadow `:2593-2607`).
- `rpcs3/Emu/RSX/Core/RSXDrawCommands.cpp:708-738` (shader window → wpos coefficients).
- `rpcs3/Emu/RSX/Program/RSXFragmentProgram.h:266,278-292` (`texcoord_control_mask`,
  `texcoord_is_2d`); `Program/program_util.h:15-48` (`fragment_program_texture_config`/`_state`).
- `rpcs3/Emu/RSX/Program/GLSLSnippets/RSXProg/RSXDefines2.glsl:4-12` (`sampler_info`),
  `RSXFragmentPrologue.glsl:154-181` (`remap_vector`), `RSXFragmentTextureOps.glsl:60-72`
  (depth decode).
- `rpcs3/Emu/RSX/VK/VKFragmentProgram.cpp:322-331,450,537-544` (texture-params buffer + sampler
  declarations).

**Inventory (generated, in-repo)** `[V]`
- `docs/research/metal4/inventory/out/clusters/texture.csv` (16 ids);
  `out/rsx_binding_slots.csv:6,12` (`fragment_texture_params_bind_slot=4`,
  `textures_first_bind_slot=12`); `out/rsx_value_space.csv:64-94` (`CellGcmTexture` formats),
  `:763-769` (dimensions); `out/SUMMARY.md:340-356` (texture cluster); `inventory/SCHEMA.md` §4
  (disposition vocab), §6 (LLD template).

**Sibling research docs (in-repo)** `[V]`
- `docs/research/metal4/10-rsx-native-metal4-architecture.md` §4.2 (binding/argument table), §4.6
  (shader/pipeline); `04-images-formats-samplers-rtt.md` §2.1 (#9/#14/#16/#17/#18/#37), §2.3 (format
  table), §2.4 (border), §2.8 (1024-sampler cap), R17-R19; `README.md` §3.4 (#114-#117), §3.5, §6
  (R17/R18/R19); `design/HLD-02-resource-model.md` §2.3/§2.5/§2.7;
  `design/HLD-05-shader-binding-contract.md` §2.4/§2.5/§2.6/§3.1/§3.2/§3.5; `design/LLD-vertex.md`
  §3.5/§3.8.

<!-- doc-end -->
