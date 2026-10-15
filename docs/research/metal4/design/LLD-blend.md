# LLD — `blend` cluster (RSX blend, alpha test, logic op, shading → Metal 4)

Owner/Scope: RSX-first Metal 4 backend, the `blend` cluster | Status: draft | Date: 2026-10-16 (Fri)
Branch: `metal4`. Design only — nothing under `rpcs3/**` is modified.

**Legend.** `[V]` verified by reading a file in this repo (or a pristine reference under
`/tmp/metal4-refs/`); `[I]` inference from `[V]` facts; `[U]` could not verify. `file:line` is
advisory (the branch moves); **symbol names are the stable reference**. Difficulty 1–5 and the
disposition vocabulary are those of `inventory/SCHEMA.md` §4. Every id below is taken verbatim from
`inventory/out/clusters/blend.csv` (15 ids); this document invents no register.

---

## 1. Scope

The `blend` cluster is the RSX **colour-combiner tail**: the per-render-target blend enable (single
and MRT), the source and destination blend factors and the blend equation (each split RGB/alpha),
the constant blend colour (8-bit and 16-bit halves), the **alpha test** triple (enable, comparison
function, reference), the fixed-function **dither** enable, the **logic op** pair (enable and the
16-valued op), the **shade mode** (flat/smooth), and the inert flat-shade op word.
`12-rsx-state-space-inventory.md:272-273` states the cluster's two hard facts up front: blend and
the alpha test must be lowered — no modern GPU keeps an alpha-test unit — and "logic ops have no
Metal blend path either".

In scope (15 ids, `out/clusters/blend.csv`): `M_NV4097_SET_DITHER_ENABLE`,
`M_NV4097_SET_ALPHA_TEST_ENABLE`, `M_NV4097_SET_ALPHA_FUNC`, `M_NV4097_SET_ALPHA_REF`,
`M_NV4097_SET_BLEND_ENABLE`, `M_NV4097_SET_BLEND_FUNC_SFACTOR`, `M_NV4097_SET_BLEND_FUNC_DFACTOR`,
`M_NV4097_SET_BLEND_COLOR`, `M_NV4097_SET_BLEND_EQUATION`, `M_NV4097_SET_SHADE_MODE`,
`M_NV4097_SET_BLEND_ENABLE_MRT`, `M_NV4097_SET_LOGIC_OP_ENABLE`, `M_NV4097_SET_LOGIC_OP`,
`M_NV4097_SET_BLEND_COLOR2`, `M_NV4097_SET_FLAT_SHADE_OP`.

Out of scope, owned elsewhere: the colour **write mask** (`SET_COLOR_MASK`, `SET_COLOR_MASK_MRT`)
is `depth-rop` (`LLD-depth-rop.md` §2.4/§3.2); MSAA, alpha-to-coverage, alpha-to-one and the sample
mask (`SET_ANTI_ALIASING_CONTROL`) are `depth-rop` (`LLD-depth-rop.md` §2.6/§3.5) and interact with
alpha test here (§2.4, §3.5); the render-target formats and MRT count (`SET_SURFACE_FORMAT`,
`SET_SURFACE_COLOR_TARGET`) are `surface` / `HLD-02`; PSO assembly, the argument-table slot map and
the MSL post-pass are `HLD-05`; the ROP epilogue snippets (`RSXROPEpilogue.glsl`) and the
programmable-blend `[[color(i)]]` read-back are `HLD-05` §2.4/§2.6. This document fixes only how the
15 blend registers lower onto the render-pipeline colour attachments, the render encoder's blend
colour, the fragment shader, and the one genuine gap (logic op).

**The shape change.** On Vulkan the whole cluster is one `pipeline_props` struct hashed into one
`VkPipeline` (`VKPipelineCompiler.h:11-51,226-246`). On Metal 4 the cluster **splits three ways**,
and this split is the central design decision:

1. **Per-attachment blend → the PSO.** `setBlendingState` / `setSourceRGBBlendFactor` /
   `setDestinationRGBBlendFactor` / `setRgbBlendOperation` (and the alpha counterparts) on
   `MTL4RenderPipelineColorAttachmentDescriptor` (`MTL4RenderPipeline.hpp:74-90`). Immutable once
   the pipeline is built; the whole blend state enters the PSO key (`HLD-05 §3.5`).
2. **Alpha test, dither, flat shading → the fragment shader.** MSL is immutable too, so anything
   that changes the shader *code* forces a new MSL/PSO; the alpha function and reference ride the
   fragment-state constant buffer (slot 3, `HLD-05 §3.2`) and the test itself becomes a
   `discard_fragment()` in the ROP epilogue.
3. **Blend colour → the encoder.** The constant colour is per-draw dynamic state:
   `MTL4RenderCommandEncoder.setBlendColor(r,g,b,a)` (`MTL4RenderCommandEncoder.hpp:86`), exactly as
   the VK path uses `vkCmdSetBlendConstants` (`VKDraw.cpp:180-185`).

Logic op is the fourth, degenerate case: **no Metal stage exists**, so it is neither PSO nor shader
state but a correctness gap (`HLD-05 §2.6`, F7).

---

## 2. Register model

