# LLD — `depth-rop` cluster (RSX depth/stencil/ROP + ZCULL → Metal 4)

Owner/Scope: RSX-first Metal 4 backend, the `depth-rop` cluster | Status: draft | Date: 2026-10-16 (Fri)
Branch: `metal4`. Design only — nothing under `rpcs3/**` is modified.

**Legend.** `[V]` verified by reading a file in this repo (or a pristine reference under
`/tmp/metal4-refs/`); `[I]` inference from `[V]` facts; `[U]` could not verify. `file:line` is
advisory (the branch moves); **symbol names are the stable reference**. Difficulty 1–5 and the
disposition vocabulary are those of `inventory/SCHEMA.md` §4. Every id below is taken verbatim from
`inventory/out/clusters/depth-rop.csv` (38 ids); this document invents no register.

---

## 1. Scope

The `depth-rop` cluster is the RSX **depth/stencil test state and the tile-level depth cull
(ZCULL)**: the depth test/func/mask triple, the front- and back-face stencil function/op/mask/ref
quintuple plus the two-sided enable, the per-channel colour write mask (single and MRT), the depth
bounds test, the depth clip/clamp control (`ZMIN/MAX`), the combined depth/stencil clear value, the
anti-aliasing control (MSAA enable, alpha-to-coverage, alpha-to-one, sample mask), and the whole
ZCULL surface: unit enable, statistics enable, ZPASS pixel counting, ZCULL-memory clear, stencil
cull, and the ZCULL/clip-id/reduce-dst control words. `12-rsx-state-space-inventory.md:175`
("depth/stencil/ROP + **ZCULL**"), `:269-271` (ZCULL is "the canonical high-risk cluster").

In scope (38 ids, `out/clusters/depth-rop.csv`): `M_NV4097_INVALIDATE_ZCULL`,
`M_NV4097_SET_COLOR_MASK`, `M_NV4097_SET_STENCIL_TEST_ENABLE`, `M_NV4097_SET_STENCIL_MASK`,
`M_NV4097_SET_STENCIL_FUNC`, `M_NV4097_SET_STENCIL_FUNC_REF`, `M_NV4097_SET_STENCIL_FUNC_MASK`,
`M_NV4097_SET_STENCIL_OP_FAIL`, `M_NV4097_SET_STENCIL_OP_ZFAIL`, `M_NV4097_SET_STENCIL_OP_ZPASS`,
`M_NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE`, `M_NV4097_SET_BACK_STENCIL_MASK`,
`M_NV4097_SET_BACK_STENCIL_FUNC`, `M_NV4097_SET_BACK_STENCIL_FUNC_REF`,
`M_NV4097_SET_BACK_STENCIL_FUNC_MASK`, `M_NV4097_SET_BACK_STENCIL_OP_FAIL`,
`M_NV4097_SET_BACK_STENCIL_OP_ZFAIL`, `M_NV4097_SET_BACK_STENCIL_OP_ZPASS`,
`M_NV4097_SET_COLOR_MASK_MRT`, `M_NV4097_SET_DEPTH_BOUNDS_TEST_ENABLE`,
`M_NV4097_SET_DEPTH_BOUNDS_MIN`, `M_NV4097_SET_DEPTH_BOUNDS_MAX`, `M_NV4097_ZCULL_SYNC`,
`M_NV4097_SET_DEPTH_FUNC`, `M_NV4097_SET_DEPTH_MASK`, `M_NV4097_SET_DEPTH_TEST_ENABLE`,
`M_NV4097_CLEAR_ZCULL_SURFACE`, `M_NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE`,
`M_NV4097_SET_ZCULL_STATS_ENABLE`, `M_NV4097_SET_ZMIN_MAX_CONTROL`,
`M_NV4097_SET_ANTI_ALIASING_CONTROL`, `M_NV4097_SET_ZCULL_EN`, `M_NV4097_SET_ZSTENCIL_CLEAR_VALUE`,
`M_NV4097_SET_CLIP_ID_TEST_ENABLE`, `M_NV4097_SET_ZCULL_CONTROL0`,
`M_NV4097_SET_ZCULL_CONTROL1`, `M_NV4097_SET_SCULL_CONTROL`, `M_NV4097_SET_REDUCE_DST_COLOR`.

Out of scope, owned elsewhere: the depth/stencil *surface* (format, pitch, tiling, residency,
aliasing) is the `surface` cluster / `HLD-02` (§2.2, §2.7); the render-pass descriptor that carries
the clear values and the attachment set is `surface` / `HLD-02` (`10 §4.3`); the report block,
semaphore and `thread::sync()` flush are `sync-flip` / `HLD-03` (§2.2, §3.3); PSO assembly and the
argument-table slot map are `HLD-05`; the raster state that shares the encoder (viewport, scissor,
cull, depth bias) is the `raster` cluster. This document fixes only how the 38 depth-rop registers
lower onto `MTL::DepthStencilState`, the encoder's depth/stencil/AA setters, the render-pipeline
colour-write masks, and the ZCULL visibility sink.

**The shape change (depth-stencil).** On Vulkan the entire depth-stencil configuration is folded
into one `VkPipelineDepthStencilStateCreateInfo` and hashed into the pipeline
(`VKPipelineCompiler.cpp:112-125`). On Metal the depth-stencil half **leaves the PSO** and becomes a
`MTL::DepthStencilState` set on the already-open encoder (`HLD-05 §2.6`, `:172-174`; `:190`). The
PSO keeps only the colour write masks, the sample count and the A2C/A2one flags. This split is the
central design decision of the cluster: depth-stencil is *context state*, not pipeline state.

