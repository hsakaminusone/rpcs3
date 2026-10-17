# 03 — RPCS3 graphics backend architecture

All paths relative to repo root. Scale measured on branch `kosmickrisp-integration`.

## Code scale

| Area | LOC | Files |
|---|---|---|
| `rpcs3/Emu/RSX/VK` (Vulkan backend) | **33,089** | 111 |
| `rpcs3/Emu/RSX/GL` (OpenGL backend) | 18,193 | 66 |
| `rpcs3/Emu/RSX/Common` (shared) | 17,164 | 34 |
| `rpcs3/Emu/RSX/Overlays` (shared UI) | 20,151 | 98 |
| `rpcs3/Emu/RSX/Program` (shader decompilers) | 10,719 | 34 |
| `rpcs3/Emu/RSX/NV47` (RSX method/FIFO decode) | 3,246 | 23 |
| `rpcs3/Emu/RSX/Core` (state/command) | ~1,874 | — |
| `rpcs3/Emu/RSX/Null` (null backend) | 31 | — |

Shared, backend-agnostic RSX code ≈ **55,000 LOC** (Common + Program + Overlays + Core + NV47 + Utils + Host).

## There is no `IGraphicsBackend` interface

A backend is a class deriving from **`rsx::thread`** (`Emu/RSX/RSXThread.h`) — which itself inherits `cpu_thread`, `GCM_context`, `GRAPH_backend` — and usually via **`GSRender`** (`Emu/RSX/GSRender.h`).

### Virtual contract to implement (`rsx::thread`, RSXThread.h)
- **Pure / must override:** `get_cycles()`, `get_display_refresh_rate()`, `on_init_thread()`, `flip()`.
- **Render loop:** `begin()`, `end()`, `execute_nop_draw()`, `emit_geometry()`, `clear_surface()`.
- **Lifecycle/local:** `on_task()`, `on_exit()`, `do_local_task()`, `on_frame_end()`, `renderctl()`, `timestamp()`.
- **Sync/memory:** `write_barrier()`, `sync_hint()`, `release_GCM_label()`, `on_access_violation()`, `on_invalidate_memory_range()`, `notify_tile_unbound()`, `on_semaphore_acquire_wait()`.
- **Conditional render:** `begin_conditional_rendering()`, `end_conditional_rendering()`.
- **Misc:** `scaled_image_from_memory()`, `get_programs()`, `is_current_program_interpreted()`.
- **Capability table:** `backend_configuration backend_config` — backend advertises features (multidraw, hw A2C, MSAA, conditional render, instancing, passthrough DMA, async compute, host GPU labels, normalized barycentrics, last provoking vertex, programmable blending, extended depth range, framebufferless rendering). VK fills ~20 flags in `VKGSRender.cpp:653-816`.
- **ZCULL:** `rsx::reports::ZCULL_control` (`RSXZCULL.h:211`) — occlusion query lifecycle.
- **Transform constants:** `rsx::GRAPH_backend::patch_transform_constants()` (`NV47/FW/GRAPH_backend.h`, 19 lines, "TODO: Flesh this out").
- **Window/present:** `GSFrameBase` (`Emu/RSX/GSFrameBase.h`) is implemented by the *app* (`rpcs3qt/gs_frame.cpp`, `gl_gs_frame.cpp`) — a Metal backend needs an equivalent producing a `CAMetalLayer` view.

**Minimal backend reference:** `NullGSRender` (`Emu/RSX/Null/NullGSRender.h`, 14 lines) overrides only `get_cycles` + `end`.
**Full backend reference:** `VKGSRender` overrides ~25 methods (`VK/VKGSRender.h:210-313`).

### Entry points / selection
- `nv4097.cpp` (`NV47/HW/nv4097.cpp`) calls `RSX(ctx)->begin()` / `->end()` — the draw dispatch enters the backend **only** through these virtuals.
- Backend instantiation: `rpcs3qt/gui_application.cpp:787-811` (`init_gs_render` switch on `g_cfg.video.renderer`) → `g_fxo->init<rsx::thread, named_thread<{Null,GL,VK}GSRender>>(ar)`.
- Renderer enum: `Emu/system_config_types.h:44` `enum class video_renderer { null, opengl, vulkan }`.
- Config: `Emu/system_config.h:118-126` `cfg::_enum<video_renderer> renderer`.
- UI: `rpcs3qt/settings_dialog.cpp:388-396`, `rpcs3qt/emu_settings.cpp:1046-1050`.

