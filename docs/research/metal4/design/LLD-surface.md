# LLD — `surface`: the RSX render-surface descriptor, window and clear state

Backend: native Apple **Metal 4** (`MTL4*`) on Apple-silicon (M1+), macOS 26+ (`10` §10 `[V]`).
Cluster `surface` (40 ids, `inventory/out/clusters/surface.csv`). This LLD obeys
`HLD-02-resource-model.md` (guest bytes are canonical; a surface is a `MTL::Texture` projection of a
guest range, `HLD-02` §2.1/§2.2; pitch is kept as two distinct numbers, §2.3; identity is the app
address map, §2.5) and `HLD-03-synchronization-model.md` (encoder boundaries are derived from
surface hazards, not attachment identity, §2.4; every barrier goes through the §3.4 chokepoint).

Reading conventions: `[V]` verified against the tree (`file:line`), `[I]` inference, `[U]`
unverified. IDs are taken verbatim from the cluster shard; **no register is invented.**

---

## 1. Scope

`surface` is the register file that **describes the render target set** of the RSX: the colour
targets A–D, the depth ("zeta") target, their guest addresses and pitches, the surface pixel format
and antialiasing, the surface clip rectangle, the window offset/clip, and the clear path. In RSX
terms these are plain latched values, read at draw time by the framebuffer-layout code
(`get_framebuffer_layout`, `RSXThread.cpp:1472-1660`) and at clear time by
`thread::clear_surface(arg)` (`nv4097.cpp:561-566`). They do not, by themselves, *do* anything;
they are the inputs to pass synthesis and to the surface store.

This cluster owns the **descriptor of the attachment set**, not the attachment memory: the
`MTL::Texture` allocation, aliasing and residency are `HLD-02`'s concern; the encoder-boundary and
hazard logic is `HLD-03`'s. What is decided here is the mapping from the register fields onto
`MTLTextureDescriptor` / `MTL4RenderPassDescriptor` and the clip/window/clear handoff.

**Correction to the brief.** `NV4097_SET_BACK_END_WRITE_ENABLE` **does not exist** in the RSX method
enum; the only `BACK_END_WRITE` symbol is `NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE` (0x1d70), which
belongs to the `sync-flip` cluster, not here. No row is created for it. (This mirrors the
`HLD-03` §1.1 `SET_REFERENCE` correction.)

**Non-goals.** The texture-cache view of the same bytes (`LLD-*` texture clusters); the blend/depth
test state that consumes the attachments; the semaphore/label write path (`sync-flip`); the DMA
engines (`nv2d-surface`, `nv0039-dma`). The `M_NV4097_SET_ZSTENCIL_CLEAR_VALUE` register is **not in
this shard** and is not discussed as if it were.

---

## 2. Register model

