# Metal 4 vs Vulkan vs OpenGL — verified parity audit

Scope: what the Metal 4 backend does differently from the Vulkan and OpenGL
backends, where it silently ignores or approximates a setting, and which of those
differences can change a guest-visible result. The RSX behaviour is the reference;
a difference is only interesting when it changes what the guest sees or when it
turns a documented option into a no-op.

Paths are repository-relative. `MTL` = `rpcs3/Emu/RSX/MTL`, `VK` =
`rpcs3/Emu/RSX/VK`, `GL` = `rpcs3/Emu/RSX/GL`, `Common` = `rpcs3/Emu/RSX/Common`,
`Core` = `rpcs3/Emu/RSX/Core`.

Every claim below was produced by reading the whole call path, not by searching
for a key. Where a consequence is inferred from a verified path but was not
observed at runtime it is tagged `[I]`.

**Confidence tags**

- `[V]` verified: the path was read end-to-end in this tree and the cited lines
  are the ones that decide the behaviour.
- `[I]` inference: the code path is verified, the user-visible consequence is
  not observed.
- `[U]` unverified: the mechanism is not traced far enough to claim anything.

This document supersedes the following stale claims in
`DEVIATIONS-FEATURES.md`; see §16 for the list and the corrected evidence.

---

## 1. Method

1. Read `node_video` in order (`rpcs3/Emu/system_config.h:116-189`) and, for each
   key, follow every read site in `MTL`, `VK`, `GL` and the shared layer.
2. Read the capability surface: `rsx::backend_configuration`
   (`Core/RSXRenderBackend.h:96-118`), Metal's `fill_backend_config`
   (`MTL/MTLDevice.cpp:357-394`), Vulkan's block (`VK/VKGSRender.cpp:727-800`) and
   OpenGL's two blocks (`GL/GLGSRender.cpp:38-57`, `GL/GLGSRender.cpp:180-235`).
3. For every flag, find the shared consumer and decide whether Metal's value
   changes the outcome or only the path.
4. Read the three draw paths against each other: topology and index handling,
   vertex fetch, shader translation, ROP, texture/sampler decode, surface and
   depth formats, ZCULL, blit, present.
5. Classify each row: *identical*, *different but equivalent*, *degraded*
   (correctness), *approximated* (quality), *inert* (setting has no effect), or
   *absent feature*.

A "degraded" row is a defect candidate. Under the project rule, only rows that
are both `[V]` and demonstrably reachable are fixed in this pass; the rest are
recorded here and in `LOG-FINDINGS.md`.

---

## 2. Confirmed divergences, ranked

| id | area | Metal | Vulkan / OpenGL | class | impact | conf |
|----|------|-------|-----------------|-------|--------|------|
| D1 | Flat shading (`RSX_SHADER_CONTROL_FLAT_SHADING`) | flag never set, colour varyings interpolate smoothly (`MTL/MTLDevice.cpp:384`, `Core/RSXGraphEngine.cpp:1172-1177`, `Program/GLSLCommon.cpp:741-752`) | VK uses `VK_EXT_provoking_vertex` + `provokingVertexLast`; GL is `GL_LAST_VERTEX_CONVENTION` by default (`VK/VKGSRender.cpp:732-737`, `GL/GLGSRender.cpp:46-47`) | degraded | flat-shaded guest colours are wrong everywhere they are used | `[V]` |
| D2 | Framebufferless draws | advertised `true` (`MTL/MTLDevice.cpp:373`) but `framebuffer::valid()` needs an attachment (`MTL/MTLFramebuffer.h:51`), `open_pass` returns null (`MTL/MTLFramebuffer.cpp:166-171`), hook returns false (`MTL/MTLGSRender.cpp:1876`), core clears validity (`Core/RSXGraphEngine.cpp:839-843`), every draw is skipped (`Core/RSXCoreRenderer.cpp:745-752`) | VK builds a no-attachment render pass (`VK/VKGSRender.cpp:2805-2812`), GL sets default extents/samples (`GL/GLRenderTargets.cpp:254-263`); both rasterize and increment ZPASS | degraded | zero-attachment passes are dropped, occlusion counters never advance | `[V]` |
| D3 | `texture2DMS` depth mirror with a 1-sample host surface | `supports_hw_msaa = true` is unconditional (`MTL/MTLDevice.cpp:362`), so `ROP_MULTISAMPLED` is set (`Core/RSXGraphEngine.cpp:933-939`, `:964-970`) while the mirror honours the video MSAA level (`MTL/MTLSurface.cpp:63-65`, `MTL/MTLSurface.cpp:142-151`) and the shader declares `texture2DMS` (`MTL/MTLFragmentProgram.cpp:217-221`) | VK/GL clear `supports_hw_msaa` when `MSAA = none` (`VK/VKGSRender.cpp:746-752`, `GL/GLGSRender.cpp:50-56`), so no MS declaration is emitted | degraded, fixed | with `MSAA = none` and `Emulate Special Depth Comparison`, the depth fetch was declared multisample against a single-sample texture; §18 now derives the flag from `antialiasing_level` | `[V]` path, `[I]` runtime |
| D4 | `output_scaling = fsr` | always bilinear or nearest (`MTL/MTLPresent.cpp:101-102`); no FSR, no RCAS | VK/GL build an `fsr_upscale_pass` and consume `rcas_sharpening_intensity` (`VK/VKPresent.cpp:836-838`, `VK/upscalers/fsr1/fsr_pass.cpp:182`, `GL/GLPresent.cpp:428-429`) | absent feature | the FSR option silently degrades to bilinear; the CAS slider does nothing | `[V]` |
| D5 | Scaled texture transfer | logs "scaled texture copy is not supported" and skips the region (`MTL/MTLTextureCache.cpp:960-964`) | `vkCmdBlitImage` / `glBlitFramebuffer` scale | degraded | a scaled temp-subresource keeps undefined contents when a region is skipped | `[V]` path, `[I]` reachability |
| D6 | Typeless copy with different texel sizes | logs and skips (`MTL/MTLTextureCache.cpp:981-986`) | format conversion through the blit path | degraded | same shape as D5, different trigger | `[V]` path, `[I]` reachability |
| D7 | GPU blit engine (`nv3089`) | `scaled_image_from_memory` always returns false (`MTL/MTLGSRender.cpp:7855-7912`), so the shared CPU `SWScale`/`memcpy` path runs | VK/GL blit through the texture cache (`NV47/HW/nv3089.cpp:614-620`, `VK/VKGSRender.cpp:2852-2869`, `GL/GLGSRender.cpp:1360-1370`) | absent feature | correct but slower; `force_cpu_blit_processing` and `disable_blit_engine_upscaling` become no-ops | `[V]` |
| D8 | A2C when video MSAA is off | `supports_hw_a2c = true` unconditionally (`MTL/MTLDevice.cpp:363`), so the shared path asks for hardware A2C on a 1-sample target (`Core/RSXGraphEngine.cpp:1257-1265`) | VK/GL clear the flag when `MSAA = none`, so the stochastic shader emulation runs (`Program/GLSLSnippets/RSXProg/RSXFragmentPrologue.glsl:106-113`, `RSXROPEpilogue.glsl:51-56`) | degraded, fixed | A2C had no visible effect on Metal with MSAA off while VK/GL still dithered coverage; §18 now clears the flag when MSAA is off | `[V]` path, `[I]` visibility |
| D9 | Border colour | approximated to Metal's three values (`MTL/MTLSamplerState.cpp:131-146`) | VK/GL pass the decoded colour exactly (`VK/VKDraw.cpp:297-301`, `GL/glutils/sampler.cpp:81-113`) | approximated | border-sampled fetch returns the nearest of white/black/transparent black | `[V]` mapping, `[I]` visual |
| D10 | Anisotropy and LOD bias | rounded up to 1/2/4/8/16 and bias quantised to 0.5 (`MTL/MTLSamplerState.cpp:114-128`) | VK uses the exact float value (`VK/VKFormats.cpp:104-130`) | approximated | slight filter difference, no state error | `[V]` |
| D11 | Mirror-once wrap | collapsed to `MirrorClampToEdge`, border/edge/clamp variants all collapse (`MTL/MTLSamplerState.cpp:30-38`) | VK collapses identically (`VK/VKFormats.cpp:96-98`); GL has `GL_MIRROR_CLAMP_TO_BORDER_EXT` (`GL/glutils/sampler.cpp:21-23`) | approximated | Metal matches VK, is less exact than GL | `[V]` |
| D12 | ZCULL result evaluation | always the blocking `read_barrier`; `on_sync_hint` is the base no-op (`Core/RSXRenderBackend.h:208`) | VK overrides it and flushes early on a conditional-render hint (`VK/VKGSRender.cpp:1820-1850`) | different but equivalent | correctness preserved, a read stalls the FIFO where VK can avoid it | `[V]` |
| D13 | Multi-draw capability | `supports_multidraw = false` (`MTL/MTLDevice.cpp:361`) | both set `true` (`VK/VKGSRender.cpp:727`, `GL/GLGSRender.cpp:43`) | inert | the field has no reader anywhere in this tree | `[V]` |
| D14 | Async-memory / async-compute claims | `supports_asynchronous_compute = true` (`MTL/MTLDevice.cpp:374`) | consumed by VK only (`VK/VKTextureCache.cpp:1242`, `VK/VKDraw.cpp:640`) | inert | dead claim on Metal; no behaviour depends on it | `[V]` |
| D15 | Debug/developer settings | `debug_output` and `renderdoc_compatiblity` have no Metal reader | VK loads validation and a debug callback (`VK/vkutils/instance.cpp:140`, `:268-269`); GL uses both | inert | the checkbox does nothing on Metal | `[V]` |
| D16 | Shader compilation hint | no Metal reader of `show_shader_compilation_hint` | VK/GL show a hint while a pipeline builds | absent feature | a stall looks like a hang | `[V]` |

