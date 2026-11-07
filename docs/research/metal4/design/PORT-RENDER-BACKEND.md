# Porting the Metal 4 backend onto `rsx::render_backend`

Status: **DRAFT v0.2** — the normative implementation spec for the port. It is
kept current with the work; the running observations are in
`build/probe/METAL4-PORT-SPEC-SCRATCH.md` (gitignored).

Audience: whoever implements, reviews or resumes this port.

**Resume protocol.** After any summarisation or context hand-off, read this file
first, then `docs/research/metal4/design/LOG-FINDINGS.md` for the RCA ledger and
`build/probe/METAL4-PORT-SPEC-SCRATCH.md` for the live state of the port. This
file is the single specification; the other two are evidence and notes.

**Pre-rebase reference.** The last working Metal 4 backend before the upstream
rebase is the branch `metal4-pre-rebase` (commit `7c979f2a3`). It still contains
the deleted `GSRender.h/.cpp` and the pre-port `MTLGSRender`, and
`git diff metal4-pre-rebase..metal4 -- rpcs3/Emu/RSX/MTL/` is empty. Use it to
answer "what did this code look like before the port" and to recover exact
bodies; do not copy its thread/GSRender structure. Every reference to an old
method in this document can be resolved there.

---

## 0. One-paragraph summary

Upstream removed the `GSRender` spine and made every renderer an
`rsx::render_backend`: the core decodes registers once, owns the frame/draw
lifecycle, and hands the backend decoded state. The Metal 4 backend
(`MTLGSRender`) still inherits the deleted `GSRender`, re-decodes registers,
keeps shadow copies of core state, and calls thread methods that no longer
exist. This port makes `MTLGSRender` a first-class `rsx::render_backend` by
**consuming decoded state instead of re-deriving it**, deleting every shadow
copy, and moving the two backend-owned thread hooks (`on_fifo_desync`,
`sync_label`) onto the backend interface. No new abstraction, wrapper or
compatibility layer is introduced.

---

## 1. Goals and non-goals

### Goals

1. `MTLGSRender` compiles and runs as `rsx::render_backend` on top of
   `master`/`main`, with Metal 4 (`MTL4::CommandQueue`, `MTL4::Compiler`,
   `GPUFamilyMetal4`) unchanged.
2. RSX semantics are preserved for Ridge Racer 7 (`BLUS30001`) and the wider
   library: same draws, same state, same surface behaviour.
3. Exactly one owner for every piece of state. The backend never keeps a
   parallel copy of anything the core already decoded.
4. L149 (nested `CALL` / `stream_tracker`) stays fixed; performance does not
   regress; probes remain removable.
5. The port is reviewable as a sequence of small, single-purpose commits.

### Non-goals

- No new rendering features, no Metal 3 fallback, no shader-cache format change.
- No changes to VK/GL/null behaviour, except the two shared interface moves
  described in §9.
- No restoration of `GSRender`.
- No "compatibility shim" that re-declares removed thread state in the backend.

---

## 2. Principles

These decide every ambiguous case. If an implementation choice violates one,
the choice is wrong.

**P1 — RSX is the specification.** Vulkan, OpenGL and upstream are evidence for
*how the contract is expressed*, never for *what RSX does*. Native Metal 4 is
the target; no translation through Vulkan or GL concepts.

**P2 — The core owns decoding; the backend owns translation.** Only the core
reads command registers to produce `rsx::draw_state_t`, `framebuffer_layout`,
`rsx::program_state`, `rsx::texture_state`. A backend may read `rsx_state`
directly *only* for inputs the core does not decode. That list is exhaustive and
is written down in §5.

**P3 — One source of truth.** No member mirrors `m_ctx->...`. If the backend
needs "the layout", it reads `m_ctx->fb.layout`. If it needs "the scissor", it
reads `m_ctx->draw_state.scissor`. Shadow copies are deleted, not synchronised.

**P4 — The core owns lifecycle.** The backend does not call
`should_skip_draw`, `execute_nop_draw`, `analyse_current_rsx_pipeline`,
`get_current_*_program`, `load_texture_env`, `post_draw_hook`, or
`rsx::thread::begin/end`. It implements hooks and returns status.

**P5 — Failure is a return value.** Every hook that can fail returns false and
leaves the core to take the documented fallback. No hook "does the core's
fallback itself".

**P6 — Performance is a requirement.** No per-draw allocation, no extra
register walks, no new barriers, no extra submit. The information needed was
already computed once by the core; the backend reuses it.

**P7 — Diagnostics are removable.** Every probe/diagnostic keeps its
`TODO(excise)` tag, is gated by `MTL_PROBE` level 1–5, and must be deletable
without touching production logic. No diagnostic changes control flow.

**P8 — Minimal, human, repo-style.** Reuse existing MTL modules; change the
backend spine and the decode seams only. Comments explain *why*, cite the
contract (file:line) and stay in the surrounding style.

---

## 3. The core contract (verified against `master`)

### 3.1 Object model

```
rsx::thread (named_thread)          owns the FIFO, contexts, display engine, zcull control
  └─ rsx::context (per guest context)
       ├─ core_renderer                 begin/end/clear, decode, GRAPH binding
       ├─ draw_state_t                  decoded pipeline state
       ├─ fb (framebuffer_state)        layout + surface_info
       ├─ program_state                 current programs + metadata
       ├─ textures                      dirty/volatile masks
       ├─ stats / frame_skip / display_buffers
       └─ resources (context_resources) backend-created, core-owned
  └─ rsx::render_backend (one, switches between contexts)
       └─ mtl::MTLGSRender
```

`render_backend` is constructed with `(context* ctx, display_output* display)`.
The constructor sets `m_frame`, `m_vsync_mode`, `m_profiler`, but does not
initialise the device. `m_ctx` is rebound by `render_backend::bind_context`.
`bind_context` calls `on_unbind_context`/`on_bind_context` only when the
context actually changes — and it **returns early for the initial context**
(`RSXRenderBackend.cpp:24-36`). Every backend therefore calls
`on_bind_context(m_ctx)` itself in its constructor (`VKGSRender.cpp:438`,
`GLGSRender.cpp:39`); MTL must do the same.

The frame's draw context is not created by the constructor. `on_init` calls
the protected `create_frame_context()` and `on_shutdown` calls
`destroy_frame_context()`, exactly as VK does (`VKGSRender.cpp:1386`,
`VKGSRender.cpp:1516`). On macOS `gs_frame::make_context()` is a null-op, but
the calls keep the lifecycle honest and match the contract.

The backend is **not** a thread and has no cycles, scheduler, FIFO or display
state. `get_cycles()` and the `named_thread` overrides disappear.

Protected members available to MTL: `m_config`, `m_ctx`, `m_frame`, `m_context`,
`m_vsync_mode`, `m_profiler`, `m_host_dma_ctrl`, `m_fs_sampler_descriptors`,
`m_vs_sampler_descriptors`.

### 3.2 Frame lifecycle

```
thread::on_task
  ├─ renderer->on_init()                      device + frame_resources
  ├─ create_backend_resources()               create_context_resources per ctx
  ├─ zcull_ctrl->set_provider(renderer->occlusion_queries())
  └─ loop
       ├─ do_context_management()             bind_context / reset_renderer
       ├─ do_local_task(state)                renderer->on_local_task
       └─ run_FIFO
            ├─ begin()   → core_renderer::begin
            │    ├─ cond render, in_begin_end, frame skip
            │    ├─ decode pending viewport
            │    ├─ graph.bind_framebuffer(context_draw)
            │    └─ renderer->begin_draw()
            ├─ vertex/texture/constant methods
            ├─ end()     → core_renderer::end
            │    ├─ should_skip_draw() → nop + post_draw_hook
            │    ├─ decode_draw_state()
            │    ├─ resolve_programs()          → resolve_sampler_state + get_current_*_program
            │    ├─ resolve_pipeline_state()
            │    ├─ renderer->end_draw()
            │    └─ post_draw_hook()            unconditionally
            ├─ clear_surface() → core_renderer::clear
            │    ├─ decode_draw_state + decode_clear_state
            │    ├─ graph.bind_framebuffer(clear ctx)
            │    └─ renderer->clear()
            └─ flip → display_engine::flip
                 ├─ renderer->present_frame(info)   presented?
                 ├─ renderer->post_present()        only if presented
                 └─ renderer_info/configured_logical_id check
```

Key consequences for Metal:

- `begin_draw()` may be called before any `end_draw()` and must be idempotent
  about opening the frame. `bind_framebuffer` already ran. The core also ran
  `update_program_state()` before it (`RSXCoreRenderer.cpp:392`), which sets
  the `dirty` group bits for programs it will resolve again. VK/GL drop their
  cached `m_program` pointer in `begin_draw()` because they hold one
  (`VKDraw.cpp:1263-1273`, `GLDraw.cpp:885-895`). MTL has no such pointer: it
  looks the pipeline up by program identity every draw, so it does not need a
  drop step. Do not add one.
- `end_draw()` is reached **after** the core decoded `draw_state`, resolved
  samplers and programs, and moved the pipeline bits into
  `draw_state.dirty` (`resolve_pipeline_state`). It only translates.
- `end_draw()` return semantics are load-bearing: true clears
  `m_ctx->draw_state.dirty`; false makes the core call `execute_nop_draw()`
  and keep the dirty groups for the next draw
  (`RSXCoreRenderer.cpp:420-437`).
- `post_draw_hook()` (draw stats, `in_begin_end = false`, draw-clause cleanup,
  interrupts, ROP sync, capture) is the core's and runs after every `end()`
  except the device-lost early return. The backend must not duplicate it.
- `clear()` is reached with `m_ctx->draw_state.clear` fully decoded and after
  the core checked `rtt_config_valid`. Its decoded groups stay dirty until the
  next executed draw.
- `present_frame()` is the only presentation path. `post_present()` only runs
  when it returned true.

### 3.3 Framebuffer binding (`graph_engine::bind_framebuffer`, `RSXGraphEngine.cpp:800-840`)

1. Fast path: same `fb.current_context`, `!rtt_config_dirty`, and
   `backend->is_framebuffer_bound()` → `resolve_scissor`, return.
2. Clear `rtt_config_*`, decode `fb.layout` via `get_framebuffer_layout`.
3. If `!rtt_config_valid` → return (backend never called).
4. If `layout.ignore_change && is_framebuffer_bound()` → `on_framebuffer_bound`
   (which may fast-return true), then `resolve_scissor`, return.
5. Otherwise `on_framebuffer_bound(layout)`; false clears `rtt_config_valid`.
6. On success: `resolve_scissor`, `on_framebuffer_layout_updated()`,
   `check_zcull_status(true)`.

