# Metal 4 backend — code audit, 2026-11-13

An independent read of the Metal 4 backend against the Vulkan and OpenGL backends, looking for
behaviour that the guest can reach and that the other two backends do not have. Items that were
suspected and then found to match Vulkan are listed in §6, so they are not re-litigated later.

**Status: v2.** Sections 2–9 are code-level findings, established by reading the tree and the two
reference backends. Section 10 is evidence from the 2026-11-13 probe run, and section 11 is a read
of the kosmickrisp Vulkan driver that RPCS3 ships. Nothing here has been reproduced under a debugger
or fixed; each item carries what would settle it. The one crash in §10 is the only observed failure.

Scope of the read: `rpcs3/Emu/RSX/MTL/` (all 67 files, with the diagnostics-heavy paths skimmed),
plus the shared files the backend changed (`RSXFIFO.cpp/.h`, `RSXThread.cpp`, `Core/RSXRenderBackend.h`,
`Core/RSXDisplayEngine.cpp`, `Common/surface_store.h`, `Common/TextureUtils.cpp`).

---

## 1. Summary

| ID | Severity | Area | One line |
|---|---|---|---|
| H1 | High | surface write-back | `surface::write_to_memory` does not preserve byte-range semantics: partial rows are dropped, the destination column is lost, and the scaled path is absent |
| H2 | Medium (latent) | label path | the label staging batch can be dropped or committed on a closed command buffer; only call-site ordering prevents it today |
| H3 | Medium | texture upload | a failed staging allocation is dereferenced unconditionally |
| M1 | Medium | labels | per-page label imports are cached for the process lifetime and are never invalidated on unmap/remap |
| M2 | Medium | texture upload | no GPU-side conversion is ever enabled, so every upload is CPU-expanded into a fresh shared buffer (Vulkan enables it above 1 KiB) |
| M3 | Medium | read-back | the depth/packed read-back paths stall the RSX thread through `flush_for_readback` once per surface per DMA block |
| M4 | Low | present | the CPU present-upload path leaves a row tail uninitialised when the guest pitch is shorter than the row |
| M5 | Low | ZCULL | the counter free list is pushed without a capacity check |
| M6 | Low | diagnostics | `frame_slot::debug_commit_time` is written on the RSX thread and read in the commit-feedback callback without synchronisation |
| M7 | Low | inline draws | the inline-array visitor reports `max_index = vertex_count` where every other path reports the last index, so one extra vertex is fetched and uploaded per draw |
| X1 | Risk | shared FIFO | the FIFO parser's acknowledgement and dispatch rules were rewritten in shared code for a Metal-only symptom; Vulkan and OpenGL inherit the change |
| P1-P7 | Perf | — | see §9: upload conversion, staging churn, the heap free list, read-back stalls, residency commits |
| R1 | High | live run | the 2026-11-13 run crashed on the RSX thread inside `emit_geometry`, reading `null + 0x28a0` |
| P10 | Perf | live run | one render pass per draw in practice: 32 688 pass opens for 31 977 draws |
| P11 | Perf | live run | the GPU frame is 6.4 ms while the display gap is 35 ms; acquire stalls on half the frames |
| O1 | Open | live run | the capture's vertex trace decodes `NaN` positions for 1 024 of the traced vertices |
| V1 | Medium | Vulkan path | passthrough DMA is enabled without checking the host-pointer alignment the driver requires |
| V2 | Verify | residency | our heaps are committed to the residency set but `requestResidency()` is never called |
| V3 | Note | barriers | our barrier scope is *stricter* than the reference driver's; do not "match" it |

---

## 2. H1 — `surface::write_to_memory` does not preserve byte-range semantics

**Where:** `rpcs3/Emu/RSX/MTL/MTLSurface.cpp:552-700` (the whole function), with the decisive
arithmetic at `:565-567` and `:585`.

**What the shared code asks for.** `surface_store::download_surface` / the DMA path hands the
backend a *byte range* pair (`Common/surface_store.h:995-1014`):

```cpp
if (this_range.start < block_range.start) { src_offset = block_range.start - this_range.start; dst_offset = 0; }
else                                      { src_offset = 0; dst_offset = this_range.start - block_range.start; }
write_length = std::min(max_length, block_length - dst_offset);
Traits::write_render_target_to_memory(command_list, bo, surface, dst_offset, src_offset, write_length);
```

Both offsets are arbitrary byte offsets derived from address ranges. Neither is rounded to a row,
and `write_length` is an arbitrary length, not a whole number of rows.

**What Vulkan does.** `VKRenderTargets.h:633-698`:

1. the surface is copied into a scratch buffer with `bufferRowLength = rsx_pitch / bpp`, so the
   scratch holds the surface in guest row layout;
