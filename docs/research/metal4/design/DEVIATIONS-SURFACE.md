# DEVIATIONS-SURFACE — the Metal 4 surface, clear, blit and 2D-engine paths

Scope: the RSX Metal 4 backend (`rpcs3/Emu/RSX/MTL`). This file audits only the surface,
clear, blit and 2D-engine paths. It compares them with the Vulkan backend
(`rpcs3/Emu/RSX/VK`), the OpenGL backend (`rpcs3/Emu/RSX/GL`) and the shared code
(`rpcs3/Emu/RSX/Common`). It also mines the KosmicKrisp Metal driver for valid Metal 4
techniques.

The file holds three parts: the deviation table, the KosmicKrisp technique table, and the
prose commentary.

## Method

The RSX is the source of truth. Vulkan and OpenGL are the reference implementations. A row
enters the deviation table when the Metal path does not reproduce the behaviour the reference
shows. Each row names the exact `file:line` on both sides. When the reference has no
equivalent, the row says so.

Read-only. No build, no run, no commit. One file is created.

The Metal scope files are `MTLGSRender.cpp`, `MTLSurface.{cpp,h}`, `MTLSurfaceCache.{cpp,h}`,
`MTLSurfaceState.{cpp,h}`, `MTLFill.{cpp,h}`, `MTLFramebuffer.{cpp,h}` and `MTLZCULL.cpp`.
The reference files are `VKGSRender.cpp`, `VKRenderTargets.{cpp,h}`, `VKTexture.cpp`,
`VKTextureCache.{cpp,h}`, `GLGSRender.cpp`, `GLRenderTargets.cpp`, `GLTextureCache.h`,
`Common/surface_store.h`, `Common/surface_utils.h`, `Common/texture_cache.h` and
`Utils/color_utils.hpp`. The 2D-engine methods are `NV47/HW/nv0039.cpp`, `nv3089.cpp` and
`nv308a.cpp`.

The KosmicKrisp source is a Vulkan driver on Metal. It is read-only reference material for the
Metal 4 API. A KosmicKrisp technique enters the second table only when it is valid for the RSX.
A technique that only satisfies Vulkan semantics is rejected, with the reason stated.

The class of a row is one of: `wrong result`, `missing feature`, `performance`, `diagnostic` or
`not a deviation`. The confidence is `yes (code)` for a static proof, `yes (TODO <id>)` for a
defect the existing work list already records, and `no` for a claim that needs a run to confirm.

The severity is `H`, `M` or `L`. Severity reflects the effect on the rendered image and the
cost to fix, not the size of the code change.

## Deviation table

