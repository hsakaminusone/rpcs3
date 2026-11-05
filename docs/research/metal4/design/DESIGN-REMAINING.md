# DESIGN-REMAINING — the architecture of the open Metal 4 parity work

This document designs the work that is confirmed but not yet implemented. It does not repeat the
inventory: `METAL4-TODO.md` holds the rows, `DEVIATIONS-SURFACE.md`, `DEVIATIONS-DRAW.md`,
`DEVIATIONS-SYNC.md` and `DEVIATIONS-FEATURES.md` hold the evidence. This document decides the
architecture, the data structures, the integration points and the order.

The reference for every decision is Vulkan (`rpcs3/Emu/RSX/VK`) and OpenGL (`rpcs3/Emu/RSX/GL`).
The Metal path must translate the same semantics, not approximate them.

---

## 0. Principles

1. **One concept, one map.** A guest-to-host channel map is a property of the format. It is built
   once and reused by the clear, the layout and the sample path. No second table.
2. **The per-draw path stays allocation-free and branch-light.** Every new capability is a cached
   object keyed by its inputs. A capability that needs a new pipeline key adds a cache entry, not a
   per-draw build.
3. **Reuse the shared helpers.** `Utils/color_utils.hpp` already states the RSX clear and write-mask
   semantics that Vulkan consumes. The Metal clear consumes the same helpers.
4. **A Metal limit is met with a Metal primitive, not a workaround.** Metal has no scissored clear,
   no per-channel load action and no primitive-restart toggle. The scissored `fill_pass` is the
   Metal form of `vkCmdClearAttachments`; a pipeline write mask and a stencil-write state are the
   Metal forms of the missing load actions; index promotion is the Metal form of a disabled
   restart.
5. **Measure before an unproven change.** Rows marked `No` in an audit (SY7, SY8, SY11) need a GPU
   capture. They are designed here but not scheduled before the measurement.

---

## 1. Cluster A — surface format semantics (S7) and the format-aware clear (S4)

These two rows share one table. `DEVIATIONS-SURFACE.md` names the pair K1+K2 and says to adopt them
together.

### 1.1 The component-layout map (S7)

**Today.** `surface::create_color` sets the pixel format and stops. The base `texture` keeps the
default layout `{A, R, G, B}`, which `get_view` treats as identity. When a surface is sampled, the
guest remap composes with an identity layout, so the channels are wrong for the formats whose host
format is a single-channel or two-channel image.

**Decision.** Add one free function in `MTLFormats`:

```cpp
// The guest-to-host channel meaning of a surface colour format, in A-R-G-B order
// so it composes with the RSX remap (MTLHandles.cpp, get_view).  It states which
// host channel carries which guest channel; a guest channel the format does not
// store is forced to one or zero (color_utils.hpp:300).
std::array<MTL::TextureSwizzle, 4> get_surface_component_layout(rsx::surface_color_format format);
```

The values are the surface analogue of `get_texture_component_layout`, and they agree with
Vulkan's `get_compatible_surface_format` channel maps (`VKGSRender.cpp:37-110`):

| surface format | layout `{A, R, G, B}` | why |
|---|---|---|
| `b8` | `{One, Red, Red, Red}` | one red channel, guest reads it in every colour; `VK no_alpha` |
| `g8b8` | `{Green, Red, Green, Red}` | store `RG8`; guest `G8B8` is `r`, `b` | 
| `x32` | `{Red, Red, Red, Red}` | one float, replicated; `VK rrrr` |
| `x1r5g5b5_z1r5g5b5`, `x8r8g8b8_z8r8g8b8`, `x8b8g8r8_z8b8g8r8` | `{Zero, Red, Green, Blue}` | the X byte must read as zero alpha |
| `x1r5g5b5_o1r5g5b5`, `x8r8g8b8_o8r8g8b8`, `x8b8g8r8_o8b8g8r8` | `{One, Red, Green, Blue}` | the X byte must read as one alpha |
| `r5g6b5`, `a8r8g8b8`, `a8b8g8r8`, `w16z16y16x16`, `w32z32y32x32` | identity `{Alpha, Red, Green, Blue}` | the host format already carries the guest channels in order |