2. a plain `vkCmdCopyBuffer(scratch, bo, {src_offset_in_buffer, dst_offset_in_buffer, max_copy_length})`
   moves exactly the requested byte range.

Byte-range semantics are preserved by construction, including a range that starts or ends inside a
row.

**What Metal does.**

```cpp
const u32 row    = src_offset / rsx_pitch;              // MTLSurface.cpp:565
const u32 column = (src_offset % rsx_pitch) / get_bpp(); // :566
u32 rows         = write_length / rsx_pitch;             // :567
...
const u32 copy_width = std::min<u32>(width() - column, rsx_pitch / get_bpp()); // :585
encoder->copyFromTexture(source, 0, 0,
    MTL::Origin::Make(column, row, 0), MTL::Size::Make(copy_width, rows, 1),
    target, dst_offset, rsx_pitch, rsx_pitch * rows);    // :694-697
```

Three defects follow from this, all reachable from guest behaviour:

1. **A range shorter than one row writes nothing.** `rows = write_length / rsx_pitch` is zero, and
   `:568` returns early. A guest that downloads a partial row (a sub-rectangle smaller than the
   surface pitch, the common case for a one-line or small-block transfer) reads whatever was in
   guest memory before. Vulkan copies all `max_copy_length` bytes.
2. **The destination column is dropped.** `column` is applied to the *source* origin only. The
   blit writes row 0 at `dst_offset`, so unless `src_offset % rsx_pitch` equals
   `dst_offset % rsx_pitch` (which the shared code does not guarantee — and cannot, since
   `dst_offset` is relative to a *different* base), every row lands shifted by
   `(dst_offset % pitch - src_offset % pitch)` bytes. The destination is a guest address; a shift
   is visible to the guest as a diagonal smear and as wrong bytes in the corners.
3. **The scaled case is unimplemented.** With `resolution_scale_percent > 100` the host texture is
   larger than the guest surface. `width()` is the host width, `copy_width` is bounded by the guest
   row, and the blit is 1:1, so the write-back copies the top-left corner of the upscaled image at
   guest scale. Vulkan explicitly handles this (`is_scaled` → `copy_scaled_image` into a scratch at
   guest extent, `VKRenderTargets.h:645-655`); Metal's own comment at `:580-584` says "The scaled
   case still needs a downscale". A DMA read-back while resolution scaling is on is therefore
   corrupt by definition.
4. The same function early-returns without a word when `row >= source->height()` or
   `column >= width()` (`:568-583`), so an out-of-range request is silently a no-op rather than a
   clipped copy.

The depth branch (`:594-647`) and the packed-16 branch (`:649-671`) inherit (1) and (2) as well;
they fix up only the *format*, not the range.

**Why this is high.** Any title that reads a render target back through the guest DMA path — the
classic "read the framebuffer to system memory" workflow, used by screenshots, video-out, some
post-processing and many save-icon / menu effects — goes through this function. The failure is not
a one-pixel artefact: the whole transferred block is misplaced or absent.

**Fix.** Mirror Vulkan: copy the surface into a scratch buffer at guest row pitch, then do a
`copyFromBuffer(scratch, src_offset, target, dst_offset, write_length)`; add the scaled downscale
first when `width() != surface_width`. `MTL::BlitCommandEncoder`/MTL4 compute `copyFromBuffer` is a
byte copy and has none of the row/column constraint.

**Confidence.** Certain from the code; the reachable cases are the partial row and the non-aligned
destination. A device test that downloads a 1-row block and a block starting mid-row from a surface
would confirm both in one run.

---

## 3. H2 — the label batch has no "command buffer is open" invariant

**Where:** `MTLCommandRecorder.cpp:630-675` (`write_label`), `:677-706` (`commit_pending_labels`),
`:782-850` (`flush_label_writes`), `:600-628` (`open_copy_encoder`), `:528-560` (`reenter_slot`).

Two independent holes, both currently hidden by call-site ordering:

**(a) The batch can be dropped after `first` was computed.** `flush_label_writes` computes the
staging offset first and opens the encoder second:

```cpp
const u32 first = slot.label_staging_offset - m_pending_count * sizeof(u32); // :793
MTL4::ComputeCommandEncoder* encoder = open_copy_encoder();                  // :794
```

`open_copy_encoder` re-enters the slot when its command buffer is closed
(`:614-620`), and `reenter_slot` resets `label_staging_offset = 0` and `m_pending_count = 0`
(`:536-541`). After that, the copy loop iterates `m_pending_count` times — zero times — and
`:846` leaves the count at zero. The label addresses already pushed onto `slot.labels`
(`:660`) survive, so a later `sync_label` waits on a write that was never encoded.

**(b) `commit_pending_labels` calls `endCommandBuffer()` without checking `buffer_open`**
(`:699`). The frame's `submit()` also ends the buffer and advances `m_slot` (`:419-425`), which
leaves the new slot's `buffer_open == false` while `m_reenter_pending == false` — exactly the
state in which `sync_label` (`:735-742`) will call `commit_pending_labels`.