**The shape change (ZCULL).** ZCULL is a tile-level depth cull whose *statistics are
guest-observable* (`12-rsx-state-space-inventory.md:269-271`). Metal 4 exposes **no cull unit and no
statistics unit**; the only related primitive is a render-pass-scoped **visibility result buffer**
that counts samples passing the depth test (`02 #33`; `09 §3 #15`; `README §6 R12`). ZCULL is
therefore split into (a) a visibility-buffer binding that carries ZPASS, and (b) host-side
accumulation that fabricates the statistics the guest reads back through `M_NV4097_GET_REPORT`
(`sync-flip` cluster; `HLD-03 §2.2`). The report read is a **correctness obligation**, not a
best-effort hint.

---

## 2. Register model

### 2.1 The shard

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_INVALIDATE_ZCULL` | 0x234 | none (no decoder) | — (unbound) | na | none (host-managed; visibility sink resets per pass) |
| `M_NV4097_SET_COLOR_MASK` | 0x324 | A/R/G/B at bits 0/8/16/24; `is_invalid()` | `nv4097::set_color_mask` | direct | `MTL4RenderPipelineColorAttachmentDescriptor.setWriteMask` |
| `M_NV4097_SET_STENCIL_TEST_ENABLE` | 0x328 | whole word (bool) | `set_surface_options_dirty_bit` | reshape | `MTL::DepthStencilDescriptor` front/back passthrough descriptor |
| `M_NV4097_SET_STENCIL_MASK` | 0x32c | whole word (u8 write mask) | dirty-bit handler | direct | `MTL::StencilDescriptor.setWriteMask` |
| `M_NV4097_SET_STENCIL_FUNC` | 0x330 | `to_comparison_function` | — (unbound) | direct | `MTL::StencilDescriptor.setStencilCompareFunction` |
| `M_NV4097_SET_STENCIL_FUNC_REF` | 0x334 | bits 0:8 (reference) | — (unbound) | direct | `MTL4RenderCommandEncoder.setStencilReferenceValues(front, back)` |
| `M_NV4097_SET_STENCIL_FUNC_MASK` | 0x338 | bits 0:8 (read mask) | — (unbound) | direct | `MTL::StencilDescriptor.setReadMask` |
| `M_NV4097_SET_STENCIL_OP_FAIL` | 0x33c | `to_stencil_op` | `nv4097::set_stencil_op` | direct | `MTL::StencilDescriptor.setStencilFailureOperation` |
| `M_NV4097_SET_STENCIL_OP_ZFAIL` | 0x340 | `to_stencil_op` | `nv4097::set_stencil_op` | direct | `MTL::StencilDescriptor.setDepthFailureOperation` |
| `M_NV4097_SET_STENCIL_OP_ZPASS` | 0x344 | `to_stencil_op` | `nv4097::set_stencil_op` | direct | `MTL::StencilDescriptor.setDepthStencilPassOperation` |
| `M_NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE` | 0x348 | whole word (bool) | `set_surface_options_dirty_bit` | reshape | `MTL::DepthStencilDescriptor.setBackFaceStencil` |
| `M_NV4097_SET_BACK_STENCIL_MASK` | 0x34c | whole word (u8) | dirty-bit handler | direct | back-face `MTL::StencilDescriptor.setWriteMask` |
| `M_NV4097_SET_BACK_STENCIL_FUNC` | 0x350 | `to_comparison_function` | — (unbound) | direct | back-face `.setStencilCompareFunction` |
| `M_NV4097_SET_BACK_STENCIL_FUNC_REF` | 0x354 | bits 0:8 | — (unbound) | direct | `setStencilReferenceValues(front, back)` |
| `M_NV4097_SET_BACK_STENCIL_FUNC_MASK` | 0x358 | bits 0:8 | — (unbound) | direct | back-face `.setReadMask` |
| `M_NV4097_SET_BACK_STENCIL_OP_FAIL` | 0x35c | `to_stencil_op` | `nv4097::set_stencil_op` | direct | back-face `.setStencilFailureOperation` |
| `M_NV4097_SET_BACK_STENCIL_OP_ZFAIL` | 0x360 | `to_stencil_op` | `nv4097::set_stencil_op` | direct | back-face `.setDepthFailureOperation` |
| `M_NV4097_SET_BACK_STENCIL_OP_ZPASS` | 0x364 | `to_stencil_op` | `nv4097::set_stencil_op` | direct | back-face `.setDepthStencilPassOperation` |
| `M_NV4097_SET_COLOR_MASK_MRT` | 0x370 | 4-bit nibble per RT index | dirty-bit handler | direct | per-attachment `MTL4RenderPipelineColorAttachmentDescriptor.setWriteMask` |
| `M_NV4097_SET_DEPTH_BOUNDS_TEST_ENABLE` | 0x380 | whole word (bool) | — (unbound) | emulate | MSL depth-range discard epilogue |
| `M_NV4097_SET_DEPTH_BOUNDS_MIN` | 0x384 | f32 bound | — (unbound) | emulate | `MTL4ArgumentTable.setAddress` (fragment constant) |
| `M_NV4097_SET_DEPTH_BOUNDS_MAX` | 0x388 | f32 bound | — (unbound) | emulate | `MTL4ArgumentTable.setAddress` (fragment constant) |
| `M_NV4097_ZCULL_SYNC` | 0xa1c | none (no decoder) | — (unbound) | na | none (retirement via `thread::sync`) |
| `M_NV4097_SET_DEPTH_FUNC` | 0xa6c | `to_comparison_function` | dirty-bit handler | direct | `MTL::DepthStencilDescriptor.setDepthCompareFunction` |
| `M_NV4097_SET_DEPTH_MASK` | 0xa70 | whole word (bool) | dirty-bit handler | direct | `MTL::DepthStencilDescriptor.setDepthWriteEnabled` |
| `M_NV4097_SET_DEPTH_TEST_ENABLE` | 0xa74 | whole word (bool) | dirty-bit handler | reshape | `.setDepthCompareFunction` = `Always` when off |
| `M_NV4097_CLEAR_ZCULL_SURFACE` | 0x1438 | none (no decoder) | `nv4097::clear_zcull` | na | none (stub handler; ZCULL memory is host-managed) |
| `M_NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE` | 0x17cc | whole word (bool) | `set_zcull_pixel_count_enable` | reshape | `MTL4RenderCommandEncoder.setVisibilityResultMode(Counting, offset)` |
| `M_NV4097_SET_ZCULL_STATS_ENABLE` | 0x1804 | whole word (bool) | `set_zcull_stats_enable` | emulate | host statistics accumulation from the visibility buffer |
| `M_NV4097_SET_ZMIN_MAX_CONTROL` | 0x1d78 | clip b0, clamp b4, ignore_w b8 | `nv4097::set_zmin_max_control` | reshape | `MTL4RenderCommandEncoder.setDepthClipMode` + MSL depth-range emulation |
| `M_NV4097_SET_ANTI_ALIASING_CONTROL` | 0x1d7c | msaa b0, a2c b4, a2one b8, mask 16:16 | `nv4097::set_aa_control` | reshape | `setRasterSampleCount` + `setAlphaToCoverageState` + `setAlphaToOneState` |
| `M_NV4097_SET_ZCULL_EN` | 0x1d84 | whole word (bool) | `set_zcull_render_enable` | reshape | `setVisibilityResultMode` arm/disarm + host gating |
| `M_NV4097_SET_ZSTENCIL_CLEAR_VALUE` | 0x1d8c | stencil 0:8, z24 8:24 | — (unbound) | direct | pass `RenderPassDepthAttachmentDescriptor.setClearDepth` + `RenderPassStencilAttachmentDescriptor.setClearStencil` |
| `M_NV4097_SET_CLIP_ID_TEST_ENABLE` | 0x1da4 | none (no decoder) | — (unbound) | na | none (clip-id test unimplemented) |
| `M_NV4097_SET_ZCULL_CONTROL0` | 0x1ea4 | none (no decoder) | — (unbound) | na | none (inert; regions arrive via the context attribute) |
| `M_NV4097_SET_ZCULL_CONTROL1` | 0x1ea8 | none (no decoder) | — (unbound) | na | none (inert) |
| `M_NV4097_SET_SCULL_CONTROL` | 0x1eac | none (no decoder) | — (unbound) | na | none (stencil cull unimplemented) |
| `M_NV4097_SET_REDUCE_DST_COLOR` | 0x1fe0 | none (no decoder) | — (unbound) | na | none (ROP destination-read hint; unimplemented) |

Decode offsets are ground truth from `rsx_decode.h`; the per-register decoders are
`SET_COLOR_MASK` (`:3009`), `SET_COLOR_MASK_MRT` (`:3064`), `SET_ZSTENCIL_CLEAR_VALUE` (`:3396`),
`SET_ANTI_ALIASING_CONTROL` (`:4000`), `SET_ZMIN_MAX_CONTROL` (`:624`), `SET_DEPTH_FUNC` (`:2243`),
`SET_STENCIL_FUNC` (`:2266`), `SET_STENCIL_FUNC_REF` (`:2473`), `SET_STENCIL_FUNC_MASK` (`:2519`),
`SET_DEPTH_BOUNDS_*` (`:726`, `:1002`, `:1025`). `[V]`

### 2.2 Value vs dirty-bit state

Two lifetimes coexist and must not be conflated. `[V]`

1. **Value state (in `rsx_state::registers`).** Every register in the table stores its raw word in
   the register file and is read back through a typed accessor — e.g. `depth_bounds_test_enabled()`,
   `depth_bounds_min()`, `depth_bounds_max()` (`rsx_methods.h:428-441`). Crucially, **most of this
   cluster is unbound**: `SET_STENCIL_FUNC`, `SET_STENCIL_FUNC_REF`, `SET_STENCIL_FUNC_MASK`, all
   three back-face func/ref/mask registers, `SET_DEPTH_BOUNDS_*`, `SET_ZSTENCIL_CLEAR_VALUE`,
   `SET_CLIP_ID_TEST_ENABLE`, `SET_ZCULL_CONTROL0/1`, `SET_SCULL_CONTROL`,
   `SET_REDUCE_DST_COLOR`, `INVALIDATE_ZCULL` and `ZCULL_SYNC` all have
   `methods[...] = nullptr` (`rsx_methods.cpp:1303, 1344-1346, 1364-1366, 1391, 1473, 1478,
   1488-1490, 1504`). The absence of a handler does **not** mean the value is unused — it means the
   value is consumed directly by the accessor/decoder, not by a FIFO side effect.
2. **Dirty-bit / signal state.** A handful of registers carry a bound handler whose only job is to
   raise a dirty flag: `set_surface_options_dirty_bit` (`nv4097.cpp:333-340`) is bound to
   `SET_STENCIL_TEST_ENABLE`, `SET_STENCIL_MASK`, `SET_TWO_SIDED_STENCIL_TEST_ENABLE`,
   `SET_BACK_STENCIL_MASK`, `SET_COLOR_MASK_MRT`, `SET_DEPTH_FUNC`, `SET_DEPTH_MASK` and
   `SET_DEPTH_TEST_ENABLE` (`rsx_methods.cpp:1692` and neighbours); `SET_STENCIL_FUNC` is a
   `state_signal` (`rsx_methods.cpp:782`) and `SET_DEPTH_BOUNDS_*` raise
   `depth_bounds_state_dirty` (`rsx_methods.cpp:774-776`). `SET_COLOR_MASK` and `set_stencil_op`
   are *validating* wrappers: they roll the register back to the latch on an invalid decode
   (`nv4097.cpp:349-356`, `:366-373`), so an unvalidated backend would silently accept garbage.

The backend consumes the register file, not the handler callbacks. The handlers exist to schedule
work; the value is always authoritative.

### 2.3 Depth-stencil state (`SET_DEPTH_*`, `SET_STENCIL_*`, two-sided)

The depth triple is `SET_DEPTH_TEST_ENABLE`, `SET_DEPTH_FUNC` (→ `to_comparison_function`,
`gcm_enums.h:1430`) and `SET_DEPTH_MASK`. The stencil quintuple is `FUNC` / `FUNC_REF` /
`FUNC_MASK` / `OP_FAIL` / `OP_ZFAIL` / `OP_ZPASS`, duplicated for the back face, with the
`TWO_SIDED_STENCIL_TEST_ENABLE` gate. `to_stencil_op` (`gcm_enums.h:1567`) is the eight-valued
`keep/zero/replace/incr/decr/invert/incr_wrap/decr_wrap` set. Reset defaults are load-bearing:
depth func `0x201` (`rsx_methods.cpp:354`), stencil func `0x207`, ref `0`, mask `0xff`
(`:373-375`), depth bounds min/max `0.0`/`1.0` (`:351-352`), zstencil clear `0xffffff00`
(`:345`). `[V]`

Two facts fix the Metal shape:

- **Front and back references are dynamic encoder state, not object state.** Metal separates the
  per-face reference (`setStencilReferenceValues(front, back)`) from the immutable
  `MTL::StencilDescriptor` (`MTL4RenderCommandEncoder.hpp:114`). RSX's `FUNC_REF` registers
  therefore do not enter the depth-stencil object; they are encoder setters. `[V]`
- **The enable booleans have no Metal counterpart.** Metal always applies whatever
  `MTL::DepthStencilState` is bound. A disabled depth test is encoded as
  `depthCompareFunction = Always`; a disabled stencil test as a passthrough descriptor (compare
  `Always`, all ops `Keep`); a disabled two-sided test as `backFaceStencil == frontFaceStencil`.
  These are *reshape*, not *direct*: the state survives, the representation changes. `[I]`

### 2.4 Colour write mask (`SET_COLOR_MASK`, `SET_COLOR_MASK_MRT`)

`SET_COLOR_MASK` carries one byte per channel — B at 0:8, G at 8:16, R at 16:24, A at 24:32 — with
`color_write_enabled()` true iff any byte is non-zero and `is_invalid()` flagging a partially-set
mask (`rsx_decode.h:3019-3047`). `SET_COLOR_MASK_MRT` carries a 4-bit group per render-target
index, with A/R/G/B at nibble bits 0/1/2/3 (`:3074-3097`). `[V]` The per-channel mask maps exactly
onto `MTLColorWriteMask`; the MRT form fans out across the colour attachments (A/B/C/D) in the
order established by `SET_SURFACE_COLOR_TARGET` (`surface` cluster). The invalid-mask rollback in
`set_color_mask` (`nv4097.cpp:349-354`) is a desync guard the backend must preserve. `[V]`

### 2.5 Depth clip/clamp and depth bounds

`SET_ZMIN_MAX_CONTROL` decodes `depth_clip_enabled` (bit 0), `depth_clamp_enabled` (bit 4) and
`depth_clip_ignore_w` (bit 8) (`rsx_decode.h:634-647`). The handler raises
`pipeline_config_dirty` and, when the backend lacks extended depth range, `fragment_state_dirty`
so the fragment epilogue can emulate the depth range (`nv4097.cpp:282-288`). `[V]`

`SET_DEPTH_BOUNDS_*` is a separate test: the depth bounds test rejects a fragment whose window-space
depth lies outside `[min, max]`, and it does so **independently of the depth test** — RPCS3
explicitly keeps the Z buffer bound for the bounds check even when the depth buffer is otherwise
unused (`RSXThread.cpp:1591-1597`). Vulkan maps it to `VK_DYNAMIC_STATE_DEPTH_BOUNDS` only when the
device supports it, and disables the test with a warning otherwise
(`VKPipelineCompiler.cpp:114-125`). `[V]` Metal exposes **no depth-bounds test** in
`MTL::DepthStencilDescriptor`; the only fallback is a shader compare (`§3.4`).

### 2.6 Anti-aliasing control (`SET_ANTI_ALIASING_CONTROL`)

`SET_ANTI_ALIASING_CONTROL` decodes `msaa_enabled` (bit 0), `msaa_alpha_to_coverage` (bit 4),
`msaa_alpha_to_one` (bit 8) and a 16-bit `msaa_sample_mask` (bits 16:32) (`rsx_decode.h:4010-4028`).
Reset default `0xffff0000` (`rsx_methods.cpp:499`), i.e. a full sample mask. The *sample count* is
not in this register — it comes from the surface AA format
(`SET_SURFACE_FORMAT.antialias`), where `to_surface_antialiasing` maps the RSX 2-sample mode onto
`square_rotated_4` (`gcm_enums.h:1355-1361`). `set_aa_control` (`nv4097.cpp:376-401`) raises
`pipeline_config_dirty`, and — when the backend does not advertise full hardware A2C
(`supports_hw_a2c`/`supports_hw_a2c_1spp`) — flags `fragment_program_state_dirty` for a shader
emulation. `[V]` Metal's hardware A2C is free (`10 §2.2`; `HLD-05 §2.6`), so the shader path is the
exception, not the rule.

### 2.7 ZCULL (the hard sub-area)

ZCULL state is *not* the FIFO control words. The cull regions are configured out-of-band via
`sys_rsx_context_attribute(0x5555'5555, 0x301, ...)` and stored in `GcmZcullInfo`
(`GCM.h:96-127`: `offset`, `width`, `height`, `cullStart`, `zFormat`, `aaFormat`, `zcullDir`,
`zcullFormat`, `sFunc`, `sRef`, `sMask`, `bound`) in the `zculls[8]` array
(`Core/RSXContext.h:15,39`). The FIFO registers only *enable* and *observe*: `SET_ZCULL_EN`,
`SET_ZCULL_STATS_ENABLE` and `SET_ZPASS_PIXEL_COUNT_ENABLE` are read directly by
`check_zcull_status` (`RSXThread.cpp:2808-2810`), which also matches the current zeta address
against the bound zculls to decide `zcull_surface_active` (`:2812-2835`). `SET_ZCULL_CONTROL0/1`
and `SET_SCULL_CONTROL` are unbound with no reader (`rsx_methods.cpp:1488-1490`); their reset
defaults (`0x10`, `0x1000100`, `0xff000002`, `:561-563`) exist only for the register printer.
`[V]`

The **guest-observable** surface is the report. `ZCULL_control` (`RSXZCULL.h:98`) queues report
writes (`queued_report_write`, `:45-55`), retires them after a delay or under pressure, and exposes
the backend seam `begin/end/check/get/discard_occlusion_query` (`:212-216`). The guest reads a
report through `M_NV4097_GET_REPORT` → `get_zcull_stats` → `ZCULL_control::read_report`
(`RSXThread.cpp:2846-2861`; `HLD-03 §2.2`). The constants `max_zcull_delay_us = 300`,
`min_zcull_tick_us = 100`, `max_safe_queue_depth = 1792` (`RSXZCULL.h:89-96`) define the retirement
latency the guest can observe. `[V]` `CLEAR_ZCULL_SURFACE` is a **stub**: `nv4097::clear_zcull`
only emits a capture event (`nv4097.cpp:568-571`); the statistics clear the guest actually triggers
is `M_NV4097_CLEAR_REPORT_VALUE` → `clear_zcull_stats` (`nv4097.cpp:677-690`;
`RSXThread.cpp:2841`), which lives in the `sync-flip` cluster. `[V]`

---

## 3. Metal 4 mapping

### 3.1 The depth-stencil object

The 19 depth/stencil registers in the table lower onto two objects and one dynamic pair:

| Metal 4 object | Carries | Source registers |
|---|---|---|
| `MTL::DepthStencilDescriptor` | depth compare, depth write, front/back stencil descriptors | `SET_DEPTH_FUNC`, `SET_DEPTH_MASK`, `SET_DEPTH_TEST_ENABLE`, all stencil func/op/mask, two-sided enable |
| `MTL::StencilDescriptor` (front/back) | compare, read/write mask, three ops | `SET_STENCIL_FUNC`, `SET_STENCIL_MASK`, `SET_STENCIL_FUNC_MASK`, `SET_STENCIL_OP_*` (×2 faces) |
| `MTL4RenderCommandEncoder.setStencilReferenceValues(front, back)` | per-face reference (dynamic) | `SET_STENCIL_FUNC_REF`, `SET_BACK_STENCIL_FUNC_REF` |
| `MTL4RenderCommandEncoder.setDepthStencilState` | binds the immutable object | — (the object above) |

`MTL::DepthStencilDescriptor` exposes exactly the fields needed: `setDepthCompareFunction`,
`setDepthWriteEnabled`, `setFrontFaceStencil`, `setBackFaceStencil`; `MTL::StencilDescriptor`
exposes `setStencilCompareFunction`, `setStencilFailureOperation`, `setDepthFailureOperation`,
`setDepthStencilPassOperation`, `setReadMask`, `setWriteMask` (`MTLDepthStencil.hpp:60-120`). The
`MTL::CompareFunction` and `MTL::StencilOperation` enums are the direct images of
`comparison_function` and `stencil_op`. `[V]`

**Cache keying.** Because the object leaves the PSO, it must **not** be folded into the PSO key
(`HLD-05 §3.5`, `:347-348`), or the backend over-creates pipelines whenever only the depth compare
changes. It is keyed and cached separately as an API object (`HLD-05 §3.4`, `:311`). `[I]`

### 3.2 Colour write mask

`MTLColorWriteMask` is a per-channel bitmask carried on the *pipeline* colour-attachment descriptor,
not the encoder: `MTL4RenderPipelineColorAttachmentDescriptor.setWriteMask`
(`MTL4RenderPipeline.hpp:90`). `SET_COLOR_MASK` → attachment 0; `SET_COLOR_MASK_MRT` → attachments
0..3 by nibble. This is the one part of the cluster that stays in the PSO — it changes the pipeline
object, and therefore *does* enter the PSO key (`HLD-05 §2.6`). `[I]`

### 3.3 Depth clip mode and the depth range

`SET_ZMIN_MAX_CONTROL` splits cleanly:

- `depth_clip_enabled` / `depth_clamp_enabled` → `MTL4RenderCommandEncoder.setDepthClipMode` with
  `MTL::DepthClipModeClip` / `MTL::DepthClipModeClamp`
  (`MTL4RenderCommandEncoder.hpp:77-79, 96`). This is genuine encoder state, not a PSO field. `[V]`
- `depth_clip_ignore_w` and the extended-depth-range case (`depth_clip` off) are **not** a Metal
  state; they are handled by the existing fragment-epilogue depth-range emulation, which
  `set_zmin_max_control` already forces dirty when `supports_extended_depth_range` is false
  (`nv4097.cpp:284-288`; `HLD-05 §2.6` ROP `ROP_emulate_depth_range`). `[I]`

### 3.4 Depth bounds — emulation only

`MTL::DepthStencilDescriptor` has no bounds test. `MTL4::RenderCommandEncoder` does
carry `setDepthTestBounds`, but there is no usable implementation behind it: the call
from the first draw that reached an encoder trapped in the driver, `EXC_BREAKPOINT` in
`-[AGXG17FamilyRenderContext_mtlnext setDepthTestMinBound:maxBound:]` with no argument
outside `[0,1]` (`REVIEW-FINDINGS E7`). The chosen
fallback is a fragment-shader compare: the epilogue reads the two bounds (fed through the fragment
constant buffer via `MTL4ArgumentTable.setAddress`, `HLD-05 §3.3`) and `discard_fragment()`s
fragments outside `[min, max]`. This preserves the guest-visible result (the fragment is not
written) but *not* the early-out timing. The alternative — silently dropping the test, as the VK
path does on unsupported devices (`VKPipelineCompiler.cpp:118-125`) — is recorded as a fallback for
bring-up only and is the position the backend holds. Difficulty 4, risk med; whether any live title
exercises the test is open (§6 Q3).
`[I]`

### 3.5 Anti-aliasing, sample count, A2C, sample mask

`SET_ANTI_ALIASING_CONTROL` and the surface AA format lower onto:

- `MTL4RenderPipelineDescriptor.setRasterSampleCount` (`MTL4RenderPipeline.hpp:189`) — the sample
  count, from the surface AA format (not from this register).
- `MTL4RenderPipelineDescriptor.setAlphaToCoverageState` / `setAlphaToOneState`
  (`:175-177`) — the A2C and A2one flags. These are free in hardware (`10 §2.2`). `[V]`
- The 16-bit `msaa_sample_mask` has **no Metal encoder setter** in the MTL4 surface; the sample
  mask is only expressible as the shader `[[sample_mask]]` output (an MSL fragment output), which
  is a per-draw rewrite. This is the one AA sub-facet that is not free (§6 Q2). `[U]`

### 3.6 ZCULL → visibility result buffer + host statistics

The ZCULL lowering has three layers:

1. **ZPASS pixel count.** `SET_ZPASS_PIXEL_COUNT_ENABLE` arms the counting. Metal's equivalent is
   `MTL4RenderCommandEncoder.setVisibilityResultMode(MTL::VisibilityResultModeCounting, offset)`
   with the destination supplied by `MTL4RenderPassDescriptor.setVisibilityResultBuffer`
   (`MTL4RenderCommandEncoder.hpp:127`; `MTL4RenderPass.hpp:97`; `02 #33`; `09 §3 #15`). The
   visibility buffer counts **samples that pass the depth test** — exactly the ZPASS model. `[V]`
2. **Unit enable / stats enable.** `SET_ZCULL_EN` and `SET_ZCULL_STATS_ENABLE` gate whether the
   query is armed at all; they are host-side booleans feeding the visibility-mode calls and the
   statistics accumulation, because Metal has no cull or stats unit. `[I]`
3. **Report fabrication.** The visibility count is read back into the shared report-block
   `MTL::Buffer` and the host accumulates the `ZCULL_STATS*` counters the guest reads through
   `M_NV4097_GET_REPORT`. The sink, the retirement delay and the GPU→CPU visibility point are the
   `sync-flip`/`HLD-03` responsibility (`HLD-03 §3.3`, `:365`; `:196-198`); this cluster only
   states the obligation. `[I]`

**Pass-scoped reset is the trap.** The visibility buffer is reset by the GPU at render-pass start
and is selected per-draw (`02 §4 item 3`; `README §6 R12`). A ZCULL query that spans draws is fine;
one that spans passes needs `MTLVisibilityResultType` accumulation or a manual sum. `[V]`

### 3.7 Zstencil clear value

`SET_ZSTENCIL_CLEAR_VALUE` feeds the render pass, not the encoder:
`MTL::RenderPassDepthAttachmentDescriptor.setClearDepth` and
`MTL::RenderPassStencilAttachmentDescriptor.setClearStencil`
(`MTLRenderPass.hpp:166, 179`), consumed when the pass's load action is `Clear` (`surface` /
`04 §2.6`). The decode is depth-dependent: `clear_stencil` is bits 0:8 and the depth clear is
`clear_z24` (bits 8:24) for a depth-stencil target or `clear_z16` (bits 0:16) otherwise
(`rsx_decode.h:3403-3420`). `[V]`