The identity rows are not a guess. The texture path already stores the same guest channels in the
same host formats with an identity layout, and its probe confirmed the order
(`MTLFormats.cpp:328-337`). The packed 16-bit host formats are chosen so the identity holds; only
the single-channel, two-channel and float-replicating formats need a map.

**Integration.** Call `set_native_component_layout(get_surface_component_layout(format))` in
`surface::create_color`, and copy the layout to `resolve_surface` in `surface::create` when a
resolve target exists. Nothing else changes: the layout reaches the sampler only through
`get_view`.

**Cost.** One switch at surface creation. Zero per-draw cost.

### 1.2 The clear-colour resolver (S4)

**Today.** `capture_surface_state` stores `clear_color` as `r/255, g/255, b/255, a/255`. The clear
then loads all four channels with that value. For `b8`, `g8b8`, `r5g6b5` and the `x1r5g5b5` family
the guest clear colour is a packed or renormalised value that the plain division does not produce,
so the surface clears to the wrong channels.

**Decision.** Move the transform into the latch. Add a free function in `MTLFormats`:

```cpp
// The colour a clear must load for this surface format, and the colour channels it
// actually writes.  It mirrors VKGSRender.cpp:1526-1578 and the shared helpers in
// Utils/color_utils.hpp.
struct resolved_clear
{
    color4f color;   // 0..1, in the host channel order of the format
    u32 mask;        // the effective RSX_GCM_CLEAR_* colour bits, or 0 for a NOP
    bool fast;       // true when a full MTLLoadActionClear can express it
};

resolved_clear resolve_surface_clear(rsx::surface_color_format format, u8 r, u8 g, u8 b, u8 a, u32 mask);
```

The body is the switch at `VKGSRender.cpp:1526-1578`: it calls `rsx::get_b8_clear_color`,
`get_g8b8_clear_color`, `get_rgb565_clear_color`, `get_a1rgb555_clear_color` (alpha 255 for the
`o` variant, 0 for the `z` variant) and `get_abgr8_clear_color`, reduces the mask with
`get_b8_clearmask`, `get_g8b8_r8g8_clearmask` and `get_abgr8_clearmask`, and leaves `x32`,
`w16z16y16x16` and `w32z32y32x32` with no colour clear at all.

`surface_state` gains `clear_colormask` and holds the transformed colour; `capture_surface_state`
fills both. `fast` is true when `mask` covers every channel the format stores and the clear covers
the attachment.

**Integration.** `framebuffer::open_pass` uses `state.clear_colormask` for the `LoadActionClear`
decision and `state.clear_color` for the value. `clear_surface` uses `resolved.fast` to choose the
load-action clear or the fill. `cache.on_write` uses `state.clear_colormask`, not the raw mask.

**Cost.** One switch at capture, once per clear. Zero per-draw cost.

---

## 2. Cluster B — the masked fill (S3, S5)

Metal cannot scissor-clear, cannot load-clear a subset of channels, and cannot load-clear a subset
of stencil bits. Vulkan expresses all three with `vkCmdClearAttachments` plus a `colormask`. The
Metal form is the existing full-screen `fill_pass`, extended with a colour write mask and a
stencil-write state. Rows S14 and K11 already record that the scissor is the region; this cluster
adds the channel and stencil dimension.

### 2.1 The fill request

Replace the two booleans of `fill_pass::fill` with a request that names every dimension:

```cpp
struct fill_request
{
    MTL::GPUAddress values;              // one fill_values block (MTLConstants.h)
    MTL::PixelFormat color_format;       // the pass colour format, or Invalid for a colourless pass
    u8 sample_count;
    u8 color_mask;                       // 4-bit R,G,B,A: which colour channels the fill writes
    bool write_depth;
    MTL::DepthStencilState* depth_stencil; // resolved by the caller (see 2.3)
    u8 stencil_reference;                // the value the stencil op stores
    bool write_stencil;
};

void fill(MTL4::ArgumentTable* table, MTL4::RenderCommandEncoder* encoder, const fill_request& request);
```

### 2.2 The colour write mask

