# HLD-05 — Shader translation & binding contract (RSX CgBinary VP/FP → MSL)

Owner/Scope: RSX-first Metal 4 backend, shader/pipeline boundary | Status: draft | Date: 2026-10-16 (Fri)
Branch: `metal4`. Design only — nothing under `rpcs3/**` is modified.

**Legend used throughout:** `[V]` verified by reading a file in this repo or a primary external
source; `[I]` inference from verified facts; `[U]` could not verify.
`file:line` is advisory (the branch moves); **symbol names are the stable reference**.

---

## 1. Purpose & non-goals

### 1.1 Purpose
This document fixes the **contract** that joins RSX shader state to a Metal 4 backend: how guest
shader microcode and the fixed-function registers around it become (a) Metal Shading Language
source, (b) one flat per-stage binding index space realised as `MTL4::ArgumentTable`, and (c) an
`MTL4::RenderPipelineState` plus its cache key. It is the boundary that scopes 01 (device/residency),
02 (encoders/passes), and 04 (textures/samplers/RTT) bind into: they supply objects for indices this
document defines, and consume a pipeline object this document defines.

Scope 03 (`03-shaders-pipelines-descriptors.md`) already established the *choice* of the
GLSL → SPIR-V → SPIRV-Cross → MSL path and surveyed the per-construct mapping. HLD-05 is narrower
and deeper: it writes down the **stable index map, the PSO assembly recipe, the cache key, and the
fixed-function lowering** so that the other scopes have one unambiguous contract to compile against.

### 1.2 Non-goals
- Not re-deriving the RSX→GLSL decompiler. `Program/` (`VertexProgramDecompiler.cpp`,
  `FragmentProgramDecompiler.cpp`, `GLSLCommon.cpp`, `GLSLSnippets/**`) is shared, backend-agnostic
  code and stays as-is; this document consumes its output. `[V]`
- Not the compute/blit shader path (`GLSLSnippets/**` used by `VKCompute.cpp`, resolve/deswizzle),
  except where it shares the argument-table mechanism (§3.4). `[I]`
- Not texture/sampler decoding or format redirection (scope 04), nor render-pass synthesis (scope 02/04). `[I]`
- Not SPIRV-Cross vendoring, CMake wiring, or the runtime-vs-offline MSL decision (scope 06). `[I]`
- Not an implementation. No Metal device is available to this task; every Metal API claim is against
  the already-catalogued metal-cpp surface in scope 03, not against a running driver. `[I]`

---

## 2. Model — RSX semantics → Metal 4 semantics

### 2.1 The existing shader pipeline (verified)
RSX ucode is decompiled to **GLSL text at runtime**, then that GLSL is compiled to SPIR-V by glslang,
then handed to `vkCreateShaderModule`:

- Emission: `VKVertexProgram.cpp` / `VKFragmentProgram.cpp` are thin `Program/` subclasses
  (`VKVertexDecompilerThread : VertexProgramDecompiler`, `VKFragmentDecompilerThread :
  FragmentProgramDecompiler` — `VKVertexProgram.h:11-54`, `VKFragmentProgram.h:13-50`). `[V]`
- GLSL → SPIR-V: `Program/SPIRVCommon.cpp` `compile_glsl_to_spv` (Vulkan rules target). `[V]`
- The GLSL is **not** hand-written and is **not** backend-specific in its body; only the wrapper
  (`Vk*`) and the resource declarations differ between backends. `[V]`

The consequences the whole document rests on:
1. A Metal backend inherits a complete, validated SPIR-V front-end for free. `[V]`
2. The *shape of the binding declarations* is the only part of the GLSL that is backend-coupled. `[I]`

### 2.2 The shape change
Keep `RSX ucode → GLSL → glslang → SPIR-V` unchanged; insert **SPIRV-Cross `CompilerMSL`** before
`MTL4Compiler` (`10-rsx-native-metal4-architecture.md` §4.6). Metal 4 did **not** change the
shader-binary model: the argument table indexes the classic `[[buffer(N)]]` / `[[texture(N)]]` /
`[[sampler(N)]]` spaces, so SPIRV-Cross needs **no** Metal-4 awareness. `[V]`/`[I]`

The binding contract therefore changes in exactly one dimension:

| | Vulkan (today) | Metal 4 (proposed) |
|---|---|---|
| Namespace | 2 descriptor sets (`binding_set_index_vertex=0`, `binding_set_index_fragment=1`) — `VKProgramPipeline.h:185-197` | 1 flat index space per stage; argument table is the only mutable indirection |
| Index assignment | per-program **dynamic** (`prepareBindingTable()` renumbers from 0 every compile) | **fixed** — the same RSX source always gets the same index (§3.2) |
| Push constants | `VkPushConstantRange` + `vkCmdPushConstants` (`VKGSRender.cpp:2429-2458`) | constant buffer bound by `setAddress` (§3.3) |
| Layout object | `VkPipelineLayout` / `VkDescriptorSetLayout` per program (`VKProgramPipeline.cpp:361-695`) | **no object**; table sized from reflected bindings (`ArgumentTableDescriptor`) |
| Pipeline link | `SEPARATE_SHADER_OBJECTS`, VS/FS linked independently | VS/FS validated together only at PSO creation |

### 2.3 Pipeline choice — direct MSL vs keep GLSL + SPIRV-Cross
Three candidates; the decision is fixed by evidence, not preference.

- **(A) Keep GLSL, insert SPIRV-Cross.** `RSX → GLSL → SPIR-V → CompilerMSL → MSL → MTL4Compiler`.
  Reuses `Program/` unchanged (72 files; VP/FP decompilers, `GLSLCommon`, the `GLSLSnippets`
  emulation library, the per-title fixups). SPIRV-Cross is battle-tested (MoltenVK, Godot) and is a
  pinned vendored revision. `[V]`