Forty plain value registers. Fifteen are inert (no handler and no reader) and are dispositioned
`na`; the remaining twenty-five are live state consumed by the framebuffer-layout, surface-store and
clear paths. Only `SET_SURFACE_FORMAT` carries a dedicated handler; every other live register is
latched through `nv4097::set_surface_dirty_bit` (`nv4097.cpp:251`) or is unbound and read directly.

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_OBJECT` | 0x0000 | none |  | na | none (inert subchannel handle) |
| `M_NV4097_SET_CONTEXT_DMA_NOTIFIES` | 0x0180 | none |  | na | none (notify value arrives via `NV4097_NOTIFY`) |
| `M_NV4097_SET_CONTEXT_DMA_A` | 0x0184 | none |  | na | none (legacy context; sentinel only) |
| `M_NV4097_SET_CONTEXT_DMA_B` | 0x0188 | none |  | na | none (legacy context; sentinel only) |
| `M_NV4097_SET_CONTEXT_DMA_COLOR_B` | 0x018c | `CellGcmLocation dma_surface_b` (`rsx_decode.h:3494`) | `set_surface_dirty_bit` | reshape | guest-window partition base for surface B |
| `M_NV4097_SET_CONTEXT_DMA_STATE` | 0x0190 | none |  | na | none (state decoded from the FIFO) |
| `M_NV4097_SET_CONTEXT_DMA_COLOR_A` | 0x0194 | `CellGcmLocation dma_surface_a` (`rsx_decode.h:3471`) | `set_surface_dirty_bit` | reshape | guest-window partition base for surface A |
| `M_NV4097_SET_CONTEXT_DMA_ZETA` | 0x0198 | `CellGcmLocation dma_surface_z` (`rsx_decode.h:3553`) | `set_surface_dirty_bit` | reshape | guest-window partition base for zeta |
| `M_NV4097_SET_CONTEXT_DMA_VERTEX_A` | 0x019c | none |  | na | none (legacy context; sentinel only) |
| `M_NV4097_SET_CONTEXT_DMA_VERTEX_B` | 0x01a0 | none |  | na | none (legacy context; sentinel only) |
| `M_NV4097_SET_CONTEXT_DMA_REPORT` | 0x01a8 | `to_context_dma` (`rsx_decode.h:3701`) |  | reshape | report-block location selector over a shared `MTL::Buffer` |
| `M_NV4097_SET_CONTEXT_DMA_CLIP_ID` | 0x01ac | none |  | na | none (clip-id test not implemented) |
| `M_NV4097_SET_CONTEXT_DMA_CULL_DATA` | 0x01b0 | none |  | na | none (no reader) |
| `M_NV4097_SET_CONTEXT_DMA_COLOR_C` | 0x01b4 | `CellGcmLocation` (`rsx_decode.h:3507`) | `set_surface_dirty_bit` | reshape | guest-window partition base for surface C (MRT) |
| `M_NV4097_SET_CONTEXT_DMA_COLOR_D` | 0x01b8 | `CellGcmLocation` (`rsx_decode.h:3530`) | `set_surface_dirty_bit` | reshape | guest-window partition base for surface D (MRT) |
| `M_NV4097_SET_SURFACE_CLIP_HORIZONTAL` | 0x0200 | `origin_x[0:16] width[16:16]` (`rsx_decode.h:160`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor.width` + pass attachment width |
| `M_NV4097_SET_SURFACE_CLIP_VERTICAL` | 0x0204 | `origin_y[0:16] height[16:16]` (`rsx_decode.h:188`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor.height` + pass attachment height |
| `M_NV4097_SET_SURFACE_FORMAT` | 0x0208 | `color_fmt[0:5] depth_fmt[5:3] type[8:4] antialias[12:4] log2w[16:8] log2h[24:8]` (`rsx_decode.h:3330`) | `set_surface_format` | reshape | `MTL4RenderPassDescriptor` attachment set + `MTLTextureDescriptor.pixelFormat`/`sampleCount` |
| `M_NV4097_SET_SURFACE_PITCH_A` | 0x020c | `u32 pitch` (`rsx_decode.h:1485`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor` rowBytes (`native_pitch`) / `memory_range` |
| `M_NV4097_SET_SURFACE_COLOR_AOFFSET` | 0x0210 | `u32 surface_a_offset` (`rsx_decode.h:1403`) | `set_surface_dirty_bit` | reshape | guest-address identity for surface A |
| `M_NV4097_SET_SURFACE_ZETA_OFFSET` | 0x0214 | `u32` (`rsx_decode.h:1577`) | `set_surface_dirty_bit` | reshape | guest-address identity for zeta |
| `M_NV4097_SET_SURFACE_COLOR_BOFFSET` | 0x0218 | `u32` (`rsx_decode.h:1416`) | `set_surface_dirty_bit` | reshape | guest-address identity for surface B (MRT) |
| `M_NV4097_SET_SURFACE_PITCH_B` | 0x021c | `u32` (`rsx_decode.h:1508`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor` rowBytes for surface B / `memory_range` |
| `M_NV4097_SET_SURFACE_COLOR_TARGET` | 0x0220 | `to_surface_target` (`rsx_decode.h:2749`) | `set_surface_dirty_bit` | reshape | `MTLLogicalToPhysicalColorAttachmentMap` + attachment count |
| `M_NV4097_SET_SURFACE_PITCH_Z` | 0x022c | `u32` (`rsx_decode.h:1600`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor` rowBytes for zeta / `memory_range` |
| `M_NV4097_SET_CYLINDRICAL_WRAP` | 0x0238 | none |  | na | none (fixed-function cylindrical wrap, unused) |
| `M_NV4097_SET_CYLINDRICAL_WRAP1` | 0x023c | none |  | na | none (fixed-function cylindrical wrap, unused) |
| `M_NV4097_SET_SURFACE_PITCH_C` | 0x0280 | `u32` (`rsx_decode.h:1531`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor` rowBytes for surface C / `memory_range` |
| `M_NV4097_SET_SURFACE_PITCH_D` | 0x0284 | `u32` (`rsx_decode.h:1554`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor` rowBytes for surface D / `memory_range` |
| `M_NV4097_SET_SURFACE_COLOR_COFFSET` | 0x0288 | `u32` (`rsx_decode.h:1439`) | `set_surface_dirty_bit` | reshape | guest-address identity for surface C (MRT) |
| `M_NV4097_SET_SURFACE_COLOR_DOFFSET` | 0x028c | `u32` (`rsx_decode.h:1462`) | `set_surface_dirty_bit` | reshape | guest-address identity for surface D (MRT) |
| `M_NV4097_SET_WINDOW_OFFSET` | 0x02b8 | `window_offset_x[0:16] window_offset_y[16:16]` (`rsx_decode.h:366`) | `set_surface_dirty_bit` | reshape | surface base-address shift (app address map) |
| `M_NV4097_SET_WINDOW_CLIP_TYPE` | 0x02bc | none |  | na | none (unimplemented window clip) |
| `M_NV4097_SET_WINDOW_CLIP_HORIZONTAL` | 0x02c0 | none |  | na | none (diagnostic only) |
| `M_NV4097_SET_WINDOW_CLIP_VERTICAL` | 0x02c4 | none |  | na | none (diagnostic only) |
| `M_NV4097_SET_SURFACE_COMPRESSION` | 0x1d80 | none |  | na | none (RSX surface compression, unimplemented) |
| `M_NV4097_SET_COLOR_CLEAR_VALUE` | 0x1d90 | `blue[0:8] green[8:8] red[16:8] alpha[24:8]` (`rsx_decode.h:2608`) |  | direct | `MTLClearColor` on `MTL4RenderPassDescriptor.colorAttachments[i].clearColor` |
| `M_NV4097_CLEAR_SURFACE` | 0x1d94 | `u32 mask` (`RSX_GCM_CLEAR_*`) | `nv4097::clear` | reshape | `MTLLoadActionClear` (else a scissored clear draw/compute) |
| `M_NV4097_SET_CLEAR_RECT_HORIZONTAL` | 0x1d98 | `origin_x[0:16] width[16:16]` (`rsx_decode.h:226`) |  | reshape | `MTLScissorRect` for a clear draw, or full-attachment `MTLLoadActionClear` |
| `M_NV4097_SET_CLEAR_RECT_VERTICAL` | 0x1d9c | `origin_y[0:16] height[16:16]` (`rsx_decode.h:254`) |  | reshape | `MTLScissorRect` for a clear draw, or full-attachment `MTLLoadActionClear` |

**2.1 DMA context and subchannel.** `SET_OBJECT` is the NV4097 subchannel handle and is inert
(`rsx_methods.cpp:1270`). Of the context-DMA registers, only the five surface DMAs plus the report
DMA are read: `surface_dma(i)` returns the `CellGcmLocation` for colour target `i`
(`rsx_methods.h:832-835`) and `surface_z_dma()` for zeta (`rsx_methods.h:851`); `get_address()`
folds the location (main/local) with the byte offset (`rsx_utils.h:236-237`).
`SET_CONTEXT_DMA_A/B/STATE/VERTEX_A/VERTEX_B/CLIP_ID/CULL_DATA/NOTIFIES` are installed as `nullptr`
(`rsx_methods.cpp:1275-1287`) and carry only the `cellGcmInit` sentinels (`rsx_methods.cpp:167-174`);
they are `na`. `SET_CONTEXT_DMA_REPORT` is decoded to a report-block location selector and consumed
by `get_report_data_impl` (`RSXThread.cpp:190-201`). `SET_CYLINDRICAL_WRAP`/`1` are unbound
(`rsx_methods.cpp:1304-1305`) with no reader — the RSX-first pipeline model has no cylindrical-wrap
stage (`10` §3.2) — and `SET_SURFACE_COMPRESSION` is unbound (`rsx_methods.cpp:1470`) with no reader
anywhere in the tree.

**2.2 Surface format.** `SET_SURFACE_FORMAT` is the only dedicated handler in the cluster
(`nv4097::set_surface_format`, `nv4097.cpp:291`). Six fields decode (`rsx_decode.h:3330-3385`):
`color_fmt` (17 values, `rsx_value_space.csv:620-633`), `depth_fmt` (`Z16`/`Z24S8`,
`rsx_value_space.csv:614-615`), `type` (linear/swizzle), `antialias`
(`CENTER_1`/`DIAGONAL_CENTERED_2`/`SQUARE_CENTERED_4`/`SQUARE_ROTATED_4`, values 0/3/4/5,
`rsx_value_space.csv:616-619`), and `log2width`/`log2height`. The log2 fields are **advisory** — the
surface clip wins (see §2.4) — and the `is_remapped_format()` predicate flags the eight formats whose
channel order is reinterpreted on import (`rsx_decode.h:3373-3385`). This register is the single
highest-risk row of the cluster (difficulty 4, risk high) because it drives pixel format, sample
count and pass synthesis at once.

**2.3 Colour targets A–D and zeta.** Each colour target is a `(context DMA, offset, pitch)` triple;
zeta adds `SET_SURFACE_ZETA_OFFSET` and `SET_SURFACE_PITCH_Z`. `SET_SURFACE_COLOR_TARGET` selects
which targets are live: `SURFACE_TARGET_0` (single), and `MRT1`/`MRT2`/`MRT3` = 0x13/0x17/0x1f which
select contiguous `A..D` prefixes (`rsx_value_space.csv:608-613`; `get_rtt_indexes()`,
`surface_store.h:18`). Offsets are base addresses folded with the DMA location; pitches are guest
bytes-per-row and may be corrected or cause the depth surface to be dropped when too small
(`RSXThread.cpp:1628-1660`).

**2.4 Surface clip.** `SET_SURFACE_CLIP_HORIZONTAL`/`_VERTICAL` give the origin and size that become
the framebuffer width/height (`RSXThread.cpp:1477-1478`) and feed `prepare_render_target`'s
`clip_width`/`clip_height` (`surface_store.h:1025-1052`). This is the authoritative extent; the
`log2` fields of `SET_SURFACE_FORMAT` are not.

**2.5 Window.** `SET_WINDOW_OFFSET` shifts every surface base address by `pitch × y + texel × x`
(`RSXThread.cpp:1827-1852`). `SET_WINDOW_CLIP_TYPE`/`_HORIZONTAL`/`_VERTICAL` are unbound
(`rsx_methods.cpp:1315-1317`); the horizontal/vertical values are read **only** to emit a warning
when smaller than the surface clip (`RSXThread.cpp:1855-1860`) and are never applied — hence `na`.

**2.6 Clear path.** `SET_COLOR_CLEAR_VALUE` decodes an RGBA8 colour (`rsx_decode.h:2608-2611`);
`CLEAR_SURFACE` is bound to `nv4097::clear` (`rsx_methods.cpp`), which calls
`thread::clear_surface(arg)` (`nv4097.cpp:561-566`) with a mask of `RSX_GCM_CLEAR_*` bits
(`rsx_value_space.csv:602-607`). `SET_CLEAR_RECT_HORIZONTAL`/`_VERTICAL` are unbound but read by
`clear_surface`; they scope the clear to a sub-rectangle.

**Lifetime.** Value registers; no cross-draw state of their own. The surface store owns the derived
resource and its lifetime (`HLD-02` §2.5); these registers are re-read whenever the layout or the
clear is re-evaluated.

---

## 3. Metal 4 mapping

**The shape change.** RSX names a render target by a *descriptor in guest address space*
`(base, pitch, format, sample layout)`; Metal has no address-identity resource (`HLD-02` §2.0). Under
`HLD-02` §2.1 the guest byte image is one `StorageModeShared` `MTL::Buffer`; a surface is a
`MTL::Texture` projection of a range within it. This cluster's registers are therefore the
**operands that build two objects**: an `MTLTextureDescriptor` (the attachment resource) and a
`MTL4RenderPassDescriptor` (the encoder's attachment configuration). There is no `VkRenderPass`,
`VkFramebuffer` or packed-`u64` cache key (`10` §4.3; `HLD-03` §2.5).

The table below repeats the §2 rows grouped by mapping sub-area; the prose after it gives the
concrete objects.

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_OBJECT` | 0x0000 | none |  | na | none (inert subchannel handle) |
| `M_NV4097_SET_CONTEXT_DMA_NOTIFIES` | 0x0180 | none |  | na | none (notify value arrives via `NV4097_NOTIFY`) |
| `M_NV4097_SET_CONTEXT_DMA_A` | 0x0184 | none |  | na | none (legacy context; sentinel only) |
| `M_NV4097_SET_CONTEXT_DMA_B` | 0x0188 | none |  | na | none (legacy context; sentinel only) |
| `M_NV4097_SET_CONTEXT_DMA_STATE` | 0x0190 | none |  | na | none (state decoded from the FIFO) |
| `M_NV4097_SET_CONTEXT_DMA_VERTEX_A` | 0x019c | none |  | na | none (legacy context; sentinel only) |
| `M_NV4097_SET_CONTEXT_DMA_VERTEX_B` | 0x01a0 | none |  | na | none (legacy context; sentinel only) |
| `M_NV4097_SET_CONTEXT_DMA_CLIP_ID` | 0x01ac | none |  | na | none (clip-id test not implemented) |
| `M_NV4097_SET_CONTEXT_DMA_CULL_DATA` | 0x01b0 | none |  | na | none (no reader) |
| `M_NV4097_SET_CYLINDRICAL_WRAP` | 0x0238 | none |  | na | none (fixed-function cylindrical wrap, unused) |
| `M_NV4097_SET_CYLINDRICAL_WRAP1` | 0x023c | none |  | na | none (fixed-function cylindrical wrap, unused) |
| `M_NV4097_SET_SURFACE_COMPRESSION` | 0x1d80 | none |  | na | none (RSX surface compression, unimplemented) |
| `M_NV4097_SET_CONTEXT_DMA_COLOR_A` | 0x0194 | `CellGcmLocation dma_surface_a` (`rsx_decode.h:3471`) | `set_surface_dirty_bit` | reshape | guest-window partition base for surface A |
| `M_NV4097_SET_CONTEXT_DMA_COLOR_B` | 0x018c | `CellGcmLocation dma_surface_b` (`rsx_decode.h:3494`) | `set_surface_dirty_bit` | reshape | guest-window partition base for surface B |
| `M_NV4097_SET_CONTEXT_DMA_COLOR_C` | 0x01b4 | `CellGcmLocation` (`rsx_decode.h:3507`) | `set_surface_dirty_bit` | reshape | guest-window partition base for surface C (MRT) |
| `M_NV4097_SET_CONTEXT_DMA_COLOR_D` | 0x01b8 | `CellGcmLocation` (`rsx_decode.h:3530`) | `set_surface_dirty_bit` | reshape | guest-window partition base for surface D (MRT) |
| `M_NV4097_SET_CONTEXT_DMA_ZETA` | 0x0198 | `CellGcmLocation dma_surface_z` (`rsx_decode.h:3553`) | `set_surface_dirty_bit` | reshape | guest-window partition base for zeta |
| `M_NV4097_SET_CONTEXT_DMA_REPORT` | 0x01a8 | `to_context_dma` (`rsx_decode.h:3701`) |  | reshape | report-block location selector over a shared `MTL::Buffer` |
| `M_NV4097_SET_SURFACE_FORMAT` | 0x0208 | `color_fmt[0:5] depth_fmt[5:3] type[8:4] antialias[12:4] log2w[16:8] log2h[24:8]` (`rsx_decode.h:3330`) | `set_surface_format` | reshape | `MTL4RenderPassDescriptor` attachment set + `MTLTextureDescriptor.pixelFormat`/`sampleCount` |
| `M_NV4097_SET_SURFACE_COLOR_TARGET` | 0x0220 | `to_surface_target` (`rsx_decode.h:2749`) | `set_surface_dirty_bit` | reshape | `MTLLogicalToPhysicalColorAttachmentMap` + attachment count |
| `M_NV4097_SET_SURFACE_COLOR_AOFFSET` | 0x0210 | `u32 surface_a_offset` (`rsx_decode.h:1403`) | `set_surface_dirty_bit` | reshape | guest-address identity for surface A |
| `M_NV4097_SET_SURFACE_COLOR_BOFFSET` | 0x0218 | `u32` (`rsx_decode.h:1416`) | `set_surface_dirty_bit` | reshape | guest-address identity for surface B (MRT) |
| `M_NV4097_SET_SURFACE_COLOR_COFFSET` | 0x0288 | `u32` (`rsx_decode.h:1439`) | `set_surface_dirty_bit` | reshape | guest-address identity for surface C (MRT) |
| `M_NV4097_SET_SURFACE_COLOR_DOFFSET` | 0x028c | `u32` (`rsx_decode.h:1462`) | `set_surface_dirty_bit` | reshape | guest-address identity for surface D (MRT) |
| `M_NV4097_SET_SURFACE_ZETA_OFFSET` | 0x0214 | `u32` (`rsx_decode.h:1577`) | `set_surface_dirty_bit` | reshape | guest-address identity for zeta |
| `M_NV4097_SET_SURFACE_PITCH_A` | 0x020c | `u32 pitch` (`rsx_decode.h:1485`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor` rowBytes (`native_pitch`) / `memory_range` |
| `M_NV4097_SET_SURFACE_PITCH_B` | 0x021c | `u32` (`rsx_decode.h:1508`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor` rowBytes for surface B / `memory_range` |
| `M_NV4097_SET_SURFACE_PITCH_C` | 0x0280 | `u32` (`rsx_decode.h:1531`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor` rowBytes for surface C / `memory_range` |
| `M_NV4097_SET_SURFACE_PITCH_D` | 0x0284 | `u32` (`rsx_decode.h:1554`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor` rowBytes for surface D / `memory_range` |
| `M_NV4097_SET_SURFACE_PITCH_Z` | 0x022c | `u32` (`rsx_decode.h:1600`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor` rowBytes for zeta / `memory_range` |
| `M_NV4097_SET_SURFACE_CLIP_HORIZONTAL` | 0x0200 | `origin_x[0:16] width[16:16]` (`rsx_decode.h:160`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor.width` + pass attachment width |
| `M_NV4097_SET_SURFACE_CLIP_VERTICAL` | 0x0204 | `origin_y[0:16] height[16:16]` (`rsx_decode.h:188`) | `set_surface_dirty_bit` | reshape | `MTLTextureDescriptor.height` + pass attachment height |
| `M_NV4097_SET_WINDOW_OFFSET` | 0x02b8 | `window_offset_x[0:16] window_offset_y[16:16]` (`rsx_decode.h:366`) | `set_surface_dirty_bit` | reshape | surface base-address shift (app address map) |
| `M_NV4097_SET_WINDOW_CLIP_TYPE` | 0x02bc | none |  | na | none (unimplemented window clip) |
| `M_NV4097_SET_WINDOW_CLIP_HORIZONTAL` | 0x02c0 | none |  | na | none (diagnostic only) |
| `M_NV4097_SET_WINDOW_CLIP_VERTICAL` | 0x02c4 | none |  | na | none (diagnostic only) |
| `M_NV4097_SET_COLOR_CLEAR_VALUE` | 0x1d90 | `blue[0:8] green[8:8] red[16:8] alpha[24:8]` (`rsx_decode.h:2608`) |  | direct | `MTLClearColor` on `MTL4RenderPassDescriptor.colorAttachments[i].clearColor` |
| `M_NV4097_CLEAR_SURFACE` | 0x1d94 | `u32 mask` (`RSX_GCM_CLEAR_*`) | `nv4097::clear` | reshape | `MTLLoadActionClear` (else a scissored clear draw/compute) |
| `M_NV4097_SET_CLEAR_RECT_HORIZONTAL` | 0x1d98 | `origin_x[0:16] width[16:16]` (`rsx_decode.h:226`) |  | reshape | `MTLScissorRect` for a clear draw, or full-attachment `MTLLoadActionClear` |
| `M_NV4097_SET_CLEAR_RECT_VERTICAL` | 0x1d9c | `origin_y[0:16] height[16:16]` (`rsx_decode.h:254`) |  | reshape | `MTLScissorRect` for a clear draw, or full-attachment `MTLLoadActionClear` |

### 3.1 The surface → attachment projection

`SET_SURFACE_FORMAT` is the pivot. Its `color_fmt`/`depth_fmt` select the `MTLPixelFormat` via the
`HLD-02` §2.7 table (with the eight `is_remapped_format()` values requiring a channel-order
reinterpretation on import); `antialias` selects the sample count and the resolve relationship
(`HLD-02` §2.2); `type` (linear/swizzle) is **not** a Metal field but surface metadata that selects
the import/export converter (`HLD-02` §2.4). The resulting resource is a `MTL::Texture` created from
an `MTLTextureDescriptor` (or `MTL::Heap::newTextureWithDescriptor:offset:` when sub-allocated,
`HLD-02` §2.2). `[I]`

`SET_SURFACE_CLIP_HORIZONTAL`/`_VERTICAL` set `MTLTextureDescriptor.width`/`height` — the clip, not
the advisory `log2` fields, is authoritative. `SET_SURFACE_PITCH_*` map to the texture row stride as
`native_pitch` and to the surface `memory_range` as `rsx_pitch × height`; the invariant
`native_pitch ≥ rsx_pitch` is `HLD-02` §2.3's, and the existing pitch-fixup path
(`surface_store.h:1177-1230`, `truncate_memory_range_by_pitch`) is what the depth-drop case
(`RSXThread.cpp:1644-1657`) rides on. `[I]`

### 3.2 Colour targets A–D → colour attachments and MRT

Each live colour target becomes one `MTL::Texture` bound as
`MTL4RenderPassDescriptor.colorAttachments[i].texture`, and zeta becomes
`depthAttachment.texture`/`stencilAttachment.texture` (per the format's combined depth-stencil
grouping, `HLD-02` §2.5). `SET_SURFACE_COLOR_TARGET`'s `MRT1/2/3` prefixes are realised with
`MTLLogicalToPhysicalColorAttachmentMap` — one encoder configures a superset of colour attachments
and remaps shader outputs per pipeline (`10` §4.3 `[V]`; `README.md` #139) — which avoids the pass
splits a distinct-attachment scheme would force. The offsets are the app-address identity, not a
Metal field (`HLD-02` §2.1/§2.5). `[I]`

### 3.3 Clip → viewport/scissor handoff

The surface clip becomes the texture extent (§3.1) **and** the base viewport/scissor rectangle of
draws targeting it. The per-draw viewport/scissor state (the `viewport`/`scissor` clusters, not this
shard) narrows within the surface clip; when it does not, the surface clip is the bound. Metal's
`MTLViewport`/`MTLScissorRect` are set on the `MTL4RenderCommandEncoder`; the surface clip is the
*attachment* constraint (the render area is implicit in the encoder's attachments, `10` §4.3), not a
separate `VkRenderArea`. `[I]`

### 3.4 Window offset and window clip

`SET_WINDOW_OFFSET` is folded into each surface base address **before** the address map lookup
(`RSXThread.cpp:1827-1852`), so it is an app-address-map concern (`HLD-02` §2.5), not a Metal
object: the shifted address resolves to the same `MTL::Buffer` range and hence the same
`MTL::Texture`. `SET_WINDOW_CLIP_*` are `na`; the mapping deliberately does not implement a window
clip, matching the current tree (diagnostic only, `RSXThread.cpp:1855-1860`). `[I]`

### 3.5 Clear path and the clear-before-draw reduction

The clear is the clearest place to collapse RSX semantics onto Metal 4. When the clear covers the
whole attachment (`SET_CLEAR_RECT_*` equals the surface clip) **and** the target is freshly bound,
it reduces to `MTLLoadActionClear` with `clearColor = MTLClearColor` from
`SET_COLOR_CLEAR_VALUE` (converted unorm→double) on the `MTL4RenderPassDescriptor` — no separate
command, no extra pass. This is the **clear-before-draw reduction**: RSX's explicit `CLEAR_SURFACE`
followed by a draw becomes the opening encoder's load action, and the clear disappears from the
stream. `[I]`

When the clear rect is a **sub-rectangle**, a whole-attachment load action is wrong; the clear
becomes either a scissored clear draw on the `MTL4RenderCommandEncoder` (`MTLScissorRect` from
`SET_CLEAR_RECT_*`) or a compute fill, inserted as its own pass or as an intra-encoder step. The
depth/stencil half of the mask (`RSX_GCM_CLEAR_DEPTH_BIT`/`_STENCIL_BIT`) uses the
`depthAttachment`/`stencilAttachment` clear values on the same descriptor. `[I]`

### 3.6 Pass-synthesis role (`HLD-03`)

This cluster supplies the **attachment set** that the hazard-derived scheduler consumes: the target
attachments (`HLD-03` §2.4 "target attachment set", `RSXThread.h:145-147`) and, through the clear
path, the per-attachment `MTLLoadAction`. A new `MTL4RenderPassDescriptor` (and hence encoder) is
opened when the attachment set changes or when a hazard cannot be satisfied intra-pass
(`HLD-03` §2.4; `10` §4.3); where the next encoder shares attachments it is linked with
`MTL4RenderEncoderOptionSuspending`/`Resuming`. The `surface_store` remains the owner of the
resources and the hazard bookkeeping (`HLD-02` §2.5): this cluster's registers are re-read to build
`mtl::pass_node.attachments` and the per-attachment load/store actions (`HLD-03` §3.2). `[V]`/`[I]`

**Objects used.** `MTLTextureDescriptor`, `MTL::Texture` (colour A–D, zeta/resolve);
`MTL4RenderPassDescriptor` with `colorAttachments[i]`/`depthAttachment`/`stencilAttachment`;
`MTLClearColor`; `MTLLogicalToPhysicalColorAttachmentMap`; `MTL4RenderCommandEncoder` (scissored
clear); `MTLScissorRect`/`MTLViewport`. No new object class is introduced by this cluster; the
hazard barriers that order the attachment writes are `HLD-03`'s chokepoint.

---

## 4. Impossible / not-recommended

- **Not-recommended: per-attachment pass splits to realise MRT.** Binding distinct physical
  attachments and splitting the pass is expressible but is exactly the trap `10` §4.3 and
  `README.md` #139 avoid; `MTLLogicalToPhysicalColorAttachmentMap` is the chosen mechanism
  (§3.2).
- **Not-recommended: whole-attachment `MTLLoadActionClear` when the clear rect is a sub-rectangle.**
  It would clear the wrong pixels; the fallback is a scissored clear draw/compute (§3.5).
- **Not-recommended: reviving a `VkRenderPass`/attachment-identity cache.** The attachment set is an
  input to the hazard-derived scheduler, not a cache key (`10` §4.3, `HLD-03` §2.5).
- **Not-recommended: Metal heap aliasing for surface overlap.** Guest alias granularity does not
  match heap offsets and the previous contents are needed anyway (`HLD-02` §2.5).
- **Impossible/`na`: RSX surface compression.** `SET_SURFACE_COMPRESSION` has no reader and no
  Metal path; framebuffer compression is not modelled (§2.1). `na`, not `impossible`, because the
  construct is inert in this tree.
- **`na` (unimplemented): hardware window clip.** `SET_WINDOW_CLIP_*` is never applied; the chosen
  fallback is to ignore it, as today (§3.4).

---

## 5. Invariants & failure modes

**Invariants.**
1. The surface clip, **not** `SET_SURFACE_FORMAT`'s `log2width`/`log2height`, determines the
   attachment extent (`RSXThread.cpp:1477-1478`). `[V]`
2. `native_pitch ≥ rsx_pitch` and `native_pitch ≥ bpp × width`; a depth surface whose pitch is below
   the minimum is dropped, not silently mis-addressed (`RSXThread.cpp:1644-1657`; `HLD-02` §2.3).
3. `SET_SURFACE_COLOR_TARGET`'s `MRT1/2/3` select **contiguous** `A..D` prefixes; the attachment
   count equals the prefix length (`rsx_value_space.csv:608-613`).
4. A sub-rectangle clear is never realised as a whole-attachment load action (§3.5).
5. Every attachment the encoder writes has an explicit ordering from the `HLD-03` barrier
   chokepoint; no correctness argument cites implicit Metal hazard tracking (`HLD-03` §4).
6. The window offset is folded into the address before identity lookup; the same guest address under
   a window offset must not alias a different resident surface by accident (`HLD-02` §2.5). `[I]`

**Failure modes.**
- **F1 wrong pixel format / remap.** The eight `is_remapped_format()` colour formats
  (`rsx_decode.h:3373-3385`) must be reinterpreted on import, or channels swap. Detection: H5 region
  diff against guest bytes.
- **F2 sample-count mismatch.** `antialias` values 0/3/4/5 must select the sample count and the
  resolve relationship; a wrong count corrupts the resolved image (`HLD-02` §2.2). Detection: H5 on
  an MSAA target plus a resolve.
- **F3 sub-rect clear applied whole-attachment.** Clears pixels outside the rect (§4). Detection: H2
  decision trace on `CLEAR_SURFACE` with a non-full rect.
- **F4 depth-surface drop.** The minimum-zeta-pitch correction silently drops the depth surface
  (`RSXThread.cpp:1644-1657`); the scheduler must not then reference a stale attachment. Detection:
  H3 state oracle.
- **F5 window-offset aliasing.** A non-zero window offset shifts the base into another surface's
  range; without care the address map resolves the wrong resource. Detection: H3 + a replay whose
  window offset is non-zero.
- **F6 MRT remap error.** A wrong logical→physical map writes a shader output to the wrong
  attachment. Detection: H5 per-attachment diff.

---

## 6. Open questions

- **Q1 `[U]` MRT: remap or distinct attachments?** `MTLLogicalToPhysicalColorAttachmentMap` lets one
  encoder configure a superset and remap per pipeline (`10` §4.3); the alternative is a distinct
  attachment per `SET_SURFACE_COLOR_TARGET` configuration. The former avoids pass splits but depends
  on the map being available for the required pixel formats. *Experiment:* a three-MRT replay
  compiled with a remapped superset vs distinct attachments; compare pass count and attachment diff.
- **Q2 `[U]` Mid-frame partial clear vs pass split.** A `CLEAR_SURFACE` on a live attachment may
  force a pass boundary (`HLD-03` §2.4). Does the clear-before-draw reduction (§3.5) cover the
  common case, or do titles clear mid-frame often enough to need the scissored-clear path as the
  default? *Experiment:* count `CLEAR_SURFACE` occurrences between two draws on the same attachment.
- **Q3 `[U]` Does any title use a non-zero window offset against a resident RTT?** The offset shifts
  the base address before lookup (`RSXThread.cpp:1827-1852`); if it never targets a live surface in
  practice the aliasing risk (F5) is theoretical. *Experiment:* grep replays for non-zero
  `SET_WINDOW_OFFSET` preceding a draw.
- **Q4 `[U]` Does the clear rect ever differ from the surface clip?** If it never does, §3.5's
  reduction is total and the scissored-clear path is dead code. *Experiment:* compare
  `SET_CLEAR_RECT_*` against `SET_SURFACE_CLIP_*` across a replay corpus.
- **Q5 `[U]` Does any title depend on RSX surface compression?** `SET_SURFACE_COMPRESSION` is
  unbound and unread; if no title issues it, the `na` is safe. *Experiment:* scan FIFOs for the
  register.
- **Q6 `[I]` Is `SET_CONTEXT_DMA_REPORT` in scope for the surface pass synthesis?** It selects the
  report-block location for `get_report_data_impl` (`RSXThread.cpp:190-201`); it is grouped here by
  address but is a synchronization sink (`HLD-03` §3.3). Confirm it belongs to `sync-flip`'s LLD
  instead.

---

## 7. Provenance

**This tree `[V]` (paths under `rpcs3/Emu/RSX/`).**
`rsx_decode.h:160-184`/`:188-213` (surface clip), `:216-241`/`:244-269` (clear rect), `:356-381`
(window offset), `:1393-1423` (`COLOR_AOFFSET`), `:1416-1462` (`BOFFSET`/`COFFSET`/`DOFFSET`),
`:1485-1623` (`PITCH_A/B/C/D`, `ZETA_OFFSET`, `PITCH_Z`), `:2598-2618`
(`SET_COLOR_CLEAR_VALUE`), `:2737-2757` (`SET_SURFACE_COLOR_TARGET`), `:3323-3393`
(`SET_SURFACE_FORMAT`), `:3461-3575` (`CONTEXT_DMA_COLOR_A/B/C/D`, `CONTEXT_DMA_ZETA`),
`:3691-3711` (`CONTEXT_DMA_REPORT`); `rsx_methods.h:832-851` (`surface_dma`/`surface_z_dma`);
`rsx_methods.cpp:1270-1317` (null method table, surface/window rows), `:1470`
(`SURFACE_COMPRESSION`), `:167-174` (context sentinels); `NV47/HW/nv4097.cpp:251`
(`set_surface_dirty_bit`), `:291` (`set_surface_format`), `:561-566` (`clear` →
`thread::clear_surface`); `RSXThread.cpp:190-201` (`get_report_data_impl`), `:1440-1469`
(surface address fold), `:1472-1660` (`get_framebuffer_layout`; clip `:1477-1478`; pitch fixup
`:1628-1660`), `:1826-1860` (window offset shift and window-clip diagnostic);
`Common/surface_store.h:18` (`get_rtt_indexes`), `:1012-1069` (`prepare_render_target`),
`:1177-1230` (`truncate_memory_range_by_pitch`); `Common/TextureUtils.h:49-66` (`surface_access`).

**Inventory (facts).** `inventory/out/clusters/surface.csv` (40 ids; 25 bound rows);
`inventory/out/rsx_value_space.csv:602-633` (clear bits, targets, AA, surface colour/depth
formats); `inventory/out/SUMMARY.md`; `inventory/SCHEMA.md` §4 (disposition vocab) §5 (CSV columns)
§6 (LLD template).

**Sibling scopes `[V]`.** `HLD-02-resource-model.md` §2.0 (shape change), §2.1 (guest window,
`surface_dma`/offsets), §2.2 (colour A–D + zeta → `MTLTexture`; MRT map), §2.3 (pitch), §2.4
(raster swizzle as metadata), §2.5 (address map, aliasing, `surface_store` role), §2.7 (format
table); `HLD-03-synchronization-model.md` §2.4 (hazard-derived pass scheduler; attachment set;
flush conditions), §2.5 (no `VkRenderPass`), §3.2 (`pass_node`), §3.3 (`label_sink`), §3.4 (barrier
chokepoint), §4 (invariants); `10-rsx-native-metal4-architecture.md` §2.3 (caches), §4.3 (pass /
attachment model; `MTLLogicalToPhysicalColorAttachmentMap`), §4.4 (explicit barriers), §3.2
(feature-dumped and unused); `04-images-formats-samplers-rtt.md` §2.6 (clear path),
§2.7 (resolve/store actions); `README.md` §4 shift 3 (no `VkRenderPass`), §6 (RN1, #139 MRT).

<!-- doc-end -->
