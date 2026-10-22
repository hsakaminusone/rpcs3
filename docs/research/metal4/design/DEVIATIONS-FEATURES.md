# Deviations, Missing Features and Metal-Only Opportunities

Metal 4 backend against the Vulkan and OpenGL backends.

All paths are relative to the repository root. `MTL` means `rpcs3/Emu/RSX/MTL`,
`VK` means `rpcs3/Emu/RSX/VK`, `GL` means `rpcs3/Emu/RSX/GL`.

---

## 1. Method

1. Enumerate every key of `node_video` in `rpcs3/Emu/system_config.h:118-208`.
2. Find every read of `g_cfg.video.<key>` in `MTL`, `VK`, `GL` and in shared code
   (`rpcs3/Emu/RSX/Common`, `rpcs3/Emu/RSX/Core`, `rpcs3/Emu/RSX/RSXThread.cpp`,
   `rpcs3/Emu/Cell`, `rpcs3/rpcs3qt`).
3. Read the Metal present path (`MTLPresent.cpp`, `MTLGSRender.cpp`,
   `MTLOverlays.cpp`, `MTLMetalLayer.mm`, `MTLPresenter.cpp`) and compare it with
   `VKPresent.cpp` and `GLPresent.cpp`.
4. Verify the self-declared ignored list in `MTLDevice.cpp:79-113`. Some entries
   in that list are read by shared code (Table A).
5. Read the Metal 4 capability block in `MTLDevice.cpp:285-315` and compare it
   with `VKGSRender.cpp:740-910` and `GLGSRender.cpp:40-64,180-229`.
6. Mine KosmicKrisp (`.kk-mesa/src/kosmickrisp/`) and the Mesa WSI layer it uses
   (`.kk-mesa/src/vulkan/wsi/wsi_common_metal_layer.m`) for Metal-4-only
   techniques.
7. Classify each row. Rank each deviation by user-visible impact.

No build, no run, no commit. This document is the only file that was written.

Classification values:

- `missing feature` — the backend does not implement the function at all.
- `ignored setting` — the setting exists, but the backend never reads it.
- `different behaviour` — the backend reads the setting and acts differently.
- `capability mismatch` — the backend reports a lower or higher capability.
- `optional enhancement` — no other backend implements it; parity is not at risk.
- `not a deviation` — the setting is honoured, or the setting is backend-specific.

---

## 2. Table A — Settings parity

`VK/GL file:line` gives the read site. `(shared)` means the read is in shared
code, so both Vulkan and OpenGL use it. `Metal never reads it` is used only when
the whole MTL tree has no read of that key.

