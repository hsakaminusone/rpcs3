# The one-run capture: what a defect frame has to carry

Status: design, 2026-11-03. This document answers one question: if the next run is
the last diagnostic run for a while, what must it record so that every remaining
root cause can be discriminated from the bytes on disk, without another run?

It is deliberately written before the code, because the last month was spent
capturing clean frames and falsifying one hypothesis per run. The cost of a
missing field is a week; the cost of a redundant field is a few megabytes.

## 1. Why the current capture cannot answer the question

The 2026-11-03 run produced 770 MB of ring dumps, 294 frames, seven F12 presses,
and **no defect frame**. That is not bad luck alone. The capture has four
structural blind spots:

1. **It dumps the wrong rings.** `frame_snapshot` copies persistent, volatile,
   index, draw-parameters, sampler-info and fragment-context
   (`MTLConstants.h:329-340`). It does **not** copy the transform-constant file,
   the fragment-constant file, the vertex context, the stipple, the blend values,
   the instancing tables, the instruction ring or the overlay rings. A collapsed
   glyph is a vertex position; a vertex position is
   `scale_offset_mat * position`. The matrix is in the transform-constant file,
   which the capture never saved. **The single most likely explanation for a
   collapsed quad is the one block the dump omits.**
2. **It snapshots at the wrong time.** `snapshot_frame` runs after `present`
   (`MTLGSRender.cpp:5148`) and copies the ring's *current* buffer. A
   `block_ring`/`stream_ring`/`byte_ring` that grows mid-frame allocates a new
   buffer and retires the old one (`MTLConstants.h:174-190`,
   `MTLConstants.cpp:120-145`). Draws encoded before the growth read the retired
   buffer; the snapshot only sees the new one. The manifest then looks
   self-consistent while describing the wrong bytes. The capture must record the
   generation of every ring and dump every retired generation, or capture at draw
   time.
3. **It has no per-draw record.** The manifest prints the draw-parameter block,
   but not the index offset/count/type, the vertex count/base, the pipeline, the
   vertex descriptor hash, the texture binds, the sampler state, the scissor,
   the viewport, the blend/depth state or the attachment formats. `draw_id` is
   hard-coded to 0 (`MTLGSRender.cpp:3915`), so a dumped draw cannot even be
   joined to the census's global draw id.
4. **It triggers on a human.** The screenshot is written a frame or more after
   the press; the ring history is three frames; the defect is intermittent. A
   press that lands two frames late reaches a clean frame. The fix is a deep
   rolling history plus automatic triggers that fire on invariants, not on a
   person.

## 2. What the 2026-11-03 data actually says

The run has two boots. The first ends at 66.7 s with a burst of guest-thread
access violations and a **66.36 s present gap**; the device-init line prints
again at 132.9 s. That is a hang and a restart, not a slow frame. The second boot
is the one with screenshots.

The seven presses produced title, menu, menu, two Machine Select cars and four
race frames. Crops of both cars and the rear window are clean. So the frames we
dumped are not the defect frames; the defect is intermittent and was not on
screen at any press.

The census is still informative:

- The font atlas `0xc441e080` (BC2, 2048x2048) is uploaded exactly once per boot
  (25.071 s, 154.966 s). There is no re-upload path inside a boot, so a stale
  atlas cannot explain a glyph that appears later in the same boot.
- The car atlas `0xcc4a7f00` (BGRA8, 512x512) is uploaded once at 246.523 s and
  every dword is nonzero. The car draw also binds `0xcc467f00` (BC1, 1024x512),
  whose source has **three distinct dwords** and a half-zero, alternating
  pattern (`841f841f 00000000 841f841f 00000000`). That is a generated or
  placeholder texture, not a photograph. Whether the game expects it is a
  question only a texture dump at the defect frame can answer.
