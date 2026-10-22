# Shader and pipeline parity register — Metal 4 vs Vulkan/OpenGL

Owner/Scope: RSX Metal 4 backend (`rpcs3/Emu/RSX/MTL`) | Status: audit + first fix | Date: 2026-10-19
Branch: `metal4` (uncommitted work in progress)

Method: static inspection of the Metal sources against the Vulkan and OpenGL paths on the same
branch, plus the Metal 4 semantics exercised by KosmicKrisp (`.kk-mesa/src/kosmickrisp/`).
No emulator, game, or GUI was launched. The build and the `MTL*` unit tests are the runtime
evidence. `file:line` is advisory; symbol names are the stable reference.

Legend: **Fixed** = changed and tested in this pass. **Open** = real gap, but a design decision or a
missing runtime capability blocks a safe change. **Deferred** = real gap with a known fix that is
too risky or too broad for this pass. **No gap** = checked and found at parity.

---

## 1. Ranked register

Rows are ordered by expected impact on real titles, not by effort.

| ID | Area | VK/GL reference (file:line) | Metal state (file:line) | Impact | Fix | Risk/effort | Status |
|---|---|---|---|---|---|---|---|
| S01 | Sampler LOD bias | `VKDraw.cpp:481-511`, `GLDraw.cpp` sampler path | `MTLSamplerState.cpp:142-166`, `:210-236` | Every mipmapped texture with a non-zero guest bias selects the wrong mip. Bias is common in terrain, water, and UI. | Latch `texture.bias()`, round nearest-mip bias to 0.5, clamp to Metal's range, `setLodBias`, include in equality/hash. | Low | **Fixed** |
| S02 | MSAA sample mask | `VKGSRender.cpp:400`, `VKPipelineCompiler.cpp:151`, `GLDraw.cpp:244` | `MTLDepthStencil.h:69` says the mask travels in MSL; no emission exists (`MTLGSRender.cpp`, `MTLFragmentProgram.cpp`) | Guest `SET_MULTISAMPLE_MASK` is a no-op. Titles that exclude samples see extra coverage (edge sparkle, wrong alpha-to-coverage). | Lower the static mask into the fragment shader as KosmicKrisp does (`.kk-mesa/src/kosmickrisp/vulkan/kk_shader.c:552-567`): compare `gl_SampleMaskIn` with the sample bit and discard, then reflect the same mask in the pipeline key. | Med-high; needs a shader-constant or MSL input decision and pipeline-key plumbing. | Open — design decision |
| S03 | Fragment fp16 | `VKFragmentProgram.cpp:632-638` | `MTLFragmentProgram.cpp:546-557` | Shaders always use fp32 registers. Cost and precision differ from the guest's fp16 path; titles with range-sensitive math may render differently. | Pass device capabilities into `MTLFragmentDecompilerThread` instead of hardcoding false, then add the capability word to the shader cache key. | Med; the shader cache alias (`MTLGSRender.cpp:656`) and archive slices must be invalidated when capability bits change. | Deferred |
| S04 | Custom border color | `VKDraw.cpp:295-299`, `:420-441`; `GL` uses the exact color | `MTLSamplerState.cpp:108-123` maps the guest color to white/black/transparent | Non-black/white borders clamp to the nearest of three values. Visible in skyboxes, shadow maps, and wrapped effects that rely on a colored border. | Apple hardware exposes the exact border through an AGX texture query; KosmicKrisp emulates it in the shader (`.kk-mesa/src/kosmickrisp/vulkan/kk_nir_lower_custom_border.c`). SPIRV-Cross has no portable path for that query, so this needs a Metal-specific shader lowering. | High | Deferred |
| S05 | Primitive restart corner | `BufferUtils.cpp:206-230`, `:535-564`; `VKDraw.cpp` index handling | `MTLGSRender.cpp:215-231` leaves disabled-restart indices untouched; Metal restarts strip indices equal to the max value | A guest **strip** draw with restart disabled and a max index in the buffer restarts where it should not. Rare but silent geometry corruption. | Rewrite/expand the affected index buffer for that draw, as KosmicKrisp does (`.kk-mesa/src/kosmickrisp/vulkan/kk_cmd_draw.c:1809-1818`). | Med; rare on real titles, per-draw CPU cost. | Deferred |
| S06 | Pipeline build is synchronous | `VKDraw.cpp:1319-1322`, `GLDraw.cpp:952-955` yield while the pipeline compiles | `MTLProgramPipeline.cpp:276-335` ignores `compile_async` and calls `newRenderPipelineState` inline | First use of every PSO blocks the RSX thread; frame hitches on new effects. Correctness is unaffected. | Use the MTL4 completion form and return the shared cache's "not ready" result; yield in the draw path until the callback delivers the pipeline. | High; changes the `build_pipeline` contract and every caller. | Deferred |
| S07 | Depth bounds test | `VKDraw.cpp:241-260`, `GLDraw.cpp:103-116` | Captured in `MTLEncoderState.cpp:35-37`; `:65-75` deliberately does not call the encoder setter | Guest depth bounds are ignored. Any title relying on them for shadow/particle clipping draws where it should not. | Fragment-shader discard from the captured bounds (the design fallback recorded in `LLD-depth-rop.md`). | Med; shader epilogue and constants, then remove the capture-only fallback. | Deferred — documented bring-up fallback |
| S08 | Last-provoking vertex | `VKGSRender.cpp:751-754` (`VK_EXT_provoking_vertex`), `GLGSRender.cpp:54` | `MTLDevice.cpp:312` sets `supports_last_provoking_vertex = false` | Flat-shaded titles fall back to smooth interpolation. Metal has no public provoking-vertex mode, so this is a fixed backend limitation. | Per-draw index rotation or geometry-shader-style expansion; not viable as a general path. | High | Deferred — backend limitation |
| S09 | Hardware conditional render | `VKGSRender.cpp:795`, `:3044-3080` | `MTLDevice.cpp:313` sets `supports_hw_conditional_render = false` | CPU-side skip instead of GPU predicate; CPU cost and a possible sync difference on occlusion-heavy scenes. | Use an indirect draw with a predicate buffer, or keep the CPU path. | Med-high | Deferred |
| S10 | Fast math and float invariance | `GLSLCommon`/shared GLSL; VK compiles the same SPIR-V with driver fast-math | `MTLShader.cpp:104-116` never sets `use_fast_math_pragmas` or `invariant_float_math` | No perf gain from guest fast-math; invariant vertex position is not forced. | `use_fast_math_pragmas` requires MSL 3.2 and the backend is pinned to MSL 2.3. `invariant_float_math` would require a policy decision. | Low correctness risk; MSL version raise is unverified. | Deferred |
| S11 | Multi-draw | `VKGSRender`/`GLGSRender` support multi-draw | `MTLDevice.cpp:287-289` sets `supports_multidraw = false` | Draw clauses loop on the CPU. Performance only. | Host-built indirect batcher (already noted in the source). | High | Deferred |
| S12 | Shader cache, threading, key completeness | `rsx_cache.h:280-281` already includes Metal; VK cache layout | Archive: `MTLDevice.cpp:147-265`; alias: `MTLGSRender.cpp:656`; key: `MTLPipeline.h:26-59` | No gap found. Threaded preload, per-device archive path, MSL cache alias, and the PSO key all match the landed design. | None this pass. Bump the `"v1"` alias when MSL options or capability bits change. | — | No gap |