`resolve_scissor` writes `m_ctx->draw_state.scissor` in guest coordinates (it
applies the RSX viewport-rect intersection and window clamp), sets
`draw_state.dirty(scissor)`, and may clear `rtt_config_valid` for an
empty/out-of-window scissor. The backend applies
`rsx::apply_resolution_scale<...>` itself. For clears `clip_viewport=false`.

### 3.4 Exact decode contract (`RSXCoreRenderer.cpp:15-205`, `340-762`)

| decoded struct | fields written | notes |
|---|---|---|
| `raster_state` | `cull_face_enabled`, `cull_face_mode`, `front_face_mode`, `polygon_mode_front/back`, `line_width`, `line_smooth_enabled`, `poly_offset_point/line/fill_enabled`, `poly_offset_scale/bias` | no point size, clip range, user clip, stipple or point sprite |
| `depth_stencil_state` | `depth_test_enabled`, `depth_write_enabled`, `depth_func` (only if test), `depth_clamp_enabled`, `depth_clip_enabled`, `depth_bounds_test_enabled`, `depth_bounds_min/max` (clamped, only if test), `stencil_test_enabled`, `two_sided_stencil_test_enabled`, front/back `func`, `func_ref`, `func_mask`, `write_mask`, `op_fail/zfail/zpass` (only if stencil test) | refs only present while stencil test is enabled |
| `blend_state` | per-target `color_mask` (format remapped + output-mask filtered), `dither_enabled`, `logic_op_enabled`, `logic_operation` (only if enabled), `enable_mask`, factors/equations (only if `enable_mask`), `constant_color` (format decoded) | `enable_mask` is the raw enable mask, not format-filtered |
| `multisample_state` | `antialias`, `msaa_enabled`, `sample_mask`, `alpha_to_coverage_enabled`, `alpha_to_one_enabled` | |
| `viewport_state` | `clip_width`, `clip_height`, `z_clip_min`, `z_clip_max` | **the only decoded source of clip min/max** |
| `input_assembly_state` | `restart_index_enabled`, `restart_index`, `index_type` | |
| `stipple_state` | `enabled`, `pattern[32]` (only if enabled) | |
| `fragment_env_state` | `alpha_func` | no `alpha_test_enabled`, `alpha_ref`, `shade_mode` |
| `clear_state` | `mask`, `depth_format`, `stencil_write_mask`, `scissor_covers_surface`, `depth_value` (**format-decoded to a float by `rsx::get_depth_clear_value`; no host encoding is applied**), `stencil_value`, `color_mask` (format remapped and output-mask filtered), `color_r/g/b/a` (format remapped), `color_mask_full` | per clear command, not change tracked. `depth_value` only decoded when the depth bit is set; `stencil_value` only when the stencil bit is set and the format has stencil; the colour fields only when `color_mask != 0` |
| `draw_state.scissor` | `x1,y1,x2,y2`, guest coords, resolved by GRAPH | not a decode group, but a resolved field |

`resolve_pipeline_state` moves `transform_constants_dirty`,
`fragment_constants_dirty`, `vertex_state_dirty`, `fragment_state_dirty`,
`zeta_address_cyclic_barrier` and `fragment_texture_state_dirty` into
`draw_state.dirty` and clears those bits from `graphics_state`.
`resolve_programs` calls `analyse_current_rsx_pipeline`, then
`resolve_sampler_state(invalidated_programs)`, and only if that returned true
clears the referenced texture dirty masks, ORs `invalidated_programs` into
`graphics_state`, and updates the volatile units. It then calls
`update_program_state`/`get_current_fragment/vertex_program` **only when
`program_bits` is non-zero** (`RSXCoreRenderer.cpp:470-496`). A false return
from `resolve_sampler_state` therefore skips program resolution for that
draw; MTL always returns true (it binds null for a unit it cannot resolve).

---

## 4. Target class shape

`MTLGSRender.h` (metal-cpp-free; the cpp may include Metal):

```cpp
namespace mtl
{
    class MTLGSRender final : public rsx::render_backend
    {
    public:
        MTLGSRender(rsx::context* ctx, rsx::display_output* display) noexcept;
        ~MTLGSRender() override;

        // inspection / identity (the only overrides VK makes public besides sync)
        bool is_current_program_interpreted() const override;
        std::string logical_id() const override;
        std::string configured_logical_id() const override;

        // sync
        void write_barrier(u32 memory_address, u32 memory_range) override;
        bool release_GCM_label(u32 type, u32 address, u32 value) override;
        bool sync_label(u32 address) override;                          // moved onto render_backend
        void on_fifo_desync(const rsx::fifo_desync_info& info) override; // moved onto render_backend

    protected:
        // lifecycle
        void on_init() override;
        void on_shutdown() override;
        rsx::device_status check_device_status() const override;

        // context resources
        std::unique_ptr<rsx::context_resources> create_context_resources(rsx::context* ctx) override;
        void on_release_context_resources(rsx::context* ctx) override;
        void on_bind_context(rsx::context* ctx) override;
        void on_unbind_context(rsx::context* ctx) override;

        // framebuffer
        bool on_framebuffer_bound(const rsx::framebuffer_layout& layout) override;
        bool is_framebuffer_bound() const override;

        // draw
        void begin_draw() override;
        bool end_draw() override;
        void clear() override;
        void emit_geometry(u32 sub_index) override;
        bool resolve_sampler_state(rsx::flags32_t& invalidated_programs) override;

        // present
        bool present_frame(const rsx::display_flip_info_t& info) override;
        void post_present() override;

        // memory
        bool on_access_violation(rsx::context* ctx, u32 address, bool is_writing) override;
        void on_invalidate_memory_range(rsx::context* ctx, const utils::address_range32& range, rsx::invalidation_cause cause) override;
        void on_unmap_memory_range(const utils::address_range32& range) override;

        // blit / local work
        void notify_tile_unbound(u32 tile) override;
        bool scaled_image_from_memory(const rsx::blit_src_info& src, const rsx::blit_dst_info& dst, bool interpolate) override;
        void on_semaphore_acquire_wait() override;
        bool on_local_task(rsx::FIFO::state state) override;

        // local task / control
        rsx::reports::occlusion_query_provider* occlusion_queries() override;
        void patch_transform_constants(rsx::context* ctx, u32 index, u32 count) override;

    private:
        struct frame_resources;       // defined in the .cpp
        struct flush_queue;           // defined in the .cpp

        void open_frame();
        void get_present_source(...);
        void reconcile_render_targets();
        void record_pass_hazards(bool sample_textures);

        std::unique_ptr<device> m_device;
        std::unique_ptr<frame_resources> m_frame_resources;
        std::unique_ptr<flush_queue> m_flush_queue;
        bool m_frame_open = false;
        std::array<MTL::Texture*, 2> m_flip_scratch{};
    };
}
```

Still owned by the class: `m_device`, `m_frame_resources`, `m_flush_queue`,
`m_frame_open`, `m_flip_scratch`. Inherited from `render_backend` and must not
be redeclared: `m_config`, `m_ctx`, `m_frame`, `m_context`, `m_vsync_mode`,
`m_profiler`, `m_host_dma_ctrl`, `m_fs_sampler_descriptors`,
`m_vs_sampler_descriptors`.

The class does **not** override `get_programs()`. The base default returns
`{"", ""}`, which is what the pre-rebase Metal renderer did too; the override
would only invent information the backend does not have.

Deleted from the class: `m_graphics_state`, `m_framebuffer_layout`,
`m_current_framebuffer_context`, `m_surface_info`, `m_depth_surface_info`,
`m_scissor`, `m_textures_dirty`, `m_vertex_textures_dirty`,
`current_vertex_program`, `current_fragment_program`, `current_vp_metadata`,
`current_fp_metadata`, `m_program_cache_hint`, `fs_sampler_state`,
`vs_sampler_state`, `m_frame_stats`, `display_buffers`, `display_buffers_count`,
`current_display_buffer`, `backend_config`, `m_overlay_manager`,
`resolution_scaling_config`, `async_flip_requested`, `cond_render_ctrl`,
`skip_current_frame`, `in_begin_end`, `m_eng_interrupt_mask`, `zcull_ctrl`,
`get_cycles`, `on_init_thread`, `on_exit`, `begin`, `end`, `flip`,
`clear_surface`, `do_local_task`, `bind_framebuffer`, `update_scissor`.

---

## 5. Decoded-state field availability (exhaustive)

This is the authoritative mapping used by every retarget. "raw" means the core
does **not** decode it and the backend reads `REGS(m_ctx)` / `*m_ctx->register_state`
explicitly.

