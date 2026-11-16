# Metal 4: closing parity gaps D7, D5/D6, D1 and D2

Status: design for review, then implementation.  Scope: the four gaps the parity
audit left at the top of the list (`METAL4-VK-GL-PARITY.md` §18).  RSX behaviour
is the specification; every change must be exactly RSX-correct, must not slow the
default path, and must be expressible on native Metal 4.

Evidence tags: `[V]` verified by reading the code at the cited line, `[I]`
inferred from the cited code or from a measurement, `[U]` unknown / needs the
runtime to settle.

---

## 0. Summary of the work

| Gap | Defect today | Fix | Risk |
|---|---|---|---|
| D7 | `nv3089` transfers fall back to the shared CPU `SWScale` path, the only Metal-only writer of guest memory | run the transfer on the GPU through the shared `upload_scaled_image`, with a purpose-built render pass blitter | medium: new pass, shared-cache integration |
| D5 | a scaled subresource copy is dropped with an error line | same blitter, nearest filter | low |
| D6 | a typeless copy between different texel sizes is dropped with an error line | staging bit-cast with the destination texel width | low |
| D1 | flat shading silently degrades to smooth interpolation | claim last-vertex provoking and rotate every triangle/line so the RSX provoking vertex is Metal's first | medium: index writers |
| D2 | a framebufferless draw opens no pass and is a NOP, so occlusion counters do not advance | depth-only scratch attachment and a neutralised depth test, backend only | low |

Order of implementation, cheapest first and each independently revertible:
D2 → D7 → D5/D6 → D1.  D7 and D5/D6 share one blitter, so D5/D6 lands after D7.

---

## 1. Metal facts the design depends on

Measured with `docs/research/metal4/design/EXPERIMENT-blit-and-attachments.mm`
(which re-execs itself per case, so an API assertion cannot hide the following
cases) and `EXPERIMENT-blit-channel-order.mm`, under `MTL_DEBUG_LAYER=1` on the
development machine:

| Case | Question | Result |
|---|---|---|
| A | zero-attachment pass with `setDefaultRasterSampleCount(1)` | pass builds, **zero fragments rasterised** `[V]` |
| B | colour attachment + colour-less pipeline | assertion at `setRenderPipelineState`: `pipeline's pixelFormat (Invalid) does not match the framebuffer's (BGRA8Unorm)` `[V]` |
| C | depth-only pass + colour-less pipeline | pass builds and rasterises (occlusion 4096/4096) `[V]` |
| D | colour pass with `LoadActionDontCare` + `StoreActionDontCare` | rasterises and counts occlusion normally `[V]` |
| E | `float4` fragment output into `BGR5A1Unorm` | accepted; `0.25/0.5/0.75` packs to `0xa217`, i.e. 5/6/5 round-to-nearest `[V]` |
| F | `Depth32Float` sampled as `texture2d<float>` with a filter | accepted and runs `[V]` |
| G | texture view `Depth32Float → R32Float` / `Depth16Unorm → R16Unorm` | **rejected**: "source texture pixelFormat … not castable" `[V]` |
| H | `BGR5A1Unorm` sampled into a `BGRA8Unorm` target | accepted and runs `[V]` |
| I | a `B5G6R5Unorm` image holding guest words `001f 07e0 f800 1234`, sampled raw and through the `{One, Blue, Green, Red}` view | raw reads `001f` as blue, view reads it as red; raw→`B5G6R5` is bit-exact, view→`B5G6R5` rotates `[V]` |
| J | a pass into level 1 of a 2-level image, viewport 1×1 at (1,1), `LoadActionLoad` | wrote exactly that texel; the other three inside the render area and all of level 0 untouched `[V]` |

Consequences:

- A pass needs at least one attachment; a depth attachment is enough.  A
  colour-less pipeline must not meet a colour pass (B), but does meet a
  depth-only pass (C).  That is the shape of D2.
- A `DontCare` attachment does not disable rasterisation (D), so a scratch depth
  attachment costs a pass but loses nothing.
- Metal has no depth↔colour format view, so a depth-aspect transfer cannot be a
  GPU render-pass blit that reads or writes the depth plane under another
  format.  Depth transfers stay on the CPU path (G).
- 16-bit colour attachments and 16-bit sources are fine for the blitter (E, H).

---

## 2. D7 — `nv3089` transfers must run on the GPU

### 2.1 What the guest asks for

`nv3089` decodes a 2D engine transfer and, for a draw, calls
`renderer()->scaled_image_from_memory(src, dst, interpolate)`
(`NV47/HW/nv3089.cpp:594-620`) `[V]`.  The transfer can scale, flip, change
between `a8r8g8b8` and `r5g6b5`, target VRAM or main memory, and interpolate.

