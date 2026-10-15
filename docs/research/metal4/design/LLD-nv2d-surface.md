# LLD — `nv2d-surface`: the NV3062 / NV309E 2D surface copy & swizzle engines

Backend: native Apple **Metal 4** (`MTL4*`) on Apple-silicon (M1+), macOS 26+ (`10` §10 `[V]`).
Cluster `nv2d-surface` (13 ids, `inventory/out/clusters/nv2d-surface.csv`). This LLD obeys
`HLD-02-resource-model.md` (guest bytes are canonical; surfaces/textures are projections; swizzle
is translated on import/export, `HLD-02` §2.1/§2.4) and `HLD-03-synchronization-model.md` (explicit
barriers; transfer edges via the `surface_access` classes, `HLD-03` §2.4).

Reading conventions: `[V]` verified against the tree (`file:line`), `[I]` inference, `[U]`
unverified. IDs are taken verbatim from the cluster shard; **no register is invented.**

---

## 1. Scope

`NV3062` (`NV30_CONTEXT_SURFACES_2D`, `gcm_enums.h:765-773`) and `NV309E`
(`NV30_CONTEXT_SURFACE_SWIZZLED`, `gcm_enums.h:775-780`) are the **destination-surface descriptor
objects** of the RSX blit engine. The engine's operation is selected by
`M_NV3089_SET_CONTEXT_SURFACE` (`blit_engine::context_surface ∈ {surface2d, swizzle2d}`,
`gcm_enums.h:1819-1832`, `rsx_methods.h:1132-1135`): `surface2d` writes a linear destination
described by the `NV3062` registers, `swizzle2d` writes a *swizzled* destination described by the
`NV309E` registers (`nv3089.cpp:97-141`).

Every `NV3062_*`/`NV309E_*` method is installed as `nullptr` (`rsx_methods.cpp:1526-1541`): this
cluster is **pure configuration**, read only by the trigger handlers — `nv3089::image_in`
(`nv3089.cpp:572`, in cluster `nv3089-image`) and `nv308a::color` (`nv308a.cpp:13`, in cluster
`nv308a-image`). So this LLD owns the *destination descriptor* of a guest-issued
surface-to-surface copy / format convert / swizzle convert, and the Metal 4 realization of that
operation.

**Non-goals.** The source descriptor and scaling/clipping/clipping (`nv3089-image`); the
CPU-to-surface `NV308A_COLOR` path (`nv308a-image`); the DMA linear copy (`LLD-nv0039-dma`); the
hazard placement of the transfer (RN1/`HLD-03`). The `notify` the guest can attach to a blit is the
sibling clusters' concern.

---

## 2. Register model