| MTL consumer | decoded source | raw source (only if no decoded source) |
|---|---|---|
| raster cull enable/mode, front face | `draw_state.raster.cull_face_enabled`, `.cull_face_mode`, `.front_face_mode` | — |
| raster polygon fronts/backs | `draw_state.raster.polygon_mode_front/back` | — |
| raster line width/smooth | `draw_state.raster.line_width`, `.line_smooth_enabled` | — |
| raster poly offset fill/scale/bias | `draw_state.raster.poly_offset_fill_enabled`, `.poly_offset_scale`, `.poly_offset_bias` | — |
| raster clip min/max | `draw_state.viewport.z_clip_min`, `.z_clip_max` | — |
| raster point size | — | `regs.point_size()` |
| raster user clip mask | — | `regs.clip_planes_mask()` |
| raster point sprite enable/mask | — | `regs.point_sprite_enabled()`, `regs.point_sprite_control_mask()` |
| raster stipple enable/pattern | `draw_state.stipple.enabled`, `.pattern[]` | — |
| depth test/write/func | `draw_state.depth_stencil.depth_test_enabled`, `.depth_write_enabled`, `.depth_func` | — (the function is only decoded while the test is enabled) |
| depth clamp/clip | `draw_state.depth_stencil.depth_clamp_enabled`, `.depth_clip_enabled` | — |
| depth bounds enable/min/max | `draw_state.depth_stencil.depth_bounds_test_enabled`, `.depth_bounds_min`, `.depth_bounds_max` | — (the bounds are only decoded while the test is enabled, and are format-clamped but **not** depth-range-emulated) |
| stencil test/two-sided | `draw_state.depth_stencil.stencil_test_enabled`, `.two_sided_stencil_test_enabled` | — |
| stencil front/back | `draw_state.depth_stencil.front/back` | — |
| stencil refs | `draw_state.depth_stencil.front.func_ref`; back ref only if two-sided | — |
| blend enable mask / factors / equations | `draw_state.blend.enable_mask`, `.sfactor_*`, `.dfactor_*`, `.equation_*` | — (the factors, equations and constant colour are only decoded while `enable_mask != 0`) |
| blend per-target write mask | `draw_state.blend.color_mask[i]` (format remapped) | — |
| blend constant colour | `draw_state.blend.constant_color` | — (only decoded while `enable_mask != 0`) |
| logic op enable/op | `draw_state.blend.logic_op_enabled`, `.logic_operation` | — (the operation is only decoded while the logic op is enabled) |
| alpha test enable / ref | — | `regs.alpha_test_enabled()`, `regs.alpha_ref()` |
| alpha func | `draw_state.fragment_env.alpha_func` | — |
| shade mode | — | `regs.shade_mode()` |
| multisample aa/msaa/sample mask | `draw_state.multisample.antialias`, `.msaa_enabled`, `.sample_mask` | — |
| a2c / a2one | `draw_state.multisample.alpha_to_coverage_enabled`, `.alpha_to_one_enabled` | — |
| viewport clip w/h | `draw_state.viewport.clip_width`, `.clip_height` | — |
| index type / restart | `draw_state.input_assembly.index_type`, `.restart_index_enabled`, `.restart_index` | — |
| vertex base offset/index | — | `regs.vertex_data_base_offset()`, `regs.vertex_data_base_index()` |
| index array address/location | — | `regs.index_array_address()`, `regs.index_array_location()` |
| draw clause (primitive, ranges, inline data) | — | `regs.current_draw_clause` |
| fragment/vertex texture descriptors | — | `regs.fragment_textures[]`, `regs.vertex_textures[]` |
| sampler registers (wrap/filter/lod/border) | — | `regs.fragment_textures[]` / `vertex_textures[]` |
| transform/fragment constant banks | core only raises the dirty group | `graphics_state` / `draw_state.dirty` group |
| shader control / two-side light | — | `regs.shader_control()`, `regs.two_side_light_en()` |
| transform branch bits | — | `regs.transform_branch_bits()` |
| surface target/format/aa/raster | `fb.layout.{target,color_format,depth_format,aa_mode,raster_type}` | — |
| framebuffer width/height | `fb.layout.width/height` | — |
| attachment addresses/pitches | `fb.layout.color_addresses[]`, `actual_color_pitch[]`, `zeta_address`, `actual_zeta_pitch` | — |
| attachment write enable | `fb.layout.color_write_enabled[]`, `zeta_write_enabled` | — |
| clear region | `draw_state.scissor` / `clear_state.scissor_covers_surface` | — |
| clear values/masks | `draw_state.clear.*` | — |
| scissor | `draw_state.scissor` | — |
| programs + metadata | `program.vertex_program`, `.fragment_program`, `.vp_metadata`, `.fp_metadata` | — |
| texture dirty masks | `textures.fragment_textures_dirty`, `.vertex_textures_dirty` | — |
| stats | `stats.*` | — |
| display buffers | `display_buffers[]`, `.count`, `.current` | — |
| resolution scaling | `RSX(m_ctx)->resolution_scaling_config` | — |
| overlay manager | `RSX(m_ctx)->overlay_manager()` | — |
| frame skip | `frame_skip.skip_current_frame` | — |

Anything else is a bug: add it to the core decoder, not to the backend.

---

## 6. State mapping (old MTL → new owner)

| Old member / call | New expression | Notes |
|---|---|---|
| `m_graphics_state` | `m_ctx->graphics_state` | core-only writes; backend reads (device status) |
| `m_framebuffer_layout` | `m_ctx->fb.layout` | decoded by GRAPH |
| `m_current_framebuffer_context` | `m_ctx->fb.current_context` | core-only |
| `m_surface_info[]` | `m_ctx->fb.surface_info[]` | backend fills in reconcile |
| `m_depth_surface_info` | `m_ctx->fb.depth_surface_info` | backend fills in reconcile |
| `m_scissor` | none (local `areau` per hook) | derived from `draw_state.scissor` |
| `m_textures_dirty` | `m_ctx->textures.fragment_textures_dirty` | core clears consumed bits |
| `m_vertex_textures_dirty` | `m_ctx->textures.vertex_textures_dirty` | core clears consumed bits |
| `current_vertex_program` | `m_ctx->program.vertex_program` | core resolves |
| `current_fragment_program` | `m_ctx->program.fragment_program` | core resolves |
| `current_vp_metadata` | `m_ctx->program.vp_metadata` | core resolves |
| `current_fp_metadata` | `m_ctx->program.fp_metadata` | core resolves |
| `m_program_cache_hint` | `m_ctx->program.cache_hint` | core-owned |
| `fs_sampler_state` | `m_fs_sampler_descriptors` | base-class member |
| `vs_sampler_state` | `m_vs_sampler_descriptors` | base-class member |
| `m_frame_stats` | `m_ctx->stats` | core accumulates |
| `display_buffers[]` | `m_ctx->display_buffers[]` | per-context |
| `display_buffers_count` | `m_ctx->display_buffers_count` | |
| `current_display_buffer` | `m_ctx->current_display_buffer` | |
| `backend_config` | `m_config` | filled in `on_init` |
| `resolution_scaling_config` | `RSX(m_ctx)->resolution_scaling_config` | thread-owned live setting |
| `m_overlay_manager` | `RSX(m_ctx)->overlay_manager()` | may be null |
| `get_load()` | `RSX(m_ctx)->get_load()` | |
| `get_frame()` | `m_frame` | base-class |
| `skip_current_frame` | `m_ctx->frame_skip.skip_current_frame` | read-only in backend |
| `cond_render_ctrl.disable_rendering()` | core-owned skip | delete from backend |
| `m_eng_interrupt_mask \|= backend_interrupt` | `RSX(m_ctx)->raise_backend_interrupt()` | |
| `is_current_thread()` | `RSX(m_ctx)->is_current_thread()` | |
| `async_flip_requested` | `RSX(m_ctx)->display_ctrl().has_flip_request(flip_request::emu_requested)` | only in `on_semaphore_acquire_wait` |
| `evaluate_cpu_usage_reduction_limits()` | `RSX(m_ctx)->display_ctrl().evaluate_cpu_usage_reduction_limits()` | |
| `GRAPH_frontend()` | `m_ctx->draw_processor` | same object |
| `rsx::method_registers` | `REGS(m_ctx)` / `*m_ctx->register_state` | only undecoded inputs |
| `rsx::thread::begin/end` | (removed) | core owns |
| `GSRender::flip` | `m_frame->flip(m_context, skip)` | window bookkeeping only |
| `rsx::thread::flip` | (removed) | display engine owns |
| `zcull_ctrl` | `m_frame_resources->zcull_control` | returned by `occlusion_queries()` |

---

## 7. Exact per-file changes

### 7.1 `Core/RSXRenderBackend.h` / `.cpp`

1. Move `dispatch_entry` and `dispatch_history_size` out of `rsx::thread` to
   `rsx` namespace scope in this header. Keep the `TODO(excise)` comments.
2. Move `fifo_desync_info` to namespace scope too (it embeds
   `dispatch_entry history[dispatch_history_size]`).
3. Add to `render_backend`:
   ```cpp
   // Backend housekeeping for the FIFO desync capture. The shared code gathers
   // the data; a backend that does not care ignores it.
   // TODO(excise): level 1; the hook goes with the capture.
   virtual void on_fifo_desync(const fifo_desync_info& /*info*/) {}

   // A semaphore acquire reads guest memory on the CPU, so a release this
   // backend still has in flight at `address` must complete first.
   virtual bool sync_label(u32 /*address*/) { return false; }
   ```
   The header already includes `RSXFIFO.h` and `<string>`.

### 7.2 `RSXThread.h` / `.cpp`

1. Delete the nested `dispatch_entry`, `dispatch_history_size`, `fifo_desync_info`
   definitions; the names now resolve to the namespace-scope versions.
2. Delete `virtual void on_fifo_desync(...)` and `virtual bool sync_label(...)`.
3. Keep the history storage and helpers (`m_dispatch_history`,
   `note_dispatch`, `note_label_release`, `last_label_address/value`,
   `dispatch_history*`), because the FIFO dispatch loop lives on the thread.
4. `recover_fifo` already fills `fifo_desync_info info;` and calls
   `on_fifo_desync(info)`; change the call to
   `m_renderer->on_fifo_desync(info)`.
5. `m_renderer_info`/`configured_logical_id` path unchanged.

**Name-scope trap.** Today `MTLGSRender.h`/`.cpp` write `fifo_desync_info` and
`dispatch_entry` unqualified. They resolve only because the class inherits from
`rsx::thread`, whose class scope is searched. After the port the base is
`rsx::render_backend` and the names live in namespace `rsx`, so the backend must
spell `rsx::fifo_desync_info` / `rsx::dispatch_entry` (line 73 of the header,
5599 and 5635 of the cpp). `RSXThread.cpp` is already inside `namespace rsx` and
needs no change.

### 7.3 `NV47/HW/nv406e.cpp`

`RSX(ctx)->sync_label(addr)` → `RSX(ctx)->renderer()->sync_label(addr)`.

### 7.4 `MTL/MTLZCULL.h` / `.cpp`

1. `class zcull : public rsx::reports::occlusion_query_provider` (not
   `ZCULL_control`).
2. Keep the five overrides, `init`, `shutdown`, `visibility_buffer`, `apply`,
   `on_slot_complete` unchanged. They already use only the provider hooks.
3. Delete any `ZCULL_control`-only members if present (none today).
4. The header no longer needs `RSXZCULL.h` for the base; it still needs
   `occlusion_query_info` and `occlusion_query_provider`, both in that header —
   keep the include.

### 7.5 `MTL/MTLGSRender.h`

Replace `#include "Emu/RSX/GSRender.h"` with
`#include "Emu/RSX/Core/RSXRenderBackend.h"` plus the forward declarations the
public API needs (`rsx::context`, `rsx::display_output`, `rsx::framebuffer_layout`,
`rsx::display_flip_info_t`, `rsx::blit_src_info`, `rsx::blit_dst_info`,
`rsx::invalidation_cause`, `rsx::fifo_desync_info`). Keep the metal-cpp-free
rule (`namespace MTL { class Texture; }`). Replace the class body with §4.

### 7.6 `MTL/MTLGSRender.cpp`

