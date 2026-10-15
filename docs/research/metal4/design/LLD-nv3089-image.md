# LLD — `nv3089-image`: the NV3089 scaled image-from-memory blit engine

Backend: native Apple **Metal 4** (`MTL4*`) on Apple-silicon (M1+), macOS 26+ (`10` §10 `[V]`).
Cluster `nv3089-image` (21 ids, `inventory/out/clusters/nv3089-image.csv`). This LLD obeys
`HLD-02-resource-model.md` (guest memory is one `StorageModeShared` `MTL::Buffer`; guest bytes are
canonical; surfaces/textures are projections kept coherent by copies, `HLD-02` §2.1/§2.4) and
`HLD-03-synchronization-model.md` (no driver hazard tracking; the transfer's RAW/WAR edges are
`rsx::surface_access::transfer_read`/`transfer_write` routed through the `mtl::emit_barrier`
chokepoint, `HLD-03` §2.4/§3.4).

Reading conventions: `[V]` verified against the tree (`file:line`), `[I]` inference, `[U]`
unverified. IDs are taken verbatim from the cluster shard; **no register is invented.**

---

## 1. Scope

`NV3089` is the RSX **`NV30_SCALED_IMAGE_FROM_MEMORY`** engine (`gcm_enums.h:800-821`): a
guest-issued blit that reads a *source* 2D image from guest memory and writes a *destination* 2D
image, with optional scaling, clipping, mirror, source→destination format conversion, and a swizzled
destination. It is triggered by exactly one method, `M_NV3089_IMAGE_IN` (handler `nv3089::image_in`,
`nv3089.cpp:572`; registered `rsx_methods.cpp:1725-1726`); the other twenty `NV3089_*` methods are
pure configuration installed as `nullptr` (`rsx_methods.cpp:1562-1582`).

The engine is the **DMA-source** half of the RSX blit pair. Its sibling `NV308A`
(`NV30_IMAGE_FROM_CPU`, `gcm_enums.h:782-798`) takes its source **inline from the FIFO** and is
`LLD-nv308a-image`; `NV3089` takes its source from a guest address and is this LLD. Both write
through the **same destination descriptor objects** — `NV3062` (linear 2D) and `NV309E` (swizzled 2D)
— owned by `LLD-nv2d-surface`. §3.6 states the division of labour explicitly.

This is a **guest-observable memory** operation: after `IMAGE_IN` returns, the destination range must
byte-match the reference conversion (`07`/`README` §4, layer A). Correctness is mandatory, not
best-effort.

**Non-goals.** The destination descriptor objects `NV3062`/`NV309E` (`LLD-nv2d-surface`); the inline
CPU upload `M_NV308A_COLOR` (`LLD-nv308a-image`); the `NV0039` buffer DMA (`LLD-nv0039-dma`); hazard
placement and the barrier chokepoint (RN1/`HLD-03`); the FIFO/notify protocol and ZCULL report
interface (`HLD-03`, `sync-flip`).

---

## 2. Register model

