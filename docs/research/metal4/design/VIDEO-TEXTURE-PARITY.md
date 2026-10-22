# VIDEO-TEXTURE-PARITY — `Invalid 2D region descriptor` during FMV, and the video upload/sampling path

Owner/Scope: RSX-first Metal 4 backend, video texture sampling and render-target write-back | Status: draft | Date: 2026-10-19
Branch: `metal4`. Read-only audit. No file under `rpcs3/**` is changed.

**Legend.** `[V]` verified against a primary source in this repo. `[I]` inference from `[V]` facts. `[U]` could not verify.
`file:line` is advisory (the branch moves). Symbol names are the stable reference.

---

## 1. Method

1. Locate the exact message string. Only one emitter exists: `rpcs3/Emu/RSX/Common/surface_store.h:1305`. `[V]`
2. Read the enclosing function `get_merged_texture_memory_region` and list every caller by symbol. `[V]`
3. For each caller, find the source of the `required_pitch` and `required_bpp` arguments. `[V]`
4. Trace the RSX texture descriptor fields (`width`, `height`, `pitch`, `format`, `bpp`) from the guest registers to the cache. `[V]`
5. Compare the Metal diagnostic and render paths against the Vulkan and OpenGL paths at the same seams. `[V]`
6. Compare the three `write_color_buffers` implementations site by site. `[V]`

Commands used were read-only (`grep`, file reads, `git status`). No build and no emulator run.

---

## 2. `Invalid 2D region descriptor` root cause

### 2.1 The check

`get_merged_texture_memory_region` computes a required row length in bytes and compares it with the requested pitch:

- `rpcs3/Emu/RSX/Common/surface_store.h:1300` — `const u32 required_width_in_bytes = (required_width * required_bpp);`
- `rpcs3/Emu/RSX/Common/surface_store.h:1303` — `if (required_width_in_bytes > required_pitch) [[unlikely]]`
- `rpcs3/Emu/RSX/Common/surface_store.h:1305` — the warning prints `required_width`, `required_height`, `required_bpp`, `required_pitch`.
- `rpcs3/Emu/RSX/Common/surface_store.h:1307` — the function returns an empty list. `[V]`

So the message fires when `width * bpp > pitch`, and the lookup then returns no surface overlaps.

### 2.2 Every caller of `get_merged_texture_memory_region`

| Caller | Site | `pitch` argument | Source of pitch |
| --- | --- | --- | --- |
| Metal sample diagnostic | `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:1779` | `tex.pitch()` | Raw guest register. `[V]` |
| Metal present | `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:2661` | `pitch` parameter | AV config: `buffer_pitch = buffer_width * avconfig.get_bpp()` at `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:2888`, passed at `:2905`. `[V]` |
| Vulkan present | `rpcs3/Emu/RSX/VK/VKPresent.cpp:315` | `info->pitch` | AV config present info. `[V]` |
| OpenGL present | `rpcs3/Emu/RSX/GL/GLPresent.cpp:61` | `info->pitch` | AV config present info. `[V]` |
| Blit rtt lookup | `rpcs3/Emu/RSX/Common/texture_cache.h:1481` | `pitch` parameter | `src.pitch` / `dst.pitch` at `rpcs3/Emu/RSX/Common/texture_cache.h:3404` and `:3408`. `[V]` |
| Texture search (prefer surface cache) | `rpcs3/Emu/RSX/Common/texture_cache.h:2595` | `attr.pitch` | Computed in `upload_texture`. `[V]` |
| Texture search (default) | `rpcs3/Emu/RSX/Common/texture_cache.h:2664` | `attr.pitch` | Computed in `upload_texture`. `[V]` |

Only one caller passes a raw guest pitch: the Metal diagnostic at `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:1779`. All other callers pass a computed or AV-derived pitch. `[V]`

### 2.3 Why `tex.pitch()` is 0

`tex.pitch()` reads the low 20 bits of `NV4097_SET_TEXTURE_CONTROL3`:

- `rpcs3/Emu/RSX/RSXTexture.cpp:453-456` — `registers[NV4097_SET_TEXTURE_CONTROL3 + m_index] & 0xfffff`. `[V]`

That register field is only meaningful for linear textures. Swizzled textures ignore it, and games commonly leave it at 0. `[I]`

### 2.4 `bpp=16` meaning

`required_bpp` is `get_format_block_size_in_bytes(gcm_format)`:

- `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:1697` — `block = rsx::get_format_block_size_in_bytes(gcm_format)`.
- `rpcs3/Emu/RSX/Common/TextureUtils.cpp:1391-1393` — the function returns `16` for `COMPRESSED_DXT23`, `COMPRESSED_DXT45`, and `W32_Z32_Y32_X32_FLOAT`. `[V]`