| Old | New |
|---|---|
| ctor `MTLGSRender(utils::serial*) : GSRender(ar)` | `MTLGSRender(rsx::context* ctx, rsx::display_output* display) : render_backend(ctx, display)`; call `on_bind_context(m_ctx)` (the base returns early for the initial context); keep `spirv::initialize_compiler_context()` |
| dtor `zcull_ctrl.release()` | remove (no `ZCULL_control`); keep `spirv::finalize_compiler_context()` |
| `get_cycles` | delete |
| `on_init_thread` | `on_init`; call `create_frame_context()` after the device exists; body otherwise unchanged except below |
| `m_device->fill_backend_config(backend_config)` | `m_device->fill_backend_config(m_config)` |
| `zcull_ctrl.reset(&...zcull_control)` | delete; `m_frame_resources->zcull_control.init(...)` stays and `occlusion_queries()` returns it |
| `if (GSFrameBase* frame = get_frame())` | `if (m_frame)`; `frame->handle()` → `m_frame->handle()` |
| `m_overlay_manager` | `RSX(m_ctx)->overlay_manager()`; local `GSFrameBase* frame = m_frame` |
| `rsx::shader_loading_dialog_native dlg(this)` | `dlg(RSX(m_ctx))` |
| `GSRender::on_init_thread()` | remove (replaced by `create_frame_context()`) |
| `on_exit` | `on_shutdown`; remove `zcull_ctrl.release()` and `GSRender::on_exit()`; keep the leak-on-failure returns; call `destroy_frame_context()` before every return, including the leaks |
| `begin` | replace with `begin_draw` (§8.1) |
| `update_scissor` | delete; compute the local scissor in `end_draw`/`clear` (§8.3) |
| `bind_framebuffer` | replace with `on_framebuffer_bound` + `is_framebuffer_bound` (§8.2) |
| `reconcile_render_targets` | retarget to `m_ctx->fb.*` and `m_ctx->fb.layout`: `m_surface_info[]` → `m_ctx->fb.surface_info[]`, `m_depth_surface_info` → `m_ctx->fb.depth_surface_info`, `m_framebuffer_layout` → `m_ctx->fb.layout` (see the note below; not merely cosmetic) |
| `clear_surface(mask)` | `clear()` (§8.4) |
| `load_texture_env()` | `resolve_sampler_state(invalidated_programs)` (§8.5) |
| `end()` | `end_draw()` (§8.6) |
| `emit_geometry` | unchanged except retargets (§8.7) |
| `flip(info)` | `present_frame(info)` (§8.8) |
| `do_local_task(state)` | `on_local_task(state)` (§8.9) |
| `on_access_violation(u32,bool)` | `on_access_violation(rsx::context*, u32, bool)`; guard `ctx == m_ctx`; `is_current_thread` → `RSX(m_ctx)->is_current_thread()`; **delete the `rsx::mm_flush(address)` call** (the thread fault handler does it before dispatching, `RSXThread.cpp:545`) and **delete the `zcull_ctrl->on_access_violation` fallback** (the thread calls it after every backend returned false, `RSXThread.cpp:570-573`); return false unhandled |
| `on_invalidate_memory_range(range,cause)` | add `rsx::context*` and guard `ctx == m_ctx`; keep the body as-is, including the `cause == unmap` purge (VK/GL keep it there too) |
| `on_unmap_memory_range(range)` | MTL has no DMA-side unmap; the default no-op is correct |
| `on_fifo_desync` | unchanged body; now an override of the base |
| `sync_label` | unchanged body; now an override of the base |

**Inherited-member retarget table.** The pre-port class inherited from
`rsx::thread`, so these names resolved through it. After the port they either
move to `render_backend`, to the context, or to the thread accessor. Every one
of them is a compile error or a silent shadow if missed:

| Old (thread/base member) | New |
|---|---|
| `get_frame()` | `m_frame` |
| `resolution_scaling_config` | `RSX(m_ctx)->resolution_scaling_config` |
| `m_graphics_state` | `m_ctx->graphics_state` |
| `m_framebuffer_layout` | `m_ctx->fb.layout` |
| `m_surface_info[]` | `m_ctx->fb.surface_info[]` |
| `m_depth_surface_info` | `m_ctx->fb.depth_surface_info` |
| `m_frame_stats` / `get_stats()` | `m_ctx->stats` |
| `m_program_cache_hint` | `m_ctx->program.cache_hint` |
| `current_vertex_program` | `m_ctx->program.vertex_program` |
| `current_fragment_program` | `m_ctx->program.fragment_program` |
| `current_vp_metadata` | `m_ctx->program.vp_metadata` |
| `current_fp_metadata` | `m_ctx->program.fp_metadata` |
| `skip_current_frame` | `m_ctx->frame_skip.skip_current_frame` |
| `m_overlay_manager` | `RSX(m_ctx)->overlay_manager()` |
| `m_eng_interrupt_mask \|= rsx::backend_interrupt` | `RSX(m_ctx)->raise_backend_interrupt()` |
| `is_current_thread()` | `RSX(m_ctx)->is_current_thread()` |
| `get_current_vertex_program(...)` / `get_current_fragment_program(...)` | delete; the core resolves both (`RSXCoreRenderer.cpp:488-506`) |
| `cond_render_ctrl` | delete; `supports_hw_conditional_render = false` |
| `m_textures_dirty` / `m_vertex_textures_dirty` | `m_ctx->textures.fragment_textures_dirty` / `vertex_textures_dirty` |
| `m_scissor` | build from `m_ctx->draw_state.scissor` per draw/clear |
| `m_current_framebuffer_context` | delete; the core owns `fb.current_context` |
| `zcull_ctrl` | delete; `occlusion_queries()` |
| `m_config` | inherited `render_backend` member (keep; fill via `fill_backend_config(m_config)`) |

**`reconcile_render_targets` is a correctness retarget.** `m_surface_info` /
`m_depth_surface_info` were members of `rsx::thread` before the rebase; in
master they live in `m_ctx->fb`. `get_framebuffer_layout` compares the newly
computed layout against `m_ctx->fb.surface_info[]` / `depth_surface_info` to set
`layout.ignore_change` (`RSXGraphEngine.cpp:580-616`). If the backend keeps
writing a private copy, the core compares against zeros: every layout reports
"changed", `ignore_change` never becomes true, and the backend rebinds on every
draw (a full loss of the framebuffer fast path). Write `m_ctx->fb.surface_info[]`
and `m_ctx->fb.depth_surface_info` and read `m_ctx->fb.layout`; delete the
shadowed members if any are declared (`MTLGSRender.h` does not declare them — it
inherited them from `rsx::thread`).

### 7.7 `MTL/MTLFormats.h` / `.cpp`

Delete `resolved_clear` and `resolve_surface_clear` (the core's
`decode_clear_state` is the same remap). Keep `to_color_write_mask` (used by the
clear fill) and every format helper.

### 7.8 `MTL/MTLSurfaceState.h` / `.cpp`

- Delete `clip_x`, `clip_y` (never read).
- Delete `depth_enabled` (never set; the only read is a diagnostic at
  `MTLGSRender.cpp:917`).
- Delete `clear_x`, `clear_y`, `clear_width`, `clear_height` and
  `clear_covers_attachment()`: the clear path now uses
  `m_ctx->draw_state.clear.scissor_covers_surface`.
- `capture_surface_state(const rsx::context& ctx)`:
  - `color_format`, `depth_format`, `antialias`, `raster_type`, `color_target`
    from `ctx.fb.layout`;
  - `color_target_mask = (1u << rsx::utility::get_mrt_buffers_count(ctx.fb.layout.target)) - 1`;
  - `clip_width/height` from `ctx.fb.layout`;
  - colour attachment addresses/pitches from `ctx.fb.layout.color_addresses[]`
    and **`ctx.fb.layout.actual_color_pitch[]`** (not `color_pitch[]`; the
    actual pitch is what the surface store allocated and what
    `reconcile_render_targets` asserts against);
  - zeta address/pitch from `ctx.fb.layout.zeta_address` and
    **`ctx.fb.layout.actual_zeta_pitch`**;
  - **no window-offset arithmetic** (the core already applied it);
  - delete the clear-colour/depth/stencil assignments: `clear()` builds them
    from `ctx.draw_state.clear`.
- Keep `surface_state` as the surface-identity record used by the cache; it is
  no longer a clear record. Keep only the clear *value* fields that `clear()`
  assigns (`clear_color`, `clear_colormask`, `clear_color_fast`, `clear_depth`,
  `clear_stencil`, `clear_stencil_fast`); delete every clear *region* field and
  `clear_covers_attachment()`. Update the struct comment that names the deleted
  `resolve_surface_clear`.

### 7.9 `MTL/MTLRasterState.h` / `.cpp`

- `capture_raster_state(const rsx::context& ctx)`: exact mapping into `raster_state`:
  | `raster_state` field | source |
  |---|---|
  | `cull_face` | `ctx.draw_state.raster.cull_face_mode` |
  | `cull_enabled` | `ctx.draw_state.raster.cull_face_enabled` |
  | `front_face` | `ctx.draw_state.raster.front_face_mode` |
  | `front_polygon_mode` | `ctx.draw_state.raster.polygon_mode_front` |
  | `back_polygon_mode` | `ctx.draw_state.raster.polygon_mode_back` |
  | `poly_offset_fill` | `ctx.draw_state.raster.poly_offset_fill_enabled` |
  | `poly_offset_scale` | `ctx.draw_state.raster.poly_offset_scale` |
  | `poly_offset_bias` | `ctx.draw_state.raster.poly_offset_bias` |
  | `line_width` | `ctx.draw_state.raster.line_width` |
  | `line_smooth` | `ctx.draw_state.raster.line_smooth_enabled` |
  | `clip_min` / `clip_max` | `ctx.draw_state.viewport.z_clip_min` / `z_clip_max` |
  | `stipple_enabled` | `ctx.draw_state.stipple.enabled` |
  | `stipple_pattern` | `ctx.draw_state.stipple.pattern`, copied **only** when enabled (zero otherwise) |
  | `point_size` | raw `ctx.register_state->point_size()` |
  | `user_clip_control` | raw `ctx.register_state->clip_planes_mask()` |
  | `point_sprite_enabled` | raw `ctx.register_state->point_sprite_enabled()` |
  | `point_sprite_mask` | raw `ctx.register_state->point_sprite_control_mask()` |
- `clip_min/clip_max` are consumed by `capture_clip_range` (the encoder's
  `has_clip_range`), so they must come from the decoded viewport, not the
  register file.
- The header comment that calls `point_size`, `line_width`, `line_smooth`,
  `user_clip_control`, point sprite and the stipple pattern "harness-only"
  stays true: production reads those through `fill_vertex_env` / the shader
  key at the point of use. Do not delete the latch fields; the dial harness
  asserts them.

### 7.10 `MTL/MTLBlendState.h` / `.cpp`

- `capture_blend_state(const rsx::context& ctx)`: exact mapping into `blend_state`:
  | `blend_state` field | source |
  |---|---|
  | `enable_mask` | `ctx.draw_state.blend.enable_mask` (raw, full 4 bits) |
  | `source_rgb` / `source_a` | `blend.sfactor_rgb` / `blend.sfactor_a` |
  | `destination_rgb` / `destination_a` | `blend.dfactor_rgb` / `blend.dfactor_a` |
  | `equation_rgb` / `equation_a` | `blend.equation_rgb` / `blend.equation_a` (only valid while `enable_mask != 0`) |
  | `alpha_test_enabled` | raw `ctx.register_state->alpha_test_enabled()` |
  | `alpha_func` | `ctx.draw_state.fragment_env.alpha_func` |
  | `alpha_ref` | raw `ctx.register_state->alpha_ref()` |
  | `logic_op_enabled` | `blend.logic_op_enabled` |
  | `logic_operation` | `blend.logic_operation` **only when `logic_op_enabled`**, else leave the default `logic_copy` |
  | `shade_mode` | raw `ctx.register_state->shade_mode()` |