---

## 4. Impossible / not-recommended

| Item | Status | Fallback actually chosen |
|---|---|---|
| Native **depth bounds test** | **no usable Metal API path** — `MTL::DepthStencilDescriptor` has no bounds field, and the encoder setter that does exist traps in the driver (§3.4, `REVIEW-FINDINGS E7`) | emulate: fragment-shader compare + `discard_fragment()`, bounds fed as fragment constants (§3.4). Not `impossible` because the RSX *result* survives. Bring-up drops it, as the VK path does (`VKPipelineCompiler.cpp:118-125`). `[I]` |
| Native **ZCULL tile cull** | **no Metal API path** — no cull unit | reshape: visibility result buffer for the count (§3.6); the cull itself is simply not performed (a perf loss, never a correctness loss, because the depth test still runs). `[I]` |
| Native **ZCULL statistics unit** | **no Metal API path** | emulate: host accumulation of the report counters from the visibility buffer, retired through the report block (`HLD-03 §3.3`). The read is a correctness obligation. `[I]` |
| Native **per-draw sample mask** | **no MTL4 encoder setter** | shader `[[sample_mask]]` output, per-draw MSL rewrite. `[U]` |
| **Clip-id test** | unimplemented in every backend | drop (register unbound, `rsx_methods.cpp:1478`; only a not-implemented comment, `GLDraw.cpp:325`). `[V]` |
| **Stencil cull** (`SCULL_CONTROL`) | unimplemented, no reader | drop (`rsx_methods.cpp:1490`). `[V]` |
| **ZCULL control words** (`ZCULL_CONTROL0/1`), `REDUCE_DST_COLOR` | inert, no reader | drop; ZCULL regions come from `GcmZcullInfo`, and destination read-back belongs to the ROP epilogue. `[V]` |
| **Folding depth-stencil into the PSO** | expressible but a known trap | **not-recommended**: it over-creates pipelines; key the object separately (`HLD-05 §3.5`). `[I]` |

