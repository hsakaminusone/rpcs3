# Metal 4 backend: bugs found during the render-backend port

This file records the defects and duplications found in the pre-port Metal 4
backend while moving it onto `rsx::render_backend`. It is part of the port
deliverable: the spec in `PORT-RENDER-BACKEND.md` depends on these findings, and
several of its "delete this" instructions exist because of an entry here.

Every entry names the code as it stood before the port (recoverable from the
branch `metal4-pre-rebase`, commit `7c979f2a3`), why it is wrong, and what the
port does instead. Line numbers refer to that tree unless stated otherwise.

The central defect is architectural: the backend inherited the deleted
`GSRender` spine and therefore re-implemented, re-decoded and shadow-copied
state that the core now owns. Most entries below are instances of that one
defect; they are listed separately because each is a distinct place where a
future reader could reintroduce it.

Severity:

- **S1** — wrong output or wrong hardware behaviour.
- **S2** — duplicated work / duplicated source of truth; drift risk.
- **S3** — dead code or misleading surface.

---

## B1 — `jobs.advance()` runs twice per draw

**Severity:** S2 (pipeline publication advanced twice).

**Where:** `MTL/MTLGSRender.cpp:1595-1603` (`begin`) and
`MTL/MTLGSRender.cpp:2930-2939` (`end`).

**What:** `begin()` publishes completed pipeline links with
`m_frame_resources->jobs.advance()`, and `end()` does the same at its top. A
single guest draw therefore calls it twice.

**Why it is wrong:** `jobs.advance()` is the drain point for a link that
finished while the previous frame presented. It is a per-draw publication, not
a per-hook one. Calling it in both hooks doubles the bookkeeping and makes the
publication point depend on which hook happens to run.

**Port:** `jobs.advance()` lives in `begin_draw()` only. `end_draw()` does not
call it. See `PORT-RENDER-BACKEND.md` §8.1 and §8.6.

---

## B2 — `clear_surface()` re-implements the core clear decode and rebinds

**Severity:** S2.

**Where:** `MTL/MTLGSRender.cpp:1917-1997`.

**What:** the hook re-reads `stencil_mask()`, drops the stencil clear bit,
builds the framebuffer creation context from the clear mask, calls
`bind_framebuffer`, and re-decodes the clear colour / depth / stencil through
`resolve_surface_clear`.

**Why it is wrong:** the core `clear()` (`Core/RSXCoreRenderer.cpp:660-690`)
already skips a skipped frame, drops the stencil bit when the stencil mask is
zero, decodes `draw_state.clear`, and binds the framebuffer with the correct
`context_clear_color` / `context_clear_depth` bits before calling the backend.
The backend's rebind can only re-derive the same layout, and its decode is a
second copy of `decode_clear_state`.

**Port:** `clear()` only consumes `m_ctx->draw_state.clear`; it does not bind
and does not read registers. See `PORT-RENDER-BACKEND.md` §8.4.

---

## B3 — `resolve_surface_clear()` duplicates `decode_clear_state()`

**Severity:** S2 (two copies of a format remap, already subtly divergent).

**Where:** `MTL/MTLFormats.cpp:119-177`, declared in `MTL/MTLFormats.h:31-41`;
core copy at `Core/RSXCoreRenderer.cpp:203-299`.

**What:** both functions apply the same per-format clear-colour encoding
(`get_b8_clear_color`, `get_g8b8_clear_color`, `get_rgb565_clear_color`,
`get_a1rgb555_clear_color`, `get_abgr8_clear_color`), the same colour-mask
remap (`get_b8_clearmask`, `get_g8b8_r8g8_clearmask`, `get_abgr8_clearmask`),
the same float-format rejection, and the same "full mask" fast-clear test.

The two copies are not textually identical: the MTL copy keeps the
`MTL::ClearColor` and the fast-clear flag, while the core copy also records
`scissor_covers_surface`, host-encoded depth and the stencil write mask. Any
future format fix has to be made twice, and the copies already differ in the
float-format early-return shape.

**Why it is wrong:** the core is the single decoder. A backend copy is a second
source of truth.

**Port:** delete `resolved_clear` and `resolve_surface_clear`. The clear path
reads `draw_state.clear.{color_r,g,b,a,color_mask,color_mask_full,depth_value,
stencil_value,stencil_write_mask,scissor_covers_surface}`. See
`PORT-RENDER-BACKEND.md` §7.7 and §8.4.