- Delete `capture_blend_color()`: the encoder reads
  `ctx.draw_state.blend.constant_color`, which the core already decoded through
  the surface-format specific path and which is only valid when
  `enable_mask != 0`. When blending is off the value is unused; do not read it
  from the register file.
- Do not add `color_mask` to `mtl::blend_state`; the per-target write mask is a
  pipeline field read directly from `ctx.draw_state.blend.color_mask[i]`.

### 7.11 `MTL/MTLDepthStencil.h` / `.cpp`

- `capture_depth_stencil_state(const rsx::depth_stencil_state& decoded)`:
  mirror the decoded enable gates exactly:
  - `if (decoded.depth_test_enabled)`: `depth_compare = decoded.depth_func`,
    `depth_write = decoded.depth_write_enabled`. With the test off both keep
    their defaults (`always`, `false`), which is Metal's way of disabling depth.
  - `if (decoded.stencil_test_enabled)`: front =
    `{decoded.front.func, decoded.front.op_fail, decoded.front.op_zfail,
    decoded.front.op_zpass, decoded.front.func_mask, decoded.front.write_mask}`;
    back = the same from `decoded.back` when
    `decoded.two_sided_stencil_test_enabled`, else `back = front`.
  - With the stencil test off both faces keep their defaults.
- `capture_stencil_references(const rsx::depth_stencil_state& decoded)`:
  front = `decoded.front.func_ref`; back =
  `decoded.two_sided_stencil_test_enabled ? decoded.back.func_ref : front`.
- `capture_depth_bounds(const rsx::depth_stencil_state& decoded, bool emulate)`:
  gate on `decoded.depth_bounds_test_enabled`;
  - disabled: return `{}` (`{0.0f, 1.0f}`, the open range; the decoded
    min/max are stale/zero in this case and must not be read);
  - enabled: take `decoded.depth_bounds_min/max` (already saturate-clamped into
    the bound RSX depth format's range); if `emulate` (the draw's
    `RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE`, taken from the resolved fragment
    program, not the register file) apply `rsx::encode_emulated_depth` to both,
    then `std::clamp(..., 0.f, 1.f)`. This mirrors `VKDraw.cpp:250-270`.
  - No `surface_depth_fmt` read: the core already did the format clamp.
- `capture_anti_aliasing_state(const rsx::multisample_state& decoded)`.
- `capture_color_write_mask(const rsx::color_write_mask& mask)`: direct
  translation, no format argument (the core already remapped and filtered).
- `capture_depth_clip_mode(const rsx::depth_stencil_state& decoded)`:
  `decoded.depth_clip_enabled && !decoded.depth_clamp_enabled`.

### 7.12 `MTL/MTLEncoderState.h` / `.cpp`

`capture_encoder_state(const rsx::context& ctx, bool emulate_depth_range)`:
- `capture_raster_state(ctx)`, `capture_surface_state(ctx)`;
- `capture_depth_clip_mode(ctx.draw_state.depth_stencil)`;
- `capture_stencil_references(ctx.draw_state.depth_stencil)`;
- `capture_depth_bounds(ctx.draw_state.depth_stencil, emulate_depth_range)`;
- `capture_depth_stencil_state(ctx.draw_state.depth_stencil)`;
- `state.blend_color = ctx.draw_state.blend.constant_color` (replace the deleted
  `capture_blend_color()`; the value is only valid while `enable_mask != 0`, and
  is `{0,0,0,0}` otherwise, which `setBlendColor` accepts);
- `state.clip_width/clip_height` from `capture_surface_state(ctx)`.
- Every helper takes `ctx.draw_state.*` (or the already-captured latch), never
  the register file.
- Update the header comment: the scissor comes from
  `m_ctx->draw_state.scissor` (filled by `graph_engine::resolve_scissor`), not
  `rsx::thread::get_scissor`.

### 7.13 `MTL/MTLDrawState.h` / `.cpp`

`capture_draw_state(const rsx::context& ctx)`:
- raw: `base_offset`, `base_index`, `index_address`, `index_location` from
  `*ctx.register_state`;
- decoded: `index_type` from `ctx.draw_state.input_assembly.index_type` **unless**
  `ctx.register_state->current_draw_clause.is_immediate_draw`, in which case it
  is u32; `restart_enabled`, `restart_index` from `ctx.draw_state.input_assembly`.

### 7.14 `MTL/MTLSamplerState.h` / `.cpp`

No signature change needed: sampler state and texture descriptors are
undecoded inputs. Replace the register access with an explicit
`const rsx::rsx_state& regs` parameter (or `const rsx::context&`), so no helper
reaches a global. The two call sites (`capture_fragment_sampler`,
`capture_vertex_sampler`) pass `*m_ctx->register_state`.

### 7.15 `MTL/MTLShaderInterpreter.h` / `.cpp`

`build_request` gains the context (it needs one raw input that the core does not
decode):
```cpp
static bool build_request(const rsx::context& ctx,
    const RSXVertexProgram& vertex, const RSXFragmentProgram& fragment,
    const vertex_program_metadata&, const fragment_program_metadata&,
    interpreter_request&);
```
Use `ctx.register_state->alpha_test_enabled()` (raw — the core does not decode
it), `ctx.draw_state.fragment_env.alpha_func`, and
`ctx.draw_state.stipple.enabled` instead of the three register reads. The call
site in `MTLGSRender.cpp:3344` passes `*m_ctx`.

### 7.16 `MTL/MTLGSRender.cpp` remaining raw reads

The 44 `rsx::method_registers` uses fall into five groups:

1. **Textures (raw, legitimate).** Fragments at 385, 1047, 1086, 2514, 3040,
   3110, 3140, 4298 and vertex at 2786 are `fragment_textures[i]` /
   `vertex_textures[i]` descriptors. The core does not decode them. Replace
   `rsx::method_registers` with `*m_ctx->register_state`.
2. **Draw clause (raw, legitimate).** 707, 767, 866, 1157, 3015, 3855 read
   `current_draw_clause` (primitive, ranges, inline data). The core keeps the
   primitive in the clause, not in `draw_state`. Replace with
   `*m_ctx->register_state`.
3. **Index/vertex bases (mixed).** 770 `index_type` becomes
   `m_ctx->draw_state.input_assembly.index_type`, but only on the
   `!clause.is_immediate_draw` branch: an immediate draw is always u32 (the
   decoded field is `regs.index_type()`, which is irrelevant there). 778/810/811
   restart enable/index become `m_ctx->draw_state.input_assembly.restart_index_enabled`,
   `.restart_index`. 860/3931 `vertex_data_base_index` and 3889
   `vertex_data_base_offset` stay raw: the draw-clause barriers rewrite them
   between subdraws, so they are not a decodable group.
4. **Program keys (raw + decoded).** 3506 `two_side_light_en`, 3514/3548
   `shader_control` stay raw (`*m_ctx->register_state`): the core does not
   decode them. 4669-4681 are **not** a hash; they fill the programmable-blend
   `blend_values` block inside `emit_geometry`. Use the decoded state exactly as
   VK does (`VKGSRender.cpp:2439-2458`):
   `m_ctx->draw_state.blend.equation_rgb/a`, `.sfactor_rgb/a`,
   `.dfactor_rgb/a`, `.logic_operation` (zero/default when the logic op is not
   enabled), and `.constant_color` instead of `rsx::get_constant_blend_colors()`.
   `hash_blend_state` (line 94) already consumes `blend_state` and needs no
   register access.
5. **Deleted code.** 1928/1961-1967 belong to `clear_surface()`; the whole
   function is replaced by `clear()` built from `m_ctx->draw_state.clear`
   (§8.4), so those reads disappear. 908/2980 are diagnostic blocks inside
   `#if MTL_DEBUG_LOGS`; they are excised with the probes, not retargeted.
   `vertex_data_base_index()` in `emit_geometry` stays raw for the same reason
   as item 3.

`clip_min()`/`clip_max()` (4071-4072) have a decoded home
(`draw_state.viewport.z_clip_min/max`), but that block is replaced wholesale by
`fill_vertex_env` (§10.1), which owns the reads. `point_size` and
`polygon_stipple_pattern` are raw inputs to the vertex environment; the stipple
copy is gated on `draw_state.stipple.enabled` (the pattern is only decoded
then).

### 7.17 `Core/RSXRendererFactory.cpp`

`#include "Emu/RSX/MTL/MTLAvailability.h"` at the top (it defines
`RSX_ENABLE_METAL4` when the backend is buildable), then `#include
"Emu/RSX/MTL/MTLGSRender.h"` under `#ifdef RSX_ENABLE_METAL4`. Add a
`video_renderer::metal` case in `make_render_backend` returning
`std::make_unique<mtl::MTLGSRender>(ctx, display)`. Keep the null fallback.

Do not invent a second guard such as `#if defined(__APPLE__) &&
defined(RSX_ENABLE_METAL4)`: `RSX_ENABLE_METAL4` already encodes both, and
`MTLAvailability.h` is the single definition site.

### 7.18 `Emu/system_config` / identity

`rsx::resolve_render_backend_type` (same file) must map
`video_renderer::metal` to itself under `#ifdef RSX_ENABLE_METAL4`; include
`MTLAvailability.h` so the macro is defined there too. On a build without Metal
4, a configured Metal renderer resolves to `null` through the existing default,
which is the honest result (the factory could not construct a Metal backend).

`configured_logical_id()` then mirrors `logical_id()` (§8.14). No config schema
change is needed: `video_renderer::metal` already exists
(`system_config_types.h:49`).

---

## 8. Hook behaviour, exact

### 8.1 `begin_draw()`

```cpp
void MTLGSRender::begin_draw()
{
    if (!m_frame_resources)
        return;
    m_frame_resources->jobs.advance();   // publish a link finished since the last clause
    open_frame();                        // idempotent
}
```

- No `rsx::thread::begin()`.
- No `bind_framebuffer`, no `skip_current_frame`/`cond_render` test: the core
  applied both and bound the framebuffer.
- `jobs.advance()` is here and **only** here (the old code also advanced at the
  top of `end()`; that double-advance is a bug, see `PORT-BUGS.md`).

### 8.2 `on_framebuffer_bound` / `is_framebuffer_bound`

```cpp
bool MTLGSRender::on_framebuffer_bound(const rsx::framebuffer_layout& layout)
{
    open_frame();
    if (layout.ignore_change && m_frame_resources->attachments.valid())
        return true;
    m_frame_resources->attachments.bind(m_frame_resources->recorder,
        m_frame_resources->cache, m_ctx->fb.layout,
        RSX(m_ctx)->resolution_scaling_config);
    reconcile_render_targets();
    return m_frame_resources->attachments.valid();
}

bool MTLGSRender::is_framebuffer_bound() const
{
    return m_frame_resources && m_frame_resources->attachments.valid();
}
```

