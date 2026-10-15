# LLD — `raster`: viewport, scissor, clipping, culling and rasterizer state

Backend: native Apple **Metal 4** (`MTL4*`) on Apple-silicon (M1+), macOS 26+ (`10` §10 `[V]`).
Cluster `raster` (30 ids, `inventory/out/clusters/raster.csv`). This LLD obeys
`HLD-04-command-encoding.md` (viewport, scissor, depth bias, depth-clip, triangle fill and cull are
`MTL4::RenderCommandEncoder` dynamic state; the render pipeline carries topology and the raster
key, §3.4) and `HLD-05-shader-binding-contract.md` (user clip planes → MSL `[[clip_distance]]`,
point size → `[[point_size]]`, polygon stipple → `discard_fragment()`; §2.6, §3.5). The
surface-clip → viewport/scissor handoff is owned by `LLD-surface.md` §3.3 and consumed here.

Reading conventions: `[V]` verified against the tree (`file:line`), `[I]` inference, `[U]`
unverified. IDs are taken verbatim from the cluster shard; **no register is invented.**

---

## 1. Scope

`raster` is the RSX fixed-function **rasterizer configuration**: the viewport rectangle and its
scale/offset transform, the scissor rectangle, the depth clip range, the user clip-plane control
word, face culling and winding, polygon mode, polygon offset, line and point rasterization state,
and the two stipple enables. In RSX terms these are plain latched values, read at draw time by the
backend's draw-state setup (`VKGSRender::set_viewport` `VKGSRender.cpp:1296`, `set_scissor` `:1326`,
`update_draw_state` `VKDraw.cpp:167`) and by the vertex/fragment program assembly
(`RSXThread.cpp:2411-2437`). They allocate no resource; they are operands to encoder dynamic state,
pipeline raster state, and shader inputs.

This cluster owns the **per-draw rasterizer configuration only**. It does not own the surface clip
that bounds the viewport/scissor (`surface` cluster; the handoff is `LLD-surface.md` §3.3), the
depth test that consumes the stored depth the clip range shapes (`depth-rop`), or the primitive
topology that polygon mode acts on (`draw`; `LLD-draw.md` §3.5). User clip planes are *declared*
here but their vertex-side realisation is a vertex-program concern (`vertex`, `HLD-05`).

**Non-goals.** `SET_SHADE_MODE`/`SET_FLAT_SHADE_OP` (shading, not in this shard);
`SET_ZMIN_MAX_CONTROL` (depth-clip enable, `depth-rop`); `SET_SURFACE_CLIP_*`/`SET_WINDOW_*`
(`surface`). No register is invented: the shard is exactly the 30 ids tabulated below.

---

## 2. Register model