- **(B) Emit MSL directly from the decompiler.** Rejected. It would fork every `Program/` emitter
  *and* re-implement the GLSL emulation library (`_builtin_*`, pack/unpack, ROP epilogue,
  `GLSLCommon.cpp` helpers) in MSL, plus every `GLSLSnippets/**` snippet. A second backend to
  maintain for no correctness gain. `[I]`
- **(C) NIR→MSL back end (à la KosmicKrisp).** Not reachable — RPCS3's front-end is GLSL glslang,
  not Mesa NIR, so KK's compiler is not reusable (only its workaround catalogue is). `[V]`

**Decision: (A).** Difficulty 3; it costs ~0 to reuse `Program/` and needs only a one-time binding
remap (`add_msl_resource_binding`) plus the post-pass in §2.4. `[I]`

### 2.4 The MSL post-pass (the only places the cross-compiler must be corrected)
SPIRV-Cross cannot know RSX-specific rewrites. Exactly three are required (`10` §4.6): `[I]`
1. **Programmable blending** — the FS `subpassInput` read-back (`VKFragmentProgram.cpp:276-289`) must
   become a tile framebuffer-fetch `[[color(n)]]` fetch of the currently-bound output attachment
   (§2.6, T3 in `03` §5).
2. **Push-constant substitution** — MSL has no push constants and metal-cpp `MTL4::ArgumentTable`
   exposes no `setBytes` (only `setAddress/setResource/setTexture/setSamplerState`); the two RSX
   blocks (VS 4 B, FS 28 B) must become constant-buffer reads (§3.3).
3. **Border-colour / LOD-bias injection** — the sampler border colour and mip LOD bias RSX folds
   into the sampler (`04` R18/R19) are not expressible in MSL sampler state alone and need
   shader-side injection.

The post-pass is the *only* MSL text editing the port performs; everything else is stock
`CompilerMSL::compile()`. `[I]`

### 2.5 RSX shader facets (enumerated from the decompilers, not invented)
These are the units of behaviour the contract must preserve. They are the `glsl::shader_properties`
bits (`Program/GLSLTypes.h:21-72`) and the flags the wrappers derive from the RSX control word
(`gcm_enums.h:450-483`, `RSX_SHADER_CONTROL_*`) plus the decompiler tables.

**Vertex program facets**
- Inputs: the vertex attribute register file — `in_pos`, `in_weight`, `in_normal`, `in_diff_color`,
  `in_spec_color`, `in_fog`, `in_point_size`, `in_tc0..in_tc7` — each with a component-usage mask
  (attribute input mask). `[V]` (`VKVertexProgram.cpp` wrapper; `VertexProgramDecompiler.cpp`)
- Outputs: `gl_Position`, `diff_color`/`diff_color1`, `spec_color`/`spec_color1`, `fog_c`,
  `gl_ClipDistance[0..5]` (user clip planes UC0–UC5, gated by `is_user_clip_enabled(n)` and scaled
  by `user_clip_factor(n)`), `gl_PointSize`, `tc0..tc9` — the output table is
  `VKVertexProgram.cpp:276-303` (`reg_table[]`), gated per-output by
  `CELL_GCM_ATTRIB_OUTPUT_MASK_*`. `[V]`
- Control: `RSX_SHADER_CONTROL_FLAT_SHADING` (flat-qualify front/back colours),
  `RSX_SHADER_CONTROL_INSTANCED_CONSTANTS` (indexed constant addressing),
  `RSX_SHADER_CONTROL_INTERPRETER_MODEL`, `RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE`. `[V]`
- Constants: a single vertex constant register file (`vc[]`), or an instancing LUT + instanced
  constants array when `INSTANCED_CONSTANTS` is set (`VKVertexProgram.cpp:186-241`). `[V]`
- Vertex texture fetch: 4 TIUs, `sampler1D/2D/3D/Cube` (+ `sampler2DMS` when multisampled). `[V]`
- Functions: `require_lit_emulation`, `emulate_zclip_transform`, `emulate_depth_clip_only`,
  `require_clip_plane_functions`, `require_explicit_invariance`. `[V]` (`GLSLTypes.h:26-32`)

**Fragment program facets**
- Precision: `REAL/HALF/FIXED12/FIXED9/SATURATE/UNKNOWN` register precision; `fp32_outputs`
  (`CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS`); native `half` vs emulated (`clamp16`). `[V]`
  (`GLSLTypes.h:35,44`)
- Pack/unpack: `PK2/PK4/PK16/PKB/PKG` and `UP2/UP4/UP16/UPB/UPG`/`OR16`, plus `require_srgb_to_linear`
  (`has_upg`) and `require_linear_to_srgb` (`has_pkg`). `[V]` (`GLSLTypes.h:37-38`;
  `FragmentProgramDecompiler.cpp`)
- Texture ops: 1D/2D/3D/cube, `sampler2DMS`, shadow (`require_tex_shadow_ops`,
  `emulate_shadow_compare`), shadow-projection (`require_shadowProj_ops`), depth-as-RGBA
  (`require_depth_conversion`), sign-expand (`require_texture_expand`), per-format convert
  (`require_color_format_convert`), alpha kill (`require_alpha_kill`), dynamic indexed
  constant/register loads. `[V]` (`GLSLTypes.h:60-72`; `VKFragmentProgram.cpp:195-244`)
- ROP bits: `ROP_output_multisampled`, `ROP_output_rounding`, `ROP_sRGB_packing`, `ROP_alpha_test`,
  `ROP_alpha_to_coverage_test`, `ROP_polygon_stipple_test`, `ROP_discard`, `ROP_channel_remap`,
  `ROP_programmable_blend`, `ROP_emulate_depth_range`, `ROP_depth_export`. `[V]`
  (`GLSLTypes.h:48-58`; derived at `VKFragmentProgram.cpp:411-429`; realised in
  `GLSLSnippets/RSXProg/RSXROPEpilogue.glsl`)