Two destination context objects. All plain value registers, latched by the guest and consumed at
trigger time; none has a dirty bit and none is event-driven.

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV3062_SET_OBJECT` | 0x6000 | none |  | na | none (inert subchannel handle) |
| `M_NV3062_SET_CONTEXT_DMA_NOTIFIES` | 0x6180 | none |  | na | none (inert notify context) |
| `M_NV3062_SET_CONTEXT_DMA_IMAGE_SOURCE` | 0x6184 | none |  | na | none (source DMA comes from NV3089) |
| `M_NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN` | 0x6188 | `CellGcmLocation output_dma` (`rsx_decode.h:3599`) |  | reshape | `MTL::Buffer` (guest window) surface2d destination base |
| `M_NV3062_SET_COLOR_FORMAT` | 0x6300 | `transfer_destination_format` (`rsx_decode.h:2875`) |  | reshape | `MTLPixelFormat` operand of the copy/convert encoder |
| `M_NV3062_SET_PITCH` | 0x6304 | `alignment[0:16]` / `pitch[16:16]` (`rsx_decode.h:441`) |  | reshape | destination rowBytes / host copy pitch |
| `M_NV3062_SET_OFFSET_SOURCE` | 0x6308 | none |  | na | none (source offset comes from NV3089) |
| `M_NV3062_SET_OFFSET_DESTIN` | 0x630c | `u32 output_offset` (`rsx_decode.h:1995`) |  | reshape | `MTL::Buffer` destination offset |
| `M_NV309E_SET_OBJECT` | 0x8000 | none |  | na | none (inert subchannel handle) |
| `M_NV309E_SET_CONTEXT_DMA_NOTIFIES` | 0x8180 | none |  | na | none (inert notify context) |
| `M_NV309E_SET_CONTEXT_DMA_IMAGE` | 0x8184 | `CellGcmLocation context_dma` (`rsx_decode.h:3622`) |  | reshape | `MTL::Buffer` (guest window) swizzle2d destination base |
| `M_NV309E_SET_FORMAT` | 0x8300 | `format[0:16]` / `sw_width_log2[16:8]` / `sw_height_log2[24:8]` (`rsx_decode.h:3774`) |  | emulate | `MTL4ComputeCommandEncoder` running the MSL swizzle kernel |
| `M_NV309E_SET_OFFSET` | 0x8304 | `u32 offset` (`rsx_decode.h:2018`) |  | reshape | `MTL::Buffer` swizzle destination offset |

**Decode facts.**
- *Unread registers.* The source location/offset of a blit are **not** taken from
  `NV3062_SET_CONTEXT_DMA_IMAGE_SOURCE` / `NV3062_SET_OFFSET_SOURCE`; they come from
  `blit_engine_input_location()` = `NV3089_SET_CONTEXT_DMA_IMAGE` (`rsx_methods.h:1127-1130`,
  `nv3089.cpp:209`) and `blit_engine_input_offset()` = `NV3089_IMAGE_IN_OFFSET`
  (`rsx_methods.h:1122-1125`, `nv3089.cpp:189`). Hence the two `na` rows. `[V]`
- *Destination formats.* `NV3062_SET_COLOR_FORMAT` and `NV309E_SET_FORMAT` both decode to
  `transfer_destination_format` (`{r5g6b5, a8r8g8b8, y32}`, `gcm_enums.h:1802-1817`); the source
  set is the 13-value `transfer_source_format` (`gcm_enums.h:1777-1800`). Unknown values are
  rejected, not coerced (`nv3089.cpp:80-85,257-276`). `[V]`
- *Swizzle geometry.* `sw_width_log2`/`sw_height_log2` are decoded but RPCS3 deliberately ignores
  them and rounds the output to `next_pow2(out_w)`/`next_pow2(out_h)` (`nv3089.cpp:481-520`),
  because the guest may request a size the hardware does not honour. `[V]`
- *Pitch zero.* `out_pitch == 0` means `out_bpp × out_w` (`nv3089.cpp:146-149`). `[V]`

**Lifetime.** Value registers; consumed once per trigger. No cross-trigger state.

---

## 3. Metal 4 mapping

**The shape change.** RSX names a *destination surface* by `(base, offset, pitch, format)` in guest
address space; Metal has no address-identity resource (`HLD-02` §2.0). Under `HLD-02` §2.1 the guest
byte image is one `StorageModeShared` `MTL::Buffer`; a surface/texture that lives in that range is a
separate `MTL::Texture` projection. Therefore this cluster's registers are the **operands of a
transfer encode**, not themselves a resource.

**There is no blit encoder** (`README` §4.4, `04` §2.6): copies fill/mipmap on
`MTL4ComputeCommandEncoder`. `vkCmdBlitImage` — the shape the VK backend uses for a scaled,
format-converting, possibly-swizzled image transfer — has no direct Metal equivalent
(`README` #122, `04` #28), so the operation is realized as a compute/meta pass, with a
`copyFromTexture:...toTexture:...` fast path only when the transfer is a same-format, same-sample,
pitch-preserving block copy.

### 3.1 `surface2d` path (NV3062)

| Sub-case | Condition | Metal 4 realization |
|---|---|---|
| block transfer, no convert, no clip/scale | `is_block_transfer` (`nv3089.cpp:105,156-159`) and `scale==±1` | `MTL4ComputeCommandEncoder.copyFromTexture:...toTexture:...` (or `copyFromBuffer` on the guest window when the region is not a known surface) |
| format convert, or clip, or scale | `need_convert`/`need_clip` (`nv3089.cpp:623-629`) | MSL compute kernel (bilinear when `transfer_interpolator::foh`, else point) writing the destination region/`MTL::Texture` |
| tiled destination | `get_tiled_memory_region` non-null (`nv3089.cpp:634-683`) | compute kernel de-tiles into the linear projection, then re-tiles on writeback (`HLD-02` §2.4) |

The register fields map to operands: `output_dma`+`output_offset` select the destination
`MTL::Buffer`/`MTL::Texture` and its offset; `output_pitch`/`alignment` are the copy stride;
`transfer_dest_fmt` selects the `MTLPixelFormat` (via the `HLD-02` §2.7 table). `[I]`

### 3.2 `swizzle2d` path (NV309E)

The destination is a **swizzled** region. Metal has no swizzled texture storage mode for either
mechanism (`HLD-02` §2.4), so the conversion is realized in an MSL compute kernel that ports
`convert_linear_swizzle` (`nv3089.cpp:525-531`) after the `next_pow2` pad (`:496-520`); the kernel
reads the intermediate linear image (`swizzled_copy_1`, `nv3089.cpp:425-471`) and writes the
swizzled `MTL::Buffer`/`MTL::Texture`. `HLD-02` §2.4's Option B ("translate on import/export") is
the pinned decision this obeys. `NV309E_SET_FORMAT.format` selects the `MTLPixelFormat`;
`sw_width_log2`/`sw_height_log2` are **not** used to size the kernel (preserve `next_pow2`,
`nv3089.cpp:481-499`); `context_dma`+`offset` select the destination. `[I]`

### 3.3 Shared with the texture-cache flush path

The swizzle converter here and the `texture_cache` flush converter (`HLD-02` §2.4 carve-out;
`VKTextureCache.h:299` in the VK frame) are the *same* transform. They should be **one MSL kernel**
(and one scalar CPU reference for H0) so `11` H1/H5 can test it once. `[I]`

**Objects used.** `MTL4ComputeCommandEncoder`; `MTL::Texture`/`MTL::Buffer` (destination
projection); the MSL swizzle/convert kernels; the `HLD-03` §3.4 barrier chokepoint for the
transfer's RAW/WAR edges (`surface_access::transfer_read`/`transfer_write`, `HLD-03` §2.4). No new
object class is introduced by this cluster.

---

## 4. Impossible / not-recommended

- **Impossible: a Metal swizzled destination texture.** There is no swizzled/TBD storage mode; the
  conversion *must* be a compute kernel (`HLD-02` §2.4). This is why `NV309E_SET_FORMAT` is
  `emulate` and rated 4.
- **Not-recommended: reviving `vkCmdBlitImage`'s shape.** A scaled, format-reinterpreting blit with
  no blit encoder is a compute/graphics meta pass (`README` #122, `04` #28); treating it as a
  "copy" would hide the format conversion and lose the swizzle.
- **Not-recommended: Metal heap aliasing** for the destination region. Guest alias granularity and
  `old_contents` inheritance do not match heap offsets (`HLD-02` §2.5); the destination is a copy,
  not an alias.
- **Fallback actually chosen for the swizzle path:** the compute kernel (§3.2), not a buffer-only
  shader path and not "keep guest layout" (`HLD-02` §2.4 Option A rejected).

---

## 5. Invariants & failure modes

**Invariants.**
1. After the trigger, the destination region byte-matches the reference conversion described by the
   registers — guest-observable and mandatory (`07`/`README` §4 layer A).
2. Swizzle geometry uses `next_pow2(out_w)`/`next_pow2(out_h)`, never the register's `*_log2`
   fields (`nv3089.cpp:498-499`).
3. Only the decoded format sets are legal: 3 destination values, 13 source values
   (`gcm_enums.h:1777-1817`); an unknown value aborts (`nv3089.cpp:257-276`).
4. `out_pitch == 0` ⇒ `out_bpp × out_w` (`nv3089.cpp:146-149`); `in_pitch`/`out_pitch` are never
   assumed to equal `bpp × width`.
5. The block-transfer fast path is taken only when both pitches are exact and `in_bpp == out_bpp`
   (`nv3089.cpp:156-159`); otherwise the converting path runs.

**Failure modes.**
- **F1 wrong swizzle:** sampling the destination yields corrupted texels (the `README` R25 area).
  Detection: H1 (kernel compile) + H5 (region diff).
- **F2 format-convert precision:** `r5g6b5` quantization must match the reference rounding
  (`nv3089.cpp:143-144` shows the 5/6/5 path). Detection: H5 on a small 16-bit target.
- **F3 source/destination mis-binding:** the `na` rows exist precisely to prevent binding the
  source from `NV3062`; using `NV3062_SET_CONTEXT_DMA_IMAGE_SOURCE` silently reads the wrong range.
  Detection: H3 state oracle + H2 decision trace.
- **F4 tiled destination:** de-tile over-reads whole `64 × tile->pitch` units
  (`HLD-02` F9); align the copy, not the resource.
- **F5 transfer hazard:** the blit touches a surface a draw also uses; the `surface_store`
  `memory_barrier(transfer_*)` must fire (`HLD-03` §2.4).

---

## 6. Open questions

- **Q1 `[I]` Is the surface2d destination ever a live render target?** If yes (RTT written by a
  blit), the fast path targets an existing `MTL::Texture` and the copy is `copyFromTexture`; if no,
  it is always a compute kernel over the guest-window `MTL::Buffer`. This decides whether the
  descriptor needs a surface lookup at all. *Experiment:* grep replays for blits whose
  `output_dma + output_offset` matches a live colour surface.
- **Q2 `[U]` GPU vs CPU for a guest-read-back blit.** If the destination is immediately read by the
  guest (the common `scaled_image_from_memory` direction, `nv3089.cpp:592-598` is the opposite), a
  GPU convert adds a CPU-visibility round-trip; a CPU convert may win. Same shape as
  `LLD-nv0039-dma` Q1.
- **Q3 `[I]` One swizzle kernel or two?** Share the converter with the `texture_cache` flush path
  (`HLD-02` §2.4) as one MSL kernel keyed by element size (1/2/4, `nv3089.cpp:522-533`).
- **Q4 `[U]` `y32` destination.** `transfer_destination_format::y32` is accepted by the decoder but
  the CPU path only handles `a8r8g8b8`/`r5g6b5` (`nv3089.cpp:257-261`); confirm whether `y32` is
  ever issued before designing its `MTLPixelFormat`.

---

## 7. Provenance

**This tree `[V]` (paths under `rpcs3/Emu/RSX/`).**
`gcm_enums.h:765-780` (`NV3062`/`NV309E` method enums), `:1777-1832` (`transfer_source_format`,
`transfer_destination_format`, `context_surface`); `rsx_decode.h:441` (`NV3062_SET_PITCH`),
`:1995` (`NV3062_SET_OFFSET_DESTIN`), `:2018` (`NV309E_SET_OFFSET`), `:2875`
(`NV3062_SET_COLOR_FORMAT`), `:3599` (`NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN`), `:3622`
(`NV309E_SET_CONTEXT_DMA_IMAGE`), `:3774` (`NV309E_SET_FORMAT`);
`rsx_methods.h:1122-1195` (`blit_engine_*` accessors); `rsx_methods.cpp:1526-1541` (all methods
`nullptr`); `NV47/HW/nv3089.cpp:16-319` (`decode_transfer_registers`; `context_surface` switch
`:97-141`; format validation `:257-276`), `:321-423` (`linear_copy`), `:425-534`
(`swizzled_copy_1`/`swizzled_copy_2`, `convert_linear_swizzle` `:525-531`), `:572-684`
(`image_in`; `scaled_image_from_memory` fast path `:592-598`; `get_tiled_memory_region`
`:634-683`); `NV47/HW/nv308a.cpp:13-163` (`color::impl`; the 5/6/5 reference converter
`:129-140`).

**Inventory (facts).** `inventory/out/clusters/nv2d-surface.csv` (13 ids; no bound rows);
`inventory/out/SUMMARY.md:171-187,475-478`; `inventory/SCHEMA.md` §4 (disposition vocab) §5 (CSV
columns) §6 (LLD template).

**Sibling scopes `[V]`.** `HLD-02-resource-model.md` §2.0 (shape change), §2.1 (guest window),
§2.3 (pitch), §2.4 (tiling/swizzle Option B; `nv3089.cpp:525-531` carve-out), §2.5 (no heap
aliasing), §2.7 (format table), F9 (tile over-read); `HLD-03-synchronization-model.md` §2.4
(transfer edges, chokepoint §3.4); `04-images-formats-samplers-rtt.md` §2.6 (copies on the compute
encoder; `vkCmdBlitImage` gap); `10-rsx-native-metal4-architecture.md` §3.1 (blit engine semantics,
not `vkCmdBlitImage`), §5 (blit-engine VK plumbing deleted); `README.md` §4 shift 4 (no blit
encoder), §6 R25.

<!-- doc-end -->