VK answers it with the shared `upload_scaled_image` through
`vk::texture_cache::blit` (`VK/VKTextureCache.cpp:1683-1699`) `[V]`, which resolves
both endpoints against the surface store, locks/protects the destination section,
commits the correct data, runs `blitter.scale_image`, and reports the touched
range so the caller can flush a likely cache miss `[V]`.

Metal answers `false` from `MTLGSRender::scaled_image_from_memory`
(`MTL/MTLGSRender.cpp:7855-7913`) after invalidating both guest ranges `[V]`.
`nv3089` then runs its own `SWScale`/`memcpy` fallback on guest memory `[V]`.
That fallback is the **only** Metal-only guest-memory writer in the default
configuration `[I]` (the texture cache writes guest memory too, but through the
same shared flush machinery VK uses).

### 2.2 Integration shape

`MTLTextureCache` already derives from the same shared template as VK
(`MTL/MTLTextureCache.h:25-40,267-271`) `[V]`, and implements every abstract hook
the template declares (it compiles) `[V]`.  So the correct integration is the one
VK uses, not a re-implementation:

```cpp
bool texture_cache::blit(const rsx::blit_src_info& src, const rsx::blit_dst_info& dst,
    bool interpolate, surface_cache& rtts, command_recorder& cmd)
{
    auto reply = upload_scaled_image(src, dst, interpolate, cmd, rtts, m_blitter);
    if (!reply.succeeded) return false;
    if (reply.dst_range.valid()) flush_if_cache_miss_likely(cmd, reply.dst_range);
    return true;
}
```

and `MTLGSRender::scaled_image_from_memory` becomes:

```cpp
if (m_frame_resources->textures.blit(src, dst, interpolate, m_frame_resources->cache,
        m_frame_resources->recorder))
{
    m_samplers_dirty.store(true);   // the transfer wrote a sampled surface
    return true;
}
return false;                        // CPU fallback, as today
```

The existing `invalidate_range` calls at `:7896-7910` are the CPU path's cache
coherence workaround and are dropped: the shared function performs the real
invalidation and section protection.  Keeping them would additionally evict the
source surface the GPU blit is about to read.

### 2.3 The decline gate

The shared function mutates before it blits: it invalidates the destination
range (`Common/texture_cache.h:3419-3424`), materialises CPU sections
(`:3560-3585`), and creates/mutates a destination surface (`:3636`, then
`create_blit_dst` at `:1667-1840`) `[V]`.  Returning `false` after that point
would leave the cache in a state the CPU fallback does not expect, so the
decision to use the CPU must be made **before** the first mutation.

There is exactly one place that is early enough: after the two `rtt_lookup`
calls and after the local decisions that feed it — `copy_to_main_memory`,
`use_null_region` and `get_optimal_blit_target_properties`, all pure reads of
the context (`Common/texture_cache.h:3402-3475`) `[V]`.  Both lookups are
read-only (`surface_store.h:1177-1188`, `get_surface_at`) `[V]`, so a predicate
there can decline without side effects.  The alternative — before the
`use_null_region` branch at `:3435` — cannot be used, because the gate's
`use_null_region` argument is only known after that block.

Add to the shared function, right after the two lookups and after
`get_optimal_blit_target_properties` (which is pure), behind a `requires`
expression so GL and VK are byte-for-byte unchanged — neither backend's blitter
has the member:

```cpp
// A backend whose host API cannot express this transfer runs the caller's CPU
// fallback instead; the predicate must not mutate anything.
if constexpr (requires (blitter_type& b) { b.supports_transfer(...); })
{
    if (!blitter.supports_transfer(src, dst,
        src_subres.surface ? src_subres.surface->get_bpp() : u8{ 0 },
        dst_subres.surface ? dst_subres.surface->get_bpp() : u8{ 0 },
        src_subres.is_depth, dst_subres.is_depth,
        is_copy_op, is_format_convert, is_graphics2d,
        src_is_tiled, dst_is_tiled, use_null_region))
    {
        return blit_op_result(false);
    }
}
```

The predicate receives only values already computed above it: the matched
surfaces' bpp (zero when that side is not a surface), the depth flags,
`is_copy_op`/`is_format_convert`/`is_graphics2d`, the tiled flags, and
`use_null_region`.  That last one can only be *cleared* further down
(resolution scaling, `:3547`) and never set, so the value at the gate is a safe
over-approximation: it can send to the CPU a case that would later have been
expressible, never keep on the GPU a case that later becomes a null region.

`blit_op_result{}` is `false` (`VK/VKTextureCache.cpp:1688-1698` relies on the same
convention) `[V]`.

