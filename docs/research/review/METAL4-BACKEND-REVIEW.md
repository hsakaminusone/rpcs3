# RPCS3 native Metal 4 backend: review, fixes and long-term roadmap

**Version:** v1, 2026-11-27.
**Code under review:** `/Users/aa77982/PersonalWS/rpcs3`, branch `release/metal4` (HEAD `e6fd9ca7b`, one squashed commit, about 23,200 lines in `rpcs3/Emu/RSX/MTL/` plus about 2,600 real lines of shared RSX changes). `metal4-dev` was used for comparison.
**References:** the Vulkan (`VK/`) and OpenGL (`GL/`) backends; the shared RSX templates (`Common/texture_cache.h`, `surface_store.h`, `TextureUtils.cpp`); Mesa KosmicKrisp and its workaround list; the macOS SDK headers; Apple's Metal 4 documentation, Game Porting Toolkit skills and WWDC23-10127 / WWDC25-205 / WWDC25-254; Dolphin's Metal backend; upstream RPCS3 issues and PRs.
**Method:** static reading only. Nothing was compiled or run. Line numbers are for `release/metal4`.

**How to read a finding:**
- **Severity.** **H** = visible corruption, a hang or crash, or a large performance loss. **M** = wrong in specific cases, or a moderate cost. **L** = an edge case or a small cost.
- **Confidence.** *Verified* = the code path was read end to end against a reference. *Derived* = follows from the code plus a documented rule, and needs a runtime check. *Reported* = found by a sub-review and spot-checked, not fully re-read.
- **Fix** gives the implementation. Code is a sketch against the real function names; adapt it to local helpers.

---

## Contents

1. Summary
2. Fix order (one table)
3. Must-fix findings (severity H)
4. Should-fix findings (severity M)
5. Minor findings (severity L)
6. Maintainability and repository hygiene
7. Long-term redesign
8. State-of-the-art gap analysis
9. Performance roadmap
10. Settings
11. Validation plan
12. Small PRs to send upstream
13. Reference: KosmicKrisp cross-check, workflow trace, retracted items, history, coverage

---

## 1. Summary

**Verdict.** The architecture is the right one for an Apple-only renderer, and it is well built: Metal 4 command allocators and argument tables, untracked placement heaps with explicit barriers, residency sets, an `MTL4Compiler` with an archive, an async pipeline job system with an interpreter fallback, and a pass scheduler. Most defects are local and fixable in tens of lines each.

There are four clusters of real problems:

1. **Synchronisation and submission.** There is no ordering between commands inside a reused compute encoder (S1). Every mid-frame commit makes the RSX thread wait for the GPU (A1). A slot that times out frees resources the GPU is still using (L1).
2. **Surface and texture correctness against the shared RSX contract.** Surface memory is never byte-swapped (T2). RSX MSAA is averaged away instead of kept as a sample mosaic (T3). 3D textures lose every slice after the first (T4). In-place blits are dropped (T5). The A4R4G4B4 swizzle is wrong (T1). The blit render area ignores the destination origin (C4).
3. **Shader math, constants and rasterisation.** Fast math folds the NaN guards (C1). The blend-constant block is misaligned after the first draw (D1). The scissor is mirrored (C8). Resolution scale is ignored in several places (C2). The depth range is applied twice (C19). The MSAA sample mask is ignored (D2).
4. **Compile pipeline blocking the RSX thread.** The interpreter is resolved before the real pipeline (C17). A failed compile is latched forever (C27). The cache keys miss inputs that change generated code (C14).

**Production readiness: about 55%.** Architecture about 80%, correctness on a broad game set about 50–55%, performance tuning about 40–50%, tooling about 35%. See section 13.5.

**Symptom map** (ranked by how directly the code explains each symptom):