| id | deviation | Metal file:line | VK/GL reference file:line | class | severity | confirmed? | proposed fix |
|---|---|---|---|---|---|---|---|
| S1 | The bound-target lock loop has no `address && pitch` guard. A zero-address target is still locked. | `MTLGSRender.cpp:875-908` (lock at `:898-900`) | `VKGSRender.cpp:2780`; `GLRenderTargets.cpp:375` | wrong result | M | yes (TODO D1) | Skip the index when `m_surface_info[index].address` or `.pitch` is zero, as the reference does. |
| S2 | The orphan write-back loop has no `is_locked()` re-check. An already-locked orphan is locked again. | `MTLGSRender.cpp:996-1015` | `VKGSRender.cpp:2742-2776` (check at `:2749-2761`); `GLRenderTargets.cpp:338-364` | wrong result | M | yes (TODO D2) | Re-check `surface->is_locked()` before `lock_memory_region`; otherwise call `commit_framebuffer_memory_region`. |
| S3 | The clear ignores the per-channel clear mask. Any RGBA bit becomes a full RGBA clear. | `MTLFramebuffer.cpp:140,148,160-163`; `MTLGSRender.cpp:1061,1063,1107` | `VKGSRender.cpp:1516-1620`; `GLGSRender.cpp:735-812` | wrong result | M | yes (code) | Pass the channel mask to the clear. Use a full `LoadActionClear` only for the full mask. Use `fill_pass` with a write mask for a partial mask. |
| S4 | The clear loses the format-aware clear colour. The code uses raw `clear_color_*/255` and never applies the format map. | `MTLSurfaceState.cpp:95-99`; `MTLFramebuffer.cpp:162`; `MTLGSRender.cpp:1109-1118` | `VKGSRender.cpp:1526-1578`; `GLGSRender.cpp:742-791`; `Utils/color_utils.hpp:186-278` | wrong result | H | yes (code) | Apply the surface's native component map to the clear colour before `setClearColor` or the fill. See technique K1 and K2. |
| S5 | A partial stencil clear ignores `stencil_mask()`. Only a zero mask drops the stencil bit. Any other partial mask clears all eight bits. | `MTLGSRender.cpp:1031-1034`; `MTLFramebuffer.cpp:142,185-191` | `VKGSRender.cpp:1624-1638`; `GLGSRender.cpp:694` | wrong result | M | yes (code) | Add a stencil clear pass that writes `stencil_clear_value() & stencil_mask()` on the masked bits. Mirror `vk::stencil_clear_pass`. |
| S6 | `surface_matches_properties` compares the guest sample count with the host sample count. The two differ when the AA override is not `_auto`. | `MTLSurfaceCache.cpp:71,84` (`host_sample_count` at `:11-14`) | `VKRenderTargets.h:595` (`get_format_sample_count(antialias)`) | performance | M | yes (code) | Compare `get_spp()` with `rsx::get_format_sample_count(antialias)`, as Vulkan does, or compare it with the stored host sample count. |
| S7 | A surface never sets a native component layout. The default layout is `{A,R,G,B}`, which `get_view` treats as identity. The `b8`, `g8b8` and `x32` reads are wrong. | `MTLSurface.cpp:24-77` (no call); default at `MTLHandles.h:127-130`; `MTLHandles.cpp:24-36` | `VKRenderTargets.h:290,358,538`; `GLRenderTargets.cpp:42-62`; `VKGSRender.cpp:88-104` | wrong result | H | yes (code) | Add `get_surface_component_layout(gcm_format)` and call `set_native_component_layout` in `surface::create`. Mirror `MTLFormats.cpp:314-364`. |
| S8 | The blit engine has no GPU path. `scaled_image_from_memory` still returns `false`. The texture cache has no `upload_scaled_image`. | `MTLGSRender.cpp`; `MTLTextureCache.h` (no blit) | `VKTextureCache.cpp:1686`; `VKTextureCache.h:537`; `GLTextureCache.h:977-979` | performance | L | partial (correctness fixed) | Add a GPU blit path to the texture cache. The CPU fallback in `nv3089.cpp:592-598` now flushes the source and invalidates the destination, so the result is correct but slower. See technique K4. |
| S9 | The depth read-back writes the host plane, not the guest pack. `z16_float` writes four bytes per texel into a two-byte row. `z24s8` loses the stencil and writes a float. | `MTLSurface.cpp`; `MTLTextureCache.cpp` | `VKTexture.cpp:118-229`; `VKRenderTargets.h:635-698` | wrong result | H | fixed | Copy the depth plane and the stencil plane with the two blit options, pack them with `encode_depth_plane`, and copy the packed row into the target. See technique K3. |
| S10 | The read-back and the background upload mix the host extent with the guest pitch. They disagree when resolution scaling is on. | `MTLSurface.cpp:471-506`; `MTLSurface.cpp:391-394` | `VKRenderTargets.h:645-655` (`is_scaled` → `copy_scaled_image`) | wrong result | M | no (needs run) | Detect a scaled surface and downscale it first, as Vulkan does, or keep the read-back in guest units. |
| S11 | `is_compatible_surface` compares the guest GCM format, not the host format. Two guest formats that share one host format do not match. | `MTLSurfaceCache.cpp:45` | `VKRenderTargets.h:480` (`surface->format()`) | performance | L | yes (code) | Compare `pixel_format()` on both surfaces, or accept a shared host format as a match. |
| S12 | `reconcile_render_targets` locks the bound targets before it releases the superseded and orphaned surfaces. The release can drop a lock the same call just took. | `MTLGSRender.cpp:875-908` then `:989-1017` | `VKGSRender.cpp:2732-2776` then `:2778-2810` | wrong result | M | yes (code) | Run the superseded and orphaned pass first, then lock the bound targets, as Vulkan does. |
| S13 | `bind_framebuffer` never calls `check_zcull_status(true)`. The occlusion result is stale after a framebuffer change. | `MTLGSRender.cpp:768-825` | `VKGSRender.cpp:2851`; `GLRenderTargets.cpp:321` | wrong result | M | yes (TODO PF7) | Call `check_zcull_status(true)` at the end of `bind_framebuffer`. |
| S14 | Not a deviation. A full clear becomes `LoadActionClear`; a partial clear becomes a scissored `fill_pass`. The scissor defines the clear region. | `MTLGSRender.cpp:1068-1077,1080-1121`; `MTLFramebuffer.cpp:148`; `MTLFill.h:11-14` | `VKGSRender.cpp:1596-1615,1653-1657`; `GLGSRender.cpp:820-825` | not a deviation | L | yes (code) | None. This is the Metal form of `vkCmdClearAttachments`. |
| S15 | Not a deviation. `write_render_target_to_memory` calls `read_barrier` before the copy. | `MTLSurfaceCache.cpp:203-213` | `VKRenderTargets.h:643` | not a deviation | L | yes (code) | None. |
| S16 | Not a deviation. NV0039 and NV308A run on the CPU, so they have no backend path to deviate from. NV3089 dispatches to the backend only for a frame-buffer DMA. | `MTLGSRender.cpp:3213-3241` | `NV47/HW/nv0039.cpp`; `NV47/HW/nv308a.cpp`; `NV47/HW/nv3089.cpp:592-598` | not a deviation | L | yes (code) | None. |