D13, D14 and D15 are not defects in behaviour; D1, D2 and D3 change guest-visible
results. D4 through D8 are the quality/coverage gaps. D3 and D8 are fixed in this
pass (§18); D1, D2 and D4-D8 remain open.

---

## 3. Capability flags, field by field

`rsx::backend_configuration` is the only capability contract
(`Core/RSXRenderBackend.h:96-118`). Vulkan and OpenGL leave most fields at the
zero-initialised default (`Core/RSXRenderBackend.h:244`) and only raise what they
support; Metal sets every field explicitly (`MTL/MTLDevice.cpp:361-393`).

| flag | Metal | Vulkan | OpenGL | shared consumer | verdict | conf |
|------|-------|--------|--------|-----------------|---------|------|
| `supports_multidraw` | false | true | true | none in this tree | inert, no reader | `[V]` |
| `supports_hw_a2c` | MSAA != none | MSAA != none | MSAA != none | `Core/RSXGraphEngine.cpp:1257-1265` | identical (D8 fixed) | `[V]` |
| `supports_hw_a2c_1spp` | false | true when MSAA != none | false | same | Metal matches GL, not VK | `[V]` |
| `supports_hw_renormalization` | false | NVIDIA only | NVIDIA only | `Core/RSXGraphEngine.cpp:1438-1459` | correct for Apple silicon | `[V]` |
| `supports_hw_msaa` | MSAA != none | MSAA != none, sample-count gated | MSAA != none | `Core/RSXGraphEngine.cpp:933-939`, `:964-970` | identical (D3 fixed) | `[V]` |
| `supports_hw_a2one` | true | device query | true | MTL drives state from registers (`MTL/MTLGSRender.cpp:1322-1323`, `MTL/MTLPipeline.cpp:79`); VK gates the same state on the flag (`VK/VKGSRender.cpp:389`); GL gates it (`GL/GLDraw.cpp:241-244`) | equivalent outcome | `[V]` |
| `supports_hw_conditional_render` | false | true except MVK | false (default) | `Core/RSXCoreRenderer.cpp:745-752` skips the draw on the CPU | correct, blocking | `[V]` |
| `supports_hw_instanced_rendering` | true | true | true, dropped without SSBO | `Core/RSXGraphEngine.cpp` instancing path | identical | `[V]` |
| `supports_passthrough_dma` | false | host-memory import probe | false (default) | only `VK/VKDMA.cpp:306` | Metal matches GL | `[V]` |
| `supports_asynchronous_compute` | true | setting + queue check | false | VK only | dead claim (D14) | `[V]` |
| `supports_host_gpu_labels` | follows the setting | follows the setting, with driver checks | follows the setting | `NV47/HW/nv47_sync.hpp:37`; Metal implements `release_GCM_label` (`MTL/MTLGSRender.cpp:7730-7784`) | honoured | `[V]` |
| `supports_normalized_barycentrics` | true | false on NVIDIA, or low precision | false on NVIDIA, or low precision | `Program/GLSLCommon.cpp` barycentric path | correct for Apple | `[V]` |
| `supports_last_provoking_vertex` | false | device query (optional) | true (default state) | `Core/RSXGraphEngine.cpp:1172-1177` | D1 | `[V]` |
| `supports_programmable_blending` | true | true | false | `Core/RSXGraphEngine.cpp:921-931`, `:933-939` | Metal matches VK, ahead of GL | `[V]` |
| `supports_extended_depth_range` | false | device query | `NV_depth_buffer_float` | `Core/RSXGraphEngine.cpp:975-983` | emulated, complete | `[V]` |
| `supports_framebufferless_rendering` | true | sample-count gated | extension gated | `Core/RSXGraphEngine.cpp:501-512` | claim is false in practice (D2) | `[V]` |
| `supports_native_logic_op` | false | true (default) | true (default) | `Core/RSXGraphEngine.cpp:1249-1255` | emulated in shader, complete | `[V]` |

### 3.1 The provoking-vertex gap (D1)