**Why it is latent, not live.** `release_GCM_label` calls `open_frame()` before `write_label`
(`MTLGSRender.cpp:7136`), and `on_semaphore_acquire_wait` checks `m_frame_open`
(`MTLGSRender.cpp:6963`), so today the buffer is open whenever a label is staged. Both facts are
call-site properties, not invariants of the recorder, and the recorder is the class that documents
itself as "the single chokepoint … one way to open a pass, one way to submit"
(`MTLCommandRecorder.h`). Any new label site, or a future `release` path that runs after
`submit()`, reproduces the silent drop, whose symptom is a parked FIFO and a guest hang, not a
warning.

**Fix.** Make the invariant explicit: an `ensure(slot.buffer_open)` in `write_label` and
`commit_pending_labels`, and `reenter_slot()` at the top of `write_label` when the slot is closed —
the same guard `open_pass()` and `open_copy_encoder()` already carry. Then derive `first` *after*
the encoder is open.

**Confidence.** Certain that the code does this; the question is only reachability, which needs
either a new call site or a counter of "label staged into a closed buffer" to prove.

---

## 4. H3/M1/M2/M3 — resources, labels, uploads, read-back

### H3 — unguarded staging allocation in `upload_texture_data`
`MTLTextureCache.cpp:172-174`:

```cpp
MTL::Buffer* staging = cmd.resources().new_buffer(size, MTL::StorageModeShared);
rsx::io_buffer io_buf(staging->contents(), size);
```

`clear_texture` guards the same call (`:453-457`) and the surface paths guard theirs
(`MTLSurface.cpp:330-336`, `:606-612`). This one does not. `new_buffer` returns null when heap
growth or `newBuffer` fails (`MTLResourceManager.cpp:300-316`), so the first allocation failure
under memory pressure is a null dereference on the RSX thread instead of a skipped upload.
**Fix:** one `if (!staging) { continue; }`.

### M1 — label imports are never invalidated
`MTLResourceManager.cpp:353-390` keeps one `newBuffer(vm::g_sudo_addr + base, 0x10000)`
per 64 KiB page in `m_label_pages`, for the process lifetime. `MTLGSRender::on_unmap_memory_range`
(`:7523-7526`) is deliberately empty ("Metal has no DMA-side unmap"), and `on_invalidate_memory_range`
never touches `m_label_pages`. L136 in the handoff made the import per-page precisely because a
window import is fixed at creation; the same argument then applies to *this* import when the guest
unmaps or re-maps the page. On a 1:1 `g_sudo_addr` projection the host address is stable, so the
practical exposure is small — but the code has no hook that would notice a change, and the comment
claims the import is made "after the page exists" as if that were sufficient for all time.
**Fix:** drop the page's import in `on_unmap_memory_range` (and on `invalidate_range` with the
`unmap` cause), or key the cache on `(base, vm::get_super_ptr(base))` validity.

### M2 — uploads never use GPU-side conversion
`MTLTextureCache.cpp:113-121` sets `supports_byteswap = false`, `supports_vtc_decoding = false`,
`supports_hw_deswizzle = false`, `supports_zero_copy = false`, once per call. Vulkan enables
byteswap, hw-deswizzle and zero-copy for any subresource of at least 1 KiB
(`VK/VKTexture.cpp:1174-1181`), which turns the common case into a straight GPU copy of guest
memory. Metal does the expansion and the deswizzle on the CPU for every upload, then allocates a
fresh shared staging buffer per subresource (`:172`) and defers its release. For the DXT-heavy
title in the handoff (≈580 k compressed bind format instances, L28) this is the single largest
avoidable CPU cost on the draw path.
**Fix:** follow Vulkan — enable the caps above a size threshold and copy straight from
`guest_buffer()` when the guest block is already in the host layout.

### M3 — read-back stalls the RSX thread per transfer
`MTLSurface.cpp:640` and `:665` call `cmd.flush_for_readback()`, which commits the open command
buffer and waits for its completion value (`MTLCommandRecorder.cpp:445-470`). That is correct for
one read-back and catastrophic for a sequence: the depth-format and packed-format cases are the
ones that go through it, so a title that downloads depth every frame (shadow maps, depth-of-field,
some menu effects) serialises the RSX thread against the GPU once per transfer. Vulkan never blocks
here: it copies to a scratch buffer and lets the guest's own later read consume it.
**Fix:** stage all pending read-backs into one shared buffer, commit once, and let the *next*
transfer's staging pass wait — or hand the wait to the requesting thread, as `on_access_violation`
already does for guest faults.

---

## 5. M4/M5/M6 — smaller items