One trigger, five scalar geometry pairs, a format/operation/surface selector, and the source
descriptor. All plain **value** registers: none has a dirty bit, none is event-driven, and the
engine's semantics live entirely in the trigger handler. Only `NV3089_IMAGE_IN` is bound; the other
twenty are `nullptr` (`rsx_methods.cpp:1562-1582`).

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV3089_SET_OBJECT` | 0xc000 | none |  | na | none (inert subchannel handle) |
| `M_NV3089_SET_CONTEXT_DMA_NOTIFIES` | 0xc180 | none |  | na | none (inert notify context) |
| `M_NV3089_SET_CONTEXT_DMA_IMAGE` | 0xc184 | `CellGcmLocation context_dma` |  | reshape | `mtl_guest_window` address resolution |
| `M_NV3089_SET_CONTEXT_PATTERN` | 0xc188 | none |  | moot | none (pattern-blit source unreachable) |
| `M_NV3089_SET_CONTEXT_ROP` | 0xc18c | none |  | moot | none (ROP op unreachable) |
| `M_NV3089_SET_CONTEXT_BETA1` | 0xc190 | none |  | moot | none (blend op unreachable) |
| `M_NV3089_SET_CONTEXT_BETA4` | 0xc194 | none |  | moot | none (blend op unreachable) |
| `M_NV3089_SET_CONTEXT_SURFACE` | 0xc198 | `ctx_surface` (enum) |  | reshape | `MTL4ComputeCommandEncoder` path select |
| `M_NV3089_SET_COLOR_CONVERSION` | 0xc2fc | none |  | emulate | `MTL4ComputeCommandEncoder` uniform `MTL::Buffer` |
| `M_NV3089_SET_COLOR_FORMAT` | 0xc300 | `transfer_source_fmt` (enum) |  | reshape | `MTLPixelFormat` + compute decode branch |
| `M_NV3089_SET_OPERATION` | 0xc304 | `transfer_op` (enum) |  | reshape | host op select before `MTL4ComputeCommandEncoder` |
| `M_NV3089_CLIP_POINT` | 0xc308 | `clip_x[0:16]` / `clip_y[16:16]` |  | reshape | `MTL4ComputeCommandEncoder` `dispatchThreads` origin |
| `M_NV3089_CLIP_SIZE` | 0xc30c | `clip_width[0:16]` / `clip_height[16:16]` |  | reshape | `MTL4ComputeCommandEncoder` `dispatchThreads` grid |
| `M_NV3089_IMAGE_OUT_POINT` | 0xc310 | `x[0:16]` / `y[16:16]` |  | reshape | `MTL4ComputeCommandEncoder` dst region origin |
| `M_NV3089_IMAGE_OUT_SIZE` | 0xc314 | `width[0:16]` / `height[16:16]` |  | reshape | `MTL4ComputeCommandEncoder` dst region extent |
| `M_NV3089_DS_DX` | 0xc318 | `ds_dx` (fxp 11.20) |  | reshape | `MTL::SamplerState` filter + UV-grid uniform |
| `M_NV3089_DT_DY` | 0xc31c | `dt_dy` (fxp 11.20) |  | reshape | `MTL::SamplerState` filter + UV-grid uniform |
| `M_NV3089_IMAGE_IN_SIZE` | 0xc400 | `width[0:16]` / `height[16:16]` |  | reshape | `MTL4ComputeCommandEncoder` src region extent |
| `M_NV3089_IMAGE_IN_FORMAT` | 0xc404 | `format[0:16]` + `transfer_origin_raw[16:8]` + `transfer_interpolator_raw[24:8]` |  | reshape | `MTL::Texture` src region + `MTL::Buffer` pitch uniform |
| `M_NV3089_IMAGE_IN_OFFSET` | 0xc408 | `u32 input_offset` |  | reshape | `mtl_guest_window` byte offset |
| `M_NV3089_IMAGE_IN` | 0xc40c | `x[0:16]` / `y[16:16]` (16-bit fxp) | `nv3089::image_in` | emulate | `MTL4ComputeCommandEncoder` `dispatchThreads` or `copyFromTexture` |

**Decode facts.**

- *Operation selector.* `SET_OPERATION` decodes to the 6-value `transfer_operation`
  (`rsx_decode.h:2816-2819`; `gcm_enums.h:1759-1775`). RPCS3 accepts only `srccopy`; anything else
  logs, calls `recover_fifo()` and drops the blit (`nv3089.cpp:73-78`). Consequently the
  `SET_CONTEXT_ROP`/`SET_CONTEXT_BETA1`/`SET_CONTEXT_BETA4`/`SET_CONTEXT_PATTERN` configuration —
  consumed only by `rop_and`/`blend_and`/`blend_premult` — is unreachable. `[V]`
- *Source format.* `SET_COLOR_FORMAT` decodes to the 13-value `transfer_source_format`
  (`rsx_decode.h:2839-2842`; `gcm_enums.h:1777-1800`); a null/unknown value drops the blit
  (`nv3089.cpp:80-85`), and only `r5g6b5` and `a8r8g8b8` (with `x8r8g8b8` promoted to `a8r8g8b8`)
  reach the pixel loop (`nv3089.cpp:263-276`). `[V]`
- *The low 16 bits of `IMAGE_IN_FORMAT` are the source pitch, not a pixel format.*
  `blit_engine_input_pitch()` reads `decode<NV3089_IMAGE_IN_FORMAT>().format()`
  (`rsx_methods.h:1090-1093`); bits 16-23 are `transfer_origin` and 24-31 `transfer_interpolator`
  (`rsx_decode.h:3744-3745,3755-3763`). `[V]`
- *Geometry.* `CLIP_POINT`/`CLIP_SIZE` are the clip rect (`rsx_decode.h:272-325`),
  `IMAGE_OUT_POINT`/`IMAGE_OUT_SIZE` the output rect (`:328-410`), `IMAGE_IN_SIZE` the source rect
  (`:413-438`). The clip is clamped to the output extent (`clip_w = min(clip_width, out_w)`,
  `nv3089.cpp:39-40`); a zero clip dimension NOPs the operation (`:43-47`) and a zero input
  dimension throws (`:49-53`). `[V]`
- *Scale.* `DS_DX`/`DT_DY` decode to `1 / fxp<11,20>` (signed 11.20 fixed point); a zero register
  yields `0`, reported at trigger time (`rsx_decode.h:2051-2063,2083-2095`). A negative scale selects
  the mirror transform (`nv3089.cpp:612-614`). `[V]`
- *Source address.* `SET_CONTEXT_DMA_IMAGE` decodes to a `CellGcmLocation`
  (`rsx_decode.h:3586-3589`) selecting the main vs local guest window; `IMAGE_IN_OFFSET` is a raw
  `u32` byte offset (`rsx_decode.h:1982-1985`). `[V]`
- *Destination select.* `SET_CONTEXT_SURFACE` decodes to `context_surface ∈ {surface2d, swizzle2d}`
  (`rsx_decode.h:2862-2865`; `gcm_enums.h:1819-1832`) and selects which destination descriptor
  (`NV3062` vs `NV309E`) is read (`nv3089.cpp:97-141`). `[V]`

**Unread registers.** `SET_OBJECT` and `SET_CONTEXT_DMA_NOTIFIES` are dispatched to `nullptr` and
never read (`rsx_methods.cpp:1562-1563`); the latter is *not* the blit's notify sink.
`SET_CONTEXT_PATTERN`/`ROP`/`BETA1`/`BETA4` are never read (`rsx_methods.cpp:1565-1568`) because only
`srccopy` is accepted. `SET_COLOR_CONVERSION` has no decoder (`has_decoder=0`) and no accessor, so it
is never read. `[V]`

**Lifetime.** Value registers, latched by the guest and consumed once per `IMAGE_IN` trigger; no
cross-trigger state. The destination descriptor registers live in the sibling clusters and are read
through the shared `blit_engine_output_*`/`blit_engine_nv309E_*` accessors
(`rsx_methods.h:1137-1175`). `[V]`

---

## 3. Metal 4 mapping

**The shape change.** RSX names a *source* and a *destination* as byte ranges in the unified guest
address space; Metal has no address-identity resource (`HLD-02` §2.0). Under `HLD-02` §2.1 the whole
guest window is one `StorageModeShared` `MTL::Buffer` created with `newBufferWithBytesNoCopy:`; a
`MTL::Texture` projection exists only for a range that is a live surface/texture. The engine's
registers are therefore the **operands of a transfer encode**, not resources. There is **no blit
encoder** (`README` §4.4, `04` §2.6): copies ride on `MTL4ComputeCommandEncoder`. `vkCmdBlitImage` —
the VK backend's shape for a scaled, format-reinterpreting image transfer — has no direct Metal
equivalent (`README` #122, `04` #28), so the general case is a compute/meta pass. The trigger is
`emulate` (host selects the realization); the operands are `reshape` (they become encoder operands).

### 3.1 Source descriptor and address resolution

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV3089_SET_CONTEXT_DMA_IMAGE` | 0xc184 | `CellGcmLocation context_dma` |  | reshape | `mtl_guest_window` address resolution |
| `M_NV3089_IMAGE_IN_OFFSET` | 0xc408 | `u32 input_offset` |  | reshape | `mtl_guest_window` byte offset |
| `M_NV3089_IMAGE_IN_FORMAT` | 0xc404 | `format[0:16]` + `transfer_origin_raw[16:8]` + `transfer_interpolator_raw[24:8]` |  | reshape | `MTL::Texture` src region + `MTL::Buffer` pitch uniform |
| `M_NV3089_IMAGE_IN_SIZE` | 0xc400 | `width[0:16]` / `height[16:16]` |  | reshape | `MTL4ComputeCommandEncoder` src region extent |