Metal has no per-pipeline provoking-vertex modifier: `flat` interpolation always
takes the first vertex of the primitive. RSX flattens the colour of the **last**
vertex, so simply setting the shared `flat` qualifier would trade one wrong colour
for another; this is why the backend reports the capability false rather than
emitting `flat` unconditionally.

The path is unambiguous:

- `Core/RSXGraphEngine.cpp:1172-1177` sets `RSX_SHADER_CONTROL_FLAT_SHADING`
  only when the guest shade mode is flat **and** the backend claims the flag.
- Metal claims false (`MTL/MTLDevice.cpp:384`), so the bit is never set.
- All three backends emit the varying through the same shared block
  (`Program/GLSLCommon.cpp:741-752`, called from
  `MTL/MTLFragmentProgram.cpp:302`, `VK/VKFragmentProgram.cpp:148`,
  `GL/GLFragmentProgram.cpp:79`), which emits `flat` only for the diff/spec
  colour registers when that bit is set.

So the two backends that have the capability render the guest's flat colour and
Metal smooth-interpolates it. KosmicKrisp demonstrates the workaround that is
available to a Metal-backed Vulkan driver: it advertises
`VK_EXT_provoking_vertex` with `provokingVertexLast`
(`.kk-mesa/src/kosmickrisp/vulkan/kk_physical_device.c:197`, `:497-498`,
`:860-862`) and unrolls/rotates the geometry when the pipeline asks for the last
vertex (`.kk-mesa/src/kosmickrisp/vulkan/kk_cmd_draw.c:1822-1837`,
`:2065-2068`). Metal cannot turn the mode on, but the backend already has
fan/quad/line-loop expansion machinery (`MTL/MTLGSRender.cpp:826-828`,
`MTL/MTLGSRender.cpp:935-976`) that could rotate a flat primitive the same way.

Not fixed in this pass: the correct rotation is per-primitive and interacts with
indexed strips and primitive restart, so it needs its own design. Recorded as a
correctness divergence.

### 3.2 The framebufferless gap (D2)

The core trusts the flag: when the layout has no attachments and no depth range,
`Core/RSXGraphEngine.cpp:501-512` proceeds instead of warning, marks the config
valid at `Core/RSXGraphEngine.cpp:532`, and calls the backend hook. Metal's hook
binds nothing and returns `attachments.valid()`
(`MTL/MTLGSRender.cpp:1846-1876`), which is
`m_color_count != 0 || m_depth != nullptr` (`MTL/MTLFramebuffer.h:51`). The core
then clears `rtt_config_valid` (`Core/RSXGraphEngine.cpp:839-843`) and
`should_skip_draw()` returns true for every draw
(`Core/RSXCoreRenderer.cpp:745-752`).

Setting the flag false would not fix it: the same regression happens one level up
at `Core/RSXGraphEngine.cpp:504-509`, which warns and returns before the hook. After
a failed bind `is_framebuffer_bound()` is false (`MTL/MTLGSRender.cpp:1879-1882`), so
the fast path at `Core/RSXGraphEngine.cpp:808-813` never engages and the warning
would print once per draw command. Flipping the flag therefore trades a silent drop
for log spam without saving a draw; it was left alone (§18). VK and GL both build a
real zero-attachment pass (`VK/VKGSRender.cpp:2805-2812`,
`GL/GLRenderTargets.cpp:254-263`), so their ZPASS counters advance and Metal's do
not.

Metal could rasterize without attachments only by binding a dummy 1x1 attachment
wide enough for the scissor, or by counting occlusion in a compute pass; neither
is implemented.

---

## 4. Video settings

Every key of `node_video` (`rpcs3/Emu/system_config.h:116-189`), with the deciding
read site. "Shared" means the read is in code all three backends run, so the
setting behaves the same by construction.