- **M4 (present, CPU upload).** `MTLGSRender.cpp:6292` does
  `copy_bytes = min(row_bytes, pitch)` and then copies row by row into a tightly packed staging
  row of `row_bytes`. When the guest pitch is *shorter* than `width * pixel_size`, the tail of every
  row is never written and the presented image shows uninitialised shared memory. OpenGL avoids the
  question by declaring `row_length(pitch / 4)` to the unpack state and letting the driver place the
  bytes (`GL/GLPresent.cpp:127-133`). Low impact (a short pitch is illegal-ish), but it is
  uninitialised data on screen rather than a deterministic artefact.
- **M5 (ZCULL).** `MTLZCULL.cpp:115, 342, 349` push onto `m_free` without checking
  `m_free_count < counter_capacity`. The `owned` flag stops a double release today
  (`:341` sets it before the branches), but nothing in `release_counter` asserts it, and the pool is
  a fixed array in a header — a second release path would be an out-of-bounds write in the RSX
  thread, not a warning.
- **M7 (inline draws, one extra vertex).** `MTLGSRender.cpp:1013-1016` computes
  `vertex_count` from the inline stream and then sets `result.max_index = vertex_count`, while
  `draw_array_command` (`:868`) and the indexed visitor set `max_index` to the *last* index. The
  draw itself is unaffected — `drawPrimitives` takes `vertex_draw_count`, which is correct — but
  `emit_geometry:5090` derives the upload size as `(max_index - min_index) + 1`, so every inline
  draw fetches and copies one vertex of guest memory it never reads. Harmless today, and it is a
  trap for the next reader: the moment anything derives the draw count from `max_index` (as the
  other two visitors do), inline draws grow a phantom vertex.
- **M6 (diagnostics race).** `frame_slot::debug_commit_time` is a plain `f64` written by `submit()`
  on the RSX thread (`MTLCommandRecorder.cpp:436`) and read by `on_feedback` on the driver's
  callback thread (`:930`, `:944`). Harmless numerically, but it is a data race in a build that
  ships with `MTL_DIAGNOSTICS=1`, and the neighbouring `generation` field is atomic for exactly
  this reason.

---

## 6. X1 — the shared FIFO rewrite is a cross-backend change

`RSXFIFO.cpp` was rewritten on this branch for a symptom that only Metal showed: the label write
could not be ordered against the work it released, so the parser could race the producer. The
mechanisms changed in *shared* code are:

| change | where | affects |
|---|---|---|
| `sync_get()` publishes `m_fifo_pos` (the executed position) instead of the end of the prefetched block | `RSXFIFO.cpp:47-56` | VK, GL, Metal |
| a packet is dispatched only when its whole argument block is published (`m_next_cmd_entry > put` → `FIFO_EMPTY`) | `:1073-1086` | VK, GL, Metal |
| the per-stream frontier is gone; PUT is the only gate, and a cursor *ahead* of PUT is walked ungated | `:993-1000`, `:1044-1050` | VK, GL, Metal |
| `fetch_u32` serves a cache hit only inside `[cache_get, cache_put)` snapshotted at fill time | `:487-500` | VK, GL, Metal |
| the word at `PUT` is never treated as a link (`put == m_fifo_pos` → `FIFO_EMPTY`) | `:955-990` | VK, GL, Metal |

Each of these changes the *timing* contract between the guest producer and the parser. The unit
tests added with them (`rpcs3/tests/test_rsx_fifo_streams.cpp`) model the two-command-buffer
handshake, which is exactly the pattern the tests were written around; they do not model a
single-buffer title, a `CALL`/`RETURN`-heavy stream, or a producer that rewrites a ring in place.

**Risk.** A perf-only regression of the "publisher publishes less often" kind is invisible to the
tests; a correctness regression would appear as a title that boots on `master` and desyncs here.
The branch currently has no Vulkan/OpenGL boot A/B of the same title recorded against these five
rules, and the handoff's evidence for them is Metal-only (`RR7`).

**Recommendation.** One Vulkan run and one OpenGL run of a title that exercises the two-buffer
handshake, on `master` and on this branch, with the desync/probe counters compared. If the numbers
are identical, this is a non-issue and worth one line in the handoff.

---

## 7. Suspected, then found to match the reference backends

These were checked and are **not** defects; recording them stops the next reader repeating the work.

