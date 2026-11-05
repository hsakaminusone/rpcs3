# GAP-PLAN — the confirmed deviations from Vulkan/OpenGL and the order to close them

Date: 2026-10-25. Scope: `rpcs3/Emu/RSX/MTL`. The RSX is the specification; Vulkan and OpenGL
are evidence. Every row below was re-read in the code, not copied from the audit tables: the
dated audits (`DEVIATIONS-*.md`) were written between 2026-10-20 and 2026-10-22 and a dozen of
their rows have since been fixed without the row being updated. This file is the live list.

## 1. Audit result of the GPU label write (L131, SY13)

The implementation was re-read line by line. **Three liveness defects were found after the first
device runs (L134, L135, L136/L137); all three are fixed.** The checks that matter:

- Byte order: `std::bit_cast<u32, be_t<u32>>(data)` is the same expression VK writes
  (`VKGSRender.cpp:1726`); `be_t` stores swapped on this host, so the copied bytes are the
  big-endian guest word, which is what `vm::write<atomic_t<RsxSemaphore>>` would have stored.
- Order: one queue-scope `All → Blit` barrier per batch, emitted in the same encoder as the
  copies, before them. Nothing encoded earlier can pass the write; a label is a release, so
  nothing later may need it earlier.
- Liveness: `commit_pending_labels` commits without waiting, and the slot is re-entered by the
  next `open_pass`/`open_compute`/`write_label`/`flush_for_readback`, which is the wait that
  already existed. `commit_pending_labels` calls `commit_slot` directly, so it neither signals
  the drawable nor advances the ring; `submit` handles the "already committed" case.
  **The first defect (L134):** the only caller was `on_semaphore_acquire_wait`, and this guest
  waits on the label without an RSX acquire. The RSX thread parks in `run_FIFO`/`FIFO_EMPTY`, the
  batch stays in the open frame, and the producer never publishes the next chunk.
  `release_GCM_label` now commits the back-end batch at the release; the guest wait is satisfied
  by the GPU, not by a later hook.
  **The second defect (L135):** the release is committed, but the adjacent acquire reads the old
  guest word before the GPU copy and takes the `sema == arg` fast path, so it never waits. The
  recorder now keeps the label address of every write in the command buffer;
  `semaphore_acquire` calls `sync_label(address)`, which commits the buffer carrying that address
  and waits only for that slot's feedback. The acquire then re-reads the word and runs its normal
  spin.
  **The third defect (L136/L137):** the word never appeared even after the wait. The Metal device
  imports the guest window at 4.58 s, the RSX reports page is mapped at 18.96 s, and a no-copy
  import is fixed at creation, so the GPU wrote the old physical page. A standalone remap
  experiment reproduces it. `resource_manager::label_buffer` now caches one 64 KiB import per
  label page, created after the page exists; the GPU write and the CPU word are the same page.
- Encode boundaries: `open_pass` flushes and then closes (line 461-462), so a batch never splits
  a recorded pass; every present/overlay path goes through `open_pass`/`open_compute`.
- Reset safety: a staging region is reset only at `begin_frame`/`reenter_slot`, both after the
  slot's feedback (or after a rebuild). The shared 48 KB staging buffer is `StorageModeShared`.

Three limitations are accepted and documented (SY13):

1. **Visibility window.** A label not waited on becomes visible at the next commit. The guest
   can only react to a release, so this is safe; but a guest that resets the same label on the
   CPU without waiting could have its reset overwritten. That is already a race on hardware.
2. **Abandoned slot.** On a feedback timeout the slot is rebuilt and its staging region reused
   while the abandoned command buffer may still read it. Same class as SY2; abnormal path only.
3. **Region wrap.** More than 4096 labels in one frame forces a commit + wait. Not expected in
   this title; measured as `sync/label write/region wrap` at probe level 2.