| key | classification | evidence | conf |
|-----|----------------|----------|------|
| `resolution` | honoured, shared | window/present size in the shared display path | `[V]` |
| `aspect_ratio` | honoured, shared | converted in `MTL/MTLGSRender.cpp:7338-7340` | `[V]` |
| `frame_limit` | honoured, shared | `Core/RSXDisplayEngine.cpp:128-139` | `[V]` |
| `second_frame_limit` | honoured, shared | `Core/RSXDisplayEngine.cpp:145` | `[V]` |
| `antialiasing_level` | honoured (host sample count and the MS/A2C flags) | `MTL/MTLSurface.cpp:63-65`, `MTL/MTLDevice.cpp:361-372` | `[V]` |
| `shadermode` | honoured, interpreter included | `MTL/MTLGSRender.cpp:4257-4303` | `[V]` |
| `shader_precision` | honoured | `MTL/MTLFragmentProgram.cpp:676`, `:368` | `[V]` |
| `vsync` | approximated | `MTL/MTLPresenter.cpp:232-251` | `[V]` |
| `write_color_buffers` | honoured through shared locking + MTL read-back | `Common/texture_cache.h:1454`; `MTL/MTLTextureCache.cpp:660-800` | `[V]` |
| `write_depth_buffer` | honoured, same path | `Common/texture_cache.h:1454`; `MTL/MTLTextureCache.cpp:660-760` | `[V]` |
| `read_color_buffers` | honoured, shared | `Common/surface_utils.h:383-398` from `MTL/MTLSurface.cpp:343-350` | `[V]` |
| `read_depth_buffer` | honoured, shared | same function, depth branch | `[V]` |
| `handle_tiled_memory` | honoured, shared | `NV47/FW/context.cpp:16` | `[V]` |
| `log_programs` | honoured | `MTL/MTLProgramPipeline.cpp:117-127` writes GLSL+MSL | `[V]` |
| `debug_output` | inert | no Metal read (D15) | `[V]` |
| `debug_overlay` | honoured | `MTL/MTLGSRender.cpp:7351-7375` | `[V]` |
| `renderdoc_compatiblity` | inert | no Metal read (D15) | `[V]` |
| `use_gpu_texture_scaling` | honoured, shared | `Common/texture_cache.cpp:407` | `[V]` |
| `stretch_to_display_area` | honoured | `MTL/MTLGSRender.cpp:7338-7340` | `[V]` |
| `force_high_precision_z_buffer` | inert, already satisfied | Metal is `Depth32Float` / `Depth32Float_Stencil8` where VK/GL would upgrade (`MTL/MTLFormats.cpp:192-209`, `VK/vkutils/device.cpp:1229-1233`); `z16_uint` stays `Depth16Unorm` in all three | `[V]` |
| `strict_rendering_mode` | honoured, shared | `RSXOffload.cpp:52`, `:118`, `Core/RSXGraphEngine.cpp:1426`, `Common/texture_cache.h:2764`, `NV47/HW/nv308a.cpp:64`, `NV47/HW/nv4097.cpp:829` | `[V]` |
| `fb_aliasing_bias` | honoured, shared | `Core/RSXGraphEngine.cpp:461`, `Common/surface_store.h:1465-1469` | `[V]` |
| `disable_zcull_queries` | honoured, shared | `RSXThread.cpp:1560`, `Core/RSXCoreRenderer.cpp:755` | `[V]` |
| `disable_video_output` | honoured, shared | `Core/RSXDisplayEngine.cpp:201`, `NV47/HW/nv4097.cpp:553` | `[V]` |
| `disable_vertex_cache` | inert by construction | Metal has no vertex cache; `create_context_resources` returns an empty set (`MTL/MTLGSRender.cpp:7481-7486`) | `[V]` |
| `disable_FIFO_reordering` | honoured, shared | `RSXThread.cpp:2439`, `:2458` | `[V]` |
| `emulate_depth_compare` | honoured, with D3 | `Core/RSXGraphEngine.cpp:946-970`; mirror `MTL/MTLGSRender.cpp:4505-4514` | `[V]` |
| `emulate_extended_depth_range` | honoured, full transform | `Core/RSXGraphEngine.cpp:975-983`; `MTL/MTLVertexProgram.cpp:255-257`, `:416-425`; `MTL/MTLFragmentProgram.cpp:246-248`; `MTL/MTLDepthStencil.cpp:107-126` | `[V]` |
| `frame_skip_enabled` | honoured, shared | `RSXThread.cpp:2446`, `:2464` | `[V]` |
| `force_cpu_blit_processing` | always on (no GPU path) | `NV47/HW/nv3089.cpp:614-620`; Metal returns false (D7) | `[V]` |
| `disable_on_disk_shader_cache` | honoured twice | `rsx_cache.h:231` and the pipeline archive `MTL/MTLDevice.cpp:184` | `[V]` |
| `disable_vulkan_mem_allocator` | VK-only key | no Metal reader by design | `[V]` |
| `full_rgb_range_output` | honoured | `MTL/MTLGSRender.cpp:7387` | `[V]` |
| `strict_texture_flushing` | honoured, shared | `Common/texture_cache.h:1197`, `Common/texture_cache_types.cpp:32` | `[V]` |
| `multithreaded_rsx` | honoured, shared | `RSXOffload.cpp:27`, `:102`, `:115`, `:131` | `[V]` |
| `relaxed_zcull_sync` | honoured, shared | `RSXZCULL.cpp:533`, `:675`, `NV47/HW/nv308a.cpp:64` | `[V]` |
| `force_hw_MSAA_resolve` | honoured, shared | `Common/texture_cache_helpers.h:93` | `[V]` |
| `stereo_enabled` | honoured | `Cell/Modules/cellVideoOut.cpp:215`; `MTL/MTLGSRender.cpp:7235-7247` | `[V]` |
| `stereo_render_mode` | honoured | `MTL/MTLOverlays.cpp:792-796` | `[V]` |
| `screen_size` | honoured, shared | `Cell/Modules/cellAvconfExt.cpp:571` | `[V]` |
| `debug_program_analyser` | honoured, shared | `Program/ProgramStateCache.cpp:258` | `[V]` |
| `precise_zpass_count` | honoured | `MTL/MTLZCULL.cpp:158-164` | `[V]` |
| `consecutive_frames_to_draw` | honoured, shared | `RSXThread.cpp:2450` | `[V]` |
| `consecutive_frames_to_skip` | honoured, shared | `RSXThread.cpp:2464` | `[V]` |
| `resolution_scale_percent` | honoured | `RSXThread.cpp:837`; consumed at `MTL/MTLSurface.cpp:56` | `[V]` |
| `anisotropic_level_override` | honoured, then quantised | `RSXTexture.cpp:217`; `MTL/MTLSamplerState.cpp:114-117` | `[V]` |
| `texture_lod_bias` | honoured, then quantised | `RSXTexture.cpp:315`; `MTL/MTLSamplerState.cpp:120-128` | `[V]` |
| `min_scalable_dimension` | honoured, shared | `RSXThread.cpp:838` | `[V]` |
| `shader_compiler_threads_count` | honoured, Metal is threaded | `rsx_cache.h:301-303` treats `video_renderer::metal` as threaded | `[V]` |
| `driver_recovery_timeout` | honoured, shared | `NV47/HW/nv406e.cpp:99` | `[V]` |
| `driver_wakeup_delay` | honoured, shared | `RSXThread.cpp:1882`, `:1954` | `[V]` |
| `vblank_rate` | honoured, shared | `Core/RSXDisplayEngine.cpp:132`, `:342` | `[V]` |
| `vblank_ntsc` | honoured, shared | `Core/RSXDisplayEngine.cpp:343` | `[V]` |
| `decr_memory_layout` | honoured, shared | `Cell/lv2/sys_rsx.cpp:538` | `[V]` |
| `host_label_synchronization` | honoured | `MTL/MTLDevice.cpp:393`; `MTL/MTLGSRender.cpp:7730-7784`; shared gate `NV47/HW/nv47_sync.hpp:37` | `[V]` |
| `disable_msl_fast_math` | honoured | `MTL/MTLShader.cpp:224-228` | `[V]` |
| `disable_async_host_memory_manager` | honoured, shared | `Host/MM.cpp:129` | `[V]` |
| `output_scaling` | nearest/bilinear honoured, `fsr` falls back | `MTL/MTLPresent.cpp:101-102` | `[V]` |
| `record_with_overlays` | honoured | `MTL/MTLGSRender.cpp:7281` | `[V]` |
| `disable_hardware_blending` | honoured, shared | `Core/RSXGraphEngine.cpp:43-47` | `[V]` |
| `disable_hardware_texel_remapping` | inert by design | Metal always emulates the remap in the shader (`Core/RSXGraphEngine.cpp:1462-1470`, `MTL/MTLSamplerState.cpp:220-223`); VK/GL use the key to toggle a hardware SNORM/sRGB view (`VK/VKDraw.cpp:365-395`, `GL/GLDraw.cpp:373-402`) | `[V]` |
| `rcas_sharpening_intensity` | inert | only the VK/GL FSR passes read it (D4) | `[V]` |

### 4.1 The three settings Metal declares ignored

`MTL/MTLDevice.cpp:98-108` reports `disable_hardware_texel_remapping`,
`force_high_precision_z_buffer` and `disable_vertex_cache`. All three are accurate
and all three are harmless:

- the Metal path has no hardware texel-remap state to disable, because the shader
  always re-expands;
- depth is already at the precision the option asks for, for every format where
  VK/GL honour it;
- there is no vertex cache, so "disable" is the permanent state.

The report runs only under `MTL_DIAGNOSTICS` (`MTL/MTLDevice.cpp:117-119`), so a
release build says nothing. That is a visibility issue, not a parity issue.

### 4.2 Settings that are read but cannot matter

`force_cpu_blit_processing` (`NV47/HW/nv3089.cpp:614`) and
`disable_blit_engine_upscaling` (`Common/texture_cache.h:1788`) both gate the GPU
blit path. Metal never enters it (D7), so both are no-ops for a different reason
than the three above: the facility they control does not exist on this backend.

---

## 5. Draw path: topology, indices, restart

Metal's native topologies are points, lines, line strip, triangles, triangle
strip. Fans, quads, polygon and line loop are expanded by the shared writer.