Thirty plain value registers. Two are bound — `SET_CULL_FACE` and `SET_FRONT_FACE`, both to
`nv4097::set_face_property` (`rsx_methods.cpp:1622,1624`), which raises `pipeline_config_dirty`
(`nv4097.cpp:573-593`). One is a `bind_array` of 32 dwords with a `nullptr` handler
(`SET_POLYGON_STIPPLE_PATTERN`, `rsx_methods.cpp:1420`). Every other id is
`methods[...] = nullptr` and is read through the `rsx_methods.h` accessors; dirty classification is
carried by the separate `state_signals[]` table (`rsx_methods.cpp:714-777`), not by the method
entry. So the register array is the value store and the dirty signal is the lifetime carrier.

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_CLIP_MIN` | 0x0394 | `f32 clip_min` (`rsx_decode.h:1104`) | unbound | emulate | MSL `apply_zclip_xform`/`apply_depth_range_xform`; `MTLViewport.znear/zfar` for [0,1] |
| `M_NV4097_SET_CLIP_MAX` | 0x0398 | `f32 clip_max` (`rsx_decode.h:1127`) | unbound | emulate | same; viewport maxDepth only when range is [0,1] |
| `M_NV4097_SET_LINE_WIDTH` | 0x03b8 | `f32 (v>>3)+(v&7)/8` (`rsx_decode.h:3259`) | unbound | emulate | clamp to 1.0; true width via host line-quad expansion |
| `M_NV4097_SET_LINE_SMOOTH_ENABLE` | 0x03bc | `bool` (`rsx_decode.h:828`) | unbound | emulate | host line-quad expansion with coverage alpha; default ignore |
| `M_NV4097_SET_SCISSOR_HORIZONTAL` | 0x08c0 | `origin_x[0:16] width[16:16]` (`rsx_decode.h:114`) | unbound | reshape | `MTLScissorRect` via `setScissorRects` |
| `M_NV4097_SET_SCISSOR_VERTICAL` | 0x08c4 | `origin_y[0:16] height[16:16]` (`rsx_decode.h:142`) | unbound | reshape | `MTLScissorRect` via `setScissorRects` |
| `M_NV4097_SET_VIEWPORT_HORIZONTAL` | 0x0a00 | `origin_x[0:16] width[16:16]` (`rsx_decode.h:58`) | unbound | reshape | `MTLScissorRect` intersection (RSX viewport rect) |
| `M_NV4097_SET_VIEWPORT_VERTICAL` | 0x0a04 | `origin_y[0:16] height[16:16]` (`rsx_decode.h:86`) | unbound | reshape | `MTLScissorRect` intersection (RSX viewport rect) |
| `M_NV4097_SET_POINT_CENTER_MODE` | 0x0a0c | none (`has_decoder=0`) | unbound | na | none (inert; no reader) |
| `M_NV4097_SET_VIEWPORT_OFFSET` | 0x0a20 | `f32 x / +1 y / +2 z / +3 w` (`rsx_decode.h:1288`) | unbound | reshape | `scale_offset_mat` in the per-draw vertex-env buffer |
| `M_NV4097_SET_VIEWPORT_SCALE` | 0x0a30 | `f32 x / +1 y / +2 z / +3 w` (`rsx_decode.h:1196`) | unbound | reshape | `scale_offset_mat` in the per-draw vertex-env buffer |
| `M_NV4097_SET_POLY_OFFSET_POINT_ENABLE` | 0x0a60 | `bool` (`rsx_decode.h:851`) | unbound | impossible | none — no point depth-bias path; fallback ignore |
| `M_NV4097_SET_POLY_OFFSET_LINE_ENABLE` | 0x0a64 | `bool` (`rsx_decode.h:874`) | unbound | impossible | none — no line depth-bias path; fallback ignore |
| `M_NV4097_SET_POLY_OFFSET_FILL_ENABLE` | 0x0a68 | `bool` (`rsx_decode.h:897`) | unbound | reshape | `setDepthBias:slopeScale:clamp:` |
| `M_NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR` | 0x0a78 | `f32` (`rsx_decode.h:1150`) | unbound | reshape | `setDepthBias(...slopeScale:)` |
| `M_NV4097_SET_POLYGON_OFFSET_BIAS` | 0x0a7c | `f32` (`rsx_decode.h:1173`) | unbound | reshape | `setDepthBias(depthBias:...)` |
| `M_NV4097_SET_USER_CLIP_PLANE_CONTROL` | 0x1478 | `6 x clip_planeN[4N:4] → op` (`rsx_decode.h:3195-3233`) | unbound | emulate | MSL `[[clip_distance]]` ×6 via `user_clip_factor` |
| `M_NV4097_SET_POLYGON_STIPPLE` | 0x147c | `bool` (`rsx_decode.h:4140`) | unbound | emulate | fragment `_ENABLE_POLYGON_STIPPLE` + `discard_fragment()` |
| `M_NV4097_SET_POLYGON_STIPPLE_PATTERN` | 0x1480 | none (`bind_array` ×32) | `bind_array` (nullptr) | emulate | `RasterizerHeap` buffer at `MTL4ArgumentTable` slot 9 |
| `M_NV4097_SET_FRONT_POLYGON_MODE` | 0x1828 | `to_polygon_mode` (`rsx_decode.h:4071`) | unbound | reshape | `MTLTriangleFillMode` via `setTriangleFillMode` |
| `M_NV4097_SET_BACK_POLYGON_MODE` | 0x182c | `to_polygon_mode` (`rsx_decode.h:4094`) | unbound | reshape | `MTLTriangleFillMode` via `setTriangleFillMode` |
| `M_NV4097_SET_CULL_FACE` | 0x1830 | `cull_face front/back/front_and_back` (`rsx_decode.h:2724`) | `set_face_property` | reshape | `MTL::CullMode` via `setCullMode` |
| `M_NV4097_SET_FRONT_FACE` | 0x1834 | `to_front_face cw/ccw` (`rsx_decode.h:2702`) | `set_face_property` | reshape | `MTL::Winding` via `setFrontFacingWinding` |
| `M_NV4097_SET_POLY_SMOOTH_ENABLE` | 0x1838 | `bool` (`rsx_decode.h:943`) | unbound | na | none (accessor exists, no consumer) |
| `M_NV4097_SET_CULL_FACE_ENABLE` | 0x183c | `bool` (`rsx_decode.h:920`) | unbound | reshape | gate `MTL::CullMode` (None vs mode) |
| `M_NV4097_SET_LINE_STIPPLE` | 0x1db4 | none (`has_decoder=0`) | unbound | impossible | none — no Metal line stipple; fallback ignore |
| `M_NV4097_SET_LINE_STIPPLE_PATTERN` | 0x1db8 | none (`has_decoder=0`) | unbound | impossible | none — no Metal line stipple; fallback ignore |
| `M_NV4097_SET_POINT_SIZE` | 0x1ee0 | `f32` (`rsx_decode.h:3282`) | unbound | reshape | MSL `[[point_size]]` from the vertex-env buffer |
| `M_NV4097_SET_POINT_PARAMS_ENABLE` | 0x1ee4 | none (`has_decoder=0`) | unbound | na | none (inert; no reader) |
| `M_NV4097_SET_POINT_SPRITE_CONTROL` | 0x1ee8 | `enabled[0:1] texcoord_mask[8:10]` (`rsx_decode.h:3305-3313`) | unbound | emulate | MSL `[[point_coord]]` in the FS; fragment-program variant |

**2.1 Viewport and scissor.** `SET_VIEWPORT_HORIZONTAL`/`_VERTICAL` (`origin_x`/`width`,
`origin_y`/`height`) and `SET_SCISSOR_HORIZONTAL`/`_VERTICAL` are the two rectangles.
`SET_VIEWPORT_SCALE`/`_OFFSET` are the separate 4-vector NDC→window transform. The RSX viewport
*rectangle* is **not** the transform: on a draw the backend intersects the scissor with the viewport
rect (`get_scissor(clip_viewport=true)`, `RSXThread.cpp:2115-2129`, called with
`clipped_scissor = (context == context_draw)` `VKGSRender.cpp:2618`), while the transform is built
from scale/offset and the **surface clip** (`fill_scale_offset_data`,
`RSXDrawCommands.cpp:563-586`). `[V]`

**2.2 Depth clip range.** `SET_CLIP_MIN`/`SET_CLIP_MAX` are two `f32`s (`bit_cast`, `rsx_decode.h:1104`,
`:1127`); defaults `0x0`/`0x3f800000` (`rsx_methods.cpp:480-481`). They are consumed three ways:
as the viewport `minDepth`/`maxDepth` when the device exposes unrestricted depth range
(`VKGSRender.cpp:1311-1320`, `:1346-1347`), as the depth-bounds fallback bound
(`VKDraw.cpp:254-255`), and as two vertex-env scalars for the shader (`buf+76`/`buf+80`,
`VKGSRender.cpp:2138-2139`). Changing either raises `invalidate_zclip_bits`
(`rsx_methods.cpp:717-718`). `[V]`

**2.3 Face culling, winding and polygon mode.** `SET_CULL_FACE` decodes to
`cull_face{front,back,front_and_back}` (`rsx_decode.h:2724`), `SET_FRONT_FACE` to
`front_face{cw,ccw}` (`:2702`), and `SET_CULL_FACE_ENABLE` gates culling (`:920`). Both face
registers are the cluster's only bound ids; `set_face_property` validates the raw value and sets
`pipeline_config_dirty` (`nv4097.cpp:573-593`). `SET_FRONT_POLYGON_MODE`/`SET_BACK_POLYGON_MODE`
decode to `polygon_mode{point,line,fill}` (`rsx_decode.h:4071`, `:4094`;
`gcm_enums.h:1714-1727`); defaults are `0x1b02` = fill (`rsx_methods.cpp:500-503`). `[V]`

**2.4 Polygon offset.** `SET_POLY_OFFSET_FILL_ENABLE` gates the offset, and
`SET_POLYGON_OFFSET_SCALE_FACTOR`/`_BIAS` carry the slope and constant factors
(`rsx_decode.h:1150`/`:1173`); all three raise `polygon_offset_state_dirty`
(`rsx_methods.cpp:771-773`). `SET_POLY_OFFSET_POINT_ENABLE`/`_LINE_ENABLE` select the point and line
primitive modes (`rsx_decode.h:851`/`:874`) and have no dirty signal — they are read by GL
(`GLDraw.cpp:274-275`) and ignored by VK. `[V]`

**2.5 User clip planes.** `SET_USER_CLIP_PLANE_CONTROL` packs six 4-bit ops
(`disable`/`less_than`/`greater_or_equal`, `rsx_decode.h:3195-3233`) and raises `vertex_state_dirty`
(`rsx_methods.cpp:715`). `clip_planes_mask()` (`rsx_methods.h:719-722`) and
`fill_user_clip_data` (`RSXDrawCommands.cpp:588-640`) pack a 12-bit field (`0b01…01` fast path for
"all disabled") into the vertex env at `buf+64`. `[V]`

**2.6 Stipple.** `SET_POLYGON_STIPPLE` is a boolean enable (`rsx_decode.h:4140`) that sets the
fragment-program control bit `RSX_SHADER_CONTROL_POLYGON_STIPPLE` for polygon draws
(`RSXThread.cpp:2428-2431`) and raises `fragment_program_state_dirty` (`rsx_methods.cpp:738`). Its
pattern is the 32-dword `SET_POLYGON_STIPPLE_PATTERN` array (`rsx_methods.h:1349-1352`), each dword
raising `polygon_stipple_pattern_dirty` (`rsx_methods.cpp:739-770`). `SET_LINE_STIPPLE` and
`SET_LINE_STIPPLE_PATTERN` have **no decoder and no reader** in the tree (`rsx_methods.cpp:1481-1482`).
`[V]`

**2.7 Line and point rasterization.** `SET_LINE_WIDTH` decodes the packed fixed/fraction form
(`rsx_decode.h:3259`), default `0x8` = 1.0 (`rsx_methods.cpp:361`). `SET_LINE_SMOOTH_ENABLE` is a
boolean (`:828`). `SET_POINT_SIZE` is an `f32` (`:3282`), default 1.0 (`rsx_methods.cpp:364`), written
into the vertex env at `buf+72` (`VKGSRender.cpp:2137`). `SET_POINT_SPRITE_CONTROL` carries the
enable and the 10-bit texcoord override mask (`rsx_decode.h:3305-3313`); it is folded into the high
word of `texcoord_control_mask` only for point draws (`RSXThread.cpp:2433-2437`) and raises
`fragment_program_state_dirty` (`rsx_methods.cpp:714`). `SET_POINT_CENTER_MODE`,
`SET_POINT_PARAMS_ENABLE` and `SET_POLY_SMOOTH_ENABLE` have no decoder/reader (or, for poly-smooth,
an accessor with no consumer) and are inert. `[V]`

**2.8 Lifetime.** Value registers with no cross-draw object. The dirty class decides the carrier:
viewport/scissor → encoder dynamic state; scale/offset, user clip, point size, clip min/max →
vertex env (rebuilt on `vertex_state_dirty`); cull/front/polygon-mode → the raster pipeline key;
polygon stipple enable / point sprite → fragment-program state; polygon-offset → encoder depth bias.

---

## 3. Metal 4 mapping

**The shape change.** RSX rasterizer state is one flat register file read at draw time; Metal 4
splits it across **three carriers** and deletes the Vulkan rasterization object:

1. **`MTL4::RenderCommandEncoder` dynamic state** — viewport, scissor, depth bias, triangle fill,
   cull mode, front-facing winding (`HLD-04` §3.4). No `VkPipelineRasterizationStateCreateInfo`, no
   `VkViewport`-as-clip-range.
2. **The render-pipeline state** — the raster key (topology class, raster enabled) that
   `MTL4::Compiler::newRenderPipelineState` bakes and that feeds the PSO cache key (`HLD-05` §3.5).
3. **The shader** — the vertex env buffer (scale/offset matrix, user-clip field, point size, clip
   range) and MSL attributes (`[[clip_distance]]`, `[[point_size]]`, `[[point_coord]]`).

The pivotal consequence: **the Metal viewport is the surface clip, not the RSX viewport rectangle.**
`fill_scale_offset_data` already folds the viewport transform into the matrix the VP multiplies
`gl_Position` by (`VKGSRender.cpp:1305` comment; `RSXDrawCommands.cpp:563-586`), so the encoder's
`MTLViewport` is a full-attachment rectangle and the RSX viewport rect survives only as the scissor
clip. This mirrors the existing VK/GL mapping and must not be "corrected" into a naive
`viewport_origin_x/y/width/height → MTLViewport` port. `[V]`

The table below repeats the §2 rows grouped by mapping sub-area; the prose after it gives the
concrete objects.

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_VIEWPORT_HORIZONTAL` | 0x0a00 | `origin_x[0:16] width[16:16]` (`rsx_decode.h:58`) | unbound | reshape | `MTLScissorRect` intersection (RSX viewport rect) |
| `M_NV4097_SET_VIEWPORT_VERTICAL` | 0x0a04 | `origin_y[0:16] height[16:16]` (`rsx_decode.h:86`) | unbound | reshape | `MTLScissorRect` intersection (RSX viewport rect) |
| `M_NV4097_SET_VIEWPORT_OFFSET` | 0x0a20 | `f32 x / +1 y / +2 z / +3 w` (`rsx_decode.h:1288`) | unbound | reshape | `scale_offset_mat` in the per-draw vertex-env buffer |
| `M_NV4097_SET_VIEWPORT_SCALE` | 0x0a30 | `f32 x / +1 y / +2 z / +3 w` (`rsx_decode.h:1196`) | unbound | reshape | `scale_offset_mat` in the per-draw vertex-env buffer |
| `M_NV4097_SET_SCISSOR_HORIZONTAL` | 0x08c0 | `origin_x[0:16] width[16:16]` (`rsx_decode.h:114`) | unbound | reshape | `MTLScissorRect` via `setScissorRects` |
| `M_NV4097_SET_SCISSOR_VERTICAL` | 0x08c4 | `origin_y[0:16] height[16:16]` (`rsx_decode.h:142`) | unbound | reshape | `MTLScissorRect` via `setScissorRects` |
| `M_NV4097_SET_CLIP_MIN` | 0x0394 | `f32 clip_min` (`rsx_decode.h:1104`) | unbound | emulate | MSL `apply_zclip_xform`/`apply_depth_range_xform`; `MTLViewport.znear/zfar` for [0,1] |
| `M_NV4097_SET_CLIP_MAX` | 0x0398 | `f32 clip_max` (`rsx_decode.h:1127`) | unbound | emulate | same; viewport maxDepth only when range is [0,1] |
| `M_NV4097_SET_CULL_FACE` | 0x1830 | `cull_face front/back/front_and_back` (`rsx_decode.h:2724`) | `set_face_property` | reshape | `MTL::CullMode` via `setCullMode` |
| `M_NV4097_SET_FRONT_FACE` | 0x1834 | `to_front_face cw/ccw` (`rsx_decode.h:2702`) | `set_face_property` | reshape | `MTL::Winding` via `setFrontFacingWinding` |
| `M_NV4097_SET_CULL_FACE_ENABLE` | 0x183c | `bool` (`rsx_decode.h:920`) | unbound | reshape | gate `MTL::CullMode` (None vs mode) |
| `M_NV4097_SET_FRONT_POLYGON_MODE` | 0x1828 | `to_polygon_mode` (`rsx_decode.h:4071`) | unbound | reshape | `MTLTriangleFillMode` via `setTriangleFillMode` |
| `M_NV4097_SET_BACK_POLYGON_MODE` | 0x182c | `to_polygon_mode` (`rsx_decode.h:4094`) | unbound | reshape | `MTLTriangleFillMode` via `setTriangleFillMode` |
| `M_NV4097_SET_POLY_OFFSET_FILL_ENABLE` | 0x0a68 | `bool` (`rsx_decode.h:897`) | unbound | reshape | `setDepthBias:slopeScale:clamp:` |
| `M_NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR` | 0x0a78 | `f32` (`rsx_decode.h:1150`) | unbound | reshape | `setDepthBias(...slopeScale:)` |
| `M_NV4097_SET_POLYGON_OFFSET_BIAS` | 0x0a7c | `f32` (`rsx_decode.h:1173`) | unbound | reshape | `setDepthBias(depthBias:...)` |
| `M_NV4097_SET_POLY_OFFSET_POINT_ENABLE` | 0x0a60 | `bool` (`rsx_decode.h:851`) | unbound | impossible | none — no point depth-bias path; fallback ignore |
| `M_NV4097_SET_POLY_OFFSET_LINE_ENABLE` | 0x0a64 | `bool` (`rsx_decode.h:874`) | unbound | impossible | none — no line depth-bias path; fallback ignore |
| `M_NV4097_SET_USER_CLIP_PLANE_CONTROL` | 0x1478 | `6 x clip_planeN[4N:4] → op` (`rsx_decode.h:3195-3233`) | unbound | emulate | MSL `[[clip_distance]]` ×6 via `user_clip_factor` |
| `M_NV4097_SET_POLYGON_STIPPLE` | 0x147c | `bool` (`rsx_decode.h:4140`) | unbound | emulate | fragment `_ENABLE_POLYGON_STIPPLE` + `discard_fragment()` |
| `M_NV4097_SET_POLYGON_STIPPLE_PATTERN` | 0x1480 | none (`bind_array` ×32) | `bind_array` (nullptr) | emulate | `RasterizerHeap` buffer at `MTL4ArgumentTable` slot 9 |
| `M_NV4097_SET_LINE_WIDTH` | 0x03b8 | `f32 (v>>3)+(v&7)/8` (`rsx_decode.h:3259`) | unbound | emulate | clamp to 1.0; true width via host line-quad expansion |
| `M_NV4097_SET_LINE_SMOOTH_ENABLE` | 0x03bc | `bool` (`rsx_decode.h:828`) | unbound | emulate | host line-quad expansion with coverage alpha; default ignore |
| `M_NV4097_SET_LINE_STIPPLE` | 0x1db4 | none (`has_decoder=0`) | unbound | impossible | none — no Metal line stipple; fallback ignore |
| `M_NV4097_SET_LINE_STIPPLE_PATTERN` | 0x1db8 | none (`has_decoder=0`) | unbound | impossible | none — no Metal line stipple; fallback ignore |
| `M_NV4097_SET_POINT_SIZE` | 0x1ee0 | `f32` (`rsx_decode.h:3282`) | unbound | reshape | MSL `[[point_size]]` from the vertex-env buffer |
| `M_NV4097_SET_POINT_SPRITE_CONTROL` | 0x1ee8 | `enabled[0:1] texcoord_mask[8:10]` (`rsx_decode.h:3305-3313`) | unbound | emulate | MSL `[[point_coord]]` in the FS; fragment-program variant |
| `M_NV4097_SET_POINT_CENTER_MODE` | 0x0a0c | none (`has_decoder=0`) | unbound | na | none (inert; no reader) |
| `M_NV4097_SET_POINT_PARAMS_ENABLE` | 0x1ee4 | none (`has_decoder=0`) | unbound | na | none (inert; no reader) |
| `M_NV4097_SET_POLY_SMOOTH_ENABLE` | 0x1838 | `bool` (`rsx_decode.h:943`) | unbound | na | none (accessor exists, no consumer) |