The source base is `src_dma` + `src_offset` (`nv3089.cpp:87-88`), resolved with the subpixel origin
and pitch into a byte address and offset:
`src_address = get_address(src_offset, src_dma, data_length)` (`:209,233`),
`in_offset = in_x * in_bpp + in_pitch * in_y` (`:189`). Under Metal this is a **byte offset into the
guest-window `MTL::Buffer`** (`HLD-02` §3.1 `mtl_guest_window`), never a new allocation. When the
source range is a live texture/surface, the shard's target is a `MTL::Texture` src region plus a
`MTL::Buffer` pitch uniform — `IMAGE_IN_FORMAT`'s low 16 bits are a **pitch uniform**, not a format
(`rsx_methods.h:1090-1093`). `IMAGE_IN_SIZE`'s zero case throws before any encode
(`nv3089.cpp:49-53`); `src_line_length` is derived at `:192`. `[I]`

### 3.2 Output geometry, clip and scale

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV3089_IMAGE_OUT_POINT` | 0xc310 | `x[0:16]` / `y[16:16]` |  | reshape | `MTL4ComputeCommandEncoder` dst region origin |
| `M_NV3089_IMAGE_OUT_SIZE` | 0xc314 | `width[0:16]` / `height[16:16]` |  | reshape | `MTL4ComputeCommandEncoder` dst region extent |
| `M_NV3089_CLIP_POINT` | 0xc308 | `clip_x[0:16]` / `clip_y[16:16]` |  | reshape | `MTL4ComputeCommandEncoder` `dispatchThreads` origin |
| `M_NV3089_CLIP_SIZE` | 0xc30c | `clip_width[0:16]` / `clip_height[16:16]` |  | reshape | `MTL4ComputeCommandEncoder` `dispatchThreads` grid |
| `M_NV3089_DS_DX` | 0xc318 | `ds_dx` (fxp 11.20) |  | reshape | `MTL::SamplerState` filter + UV-grid uniform |
| `M_NV3089_DT_DY` | 0xc31c | `dt_dy` (fxp 11.20) |  | reshape | `MTL::SamplerState` filter + UV-grid uniform |

The destination byte origin is `out_x * out_bpp + out_pitch * out_y` (`nv3089.cpp:190`); the clip rect
is clamped to the output extent and a zero dimension NOPs (`:39-47`); `clip_x`/`clip_y` are forced
back to `0` when the rect overruns the output surface (`:59-60`). The scale selects the sampler
filter — `transfer_interpolator::foh` → bilinear, else point (`nv3089.cpp:631`) — and negative scale
selects the mirror transform (`:612-614`). On the compute path the clip origin becomes the
`dispatchThreads` thread origin and the clip extent the grid; the reciprocal fixed-point scale
becomes a UV-grid uniform. `[I]`

### 3.3 Format conversion and operation

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV3089_SET_COLOR_FORMAT` | 0xc300 | `transfer_source_fmt` (enum) |  | reshape | `MTLPixelFormat` + compute decode branch |
| `M_NV3089_SET_OPERATION` | 0xc304 | `transfer_op` (enum) |  | reshape | host op select before `MTL4ComputeCommandEncoder` |
| `M_NV3089_SET_COLOR_CONVERSION` | 0xc2fc | none |  | emulate | `MTL4ComputeCommandEncoder` uniform `MTL::Buffer` |