| primitive | Metal | Vulkan | OpenGL | conf |
|-----------|-------|--------|--------|------|
| points, lines, line strip, triangles, triangle strip | native | native | native | `[V]` |
| quad strip | native triangle strip | native triangle strip (`VK/VKVertexBuffers.cpp:27-28`) | native triangle strip (`GL/GLHelpers.cpp:30`) | `[V]` |
| triangle fan | expanded | native on desktop, expanded on Apple (`VK/VKVertexBuffers.cpp:29-33`) | native | `[V]` |
| quads, polygon | expanded | expanded (`VK/VKVertexBuffers.cpp:34-36`) | expanded (`GL/GLHelpers.cpp:181`) | `[V]` |
| line loop | expanded | native strip + closing index (`VK/VKVertexBuffers.cpp:20-21`) | native | `[V]` |

The expansion predicate is the shared `::is_primitive_native`
(`Common/BufferUtils.cpp:816-835`), used from `MTL/MTLGSRender.cpp:826-828`.
There is no format-coverage gap here.

**Primitive restart.** The guest restart index must become the host sentinel.
The shared `primitive_restart_impl::upload_untouched`
(`Common/BufferUtils.cpp:533-562`) rewrites it, and Metal calls the same
`::write_index_array_data_to_buffer` (`MTL/MTLGSRender.cpp:968-975`). Metal has
one extra case: a 16-bit **strip** with restart disabled cannot keep `0xFFFF` as a
vertex index, because Metal always restarts at the largest index and has no toggle.
The backend widens the array to 32 bits in that case
(`MTL/MTLGSRender.cpp:940-965`, converter at `:841-860`). Vulkan and GL set the
index type and restart state directly (`VK/VKVertexBuffers.cpp`, `GL/GLHelpers.cpp:66-150`),
so they do not need the widening. This is a correct Metal-specific workaround,
not a divergence.

---

## 6. Vertex fetch

All three backends use the same software vertex puller,
`glsl::insert_vertex_input_fetch` (`Program/GLSLCommon.cpp:119`), called from
`MTL/MTLVertexProgram.cpp:286` and `:453`, `VK/VKVertexProgram.cpp:373`,
`GL/GLVertexProgram.cpp:190`. The bytes are written by the shared
`draw_command_processor::write_vertex_data_to_memory`
(`Core/RSXDrawCommands.cpp:535-591`), so format decode, interleaved blocks,
inlined arrays and immediate draws are identical.

The only difference is the binding form: Metal declares two `usamplerBuffer`
streams (`MTL/MTLVertexProgram.cpp:146-150`) and uploads into a ring
(`MTL/MTLGSRender.cpp:5225-5252`), while VK/GL bind storage buffers. There is no
format or layout gap `[V]`.

`disable_vertex_cache` cannot be honoured because there is no cache to disable:
`create_context_resources` returns an empty `rsx::context_resources`
(`MTL/MTLGSRender.cpp:7481-7486`). The consequence is CPU cost per draw and the
absence of vertex-cache statistics in the debug overlay. Not a correctness gap.

---

## 7. Shaders

### 7.1 Precision and fp16

All three backends enable native half only when `shader_precision == low`:
`MTL/MTLFragmentProgram.cpp:676`, `VK/VKFragmentProgram.cpp:629-634`,
`GL/GLFragmentProgram.cpp:405-411`. Metal assumes native fp16 on Apple silicon and
sets `has_low_precision_rounding = false` (`MTL/MTLFragmentProgram.cpp:678`), which
is the NVIDIA-only workaround the other two guard with a vendor check. Metal also
never sets `require_explicit_invariance`; only the NVIDIA/Mesa paths do
(`VK/VKVertexProgram.cpp:368`, `GL/GLVertexProgram.cpp:184`), so Metal matches the
Apple-Vulkan and non-Mesa-GL case `[V]`.

`ROP_output_rounding` is computed identically in all three
(`MTL/MTLFragmentProgram.cpp:368`, `VK/VKFragmentProgram.cpp:411`,
`GL/GLFragmentProgram.cpp:242`).

### 7.2 Interpreter

Metal has a full interpreter path (`MTL/MTLShaderInterpreter.cpp`) and honours
`shadermode` (`MTL/MTLGSRender.cpp:4257-4303`). It stands aside, returning false
from `build_request`, for seven conditions:
vertex textures (`:645-650`), six fragment control bits (`:655-665`), shadow /
redirected / multisampled texture state and `ROP_MULTISAMPLED` (`:667-675`),
zero render targets (`:677-682`), and alpha-test cases it cannot express (`:688-705`).
Vulkan's interpreter has no such reject list.

This is a coverage difference, not a correctness one: a rejected draw falls back to
the recompiler, which is always correct. It matters only for the shader-stall
scenario the mode exists for.

### 7.3 Cache, threads, archive

`shader_compiler_threads_count` is honoured for Metal through the shared
`rsx::shaders_cache`, which treats `video_renderer::metal` as threaded
(`rsx_cache.h:299-306`). `disable_on_disk_shader_cache` is honoured twice: for the
program cache (`rsx_cache.h:231`) and for the Metal pipeline archive
(`MTL/MTLDevice.cpp:184`). Vulkan and OpenGL have no third layer, so the archive is
a Metal-only extra that correctly disappears when the setting is on `[V]`.

---

## 8. ROP, blending and multisampling

### 8.1 Logic op

Metal has no logic-op pipeline state, so the backend reports
`supports_native_logic_op = false` (`MTL/MTLDevice.cpp:379`). The shared core then
sets `RSX_SHADER_CONTROL_LOGIC_OP` (`Core/RSXGraphEngine.cpp:1249-1255`) and the
ROP epilogue applies the op to the fetched destination
(`Program/GLSLSnippets/RSXProg/RSXROPEpilogue.glsl:70-78`). Blending is disabled
when the logic op is enabled (`MTL/MTLBlendState.cpp:110-112`), and the op travels
as a shader constant (`MTL/MTLConstants.h:84`, `MTL/MTLGSRender.cpp:6401`).

VK and GL keep the default `true` (`Core/RSXRenderBackend.h:117`) and use native
state. The emulation is complete: it reads the same destination the hardware would,
in the framebuffer's unorm encoding. `[V]`

### 8.2 Programmable blending

Metal advertises it (`MTL/MTLDevice.cpp:367`) and implements it with framebuffer
fetch: SPIRV-Cross runs with `use_framebuffer_fetch_subpasses`
(`MTL/MTLShader.cpp:120`), and the blend inputs map onto the
`fragment_subpass_slot_base` range (`MTL/MTLArgumentBinds.h:55-57`). Vulkan also
claims it; OpenGL does not. No gap relative to VK `[V]`.

### 8.3 Alpha to coverage (D8, fixed)

`supports_hw_a2c` follows `antialiasing_level`, and `supports_hw_a2c_1spp` is
`false` (`MTL/MTLDevice.cpp:361-372`). The shared gate
(`Core/RSXGraphEngine.cpp:1257-1265`) emulates in-shader when the backend lacks
hardware A2C, or when the draw is 1-sample and the backend lacks 1-spp A2C. The
emulation is a stochastic `discard` (`RSXFragmentPrologue.glsl:106-113`,
`RSXROPEpilogue.glsl:51-56`).