`mtl::blitter::supports_transfer` declines when Metal cannot reproduce the
transfer *exactly*, using the arguments above rather than re-deriving them:

1. **Depth on either side** (`src_is_depth`, `dst_is_depth`, which the shared
   code takes from the matched surface).  Probe G shows no depth↔colour view
   exists.
2. **A surface whose storage granularity differs from the transfer's texel grid**
   (`src_surface_bpp != src.bpp`, or the same for the destination; a zero bpp
   means "not a surface" and skips the test).  The shared code resolves that
   difference by reinterpreting the surface as the transfer format
   (`configure_typeless_format`); a sampling pass would decode the wrong
   channels, and no host view changes a texel's size.
3. **Tiled memory** on either range (`src_is_tiled`, `dst_is_tiled`).  The surface
   store has no tiled allocation path on MTL.
4. **`use_null_region`.**  That branch is the DMA/guest-mirror path, which needs
   `cached_texture_section::dma_transfer`; MTL has no DMA section because
   `create_nul_section` creates a texture-less section
   (`MTL/MTLTextureCache.h:744-757`) and Metal 4 has no blit encoder `[V]`.  The
   CPU path is byte-exact here, so declining is a behaviour-preserving choice.

Everything else goes to the GPU: surface→surface, surface→texture with scaling,
flips and 16↔32-bit format conversion, and scaling/format-converting writes into
main memory (the shared code promotes those to a render target through
`create_blit_dst` → `create_surface_from_rsx_section`,
`surface_store.h:1132-1170`) `[V]`.

### 2.4 `mtl::blitter::scale_image`

New unit `MTL/MTLBlit.{h,cpp}`.  `blit_pass` owns one library and a pipeline
cache; `mtl::blitter` is the small value type the shared template and the texture
cache hold.  It follows `fill_pass` exactly for construction, library creation and
argument-table use (`MTL/MTLFill.cpp:87-130,146-227`) `[V]`.

```
blitter::scale_image(cmd, src, dst, src_area, dst_area, interpolate, xfer):
    if (!m_pass || !m_constants || !src || !dst) return;
    if (src->image() == dst->image()) { error; return; }          // read-write hazard
    fold xfer.flip_horizontal/flip_vertical into src_area         // signed extent
    validate the source rectangle and the destination subresource (below)
    values = m_constants->alloc_blit_values(u0, v0, du, dv)       // one 16-byte block
    m_pass->scale_image(cmd, values, src->get_view(default_remap_vector)->value(),
        dst->value(), dst_area, interpolate)

blit_pass::scale_image(cmd, values, src, dst, dst_area, interpolate,
    level, slice, depth_plane):
    if (!m_library || !src || !dst || !values || !cmd.blit_table()) return;
    if (src->sampleCount() != 1 || dst->sampleCount() != 1) { error; return; }
    if (src->textureType() != TextureType2D || src->arrayLength() != 1) { error; return; }
    if (is_depth_format(dst->pixelFormat())) { error; return; }          // probe G
    width/height = the far corner of dst_area
    MTL4::RenderPassDescriptor* pass = alloc/init
    pass->setRenderTargetWidth(width); pass->setRenderTargetHeight(height);
    pass->setDefaultRasterSampleCount(1);
    colour[0].texture = dst; colour[0].level/slice/depthPlane = the subresource;
    colour[0].loadAction  = LoadActionLoad;      // preserve texels outside dst_area
    colour[0].storeAction = StoreActionStore;
    encoder = cmd.open_pass(pass);               // closes the draw encoder + ALL→ALL barrier
    encoder->setViewport(dst_area); encoder->setScissorRect(dst_area);
    encoder->setRenderPipelineState(pipeline(dst->pixelFormat()));
    encoder->setDepthStencilState(depth-off); encoder->setCullMode(None);
    encoder->setTriangleFillMode(Fill);
    table->setAddress(values, 0); table->setTexture(src->gpuResourceID(), 0);
    table->setSamplerState(sampler(interpolate)->gpuResourceID(), 0);
    encoder->setArgumentTable(table, Vertex); encoder->setArgumentTable(table, Fragment);
    encoder->drawPrimitives(Triangle, 0, 3); cmd.close_encoder();
```

`blit_pass` takes only the destination rectangle: the source rectangle is
already the value block, so the pass itself never sees a texel-space source
area.

Validation lives in `blitter::scale_image`, before the value block is allocated,
because that is the only place the guest's subresource numbers are still
available.  It rejects, with a distinct error line each:

- a destination `level` beyond `mipmapLevelCount()`;
- a destination rectangle outside `width() >> level` / `height() >> level`
  (dump-level `shift` clamped to 31);