---

## 5. Invariants & failure modes

**Invariants (must hold at every encoder that draws depth/stencil state).**

1. **Attachment agreement.** A bound `MTL::DepthStencilState` is only legal against a pass whose
   depth/stencil attachment formats match the state's assumptions; the depth/stencil formats come
   from the `surface` cluster (`HLD-02 §2.7`). `[I]`
2. **Depth-stencil is not in the PSO key.** The object is keyed and cached separately; only the
   colour write masks, sample count and A2C/A2one flags remain in the pipeline key
   (`HLD-05 §3.5`). `[I]`
3. **Stencil reference is dynamic.** The `FUNC_REF` registers must be applied through
   `setStencilReferenceValues` after the depth-stencil state is bound and before the draw; they
   never enter the cached object. `[V]`
4. **Depth bounds implies a bound Z buffer.** Even when the depth test is disabled, an active
   depth-bounds test requires the depth surface to be resident and readable
   (`RSXThread.cpp:1591-1597`). `[V]`
5. **ZCULL report ordering.** A report read (`M_NV4097_GET_REPORT`) must be preceded by a GPU→CPU
   visibility point for the retiring write (`HLD-03 §3.3`, `:196-198`). `[V]`
6. **Visibility sink scope.** The visibility buffer is pass-scoped and reset at pass start; a
   query that crosses a pass boundary must accumulate explicitly (`02 §4 item 3`). `[V]`