Two consequences:

1. With `MSAA = none`, VK and GL clear `supports_hw_a2c`
   (`VK/VKGSRender.cpp:746-752`, `GL/GLGSRender.cpp:50-56`), so they always take the
   emulation. Metal used to keep hardware A2C, which is a no-op on a 1-sample
   attachment, so the dithering disappeared (D8); it now follows the same rule.
2. With `MSAA = auto` and a 1-sample guest surface, Metal claims 1-spp A2C is
   unsupported and emulates, while VK claims support and relies on hardware. On
   Apple hardware the VK claim is the questionable one, so Metal's choice is the
   conservative one.

### 8.4 MSAA and the depth mirror (D3, fixed)

Until this pass Metal's `supports_hw_msaa` was unconditional, while VK and GL only
set it when video MSAA is not `none`. `msaa_level` has exactly two values, `none`
and `_auto` (`rpcs3/Emu/system_config.h:130`), so with `_auto` the host and guest
sample counts agree and everything was already consistent.

With `MSAA = none` the old configuration disagreed:

- the surface's host sample count is 1 (`MTL/MTLSurface.cpp:63-65`);
- the guest still reports its AA mode, so `surface_antialias() != center_1_sample`
  is true for a guest MSAA surface;
- the shared core set `ROP_MULTISAMPLED` whenever a programmable-blend or
  depth-compare draw was emitted at that guest AA mode
  (`Core/RSXGraphEngine.cpp:933-939`, `:964-970`) because Metal claimed MSAA;
- the fragment program then declared `frag_depth` as `texture2DMS`
  (`MTL/MTLFragmentProgram.cpp:217-221`);
- the mirror is created with the *host* sample count
  (`MTL/MTLSurface.cpp:142-151`), i.e. 1.

Declaring a multisample texture and binding a single-sample one is an invalid
combination. The programmable-blend half was harmless: SPIRV-Cross forwards the
subpass expression and drops the sample index
(`3rdparty/spirv-cross/spirv_msl.cpp:11254-11265`), so no `subpassInputMS` reaches
MSL. The depth-compare half does reach MSL as a texture declaration and was a real
latent defect. `[V]` path, `[I]` runtime.

Both flags now follow `antialiasing_level` (`MTL/MTLDevice.cpp:361-372`), so the
declaration and the bound surface always agree.

### 8.5 Alpha to one

The flag is read only by GL (`GL/GLDraw.cpp:241-244`) and wired into
`VkPipelineMultisampleStateCreateInfo` by VK (`VK/VKGSRender.cpp:389-396`). Metal
drives `setAlphaToOneState` directly from the decoded register
(`MTL/MTLGSRender.cpp:1322-1323`, `MTL/MTLPipeline.cpp:79`), which produces the
same state without consulting the flag. Equivalent `[V]`.

### 8.6 Depth compare and depth range

`emulate_depth_compare` is a shared gate
(`Core/RSXGraphEngine.cpp:946-970`). Metal's mirror copy runs at
`MTL/MTLGSRender.cpp:4505-4514`; the shader reads the mirror through
`texture2DMS`/`texture2D` and the shared ROP epilogue. The depth-range emulation is
fully implemented: the vertex stage passes `depth_range` as a flat varying
(`MTL/MTLVertexProgram.cpp:255-257`, applied at `:416-425`), the fragment stage
consumes it (`MTL/MTLFragmentProgram.cpp:246-248`, `:421-422`), and the depth
bounds are re-encoded (`MTL/MTLDepthStencil.cpp:107-126`, `MTL/MTLEncoderState.cpp:9`,
`:37`).

So D3 is the only defect in this cluster, and it is a sample-count mismatch, not a
missing feature `[V]`.

---

## 9. Textures and samplers

### 9.1 Format coverage

`MTL/MTLFormats.cpp:583-665` covers every guest texture format, including the Apple
packed-format expansions and the BC1/2/3 BGRA8 sampling fallback. There is no
`PixelFormatInvalid` reachable for a guest format `[V]`.

### 9.2 Wrap modes

The Metal and Vulkan mappings are the same collapse: `clamp` and
`clamp_to_edge` both become edge clamp, and all three `mirror_once_*` become edge
mirror, dropping the border (MTL `MTLSamplerState.cpp:19-45`, VK
`VKFormats.cpp:87-102`). OpenGL is more exact and keeps
`GL_MIRROR_CLAMP_TO_BORDER_EXT` for the border variant
(`GL/glutils/sampler.cpp:21-23`). Metal therefore matches VK, not GL `[V]`.

### 9.3 Border colour

Metal exposes three border colours, so the decoded guest colour is reduced to
opaque white, opaque black or transparent black
(`MTL/MTLSamplerState.cpp:131-146`). VK passes the decoded colour as
`VkBorderColor`/custom-border ARGB and GL passes `GL_TEXTURE_BORDER_COLOR`
(`VK/VKDraw.cpp:297-301`, `:421-443`, `GL/glutils/sampler.cpp:81-113`). This is a
real quality limitation with no API workaround short of fetching the border in the
shader. `[V]` for the mapping, `[I]` for the visual size.

### 9.4 Anisotropy and LOD bias

Metal accepts only 1/2/4/8/16, so the guest value is rounded up
(`MTL/MTLSamplerState.cpp:114-117`); Vulkan takes the exact float
(`VK/VKFormats.cpp:104-130`). The LOD bias is rounded to 0.5 for nearest mip
filtering and clamped to `[-16, 15.999]` (`MTL/MTLSamplerState.cpp:120-128`),
matching VK's rounding. These are quantisation, not state errors `[V]`.

### 9.5 Texel remapping

Metal always emulates the signed-channel and sRGB remap in the shader: the sampler
is forced to nearest when a remap is active
(`MTL/MTLSamplerState.cpp:217-223`) and the fragment program sets
`RSX_SHADER_CONTROL_TEXTURE_FORMAT_CONVERT` from
`format_ex.texel_remap_control` (`Core/RSXGraphEngine.cpp:1462-1470`),
independently of `disable_hardware_texel_remapping`. VK and GL instead swap the
view format to a hardware SNORM/sRGB equivalent when the setting allows
(`VK/VKDraw.cpp:365-395`, `GL/GLDraw.cpp:373-402`). Metal's path is correct but
always pays the shader cost and loses linear filtering on those units `[V]`.

### 9.6 Feedback loops

Metal has no texture barrier. When a draw samples a bound attachment, the recorder
closes the pass, stores and resolves, then opens a fresh one
(`MTL/MTLGSRender.cpp:4516-4522`, detection at `:4522-4548`, policy split at
`:4589-4600`). GL uses `glTextureBarrier` and VK uses the feedback-loop layout.
Correct in all three; Metal pays a pass split `[V]`.

### 9.7 Transfer skips (D5, D6)