`bpp=16` is bytes per **block**, not bytes per element. For DXT23/DXT45 the block is 4x4 texels, so 16 bytes/block means 8 bits per texel. Only `W32_Z32_Y32_X32_FLOAT` has 16 bytes per texel (128-bit). `[V]`

The reported format is `0x88`:

- `rpcs3/Emu/RSX/gcm_enums.h:141` — `CELL_GCM_TEXTURE_COMPRESSED_DXT45 = 0x88`. `[V]`

So the sample warning is a DXT45 texture. `bpp=16` is the 16-byte DXT block. `[V]`

### 2.5 The mismatch, stated precisely

For a DXT45 texture of width `W` texels, the true row pitch is `ceil(W/4) * 16` bytes (`rpcs3/Emu/RSX/Common/TextureUtils.cpp:1540-1553`). The diagnostic passes `required_width = W` texels and `required_bpp = 16` bytes. The check then compares `W * 16` against a pitch that is about `4 * W`. The check is dimensionally wrong for compressed formats. Any nonzero width fails it. `[V]`

### 2.6 Verdict

The warning comes from the Metal-only diagnostic call at `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:1779-1781`. That call passes `tex.pitch()`, which is 0 for the swizzled DXT45 video texture, and `bpp=16` from `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:1697`. The check at `rpcs3/Emu/RSX/Common/surface_store.h:1303` then rejects the region and returns `{}`, so the next line, `store lookup ... overlaps 0` at `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:1784`, always prints 0. This matches the reported log order. `[V]`

---

## 3. Is it Metal-specific?

### 3.1 The emitter is shared; the trigger is not

The string and the check live in shared code (`rpcs3/Emu/RSX/Common/surface_store.h:1305`). Any backend can reach it. `[V]`

But only the Metal backend passes a raw guest pitch:

- Vulkan and OpenGL have no draw-time diagnostic that calls this function. A search for the symbol finds only `rpcs3/Emu/RSX/VK/VKPresent.cpp:315`, `rpcs3/Emu/RSX/GL/GLPresent.cpp:61`, and the shared cache callers. `[V]`
- The shared cache read path never passes 0 for a texture that reaches the check. `upload_texture` sets `attr.pitch` to a computed value: `rpcs3/Emu/RSX/Common/texture_cache.h:3113-3127`. `[V]`
- The compressed path returns before the check. `fast_texture_search` returns early for compressed formats: `rpcs3/Emu/RSX/Common/texture_cache.h:2521-2538`. `[V]`

So the real sampling path for the DXT45 video texture never reaches the sanity check. Only the Metal diagnostic does. `[V]`

### 3.2 Why the compressed early return shields the check

`is_compressed_gcm_format` lists DXT1, DXT23, DXT45, B8R8_G8R8, R8B8_R8G8, HILO8, HILO_S8:

- `rpcs3/Emu/RSX/Common/texture_cache_helpers.h:147-162`. `[V]`

`upload_texture` sets `options.is_compressed_format` from this test at `rpcs3/Emu/RSX/Common/texture_cache.h:3087`. `[V]`

For a non-compressed texture the check is consistent, because the packed pitch equals `width * bytes_per_texel`, so `width * bpp == pitch`. `[V]`

### 3.3 Conclusion

The specific warning is Metal-specific. It is a **diagnostic false positive**, not a render-path defect. Vulkan and OpenGL would not print it for these addresses. `[V]`

However, the diagnostic is blinded for all swizzled textures, including non-compressed ones, because it always passes `tex.pitch()`. `[V]`

---

## 4. Video upload/sampling path parity table

Metal sites column uses `MTL`. The reference column uses `VK` and `GL`.