| Symptom | Most likely causes, in order | First thing to try |
|---|---|---|
| Exact seam at x=640, only the right half sharp | **C4**, then C2/C8 if scale ≠ 100% | The C4 fix (two lines) |
| White, flashing floor water (Demon's Souls) | **C1**, **S1**, C23, C5 | "Disable MSL Fast Math" plus delete `shaders_cache/metal/` (C14) |
| Black/grey flashing across the whole image | **S1**, **C13**, **T2** (with Write Color Buffers on), F6 restart path, C19 | The S1 fix |
| FIFO desyncs; needing Atomic/Ordered FIFO | **F7**, **C17/C26**, **A1**, F6, F2, F4 | Cherry-pick #20139, then the C17 reorder |
| Low GPU utilisation, RSX thread "waiting" in profiles | **A1**, F4, A2 | A1 |
| Geometry missing for a while, or forever | C18, **C27** | The C27 retry fix |
| Wrong colours in UI or fonts | **T1**, T2 | T1 (one line) |
| Wrong post-processing with RSX anti-aliasing | **T3** | T3 |
| Volume fog or 3D LUT looks flat | **T4** | T4 (one line) |
| Garbled trophy pop-up or performance graph | **L5** | L5 |
| Blend colour or logic op wrong on some draws, depending on draw order | **D1** | D1 (two lines) |
| Crash or corruption after a GPU hang, or on quit | **L1**, **L2**, **L3** | L1–L3 |

---

## 2. Fix order (one table)

Ordered by value ÷ effort within each block. IDs link to sections 3–6.

| # | ID | Change | Size | Fixes |
|---|---|---|---|---|
| **Block 1: small, do first** |||||
| 1 | F7 | Cherry-pick upstream #20139 | 3 files | Freezes, "FIFO desync" |
| 2 | S1 | Encoder-scope barrier when a compute encoder is reused | ~5 lines | Black/grey flashing |
| 3 | C4 | Blit render area covers `x0+w` × `y0+h` | 2 lines | Seam at 640 |
| 4 | D1 | Pad the blend-constant block to 48 bytes | 2 lines | Wrong blend colour / logic op after the first draw |
| 5 | T1 | A4R4G4B4 swizzle | 1 line | Wrong channel colours |
| 6 | T4 | 3D upload `bytesPerImage` | 2 lines | 3D textures |
| 7 | C19 | Viewport depth always [0,1] | 1 line | Depth fighting |
| 8 | T10 | Multisample scratch depth texture type | 1 line | Framebuffer-less MSAA draws |
| 9 | L5 | Reallocate dirty overlay images instead of `replaceRegion` | ~10 lines | Garbled overlays |
| 10 | C30 | `setShouldMaximizeConcurrentCompilation(true)` | 1 line | Compile stutter |
| 11 | C11 | `setSupportArgumentBuffers(true)` on samplers | 2 lines | Insurance |
| 12 | C31 | Clamp the subrange index count | 1 line | Rare garbage triangles |
| 13 | L3 | Feedback control block, null check, error → device lost | ~30 lines | Crash on quit, silent GPU faults |
| **Block 2: medium, high value** |||||
| 14 | C17 + C26 | Real pipeline before interpreter; non-blocking interpreter | ~60 lines | Stalls that look like FIFO desync |
| 15 | C1 + C23 | Integer NaN guards; Relaxed as the default math mode | ~30 lines | White water, precision |
| 16 | C8 + C2 | Unmirrored, clamped, scaled scissor; scaled viewport and pass size | ~20 lines | Seams and cropping at scale ≠ 100% |
| 17 | C14 + C29 | Versioned cache keys; per-session archives flushed periodically | ~60 lines | Stale binaries, lost archives |
| 18 | C27 | Retry soft compile failures | ~20 lines | Missing geometry |
| 19 | T5 | Copy through a temporary for in-place blits | ~30 lines | Missing blur, scale and UI effects |
| 20 | T2 | Byte-swap on surface upload and readback | ~80 lines + a kernel | Read/Write Color/Depth Buffers |
| 21 | L1 + L2 | Never free or reuse resources of an unfinished commit | ~60 lines | Corruption after a GPU stall, crash on quit |
| 22 | C20 | Drop `PixelFormatView` from colour usage | 2 lines | Bandwidth on every texture |
| 23 | C13 | Pre-flush before the guest-memory present; format cast | ~40 lines | Black frames |
| 24 | C5 | Position invariance on | ~5 lines | Z-fighting between passes |
| 25 | D2 | Apply the RSX MSAA sample mask through `[[sample_mask]]` | ~30 lines | MSAA draws that mask samples |
| **Block 3: larger** |||||
| 26 | A1 | Several command buffers per slot; no wait on mid-frame commits | ~150 lines | GPU utilisation, frame time |
| 27 | T3 | RSX MSAA sample mosaic: resolve/unresolve passes | ~250 lines | Anti-aliased titles |
| 28 | T6 + T7 | Typeless surface inheritance; stencil upload | ~200 lines | Depth/colour aliasing, stencil |
| 29 | T8 | sRGB views instead of shader gamma; linear PCF | ~80 lines | Texture filtering quality |
| 30 | H1 | Restore upstream line endings in shared files | mechanical | Rebase conflicts |
| 31 | C22 | Rebase onto current upstream | merge | Several upstream fixes |

---

## 3. Must-fix findings (severity H)

### F7. The fork has upstream's `sys_rsx_mtx` vblank/flip deadlock
*Severity H · Confidence: verified (upstream diff read).*
- **What happens.** `_sys_rsx_lock_context_calls` documents that "every sys_rsx_context_* call holds sys_rsx_mtx" (`sys_rsx.cpp:123-124`). `send_event` loops on `CELL_EBUSY` while holding that mutex (`:218-228`), and the gcm interrupt thread that would drain the event queue needs the same lock. The result is a deadlock. Upstream reports fixed by #20139: Portal 2 #20048 (macOS M4 Pro), Toy Story 3, LBP Karting, Drakengard 3, NFS The Run, FF XIII, Wipeout HD.
- **Fix.** Cherry-pick #20139 (`sys_rsx.cpp`, `RSXThread.cpp`, `RSXThread.h`):
  1. `send_event` becomes `_sys_rsx_send_event(ctx, ..., std::unique_lock<shared_mutex>* lock)`.
  2. Inside the busy loop it unlocks around `thread_ctrl::wait_for(100)`, re-locks through `_sys_rsx_lock_context_calls`, and re-finds the context by ID (the context may have been destroyed while unlocked).
  3. The four context-attribute call sites pass `&lock`; `RSXThread.cpp` passes `nullptr`.

  The fork's `sys_rsx.cpp` has local changes, so resolve conflicts by keeping upstream's lock discipline.

### S1. Commands inside a reused compute encoder are not ordered
*Severity H · Confidence: verified.*
- **The Metal 4 rule** (SDK `MTL4CommandEncoder.h:56-113`). `barrierAfterQueueStages` only waits for earlier *encoders*. `barrierAfterStages` only blocks later *encoders*. Inside one `MTL4ComputeCommandEncoder`, blits and dispatches may run concurrently. The only intra-encoder ordering is `barrierAfterEncoderStages`.
- **What the code does.** `open_copy_encoder` returns the open encoder when there is one (`MTLCommandRecorder.cpp:544-547`). Callers order their work only with queue or producer barriers: `clear_texture` (`MTLTextureCache.cpp:372`), `emit_content_barrier` (`MTLSurface.cpp:22-27`, used at `:313`, `:388`, `:465`), and `copy_transfer_regions_impl` (`MTLTextureCache.cpp:~733`). Only the typeless copy emits an encoder-scope edge (`:831-839`).
- **Races:**
  1. `clear_texture(...)` then `upload_texture_data(...)` on the same image share one encoder (`MTLTextureCache.h:595-625`, `680-705`). The upload can run before the clear finishes, which gives partially black textures that change from frame to frame.
  2. `surface::write_to_memory`: `copyFromTexture(source → scratch)` then `copyFromBuffer(scratch → guest)` with no edge (`MTLSurface.cpp:696-707`). With Write Color Buffers on, the guest receives stale rows.
- **Fix.**

```cpp
// MTLCommandRecorder.cpp — open_copy_encoder()
if (m_compute_encoder)
{
	// Commands in one MTL4 compute encoder run concurrently; queue and producer
	// barriers only order other encoders.
	m_compute_encoder->barrierAfterEncoderStages(
		MTL::StageBlit | MTL::StageDispatch,
		MTL::StageBlit | MTL::StageDispatch,
		MTL4::VisibilityOptionDevice);
	return m_compute_encoder;
}
```

  - In `surface::write_to_memory`, add the same `barrierAfterEncoderStages(StageBlit, StageBlit, Device)` between `copyFromTexture` and `copyFromBuffer` (`MTLSurface.cpp:~700`). That sequence sits inside one `open_*` call, so the hook above doesn't cover it.
  - Audit: `rg -n "open_compute\(|open_copy_encoder\(" rpcs3/Emu/RSX/MTL`. For each call site that issues two or more `copyFrom*`/`dispatch*` that depend on each other, insert the encoder barrier between them.
- **Cost.** Uploads in one encoder are serialised. Redesign R3 replaces this with tracked edges later.

### C4. The blit pass render area ignores the destination origin (seam suspect)
*Severity H · Confidence: verified.*
- `MTLBlit.cpp:283-303` sets `renderTargetWidth = w` and `renderTargetHeight = h`, then draws at viewport and scissor origin `x0,y0`. Metal's render area always starts at (0,0), so a blit into a rectangle with `x0 > 0` or `y0 > 0` writes nothing outside the overlap. API validation reports a scissor outside the render area.
- Every nv3089 scaled copy goes through it (`texture_cache.h:3858` → `MTLBlit.cpp:553` → `:502`), and so do scaled texture-cache copies (`MTLTextureCache.cpp:879`). This happens at **100% scale**. A copy into the right half of a 1280-wide target loses x ≥ 640, which keeps the older sharp image. That matches the reported seam.
- **Fix.**

```cpp
// MTLBlit.cpp:302-303 — after clamping x0/y0 to >= 0
const u32 rt_w = std::min<u32>(static_cast<u32>(x0 + w), dst->width()  >> level);
const u32 rt_h = std::min<u32>(static_cast<u32>(y0 + h), dst->height() >> level);
descriptor->setRenderTargetWidth(rt_w);
descriptor->setRenderTargetHeight(rt_h);
```

  Keep `LoadActionLoad`. The extra area is only the band between 0 and x0.

### A1. Every mid-frame commit makes the RSX thread wait for the GPU
*Severity H (performance) · Confidence: verified.*
- **Shape.** Each frame slot owns one `MTL4::CommandAllocator` and one `MTL4::CommandBuffer` (`MTLCommandRecorder.h:17-56`; `frame_slot_count = 3`, `MTLDevice.h:22`).
- **Path.** A back-end label release (`NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE`) calls `commit_pending_labels()` (`MTLGSRender.cpp:3090-3093`). That ends and commits the slot's buffer and sets `m_reenter_pending` (`MTLCommandRecorder.cpp:614-649`). The next encoder open calls `reenter_slot()` (`:443-469`), which runs `await_slot()` (waits for everything committed so far) and then `allocator->reset()`.
- **The same wait** happens on every readback: `flush_for_readback` (`:409-441`) at 4 sites in `MTLSurface.cpp`, 4 in `MTLTextureCache.cpp`, and `MTLZCULL.cpp:302`. It also happens in `sync_label`.
- **Effect.** Titles that release labels several times per frame (SPURS-driven engines do) lose CPU/GPU overlap inside the frame. The code comment "the slot comes back on the next encoder, after the completion has almost certainly arrived" only holds for light frames.
- **Fix.** One allocator per slot can back several command buffers, encoded one after another. Reset the allocator only when the slot is recycled.

```cpp
// MTLCommandRecorder.h — frame_slot
struct frame_slot
{
	MTL4::CommandAllocator* allocator = nullptr;
	std::vector<MTL4::CommandBuffer*> buffers;   // grows on demand, reused every frame
	u32 open_buffer = 0;                          // index of the buffer being encoded
	bool buffer_open = false;                     // buffers[open_buffer] has begun and not ended
	u64 completion_value = 0;                     // value of the slot's LAST commit
	// ... tables, labels, label_staging_offset unchanged
};

// Begin the next buffer of the open slot without waiting.
MTL4::CommandBuffer* command_recorder::next_buffer(frame_slot& slot)
{
	if (++slot.open_buffer == slot.buffers.size())
		slot.buffers.push_back(m_device->newCommandBuffer());
	MTL4::CommandBuffer* cb = slot.buffers[slot.open_buffer];
	cb->beginCommandBuffer(slot.allocator);
	slot.buffer_open = true;
	return cb;
}

// Commit the open buffer; returns the completion value of that commit.
u64 command_recorder::commit_open_buffer(frame_slot& slot, u32 index)
{
	flush_label_writes();
	close_encoder();
	publish_residency();
	MTL4::CommandBuffer* cb = slot.buffers[slot.open_buffer];
	cb->endCommandBuffer();
	slot.buffer_open = false;
	commit_slot_buffer(slot, index, cb);          // commit + signalEvent(++m_completion_sequence)
	return slot.completion_value;                 // updated by commit_slot_buffer
}

void command_recorder::commit_pending_labels()
{
	frame_slot& slot = m_slots[m_slot];
	if (!slot.buffer_open) return;
	commit_open_buffer(slot, m_slot);
	next_buffer(slot);                            // no wait, no allocator reset
}

void command_recorder::flush_for_readback()
{
	frame_slot& slot = m_slots[m_slot];
	const u64 value = commit_open_buffer(slot, m_slot);
	await_value(value, "readback");               // waits for THIS commit only
	next_buffer(slot);
}

bool command_recorder::begin_frame()
{
	frame_slot& slot = m_slots[m_slot];
	if (!await_slot(slot, "begin frame")) { /* see L1 */ }
	slot.allocator->reset();                      // the only reset point
	slot.open_buffer = 0;
	slot.buffers[0]->beginCommandBuffer(slot.allocator);
	// ... label staging, rings, tables reset as today
}
```

  - Remove `m_reenter_pending` and `reenter_slot()`.
  - Argument tables: Metal 4 captures table contents when each draw is encoded, so per-slot tables stay valid across the slot's buffers.
  - Label staging: offsets keep growing across the slot's buffers and reset in `begin_frame`, as today.
  - `sync_label` for the open slot: `commit_pending_labels()` then `await_value(slot.completion_value)`.
  - **Check with API validation on.** Apple documents that an allocator must not be reset while any of its buffers is in flight, and that buffers sharing an allocator must not be *encoded* concurrently. Encoding them one after another while earlier ones execute is the intended use; confirm on macOS 27.

### C17. The interpreter is resolved before the real pipeline, synchronously, on the RSX thread
*Severity H · Confidence: verified.*
- **Order in `end_draw`.** `interpreter_cache::build_request` and `frame.interpreter.get(...)` run first (`MTLGSRender.cpp:1532-1546`). The real pipeline is looked up after that (`:1566-1578`). The interpreter result is used only when the real pipeline is missing (`:1592-1595`).
- **Cost.** `interpreter_cache::get` (`MTLShaderInterpreter.cpp:897-965`) waits up to **10 s** in `await_library` (`:850-864`), then links with `run_link_sync` on the RSX thread (`:959`). A new material combination stalls the RSX thread even when the real pipeline is ready. The FIFO stops, and guest timeouts look like "FIFO desync". Vulkan only touches the interpreter after a real-pipeline miss.
- **Fix.**

```cpp
// MTLGSRender.cpp — end_draw
const pipeline_state* pipeline = frame.programs.get_graphics_pipeline(..., compile_async);
if (!pipeline && interpreter_allowed)
{
	const auto request = frame.interpreter.build_request(...);
	pipeline = frame.interpreter.try_get(request);   // never blocks
}
if (!pipeline)
{
	m_stats.draws_skipped_compiling++;
	return false;                                    // same contract as Vulkan
}

// MTLShaderInterpreter.cpp
const pipeline_state* interpreter_cache::try_get(const request& r)
{
	if (auto* hit = lookup(r.key)) return hit;
	if (library_state(r.base) != ready) { queue_library(r.base); return nullptr; }
	queue_link(r);                                   // job system, no m_compile_mutex on this thread
	return nullptr;
}
```

  - Prebuild the seven base libraries and the most common link variants at boot on the job system (Dolphin's "PrecompileUberShaders").
  - Delete `run_link_sync` from RSX-thread paths. Keep it only for tests.

### C1. Fast math defeats RSX NaN/Inf handling
*Severity H · Confidence: verified (code) / derived (compiler behaviour; see KosmicKrisp WA14/WA17).*
- `MTLShader.cpp:261` and `:321` use `setMathMode(disable_msl_fast_math ? Safe : Fast)`, and the default is Fast. The shared helpers implement "NaN → 0" with `isnan(x)` (`FragmentProgramDecompiler.cpp:907-925`), and divides are plain `/`. Under `MathModeFast` the compiler may assume no NaN, so `isnan(x)` folds to false. KosmicKrisp shows that even **Relaxed** folds `NAN*0 → 0` with constants.
- The Demon's Souls water line `h0.xyz = precision_clamp(_builtin_div(r1, c[68].yyyy.x), 0., 1.)` has exactly this shape.
- **Fix, layer 1 (robust).** Integer bit tests cannot be folded under any math mode. Apply this to the post-#20078 helpers (rebase first, C22):

```cpp
// FragmentProgramDecompiler.cpp — when device_props.nan_guard_via_bits
"$float4 precision_clamp($float4 x, float _min, float _max)\n"
"{\n"
"	bvec4 nans = greaterThan(floatBitsToUint(x) & 0x7fffffffu, uvec4(0x7f800000u));\n"
"	x = _select(x, $float4(0.), nans);\n"
"	return clamp(x, _min, _max);\n"
"}\n"
```

  Add `bool nan_guard_via_bits` to `device_props` (`Program/GLSLTypes.h` or wherever `device_props` lives). Set it in `MTLFragmentProgram.cpp` and leave it false for VK/GL on x86, so their output is unchanged.
- **Fix, layer 2.** Change the default to `MTL::MathModeRelaxed`. Replace the "Disable MSL Fast Math" bool with an enum `{Relaxed, Fast, Safe}` (section 10), and put it in the cache key (C14).

### T2. Surface upload and readback skip the big-endian swap
*Severity H · Confidence: verified.*
- **Contract.** `get_compatible_gcm_format` returns `swap = true` for every multibyte surface format (`Common/TextureUtils.cpp:1717-1745`, "HW tests prove that all multibyte formats need to swap bytes"). VK stores the flag on framebuffer sections (`VKTextureCache.h:105-122`) and runs `cs_shuffle_16/32` when flushing.
- **Metal.**
  - The framebuffer section `create` never sets `pack_unpack_swap_bytes` (`MTLTextureCache.h:163-169`).
  - `surface::load_background` copies the guest window straight into a `BGRA8Unorm` surface (`MTLSurface.cpp:449-455`).
  - Unscaled readback copies texels straight out (`MTLSurface.cpp:685-706`).
  - The packed16 and depth encoders/decoders use native-endian loads (`MTLFormats.cpp:384-392`, `458-482`); only R6G5B5 swaps (`:513-516`).
- **Effect.** With Read/Write Color Buffers or Read Depth Buffer on, guest-visible surface bytes are reversed (A8R8G8B8 arrives as B,G,R,A). CPU-written surfaces are drawn with wrong colours or depth.
- **Fix.**
  1. Add two small compute kernels to the internal library that `MTLFill.cpp` already builds:

```metal
kernel void bswap32(device uint* d [[buffer(0)]], constant uint& n [[buffer(1)]],
                    uint i [[thread_position_in_grid]])
{ if (i < n) { uint v = d[i]; d[i] = (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24); } }

kernel void bswap16x2(device uint* d [[buffer(0)]], constant uint& n [[buffer(1)]],
                      uint i [[thread_position_in_grid]])
{ if (i < n) { uint v = d[i]; d[i] = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu); } }
```

  2. Choose the kernel by element width: 16-bit for `r5g6b5`, `x1r5g5b5_*`, `w16z16y16x16`, `g8b8` and `z16`; 32-bit for `a8r8g8b8`/`x8*`, `w32z32y32x32`, `x32` and `z24s8`; none for `b8`.
  3. **Upload** (`load_background`):
     - `copyFromBuffer(guest → staging)`.
     - `barrierAfterEncoderStages(Blit → Dispatch)`.
     - `dispatch bswap*` on the staging buffer.
     - `barrierAfterEncoderStages(Dispatch → Blit)`.
     - `copyFromBuffer(staging → texture)`.

     Never dispatch on the guest window itself.
  4. **Readback** (`write_to_memory`): `copyFromTexture(texture → scratch)` → barrier → `bswap*` on scratch → barrier → `copyFromBuffer(scratch → guest)`.
  5. On the CPU paths (`decode_packed16_plane`, `decode_depth_plane`, and the `encode_*` functions), read and write `be_t<u16>`/`be_t<u32>` instead of native types.
  6. In the framebuffer `create` overload, set `pack_unpack_swap_bytes = get_compatible_gcm_format(surf->get_surface_color_format()).second` (depth: `true`), as VK does. The shared flush path then swaps when it is the one copying.
  7. Long term, route surface load through `rsx::upload_texture_subresource` (redesign R5).
  8. Add a unit test: a known guest A8R8G8B8 word, upload, readback, and compare bytes.

### T1. The A4R4G4B4 swizzle is wrong on Apple silicon
*Severity H · Confidence: derived (bit layouts). Confirm with a test texture.*
- On arm64 the format stays packed as `ABGR4Unorm` (`MTLFormats.cpp:587-588`). The uploader writes the byte-swapped guest word A[15:12] R[11:8] G[7:4] B[3:0]. Metal's `ABGR4Unorm` is A[15:12] B[11:8] G[7:4] R[3:0]. So host A = guest A, host B = guest R, host G = guest G, and host R = guest B.
- The code returns `{Red, Alpha, Blue, Green}` (`MTLFormats.cpp:676-680`, A-R-G-B order), based on a comment that misreads VK's non-identity swizzle for `R4G4B4A4_UNORM_PACK16` (`VKFormats.cpp:146-147, 206`).
- **Fix.**

```cpp
// MTLFormats.cpp:676-680 — same permutation as the A1R5G5B5 entry
case CELL_GCM_TEXTURE_A4R4G4B4:
	return { MTL::TextureSwizzleAlpha, MTL::TextureSwizzleBlue, MTL::TextureSwizzleGreen, MTL::TextureSwizzleRed };
```

  Add a table-driven test to `rpcs3/tests/test_rsx_metal4_texture_types.cpp`. For each packed format, take one guest word with distinct channel values, apply the uploader's byte transform, decode it as the Metal pixel format, apply the swizzle, and expect the guest channels.

### T4. 3D texture upload strides one slice by the whole volume
*Severity H · Confidence: verified.*
- `span.rows = height * depth` (`MTLTextureCache.cpp:186-197`), and the copy passes `sourceBytesPerImage = row_pitch * span.rows` with `sourceSize.depth = layout.depth` (`:284-286`). Slice 1 is read past the end of the volume.
- **Fix.**

```cpp
// MTLTextureCache.cpp — keep span.size for allocation, add the per-slice stride
span.image_rows = expanded_blocks ? layout.height_in_texel : layout.height_in_block;
span.rows       = span.image_rows * layout.depth;
span.size       = span.row_pitch * span.rows;
...
encoder->copyFromBuffer(staging, span.offset, span.row_pitch, span.row_pitch * span.image_rows,
	MTL::Size::Make(layout.width_in_texel, layout.height_in_texel, layout.depth),
	dst->value(), slice, layout.level, MTL::Origin::Make(0, 0, z));
```

  Apply the same change to the depth-plane branch (`:262-275`): `plane_pitch * layout.height_in_texel` per image.

### T5. Blits whose source and destination are the same image are dropped
*Severity H · Confidence: verified.*
- `blit_pass::scale_image` logs and returns when `src->image() == dst->image()` (`MTLBlit.cpp:518-522`). The shared code has already chosen the GPU route, so nothing happens. VK copies through a temporary (`VKTexture.cpp:631`). In-place scaling, blur and stretch blits are common.
- **Fix.**

```cpp
// MTLBlit.cpp — before encoding the scaled pass
texture* source = src;
std::unique_ptr<texture> temp;
if (src->image() == dst->image())
{
	MTL::TextureDescriptor* d = MTL::TextureDescriptor::texture2DDescriptor(
		src->pixel_format(), src_w, src_h, false);
	d->setUsage(MTL::TextureUsageShaderRead);
	temp = std::make_unique<texture>(&resources);
	temp->assign(resources.new_texture(d, MTL::StorageModePrivate));
	d->release();

	auto* enc = cmd.open_copy_encoder();          // S1 barrier applies
	enc->copyFromTexture(src->value(), 0, src_level, MTL::Origin::Make(src_x, src_y, 0),
		MTL::Size::Make(src_w, src_h, 1), temp->value(), 0, 0, MTL::Origin::Make(0, 0, 0));
	source = temp.get();                          // sample from (0,0) with src area rebased
	resources.defer_release(temp->value());
}
```

  Rebase the source rectangle to (0,0) when sampling from `temp`. The render pass that follows is a new encoder, so the encoder-end barrier orders the copy before the sample.

### T3. RSX MSAA is resolved by hardware averaging; unresolve does nothing
*Severity H · Confidence: verified.*
- **RSX layout.** 2x and 4x samples live in guest memory as a 2×1 or 2×2 mosaic per pixel. VK's resolve target is `width*samples_x` by `height*samples_y`, filled by compute scatter and gather (`VKRenderTargets.cpp:325-337`, `VKResolveHelper.cpp`).
- **Metal.**
  - The resolve image is pixel-sized (`MTLSurface.cpp:79-84`) and filled by `StoreAndMultisampleResolve`, which averages.
  - `unresolve()` only clears the flag (`:523-529`), so a CPU-loaded MSAA surface is drawn over stale samples.
  - The texture cache skips sample scaling (`MTLTextureCache.cpp:399-402`).
- **Fix.**
  1. Create `resolve_surface` at `width*samples_x` × `height*samples_y` (from `surface_utils.h` sample metrics).
  2. **Resolve** is a compute kernel reading the multisample texture:

```metal
kernel void rsx_resolve(texture2d_ms<float> src [[texture(0)]],
                        texture2d<float, access::write> dst [[texture(1)]],
                        constant uint2& grid [[buffer(0)]],   // (samples_x, samples_y)
                        uint2 p [[thread_position_in_grid]])
{
	if (p.x >= src.get_width() || p.y >= src.get_height()) return;
	for (uint s = 0; s < src.get_num_samples(); ++s)
	{
		const uint2 o = uint2(s % grid.x, s / grid.x);        // match VKResolveHelper index math
		dst.write(src.read(p, s), p * grid + o);
	}
}
```

     Depth needs a depth texture variant (`depth2d_ms`) writing to an `R32Float` resolve view. Stencil uses `texture2d_ms<uint>` on the stencil view.
  3. **Unresolve** is a render pass into the MSAA image with a fragment function at sample rate:

```metal
fragment float4 rsx_unresolve(float4 pos [[position]], uint sid [[sample_id]],
                              texture2d<float> src [[texture(0)]], constant uint2& grid [[buffer(0)]])
{
	const uint2 p = uint2(pos.xy) * grid + uint2(sid % grid.x, sid / grid.x);
	return src.read(p);
}
```

     Depth writes `[[depth(any)]]`; stencil uses `[[stencil]]` output (stencil export is available on Apple GPUs).
  4. Use the order of samples in VK's `cs_resolve` and `cs_unresolve` so both backends agree.
  5. Drop the special case at `MTLTextureCache.cpp:399-402` and scale regions by `samples_x/y` as VK does (`VKTextureCache.h:241-244`).
  6. Keep the hardware average only for the present path.

### T6. Inherited surface contents are copied without a format or aspect check
*Severity H · Confidence: reported.*
- `flush_old_contents` always calls `copyFromTexture` (`MTLSurface.cpp:298-302`). VK goes typeless when formats or aspects differ (`VKRenderTargets.cpp:1058-1123`). `get_texel_size` reports `Depth32Float_Stencil8` as 8 bytes (`MTLFormats.cpp:244-246`), while VK reports 4 (the z24s8 guest size), so the typeless path in `copy_transfer_regions_impl` (`MTLTextureCache.cpp:783-844`) scales rectangles wrongly.
- **Fix.**
  1. Change `get_texel_size(Depth32Float_Stencil8)` to return 4 *for transfer arithmetic* (guest bpp). Keep a separate `host_texel_size` where real host bytes are needed.
  2. Write one `copy_typeless(cmd, src, dst, src_rect, dst_rect)`:
     - Same `MTL::PixelFormat` → `copyFromTexture`.
     - Same byte width, colour ↔ colour → stage through a buffer: `copyFromTexture(src → buf)`, barrier, `copyFromBuffer(buf → dst)`.
     - Depth ↔ colour → conversion kernel. D32F+S8 → guest z24s8: `uint(depth * 16777215.0) << 8 | stencil`. The reverse unpacks. Read depth with `BlitOptionDepthFromDepthStencil` and stencil with `StencilFromDepthStencil` into separate buffers first. This mirrors VK's `cs_shuffle_d32fx8_to_x8d24f` and related jobs in `VKCompute.h`.
  3. Call it from `flush_old_contents` and from `copy_transfer_regions_impl`.

### C8 + C2. The scissor is mirrored, and resolution scale is ignored by the viewport, scissor and pass size
*Severity H · Confidence: verified.*
- **C8.** `MTLRasterState.cpp:137-145` computes `y = clip_height - y2`. The viewport `{0, H, W, -H}` (`:126-135`) maps NDC to the same window rows as Vulkan's positive viewport, so the scissor must not be mirrored. VK (`VKGSRender.cpp:1355-1358`), GL and KosmicKrisp (`kk_cmd_draw.c:750-808`) don't mirror it. The subtraction also wraps when `y2 > clip_height`.
- **C2.** `clip_width/height = layout.width/height`, unscaled (`MTLSurfaceState.cpp:53-54`). These feed `build_viewport` (`MTLEncoderState.cpp:49-50, 93`) and the pass size (`MTLFramebuffer.cpp:131-132, 228-229`), while the attachments are scaled (`MTLSurfaceCache.cpp:21-26`). At scale ≠ 100%, draws cover only the top-left part of the target.
- **Fix.**

```cpp
// MTLSurfaceState.cpp
std::tie(state.clip_width, state.clip_height) =
	rsx::apply_resolution_scale<true>(ctx.scaling, layout.width, layout.height);

// MTLFramebuffer.cpp — m_width/m_height = min over attachments of the real (scaled) size

// MTLRasterState.cpp
MTL::ScissorRect build_scissor_rect(u32 x1, u32 y1, u32 x2, u32 y2, u32 rt_w, u32 rt_h)
{
	x1 = std::min(x1, rt_w); x2 = std::clamp(x2, x1, rt_w);
	y1 = std::min(y1, rt_h); y2 = std::clamp(y2, y1, rt_h);
	return MTL::ScissorRect{ x1, y1, x2 - x1, y2 - y1 };
}
```

  Update both callers (draws `MTLGSRender.cpp:~1888`, clears `:~1188`) to pass the pass size. The clear path needs the scaled viewport too. `metal4-dev`'s F60 change implements most of this.

### C19. The viewport depth range is applied on top of the shader's depth transform
*Severity H · Confidence: verified.*
- `MTLEncoderState.cpp:91-93` sets `znear/zfar = clip_min/clip_max`. The vertex shader always uses the fp32 fallback z transform (`MTLVertexProgram.cpp:279-283`), which outputs final window z and expects a [0,1] viewport (`RSXVertexPrologue.glsl:7-12, 32-52`). VK and GL only set a non-default range with the STANDARD form (`VKGSRender.cpp:1328-1336`).
- **Fix.**

```cpp
// MTLEncoderState.cpp:91-93
encoder->setViewport(build_viewport(state.clip_width, state.clip_height, 0.0, 1.0));
```

### C27. A failed link or library is latched forever; the retry branch is dead
*Severity H · Confidence: verified.*
- `MTLProgramPipeline.cpp:696-701` retries only when `work->error[0] == '\0' && attempts < 2`. Every failure path writes an error (`:342`, `:553`, `:614`), so it never retries. Failures are latched in `m_failures` (`:735-742`) and refused at `submit` (`:444`). The library stays `failed` (`:303`).
- **Fix.**

```cpp
// MTLProgramPipeline.cpp
static bool is_hard_compile_error(std::string_view e)
{
	return e.find("unsupported") != umax || e.find("error: ") != umax /* MSL syntax */ ||
	       e.find("validation") != umax;
}
...
if (!is_hard_compile_error(work->error) && work->attempts < 3)
{
	work->error[0] = '\0';
	work->not_before = steady_clock::now() + 50ms * (1u << work->attempts++);
	m_library_state = library_state::idle;          // clear the library latch on a soft retry
	requeue(std::move(work));
	return;
}
m_failures.insert(work->key);                      // hard: latch, and count it in the overlay
```

### C14. Archive and shader-cache keys miss everything that changes generated code
*Severity H · Confidence: verified.*
- The archive key is `archive-<registryID>-<macOS major>.bin` (`MTLDevice.cpp:138-142`). The shader cache key is `shader_cache(programs, "metal", "v1")` (`MTLGSRender.cpp:763`). Neither includes the generator version, math mode, invariance or build. Settings experiments silently run old binaries.
- **Fix.**

```cpp
// MTLDevice.h
constexpr u32 MTL_GENERATOR_VERSION = 2;   // bump on any MSL/GLSL emitter change

// MTLDevice.cpp
const std::string key = fmt::format("g%u-m%u-i%u-%s", MTL_GENERATOR_VERSION,
	static_cast<u32>(g_cfg.video.metal_math_mode.get()), g_cfg.video.metal_invariance ? 1 : 0,
	rpcs3::get_commit_and_hash().second);
m_archive_path = fmt::format("%sshaders_cache/metal/archive-%llx-%u-%s.bin",
	root, m_device->registryID(), macos_major(), key);

// MTLGSRender.cpp:763
m_shader_cache = std::make_unique<shader_cache>(programs, "metal", "v2-" + key);
```

  The commit hash makes every build cold. Use the generator version alone for releases, and add the hash only in development builds.

### L1. A slot that times out is treated as idle, so in-flight resources are freed and rings overwritten
*Severity H · Confidence: verified.*
- On a 10 s feedback timeout, `begin_frame` calls `allocate_slot` (which `release_slot`s the old allocator, buffer and tables) and sets `completion_value = 0` (`MTLCommandRecorder.cpp:297-303`). `slot_idle()` returns true for `completion_value == 0` (`:161-165`), so `ring_idle()` can be true while the abandoned buffer still runs. `drain_quarantine()` then frees what was just quarantined (`:325-332`). `label_staging_offset` and the per-slot constant rings are reset and overwritten. `reenter_slot` (`:456-464`) does the same without quarantining.
- **Fix, recommended simple form.** A 10 s GPU stall is a hang. Treat it as device loss instead of recycling:

```cpp
if (!reported)
{
	rsx_log.fatal("Metal 4: slot %u did not complete in %u s; the GPU is hung", m_slot, recorder_feedback_timeout);
	m_device_lost = true;                         // check_device_status() reports it
	return false;                                 // caller stops recording; no release, no ring reset
}
```

  `MTLGSRender::check_device_status` returns `rsx::device_status::lost`. The core then stops issuing draws, and the UI can tell the user.
- **Fix, recovery form** (if recovery matters more):
  - Keep the old `completion_value`, and add `u64 abandoned_value`, which `slot_idle` and `ring_idle` treat as busy until `signaledValue() >= abandoned_value`.
  - Move the old allocator, buffers and tables into a `graveyard` entry tagged with that value, and allocate new ones without `release_slot`.
  - Skip `constants.begin_frame(slot)` and the label-staging reset for that slot until the value signals.
  - Free graveyard entries in `begin_frame` once their value is reached.

### L2. Shutdown frees the resource manager even when the ring did not drain
*Severity H · Confidence: reported.*
- `recorder.shutdown()` skips `release_slot` on a failed drain (`MTLCommandRecorder.cpp:222-276`), but `on_shutdown` continues. It runs `present.shutdown()`, destroys the caches, and `resources.shutdown()` → `garbage_collect()` → `release_list` (`MTLGSRender.cpp:830-846`).
- **Fix.**

```cpp
// MTLCommandRecorder: make shutdown report the drain
bool command_recorder::shutdown();                 // false when the ring did not drain

// MTLGSRender::on_shutdown
if (!m_frame_resources->recorder.shutdown())
{
	rsx_log.error("Metal 4: GPU work still in flight at shutdown; leaking the backend to stay safe");
	(void)m_frame_resources.release();             // intentional leak
	(void)m_device.release();
	return;
}
m_frame_resources->present.shutdown();
// ... rest unchanged
```

### L3. Commit feedback blocks capture `this` and are never drained; GPU errors count as success
*Severity H · Confidence: verified.*
- `addFeedbackHandler([this, index, generation] ...)` (`MTLCommandRecorder.cpp:803`). Waits poll the shared event, which is not ordered with the CPU callback, so a block can run after the recorder is destroyed. `on_feedback` dereferences `error->localizedDescription()` without a null check (`:827-830`). A GPU error only logs, `m_completed` still advances (ZCULL reads it as "result visible"), and the device is not marked lost.
- **Fix.**

```cpp
// MTLCommandRecorder.h
struct feedback_owner
{
	std::mutex lock;
	command_recorder* recorder = nullptr;          // null after shutdown
	std::atomic<u32> in_flight{0};
};
std::shared_ptr<feedback_owner> m_feedback = std::make_shared<feedback_owner>();

// commit_slot
m_feedback->recorder = this;
m_feedback->in_flight.fetch_add(1);
options->addFeedbackHandler([owner = m_feedback, index, generation](MTL4::CommitFeedback* fb)
{
	{
		std::lock_guard lk(owner->lock);
		if (owner->recorder) owner->recorder->on_feedback(index, generation, fb);
	}
	owner->in_flight.fetch_sub(1);
	owner->in_flight.notify_all();
});

// on_feedback
if (NS::Error* error = feedback->error())
{
	const NS::String* text = error->localizedDescription();
	rsx_log.error("Metal 4: commit failed: %s", text ? text->utf8String() : "(no description)");
	m_device_lost.store(true);
	return;                                        // do not advance m_completed
}

// shutdown, after the event wait
for (u32 v; (v = m_feedback->in_flight.load()) != 0;) m_feedback->in_flight.wait(v);
std::lock_guard lk(m_feedback->lock);
m_feedback->recorder = nullptr;
```

  Bound the `in_flight` wait with the same timeout as the event wait. On timeout, null the owner and leak (as in L2).

### L5. Overlay images are rewritten in place while an earlier frame may still sample them
*Severity H · Confidence: verified.*
- `find_temp_image` calls `upload(...)` → `replaceRegion` on a cached `StorageModeShared` texture whenever `desc->dirty` is set (`MTLOverlays.cpp:568-580`, `527-529`). Up to three frames are in flight. The result is torn trophy pop-ups, performance graphs and dialogs.
- **Fix.**

```cpp
// MTLOverlays.cpp — find_temp_image
if (cached != m_temp_cache.end())
{
	if (dirty)
	{
		resources.defer_release(cached->second);              // freed when this slot retires
		cached->second = create_image(resources, desc->w, desc->h, 1);
		upload(cached->second, desc->get_data(), desc->w, desc->h, 1, false);
		resources.make_persistent(cached->second);
	}
	return cached->second;
}
```

  Apply the same rule to `find_font` when an atlas is updated in place. Any CPU write to a texture an in-flight slot may read must go to a new object, or through a per-slot copy.

### C20. `PixelFormatView` disables lossless compression on every cached texture
*Severity H (performance) · Confidence: verified.*
- `MTLTextureCache.h:364` uses `ShaderRead | PixelFormatView | RenderTarget`, and `MTLSurface.cpp:75` adds `PixelFormatView` to colour surfaces. Apple: lossless compression is not applied when this flag is set. The views actually used keep the same pixel format with a swizzle (`MTLHandles.cpp:66`). Only the depth surface's `X32_Stencil8` stencil view changes layout (`MTLSurface.cpp:161`).
- **Fix.**

```cpp
// MTLTextureCache.h:364
descriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
// MTLSurface.cpp:75 — colour surfaces: drop PixelFormatView; depth-stencil surfaces keep it
```

  sRGB ↔ linear views (T8) don't need the flag. SNORM ↔ UNORM views do, so add it only to formats that are remapped that way.

### D1. The blend-constant block is 36 bytes, so every block after the first is misaligned
*Severity H · Confidence: verified (layout); derived (what the GPU reads).*
- `blend_values` is 36 bytes (`MTLConstants.h:77-86`). It lives in a `block_ring`, which strides by `sizeof(T)` (`address_of`, `MTLConstants.h:229`), and each draw binds its own block at `fragment_blend_slot` (`MTLGSRender.cpp:2240-2266`, `blend_values_block_address`).
- The shader's block (`MTLFragmentProgram.cpp:214-221`) puts `vec4 blend_constants` at byte 16. In MSL that is a `float4` with 16-byte alignment, and the struct is 48 bytes.
- Block *k* starts at byte 36·*k*. Its `float4` is aligned only when *k* is a multiple of 4. Blocks 1, 2 and 3 are off by 4, 8 and 12 bytes, and the pattern repeats. The shader also reads 48 bytes per block, so the last block in a full buffer reads 12 bytes past the end.
- **Effect.** Three out of four programmable-blend or logic-op draws in a frame read the constant colour and `logic_op` from a misaligned address. The result is undefined. Expect a wrong blend colour or a wrong logic op that changes with the draw's position in the frame, not with its content. Vulkan sends this block as a push constant, so it has no stride and no such bug.
- `blit_values` is padded to 16 bytes for exactly this reason (comment at `MTLConstants.h:100-104`). The other ring types are fine: `vertex_context` 96, `sampler_info` 48, `fill_values` 32, `fragment_context` 32, and `draw_parameters` 168 (its GLSL struct has 8-byte alignment, so its stride is 168).
- **Fix.**

```cpp
// MTLConstants.h
struct alignas(16) blend_values
{
	u32 reserved;
	u32 blend_eqn;
	u32 blend_sfactors;
	u32 blend_dfactors;
	f32 blend_constants[4];
	u32 logic_op;
	u32 pad[3];
};
static_assert(sizeof(blend_values) == 48);
static_assert(offsetof(blend_values, blend_constants) == 16);
```

  To stop a repeat, give `block_ring` a template parameter for the shader-side stride and assert it, instead of trusting `sizeof(T)`:

```cpp
template <typename T, usz ShaderStride = sizeof(T)>
class block_ring
{
	static_assert(sizeof(T) == ShaderStride, "C++ block and MSL block strides differ");
	// ...
};

block_ring<blend_values, 48> blend_values;   // MSL push_constants_block is 48 bytes
```

  A blanket `static_assert(sizeof(T) % 16 == 0)` won't work, because `u32`, `s32` and `draw_parameters` are valid rings whose sizes aren't multiples of 16. Back the explicit strides with a CPU test (H4) that runs each block's GLSL through SPIRV-Cross and compares the reflected MSL struct size with `sizeof(T)`.

---

## 4. Should-fix findings (severity M)

### A2. A label release always splits the open render pass
*Confidence: verified.*
- Labels are written by a compute-encoder `copyFromBuffer` (`MTLCommandRecorder.cpp:727-768`), so a release inside a pass closes the render encoder (tile store, then reload of every attachment). A back-end release also commits (A1).
- **Fix (after A1).**
  1. Don't commit in `release_GCM_label` (`MTLGSRender.cpp:3090-3093`). Record the label, and mark `m_label_commit_deadline = now + 150us`.
  2. Commit pending labels at the first of these:
     - The next natural pass boundary (`close_encoder` already flushes labels into the stream; add `commit_pending_labels()` there when a deadline is set).
     - `on_semaphore_acquire_wait` (already a hook).
     - The deadline, checked in `on_local_task` while the FIFO is parked.
  3. Never split an open pass just for a label.

### C26. The interpreter's synchronous link shares the worker mutex
*Confidence: verified.*
- The link worker holds `m_compile_mutex` for each `newRenderPipelineState` (`MTLProgramPipeline.cpp:352-383`). `run_link_sync` (`:526-543`) takes it on the RSX thread.
- **Fix.** C17 removes the synchronous link from the RSX thread. C30 and section 8.3 remove the global mutex: one `MTL4Compiler` is concurrency-safe on macOS 27 (KosmicKrisp's WA11 note). Keep a mutex only around the serializer if validation shows it is not re-entrant.

### C30. Metal's compiler concurrency is left at the default
*Confidence: verified.*
- `shouldMaximizeConcurrentCompilation` is never set (the default is NO).
- **Fix.**

```cpp
// device::create, after the device is obtained
m_device->setShouldMaximizeConcurrentCompilation(true);
// link worker count
const u32 workers = std::clamp<u32>(g_cfg.video.shader_compiler_threads_count
	? g_cfg.video.shader_compiler_threads_count
	: static_cast<u32>(m_device->maximumConcurrentCompilationTaskCount()), 1, 16);
```

### C29. The pipeline archive is written only on a clean exit
*Confidence: verified.*
- `flush_pipeline_archive` runs once at teardown (`MTLDevice.cpp:215-238`), because `serializeAsArchiveAndFlushToURL` is destructive. A crash or force-quit loses the whole session.
- **Fix.**
  1. Name archives per session: `archive-<id>-<os>-<key>-<unix_time>.bin`.
  2. At boot, list all matching archives (newest first) and pass them all to `lookupArchives`.
  3. Every 256 new pipelines or 5 minutes, at a frame boundary: create a new serializer and a new `MTL4Compiler` bound to it, swap the compiler pointer under the job-system lock, then flush the old serializer to its session file. WA11 is fixed on macOS 27, so two compilers briefly coexisting is safe.
  4. On boot, if there are more than 8 archives, compact them: preload the RPCS3 shader cache with one serializer attached, flush one archive, and delete the rest.

### C5. No position invariance
*Confidence: verified.*
- `require_explicit_invariance` is never set (`MTLVertexProgram.cpp:275-283, 443-451`), and `setPreserveInvariance` is never called. KosmicKrisp turns invariance on by default. Upstream #19815 (BioShock) is an invariance report on Apple hardware.
- **Fix.** `props.require_explicit_invariance = true;` in both vertex emitters. `options->setPreserveInvariance(true);` next to `setMathMode` (`MTLShader.cpp:261, 321`). Add a setting (default on) and put it in the C14 key.

### C10. The depth-bounds test is dropped
*Confidence: verified.*
- `MTLEncoderState.cpp:70-80`: the native setter traps on this driver, and the shader fallback was never written.
- **Fix.**
  1. Add a pipeline key bit `depth_bounds` (set when `depth_bounds_test_enabled()`).
  2. Emit the test in the fragment epilogue only when the bit is set:

```glsl
// fragment epilogue
if (gl_FragCoord.z < depth_bounds.x || gl_FragCoord.z > depth_bounds.y) discard;
```

  3. Feed `depth_bounds` (min, max) from the fragment constants block. `gl_FragCoord.z` is window z because of C19.

### C23. Apple's compiler removes `clamp16` half round trips
*Confidence: derived (upstream #19870).*
- Upstream #20078 already rewrote `clamp16` with integer bit tests (`FragmentProgramDecompiler.cpp:1019-1027` upstream). The fork predates it.
- **Fix.** Rebase (C22). Upstream's `_builtin_f16_sw_quantize` is integer-based and survives fast math.

### C13. The guest-memory present fallback reads stale bytes and has no format cast
*Confidence: verified.*
- `get_present_source` reads `vm::get_super_ptr(address)` raw (`MTLGSRender.cpp:2573-2661`). VK first flushes every unsynchronised `blit_engine_dst` or `framebuffer_storage` section overlapping the range (`VKPresent.cpp:366-388`), and force-casts a mismatched format (`:392+`).
- **Fix.**

```cpp
// MTLGSRender.cpp — get_present_source, before the CPU upload
const auto range = utils::address_range32::start_length(address, pitch * height);
auto overlapping = m_texture_cache.find_texture_from_range<true>(range, 0,
	rsx::texture_upload_context::blit_engine_dst | rsx::texture_upload_context::framebuffer_storage);
bool flushed = false;
for (auto* section : overlapping)
{
	if (!section->is_synchronized()) { section->copy_texture(cmd, true); flushed = true; }
}
if (flushed) { cmd.flush_for_readback(); }
m_texture_cache.invalidate_range(cmd, range, rsx::invalidation_cause::read);
```

  For the format cast, when the source surface's format differs from the AV output format, create a scratch BGRA8 texture and run the `copy_typeless` path (T6) into it.

### C11. Samplers are bound by `gpuResourceID` without `supportArgumentBuffers`
*Confidence: verified.*
- **Fix.** `descriptor->setSupportArgumentBuffers(true);` in `MTLSamplerState.cpp:~250` and `MTLBlit.cpp:~123`.

### C9. Draws are dropped silently when a ring allocation fails
*Confidence: verified.*
- There is a bare `return;` at `MTLGSRender.cpp:2076, 2085, 2110, 2135, 2180, 2245`.
- **Fix.**

```cpp
if (!alloc)
{
	m_stats.ring_drops[ring_id]++;
	rsx_log.error("Metal 4: %s ring exhausted (frame %llu), draw dropped", ring_name(ring_id), m_frame_index);  // once per frame
	return;
}
```

  Show `ring_drops` in the performance overlay. Grow the ring next frame when drops were seen.

### F6. Guest reads inside an open pass trigger flushes that end the encoder
*Confidence: verified.*
- Index, vertex and constant copies read guest memory inside the pass. A read that hits a protected page flushes a section and ends the render encoder. The restart path reopens with LOAD and replays state (`MTLGSRender.cpp:1891-1899`, `2270-2321`). The TODO at `:3236-3240` is the same problem. 16 KB host pages make it worse: the texture cache protects 4 KB units (`texture_cache_utils.h:464`), but `utils::memory_protect` rounds to 16 KB (`vm_native.cpp:325,454`).
- **Fix.**
  1. In `begin_draw`, before `open_pass`, walk the draw clause's vertex, index and constant ranges, and call `m_texture_cache.invalidate_range(cmd, range, rsx::invalidation_cause::read)` on each. This triggers any flush while no pass is open.
  2. On Apple hosts, in `texture_cache::lock_memory_region`, don't protect a 16 KB host page that a section covers only partly. Mark the section `unprotected` and verify it by hash at bind time (the existing "unprotected + hash" mode).
  3. Count restarts in the overlay.

### R1. The GPU's guest window is a snapshot of the mapping at import
*Confidence: verified.*
- `MTLDevice.cpp:240-254` imports `vm::g_sudo_addr` once as a no-copy `MTLBuffer`. `shm`-backed allocations made later (user 64K/1M areas, mmapper shared memory, RawSPU and RSX context areas) are mapped after import (`vm.cpp:846`), so GPU access there reads the old pages.
- **Fix.** Extend `vm::on_notify_memory_mapped` (`vm.cpp:808-811`, today 1 MB pages only) to all page sizes. In the Metal handler, create a per-block no-copy buffer import for the new block, and register it in `resource_manager` keyed by address range. Resolve guest addresses through that map, falling back to the window. As a stopgap, route GPU copies outside `0x00010000-0x2FFFFFFF` and `0xC0000000-0xCFFFFFFF` through a CPU staging copy.

### F2. Numeric PUT compares fail when PUT rewinds to a lower address
*Confidence: derived (fork's FIFO code).*
- `RSXFIFO.cpp:189` and `:217-220` use unsigned compares. After a rewind (`addr > put`), the fill reads a full block ahead.
- **Fix.** Compare in ring order with GET as the origin:

```cpp
static inline bool before_put(u32 addr, u32 get, u32 put)   // modulo 2^32
{ return (addr - get) < (put - get); }
const u32 cache_end = before_put(addr, get, put) ? std::min(put, addr + sizeof(m_cache)) : addr;
```

  Needs a guest A/B test. It affects all backends in the fork.

### F3. `cellGcmCallback` ordering differs from upstream
*Confidence: verified.*
- The release branch writes the jump word before `ctrl.put.exchange` (`cellGcmSys.cpp:~1582-1592`). `metal4-dev` reverts this.
- **Fix.** Revert to upstream order unless an A/B test shows the fork's order fixes a title.

### F4. Metal-specific RSX-thread waits
*Confidence: verified.*
- `begin_frame` slot waits (`MTLGSRender.cpp:868-872`), `nextDrawable` (up to 1 s), readbacks, and `sync_label`.
- **Fix.** A1 (readbacks wait only for their own commit), plus the present thread (redesign R6).

### L4. Present reads Qt and AppKit state from the RSX thread
*Confidence: reported (AppKit part verified by reading `MTLMetalLayer.mm`).*
- `present_frame` calls `m_frame->client_width()/client_height()` (`MTLGSRender.cpp:2807-2816`). Upstream VK and GL do the same, so that part is shared. The Metal-specific part:
  - `metal_layer_visible` reads `NSView.hiddenOrHasHiddenAncestor` and `NSWindow.occlusionState`.
  - `set_metal_layer_drawable` writes `drawableSize` and `pixelFormat` from the RSX thread (`MTLMetalLayer.mm:110-119`, `192-219`).
  - The adopted layer isn't retained (`:73-80`).
- **Fix.**
  1. In `gs_frame` (main thread), on resize, DPI change and occlusion notifications, store `width`, `height` and `visible` into `std::atomic`s on the display output.
  2. `present_frame` reads only those atomics.
  3. Set `drawableSize` and `pixelFormat` on the main thread (`dispatch_async(dispatch_get_main_queue(), ...)` when the size changes). The RSX thread compares `drawable.texture.width/height` with the target and skips one frame on mismatch.
  4. `[layer retain]` in `attach_metal_layer`, `[layer release]` in `detach_metal_layer`.

### L6. `mtl::device` never releases what it owns; a failed shader shutdown can leave two compilers
*Confidence: reported.*
- `~device` only removes the notification observer (`MTLDevice.cpp:78-84`). The device, queue, compiler, serializer, archive, task options and guest window buffer are never released. `init` and the archive flush create autoreleased objects on the RSX thread with no pool. On a shader-job shutdown timeout (`MTLProgramPipeline.cpp:839-855`), the link worker isn't stopped, and a later `on_init` creates a second compiler.
- **Fix.**

```cpp
device::~device()
{
	if (m_removal_observer) { NS::NotificationCenter::defaultCenter()->removeObserver(m_removal_observer); }
	auto rel = [](auto*& p) { if (p) { p->release(); p = nullptr; } };
	rel(m_task_options); rel(m_archive); rel(m_serializer); rel(m_compiler);
	rel(m_guest_window); rel(m_queue); rel(m_device);
}
// device::create / flush_pipeline_archive
NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
// ...
pool->release();
```

  Add a process-wide `std::atomic<bool> g_mtl_abandoned_worker`. Set it on the job-shutdown timeout. `MTLGSRender::on_init` refuses to create a device while it is set, and shows "restart RPCS3 to use Metal again".

### L7. Residency entries are removed at free time, and empty heaps are never retired
*Confidence: reported.*
- `release_list` calls `m_residency.remove(allocation)`, which does `removeAllocation` + `commit()` immediately (`MTLResourceManager.cpp:237-252`, `533-548`). A dedicated heap (`size * 2 > block_size`, `:68-79`) stays resident after its only span is freed.
- **Fix.**
  1. `residency_manager::remove` queues the removal. `flush_residency()` (already called before every commit) applies the queued removals and commits once.
  2. `heap_pool::free`: when a dedicated heap has no live spans, move it to `m_empty_heaps` tagged with the current completion value. In `garbage_collect`, once the value is reached, call `removeAllocation(heap)` (queued) and `heap->release()`.

### T7. Guest z24s8 stencil is never uploaded
*Confidence: reported.*
- `load_background` decodes only the depth plane and sets a flag (`MTLSurface.cpp:406-423`). Readback extracts stencil, so data doesn't round-trip.
- **Fix.** In the decode, also write the low byte of each (byte-swapped) guest word to a `u8` stencil plane. Then `copyFromBuffer(stencil_plane, ..., BlitOptionStencilFromDepthStencil)` into the depth-stencil texture, after the depth copy, with an encoder barrier between them (S1).

### T8. Gamma textures and depth-compare samplers are forced to nearest filtering
*Confidence: verified.*
- `force_nearest` includes `depth_compare` and `hw_SRGB_possible()` (`MTLSamplerState.cpp:199-201`), because gamma is applied in the shader after the fetch. VK samples an sRGB view, so the hardware decodes before filtering, and keeps linear filtering for PCF.
- **Fix.**
  1. Add `MTL::PixelFormat to_srgb(MTL::PixelFormat)`: `BGRA8Unorm → BGRA8Unorm_sRGB`, `RGBA8Unorm → RGBA8Unorm_sRGB`, `BC1_RGBA → BC1_RGBA_sRGB`, `BC2_RGBA → BC2_RGBA_sRGB`, `BC3_RGBA → BC3_RGBA_sRGB`, and none otherwise.
  2. When `hw_SRGB_possible()` and `to_srgb()` returns a format, bind a view with that format (the view cache in `MTLHandles.cpp` keys on format and swizzle). Clear the texture's gamma bits in the fragment texture-control word, as VK does (`VKDraw.cpp:383-395`).
  3. `force_nearest = (texel_remap_control & SEXT_MASK) != 0;` Depth compare keeps the requested min/mag filter, so the sampler does hardware PCF.
  4. sRGB ↔ linear views don't need `PixelFormatView` (C20).

### T9. Vertex-texture LOD range is cleared
*Confidence: reported.*
- **Fix.** `MTLSamplerState.cpp:214-220`: keep nearest filters and `lod_bias = 0`, and set `state.min_lod = tex.min_lod(); state.max_lod = tex.max_lod();` as the fragment path does (`:167-172`).

### C6. Sampling an attachment of the open pass is only detected when an encoder is open
*Confidence: verified.*
- `policy.samples_attachment = policy.encoder_open && samples_open_pass()` (`MTLGSRender.cpp:1821-1831`). The texture cache always returns a copy (`requires_feedback_loop_copy`), so this is defence in depth.
- **Fix.** Check against the attachments the draw is about to bind as well. In debug builds, `ensure()` that no sampled texture is also an attachment.

### C18. Draws are skipped while compiling, and the interpreter refuses many programs
*Confidence: verified.*
- `build_request` (`MTLShaderInterpreter.cpp:634-728`) rejects vertex textures, depth-range or depth-compare emulation, alpha-kill, format convert, programmable blending, logic op, shadow or redirected units, MSAA textures and no-colour-output programs.
- **Fix, staged.** Add shadow compare and alpha-kill to the ubershader first (both are a few lines: a compare in the texture fetch and a `discard` on a uniform flag). Then vertex textures. Track "interpreter refused" in the overlay to see which reasons matter.

### C22. Upstream fixes missing from the fork
*Confidence: verified.*
- #20078 (FP precision rewrite), #20133 (programmable-blend typo), #20154 (compile abort logic), and #20111 / #20109 (ARM decommit and reserve races).
- **Fix.** Rebase. Do H1 first, so the NV47 files don't conflict on line endings.

### C25. The host mprotect queue isn't flushed before submit
*Confidence: verified.*
- **Fix.** Call `rsx::mm_flush()` at the top of `command_recorder::submit` (`MTLCommandRecorder.cpp:357`) and of `commit_open_buffer` (A1). It is cheap when the queue is empty. VK does this at `VKGSRender.cpp:2518`.

### D2. The RSX MSAA sample mask is never applied
*Confidence: verified.*
- The comment at `MTLDepthStencil.h:73-77` says the per-draw sample mask "travels in the MSL as `[[sample_mask]]`". Nothing implements that. `anti_aliasing_state` holds only alpha-to-coverage and alpha-to-one, no shader writes `gl_SampleMask`, and no code reads `msaa_sample_mask()` (`rsx_methods.h:1282`).
- Vulkan puts the mask in the pipeline's multisample state (`VKGSRender.cpp:387-396`, `VKPipelineCompiler.cpp:153`). GL calls `glSampleMaski` (`GLDraw.cpp:226`).
- **Effect.** An MSAA draw that masks samples (used for per-sample tricks and some transparency effects) writes every sample instead.
- **Fix.** Metal has no pipeline or encoder sample mask, so the fragment shader must write `[[sample_mask]]`. That output exists as soon as the shader declares it, and an unwritten one is undefined, so a function-constant branch alone isn't enough. Use a shader variant for "mask active", plus a function constant for the value. That leaves the shared `fragment_context` layout (`RSXDefines2.glsl`) unchanged:
  1. Pipeline key: add `u16 sample_mask` to `pipeline_properties`. Set it to `0xFFFF` (no mask) unless `msaa_enabled()` is on and the surface has more than one sample. Hash it.
  2. Fragment program key: add a bit `sample_mask_active = (sample_mask != 0xFFFF)`, so it selects a different generated shader (put it in the C14-keyed shader cache too).
  3. Fragment decompiler (`MTLFragmentDecompilerThread::insertMainEnd`): only when that bit is set, emit
     ```glsl
     layout(constant_id = 64) const uint rsx_sample_mask = 0xFFFFu;
     ...
     gl_SampleMask[0] = int(rsx_sample_mask);
     ```
     SPIRV-Cross turns the specialisation constant into an MSL `[[function_constant(64)]]`. One shader then serves every mask value.
  4. `build_pipeline`: specialise the fragment function with `MTL4SpecializedFunctionDescriptor` and an `MTLFunctionConstantValues` holding the mask.
  5. Map the guest bits to host samples the same way VK does (pass the 16-bit value through for 1, 2 and 4 samples).
- Writing `[[sample_mask]]` acts like `discard` for Apple's hidden-surface removal. The variant bit keeps that cost off every draw without a mask.

---

## 5. Minor findings (severity L)

| ID | Finding | Fix |
|---|---|---|
| U1 | FSR passes inherit fast math (`MTLUpscaler.cpp:475-478`) | Compile the upscaler library with `MathModeSafe`; it is small and bandwidth-bound |
| O1 | Overlay, blit and fill pipelines compile synchronously on first use without the archive (`MTLOverlays.cpp:297`, `MTLBlit.cpp:217`, `MTLFill.cpp:209`) | Build them in `on_init`, passing `m_device->pipeline_task_options()` |
| C28 | Completion handlers capture `&program` (`MTLProgramPipeline.cpp:481-491`) | Safe while programs are never evicted. Use `shared_ptr<shader_program>` before adding eviction |
| P1 | `nextDrawable` can block the RSX thread up to 1 s (`MTLPresenter.cpp:132`) | Present thread (R6) |
| C12 | `ensure(transfer_width && transfer_height)` aborts when the readback trims to zero (`MTLTextureCache.cpp:426`); signed blend ops approximated; dither ignored; point fill draws filled | Return early instead of `ensure`; document the rest |
| C31 | Subrange index count not clamped to remaining bytes (`MTLGSRender.cpp:2453-2467`); KosmicKrisp WA13 shows 16-bit out-of-bounds index reads aren't robust | `count = std::min(get_index_count(prim, range.count), (index_length - byte_offset) / type_size);` |
| T10 | Framebuffer-less MSAA scratch depth is `TextureType2D` with `sampleCount > 1` (`MTLFramebuffer.cpp:188-196`) | `setTextureType(samples > 1 ? TextureType2DMultisample : TextureType2D)` |
| T11 | Border colours snap to three presets; pure red becomes black (`MTLSamplerState.cpp:130-145`) | For non-preset borders: clamp-to-edge plus a shader border test (`if (any(uv < 0 \|\| uv > 1)) return border;`), behind a key bit |
| C7 | Cross-frame surface reuse | Already ordered: single queue plus the encoder-end barrier. Keep `metal4-dev`'s F62 guard for CPU-mapped reuse only |
| F5 | `sync_get()` publishes `m_fifo_pos` (`RSXFIFO.cpp:55`); nested CALL allowed | Document; no change |
| D3 | Depth bias passes the raw RSX factor (`MTLRasterState.cpp:98-103` → `MTLEncoderState.cpp:82`), while z24s8 and z16_float are stored as `Depth32Float(_Stencil8)` (`MTLFormats.cpp:189-201`). A float target's bias unit is one ulp at the primitive's max depth, so the offset matches D24 only for depths in [0.5, 1) and shrinks closer to the camera. Vulkan on MoltenVK has the same behaviour (no D24 on Apple silicon); GL scales D24 per vendor (`GLDraw.cpp:265-280`) | Add a "Depth bias scale" setting (default 1.0), and track z-fighting titles. An exact D24 bias would need the fragment shader to write depth (`[[depth(any)]]`), which disables early depth testing and hidden-surface removal. Not worth it until a title needs it |
| D4 | Line width is captured (`MTLRasterState.cpp:33`) and never used; lines are always 1 pixel, including at resolution scale > 100% | Metal has no line width, and neither does MoltenVK on Apple GPUs, so this is parity. If needed: expand lines with `line_width > 1` to quads in the index path (two triangles per segment, offset by the screen-space normal in the vertex shader), scaled by the resolution factor |
| D5 | Restart can't be disabled for 32-bit strips. 16-bit strips with restart off are widened to u32 so `0xFFFF` is a real index (`MTLGSRender.cpp:276-279`), but a u32 strip with restart off still restarts at `0xFFFFFFFF`, because Metal always restarts strips at the maximum index | Only matters if a guest uses index `0xFFFFFFFF` as a real vertex, which needs a 4-billion-entry vertex range. When restart is off and the type is u32, have `write_index_array_data_to_buffer` convert the strip to a list (the existing `expand` path) if it finds `0xFFFFFFFF` |

---

## 6. Maintainability and repository hygiene

### H1. Line-ending churn hides real fixes in shared RSX files
*Severity H (maintenance) · Confidence: verified.*
- Upstream stores `NV47/HW/nv3089.cpp`, `nv0039.cpp` and `nv308a.cpp` with CRLF line endings, and the fork converted them to LF.
  - Plain diff of shared RSX code: +3,838/−1,399.
  - Ignoring line endings (`git diff -w --ignore-cr-at-eol`): +2,636/−197.
  - `nv3089.cpp` alone: 702/686 changed lines shrink to 21/5.
- 32 upstream RSX commits landed in the two weeks after the merge base. Every one that touches these files conflicts.
- **Fix.**

```bash
mb=9b8974d32
files="rpcs3/Emu/RSX/NV47/HW/nv3089.cpp rpcs3/Emu/RSX/NV47/HW/nv0039.cpp rpcs3/Emu/RSX/NV47/HW/nv308a.cpp"
git diff -w --ignore-cr-at-eol $mb HEAD -- $files > /tmp/nv47-real.patch   # the real changes
git checkout $mb -- $files                                                # back to CRLF originals
git apply --ignore-whitespace /tmp/nv47-real.patch                        # re-apply the real changes
git diff --stat $mb -- $files                                             # should be ~40 lines
```

  Find other affected files: `for f in $(git diff --name-only $mb HEAD); do git show $mb:$f 2>/dev/null | rg -q $'\r' && echo $f; done`. Set the editor to keep each file's existing line endings (VS Code `"files.eol": "auto"`, or an `.editorconfig` without `end_of_line`).

### H2. Inline and scaled transfers invalidate the fragment-program cache at the wrong offset (upstream bug, fixed in the fork)
*Severity M · Confidence: verified.*
- Upstream `nv308a.cpp` writes at `dst_offset + x*bpp + y*out_pitch`, but calls `invalidate_fragment_program(dst_dma, dst_offset, ...)`. `nv3089.cpp` omits `out_offset`. A fragment program uploaded by inline transfer at a non-zero x/y can stay stale in the program cache. The fork fixes both, inside the H1 churn.
- **Fix.** Send it upstream as a small PR (section 12).

### H3. About 260 code comments cite design docs that aren't in the repository
*Severity M · Confidence: verified.*
- `HLD-01`…`HLD-07`, `REVIEW-FINDINGS.md`, `CONFLICTS.md`, `DEVIATIONS-SYNC.md`, `RR7-FREEZE-FORENSICS.md`, `REALISATION.md`, `docs/research/metal4/06` appear about 260 times in `rpcs3/Emu/RSX/MTL/`. `git ls-files` contains none of them.
- **Fix.** Two steps:
  1. Commit `docs/metal4/` with the docs the code depends on (design, barrier model, lifetime model). This makes the references resolvable now.
  2. Over time, rewrite comments to state the constraint in place, and drop the reference. Upstream review will require that. Track progress with `rg -c "HLD-|\.md\b" rpcs3/Emu/RSX/MTL`.

### H4. Test coverage is narrow where bugs actually are
*Severity M · Confidence: verified.*
- Tests exist for the scheduler, surface writeback, texture types, pass policy and FSR (`rpcs3/tests/test_rsx_metal4_*.cpp`). T1, T2, T4 and T6 are all catchable without a GPU.
- **Fix.** Add CPU-only, table-driven tests:
  - **Format round-trip.** For every GCM texture and surface format: guest bytes → uploader transform → host pixel bytes → swizzle → expected guest channels.
  - **Copy-region arithmetic.** `bytesPerRow`/`bytesPerImage` for 2D, cube and 3D with mips (T4).
  - **Typeless sizes.** Guest bpp per format (T6).
  - **Scissor and viewport.** `build_scissor_rect` and `build_viewport` against VK's values (C2, C8).

### H5. Design smells (no single bug, but they keep producing bugs)
1. **Three copies of the format and swizzle tables** (`MTLFormats.cpp`, `VKFormats.cpp`, `GLTexture.cpp`). T1 came from misreading another backend's table. → Redesign R5.
2. **Surface DMA doesn't use the shared uploader.** `load_background` and `write_to_memory` reimplement row copies, which is why T2 exists while CPU texture uploads are correct. → R5.
3. **MSAA as a hardware average** instead of the shared sample mosaic (T3). The texture cache then special-cases it. → R5.
4. **Two completion channels.** The shared event gates reuse, while the feedback block gates `m_completed` and ZCULL, and nothing orders them (L3). "Idle" means "`completion_value == 0` or caught up" (L1). → R1.
5. **`device` isn't an owner** (L6), and FSR and overlay passes keep one argument table for every frame (`MTLUpscaler.cpp:644-653`, `MTLOverlays.cpp:404-425`). That is safe only while tables are captured at encode time. → R2.
6. **`MTLGSRender.cpp` is 3,356 lines.** `end_draw` is ~500 lines (`:1478-1987`) and `emit_geometry` ~500 (`:1987-2501`). → R8.
7. **`block_ring` strides by `sizeof(T)` with nothing tying it to the shader's stride** (`MTLConstants.h:213-229`). D1 is the result. → The stride parameter in the D1 fix.
8. **The pipeline key hashes uniform state.** `logic_operation`, `shade_mode` and `alpha_test_enabled` are hashed (`MTLPipeline.h:96-99`), but the shader reads the logic op from the blend block and the alpha test from the fragment context. A logic-op change builds a new pipeline whose Metal descriptor is identical. Drop the three fields from the key, but keep anything that changes generated code (the `ctrl` bits) in the shader key. → R4.
9. **Dead state.** `raster_state` stores `line_width`, `line_smooth`, `point_sprite_enabled`, `point_sprite_mask` and `user_clip_control`, and no code reads them (`MTLRasterState.h:53-73`). The `conditional_render_predicate` argument slot is declared and bound (`MTLArgumentBinds.h:26`, `MTLArgumentBinds.cpp:102`), but never written, because conditional rendering is a CPU skip (`MTLDevice.cpp:300`). Delete the fields and the slot, or wire them up. Dead state hides gaps like D4.

---

## 7. Long-term redesign

These redesigns fit inside the existing module boundaries (recorder, resource manager, pass scheduler, job system, texture cache traits). None needs a rewrite of the backend. Each lists the problem it solves, the target design, the implementation steps and how to land it safely.

### R1. Submission v2: chunked command buffers on one completion timeline
**Solves:** A1, A2, L1, L3, F4, and smell 4.

**Design.**
- **One timeline.** Every commit gets a value from `m_completion_sequence` and signals `m_completion_event`. This already exists.
- **Every commit is tracked.** Each commit returns its value. Anything that must wait for GPU work waits for *that value*, never for "the slot".
- **A frame slot = one allocator + N command buffers + per-slot rings and tables.** Command buffers are chunks. A chunk ends at a label commit, a readback, or the frame end. The allocator resets only at slot recycle, after the slot's last value has signalled.
- **The feedback block is diagnostics only.** Completion is defined by the event. ZCULL readiness uses `signaledValue() >= query.commit_value` instead of `m_completed`. Errors set `device_lost`.
- **Device loss is a state, not a recycle.** A timeout or a feedback error sets `device_lost`. Rendering stops cleanly, and nothing is freed until the event proves completion.

**Implementation steps.**
1. Add `u64 commit_value` to the readback, label and query records. `flush_for_readback`, `commit_pending_labels` and `sync_label` return or await values (A1 sketch).
2. Replace `frame_slot::command_buffer` with `buffers` + `open_buffer` (A1 sketch).
3. Delete `m_reenter_pending`, `reenter_slot` and the "fresh objects on timeout" path. Add `device_lost` (L1 simple form).
4. Move ZCULL's readiness to event values (`MTLZCULL.cpp:94-103`, `254`, `313`).
5. Add the feedback owner block (L3).
6. Then do A2 (deferred label commits).

**Landing.** Behind a temporary `g_cfg.video.metal.chunked_submit` toggle for one release cycle. Compare encoder counts, commits per frame, and RSX-thread wait time (add these counters, R7).

### R2. Lifetime v2: retire everything on the timeline
**Solves:** L1, L2, L5, L6, L7, smell 5, and future eviction (C28).

**Design.**
- `resource_manager::defer_release(obj)` records `(obj, current_open_commit_value)`. It is freed when `signaledValue() >= value`. This replaces the per-slot `on_commit_complete` lists and `quarantine`.
- Residency removals and empty-heap retirement join the same queue (L7).
- CPU writes to GPU-visible objects (`replaceRegion`, `contents()` writes) are only allowed on objects created in the current open commit. Anything older must be reallocated or copied through a staging upload. Enforce this with a debug check: `resource_manager::cpu_write_check(obj)` compares the object's last-use value against the event.
- `device` and the passes (FSR, overlays) own their objects in RAII wrappers (`mtl::ref<T>` with `release()` in the destructor), and keep argument tables per slot like the draw path.

**Implementation steps.**
1. Add `mtl::ref<T>` (retain/release RAII) in `MTLHandles.h`. Use it in `device`, `presenter`, `fsr_upscale_pass` and `ui_overlay_renderer`.
2. Add a `retire_queue` (`std::deque<std::pair<u64, MTL::Allocation*>>`) to `resource_manager`, drained in `begin_frame` and in `garbage_collect`.
3. Route `defer_release`, residency removal and heap retirement through it.
4. Give FSR and overlay passes per-slot argument tables (mirroring `frame_slot`'s tables).
5. Add the debug CPU-write check, and run once with Metal API validation.

### R3. Pass model v2: hoist copies, track edges, narrow barriers
**Solves:** S1 (structurally), F6, A2, and the cost of the encoder-end ALL→ALL barrier.

**Design.**
- **Per draw clause, two phases.**
  1. A *prepare* phase records every copy, upload, clear and invalidation into the copy encoder *before* the pass opens: vertex, index and constant staging, texture uploads, feedback copies, and the F6 pre-invalidation.
  2. An *execute* phase opens the pass (or continues the open one) and only draws.
- **Tracked edges.** The pass scheduler already knows producers and consumers (`MTLPassScheduler.cpp`). Emit `barrierAfterQueueStages(Blit|Dispatch → Vertex|Fragment)` once before the pass for the prepare phase, and `Fragment → Fragment|Vertex` only for render-to-texture dependencies. Keep the ALL→ALL barrier only at frame end and before present.
- **Inside the copy encoder,** emit an encoder barrier only between dependent commands (same destination, or a read after a write), using a small per-encoder write set instead of S1's blanket barrier.

**Implementation steps.**
1. Move the guest reads of `emit_geometry` (vertex, index, constants) into `begin_draw` and make them produce GPU buffers before `open_pass`.
2. Make `resolve_sampler_state` finish all texture-cache work (uploads, feedback copies) before `open_pass`. It already does part of this.
3. Add `copy_write_set` (a small vector of `(MTL::Resource*, range)`) to the recorder. `open_copy_encoder` emits an encoder barrier only when the next command overlaps the set, and clears the set after the barrier.
4. Replace `close_encoder`'s ALL→ALL with scheduler-provided edges, keeping a `g_cfg` debug switch "Conservative Metal barriers" for A/B testing.
5. Measure encoders per frame and pass restarts (target: zero restarts, one render encoder per guest surface bind).

### R4. Pipeline v2: flexible pipelines, hybrid interpreter, crash-safe archives
**Solves:** C17, C26, C27, C29, C30, O1, and first-run stutter.

**Design.**
- **Shader key and specialisation key.** The shader key is (vp, fp, colour formats, sample count, vertex layout), compiled with `MTLBlendFactorUnspecialized`, `MTLColorWriteMaskUnspecialized` and related unspecialised values. Blend, write mask and alpha-to-coverage become cheap `newRenderPipelineStateBySpecialization...` calls.
- **A worker pool** of size `min(cfg threads, maximumConcurrentCompilationTaskCount)`, with no global mutex, and on-demand jobs ahead of cache preload.
- **The interpreter as a true hybrid ubershader.** Base variants are prebuilt at boot, it never blocks, and it widens over time (C18).
- **Per-session archives** with periodic flush and compaction (C29), and versioned keys (C14).
- **Optional AOT.** An "Export pipeline script" debug action writes `.mtl4-json` for `metal-tt`.

**Implementation steps.**
1. Do C17, C27, C30 and C14 first (they're needed anyway).
2. Split `pipeline_properties::operator==` (`MTLPipeline.cpp:33-42`) into `shader_key` and `spec_key`. The cache becomes `map<shader_key, unspecialised_pipeline>` + `map<(shader_key, spec_key), MTL::RenderPipelineState*>`.
3. Replace the single link worker with a pool and a priority queue (on-demand jobs first).
4. Implement C29's archive rotation.
5. Precompile interpreter base variants at boot.

### R5. Surface and texture contract v2: reuse the shared helpers
**Solves:** T1, T2, T3, T6, T7 and smells 1–3, and keeps Metal aligned with future upstream texture-cache changes.

**Design.**
- **One format description table** in `Common/` (or `MTLFormats.cpp` generated from it): for each GCM format, the guest channel order, bytes per element, swap width, and the host packed layout. Each backend supplies only a host format and a permutation, which a test derives and checks (H4).
- **Surface upload** goes through `rsx::upload_texture_subresource` into a staging buffer (which already byte-swaps, deswizzles and expands), then copies into the surface, like `vk::upload_image`.
- **Surface readback** uses one `pack_unpack` path: copy to staging, then the swap kernel, then the guest. The section's `pack_unpack_swap_bytes` is set as VK does.
- **MSAA** uses sample-mosaic resolve and unresolve kernels (T3). The texture cache honours `samples_x/y`.
- **Typeless copies** use one implementation keyed on guest bpp and aspect (T6).

**Implementation steps.**
1. Write the H4 format round-trip test first. It will fail on T1 and T2 and pass after the fixes.
2. Implement T2's swap kernels and the `copy_typeless` helper (T6).
3. Replace `load_background`'s row copy with `upload_texture_subresource` into staging.
4. Implement T3's kernels, then remove the special cases in `MTLTextureCache.cpp:399-402`.
5. Fold `MTLFormats.cpp`'s tables into the shared description (an upstream-friendly change if GL and VK adopt it).

### R6. Present v2: a present thread and main-thread layer ownership
**Solves:** P1, L4, F4 (drawable waits).

**Design.** This is Apple's Metal 4 "Scheme B".
1. The RSX thread commits the frame's scene work and hands `(present_source_texture, commit_value)` to a present thread through a small queue (depth 2).
2. The present thread waits for `commit_value` using `MTL::SharedEvent::notifyListener` (or a wait with a deadline), acquires `nextDrawable` as late as possible, and encodes the final copy, upscale and overlay pass in its own command buffer on the same queue. Then it calls `signalDrawable` and commits.
3. The main thread owns `CAMetalLayer` properties and publishes size and visibility through atomics (L4).
4. The RSX thread never blocks on a drawable. If the present queue is full, it drops or replaces the oldest pending frame, depending on vsync mode.

**Implementation steps.**
- Move `presenter::acquire` and the final pass out of `present_frame` into a `named_thread("Metal Present")`.
- Give it its own argument tables and an allocator ring.
- Keep the current path behind a setting for one release.

### R7. Observability as a first-class feature
**Solves:** "nothing is measured", and makes R1–R6 verifiable.

**Implementation.**
- **Counters in an `mtl::stats` struct**, shown in the performance overlay and written to the log once per second at debug level:
  - encoders per frame, by type
  - commits per frame
  - RSX-thread GPU-wait microseconds (A1, F4)
  - pass restarts (F6)
  - ring drops (C9)
  - draws skipped while compiling
  - interpreter draws and refusals by reason (C18)
  - latched compile failures (C27)
  - upload and readback bytes
  - residency set size and `currentAllocatedSize`
- **GPU capture:** `MTL::CaptureManager` scoped to one RSX frame, from a hotkey or `RPCS3_MTL_CAPTURE=<frame>`. This needs the `MetalCaptureEnabled` Info.plist key in development builds.
- **Debug groups:** `pushDebugGroup` per draw clause and per pass.
- **Golden images:** replay RSX captures headless (RPCS3 has `rsx_capture` replay) through Metal and VK (MoltenVK or KosmicKrisp), and diff them in CI on a macOS runner. That would have caught C1, C4, C8, C19, D1, T1, T2 and T4.

### R8. Code structure: split `MTLGSRender.cpp`
**Solves:** smell 6. It's a prerequisite for reviewing R1–R3 in reasonable diffs.

**Implementation.** Move code without changing behaviour, in this order:
1. `MTLDrawPrepare.cpp`: the prepare phase of `end_draw` and the guest reads of `emit_geometry` (becomes R3's prepare).
2. `MTLDrawEmit.cpp`: encoding draws and subranges.
3. `MTLPresentFrame.cpp`: `get_present_source`, `present_frame`, `post_present` (becomes R6).
4. `MTLSync.cpp`: labels, semaphores, `sync_label`, `on_local_task`, access violations and invalidation.

Keep `MTLGSRender.cpp` for lifecycle and the backend interface. Each move is one commit with no logic changes, so `git log -L` and blame survive.

### R9. Upstream strategy
**Solves:** rebase cost, and makes the shared changes reviewable.
1. Do H1 (line endings).
2. Split the fork's shared RSX changes into a branch of upstreamable commits:
   - H2 (the invalidation offsets)
   - the `nv3089` zero-scale default
   - the `supports_transfer` backend hook in `texture_cache.h`
   - `requires_feedback_loop_copy`
   - the R6G5B5 Apple path in `TextureUtils.cpp`

   Send each as its own PR (section 12). Every merged one shrinks the fork.
3. Keep fork-only shared changes (the FIFO work, F2–F5) behind clear `// fork:` markers, and rebase weekly. Upstream RSX moves about 15 commits a week.
4. H3: commit `docs/metal4/` so the code's references resolve.

---

## 8. State-of-the-art gap analysis

### 8.1 Synchronisation model
- **Today:** a barrier chokepoint, queue, encoder and producer forms, and a KosmicKrisp-style ALL→ALL barrier at every encoder end. This is correct in spirit.
- **Gaps:** intra-encoder ordering (S1); a blanket encoder-end barrier where Apple recommends deferred, narrow barriers.
- **Target:** R3.

### 8.2 Encoders and passes
- **Today:** every upload, readback, label flush or compute closes the render encoder (`MTLCommandRecorder.cpp:540-565`), so each split costs a tile store plus a reload.
- **Target:** R3's prepare/execute split, and A2. Measure with `MTL_HUD_ENABLED=1` and Xcode's "render pass split" insight.

### 8.3 Shader compilation (the main differentiator against KosmicKrisp and MoltenVK)
- **Today:** one link worker behind a mutex, concurrency off, full pipelines specialised on every blend or format change, a blocking interpreter, and an archive written only at clean exit.
- **SOTA references:**
  - **Dolphin:** hybrid ubershaders, `shouldMaximizeConcurrentCompilation`, compile priority, and boot precompile.
  - **Apple (WWDC25-205/254):** flexible render pipelines, parallel compilation sized by `maximumConcurrentCompilationTaskCount`, and harvested pipeline scripts plus `metal-tt` archives.
- **Target:** R4.

### 8.4 Memory and textures
- Lossless compression (C20). Round the backend's own scratch and clone targets up to 8 pixels.
- Replace one-off Shared staging buffers per upload (`MTLTextureCache.cpp:216`, `MTLSurface.cpp:409,432,670,688`) with a per-slot upload ring.
- Use `StorageModeMemoryless` plus `MultisampleResolve` for MSAA images nothing samples, once T3 lands.

### 8.5 Hardware texel remapping
- `disable_hardware_texel_remapping` has no reader in Metal. T8's sRGB views implement the sRGB half. SNORM via `PixelFormatView` only on affected formats.

### 8.6 Memory budget and residency
- **Today:** no budget and no `recommendedMaxWorkingSetSize` check. Heaps are fixed blocks of 32 MiB shared and 128 MiB private (`MTLResourceManager.h:16-17`).
- **Target:** budget = cfg, or 75% of `recommendedMaxWorkingSetSize`. LRU-evict texture-cache sections when over budget, as VK's `vram_allocation_limit` does. Show `currentAllocatedSize` in the overlay. This needs R2 so eviction is safe.

### 8.7 Tooling
- R7.

### 8.8 Output scaling
- Add MetalFX spatial scaling (`MTL4FXSpatialScaler`) as an Output Scaling option next to FSR 1. Temporal scaling needs motion vectors, which PS3 games don't provide.

### 8.9 Asynchronous transfer
- `supports_asynchronous_compute = false` (`MTLDevice.cpp:286`). A second queue with a shared-event edge could carry streaming and readbacks, but only after R1 and R3, because the scheduler assumes one queue.

### 8.10 Guest-memory passthrough DMA
- `supports_passthrough_dma = false` (`MTLDevice.cpp:295`). It requires R1 (refreshed imports) and hazard rules for GPU reads of live guest memory. Defer it.

---

## 9. Performance roadmap

| # | Item | Expected gain | Effort |
|---|---|---|---|
| 9.1 | A1 / R1: no GPU wait on mid-frame commits | CPU/GPU overlap restored in label-heavy titles; likely the largest single item | M |
| 9.2 | C30 plus a worker pool | First-run stutter cut by roughly the core count | S |
| 9.3 | R3: hoist and batch copies out of passes; A2 | Removes the tile store and reload per split, the biggest bandwidth item on a TBDR GPU | M |
| 9.4 | C20 usage flags | Bandwidth on every sampled texture | S |
| 9.5 | Per-slot upload ring | CPU allocator traffic and heap fragmentation | S |
| 9.6 | R6 present thread | No drawable waits on the RSX thread | M |
| 9.7 | R3 narrow barriers | GPU overlap between passes | M |
| 9.8 | R4 flexible pipelines | Fewer compiles in blend-heavy titles | M–L |
| 9.9 | T8 sRGB views | Removes per-sample shader gamma math, and restores filtering | S–M |
| 9.10 | Vertex cache (makes `disable_vertex_cache` meaningful) | CPU copy and ring pressure | M |
| 9.11 | Shader-side conditional render (VK's `emulate_conditional_rendering`) | Removes ZCULL readback waits | M |
| 9.12 | Programmable-blend `[[color(n)]]` reads instead of a feedback copy when a shader reads only its own pixel | One copy plus one pass split per feedback draw | M |

---

## 10. Settings

### 10.1 Existing options that aren't honoured, or only partly

| Option | Metal today | Action |
|---|---|---|
| Disable Hardware ColorSpace Remapping | No reader | Implement T8 and section 8.5, or grey it out |
| Shader Compiler Threads | Decompile pool only; the link is serial | Drive the link pool (C30, R4) |
| Force High Precision Z | Always D32F+S8 | Grey it out on Metal |
| Disable Vertex Cache | No vertex cache | Implement 9.10, or grey it out |
| Renderdoc Compatibility | No equivalent | Map it to GPU capture (R7) |
| VSync Adaptive | Emulated by pacing | OK; document it |
| Disable MSL Fast Math | Honoured, but hidden by cache keys (C14), and Fast is the default (C1) | Replace with a math-mode enum |

### 10.2 New Metal options (proposed)

| Setting | Default | Reason |
|---|---|---|
| Metal shader math: Relaxed + guards / Fast / Safe | Relaxed + guards | C1 |
| Position invariance | On | C5 |
| Frames in flight: 2 / 3 | 3 | Latency against run-ahead. Needs `frame_slot_count` (`MTLDevice.h:22`) to be a runtime value |
| Pipeline compile threads (0 = auto) | Auto | C30 |
| Interpreter: Hybrid (non-blocking) / Off | Hybrid | C17 |
| Pipeline archive on/off, plus "Clear Metal cache" | On | C14, C29 |
| GPU memory budget (MB, 0 = auto) | 0 | Section 8.6 |
| Output scaling: MetalFX Spatial | — | Section 8.8 |
| GPU capture hotkey | Off | R7 |
| Metal diagnostics overlay | Off | R7 |
| Conservative barriers (debug) | Off | R3 A/B testing |

Every option that changes generated code must be part of the C14 key.

---

## 11. Validation plan (cheapest first)

0. Rebase onto upstream (C22), or at least cherry-pick #20139 (F7). Delete `<cache>/shaders_cache/metal/` before every experiment until C14 lands.
1. Run once with `MTL_DEBUG_LAYER=1`. C4 and C8 appear as scissor-outside-render-target errors, T10 as a texture-creation error, and S1/L5 races may appear as hazard warnings. Run once more with `MTL_SHADER_VALIDATION=1` in a title that uses logic op or programmable blending. D1's last-block read past the buffer end should be reported as an out-of-bounds access.
2. **Seam:** C4 at 100% scale. If the seam remains, test C2 and C8.
3. **Flashing:** S1, then Write Color Buffers on/off (exposes T2).
4. **Water:** "Disable MSL Fast Math" on (forces Safe). If fixed, land C1 layer 1 and default to Relaxed.
5. **FIFO:** with F7 in place, compare Shader Mode "Async (no interpreter)" against the default. Fewer stalls confirms C17.
6. **A1:** count commits per frame and RSX-thread GPU-wait time (add the R7 counters first) in a SPURS-heavy title.
7. **Formats:** a homebrew test or a known title with A4R4G4B4 UI (T1), a 3D-texture title (T4), and an RSX-AA title (T3).
8. **Depth:** C19, then C5.
9. **Lifetime:** force a GPU hang (an infinite-loop test shader) and confirm a clean device-lost state with no crash (L1–L3).

---

## 12. Small PRs to send upstream

The Metal backend itself isn't upstream and is too large to send as small PRs. Most traffic goes the other way (F7, C22). These items touch shared code that upstream ships today:

| # | PR title (suggested) | Files | Size | Evidence to attach |
|---|---|---|---|---|
| 1 | rsx: invalidate fragment programs at the actual inline/scaled transfer offset | `NV47/HW/nv308a.cpp`, `NV47/HW/nv3089.cpp` | ~10 lines | H2. Upstream invalidates at `dst_offset` while writing at `dst_offset + x*bpp + y*pitch` (nv308a) or `+ out_offset` (nv3089). A title that uploads fragment programs by inline transfer at an offset, or a reasoned code walk-through |
| 2 | macOS: declare game category and Game Mode support | `rpcs3/rpcs3.plist.in` | 4 lines | Shared with `CPU-NATIVE-REVIEW.md` §6. Send once. Helps the MoltenVK path too |
| 3 | rsx/interpreter: NaN test that survives fast math | `Program/GLSLInterpreter/FragmentInterpreter.glsl:249` | 1–3 lines | Upstream's `clamp16` already uses integer tests. The interpreter still uses `isnan`. Reproduce on MoltenVK first, otherwise it's a hygiene PR |
| 4 | rsx: nv3089 treats a zero scale register as 1.0 | `NV47/HW/nv3089.cpp` | ~10 lines | The fork's change: a zero `ds_dx`/`dt_dy` is the power-on value, and the subpixel correction divides by it |
| 5 | rsx: count texture-cache protection faults caused by host-page rounding | `Common/texture_cache_utils.h` (~`:464`), perf overlay | small | Counter only. Data for the 16 KB policy change (F6 fix 2; same root as #19610, #19010, #19881) |

**Propose as an issue first:** the 16 KB partial-page protection policy (F6 fix 2), and the `supports_transfer` backend hook in `texture_cache.h`. Both change shared behaviour and need maintainer buy-in.

**Not upstreamable on its own:** R1's guest-import refresh. It only matters for a backend that imports guest memory as one no-copy buffer.

---

## 13. Reference

### 13.1 KosmicKrisp cross-check (its known issues versus this backend)

Sources: Mesa `docs/drivers/kosmickrisp/workarounds.rst` (WA1–WA20), the mach9 and steamac issue trackers, RPCS3 #19986, #19973, #19791, #19815.

| KosmicKrisp item | Relevance here | Action |
|---|---|---|
| WA14 / WA17: NaN folding under relaxed math with constants | **High**: C1. Relaxed alone isn't enough for the guards | Integer guards (C1, C23) |
| Per-instruction `math_mode` pragmas (`nir_to_msl.c:2050-2083`) | High | Optional C1 layer: inject `#pragma METAL fp math_mode(safe)` into guard helpers |
| `vs_position_always_invariant` default on | Medium | C5 |
| WA13: 16-bit index out-of-bounds not robust | Low | C31 |
| WA12: 26.x index length register bug | None (requires 27) | — |
| WA11: concurrent `MTL4Compiler` instances corrupt the heap (fixed in 27); one compiler can compile concurrently | The backend serialises one compiler behind a mutex | C26, C30, R4 |
| WA20: allocation over `recommendedMaxWorkingSetSize` panics on 26 | No budget here; on 27 it pages | Section 8.6 |
| WA2: infinite-loop shapes hang the compiler (M5) | RSX loops come from guest counters | Clamp loop trip counts in decompiler output for Metal (`for (i < min(n, 256))`) |
| WA7 / WA5 / WA4 / WA19 | None today (no `sample_mask` writes, no fragment side effects, no indirect dispatch) | The D2 fix adds `sample_mask` writes. Re-read WA7 before landing it |
| WA10: `bcsel` miscompile | Possible for SPIRV-Cross `select()` output; no evidence | Watch |
| WA15 / WA6 / WA18: coherency miscompiles | No coherent storage used | — |
| WA16: M5 out-of-bounds LOD clamps | Low | Clamp LOD in shader on M5 only if a title shows it |
| mach9#194: a fragment tile running ~80 ms triggers a GPU restart | A guest infinite loop would hit it | Same loop clamp as WA2 |
| RPCS3 #19986: KosmicKrisp 54% slower than MoltenVK on Demon's Souls | Compile throughput and per-draw overhead are where a native backend wins | R4, section 9 |
| RPCS3 #19791 / #19815: clip-plane depth drift, invariance | Same Metal behaviour applies (`gl_ClipDistance` at `MTLVertexProgram.cpp:216-222`) | C5; shader-side clip with `discard` as an option |

### 13.2 Workflow trace (what a game does → where Metal does it → verdict)

| Workflow | Metal path | Verdict |
|---|---|---|
| Boot, device, archive | `MTLDevice.cpp` | Sound; C14, C29, C30, L6 |
| FIFO consume | shared `RSXFIFO.cpp` (fork changes) | Ordering OK (F1 retracted); F2, F3 fork-specific; F7 |
| Frame open and submission | `MTLCommandRecorder.cpp` | **A1**, L1, L3 |
| Surface reconcile | `MTLSurfaceCache.cpp`, `reconcile_render_targets` | Close to VK; C2 at scale ≠ 100%; T6 |
| Draw state | `MTLEncoderState.cpp`, `MTLRasterState.cpp`, `MTLDepthStencil.cpp` | C8, C19, C10, **D2**, D3, D4; two-sided stencil, user clip and alpha test correct |
| Vertex and constants | `MTLVertexProgram.cpp`, `MTLConstants.cpp` | VK semantics; vertex formats and constant dirty tracking correct; **D1**, C5, C9, C31 |
| Primitive handling | `MTLDrawState.cpp`, `emit_geometry` | Fans, quads and polygons expanded to lists; flat shading unrolled; correct except D5 |
| Fragment math | shared decompiler → SPIRV-Cross → MSL | C1, C23 |
| Compile | `MTLProgramPipeline.cpp`, `MTLShaderInterpreter.cpp` | C17, C26, C27, C30 |
| Texture upload and sampling | `MTLTextureCache.*`, `MTLSamplerState.cpp`, `MTLFormats.cpp` | **T1**, **T4**, T8, T9, S1, C20, C11 |
| Surface DMA | `MTLSurface.cpp` | **T2**, **T3**, T7, S1 |
| Blit engine | `MTLBlit.cpp` via `texture_cache.h:3858` | **C4**, **T5** |
| ZCULL | `MTLZCULL.cpp` | Consistent with VK; readiness should move to event values (R1) |
| Present | `get_present_source`, `MTLPresenter.cpp`, `MTLMetalLayer.mm` | C13, P1, L4 |
| Upscale | `MTLUpscaler.cpp` | Correct lane mapping; U1 |
| Overlays | `MTLOverlays.cpp` | **L5**, O1 |
| Invalidate and unmap | `MTLGSRender.cpp` 3190–3350 | Same shape as VK; F6 |
| Teardown | `on_shutdown`, `~device` | L2, L6 |

**Checked and correct:** `insertMainEnd` against VK; depth clip and clamp mapping; ZCULL slot gating; the FSR tile mapping; deferred release on the normal path; argument-table zero-fill of unused slots; the feedback-loop copy policy (`requires_feedback_loop_copy`, a strength on TBDR); the format table for every packed format except A4R4G4B4; the logic-op and programmable-blend shader logic (its constant-block stride is D1); the Metal 4 availability gate (offered from macOS 27, because the 26 compiler is broken).

### 13.3 Retracted and downgraded items

| ID | Original claim | Status | Evidence |
|---|---|---|---|
| F1 | Fast FIFO reads PUT with a relaxed load, so ARM64 can reorder | **Retracted** | `atomic_t` conversion → `atomic_storage::load()` with acquire fences on both sides (`util/atomic.hpp:322-334, 1399-1402`) |
| C7 | In-flight surface reuse race | **Downgraded to L** | Single queue plus ALL→ALL producer barrier at every encoder end (`MTLCommandRecorder.cpp:770-790`) |
| C13 (depth part) | Present can pick a depth surface | **Parity** | VK also uses `back()` |
| C3 | Single missing blit barrier | **Merged into S1** | — |
| C16 | Clear viewport | **Merged into C2** | — |
| `x.x` clamp16 typo | Needs an upstream fix | **No upstream PR needed** | Upstream #20078 rewrote the helper; the rebase fixes the fork |
| Draw-path sub-review #2 | MRT attachments the shader doesn't write keep a pixel format (`MTLPipeline.cpp:91-96`), so pipeline creation fails; fix by setting `PixelFormatInvalid` | **Rejected** | Metal requires each pipeline colour format to match the render pass attachment, and setting `Invalid` while a texture is bound fails validation ("render pipeline's pixelFormat (Invalid) does not match the framebuffer's"). An attachment the shader doesn't write is allowed, and a write mask of `None` (`MTLProgramPipeline.cpp:978-983`) leaves it untouched. The current code is correct |
| Draw-path sub-review #3, #5, #6 | Depth bias, line width and 32-bit restart rated M | **Downgraded to L** (D3–D5) | D3 and D4 match Vulkan on Apple silicon; D5 needs index `0xFFFFFFFF` as a real vertex |

### 13.4 History and branches

- **Upstream macOS history:**
  - MoltenVK since 2018.
  - Format and swizzle hacks removed in 2026 (#19311).
  - MoltenVK vertex explosions in Demon's Souls and GoW3 (#17913).
  - TBDR feedback-loop artefacts (TLoU #19434, UC2, Batman #19869), which this backend avoids with forced feedback copies.
  - Apple compiler folding (Ni no Kuni #19870, SotC #19940). This backend isn't protected until C1 and C23 land.
  - 16 KB page issues (#19610, #19010, #19881), related to F6.
  - The maintainers' direction is KosmicKrisp as the default. The native backend's value is performance and control, so benchmark against KosmicKrisp.
- **`cpu-native-p1`:** see `CPU-NATIVE-REVIEW.md`.
  - Its decommit and sub-page work help stability.
  - Its planned non-volatile guest accesses must keep the label and GET polling loops volatile or fenced.
- **`metal4-dev`:** source of F60 (C2), F62 (C7) and the present-selection guard (C13). It has the same C17 structure.
- **`kosmickrisp-integration`:** the practical A/B baseline.

### 13.5 Assessment

| Area | Estimate | Why |
|---|---|---|
| Architecture | ~80% | Metal 4 used as intended, deliberate hazard model, shared templates reused. Held back by submission and lifetime design (R1, R2) |
| Correctness on a broad game set | ~50–55% | P0 items are mostly local, but the surface contract (T2, T3, T6) affects every title that uses surface DMA or RSX AA |
| Performance tuning | ~40–50% | A1, pass splits and over-barriering (R3), compile throughput (R4) |
| Tooling and observability | ~35% | No counters, no capture hook, no golden images |
| Maintainability | ~55% | Line-ending churn (H1), missing referenced docs (H3), a 3,356-line render file (R8); good unit-test base to build on |
| **Overall production readiness** | **~55%** | |

**What "state of the art" would mean concretely:**
1. Zero RSX-thread waits on the GPU in steady state (R1, R6).
2. Zero RSX-thread blocking on compilation (R4).
3. Zero render-pass splits per draw in steady state (R3).
4. Narrow, tracked barriers (R3).
5. Surface DMA that matches the shared contract bit for bit (R5).
6. Lossless compression wherever possible (C20).
7. A memory budget (8.6) on safe lifetime rules (R2).
8. Counters, capture and golden-image tests (R7).

### 13.6 Coverage

- **Read line by line** (across passes): every `.cpp` and `.mm` in `rpcs3/Emu/RSX/MTL/` and the main headers. Small declaration-only headers were checked by search. MSL kernel strings were checked for structure.
- **Re-verified in the final pass:** A1, S1, C1, C2, C4, C8, C11, C13, C14, C17, C19, C20, C27, C29, C30, C31, D1–D5, L1, L5, T2, T3, T4, T5, T10, H1, H2, F7.
- **Sub-reviews (reported, spot-checked):**
  - [Metal lifetime and threading bug hunt](295558da-2202-4d15-8de6-d80fb780957c): L2, L3, L4, L6, L7.
  - [Metal texture and surface bug hunt](e2a03bc9-4062-4d76-bd17-c7573fd2bbdb): T1, T6, T7, T8, T9, T11.
  - [Metal draw, shader and query bug hunt](61951c2f-715c-4051-a134-fc35858ae607): six claims, each re-read against the code. D1 and D2 confirmed. D3–D5 confirmed but downgraded to L. The MRT pixel-format claim was rejected (section 13.3). Checked and correct in that pass: vertex formats, divisors and endianness; constant dirty tracking (coarser than VK, not stale); encoder-state replay on reopen; two-sided stencil; user clip planes; alpha test; occlusion queries and the CPU conditional-render skip.
- **Shared code read:** `RSXFIFO.cpp`, `util/atomic.hpp`, `sys_rsx.cpp`, `FragmentProgramDecompiler.cpp`, `RSXVertexPrologue.glsl`, `texture_cache.h` (blit route and the fork's changes), `TextureUtils.cpp` (`get_compatible_gcm_format`), the NV47 transfer handlers.
- **Not done:** any runtime test. Every finding is from code against a reference, not a measured result.