---

## B4 — `capture_surface_state()` re-applies the window offset

**Severity:** S1 in the packed-pitch case, S2 otherwise.

**Where:** `MTL/MTLSurfaceState.cpp:61-87`.

**What:** the helper computes `window_offset_x/y`, then for every colour
attachment and for zeta adds
`pitch * window_offset_y + texel_size * window_offset_x` to the surface
address. The core `get_framebuffer_layout()` already does exactly this at
`Core/RSXGraphEngine.cpp:535-560`.

It also uses `surface_pitch(index)` / `surface_z_pitch()` (the declared guest
pitch) for the multiplication, while the core uses `layout.actual_color_pitch[]`
/ `layout.actual_zeta_pitch`, which may be the packed pitch
(`Core/RSXGraphEngine.cpp:486-497`, `420-424`). When a packed surface is
shifted by a window offset, the two computations disagree and the backend
addresses the wrong row.

**Why it is wrong:** double offsetting shifts every address twice, and the
pitch basis differs from the one the layout was resolved with.

**Port:** `capture_surface_state(const rsx::context&)` reads
`ctx.fb.layout.color_addresses[]` / `.actual_color_pitch[]` /
`.zeta_address` / `.actual_zeta_pitch` and performs no offset arithmetic. See
`PORT-RENDER-BACKEND.md` §7.8 and §10.3.

---

## B5 — `capture_color_write_mask()` repeats the core's format remap

**Severity:** S2.

**Where:** `MTL/MTLDepthStencil.cpp:139-172`.

**What:** the helper reads `color_mask_r/g/b/a(index)`, applies
`get_b8_colormask` / `get_g8b8_r8g8_colormask`, then filters through
`get_write_output_mask(format)` before producing an `MTL::ColorWriteMask`.

**Why it is wrong:** `Core/RSXCoreRenderer.cpp:105-136` already does the same
remap and the same output-mask filter, and stores the result in
`draw_state.blend.color_mask[index]` (per target, as booleans). The MTL copy is
a second derivation of the same value, and its `index`/`format` parameters are
exactly the inputs the core used.

**Port:** `capture_color_write_mask(const rsx::color_write_mask&)` is a direct
boolean-to-`MTL::ColorWriteMask` translation. No format argument. See
`PORT-RENDER-BACKEND.md` §7.11 and §5.

---

## B6 — `capture_draw_state()` re-decodes index type and restart

**Severity:** S2.

**Where:** `MTL/MTLDrawState.cpp:11-20`.

**What:** it reads `index_type()`, `restart_index_enabled()` and
`restart_index()` from the register file into the MTL draw record.

**Why it is wrong:** all three are decoded into
`draw_state.input_assembly` by the core (`RSXCoreRenderer.cpp`, group
`input_assembly`). Only `vertex_data_base_offset`, `vertex_data_base_index`,
`index_array_address` and `index_array_location` are undecoded, because the
guest rewrites them between subdraws.

**Port:** decoded fields from `ctx.draw_state.input_assembly`; raw fields from
`*ctx.register_state`. See `PORT-RENDER-BACKEND.md` §7.13.

---

## B7 — `capture_blend_state()` re-decodes the blend equation and constant colour

**Severity:** S2.

**Where:** `MTL/MTLBlendState.cpp:17-44` and
`MTL/MTLGSRender.cpp:4661-4682`.

**What:** the helper reads `blend_enabled_mask`, the four factors, the two
equations and the logic op from the registers. Separately, the programmable
blend constant block re-reads the same factors/equations and calls
`rsx::get_constant_blend_colors()`.

**Why it is wrong:** `draw_state.blend` already carries `enable_mask`, the
factors, the equations, `logic_op_enabled`, `logic_operation` and the decoded
`constant_color` (`RSXCoreRenderer.cpp:100-170`). `get_constant_blend_colors()`
decodes the same constant-colour registers again.

**Port:** `capture_blend_state(ctx)` reads `ctx.draw_state.blend`; the
programmable-blend block reads the same decoded fields and
`blend.constant_color`. `capture_blend_color()` is deleted. The raw-only
fields are `alpha_test_enabled`, `alpha_ref` and `shade_mode`. See
`PORT-RENDER-BACKEND.md` §7.10 and §7.16.

---

## B8 — `capture_raster_state()` reads clip min/max from the wrong owner

