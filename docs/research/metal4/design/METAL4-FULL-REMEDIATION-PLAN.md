# Metal 4 full remediation plan

**Status:** active implementation plan. Companion to
`docs/research/review/METAL4-BACKEND-REVIEW.md` and
`METAL4-REVIEW-IMPLEMENTATION.md`.
**Tree:** `metal4-dev`, rebased onto `upstream/master` `0ca5eb0ef`.
**Rule:** the review's line numbers are for `release/metal4` `e6fd9ca7b`.
Every item is re-read against this tree before it is changed.

This plan covers **every** finding in the review, not only the ones on the
Demon's Souls path. Items that the rebase or earlier commits retired are kept
in the table so they are not re-fixed by mistake. The large redesigns (R1-R9)
are treated as work items with explicit landing order, not as future ideas.

The Demon's Souls measurements stay in `DS-STROBE-INVESTIGATION.md`. This
file does not modify that document.

---

## 1. Current state

Already retired or implemented before this plan:

| Item | State |
|---|---|
| F7 | Fixed upstream (`#20139`); in the rebase. |
| C23 / C1 layer 1 | Fixed upstream (`#20078`); integer bit tests in the shared decompiler. |
| C10 | Already emitted from `mtl_rasteriser_state.x/.y`. |
| D2 | Already emitted as `gl_SampleMask[0]` from `mtl_rasteriser_state.z`. |
| C4 | Fixed in `160d310bf5`. |
| C7 | Downgraded to L; single queue + encoder-end producer barrier. Keep the CPU-mapped guard. |
| F1 | Retracted. |

Implemented by this work:

| Item | Commit |
|---|---|
| S1, D1, T1, T4, C19, C8, C27, C30, C11, C1 layer 2 | `49553cd6e` |
| C13 pre-flush | `b3075bc6a` |
| T5 (in-place blit staging) | `cc7157d43` |

Everything else below is open.

---

## 2. Phases

The order is dependency-driven: a fix lands only after the machinery it
assumes exists. Each phase is one commit set, one build, one `rpcs3_test`
run, and one scrub check. A phase is not split into per-finding commits unless
a finding is independently revertable and the phase is large.

### Phase A — surface and format contract

Fixes the data that reaches the GPU: byte order, aspect, MSAA layout,
typeless inheritance, stencil, and the sampler decisions that depend on the
format. This is the highest correctness value after the P0 set and unblocks
T3/R5.

| ID | Work | Design |
|---|---|---|
| T2 | Big-endian surface upload/readback | Add `bswap32` and `bswap16x2` compute kernels to the internal library. Upload: guest → staging, encoder barrier, swap, barrier, staging → texture. Readback: texture → scratch, barrier, swap, barrier, scratch → guest. CPU decode/encode paths use `be_t<u16>/be_t<u32>`. Framebuffer `create` sets `pack_unpack_swap_bytes` from `get_compatible_gcm_format`. |
| T6 | Typeless inheritance | One `copy_typeless(cmd, src, dst, src_rect, dst_rect)` with three routes: same format → `copyFromTexture`; same byte width, colour↔colour → buffer round trip; depth↔colour → conversion kernel (D32F+S8 ↔ z24s8). `get_texel_size(Depth32Float_Stencil8)` returns the guest 4 bytes for transfer arithmetic; a separate `host_texel_size` is used where real host bytes matter. |
| T7 | Stencil upload | Decode the low byte of each byte-swapped guest depth word into a `u8` stencil plane, then `copyFromBuffer(..., StencilFromDepthStencil)` after the depth copy, with an encoder barrier between them. |
| T8 | sRGB views | `to_srgb()` for BGRA8/RGBA8/BC1/BC2/BC3. Bind an sRGB view when `hw_SRGB_possible()`, clear the texture gamma bits in the fragment texture-control word, and stop forcing nearest for gamma and depth-compare. |
| T9 | Vertex-texture LOD | Keep nearest filters and zero bias; set `min_lod`/`max_lod` as the fragment path does. |
| T10 | MSAA scratch type | `TextureType2DMultisample` when `sampleCount > 1`. |
| T11 | Border colours | Non-preset borders use clamp-to-edge plus a shader border test behind a pipeline key bit; presets stay hardware. |
| C20 | Drop `PixelFormatView` | Remove from ordinary sampled and colour textures; keep it only on the depth-stencil stencil view and SNORM↔UNORM views. |
| C12 | Readback trim | Return early when the trimmed region is empty instead of `ensure`. |
| C31 | Index clamp | Clamp the subrange count to the remaining index bytes. |
| C25 | mprotect flush | `rsx::mm_flush()` at the start of `submit` and `commit_open_buffer`. |