**Failure modes.**

- **Stale depth-stencil object.** Caching the object by the wrong key (or reusing it across an
  attachment-set change) yields a wrong depth/stencil result, not a crash. The object must be
  re-derived whenever any of the 19 depth/stencil source registers changes. `[I]`
- **PSO churn.** Folding depth-stencil into the PSO key re-creates a pipeline on every depth-func
  change — a perf failure the invariant-2 split prevents (`HLD-05 §3.5`). `[I]`
- **ZCULL report read too early.** Reading the report before the queued write retires returns a
  stale `RsxReport`; the 300 µs / 1792-deep queue (`RSXZCULL.h:89-96`) is the observable bound.
  `HLD-03 §5` lists this as a divergence class caught by H3. `[V]`
- **Visibility reset semantics.** The pass-scoped reset differs from `vkCmdResetQueryPool`; a
  query whose lifetime the guest believes spans passes silently truncates. `[U]` (`README §6 R12`).
- **Dropped depth bounds.** Silently disabling the test writes fragments the guest expects to be
  rejected; only the shader fallback (§3.4) preserves the image. `[I]`
- **Invalid colour mask accepted.** Skipping the `is_invalid()` rollback (`nv4097.cpp:349-354`)
  propagates a garbage mask into `MTLColorWriteMask` and silently drops channels. `[V]`