- The core owns `fb.current_context`, `rtt_config_dirty` and the
  `is_framebuffer_bound` check. It still calls this hook on the
  `ignore_change && is_framebuffer_bound()` path
  (`RSXGraphEngine.cpp:827-839`), so the hook must return `true` without
  rebinding there; it must not repeat any other core check.
- Use `m_ctx->fb.layout`, not the `layout` argument, for the bind (the argument
  is the same object in practice; reading the context keeps one source).
- Delete `update_scissor(...)` and `check_zcull_status(true)` from the bind;
  both are core work now.
- Keep the diagnostic histograms; they only read the layout.

### 8.3 Scissor

A single helper in the backend (or `MTLRasterState`):
```cpp
areau mtl::scaled_scissor(const rsx::scissor_state& region,
    const rsx::framebuffer_layout& layout);
```
`x1/y1` use `rsx::apply_resolution_scale<false>`, `x2/y2` use `<true>`, exactly
as VK's `set_scissor` (`VKGSRender.cpp:1342-1358`). The input is always
`m_ctx->draw_state.scissor`: the core's `resolve_scissor` already intersected the
viewport (draw) or not (clear), clamped to the window and marked the invalid
case (`RSXGraphEngine.cpp:855-916`). The backend has **no** `clip_viewport`
parameter and never re-intersects; both the draw encoder state and the clear
fill consume the resolved region.

### 8.4 `clear()`

The core has already: skipped the frame, dropped the stencil bit when
`stencil_mask == 0`, decoded `draw_state.clear`, and bound the framebuffer with
the correct clear context.

```cpp
void MTLGSRender::clear()
{
    if (!m_frame_resources) return;
    const auto& clear = m_ctx->draw_state.clear;
    const auto mask = clear.mask;
    if (!mask) return;

    surface_state state = capture_surface_state(*m_ctx);

    // The colour fields are only valid when the core decoded a colour clear;
    // for x32/float targets it deliberately zeroes color_mask and returns early,
    // leaving color_* and color_mask_full from an earlier clear.
    const bool write_color = clear.color_mask != 0;
    if (write_color)
    {
        state.clear_color = MTL::ClearColor::Make(
            clear.color_r / 255.f, clear.color_g / 255.f,
            clear.color_b / 255.f, clear.color_a / 255.f);
        state.clear_colormask = clear.color_mask;
        state.clear_color_fast = clear.color_mask_full;
    }

    const bool write_depth = (mask & RSX_GCM_CLEAR_DEPTH_BIT) != 0;
    const bool write_stencil = (mask & RSX_GCM_CLEAR_STENCIL_BIT) != 0;
    if (write_depth)
        state.clear_depth = clear.depth_value;      // format-decoded float; not host-encoded
    if (write_stencil)
        state.clear_stencil = clear.stencil_value;

    const bool partial_stencil = write_stencil && clear.stencil_write_mask != 0xff;
    state.clear_stencil_fast = !partial_stencil;
    ...
}
```

- `write_color = clear.color_mask != 0` (not `resolved.mask`; identical, but the
  decoded field is the source).
- Whole-attachment path: `clear.scissor_covers_surface &&
  (!write_color || clear.color_mask_full) && !partial_stencil` (the deleted
  `surface_state::clear_covers_attachment()` is not used).
- Scissored fill: `scaled_scissor(m_ctx->draw_state.scissor,
  m_ctx->fb.layout)` (the core already resolved it with `clip_viewport=false`);
  then `rsx::clip_region<u16>(attachments.width(), attachments.height(), x, y,
  width, height, true)`, `build_scissor_rect`, and
  `fill.fill(..., to_color_write_mask(clear.color_mask), write_depth,
  write_stencil ? fill_stencil{clear.stencil_value, clear.stencil_write_mask}
  : fill_stencil{})`. The `clear_state` has no scissor region of its own; only
  `scissor_covers_surface`.
- `m_frame_resources->cache.on_write({write_color,...}, write_depth || write_stencil)`
  unchanged.
- No `bind_framebuffer`, no `update_scissor`, no register reads, no
  `resolve_surface_clear`.

### 8.5 `resolve_sampler_state(invalidated_programs)`

Old `load_texture_env` with four contract changes:

1. Return `bool`: **always true**, exactly like VK/GL (a false return makes the
   core skip program resolution and leaves the sampler state half-built). A unit
   that cannot be resolved binds null; that is not a reason to skip the draw.
2. Do **not** write `m_ctx->graphics_state`. When a fragment unit's host image
   changes format class, OR `rsx::fragment_program_state_dirty` into
   `invalidated_programs`. The core propagates it into `graphics_state`.
3. Do **not** clear `m_ctx->textures.fragment_textures_dirty` /
   `vertex_textures_dirty`; read the bit (`m_ctx->textures.fragment_textures_dirty
   & (1u << i)`) to decide whether to re-upload. The core clears the referenced
   mask after the true return. (Old code cleared its own `m_textures_dirty[]`.)
4. Do **not** add the elapsed time to `m_ctx->stats.textures_upload_time`; the
   core already does (`RSXCoreRenderer.cpp:472`).

**Delete the shadow dirty arrays.** The backend keeps `m_textures_dirty` /
`m_vertex_textures_dirty` (`std::array<bool, …>`) and nothing else writes them
from the core, so they drift. Remove both and use the context masks:
- re-upload test at `MTLGSRender.cpp:2528` / `2797`:
  `m_ctx->textures.fragment_textures_dirty & (1u << i)` (vertex equivalent);
- sampler invalidation at `5819-5820`, `6055-6056`, `6143-6144`:
  set the core masks to every unit, as in §8.9.

Everything else unchanged. The program metadata is fresh: the core calls
`analyse_current_rsx_pipeline()` immediately before this hook, which refreshes
`m_ctx->program.fp_metadata` / `vp_metadata`. Read the referenced masks and the
texture descriptors from `m_ctx->program.*` and `*m_ctx->register_state`;
`REGS(m_ctx)->fragment_textures[]` / `vertex_textures[]` are legitimate raw
inputs (the core does not decode texture descriptors).

### 8.6 `end_draw()`

Body = the part of old `end()` that remains after removing everything the core
now does:

- **Remove**: `jobs.advance()` (in `begin_draw`), `should_skip_draw()` and its
  branches, `bind_framebuffer`, `analyse_current_rsx_pipeline()`,
  `load_texture_env()`, `execute_nop_draw()`, `rsx::thread::end()`, any
  `post_draw_hook` equivalent.
- **Keep**: read `m_ctx->program.*` and `m_ctx->draw_state.*`; build/lookup the
  pipeline/interpreter; on any "not ready / failed / interpreter ring
  exhausted / pass failed" path `return false`; otherwise open the encoder,
  upload constants, set state, run the subdraw loop (`emit_geometry`), close the
  encoder, `frame.cache.on_write(m_ctx->fb.layout.color_write_enabled,
  m_ctx->fb.layout.zeta_write_enabled)`, clear `require_resolve` on colour
  attachments, return true.
- The depth-range-emulation flag for `capture_encoder_state` comes from the
  resolved program: `m_ctx->program.fragment_program.ctrl &
  RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE`. `analyse_current_rsx_pipeline()`
  recomputes that bit from `graph().requires_depth_range_emulation()` through
  `fs_export_config_mask` (`RSXGraphEngine.cpp:919-972`, `:1062-1066`); do not
  re-derive it from the raw register file.
- Program/pipeline *construction* stays backend work. Only *resolution* moved.
- Do not increment `m_ctx->stats.draw_calls` (`post_draw_hook` does).
- Keep `m_ctx->stats.draw_exec_time` accumulation.

Return semantics:

| path | return | core result |
|---|---|---|
| resources missing / device lost | false | nop draw |
| program not ready (async link) | false | nop draw, dirty forwarded |
| interpreter ring exhausted | false | nop draw |
| pass failed to open | false | nop draw |
| draw encoded | true | dirty cleared |

Never call `execute_nop_draw()` or `rsx::thread::end()` from here.

### 8.7 `emit_geometry(sub_index)`

Unchanged except:
- `m_ctx->program.*`, `m_ctx->draw_state.*`, `m_ctx->fb.layout`;
- `REGS(m_ctx)->current_draw_clause`, vertex arrays, index state for raw inputs;
- transform/fragment constant uploads keyed by `m_ctx->draw_state.dirty` groups;
- vertex upload time stays in `stats.vertex_upload_time`;
- replace the six manual `vertex_context` writes with
  `m_ctx->draw_processor.fill_vertex_env(context, scale)` (see §10.3);
- do not increment `stats.draw_calls`.

### 8.8 `present_frame(info)` / `post_present()`

`present_frame` is old `flip()` minus the two removed base calls and minus the
thread-level display bookkeeping:

- `!m_frame_resources` or device lost: return false (do not flip the window).
- `open_frame()`; `m_profiler.start()`.
- Live resolution scaling: compare and `cache.sync_scaling_config` as today,
  read/write `RSX(m_ctx)->resolution_scaling_config`.
- `resources.output.deliver(slot, *m_frame)`.
- `info.skip_frame`: drop the source (no acquire, no output pass, no compose).
  Still `resources.present.present(recorder)` — the recorder owns an open
  command buffer and its slot must be submitted, or the next `begin_frame` on
  that slot waits for feedback that will never arrive and rebuilds it. Then
  retire overlays/textures, `m_frame_open = false`,
  `m_ctx->stats.flip_time = m_profiler.duration()`,
  `m_frame->flip(m_context, true)`, return **false**. This is the GL skip path
  (`GLPresent.cpp:174-178`) with the Metal submit obligation made explicit.
  `present()` submits unconditionally; with no drawable acquired it simply has
  nothing to signal.
- Normal path: resolve the source from `m_ctx->display_buffers[info.buffer]` and
  the AV config; acquire the drawable; open the output pass; compose; present;
  retire; `m_frame_open = false`; `m_ctx->stats.flip_time = m_profiler.duration()`;
  `m_frame->flip(m_context)`; return **true** only if a drawable was acquired and
  presented.
- Remove `GSRender::flip(info)` and `rsx::thread::flip(info)`.
- `evaluate_cpu_usage_reduction_limits()` → `RSX(m_ctx)->display_ctrl().evaluate_cpu_usage_reduction_limits()`,
  only on `info.emu_flip`.
- Overlay stats read `info.stats`.
- `post_present()` is empty: Metal has no swapchain to rotate. All per-frame
  cleanup (`retire_overlays`, `textures.on_frame_end`, `m_frame_open = false`,
  `flip_time`) happens inside `present_frame` because the skipped path also
  needs it.
- If the acquire fails (minimised window), still submit and present the held
  drawable, do the cleanup, call `m_frame->flip(m_context)`, return false.

### 8.9 `on_local_task(state)`