**Device check (runs C/D/K/L, 2026-10-25/26).** The boot path latched 2 GPU label writes and released
in 0.966 ms total, but the profile also has **zero** `sync/semaphore waits`: the guest never reached
`on_semaphore_acquire_wait`, so the batch was staged and never submitted, and the producer stayed at
`0xcb09d4` (L134). The 2 latches were staging events, not visible writes. The release now commits the
back-end batch (L134). Run K captured the boot chunk tail and showed the release immediately before
an acquire at `arg=0` that reads the old `0`; the acquire now calls `sync_label` and waits for the
buffer before it re-reads (L135). Run L showed the wait and the feedback, but the word was still `0`;
the device import predates the label page's mapping (L136), and the label write now uses a per-page
import created after the page exists (L137). The race's larger label count, the visibility-window
limit and the region-wrap limit still need a run that reaches 3D.

## 2. Confirmed remaining gaps

Ordered by what to do first, not by severity alone. `id` cross-references the audit file.

### Tier 1 — the showstopper (not a VK/GL parity row)

| id | gap | effect | closure |
|---|---|---|---|
| L115/L120/L126/L127/L128/L132/L133/L145 | The FIFO parser had one numeric frontier for two command buffers. `fetch_u32` accepted GET at or past PUT, the cache served the word at PUT, and a jump into the other buffer was taken before its target was published (L125–L127). GET also acknowledged the prefetched block end, so the guest recycled the chunk under the parser (L145). | Desync, then `Dead FIFO`. The only remaining title-level failure. | **Fixed in code (§10.14, §10.15, §10.19).** `stream_tracker` keeps `{base, frontier, linear}` per stream; PUT is attributed per stream; `fetch_u32` refuses `addr >= frontier`; the word at the frontier is consumed only as a jump/call/return link; the cache is capped at the frontier; a method packet is dispatched only when fully published; jump/call/return feed the tracker. Boot/recovery stay linear. Run C found the `put == addr` early return still bypassed the link rule; the equality now takes the frontier path and a non-link park drops the cache. Run D's park word was `0x0`, so the run-C stall was the unsubmitted label, not the link (L134); the equality fix stays for the healthy case. Run T showed the remaining half: GET published the prefetched block end and the guest reused A during the 188 ms label acquire, so the link became data. `sync_get()` now publishes `m_fifo_pos`, `cellGcmCallback` writes the jump before PUT, the publish-sample layer is deleted, and fast mode enforces the same frontier. 12 `FifoStreams` cases, 516/516. **Run U confirmed it:** the live link at `A+0x9d4` was consumed, the parser entered B, and 2,892 handovers ran with no `Dead FIFO`. |
| L131/L134/L135/L136/L137/SY13 | The label write is now on the GPU, the back-end batch is submitted at the release, an acquire on the address waits for that slot before it re-reads, and the write targets a per-page import created after the page exists. | Removes the 57.9 ms drain and takes the RSX thread off the producer frontier (L129, L38); without the submission the boot producer stalls (L134), without the acquire wait it reads the pre-release word and parks (L135), and without the per-page import the GPU writes a physical page the CPU no longer maps (L136). | **Run C/D found the unsubmitted batch:** 2 GPU latches, 1 batch, 0.966 ms, but zero `sync/semaphore waits` and no next `PUT`; the boot never reached 3D. `release_GCM_label` now commits the back-end batch. **Run K found the stale acquire:** the boot chunk's release is followed by an acquire at `arg=0` that reads `0`; `sync_label` now waits for the buffer carrying the address. **Run L found the wait runs but the word is still `0`:** the slot feedback arrives yet `sudo=guest=0`, and the device import predates the label page's mapping; labels now use a per-page import, with a same-commit readback in the next build. The race's larger label count remains the confirmation. |

### Tier 2 — performance, shared by every frame