**Severity:** S1 (stale values when the viewport changes), S2 (duplication).

**Where:** `MTL/MTLRasterState.cpp:26-27`.

**What:** `state.clip_min = method_registers.clip_min()` and
`state.clip_max = method_registers.clip_max()`, later consumed by
`capture_clip_range()` to build the viewport depth range.

**Why it is wrong:** the core decodes the same registers into
`draw_state.viewport.z_clip_min/z_clip_max` (`RSXCoreRenderer.cpp:176-177`).
The decode is change-tracked as the `viewport` group; the backend's copy is
not, so a draw that only changes clip min/max uses the register file while the
rest of the pipeline uses the decoded record. The spec's single-source rule
(`PORT-RENDER-BACKEND.md` P3) makes `viewport_state` the only owner.

**Port:** `capture_raster_state` takes clip min/max from
`ctx.draw_state.viewport`; the raw-only fields (`point_size`,
`clip_planes_mask`, point-sprite enable/mask) stay register reads. See
`PORT-RENDER-BACKEND.md` §7.9 and §5.

---

## B9 — `capture_raster_state()` re-reads stipple state

**Severity:** S2.

**Where:** `MTL/MTLRasterState.cpp:29-30` and
`MTL/MTLGSRender.cpp:4076`.

**What:** stipple enable and the 32-word pattern are read from the register
file in the raster-state helper, and the pattern is read a second time when the
constant block is filled.

**Why it is wrong:** `draw_state.stipple` is the core's decoded, change-tracked
copy (`RSXCoreRenderer.cpp:187-196`). Two register reads become three sources
of the same pattern.

**Port:** `capture_raster_state` uses `ctx.draw_state.stipple`; the constant
block copies from the same decoded pattern. See
`PORT-RENDER-BACKEND.md` §7.9 and §8.7.

---

## B10 — `load_texture_env()` shadows the core's texture state and timing

**Severity:** S1 (lost uploads), S2 (duplicated timing/state).

**Where:** `MTL/MTLGSRender.cpp:2479-2860` (dirty flags at `2528-2529`,
`2797-2798`; `graphics_state` write at `2579`; timing at `2855`).

**What:** the hook keeps `m_textures_dirty` / `m_vertex_textures_dirty`
shadow masks, clears a unit's bit before it has uploaded, writes
`m_graphics_state |= fragment_program_state_dirty` when a host image changes
format class, and adds its own elapsed time to `m_frame_stats.textures_upload_time`.

**Why it is wrong:** the core owns `m_ctx->textures.{fragment,vertex}_textures_dirty`
and clears only the *referenced* bits after the sampler resolve returns true
(`Core/RSXCoreRenderer.cpp`, `resolve_programs`). Clearing a unit's bit in the
backend loses the dirty state for every unit the program did not reference in
this draw, so the next draw that does reference it can sample a stale image.
The `graphics_state` write and the timing are likewise the core's; the backend
adds `textures_upload_time` a second time.

**Port:** `resolve_sampler_state(flags)` reads the core dirty mask, never
clears it, ORs `fragment_program_state_dirty` into the returned flag set
instead of writing the core state, and adds no timing. See
`PORT-RENDER-BACKEND.md` §8.5 and §11 invariants 3-4.

---

## B11 — `on_access_violation()` runs the ZCULL fallback twice

**Severity:** S1 (the report can be consumed twice), S2.

**Where:** `MTL/MTLGSRender.cpp:6037-6041`; thread side at
`RSXThread.cpp:570-573`.

**What:** when the texture cache does not handle the fault, the backend calls
`zcull_ctrl->on_access_violation(address)` and returns its result. The thread's
fault handler then, on a backend `false`, calls
`zcull_ctrl->on_access_violation(address)` again.

**Why it is wrong:** the backend inherited the old thread's ownership of the
fallback. The thread already owns the zcull control and owns the fallback; the
backend must report "not handled" and let the thread run it. Calling it in the
backend first means a report that the backend consumes is invisible to the
thread's own bookkeeping.

**Port:** `on_access_violation` returns `false` for a range it did not handle;
the thread's fallback is the only zcull call. See
`PORT-RENDER-BACKEND.md` §8.10.

---

## B12 — `zcull_ctrl` ownership is a raw alias of the thread's `unique_ptr`

**Severity:** S1 (release of an object the thread owns).