`SET_OPERATION` must be `srccopy` or the blit is dropped (`nv3089.cpp:73-78`); `SET_COLOR_FORMAT`
gates the pixel loop to `r5g6b5`/`a8r8g8b8` (`:263-276`). `need_convert` is set when
`out_format != in_format` or the scale is not ±1 (`nv3089.cpp:629`); the CPU path runs ffmpeg
`convert_scale_image` (`:413-422`) and the `r5g6b5` rounding must match the shared reference at
`nv308a.cpp:129-140`. `SET_COLOR_CONVERSION` (a YUV→RGB matrix) has no decoder and is never read; it
is required only if YUV source formats are ever enabled, which is why its disposition is `emulate`
(compute uniform `MTL::Buffer`) and `[U]`. `[V]`/`[I]`

### 3.4 Destination select — `SET_CONTEXT_SURFACE`

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV3089_SET_CONTEXT_SURFACE` | 0xc198 | `ctx_surface` (enum) |  | reshape | `MTL4ComputeCommandEncoder` path select |

`surface2d` reads the linear `NV3062` descriptor; `swizzle2d` reads the swizzled `NV309E` descriptor
(`nv3089.cpp:97-141`). Under Metal the selector chooses the encode path: `surface2d` → block copy or
convert kernel; `swizzle2d` → the swizzle kernel (`swizzled_copy_1`+`swizzled_copy_2`,
`nv3089.cpp:425-534`). The destination operands themselves belong to `LLD-nv2d-surface` §3; this LLD
owns only the selection and the source/geometry that feed the encode. `[I]`

### 3.5 The trigger `IMAGE_IN` — realization selection

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV3089_IMAGE_IN` | 0xc40c | `x[0:16]` / `y[16:16]` (16-bit fxp) | `nv3089::image_in` | emulate | `MTL4ComputeCommandEncoder` `dispatchThreads` or `copyFromTexture` |