`MTL::RenderPipelineColorAttachmentDescriptor::setWriteMask` carries the mask. The mask is a
pipeline property, not an encoder property, so it joins the pipeline key. The key becomes

```
(serialised) color_format | sample_count | color_mask(4 bits) | depth_write | stencil_write | colorless
```

The set of masks a title uses is tiny (the RSX writes all, RGB, RG, R or none), so the cache holds a
handful of objects. The fragment function is unchanged for a coloured pass; for a pass with no
colour attachment the pipeline declares no colour output and uses a `fragment void` entry (verify at
build that MSL accepts a void fragment; if not, use a colour output with a `color_format` of
`Invalid` and no attachment).

### 2.3 The partial stencil clear

Metal writes stencil through the depth-stencil object, not a shader output. A stencil clear is a
full-screen draw with:

- a `depth_stencil_state` whose front and back faces are `compare = always`, `zpass = replace`,
  `write_mask = stencil_mask`, and whose depth test is `always` so no fragment is rejected;
- the encoder's stencil reference set to the clear value.

The state is keyed by the write mask, so it is built through the existing `depth_stencil_cache`
(`MTLDepthStencil.h:94`). The caller (`clear_surface`) builds the state and passes it in the
request; `fill` binds it and sets `encoder->setStencilReferenceValue(request.stencil_reference)`.

This mirrors `vk::stencil_clear_pass` (`VKGSRender.cpp:1622-1638`) and applies only when
`stencil_mask() != 0xff`; a full-mask stencil clear stays `MTL::LoadActionClear` on the stencil
attachment.

### 2.4 The decision ladder in `clear_surface`

```
effective = resolve_surface_clear(...)          // Cluster A
write_color = effective.mask != 0
write_stencil = (mask & STENCIL_BIT) && stencil_mask() != 0xff

if effective.fast && clear_covers_attachment() && !write_stencil:
    open the pass with LoadActionClear for the colour and the depth-stencil as today
else:
    open the pass with no colour/depth clear
    set the scissor to the clear region
    fill(request) with effective.color, effective.mask, write_depth, write_stencil
```

The existing full-cover fast path is kept: it is the common case and costs no draw.

**Cost.** A partial clear is one full-screen triangle, as today. The only new cost is a pipeline
variant per write mask, built on first use. A full clear is unchanged.

**Tests.** `MTLDialSpine`-style unit tests for `resolve_surface_clear` (one case per format) and for
the write-mask key. The render result needs a run.

---

## 3. Cluster C — the depth read-back (S9)

**Today.** `surface::write_to_memory` blits the depth texture straight into the guest buffer. For a
combined `Depth32Float_Stencil8` it names the depth plane; for the other host formats it copies the
host plane, so a `z16` pack gets four bytes per texel in a two-byte row and a `z24s8` pack loses the
stencil. Metal forbids a host copy of a combined depth-stencil texture (K9), so the plane must go
through a blit option into staging first.

**Decision.** Make the read-back the exact inverse of `load_background`
(`MTLSurface.cpp:355-395`), which already decodes with a blit option:

1. Blit the depth plane into a `StorageModeShared` staging buffer with
   `get_depth_plane_option(format, false)` (one `f32` per texel).
2. When the format is combined, blit the stencil plane into a second staging buffer with
   `get_depth_plane_option(format, true)` (one byte per texel).
3. On the CPU, run `encode_depth_plane(out, depth_staging, width, rows, rsx_pitch, bpp, format,
   stencil_staging, width)` into a third `StorageModeShared` buffer, the guest pack.
4. Blit the packed buffer into the guest buffer at `dst_offset`.

Steps 1–2 are issued into the current frame's encoder (or a compute encoder, as `load_background`
does). Step 3 needs the blit to have completed: the read-back caller already waits on the frame
feedback, which is exactly the point SY1 bounds. Step 4 is a buffer-to-buffer blit into the guest
window.

**Alternative considered.** A Metal compute kernel that packs depth on the GPU (the GPU form of
`encode_depth_plane`). It removes the CPU round trip and the wait, but it is a new MSL kernel and a
new correctness surface. Vulkan packs on the CPU; the first version mirrors Vulkan, and the kernel
is recorded as a later optimisation if a title makes the read-back hot.