## VK backend subsystem inventory (`Emu/RSX/VK/`)

| File / class | Responsibility |
|---|---|
| `VKGSRender` | Root: instance/device/swapchain, heaps, program cache, frame ring, capability detection, VRAM pressure recovery |
| `VKDraw.cpp` | begin/end/emit_geometry, render-pass open/close, draw state, texture env, vertex upload |
| `VKPresent.cpp` | flip/present, swapchain reinit, frame advance, screenshots |
| `VKVertexBuffers.cpp` | Vertex stream analysis/upload |
| `VKTextureCache.{h,cpp}`, `VKTexture.{h,cpp}` | Texture upload + RSX texture cache (`rsx::texture_cache` specialization) |
| `VKRenderTargets.{h,cpp}` | Render targets, `vk::surface_cache : rsx::surface_store`, MSAA resolve, spilling |
| `VKRenderPass.{h,cpp}`, `VKFramebuffer.{h,cpp}` | Render pass + framebuffer caching |
| `VKProgramPipeline.{h,cpp}` | GLSL→SPIR-V compile, descriptors, pipeline layout, bind |
| `VKVertexProgram.{h,cpp}`, `VKFragmentProgram.{h,cpp}` | VP/FP decompile + GLSL emit + `VkShaderModule` |
| `VKPipelineCompiler.{h,cpp}` | Async pipeline build, state hashing |
| `VKShaderInterpreter.{h,cpp}` | Precompiled interpreter path (async shader modes) |
| `VKCommandStream.{h,cpp}` | Command buffer chain/chunks + submits |
| `VKCompute.{h,cpp}`, `VKAsyncScheduler.{h,cpp}`, `VKResolveHelper.{h,cpp}` | Compute deswizzle/copy/mipgen/resolve, async queue scheduling |
| `VKQueryPool.{h,cpp}` | Occlusion query pools |
| `VKDataHeapManager.{h,cpp}` | Ring/heap savestate + flush |
| `VKDMA.{h,cpp}` | RSX DMA engine; `dma_block_EXT` uses host-pointer import |
| `VKFormats.{h,cpp}` | Format / sampler / topology translation |
| `VKOverlays.{h,cpp}` | Renderer for shared overlay UI |
| `vkutils/*` (**8,977 LOC**) | Vulkan RAII/device layer (device, instance, memory, data_heap, buffer, image, descriptors, sampler, sync, commands, swapchain*, pipeline state, chip_class, …) |
| `upscalers/*` (577) | Nearest / Bilinear / FSR1 present scalers |

## Reusable for a Metal backend

**Fully reusable (no GPU API):** `Core/` (1,874 — draw command processor, display, context, tiles, ZCULL), `NV47/` (3,246 — RSX method decode, FIFO; `nv4097.cpp` draw dispatcher), `rsx_methods.cpp`, `rsx_decode.h`, `RSXFIFO.cpp`, `RSXOffload.cpp`, `RSXZCULL.cpp`, `GSRender.cpp`, `Utils/` (1,417), `Host/` (480 — `RSXDMAWriter`, MM).

**Templated on backend traits (reuse with a new traits struct):**
- `Common/surface_store.h` — `rsx::surface_store<Traits>` ← `vk::surface_cache_traits`
- `Common/texture_cache.h` (+ utils/predictor) — `rsx::texture_cache<Derived,Traits>`
- `Common/surface_utils.h` — `rsx::render_target_descriptor<ImageType>`
- `Program/ProgramStateCache.h` — `program_state_cache<backend_traits>` ← `VKTraits` / `GLTraits`
- `Common/ring_buffer_helper.h` — `rsx::data_heap` base
- `Common/BufferUtils.cpp`, `TextureUtils.*`, `tiled_dma_copy.hpp`, `surface_cache_dma.hpp`

**Backend-specific (must be reimplemented):** everything in `VK/` except pure format helpers; `GL/`. Overlay *UI* is shared (`compiled_resource`); only per-backend `overlay_pass` renders it.

## Shader pipeline