- The font draws are vp 25 / fp 68, four-vertex half-float quads (descriptor
  `0x1a000114`, stride 20, size 3; the first draw is `0x12000110`, stride 16,
  size 2). The layout advance is intact, so the quad is encoded; what is not
  proven is the four positions the GPU fetched.
- The index ring is u16, not u32; the first draw is `0,1,2,2,3,0`. The index
  data is sane in the captured frames.

The data therefore points at **the inputs of a specific draw in a specific
frame**, not at a global format or FIFO fault. That is exactly what the capture
must record.

## 3. The failure-layer matrix

Every defect is a pixel that differs from what the guest asked for. A pixel is
the product of a chain; the table names each link, the observable that proves it
right or wrong, and the evidence the capture must carry.

| Layer | What can be wrong | Discriminating observable | Capture must carry |
|---|---|---|---|
| L0 guest FIFO | draw executed twice/out of order; stale register | draw sequence vs. guest trace | global draw id, frame id, FIFO position |
| L1 draw decode | count/base/primitive/index type wrong | `index_count`, `vertex_count`, `min/max` vs. guest | those fields per draw |
| L2 vertex upload | wrong range, wrong stride, wrong type, wrong swap | uploaded bytes vs. guest source | persistent/volatile bytes, guest CRC, upload CRC |
| L3 index upload | wrong offset/count, widen bug | index bytes, min/max, bounds | index bytes, offset, length, type |
| L4 ring lifetime | CPU overwrote a buffer the GPU still reads | serialise run; generation change | ring generation per draw, retired buffers |
| L5 constants | wrong transform/fragment/instancing block | the block bytes at the draw's offset | transform, fragment, instancing, vertex context |
| L6 pipeline | wrong program, wrong vertex descriptor, key collision | pipeline id + descriptor hash for the same program | vp/fp id, pipeline pointer, descriptor hash |
| L7 texture | wrong upload, stale cache, wrong mip/layer/format | texture bytes at bind vs. guest source | guest address, format, extent, sampler, texture bytes |
| L8 sampler | wrong filter/wrap/LOD/remap | sampler state + the UV | sampler block per unit, sampler object hash |
| L9 raster | scissor/viewport/cull/blend/depth wrong | per-draw state vs. RSX registers | scissor, viewport, blend/depth/raster hash |
| L10 pass/target | wrong attachment, load/store, resolve | target address/format + pass state | colour/depth targets and formats |
| L11 composition | scale/format/barrier wrong | presented surface vs. render target | both images, compose state |
| L12 present | wrong surface/double-buffer | present address vs. frame | present address, drawable, surface image |
| L13 concurrency | only appears with overlapping frames | serialise run | timestamps, slot state, GPU feedback |

The matrix is exhaustive in the sense that a pixel cannot depend on anything
outside it: geometry, constants, pipeline, texture, sampler, raster, target,
composition, present. If the capture carries one row per link for a defect
frame, no hypothesis is left standing without an observation.

## 4. The capture data model

### 4.1 Per-draw record (always on at level 5, ring of `history_depth` frames)

One fixed-size POD per subdraw, written at encode time, not reconstructed at
flip time. Fields, grouped by matrix row:

- identity: `global_id`, `frame_id`, `subdraw`, `command`, `primitive`, flags
  (indexed, rebase, interpreter, volatile).
- geometry: `index_type`, `index_count`, `index_offset`, `index_length`,
  `vertex_count`, `vertex_base`, `vertex_index_offset`, `vertex_base_index`,
  `min_index`, `max_index`, `persistent_base/bytes`, `volatile_base/bytes`.
- pipeline: `vp_id`, `fp_id`, `pipeline_key`, `pipeline_state` pointer,
  `vertex_desc_hash`, `xform_offset`, `vs_context_offset`, `fs_context_offset`,
  `fs_constants_offset`, `fs_texture_base_index`, `fs_stipple_offset`,
  `blend_values_index`.
- raster/target: four colour targets and the format, depth target and format,
  width/height, scissor, viewport, `blend_hash`, `depth_hash`, `raster_hash`.