---

## 2. Fixed in this pass — S01 sampler LOD bias

### 2.1 What was wrong

`capture_sampler` latched the LOD range but explicitly recorded the guest bias as a limitation
("Metal has no sampler LOD bias", `MTLSamplerState.cpp:141-143`). That statement is false:
`MTL::SamplerDescriptor::setLodBias` exists in the vendored metal-cpp
(`3rdparty/metal-cpp/Metal/MTLSampler.hpp:111`, `:257`) and Apple documents the range as
`[-16.0, 15.999]` on the macOS 26 API. The product floor is macOS 27
(`MTLAvailability.h:18-22`), so availability is not a concern.

Vulkan applies the bias only when the min filter samples mips and the resident chain has more than
one level, clamps the LOD range to that chain, and rounds a **nearest** mip filter to 0.5 steps
because some titles encode a fractional bias the hardware rounds differently
(`VKDraw.cpp:481-511`). Metal was doing everything except the bias.

### 2.2 Change

- `sampler_state` gained `f32 lod_bias` (`MTLSamplerState.h:31`).
- `capture_sampler` now latches `texture.bias()` under the same condition as the LOD range
  (`MTLSamplerState.cpp:151-161`).
- `get_sampler_lod_bias` is a pure helper (`MTLSamplerState.cpp:108-117`) that applies the Vulkan
  nearest-mip quantization and clamps to the Metal range.
