# LLD-vertex — Vertex cluster: attribute streams, transform constants, masks, VTF

Owner/Scope: RSX-first Metal 4 backend, vertex-cluster boundary | Status: draft | Date: 2026-10-16
Branch: `metal4`. Design only — nothing under `rpcs3/**` is modified.

**Legend.** `[V]` verified against a primary source in this repo; `[I]` inference from `[V]`
facts; `[U]` could not verify. `file:line` is advisory (the branch moves); **symbol names are the
stable reference**. Difficulty scale: **1** trivial rename · **2** mechanical · **3** real work,
well-trodden · **4** hard / needs design · **5** unknown.

---

## 1. Scope

The **vertex** cluster is the 30 registers at NV4097 addresses `0x0900`–`0x1ff8`
(`inventory/out/clusters/vertex.csv`). It is everything the guest writes to configure
**vertex-shader input**: the 16 attribute streams (format + offset), the register-file fallback
attributes, the transform-program upload channel and entry point, the transform-constant register
file and its two write cursors, the four vertex texture units (VTF), the attribute input/output
masks, the sRGB output packer, and the transform branch bits.

**In scope:** the 30 `M_NV4097_*` ids listed in §2.1.
**Out of scope (owned elsewhere):**
- `NV4097_SET_VERTEX_DATA_BASE_OFFSET` / `_BASE_INDEX` — **draw** cluster; treated as the
  attribute-ring base in this document only as a seam (`RSXDrawCommands.cpp:189`, §3.3). `[V]`
- Draw dispatch, index handling, `SET_BEGIN_END`, `SET_INDEX_ARRAY_*` — draw doc. `[V]`
- Vertex **program decompilation** (ucode→GLSL) — `Program/` + HLD-05. This document fixes only how
  the *upload channel* and *entry* map to a Metal 4 library. `[V]`
- Vertex **texture decode body** (format/deswizzle/upload) — scope 04; here only the VTF *state*
  and its bind target. `[V]`
- `NV4097_SET_FREQUENCY_DIVIDER_OPERATION` — **misc** cluster; it is the divisor half of the
  per-attribute frequency pair and is referenced, not owned, here (`RSXDrawCommands.cpp:62,122`). `[V]`

This document is the single owner of: the software-vertex-pulling decision (§3.1), the RSX-vertex
argument-table slots (§3.0), and the vertex-constant/instancing binding shape (§3.4, §3.6).

---

## 2. Register model