**Rows.** S9 closes. K3, K9 and K10 are the method.

---

## 4. Cluster D — 16-bit index promotion (DR3)

**Today.** The index visitor writes the guest indices in the guest type and passes that type to
`drawIndexedPrimitives`. Metal always restarts a strip at the index type's maximum and has no state
to turn it off, so a legitimate `0xFFFF` vertex in a 16-bit strip becomes a restart.

**Decision.** In the index visitor, when the guest restart is off, the topology is disjointed
(strip or fan) and the type is 16 bit, write u32 indices:

```cpp
const bool promote = (type == rsx::index_array_type::u16)
    && !rsx::method_registers.restart_index_enabled()
    && rsx::is_primitive_disjointed(clause.primitive);
const rsx::index_array_type host_type = promote ? rsx::index_array_type::u32 : type;
const u32 length = index_count * ::get_index_type_size(host_type);
void* dst = index_ring.alloc(length, offset);
// write the u16 array into the first half, then widen it in place, backwards
```

The widening is in place and backwards, so no second buffer is needed and the source word of entry
`i` is still intact when entry `i` is widened. A disjointed primitive never expands, so `written`
equals `index_count` and the widened span fits the allocation. The visitor returns
`index_type = u32` and `index_length = length`, and the draw dispatch (which already reads
`input.index_type`) needs no change. This mirrors `kk_cmd_draw.c:1708-1733`.

**Cost.** One extra pass over the indices for the promoted case only, and only for a disabled
16-bit strip. Strips are common, but the pass is a linear widen of a small array.

---

## 5. Cluster E — synchronization and lifetime hardening

These rows come from `DEVIATIONS-SYNC.md` and are the quickest high-value fixes. SY2 is the only
row that can corrupt memory.

| id | decision | files |
|---|---|---|
| SY1 | Bound the readback wait with `recorder_feedback_timeout`, as `begin_frame` already does. On timeout, log, keep the allocator, take fresh slot objects and a new generation. Put the wait in one place so `begin_frame` and `flush_for_readback` share it. | `MTLCommandRecorder.cpp` |
| SY2 | Release the deferred list only when the slot reported. A timed-out slot's list goes to a quarantine that a later real feedback drains, or that shutdown drains with the ring idle. | `MTLCommandRecorder.cpp` |
| SY3 | Release the four slot objects before `allocate_slot` overwrites them. At shutdown, if the drain times out, keep the objects alive rather than release an allocator with work in flight. | `MTLCommandRecorder.cpp` |
| SY4 | Write a shared null texture and null sampler on a non-zero-to-zero transition of a bound slot, mirroring `VKDraw.cpp:727`. If Metal accepts a null `ResourceID`, use it; otherwise use the 1x1 null image. | `MTLArgumentBinds.cpp` |
| SY5 | Add `residency_manager::remove(MTL::Allocation*)` (`removeAllocation`, `commit`, erase from the published set) and call it from `release_list` for a published allocation. Remove each heap at shutdown. | `MTLResourceManager.cpp` |
| SY9 | Make `frame_slot::generation` a `std::atomic<u32>` (relaxed is enough for the compare). | `MTLCommandRecorder.h` |
| SY10 | Store colour only in `open_pass`, as depth already does, and resolve once in `surface::resolve` when `require_resolve` is set. | `MTLFramebuffer.cpp` |

SY7, SY8 and SY11 stay measurement-gated: SY7 and SY8 need a two-frame GPU capture, SY11 needs a
capture to size the residency cost. SY6 is a reporting change first (the pool high-water mark).

**Cost.** SY1–SY3 and SY9 are recovery-path changes: no steady-state cost. SY4 is one compare per
binding. SY5 is one erase per released published allocation. SY10 removes N resolves per MSAA
clause.

---

## 6. Cluster F — the output chain and present recovery