- an array `slice` beyond `arrayLength()`, or a volume `depthPlane` beyond
  `depth() >> level`;
- a source rectangle outside the source image (checked against the real extent, so
  a flipped rectangle is normalised first);
- an empty or reversed destination rectangle;
- an allocation failure from the blit-values ring.

A view of the destination image is not a separate resource, so
`src->image() == dst->image()` is rejected before the pass is built: Metal treats
the whole texture as one resource, and a pass that samples its own attachment has
an undefined read-write race.  The shared code has already committed to the GPU
route at that point, so the case is reported rather than encoded.

The multisample, texture-type and depth-format checks in `blit_pass` are
belt-and-braces: the upload gate declines depth and multisample resolves to a
single-sample image (`MTLSurface.cpp:37-47`) `[V]`, but
`copy_transfer_regions_impl` calls the pass directly and must not be able to
construct a pass the pipeline cannot serve.  A depth destination is rejected by
format (`is_depth_format`, MTLFormats.cpp) because a depth image has no colour
view (probe G).

The vertex stage emits one triangle over the clip square from `vertex_id`, so no
vertex buffer is bound; the fragment stage samples `texture2d<float>` with a
sampler and writes `float4`.  The source rectangle is one `constant` block
instead of `setBytes` (Metal 4 has none):

```
struct blit_values { float4 src_uv; };   // xy = normalised origin, zw = extent
corner  = float2((vertex_id << 1) & 2, vertex_id & 2);   // (0,0) (2,0) (0,2)
position = float4(corner * 2 - 1, 0, 1);
uv = src_uv.xy + float2(corner.x, 1 - corner.y) * src_uv.zw;
```

`src_uv.xy = src_area.origin / src_size` and `src_uv.zw = src_area.size /
src_size`, signed: flips are already folded into `src_area` by the shared
`blit_engine_helpers::apply_pixel_transforms`/`reproject_clip_offsets` before
the call (`Common/texture_cache_blit_helpers.cpp`) `[V]`, so a flipped source
produces a negative extent and the sampler mirrors without a shader branch.  The
v term is `1 - corner.y` because texture v grows downward and clip y upward.

**Channels.**  The copy moves guest channels, but a Metal image holds the guest
bits behind a host field order that differs for the packed formats
(`MTLFormats.cpp:667-698`) `[V]`.  The blitter therefore samples the
guest-channel view `src->get_view(rsx::default_remap_vector)`, the same view a
draw binds (`MTLGSRender.cpp:3662`) `[V]`, and leaves the destination raw:
Metal applies a swizzle on reads only (`MTLTexture.h:250`) `[V]`, and a surface's
raw fields already hold the guest channels because the surface plane is
permuted at the guest-memory boundary (`MTLFormats.cpp:410-433`) `[V]`.
Measured in `EXPERIMENT-blit-channel-order.mm` (probe I below): the raw sample
reads guest red `0x001f` as blue, the view reads it as red, and a raw same-format
resample is bit-exact.  The image the cache hands `copy_transfer_regions_impl`
is the one exception: it copies into a pooled temporary that inherits the
source's layout, so that path stays raw→raw and bit-exact.

Sampler: `min/mag = interpolate ? linear : nearest`,
`s/t address = clamp_to_edge` (a scaled copy must not blend in borders, and VK
does the same by never setting an address mode other than clamp) `[I]`.

Colour conversion: only colour-class destinations reach here (gate), so Metal's
own write-back conversion does the 5/6/5 quantisation (probe E) `[V]`.

Sample count: both images are single-sampled.  A transfer access resolves a
multisample surface (`MTLSurface.cpp:37-47`) `[V]`, so the pass needs no MSAA
state and the pipeline key is the destination's pixel format alone; the guard in
`blit_pass` exists so the texture-cache route cannot construct a pass the
single-sample pipeline cannot serve.

Ordering: `command_recorder::open_pass` closes the current encoder, which emits a
producer-scope `ALL→ALL` barrier (`MTL/MTLCommandRecorder.h:116-125`,
`MTL/MTLCommandRecorder.cpp:1051`) `[V]`, so the pass is ordered after the prior
draws that wrote the source.  A following draw finds
`frame.encoder != recorder.current_encoder()` and re-opens its pass
(`MTL/MTLGSRender.cpp:4583-4612`) `[V]`.

### 2.5 Ownership

`blit_pass` lives in `frame_resources` beside `fill_pass`
(`MTL/MTLGSRender.cpp:1390`) and is initialised next to it (`:1612`) `[V]`.
`texture_cache` holds a `mtl::blitter` set once at startup
(`textures.set_blitter(blitter{ &frame.blit })`), so `blit()` and
`copy_transfer_regions_impl()` share the same pipeline cache.