| id | setting / config key | VK/GL file:line | Metal file:line | Metal state | class | user-visible effect | priority | proposed fix |
|----|----------------------|-----------------|-----------------|-------------|-------|---------------------|----------|--------------|
| A1 | `renderer` (`system_config.h:123`) | `settings_dialog.cpp:401` | `MTLGSRender.cpp:607` | honoured | not a deviation | Metal appears in the renderer list on macOS 26 (`MTLAvailability.h:33`) | - | none |
| A2 | `resolution` (`system_config.h:128`) | `gui_application.cpp:635` (shared) | same shared site | honoured | not a deviation | window size follows the setting | - | none |
| A3 | `aspect_ratio` (`system_config.h:129`) | `system_utils.cpp:691` (shared) | same shared site | honoured | not a deviation | aspect is converted in `MTLGSRender.cpp:3058` | - | none |
| A4 | `frame_limit` (`system_config.h:130`) | `RSXThread.cpp:3663` (shared) | same shared site | honoured | not a deviation | frame pacing works | - | none |
| A5 | `second_frame_limit` (`system_config.h:131`) | `RSXThread.cpp:3680` (shared) | same shared site | honoured | not a deviation | second limit works | - | none |
| A6 | `antialiasing_level` (`system_config.h:132`) | `GLGSRender.cpp:179` | `MTLSurface.cpp:38-39` | honoured | not a deviation | MSAA sample count is selected; MSAA is resolved in `MTLSurface.cpp:181` | - | none |
| A7 | `shadermode` (`system_config.h:133`) | `VKGSRender.cpp:639,1367-1377`; `GLGSRender.cpp:109,207,830` | Metal never reads it | no interpreter path and no async pipeline | different behaviour | "Interpreter only" and "async with interpreter" still recompile synchronously; a game with a shader stall shows no interpreter fallback | H | map the mode, or add the interpreter path |
| A8 | `shader_precision` (`system_config.h:134`) | `RSXThread.cpp:2627` (shared) | `MTLFragmentProgram.cpp:290` | honoured | not a deviation | ROP output rounding follows the setting | - | none |
| A9 | `vsync` (`system_config.h:135`) | `VKPresent.cpp:146,438`; `GLGSRender.cpp:621` | `MTLPresenter.cpp:220-227` | honoured | not a deviation | `full`, `adaptive` and `off` map to display sync and present interval | - | none |
| A10 | `write_color_buffers` (`system_config.h:137`) | `VKGSRender.cpp:2662,2783` | `MTLGSRender.cpp:847,945` | honoured | not a deviation | colour buffer export works | - | none |
| A11 | `write_depth_buffer` (`system_config.h:138`) | `VKGSRender.cpp:2679,2799` | `MTLGSRender.cpp:863,1029` | honoured | not a deviation | depth buffer export works | - | none |
| A12 | `read_color_buffers` (`system_config.h:139`) | `texture_cache.h:1446` (shared) | same shared site | honoured | not a deviation | self-declared ignored at `MTLDevice.cpp:93`, but shared code reads it; the list over-reports | - | remove from the ignored list |
| A13 | `read_depth_buffer` (`system_config.h:140`) | `texture_cache.h:1446` (shared) | same shared site | honoured | not a deviation | self-declared ignored at `MTLDevice.cpp:94`; the list over-reports | - | remove from the ignored list |
| A14 | `handle_tiled_memory` (`system_config.h:141`) | `RSXContext.cpp:17` (shared) | same shared site | honoured | not a deviation | tiled layouts are decoded for all backends | - | none |
| A15 | `log_programs` (`system_config.h:142`) | `VKFragmentProgram.cpp:647`; `GLProgramBuffer.h:64` | Metal never reads it | no shader dump | missing feature | a user who wants an MSL/IR dump for a bug report gets nothing | L | dump the SPIR-V/IR and the MSL source |
| A16 | `debug_output` (`system_config.h:143`) | `VK/vkutils/instance.cpp:140`; `GLGSRender.cpp:151` | Metal never reads it | no validation layer from config | ignored setting | the debug checkbox does not turn on Metal validation; a developer must set an environment variable by hand | L | map the flag to `MTL_DEBUG_LAYER` or the Metal validation API |
| A17 | `debug_overlay` (`system_config.h:144`) | `VKPresent.cpp:947`; `GLPresent.cpp:470` | MTL creates the overlay in shared code, but never calls `set_debug_overlay_text` | overlay is present and empty | different behaviour | the RSX statistics pane is blank on Metal; `set_debug_overlay_text` has only two callers, both VK/GL (`overlay_debug_overlay.cpp:67`) | M | build the same text in `MTLGSRender.cpp` |
| A18 | `renderdoc_compatiblity` (`system_config.h:145`) | `VK/vkutils/device.cpp:949`; `GL/glutils/capabilities.cpp:48` | Metal never reads it | no RenderDoc path | ignored setting | no effect on Metal; RenderDoc has no Metal capture path here, so the row is low impact | L | hide the checkbox for Metal, or document the limit |
| A19 | `use_gpu_texture_scaling` (`system_config.h:146`) | `texture_cache.cpp:407` (shared) | same shared site | honoured | not a deviation | GPU texture scaling works | - | none |
| A20 | `stretch_to_display_area` (`system_config.h:147`) | `VKPresent.cpp:640`; `GLPresent.cpp:265` | `MTLGSRender.cpp:3058` | honoured | not a deviation | stretch and aspect modes work | - | none |
| A21 | `force_high_precision_z_buffer` (`system_config.h:148`) | `device.cpp:1187`; `GLGSRender.cpp:179` | Metal never reads it; depth is always `Depth32Float` (`MTLFormats.cpp:82,85`) | setting has no effect | ignored setting | Metal already uses high precision, so the user sees no change either way | M | remove from the ignored list and document that Metal is always high precision |
| A22 | `strict_rendering_mode` (`system_config.h:149`) | `RSXOffload.cpp:52` (shared) | same shared site | honoured | not a deviation | self-declared ignored at `MTLDevice.cpp:97`; the list over-reports | - | remove from the ignored list |
| A23 | `fb_aliasing_bias` (`system_config.h:150`) | `RSXThread.cpp:1757` (shared) | same shared site | honoured | not a deviation | aliasing heuristic works | - | none |
| A24 | `disable_zcull_queries` (`system_config.h:151`) | `RSXThread.cpp:1020` (shared) | same shared site | honoured | not a deviation | occlusion queries are disabled as asked | - | none |
| A25 | `disable_video_output` (`system_config.h:152`) | `RSXThread.cpp:3735` (shared) | same shared site | honoured | not a deviation | headless flips are skipped | - | none |
| A26 | `disable_vertex_cache` (`system_config.h:153`) | `VKGSRender.cpp:716`; `GLGSRender.cpp:45` | Metal never reads it; Metal has no vertex cache (`MTLGSRender.cpp:2115-2157` uploads per draw) | setting has no effect | ignored setting | the "Disable Vertex Cache" workaround cannot be used; a user with a broken index path cannot turn it off because it is already off | H | document that Metal is permanently in this mode |
| A27 | `disable_FIFO_reordering` (`system_config.h:154`) | `RSXThread.cpp:3542` (shared) | same shared site | honoured | not a deviation | command order control works | - | none |
| A28 | `emulate_depth_compare` (`system_config.h:155`) | `RSXThread.cpp:2194` (shared) | same shared site | honoured | not a deviation | depth compare emulation works | - | none |
| A29 | `emulate_extended_depth_range` (`system_config.h:156`) | `RSXThread.cpp:2224` (shared) | same shared site | honoured | not a deviation | self-declared ignored at `MTLDevice.cpp:92`; shared code runs the emulation because Metal reports `supports_extended_depth_range=false` (`MTLDevice.cpp:300`) | - | remove from the ignored list |
| A30 | `frame_skip_enabled` (`system_config.h:157`) | `RSXThread.cpp:3549` (shared) | same shared site | honoured | not a deviation | frame skip works | - | none |
| A31 | `force_cpu_blit_processing` (`system_config.h:158`) | `nv3089.cpp:592` (shared) | Metal never reads it; `scaled_image_from_memory` returns false (`MTLGSRender.cpp:3291`) | always CPU blit | different behaviour | the setting has no effect because Metal has no GPU blit path to disable | L | implement the GPU blit path, then the setting becomes meaningful |
| A32 | `disable_on_disk_shader_cache` (`system_config.h:159`) | `rsx_cache.h:216` (shared) | `MTLDevice.cpp:150` | honoured | not a deviation | the Metal pipeline archive is disabled as asked | - | none |
| A33 | `disable_vulkan_mem_allocator` (`system_config.h:160`) | `device.cpp` VMA init | not applicable to Metal | Vulkan-only key | not a deviation | no effect outside Vulkan | - | none |
| A34 | `full_rgb_range_output` (`system_config.h:161`) | `VKPresent.cpp:826`; `GLPresent.cpp:396` | `MTLGSRender.cpp:3080` | honoured | not a deviation | limited and full range calibration works | - | none |
| A35 | `strict_texture_flushing` (`system_config.h:162`) | `texture_cache.h:1196` (shared) | same shared site | honoured | not a deviation | flush policy works | - | none |
| A36 | `multithreaded_rsx` (`system_config.h:163`) | `RSXOffload.cpp:27` (shared) | same shared site | honoured | not a deviation | the RSX offload thread works | - | none |
| A37 | `relaxed_zcull_sync` (`system_config.h:164`) | `nv308a.cpp:64` (shared) | same shared site | honoured | not a deviation | self-declared ignored at `MTLDevice.cpp:98`; the list over-reports | - | remove from the ignored list |
| A38 | `force_hw_MSAA_resolve` (`system_config.h:165`) | `texture_cache_helpers.h:93` (shared) | same shared site | honoured | not a deviation | MSAA resolve policy works | - | none |
| A39 | `stereo_enabled` (`system_config.h:166`) | `VKPresent.cpp:565` | `MTLGSRender.cpp:2997,3080` | honoured | not a deviation | second eye is read below the first, per `VKPresent.cpp:565` | - | none |
| A40 | `stereo_render_mode` (`system_config.h:167`) | `VKPresent.cpp:833` | `MTLOverlays.cpp:778-785` | honoured | not a deviation | all stereo merge modes work through `stereo_config` | - | none |
| A41 | `screen_size` (`system_config.h:168`) | `cellAvconfExt.cpp:571` (shared) | same shared site | honoured | not a deviation | reported screen size works | - | none |
| A42 | `debug_program_analyser` (`system_config.h:169`) | `ProgramStateCache.cpp:341` (shared) | same shared site | honoured | not a deviation | program analyser works | - | none |
| A43 | `precise_zpass_count` (`system_config.h:170`) | `RSXZCULL.cpp:353` (shared) | same shared site | honoured | not a deviation | self-declared ignored at `MTLDevice.cpp:96`; the list over-reports | - | remove from the ignored list |
| A44 | `consecutive_frames_to_draw/_skip` (`system_config.h:171-172`) | `RSXThread.cpp:3553` (shared) | same shared site | honoured | not a deviation | frame skip counts work | - | none |
| A45 | `resolution_scale_percent` (`system_config.h:173`) | `RSXThread.cpp:1171` (shared) | `MTLSurface.cpp:49` | honoured | not a deviation | internal resolution scaling works | - | none |
| A46 | `anisotropic_level_override` (`system_config.h:174`) | `RSXTexture.cpp:217` (shared) | MTL sampler state | honoured | not a deviation | anisotropic override works | - | none |
| A47 | `texture_lod_bias` (`system_config.h:175`) | `RSXTexture.cpp:315` (shared) | MTL sampler state | honoured | not a deviation | LOD bias works | - | none |
| A48 | `min_scalable_dimension` (`system_config.h:176`) | `RSXThread.cpp:1172` (shared) | same shared site | honoured | not a deviation | scale threshold works | - | none |
| A49 | `shader_compiler_threads_count` (`system_config.h:177`) | `VKGSRender.cpp:700-702,1395-1397`; `GLGSRender.cpp:127` | Metal never reads it; `rsx_cache.h:280-281` uses `utils::get_thread_count()` | user count is ignored | ignored setting | a user on a low-core or high-core machine cannot tune compile workers; Metal always uses all cores | M | apply the count in `rsx_cache.h:280-281` |
| A50 | `driver_recovery_timeout` (`system_config.h:178`) | `nv406e.cpp:68` (shared) | same shared site | honoured | not a deviation | works | - | none |
| A51 | `driver_wakeup_delay` (`system_config.h:179`) | `RSXThread.cpp:3066` (shared) | same shared site | honoured | not a deviation | works | - | none |
| A52 | `vblank_rate` / `vblank_ntsc` (`system_config.h:180-181`) | `RSXThread.cpp:1198-1199` (shared) | same shared site | honoured | not a deviation | works | - | none |
| A53 | `decr_memory_layout` (`system_config.h:182`) | `sys_rsx.cpp:459` (shared) | same shared site | honoured | not a deviation | works | - | none |
| A54 | `host_label_synchronization` (`system_config.h:183`) | `VKGSRender.cpp:801`; `GLGSRender.cpp:222` | Metal never reads it; Metal forces `supports_host_gpu_labels=false` (`MTLDevice.cpp:314`) | setting is ignored | ignored setting | the "Allow Host GPU Labels" workaround has no effect on Metal, so a game that needs label-driven sync cannot use it | M | implement host labels, or hide the checkbox for Metal |
| A55 | `disable_msl_fast_math` (`system_config.h:184`) | `VK/vkutils/instance.cpp:185` (MoltenVK) | Metal never reads it | setting is ignored | ignored setting | a Metal-only setting does nothing on the Metal backend; a user who sees fast-math artifacts cannot turn fast math off | H | set the MSL math mode in `MTLShader.cpp` |
| A56 | `disable_async_host_memory_manager` (`system_config.h:185`) | `MM.cpp:129` (shared) | same shared site | honoured | not a deviation | works | - | none |
| A57 | `output_scaling` (`system_config.h:186`) | `VKPresent.cpp:804-820`; `GLPresent.cpp:399-413` | Metal never reads it; `compose` always runs the bilinear `m_scale` or the calibration pass (`MTLPresent.cpp:88-97`) | `nearest` and `fsr` are silently replaced by `bilinear` | missing feature | the user selects "FidelityFX Super Resolution" and gets bilinear; `m_nearest` is initialised (`MTLPresent.cpp:31`) but never used | H | drive the upscaler from the setting; add an FSR pass |
| A58 | `record_with_overlays` (`system_config.h:187`) | `VKPresent.cpp:684` | `MTLGSRender.cpp:3033` | honoured | not a deviation | recording with or without overlays works | - | none |
| A59 | `disable_hardware_blending` (`system_config.h:188`) | `RSXThread.cpp:319` (shared) | same shared site | honoured | not a deviation | ROP blending control works | - | none |
| A60 | `disable_hardware_texel_remapping` (`system_config.h:189`) | `VKDraw.cpp:368`; `GLDraw.cpp:398` | Metal never reads it; self-declared ignored at `MTLDevice.cpp:89` | setting is ignored | ignored setting | Metal has no hardware remap path, so the workaround is already "off"; the user sees no change | M | document it, or implement the remap path |
| A61 | `rcas_sharpening_intensity` (`system_config.h:190`) | `VK fsr_pass.cpp:182`; `GL fsr_pass.cpp:157` | Metal never reads it | the sharpening slider has no effect | missing feature | the slider is visible for Metal (`settings_dialog.cpp:897-902`) but nothing consumes it | M | read it in the FSR/RCAS pass that A57 adds |
| A62 | `disable_blit_engine_upscaling` (`system_config.h:191`) | `texture_cache.h:1787` (shared) | Metal never reads it; no GPU blit path exists | setting has no effect | different behaviour | the user sees no change, because Metal never upscales in the blit engine | L | implement the GPU blit path, then the setting becomes meaningful |
| A63 | `show_shader_compilation_hint` (`system_config.h:387`, misc) | `VKGSRender.cpp:2045`; `GLGSRender.cpp:889` | Metal never reads it | no hint is shown | ignored setting | a user with a shader stall gets no "compiling shaders" hint on Metal | L | show the hint on a pipeline build |