## KosmicKrisp technique table

| id | KK file:line | what KK does | valid for RSX? | why | how we would apply it |
|---|---|---|---|---|---|
| K1 | `kk_cmd_draw.c:170-202` | Builds the Metal clear colour from the format's `unswizzle` map, because Metal forbids a swizzle on a renderable texture. | adopt | It is the exact fix for S4. The map moves the clear colour into the host channel order at the pass. | Apply the surface's component map to `state.clear_color` before `setClearColor` and before the fill value is packed. |
| K2 | `kk_format.c:107-161,231-236` | Each format carries an `is_native` flag and an `unswizzle` channel map. A non-native format uses a base native format plus the map. | adopt | It is the exact fix for S7. The map states the guest-to-host channel order per format. | Add `get_surface_component_layout(gcm_format)`. Call `set_native_component_layout` in `surface::create`. Reuse the map for K1. |
| K3 | `mtl_types.h:96-100`; `kk_cmd_copy.c:50-58,160-178` | Copies the depth plane and the stencil plane with `MTL_BLIT_OPTION_DEPTH_FROM_DEPTH_STENCIL` and `MTL_BLIT_OPTION_STENCIL_FROM_DEPTH_STENCIL`. | adopt | It is the exact fix for S9. Metal cannot copy a combined depth-stencil texture without an aspect option. | In `write_to_memory`, copy the depth aspect and the stencil aspect into two staging buffers, then pack with `encode_depth_plane`. |
| K4 | `kk_cmd_copy.c:265-293,351-405` | Allows a texture-to-texture copy only when the format and the sample count match. Any other copy goes through a buffer. | adopt | It states a Metal rule. It explains the RSX `unresolve` no-op (`MTLSurface.cpp:463-469`) and it constrains the future blit engine (S8). | The blit engine checks format equality and sample-count equality. A mismatched copy routes through a staging buffer. |
| K5 | `kk_cmd_buffer.c:895-923`; `kk_cmd_draw.c:368-372` | Chooses the store action from the resolve mode, the load and store ops, and whether the pass covers the whole framebuffer. | adopt with condition | RSX can drop store traffic when the pass is not the last writer, but the resolved host image must survive. | Use `StoreActionDontCare` only when no later reader needs the image and no resolve needs it. Keep `Store` otherwise. |
| K6 | `kk_cmd_buffer.c:458-488` | Splits the render pass for every multisample pass and for every barrier that needs a split. It applies the store ops before the split. | adopt | The RSX multisample passes are split too. The store op must run at the split, not at the pass start. | Make the pass scheduler split every multisample pass and re-apply the store ops at the split. |
| K7 | `kk_cmd_buffer.c:1000-1005` | Forces a depth or stencil resolve into a meta (compute) resolve. The comment records a Metal synchronization bug for depth render-pass resolves. | adopt with condition | The RSX depth resolve uses a render-pass resolve (`MTLSurface.cpp:416-461`). The Metal bug may affect it. | Verify the RSX depth resolve with a queue-scope barrier. If it is flaky, move the depth resolve to a compute pass. |
| K8 | `kk_cmd_buffer.c:966-985` | Records that Metal forbids `MULTISAMPLE_RESOLVE` for one of depth and stencil while the other uses `STORE`. | informational | The RSX resolve uses `StoreAndMultisampleResolve` for both aspects, so there is no conflict. | Keep both store actions equal in the resolve pass. Do not mix a resolve with a plain store. |
| K9 | `kk_image.c:93-94`; `mtl_texture.m:104-135` | Records that a combined depth-stencil texture has no CPU access. `getBytes` and `replaceRegion` are the host-copy paths. | adopt | The RSX must not use a host copy on a combined depth-stencil texture. It must use the blit options or a compute pass. | The depth read-back uses K3. The depth upload uses a decode compute pass (`decode_depth_plane`). |
| K10 | `kk_cmd_copy.c:72-75,1006-1009` | Sets `buffer_2d_image_size_B` to zero for a 2D image, because Metal requires zero there. | adopt | It is a Metal rule the RSX blit and read-back helpers must honour. | Set the image size to zero when the copy is 2D. |
| K11 | `kk_cmd_draw.c:517-526`; `kk_cmd_meta.c:284-317` | Runs a partial clear as `vkCmdClearAttachments` with a rectangle, over a meta pipeline. | reject (already covered) | Metal 4 has no scissored clear. The RSX already uses a scissored `fill_pass`, which is the same idea. | None. The Metal path is already equivalent, as row S14 records. |