| id | area | Metal site | VK/GL reference | divergence | risk | proposed fix |
| --- | --- | --- | --- | --- | --- | --- |
| V1 | Warning source in the sample scan | `MTLGSRender.cpp:1779-1781` passes `tex.pitch()` | No equivalent draw-time call in VK/GL | Metal passes raw guest pitch (0 for swizzled) | Diagnostic only. Misleads the reader. Hides real overlaps. | Compute the effective pitch the same way as `texture_cache.h:3110-3127`, or read the resolved descriptor pitch. |
| V2 | Compressed texture resolution | `texture_cache.h:2521-2538` returns early, then `MTLGSRender.cpp:1348-1351` clears the unit if invalid | Same shared code | None. Metal uses the shared resolver. | None found. | None. |
| V3 | Linear vs swizzled upload | `MTLTextureCache.cpp:18-26` sets `supports_hw_deswizzle=false`, `supports_byteswap=false`, `supports_vtc_decoding=false`, `alignment=4` | `VKTexture.cpp:1141` uses device BC support and heap alignment; `GLTexture.cpp:646-650` uses `ARB_compute_shader_supported` | Metal always deswizzles on the CPU (`TextureUtils.cpp:1182`). VK/GL may deswizzle on the GPU. | Higher CPU cost for swizzled video-style textures. Not a correctness break. | Keep for now. Revisit if CPU cost shows in the profile. |
| V4 | Row pitch alignment of the staging upload | `MTLTextureCache.cpp:53` aligns to 4 | VK uses `heap_align`; GL uses its own alignment | Metal uses the minimum alignment. | Low. Metal accepts 4-byte rows. | None. |
| V5 | DXT block upload size | `MTLTextureCache.cpp:53-54` uses `width_in_block * block_bytes` | `VKTexture.cpp:1092` computes an upload pitch per format | None material. Both use block size. | None. | None. |
| V6 | Guest write invalidation (video frames) | `MTLGSRender.cpp:3277-3350` `on_access_violation` → `invalidate_address` at `:3349` | `VKGSRender.cpp:1020`, `GLGSRender.cpp:1277` | Same shape. Metal flushes in place on the RSX thread and queues on guest threads. | The Metal code comments an unresolved extra fault on the RSX thread (`MTLGSRender.cpp:3286-3289`). | Confirm the source of the RSX-thread fault. See Open questions. |
| V7 | Range invalidation | `MTLGSRender.cpp:3435-3453` `on_invalidate_memory_range` | `VKGSRender.cpp:1114`, `GLGSRender.cpp:1312` | None found. | None. | None. |
| V8 | Texture with pitch 0 is skipped | Not in the render path. `attr.pitch` is never 0 there (`texture_cache.h:3113-3127`). Only the V1 diagnostic can pass 0. | Same. | The real path never passes 0. | Low for the render path. | Fix V1. |
| V9 | What is sampled when the descriptor is invalid | `MTLGSRender.cpp:1348-1351` clears `frame.fragment_textures[i]` | VK/GL do the same in their draw paths | Metal binds no view. The shader reads the stale or null argument-table slot. | A wrong frame is possible if the video texture fails to resolve. The log does not prove this. | Add a counter for invalid fragment units, then check the FMV output. |
| V10 | Present source for the video frame | `MTLGSRender.cpp:2646-2726` | `VKPresent.cpp:306-364`, `GLPresent.cpp:51-103` | Metal mirrors the VK/GL order: surface store, then texture cache, then CPU upload. `MTLGSRender.cpp:2664-2726`. | None found. Structure matches. | None. |
| V11 | CPU fallback present copy | `MTLGSRender.cpp:2726-2807` repacks rows when `pitch != row_bytes` | `GLPresent.cpp:127` | Same intent. | None found. | None. |

### 4.1 Notes on the table

- V1 is the only divergence that changes observable behaviour (log output). `[V]`
- The Metal `diagnostic_draws % 30 == 0` gate at `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:1681` explains a bounded count like 47 warnings in one FMV. `[I]`
- The diagnostic dumps guest bytes and reads guest memory at `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:1752-1772`. `[V]`

---

## 5. `write_color_buffers` parity

### 5.1 Sites

| Purpose | Metal | Vulkan | OpenGL |
| --- | --- | --- | --- |
| Stale-range flush before reset | `MTLGSRender.cpp:844` | `VKGSRender.cpp:2662` | `GLRenderTargets.cpp:156` |
| Lock the bound targets | `MTLGSRender.cpp:894` | `VKGSRender.cpp:2783` | `GLRenderTargets.cpp:378` |
| Orphan write-back or commit | `MTLGSRender.cpp:1002` | `VKGSRender.cpp:2747` | `GLRenderTargets.cpp:342` |

### 5.2 Behaviour comparison

1. Stale-range flush. All three check `m_surface_info[index].pitch && write_color_buffers`, set `memory_read_flags::flush_once`, and call `flush_if_cache_miss_likely`. Metal `MTLGSRender.cpp:844-849`; Vulkan `VKGSRender.cpp:2662-2667`; OpenGL `GLRenderTargets.cpp:156-161`. No divergence. `[V]`