### Table A notes

- The self-declared list at `MTLDevice.cpp:79-113` contains ten entries. Six of them
  (`read_color_buffers`, `read_depth_buffer`, `precise_zpass_count`,
  `relaxed_zcull_sync`, `strict_rendering_mode`, `emulate_extended_depth_range`)
  are read by shared code. The list over-reports by six.
- `report_ignored_settings()` runs only when `MTL_DIAGNOSTICS` is defined
  (`MTLDevice.cpp:194-199`). A release build emits no warning at all.

---

## 3. Table B — Backend features and capabilities

`| id | feature | VK/GL file:line | Metal file:line | Metal state | class | required for parity? | proposed fix |`

| id | feature | VK/GL file:line | Metal file:line | Metal state | class | required for parity? | proposed fix |
|----|---------|-----------------|-----------------|-------------|-------|----------------------|--------------|
| B1 | Multi-draw | `VKGSRender.cpp:746`; `GLGSRender.cpp:50` | `MTLDevice.cpp:289` (`false`) | draw clauses loop | capability mismatch | yes, for draw-call throughput | host-built indirect batcher |
| B2 | Hardware A2C at 1 sample | `VKGSRender.cpp:769` | `MTLDevice.cpp:292` (`false`) | off | capability mismatch | no; GL is also off (`GLGSRender.cpp:61`) | none; keep the software path |
| B3 | Programmable blending | `VKGSRender.cpp:748` | `MTLDevice.cpp:295` (`true`) | framebuffer fetch works | not a deviation | no; Metal is ahead of GL here | none |
| B4 | Extended depth range | `VKGSRender.cpp:750`; `GLGSRender.cpp:55` | `MTLDevice.cpp:300` (`false`) | shader clip emulation | capability mismatch | no; the emulation path covers it (`RSXThread.cpp:2224`) | none |
| B5 | Last provoking vertex | `VKGSRender.cpp:751`; `GLGSRender.cpp:54` | `MTLDevice.cpp:312` (`false`) | smooth fallback | capability mismatch | possible; some games depend on it | flat-shader emulation with a provoking index |
| B6 | Asynchronous compute | `VKGSRender.cpp:807` | `MTLDevice.cpp:302` (`true`) | independent command buffers | not a deviation | no | none |
| B7 | Passthrough DMA | `VKGSRender.cpp:798` | `MTLDevice.cpp:311` (`false`) | off | capability mismatch | no; a performance feature | none for parity |
| B8 | Host GPU labels | `VKGSRender.cpp:801`; `GLGSRender.cpp:224` | `MTLDevice.cpp:314` (`false`) | off | capability mismatch | yes, for the setting in A54 | implement label-driven sync |
| B9 | Hardware renormalization | `GLGSRender.cpp:380` | `MTLDevice.cpp:310` (`false`) | off | capability mismatch | no; GL sets it for NVIDIA only | none |
| B10 | Vertex cache | `VKGSRender.cpp:716` (`disable_vertex_cache`); vertex cache in shared offload | no MTL vertex cache; per-draw upload (`MTLGSRender.cpp:2115-2157`) | absent | missing feature | not for correctness; yes for CPU cost | add an index/vertex cache |
| B11 | GPU blit engine (NV3089) | `VKGSRender.cpp:2870`; `GLGSRender.cpp:1395` | `MTLGSRender.cpp:3264-3291` returns `false` | CPU blit only | missing feature | yes, for parity of `force_cpu_blit_processing` and `disable_blit_engine_upscaling` | implement `scaled_image_from_memory` |
| B12 | FSR 1.0 upscale | `VKPresent.cpp:814`; `GLPresent.cpp:408` | `MTLPresent.cpp:88-97` always scales bilinearly | absent | missing feature | yes, for the "Output Scaling Mode" option | add an FSR pass and select it in `compose` |
| B13 | Nearest upscale | `VKPresent.cpp:810`; `GLPresent.cpp:405` | `m_nearest` is built (`MTLPresent.cpp:31`) but never run | dead object | missing feature | yes | run `m_nearest` when the setting is `nearest` |
| B14 | RCAS sharpening | `VK fsr_pass.cpp:182`; `GL fsr_pass.cpp:157` | Metal never reads the key | absent | missing feature | yes, with B12 | add the CAS step to the FSR pass |
| B15 | Debug overlay statistics text | `VKPresent.cpp:947`; `GLPresent.cpp:470` | no call to `set_debug_overlay_text` | empty overlay | missing feature | yes, for the debug overlay | fill the text in `MTLGSRender.cpp` |
| B16 | Shader compilation hint | `VKGSRender.cpp:2045-2054`; `GLGSRender.cpp:889` | Metal never reads the key | no hint | missing feature | no | show the hint on a pipeline build |
| B17 | Async pipeline compile | `VKGSRender.cpp:1395-1397` | Metal builds in `MTLShader.cpp` / `MTLDevice.cpp:157-188` | synchronous only | different behaviour | no, if compiler threads stay tunable | keep synchronous; wire A49 |
| B18 | Alpha window support | `GLPresent.cpp:397` (`has_alpha`) | `MTLMetalLayer.mm:128` sets `opaque = YES` | never transparent | capability mismatch | no; RPCS3 has no transparent-window video setting | none |
| B19 | HDR output | no VK/GL site | `MTLMetalLayer.mm:140` uses `kCGColorSpaceSRGB` only | sRGB 8-bit only | optional enhancement | no; no backend implements HDR | optional, see C9-C13 |
| B20 | Variable refresh rate | no VK/GL site | `MTLMetalLayer.mm:174` toggles display sync; `MTLPresenter.cpp:303` uses `presentAfterMinimumDuration` | sync off gives an uncapped present | optional enhancement | no | optional; a frame-limit policy is the real control |
| B21 | Occlusion query path | `VKGSRender.cpp:1483` region | `MTLZCULL.cpp:31-51` uses a shared visibility buffer | works | not a deviation | no | none |
| B22 | MSAA hardware | `VKGSRender.cpp:767` (`supports_hw_msaa`) | `MTLDevice.cpp:291` (`true`) | works | not a deviation | no | none |