- **Unvalidated stencil op.** `to_stencil_op` returning empty means the register should roll back
  (`nv4097.cpp:366-373`); accepting it mis-selects an `MTL::StencilOperation`. `[V]`
- **Sample-mask divergence.** Because the sample mask has no encoder setter, any title that varies
  it per draw without a shader rewrite diverges (§6 Q2). `[U]`

---

## 6. Open questions

- **Q1 `[U]`** — ZCULL statistics beyond ZPASS: the report block carries `ZCULL_STATS` and
  `ZCULL_STATS1..3` (`RSXThread.cpp:2854-2857`), but the visibility buffer only yields a
  count-of-samples. Can the non-ZPASS counters be fabricated from the same count, or does a title
  observe a divergence? Needs a title trace.
- **Q2 `[U]`** — Per-draw sample mask: is the MSL `[[sample_mask]]` output sufficient, and can it
  be hoisted into a pipeline constant instead of a per-draw shader rewrite? The MTL4 encoder
  surface has no setter (`§3.5`).
- **Q3 `[U]`** — Depth bounds: emulate in the fragment epilogue from day one, or drop at bring-up
  as the VK path does? Does any live title enable it with a non-default range? `[I]`
- **Q4 `[U]`** — `SET_ZCULL_EN` / `SET_ZCULL_STATS_ENABLE` arming: exactly when is the visibility
  mode set, and how does the pass-scoped reset interact with a ZCULL region that the guest binds
  across passes (`check_zcull_status`, `RSXThread.cpp:2806`)? `[I]`