- `create_sampler_state` calls `setLodBias` (`MTLSamplerState.cpp:222`).
- Field equality and the cache hash include the bias (`MTLSamplerState.cpp:248`, `:272`), so a
  changed bias cannot alias a cached sampler.
- `MTLSamplerState.h:66-69` documents the helper; the stale "no LOD bias" comment is gone.

### 2.3 Verification

- New test `MTLSamplerMapping.LodBiasRoundsNearestMipsAndClampsToMetal`
  (`rpcs3/tests/test_rsx_metal4_state.cpp:1268-1300`) covers nearest-mip rounding in both signs, the
  linear-mip pass-through, and both clamp ends.
- `LatchEqualityTracksEveryField` and `LatchHashTracksEveryField` gained `lod_bias` probes
  (`test_rsx_metal4_state.cpp:1370-1375`, `:1417`).
- Build and test results are recorded in §6.

### 2.4 Remaining risk

The guest bias is a float register; the clamp is a Metal API requirement, not a guest semantic. A
value outside `[-16, 15.999]` therefore behaves differently from a backend with a wider sampler
range. This is preferable to a validation failure. The 0.5 rounding is deliberately Vulkan's
workaround, so Vulkan and Metal agree even where the guest value is fractional.

---

## 3. Open — S02 MSAA sample mask

The header comment at `MTLDepthStencil.h:65-70` states that the mask is applied through an encoder
setter and travels in the MSL as `[[sample_mask]]`. Neither half exists: there is no encoder call and
no MSL emission. Vulkan binds the mask through `VkPipelineMultisampleStateCreateInfo::pSampleMask`
(`VKPipelineCompiler.cpp:151`) and OpenGL calls `glSampleMaski` (`GLDraw.cpp:244`).

Metal 4 has no public dynamic sample-mask setter. KosmicKrisp solves it entirely in the shader: when
the static mask is not all-ones it loads the sample id, tests the corresponding bit of
`gl_SampleMaskIn`, and discards the fragment (`.kk-mesa/src/kosmickrisp/vulkan/kk_shader.c:552-567`).
Two designs are possible here:

1. **Shader epilogue.** Emit the same test in the fragment MSL and pass the 16-bit mask in the
   existing fragment constant block. This keeps the pipeline object unchanged and matches the
   guest's static mask model.
2. **Pipeline key.** Put the mask into `pipeline_properties` and compile a variant per mask. This
   is simpler to reason about but multiplies PSOs and does not match how the rest of the dynamic
   state is handled.

This is a design decision, so the register leaves it Open rather than guessing. The implementation
cost is moderate either way; the hard part is choosing where the mask lives.

---

## 4. Deferred items

### S03 fp16

`MTLFragmentDecompilerThread::Task` hardcodes all four device capability bits to false
(`MTLFragmentProgram.cpp:546-557`). The Vulkan decompiler takes them from the physical device
(`VKFragmentProgram.cpp:632-638`), which selects `half4` registers and native shadow compares.
Apple silicon supports fp16, so the current choice is conservative rather than forced. Enabling it
requires the capability word to enter the shader-cache key (the shader is cached by program id and
the archive is device-sliced, but not capability-sliced), plus validation of the decompiler's
half path. That is a larger change than this pass allows.

### S04 custom border color