| id | gap | effect | closure |
|---|---|---|---|
| DR10 | Every draw clause opens and closes a render pass (`MTLGSRender.cpp` open at `end()`, close after the draw). Each pass loads and stores every attachment. | The largest remaining GPU cost on a multi-clause draw. VK reuses one pass (`VKDraw.cpp:1155-1171`). | Keep the pass open across consecutive draws with the same attachment set and load/store action. Close on: attachment change, clear, readback, label commit, flip, ZCULL re-apply. Valid because a split after a clear stores the cleared image and the next pass loads it. |
| SY10 | Colour resolves on every MSAA clause pass, then `surface::resolve` can resolve again on demand. | N full colour resolves per N-clause draw, plus a possible second one. | Store colour only; resolve once in `surface::resolve` when `require_resolve` is set. Depth already does this. |
| VX4 | The vertex upload writes `[min_index, max_index]` in full on every subdraw. | Repeated host writes for split clauses. GL uploads only the missing tail. | Per-ring range cache keyed by storage address + attribute layout + slot, invalidated on a guest write to the range. Measure before keeping it. |
| SY7/SY8/SY11 | Cross-frame hazards and the residency publication model are unproven. | Unknown: could be a correctness hole, could be free. | One GPU capture: two frames writing/reading one surface with no CPU wait; and one residency capture. Do not change code before the capture. |

### Tier 3 — wrong results reachable at default settings

| id | gap | effect | closure |
|---|---|---|---|
| SF14 | A packed 16-bit surface written back to guest memory moves the red and blue fields and the `x1r5g5b5` alpha bit, because the host format is not in guest bit order. | Wrong pixels in a CPU read of a 16-bit target. | Expand/repack in a compute pass on write-back (`REVIEW R2`). The shared `TextureUtils` already has the expand rule. |
| SY4 | Fixed in the binder (null texture/sampler on a zero transition) but not confirmed on device. | Stale binding if the transition is missed. | Confirm with one draw that unbinds a unit. If `setTexture(null)` is rejected at runtime, switch to the 1x1 null image VK uses. |
| F12 | The RSX sample mask is not applied: no encoder setter, no `[[sample_mask]]` in the MSL. | Alpha-to-coverage / sample-mask effects draw with the full mask. | Emit the mask test in the fragment epilogue and set the encoder mask. Needs a title that uses it. |
| TX20 | `R6G5B5` stores six bits of red; `B5G6R5Unorm` stores six bits of green. No permutation recovers the guest field. | Wrong colour for that texture format. | Expand to `RGBA8Unorm` at upload (`REVIEW R3`). |
| TX10 | A texture-cache copy with differing extents is logged and skipped, leaving the temporary uninitialised. | Wrong sample when a scaled blit reaches the cache. | A compute sampler pass (or the shared `upload_scaled_image` + blitter, SF8). Gated on a title; at 100 % scale it should not fire. |
| TX11 | The surface compatibility test requires the exact host pixel format where VK/GL accept a set. | Forces extra copies (and steers into TX10). | Per-format compatible-host set, including depth-as-colour. |
| E7/F14 | The depth-bounds test is dropped (the encoder setter traps on this driver). | Guest effect lost where enabled. | Fragment-shader epilogue using the captured bounds (LLD-depth-rop §3.4), if a title enables it. |
| TX8 | A cube that needs a view now keeps `TextureTypeCube`, but the six-slice path is unverified. | Wrong cube sampling if the view path is hit. | One cube probe. Likely already correct (view now keeps the source type). |
| SF10 | A read-back on a resolution-scaled surface is bounded by both extents (no overrun) but not downscaled. | Wrong content when scaled. | Detect `is_scaled` and downscale first, as VK does. Needs a run at non-100 % scale. |

### Tier 4 — settings and low-impact parity