| id | item | decision |
|---|---|---|
| PF8 | A nil drawable drops the frame. | Retry the acquire on the next flip and never present a stale image; a nil drawable with no previous frame presents a cleared drawable (PF11 already does the fallback). |
| PF9 | `flip` does not re-sync `resolution_scaling_config`. | Re-sync on a config change; a slot tag carries the salt, so a mismatch already re-allocates. Verify against `VKPresent.cpp:991-1016`. |
| F2 | RCAS sharpening. | After the scale pass, run a sharpening pass when `rcas_sharpening_intensity` is non-zero. It is a small compute or fragment pass; port the RSX/VK kernel shape. |
| F3 | FSR. | A faithful port of the FidelityFX compute pass to MSL. It is a self-contained unit of work behind the `output_scaling == fsr` case; scheduled after F2 because it shares the output chain. |
| F11/S8 | No GPU blit path. | Add `upload_scaled_image` to the texture cache and make `scaled_image_from_memory` return true. The format and sample-count rule from K4 gates it; a mismatch routes through staging. The CPU fallback in `nv3089.cpp:592-598` keeps the result correct until then. |

F1 (nearest scaling) is already landed.

---

## 7. Cluster G — settings and small features

Each is a small, local read or a small object. They are grouped here because none needs a new
subsystem.

| id | setting | decision |
|---|---|---|
| F4 | `disable_msl_fast_math` | Pass the flag to the SPIRV-Cross MSL options (`MTLShader.cpp`, `build_msl`). IRC (`spirv_cross::CompilerMSL::Options::enable_fast_math`-equivalent) and the compiler's math mode are the two levers; read the option once at device init. |
| F6 | `shader_compiler_threads_count` | Use the configured count for the Metal worker depth when it is non-zero; keep `utils::get_thread_count()` otherwise. The `threaded` gate already includes `video_renderer::metal` (`rsx_cache.h:280`). |
| F7 | `host_label_synchronization` | Metal has no host label object; the setting maps to the residency/feedback wait policy. Decide whether Metal can honour it at all; if not, report it as unsupported rather than silently ignoring it. |
| F9 | `force_high_precision_z_buffer` | Depth is always `Depth32Float` (`MTLFormats.cpp:82`), so the setting is already satisfied for the high-precision case; the low-precision case is a no-op. Record the mapping and drop the warning. |
| F5 | `shadermode` | The interpreter fallback needs a shader-interpreter path, which Metal does not have. Report it as unsupported with a clear line rather than a silent ignore. |
| F8 | `disable_vertex_cache` | Metal uploads vertex data per draw, so the cache the setting disables does not exist. Report it as already-effective. |
| F10 | debug overlay | Create the overlay text: the statistics pane is built but never given rows. Fill it from the frame counters. Mirror the GL/VK overlay content. |

---

## 8. Cluster H — performance and measurement

| id | item | decision |
|---|---|---|
| DR10 | A pass per clause. | The largest single performance row. Measure first with a GPU capture of a draw-heavy scene; then batch clauses that share a target and no barrier. This is a design change to `MTLPassScheduler`, not a local fix. |
| DR9/S5 | First-use async pipeline build. | Keeps `compile_async` honoured: build the pipeline on a worker and let the shared cache's callback store it. The precompiled set already covers the warm start. Schedule after the correctness rows. |
| SY7, SY8, SY11 | Cross-frame hazards, WAW edges, residency batching. | Measurement-gated. A two-frame capture settles SY7 and SY8; a capture of the residency commit cost settles SY11. |

---

## 9. Cluster I — textures, samplers and copies

Rows from `DEVIATIONS-TEXTURE.md` (TX1–TX18). They are grouped here because most share the sampler
capture and the texture-cache copy path.