Two copy paths silently degrade. A scaled region is refused
(`MTL/MTLTextureCache.cpp:960-964`) and a typeless copy between different texel
sizes is refused (`:978-986`). VK and GL use `vkCmdBlitImage` /
`glBlitFramebuffer`, which scale and convert. Both Metal paths log an error and
`continue`, so the destination keeps whatever it had. `[V]` path. Which games
reach them is `[U]`.

---

## 10. Surfaces and depth formats

| guest depth format | Metal | Vulkan | OpenGL | conf |
|--------------------|-------|--------|--------|------|
| `z16_uint` | `Depth16Unorm` | `D16_UNORM` | `GL_DEPTH_COMPONENT16` | `[V]` |
| `z16_float` | `Depth32Float` | `D32_SFLOAT` | float depth | `[V]` |
| `z24s8_uint` | `Depth32Float_Stencil8` | `D24_UNORM_S8` or `D32_SFLOAT_S8` | `GL_DEPTH24_STENCIL8` | `[V]` |
| `z24s8_float` | `Depth32Float_Stencil8` | `D32_SFLOAT_S8` | float+stencil | `[V]` |

Metal always picks the wide variant for the mixed formats
(`MTL/MTLFormats.cpp:192-209`), so it is at or above the precision VK/GL choose,
and `force_high_precision_z_buffer` is already satisfied. The only narrow case,
`z16_uint`, is narrow in all three `[V]`.

Surface creation, the MSAA resolve target, the depth mirror and the stencil view
are all in `MTLSurface.cpp` (`:49-101`, `:121-125`, `:127-155`, `:157-174`). The
read-back path (`MTL/MTLTextureCache.cpp:684-800`) covers depth, stencil and packed
colour, which is what makes `write_depth_buffer` / `write_color_buffers` behave
`[V]`.

---

## 11. Synchronisation, queries, ZCULL

`MTL/MTLZCULL.cpp:123-172` maps the shared occlusion query onto Metal's
`VisibilityResultMode`, choosing `Counting` when `precise_zpass_count` is on and
`Boolean` otherwise (`:158-164`). Counters are pooled per live query with a
deferred release (`:100-120`, `:195-250`), and the mode is re-emitted once per pass
by the draw path (`MTL/MTLGSRender.cpp:6664-6666`) because the mode does not
survive an encoder boundary.

Vulkan additionally ends the render pass on strict-scope drivers and can flush
early on a conditional-render hint
(`VK/VKGSRender.cpp:1820-1850`, `:2891`). Metal does not override `on_sync_hint`
(the base is a no-op, `Core/RSXRenderBackend.h:208`), so it always takes the
blocking `ZCULL_control::read_barrier` path
(`RSX/RSXZCULL.cpp:761-802`). Correct, but the FIFO reader can stall where VK
would not.

`supports_hw_conditional_render = false` (`MTL/MTLDevice.cpp:385`) matches
OpenGL's default and is the conservative choice: the draw is skipped on the CPU
(`Core/RSXCoreRenderer.cpp:745-752`) instead of using a host predicate. VK uses
the host feature except on MVK (`VK/VKGSRender.cpp:776`) `[V]`.

`write_barrier` is the same in all three: invalidate the affected surface range
(MTL `MTL/MTLGSRender.cpp:7915-7942`, VK `VK/VKGSRender.cpp:1814-1818`, GL
`GL/GLGSRender.cpp:1448-1452`) `[V]`.

---

## 12. Blit engine

The guest has three transfer engines and all three backends handle them the same
way except one step.

| engine | Metal | Vulkan | OpenGL | conf |
|--------|-------|--------|--------|------|
| `nv0039` (context DMA copy) | shared host copy (`NV47/HW/nv0039.cpp:90-175`) | same | same | `[V]` |
| `nv308a` (image-to-buffer) | shared, byte-faithful except `r5g6b5` conversion (`NV47/HW/nv308a.cpp:64`, `:107`) | same | same | `[V]` |
| `nv3089` (image-in, optional GPU blit) | CPU `SWScale`/`memcpy` fallback (`MTL/MTLGSRender.cpp:7855-7912`) | GPU blit through the texture cache (`VK/VKGSRender.cpp:2852-2869`) | GPU blit (`GL/GLGSRender.cpp:1360-1370`) | `[V]` |

The dispatch is shared: `NV47/HW/nv3089.cpp:614-620` calls
`renderer()->scaled_image_from_memory(...)` and falls back to the CPU path when the
backend returns false. Metal always returns false, so the CPU path always runs,
with explicit source/destination invalidation to avoid stale host copies. Correct
`[V]`, slower `[I]`.

Two settings are attached to that path and are therefore no-ops on Metal:
`force_cpu_blit_processing` and `disable_blit_engine_upscaling` (§4.2).

---

## 13. Memory import and DMA

Metal imports the whole guest window once, with a zero-copy shared buffer over the
unprotected mirror (`MTL/MTLDevice.cpp:340-355`). Vulkan imports the same region
when the device supports host-pointer external memory and exposes
`supports_passthrough_dma` (`VK/VKGSRender.cpp:779`); OpenGL leaves it false.
`supports_passthrough_dma = false` on Metal is therefore consistent with GL and
with the fact that only `VK/VKDMA.cpp:306` consumes the flag `[V]`.

`supports_asynchronous_compute = true` has no Metal consumer; the backend has one
queue and uploads inline (`MTL/MTLTextureCache.cpp:126-251`). The claim is inert
(D14) `[V]`.

---

## 14. Present, output scaling, stereo, overlays

- **Scaling.** `nearest` and `bilinear` run the corresponding output pass
  (`MTL/MTLPresent.cpp:101-104`). `fsr` selects the bilinear pass because no FSR
  pass exists (D4).
- **RCAS.** `rcas_sharpening_intensity` is read only by the VK/GL FSR passes
  (`VK/upscalers/fsr1/fsr_pass.cpp:182`, `GL/upscalers/fsr1/fsr_pass.cpp:157`), so
  the slider is inert on Metal (D4).
- **Vsync.** Metal maps `off` to display-sync off plus immediate present, `full` to
  display-sync on, and `adaptive` to a one-refresh present interval
  (`MTL/MTLPresenter.cpp:232-251`). Vulkan picks a present-mode chain (FIFO by
  default; `IMMEDIATE`/`MAILBOX`/`FIFO_RELAXED` for the other modes,
  `VK/vkutils/swapchain.cpp:236-250`). This is an approximation, not a bug `[V]`.
- **Stereo.** Metal fetches the second eye below the first across the blanking
  interval (`MTL/MTLGSRender.cpp:7235-7247`), runs the same shared calibration pass
  shader (`MTL/MTLOverlays.cpp:776-781`, shader at
  `Program/GLSLSnippets/VideoOutCalibrationPass.glsl:34-51`), and fills the same
  stereo config, including the anaglyph matrices (`MTL/MTLOverlays.cpp:790-800`).
  Every merge mode is honoured. No gap `[V]`.