| id | gap | effect | closure |
|---|---|---|---|
| F7 | `host_label_synchronization` is never read; `supports_host_gpu_labels` is forced true. | The checkbox does nothing. Deliberate: the GPU path is correct and is the only one worth having. | Either honour it as "GPU labels on/off" or mark the setting as having no effect on Metal. |
| F9 | `force_high_precision_z_buffer` is ignored; depth is always `Depth32Float`. | Setting has no effect. | Map to `Depth32Float` vs `Depth24Unorm_Stencil8`-class selection where legal. |
| F8 | `disable_vertex_cache` has no effect because uploads are per-draw. | Setting has no effect. | Follows VX4; make the cache honour the switch. |
| F10 | The debug overlay is created but never given text. | The RSX statistics pane is blank, which costs us a diagnostic. | Feed the shared `rsx::overlays` text path. |
| F2 | RCAS sharpening is never read. | The slider does nothing. | Port the RCAS compute pass; F1's nearest path already exists. |
| F3 | FSR is absent. | The upscaler option silently falls back. | Separate unit of work; reuse the shared upscaler interface if it can target Metal 4. |
| TX5 | A custom border colour is reduced to the three Metal values. | Rare sampler mismatch. | KK's LUT/constant-buffer technique (`DEVIATIONS-TEXTURE K3`). |
| TX14 | An unknown guest format logs and becomes `BGRA8Unorm`. | Samples a wrong format instead of failing the draw. | Fail the lookup and skip the draw with one clear message. |
| SF8/F11 | No GPU blit path; `scaled_image_from_memory` returns false. | Two blit settings are no-ops; the CPU fallback is correct but slow. | Shared `upload_scaled_image` + one `blitter`. |
| Q7 | No GPU counter heap, so no per-pass GPU time. | Diagnostic only. | Optional counter heap behind the profiler switch. |
| D4 | Flat shading is unreachable; Metal's provoking vertex is the first, RSX's is the last. | Flat-shaded effects interpolate smoothly. Parked as G1. | Index rotation for lists, vertex multiplication for strip/fan. Only if a title flat-shades. |
| F5 | No shader interpreter; a pipeline compile stall has no fallback. | Cold-shader hitches. | `LLD-shader-interpreter.md` is complete and audited; implementation pending (P2). |

### Gates already scheduled

| id | what it needs |
|---|---|
| DR9/S5 | Device acceptance of the asynchronous pipeline build (code landed). |
| S6/A9 | The in-tree GLSL-to-MSL hop has no test reach; first light is the verification. |
| PF8 | A nil drawable drops the frame; confirm the re-acquire on a window resize. |
| PF12/I7 | The present-buffer content needs a flicker to reproduce. |

## 3. Rows that are not Metal gaps

Re-read in the reference before dropping:

- **C4 point polygon mode.** VK hardcodes `VK_POLYGON_MODE_FILL` (`graphics_pipeline_state.hpp:33`);
  GL draws points. Metal matches VK, so this is not a Metal regression.
- **C10 dither.** Metal and VK both drop it; only GL enables `GL_DITHER`. Not a Metal gap.
- **C7 polygon-offset enables.** `capture_depth_bias` returns `poly_offset_fill_enabled()` and the
  encoder state zeroes the bias otherwise, which is the only enable Metal can honour without
  per-primitive state. Line topology still keys on the fill bit; low, not worth a row.
- **D8/A8 conditional-render slot.** Dead ABI; the CPU skip path is correct. No action.
- **D11/A11 inlined `max_index`.** Metal matches GL; only VK differs.

## 4. Stale rows corrected in this pass

| file | row | was | now |
|---|---|---|---|
| `DEVIATIONS-DRAW.md` | D1, D2 | open, H | fixed: one draw per clause subrange |
| `DEVIATIONS-DRAW.md` | D3 | open | fixed: the index visitor widens a 16-bit strip to `u32` |
| `DEVIATIONS-DRAW.md` | D6, D9 | open | fixed: NOP drain; async pipeline build |
| `DEVIATIONS-DRAW.md` | D5 | open | fixed, device-confirmed (L116) |
| `DEVIATIONS-SURFACE.md` | S1–S8, S12, S13 | open | fixed (`REVIEW-FINDINGS` A3, B2, B4, B14, F24, TX19) |
| `DEVIATIONS-TEXTURE.md` | TX3, TX9 | open | fixed |
| `DEVIATIONS-TEXTURE.md` | TX7 | open | fixed: the view keeps the source type (3D included) |
| `DEVIATIONS-SYNC.md` | SY1, SY2, SY3, SY5, SY9 | open | fixed |
| `DEVIATIONS-SYNC.md` | SY4 | partial | fixed in the binder, device confirmation pending |
| `METAL4-TODO.md` | DR5 | needs A/B | fixed, device-confirmed |
| `METAL4-TODO.md` | F7 | "forced false" | forced **true**; the setting is what is ignored |
| `METAL4-TODO.md` | TX7, SY4 | open/partial | fixed |

<!-- doc-end -->