`get_closest_border_color` collapses the guest color to one of Metal's three descriptor values
(`MTLSamplerState.cpp:108-123`). Vulkan passes the exact decoded color
(`VKDraw.cpp:295-299`, `:420-441`); OpenGL likewise. Apple hardware can return the exact border
through an AGX-specific texture query, and KosmicKrisp uses it to emulate the color in the shader
(`.kk-mesa/src/kosmickrisp/vulkan/kk_nir_lower_custom_border.c:17-80`). That path is not reachable
through SPIRV-Cross, so a Metal-specific lowering pass or a hand-written fetch helper would be
needed. It is visible but uncommon, hence Deferred.

### S05 primitive restart

Metal restarts indexed strip topologies on the maximum index and has no toggle. RSX restart is
enabled per draw and the index is a register. `write_index_array_data_to_buffer` already maps an
enabled guest restart index to the max (`BufferUtils.cpp:535-564`), and list topologies are expanded,
so the only unhandled case is:

- restart **disabled** while a max index is present in a strip buffer (Metal restarts anyway).

KosmicKrisp treats the equivalent corner as "requires unroll" and rewrites the draw through a
compute pass (`.kk-mesa/src/kosmickrisp/vulkan/kk_cmd_draw.c:1809-1818`). Correct but costly; rare
on real titles, so Deferred.

### S06 synchronous pipeline build

`build_pipeline` takes `compile_async`, discards it, and blocks in
`newRenderPipelineState` (`MTLProgramPipeline.cpp:276-335`). Vulkan and OpenGL yield the RSX thread
while the pipeline is not ready (`VKDraw.cpp:1319-1322`, `GLDraw.cpp:952-955`). Metal is
structurally synchronous, so the "not ready" branch does not exist. Correctness is unaffected, but
first-use hitches remain. Fixing it means changing the shared-cache callback contract, which is a
cross-backend change and therefore out of scope for a Metal-only edit.

### S07 depth bounds

The bounds are captured (`MTLEncoderState.cpp:35-37`) but intentionally not sent to the encoder
because the call traps in the driver (`MTLEncoderState.cpp:65-75`, crash report 2026-10-18).
`LLD-depth-rop.md` records the fragment-discard fallback as the next step. The VK path uses the
native extension when available and an emulation otherwise (`VKPipelineCompiler.cpp:114-125`,
`VKDraw.cpp:241-260`); GL uses `GL_EXT_depth_bounds_test` (`GLDraw.cpp:103-116`). This is a known
bring-up gap, not a silent omission.

### S08 provoking vertex, S09 conditional render, S11 multi-draw

These are backend-capability choices in `MTLDevice.cpp:287-313`:

- Metal has no public last-provoking-vertex mode, so flat shading falls back to smooth;
- conditional render is CPU-side, which changes cost but not results;
- multi-draw is a loop, which changes cost but not results.

All three are performance or fixed-function limitations rather than shader/pipeline correctness, and
each needs a separate design. They stay Deferred.

### S10 fast math and invariance

`MTLShader.cpp:104-116` sets MSL 2.3, `texture_buffer_native`, and framebuffer-fetch subpasses. It
does not set `use_fast_math_pragmas`; SPIRV-Cross documents that option as requiring MSL 3.2
(`3rdparty/SPIRV-Cross/spirv_msl.hpp:552-555`, enforced at `spirv_msl.cpp:5828`). Raising the MSL
version is not verified against the deployment target and the SPIRV-Cross output, so the safe move
is to leave it alone. `invariant_float_math` is separate; the shared vertex prologue already
declares `invariant gl_Position` (`RSXVertexPrologue.glsl:4`), and SPIRV-Cross honors the relevant
NoContraction decorations without the global flag. No change made.

---

## 5. Checked at parity

These were audited and need no row of their own:

- **MSL translation shape.** `spv_to_msl` goes SPIR-V → `CompilerMSL` with the shared GLSL front
  end, catches cross-compiler exceptions per pipeline, and reports an empty module as failure
  (`MTLShader.cpp:95-203`). Matching Vulkan's SPIR-V output is the right seam.
- **Vertex input.** The backend uses software vertex pulling from texture buffers
  (`MTLShader.cpp:108-110`), so there is no vertex descriptor or attribute-format mismatch to key.