- Depth: `gl_FragDepth` export (writes `r1.z`), depth-compare emulation
  (`RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE`), `gl_FragCoord.w`. `[V]`
- Other: `require_fog_read`, `require_wpos`, two-sided lighting, `discard` (`RSX_SHADER_CONTROL_USES_KIL`). `[V]`

**Control flow (both stages):** `BRA/BRI/CAL/CLI/RET/BRB/CLB/PSH/POP` + `jump_table`, condition codes
`cc0/cc1`, `has_branch_instructions` / `has_pack_instructions` metadata. `[V]`
(`VertexProgramDecompiler.cpp`, `FragmentProgramDecompiler.cpp`; metadata in
`ProgramStateCache.h:26-31,52-64`)

### 2.6 Fixed-function lowering — shader vs pipeline state vs encoder
The register clusters that surround the shader (from `inventory/out/SUMMARY.md`) do **not** all land
in the same Metal object. This is the single most important structural fact for the binding contract:
RSX "fixed-function" state is split across the shader (baked at MSL compile time), the pipeline object
(baked at PSO creation), and per-draw encoder setters. The split is forced by Metal: MSL and PSO state
are immutable once built, so anything RSX can change per draw must go to the encoder (or the argument
table), and anything that changes the *shader code* must force a new MSL/PSO.

| RSX cluster (`SUMMARY.md`) | Representative registers | Lowers to | Metal 4 object/method | Tag |
|---|---|---|---|---|
| `fixed-function` (4) | `FOG_MODE`, `FOG_PARAMS`, `SPECULAR_ENABLE`, `TWO_SIDE_LIGHT_EN` | **shader** (fog function + constants; specular/two-sided output selection) | MSL body via `Program/`; values via the constant buffers in §3.2 | `[I]` |
| `blend` (15) | `BLEND_ENABLE`, `BLEND_FUNC_SFACTOR/DFACTOR`, `BLEND_EQUATION`, `BLEND_COLOR`, `BLEND_ENABLE_MRT`, `ALPHA_TEST*`, `DITHER_ENABLE`, `SHADE_MODE`, `LOGIC_OP*`, `BLEND_COLOR2` | **PSO** (per-attachment blend) + **shader** (alpha test, dither, flat shading) + **encoder** (blend colour) | `RenderPipelineDescriptor::colorAttachments()->object(i)->setBlendingState/setBlendingFactors/setBlendingOperations/setWriteMask`; `RenderCommandEncoder::setBlendColor`; alpha-test/dither → MSL `discard_fragment()`; `SHADE_MODE` → `RSX_SHADER_CONTROL_FLAT_SHADING` | `[I]` |
| `raster` (30) | cull/front face, polygon mode, viewport, scissor, poly offset, point size, `USER_CLIP_PLANE_CONTROL`, polygon stipple, line width | **PSO** (cull, front face, topology class) + **encoder** (viewport/scissor/depth bias) + **shader** (user clip planes → `[[clip_distance]]`, stipple → `discard_fragment()`, point size → `[[point_size]]`) | `RenderPipelineDescriptor::setInputPrimitiveTopology/setRasterizationEnabled`; `RenderCommandEncoder::setViewports/setScissorRects/setDepthBias`; MSL attributes | `[I]` |
| `depth-rop` (38) | depth func/mask/test, front+back stencil func/op/mask/ref, `COLOR_MASK`/`COLOR_MASK_MRT`, `DEPTH_BOUNDS_*`, `ANTI_ALIASING_CONTROL`, zcull | **separate depth-stencil object** + encoder + **PSO** (colour write mask) + **shader** (depth export, depth-range emulation) | `MTL::DepthStencilState` + `RenderCommandEncoder::setDepthStencilState/setStencilReferenceValue/setStencilFront/BackReferenceValue`; `colorAttachments()->object(i)->setWriteMask`; zcull → no Metal path (drop) | `[I]` |

Two facts worth calling out:

- On Vulkan all of this is one `pipeline_props` struct hashed into one key
  (`VKPipelineCompiler.h:11-51,226-246`). On Metal the depth-stencil half **leaves** the PSO and
  becomes a `MTL::DepthStencilState` set on the encoder (item 22, `03` §2). The cache key therefore
  must not fold depth-stencil state into the PSO key (see §3.5), or it will over-create PSOs. `[I]`
- `LOGIC_OP` / `LOGIC_OP_ENABLE` are in the `blend` cluster and in `pipeline_props`
  (`VKPipelineCompiler.h:29-30`) but Metal has no logic-op stage; this is a genuine gap, not a
  reshape (§5). `[I]`

### 2.7 Comparison with the Vulkan and OpenGL shader backends
All three share `Program/`; they differ only in the resource/binding wrapper and the pipeline object.

| Aspect | Vulkan (today) | OpenGL (today) | Metal 4 (this design) |
|---|---|---|---|
| Intermediate | `SPIRVCommon.cpp:128` glslang → SPIR-V | GLSL straight to `glCreateShader` | glslang → SPIR-V → `CompilerMSL` |
| Stage object | `VkShaderModule` | `GLuint` program | `MTL4::Library` + `LibraryFunctionDescriptor` |
| Binding space | 2 descriptor sets (`VKProgramPipeline.h:185-197`) | fixed UBO block indices `UBO_SLOT(x)=x+8` (`GL/glutils/common.h:11-20`) | 1 flat slot space, per-stage `MTL4::ArgumentTable` |
| Per-program renumber | **yes** — dynamic `prepareBindingTable()` (`VKVertexProgram.cpp:30-71`, `VKFragmentProgram.cpp:29-108`) | **no** — fixed `layout(binding=)` macros | **no** — fixed RSX map (§3.2) |
| Push constants | `vkCmdPushConstants` (`VKGSRender.cpp:2429-2458`) | implicit (uniforms) | constant buffer by `setAddress` (§3.3) |
| Pipeline object | `VkPipeline` | none (context state) | `MTL4::RenderPipelineState` |
| Depth-stencil | inside the PSO | context state | separate `MTL::DepthStencilState` |
| Cache | `VkPipelineCache` (`g_pipeline_cache`) | `GLProgramBuffer` | `MTL4::PipelineDataSetSerializer` → `MTL4::Archive` |

