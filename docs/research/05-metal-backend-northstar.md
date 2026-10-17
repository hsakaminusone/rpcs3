# 05 — Northstar: a native Apple Metal backend

Goal: a first-class **Metal 4** RSX backend that replaces the Vulkan backend **+ MoltenVK/KosmicKrisp** on macOS.

## Is it feasible?

Yes, and the codebase is friendly to it, but it is a **large multi-person, multi-year** effort — roughly the size of the existing Vulkan backend. The RSX *emulation* (the hard part of an emulator) is shared and reusable; only the GPU-resource layer must be rewritten.

## 1. Shape of the change

There is **no `IGraphicsBackend`**. A backend derives from `rsx::thread` (usually via `GSRender`). Today: `VKGSRender`, `GLGSRender`, `NullGSRender`. You would add:

- `rpcs3/Emu/RSX/MTL/` (new subsystem, ~**25–30k LOC**) with `MTLGSRender` + a `mtl::` RAII layer analogous to `vkutils/`.
- `video_renderer::metal` enum member + config + UI entry + instantiation in `gui_application.cpp:787`.
- `mtl_gs_frame` producing a `CAMetalLayer`-bearing view (reuse `vkutils/metal_layer.mm`'s `GetCAMetalLayerFromMetalView`).

### Contract to implement (`rsx::thread`, `Emu/RSX/RSXThread.h`)
Must: `get_cycles`, `get_display_refresh_rate`, `on_init_thread`, `flip`.
Also: `begin`, `end`, `execute_nop_draw`, `emit_geometry`, `clear_surface`, `on_task`, `on_exit`, `do_local_task`, `on_frame_end`, `renderctl`, `timestamp`, `write_barrier`, `sync_hint`, `release_GCM_label`, `on_access_violation`, `on_invalidate_memory_range`, `notify_tile_unbound`, `on_semaphore_acquire_wait`, `begin/end_conditional_rendering`, `scaled_image_from_memory`, `get_programs`, `is_current_program_interpreted`, `patch_transform_constants`, plus the `ZCULL_control` occlusion API and the `backend_configuration` capability struct. Draws enter only through `RSX(ctx)->begin()/end()` (`NV47/HW/nv4097.cpp`).

## 2. Reusable (≈55k LOC) vs new (≈30k LOC)

**Reusable unchanged:** `Core/`, `NV47/`, `rsx_methods.cpp`, `RSXFIFO`, `RSXOffload`, `RSXZCULL`, `GSRender.cpp`, `Utils/`, `Host/`.
**Reusable with a new traits struct:** `Common/texture_cache.h`, `Common/surface_store.h`, `Common/surface_utils.h`, `Program/ProgramStateCache.h`, `Common/ring_buffer_helper.h`, `Common/BufferUtils.cpp`, `TextureUtils.*`.
**Reusable:** overlay UI (`Overlays/`, `compiled_resource`) — only the `overlay_pass` renderer is backend work.

**New (mirror of `VK/`, ~33k LOC):** device/queue/present, resource manager, texture-cache backing, surface/render-target backing, render-pass-equivalent encoding, pipeline state objects, argument-table binding, command stream, compute tasks (deswizzle/copy/mipgen/resolve), async scheduler, query pools, memory allocator, format translation, upscalers, overlays renderer.

The existing isolation (virtual interface + traits templates + `backend_configuration`) means the new backend plugs in **without touching the shared 55k**.

## 3. Shader strategy (the make-or-break decision)

Today: `RSX ucode → Vertex/FragmentProgramDecompiler → GLSL → glslang → SPIR-V → VkShaderModule`. There is **no MSL codegen** in RSX; `glslang` is vendored (`3rdparty/glslang`).

Options:
- **(A) Reuse GLSL decompilers → SPIR-V → SPIRV-Cross `CompilerMSL` → `xcrun metal` → `metallib`.** Cheapest correct path; essentially MoltenVK's shader stage without the Vulkan API. Needs SPIRV-Cross added (`3rdparty`; Apache-2.0). Risk: dual compiler and MSL limits (argument buffers, dynamic array lengths, stage-in/out structs).
- **(B) Emit MSL directly from the decompilers.** Cleaner/faster compile, avoids SPIRV-Cross, but is a **second full decompiler target** with independent quirk tracking. High effort, high divergence risk.
- **(C) Reuse `Program/Assembler` + `GLSLInterpreter`** to build a Metal codegen/interpreter.

**Recommendation:** start with **(A)**. Note Metal 4's MSL is C++17-based / MSL 4.x — generated MSL must target `metal4.x`. Metal 4 changed the *command* model, not the shader language.

> Counterpoint from the KK team (Khronos/XDC slides): *"SPIRV-cross → MSL (used by MoltenVK) isn't a compiler stack"* — that is precisely why KK uses **Mesa NIR** instead. For a from-scratch backend, SPIRV-Cross is still the pragmatic starting point, but expect to outgrow it.

## 4. Memory model (a strength, not a weakness)

Apple Silicon is unified, so Metal maps better than Vulkan here:
- Guest memory → buffers: `newBufferWithBytesNoCopy:...options:MTLStorageModeShared` (no-copy). RPCS3's `supports_passthrough_dma` fast path is **off under MoltenVK today**, so there is headroom.
- Ring/heap for streaming: shared `MTLBuffer`, persistently mapped (`contents`); coherent by construction.
- Device-local: `MTLStorageModePrivate` + `MTLHeap`.
- Replace VMA with a small `MTLHeap` sub-allocator.

Gaps vs Vulkan:
- **No `VkRenderPass`/`VkFramebuffer`** → `MTLRenderPassDescriptor` + per-encoder `loadAction`/`storeAction`.
- **Descriptors → `MTL4ArgumentTable` / argument buffers**, replacing descriptor sets + pipeline layouts.
- **Sync.** `MTLSharedEvent`/`MTLFence` + Metal 4 **next-gen barriers** replace semaphores/fences.

## 5. Metal 4 specifics (macOS 26+; so also macOS 27)

See **doc 07** for the full API map. The pieces that matter most: `MTL4CommandQueue`/`MTL4CommandBuffer`/`MTL4CommandAllocator` (reusable, thread-submittable), `MTL4RenderCommandEncoder` (attachment map), `MTL4ComputeCommandEncoder` (blit+compute merged), `MTL4ArgumentTable`, **residency sets**, **next-gen barriers**, placement-sparse resources, `MTL4Compiler`. Targeting Metal 4 + macOS 27 = **Apple Silicon only** (consistent with dropping Intel).

## 6. Constraint surface you inherit (see doc 07)

- Vertex stride / attribute offsets **must be multiples of 4**; no vertex-buffer robustness ⇒ likely need **vertex pulling** (Cemu: "no easy solution").
- TBDR (tile memory) changes MSAA/load-store economics vs the VK path.
- **MSL compiler / Metal runtime bugs**: KK documents 18 workarounds (coherency model violations, early-discard with side effects, subgroup ballot/simd bugs, `MTL4Compiler` concurrency heap corruption, index-buffer/manual robustness, M5 OOB LOD differences, Rosetta heap-size mismatch). A native backend hits the same bugs.

## 7. What prior art says it is worth (doc 08)

- **Dolphin**: native Metal (TellowKrinkle) beats a healthy MoltenVK only **a little**; up to **+23–30%** in GPU-heavy titles. Its real side benefit was **exposing a 45% MoltenVK regression**.
- **FNA**: Metal backend is hard to maintain with *one* person; *"not entirely convinced the delta is significant"*; sometimes MVK is faster.
- **Cemu**: vertex-stride constraint is a genuine blocker.
- **RPCS3 upstream (2022)**: explicitly *not* asking for native Metal.

## 8. Difficulty estimate

Order-of-magnitude, not a schedule:

| Milestone | Scope | Rough effort |
|---|---|---|
| Bring-up | device/queue/swapchain, clear + present, overlay UI | weeks–low months |
| MVP | VP/FP pipeline, textures, vertex upload, basic draws, a few bootable titles | ~6–12 person-months |
| Playable subset | render targets, MSAA resolve, texture-cache correctness, format/deswizzle compute | +12–24 person-months |
| Parity with VK backend | all RSX quirks, async compute, ZCULL, savestate, FSR/upscalers, edge formats | **multi-person-year**; effectively a second `VK/` |

**Biggest risks**: (1) the RSX **per-title quirk surface**; (2) **shader correctness** via SPIR-V→MSL; (3) the **render-pass→encoder** redesign; (4) **TBDR performance** tuning; (5) **long-term maintenance** by few people.

## 9. Where this leaves KosmicKrisp

KK already *is* a Vulkan→Metal driver and absorbs all the constraint work above. A native Metal backend removes a layer (no Vulkan API/loader/ICD; possibly no runtime SPIRV-Cross with option B) and enables RSX-specific Metal 4 features, but its measured upside over a healthy KK/MVK is **modest**, while its cost is **large**.

**Recommendation:** ship MVK+KK selection first (doc 04) — it is cheap, makes the Vulkan path conformant and Metal-4-native, and delivers most of the practical benefit. Treat the native backend as a **research northstar** with a de-risking spike (window + present + overlay, then one draw) before any parity commitment.

## 10. Suggested de-risking path

1. Ship MVK+KK selection (doc 04).
2. Research spike: minimal `MTLGSRender` that opens a window and presents the overlay (no RSX draw) — proves device/swapchain/`CAMetalLayer` + MSL build.
3. Add the Option (A) shader path and one simple draw; measure against KK and MVK.
4. Decide Option (A) vs (B) with data; decide go/no-go on parity.

_Status: analysis only._