`RSX ucode → decompiler → GLSL text → glslang → SPIR-V → VkShaderModule → pipeline`

- Inputs: `Program/RSXVertexProgram.h`, `Program/RSXFragmentProgram.h`; analysis in `Program/ProgramStateCache.cpp`.
- Decompiler bases with virtual GLSL hooks: `Program/VertexProgramDecompiler.{h,cpp}`, `Program/FragmentProgramDecompiler.{h,cpp}`.
- Emit helpers: `Program/GLSLCommon.{h,cpp}`, `GLSLTypes.h`, `ShaderParam.h`, `GLSLSnippets/` (~3,297 incl. `RSXProg/`), `Program/Assembler/` (FPASM→CFG IR), `GLSLInterpreter/`, `MSAA/`, `Upscalers/`.
- Backend subclasses (the natural extension point): `VK/VKVertexProgram.cpp`, `VK/VKFragmentProgram.cpp`; `GL/GLVertexProgram.cpp`, `GL/GLFragmentProgram.cpp`.
- GLSL→SPIR-V: `Program/SPIRVCommon.cpp` (`spirv::compile_glsl_to_spv`, glslang).
- VK objects: `vk::glsl::shader` / `vk::program` in `VK/VKProgramPipeline.cpp`; pipelines in `VK/VKPipelineCompiler.cpp`; cache in `VK/VKProgramBuffer.h`.
- **No MSL codegen exists anywhere in RSX today.**

## Memory model (VK → Metal mapping)

| Vulkan (RPCS3) | Metal equivalent | Notes |
|---|---|---|
| `vk::memory_block_host` (`VK_EXT_external_memory_host`, `vkutils/memory.cpp:472`) | `newBufferWithBytesNoCopy:...options:MTLStorageModeShared` | Apple has no external-host-ptr import; the `supports_passthrough_dma` fast path is **off under MVK today**; maps well on unified memory |
| `device_local` | `MTLStorageModePrivate` + `MTLHeap` | |
| `device_bar` | `MTLStorageModeShared` | |
| `host_visible_coherent` ring (`vk::data_heap`) | persistently-mapped shared `MTLBuffer` | coherent by construction |
| `mem_allocator_vma` / `mem_allocator_vk` | `MTLHeap` suballocation + manual allocator | no VMA equivalent |
| `VkBuffer`/`VkImage`/`VkImageView` | `MTLBuffer`/`MTLTexture`/`MTLTextureView` | |
| descriptor sets/pools | `MTLArgumentEncoder` / argument tables / bindless | needs restructuring |
| `VkRenderPass`/`VkFramebuffer` | **no direct equivalent** — `MTLRenderPassDescriptor` + `loadAction`/`storeAction` per encoder | biggest conceptual gap |
| `VkSemaphore`/`VkFence`/events | `MTLSharedEvent` / `MTLFence` | |
| Swapchain (`vkutils/swapchain_macos.hpp`) | `CAMetalLayer` + `nextDrawable` | `metal_layer.mm` already extracts the layer |
| Async compute queue | `MTLCommandQueue` + concurrent/parallel encoders | scheduler logic reusable |

Notes: guest memory backing uses `rsx::io_buffer` (API-neutral, `Common/io_buffer.h`). GPU→CPU label writes use `vkCmdUpdateBuffer` (`VKGSRender::map_host_object_data`).

## Existing isolation mechanisms

1. Virtual interface on `rsx::thread` + `ZCULL_control` + `GRAPH_backend`.
2. `backend_configuration` capability struct.
3. Templates on traits: `program_state_cache<>`, `texture_cache<>`, `surface_store<>`, `render_target_descriptor<>`, `data_heap`.
4. Abstract decompiler bases with virtual GLSL hooks.
5. API-neutral data types: `rsx::io_buffer`, `GCM_context`, `host_gpu_context_t`.
6. Overlay UI `compiled_resource`.
7. Runtime backend selection in `gui_application.cpp:787`.

`#ifdef` usage inside RSX: `_WIN32` 43, `__APPLE__` 31, `HAVE_X11` 15, `HAVE_VULKAN` 2. Apple-specific code already exists in `VK/vkutils/{instance,device,swapchain_macos,metal_layer,chip_class}`, `VK/VulkanAPI.h`.