The instructive contrast is **Vulkan vs OpenGL on index assignment**: Vulkan renumbers per program
because its set/binding space is validated independently; OpenGL already uses a *fixed* map. Metal 4
is structurally closer to OpenGL here — the shader's `[[buffer(N)]]` is baked at compile time — so the
Metal backend should adopt the **fixed map**, not Vulkan's dynamic one. `[I]`

---

## 3. Interfaces / data structures

### 3.1 The canonical slot map
`VK/vkutils/pipeline_binding_table.h:7-22` declares the intended RSX shader argument slot map:

```
vertex_params_bind_slot             = 0
vertex_constant_buffers_bind_slot   = 1
fragment_constant_buffers_bind_slot = 2
fragment_state_bind_slot            = 3
fragment_texture_params_bind_slot   = 4
vertex_buffers_first_bind_slot      = 5
conditional_render_predicate_slot   = 8
rasterizer_env_bind_slot            = 9
instancing_lookup_table_bind_slot   = 10
instancing_constants_buffer_slot    = 11
textures_first_bind_slot            = 12
```

Important verified nuance: **this header is currently inert.** It is included
(`VKVertexProgram.h:4`, `VKFragmentProgram.h:6`, `vkutils/device.h:5`) but no member is read; the live
VK path instead computes *dynamic* locations in `prepareBindingTable()` (`VKVertexProgram.cpp:30-71`,
`VKFragmentProgram.cpp:29-108`) and the interpreter recomputes its own (`VKShaderInterpreter.cpp:124-163`). `[V]`

**Decision.** The Metal backend promotes this header to the **canonical MSL binding index map**. Every
RSX source gets its fixed index from this map, and `prepareBindingTable()`'s dynamic packing is
dropped: SPIRV-Cross emits `[[buffer(N)]]`/`[[texture(N)]]`/`[[sampler(N)]]` with `N` taken from the
map, and the argument table is the only thing that changes between draws. The reason is structural:
MSL indices are compile-time constants, so a dynamic scheme would force a fresh SPIRV-Cross pass (and
a fresh MSL/MSL-library cache entry) for every binding permutation — cache blow-up with no benefit,
since the argument table can bind any index at draw time. `[I]`

Because slot `12` is where both `textures_first` and `vertex_textures_first` point (the header labels
the latter "Invalid, has to be initialized properly", `pipeline_binding_table.h:20-21`), and Metal
argument tables are bound **per stage** (`RenderCommandEncoder::setArgumentTable(table, RenderStage)`),
the vertex and fragment texture ranges both start at 12 in their respective stage tables without
collision. `[I]`

### 3.2 Slot → `MTL4ArgumentTable` index → MSL declaration
The concrete mapping. "Stage" is `RenderStageVertex` or `RenderStageFragment`; both stages may share
the same index because they are separate tables. Substituted push-constants (§3.3) are marked as
design additions, not members of `pipeline_binding_table.h`.

| RSX source | Index | Stage | MSL declaration (SPIRV-Cross output) | `MTL4::ArgumentTable` call |
|---|---|---|---|---|
| Vertex context params | 0 | V | `constant VertexContextBuffer& … [[buffer(0)]]` | `setAddress(gpuAddress + vs_context_offset, 0)` |
| Vertex constants | 1 | V | `constant VertexConstantsBuffer& vc [[buffer(1)]]` | `setAddress(vertexConstantsBuffer->gpuAddress(), 1)` |
| Fragment constants | 2 | F | `constant FragmentConstantsBuffer& fc [[buffer(2)]]` | `setAddress(fragmentConstantsBuffer->gpuAddress(), 2)` |
| Fragment state | 3 | F | `constant FragmentStateBuffer& … [[buffer(3)]]` | `setAddress(gpuAddress + fs_context_offset, 3)` |
| Fragment texture params | 4 | F | `constant TextureParametersBuffer& … [[buffer(4)]]` | `setAddress(gpuAddress + fs_texture_base_index, 4)` |
| Vertex input streams (persistent) | 5 | V | `texture_buffer<uint> persistent_input_stream [[texture(5)]]` | `setTexture(view.gpuResourceID(), 5)` |
| Vertex input streams (volatile) | 6 | V | `texture_buffer<uint> volatile_input_stream [[texture(6)]]` | `setTexture(…, 6)` |
| Draw-parameters heap | 7 | V | `constant DrawParametersBuffer& draw_parameters [[buffer(7)]]` | `setAddress(vertexLayoutStream->gpuAddress(), 7)` |
| Conditional-render predicate | 8 | V | `constant uint& cr_predicate_value [[buffer(8)]]` | `setAddress(scratch->gpuAddress() + offset, 8)` |
| Rasterizer heap (stipple) | 9 | F | `constant RasterizerHeap& stipple_pattern [[buffer(9)]]` | `setAddress(gpuAddress + fs_stipple_pattern_offset, 9)` |
| Instancing LUT | 10 | V | `constant int* constants_addressing_lookup [[buffer(10)]]` | `setAddress(lut->gpuAddress(), 10)` |
| Instancing constants | 11 | V | `constant vec4* instanced_constants_array [[buffer(11)]]` | `setAddress(array->gpuAddress(), 11)` |
| Fragment textures T0..T15 | 12+i | F | `texture{1d,2d,3d,cube,2d_ms}<float> tex<i> [[texture(12+i)]]` + `sampler smp<i> [[sampler(12+i)]]` (type chosen per `sampler_info`) | `setTexture(…, 12+i)` + `setSamplerState(…, 12+i)` |
| Vertex textures | 12+id | V | same as above, in the vertex table | same, in the vertex table |
| **VS draw-param index** (substituted push const) | `S_vs_draw_params` † | V | `constant uint& draw_parameters_offset [[buffer(S)]]` | `setAddress(transient->gpuAddress(), S)` |
| **FS programmable-blend config** (substituted push const) | `S_fs_blend` † | F | `constant BlendConfig& blend [[buffer(S)]]` | `setAddress(transient->gpuAddress() + 4, S)` |
| ROP read-back (programmable blend) | — | F | `[[color(i)]]` fragment input (framebuffer fetch) | none — resolved by the render pass load action |
| Colour outputs | — | F | `[[color(0..mrt_buffers_count-1)]]` | none — bound as attachments |
| Depth output | — | F | `[[depth(any)]]` | none |
| Builtins | — | both | `[[position]]`, `[[point_size]]`, `[[clip_distance]]`, `[[front_facing]]`, `[[sample_id]]`, `[[stencil]]`, `[[vertex_id]]`, `[[barycentric_coord]]` | none |
| Varyings | — | both | `[[user(locnN)]]` struct (locations from `VKCommonDecompiler.cpp:7-27`) | none |

