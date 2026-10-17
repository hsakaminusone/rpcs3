# 3. Shaders, pipeline objects, descriptors/argument tables & dynamic state

Owner/scope: Scope 3 | Status: draft | Date: 2026-10-15 (Thu)

**Verification legend used throughout:**
- `[V]` = verified this session by reading a file in-repo, a pristine reference checkout under `/tmp/metal4-refs/`, or an immutable raw source fetched over HTTP (URL listed in §8).
- `[I]` = inference / reasoned conclusion from `[V]` facts — not directly stated by any source.
- `[U]` = unverified / could not be confirmed (Apple HTML docs are JS-rendered and returned no text; MSL spec PDF not machine-read this session; MoltenVK's SPIRV-Cross submodule is an unpopulated symlink locally).

Repo is strictly read-only for this task; only this file was created. No build was run. No file under `rpcs3/**` was touched.

---

## 1. Scope & RPCS3 inventory

### 1.1 What Scope 3 covers
Shader translation (RSX→GLSL→SPIR-V→MSL), pipeline objects (graphics/compute PSO creation & caching), the descriptor/binding model (Vulkan descriptor sets/pools/templates/push constants → Metal 4 argument tables), and dynamic state. Scope 3 does **not** re-derive the RSX→GLSL decompiler itself (that is shared code in `Emu/RSX/Program/**` and is out of scope to change); it covers what changes when that GLSL is retargeted to Metal.

### 1.2 The current VK shader path (verified)
RPCS3 does **not** ship hand-written GLSL. It emits GLSL text at runtime from RSX microcode decompilers, then compiles that GLSL to SPIR-V with **glslang**, then hands the SPIR-V to `vkCreateShaderModule`:

- `rsx::air` / `rsx::glsl` decompilers produce GLSL: `rpcs3/Emu/RSX/VK/VKVertexProgram.cpp`, `rpcs3/Emu/RSX/VK/VKFragmentProgram.cpp` (thin VK layers) and `rpcs3/Emu/RSX/Program/FragmentProgramDecompiler.cpp` + `VertexProgramDecompiler.cpp` (the actual opcode→GLSL translation). `[V]`
- GLSL→SPIR-V: `rpcs3/Emu/RSX/Program/SPIRVCommon.cpp:128` `compile_glsl_to_spv(...)`; Vulkan rules at `:144-146` = `EShClientVulkan`, `EShTargetVulkan_1_2`, `EShTargetSpv_1_5`, `EShMsgVulkanRules | EShMsgSpvRules | EShMsgEnhanced`. `[V]`
- GLSL feature surface actually requested (grep): `GL_EXT_scalar_block_layout`, `GL_EXT_uniform_buffer_unsized_array`, `GL_ARB_separate_shader_objects`, `GL_EXT_shader_explicit_arithmetic_types_float16`, `GL_ARB_shader_texture_image_samples`, `GL_EXT_fragment_shader_barycentric`, `GL_KHR_shader_subgroup_{arithmetic,ballot,quad,shuffle}` (FSR only). `[V]`

**Key structural fact for a Metal port:** because the decompilers emit GLSL and glslang turns that into SPIR-V, a Metal backend inherits a complete, validated SPIR-V front-end for free. The chosen approach (Option A, per project decisions) inserts **SPIRV-Cross** between glslang and the GPU: `GLSL → (unchanged) → SPIR-V → SPIRV-Cross → MSL → MTL4Compiler/LibraryDescriptor`. `[I]`

### 1.3 Stage inventory
| Stage | Emitted from | Notes |
|---|---|---|
| Vertex | `VKVertexProgram.cpp` | software vertex-pulling; streams read as `usamplerBuffer` |
| Fragment | `VKFragmentProgram.cpp` | textures, ROP/programmable blend, depth/stencil |
| Compute | `VKCompute.cpp` + `Program/GLSLSnippets/**` | deswizzle, resolve/unresolve, copy, mip-gen, D32 shift |

**No geometry or tessellation shaders exist in the VK path.** `[V]` grep for real GS/TS usage returns nothing; the only hits are a dead `VkPipelineTessellationStateCreateInfo m_ts{};` stub at `rpcs3/Emu/RSX/VK/VKShaderInterpreter.cpp:35` populated at `:112`, and a device feature *test* at `rpcs3/Emu/RSX/VK/vkutils/device.cpp:979`. RSX itself has no GS/TS. This directly answers the "how do we emulate geometry shaders" question: **we don't have to.** KosmicKrisp's whole GS/tessellation-compute emulation stack (`libkk_unroll_geometry`, `libkk_tessellate`, `libkk_prefix_sum_tess`, `mtl_set_vertex_amplification_count`) is irrelevant to RPCS3. `[I]`

### 1.4 Descriptor / binding inventory (exact symbols)
Vulkan-descriptor-type mapping is centralised at `VKProgramPipeline.cpp:54-64`: `UNIFORM_BUFFER`, `UNIFORM_TEXEL_BUFFER`, `COMBINED_IMAGE_SAMPLER`, `STORAGE_BUFFER`, `STORAGE_IMAGE`, `INPUT_ATTACHMENT`. `[V]`

- `program::create_pipeline_layout()` — `VKProgramPipeline.cpp:361-397`: builds `VkDescriptorSetLayout`s per set and a `VkPushConstantRange` array from `input_type_push_constant` inputs. `[V]`
- `descriptor_table_t::create_descriptor_set_layout()` — `:625-695`: one `VkDescriptorSetLayoutBinding` per reflected GLSL input; array counts parsed from the GLSL name (`name[..]`); `vk::descriptors::create_layout(bindings)`. `[V]`
- `descriptor_table_t::create_descriptor_pool()` — `:698-701`: pool sized from `m_descriptor_pool_sizes`. `[V]`
- Binding **location limit**: `:199` `ensure(item.location < 128u || item.type == input_type_push_constant)`. `[V]`
- Fixed slot layout: `rpcs3/Emu/RSX/VK/vkutils/pipeline_binding_table.h` — `vertex_params_bind_slot=0`, `vertex_constant_buffers=1`, `fragment_constant_buffers=2`, `fragment_state=3`, `fragment_texture_params=4`, `vertex_buffers_first=5`, `conditional_render_predicate=8`, `rasterizer_env=9`, `instancing_lookup_table=10`, `instancing_constants_buffer=11`, `textures_first=12`. `[V]`
- `vk/utils/descriptors.{h,cpp}`: `descriptor_pool`, `descriptor_set`, dynamic offsets, `UPDATE_AFTER_BIND` (bindless textures). `[V]`

**Push constants actually used (all are tiny and offset/packed in one block):**
| Program | Block | Offset / size | Site |
|---|---|---|---|
| VS (main) | `push_constants_block { uint draw_parameters_offset; }` | offset 0, size 4 | `VKVertexProgram.cpp:145-157` |
| FS programmable blending | `push_constants_block { uint blend_eqn; uint blend_sfactors; uint blend_dfactors; vec4 blend_constants; }` | offset 4, **size 28** | `VKFragmentProgram.cpp:341-356` |
| UI overlay VS/FS | `push_constants` | VS then FS, split (`vkCmdPushConstants` at `:560`, `:589`) | `VKOverlays.cpp:485-513` |
| Attachment-clear VS | `push_constants` | `vertex_push_constants_size` | `VKOverlays.cpp:749-762` |
| Video-out calibration FS | `push_constants` | fragment | `VKOverlays.cpp:905-915` |
| Resolve helper | push constant | `VKResolveHelper.h:124` |
| Compute helpers | push constant | `VKCompute.cpp:32` |

The VS/FS blocks are **not** in the same push-constant range; on Vulkan the VS range covers `[0,4)` and the FS range covers `[4,32)`. Note `draw_parameters` itself (the draw call table) is an **SSBO**, not a push constant: `VKVertexProgram.cpp:87` `#define get_draw_params() draw_parameters[draw_parameters_offset]`, `:131` `draw_parameters_t draw_parameters[];`. `[V]`

### 1.5 Builtins actually emitted by RPCS3 shaders (grep-verified)
`gl_Position`, `gl_PointSize` (`VKVertexProgram.cpp:289,481`), `gl_ClipDistance[0..5]` (`VKVertexProgram.cpp:286-292`; user clip planes UC0–UC5, backed by `user_clip_factor`), `gl_VertexIndex` (as `_gl_VertexID`, `RSXVertexFetch.glsl:33-37`), `gl_FragCoord`, `gl_FrontFacing`, `gl_SampleID` (`MSAA/DepthUnresolvePass.glsl:17`, `StencilUnresolvePass.glsl:22`), `gl_FragDepth` (`RSXROPEpilogue.glsl:79`), `gl_FragStencilRefARB` (`MSAA/DepthStencilResolvePass.glsl:25`, `CopyBufferToGenericImage.glsl:185`), `gl_BaryCoord{EXT,AMD}` (`GLSLCommon.cpp:760-771`). `[V]`

**No `gl_CullDistance` usage found** in RPCS3 (unlike DXVK). `[V]` (grep returned zero). `gl_Layer`/`gl_ViewportIndex` also absent. `[V]`

### 1.6 Texture-sampling inventory
- `sampler2D` / `sampler1D` / `sampler3D` / `samplerCube`; `sampler2DMS` for MSAA (`VKFragmentProgram.cpp:214,219,260`; `GLSLCommon.cpp:530,533` emits `sampler2DMS` and `usampler2DMS`).
- Depth/shadow: `sampler2DShadow` with `_EMULATED_TEXSHADOW` (`GLSLCommon.cpp:454`) and `_ENABLE_SHADOW`/`_ENABLE_SHADOWPROJ` (`:459,477`).
- Input attachments: `subpassInput` / `subpassInputMS` (`VKFragmentProgram.cpp:277-278`) used by ROP for read-back ("programmable blending").
- Depth-as-RGBA + Z24S8 reconstruct: `RSXFragmentTextureDepthConversion.glsl` (`ZS_READ`, `decode_depth24`). `[V]`
- **Vertex texture fetch is `texelFetch` on a buffer**, not a texture: `attribute_desc` + `usamplerBuffer` at `RSXVertexFetch.glsl:75` `fetch_attribute(desc, vertex_id, usamplerBuffer input_stream)`, declared in VS as `layout(set=0, binding=N) uniform usamplerBuffer <stream>;` (`VKVertexProgram.cpp:173`). This is *software vertex pulling* (the whole point: no `VkVertexInputBinding` for RSX streams). `[V]`

### 1.7 Dynamic state & pipeline caching (verified)
- Dynamic-state list (`VKShaderInterpreter.cpp:37-46`): `VIEWPORT, SCISSOR, LINE_WIDTH, BLEND_CONSTANTS, STENCIL_COMPARE_MASK, STENCIL_WRITE_MASK, STENCIL_REFERENCE, DEPTH_BIAS`; `DEPTH_BOUNDS` pushed conditionally at `:77`. `[V]`
- Async pipeline compilation: `VKPipelineCompiler.{h,cpp}` `pipe_compiler`, single `g_pipeline_cache` (`VkPipelineCache`), `pipeline_props` hashing, `SEPARATE_SHADER_OBJECTS`. `[V]`
- Full pipeline state struct: `vkutils/graphics_pipeline_state.hpp` (IA/RS/DS/blend/MS). `[V]`

### 1.8 Instruction-level (RSX opcode → GLSL) inventory
Representative decompiled ops that must survive GLSL→SPIR-V→MSL:
`ADD/DIV/DIVSQ/MAD(→fma)/MUL/MAX/MIN/RCP/RSQ/SEQ/SGE/SGT/SLE/SLT/SNE/SFL/STR/SSG/FLR/FRC/LIT/LIF/LRP/LG2/EX2/COS/SIN/REFL/NRM/POW/DP2/DP3/DP4/DP2A`, plus the pack/unpack family `PK2/PK4/PK16/PKB/PKG` and `UP2/UP4/UP16/UPB/UPG`. `[V]` (`FragmentProgramDecompiler.cpp:1085-1144`, `:1233`, `:964-1014`). Emulation helpers are GLSL `#define`s / functions: `_builtin_rcp/rsq/sqrt/log2/div/lerp/floor/normalize/lit/lif/pkg/upg/divsq` (`:964-1014`). `[V]`

---

## 2. Vulkan/RSX → Metal 4 mapping table

Columns: construct (exact symbol) | Metal 4 equivalent | objects/methods | difficulty | notes.
Difficulty: 1 trivial rename · 2 mechanical · 3 real work but well-trodden · 4 hard/needs design · 5 unknown/impossible.

| # | Vulkan / RSX construct (exact API or symbol) | Metal 4 equivalent | Metal 4 objects/methods | Difficulty (1-5) | Notes |
|---|---|---|---|---|---|
| 1 | GLSL→SPIR-V: `compile_glsl_to_spv` (glslang) `SPIRVCommon.cpp:128` | **unchanged** | — | 1 | SPIR-V stays the intermediate. Do not touch glslang. `[V]` |
| 2 | `VkShaderModule` from SPIR-V | MSL text (SPIRV-Cross) → `MTL::Library` | `MTL4::Compiler::newLibrary(MTL4::LibraryDescriptor{setSource,setOptions})` | 3 | SPIR-V→MSL is the real work; needs resource-binding remap. `[V]` |
| 3 | `VkPipelineShaderStageCreateInfo` (VS/FS) | `MTL4::FunctionDescriptor` (library + name) | `MTL4::LibraryFunctionDescriptor{setLibrary,setName}` | 2 | Metal 4 uses descriptors, not `MTLFunction`. `[V]` |
| 4 | `VkSpecializationInfo` | function constants / MSL `#define` macros | `MTL4::SpecializedFunctionDescriptor{setConstantValues,setSpecializedName}` + `MTL::FunctionConstantValues` | 3 | SPIRV-Cross maps SPIR-V spec constants to MSL macros OR function constants (`specialization_constant_is_macro`). `[V]` |
| 5 | `VkPipelineLayout` / `vkCreatePipelineLayout` (`VKProgramPipeline.cpp:361`) | **no object**; binding layout is implicit in argument table + shader | `MTL4::ArgumentTableDescriptor` sizing | 4 | Layout concept disappears; must derive table sizes from reflected bindings. `[V]`/`[I]` |
| 6 | `VkPipelineCache` (`g_pipeline_cache`, `VKPipelineCompiler`) | `MTL4::PipelineDataSetSerializer` → `MTL::Archive` | `MTL4::CompilerDescriptor{setPipelineDataSetSerializer}`; `serializeAsArchiveAndFlushToURL`; `MTL::Device::newArchive`; `CompilerTaskOptions.lookupArchives` | 3 | Binary archives are compatible across Metal 3/4. `[V]` |
| 7 | `vkCreateGraphicsPipelines` | `MTL::RenderPipelineState` | `MTL4::Compiler::newRenderPipelineState(RenderPipelineDescriptor,…)` | 2 | Polymorphic selector shared with mesh/tile. `[V]` |
| 8 | `vkCreateComputePipelines` | `MTL::ComputePipelineState` | `newComputePipelineState(ComputePipelineDescriptor,…)` | 2 | `.threadGroupSizeIsMultipleOfThreadExecutionWidth` opt. `[V]` |
| 9 | `VkPipeline` destruction | ARC release | `NS::Object::release` | 1 | — |
| 10 | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | argument table buffer slot | `ArgumentTable::setAddress(buffer->gpuAddress(), idx)` | 2 | GPU address supports pointer arithmetic. `[V]` |
| 11 | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | argument table buffer slot | `setAddress(gpuAddress, idx)` | 2 | Same space as UBO. `[V]` |
| 12 | `VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER` (`usamplerBuffer`, VS streams) | texel buffer **(or emulated)** | `texture_buffer_native` MSL type; `MTL::TextureTypeTextureBuffer`; `MTLTextureViewPool::setTextureViewFromBuffer` | **4** | **Hardest binding item.** Metal has no `usamplerBuffer`; needs `texture_buffer` (2D, width=4096, r32ui alignment) or a manual SSBO path. `[V]` |
| 13 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | texture + sampler **split** | `setTexture(tex->gpuResourceID(), tIdx)` + `setSamplerState(smp->gpuResourceID(), sIdx)` | 3 | Metal separates; SPIRV-Cross `MSLResourceBinding` gives separate msl_texture/msl_sampler. `[V]` |
| 14 | `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` | read/write texture | `setTexture(gpuResourceID, idx)` | 3 | Metal R/W textures need `access::read_write`; MSL 3.2+ fence handling. `[V]` |
| 15 | `VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT` / `subpassInput(MS)` (`VKFragmentProgram.cpp:277`) | framebuffer fetch / programmable blend | MSL fragment input `[[color(n)]]`; `MTL4::RenderPassDescriptor` load actions | **4** | No subpasses in Metal; read-back = tile framebuffer fetch. `.loadAction` must be `Load` or read is UB. `[V]` |
| 16 | `vkCmdPushConstants` (VS 4B; FS 28B `VKFragmentProgram.cpp:341`) | **emulated** | no table `setBytes` in metal-cpp → transient `MTL::Buffer` + `setAddress`; or function constants | **4** | metal-cpp `MTL4::ArgumentTable` exposes only `setAddress/setTexture/setSamplerState/setResource` — **no `setBytes`**. See §7 Q1. `[V]` |
| 17 | `VkDescriptorSet` + `vkCmdBindDescriptorSets` | argument table | `encoder->setArgumentTable(table, stageMask)` | 3 | Render: `MTL::RenderStageVertex\|Fragment`; compute: no stage arg. `[V]` |
| 18 | `descriptor_pool` dynamic offsets | pointer offset on GPU address | `setAddress(base + offset, idx)` | 3 | Argument table snapshots at each draw → cheap. `[V]` |
| 19 | `UPDATE_AFTER_BIND` bindless textures | re-`setTexture` between draws | `setTexture(...)` on live table | 3 | No descriptor-indexing emulation needed: table is a live mutable reference. `[V]` |
| 20 | `VkPipelineRasterizationStateCreateInfo` | descriptor props | `RenderPipelineDescriptor::setInputPrimitiveTopology`, `setRasterizationEnabled`, `setMaxVertexAmplificationCount` | 2 | Polygon mode point unsupported. `[V]` |
| 21 | `VkPipelineColorBlendStateCreateInfo` | per-attachment blend | `colorAttachments()[i]->setBlendingState/factors/ops/writeMask` | 2 | — |
| 22 | `VkPipelineDepthStencilStateCreateInfo` | separate object | `MTL::DepthStencilState` + `RenderCommandEncoder::setDepthStencilState` | 3 | Not part of PSO; set on encoder. `[I]` |
| 23 | MSAA `rasterizationSamples` | `setRasterSampleCount` | `RenderPipelineDescriptor::setRasterSampleCount` | 1 | — |
| 24 | `VkDynamicState` `VIEWPORT`/`SCISSOR` | encoder setters | `setViewports(count)`, `setScissorRects(count)` | 2 | — |
| 25 | `VkDynamicState` `BLEND_CONSTANTS` | encoder setter | `setBlendColorRed:green:blue:alpha:` | 2 | — |
| 26 | `VkDynamicState` `STENCIL_REFERENCE/COMPARE/WRITE` | encoder setter | `setStencilReferenceValue` / front/back variants | 2 | — |
| 27 | `VkDynamicState` `DEPTH_BIAS` | encoder setter | `setDepthBias:slopeScale:clamp:` | 2 | — |
| 28 | `VkDynamicState` `LINE_WIDTH` | not dynamic on Metal 4 without private API | — | 4 | MoltenVK needs `MVK_USE_METAL_PRIVATE_API` for `wideLines`. `[V]` |
| 29 | `gl_Position` | `[[position]]` | MSL attribute | 1 | — |
| 30 | `gl_PointSize` (`VKVertexProgram.cpp:289`) | `[[point_size]]` | MSL attribute | 1 | SPIRV-Cross `enable_point_size_builtin`. `[V]` |
| 31 | `gl_ClipDistance[0..5]` (`VKVertexProgram.cpp:286-292`) | `[[clip_distance]]` array | MSL attribute | 2 | Emitted only when written. `[V]` |
| 32 | `gl_FragCoord` / `gl_FrontFacing` | `[[position]]` (frag) / `[[front_facing]]` | MSL attributes | 2 | `[[position]]` in frag is pixel coords. `[V]` |
| 33 | `gl_FragDepth` (`RSXROPEpilogue.glsl:79`) | `[[depth(any)]]` | MSL attribute | 2 | Must force depth write ordering for input-attachment==DS. `[V]` |
| 34 | `gl_SampleID` (`MSAA/*.glsl`) | `[[sample_id]]` | MSL attribute | 2 | Forces per-sample shading (`force_sample_rate_shading`). `[V]` |
| 35 | `gl_FragStencilRefARB` (`MSAA/DepthStencilResolvePass.glsl:25`) | `[[stencil]]` | MSL attribute | 2 | — |
| 36 | `gl_VertexIndex` (software pull) | `[[vertex_id]]` | MSL attribute | 2 | Index type baked per pipeline in MSL (`IndexType` opt). `[V]` |
| 37 | `gl_BaryCoord{EXT,AMD}` (`GLSLCommon.cpp:760`) | `[[barycentric_coord]]` | MSL attribute | 3 | Requires `VK_KHR_fragment_shader_barycentric`; check MSL support. `[V]` |
| 38 | `discard` (`RSXROPEpilogue.glsl:4,37,44`) | `discard_fragment()` | MSL function | 2 | Plus KK W4/W5 workarounds (helper-thread / premature discard). `[V]` |
| 39 | Varyings (RSX outputs 0..~22) | `[[user(locnN)]]` struct in/out | MSL stage_in/out | 2 | SPIRV-Cross handles; must match names/structs (macOS requires). `[V]` |
| 40 | VS `textureLod`/`texelFetch` (VTF) `GLSLCommon.cpp:642-651` | MSL vertex texture sample | `tex.sample(smp, …)` in vertex | 2 | — |
| 41 | `sampler2DMS`/`usampler2DMS` | `texture2d_ms` | MSL type | 2 | SPIRV-Cross maps. `[V]` |
| 42 | `sampler2DShadow` shadow compare (`GLSLCommon.cpp:454`) | `sample_compare` (or emulated) | MSL `depth2d` | 3 | Emulation path already exists in GLSL; MSL has native `sample_compare`. `[V]` |
| 43 | Programmable blending ROP push-constant (28B) | function constant / buffer param | MSL `[[buffer(n)]]` | 4 | Interacts with #15 input attachment. `[V]` |
| 44 | `subgroupShuffleXor` etc. (FSR only) `FSR1/fsr_ffx_a_flattened.inc:330` | `simd_*` | `simd_shuffle_xor`, `simd_ballot`, `simd_*` | 3 | Native but KK W3 (`simd_ballot` in conditional misbehaves). `[V]` |
| 45 | `VK_EXT_shader_explicit_arithmetic_types_float16` | MSL `half` | native | 2 | MSL has half; watch `fp16` transcendentals. `[V]` |
| 46 | `SEPARATE_SHADER_OBJECTS` | n/a | — | 2 | Metal links VS/FS at PSO creation; no separate link step. `[V]` |
| 47 | `VkPipelineTessellationStateCreateInfo` stub (`VKShaderInterpreter.cpp:35`) | n/a — no TS/GS | — | 1 | Dead; delete. `[V]` |

---

## 3. Pristine reference code

Three pristine references were mined. Summary of *what each actually does* (not marketing copy):

### 3.1 KosmicKrisp (Mesa), the closest analogue — Vulkan→Metal 4, NIR→MSL
- **Its own NIR→MSL backend, not SPIRV-Cross.** `src/kosmickrisp/compiler/nir_to_msl.c`, `msl_nir_lower_common.c`, `msl_nir_lower_subgroups.c`, `msl_iomap.c`. It lowers Mesa NIR directly to MSL text. `[V]`
- **Argument table bridge:** `bridge/mtl_argument_table.h` exposes exactly two ops: `mtl_set_max_buffer_binding_count(desc,count)` and `mtl_set_address(table, address, binding)`. `[V]` i.e. the same Metal 4 argument-table model used here.
- **Its documented MSL/Metal bugs are the real prize** — `docs/drivers/kosmickrisp/workarounds.rst` (18 entries). The ones that will bite an RPCS3 port:
  - **W1** uninitialized local scratch → MSL compiler **crash** (fixed macOS 26). `[V]`
  - **W2** `while(true)` with loop-invariant break → compiler **crash / infinite loop** (still needed on M5 in macOS 27). `[V]`
  - **W3** `simd_ballot`/`quad_any`/`quad_all` inside a conditional returns **all active threads** — must rewrite as `simd_or`. `[V]`
  - **W4** `simd_is_helper_thread()` stays `false` after `discard_fragment()` on M1/M2. `[V]`
  - **W5** fragment shaders with side effects are **prematurely discarded** if a `discard_fragment()` always executes. `[V]`
  - **W6** device memory **not coherent** within a single thread across loop iterations unless marked `coherent`. `[V]`
  - **W7** `sample_mask` out is **ignored** for a stencil-only MS depth/stencil attachment with no color attachment → RSX-relevant (RSX uses stencil a lot). `[V]`
  - **W9** reconvergence: sole `break` in a loop is **re-ordered** past subgroup ops. `[V]`
  - **W10** `bcsel` (select) miscompile. `[V]`
  - **W11** concurrent `MTL4Compiler` instances **corrupt the heap** → keep one compiler **per `MTLDevice`** (fixed macOS 27 beta 5). `[V]`
  - **W13/W12** index-buffer OOB is **not robust** unless 32-bit aligned; macOS 26 missed index-length setup in the Metal 4 draw path. `[V]`
  - **W15** `volatile`/`coherent` device accesses miscompiled. `[V]`
  - **W16** M5 clamps OOB LOD instead of returning (0,0,0,1). `[V]`
- KK **does not use SPIRV-Cross**; it consumes NIR. RPCS3 emits GLSL, so KK's *compiler* is not directly reusable, but its *workaround catalogue* and its *argument-table bridge* are the transferable assets. `[I]`

### 3.2 Godot (reference for SPIR-V→MSL via SPIRV-Cross)
`drivers/metal/rendering_shader_container_metal.cpp` `[V]`:
- Uses `spirv_cross::CompilerMSL`; sets `set_msl_version`; `msl_options.texture_buffer_native = true`; `r32ui_alignment_constant_id`; `force_active_argument_buffer_resources`; `pad_argument_buffer_resources`; `agx_manual_cube_grad_fixup = true`; `readwrite_texture_fences = false` for MSL ≥ 3.2.
- `argument_buffers`: **Tier 2 if supported, else Tier 1**; Tier 1 disables argument buffers because writable textures aren't supported there (mirrors SPIRV-Cross `ArgumentBuffersTier`).
- Adds `MSLResourceBinding` per SPIR-V binding to remap set/binding → msl_buffer/msl_texture/msl_sampler.
- Compiles MSL offline: `xcrun -sdk macosx metal -O3 -fmetal-math-mode=fast` (adds `-fpreserve-invariance` when needed), packs a `.metallib`; runtime uses `MTL::BinaryArchive` + `MTL::FunctionConstantValues`.
- **Important correction:** this Godot checkout is a **Metal 3** driver. It only forward-declares `MTL4::MDCommandBuffer`; there is **no Metal 4 driver in the tree**. Any claim that "Godot's metal.cpp is a Metal 4 reference" is false for this checkout. `[V]`

### 3.3 Apple game-porting-toolkit skills (the canonical Metal 4 mapping)
`[V]` — Apple's own `translating-to-metal4-api` and `creating-metal4-shader-pipelines` skills state verbatim:
- `VkPipelineLayout` → **"Argument table layout"**; `vkCmdBindDescriptorSets` → `[encoder setArgumentTable:]`; `vkCmdPushConstants` → `[table setBytes:length:atIndex:]`.
- **But** metal-cpp `MTL4::ArgumentTable` (SDK verified) has **no `setBytes`** — only `setAddress/setTexture/setSamplerState/setResource`. `[V]` → push constants must go through a transient buffer (§6, §7 Q1).
- `MSLSourceCompilation`: `MTL4::LibraryDescriptor.setSource(mslSourceString)` then `compiler->newLibrary(desc,&err)`. Offline alternative `xcrun metal`. `[V]`
- `MTLFunctionConstantValues` + `MTL4::SpecializedFunctionDescriptor` for specialization; `newRenderPipelineStateBySpecialization` for flexible PSOs; `MTL4Archive` + `MTL4PipelineDataSetSerializer` for caching. `[V]`
- Descriptor-set → argument-table mapping: `VK_DESCRIPTOR_TYPE_*` → `setAddress`/`setTexture`/`setSamplerState`. `[V]`
- **Programmable blending is explicitly the recommended replacement for subpass read-back on TBDR** ("No subpass equivalent — use programmable blending"). `[V]` This is directly relevant to RPCS3's ROP/`subpassInput` path (#15/#43).

### 3.4 SPIRV-Cross MSL backend surface (pristine header)
`spirv_msl.hpp` `[V]` — exact `CompilerMSL::Options` fields RPCS3's driver will need:
`msl_version`, `texture_buffer_native`, `texel_buffer_texture_width` (default 4096), `r32ui_linear_texture_alignment` (4), `r32ui_alignment_constant_id`, `argument_buffers`, `argument_buffers_tier`, `force_active_argument_buffer_resources`, `pad_argument_buffer_resources`, `runtime_array_rich_descriptor`, `enable_point_size_builtin`, `enable_point_size_default`, `enable_frag_depth_builtin`, `enable_frag_stencil_ref_builtin`, `enable_clip_distance_user_varying`, `use_framebuffer_fetch_subpasses`, `force_sample_rate_shading`, `manual_helper_invocation_updates`, `check_discarded_frag_stores`, `sample_dref_lod_array_as_grad`, `readwrite_texture_fences`, `agx_manual_cube_grad_fixup`, `force_fragment_with_side_effects_execution`, `input_attachment_is_ds_attachment`, `use_fast_math_pragmas`, `invariant_float_math`, `emulate_subgroups`, `fixed_subgroup_size`, `vertex_index_type` (`IndexType::None/UInt16/UInt32`), `replace_recursive_inputs`, `emulate_reversed_depth_viewport`, `emulate_depth_clip_enable`.
API: `add_msl_resource_binding(MSLResourceBinding)`, `add_discrete_descriptor_set`, `add_dynamic_buffer`, `add_inline_uniform_block`, `set_argument_buffer_device_address_space`, `remap_constexpr_sampler_by_binding`, `set_fragment_output_components`, `get_automatic_msl_resource_binding[_secondary]`. `[V]`
SPIR-V → MSL ops that are *emulated* in generated code (SPVFuncImpl enum): texel-buffer coords, image 2D atomics, gradient cube, inverse 2x2/3x3/4x4, gather swizzles, whole subgroup family, quad broadcast/swap, chroma reconstruction, `spvArrayCopy`, `spvDescriptorArray`, padded std140, `MulExtended` (`spirv_msl.hpp` protected enum). `[V]`

**Most important thing learned from the pristine references:** Metal 4 does **not** introduce a new shader-binary model — the argument table is a **binding mechanism** that indexes the *classic* MSL `[[buffer(N)]]`/`[[texture(N)]]`/`[[sampler(N)]]` spaces. Therefore **SPIRV-Cross needs no Metal-4 awareness**: its ordinary MSL output binds directly into a Metal 4 argument table, and the entire port risk is in the *binding/setup* layer (argument table sizing + push-constant emulation + texel buffers + input attachments), not in the translator. `[V]`/`[I]`

### 3.5 Instruction-level gap sub-table (MSL feature | supported? | reference emulation | difficulty)
`[V]` unless marked.

| MSL feature / instruction | Supported? | Reference emulation | Difficulty |
|---|---|---|---|
| f32 add/sub/mul/div, min/max | yes | native | 1 |
| fma / MAD | yes (`fma`) | native; RPCS3 `MAD`→`fma` (`FragmentProgramDecompiler.cpp:1097`) | 1 |
| `rcp`, `rsq`, `sqrt`, `exp2`, `log2`, `sin`, `cos`, `pow` | yes (fast-math caveat) | native; RPCS3 has `_builtin_*` wrappers (`:964-970`) | 1 |
| `floor`/`fract` | yes | native (`_builtin_floor` adds ε, `:978`) | 1 |
| 16-bit `half` / fp16 arithmetic | yes | MSL `half`; watch fp16 transcendentals | 2 |
| Pack/unpack half2x16, snorm4x8, unorm4x8 (`PK2/PK4/PK16/PKB`) | partial | MSL `as_type`, `pack_float_to_*`; else bit ops | 2 |
| `bitfieldExtract/Insert`, `bitCount`, `findLSB/MSB`, `bitfieldReverse` | yes | native (`_get_bits/_set_bits` equivalents) | 1 |
| `floatBitsToInt/Uint`, `intBitsToFloat` | yes | `as_type<T>()` | 1 |
| derivatives `dFdx/dFdy/fwidth` (coarse/fine) | yes | `dfdx/dfdy`; coarse/fine from MSL 2.3 | 2 |
| texture `sample`, `sample_lod`, `texelFetch` | yes | native | 1 |
| texture projection (`textureProj`) | partial | `sample` with divided coords (RPCS3 already does `/ $0.w`, `GLSLCommon.cpp:572-640`) | 2 |
| shadow compare (`sampler2DShadow`) | yes | MSL `depth2d::sample_compare`; else `_EMULATED_TEXSHADOW` | 3 |
| MSAA sampling (`texture2d_ms`) | yes | native; `force_sample_rate_shading` | 2 |
| texel buffer (`usamplerBuffer`) | **no native** | `texture_buffer_native` (`texture_buffer<float>`); Godot sets it | **4** |
| image read/write + atomics | partial | native MSL 3.1+; else `SPVFuncImplImage2DAtomicCoords` | 3 |
| 64-bit integer arithmetic | partial | MSL supports `long`; **no 64-bit atomics** (MoltenVK reports unsupported) | 4 |
| subgroup ops (`simd_shuffle_xor`, `simd_ballot`, …) | yes | native; KK W3 rewrite needed in conditionals | 3 |
| quad ops (`quad_any/all`) | yes | native; KK W3 | 3 |
| `discard_fragment()` | yes | native; KK W4/W5, `manual_helper_invocation_updates`, `check_discarded_frag_stores` | 4 |
| `[[sample_mask]]` out | partial | MSL; KK W7 stencil-only MS bug; MoltenVK native only w/ private API | 4 |
| threadgroup memory + `threadgroup_barrier` | yes | native | 2 |
| memory fences | yes (MSL 3.2+) | `atomic_thread_fence`; KK W6/W15 coherency | 3 |
| reconvergence / structured control flow | yes w/ bugs | native; KK W9/W2 | 4 |
| non-uniform control flow arithmetic | partial | normal; but KK W9 break reorder | 3 |
| `double` / fp64 | no on Apple GPUs | — | 5 |
| dot products (`DP2/DP3/DP4`, `SPV_EXT_integer_dot_product`) | yes | native / SPIRV-Cross `dot` lowering | 2 |
| function constants (`[[function_constant]]`) | yes | MSL; `SpecializedFunctionDescriptor` | 2 |
| barycentric `[[barycentric_coord]]` | ? | MSL attribute; verify MSL support | 3 `[U]` |
| mesh/object shaders | n/a for RPCS3 | — | 1 |
| ray tracing | n/a for RPCS3 | — | 1 |

---

## 4. Impossible / not-recommended

**Impossible (no workable path):**
1. **`double` / fp64 shaders on Apple GPUs.** Metal has no fp64. Not used by RPCS3 today. `[V]`
2. **64-bit atomics.** SPIRV-Cross reports them unsupported; MoltenVK's own changelog lists "Report unsupported 64-bit atomics"; not used by RSX shaders. `[V]`
3. **True `VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER` as a genuine `usamplerBuffer`.** Metal has no 1D/samplerBuffer alias; must be `texture_buffer` (a 2D texture with `width == elementCount`) or read via SSBO. Not bit-for-bit identical semantics. `[V]`
4. **Vulkan push constants as a first-class, register-fast inline blob.** Metal 4 argument tables (metal-cpp) have no `setBytes`; the value must live in memory. `[V]`
5. **Per-sample coverage from a static pipeline sample mask without private API.** `MTLRenderPipelineDescriptor.sampleMask` requires `MVK_USE_METAL_PRIVATE_API` in MoltenVK, disqualifying App-Store distribution; MSL `[[sample_mask]]` is the public path but hits KK W7. `[V]`
6. **Geometry/tessellation shaders.** Not needed (RSX has none), and Metal has no GS — would require mesh-shader emulation à la Metal shader converter. `[V]`

**Not recommended:**
1. **Reusing MoltenVK's shipped MSL.** MoltenVK is now **deprecated by Khronos**, is a Vulkan *layer* (not a Metal 4 pipeline creator), and translates to Metal 3-era MSL. `[V]`
2. **Using Godot's Metal driver as the template.** It is Metal 3, not Metal 4, and has no `MTL4*` objects beyond a forward declaration. `[V]`
3. **Hand-writing a NIR→MSL or SPIR-V→MSL backend.** KK needed a full backend because it starts from NIR; RPCS3 starts from SPIR-V, so SPIRV-Cross (battle-tested, used by both MoltenVK and Godot) is strictly less work. `[I]`
4. **Porting KK's GS/tessellation compute emulation.** Dead weight; see §1.3. `[I]`
5. **Relying on `SEPARATE_SHADER_OBJECTS` / separate link step.** Doesn't exist in Metal 4; binding is only validated when the PSO is created against the argument table. `[V]`
6. **Dynamic `LINE_WIDTH`.** Unsupportable publicly on Metal (`wideLines` needs private API). Clamp to 1.0. `[V]`

---

## 5. Tricky (ranked)

Ranked by *risk × effort*, hardest first.

**T1 — Descriptor/argument-table re-architecture incl. push constants (D4-5).**
The whole binding model changes: `VkPipelineLayout`+`VkDescriptorSet`+`VkDescriptorSetLayout` are replaced by a flat, per-stage argument table whose *sizes* must be derived from reflected bindings (there is no layout object). RPCS3 currently builds layouts per program (`VKProgramPipeline.cpp:361-695`). Two sub-risks: (a) sizing `maxBufferBindCount/maxTextureBindCount/maxSamplerStateBindCount` correctly for the 12+ fixed slots plus 12+ textures; (b) `vkCmdPushConstants` has no table `setBytes` equivalent (metal-cpp), so VS `draw_parameters_offset` (4B) and FS programmable-blending (28B) must be transferred via a transient buffer bound by GPU address, or via function constants. See §7 Q1.

**T2 — Texel buffers / software vertex pulling (`usamplerBuffer`) (D4).**
RPCS3's entire vertex path reads RSX streams via `usamplerBuffer` (`RSXVertexFetch.glsl:75`, `VKVertexProgram.cpp:173`). Metal's only route is `texture_buffer<T>` (2D, width `texel_buffer_texture_width`=4096, `r32ui_linear_texture_alignment`=4) or manual SSBO indexing. This also interacts with buffer alignment and with the argument-table texture space. Godot sets `texture_buffer_native=true`; RPCS3 must do the same and validate the R32ui alignment path against the existing fetch byte/word math.

**T3 — Input attachments → framebuffer fetch / programmable blending (D4).**
`subpassInput(MS)` (ROP read-back, `VKFragmentProgram.cpp:277`) has no Metal subpass. On TBDR this becomes tile framebuffer fetch (`[[color(n)]]`) — explicitly the recommended replacement in Apple's own cross-API table — which then couples to (a) the FS programmable-blending push constant block (`VKFragmentProgram.cpp:341`), (b) load actions (`Load` required or reads are UB), and (c) the "input attachment == depth/stencil attachment" hazard (SPIRV-Cross `input_attachment_is_ds_attachment`).

**T4 — `discard` semantics + MSL compiler bugs (D4).**
RPCS3's ROP epilogue discards on alpha-test/fragment ops (`RSXROPEpilogue.glsl:4,37,44`), and decompiled shaders emit plenty of scratch. The KK catalogue predicts concrete failures: W5 (premature discard of side-effecting fragments), W4 (`simd_is_helper_thread` after discard, M1/M2), W1 (uninitialized scratch → compiler crash), W2 (`while(true)` → crash/infinite loop). Mitigations are SPIRV-Cross options `manual_helper_invocation_updates`, `check_discarded_frag_stores`, `force_fragment_with_side_effects_execution` plus KK-style scratch initialisation.

**T5 — Stencil + `sample_mask` (D4).**
RSX relies heavily on stencil. KK W7: `sample_mask` out is ignored for a stencil-only MS depth/stencil attachment (no color) → must emulate with a conditional `discard_fragment()` at the end of the shader. Interacts with `MSAA/DepthStencilResolvePass.glsl` (`gl_FragStencilRefARB`).

**T6 — Subgroup/quad ops (FSR1 only) (D3).**
Only used by the FSR1 upscaler (`fsr_ffx_a_flattened.inc:330` `subgroupShuffleXor`). Maps to `simd_shuffle_xor` etc., but KK W3 requires rewriting ballot/vote inside conditionals as `simd_or`, and reconvergence (W9) can re-order breaks.

**T7 — Pipeline cache serialisation (D3).**
`VkPipelineCache` (`g_pipeline_cache`) → `MTL4PipelineDataSetSerializer` + `MTL4Archive`. Straightforward but needs stable, order-independent descriptor population; also KK W11 says one `MTL4Compiler` per `MTLDevice` to avoid heap corruption.

**T8 — Push-constant → buffer layout binding fidelity (D3).**
Vulkan push constants have exact byte-packing; the MSL replacement buffer must reproduce offsets (`offset=4, size=28` for FS blending). SPIRV-Cross's push-constant handling (`kPushConstDescSet/kPushConstBinding`, `add_inline_uniform_block`) is the reference; Metal has no push constants, so this becomes a `[[buffer(n)]]` `constant` reference.

**T9 — Barycentric interpolation (D3).**
`gl_BaryCoord{EXT,AMD}` (`GLSLCommon.cpp:760-771`) → MSL `[[barycentric_coord]]`. MSL support for the attribute is not confirmed this session. `[U]`

**T10 — Depth/stencil PSO split + dynamic stencil (D2-3).**
`VkPipelineDepthStencilStateCreateInfo` is inside the PSO on Vulkan but a **separate `MTL::DepthStencilState`** on Metal, set on the encoder; dynamic stencil compare/write/ref become encoder setters. Mechanical but touches every draw.

---

## 6. Minimal Metal 4 code sketch (metal-cpp)

Two halves: (A) the RPCS3-side SPIR-V→MSL translator configuration (SPIRV-Cross), and (B) the Metal 4 objects. Signatures verified against `/tmp/metal4-refs/metal-cpp/Metal/MTL4*.hpp` and the Apple `metal-4-basics` sample. `[V]`

### (A) SPIR-V → MSL (SPIRV-Cross), configured for RPCS3's binding model
```cpp
#include <spirv_msl.hpp>

std::string spirv_to_msl(const std::vector<uint32_t>& spv,
                         const std::vector<Binding>& bindings /*reflected set/binding*/)
{
    spirv_cross::CompilerMSL msl(spv);
    auto& opts = msl.get_msl_options();

    opts.set_msl_version(3, 1);                 // or 4,0 where available; MSL 4 is C++17-based
    opts.platform                       = spirv_cross::CompilerMSL::Options::macOS;
    opts.texture_buffer_native          = true; // usamplerBuffer -> texture_buffer<T>  (T2)
    opts.r32ui_alignment_constant_id    = 65535;
    opts.enable_point_size_builtin      = true; // gl_PointSize (VKVertexProgram.cpp:289)
    opts.enable_frag_depth_builtin      = true; // gl_FragDepth   (RSXROPEpilogue.glsl:79)
    opts.enable_frag_stencil_ref_builtin= true; // gl_FragStencilRefARB
    opts.enable_clip_distance_user_varying = true; // gl_ClipDistance[0..5]
    opts.manual_helper_invocation_updates  = true; // KK W4
    opts.check_discarded_frag_stores       = true; // KK W5
    opts.force_fragment_with_side_effects_execution = true;
    opts.agx_manual_cube_grad_fixup     = true; // Apple Silicon cube gradients
    opts.readwrite_texture_fences       = false; // MSL >= 3.2 (Godot sets this)
    opts.input_attachment_is_ds_attachment = true; // ROP reads depth/stencil

    // Argument tables use the CLASSIC [[buffer(N)]]/[[texture(N)]]/[[sampler(N)]] space,
    // so we do NOT enable MSL argument buffers. We only flatten sets->slots:
    for (const auto& b : bindings) {
        spirv_cross::MSLResourceBinding rb;
        rb.stage = b.stage;
        rb.desc_set = b.set;
        rb.binding  = b.binding;
        rb.msl_buffer  = b.slot;            // flat slot from pipeline_binding_table.h
        rb.msl_texture = b.slot;            // combined image sampler splits into texture+sampler
        rb.msl_sampler = b.slot;
        msl.add_msl_resource_binding(rb);
    }
    // Push constants: MSL has no push constants -> expose as a constant buffer param.
    // (SPIRV-Cross handles kPushConstDescSet/kPushConstBinding; verify per-version.)
    return msl.compile();
}
```
*(The push-constant → buffer decision is the crux; see §7 Q1.)*

### (B) Metal 4 pipeline + argument table (metal-cpp)
```cpp
using namespace MTL;
using namespace MTL4;

// One compiler per MTLDevice (KK W11).
CompilerDescriptor* cdesc = CompilerDescriptor::alloc()->init();
Compiler* compiler = device->newCompiler(cdesc, &err);

// MSL source -> library.
LibraryDescriptor* ldesc = LibraryDescriptor::alloc()->init();
ldesc->setSource(NS::String::string(msl_source.c_str(), NS::UTF8StringEncoding));
ldesc->setOptions(compile_options /* fast-math / preserve-invariance */);
Library* lib = compiler->newLibrary(ldesc, &err);

// Function descriptors (NOT MTLFunction).
LibraryFunctionDescriptor* vsD = LibraryFunctionDescriptor::alloc()->init();
vsD->setLibrary(lib); vsD->setName(NS::String::string("main0", NS::UTF8StringEncoding));
LibraryFunctionDescriptor* fsD = LibraryFunctionDescriptor::alloc()->init();
fsD->setLibrary(lib); fsD->setName(NS::String::string("main0", NS::UTF8StringEncoding));

// Render PSO.
RenderPipelineDescriptor* pd = RenderPipelineDescriptor::alloc()->init();
pd->setVertexFunctionDescriptor(vsD);
pd->setFragmentFunctionDescriptor(fsD);
pd->setRasterSampleCount(4);                                  // MSAA
pd->setInputPrimitiveTopology(PrimitiveTopologyClass::PrimitiveTopologyClassTriangle);
pd->colorAttachments()->object(0)->setPixelFormat(PixelFormat::PixelFormatBGRA8Unorm);
RenderPipelineState* pso = compiler->newRenderPipelineState(pd, nullptr, &err);

// Argument table sized to the highest slot used (pipeline_binding_table.h -> 12 + textures).
ArgumentTableDescriptor* atd = ArgumentTableDescriptor::alloc()->init();
atd->setMaxBufferBindCount(32);
atd->setMaxTextureBindCount(32);
atd->setMaxSamplerStateBindCount(32);
atd->setInitializeBindings(true);          // deterministic null for unbound slots
ArgumentTable* vtable = device->newArgumentTable(atd, &err);
ArgumentTable* ftable = device->newArgumentTable(atd, &err);

// Bind buffers by GPU address (supports offsets -> dynamic offsets / draw params).
vtable->setAddress(vertexParamsBuf->gpuAddress(), 0);   // vertex_params_bind_slot = 0
ftable->setAddress(fragmentStateBuf->gpuAddress(), 3);  // fragment_state_bind_slot = 3
// Textures + samplers split (combined image sampler).
ftable->setTexture(tex->gpuResourceID(), 12);           // textures_first_bind_slot = 12
ftable->setSamplerState(smp->gpuResourceID(), 12);

// Push constants: EITHER a transient buffer bound by address (no table setBytes in metal-cpp),
// OR compiled-in via function constants. e.g.
ftable->setAddress(blendConstantsBuf->gpuAddress() + 4, /*slot*/ 31);

// Encode.
RenderPassDescriptor* rp = renderPassDescriptorForFrame();
RenderCommandEncoder* enc = cmd->renderCommandEncoder(rp);
enc->setRenderPipelineState(pso);
enc->setArgumentTable(vtable, RenderStage::RenderStageVertex);
enc->setArgumentTable(ftable, RenderStage::RenderStageFragment);
enc->setViewports(&vp, 1);
enc->setScissorRects(&sc, 1);
enc->drawIndexedPrimitives(PrimitiveType::PrimitiveTypeTriangle,
                           indexCount, IndexType::IndexTypeUInt16,
                           indexBuffer->gpuAddress() + indexOffset,
                           indexBufferLength /*full accessible range*/,
                           instanceCount, baseVertex, baseInstance);
```
Notes: all bound resources must be in an `MTL::ResidencySet`; `indexBufferLength` must be the **full accessible range**, not `indexCount*size` (silent clamping otherwise); index addresses must be aligned to the index type size (KK W13/W12). `[V]`

---

## 7. Open questions for the human

**Q1. Push constants: transient buffer vs function constants?** metal-cpp `MTL4::ArgumentTable` exposes no `setBytes` (only `setAddress/setTexture/setSamplerState/setResource`), yet Apple's own cross-API table lists `[table setBytes:length:atIndex:]`. Either (a) the C++/metal-cpp surface lags the ObjC/SDK surface, or (b) `setBytes` is not actually available and push constants must be emulated with a transient buffer. Which does the target SDK actually expose, and do we want a per-draw bump-allocated constant buffer (matching Apple's "Transient Buffer Allocation" pattern) or function-constant specialisation for the FS blend block? — blocks T1/T8. `[U]`