**Where:** `MTL/MTLGSRender.cpp:1365` (`zcull_ctrl.release()` in the
destructor), `1409` (`zcull_ctrl.reset(&m_frame_resources->zcull_control)`),
`1525` (`zcull_ctrl.release()` on shutdown).

**What:** the backend treats `zcull_ctrl` as an owning pointer that it may
`reset` and `release`.

**Why it is wrong:** on `master` the thread owns `std::unique_ptr<ZCULL_control>
zcull_ctrl` (`RSXThread.h:89`) and installs the backend's provider with
`set_provider(m_renderer->occlusion_queries())` (`RSXThread.cpp:767`). The
backend must not release, reset or delete the provider; a release here would
leave the thread's provider dangling on renderer replacement.

**Port:** the backend stores no `zcull_ctrl`; `occlusion_queries()` returns
`&m_frame_resources->zcull_control`, and the thread owns the pointer. See
`PORT-RENDER-BACKEND.md` §8.12 and §11 invariant 11.

---

## B13 — `mtl::zcull` derives from the control instead of the provider

**Severity:** S2 (inherits a contract it does not implement).

**Where:** `MTL/MTLZCULL.h:34` (`class zcull : public rsx::reports::ZCULL_control`).

**What:** the Metal ZCULL object derives from `ZCULL_control`, the thread's
report manager, but only overrides the five provider hooks
(`begin_occlusion_query`, `end_occlusion_query`, `read_report`,
`on_access_violation`, `on_slot_complete`).

**Why it is wrong:** the new contract separates the provider (backend) from the
control (thread). Deriving from the control makes the backend type carry report
management it does not implement and forces B12's aliasing.

**Port:** derive from `rsx::reports::occlusion_query_provider`. See
`PORT-RENDER-BACKEND.md` §7.4 and §8.12.

---

## B14 — `flip()` calls the removed spine and duplicates display bookkeeping

**Severity:** S2 (compile blocker; two owners).

**Where:** `MTL/MTLGSRender.cpp:5110-5134` and the body's use of
`display_buffers` / `display_buffers_count`.

**What:** `flip()` calls `GSRender::flip(info)` and `rsx::thread::flip(info)`
on the no-resources path, and reads the thread's display buffer state.

**Why it is wrong:** on `master` the display engine owns the flip decision,
`info` is already filled with the display-buffer geometry, and the per-context
display buffers live on `rsx::context`. `GSRender` and
`rsx::thread::flip` no longer exist. The backend is not a thread and has no
display state.

**Port:** `present_frame(info)` never calls either; it reads
`m_ctx->display_buffers[]` / `.count`, presents through
`frame_resources.presenter`, and calls `m_frame->flip(m_context[, skip])` for
window bookkeeping only. See `PORT-RENDER-BACKEND.md` §8.8.

---

## B15 — `begin()` binds the framebuffer and tests the skip state

**Severity:** S2.

**Where:** `MTL/MTLGSRender.cpp:1610-1620`.

**What:** after `rsx::thread::begin()`, `begin()` tests `skip_current_frame` and
`cond_render_ctrl.disable_rendering()` and calls `bind_framebuffer(context_draw)`.

**Why it is wrong:** the core `begin()` applies conditional rendering and the
frame skip, decodes the viewport and calls `bind_framebuffer` before
`begin_draw()` (`Port spec §3.2`). A second bind can only re-derive the same
layout, and the backend does not own `skip_current_frame`.

**Port:** `begin_draw()` only advances `jobs` and opens the frame. See
`PORT-RENDER-BACKEND.md` §8.1.

---

## B16 — `end()` runs the core's skip / program / sampler / nop / stats work

**Severity:** S2 (and S1 for the texture dirty shadow, see B10).

**Where:** `MTL/MTLGSRender.cpp:2930-3090`.

**What:** `end()` calls `should_skip_draw()`, `analyse_current_rsx_pipeline()`,
`load_texture_env()`, `execute_nop_draw()` and `rsx::thread::end()`, and
accumulates the draw statistics itself.

**Why it is wrong:** the core `end()` does all of it around the hook:
`decode_draw_state → resolve_programs → resolve_pipeline_state → end_draw →
post_draw_hook`, and `post_draw_hook` increments the draw stats. Running the
same work in the hook duplicates program resolution and double-counts stats.

**Port:** `end_draw()` translates and encodes only; every failure path returns
`false`, and the core takes the nop path and increments the statistics. See
`PORT-RENDER-BACKEND.md` §8.6 and §11 invariant 5.