† `S_vs_draw_params` / `S_fs_blend` are **not** in `pipeline_binding_table.h`; they are the two slots
this design adds for the substituted push constants. Their exact numeric values are an open question
(§6 Q2); the safe default is to append them above the texture range so the fixed map is never
re-shuffled. `[I]`

Source notes for the table:
- The five per-stage constant/state buffers and the draw-parameters heap are exactly the buffers
  bound today in `VKGSRender.cpp:2290-2294` and `:2290-2322`; only the indices change. `[V]`
- The three vertex streams are `persistent_input_stream`, `volatile_input_stream` and the
  draw-parameters heap (`VKVertexProgram.cpp:162-184,128-142`); they are bound as
  `uniform usamplerBuffer` and map to MSL `texture_buffer<uint>` via
  `CompilerMSL::Options::texture_buffer_native` (`03` T2). `[V]`/`[I]`
- The FS textures split into `texture` + `sampler` because MSL has no combined-image-sampler;
  SPIRV-Cross emits `msl_texture`/`msl_sampler` pairs from one SPIR-V binding (`03` item 13). `[V]`
- The colour-output table emits `layout(location=i) out vec4` per active RT
  (`VKFragmentProgram.cpp:175-192`); SPIRV-Cross turns each into `[[color(i)]]`. `[V]`/`[I]`
- Varying locations are fixed and shared: `tc0..tc9 = 0..9`, `diff_color=10`, `diff_color1=11`,
  `spec_color=12`, `spec_color1=13`, `fog_c=14`, `usr=15`, `depth_range=17` (16 is reserved for the
  interpreter) — `VKCommonDecompiler.cpp:7-27`. The VS-out and FS-in declarations must carry the same
  location and name. `[V]`

### 3.3 Push-constant substitution
Two RSX push-constant blocks exist and both must become constant-buffer reads:

| Block | Content | Offset/size | VK site |
|---|---|---|---|
| VS | `uint draw_parameters_offset` | offset 0, 4 B | `VKVertexProgram.cpp:144-159`; pushed at `VKGSRender.cpp:2429-2435` |
| FS | `blend_eqn, blend_sfactors, blend_dfactors, blend_constants(vec4)` | offset 4, 28 B | `VKFragmentProgram.cpp:338-359`; pushed at `VKGSRender.cpp:2437-2458` |

Because `MTL4::ArgumentTable` (metal-cpp) exposes no `setBytes`, the value must live in memory and be
bound with `setAddress`: a per-draw bump-allocated transient `MTL::Buffer` bound at the slot in §3.2,
carrying the exact byte layout (VS `[0,4)`, FS `[4,32)`). This preserves the offsets without SPIRV-Cross
needing a new mechanism, and it keeps the *shader* identical to the VK one save for the declaration.
`[V]`/`[I]`

### 3.4 Program, library, and pipeline objects (ownership & lifetime)
| Object | Created from | Owner | Lifetime |
|---|---|---|---|
| SPIR-V `std::vector<u32>` | `Program/` + glslang | backend shader cache (per guest program id) | until RSX program cache clear |
| MSL text | `spirv_cross::CompilerMSL` | same | until library built |
| `MTL4::Library` | `MTL4::Compiler::newLibrary(LibraryDescriptor)` (one compiler per device) | per program id | until cache clear |
| `MTL4::LibraryFunctionDescriptor` (VS/FS) | library + entry name | per PSO build | transient |
| `MTL4::RenderPipelineState` | `newRenderPipelineState(RenderPipelineDescriptor, …)` | pipeline cache, keyed (see §3.5) | until cache clear |
| `MTL4::ArgumentTable` | `device->newArgumentTable(ArgumentTableDescriptor)` | per stage, per frame-slot | frame ring |
| `MTL::DepthStencilState` | `MTL::DepthStencilStateDescriptor` | keyed separately, set on encoder | API object cache |

The `MTL4::Compiler` is **one per `MTLDevice`** (KosmicKrisp workaround W11: concurrent compilers
corrupt the heap). `[V]`

### 3.5 PSO assembly
**Vertex input: software pulling, no vertex descriptor.** RSX has no host vertex-input layout; the VS
reads its attributes from the `texture_buffer` streams by `[[vertex_id]]`
(`VKVertexProgram.cpp:162-184`; `RSXVertexFetch.glsl`). The Metal 4 PSO therefore sets **no**
`MTL::VertexDescriptor` and binds **no** vertex buffers for attributes: the streams arrive via the
argument table (slots 5–7) and `[[vertex_id]]`. This is the Metal analogue of the VK path, which also
declares no `VkVertexInputBinding`. `[V]`/`[I]`

**PSO fields** (assembled from the RSX clusters in §2.6):