```cpp
bool MTLGSRender::on_local_task(rsx::FIFO::state state)
{
    if (!m_flush_queue || !m_frame_resources)
        return true;

    // drain deferred flush requests (open a frame if needed)
    ...
    return true;
}
```

- Drain `m_flush_queue`: drop completed requests, for each unprocessed request
  `open_frame()`, `textures.flush_all`, and on `invalidate_samplers` set both
  dirty masks to every unit:
  `m_ctx->textures.fragment_textures_dirty = (1u << rsx::limits::fragment_textures_count) - 1;`
  and the vertex equivalent. Do **not** touch `graphics_state`.
- Return true in every case. `m_flush_queue->guard` serialises the drain, and the
  thread calls this on the RSX thread only; there is no re-entry to report. The
  GL backend does the same.
- Delete `GSRender::do_local_task(state)`: the thread already owns pending flips,
  invalidated-memory handling, the per-context DMA interrupt loop, pipe flush,
  stop cleanup and the native-UI flip (`RSXThread.cpp:1240-1370`). Delete the
  overlay-flip block from the backend too.

### 8.10 Memory hooks

- `on_access_violation(ctx, address, is_writing)`:
  - `if (ctx != m_ctx) return false;`
  - `is_current_thread` → `RSX(m_ctx)->is_current_thread()`;
  - `m_eng_interrupt_mask |= backend_interrupt` →
    `RSX(m_ctx)->raise_backend_interrupt()`;
  - `m_textures_dirty.fill(true)` / `m_vertex_textures_dirty.fill(true)` →
    `m_ctx->textures.fragment_textures_dirty = (1u << rsx::limits::fragment_textures_count) - 1`
    and the vertex equivalent;
  - remove the zcull fallback; return false for an unhandled range (the thread
    then tries `zcull_ctrl->on_access_violation`, `RSXThread.cpp:572`).
- `on_invalidate_memory_range(ctx, range, cause)`: `if (ctx != m_ctx) return;`
  same body; keep the `cause == unmap && data.violation_handled` purge exactly
  where it is (the thread passes `cause::unmap` here, `RSXThread.cpp:2203-2206`).
- `on_unmap_memory_range(range)`: **no-op for Metal**. VK uses it for its
  DMA-side unmap; Metal has no analogue. The old unmap purge stays in
  `on_invalidate_memory_range`, not here.
- `notify_tile_unbound`, `write_barrier`: retarget dirty masks / keep body.

### 8.11 Sync

- `release_GCM_label`: unchanged.
- `sync_label(address)`: moved to `render_backend`, same body.
- `on_semaphore_acquire_wait`:
  - commit pending labels if a frame is open;
  - if `m_flush_queue->pending` is non-empty or
    `RSX(m_ctx)->display_ctrl().has_flip_request(rsx::thread::flip_request::emu_requested)`,
    call `RSX(m_ctx)->do_local_task(rsx::FIFO::state::lock_wait)`; do not call
    the backend's own `on_local_task` (`VKGSRender.cpp:1148-1155`).
- `on_sync_hint`: Metal has no async scheduler; leave the default no-op.

### 8.12 ZCULL

- `mtl::zcull` derives from `rsx::reports::occlusion_query_provider`.
- `occlusion_queries()` returns `&m_frame_resources->zcull_control` (nullptr when
  there is no `frame_resources`). The thread installs it after `on_init`.
- Remove `zcull_ctrl`, its `.release()` and `.reset()` calls.

### 8.13 Conditional rendering

MTL sets `m_config.supports_hw_conditional_render = false`, so the core
evaluates on the CPU and the hooks stay absent (default no-ops). Do not
resurrect `cond_render_ctrl`.

### 8.14 Inspection / identity

- `logical_id()` = `"Metal | " + <device name>` using
  `m_device->dev()->name()->utf8String()` (defensive `"Metal | NULL"` when the
  device is gone), the same shape as VK's `"Vulkan | <gpu>"`.
- `configured_logical_id()` returns `""` unless
  `rsx::resolve_render_backend_type(g_cfg.video.renderer) == video_renderer::metal`;
  otherwise the same `"Metal | <name>"` string. The display engine compares it
  with `m_renderer_info` to schedule a renderer rebuild
  (`RSXDisplayEngine.cpp:285-289`).
- `get_programs()` is not overridden; the base default is correct (see §4).
- `is_current_program_interpreted()`:
  `m_frame_resources && m_frame_resources->interpreter_active`.

### 8.15 `patch_transform_constants(ctx, index, count)`

Mark the group dirty: `m_ctx->draw_state.dirty.set(draw_state_t::group::transform_constants)`.
The upload condition in `emit_geometry` becomes
`!frame.transform_constants_valid ||
 m_ctx->draw_state.dirty.test(draw_state_t::group::transform_constants)`.
Do not add a second upload path and do not clear core `graphics_state`.

---

## 9. Shared-code changes (outside `MTL/`)

### 9.1 FIFO desync hook and `sync_label`

See §7.1–7.3. The only behavioural change outside Metal is that the hook is now
on `render_backend`; VK/GL/null inherit the default no-op.

### 9.2 Factory

See §7.17.

### 9.3 ZCULL provider

No core change: the thread already calls
`set_provider(m_renderer->occlusion_queries())` after `on_init` and on renderer
replacement. Only the MTL hierarchy changes.

### 9.4 Delete `mtl::resolve_surface_clear`

See §7.7. `resolved_clear` is deleted too; the clear path builds `surface_state`
fields directly from `draw_state.clear`.

### 9.5 Retarget the `capture_*` helpers

Final signatures:

```cpp
// MTLBlendState.h
blend_state capture_blend_state(const rsx::context& ctx);

// MTLDepthStencil.h
depth_stencil_state capture_depth_stencil_state(const rsx::depth_stencil_state& decoded);
stencil_references capture_stencil_references(const rsx::depth_stencil_state& decoded);
depth_bounds capture_depth_bounds(const rsx::depth_stencil_state& decoded, bool emulate_depth_range);
anti_aliasing_state capture_anti_aliasing_state(const rsx::multisample_state& decoded);
MTL::ColorWriteMask capture_color_write_mask(const rsx::color_write_mask& mask);
MTL::DepthClipMode capture_depth_clip_mode(const rsx::depth_stencil_state& decoded);

// MTLRasterState.h
raster_state capture_raster_state(const rsx::context& ctx);

// MTLSurfaceState.h
surface_state capture_surface_state(const rsx::context& ctx);

// MTLEncoderState.h
encoder_state capture_encoder_state(const rsx::context& ctx, bool emulate_depth_range);

// MTLDrawState.h
draw_state capture_draw_state(const rsx::context& ctx);
```

Rule: a helper whose inputs are fully decoded takes the decoded struct(s). A
helper that needs undecoded inputs takes `const rsx::context&` (to reach
`register_state`) **explicitly** and documents why in a comment. No helper
reaches a global.

---

## 10. Small implementation facts (verified)

### 10.1 `draw_command_processor::fill_vertex_env`

`RSXDrawCommands.cpp:620-629` writes exactly the MTL `vertex_context` layout:
scale/offset at offset 0 (64 bytes), user clip at 64, branch bits at 68, point
size at 72, clip min at 76, clip max at 80. The six manual writes in
`emit_geometry` (`MTLGSRender.cpp:4068-4073`) must become:
```cpp
m_ctx->draw_processor.fill_vertex_env(context,
    RSX(m_ctx)->resolution_scaling_config.scale_factor());
```
`fill_vertex_env` is const and `m_ctx->draw_processor` is the
`draw_command_processor`, so this is a direct member call (the equivalent
VK/GL line is `RSX(m_ctx)->draw_processor()->fill_vertex_env(...)`,
`VKGSRender.cpp:2152`). Keep the stipple copy separate (it lives at the stipple
heap, not in the vertex env).

### 10.2 `get_draw_command`

`draw_command_processor::get_draw_command(const rsx::rsx_state&)` still branches
on `REGS(m_ctx)->current_draw_clause.command`. Pass `*m_ctx->register_state`:

```cpp
m_ctx->draw_processor.get_draw_command(*m_ctx->register_state);
```

### 10.3 Surface addresses

`get_framebuffer_layout` applies the window offset to
`layout.color_addresses[]` / `layout.zeta_address`. The backend must use those
and `actual_color_pitch` / `actual_zeta_pitch`, and must not re-apply the
offset.

### 10.4 Scissor scale helper

`rsx::apply_resolution_scale<false>(config, x1, y1, width, height)` for the
origin and `<true>` for the far edge, exactly as VK. `clip_region<u16>` clamps
to the attachment.

---

## 11. Invariants and traps

1. **Do not re-decode.** If a value exists in `m_ctx->draw_state` / `fb` /
   `program` / `textures`, reading the register is a bug.
2. **Do not advance jobs twice per draw.** `jobs.advance()` lives in
   `begin_draw`; `end_draw` must not call it again.
3. **Do not clear dirty bits in `resolve_sampler_state`.** The core clears the
   referenced masks; clearing early loses bits for unreferenced units.
4. **Do not add core timing again.** `setup_time` / `textures_upload_time` are
   added by `core_renderer`; the backend adds only `vertex_upload_time`,
   `draw_exec_time` and `flip_time`.
5. **Do not call `post_draw_hook` work.** No stats, ROP sync, interrupt, capture
   or dirty cleanup in `end_draw`.
6. **Do not call `execute_nop_draw`/`thread::end`.** Return false.
7. **`clear()` must not bind.** The core binds with the clear context.
8. **`m_frame->flip` is not presentation.** Call it for window bookkeeping; the
   drawable is presented by `presenter.present`.
9. **Skip path must still submit.**
10. **Context switch invalidates the host binding.** `attachments.reset()` and
    `set_context` on both caches.
11. **One zcull instance.** `occlusion_queries()` returns the one owned by
    `frame_resources`; the backend must not delete it.
12. **Device loss leaks deliberately.**
13. **`m_ctx->fb.surface_info` is backend-written.** The core only reads it to
    detect layout changes; `reconcile_render_targets` must fill it exactly like
    VK's `prepare_render_target`.
14. **`layout.ignore_change` fast-return.** Core calls `on_framebuffer_bound`
    even when nothing changed; the backend returns true without rebinding.
15. **Probe level 0 must be free.**
16. **`clear.scissor_covers_surface` replaces the guest clear-extent test.**
    `surface_state::clear_covers_attachment()` is deleted with the clear fields.
17. **`draw_state.viewport` is the only decoded clip min/max.** Do not read
    `clip_min()/clip_max()` in `capture_raster_state`.
18. **`draw_state.blend.enable_mask` is raw.** Do not format-filter it again.
19. **`draw_state.depth_stencil.back` is only decoded while two-sided stencil is
    enabled.** Mirror the front when it is not.

---

## 12. Tests

### 12.1 Existing suite