- **Q5 `[I]`** — Where does the depth-stencil object cache live, and what is its key? It must
  exclude the stencil reference (`§3.1`) but include the front/back stencil descriptors and the
  depth compare/write.
- **Q6 `[U]`** — `CLEAR_ZCULL_SURFACE` semantics: the handler is a stub (`nv4097.cpp:568`). Does
  any title rely on it having cleared something the host must also clear, beyond the
  `CLEAR_REPORT_VALUE` path?
- **Q7 `[U]`** — `SCULL_CONTROL`, `REDUCE_DST_COLOR`, `ZCULL_CONTROL0/1`, `CLIP_ID_TEST_ENABLE`:
  all unbound with no reader. Confirm no live title depends on stencil culling or clip-id testing
  before treating them as permanently inert.
- **Q8 `[U]`** — AA 2-sample: `to_surface_antialiasing` maps the RSX 2-sample mode onto
  `square_rotated_4` (`gcm_enums.h:1360`); does the sample-count/pattern substitution perturb any
  title's edge quality or a ZCULL region's `aaFormat` match (`RSXThread.cpp:2824`)?

---

## 7. Provenance

**In-repo RPCS3 (read-only this session)** `[V]`
- `rpcs3/Emu/RSX/rsx_decode.h` — `SET_ZMIN_MAX_CONTROL:624`, `SET_DEPTH_BOUNDS_TEST_ENABLE:726`,
  `SET_DEPTH_BOUNDS_MIN:1002`, `SET_DEPTH_BOUNDS_MAX:1025`, `SET_DEPTH_FUNC:2243`,
  `SET_STENCIL_FUNC:2266`, `SET_STENCIL_FUNC_REF:2473`, `SET_STENCIL_FUNC_MASK:2519`,
  `SET_COLOR_MASK:3009`, `SET_COLOR_MASK_MRT:3064`, `SET_ZSTENCIL_CLEAR_VALUE:3396`,
  `SET_ANTI_ALIASING_CONTROL:4000`, `SET_ZCULL_EN:4153`, `SET_ZCULL_STATS_ENABLE:4176`,
  `SET_ZPASS_PIXEL_COUNT_ENABLE:4199`.