### 3.1 Viewport and scissor

The encoder's `MTLViewport` is set to `{0, 0, clip_width, clip_height}` — the surface clip, not the
RSX viewport rect — because `scale_offset_mat` already carries the transform
(`VKGSRender.cpp:1305-1309`). `SET_VIEWPORT_SCALE`/`_OFFSET` are read into
`fill_scale_offset_data` (`RSXDrawCommands.cpp:563-586`) and consumed by the VP; they never reach a
Metal viewport setter. The `flip_y` parameter of that function is passed **`false`** by both the VK
and GL backends (`VKGSRender.cpp:2134`, `GLGSRender.cpp:979`), so the Metal viewport origin is
**not** y-flipped: the RSX Y convention is expressed through the matrix (`scale_y`/`offset_y`), and a
naive `viewport_origin_y`-flip would double-apply it. `[V]`

`SET_SCISSOR_HORIZONTAL`/`_VERTICAL` become the `MTLScissorRect`; on a draw they are first
intersected with the RSX viewport rect (`get_scissor(clip_viewport=true)`, `RSXThread.cpp:2115-2129`),
which is the only remaining role of `SET_VIEWPORT_HORIZONTAL`/`_VERTICAL`. Both are set together via
`setScissorRects`/`setViewports` on the `MTL4::RenderCommandEncoder`. `[V]`/`[I]`

