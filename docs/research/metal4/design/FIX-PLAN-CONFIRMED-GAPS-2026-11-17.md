# Fix plan — 100% confirmed Metal 4 parity defects (2026-11-17)

Source: `BEHAVIOR-PARITY-AUDIT-2026-11-17.md`. Scope is restricted to gaps whose
**code difference is verified [V]** and that are **defects**.

Excluded, with reason:

| Item | Why it is not in this plan |
|---|---|
| G2 upload row length | **Not a defect.** `upload_texture_subresource` derives its destination pitch from `caps.alignment` (`TextureUtils.cpp:711-735`); Metal passes `alignment = 4` (the packed width), VK passes `row_pitch`. Both decode the guest source stride from `src_layout.pitch_in_block`, so the tight packing is the uploader's contract, not a clamp omission. Changing `span.row_pitch` alone would misalign every decoded row. |
| G6 drawable retry | Documented as intentional (`MTLPresenter.cpp:113-125`); a retry blocks the RSX thread. Policy, not a defect. |
| G10 `force_high_precision_z_buffer` | **Platform limit.** Apple GPUs expose only `Depth32Float_Stencil8`; VK's setting-off state is `D24_UNORM_S8`, which Metal cannot create. Same class as B3/G18. |
| G13 interpreter vertex textures | Falls back to the recompiler; output is correct. Performance only. |
| G14 FIFO wedge, G15 WAW | `[I]` / masked by the StageAll backstop. Unproven. |
| B1 MSAA coords, B2 background-load byteswap | `[U]`; need a focused write-back test before a change. |
| B3 custom border colour | Metal API limit (three border colours); emulation needs a shader-side clamp. Documented TX5. |
| G18 line width | Apple Metal and Apple Vulkan both lack wide lines; "medium confidence" in the audit. Platform limit. |

Everything below is a real behavioural difference and is fixed unconditionally
(no environment gate), Metal-only, in the Metal backend files.

Status: **all included items have landed** in the working tree, with
`ninja bin/rpcs3.app/Contents/MacOS/rpcs3 bin/rpcs3_test` green and
`build-tests/bin/rpcs3_test` 696/696.

---

## Batch A — presentation (self-contained, no shader ABI change) — **done**

### A1. G1 — present-source display-format correction (HIGH) — **done**
- **Defect:** `get_present_source` handed the found surface/cached texture to the
  compose with no comparison against the AV-configured display format.
- **Fix:** `enforce_display_format` runs a byte-exact bitcast of the source into
  the expected format through a compute pass (texture → buffer → texture), cached
  per eye in `m_flip_convert[2]`; called from both the surface path and the
  cached present path.
- **Files:** `MTLGSRender.cpp`, `MTLPresent.*`, `MTLFormats.*`.
- **Test:** no automatable test (needs a present with a mismatched AV format);
  exercised by a title whose display buffer format differs from the AV format.

### A2. G4 — stereo second eye scaled twice — **done**
- **Fix:** the guest width/height are recorded before `get_present_source`
  converts its extent in place, and the second eye starts from those guest
  values, writing the result back only when `image2` exists.
- **Files:** `MTLGSRender.cpp`.
- **Test:** no automatable test; arithmetic mirrors `VKPresent.cpp:584-603`.

### A3. G5 — stereo combined-surface/inset branch missing — **done**
- **Fix:** added the symmetric `else` branch that crops `buffer_height` to
  `min(image_height - min_expected_height, scaled_buffer_height)`
  (`VKPresent.cpp:593-599`).
- **Files:** `MTLGSRender.cpp`.
- **Test:** no automatable test; same shape as the VK branch.

### A4. G7 — no memory reclaim before a resolution-scale change — **done**
- **Fix:** `mtl::device::memory_load_severity()` reports device pressure from
  `recommendedMaxWorkingSetSize()`/`currentAllocatedSize()` on Vulkan's severity
  scale and thresholds; `present_frame` reclaims the surface cache before and
  after `sync_scaling_config`. No queue flush: frees defer to slot completion.
- **Files:** `MTLDevice.*`, `MTLGSRender.cpp`.
- **Test:** no automatable test (device allocator state).

### A5. G3 — overlays vanish when the present source cannot be resolved — **done**
- **Fix:** `frame_output::compose` no longer returns before the overlay loop on a
  null `source.image`; the background/overlay passes still run.
- **Files:** `MTLPresent.cpp`.