### Phase B — submission, lifetime, and device loss

This is the R1/R2 redesign expressed as concrete changes. It removes the
mid-frame waits, makes completion value-based, and makes teardown safe.

| ID | Work | Design |
|---|---|---|
| A1 + R1 | Chunked command buffers | One allocator per slot, N buffers per slot, one completion event. `next_buffer()` begins a new buffer without waiting; `commit_open_buffer()` returns the value; `flush_for_readback` and `sync_label` await that value; the allocator resets only at slot recycle. Delete `m_reenter_pending`/`reenter_slot`. |
| L1 | Device loss on timeout | A feedback timeout sets `device_lost` and stops recording; it never recycles a slot whose buffers may still run. `check_device_status` reports lost. |
| L2 | Shutdown drain | `recorder.shutdown()` returns false when the ring did not drain; `on_shutdown` then leaks the backend instead of freeing in-flight objects. |
| L3 | Feedback owner | A `shared_ptr<feedback_owner>` with a mutex and an in-flight count; the block never captures `this` raw. A GPU error sets `device_lost` and does not advance completion. Shutdown waits for in-flight blocks with a deadline. |
| L5 | Overlay reallocation | A dirty overlay image is replaced with a new allocation and the old one is deferred; never `replaceRegion` an object an in-flight slot may read. Same rule for font atlases. |
| L6 | Device ownership | `device` releases the queue, compiler, serializer, archive, options and guest window; autorelease pools around init/flush; a process-wide abandoned-worker flag prevents a second compiler after a failed shutdown. |
| L7 | Residency/heap retirement | Queue residency removals and apply them in `flush_residency`; retire empty dedicated heaps on the completion timeline. |
| A2 | Deferred label commit | After A1, a label release records the label and a deadline; commit at the next natural pass boundary or deadline, never inside a pass. |
| C29 | Crash-safe archive | Per-session archive file, load all matching archives newest-first, rotate every 256 pipelines or 5 minutes at a frame boundary, compact when more than 8 exist. |
| C9 | Ring-drop visibility | Count drops per ring, log once per frame, expose in the overlay, grow the ring next frame. |
| F4 | RSX-thread waits | Falls out of A1 and R6. |

### Phase C — MSAA sample mosaic (T3)

Replace the hardware average with the shared 2×1/2×2 mosaic:

1. Create `resolve_surface` at `width*samples_x × height*samples_y`.
2. `rsx_resolve` compute kernel: for each pixel and sample, write to
   `p * grid + (s % grid.x, s / grid.x)`; depth variant writes `R32Float`,
   stencil variant uses the stencil view.
3. `rsx_unresolve` fragment at sample rate reads the mosaic back into the MSAA
   image; depth uses `[[depth(any)]]`, stencil uses `[[stencil]]`.
4. Match VK's sample order.
5. Remove the sample-scale special case in the texture cache; keep the
   hardware average only for present.

### Phase D — pipeline and interpreter

| ID | Work | Design |
|---|---|---|
| C17 | Non-blocking interpreter | Resolve the real pipeline first; only on a miss build a request and call `try_get`, which queues work and returns null without blocking. Prebuild the base libraries at boot. Delete `run_link_sync` from RSX-thread paths. |
| C26 | Remove the link mutex | C17 removes the synchronous link; the compiler is concurrency-safe on macOS 27. Keep a mutex only around the serializer if validation requires it. |
| C5 | Position invariance | Set `require_explicit_invariance` in both vertex emitters and `setPreserveInvariance(true)`; add the setting to the C14 key. |
| C14 | Versioned keys | Generator version + math mode + invariance + dev commit in the archive and shader-cache keys. Bump the generator version on every emitter change. |
| O1 / U1 | Pipeline warm-up | Build overlay, blit and fill pipelines in `on_init`; compile the FSR library with `MathModeSafe`. |
| C18 | Wider interpreter | Staged: shadow compare and alpha-kill first, then vertex textures; count refusals by reason. |