### 3.2 Depth clip range and the [0,1] shape change

RSX clips depth against `[clip_min, clip_max]`, which is **not** [0,1]: the RSX-first frame keeps
float depth targets whose stored window Z runs far above 1 (`apply_depth_range_xform` comment,
`RSXVertexPrologue.glsl:59`). Metal's clip space is fixed [0,1] and there is no depth-clip-range
viewport parameter; `MTLViewport.znear/zfar` is a [0,1]-clamped *depth-range* mapping, not a clip
range. The shape change is therefore: the [0,1] subset maps to `MTLViewport.znear/zfar`; the general
case is realised in the vertex shader by `apply_zclip_xform` (maps RSX window Z into stored depth,
`RSXVertexPrologue.glsl:10-55`) or, under `_EMULATE_DEPTH_RANGE`, `apply_depth_range_xform`
(`:58-69`). This mirrors the VK `_EMULATE_DEPTH_RANGE` path (`VKDraw.cpp:258-268`;
`backend_config.supports_extended_depth_range`, `10` §2.2). `[V]`/`[I]`

### 3.3 Culling, winding and polygon mode

`SET_CULL_FACE_ENABLE` selects between `MTL::CullModeNone` and the decoded
`MTL::CullModeFront`/`Back` (RSX `front_and_back` maps to `MTLCullModeNone` semantics only if the
backend folds it; VK maps it to `VK_CULL_MODE_FRONT_AND_BACK`, `VKGSRender.cpp:216-218`).
`SET_FRONT_FACE` selects `MTL::WindingClockwise`/`CounterClockwise`
(`VKGSRender.cpp:205-206`). Both are set with `setCullMode`/`setFrontFacingWinding` and are part of
the raster pipeline key (`HLD-04` §3.4, `HLD-05` §3.5). `[V]`