| PSO field | Source | Metal 4 method |
|---|---|---|
| Vertex function | VS library + entry | `setVertexFunctionDescriptor(LibraryFunctionDescriptor)` |
| Fragment function | FS library + entry | `setFragmentFunctionDescriptor(...)` |
| Topology | `M_PRIMITIVE` / input-assembly | `setInputPrimitiveTopology(PrimitiveTopologyClass…)` |
| Raster enabled / cull / front face | `raster` cluster | `setRasterizationEnabled`, `setCullMode`, `setFrontFacingWinding` |
| Per-attachment blend | `blend` cluster | `colorAttachments()->object(i)->setBlendingState/setBlendingFactors/setBlendingOperations/setWriteMask` |
| Attachment formats | `surface` cluster / pass | `colorAttachments()->object(i)->setPixelFormat`, depth/stencil pixel formats |
| MSAA sample count | `ANTI_ALIASING_CONTROL` | `setRasterSampleCount` |
| Fragment outputs | FS MRT count | inferred from `[[color(i)]]`; SPIRV-Cross `set_fragment_output_components` at MSL compile time |

**Not in the PSO:** depth-stencil state (separate `MTL::DepthStencilState`), viewport/scissor/blend
colour/depth bias/stencil ref (encoder setters — the dynamic set is
`VKShaderInterpreter.cpp:37-46,77`), and the substituted push-constant values (argument table). `[V]`/`[I]`

**Cache key.** Compose the PSO key from the two guest program ids plus the *PSO-relevant* subset of
`pipeline_props`:

- vertex program id + fragment program id — `ProgramStateCache.h:174-199` (`pipeline_key`). `[V]`
- `renderpass_key` (attachment formats + AA + log2 dims). `[V]`
- input-assembly, rasterization, colour-blend (+ per-attachment), multisample — everything in
  `VKPipelineCompiler.h:16-49` **except** depth-stencil. `[V]`
- **Exclude** depth-stencil state: it is encoder state on Metal
  (`VKPipelineCompiler.h:37-38` is PSO state on Vulkan). `[I]`
- Any device capability that changes the emitted GLSL/MSL and is *not* already implied by the program
  id (native fp16, float64 depth-clip, NVIDIA-style low-precision tests — see
  `VKVertexProgram.cpp:345-354`, `VKFragmentProgram.cpp:405-411`) must be folded into the **shader**
  cache key, not just the PSO key. `[V]`/`[I]`

### 3.6 Shader caching & async creation
The guest→program cache already exists and is backend-agnostic; reuse it unchanged.

| Layer | Mechanism | Site |
|---|---|---|
| Guest ucode hash | `get_vertex_program_ucode_hash` / `get_fragment_program_ucode_hash` | `ProgramStateCache.h:34,76` |
| Hash + compare functors | `vertex_program_storage_hash` / `fragment_program_storage_hash` + `*_compare::compare_properties` | `ProgramStateCache.h:39-49,79-89` |
| Program cache | `binary_to_vertex_program` / `binary_to_fragment_program` maps | `ProgramStateCache.h:212-213` |
| Pipeline cache | `program_state_cache::get_graphics_pipeline` → `pipeline_key` → `m_storage` | `ProgramStateCache.h:351-447` |
| Async hook | `build_pipeline(..., compile_async, callback, …)`; a `__null_pipeline_handle` placeholder prevents duplicate linking | `ProgramStateCache.h:395-396,438-446` |
| Compile queue | `pipe_compiler` worker queue (`lf_queue`) | `VKPipelineCompiler.h:53-221` |

Metal-specific additions:
- A second cache keyed by **program id** holds the MSL source and `MTL4::Library` (SPIRV-Cross is not
  free; run it once per program). (`10` §4.6) `[I]`
- `MTL4::PipelineDataSetSerializer` feeds the binary archive; `MTL4Archive` +
  `CompilerTaskOptions.lookupArchives` back it on load (`03` item 6, `06` §2.5). `[V]`
- The `build_pipeline` callback model maps onto the async `MTL4::Compiler::newRenderPipelineState`
  completion: the placeholder in `m_storage` is replaced when the pipeline arrives, matching the
  existing synchronous/pending split (`ProgramStateCache.h:401-436`). `[I]`

---

## 4. Invariants

1. **Index stability.** For a given RSX source, the MSL binding index is a compile-time constant equal
   to the argument-table index; it never depends on which other sources the program uses. `[I]`
2. **Table sufficiency.** `ArgumentTableDescriptor` buffer/texture/sampler counts are ≥ the highest
   index each stage references (base 12 + up to 16 FS textures, plus the substituted slots). `[I]`
3. **No dangling reference.** Every slot a compiled MSL references is `setAddress`/`setTexture`/
   `setSamplerState` before the draw; unused slots may be left unbound with
   `setInitializeBindings(true)`. `[I]`
4. **Attachment/PSO format agreement.** Every PSO colour attachment's `pixelFormat` equals the render
   pass attachment's format at that index; mismatch fails PSO creation. `[I]`
5. **Programmable-blend read-back.** The fragment's `[[color(i)]]` fetch is defined only if the pass's
   attachment `i` load action is `Load` (not `Clear`/`DontCare`), and `i` is both the fetch and the
   store target. `[V]`/`[I]`
6. **Varying agreement.** VS-out and FS-in locations and names match the fixed table
   (`VKCommonDecompiler.cpp:7-27`); `usr=15` and `depth_range=17` are reserved. `[V]`
7. **Constant-buffer fidelity.** The fragment constants buffer is filled with the same
   `constant_offsets` the shader was compiled against (`fill_fragment_constants_buffer`,
   `ProgramStateCache.h:449-459`); the RSX shader's `_fs_constants_offset` must equal the buffer used. `[V]`
8. **Shader cache key completeness.** Every device capability or config that alters emitted GLSL
   (native fp16, depth-clip, precision preset, flat shading) is part of the shader cache key; two
   programs with equal ucode but different caps must not share an MSL. `[V]`/`[I]`