### A6. G11 — async compile covered programs needing depth-range emulation — **done**
- **Fix:** `compile_async` also requires
  `!(ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE)`, matching Vulkan: the
  interpreter rejects those programs, so an async miss would drop geometry.
- **Files:** `MTLGSRender.cpp`.

---

## Batch B — texture / surface correctness — **done**

### B1. G2 — **excluded, not a defect** (see the table above).

### B2. G8 — stencil plane never uploaded with a depth texture — **done**
- **Fix:** `decode_stencil_plane` extracts the low byte of each guest z24s8 word;
  the upload allocates a stencil plane (`pitch = align(width, 4)`) and blits it
  with the stencil plane option.
- **Files:** `MTLTextureCache.cpp`, `MTLFormats.*`.
- **Test:** no automatable test; the existing depth read-back exercises the
  inverse.

### B3. G9 — render-target format compatibility is strict equality — **done (subset)**
- **Fix:** any depth host format serves any depth gcm format
  (`is_depth_format(host) && is_gcm_depth_format(gcm)`), plus the exact match.
  Colour ↔ depth pairs stay rejected: Metal cannot view a depth texture as
  colour, and the sampler swizzle is per-format (VK's mixed set cannot be
  mirrored).
- **Files:** `MTLTextureCache.h`.
- **Test:** no automatable test (needs a device).

### B4. G10 — **excluded, platform limit** (see the table above).

---

## Batch C — pipeline / fragment-stage state — **done**

### C1. G12 — interpreter stipple on non-polygon draws — **done**
- **Fix:** the interpreter keys stipple on
  `fragment.ctrl & RSX_SHADER_CONTROL_POLYGON_STIPPLE`, which the RSX engine sets
  only for polygon-class draws, not on the live register bit.
- **Files:** `MTLShaderInterpreter.cpp`.

### C2. G16 — depth-bounds test ignored — **done**
- **Defect:** captured, never consumed; Metal has no `setDepthBounds`.
- **Fix:** the draw appends one `vec4` beside the stipple pattern
  (`stipple_block_count = stipple_pattern_block_count + 1`) carrying
  `depth_bounds_min`, `depth_bounds_max` and the guest sample mask; the fragment
  stage compares its window depth against the bounds and discards outside.
  The captured range is `[0, 1]` whenever the guest test is off, so the compare
  needs no enable flag. The compared depth is exactly the value the stage writes
  to `gl_FragDepth`: `emulated_depth` under `_EMULATE_DEPTH_RANGE`, `r1.z` under
  depth export, `gl_FragCoord.z` otherwise.
- **Deviation from the earlier draft:** the test is not compiled in per-draw.
  Gating the shader on the live bit would recompile a pipeline whenever a title
  toggles the test, which can happen every draw; the dynamic compare costs one
  branch.
- **Files:** `MTLConstants.h`, `MTLGSRender.cpp`, `MTLFragmentProgram.cpp`,
  `FragmentInterpreter.glsl` (Metal-gated block).
- **Test:** no automatable test; the compare is the GL/VK depth-bounds rule.

### C3. G17 — sample mask ignored — **done**
- **Defect:** `multisample.sample_mask` never reached the encoder or the shader;
  the header comment claimed `[[sample_mask]]` and it was not emitted.
- **Fix:** the fragment stage writes `gl_SampleMask[0]` from the same per-draw
  block. SPIRV-Cross maps that built-in to Metal's `[[sample_mask]]` output.
  Probed on this machine: the pipeline is accepted at `rasterSampleCount` 1, 2
  and 4, with and without alpha-to-coverage, and at count 1 only bit 0 survives —
  identical to Vulkan's `pSampleMask` semantics. Dynamic from the block, so no
  pipeline variant per mask value.
- **Files:** `MTLFragmentProgram.cpp`, `FragmentInterpreter.glsl`,
  `MTLGSRender.cpp`.
- **Test:** probe (above); the guest mask reaches the stage in the same encoding
  Vulkan passes.

---

## Gates

1. `ninja bin/rpcs3.app/Contents/MacOS/rpcs3 bin/rpcs3_test` green after every
   batch.
2. `build-tests/bin/rpcs3_test` green.
3. `.metal4-release-scrub.py check` clean (these are functional fixes, not
   instrumentation: no `TODO(excise)`, no probe-only code).
4. Each item lands with a test or an explicit "no automatable test" note.

## Order

A (presentation) → B (textures) → C (pipeline), because A and B are independent
of the shader ABI that C extends. G1 is the largest single item and is done
inside Batch A.