Polygon mode is the sharpest shape change. Metal exposes **one** triangle fill mode per encoder
(`MTLTriangleFillModeFill`/`Lines`, `HLD-04` §3.4) and no point fill mode; RSX has separate
front/back modes and a point mode. The backend collapses the pair to a single mode (choose the front
mode unless front faces are culled, as GL does at `GLDraw.cpp:315`) and treats `point` as
unimplemented; the current VK backend goes further and hardcodes fill
(`vkutils/graphics_pipeline_state.hpp:33`). `[V]`/`[I]`

### 3.4 Polygon offset

`SET_POLY_OFFSET_FILL_ENABLE` gates a single encoder call,
`setDepthBias(depthBias: slopeScale: clamp:)` with `depthBias = POLYGON_OFFSET_BIAS` and
`slopeScale = POLYGON_OFFSET_SCALE_FACTOR`, matching VK's
`vkCmdSetDepthBias(bias, 0, scale)` (`VKDraw.cpp:212-239`). The NVIDIA-specific D24 `bias *= 0.5`
correction (`VKDraw.cpp:222-231`) does **not** apply on Apple silicon, which has no D24 stencil
format (`README` §4 shift 6). `SET_POLY_OFFSET_POINT_ENABLE`/`_LINE_ENABLE` have no Metal path
(§4). `[V]`