The trigger is `emulate`: the host selects among three realizations, all writing the **same guest
bytes** (correctness is the invariant, §5):

1. **Same-format block transfer.** When `is_block_transfer` holds (`fcmp(scale, ±1)` and
   `in_bpp == out_bpp`, `nv3089.cpp:105,156-159`) and the row is contiguous
   (`clip_h == 1 || (in_pitch == out_pitch && src_line_length == in_pitch)`, `:204`), a plain block
   copy: `MTL4ComputeCommandEncoder.copyFromBuffer:...toBuffer:...` over the guest-window buffer (or
   `copyFromTexture` when both ranges are live textures), or the existing CPU `memcpy`/`memmove`
   (`linear_copy`, `nv3089.cpp:381,405`). `[I]`
2. **Convert / scale / clip / swizzle.** A `MTL4ComputeCommandEncoder` running an MSL kernel:
   `dispatchThreads` grid = clip extent, point or bilinear per interpolator; the swizzle destination
   runs the `convert_linear_swizzle` kernel after the `next_pow2` pad (`nv3089.cpp:498-533`). This is
   the general path and the one the shard's `metal4_target` names throughout. `[I]`
3. **CPU path.** The existing host code, chosen when the destination is immediately guest-read, when
   the region is small, or when a tile descriptor forces a host de-tile/re-tile
   (`nv3089.cpp:633-683`); gated by `g_cfg.video.force_cpu_blit_processing` (`:592`). `[I]`

The existing backend hook `scaled_image_from_memory` (`RSXThread.h:450`; `VKGSRender.cpp:2870-2887`)
**is** the "HW-accelerated blit" seam: `image_in` enters it when either DMA is
`CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER` (`nv3089.cpp:592-598`). The Metal backend implements it as
the compute realization (case 2) and returns `false` to fall through to the CPU path (case 3). This
is the single most important design decision in the cluster (§6 Q1). `[V]`/`[I]`

**Ordering (the part that is Metal, not memcpy).** The handler brackets the transfer with the
existing RPCS3 hazards whose *meaning* changes under Metal 4 (`HLD-03` §2.3/§2.4):

- `invalidate_fragment_program(dst_dma, dst_offset, data_length)` (`nv3089.cpp:216,240`) — a write
  over the live fragment ucode forces a reload (`RSXThread.cpp:2696-2721`); under Metal this is the
  same texture/surface-cache invalidation seam as `LLD-nv0039-dma`/`LLD-nv308a-image`.
- `read_barrier(src_address, data_length, …)` (`nv3089.cpp:218,241`) — a **GPU→CPU** edge (the CPU
  must not read a range the GPU may still be writing); the producer side of `HLD-03` §2.3 Edge B,
  which needs a visibility point (`MTL4CommitFeedback`/`MTLSharedEvent`), not a barrier alone.
- The transfer's own read/write of a surface is `transfer_read`/`transfer_write` (`HLD-03` §2.4) and
  is emitted through `mtl::emit_barrier` (`HLD-03` §3.4) — never inline.
- `reservation_lock` (`nv3089.cpp:588-590`) and `mm_flush` (`:600-606`) are backend-agnostic host
  memory-manager/exclusion mechanics and survive unchanged (`11` §3.4).

**Objects used.** `MTL4ComputeCommandEncoder`; the guest-window `MTL::Buffer` (`HLD-02` §2.1); a
`MTL::Texture` projection and `MTL::SamplerState` when a range is a live surface/texture; the MSL
convert/scale/swizzle kernels; the `mtl::emit_barrier` chokepoint. No new object class is introduced
by this cluster.

### 3.6 Division of labour among the three engines

| engine | source | destination descriptor | trigger | LLD |
|---|---|---|---|---|
| `NV3089` | guest memory (DMA) | `NV3062` (linear) / `NV309E` (swizzled) | `M_NV3089_IMAGE_IN` | this doc |
| `NV308A` | inline FIFO stream | `NV3062` (linear) | `M_NV308A_COLOR` | `LLD-nv308a-image` |
| `NV0039` | guest memory (DMA) | none (buffer-to-buffer) | `M_NV0039_BUFFER_NOTIFY` | `LLD-nv0039-dma` |