| id | item | decision |
|---|---|---|
| TX1 | A vertex texture keeps the guest filters. | Set nearest min and mag filters and `SamplerMipFilterNotMipmapped` in the vertex sampler capture, as `VKDraw.cpp:606-619` does. |
| TX2 | An unnormalized vertex texture is never marked. | Add a `normalized` field, fill it from `format() & CELL_GCM_TEXTURE_UN` for vertex units, and call `setNormalizedCoordinates(false)` with clamp-to-edge on R and no mip filter (K2). |
| TX3 | A format that decodes in the shader is filtered before the decode. | Pass the format flags into the sampler capture and force nearest when a SEXT or GAMMA decode is active, as `VKDraw.cpp:376-479` does. This is the same rule as the texture path's nearest guard. |
| TX4 | The guest anisotropy is not a power of two. | Round up to the next power of two and clamp to 16 before `setMaxAnisotropy` (K1). |
| TX5 | A custom border colour is lost. | Adopt K3 for a border-clamped unit with a non-standard colour; otherwise record the approximation. |
| TX6 | `convolution_min` becomes mipmapped. | Return `SamplerMipFilterNotMipmapped` for that filter. |
| TX7 | A 3D texture gets no view, so its layout and remap are lost. | Decode in the shader for a 3D unit, or apply the map through a 3D view. |
| TX8 | A cube that needs a view gets a 2D six-slice view. | Keep `TextureTypeCube` for a cube source; a 2D view only for a 2D source. Needs a run. |
| TX9 | A format-changing copy is skipped and leaves the temporary uninitialised. | Add a compute or buffer copy path (K7). Highest severity of the cluster. |
| TX10 | A scaled copy is skipped. | Add a scaled blit or compute sampler pass (K7). |
| TX11 | The surface compatibility test is too strict. | Implement the per-format accepted set, including depth-as-colour. |
| TX12 | The texture-cache depth read-back copies the host plane. | The same fix as S9 (Cluster C). Fix both sites in one change; do not do one only. |
| TX13 | A 2-sample request folds up to 4. | Keep the guest lattice for the address paths; record the host-count mismatch. |
| TX14 | An unknown texture format becomes `BGRA8Unorm`. | Fail the lookup and skip the draw with a clear message. |

---

## 10. Order (easiest first) and exit criteria

The order is by risk and effort, correctness before performance.

1. **SY9, SY2, SY3, SY1** — one line, one guard, one release fix, one bounded wait. They touch the
   same file and are safest together.
2. **SY5, SY10** — a residency erase and a store-action change.
3. **S7** — the component-layout map. Isolated and testable by unit test.
4. **S4** — the clear-colour resolver. Shares the table with S7.
5. **S3** — the fill colour write mask.
6. **S5** — the stencil-write state.
7. **DR3** — the index promotion.
8. **TX2, TX4, TX6** — the self-contained sampler rules (normalized coordinates, anisotropy,
   convolution min). Each is a few lines in one capture.
9. **TX1, TX3** — the vertex-sampler filter and the decode-filters-nearest rule. TX3 shares the
   nearest guard the texture path already has.
10. **S9 + TX12** — the depth read-back. Both sites in one change.
11. **TX9, TX10, TX11** — the texture-copy gaps and the compatibility set.
12. **F4, F6, F9, F8, F7, F5** — the small settings.
13. **F10** — the overlay text.
14. **PF8, PF9** — present recovery, validated with the run.
15. **F2, F3, F11/S8** — the output chain.
16. **TX5, TX7, TX8, TX13, TX14** — the remaining texture rows; TX8 and TX13 need a run.
17. **DR10, DR9** — performance, after a capture.
18. **SY7, SY8, SY11, SY6** — after a capture.

Each step: build `./build -j4`, run `MTL*` and the full suite, keep the MTL tests green, and record
the unit test that covers it. A row that needs a run stays `open` until the run confirms it.

**Exit criteria.** A row leaves `METAL4-TODO.md` only when its unit test passes, and a row whose
effect is visible only on device also needs the bounded Metal run. The present-path rows need the
Vulkan reference run to separate a shared defect from a Metal one.

---

## 11. Open design questions

1. The `fragment void` fill entry for a colourless pass: confirm MSL accepts it; otherwise use a
   colour output with an `Invalid` format.
2. `MTL::ColorWriteMask` for a partial clear: confirm a pipeline write mask of `None` with a
   matching colour attachment is legal (it is on the draw path, so it should be).
3. The stencil reference setter: `setStencilReferenceValue` (both faces) is enough for a clear;
   confirm the value reaches a `Replace` op with the pipeline write mask.
4. The residency `remove` API name (`removeAllocation`) and its cost: confirm against the Metal 4
   headers in `3rdparty/metal-cpp`.

<!-- doc-end -->