### 3.5 User clip planes

`SET_USER_CLIP_PLANE_CONTROL`'s six 4-bit ops are packed by `fill_user_clip_data`
(`RSXDrawCommands.cpp:588-640`) into the vertex env (`buf+64`) and realised in MSL as six
`[[clip_distance]]` outputs gated by `user_clip_factor(n)`, exactly as VK maps them to
`gl_ClipDistance[0..5]` (`VKVertexProgram.cpp:286-292`; `README` §3 #96). Metal has no fixed-function
clip plane; the `less_than`/`greater_or_equal` polarity is the sign applied by the factor. `[V]`

### 3.6 Polygon stipple

`SET_POLYGON_STIPPLE` sets `RSX_SHADER_CONTROL_POLYGON_STIPPLE` on polygon draws
(`RSXThread.cpp:2428-2431`), which the fragment program renders as `_ENABLE_POLYGON_STIPPLE`
(`GLSLCommon.cpp:271-273`): the 32-dword pattern is bound as the `RasterizerHeap` storage buffer at
`MTL4ArgumentTable` slot 9 (`HLD-04` §3.3; VK mirror `VKFragmentProgram.cpp:333-335`) and the FS
`discard_fragment()`s where the stipple bit is clear. This is a shader emulation, not Metal state.
`[V]`

### 3.7 Line and point rasterization

Line width has no public Metal path (`wideLines` is private-API only, `README` §6 R24); the RSX-first
choice is to clamp to 1.0, with a faithful `line_width` requiring host line-quad expansion (VK clamps
unless `wide_lines`, `VKDraw.cpp:175-177`). Line smoothing and line stipple are `emulate`/`impossible`
(§4). Point size is written to the vertex env (`buf+72`, `VKGSRender.cpp:2137`) and emitted as MSL
`[[point_size]]` (`VKVertexProgram.cpp:481`); point sprites fold `SET_POINT_SPRITE_CONTROL` into the
FP's `texcoord_control_mask` high word (`RSXThread.cpp:2433-2437`) so the fragment program reads MSL
`[[point_coord]]` (`FragmentProgramDecompiler.cpp:658-673`), a shader/pipeline variant rather than
encoder state. `[V]`

### 3.8 Objects used

`MTL4::RenderCommandEncoder` (`setViewports`, `setScissorRects`, `setDepthBias:slopeScale:clamp:`,
`setTriangleFillMode`, `setCullMode`, `setFrontFacingWinding`); `MTLViewport`; `MTLScissorRect`;
`MTLTriangleFillMode`; `MTL::CullMode`/`MTL::Winding`; `MTL4::RenderPipelineDescriptor` (raster
key); `MTL4::ArgumentTable::setAddress` (vertex env, slot 9 stipple); MSL `[[clip_distance]]`,
`[[point_size]]`, `[[point_coord]]`. No new object class is introduced by this cluster.

---

## 4. Impossible / not-recommended

- **Impossible: depth bias for point and line primitives.** `SET_POLY_OFFSET_POINT_ENABLE` and
  `SET_POLY_OFFSET_LINE_ENABLE` select polygon-offset modes for non-triangle primitives; Metal's
  `setDepthBias` applies to rasterized triangles only and there is no per-primitive-mode offset
  state. Chosen fallback: ignore both, as VK does (`VKDraw.cpp:212` honours only
  `poly_offset_fill_enabled`). `[V]`
- **Impossible: line stipple.** `SET_LINE_STIPPLE`/`SET_LINE_STIPPLE_PATTERN` have no decoder and no
  reader in the tree; Metal has no line-stipple state. Chosen fallback: ignore. `[V]`
- **Not-recommended: a naive RSX-viewport-rect → `MTLViewport` port.** It would double-apply the
  transform, which `scale_offset_mat` already carries, and would also y-flip (both backends pass
  `flip_y=false`). The Metal viewport is the surface clip (§3.1). `[V]`
- **Not-recommended: forcing out-of-[0,1] `clip_min`/`clip_max` into `MTLViewport.znear/zfar`.** The
  viewport depth range is [0,1]-clamped; values outside it must go through the shader zclip/depth
  range transform (§3.2). `[V]`
- **Not-recommended: separate front/back triangle fill modes.** Metal has one fill mode per encoder;
  the front/back pair must be collapsed, and `point` is not expressible at all (§3.3). `[I]`
- **Not-recommended: emulating line width by multiplying `line_width` in a `setLineWidth`-shaped
  call.** No such call exists publicly; the only faithful route is host geometry expansion (§3.7). `[V]`
- **`na` (inert): `SET_POINT_CENTER_MODE`, `SET_POINT_PARAMS_ENABLE`, `SET_POLY_SMOOTH_ENABLE`.**
  No decoder and no consumer (poly-smooth has an accessor but no reader); the fallback is to keep
  them as opaque shadow state. `[V]`

---

## 5. Invariants & failure modes

**Invariants.**
1. The `MTLViewport` rectangle is the surface clip (`surface_clip_width/height`), never the RSX
   viewport rect; the transform lives in `scale_offset_mat` (`VKGSRender.cpp:1305`). `[V]`
2. `flip_y` stays `false`; the viewport origin is not y-flipped (`VKGSRender.cpp:2134`,
   `GLGSRender.cpp:979`). `[V]`
3. On a draw the scissor is `scissor ∩ viewport_rect` (`RSXThread.cpp:2115-2129`); an empty
   intersection marks the setup invalid and suppresses the draw (`:2138-2146`). `[V]`
4. Out-of-[0,1] clip values never reach `MTLViewport.znear/zfar`; the shader transform owns the
   general case (§3.2). `[I]`
5. `SET_USER_CLIP_PLANE_CONTROL`'s six planes are declared as `[[clip_distance]]` outputs even when
   disabled (a declared clip-distance output must always be assigned; `VKVertexProgram.cpp:285`). `[V]`
6. Depth bias is applied only when `POLY_OFFSET_FILL_ENABLE` is set and only for the fill polygon
   mode; the point/line enables change nothing (§4). `[V]`
7. The point-sprite mask is folded into the FP variant only for point draws
   (`RSXThread.cpp:2433-2434`), so it must be part of the fragment shader cache key. `[V]`

**Failure modes.**
- **F1 double-applied viewport transform.** Porting the RSX viewport rect into `MTLViewport` on top
  of `scale_offset_mat` scales/offsets every draw twice. Detection: H5 image diff on a title with a
  non-default viewport rect. `[I]`
- **F2 spurious y-flip.** Setting the Metal viewport origin/size from RSX with a Y flip inverts the
  frame; the transform is already in the matrix. Detection: H5 (vertically mirrored output). `[I]`
- **F3 depth-range collapse.** Feeding an extended `clip_max` (>1) into `znear/zfar` clamps and
  flattens float-depth precision. Detection: H5 depth diff on a float-Z target. `[I]`
- **F4 scissor/viewport intersection dropped.** Skipping the `get_scissor(clip_viewport=true)`
  intersection draws outside the RSX viewport rect. Detection: H2 decision trace on a draw whose
  viewport rect is smaller than the scissor. `[V]`
- **F5 polygon-mode mis-collapse.** Choosing the back mode when front faces are visible (or vice
  versa) changes which primitives are wireframed. Detection: H5 on a wireframe-heavy title. `[I]`
- **F6 point-sprite variant aliasing.** The point-sprite mask omitted from the FP shader key reuses a
  non-sprite shader on a point draw. Detection: H1 MSL golden diff. `[I]`
- **F7 clip-distance assignment elision.** Emitting a `[[clip_distance]]` output only when the plane
  is enabled leaves it unassigned when later enabled. Detection: H1 translate diff / H5. `[V]`

---

## 6. Open questions

- **Q1 `[U]` Does Apple float depth need the shader zclip transform at all?** `10` §2.2 rates
  `supports_extended_depth_range` "free `[I]`/`[U]`": Metal stores float depth without a documented
  [0,1] clamp toggle. If the clamp is genuinely absent for float targets, `SET_CLIP_MIN`/`SET_CLIP_MAX`
  reduce to `MTLViewport.znear/zfar` and the `_EMULATE_DEPTH_RANGE` shader path disappears for those
  targets. *Experiment:* store a depth > 1 into a `Depth32Float` target on device and read it back.
- **Q2 `[U]` Does any title rely on polygon-mode line/point?** Metal gives one fill mode and no point
  mode; the front/back collapse is lossy. If a title is wireframe-heavy, the fallback moves from
  "force fill" to a host triangle→line expansion. *Experiment:* scan replays for
  `SET_FRONT/BACK_POLYGON_MODE != 0x1b02`.
- **Q3 `[U]` Does any title rely on wide lines or line smoothing?** Both are quality features with no
  public Metal path (`README` §6 R24). If unused, the clamp-to-1.0 and ignore fallbacks are total.
  *Experiment:* scan FIFOs for `SET_LINE_WIDTH != 0x8` and `SET_LINE_SMOOTH_ENABLE != 0`.
- **Q4 `[U]` Is the RSX viewport-rect → scissor intersection ever observable?** It is applied only for
  `context_draw` (`VKGSRender.cpp:2618`). If the viewport rect always contains the scissor in
  practice, the intersection is dead and the Metal scissor is just `SET_SCISSOR_*`. *Experiment:*
  compare `SET_VIEWPORT_HORIZONTAL/VERTICAL` against `SET_SCISSOR_*` across a replay corpus.
- **Q5 `[U]` Do `SET_POLY_OFFSET_POINT_ENABLE`/`_LINE_ENABLE` ever matter?** They are ignored by VK
  and have no Metal path (§4). If no title sets them, the `impossible` rows are safe. *Experiment:*
  scan replays for non-zero values.
- **Q6 `[I]` Does `front_and_back` culling map to `MTLCullModeNone`, or must it be emulated?** RSX
  `CELL_GCM_FRONT_AND_BACK` culls both faces (nothing rasterises); Metal has no such mode. The
  candidate mappings are `MTLCullModeNone` plus a raster-disable, or dropping the draw. The choice
  belongs to the pipeline key. `[I]`
- **Q7 `[U]` Is `SET_POLYGON_STIPPLE_PATTERN`'s `bind_array` read path ever reached?** The register
  array is a `bind_array` with a `nullptr` handler; the pattern is read via `registers.data()`
  (`rsx_methods.h:1351`). Confirm the generator's binding classification does not imply a handler
  exists. `[V]`

**Highest-value (report-back set):** Q1 (float-depth clamp), Q2 (polygon-mode line/point), Q3 (wide
lines / smoothing), Q4 (viewport-rect scissor intersection).

---

## 7. Provenance

**This tree `[V]` (paths under `rpcs3/Emu/RSX/`).**
`rsx_decode.h:48-101` (viewport rect), `:104-157` (scissor), `:818-949` (line-smooth, poly-offset
point/line/fill, cull-enable, poly-smooth), `:1094-1137` (clip min/max), `:1140-1183` (polygon-offset
scale/bias), `:1186-1367` (viewport scale/offset), `:2690-2734` (front face, cull face),
`:3187-3246` (user clip plane), `:3249-3268` (line width), `:3272-3292` (point size), `:3295-3320`
(point sprite control), `:4061-4104` (front/back polygon mode), `:4130-4150` (polygon stipple).
`gcm_enums.h:1654-1698` (`front_face`, `cull_face`, `user_clip_plane_op`), `:1714-1727`
(`polygon_mode`). `rsx_methods.h:184-222` (viewport/scissor rect accessors), `:443-450`
(clip min/max), `:499-541` (line-smooth, poly-offset, cull, poly-smooth), `:689-741` (clip planes,
front/cull, line width, point size), `:1344-1367` (point-sprite mask, polygon stipple, polygon
modes). `rsx_methods.cpp:242-245,349-371,478-511` (reset defaults), `:714-777` (`state_signals`),
`:1371,1390,1400-1407,1418-1420,1441-1446,1481-1482,1491-1493` (`methods[...] = nullptr`),
`:1622,1624` (`bind` face registers), `:1600-1611` (`bind_array`). `NV47/HW/nv4097.cpp:573-593`
(`set_face_property`). `Core/RSXDrawCommands.cpp:563-586` (`fill_scale_offset_data`), `:588-640`
(`fill_user_clip_data`). `RSXThread.cpp:2115-2146` (`get_scissor` viewport clip),
`:2428-2437` (polygon-stipple and point-sprite capture). `VK/VKGSRender.cpp:205-218`
(front/cull mapping), `:1296-1355` (`set_viewport`/`set_scissor`/`bind_viewport`),
`:2124-2145` (vertex env fill), `:2244` (stipple pattern copy), `:2618-2648,2847-2848`
(`clipped_scissor` call sites). `VK/VKDraw.cpp:167-278` (`update_draw_state`: line width
`:175-177`, depth bias `:212-239`, depth bounds `:241-271`, `bind_viewport` `:273`).
`VK/vkutils/graphics_pipeline_state.hpp:33` (`VK_POLYGON_MODE_FILL`). `VK/VKVertexProgram.cpp:276-303`
(clip-distance / point-size outputs), `:394,481` (`point_size`). `GL/GLDraw.cpp:265-316`
(line width/smooth, poly-offset, cull/front, polygon mode), `GL/GLGSRender.cpp:79-99`
(`set_viewport`/`set_scissor`), `:979-984` (vertex env, `flip_y=false`).
`Program/GLSLSnippets/RSXProg/RSXVertexPrologue.glsl:7-70` (`apply_zclip_xform`,
`apply_depth_range_xform`). `Program/GLSLCommon.cpp:211,271-273` (stipple define).
`Program/FragmentProgramDecompiler.cpp:658-673` (`gl_PointCoord`). `Program/RSXFragmentProgram.h:266`
(`texcoord_control_mask`). `VK/VKFragmentProgram.cpp:333-335,415` (`RasterizerHeap`,
`ROP_polygon_stipple_test`).

**Inventory (facts).** `inventory/out/clusters/raster.csv` (30 ids, 2 bound, 1 `bind_array`);
`inventory/out/SUMMARY.md`; `inventory/SCHEMA.md` §4 (disposition vocab), §5 (CSV columns),
§6 (LLD template).

**Sibling scopes `[V]`.** `HLD-04-command-encoding.md` §3.3 (slot map; slot 9 rasterizer env), §3.4
(PSO vs encoder dynamic state; viewport/scissor/depth-bias/triangle-fill/cull);
`HLD-05-shader-binding-contract.md` §2.6 (cluster→object table, this cluster =
PSO+encoder+shader), §3.2 (slot map), §3.5 (PSO assembly; `setCullMode`/`setFrontFacingWinding`);
`LLD-surface.md` §3.3 (surface clip → viewport/scissor handoff); `10-rsx-native-metal4-architecture.md`
§2.2 (`supports_extended_depth_range`, `supports_last_provoking_vertex`), §3.1 (reachable
capabilities; line width clamp), §4.3/§4.4 (pass/barrier model); `README.md` §3 #90 (rasterization
state), #95 (dynamic line width), #96 (builtins), §4 shift 6 (D24 format), §6 R24 (wide lines).

<!-- doc-end -->