**Q2. MSL language version / SPIRV-Cross support for MSL 4.** The header only shows `set_msl_version(major,minor,patch)`; MoltenVK references "MSL 4.0+" behaviour but we did not confirm the exact max supported `msl_version`. What MSL version should the RPCS3 backend target (3.1 vs 4.0), and does SPIRV-Cross in our pinned revision emit it correctly? `[U]`

**Q3. Argument-buffer tiers vs argument tables.** Godot uses SPIRV-Cross *argument buffers* (Tier 1/2) to model descriptor sets. Apple's Metal 4 *argument table* is a different mechanism. Since Metal 4 argument tables can bind classic `[[buffer(N)]]`/`[[texture(N)]]` directly, is the plan to (a) flatten set/binding to a single flat slot space and bind the classic way (no SPIRV-Cross argument buffers), or (b) keep MSL argument buffers and bind one argument-buffer GPU address per set? (a) is simpler; (b) matches RPCS3's existing per-set structure. Decision affects slot-limit headroom. `[I]`

**Q4. Texel-buffer width/alignment.** `texel_buffer_texture_width` default is 4096 and `r32ui_linear_texture_alignment` is 4. RSX vertex streams can exceed 4096 u32? Do we accept the 2D-width tiling, or read streams through a raw SSBO (which would mean editing `RSXVertexFetch.glsl` — out of scope for Scope 3)? `[I]`