### 2.1 Master decode table (every id in `clusters/vertex.csv`)

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_VERTEX_TEXTURE_OFFSET` | 0x0900 | `texture_offset_helper` ×4 (stride 8) | bind_range x4 | reshape | `MTL::Texture` via texture cache; `setTexture(12+id)` |
| `M_NV4097_SET_VERTEX_TEXTURE_FORMAT` | 0x0904 | `texture_format_helper` ×4 | bind_range x4 | reshape | `MTLPixelFormat`+`MTLTextureType` cache entry |
| `M_NV4097_SET_VERTEX_TEXTURE_ADDRESS` | 0x0908 | `texture_address_helper` ×4 | bind_range x4 | reshape | `MTLSamplerState`; `setSamplerState(12+id)` |
| `M_NV4097_SET_VERTEX_TEXTURE_CONTROL0` | 0x090c | `texture_control0_helper` ×4 | bind_range x4 | reshape | `MTLSamplerDescriptor` (lod/aniso) |
| `M_NV4097_SET_VERTEX_TEXTURE_CONTROL3` | 0x0910 | `texture_control3_helper` ×4 | bind_range x4 | reshape | `MTLSamplerDescriptor` / cache gamma |
| `M_NV4097_SET_VERTEX_TEXTURE_FILTER` | 0x0914 | `texture_filter_helper` ×4 | bind_range x4 | reshape | `MTLSamplerDescriptor` min/mag/mip |
| `M_NV4097_SET_VERTEX_TEXTURE_IMAGE_RECT` | 0x0918 | `texture_image_rect_helper` ×4 | bind_range x4 | reshape | `MTLTexture` w/h (cache) |
| `M_NV4097_SET_VERTEX_TEXTURE_BORDER_COLOR` | 0x091c | `texture_border_color_helper` ×4 | bind_range x4 | reshape | `MTLSamplerDescriptor::borderColor` |
| `M_NV4097_SET_VERTEX_DATA_SCALED4S_M` | 0x0a80 | `register_vertex_helper<s16,4>` | bind_range x32 | emulate | transient `MTL::Buffer` @ slot 6 |
| `M_NV4097_SET_TRANSFORM_PROGRAM` | 0x0b80 | `transform_program_helper` ×32 | bind_array x32 | reshape | `MTL4::Library` + `LibraryFunctionDescriptor` |
| `M_NV4097_SET_VERTEX_DATA3F_M` | 0x1500 | `register_vertex_helper<f32,3>` | bind_range x64 | emulate | transient `MTL::Buffer` @ slot 6 |
| `M_NV4097_SET_VERTEX_DATA_ARRAY_OFFSET` | 0x1680 | `vertex_array_offset_helper` ×16 | bind_range x16 | reshape | `MTL::Buffer` window; `texture_buffer<uint>` @ slot 5 |
| `M_NV4097_SET_VERTEX_DATA_ARRAY_FORMAT` | 0x1740 | `vertex_array_helper` ×16 | bind_array x16 | reshape | stream descriptor @ slot 5; **no** `MTLVertexDescriptor` |
| `M_NV4097_SET_VERTEX_DATA2F_M` | 0x1880 | `register_vertex_helper<f32,2>` | bind_range x32 | emulate | transient `MTL::Buffer` @ slot 6 |
| `M_NV4097_SET_VERTEX_DATA2S_M` | 0x1900 | `register_vertex_helper<u16,2>` | bind_range x16 | emulate | transient `MTL::Buffer` @ slot 6 |
| `M_NV4097_SET_VERTEX_DATA4UB_M` | 0x1940 | `register_vertex_helper<u8,4>` | bind_range x16 | emulate | transient `MTL::Buffer` @ slot 6 |
| `M_NV4097_SET_VERTEX_DATA4S_M` | 0x1980 | `register_vertex_helper<u16,4>` | bind_range x32 | emulate | transient `MTL::Buffer` @ slot 6 |
| `M_NV4097_SET_VERTEX_DATA4F_M` | 0x1c00 | `register_vertex_helper<f32,4>` | bind_range x64 | emulate | transient `MTL::Buffer` @ slot 6 |
| `M_NV4097_SET_INDEXED_CONSTANT_READ_LIMITS` | 0x1d64 | — (no decoder) | unbound | na | none |
| `M_NV4097_SET_VERTEX_DATA1F_M` | 0x1e40 | `register_vertex_helper<f32,1>` | bind_range x16 | emulate | transient `MTL::Buffer` @ slot 6 |
| `M_NV4097_SET_TRANSFORM_EXECUTION_MODE` | 0x1e94 | — (no decoder) | unbound | na | none |
| `M_NV4097_SET_TRANSFORM_PROGRAM_LOAD` | 0x1e9c | `transform_program_load` | unbound | na | none (host write cursor) |
| `M_NV4097_SET_TRANSFORM_PROGRAM_START` | 0x1ea0 | `transform_program_start` | bind | reshape | `LibraryFunctionDescriptor::setName`; PSO key |
| `M_NV4097_SET_TRANSFORM_TIMEOUT` | 0x1ef8 | — (no decoder) | unbound | na | none |
| `M_NV4097_SET_TRANSFORM_CONSTANT_LOAD` | 0x1efc | `transform_constant_load` | bind | na | none (host write cursor) |
| `M_NV4097_SET_TRANSFORM_CONSTANT` | 0x1f00 | `transform_constant_helper` ×32 | bind_array x32 | reshape | `MTL4ArgumentTable::setAddress` @ slot 1 |
| `M_NV4097_SET_SHADER_PACKER` | 0x1fec | `srgb_output_enabled` | unbound | reshape | MSL fragment epilogue (sRGB) + linear attachment |
| `M_NV4097_SET_VERTEX_ATTRIB_INPUT_MASK` | 0x1ff0 | `mask()` (u16) | unbound | reshape | live-stream set @ slots 5/6 |
| `M_NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK` | 0x1ff4 | `output_mask()` (u32) | bind | reshape | MSL varyings `[[user(locnN)]]`; PSO key |
| `M_NV4097_SET_TRANSFORM_BRANCH_BITS` | 0x1ff8 | — (no decoder) | unbound | reshape | vertex context buffer @ slot 0 |

### 2.2 Decode

**Attribute stream format (`vertex_array_helper`, `rsx_decode.h:4427-4481`).** One `u32` per
attribute index 0–15, decoded into: `type_raw` (bits `[0,3)`) → `rsx::vertex_base_type`
(`s1` norm short, `f` float, `sf` half-float, `ub` norm ubyte, `s32k` unnormalised short, `cmp`,
`ub256`); `size` (bits `[4,8)`); `stride` (bits `[8,16)`); `frequency` (bits `[16,32)`). A stream is
**disabled** when `size==0` (`rsx_decode.h:4479`). The companion divisor — modulo vs divide, per
attribute — is `frequency_divider_operation_mask()` (`rsx_decode.h:548`; `rsx_methods.h:914`),
owned by the misc cluster. `[V]`

**Attribute stream offset (`vertex_array_offset_helper`, `rsx_decode.h:4488-4507`).** Raw `u32`; bit
31 is the memory location (local vs main), bits `[0,31)` the offset. `RSXDrawCommands.cpp:171` splits
it the same way. `[V]`

**Register-value attributes (`register_vertex_helper<type,N>`, `rsx_decode.h:4560-4616`).** The
`SET_VERTEX_DATA*_M` ranges are a per-attribute contiguous register file: for a `k`-component type of
`m` bytes, `increment_per_array_index = (k*m)/4` words, and `attribute_index = index /
increment_per_array_index`, `vertex_subreg = index % …` (`nv4097.h:105-132`). `SET_VERTEX_DATA3F_M`
consumes 4 words per attribute (the 4th is padding — `nv4097.h:166`, the R&C2 alignment quirk);
`4UB` and `1F` consume one. These feed `util::push_vertex_data` → `append_to_push_buffer`
(`RSXDrawCommands.cpp:238-250`), the register-file fallback used when a stream is disabled and for
inline/immediate vertex data. `[V]`

**Vertex textures.** Four units (`rsx::limits::vertex_textures_count = 4`, `rsx_utils.h:39`), each
eight consecutive FIFO words, hence the `stride 8`/`count 4` in the shard
(`rsx_methods.cpp:1684`; `RSXTexture.cpp:586-591`). Offsets/formats/rect are read via
`RSXTexture`'s VTF accessors; the border bit is inert on vertex textures
(`RSXTexture.cpp:490`). No `registers_decoder` struct exists for them (`has_decoder=0`). `[V]`

**Transform constants.** `NV4097_SET_TRANSFORM_CONSTANT` is a 32-register *write window* into a
512-`u32[4]` backing store (`rsx_methods.h:123`); only the first **468 vec4** are writable
(`nv4097.h:217`). The *word offset* is the sum of the register's own index and
`transform_constant_load*4` (`nv4097.cpp:32`), so a single `sync` of the window plus the load cursor
addresses word `[load*4, load*4+32)`. Writes are `u32`-swapped and compared
(`copy_data_swap_u32_cmp`, `nv4097.cpp:66,90`). `[V]`

**Write cursors and handshake registers.** `SET_TRANSFORM_PROGRAM_LOAD`,
`SET_TRANSFORM_CONSTANT_LOAD` are write cursors consumed only host-side
(`rsx_methods.h:1269,1277`). `SET_TRANSFORM_EXECUTION_MODE` and `SET_TRANSFORM_TIMEOUT` have
`nullptr` handlers (`rsx_methods.cpp:1484,1494`) and no decoder. `SET_INDEXED_CONSTANT_READ_LIMITS`
likewise has no reader (`rsx_methods.cpp:1464`) — see §6 Q1. `[V]`

**Masks.** `SET_VERTEX_ATTRIB_INPUT_MASK.mask()` returns the raw 16-bit mask
(`rsx_decode.h:507`); its dump names `in_pos…in_tc7` (`rsx_decode.h:516-523`).
`SET_VERTEX_ATTRIB_OUTPUT_MASK.output_mask()` is the 32-bit leaf-output mask
(`rsx_methods.h:919`). `SET_SHADER_PACKER.srgb_output_enabled()` is a single bool
(`rsx_decode.h:1718`). `SET_TRANSFORM_BRANCH_BITS` has no decoder; it is a raw `u32` read by the
decompiler (`rsx_methods.h:1282`) and by the interpreter (`VertexInterpreter.glsl:341`). `[V]`

### 2.3 Lifetime: value vs dirty-bit

Three distinct lifetimes, and the backend must honour all three:

1. **Pure value state** — array formats/offsets, masks, transform constants, VTF registers. Held in
   `rsx::method_registers` (`rsx_methods.h:123,144-145`); read at draw/compile time. `[V]`
2. **Write-cursor state** — the two `*_LOAD` registers and the branch bits: not GPU state, but they
   change *where* a later write lands or how a program decompiles. They must be captured/restored
   with the rest of `rsx_state` (the RRC `reg_state`), not dropped. `[V]`
3. **Dirty-bit state** — `transform_constant_load`/`vertex_array_offset` handlers route through
   `util::push_draw_parameter_change` (`nv4097.h:242`, `nv4097.cpp:639`), and
   `SET_TRANSFORM_PROGRAM_START` / `SET_VERTEX_ATTRIB_OUTPUT_MASK` set
   `vertex_program_ucode_dirty` / `vertex_program_state_dirty` when the value actually changes
   (`nv4097.cpp:444-458`). Transform-constant writes set `transform_constants_dirty` only on a real
   content change (`nv4097.cpp:83-94`). The VTF handlers set the per-unit
   `m_vertex_textures_dirty` (`nv4097.h:256-261`). `[V]`

The Metal backend does not own these bits: `rsx::thread` and the two caches own them. The LLD's
obligation is that every consumer in §3 keys off the same value (`rsx_state`) so a dirty-bit that is
*not* set cannot leave a stale Metal object bound. `[I]`

---

## 3. Metal 4 mapping

### 3.0 The vertex argument-table slot map

The RSX shader ABI is already fixed by `vkutils/pipeline_binding_table.h:9-20`
(`out/rsx_binding_slots.csv`). Under RSX-first this is the **vertex-stage `MTL4ArgumentTable`
index map** (HLD-05 §3.1); the vertex cluster owns the slots marked ★.

| slot | source | this cluster owns? | `MTL4::ArgumentTable` call |
|---|---|---|---|
| 0 | vertex context params | ★ (`SET_TRANSFORM_BRANCH_BITS`, user-clip, viewport-derived) | `setAddress(vs_context + off, 0)` |
| 1 | vertex constant buffers | ★ (`SET_TRANSFORM_CONSTANT`) | `setAddress(vertexConstants->gpuAddress(), 1)` |
| 5 | vertex buffers (persistent) | ★ (`ARRAY_OFFSET`/`FORMAT`) | `setTexture(view.gpuResourceID(), 5)` |
| 6 | vertex buffers (volatile) | ★ (`SET_VERTEX_DATA*_M`) | `setTexture(view.gpuResourceID(), 6)` |
| 7 | draw-parameters heap | (draw doc) | `setAddress(vertexLayoutStream->gpuAddress(), 7)` |
| 9 | rasteriser env (stipple) | no | `setAddress(…, 9)` |
| 10 | instancing LUT | ★ (instanced draws) | `setAddress(lut->gpuAddress(), 10)` |
| 11 | instancing constants | ★ (instanced draws) | `setAddress(array->gpuAddress(), 11)` |
| 12+id | vertex textures | ★ (`SET_VERTEX_TEXTURE_*`, id 0–3) | `setTexture(…, 12+id)` + `setSamplerState(…, 12+id)` |

Slot 6 is **not** in `pipeline_binding_table.h`; it is the volatile half of the two-stream split the
VK path already declares (`VKVertexProgram.cpp:162-184`: `persistent_input_stream` /
`volatile_input_stream`), promoted here to a fixed index by HLD-05 §3.2. `[V]`/`[I]`

### 3.1 Attribute format decode & the vertex-pulling decision

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_VERTEX_DATA_ARRAY_FORMAT` | 0x1740 | type/size/stride/frequency | bind_array x16 | reshape | stream descriptor @ slot 5 |
| `M_NV4097_SET_VERTEX_DATA_ARRAY_OFFSET` | 0x1680 | offset + location | bind_range x16 | reshape | `MTL::Buffer` window @ slot 5 |