- **Varyings and fragment outputs.** Locations are derived from the SPIR-V decorations; the color
  output mask is bounded to 8 (`MTLShader.cpp:165-180`).
- **Constant binding.** Both RSX push-constant blocks are remapped to the fixed draw-params and
  fragment-blend slots (`MTLShader.cpp:150-163`, `MTLArgumentBinds.h`).
- **Texture and sampler binding.** The ABI is a fixed slot map; sampler indices are remapped
  because MSL caps `[[sampler(n)]]` at 15, the depth mirror and depth input are separate
  samplerless textures (`MTLArgumentBinds.h:66-116`), and the binder's skip-write contract matches
  the argument-table lifetime.
- **PSO key and descriptor.** The key holds exactly the descriptor state and excludes encoder and
  render-pass state (`MTLPipeline.h:13-59`); the applier sets sample count, alpha-to-coverage,
  alpha-to-one, rasterization, topology class, color formats, write masks, and blend
  (`MTLPipeline.cpp:67-101`). Depth/stencil formats belong to the render pass in Metal 4 and are
  not a missing descriptor field.
- **Shader cache and threading.** `rsx_cache.h:280-281` already treats `video_renderer::metal` as
  threaded; the archive path includes the GPU registry id and macOS major
  (`MTLDevice.cpp:147-153`); the archive is opened, serialized, and flushed at exit
  (`MTLDevice.cpp:205-265`); the MSL cache alias is `"metal"/"v1"` (`MTLGSRender.cpp:656`). The
  HLD-07 statement that cache and threading are absent is stale.
- **Nop-draw and end contract.** `MTLGSRender::end` calls `execute_nop_draw` and `rsx::thread::end`
  on the skip/bind/program failure paths (`MTLGSRender.cpp:1538-1560`, `:1827-1871`), matching
  `VKDraw.cpp:1285-1286` and `GLDraw.cpp:934-935`. The missing piece is only the async yield in
  S06, which is a build-time property, not a failure-path bug.

---

## 6. Build and test evidence

Commands (run from the repository root):

```
ninja -C build -j4
./build/bin/rpcs3_test --gtest_filter='MTL*'
```

Results:

```
ninja -C build -j4
[8/8] Linking CXX executable bin/rpcs3_test
```

Only the usual macOS deployment-target linker warnings were emitted. The build completed with
exit code 0.

```
./build/bin/rpcs3_test --gtest_filter='MTL*'
[==========] 131 tests from 38 test suites ran. (9 ms total)
[  PASSED  ] 131 tests.
```

The pre-existing suite was 130 tests; the new `LodBiasRoundsNearestMipsAndClampsToMetal` case is
the 131st. `MTLSamplerMapping.*` ran 8/8.

Docs checks:

```
python3 docs/research/metal4/inventory/lint_docs.py --strict
design docs: 27  clusters: 14  gaps: 0  errors: 0       (exit 0)

python3 docs/research/metal4/inventory/validate.py --strict
spine=282 curated=282 (100.0%) shards=14                 (exit 0)
```

---

## 7. Sources

Repository evidence is cited inline. External and reference sources:

- Apple, `MTLSamplerDescriptor.lodBias` (availability macOS 26, range `[-16.0, 15.999]`):
  https://developer.apple.com/documentation/metal/mtlsamplerdescriptor/lodbias
- Apple, Metal feature set tables (MSL and sampler limits):
  https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf
- KosmicKrisp, static sample-mask lowering:
  `.kk-mesa/src/kosmickrisp/vulkan/kk_shader.c:552-567`
- KosmicKrisp, unusual restart-index unroll:
  `.kk-mesa/src/kosmickrisp/vulkan/kk_cmd_draw.c:1809-1818`
- KosmicKrisp, custom border-color emulation:
  `.kk-mesa/src/kosmickrisp/vulkan/kk_nir_lower_custom_border.c:17-80`
- SPIRV-Cross, fast-math option and MSL 3.2 requirement:
  `3rdparty/SPIRV-Cross/spirv_msl.hpp:552-555`, `spirv_msl.cpp:5828`

<!-- doc-end -->