**Q5. Barycentrics.** Does the target MSL version expose `[[barycentric_coord]]`, and do we need the `EXT` vs `AMD` flavour distinction (`GLSLCommon.cpp:760-771`) preserved? `[U]`

**Q6. Workaround policy.** Do we adopt KK's cosmetic workarounds unconditionally (e.g. initialise all scratch, wrap discards in always-true conditionals) even where the underlying Apple bug is stated fixed in macOS 27, or gate them on OS version? RPCS3's min-OS target matters here. `[I]`

**Q7. Does Scope 3 own the ROP/programmable-blend shader rewrite** (input attachment → framebuffer fetch), or does that belong to a shaders-scope sibling? It straddles binding (#15) and shader-body (#43). `[I]`

---

## 8. Sources

All fetched/read this session unless noted. `[V]` sources.
Immutable raw URLs (HTTP 200) are the load-bearing ones; Apple HTML doc pages returned no text (JS-rendered) and are cited for reference only.

**Apple — Metal 4 official**
1. Understanding the Metal 4 core API — https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api *(HTML, no text returned)*
2. Using the Metal 4 compilation API — https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api *(HTML, no text)*
3. Drawing a triangle with Metal 4 — https://developer.apple.com/documentation/metal/drawing-a-triangle-with-metal-4 *(HTML, no text)*
4. `MTL4ArgumentTable` — https://developer.apple.com/documentation/metal/mtl4argumenttable *(HTML, no text)*
5. WWDC25 session 205 — https://developer.apple.com/videos/play/wwdc2025/205/
6. WWDC25 session 254 — https://developer.apple.com/videos/play/wwdc2025/254/
7. Metal Shading Language Specification v4.1 (2026-06-04) — https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf *(PDF, not machine-read this session)*
8. MSL Specification v4 (2025-10-23) — https://developer.apple.com/metal/metal-shading-language-specification.pdf

**Apple — game-porting-toolkit skills (the canonical Metal 4 translation maps)** `[V]`
9. translating-to-metal4-api SKILL — https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/translating-to-metal4-api/SKILL.md
10. cross-api-translation (Vulkan→Metal 4) — https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/translating-to-metal4-api/references/cross-api-translation.md
11. creating-metal4-shader-pipelines SKILL — https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/creating-metal4-shader-pipelines/SKILL.md
12. integrating-metal-shaderconverter-shaders SKILL — https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/integrating-metal-shaderconverter-shaders/SKILL.md
13. …/references/binding-model.md — https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/integrating-metal-shaderconverter-shaders/references/binding-model.md
14. …/references/geometry-tessellation.md — https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/integrating-metal-shaderconverter-shaders/references/geometry-tessellation.md
15. compiling-with-metal-shaderconverter SKILL — https://raw.githubusercontent.com/apple/game-porting-toolkit/main/game-porting-skills/skills/compiling-with-metal-shaderconverter/SKILL.md

**SPIRV-Cross** `[V]`
16. README — https://raw.githubusercontent.com/KhronosGroup/SPIRV-Cross/main/README.md
17. `spirv_msl.hpp` (CompilerMSL Options/API) — https://raw.githubusercontent.com/KhronosGroup/SPIRV-Cross/main/spirv_msl.hpp
18. `spirv_common.hpp` (SPIRType, decorations) — https://raw.githubusercontent.com/KhronosGroup/SPIRV-Cross/main/spirv_common.hpp
19. Wiki home — https://github.com/KhronosGroup/SPIRV-Cross/wiki
20. Issue #2229 — https://github.com/KhronosGroup/SPIRV-Cross/issues/2229
21. Issue #1840 (gl_CullDistance) — https://github.com/KhronosGroup/SPIRV-Cross/issues/1840
22. Issue #1004 — https://github.com/KhronosGroup/SPIRV-Cross/issues/1004
23. DeepWiki: MSL backend — https://deepwiki.com/KhronosGroup/SPIRV-Cross/3.1-metal-shading-language-(msl)

**MoltenVK** `[V]`
24. README (deprecation notice; "Vulkan 1.4 subset") — https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/README.md
25. `Docs/Whats_New.md` (MSL/SPIRV-Cross history; 64-bit atomics; argument buffers) — https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/Docs/Whats_New.md
26. Issue #2638 — https://github.com/KhronosGroup/MoltenVK/issues/2638

**Mesa KosmicKrisp** `[V]` (local checkout `/tmp/metal4-refs/mesa`)
27. `docs/drivers/kosmickrisp/workarounds.rst` (KK_WORKAROUND_1…18)
28. `docs/drivers/kosmickrisp.rst`
29. `src/kosmickrisp/compiler/nir_to_msl.c`, `msl_nir_lower_common.c`, `msl_nir_lower_subgroups.c`, `msl_iomap.c`
30. `src/kosmickrisp/bridge/mtl_argument_table.h`, `mtl_compiler.h`

**Apple Metal 4 metal-cpp headers** `[V]` (local checkout `/tmp/metal4-refs/metal-cpp/Metal`)
31. `MTL4ArgumentTable.hpp`, `MTL4Compiler.hpp`, `MTL4RenderPipeline.hpp`, `MTL4LibraryDescriptor.hpp`, `MTL4LibraryFunctionDescriptor.hpp`, `MTL4SpecializedFunctionDescriptor.hpp`, `MTL4PipelineDataSetSerializer.hpp`, `MTL4Archive.hpp`, `MTL4BinaryFunction.hpp`, `MTL4LinkingDescriptor.hpp`

**Apple metal-4-basics sample** `[V]` (`/tmp/metal4-refs/metal-4-basics`)
32. `Metal4Basics/Sources/Engine/Metal4Renderer.swift` (compiler, argument table, function constants, bindings)
33. `Metal4Basics/Sources/Shaders/PBR.metal` (`[[function_constant(N)]]`, `[[attribute]]`, `[[buffer]]`, `[[position]]`)

**Godot** `[V]` (`/tmp/metal4-refs/godot`)
34. `drivers/metal/rendering_shader_container_metal.cpp` (CompilerMSL options, MSL version, argument-buffer tier, texture_buffer_native)
35. `drivers/metal/rendering_device_driver_metal.cpp`, `metal3_objects.*` (Metal 3 — confirms no Metal 4 driver in this tree)

**In-repo RPCS3 (read-only)** `[V]`
36. `rpcs3/Emu/RSX/Program/SPIRVCommon.cpp` (glslang config)
37. `rpcs3/Emu/RSX/VK/VKProgramPipeline.{h,cpp}` (descriptor types, layout, push-constant ranges, pool)
38. `rpcs3/Emu/RSX/VK/vkutils/pipeline_binding_table.h` (fixed slots)
39. `rpcs3/Emu/RSX/VK/VKVertexProgram.cpp` / `VKFragmentProgram.cpp` (builtins, push constants, samplers, usamplerBuffer)
40. `rpcs3/Emu/RSX/VK/VKPipelineCompiler.{h,cpp}`, `vkutils/graphics_pipeline_state.hpp` (caching, state)
41. `rpcs3/Emu/RSX/VK/VKShaderInterpreter.cpp` (dynamic state list; tessellation stub)
42. `rpcs3/Emu/RSX/Program/FragmentProgramDecompiler.cpp` (opcodes, `_builtin_*`, pack/unpack)
43. `rpcs3/Emu/RSX/Program/GLSLCommon.cpp` + `GLSLSnippets/RSXProg/*.glsl` (fetch, ROP, MSAA, depth conversion)
44. `rpcs3/Emu/RSX/Program/Upscalers/FSR1/fsr_ffx_a_flattened.inc` (subgroup ops)
45. `rpcs3/Emu/RSX/Program/MSAA/*.glsl`, `CopyBufferToGenericImage.glsl` (`gl_SampleID`, `gl_FragStencilRefARB`)

---

*End of Scope 3 research doc. Unverified items are explicitly marked `[U]`; the three highest-risk `[U]` items are Q1 (push-constant `setBytes`), Q2 (MSL version), Q5 (barycentrics).*