### Phase E — present, pass model, code structure

| ID | Work | Design |
|---|---|---|
| C13 cast | Present format cast | When the source surface format differs from the AV format, run `copy_typeless` (T6) into a BGRA8 scratch and present that. |
| L4 + R6 | Present thread | The RSX thread hands `(texture, commit_value)` to a present thread; the present thread acquires the drawable late and encodes the final pass. `CAMetalLayer` properties are owned by the main thread and published through atomics; the layer is retained. |
| P1 | Drawable wait | Falls out of R6. |
| F6 + R3 | Prepare/execute split | Move guest reads and cache work before `open_pass`; track a per-encoder write set and emit encoder barriers only on overlap; replace the blanket encoder-end barrier with scheduler edges, behind a "conservative barriers" switch. |
| R8 | Split `MTLGSRender.cpp` | Mechanical moves into `MTLDrawPrepare.cpp`, `MTLDrawEmit.cpp`, `MTLPresentFrame.cpp`, `MTLSync.cpp`; no logic changes. |
| R7 | Observability | `mtl::stats` counters (encoders, commits, waits, restarts, ring drops, skipped draws, interpreter refusals, latched failures, upload/readback bytes, residency), overlay output, `MTL::CaptureManager` hook, debug groups. |

### Phase F — remaining local defects and hygiene

| ID | Work | Design |
|---|---|---|
| R1-window | Guest window imports | Extend `vm::on_notify_memory_mapped` to all page sizes; create a per-block no-copy import and resolve guest addresses through the map, falling back to the window. |
| F2 | FIFO rewind compare | Ring-order compare with GET as origin (`(addr - get) < (put - get)`). Needs a guest A/B test; affects shared code. |
| F3 | `cellGcmCallback` order | Revert to upstream order unless an A/B test shows the fork's order is needed. |
| D3 | Depth-bias scale | Add an explicit setting (default 1.0); no shader depth write. |
| D4 | Line width | Document parity with MoltenVK; expand to quads only if a title needs it. |
| D5 | 32-bit strip restart | Convert a 32-bit strip to a list when it contains `0xFFFFFFFF` and restart is off. |
| H1 | Line endings | Restore CRLF in `nv3089.cpp`, `nv0039.cpp`, `nv308a.cpp` and re-apply the real diff with `-w --ignore-cr-at-eol`. |
| H2 | Shared invalidation offsets | Keep the fork's fix; prepare the upstream PR. |
| H3 | Missing docs | Commit the referenced design docs under `docs/metal4/` and rewrite comments over time. |
| H4 | Tests | Format round-trip, copy-region arithmetic, typeless sizes, scissor/viewport against VK. |
| H5 | Design smells | Fold the format tables into one description (R5); remove dead state; stop hashing uniform-only state in the pipeline key. |
| R5 | Shared texture contract | One format description; surface upload through `rsx::upload_texture_subresource`; one readback pack path; MSAA mosaic; one typeless copy. |
| R9 | Upstream strategy | Split the shared RSX changes into upstreamable commits; rebase weekly. |

Items already covered by an earlier phase are not repeated.

---

## 3. Verification

- Every phase: `cmake --build build-tests`, `rpcs3_test`, and
  `.metal4-release-scrub.py check`.
- Phase A: add the H4 format round-trip and copy-region tests first; they fail
  on T1/T2/T4/T6 and pass after the fixes.
- Phase B: run with Metal API validation and a forced-hang shader; confirm a
  clean device-lost state and no use-after-free.
- Phase C: compare resolve/unresolve against Vulkan's sample order with a
  known 2x/4x pattern.
- Phase D: count RSX-thread compile waits and latched failures.
- Phase E: count encoder splits and present-thread waits; run Xcode's render
  pass split insight.
- Release: `release/metal4` stays one squashed commit; docs and
  instrumentation are removed by the scrub.

---

## 4. Log

| Date | Phase | State |
|---|---|---|
| 2026-11-27 | plan created | T5 committed (`cc7157d43`); Phase A is next |