---

## 3. D5/D6 — `copy_transfer_regions_impl`

### 3.1 What it does today

The function copies host subresources into a pooled temporary so that the guest's
subresource view (cube face, 3D slice, mip chain, atlas) can be sampled.  It
opens a compute encoder and issues `copyFromTexture` per region
(`MTL/MTLTextureCache.cpp:934-1029`) `[V]`.  Two cases are dropped:

- `src_w != dst_w || src_h != dst_h` → "scaled texture copy is not supported"
  (`:960-964`) `[V]`.  Dropping it leaves the destination as the clear/poison
  colour: a wrong sample, not a crash `[I]`.
- different pixel formats with different texel sizes → "typeless texture copy …
  has no bit-preserving route" (`:978-986`) `[V]`.

Both cases are supported by VK: `coordinate_transform` recomputes the source
rectangle in the destination's texel space, a scaled copy goes through
`copy_scaled_image`, and the typeless case goes through a helper texture
(`VK/VKTextureCache.cpp:486-520,555-590`) `[V]`.

### 3.2 Fix

`copy_transfer_regions_impl` becomes:

1. Apply `rsx::surface_transform::coordinate_transform` exactly as VK does: scale
   `src_x`/`src_w` by `dst_bpp / src_bpp` and clear the bit
   (`VK/VKTextureCache.cpp:486-495`) `[V]`.  MTL ignores `xform` today `[V]`, so
   this is a correctness fix on its own.
2. If any other transform bit remains (swizzle, sign flip) → keep the current
   error line: the section's guest data is not in a sampleable form `[I]`.
3. If `src_format == dst_format` and the rectangle is scaled → one
   `m_blitter.scale_image(..., interpolate=false)` with the region's destination
   slice/level.  Nearest matches VK's `VK_FILTER_NEAREST`
   (`VK/VKTextureCache.cpp:589`) `[V]`.
4. If the formats differ and the texel sizes differ → staging bit-cast: copy the
   source rows into a shared buffer with an aligned `row_bytes`, then copy the
   buffer into the destination with `convert_w = src_w * src_bpp / dst_bpp` texels
   per row.  The bytes are preserved; only the texel granularity changes, which is
   what the guest asked for.  This replaces the current error `[I]`.
5. If the formats differ and the bit-cast does not describe the same bytes
   (`convert_w != dst_w` or `src_h != dst_h`) → keep the current error line: a
   resample across layouts is not a copy `[I]`.