**Decision: programmable vertex pulling; no `MTLVertexDescriptor`.** `[I]` The evidence:

- **RPCS3 already software-pulls.** `analyse_inputs_interleaved` (`RSXDrawCommands.cpp:16-191`)
  reconstructs interleaved blocks from `vertex_arrays_info`, computing base offsets, strides,
  per-attribute modulo/frequency and the real guest address; `RSXVertexFetch.glsl` is the puller.
  The VK backend declares **no** `VkVertexInputBinding` and binds the streams as
  `uniform usamplerBuffer` (`VKVertexProgram.cpp:162-184`). `[V]`
- **`MTLVertexDescriptor` cannot express the RSX model.** It has no *frequency/divisor-per-attribute*
  field of the RSX kind, no "attribute whose value comes from an immediate register", and no
  guest-address indirection. The interleave analysis above is richer than Metal's step-function/
  divisor model, and RSX's per-attribute `frequency`+divider pair (`RSXDrawCommands.cpp:62,122,161`)
  is not `MTLVertexBufferLayoutDescriptor`'s `stepRate`. `[V]`/`[I]`
- **Format gaps.** RSX carries `cmp` and `ub256` (`rsx_decode.h:4473-4474`) and non-4-byte-aligned
  strides; `MTLVertexFormat` has no direct analogue for either, and the puller sidesteps the question
  because it never asks Metal to fetch — it asks the shader to index a `texture_buffer`. `[I]`