- vertex descriptor: the 16 `(descriptor, offset)` pairs the shader reads.
- textures: per unit, guest address, format, width, height, sampler hash.
- content: a 32-bit hash of the persistent range, the volatile range, the index
  range, the transform block, the fragment-constant block and the sampler block.

The record is ~690 B. At ~1000 subdraws/frame and 12 frames that is ~8 MB of
RAM, which is affordable. It is written only at level 5.

### 4.2 The ring snapshot

The snapshot carries every ring the shader can read, not six of them:

- streams: persistent, volatile, index, instructions, overlay vertices, overlay
  uniforms;
- block rings: vertex context, draw parameters, fragment context, sampler info,
  stipple, blend values, fill values, vertex draw params, fragment constants,
  transform constants, instancing constants, instancing lookup;
- the per-draw records.

Each snapshot records the generation of every ring. A ring that grew mid-frame
is dumped with all of its retired buffers, named `-genN`, so the bytes a draw
before the growth read are on disk.

### 4.3 Textures

For a captured frame, record the `MTL::Texture*` and guest address of every
bound unit at draw time, and dump the texture at the flip, after the pass, before
`on_frame_end` retires it. A texture dump is the only way to separate "the
shader sampled the right texel" from "the texel was wrong". The dump is capped
per frame and per unit.

### 4.4 The two images

A captured frame writes both the render target (the output of the RSX pass) and
the presented surface (the output of the compose/scale pass). A defect in the
first is a draw defect; a defect only in the second is a composition defect.

### 4.5 Automatic triggers

The human press stays, but it is no longer the only trigger:

- **degenerate quad**: a four-vertex half-float draw whose four positions have
  zero area, a non-finite component, or a UV outside the atlas;
- **index out of range**: an index >= the vertex count;
- **upload mismatch**: the hash of the uploaded range differs from the hash of
  the guest range at the same moment;
- **ring growth**: a ring grew mid-frame (the snapshot is incomplete);
- **descriptor drift**: the same program and vertex layout produced two
  different descriptor hashes;
- **present gap**: no flip for more than a few seconds (the hang).

A trigger dumps the rolling history and the following frames, exactly like a
press, and names itself in the manifest.

### 4.6 The hang

The 66 s gap is a separate defect but it is cheap to instrument: a watchdog
thread stamps the last flip, and when the gap exceeds a threshold it records the
last present, the recorder slot state, the draw id and the probe scope. The RSX
thread dumps its history when it unblocks. The watchdog never touches the
renderer's state.

## 5. What this buys

For a captured defect frame the analysis can answer, from the dump alone:

1. Which draw covers the defect pixel (project the recorded vertices through the
   recorded viewport and scissor).
2. Whether the vertex bytes were right at upload (compare with the guest source).
3. Whether the GPU read the bytes the CPU wrote (generation and retired buffers).
4. Whether the transform/fragment constants at the draw's offset were right.
5. Whether the pipeline and vertex descriptor matched the program.
6. Whether the texture at the bound unit held the texels the guest wrote.
7. Whether the sampler state could produce the observed smear.
8. Whether the raster state could produce the observed clip.
9. Whether the render target or only the composed image was wrong.

If the answer is "none of the above", the remaining link is concurrency, and the
serialise run decides it.

## 6. Implementation order

1. Extend `frame_snapshot` to every ring and to the per-draw record; add ring
   generations and retired generations.
2. Write the per-draw record at encode time; stop hard-coding `draw_id`.
3. Deepen the history and make the press dump the frames before it immediately.
4. Add the texture list and the two-image dump.
5. Add the automatic triggers and the watchdog.
6. Add the analysis scripts: `capture.py` (parse and project),
   `diff.py` (defect vs. clean), `texture.py` (decode and crop).

The first three are the ones that close the current blind spots; the last three
are the ones that make the next run conclusive.