`scale_image` needs the destination as an attachment; for a 3D destination the
slice is `dst_z` and for 2D array/cube the layer is `dst_z`.  The pass names the
subresource directly on the colour attachment (`setLevel`/`setSlice`/
`setDepthPlane`) and sets the viewport and scissor to the destination rectangle
`[V]`.  Probe J settles the last open question: a pass into level 1 of a 2-level
image, writing a 1×1 rectangle at (1,1) with `LoadActionLoad`, changed exactly
that texel and left the three texels the viewport did not cover, and all of level
0, untouched `[V]`.  The render area is the rectangle's far edge (2×2 here), so
this also proves the load/store cover the whole area, not just the viewport.
No texture view is needed, and no extra render-target array length is set (the
attachment's `slice` alone selects the layer).

### 3.3 Why this is still bounded

Neither skip fired in the Demon's Souls run that exposed the world collapse
(`build/probe/run-20261115-092255-ds12`: 0 occurrences of both messages) `[V]`.
The fix is therefore parity work with no known reproduction in hand: it must be
provably correct by construction, not by a title.  Every step above is either
identical to VK or a bit-preserving byte copy.

---

## 4. D1 — flat shading

### 4.1 Semantics

RSX flat shading takes the diffuse/specular colours of the **last** vertex of
each primitive.  The core sets `RSX_SHADER_CONTROL_FLAT_SHADING` only when the
backend claims `supports_last_provoking_vertex`
(`Core/RSXGraphEngine.cpp:1172-1177,1219-1223`) `[V]`, and the GLSL/MSL writers
then emit `flat` on `diff_color*`/`spec_color*`
(`Program/GLSLCommon.cpp:741-752`, `MTL/MTLVertexProgram.cpp:243-246`) `[V]`.
Metal declares `supports_last_provoking_vertex = false` today
(`MTL/MTLDevice.cpp:392`) `[V]`, so flat shading silently becomes smooth — a
visible colour error, not a crash.

Metal's `[[flat]]` interpolant is defined to come from the **first** vertex of
the primitive.  Claiming the RSX flag without reordering would therefore take the
colour from the wrong vertex.  KosmicKrisp solves it by unrolling every primitive
to a list and writing the provoking vertex first
(`.kk-mesa/src/kosmickrisp/vulkan/kk_cmd_draw.c:1822-1837,2065-2068` and
`libkk/kk_draws.cl:80-89`) `[V]`.

### 4.2 Rotation rule

For every emitted triangle `(a, b, c)` where `c` is the RSX provoking vertex,
emit `(c, a, b)`.  A cyclic rotation preserves the winding, so culling, depth and
coverage are unchanged and only the interpolant source moves.

Per primitive, the last vertex of each RSX triangle is:

| Primitive | Triangles (RSX order) | Provoking vertex | After rotation |
|---|---|---|---|
| triangle list | `(v0,v1,v2)`, `(v3,v4,v5)`, … | `v2, v5, …` | `(v2,v0,v1)`, `(v5,v3,v4)` |
| triangle strip | sliding window `(vi,vi+1,vi+2)` | `vi+2` | `(vi+2,vi,vi+1)` |
| triangle fan / polygon | `(v0,vi,vi+1)` | `vi+1` | `(vi+1,v0,vi)` |
| quads | two per quad, order is the expansion's | last emitted index of each triple | rotate each triple |
| lines / line strip / line list | `(vi,vi+1)` | `vi+1` | `(vi+1,vi)` |

The table is exactly "the last index of each emitted triple/pair".  So the
implementation is a post-pass over the expanded index array: rotate each
consecutive group of 3 (or 2 for lines) right by one.  This is independent of the
expansion's internal triangle order, which is why it composes with
`expand_indexed_quads`/`expand_indexed_triangle_fan` unchanged `[I]`.

Quads are the one topology a per-triangle host cannot make exact.  RSX's
provoking vertex for a quad is its fourth vertex `d`, and `d` is not a vertex of
the first emitted triangle `(a, b, c)`, so rotating that triple moves the
interpolant from `b` to `c` but cannot reach `d`.  Re-splitting the quad along
the other diagonal (`(d, a, b)` first) would make `d` the first vertex, but it
changes which pixels each half covers.  The chosen rule is therefore "rotate
each emitted triple": exact for lists, strips, fans and lines, and the closest a
triangle list can express for quads and quad strips (a quad whose four corners
carry different flat colours can differ on one of its two triangles).  The
approximation is documented rather than left implicit.

Edge cases:

- Primitive restart splits a strip into independent strips; the expansion must
  reset the sliding window at a restart index (the existing expansion helpers
  already handle restart, `Common/BufferUtils.cpp:930-961`) `[V]`.
- `polygon`/`quads`/`fan` are already expanded because Metal has no such
  primitives (`MTL/MTLGSRender.cpp:826-829`) `[V]`, so they only need the
  rotation bit.
- A 16-bit index array with restart disabled is widened today
  (`MTL/MTLGSRender.cpp:834-839,940-975`) `[V]`; under expansion the output is
  always a u32 list, so widening is skipped and the restart index is consumed by
  the expansion `[I]`.

### 4.3 Where the change goes

`Common/BufferUtils.{h,cpp}` gains one flag on the two writers:

```cpp
std::tuple<u32,u32,u32> write_index_array_data_to_buffer(..., bool provoking_vertex_last);
void write_index_array_for_non_indexed_non_native_primitive_to_buffer(..., bool provoking_vertex_last);
```

When `provoking_vertex_last` is set:

- `expands()` is forced true for every triangle and line primitive (the MTL
  caller passes the predicate, so the caller decides; the writers only honour the
  expansion request that is already there) `[I]`.
- the emitted groups are rotated as in §4.2.

MTL adds a `triangle_strip`/`line_strip` expansion for the flat case (the shared
writers have none today because strips are native everywhere else) `[V]`.

`MTLGSRender`:

```cpp
const bool flat = (ctx.program.vertex_program.ctrl & RSX_SHADER_CONTROL_FLAT_SHADING) != 0;
auto expands = [flat](rsx::primitive_type p) {
    return expands_primitive(p) || (flat && generates_flat_primitive(p));
};
```

and passes `flat` to the writers.  `MTLDevice.cpp` sets
`supports_last_provoking_vertex = true`.

Exhaustiveness is the safety property: the flag makes the *shader* flat, so any
primitive that reaches the encoder unrotated shows the wrong colour.  The gate
must therefore be `generates_flat_primitive(p)` = all triangle and line
primitives, and must cover the array, indexed, immediate and inline-array paths
(`MTL/MTLGSRender.cpp:868,928,1029`) `[V]`.

### 4.4 Alternative rejected

Rotating in the vertex shader (fetch the provoking vertex by index) needs the
guest index buffer in the shader and breaks instancing/streaming; it also cannot
express "last vertex of the primitive" without knowing the topology.  Index
rotation is the only local transform.

---

## 5. D2 — framebufferless rendering

### 5.1 Today

RSX can rasterise with no attachments, and a ZPASS counter still advances.  The
core lets such a draw through when `supports_framebufferless_rendering` is set
(`Core/RSXGraphEngine.cpp:501-513`) `[V]`, which MTL claims
(`MTL/MTLDevice.cpp:381`) `[V]`.  `should_skip_draw()` then skips it unless a
ZCULL query is active (`Core/RSXCoreRenderer.cpp:754-758`) `[V]`, so the only
draw that reaches the backend in this state is one whose occlusion result the
guest is waiting for.

The bind is *not* missing: `get_framebuffer_layout` sets `rtt_config_valid`
unconditionally once the layout is built (`Core/RSXGraphEngine.cpp:531-532`)
`[V]`, so `bind_framebuffer` reaches `on_framebuffer_bound` → `bind()`
(`Core/RSXGraphEngine.cpp:822-851`, `MTL/MTLGSRender.cpp:1828-1857`) `[V]`.
What fails is the backend's verdict: the store walk binds no surface, so
`framebuffer::valid()` is false and `open_pass` refuses
(`MTL/MTLFramebuffer.cpp:222-227`) `[V]`.  The draw is a NOP.

### 5.2 Fix: a depth-only scratch pass, backend only

No shared-engine change is needed, and none is made: the core already asks the
backend to bind.  `framebuffer` learns one more state:

- `bind()` sees `m_color_count == 0 && !m_depth` and calls
  `ensure_scratch_depth(cmd, rsx::get_format_sample_count(layout.aa_mode))`,
  which allocates (or reuses) a `Depth32Float` texture at the layout extent and
  sample count `[V]`.
- `valid()` also accepts the scratch image, so `open_pass` proceeds and
  `empty_pass()` names the state `[V]`.
- `open_pass` attaches the image with `LoadActionDontCare` /
  `StoreActionDontCare`.  Probe D shows a `DontCare` attachment still
  rasterises and still counts occlusion `[V]`, so the image costs a pass and
  never a fragment.
- The pass key carries the scratch image, so a resize or a sample-count change
  opens a new pass `[V]`.

Pipeline shape needs no change: `surface_state::color_attachment_count()` is the
popcount of `color_target_mask` (`MTL/MTLSurfaceState.cpp:18-23`), and
`capture_surface_state` derives that mask from
`get_mrt_buffers_count(layout.target) == 0` for `surface_target::none`
(`MTL/MTLSurfaceState.cpp:51`, `Common/surface_store.cpp:24-27`) `[V]`.  So the
PSO is built with no colour attachment, which is exactly the object probe C
proved a depth-only pass accepts `[V]`.

### 5.3 The depth test must be neutralised

A framebufferless draw has no depth buffer, so on the reference hardware and on
a backend whose pass has no depth attachment the depth test has nothing to test
and every fragment passes.  Our scratch image is *not* a depth buffer: its
content is undefined, so leaving the guest's `less`/`gequal` compare in place
would reject fragments at random depending on the garbage, and the occlusion
count — the only effect of the draw — would be wrong.

The draw path therefore replaces the latch for this pass
(`MTL/MTLGSRender.cpp:4671`):

```cpp
if (frame.attachments.empty_pass())
{
    encoder_state.depth_stencil = {};        // compare Always, no write, stencil Keep
    encoder_state.depth_bounds_min = 0.0f;   // an open bounds range
    encoder_state.depth_bounds_max = 1.0f;
}
```

`depth_stencil_state{}` is exactly that latch (`MTL/MTLDepthStencil.h:35-42`);
the compare and write are already how a disabled RSX depth test is
represented (`MTL/MTLDepthStencil.cpp:60-64`) `[V]`.  Culling, winding, fill
mode, viewport and scissor stay as the guest set them: the geometry still
rasterises, only the buffer tests are removed.

### 5.4 What this buys

The occlusion counter advances for a framebufferless draw, which removes the
per-draw "pass could not open" path and the log-spam half of the gap, and turns
the draw into the correct ZPASS contribution.  Draws still write nothing.

## 6. Order, verification and risks

| Stage | Change | Build/test gate |
|---|---|---|
| 1 | D2 scratch depth pass + neutralised depth test | `ninja rpcs3`, `rpcs3_test`, scrub check, one DS run for log noise |
| 2 | D7 blitter + shared gate + cache integration + cache invalidation removal | same, plus transfer counts in the probe log (`transfer/*`) |
| 3 | D5/D6 writers | same, plus the two error lines must be absent from a run that reaches them |
| 4 | D1 writers + flag | same, plus a flat-shaded title check (see below) |
| 5 | DS render check, then release refresh | visual + the existing capture tapes |

Residual risks, stated explicitly:

- **D7 section bookkeeping.**  The GPU path relies on the shared function's
  section locking; a wrong `dst_range` would leave guest memory stale.  The
  mitigation is that MTL uses the same shared code as VK, and the decline gate
  keeps every case the shared code cannot express on the CPU.
- **D7 destination attachment format.**  The destination is the surface's
  resolved image; a pass writing it must use the surface's own pixel format, not
  `get_sized_blit_format`'s.  A mismatch is caught by Metal validation, so the
  first run with `MTL_DEBUG_LAYER=1` decides it.
- **D5/D6 mip/layer copies.**  A scaled copy into a non-zero mip or layer names
  the subresource on the colour attachment, so no view is needed; probe J
  confirms `LoadActionLoad` preserves everything outside the render area.  A
  scaled copy of a *block-compressed* image stays on the error line (a block
  format is neither sampled as float nor a colour attachment), and so does a
  cross-format copy whose texel counts do not line up.  Both are strictly
  better than today's silent skip: the copy still does not run, but the
  difference between "not supported" and "silently wrote nothing" is now
  observable.
- **D1 quad exactness.**  A flat-shaded quad cannot carry its fourth corner's
  colour through a per-triangle rotation (§4.2); the result is the nearest
  expressible colour, not a crash or a wrong primitive.
- **D1 exhaustiveness.**  A primitive family left unrotated is a wrong flat
  colour, not a crash.  The rule "every triangle and line primitive expands under
  flat shading" is checked by a probe of the four draw visitors, and a title with
  flat-shaded geometry is the acceptance test.
- **D2 pipeline cache growth.**  A colour-less PSO per (blend, topology) is a
  small, bounded set; it is keyed like every other PSO.

---

## 7. Self-review against the code

| Claim | Check |
|---|---|
| shared `upload_scaled_image` is usable by MTL | MTL's cache derives from the same template and implements all pure virtuals; `blit` is the only missing call `[V]` |
| `rtt_lookup` is read-only | `surface_store::get_surface_at` only reads the two ranged maps `[V]` |
| the decline point precedes every mutation | the first mutation is `m_rtts.invalidate_range` in the `use_null_region` branch (`:3419-3424`); the gate sits above it `[V]` |
| a zero-attachment pass cannot rasterise | probe A `[V]` |
| a depth-only pass can | probe C `[V]` |
| a colour pass rejects a colour-less PSO | probe B `[V]` |
| `DontCare` does not suppress rasterisation | probe D `[V]` |
| depth↔colour views do not exist | probe G `[V]` |
| 16-bit colour blits are expressible | probes E, H `[V]` |
| MTL emits `flat` already | `MTLVertexProgram.cpp:243-246` `[V]` |
| Metal flat uses the first vertex | API contract; consistent with KosmicKrisp's unroll `[V]` |
| MTL's `copy_transfer_regions_impl` ignores `xform` | read in full, no `xform` use `[V]` |
| neither D5/D6 skip fires in the DS run | `grep` over the ds12 logs: 0 `[V]` |
| the blitter reads guest channels, not host fields | probe I: raw reads guest red as blue, the view reads it as red; raw same-format is bit-exact `[V]` |
| a scaled copy needs no texture view for a mip/layer | the pass takes `level`/`slice`/`depthPlane` on the colour attachment; probe J wrote level 1 directly and validation stayed quiet `[V]` |
| `LoadActionLoad` keeps texels outside the copied rectangle | probe J: a viewport 1×1 at (1,1) inside a 2×2 render area of level 1 wrote one texel, preserved the other three, and left level 0 untouched `[V]` |
| the gate cannot change GL or VK | it sits behind a `requires` expression on a member neither blitter has, so both templates compile the block away `[V]` |
| the gate's `use_null_region` value cannot under-approximate | the only later assignment is `use_null_region = false` (`Common/texture_cache.h:3547`); nothing sets it after the gate `[V]` |
| a pass never samples its own attachment | same-root images are declined before the pass is built (`blitter::scale_image`) `[V]` |
| a depth destination cannot reach the pass | refused by the upload gate and again by `is_depth_format` in `blit_pass` `[V]` |
| every subresource the pass names has been bounds-checked | level, derived mip extent, `slice` and `depthPlane` are checked in `blitter::scale_image` before the value block is allocated `[V]` |

Probe J closed the last open question; no question remains that needs a probe
before landing.

<!-- doc-end -->