`NV3062`/`NV309E` are *pure configuration* read only by the two triggers (`LLD-nv2d-surface` §2).
`NV3089` owns the source descriptor, geometry, operation and trigger for the DMA blit; `NV308A` owns
the inline-upload trigger; `NV0039` is the buffer-only DMA with no surface descriptor. A frame can
issue all three; they share the guest-window `MTL::Buffer` and the barrier chokepoint, and must be
ordered against each other by the surface/store hazard tracking (`HLD-03` §2.4). The convert/scale
and swizzle kernels should be **one MSL kernel shared** across `NV3089`, `NV308A`, `NV309E`
(`LLD-nv2d-surface` §3.3) and the `texture_cache` flush path (`HLD-02` §2.4), tested once by `11`
H1/H5. `[I]`

---

## 4. Impossible / not-recommended

- **Impossible: a scaled, format-reinterpreting blit as a single Metal copy.** There is no blit
  encoder (`README` §4.4) and `vkCmdBlitImage` has no direct equivalent (`README` #122, `04` #28).
  The **fallback actually chosen** is the compute kernel (§3.5 case 2), with the block-copy fast path
  (§3.5 case 1) when the transfer is provably same-format and contiguous.
- **Not-recommended: reviving `vkCmdBlitImage`'s shape.** A scaled, format-reinterpreting blit
  treated as a "copy" hides the conversion and loses the swizzle (`LLD-nv2d-surface` §4). Same
  reasoning here.
- **Not-recommended: `MTL4VisibilityOptionResourceAlias` for overlapping source/destination in one
  buffer.** The overlap is a byte-range alias inside one buffer, handled by the CPU path's
  `memmove`/temp-buffer (`linear_copy`, `nv3089.cpp:338-343,381`), consistent with `HLD-02` §2.5's
  rejection of heap aliasing for guest aliasing.
- **Not-recommended: taking the source from `NV3062_SET_CONTEXT_DMA_IMAGE_SOURCE` /
  `NV3062_SET_OFFSET_SOURCE`.** Those registers are never read (`LLD-nv2d-surface` §2); the source is
  `NV3089_SET_CONTEXT_DMA_IMAGE` + `NV3089_IMAGE_IN_OFFSET` (`nv3089.cpp:87-88`). Binding the wrong
  range silently reads garbage.
- **Not-recommended: making the CPU path the only realization.** It is correct and cheap for small
  transfers, but leaves the GPU idle on large converts; the compute path is the design target and the
  CPU path the fallback (§3.5).
- **Not a mapping failure:** `SET_COLOR_CONVERSION`/YUV is `emulate` (a compute uniform), not
  `impossible` — the semantics are reachable in MSL/host code.
- **Unsupported today:** the `rop_and`/`blend_and`/`blend_premult` operations and the
  `a1r5g5b5`/`x1r5g5b5`/YUV source formats are dropped/aborted by RPCS3
  (`nv3089.cpp:73-85,257-276`), so their Metal realization is moot until the emulation is extended.

---

## 5. Invariants & failure modes

**Invariants.**

1. After `IMAGE_IN`, the destination region byte-matches the reference conversion; guest-observable
   and mandatory (`07`/`README` §4 layer A).
2. Only `srccopy` is legal; anything else drops the blit and calls `recover_fifo()`
   (`nv3089.cpp:73-78`).
3. Only `r5g6b5`/`a8r8g8b8` destinations and `r5g6b5`/`a8r8g8b8`/`x8r8g8b8` sources reach the pixel
   loop (`nv3089.cpp:257-276`); the `r5g6b5` rounding must match `nv308a.cpp:129-140`.
4. `out_pitch == 0 ⇒ out_bpp × out_w`; `in_pitch == 0 ⇒ in_bpp × in_w` (`nv3089.cpp:146-154`);
   neither pitch is ever assumed equal to `bpp × width`.
5. `IMAGE_IN_FORMAT`'s low 16 bits are the source pitch, never a pixel format
   (`rsx_methods.h:1090-1093`).
6. Swizzle geometry uses `next_pow2(out_w)`/`next_pow2(out_h)`, never the `NV309E` `*_log2` fields
   (`nv3089.cpp:498-499`).
7. The block-transfer fast path is taken only when both pitches are exact and `in_bpp == out_bpp`
   (`nv3089.cpp:156-159,204`).
8. A GPU reader of the destination observes the transfer (the `HLD-03` §2.4 `transfer_write`
   producer edge); a CPU reader is preceded by a `read_barrier` GPU→CPU visibility point
   (`nv3089.cpp:218,241`; `HLD-03` §2.3 Edge B).
9. The transfer is bracketed by `reservation_lock` over both ranges (`nv3089.cpp:588-590`).

**Failure modes.**

- **F1 wrong swizzle:** sampling the destination yields corrupted texels (the `README` R25 area).
  Detection: `11` H1 (kernel compile) + H5 (region diff).
- **F2 format-convert precision:** `r5g6b5` quantization must match the reference rounding
  (`nv308a.cpp:129-140`). Detection: H5 on a small 16-bit target.
- **F3 missing transfer edge:** the next draw/sample observes stale destination bytes — silent wrong
  content, not a crash. Detection: H5 + H3.
- **F4 pitch mis-derivation:** treating `in_pitch`/`out_pitch` as `bpp × width` produces a row shear.
  Detection: H5.
- **F5 tiled destination over-read:** de-tiling reads whole `64 × tile->pitch` units (`HLD-02` F9);
  align the copy, not the resource.
- **F6 source/destination mis-binding:** reading the source from the inert `NV3062` source registers
  silently reads the wrong range. Detection: H2 decision trace + H3 state oracle.
- **F7 reservation exclusion removed:** a guest remap during the copy reintroduces the existing race
  (`nv3089.cpp:588-590`).
- **F8 compute path on a CPU-observed destination:** a GPU convert adds two visibility round-trips on
  data that need never leave the CPU; if the destination is immediately guest-read, the CPU path
  wins (`LLD-nv0039-dma` §4).

---

## 6. Open questions

- **Q1 `[U]` Per-transfer CPU vs GPU selection.** The `emulate` trigger requires a rule; the
  candidate is "compute when the destination is GPU-only and the transfer is large, CPU otherwise".
  *Experiment:* histogram the replay corpus by `(src_dma, dst_dma, size, need_convert, swizzle)` and
  measure CPU vs compute cost per class; also test `force_cpu_blit_processing` on and off
  (`nv3089.cpp:592`). Decides §3.5.
- **Q2 `[I]` Is the `surface2d` destination ever a live render target?** If yes the fast path targets
  an existing `MTL::Texture` and is `copyFromTexture`; if no it is always a compute kernel over the
  guest-window buffer. (Shared with `LLD-nv2d-surface` Q1.)
- **Q3 `[U]` Does the compute path pay off for the *block-transfer* case** versus
  `copyFromBuffer`/`copyFromTexture` versus a CPU `memcpy`? The block case may be CPU-cheaper.
- **Q4 `[U]` Is `SET_COLOR_CONVERSION` (YUV→RGB) ever issued?** No decoder, no accessor; the
  disposition is `[U]`. *Experiment:* grep replays for a non-zero `NV3089_SET_COLOR_CONVERSION`
  followed by a YUV-class `SET_COLOR_FORMAT`.
- **Q5 `[I]` One convert/scale/swizzle MSL kernel or several?** Share with `LLD-nv2d-surface` §3.3,
  `LLD-nv308a-image` Q6 and the `texture_cache` flush converter, keyed by element size
  (1/2/4, `nv3089.cpp:522-533`).
- **Q6 `[U]` `y32` destination.** The decoder accepts `transfer_destination_format::y32`
  (`gcm_enums.h:1802-1817`) but the CPU path only handles `a8r8g8b8`/`r5g6b5`
  (`nv3089.cpp:257-261`); confirm whether `y32` is ever issued before designing its `MTLPixelFormat`.
- **Q7 `[U]` CPU→GPU visibility without a queue drain.** Identical to `LLD-nv0039-dma` Q1: can a CPU
  store into the guest-window shared buffer be ordered against already-committed encoders without a
  full queue drain? *Experiment:* blit into a region the next draw samples, with no queue wait, and
  diff across a replay.
- **Q8 `[U]` Must the compute path ever bind a `MTL::Texture` projection, or can it always operate on
  the guest-window `MTL::Buffer`?** Decides whether the shard's `MTL::Texture` src-region target
  (§3.1) is load-bearing or merely an optimisation when the source is a live surface.

---

## 7. Provenance

**This tree `[V]` (paths under `rpcs3/Emu/RSX/`).**
`gcm_enums.h:800-821` (`NV30_SCALED_IMAGE_FROM_MEMORY` method enum); `:1731-1832`
(`transfer_origin`, `transfer_interpolator`, `transfer_operation`, `transfer_source_format`,
`transfer_destination_format`, `context_surface`); `rsx_decode.h:272-325` (`NV3089_CLIP_POINT`,
`NV3089_CLIP_SIZE`), `:328-438` (`NV3089_IMAGE_OUT_POINT`, `NV3089_IMAGE_OUT_SIZE`,
`NV3089_IMAGE_IN_SIZE`), `:1972-1992` (`NV3089_IMAGE_IN_OFFSET`), `:2041-2102` (`NV3089_DS_DX`,
`NV3089_DT_DY`), `:2806-2872` (`NV3089_SET_OPERATION`, `NV3089_SET_COLOR_FORMAT`,
`NV3089_SET_CONTEXT_SURFACE`), `:3576-3596` (`NV3089_SET_CONTEXT_DMA_IMAGE`), `:3737-3771`
(`NV3089_IMAGE_IN_FORMAT`), `:3918-3955` (`NV3089_IMAGE_IN`); `rsx_methods.h:1033-1195`
(`blit_engine_*` accessors; input-pitch `:1090-1093`; NV3062/NV309E output accessors `:1137-1175`);
`rsx_methods.cpp:1562-1582` (all methods `nullptr`), `:1725-1726` (`bind(NV3089_IMAGE_IN,
nv3089::image_in)`); `NV47/HW/nv3089.h:10` (`image_in` decl); `NV47/HW/nv3089.cpp:16-319`
(`decode_transfer_registers`; operation `:73-78`; source format `:80-85,263-276`; source address
`:87-88,189,195,209,233`; context-surface switch `:97-141`; pitch defaults `:146-154`; block-transfer
test `:156-159,204`; clip `:39-47,59-60`; subpixel/scale `:176-187`; `src_line_length` `:192`),
`:321-423` (`linear_copy`; overlap `:338-343`; `memmove` `:381`; `memcpy` `:405`; `convert_scale_image`
`:413-422`), `:425-534` (`swizzled_copy_1`; `swizzled_copy_2`; `next_pow2` `:498-499`;
`convert_linear_swizzle` `:522-533`), `:536-570` (`_mirror_transform`), `:572-684` (`image_in`;
`reservation_lock` `:588-590`; `scaled_image_from_memory` `:592-598`; `mm_flush` `:600-606`; mirror
`:612-614`; `need_convert` `:629`; interpolator `:631`; `tiled_region` `:633-642`; re-tile
`:666-683`); `NV47/HW/nv308a.cpp:129-140` (shared 5/6/5 converter);
`RSXThread.h:450` (`scaled_image_from_memory` virtual); `RSXThread.cpp:3221-3225` (`read_barrier`),
`:3204-3208` (pretty-printer skip to `NV3089_SET_OBJECT`), `:2696-2721`
(`invalidate_fragment_program`); `VK/VKGSRender.cpp:2870-2887` (the VK blit fast path).

**Inventory (facts).** `inventory/out/clusters/nv3089-image.csv` (21 ids; `NV3089_IMAGE_IN` the only
bound row; 14 with decoder, 13 decoded fields); `inventory/out/dispositions/nv3089-image.csv`
(the curated rows this doc documents); `inventory/out/SUMMARY.md:190-214,482-494` (cluster table,
decoded-field table); `inventory/SCHEMA.md` §4 (disposition vocab) §5 (CSV columns) §6 (LLD
template).

**Sibling scopes `[V]`.** `HLD-02-resource-model.md` §2.0 (shape change), §2.1 (guest window,
`mtl_guest_window`), §2.4 (swizzle/import-export Option B), §2.5 (no heap aliasing for guest
aliasing), §2.7 (format table), §3.1 (`mtl_guest_window`); `HLD-03-synchronization-model.md` §2.3
(CPU↔GPU edges, Edge B), §2.4 (`transfer_read`/`transfer_write`; pass scheduler), §3.4
(`mtl::emit_barrier` chokepoint), §4 (invariants); `LLD-nv2d-surface.md` §2/§3 (the NV3062/NV309E
destination descriptors and the swizzle kernel), §3.3 (shared kernel), §4; `LLD-nv308a-image.md`
§3.1/§3.2 (the inline-upload counterpart; shared 5/6/5 converter), Q6; `LLD-nv0039-dma.md` §3/§4
(the identical host-copy + visibility argument); `10-rsx-native-metal4-architecture.md` §3.1
(blit-engine semantics matter, not `vkCmdBlitImage`), §4.1 (guest memory unified/host), §5
(blit-engine VK plumbing deleted), §9 R4 (`supports_passthrough_dma=false`); `04-images-formats-
samplers-rtt.md` §2.6 (copies ride on the compute encoder; no blit encoder); `README.md` §4 shifts
1/4 (no hazard tracking; no blit encoder), §6 R25; `11-verification-loop.md` §3.4 (FIFO desync
alarm), §9 (H0/H5 oracles).

<!-- doc-end -->