---

## B17 — `m_scissor` / `update_scissor()` shadow the decoded scissor

**Severity:** S2.

**Where:** `MTL/MTLGSRender.cpp` (`update_scissor`, `m_scissor`).

**What:** the backend intersects the viewport and window itself and stores a
derived scissor rectangle.

**Why it is wrong:** `resolve_scissor` in `RSXGraphEngine.cpp` writes the
resolved guest-space rectangle into `m_ctx->draw_state.scissor` and sets the
`scissor` dirty group. The backend's copy is a second, unordered derivation, and
the clear path calls `update_scissor(false)` after the core already resolved
the clear region.

**Port:** delete `m_scissor` and `update_scissor`; scale
`m_ctx->draw_state.scissor` at the point of use. See
`PORT-RENDER-BACKEND.md` §8.3.

---

## B18 — Dead surface fields

**Severity:** S3.

**Where:** `MTL/MTLSurfaceState.h:37-38` (`clip_x`, `clip_y`) and the
`clear_x/y/width/height` fields feeding `clear_covers_attachment()`.

**What:** `clip_x` / `clip_y` are written by `capture_surface_state()` and
never read anywhere. `clear_x/y/width/height` duplicate the guest scissor and
`clear_covers_attachment()` re-derives
`clear.scissor_covers_surface` from them.

**Why it is wrong:** dead state that looks authoritative; the clear-extent
test has a decoded owner in `draw_state.clear.scissor_covers_surface`.

**Port:** delete `clip_x`, `clip_y`, the clear-extent fields and
`clear_covers_attachment()`. `clear()` uses
`draw_state.clear.scissor_covers_surface`. See `PORT-RENDER-BACKEND.md` §7.8
and §17 D1.

---

## B19 — `clear_surface()` uses the resolved mask as the write predicate

**Severity:** S3 (equivalent today, wrong owner).

**Where:** `MTL/MTLGSRender.cpp:1958-1985`.

**What:** `write_color = resolved.mask != 0` and the whole-attachment test uses
`(!write_color || resolved.fast)`.

**Why it is wrong:** `resolved` is the deleted duplicate (B3). The decoded
fields `clear.color_mask` and `clear.color_mask_full` are the same predicates
and are the owner.

**Port:** `write_color = clear.color_mask != 0`; the whole-attachment test uses
`clear.color_mask_full`. See `PORT-RENDER-BACKEND.md` §8.4.

---

## B20 — `get_cycles()` and the `named_thread` surface

**Severity:** S2 (compile blocker).

**Where:** `MTL/MTLGSRender.h` / `.cpp` (the class inherits `GSRender`, which
inherits `rsx::thread`).

**What:** the backend exposes thread lifecycle and timing (`get_cycles`,
`on_init_thread`, `on_exit`, `begin`, `end`, `flip`, `do_local_task`,
`bind_framebuffer`, ...).

**Why it is wrong:** a render backend is not a thread and has no cycles,
scheduler, FIFO or display. The core owns the lifecycle.

**Port:** the class derives from `rsx::render_backend`; the thread surface is
deleted and replaced by the hooks in `PORT-RENDER-BACKEND.md` §4.

---

## B21 — `reconcile_render_targets()` writes a shadow copy of the surface info

**Severity:** S1 (framebuffer fast path disabled; possible stale rebind).

**Where:** `MTL/MTLGSRender.cpp:1704-1910` writes `m_surface_info[]` /
`m_depth_surface_info`, which were members of `rsx::thread`.

**What:** after the rebase the canonical copies are `m_ctx->fb.surface_info[]`
and `m_ctx->fb.depth_surface_info`, because `graph_engine::get_framebuffer_layout`
compares the new layout against those context fields to decide
`layout.ignore_change` (`RSXGraphEngine.cpp:580-616`).

**Why it is wrong:** if the backend keeps filling its own copy, the core
compares against zeros on every bind, so `ignore_change` is always false and the
backend rebinds the framebuffer on every draw. The old thread member no longer
exists, so the port would not compile; the tempting "mechanical" fix of adding a
private member would compile and reintroduce the perf regression.

**Port:** write `m_ctx->fb.surface_info[]` / `m_ctx->fb.depth_surface_info` and
read `m_ctx->fb.layout`. See `PORT-RENDER-BACKEND.md` §7.6.

---

## B22 — Comments name `rsx::thread::get_scissor`, which no longer exists

