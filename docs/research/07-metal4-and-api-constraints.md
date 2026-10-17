# 07 — Metal 4 API, and Metal/MSL constraints that matter to an emulator

Reference notes gathered for the native-Metal northstar (and useful even for the KK integration, since KK targets Metal 4).

## Metal 4 core API (macOS 26+)

Source: Apple "Understanding the Metal 4 core API" (+ WWDC25 sessions 205 "Discover Metal 4", 254 "Explore Metal 4 games").

Metal 4 is a **new generation** of the API. New `MTL4*` types are independent of the old `MTL*` types:

| Metal 4 | Replaces / role |
|---|---|
| `MTL4CommandQueue` | `MTLCommandQueue`; commit is the submission point; **submit from any thread** |
| `MTL4CommandBuffer` | `MTLCommandBuffer`; created from the **device**, **reusable** via `beginCommandBuffer(allocator:)` |
| `MTL4CommandAllocator` | explicit control of command-buffer memory |
| `MTL4CommandEncoder` | base protocol for work-specific encoders |
| `MTL4ComputeCommandEncoder` | merges blit + compute + acceleration-structure encoders |
| `MTL4RenderCommandEncoder` | render encoder with an **attachment map** (logical shader outputs → physical color attachments) |
| `MTL4ArgumentTable` | binding table for buffers/textures/samplers; shareable across stages/encoders |
| `MTL4Compiler` | explicit shader compilation control |
| **residency sets** | declare resources to keep resident (set up at startup, cheap updates) |
| **next-generation barriers** | low-overhead, **stage-to-stage** synchronization |
| **texture view pools** | pool of texture views |
| **placement sparse resources** | fine-grained memory placement |

Other notes:
- Apple explicitly frames Metal 4 as making it *"easier to adapt your apps and games from other platforms, such as DirectX and Vulkan."*
- MSL is still the shader language; **Metal 4 uses MSL 4.x / `MTLLanguageVersion4_0`**, C++17-based, `API_AVAILABLE(macos(26.0))`.
- Command buffers decoupled from queues and encodable in parallel → good fit for RPCS3's threaded command generation.
- MetalFX performance features continue alongside (ML integration, `MTLTensor`).

## Hard Metal constraints that emulators hit

These are the "gotchas" other emulators (Dolphin, PCSX2, Cemu, RPCS3+MVK) have documented:

1. **Vertex buffer stride and attribute offsets must be multiples of 4.** Metal allows no arbitrary strides; Vulkan does. RSX (and GC/PS2/WiiU) games emit arbitrary strides.
   - Cemu #444: `bufferStride` of 1 → pipeline creation fails; dev: *"no easy solution."*
   - MoltenVK user survey (an emulator): *"lack of arbitrary vertex buffer strides on Metal… vertex buffer strides must be aligned to four bytes."*
   - **Common workaround: vertex pulling** (read the vertex buffer in the shader and index manually). Chromium/Dawn always use vertex pulling on Metal *because Metal has no vertex-buffer robustness* (security). "Dynamic stride for attribute buffers" exists from **Apple4 / Metal 3** (feature-set tables) but does not remove the 4-byte limit.
2. **No vertex-buffer robustness** ⇒ out-of-range reads can fault; emulators must clamp/vertex-pull.
3. **Buffer/constant alignment:** minimum constant-buffer offset alignment is **32 B** on Mac2/Metal3 (256 B on iOS Simulator) vs **4 B** on Apple1+/Metal4. Argument-buffer GPU tier matters for bindless.
4. **Max 8 color render targets per pass** (RSX uses ≤ 4 MRT — fine).
5. **No `VkRenderPass`/`VkFramebuffer` object** ⇒ per-encoder `MTLRenderPassDescriptor` with explicit `loadAction`/`storeAction`.
6. **TBDR (tile-based deferred rendering)** on Apple GPUs ⇒ tile memory, on-chip load/store, different MSAA resolve economics than the VK path assumes.
7. **MSL compiler / Metal runtime bugs** are a live maintenance cost — see the 18 KK workarounds in doc 01 (coherency, early-discard, subgroup ballots, `MTL4Compiler` concurrency heap corruption, robustness).

## What this implies for the two efforts

- **KK path (near-term):** KK already absorbs all of the above (that is exactly what its 18 workarounds + NIR→MSL backend are for). Integrating KK is far cheaper than re-discovering these issues.
- **Native Metal path (northstar):** you inherit this constraint list directly. Budget for a **robustness/vertex-pulling strategy**, a **render-pass→encoder mapping layer**, and a **"Metal/MSL bug workaround" layer** analogous to `KK_WORKAROUND_*`.

## Sources
- Understanding the Metal 4 core API: <https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api>
- WWDC25 205 "Discover Metal 4": <https://developer.apple.com/videos/play/wwdc2025/205>
- WWDC25 254 "Explore Metal 4 games": <https://developer.apple.com/videos/play/wwdc2025/254>
- Metal feature set tables: <https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf>
- Cemu #444 (vertex stride): <https://github.com/cemu-project/Cemu/issues/444>
- MoltenVK use survey #1085: <https://github.com/KhronosGroup/MoltenVK/issues/1085>