## Highest-risk deviations

S4 is the highest risk. The clear colour is the same for every format. A `b8`, `g8b8`,
`r5g6b5`, `x1r5g5b5` or `abgr8` surface clears to the wrong channel order. The fix is small and
KK already shows the method (K1).

S7 is the next. A surface never sets a component layout. A surface that is sampled as a texture
reads the wrong channels for `b8`, `g8b8` and `x32`. The texture-cache path sets the layout
(`MTLTextureCache.h:440,493`), so the surface path is the missing half. K2 is the method.

S9 corrupts the depth read-back. For `z16_float` the copy writes four bytes per texel into a
two-byte row. For `z24s8` the stencil is lost and the depth is written as a float. K3 is the
method.

S1, S2 and S12 change the lock state of guest memory. They can protect the wrong range or drop
a lock the same call took. Vulkan and OpenGL both order the work the other way.

## KosmicKrisp adoption shortlist

Adopt first: K1 and K2 together. They close S4 and S7, and they share one format map.

Adopt second: K3, K9 and K10. They close S9 and they fix the depth read-back.

Adopt third: K4 with S8. The blit engine needs the format and sample rule before it can be
written.

Adopt with a test: K5, K6 and K7. Each changes a pass boundary or a store action. Each needs a
run to confirm the image is unchanged and the traffic falls.

Informational: K8. The RSX resolve does not hit the limit.

## Rejected KosmicKrisp ideas

The per-level clear in `kk_cmd_clear.c:37-132` is Vulkan-semantics-only. It begins and ends a
render pass for each mip level and array slice, because Vulkan's clear range is free-form. The
RSX has no per-mip clear. The RSX pass already carries the clear as a load action.

The meta fallback path in `kk_cmd_meta.c:203-330` is Vulkan-semantics-only. It exists for
Vulkan's flexible clear rectangles, blits and resolves. The RSX pass structure is fixed, so the
fallback would add a shader pass for no gain.

The load-op and store-op translation in `vk_to_mtl_map.c` and `kk_cmd_draw.c:147-153` is Vulkan
API plumbing. The RSX clear mask and the RSX sample count do not map onto it directly. Rows S3
and S5 replace it.

`MTL_STORE_ACTION_MULTISAMPLE_RESOLVE` without a store (`kk_cmd_buffer.c:918-919`) is rejected.
The RSX keeps the resolved host image across passes, so the store must stay.

The host image-to-image copy in `kk_image.c:1092-1160` is rejected. It copies through a
`ralloc_size` host buffer. The RSX blit must stay on the GPU (S8).

The vertex-amplification layer loop in `kk_cmd_meta.c:295-316` is rejected. It serves Vulkan
multiview. The RSX has no multiview path here.

---

Compliance note: this document quotes KosmicKrisp (Mesa) source for analysis only. It is not a
Mesa contribution, and it is not published to the Mesa project. The Mesa `AGENTS.md` in the
reference clone asks for a marker when prose is generated; biblioklept.

<!-- doc-end -->