- **Full range and gamma.** Both reach the calibration pass
  (`MTL/MTLGSRender.cpp:7386-7387`) `[V]`.
- **Overlays.** The debug overlay text is built and set
  (`MTL/MTLGSRender.cpp:7351-7375`), and `record_with_overlays` is honoured at
  `:7281`. No gap `[V]`.

---

## 15. Developer-facing gaps

| item | Metal | Vulkan | OpenGL | class | conf |
|------|-------|--------|--------|-------|------|
| shader log | GLSL+MSL per program under `shaderlog/` (`MTL/MTLProgramPipeline.cpp:117-127`) | SPIR-V/GLSL | GLSL | equivalent | `[V]` |
| validation on `debug_output` | no reader | loads validation + debug callback (`VK/vkutils/instance.cpp:140`, `:268-269`) | uses the flag | inert | `[V]` |
| RenderDoc compatibility | no reader | devices/instance changes | context flags | inert | `[V]` |
| compile-stall hint | no reader of `show_shader_compilation_hint` | shows a hint | shows a hint | absent | `[V]` |
| vertex-cache statistics | none (no cache) | shared counters | shared counters | absent | `[V]` |
| debug overlay counters | present, minus cache counters (`MTL/MTLGSRender.cpp:7351-7375`) | present | present | equivalent | `[V]` |

For `debug_output`, the Metal runtime reads its own `MTL_DEBUG_LAYER` environment
variable; the GUI checkbox cannot reach it without new code. That is a small,
contained addition if it is wanted.

---

## 16. Corrections to earlier documents

`DEVIATIONS-FEATURES.md` and `PORT-RENDER-BACKEND.md` pre-date several backend
changes. The following rows there are wrong in this tree:

| earlier claim | actual state | evidence | conf |
|---------------|--------------|----------|------|
| `shadermode` is never read; Metal has no interpreter | read, and a full interpreter exists | `MTL/MTLGSRender.cpp:4257-4303`, `MTL/MTLShaderInterpreter.cpp:635-705` | `[V]` |
| `log_programs` has no Metal reader | read; writes GLSL and MSL | `MTL/MTLProgramPipeline.cpp:117-127` | `[V]` |
| `debug_overlay` is never given text | text is built every frame | `MTL/MTLGSRender.cpp:7351-7375` | `[V]` |
| `disable_msl_fast_math` is never read | read; selects safe or fast math | `MTL/MTLShader.cpp:224-228` | `[V]` |
| `shader_compiler_threads_count` is not read | read through the shared cache; Metal counts as threaded | `rsx_cache.h:299-306` | `[V]` |
| `host_label_synchronization` is forced false | follows the setting; GPU label writer implemented | `MTL/MTLDevice.cpp:393`, `MTL/MTLGSRender.cpp:7730-7784` | `[V]` |
| self-declared ignored list has ten entries, six false | three entries, all accurate | `MTL/MTLDevice.cpp:98-108` | `[V]` |
| `read_color_buffers` / `read_depth_buffer` are ignored | honoured through shared `needs_cpu_upload()` | `Common/surface_utils.h:383-398`, `MTL/MTLSurface.cpp:343-350` | `[V]` |
| `strict_rendering_mode` is ignored | honoured through shared gates | `RSXOffload.cpp:52`, `Core/RSXGraphEngine.cpp:1426` | `[V]` |
| `force_high_precision_z_buffer` is a Metal gap | inert but already satisfied; VK/GL never upgrade `z16_uint` either | `MTL/MTLFormats.cpp:192-209`, `VK/vkutils/device.cpp:1229-1233` | `[V]` |
| Metal lacks a GPU blit path, so two blit settings are broken | the path is absent by design; both settings are no-ops, not broken | `MTL/MTLGSRender.cpp:7855-7912` | `[V]` |

Two further claims that other documents repeat and that are simply inert rather
than gaps: `supports_multidraw` (no reader) and `supports_asynchronous_compute`
(only VK reads it).

---

## 17. Unresolved

1. Which games reach the scaled/typeless transfer skips (D5, D6). Needs a probe that
   counts `Metal 4: scaled texture copy is not supported` and the typeless variant
   per title `[U]`.
2. Whether Metal supports a zero-attachment pass at all, and at which sample
   counts, on this OS. The API has no such object; a dummy attachment is the only
   route, and its cost is unmeasured `[U]`.
3. Whether D1 (flat shading) is visible in the titles that use guest flat shading.
   The mechanism is proven, the per-title impact is not `[U]`.

D3 and D8 were latent-by-configuration defects: neither could fire under the
default `MSAA = auto`, and neither was observed at runtime before §18 removed them.

---

## 18. Disposition under the "100% confirmed" rule

Confirmed and reachable, fixed in this pass (one block, `MTL/MTLDevice.cpp:361-372`):

1. **D3** — `supports_hw_msaa` and `supports_hw_a2c` are now both derived from
   `g_cfg.video.antialiasing_level != msaa_level::none`, exactly as VK and GL
   derive them. The host sample count already follows that setting
   (`MTL/MTLSurface.cpp:63-65`), so a single-sample target no longer makes the
   core declare `ROP_MULTISAMPLED` (`Core/RSXGraphEngine.cpp:933-939`, `:964-970`)
   or ask the driver for hardware A2C that a 1-sample attachment cannot perform.
   `antialiasing_level` has only `none` and `_auto`, so with the default `_auto`
   the previous values are unchanged and nothing that renders today moves.
2. **D8** — same expression as D3; the stochastic A2C emulation
   (`Core/RSXGraphEngine.cpp:1257-1265`) now runs when video MSAA is off, matching
   VK/GL.

`supports_hw_a2one` stays `true`: Metal drives `AlphaToOneState` from the decoded
register and no shared gate reads the flag for Metal, so gating it would only
change the debug overlay's capability report.

Confirmed but not fixed here:

3. **D2 honesty** — flipping `supports_framebufferless_rendering` to `false` does
   not recover a single draw: the core returns at `Core/RSXGraphEngine.cpp:504-509`
   instead of at `:839-843`, and `rtt_config_valid` stays clear either way. Worse,
   the failure also makes `is_framebuffer_bound()` return false
   (`MTL/MTLGSRender.cpp:1879-1882`), so `bind_framebuffer`'s fast path
   (`Core/RSXGraphEngine.cpp:808-813`) never engages and the warning would print
   once per draw command. The fix that actually helps is a dummy attachment or a
   compute occlusion count, which is design work.
4. **D1** — flat shading needs provoking-index rotation of flat primitives, or an
   equivalent trick; a plain `flat` would be wrong in the other direction.
5. **D5/D6** — scaled and cross-texel-size copies need a scaled blit or a compute
   shader; skipping is correct-by-refusal but loses content.
6. **D4** — FSR/RCAS is a new pass, not a wiring change.
7. **D9/D10/D11** — sampler approximation has no API workaround short of a shader
   fetch.

D3/D8 are the only changes that meet the "100% confirmed defect" bar with a
bounded blast radius; both are inert under the default video configuration.

<!-- doc-end -->