---

## 4. Table C — KosmicKrisp and Metal-4-only techniques

`| id | KK file:line | what KK does | valid for RSX? | why | how we would apply it |`

| id | KK file:line | what KK does | valid for RSX? | why | how we would apply it |
|----|--------------|--------------|----------------|-----|-----------------------|
| C1 | `.kk-mesa/src/kosmickrisp/vulkan/kk_device.c:303-306`; `kk_queue.c:290-291` | Creates one residency set per device and attaches it to the queue at queue creation | yes | The drawable textures must be resident before the GPU reads them | Already adopted: `MTLMetalLayer.mm:220-235` returns the layer residency set and the presenter adds it to the queue |
| C2 | `.kk-mesa/src/kosmickrisp/vulkan/kk_query_pool.c:198-202`; `kk_cmd_buffer.c:340-345`; `bridge/mtl_device.m:205-226`; `bridge/mtl_command_buffer.m:44-56` | Allocates a `MTL4CounterHeap` of type timestamp and resolves it into a buffer after the GPU writes it | yes | RSX exposes no timestamp query to a game, but RPCS3's own overlay can use it for GPU time | Add a small counter heap and a resolve step behind the debug overlay (A17) |
| C3 | `.kk-mesa/src/kosmickrisp/vulkan/kk_cmd_buffer.c:282-291`; `bridge/mtl_argument_table.m:16-46` | Binds all resources through one `MTL4ArgumentTable` per stage and marks it once per encoder | yes | It cuts per-draw bind cost | Already adopted: `MTLCommandRecorder.cpp:45-55` builds one vertex table and one fragment table per slot |
| C4 | `.kk-mesa/src/kosmickrisp/vulkan/kk_device.c:34-73`; `bridge/mtl_compiler.m:16-65` | Caches one `MTL4Compiler` per device under a lock, because concurrent compilers corrupt the heap | yes | The same macOS 26 hazard applies | Already adopted: `MTLDevice.cpp:179-188` creates one compiler per device (workaround W11), and `MTLDevice.cpp:157-171` adds the pipeline data set serializer |
| C5 | `bridge/mtl_heap.m:19-27` | Allocates a placement heap and sets `sparsePageSize = MTLSparsePageSize16` even though it does not use sparse textures | yes | The page size gives a predictable heap granularity | `MTLResourceManager.cpp:42-49` already uses `HeapTypePlacement`; add the page size |
| C6 | `kk_cmd_buffer.c:280-287`; `bridge/mtl_compiler.m:177-183` | Sets a vertex amplification count for multiview rendering in one pass | no | RSX stereo uses two separate display buffers and frame packing (`MTLGSRender.cpp:2997`), not multiview | none |
| C7 | `.kk-mesa/src/kosmickrisp/vulkan/kk_cmd_draw.c:1062,1653,1698` | Handles tessellation and unrolling pipeline paths | no | RSX has no tessellation stage | none |
| C8 | `kk_image.c:286-290` | Rejects `VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT` and states "Don't support sparse residency" | no | KK itself refuses sparse residency, so it is not a proven technique | none |
| C9 | `.kk-mesa/src/vulkan/wsi/wsi_common_metal_layer.m:175-177` | Sets `wantsExtendedDynamicRangeContent` for float formats | yes | It is the standard layer switch for an extended-range drawable | Optional: add an HDR output mode that sets the flag and picks a float layer format |
| C10 | `wsi_common_metal_layer.m:72-130` | Maps a colour space to a `CGColorSpace` (sRGB, Display P3, BT.2020, DCI-P3) | yes | It is a plain layer property | Optional: expose a colour-space choice next to the output mode |
| C11 | `wsi_common_metal_layer.m:255-263` | Builds `CAEDRMetadata` HDR10 metadata from display and content info | yes | The layer accepts it directly | Optional: use it with C9 when the source is HDR |
| C12 | `wsi_common_metal_layer.m:373-385` | Adds and removes the layer residency set on the Metal 4 queue | yes | It is the WSI form of C1 | Already adopted in the presenter queue setup |
| C13 | `wsi_common_metal_layer.m:56-62` | Supports `RGBA16Float`, `RGB10A2Unorm` and `BGR10A2Unorm` layer formats | yes | A 10-bit or float drawable is required for HDR | Optional: use with C9 |
| C14 | `wsi_common_metal_layer.m:167` | Sets `allowsNextDrawableTimeout = YES` so acquire does not stall | yes | A nil drawable is a dropped frame, not a stall | Already adopted: `MTLMetalLayer.mm:135` |