| item | verdict |
|---|---|
| `surface::clear_memory` clears colour to `(0,0,0,1)` and depth to `1.0/0xFF` | identical to `VK/VKRenderTargets.cpp:509-518` |
| `write_barrier()` only invalidates the surface store | identical to `VKGSRender.cpp:1814-1818` and `GLGSRender.cpp:1448-1452` |
| `get_surface(access)` resolves MSAA only for a transfer access | identical to `VK/VKRenderTargets.cpp:837-850`, including the "read barrier first" contract |
| `on_framebuffer_bound()` early-returns on `layout.ignore_change` | identical to `VKGSRender.cpp:2624-2630` |
| `surface_cache_traits::convert_pitch` is a stub that discards the source | Vulkan's is a `TODO` stub with the same behaviour (`VK/VKRenderTargets.h:468-476`); it is a shared guest-quirk gap, not a Metal regression |
| depth attachment stores without resolving on pass end | the resolve is the explicit pass in `surface::resolve()`, which is what `memory_barrier(transfer)` calls before a read — the same shape Vulkan uses |
| `apply_blend_state` has no native logic op | `supports_native_logic_op = false` is declared (`MTLDevice.cpp:379`) and the ROP shader emulates it |
| `present_frame()` returns `drawable != nullptr` | the return only gates `post_present()`, which no backend overrides (`Core/RSXRenderBackend.h:177-179`) |
| `setDepthBias()` could leave a stale bias when polygon offset is disabled | it cannot: `capture_encoder_state` defaults both terms to 0 and `apply_encoder_state` writes all of them on every clause (`MTLEncoderState.cpp:24-30`, `:82`), which is what `VKDraw.cpp:238-243` does in its `else` arm |
| depth compare is reversed like a shadow sampler compare | it is not. `get_compare_function(op, reverse)` is called with `reverse = false` for depth and stencil (`MTLDepthStencil.cpp:165,181`) and with `true` only for a sampler's compare (`MTLSamplerState.cpp:302`), which is the direction the two APIs actually want |
| a depth attachment that resolves on store | it never does — depth stores and resolves in its own pass (`MTLFramebuffer.cpp:279-283`), which is required because Metal has no depth resolve |
| colour attachment store/resolve | `StoreActionStore` becomes `StoreActionStoreAndMultisampleResolve` exactly when a resolve surface exists (`MTLFramebuffer.cpp:254-262`) |

---

## 8. What this audit did not cover

- No device runs: every severity below High is a claim about code, not about pixels.
- The shader pipeline (`MTLProgramPipeline.cpp`, `MTLShaderInterpreter.cpp`, the SPIRV-Cross slot
  map in `MTLShader.cpp`) was read for structure only; the slot/sampler-index agreement between
  `MTLArgumentBinds.h`, `MTLShader.cpp` and `MTLFragmentProgram.cpp` needs a targeted pass.
- `MTLConstants.cpp` (the ring allocators) and `MTLOverlays.cpp` were skimmed.
- Timing, vsync pacing and drawable-pool behaviour in `MTLPresenter.cpp` were read but not
  measured; the handoff's own §7.9 numbers are the only measurements available.

---

## 9. Performance

Nothing here changes behaviour; each item is either a mechanical change or a design change that
produces the same bytes. The first three are measured-shaped costs on the draw path; the rest are
CPU overhead that shows up in the frame-time profile rather than in the GPU time.

### P1 — Every texture upload is expanded on the CPU; none is ever a GPU copy

The staging path is the single largest avoidable CPU cost, and the fix already exists in Vulkan.

- `MTLTextureCache.cpp:113-121` sets `supports_byteswap = false`, `supports_vtc_decoding = false`,
  `supports_hw_deswizzle = false`, `supports_zero_copy = false` for every call.
- `VK/VKTexture.cpp:1174-1181` enables byteswap, hw-deswizzle and zero-copy for any subresource of
  1 KiB or more, which turns the upload into a GPU-side copy out of the guest window.

The Metal backend owns a 4 GiB no-copy import of `vm::g_sudo_addr` (`MTLDevice.cpp:348`), so the
"zero-copy" source side is already in place — the missing piece is only the capability flag and the
shared code path that consumes it. This is pure win for any title that streams textures (FMV,
video-out, decoded video, the DXT-heavy title in the handoff).

### P2 — One shared staging buffer per uploaded subresource