9. **One compiler per device.** Exactly one `MTL4::Compiler` per `MTLDevice` (KosmicKrisp W11). `[V]`
10. **Deterministic unbound slots.** `setInitializeBindings(true)` yields a deterministic null binding,
    so a shader bug reads null rather than stale data. `[I]`

---

## 5. Failure modes & limits

| # | Failure | Cause | Mitigation / disposition |
|---|---|---|---|
| F1 | `discard_fragment()` with side effects prematurely discards fragments | KosmicKrisp W4/W5 on M1/M2 | `manual_helper_invocation_updates`, `check_discarded_frag_stores`, `force_fragment_with_side_effects_execution`; wrap discards in always-true conditionals (`03` §3.1) `[V]` |
| F2 | Uninitialised local scratch → MSL compiler crash | KK W1 | Initialise scratch in the post-pass (`03` §3.1) `[V]` |
| F3 | `while(true)` + invariant break → compiler crash/hang | KK W2 | Rewrite loop shape in the post-pass (`03` §3.1) `[V]` |
| F4 | Vertex streams as `usamplerBuffer` | no Metal `usamplerBuffer` | `texture_buffer_native`; 2D tiling width 4096; buffers wider than 4096 u32 need coordinate tiling maths (§6 Q6) `[V]`/`[I]` |
| F5 | Push constants | no `setBytes` in metal-cpp | transient buffer + `setAddress` (§3.3); byte-exact offset layout `[V]`/`[I]` |
| F6 | fp16 output precision | `fp32_outputs` toggle; half/half2 transcedental differences | shader `half` path only when supported; part of the shader cache key `[V]` |
| F7 | Logic op | Metal has no logic-op stage | no native path; drop or emulate in shader (open) `[I]` |
| F8 | Polygon mode fill/line | Metal has no polygon mode | lines unfillable publicly; clamp `[V]` (`03` item 28) |
| F9 | Dynamic line width | no public Metal wide lines | clamp to 1.0 `[V]` (`03` item 28) |
| F10 | Depth/stencil read-back as `[[color(i)]]` | input attachment == DS attachment hazard | `input_attachment_is_ds_attachment` + force depth-write ordering (`03` item 15) `[V]` |
| F11 | Stencil-only MS `[[sample_mask]]` ignored | KK W7 | end-of-shader conditional discard emulation (`03` T5) `[V]` |
| F12 | Index buffer OOB not robust | KK W12/W13 | 32-bit alignment + full accessible range `[V]` |
| F13 | `simd_ballot`/quad in conditional | KK W3 | rewrite as `simd_or` (FSR1 only) `[V]` |
| F14 | Barycentric `[[barycentric_coord]]` | MSL support unconfirmed | open (§6 Q7) `[U]` |
| F15 | MSL language version | pinned SPIRV-Cross max version unconfirmed | open (§6 Q5) `[U]` |
| F16 | Slot-map growth | a new facet needs a slot not in the fixed map | extend the fixed map at the tail, never renumber; else MSL recompiles and the cache blows up `[I]` |
| F17 | Binary-archive portability | `MTL4Archive` across OS/Metal versions | `06` §2.5; treat archive as a cache, always able to recompile `[U]` |
| F18 | Interpreter path | `VKShaderInterpreter` is a separate program source | either excluded from HLD-05 scope or given its own fixed map (`03` §1.3 notes the dead TS stub) (§6 Q11) `[I]` |

---

## 6. Open questions

**Q1 — Adopt the fixed slot map, or keep per-program dynamic numbering?** This document recommends
the fixed map (`pipeline_binding_table.h`) because MSL indices are baked at compile time. Confirm that
no RSX program needs two different sources at one index in one stage (which the fixed map would
forbid). `[I]`

**Q2 — Where do the two substituted push-constant slots live (§3.2 †)?** Append above the texture
range (stable, but grows every table), fold into the existing param buffers (fewer slots, but mixes
per-draw and per-context data), or compile them in as function constants / `#define`s (no runtime
slot, but a PSO permutation per blend config). `[U]`

**Q3 — One frame-wide argument table, or per-stage / per-state tables?** The RSX-first batching
strategy (D11, `09`) leans on argument tables being shareable across a batch; confirm whether the VS
table can be shared while only the FS table is mutated, or whether one table per stage is required for
`setArgumentTable(table, stage)`. `[U]`

**Q4 — Does the target SDK's `MTL4::ArgumentTable` actually expose `setBytes`?** metal-cpp and
objc2-metal do not; if a future SDK does, F5/Q2 collapse to a direct push. `[U]`

**Q5 — Which MSL version, and which `CompilerMSL::Options` set, does the pinned SPIRV-Cross emit?**
`set_msl_version(3,1)` vs `4,0`; `texture_buffer_native`, `r32ui_alignment_constant_id`,
`enable_point_size_builtin`, `input_attachment_is_ds_attachment`, `readwrite_texture_fences`. `[U]`

**Q6 — Vertex-stream `texture_buffer` width/alignment.** Streams can exceed the default 4096-texel
width; decide between 2D tiling (`texel_buffer_texture_width`) and a raw SSBO path, which would touch
`RSXVertexFetch.glsl` (out of scope for a wrapper-only change). `[I]`

**Q7 — Who owns the ROP programmable-blend shader rewrite?** It straddles binding (input attachment →
`[[color]]`, §3.2) and shader body (the blend block, §3.3); either HLD-05's post-pass owns it or a
texture scope does. `[I]`

**Q8 — Which device caps enter the shader cache key vs the PSO key?** Native fp16, float64 depth-clip,
the precision preset and flat shading change the GLSL; splitting them correctly is needed to avoid
re-using an MSL built under different caps. `[I]`

**Q9 — Does the fixed map need slots the current VK path never binds?** e.g. vertex-texture first
index (currently "invalid"), zcull/ZPASS-pixel-count predicates, and any facet added later. `[U]`