`rpcs3_test` has a stale Metal 4 suite (4,835 lines) that calls
`rsx::method_registers` and the old helper signatures. Retarget it to the new
decoded-state inputs; do not delete coverage.

### 12.2 New tests (required)

1. **Nested `CALL` (L149).** Add to `test_rsx_fifo_streams.cpp`: an outer
   `CALL` into a buffer that `JUMP`s to code that `CALL`s a static routine and
   returns. Assert: `fifo_ret_addr` is overwritten by the inner call,
   `stream_tracker::on_call` saves the inner caller's stream on every call,
   `on_return` restores it, and the outer return is never needed because the
   buffer ends with `JUMP`.
2. **Backend-state translation.** Feed synthetic decoded structs (`raster`,
   `depth_stencil`, `blend`, `multisample`, `viewport`, `input_assembly`,
   `stipple`, `fragment_env`) and a fake `rsx::context` register state into each
   `capture_*` and assert the Metal record. The helpers take their inputs
   explicitly, so a test can call them without a live thread; the static grep
   below is the proof that none of them reaches a global.
3. **Render-backend contract.** The class-shape half is a compile-time test in
   `test_rsx_metal4_state.cpp`: `MTLGSRender` is an `rsx::render_backend`, is
   final, and `mtl::zcull` is an `occlusion_query_provider`. The lifecycle half
   (stub backend, `begin_draw` → `end_draw`, `false` → nop, `clear` after
   `decode_clear_state`, one `present_frame` per flip) has no unit fixture:
   `render_backend`'s constructor binds `m_profiler` to `RSX(ctx)->profiler()`,
   and the core hooks dereference thread-owned subsystems, so a stub would need
   a live `rsx::thread`. Those orderings stay part of the device verification
   (§13.2); the draw/clear/present wiring itself is covered indirectly by the
   decoded-state captures below.
4. **Raw/decoded isolation.** `MTLContract.*` sets a decoded field and a
   conflicting register value; it asserts the capture helpers follow the decoded
   value for raster, stipple, blend, draw, surface and encoder state, and that
   only the documented raw-only fields (`point_size`, `clip_planes_mask`,
   `point_sprite_control_mask`, `alpha_test_enabled`, `alpha_ref`, `shade_mode`,
   vertex bases, index address/location) still come from `register_state`.

### 12.3 Static checks

- `rg -n "rsx::method_registers" rpcs3/Emu/RSX/MTL` → zero.
- `rg -n "GSRender|thread::begin|thread::end|execute_nop_draw|should_skip_draw|get_scissor|resolve_surface_clear" rpcs3/Emu/RSX/MTL` → comments only.
- `MTLGSRender` is not a `named_thread` and has no `get_cycles`.
- Every pure virtual is implemented.
- `on_init` leaves `occlusion_queries()` non-null.

---

## 13. Verification plan

### 13.1 Compile gates

1. Reconfigure cmake (the stale `build/build.ninja` still references
   `GSRender.cpp`): `cmake -S . -B build` with the existing cache
   (`BUILD_RPCS3_TESTS=ON`).
2. `cmake --build build --target rpcs3` via the documented fast-relink patch
   (drop the macdeployqt `POST_BUILD` command in `build/build.ninja`; never
   `-DMACDEPLOYQT_EXECUTABLE`).
3. Build VK/GL/null too (`rpcs3` target covers them).

### 13.2 Device verification (one instance; user drives)

Launch with `build/probe/rpcs3.pid` tracking; never `pkill -f rpcs3`.

1. Boot `BLUS30001`, reach the menu, then a race.
2. L149: nested `CALL` at `0xcb07e0` handled (no desync, no freeze);
   `MTL_PROBE=2` shows the expected stream.
3. Visual parity: menu, car select, race HUD, track, replay; screenshot diff
   against the pre-rebase captures.
4. Performance: `nextDrawable`, `present/flip`, `frame/gpu wait`, semaphore
   wait counts, frame-time histogram; compare with the baseline (4.30 ms /
   7.32 ms / 9.14 ms; 65M spins / 300 frames). No regression beyond noise; no
   new tail >100 ms.
5. Discriminators when a defect appears: `MTL_PROBE_SERIALISE=1` (overlap vs
   data), `MTL_PROBE_TRACKED_HEAPS=1` (missing GPU edge).
6. Shutdown: no new abort; the known Qt teardown `.ips` is unrelated.

### 13.3 Tests

Build and run `rpcs3_test` with `BUILD_RPCS3_TESTS=ON`.

---

## 14. Implementation sequence

1. **Interface moves** (`RSXRenderBackend.h/.cpp`, `RSXThread.*`, `nv406e.cpp`):
   move `on_fifo_desync`/`sync_label`/desync structs; compile VK/GL/null.
2. **ZCULL base** (`MTLZCULL.h`): derive from `occlusion_query_provider`.
3. **Decoded-state seams** (`MTLBlendState`, `MTLDepthStencil`, `MTLRasterState`,
   `MTLSurfaceState`, `MTLEncoderState`, `MTLDrawState`, `MTLSamplerState`,
   `MTLShaderInterpreter`, `MTLFormats`): retarget `capture_*`, delete
   `resolve_surface_clear` and the clear duplicate.
4. **Backend spine** (`MTLGSRender.h/.cpp`): new base, lifecycle, framebuffer,
   draw/clear, sampler, present, local task, memory, sync, inspection.
5. **Factory + CMake**; build; fix errors.
6. **L149** in `RSXFIFO.cpp/.h` (see §15).
7. **Tests**: retarget the suite; add the three new tests; run.
8. **Device verification**; excise probes after the run; final build.
9. **Docs + commit** (author `hsaka -1.0 <340957754+hsakaminusone@users.noreply.github.com>`);
   no push without confirmation.

---

## 15. L149 — nested `CALL`

The requirement is hardware behaviour, not a Metal workaround. Fix in the shared
FIFO:

1. `RSXFIFO.cpp` `FIFO_control::on_call`: remove the single-level guard
   (`FIFO: CALL found inside a subroutine`). A `CALL` overwrites
   `fifo_ret_addr` unconditionally, exactly like the hardware's one
   return-address register.
2. `stream_tracker::on_call`: save the current stream on **every** call, not
   only when `!m_in_call`. The tracker's saved state is a stack of caller
   streams; the hardware return register only keeps the innermost.
3. `stream_tracker::on_return`: restore the most recently saved caller stream.
   If the outer buffer ends with `JUMP` rather than `RETURN`, the outer restore
   is dead code and that is the correct hardware outcome.
4. Keep the desync diagnostics; they show the nested case.

Add the nested-CALL test (§12.2 item 1).

---

## 16. Traceability matrix

| Contract hook | Core caller | MTL implementation | Verified by |
|---|---|---|---|
| `on_init` | `thread::on_task` | device + `frame_resources` (incl. its `zcull_control`); no provider install (the thread does it) | boot |
| `on_shutdown` | `thread::on_exit` | ordered teardown / leak on loss | shutdown |
| `check_device_status` | `do_context_management` | `m_device->lost_flag() ? lost : ok` | simulated loss |
| `create_context_resources` | `create_backend_resources` | empty base `context_resources` (caches are device-scoped) | context create |
| `on_bind_context` | `bind_context` | `set_context` + `attachments.reset` | multi-context |
| `on_framebuffer_bound` | `bind_framebuffer` | bind + reconcile | draw/clear |
| `is_framebuffer_bound` | `bind_framebuffer` | `attachments.valid()` | draw |
| `begin_draw` | `core_renderer::begin` | `jobs.advance` + `open_frame` | draw |
| `end_draw` | `core_renderer::end` | pipeline + pass + subdraws | draw |
| `clear` | `core_renderer::clear` | decoded clear | clear |
| `resolve_sampler_state` | `resolve_programs` | texture resolve | textured draw |
| `present_frame` | `display_engine::flip` | acquire/compose/present | flip |
| `post_present` | `display_engine::flip` | empty | flip |
| `on_local_task` | `thread::do_local_task` | flush queue | fault |
| `on_access_violation` | thread fault handler | cache invalidate | fault |
| `on_invalidate_memory_range` | thread | cache invalidate | unmap |
| `on_unmap_memory_range` | thread | no-op (purge stays in `on_invalidate_memory_range`) | unmap |
| `on_semaphore_acquire_wait` | `nv406e` | commit + local task | sync |
| `sync_label` | `nv406e` | recorder wait | sync |
| `on_fifo_desync` | `recover_fifo` | probe print | desync |
| `occlusion_queries` | thread | `zcull_control` | queries |
| `logical_id` / `configured_logical_id` | display engine | renderer identity | renderer switch |

---

## 17. Resolved decisions

- **D1 resolved.** `clear.scissor_covers_surface` is the exact guest-space test
  (`origin == 0 && width >= clip_width && height >= clip_height`). The clear
  path uses it and the `surface_state` clear fields and
  `clear_covers_attachment()` are deleted. The host attachment is the same
  extent scaled, so a full guest clear still covers it.
- **D2 resolved.** `patch_transform_constants` only sets
  `draw_state.dirty(transform_constants)`. `resolve_pipeline_state` already
  moves `transform_constants_dirty` into the same group, so there is exactly one
  upload path and one condition.
- **D3 resolved.** `configured_logical_id()` returns the same string format as
  `logical_id()` for the configured renderer, using
  `rsx::resolve_render_backend_type(g_cfg.video.renderer)`; a config change
  therefore changes the string and triggers `reset_renderer`.
- **D4 resolved.** `resolved_clear` and `resolve_surface_clear` are deleted.
  `surface_state` is the value carrier; the clear fields are assigned from
  `draw_state.clear`.
- **D5 resolved.** Metal has no async compute scheduler path in this backend
  (`supports_asynchronous_compute` is set, but no `on_sync_hint` consumer); the
  default no-op is correct. Keep the override absent.

---

## 18. Bug log

Bugs and duplications found during this exercise are recorded in
`docs/research/metal4/design/PORT-BUGS.md`. That file is part of the
deliverable; read it before changing any `capture_*` helper or `clear()`.

---

## 19. Changelog

- **v0.1** — initial normative spec. Re-derived the port around decoded-state
  consumption, deleted shadow state, deleted `resolve_surface_clear`, corrected
  `present_frame`, documented the shared-cache context semantics and the ZCULL
  provider conversion.
- **v0.2** — implementation-exact rewrite after two validation passes:
  exhaustive decoded-vs-raw file table (§5), exact per-file/per-function change
  list (§7), final `capture_*` signatures (§9.5), resolved D1–D5 (§17), L149
  fix written down (§15), test plan (§12), pre-rebase reference note, and the
  bug-log reference (§18). Corrected the earlier mistake that `raster_state`
  carries clip min/max (it is `viewport_state`), and removed the dead
  `clip_x/clip_y` and clear-extent fields from `surface_state`.