---

## 5. Silently ignored settings

These keys are read by Vulkan or OpenGL but never read by Metal. The user sees the
control and it does nothing.

1. `output_scaling` and `rcas_sharpening_intensity` (A57, A61). The output scaling
   combo and the sharpening slider are visible for Metal
   (`settings_dialog.cpp:599`, `settings_dialog.cpp:897-902`). Metal always scales
   bilinearly (`MTLPresent.cpp:88-97`). This is the most visible ignore.
2. `disable_msl_fast_math` (A55). The setting exists for Apple GPUs
   (`tooltips.h:57`), the checkbox is shown on macOS (`settings_dialog.cpp:1500-1503`),
   and the Metal backend never reads it. Only MoltenVK reads it
   (`instance.cpp:185`).
3. `shadermode` (A7). "Interpreter only" still produces recompiled shaders. Metal
   has no interpreter.
4. `shader_compiler_threads_count` (A49). Metal uses `utils::get_thread_count()`
   at `rsx_cache.h:280-281`.
5. `host_label_synchronization` (A54). The capability is forced false at
   `MTLDevice.cpp:314`, and the setting is never read.
6. `disable_vertex_cache` (A26). Metal has no vertex cache, so the workaround is
   always on.
7. `force_high_precision_z_buffer` (A21). Metal is always high precision.
8. `disable_hardware_texel_remapping` (A60). Metal has no hardware remap path.
9. `log_programs`, `debug_output`, `renderdoc_compatiblity`,
   `show_shader_compilation_hint` (A15, A16, A18, A63). Developer-facing, low impact.