**Q10 — Binary-archive lifetime and cross-OS compatibility.** How aggressively to use
`MTL4::PipelineDataSetSerializer`, and whether an incompatible archive must be detected and rebuilt. `[U]`

**Q11 — Is the shader interpreter in HLD-05 scope?** It has its own decompiler path and its own
binding numbering (`VKShaderInterpreter.cpp:124-163`); either it gets a table in this contract or it is
explicitly deferred. `[I]`

**Q12 — Varying-struct vs discrete locations.** SPIRV-Cross can emit `[[user(locnN)]]` as a struct;
confirm the VS-out/FS-in naming and structs match on macOS (item 39, `03` §2). `[V]`/`[U]`

---

## 7. Provenance

**In-repo RPCS3 (read-only this session)** `[V]`
- `rpcs3/Emu/RSX/VK/vkutils/pipeline_binding_table.h:7-22` — canonical RSX slot map.
- `rpcs3/Emu/RSX/VK/VKVertexProgram.h:11-78` — VS decompiler subclass + `binding_table` struct.
- `rpcs3/Emu/RSX/VK/VKVertexProgram.cpp:30-71` (dynamic binding table), `:73-160` (headers/push const),
  `:162-184` (vertex streams), `:186-274` (constants/textures/instancing), `:276-341` (outputs, reg_table,
  FS export), `:343-354` (shader_properties derivation).
- `rpcs3/Emu/RSX/VK/VKFragmentProgram.h:13-81` — FS decompiler subclass + `binding_table` struct.
- `rpcs3/Emu/RSX/VK/VKFragmentProgram.cpp:29-108` (dynamic binding table), `:110-389` (headers, ROP
  inputs, push const, texture params, stipple heap), `:175-192` (MRT colour outputs),
  `:195-244` (sampler declaration / shadow / MSAA), `:391-429` (fragment shader_properties).
- `rpcs3/Emu/RSX/VK/VKCommonDecompiler.h:1-11`, `VKCommonDecompiler.cpp:7-27` — varying register locations.
- `rpcs3/Emu/RSX/VK/VKGSRender.cpp:2290-2322` (bind_uniform slots), `:2429-2458` (push constants),
  `:2348-2359` (get_binding_table).
- `rpcs3/Emu/RSX/VK/VKProgramPipeline.h:17-112` (input types, `program_input`), `:185-197` (binding sets).
- `rpcs3/Emu/RSX/VK/VKPipelineCompiler.h:11-51` (`pipeline_props`), `:53-221` (worker queue),
  `:226-246` (`hash_struct<pipeline_props>`).
- `rpcs3/Emu/RSX/VK/VKShaderInterpreter.cpp:124-163` (interpreter binding numbering), `:35-46,77`
  (dynamic-state set).
- `rpcs3/Emu/RSX/Program/ProgramStateCache.h:21-90` (hash/compare), `:160-199` (`pipeline_key`),
  `:212-214` (caches), `:351-447` (`get_graphics_pipeline`, async callback), `:449-459`
  (`fill_fragment_constants_buffer`).
- `rpcs3/Emu/RSX/Program/GLSLTypes.h:21-72` (`shader_properties`).
- `rpcs3/Emu/RSX/Program/SPIRVCommon.cpp:128` (`compile_glsl_to_spv`).
- `rpcs3/Emu/RSX/Program/GLSLSnippets/RSXProg/RSXROPEpilogue.glsl` — ROP/discard/depth-export epilogue.
- `rpcs3/Emu/RSX/gcm_enums.h:450-483` — `RSX_SHADER_CONTROL_*`.
- `rpcs3/Emu/RSX/GL/glutils/common.h:11-20` — GL fixed UBO slot map (contrast in §2.7).
- `rpcs3/Emu/RSX/GL/GLVertexProgram.cpp:33-83`, `GLFragmentProgram.cpp:189-213` — GL fixed bindings.

**Inventory (generated, in-repo)** `[V]`
- `docs/research/metal4/inventory/out/SUMMARY.md:24-42` (cluster table), `:43-61` (`blend`),
  `:63-104` (`depth-rop`), `:124-131` (`fixed-function`), `:236-269` (`raster`).
- `docs/research/metal4/inventory/out/rsx_value_space.csv:230-254` (`shader_control` value space).
- `docs/research/metal4/inventory/SCHEMA.md` §2 (`S_<slot>` IDs), §4 (disposition vocabulary), §6 (HLD template).

**Sibling research docs (in-repo)** `[V]`
- `docs/research/metal4/10-rsx-native-metal4-architecture.md` §4.2 (binding model), §4.6 (shader/pipeline
  model), §5 (open questions), §6.1 (effort).
- `docs/research/metal4/README.md` §2 (verdicts), §3.4 (shader/pipeline rows), D11 (draw-parameters SSBO).
- `docs/research/metal4/03-shaders-pipelines-descriptors.md` — the per-construct mapping table,
  §3.1 (KosmicKrisp workarounds W1–W16), §3.4 (SPIRV-Cross options), §4 (impossible/not-recommended),
  §5 (T1–T10 ranked).
- `docs/research/metal4/06-build-toolchain-deployment.md` §2.5 (archive/toolchain).

**External (as catalogued in `03` §8, not re-fetched this session)** `[V]`/`[U]`
- Apple `translating-to-metal4-api` / `creating-metal4-shader-pipelines` skills — argument-table model.
- metal-cpp `Metal/MTL4ArgumentTable.hpp:73-80` — no `setBytes`. `[V]`
- SPIRV-Cross `spirv_msl.hpp` — `CompilerMSL::Options` / `MSLResourceBinding`. `[V]`
- KosmicKrisp `docs/drivers/kosmickrisp/workarounds.rst` — W1–W16. `[V]`
- Metal Shading Language Specification — attribute spellings. `[U]` (not machine-read).

<!-- doc-end -->