- **The seam stays one place.** With system-memory pulling, the only Metal operation is "bind a
  buffer of the guest window and a descriptor of the layout"; the layout descriptor is host data
  (`RSXDrawCommands.cpp:16`), not an API object. That keeps H2 (decision-trace) localisation on the
  stream set rather than on an attribute-pointer table. `[I]`

**Shape change.** The `u32` format word and offset word become a host-side *stream descriptor*
(`{base_address, stride, memory_location, locations[{index, modulo, frequency}]}` — the existing
`interleaved_range_info`, `RSXDrawCommands.cpp:161-181`) that selects the pull coordinates. The
Metal object is the attribute-ring `MTL::Buffer` and its `texture_buffer<uint>` view
(`MTLTextureViewPool::setTextureViewFromBuffer`), not a vertex-input descriptor. `[V]`/`[I]`

### 3.2 Attribute input / output masks

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_VERTEX_ATTRIB_INPUT_MASK` | 0x1ff0 | `mask()` u16 | unbound | reshape | live-stream set @ slots 5/6 |
| `M_NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK` | 0x1ff4 | `output_mask()` u32 | bind | reshape | MSL varyings / PSO key |

**Input mask.** It is *not* the set of attributes to bind; it is ANDed with the program's
referenced-inputs mask to select the live set (`RSXDrawCommands.cpp:19`; also the capture path,
`rsx_capture.cpp:126,166`). Under Metal it is realised **twice**: (a) host-side, as the stream/block
set uploaded to slots 5/6 and the volatile blocks fed from the register file
(`RSXDrawCommands.cpp:92`); (b) shader-side, implicitly, because the decompiler only emits inputs the
program references. There is no standalone `MTL4` object — the disposition is `reshape` because the
need ("declare which attributes exist") survives but is expressed as a stream set rather than a
mask register. `[V]`/`[I]`

**Output mask.** Gates which leaf outputs the VS emits (`reg_table[]`, `VKVertexProgram.cpp:276-303`;
consumed at `RSXThread.cpp:2366`). It changes the emitted MSL and the varying struct, so it is part
of the **shader cache key** and, transitively, the PSO: two programs with equal ucode but different
output masks must not share an MSL. `[V]`

### 3.3 Register-value attributes (`SET_VERTEX_DATA*`)

| id | decode | disposition | metal4 target |
|---|---|---|---|
| `SET_VERTEX_DATA4UB_M` (0x1940) | `<u8,4>` | emulate | transient `MTL::Buffer` @ slot 6 |
| `SET_VERTEX_DATA1F_M` (0x1e40) | `<f32,1>` | emulate | transient `MTL::Buffer` @ slot 6 |
| `SET_VERTEX_DATA2F_M` (0x1880) | `<f32,2>` | emulate | transient `MTL::Buffer` @ slot 6 |
| `SET_VERTEX_DATA3F_M` (0x1500) | `<f32,3>` | emulate | transient `MTL::Buffer` @ slot 6 |
| `SET_VERTEX_DATA4F_M` (0x1c00) | `<f32,4>` | emulate | transient `MTL::Buffer` @ slot 6 |
| `SET_VERTEX_DATA2S_M` (0x1900) | `<u16,2>` | emulate | transient `MTL::Buffer` @ slot 6 |
| `SET_VERTEX_DATA4S_M` (0x1980) | `<u16,4>` | emulate | transient `MTL::Buffer` @ slot 6 |
| `SET_VERTEX_DATA_SCALED4S_M` (0x0a80) | `<s16,4>` | emulate | transient `MTL::Buffer` @ slot 6 |

These registers carry literal per-attribute values, not a stream. They are the fallback when a
stream is disabled (`size==0`) and the source for inline/immediate draws
(`RSXDrawCommands.cpp:95-115,238-250`). Realisation is host materialisation into the volatile
attribute buffer (the existing `m_vertex_push_buffers` path), bound at slot 6 and reached by
`[[vertex_id]]`. This is squarely `emulate`: there is no Metal construct that represents "an
attribute whose value is a register", and none should be invented. The byte swap already happens in
the handler (`nv4097.h:118-129`), so the buffer is written in guest-endian order once. `[V]`

### 3.4 Vertex constant buffers and array/default semantics

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_TRANSFORM_CONSTANT` | 0x1f00 | `transform_constant_helper` ×32 | bind_array x32 | reshape | `setAddress` @ slot 1 over `MTL::Buffer` |
| `M_NV4097_SET_TRANSFORM_CONSTANT_LOAD` | 0x1efc | `transform_constant_load` | bind | na | host write cursor |