**Severity:** S3 (misleading surface).

**Where:** `MTL/MTLRasterState.h:17`, `:104`; `MTL/MTLEncoderState.h:72`.

**What:** the comments say the scissor comes from `rsx::thread::get_scissor`,
which owned the viewport intersection, the window clamp and the invalid case.

**Why it is wrong:** `get_scissor` was deleted with the old spine. In master the
core `graph_engine::resolve_scissor` fills `m_ctx->draw_state.scissor` and the
backend only scales it.

**Port:** point the comments at `graph_engine::resolve_scissor` /
`draw_state.scissor`; add `get_scissor` to the static-check ban list.

---

## B23 — The draw layout is never bound to the context

**Severity:** S1 (the first interleaved draw crashes the RSX thread).

**Where:** `MTL/MTLGSRender.cpp` `on_bind_context`; `frame_resources::vertex_layout`.

**What:** the frame resources own one `rsx::vertex_input_layout`, and
`on_bind_context` bound only the surface cache and the texture cache. The layout's
blocks kept `ctx == nullptr`.

**Why it is wrong:** before the rebase the shared
`interleaved_range_info::calculate_required_range` read the thread-global
`rsx::method_registers`; after the rebase it reads `REGS(ctx)`. `rsx::context`
stores `register_state` at offset 8, so an unbound block dereferences address
`0x8` on its first call. Mega Man 9 (`NPUB30036`) hit it on the first draw after
the shader preload (`GET=0xcb0368`, 0:01:59).

**Port:** `on_bind_context` calls
`m_frame_resources->vertex_layout.set_context(ctx)` beside the two caches, so the
initial bind and every context switch carry the current context. Regression test:
`MTLContract.VertexLayoutMustCarryTheBoundContext`.

---

## B24 — The renderer id is captured before the device exists

**Severity:** S1 (the renderer is replaced every frame and the shader preload never ends).

**Where:** `RSXThread.cpp` `on_task` / `reset_renderer`; `MTLGSRender::logical_id()`.

**What:** both paths stored `m_renderer_info = m_renderer->logical_id()` before
`on_init()`. `MTLGSRender::logical_id()` cannot name a GPU until `on_init()` has
created the device, so the stored value was `Metal | NULL`. After `on_init()`,
`configured_logical_id()` returned the GPU name.

**Why it is wrong:** `display_engine::flip` compares the stored id with
`configured_logical_id()` and requests a renderer update on a mismatch. The
mismatch was permanent, so every frame replaced the renderer; each replacement
ran `shader_cache::load()` again and overwrote the pipeline archive with a
partial set. The guest saw an endless “Preloading cached shaders” dialog.
Vulkan creates its device in the constructor, which is why the shared code had
never exposed the ordering defect.

**Port:** assign `m_renderer_info` after `on_init()` and
`create_backend_resources()` in both `on_task` and `reset_renderer`.

---

## B25 — The RSX thread faults inside IOGPU during teardown

**Severity:** S3 (after `Stopping emulator...`; no frame is affected).

**Where:** the Metal 4 presenter / command-recorder shutdown path; the fault PC
is in `IOGPU.framework` at `IOGPUDeviceGetNextTraceID + 40`.

**What:** every run ends with `Segfault reading location 0000000000000098` on
the RSX thread while the emulator stops. The address is inside the IOGPU device
object (`0x98`), so libGPU is dereferencing a device or trace state that the
backend has already released.

**Why it is wrong:** teardown must not touch a released device. The crash is
reproducible with the probes off (`build/probe/run-clean-3.log`), so it is not
instrumentation, but it is masked by the normal quit path and was not chased
while capture work was in flight. The main-thread `.ips` abort at `0x18` is
secondary Qt teardown and not the primary fault.

**Resolved** in the commit that landed `buffer_open` tracking (`open_copy_encoder` and `open_pass`
re-enter a closed slot): the fault was an `on_exit` invalidation readback encoded on an
`endCommandBuffer`-ed slot, not a released device. Two consecutive clean quits with
`RPCS3_TRACE_ON_SIGNAL=1` and no backtrace. See `LOG-FINDINGS.md` L159/L165.

---

## Status

All entries above are addressed by the port described in
`PORT-RENDER-BACKEND.md`. When one is fixed, append a one-line resolution note
with the commit id rather than deleting the entry; the historical record is the
reason the rule exists.