10. `force_cpu_blit_processing`, `disable_blit_engine_upscaling` (A31, A62). Both
    are no-ops, because no GPU blit path exists.

The self-declared list at `MTLDevice.cpp:79-113` is not a reliable source for this
section. Six of its ten entries are false positives (see Table A notes). Also, the
warning only runs under `MTL_DIAGNOSTICS` (`MTLDevice.cpp:194-199`), so a normal
build prints nothing.

---

## 6. Missing features, ranked by user-visible impact

1. **Output scaling mode (A57, B12, B13).** Highest impact. The user selects FSR or
   nearest, and Metal renders bilinear. This affects every game and every frame.
   The nearest pass even exists (`MTLPresent.cpp:31`) and is never run.
2. **RCAS sharpening (A61, B14).** Part of the same control group. The slider is
   shown and does nothing.
3. **Disable MSL fast math (A55).** Apple-only control. A user with fast-math
   artifacts has no fix on the Metal backend.
4. **Shader mode (A7).** A user with a shader stall cannot fall back to the
   interpreter. This is a compatibility feature, not a performance feature.
5. **Debug overlay text (A17, B15).** The overlay opens and stays empty. A tester
   cannot read the RSX statistics.
6. **Shader compiler threads (A49).** A user cannot tune compile workers.
7. **Host GPU labels (A54, B8).** A sync workaround is unavailable.
8. **Vertex cache (A26, B10).** A CPU-cost feature is missing. The workaround
   setting cannot be used because the feature is absent.