The transform-constant file is **one flat `vec4[468]`** (`nv4097.h:217`), not a descriptor-set of
named uniforms. It is the RSX analogue of a UBO and maps to a single `MTL::Buffer` bound at slot 1
with `MTL4ArgumentTable::setAddress(buffer->gpuAddress(), 1)`
(`README.md` 03 #10; HLD-05 §3.2/§3.3). This is a `reshape`, not an `emulate`: Metal has a native
constant-buffer path and RSX has no push constants (the VK push-constant workaround
`VKGSRender.cpp:2429-2458` is deleted under RSX-first, `10` §4.2). `[V]`

**Array semantics.** Two modes exist, selected by `RSX_SHADER_CONTROL_INSTANCED_CONSTANTS`
(`is_current_vertex_program_instanced`, `RSXThread.h:456`):

- **Non-indexed (default):** only the program's `constant_ids` are uploaded, densely packed
  (`fill_vertex_program_constants_data`, `RSXDrawCommands.cpp:648-663`), and the shader's constant
  indices are remapped to the packed layout (`has_indexed_constants=false`). `[V]`
- **Indexed (instanced):** the whole 468-entry array is uploaded, plus an indirection table that the
  shader indexes with the instance id (`VKVertexProgram.cpp:186-241`;
  `#define _ENABLE_INSTANCED_CONSTANTS`, `RSXVertexPrologue.glsl:72`). `[V]`

**Default semantics.** Only 468 of the 512 backing `vec4` are loadable; reads beyond are hardware
garbage and RPCS3 bounds writes at `max_transform_constants` (`nv4097.cpp:27-39`). The buffer must
therefore be sized/validated to 468 vec4, exactly as VK does
(`ensure(… >= 468*16)`, `GLGSRender.cpp:1148`). `[V]`

### 3.5 Vertex textures (VTF)

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `SET_VERTEX_TEXTURE_OFFSET` | 0x0900 | addr+location | bind_range x4 | reshape | `MTL::Texture` (cache); `setTexture(12+id)` |
| `SET_VERTEX_TEXTURE_FORMAT` | 0x0904 | format/dim/cube | bind_range x4 | reshape | `MTLPixelFormat`+`MTLTextureType` |
| `SET_VERTEX_TEXTURE_ADDRESS` | 0x0908 | wrap modes | bind_range x4 | reshape | `MTLSamplerState`; `setSamplerState(12+id)` |
| `SET_VERTEX_TEXTURE_CONTROL0` | 0x090c | lod/aniso/bias | bind_range x4 | reshape | `MTLSamplerDescriptor` |
| `SET_VERTEX_TEXTURE_CONTROL3` | 0x0910 | gamma | bind_range x4 | reshape | `MTLSamplerDescriptor`/cache |
| `SET_VERTEX_TEXTURE_FILTER` | 0x0914 | min/mag/conv | bind_range x4 | reshape | `MTLSamplerDescriptor` |
| `SET_VERTEX_TEXTURE_IMAGE_RECT` | 0x0918 | w/h | bind_range x4 | reshape | `MTLTexture` w/h |
| `SET_VERTEX_TEXTURE_BORDER_COLOR` | 0x091c | RGBA | bind_range x4 | reshape | `MTLSamplerDescriptor::borderColor` |

VTF is the same sample model as the fragment texture path, bound in the **vertex** stage table at
slot `12+id` (`rsx_binding_slots.csv:13`; HLD-05 §3.1 note on the shared base 12). The 04 scope owns
resolution (guest addr→`MTL::Texture`, format table, deswizzle); this cluster owns only the *state*
and the *bind target*. Two RSX facts that shape it: the border bit is inert on vertex textures
(`RSXTexture.cpp:490`), and the sampler must supply a border colour even so (three fixed values,
`04` R18). `[V]`/`[I]`

Vertex texture samples in MSL are ordinary `texture{1d,2d,3d,cube}::sample`/`textureLod` calls
(`README.md` 03 #40); `texelFetch`/`Lod` comes from the GLSL `TXL` opcode (`RSXVertexProgram.h:66`).
No vertex-specific Metal object is needed beyond the stage argument table. `[V]`

### 3.6 Indexed constants for instancing

The vertex cluster produces the two instancing operand buffers; the draw cluster produces the
instance count via `pass_count()` → `drawPrimitives(..., instanceCount)` (`10` §4.5; `README.md` D2).
`[V]`

| slot | content | producer | disposition |
|---|---|---|---|
| 10 | indirection LUT (`u32[N]` per instance) | `fill_constants_instancing_buffer` | reshape |
| 11 | instanced constant array (`vec4[M]`) | `fill_constants_instancing_buffer` | reshape |

`fill_constants_instancing_buffer` (`RSXDrawCommands.cpp:743-850`) already builds both: an
indirection table (`instancing_indirection_table`, one entry per live constant, one copy per
`pass_count()`) and a `vec4` data array, with a bulk fast path and a sparse per-record path. The
shader reads `constants_addressing_lookup[[instance_id]]` and indexes the constant array
(`RSXVertexPrologue.glsl:72`, `VKVertexProgram.cpp:186-241`). `[V]`

Metal mapping: slot 10 → `setAddress(lut->gpuAddress(), 10)`; slot 11 →
`setAddress(array->gpuAddress(), 11)`. Both are plain `MTL::Buffer`s in a per-frame shared ring —
`setAddress` supports the pointer arithmetic the shader needs, so there is no descriptor-set
equivalent to reproduce. The `constant_ids`/`translate_constants_range` remap
(`RSXDrawCommands.cpp:798-800`) is host logic and survives unchanged. `[V]`/`[I]`

### 3.7 Transform-program upload and entry point

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `SET_TRANSFORM_PROGRAM` | 0x0b80 | 32-reg ucode window | bind_array x32 | reshape | `MTL4::Library` + `LibraryFunctionDescriptor` |
| `SET_TRANSFORM_PROGRAM_START` | 0x1ea0 | entry offset | bind | reshape | `LibraryFunctionDescriptor::setName`; PSO key |
| `SET_TRANSFORM_PROGRAM_LOAD` | 0x1e9c | write cursor | unbound | na | host cursor |
| `SET_TRANSFORM_EXECUTION_MODE` | 0x1e94 | — | unbound | na | none |
| `SET_TRANSFORM_TIMEOUT` | 0x1ef8 | — | unbound | na | none |
| `SET_TRANSFORM_BRANCH_BITS` | 0x1ff8 | raw u32 | unbound | reshape | vertex context buffer @ slot 0 |

The ucode window is an upload channel into the program cache, not GPU state: the decompiler turns it
into GLSL (`Program/`), glslang into SPIR-V, SPIRV-Cross into MSL, and `MTL4::Compiler::newLibrary`
into a `MTL4::Library` (HLD-05 §2.4). `SET_TRANSFORM_PROGRAM_START` selects the entry, so it is the
`MTL4::LibraryFunctionDescriptor::setName` argument and part of the shader/PSO key. Its handler only
marks `vertex_program_ucode_dirty` on change (`nv4097.cpp:444-450`). `[V]`

`SET_TRANSFORM_BRANCH_BITS` is *not* a Metal object: it is written into the VS context buffer at
offset 68 (`VKGSRender.cpp:2136`) and read by the decompiler as `transform_branch_bits`
(`VKVertexProgram.cpp:393`; `VertexProgramDecompiler.cpp:304`), i.e. it is baked into MSL conditional
branches. It is a field of slot 0. `[V]`

### 3.8 sRGB output packer

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_SHADER_PACKER` | 0x1fec | `srgb_output_enabled` | unbound | reshape | MSL fragment epilogue + linear attachment |

`framebuffer_srgb_enabled()` (`rsx_methods.h:1322`) sets `RSX_SHADER_CONTROL_SRGB_FRAMEBUFFER`
(`RSXThread.cpp:2467`), which the fragment epilogue lowers into an explicit sRGB encode in the
shader body (only when not 32-bit-exporting). It belongs to this LLD because it is adjacent to the
vertex-output/attachment contract, but its lowering is the **fragment** epilogue (HLD-05 §2.4
post-pass; `04` §2.3). The attachment must therefore stay a **linear** `MTLPixelFormat`; do not use
an `…_srgb` format, or the encode happens twice. `[V]`

---

## 4. Impossible / not-recommended

- **`MTLVertexDescriptor` for RSX attributes — not-recommended.** Expressing the 16 RSX streams as
  `MTLVertexBufferLayoutDescriptor`s would (a) discard the interleave/frequency analysis RPCS3
  already does, (b) hit `cmp`/`ub256`/unaligned-stride gaps (§3.1), and (c) split the "describe the
  stream" logic across host code and an API object that H2 cannot trace uniformly. Programmable
  vertex pulling is chosen; see §3.1. `[I]`
- **Vertex attribute values from registers — no native path; emulate.** §3.3. There is no Metal
  attribute source that is a register; do not try to model it as a 1-vertex buffer per attribute
  (it aliases with the volatile stream and complicates the vertex-id mapping). `[I]`
- **`MTLVertexFormat`-level `cmp` / `ub256`.** The RSX base types `cmp` and `ub256`
  (`rsx_decode.h:4473-4474`) have no matching `MTLVertexFormat`, and the VK path also cannot feed
  them as vertex formats. The puller decodes them in-shader; not a mapping failure because no fetch
  is asked of Metal. `[V]`/`[I]`
- **Indexed-constant read limits — no reader.** `SET_INDEXED_CONSTANT_READ_LIMITS` is unhandled
  (`rsx_methods.cpp:1464`); no disposition can be verified (§6 Q1). Not a Metal problem. `[U]`
- **`SET_TRANSFORM_EXECUTION_MODE` / `SET_TRANSFORM_TIMEOUT`.** Hardware execution-mode / watchdog
  registers with `nullptr` handlers; no GPU-visible effect in RPCS3's software-transform model.
  `na`. `[V]`

---

## 5. Invariants & failure modes

**Invariants.**
1. **Stream/format agreement.** For every attribute `i` with `input_mask & (1<<i)` set, either
   `vertex_arrays_info[i].size()>0` (a stream) or `register_vertex_info[i].size>0` (a register
   value) supplies it; otherwise the attribute is unwritten (`RSXDrawCommands.cpp:106-116`). `[V]`
2. **Constant-file bound.** Every VS constant index the MSL references resolves inside the bound
   `vec4[468]`; the buffer bound at slot 1 is ≥ the program's `constant_ids` extent (or the full 468
   when `has_indexed_constants`). `[V]`
3. **Instancing operand pairing.** Slots 10 and 11 are bound together or not at all; an instanced
   draw with one-but-not-the-other is a bug. `[I]`
4. **Mask agreement with the MSL.** The attributes the emitted VS reads are a subset of
   `input_mask`, and the varyings it writes match `output_mask`; both are in the shader cache key. `[V]`
5. **Attachment/encode agreement.** If `SHADER_PACKER.srgb_output_enabled()`, the fragment epilogue
   encodes sRGB and the attachment is a linear format (never `…_srgb`). `[V]`
6. **Endianness once.** Register-value and constant bytes are swapped exactly at the handler
   (`nv4097.h:118-129`; `copy_data_swap_u32`, `nv4097.cpp:66`); the buffer writer must not swap
   again. `[V]`

**Failure modes.**

| # | Symptom | Cause | Detection / mitigation |
|---|---|---|---|
| F1 | Wrong vertex values | stream base/stride/frequency mismatch between `analyse_inputs_interleaved` and the pull descriptor | H2 decision trace on the stream set; the descriptor is one record (§3.1) |
| F2 | Garbled interleaved attributes | `frequency`/divider misread (modulo vs divide) | reuse `frequency_divider_operation_mask` (`RSXDrawCommands.cpp:122`); H0 unit test on the mask |
| F3 | Stale constants after a partial write | `transform_constants_dirty` not raised on an equal-value write | `copy_data_swap_u32_cmp` already guards (`nv4097.cpp:90`); H2 records the upload |
| F4 | Instanced draw reads another instance's constants | indirection LUT size ≠ `pass_count()` or stale array | §3.6 invariant 3; H2 records both addresses + lengths |
| F5 | Double sRGB (washed-out / crushed output) | `…_srgb` attachment plus shader encode | invariant 5; assert the chosen `MTLPixelFormat` is linear |
| F6 | VTF samples the wrong unit | per-unit dirty bit not propagated to the vertex table | `m_vertex_textures_dirty` (`RSXThread.h:250`); H2 binding trace |
| F7 | PSO/MSL reuse across differing masks | `output_mask`/`input_mask`/branch-bits omitted from the shader key | invariant 4; fold all three into the shader key (HLD-05 §3.5) |
| F8 | Vertex streams wider than the texel-buffer limit | `texture_buffer` default width 4096 | HLD-05 F4/Q6 — 2D tiling or an SSBO path; open |

---

## 6. Open questions

- **Q1 — What does `SET_INDEXED_CONSTANT_READ_LIMITS` do?** No reader in the tree
  (`rsx_methods.cpp:1464`); plausibly bounds the indexed-constant load region for hardware-transform
  programs RPCS3 never runs. Confirm on hardware before leaving it `na`. `[U]`
- **Q2 — Volatile stream index.** HLD-05 §3.2 assigns slot 6 to the volatile stream; confirm no
  collision with a later fixed-map extension, since `pipeline_binding_table.h` stops at 5. `[I]`
- **Q3 — Vertex-stream `texture_buffer` width/alignment** (HLD-05 Q6): RSX streams can exceed the
  4096-texel default; a raw SSBO path would change `RSXVertexFetch.glsl`. Decide before wiring the
  attribute ring. `[I]`/`[U]`
- **Q4 — One attribute buffer per draw or one shared ring?** The register-value and interleaved
  paths both target slot(s) 5/6; whether they can share one allocation per draw affects the H2
  record cardinality and the batcher. `[I]`
- **Q5 — Sampling a vertex texture in a `[[vertex_id]]`-indexed pull:** does MSL allow a texture
  sample with an implicit-LOD coordinate in the vertex stage under the pinned MSL version? (`README`
  03 #40 says yes; version is [U].) `[U]`
- **Q6 — Border colour for VTF.** Vertex border is inert (`RSXTexture.cpp:490`) but the sampler still
  needs a border value; pick the RSX-correct default (transparent/zero) and record it as a shader
  key input. `[I]`
- **Q7 — Constant-buffer remapping when `has_indexed_constants` toggles mid-frame:** does the
  shader cache distinguish the two layouts beyond `RSX_SHADER_CONTROL_INSTANCED_CONSTANTS`?
  (`VKVertexProgram.cpp:186-241`.) `[I]`

---

## 7. Provenance

**In-repo — decoders & handlers** `[V]`
- `rpcs3/Emu/RSX/rsx_decode.h:4427-4486` (`vertex_array_helper`), `:4488-4513`
  (`vertex_array_offset_helper`), `:496-535` (input mask), `:537-575` (frequency divider),
  `:1707-1728` (shader packer), `:1824-1844` (program start), `:4107-4127` (constant load),
  `:4299-4336` (program helper / program load), `:4580-4616` (register-vertex decoders).
- `rpcs3/Emu/RSX/NV47/HW/nv4097.h:105-132` (`set_vertex_data_impl`), `:134-205` (register-vertex
  structs), `:207-244` (transform constant / offset), `:255-262` (vertex-texture dirty bit).
- `rpcs3/Emu/RSX/NV47/HW/nv4097.cpp:24-130` (transform constant write path), `:444-458`
  (program-start / output-mask dirty bits), `:637-640` (constant-load handler).
- `rpcs3/Emu/RSX/rsx_methods.h:104-158` (state arrays; `transform_constants` `:123`), `:909-927`
  (masks/shader-control accessors), `:972-996` (base offset / program start), `:1269-1284`
  (program load / branch bits), `:1322-1325` (sRGB).
- `rpcs3/Emu/RSX/rsx_methods.cpp:139-140,358` (defaults), `:1411-1506` (method table, unbound ids),
  `:1598-1711` (bind/bind_array/bind_range registrations).

**In-repo — software pulling & consumers** `[V]`
- `rpcs3/Emu/RSX/Core/RSXDrawCommands.cpp:16-191` (`analyse_inputs_interleaved`), `:62,122,161`
  (frequency/divisor), `:238-250` (`append_to_push_buffer`), `:648-663`
  (`fill_vertex_program_constants_data`), `:743-850` (`fill_constants_instancing_buffer`).
- `rpcs3/Emu/RSX/RSXThread.cpp:2337-2366` (VS program + output mask), `:2467`
  (`RSX_SHADER_CONTROL_SRGB_FRAMEBUFFER`); `RSXThread.h:250,262` (VTF dirty/sampler), `:456`
  (instanced).
- `rpcs3/Emu/RSX/RSXTexture.cpp:490,586-591` (VTF accessors); `rsx_utils.h:39`
  (`vertex_textures_count=4`); `Capture/rsx_capture.cpp:126,166` (input mask in capture).
- `rpcs3/Emu/RSX/Program/VertexProgramDecompiler.cpp:304` (branch bits);
  `Program/GLSLSnippets/RSXProg/RSXVertexPrologue.glsl:72`; `Program/GLSLInterpreter/VertexInterpreter.glsl:341`.
- `rpcs3/Emu/RSX/VK/VKVertexProgram.cpp:162-184,186-241,276-303,393`;
  `VK/VKGSRender.cpp:2136,2318,2370`; `VK/vkutils/pipeline_binding_table.h:9-20`;
  `GL/GLGSRender.cpp:1148`.

**Inventory (generated, in-repo)** `[V]`
- `docs/research/metal4/inventory/out/clusters/vertex.csv` (30 ids);
  `out/rsx_binding_slots.csv:2-13`; `out/SUMMARY.md:360-393` (vertex cluster);
  `inventory/SCHEMA.md` §4 (disposition vocabulary), §6 (LLD template).

**Sibling research docs (in-repo)** `[V]`
- `docs/research/metal4/10-rsx-native-metal4-architecture.md` §4.2 (binding/argument table), §4.6
  (shader/pipeline), §5 (delete/reshape); `README.md` §3.4 (#83 `usamplerBuffer`, #87 push
  constants), D2/D11 (instancing, draw-parameters); `03-shaders-pipelines-descriptors.md` #12/#40;
  `04-images-formats-samplers-rtt.md` §2.3, R18, R19; `design/HLD-05-shader-binding-contract.md`
  §2.4/§3.1-§3.5/§5 (F4/F5).

<!-- doc-end -->