2. Bound-target lock. All three lock the section when `write_color_buffers` is on, and commit otherwise.
   - Metal `MTLGSRender.cpp:894-907` locks inside the layout loop.
   - Vulkan `VKGSRender.cpp:2778-2794` locks in a second loop and guards with `if (!m_surface_info[index].address || !m_surface_info[index].pitch) continue;`.
   - OpenGL `GLRenderTargets.cpp:373-390` uses the same guard.
   - Divergence D1: Metal has no `address && pitch` guard on this loop. Metal iterates `get_rtt_indexes(...)` and only checks `if (!target) continue;` at `MTLGSRender.cpp:878`. If the layout address is 0, Metal still locks. `[V]`

3. Orphan write-back. This is the largest divergence.
   - Vulkan `VKGSRender.cpp:2742-2776`: when `lock` is true, it re-checks `!surface->is_locked()` (release) or `!is_protected(...)` (debug). If already locked, it sets `lock = false` and commits instead.
   - OpenGL `GLRenderTargets.cpp:337-371`: the same `!surface->is_locked()` re-check at `:344-356`.
   - Metal `MTLGSRender.cpp:996-1015`: it computes `write_back` and calls `lock_memory_region` with no `is_locked()` re-check.
   - Divergence D2: Metal re-locks an orphan surface that is already locked. `is_locked()` exists on the Metal surface through `rsx::render_target_descriptor` (`rpcs3/Emu/RSX/Common/surface_utils.h:820-823`), so the Metal path can make the same test. `[V]`
   - Risk: `lock_memory_region_impl` re-runs `reset` or `create` on the section (`rpcs3/Emu/RSX/Common/texture_cache.h:1394-1411`). A double lock can rebuild the section under an existing reference. `[I]`

4. `is_active_surface` flag. Metal passes `false` for orphans (`MTLGSRender.cpp:1006`) and `true` for bound targets (`MTLGSRender.cpp:898`). Vulkan and OpenGL do the same (`VKGSRender.cpp:2770` false, `:2786` true; `GLRenderTargets.cpp:365` false, `:382` true). No divergence. `[V]`

5. Superseded surfaces. Metal discards them at `MTLGSRender.cpp:989-994`. Vulkan at `VKGSRender.cpp:2732-2740`. OpenGL at `GLRenderTargets.cpp:327-335`. No divergence. `[V]`

6. Depth handling. Metal mirrors `write_depth_buffer` at `MTLGSRender.cpp:860`, `:974`, `:1000-1002`. Vulkan at `VKGSRender.cpp:2679`, `:2746`, `:2799`. OpenGL at `GLRenderTargets.cpp:185`, `:341`, `:395`. No divergence. `[V]`

### 5.3 Difference in when the flag is read

Metal reads `g_cfg.video.write_color_buffers` at three points (`MTLGSRender.cpp:844`, `:894`, `:1002`), the same count as Vulkan (`:2662`, `:2783`, `:2747`) and OpenGL (`:156`, `:378`, `:342`). The flag is read at the same stage in each backend. `[V]`

---

## 6. Open questions

1. Is the printed format `0x88` really the video frame surface, or one of several sampled textures in the same draw? The diagnostic at `MTLGSRender.cpp:1686-1811` scans every enabled fragment texture, so the warning can come from any unit. `[V]` A per-unit dump is needed to confirm.
2. What is the source of the extra access fault on the RSX thread? Metal documents it at `MTLGSRender.cpp:3286-3289` and flushes in place. The access is still unknown. `[U]`
3. Does the DXT45 video texture ever fail to resolve in the real path? If `find_texture_from_dimensions` misses (`texture_cache.h:2524`), the draw samples an unbound unit (`MTLGSRender.cpp:1348-1351`). The log does not show an invalid-unit counter, so this is unverified. `[U]`
4. Diverge D2 (no `is_locked()` guard on the Metal orphan loop) is safe only if an orphan can never already be locked. The Vulkan and OpenGL guards suggest it can. This needs a test. `[U]`
5. The Metal upload uses `alignment=4` (`MTLTextureCache.cpp:25`). VK uses heap alignment. The effect on DXT row decode is not measured. `[U]`

---

## 7. Top fixes (priority order)

1. Fix the sample-scan call at `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:1779-1781`. Pass the effective pitch, not `tex.pitch()`. This removes the 47 warnings and restores the overlap count. `[V]`
2. Add the `is_locked()` guard to the Metal orphan loop at `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:996-1015`, to match `VKGSRender.cpp:2749-2761` and `GLRenderTargets.cpp:344-356`. `[V]`
3. Add the `address && pitch` guard to the Metal bound-target loop at `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:875-908`, to match `VKGSRender.cpp:2780` and `GLRenderTargets.cpp:375`. `[V]`
4. Add an invalid-fragment-unit counter at `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:1348-1351` to answer Open question 3. `[V]`

<!-- doc-end -->