`MTLTextureCache.cpp:172` allocates a fresh `MTL::StorageModeShared` buffer for each subresource
upload and defers its release (`:200`-ish). Each allocation costs a heap search, a residency-flag
probe and a `m_spans` insert, and each release costs a heap search and a free-list splice (§P3).
A per-frame-slot staging ring — the same `byte_ring` the index and instruction blocks already use —
would amortise all of it to one pointer bump. The frame-slot lifetime already provides the safety
guarantee (the ring is reused only after that slot's frame has completed).

### P3 — The heap free list is a linear scan with mid-vector inserts

`MTLResourceManager.cpp:118-215`:

- `allocate()` walks every block and, inside each, every free span, computing the aligned fit by
  hand; then it inserts or erases in the middle of a `std::vector<std::pair<u64,u64>>`, which moves
  the tail.
- `free()` walks every block looking for the heap by pointer identity, then inserts into the middle
  of the vector and merges with its neighbours.

Both run on the hot allocation path (every texture, every buffer, every staging buffer) and on the
deferred-release path (once per object per frame). This is O(blocks × spans) per operation with
vector memmoves, and it degrades exactly when a title allocates a lot of small objects — which is
the streaming-texture case. The span record already holds `heap`, `offset` and `size`
(`m_spans`), so the block index can be stored there instead of searched for; and a size-class
free list (or a `std::map` keyed by offset) removes the scan and the memmove without changing the
allocation results.

### P4 — `heapBufferSizeAndAlign` is a driver call on every allocation

`new_buffer` (`:296`) and `new_texture` (`:376`) ask the device for size and alignment before
searching the pool. The inputs are a length and a storage mode, so the answer is cacheable — and
most callers ask for one of a handful of sizes (2 MB index ring, 8 MB vertex ring, 4 KB staging,
per-texture extents). A small memo (or rounding lengths to the pool's size classes) removes a
driver round trip from every allocation.

### P5 — Read-back stalls the RSX thread once per transfer

See M3. The depth and packed-format write-backs call `flush_for_readback()`, which commits the open
command buffer and waits for its completion value. A title that downloads depth every frame pays a
full pipeline drain per transfer. Batching all pending read-backs into one shared buffer, one
commit and one wait (or handing the wait to the requesting thread) keeps the FIFO reader moving.

### P6 — Residency set commits per allocation

`residency_manager::make_persistent` (`:186-196`) calls `addAllocation` followed by `commit()`
every time. `commit()` is only reached per *heap*, not per buffer, so the count is small — but the
same function is the obvious place to note that the set supports batching: `addAllocation` many
times, `commit()` once per frame.

### P7 — `check_residency` on the allocation path

`new_buffer`/`new_texture` call `check_residency(heap)` (`:315`, `:394`), which does a hash-set
lookup plus a one-shot error flag, purely as a diagnostic for the "heap not in the set" failure
(HLD-02 F1). It is cheap but it is on every allocation, and the invariant it checks is
structural — it belongs in an `ensure` (debug builds) rather than in the shipping path.

### P8 — Design note: the guest window could serve as the stream source

Every draw copies its index array and its vertex range into per-slot rings, then reads them back
through an `R8Uint` texture buffer. The copy is not only for format conversion: Metal 4 command
buffers do not snapshot buffer contents and the GPU reads at execution time, so the copy is what
makes the draw read the data the guest had at encode time. Vulkan has the same constraint and
solves it the same way, which is why this is a note and not a defect.

What Metal has that the other two do not is a 4 GiB no-copy import at the guest address
(`guest_window`), so the *persistent* half of the split could bind the guest window at the draw's
guest address and hand the shader absolute offsets — the attribute offsets in `attrib_data` are
already relative to the bound stream base, so the shader needs no change. The index buffer is
even more direct: `drawIndexedPrimitives` takes a `GPUAddress`, and the guest address is exactly
`gpu_address + rsx_address`. Both are only safe where the existing persistent/volatile
classification already declares the data stable, and both need the alignment check Metal applies
to the index address. Worth measuring before building, because it trades a guaranteed snapshot
for a bandwidth saving.

### P9 — What is already right (do not "optimise" it)

- Textures are resolved per *clause*, not per subdraw (`MTLGSRender.cpp:3257`), and sampler states
  are cached by value (`MTLSamplerState.cpp:362`).
- The argument binder writes only the slots whose `ResourceID` changed (`MTLArgumentBinds.cpp`).
- Blend, depth/stencil and raster states are hashed and applied once per clause
  (`MTLGSRender.cpp:4599-4604`), through per-frame caches.
- `resolve_texture_bind` returns `ResourceID`s, so no texture view is created per draw.
- The instrumented paths are gated by a cached level check, and the value arguments are not
  evaluated below the level (`MTLInstrumentation.h:1696-1740`). A PR build sets
  `MTL_DIAGNOSTICS=0` and removes all of it, so the `TODO(excise)` markers are a prerequisite for
  meaningful measurement, not a style issue.

---

## 10. Evidence from the 2026-11-13 probe run

Source: `build/probe/run-20261113-164452-ds/` (4 MB probe log, 592 capture dumps, a `stdout.log`
with a crash backtrace). Level 1–3 probe, one 300-frame report interval at t=99.9 s, plus a
level-5 capture window on frame 2707.

### R1 (High) — the run died in `emit_geometry`

`stdout.log`:

```
Segfault reading location 000000000028a0 at 0000000261069334.
Emu Thread Name: 'rsx::thread'.
  #3 00000001059895ec _ZN3mtl11MTLGSRender13emit_geometryEj+0x48e0
  #4 00000001059895ec _ZN3mtl11MTLGSRender13emit_geometryEj+0x48e0
  #5 0000000105982c44 _ZN3mtl11MTLGSRender8end_drawEv+0x58e0
  #6 ... rsx::core_renderer::end
  #7 ... rsx::nv4097::set_begin_end
```

The fault address is `0x28a0`, i.e. a **null base plus a 10 400-byte member offset**, reached from a
draw. What can be said with certainty and what cannot:

- Frames #3 and #4 are the same PC, and their printed offset (`+0x48e0`) disagrees with the slide
derived from frame #5 (`+0x58e0`), so the backend's own backtrace printer mis-symbolises this
binary. `atos` on the shipped binary agrees that there is no line info at all
(`g_fixed_typemap + 25747564`). The linked fault address consistent with frame #5 is
`emit_geometry + 0x58e0`, which disassembles into the capture's `snprintf` loop, so **either the
offset or the frame is wrong**; the crash cannot be pinned to a line from this run alone.
- `emit_geometry` contains no load with an immediate offset of `0x28a0`, so the null base is
computed (an indexed access through a null member), not a direct `[null + imm]` member read.
- The last logged event before the crash region is a *guest-thread* deferred access violation
(`0xc1392c88`, writing). The whole run handled 5 such faults, all deferred, all with **zero**
sections to flush (`fault/sections to flush 0`), so a fault-recovery write-back is not a plausible
cause. The RSX-thread fault path (`fault/on RSX thread`) never fired.

**What would settle it:** a run with a symbolised build (`-g`, or a `dSYM` kept next to the app).
That single change turns this from a guess into a line number; it is worth doing before any other
crash investigation on this backend.

### P10 (Perf) — a render pass per draw, not per target

| counter | value | per frame |
|---|---|---|
| `frame/end` | 31 977 calls, 41.49 µs avg, **315 ms max** | 106.6 draws, 4.42 ms |
| `framebuffer/open pass` | 32 688 calls, 8.50 µs avg, 3.37 ms max | 109.0 |
| `recorder/open pass` | 33 233 calls, 7.43 µs avg | 110.8 |
| `framebuffer/bind` | 1 421 calls, **34.70 µs avg**, 4.69 ms max | 4.7 |
| `framebuffer/reconcile` | 1 421 calls, 3.50 µs avg | 4.7 |
| `textures/resolve` | 31 977 calls, 2.73 µs avg, 15.66 ms max | 106.6 |
| `draw` | 31 977 calls, 9.91 µs avg, 6.14 ms max | 106.6, 1.06 ms |
| `draw/argument binds` | 31 977 calls, 0.38 µs avg | 0.04 ms |

**32 688 pass opens for 31 977 draws is 1.02 passes per draw**: the pass-absorption design
(`MTLPassPolicy`, `absorbs()`) is not absorbing in this title. Each pass costs `open_pass`
(8.5 µs) plus the recorder side (7.4 µs) plus the pass hazards the scheduler emits, and a new
encoder per draw is also the worst case for the GPU (tile-memory flush per pass). The `framebuffer/bind`
figure (34.7 µs average, 4.7 calls per frame) says the *bind* path — not the pass open — is the
expensive one, so the two should be looked at together: something per draw is forcing a fresh
attachment set or a fresh pass key.

This is the single largest CPU saving available on a title shaped like this one, and unlike §9's
items it needs no new machinery — only a probe of why `pass_key_matches` / `can_absorb` fails. The
level-1 latch `recorder/pass absorbed` versus `recorder/pass opened` is exactly the counter that
answers it, and the 300-frame report was run at level 1 without the coverage table, so the run in
hand cannot say which. A level-2 run of the same title would.

### P11 (Perf) — the frame is display-paced at 6.4 ms of GPU work

- `frame/gpu` gauge: **6 370 µs average, 14 622 µs max** per frame.
- `present/display gap` gauge: **35 317 µs average, 370 110 µs max**, minimum 10 ms.
- `stall/acquire reported`: 64 of the first 129 frames.
- `present/display/dropped`: 27 of the first 129 frames.
- Largest logged acquire: **`acquire at 7155.399 took 726.355 ms`**, against a `frame/end` maximum
  of 315 ms and a `present/flip` maximum of 23.8 ms.
- `recorder/readback wait`: 2 calls, 5.2 ms average, 8.2 ms max; `recorder/re-enter slot` the same
  two calls at 8.2 ms max.

The emulator is presenting at ~28 fps (35 ms gap) while the GPU needs 6.4 ms. The `stall/acquire`
and dropped-present latches say the drawable pool (`maximumDrawableCount` 3, per the logged
`max 3`) is the gate, not the renderer. The readback path also re-enters a slot and waits 8.2 ms
inside it (see M3), which is the same class of stall from a different source.

Two things are worth separating in the next run: whether the 35 ms gap is the layer's cadence
(the display is 60 Hz, so 35 ms is not a multiple of it) or the RSX thread arriving late. The
`present/acquire` span (151 µs average, 10.8 ms max) and the 726 ms acquire are the evidence that
it is the acquire.

### O1 (Open) — `NaN` vertex positions in the capture

1 196 log lines contain `nan`, of which ~1 024 are the level-5 vertex traces of the form:

```
Metal 4: vertex 0, guest 0.0000 nan 0.0000, clip nan nan nan nan, ndc nan nan nan, raw w 0.0000 tc 0.0000
```

Every traced vertex of those draws has the *same* decoded value (x=0, y=NaN, z=0), which is not a
plausible quad, so at least one of the two is true: the fetch is producing wrong bytes for the
attribute, or the trace reads one vertex repeatedly and mis-decodes it. The trace comes from the
same `layout` the draw uses, so if the y attribute of this layout decodes to NaN the draw is dead
— it would be the "menu text / decal drawn black" family from the handoff. The dump set contains
the ring bytes the GPU actually read (`frame-2707-p1-persistent.bin` plus the per-draw
`attrib_offset` records in `frame-2707-p1-manifest.txt`), so decoding one of these vertices by hand
settles it. This audit did not get that far.

---

## 11. kosmickrisp as the Metal 4 reference (`.kk-mesa/src/kosmickrisp`)

kosmickrisp is the Vulkan driver RPCS3 ships on this platform (`.ci/build-kosmickrisp.sh`), and its
backend is a second, independent implementation of the same Metal 4 API. That makes it useful in
one direction: where it and `rpcs3/Emu/RSX/MTL/` use the API differently, one of the two is
probably wrong. Everything below is chosen for what it says about **our** backend.

### 11.1 — where our usage differs from the reference

**V1 (our side, high if reached) — the host-pointer memory import has no alignment contract.**
Our Vulkan backend turns on `supports_passthrough_dma` whenever `VK_EXT_external_memory_host` is
present (`VK/VKGSRender.cpp:779`) and then imports guest memory through host pointers
(`VK/vkutils/buffer_object.cpp:95-140`, `VK/vkutils/memory.cpp:484`). The driver accepts *any*
pointer and returns `VK_SUCCESS` with a **null** Metal buffer when the pointer is not page-aligned
or the length is not a whole number of pages, because `newBufferWithBytesNoCopy:` returns nil. The
driver does publish the requirement —
`minImportedHostPointerAlignment = os_page_size` (`kk_physical_device.c:791`) — but our
`buffer::buffer(const vk::render_device&, VkBufferUsageFlags, void* host_pointer, u64 size)`
goes from `vkGetBufferMemoryRequirements` straight to `vkBindBufferMemory` without consulting it.
Both halves need the check; the cheap one is ours, in the same place we already decide whether the
feature is available. Note that the Metal backend *does* check the analogous call
(`import_guest_window` dies on a nil buffer, `MTLDevice.cpp:349-352`).

**V2 (our side, verify) — residency is committed but never requested.**
We add each heap to the persistent set and `commit()` it (`MTLResourceManager.cpp:186-196`) and we
never call `requestResidency()` on that set; the only `requestResidency()` in the backend is for
the drawable set (`MTLPresenter.cpp:69`). The reference driver instead adds allocations *without*
committing, then commits and requests residency in one place (`kk_device.c:454-470`). If a commit
alone does not make an allocation resident, our heaps read zeros — which is exactly the failure our
own `check_residency` was written to report (`MTLResourceManager.cpp:470-486`). Either the check
never fires (commit is enough, no action) or it is the warning that explains a black texture. Worth
one instrumented run with the check promoted to an error.

**V3 (our side is the stricter one) — barrier visibility scope.**
We default every barrier to `VisibilityOptionDevice` with a written rationale that a weaker scope
under-orders an ordinary edge (`MTLBarrier.h:14-22`). The reference passes
`MTL4VisibilityOptionResourceAlias` at *every* barrier, with its own TODO saying the option "should
only be required if occlusion queries are used" (`bridge/mtl_encoder.m:31-34`). So on this point
our backend is the conservative one, and a visual difference between the Metal and the Vulkan path
on macOS is *more* likely to be the Vulkan path under-ordering than ours over-ordering. Do not
"match" the reference here.

**V4 (verified consistent, no action).** These were checked against the driver and are used the
same way, which is the strongest evidence available short of a run:

- all three MTL4 barrier forms — `barrierAfterStages`, `barrierAfterEncoderStages`,
  `barrierAfterQueueStages` (`MTLBarrier.cpp:20-46` vs `bridge/mtl_encoder.m:26-60`);
- the visibility-result buffer on the render-pass descriptor with the mode on the encoder
  (`MTLFramebuffer.cpp:212`, `MTLZCULL.cpp:164` vs `kk_cmd_draw.c:433`, `bridge/mtl_encoder.m:355`);
- untracked hazard tracking on the heap plus explicit barriers everywhere.