9. **GPU blit (A31, A62, B11).** A performance and parity gap. `force_cpu_blit_processing`
   and `disable_blit_engine_upscaling` both do nothing.
10. **Last provoking vertex (B5).** A correctness gap for a small number of games.
11. **Shader compilation hint, program log, debug output, RenderDoc (A63, A15, A16, A18).**
    Developer-facing. Low impact.

---

## 7. Metal-only techniques worth adopting

1. **Counter-heap GPU timestamps (C2).** Adopt with the debug overlay fix (A17).
   A `MTL4CounterHeap` of type timestamp gives a real GPU time for the overlay.
2. **Heap placement with a sparse page size (C5).** Adopt in
   `MTLResourceManager.cpp:42-49`. It gives a predictable heap granularity at no
   correctness risk.
3. **HDR/EDR layer plumbing (C9, C10, C11, C13).** Adopt as an optional output
   mode. It needs a colour-space choice and a float or 10-bit layer format. It
   also needs an SDR-to-HDR expansion step in the calibration shader, because the
   RSX output is SDR. Parity is not affected, because no other backend implements
   HDR.
4. **MetalFX spatial upscaling.** Not used by KK, so it is not in Table C. It is a
   valid optional alternative to the missing FSR pass (B12). It operates on the
   final colour texture, the same input that FSR uses. Adopt only after the FSR
   parity work, as an extra option. The output pass list at
   `MTLPresent.cpp:27-38` is where a new scaler would attach.