- `rpcs3/Emu/RSX/NV47/HW/nv4097.cpp` — `set_zmin_max_control:275`, `set_surface_options_dirty_bit:333`,
  `set_color_mask:342`, `set_stencil_op:359`, `set_aa_control:376`, `clear_zcull:568`,
  `clear_report_value:677`, `set_zcull_render_enable:745`, `set_zcull_stats_enable:750`,
  `set_zcull_pixel_count_enable:755`.
- `rpcs3/Emu/RSX/rsx_methods.cpp` — unbound bindings `:1303, 1344-1346, 1364-1366, 1391, 1473,
  1478, 1488-1490, 1504`; state signals `:774-776, 782`; defaults `:252, 345, 351-354, 373-375,
  499, 513, 531, 559-563`; dispatch binds `:1623, 1687-1692, 1720`.
- `rpcs3/Emu/RSX/rsx_methods.h` — `depth_bounds_test_enabled:428`, `depth_bounds_min:433`,
  `depth_bounds_max:438`.
- `rpcs3/Emu/RSX/gcm_enums.h` — `surface_depth_format2:1303`, `to_surface_depth_format:1311`,
  `surface_antialiasing:1347`, `to_surface_antialiasing:1355`, `comparison_function:1418`,
  `to_comparison_function:1430`, `stencil_op:1555`, `to_stencil_op:1567`.
- `rpcs3/Emu/RSX/RSXZCULL.h` — `occlusion_query_info:32`, `queued_report_write:45`,
  `constants:89-96`, `ZCULL_control:98`, `set_status:163`, `read_report:166`, `sync:172`,
  `read_barrier:175-176`, `update:179`, backend virtuals `:212-216`.
- `rpcs3/Emu/RSX/RSXThread.cpp` — `check_zcull_status:2806`, `clear_zcull_stats:2841`,
  `get_zcull_stats:2846`, `enable_conditional_rendering:2877`, `sync:2917`,
  `notify_zcull_info_changed:3227`, register-dump filter `:3194-3202`, depth-bounds Z bind
  `:1591-1597`.
- `rpcs3/Emu/RSX/GCM.h:96-127` (`GcmZcullInfo`), `rpcs3/Emu/RSX/Core/RSXContext.h:15,39`
  (`zculls[8]`), `rpcs3/Emu/RSX/Core/RSXDriverState.h:32` (`depth_bounds_state_dirty`).
- `rpcs3/Emu/RSX/VK/VKPipelineCompiler.cpp:112-125` (VK depth-stencil + depth-bounds gate),
  `VK/VKGSRender.cpp:2889-2994` (occlusion-query backend seam), `VK/VKDraw.cpp:244-276`
  (depth-bounds dynamic state), `GL/GLDraw.cpp:103-104, 325`.
- `rpcs3/Emu/RSX/Common/surface_store.h` — `read_barrier:229`, `clear_rw_barrier:1566/1576`.

**Reference checkouts under `/tmp/metal4-refs/`** `[V]`
- metal-cpp `Metal/MTLDepthStencil.hpp:36-120` — `MTL::CompareFunction`, `MTL::StencilOperation`,
  `StencilDescriptor` (compare, three ops, read/write mask), `DepthStencilDescriptor`
  (depth compare/write, front/back stencil).
- metal-cpp `Metal/MTL4RenderCommandEncoder.hpp:96-114, 127` — `setDepthClipMode`,
  `setDepthStencilState`, `setStencilReferenceValue(s)`, `setVisibilityResultMode`;
  `Metal/MTL4RenderPipeline.hpp:90, 175-189` — colour-attachment `setWriteMask`,
  `setAlphaToCoverageState`, `setAlphaToOneState`, `setRasterSampleCount`;
  `Metal/MTL4RenderPass.hpp:73-111` — depth/stencil attachments + `setVisibilityResultBuffer`;
  `Metal/MTLRenderPass.hpp:166, 179` — `setClearDepth`, `setClearStencil`.

**Sibling research docs (in-repo)** `[V]`
- `design/HLD-05-shader-binding-contract.md` §2.6 (depth-stencil leaves the PSO; colour write mask;
  A2C/A2one), §3.3 (fragment constants), §3.4/§3.5 (object cache and PSO key).
- `design/HLD-03-synchronization-model.md` §2.2 (ZCULL sync surface), §3.3 (visibility buffer,
  report sink, retirement), §5 (ZCULL-late divergence).
- `design/HLD-02-resource-model.md` §2.2 (surfaces), §2.7 (depth formats: `Z16` → `Depth16Unorm`,
  `Z24S8` → `Depth32Float_Stencil8`).
- `02-submission-sync.md` #33 (occlusion → visibility result buffer), §4 item 3 (pass-scoped
  reset); `04-images-formats-samplers-rtt.md` §2.6 (clear mapping); `09` §3 #15; `10` §2.2
  (A2C/A2one free), §4.3 (pass synthesis); `README.md` §6 R12 (occlusion model).
- `12-rsx-state-space-inventory.md:175, 269-271`; `inventory/SCHEMA.md` §4–§6;
  `inventory/out/clusters/depth-rop.csv`; `inventory/out/dispositions/depth-rop.csv`.

<!-- doc-end -->