### 2.1 The shard

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_DITHER_ENABLE` | 0x0300 | whole word (bool) | — (unbound) | moot | none (dropped) |
| `M_NV4097_SET_ALPHA_TEST_ENABLE` | 0x0304 | whole word (bool) | state_signal (fragment_program_dirty) | emulate | MSL discard in the ROP epilogue |
| `M_NV4097_SET_ALPHA_FUNC` | 0x0308 | `to_comparison_function` | state_signal (fragment_dirty) | emulate | ROP-control bits → `comparison_passes()` |
| `M_NV4097_SET_ALPHA_REF` | 0x030c | 8/16/32b by surface format | state_signal (fragment_dirty) | emulate | `fragment_context_t.alpha_ref` (slot 3) |
| `M_NV4097_SET_BLEND_ENABLE` | 0x0310 | whole word (bool) | state_signal (pipeline + blend dirty) | reshape | `setBlendingState` (attachment 0) |
| `M_NV4097_SET_BLEND_FUNC_SFACTOR` | 0x0314 | rgb 0:16, a 16:32 | `nv4097::set_blend_factor` | direct | `setSourceRGBBlendFactor` / `setSourceAlphaBlendFactor` |
| `M_NV4097_SET_BLEND_FUNC_DFACTOR` | 0x0318 | rgb 0:16, a 16:32 | `nv4097::set_blend_factor` | direct | `setDestinationRGBBlendFactor` / `setDestinationAlphaBlendFactor` |
| `M_NV4097_SET_BLEND_COLOR` | 0x031c | 8b BGRA + 16b RG | — (unbound) | reshape | `MTL4RenderCommandEncoder.setBlendColor` |
| `M_NV4097_SET_BLEND_EQUATION` | 0x0320 | rgb 0:16, a 16:32 | `nv4097::set_blend_equation` | direct | `setRgbBlendOperation` / `setAlphaBlendOperation` |
| `M_NV4097_SET_SHADE_MODE` | 0x0368 | `to_shading_mode` | `nv4097::set_shading_mode` | emulate | MSL flat interpolation qualifier |
| `M_NV4097_SET_BLEND_ENABLE_MRT` | 0x036c | b1/b2/b3 = B/C/D | state_signal (pipeline + blend dirty) | reshape | `setBlendingState` (attachments B/C/D) |
| `M_NV4097_SET_LOGIC_OP_ENABLE` | 0x0374 | whole word (bool) | state_signal (pipeline dirty) | emulate | shader logic-op path selection |
| `M_NV4097_SET_LOGIC_OP` | 0x0378 | `to_logic_op` | state_signal (pipeline dirty) | emulate | MSL bitwise logic over a `[[color(i)]]` fetch |
| `M_NV4097_SET_BLEND_COLOR2` | 0x037c | 16b B 0:16, A 16:32 | — (unbound) | reshape | `setBlendColor` (16-bit half) |
| `M_NV4097_SET_FLAT_SHADE_OP` | 0x1454 | none (no decoder) | — (unbound) | na | none (inert) |

Decode offsets are ground truth from `rsx_decode.h`; the per-register decoders are
`SET_DITHER_ENABLE` (`:772`), `SET_ALPHA_TEST_ENABLE` (`:657`), `SET_ALPHA_FUNC` (`:2312`),
`SET_ALPHA_REF` (`:2565`), `SET_BLEND_ENABLE` (`:795`), `SET_BLEND_FUNC_SFACTOR` (`:2935`),
`SET_BLEND_FUNC_DFACTOR` (`:2972`), `SET_BLEND_COLOR` (`:3869`), `SET_BLEND_EQUATION` (`:2898`),
`SET_SHADE_MODE` (`:4038`), `SET_BLEND_ENABLE_MRT` (`:3154`), `SET_LOGIC_OP_ENABLE` (`:749`),
`SET_LOGIC_OP` (`:2667`), `SET_BLEND_COLOR2` (`:3841`). `[V]`

### 2.2 Value vs dirty-bit state

Two lifetimes coexist and must not be conflated. `[V]`

1. **Value state (in `rsx_state::registers`).** Every register stores its raw word and is read back
   through a typed accessor — `blend_enabled()`, `blend_enabled_mask()`, `blend_func_sfactor_rgb()`,
   `blend_equation_a()`, `alpha_test_enabled()`, `alpha_func()`, `alpha_ref()`, `logic_op_enabled()`,
   `logic_operation()`, `shade_mode()`, `dither_enabled()`, and the six `blend_color_*` accessors
   (`rsx_methods.h:453-507, 614-687, 749-761, 904-907`). Most of this cluster is **unbound**:
   `SET_DITHER_ENABLE`, `SET_ALPHA_TEST_ENABLE`, `SET_ALPHA_FUNC`, `SET_ALPHA_REF`,
   `SET_BLEND_ENABLE`, `SET_BLEND_COLOR`, `SET_BLEND_ENABLE_MRT`, `SET_LOGIC_OP_ENABLE`,
   `SET_LOGIC_OP`, `SET_BLEND_COLOR2` all have `methods[...] = nullptr`
   (`rsx_methods.cpp:1332-1340, 1358-1363`). The absence of a handler does **not** mean the value is
   unused — it is consumed directly by the accessor.
2. **Dirty-bit / signal state.** Registers whose only job is to raise a dirty flag carry it in the
   **separate** `state_signals[]` table, not a method handler: `SET_ALPHA_FUNC` / `SET_ALPHA_REF`
   signal `fragment_state_dirty`, `SET_ALPHA_TEST_ENABLE` signals `fragment_program_state_dirty`
   (`rsx_methods.cpp:720-722`); `SET_LOGIC_OP_ENABLE` / `SET_LOGIC_OP` signal `pipeline_config_dirty`;
   `SET_BLEND_ENABLE` / `SET_BLEND_ENABLE_MRT` signal
   `pipeline_config_dirty + blend_config_dirty` (`:778-781`). The FIFO applies them at decode
   (`RSXFIFO.cpp:928`); a register may be either a method **or** a signal, never both
   (`rsx_methods.cpp:790-793`).
3. **Validating bound handlers.** `SET_BLEND_FUNC_SFACTOR` / `SET_BLEND_FUNC_DFACTOR` bind to
   `nv4097::set_blend_factor` and `SET_BLEND_EQUATION` binds to `nv4097::set_blend_equation`
   (`rsx_methods.cpp:1717-1719`). Both are **validating wrappers**: they decode *both* 16-bit halves
   and, on an invalid enum, roll the register back to the latch and do **not** raise the dirty bit
   (`nv4097.cpp:601-635`). An unvalidated backend would silently accept garbage into
   `MTLBlendFactor` / `MTLBlendOperation`. The desync guard must be preserved. `[V]`
4. `SET_SHADE_MODE` is also bound (`nv4097::set_shading_mode`, `rsx_methods.cpp:1709`) and raises
   `vertex_program_state_dirty + fragment_program_state_dirty` (`nv4097.cpp:728-743`), because flat
   shading is a shader-code change on both stages.

Reset defaults are load-bearing (`rsx_methods.cpp:328-344, 357, 362-363, 372`): alpha func `0x207`
(`Always`), alpha ref `0`, alpha test off, blend colour/colour2 `0`, blend enable off, blend
equation `0x80068006` (`Add`/`Add`), sfactor `0x10001` (`One`/`One`), dfactor `0` (`Zero`/`Zero`),
dither on (`0x1`), logic op off with op `0x1503` (`Copy`), shade mode `0x1d01` (`smooth`). `[V]`

### 2.3 Blend state (enable, factors, equation, per-MRT enable)

The blend enable is a 4-bit **mask**, not a boolean: `blend_enabled_mask()` returns
`(base_bit_0 | registers[NV4097_SET_BLEND_ENABLE_MRT]) & 0xf` (`rsx_methods.h:493-497`). The base
`SET_BLEND_ENABLE` bit is render-target 0; `SET_BLEND_ENABLE_MRT` supplies bits 1/2/3 for surfaces
B/C/D (`rsx_decode.h:3164-3177`). The factors and equation are each one register carrying an RGB
half at bits 0:16 and an alpha half at bits 16:32 (`rsx_decode.h:2905-2925, 2942-2962, 2979-2999`);
there is no separate per-RT factor set, so all enabled attachments share one blend config — exactly
the VK loop at `VKGSRender.cpp:332-348`. The equation enum is the eight-valued `blend_equation`
(`gcm_enums.h:1577`), the factor enum the fifteen-valued `blend_factor` (`gcm_enums.h:1598`). `[V]`

### 2.4 Alpha test (enable, function, reference)

The alpha test has **no Metal state**: it is a per-fragment comparison of the output alpha against
`alpha_ref` that either keeps or `discard`s the fragment. RPCS3 already lowers it into the shader,
gated by `RSX_SHADER_CONTROL_ALPHA_TEST` (`RSXThread.cpp:2440-2443`) which sets the `ROP_alpha_test`
shader property (`VKFragmentProgram.cpp:413`) and defines `_ENABLE_ALPHA_TEST`
(`GLSLCommon.cpp:266-268`). The epilogue reads the comparison function from the packed ROP-control
word and the reference from the fragment-state buffer, then discards:
`if (!comparison_passes(_alpha_quantize(col0.a), alpha_ref, alpha_func)) discard;`
(`RSXROPEpilogue.glsl:26-39`). `comparison_passes` is the eight-valued
`never/less/equal/lequal/greater/nequal/gequal/always` map (`RSXFragmentPrologue.glsl:136-149`),
matching `to_comparison_function` (`gcm_enums.h:1430`). `[V]`

Two RSX quirks are preserved by the accessors and must stay: `alpha_test_enabled()` returns **false**
for `x32` / `w32z32y32x32` surfaces regardless of the register (`rsx_methods.h:274-284`), and
`alpha_ref()` selects an 8-, 16- or 32-bit decode from the *surface format*
(`rsx_methods.h:749-761`; decoders `rsx_decode.h:2575-2588`). Both values are packed into the
slot-3 fragment-state payload by `fill_fragment_state_buffer`: the function into
`rop_control.set_alpha_test_func(...)` and the reference into `payload.alpha_ref`
(`RSXDrawCommands.cpp:665-718`; struct at `:668-677`). `[V]`

**Alpha-to-coverage interaction.** A2C is owned by `depth-rop` (`SET_ANTI_ALIASING_CONTROL`), and
Metal's hardware A2C is free (`10 §2.2:102`; `LLD-depth-rop.md` §2.6/§3.5). Alpha test and A2C are
independent tests that both consume the shader's alpha and both run in the ROP epilogue when
emulated: alpha test discards (`RSXROPEpilogue.glsl:26-39`), the A2C fallback discards on a
stochastic coverage test (`:41-46`). When hardware A2C is available the second block is absent and
only the alpha test remains; the ordering (alpha test before the coverage test) is fixed by the
epilogue. `[V]`

### 2.5 Blend colour

The constant blend colour is assembled from **two** registers. `SET_BLEND_COLOR` carries 8-bit BGRA
at bytes 0/8/16/24 *and* a 16-bit R/G pair (`rsx_decode.h:3879-3907`); `SET_BLEND_COLOR2` carries
16-bit B/A (`rsx_decode.h:3851-3859`). `get_constant_blend_colors()` chooses the 16-bit path only
for `w16z16y16x16` surfaces (decoding fp16), else the 8-bit path scaled by `1/255`
(`rsx_utils.cpp:82-103`). The result is a 4-float RGBA vector bound as encoder dynamic state. `[V]`

### 2.6 Logic op

`SET_LOGIC_OP` decodes the sixteen-valued `logic_op` enum (`to_logic_op`, `gcm_enums.h:1646`;
`logic_op`, `:1626`); `SET_LOGIC_OP_ENABLE` gates it. RPCS3's rule is explicit: "LogicOp and Blend
are mutually exclusive. If both are enabled, LogicOp takes precedence" (`VKGSRender.cpp:322-326`).
Vulkan realises it as a `VkLogicOp` on the pipeline (`get_logic_op`, `VKGSRender.cpp:112-135`);
**Metal has no logic-op stage at all** — there is no `MTLLogicOperation` in the metal-cpp surface
(a case-insensitive search for `logicop`/`logicoperation` under `metal-cpp/` returns nothing). This
is the cluster's genuine gap (`HLD-05 §2.6`, F7; `12 §5.2`). `[V]`

### 2.7 Dither

`SET_DITHER_ENABLE` decodes a plain boolean (`rsx_decode.h:782`), reset default `0x1`
(`rsx_methods.cpp:357,994`), unbound (`rsx_methods.cpp:1332`). It is a legacy fixed-function
feature: the GL backend maps it to `GL_DITHER` (`GLDraw.cpp:81`), which is implementation-defined
and a no-op on modern drivers; the VK backend ignores it entirely. Metal exposes no dither state.
The design recommendation is to **drop** it, not to add a shader dither (a trap that perturbs output
for no guest-observable reason). `[I]`

### 2.8 Shade mode

`SET_SHADE_MODE` decodes `smooth`/`flat` (`to_shading_mode`, `gcm_enums.h:1706`). RPCS3 enables the
flat path only when the backend advertises `supports_last_provoking_vertex`, setting
`RSX_SHADER_CONTROL_FLAT_SHADING` on the vertex and fragment programs
(`RSXThread.cpp:2368-2373, 2415-2419`), which the decompiler turns into a `flat` interpolation
qualifier on the `diff_color`/`spec_color` varyings (`GLSLCommon.cpp:736-748`). Metal exposes **no
public provoking-vertex mode**; MoltenVK gates `provokingVertexLast` on a private API, so
`supports_last_provoking_vertex` is forced **false** and the backend falls back to smooth
interpolation (`10 §2.2:109`; `VKGSRender.cpp:751-752`). `[V]`/`[I]`

### 2.9 Inert registers

`SET_FLAT_SHADE_OP` has `has_decoder=0` (`clusters/blend.csv:16`), no handler
(`rsx_methods.cpp:1416`), no accessor and no reader; it appears only in the register printer
(`gcm_printing.cpp:327`) and as a bare comment in the GL backend (`GLDraw.cpp:322`). It is inert. `[V]`

---

## 3. Metal 4 mapping

### 3.1 Per-attachment blend state (the PSO)

The blend enable, factors and equation all lower onto the render-pipeline **colour-attachment
descriptor** for each enabled RT index:

| Metal 4 object / method | Carries | Source registers |
|---|---|---|
| `MTL4RenderPipelineColorAttachmentDescriptor.setBlendingState` | enabled / disabled | `SET_BLEND_ENABLE`, `SET_BLEND_ENABLE_MRT` |
| `.setSourceRGBBlendFactor` / `.setSourceAlphaBlendFactor` | source factor | `SET_BLEND_FUNC_SFACTOR` |
| `.setDestinationRGBBlendFactor` / `.setDestinationAlphaBlendFactor` | destination factor | `SET_BLEND_FUNC_DFACTOR` |
| `.setRgbBlendOperation` / `.setAlphaBlendOperation` | equation | `SET_BLEND_EQUATION` |

`setBlendingState` takes `MTL4::BlendState` (`BlendStateDisabled` / `BlendStateEnabled`,
`MTL4PipelineState.hpp:43-47`) and is the Metal analogue of the missing "blend enable" boolean — a
*reshape*, not a *direct* mapping. For each `idx` in `blend_enabled_mask()`, the descriptor at
`colorAttachments()->object(idx)` is configured; attachments whose bit is clear get
`BlendStateDisabled`. This is the Metal form of the VK loop (`VKGSRender.cpp:341-348`). All four
setters are confirmed in the metal-cpp surface (`MTL4RenderPipeline.hpp:74-90`). `[V]`

### 3.2 Blend factor enum mapping (exhaustive)

All fifteen `blend_factor` values have a direct `MTLBlendFactor` (`MTLRenderPipeline.hpp:67-88`);
the mapping is total and injective, so no factor needs emulation. `[V]`

| RSX `blend_factor` | `CELL_GCM_*` | `MTLBlendFactor` | note |
|---|---|---|---|
| `zero` | 0x0000 | `BlendFactorZero` | `[V]` |
| `one` | 0x0001 | `BlendFactorOne` | `[V]` |
| `src_color` | 0x0300 | `BlendFactorSourceColor` | `[V]` |
| `one_minus_src_color` | 0x0301 | `BlendFactorOneMinusSourceColor` | `[V]` |
| `src_alpha` | 0x0302 | `BlendFactorSourceAlpha` | `[V]` |
| `one_minus_src_alpha` | 0x0303 | `BlendFactorOneMinusSourceAlpha` | `[V]` |
| `dst_alpha` | 0x0304 | `BlendFactorDestinationAlpha` | `[V]` |
| `one_minus_dst_alpha` | 0x0305 | `BlendFactorOneMinusDestinationAlpha` | `[V]` |
| `dst_color` | 0x0306 | `BlendFactorDestinationColor` | `[V]` |
| `one_minus_dst_color` | 0x0307 | `BlendFactorOneMinusDestinationColor` | `[V]` |
| `src_alpha_saturate` | 0x0308 | `BlendFactorSourceAlphaSaturated` | `[V]` |
| `constant_color` | 0x8001 | `BlendFactorBlendColor` | reads encoder blend colour (§3.4) |
| `one_minus_constant_color` | 0x8002 | `BlendFactorOneMinusBlendColor` | reads encoder blend colour |
| `constant_alpha` | 0x8003 | `BlendFactorBlendAlpha` | reads encoder blend colour |
| `one_minus_constant_alpha` | 0x8004 | `BlendFactorOneMinusBlendAlpha` | reads encoder blend colour |

The enum is decoded from the RGB and alpha halves separately (`to_blend_factor`,
`gcm_enums.h:1617`; VK parity `VKGSRender.cpp:137-159`). `MTLBlendFactorSource1*` and
`BlendFactorUnspecialized` have no RSX source and are never emitted. `[V]`

### 3.3 Blend equation enum mapping (exhaustive)

`MTLBlendOperation` has five values (`MTLRenderPipeline.hpp:90-97`). Five of the eight RSX
equations map directly; three **signed** equations have no equivalent and must fall back. `[V]`

| RSX `blend_equation` | `CELL_GCM_*` | `MTLBlendOperation` | note |
|---|---|---|---|
| `add` | 0x8006 | `BlendOperationAdd` | direct |
| `subtract` | 0x800A | `BlendOperationSubtract` | direct |
| `reverse_subtract` | 0x800B | `BlendOperationReverseSubtract` | direct |
| `min` | 0x8007 | `BlendOperationMin` | direct |
| `max` | 0x8008 | `BlendOperationMax` | direct |
| `add_signed` | 0xF006 | `BlendOperationAdd` | **no equivalent**; emulated as `Add` |
| `reverse_add_signed` | 0xF007 | `BlendOperationAdd` | **no equivalent**; emulated as `Add` |
| `reverse_subtract_signed` | 0xF005 | `BlendOperationReverseSubtract` | **no equivalent**; emulated as `ReverseSubtract` |

The fallback matches the VK backend, which logs an error and falls through to the unsigned op
(`VKGSRender.cpp:165-176`). The signed equations apply `(src - 0.5)` / `(0.5 - dst)` biases that
Metal cannot express in the fixed-function blender; the substitution is exact only when the source
or destination is in `[0,1]` with no bias, so it is recorded as an approximation (§4). `[V]`/`[I]`

### 3.4 Blend colour (dynamic encoder state)

The 4-float constant colour is bound per draw with
`MTL4RenderCommandEncoder.setBlendColor(r, g, b, a)` (`MTL4RenderCommandEncoder.hpp:86`). It is
**encoder state**, not PSO state, so it must be re-applied whenever it changes and must not enter
the PSO key. `get_constant_blend_colors()` (`rsx_utils.cpp:82-103`) supplies the vector; the VK path
applies it only when any blend target is enabled (`VKDraw.cpp:180-185`), which is the correct guard
since the constant factors are the only consumers. `[V]`/`[I]`

### 3.5 Alpha test → fragment discard

There is no `MTLAlphaTest`. The lowering is the existing shader path, carried unchanged into MSL:
`RSX_SHADER_CONTROL_ALPHA_TEST` (`RSXThread.cpp:2440-2443`) gates the epilogue's
`comparison_passes(_alpha_quantize(col0.a), alpha_ref, alpha_func)` discard
(`RSXROPEpilogue.glsl:26-39`). The function and reference are fragment-state constants (slot 3,
`HLD-05 §3.2`): `rop_control` and `alpha_ref` (`RSXDrawCommands.cpp:665-718`), so the alpha function
and reference are **not** PSO-key material — they ride the argument table and can change per draw
without a new pipeline, exactly as `fragment_state_dirty` implies (`rsx_methods.cpp:720-721`). The
one thing that *is* PSO-key material is the alpha-test **enable**, because toggling it adds or
removes the `discard` and therefore changes the MSL (`_ENABLE_ALPHA_TEST`, `GLSLCommon.cpp:266-268`);
`SET_ALPHA_TEST_ENABLE` correctly raises `fragment_program_state_dirty`
(`rsx_methods.cpp:722`). `[V]`/`[I]`

### 3.6 Logic op — the genuine gap

Metal 4 has no logic-op stage, and the encoder surface confirms it (no logic-op setter in
`MTL4RenderCommandEncoder.hpp`). The only faithful realisation is to fold the bitwise op into the
fragment shader over a **framebuffer fetch** of the current attachment — the same
`[[color(i)]]` read-back the programmable-blend path already needs (`HLD-05 §2.6` T3). The chosen
fallback is therefore: (a) if logic op is enabled, select a shader variant that reads
`[[color(i)]]`, applies the boolean function of (source, destination), and writes the result; (b)
at bring-up, if no such variant exists, drop the logic op and leave the fragment as-is. The
precedence rule must be honoured in either case: when `logic_op_enabled()` is set, blend is
suppressed (`VKGSRender.cpp:322-326`), so the PSO for that draw must carry
`BlendStateDisabled` on every attachment. Difficulty 4, risk med; whether any live title exercises
it is open (§6 Q4). `[I]`

### 3.7 Dither and shade mode

- **Dither.** No Metal state; dropped (§2.7). No PSO or shader change. `[I]`
- **Shade mode.** The flat path is the MSL `flat` interpolation qualifier, selected by
  `RSX_SHADER_CONTROL_FLAT_SHADING` (`GLSLCommon.cpp:736-748`) and therefore a **shader-cache**
  input, not a PSO field. Because `supports_last_provoking_vertex` is false (§2.8), the flag is
  never set on Metal and the backend renders smooth — a visible-quality divergence, not a crash
  (`10 §4.5`). `[I]`

### 3.8 PSO cache key

The blend subset **does** enter the PSO key, because the per-attachment descriptors are immutable
pipeline state: the four enable bits (`blend_enabled_mask()`), the four factors (RGB/alpha source
and destination) and the two equations (RGB/alpha) all belong to the key, alongside the attachment
formats (`HLD-05 §3.5`, `VKPipelineCompiler.h:16-49` minus depth-stencil). The **blend colour does
not** — it is encoder state (§3.4). The **alpha function and reference do not** — they are
fragment-state constants (§3.5); only the alpha-test *enable* (a shader-code toggle) enters the
shader key. The **logic-op enable and op** would enter a shader key if the shader-fetch fallback is
chosen, and otherwise are unsupported. `[I]`

---

## 4. Impossible / not-recommended

| Item | Status | Fallback actually chosen |
|---|---|---|
| Native **logic op** | **no Metal API path** — no logic-op stage exists | emulate: framebuffer-fetch + bitwise op in the ROP epilogue, reusing the programmable-blend machinery (§3.6); drop at bring-up. Precedence over blend is honoured. `[I]` |
| **Signed blend equations** (`add_signed`, `reverse_add_signed`, `reverse_subtract_signed`) | **no `MTLBlendOperation`** | emulate as `Add` / `Add` / `ReverseSubtract` (§3.3), matching VK (`VKGSRender.cpp:165-176`); exact only without the `±0.5` bias. `[V]`/`[I]` |
| **Flat shading with the last provoking vertex** | **no public provoking-vertex mode** | smooth fallback: `supports_last_provoking_vertex` forced false, `RSX_SHADER_CONTROL_FLAT_SHADING` never set (§2.8, §3.7). The MSL `flat` qualifier is a **not-recommended** substitute because Metal's provoking vertex differs from RSX's. `[V]`/`[I]` |
| Native **fixed-function alpha test** | **no Metal API path** | emulate: fragment `discard` in the ROP epilogue (§3.5); already implemented backend-agnostically. `[V]` |
| **Dither** | **no Metal API path**; no guest-observable effect | drop (§2.7); a shader dither is **not-recommended**. `[I]` |
| **Per-RT independent blend factors** | expressible only as one shared config | the RSX register file has no per-RT factor set (§2.3), so all enabled attachments share one config, as in VK. No divergence. `[V]` |
| **Blend colour in the PSO key** | expressible but a known trap | **not-recommended**: it is encoder state (§3.4); folding it into the key would over-create pipelines. `[I]` |

---

## 5. Invariants & failure modes

**Invariants (must hold at every encoder that draws with blend).**

1. **Blend state is PSO state; blend colour is encoder state.** Factors, equations and the enable
   mask are immutable pipeline fields; the constant colour is applied with `setBlendColor` after the
   pipeline is bound and before the draw (`HLD-05 §3.5`; §3.4). `[I]`
2. **Logic-op precedence.** When `logic_op_enabled()` is set, every attachment's
   `setBlendingState` is `Disabled`; blend and logic op never combine (`VKGSRender.cpp:322-326`). `[V]`
3. **Validated enums.** An invalid factor or equation rolls the register back and raises no dirty
   bit (`nv4097.cpp:601-635`); the backend must consume the register file (already rolled back), not
   the raw FIFO argument. `[V]`
4. **Alpha test is a shader effect.** The comparison function and reference are fragment constants;
   the enable is a shader-code toggle. No `MTL` object carries an alpha test. `[V]`
5. **Alpha-test surface-format gate.** `alpha_test_enabled()` is false for `x32` /
   `w32z32y32x32` regardless of the register (`rsx_methods.h:274-284`); the backend must use the
   accessor, not the raw bit. `[V]`
6. **Constant factors imply a bound blend colour.** Any enabled attachment using
   `BlendFactorBlendColor`/`BlendAlpha` requires `setBlendColor` to have been called for the draw
   (§3.4). `[I]`
7. **Attachment-set agreement.** `blend_enabled_mask()` is masked against the actual MRT count
   (`VKGSRender.cpp:332`); configuring an attachment index with no corresponding pass attachment
   fails PSO creation (`HLD-05 §4.4`). `[I]`

**Failure modes.**

- **Unvalidated blend enum.** Accepting a raw factor/equation without the rollback check
  (`nv4097.cpp:619-635`) mis-selects an `MTLBlendFactor`/`MTLBlendOperation` and produces a wrong
  colour, not a crash. `[V]`
- **Blend colour folded into the PSO key.** Over-creates pipelines on every constant-colour change;
  the invariant-1 split prevents it. `[I]`
- **Missing blend-colour apply.** A pipeline using a constant factor with no `setBlendColor` reads
  the encoder's stale/white colour; the enabled-mask guard (`VKDraw.cpp:180-185`) must be mirrored. `[I]`
- **Logic op silently dropped.** A title that relies on `SET_LOGIC_OP` writes a blended image the
  guest did not expect; only the shader-fetch fallback (§3.6) preserves the image. `[I]`
- **Signed-equation approximation.** `add_signed` / `reverse_add_signed` / `reverse_subtract_signed`
  rendered as unsigned ops diverge wherever the `±0.5` bias is material (§3.3, §4). `[I]`
- **Smooth fallback for flat shading.** Any title using `SHADE_MODE=flat` on a primitive whose
  vertices disagree on the flat colour interpolates instead of holding the provoking vertex's value
  (§2.8). `[U]` (§6 Q2)
- **Alpha-test enable vs constants.** Treating the alpha *function* as PSO-key material over-creates
  pipelines; treating the alpha-test *enable* as constant-only under-invalidates the shader. The
  split is §3.5. `[I]`

---

## 6. Open questions

- **Q1 `[U]`** — Does any live title exercise the signed blend equations (`add_signed`,
  `reverse_add_signed`, `reverse_subtract_signed`)? The VK backend logs an error and emulates; if no
  title uses them the approximation is free, otherwise the `±0.5` bias needs a shader fallback.
- **Q2 `[U]`** — Which titles regress under the smooth-interpolation fallback for `SHADE_MODE=flat`
  (`10 §2.2:109`)? This is the same open item as `10 §5` item 6.
- **Q3 `[U]`** — Alpha-test quantisation: the epilogue quantises `col0.a` before the compare
  (`RSXROPEpilogue.glsl:27-35`); does that ordering survive the SPIRV-Cross → MSL path for all
  surface formats, and is the `x32`/`w32z32y32x32` special case preserved?
- **Q4 `[U]`** — Logic op: is the framebuffer-fetch + bitwise fallback (§3.6) sufficient, and does
  any title enable it? If not, it can be dropped at bring-up like the VK device-unsupported path.
- **Q5 `[U]`** — Logic-op fetch semantics: does `[[color(i)]]` return the exact stored bits for
  integer/unorm targets so a bitwise op is faithful, or is a format-qualified fetch needed?
- **Q6 `[I]`** — Dither: confirm dropping it (`moot`) rather than emulating, i.e. that no title's
  output depends on `GL_DITHER`-equivalent behaviour.
- **Q7 `[U]`** — Is the blend colour ever 16-bit (`w16z16y16x16` surfaces) in practice, and does the
  fp16 decode (`rsx_utils.cpp:85-92`) need to be exercised, or is the 8-bit path sufficient for all
  live titles?
- **Q8 `[I]`** — Where does the per-attachment blend descriptor assembly live (a blend-state cache
  keyed by the enable mask + factors + equations), and is it shared with the colour-write-mask path
  in `depth-rop` (`LLD-depth-rop.md` §3.2), which writes the same descriptor array?

---

## 7. Provenance

**In-repo RPCS3 (read-only this session)** `[V]`
- `rpcs3/Emu/RSX/rsx_decode.h` — `SET_ALPHA_TEST_ENABLE:657`, `SET_LOGIC_OP_ENABLE:749`,
  `SET_DITHER_ENABLE:772`, `SET_BLEND_ENABLE:795`, `SET_ALPHA_FUNC:2312`, `SET_ALPHA_REF:2565`,
  `SET_LOGIC_OP:2667`, `SET_BLEND_EQUATION:2898`, `SET_BLEND_FUNC_SFACTOR:2935`,
  `SET_BLEND_FUNC_DFACTOR:2972`, `SET_BLEND_ENABLE_MRT:3154`, `SET_BLEND_COLOR2:3841`,
  `SET_BLEND_COLOR:3869`, `SET_SHADE_MODE:4038`.
- `rpcs3/Emu/RSX/gcm_enums.h` — `to_comparison_function:1430`, `blend_equation:1577`,
  `to_blend_equation:1589`, `blend_factor:1598`, `to_blend_factor:1617`, `logic_op:1626`,
  `to_logic_op:1646`, `shading_mode:1700`, `to_shading_mode:1706`; `CELL_GCM_*` constants
  `:182-207, 311-321, 990-1004, 1119-1120`.
- `rpcs3/Emu/RSX/NV47/HW/nv4097.cpp` — `set_blend_equation:601`, `set_blend_factor:619`,
  `set_shading_mode:728`, `set_aa_control:376`.
- `rpcs3/Emu/RSX/rsx_methods.cpp` — `state_signals` `:720-722, 778-781`; unbound `:1332-1340,
  1358-1363`; `SET_FLAT_SHADE_OP` unbound `:1416`; dispatch binds `:1709, 1717-1719`; defaults
  `:328-344, 357, 362-363, 372, 965-1009`; method-vs-signal sanity check `:790-793`.
- `rpcs3/Emu/RSX/rsx_methods.h` — `logic_op_enabled:453`, `dither_enabled:468`, `blend_enabled:473`,
  `blend_enabled_surface_1..3:478-491`, `blend_enabled_mask:493`, `shade_mode:504`, blend-colour
  accessors `:614-652`, blend-equation accessors `:654-662`, blend-factor accessors `:664-682`,
  `logic_operation:684`, `alpha_ref:749`, `alpha_func:904`, `alpha_test_enabled:274`.
- `rpcs3/Emu/RSX/Core/RSXDrawCommands.cpp` — `fill_fragment_state_buffer:665`,
  `fragment_context_t:668-677`, `set_alpha_test_func:684-685`, `payload.alpha_ref:718`.
- `rpcs3/Emu/RSX/Program/GLSLSnippets/RSXProg/RSXROPEpilogue.glsl:26-46` (alpha test + A2C discard);
  `RSXFragmentPrologue.glsl:108` (`coverage_test_passes`), `:136` (`comparison_passes`).
- `rpcs3/Emu/RSX/Program/GLSLCommon.cpp:266-268` (`_ENABLE_ALPHA_TEST`), `:344-346`
  (`_ENABLE_ALPHA_TO_COVERAGE_TEST`), `:736-748` (flat qualifier).
- `rpcs3/Emu/RSX/VK/VKFragmentProgram.cpp:413-414` (ROP properties), `:566-569` (alpha_ref read).
- `rpcs3/Emu/RSX/RSXThread.cpp:2368-2373, 2415-2419` (flat shading gate), `:2440-2453`
  (alpha test + A2C control bits).
- `rpcs3/Emu/RSX/VK/VKGSRender.cpp:112-135` (`get_logic_op`), `:137-159` (`get_blend_factor`),
  `:161-182` (`get_blend_op`), `:322-348` (logic-op precedence + blend loop), `:751-752`
  (`supports_last_provoking_vertex`).
- `rpcs3/Emu/RSX/VK/VKDraw.cpp:180-185` (blend constants), `rpcs3/Emu/RSX/Utils/rsx_utils.cpp:82-103`
  (`get_constant_blend_colors`), `rpcs3/Emu/RSX/GL/GLDraw.cpp:81, 322`,
  `rpcs3/Emu/RSX/gcm_printing.cpp:327`, `rpcs3/Emu/RSX/RSXFIFO.cpp:928` (state-signal apply),
  `rpcs3/Emu/RSX/Core/RSXDriverState.h:13, 29, 34, 47` (dirty flags).

**Reference checkouts under `/tmp/metal4-refs/`** `[V]`
- metal-cpp `Metal/MTLRenderPipeline.hpp:67-88` (`MTL::BlendFactor`), `:90-97`
  (`MTL::BlendOperation`), `:130-137` (`MTL::ColorWriteMask`).
- metal-cpp `Metal/MTL4RenderPipeline.hpp:74-90` (colour-attachment blend setters),
  `:175-177` (`setAlphaToCoverageState` / `setAlphaToOneState`), `:189` (`setRasterSampleCount`).
- metal-cpp `Metal/MTL4PipelineState.hpp:33-47` (`AlphaToOneState`, `AlphaToCoverageState`,
  `BlendState`), `Metal/MTL4RenderCommandEncoder.hpp:86` (`setBlendColor`). No logic-op symbol exists
  anywhere under `metal-cpp/` (case-insensitive search).

**Sibling research docs (in-repo)** `[V]`
- `design/HLD-05-shader-binding-contract.md` §2.6 (the blend-cluster split: PSO + shader + encoder;
  logic op as a genuine gap), §3.2 (slot map), §3.3 (fragment constants / push-constant
  substitution), §3.5 (PSO assembly and cache key), §4.4 (attachment/format agreement), F7 (logic
  op).
- `design/LLD-depth-rop.md` §2.4/§3.2 (colour write mask on the same descriptor), §2.6/§3.5
  (A2C/MSAA interaction).
- `10-rsx-native-metal4-architecture.md` §2.2 (A2C/A2one free, `supports_last_provoking_vertex`
  false), §4.5 (visible-quality divergences), §4.6 (shader/pipeline model), §5 item 6 (provoking
  vertex); `12-rsx-state-space-inventory.md:272-273`; `inventory/SCHEMA.md` §4–§6;
  `inventory/out/clusters/blend.csv`; `inventory/out/dispositions/blend.csv`.

<!-- doc-end -->