Already adopted, listed here for completeness: residency sets (C1, C12),
argument tables (C3), one compiler per device (C4), and the drawable acquire
timeout (C14).

---

## 8. Explicitly rejected ideas

1. **Ray tracing.** No RSX stage maps to it. KosmicKrisp has no ray-tracing path
   in `.kk-mesa/src/kosmickrisp/`. Reject.
2. **Frame interpolation (MetalFX).** It needs motion vectors and a second frame.
   RSX provides neither, and a game's presented image has no such data. It also
   adds input latency. Reject.
3. **Temporal upscaling (MetalFX).** It needs motion vectors, a jitter sequence
   and a history buffer. RSX exposes none of these. Reject.
4. **Sparse residency (C8).** KosmicKrisp itself rejects it at
   `kk_image.c:286-290`. Reject.
5. **Multiview / vertex amplification for stereo (C6).** RSX stereo uses two
   display buffers and frame packing (`MTLGSRender.cpp:2997`). Reject.
6. **Tessellation (C7).** RSX has no tessellation stage. Reject.
7. **A transparent window (B18).** RPCS3 has no transparent-window video setting.
   The Metal layer is opaque by design (`MTLMetalLayer.mm:128`). Reject.
8. **A Metal-only "variable refresh rate" control (B20).** `CAMetalLayer` exposes
   no public VRR range. `displaySyncEnabled` plus a frame limit is the correct
   control. Reject a new setting.

---

## 9. Fifteen-line summary

1. Metal honours most video settings through shared code, and the self-declared ignored list at `MTLDevice.cpp:79-113` over-reports by six entries.
2. The largest deviation is `output_scaling`: Metal never reads it and always scales bilinearly (`MTLPresent.cpp:88-97`).
3. The `nearest` upscaler is built at `MTLPresent.cpp:31` and never run; FSR and RCAS are absent, and `rcas_sharpening_intensity` is never read.
4. `disable_msl_fast_math` is Apple-specific, is shown in the GUI, and is never read by the Metal backend.
5. `shadermode` is never read, so Metal has no interpreter fallback for a shader stall.
6. `shader_compiler_threads_count` is never read; `rsx_cache.h:280-281` uses all cores instead.
7. `host_label_synchronization` is never read, and the capability is forced false at `MTLDevice.cpp:314`.
8. `disable_vertex_cache` has no effect, because Metal uploads vertex data per draw.
9. `force_high_precision_z_buffer` has no effect, because Metal depth is always `Depth32Float` (`MTLFormats.cpp:82`).
10. The debug overlay is created but never given text, so the RSX statistics pane is blank.
11. No GPU blit path exists (`MTLGSRender.cpp:3291` returns false), so two blit settings are no-ops.
12. Missing features rank as: output scaling, RCAS, MSL fast math, shader mode, debug overlay, compiler threads, host labels, vertex cache, GPU blit, provoking vertex.
13. KosmicKrisp techniques worth adopting are counter-heap timestamps, heap placement page size, and the HDR/EDR layer plumbing.
14. Residency sets, argument tables and the single-compiler-per-device pattern are already adopted.
15. Rejected: ray tracing, frame interpolation, temporal upscaling, sparse residency, multiview stereo, tessellation, transparent window, and a separate VRR control.

<!-- doc-end -->
