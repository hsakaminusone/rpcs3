# LLD — Shader interpreter and draw fallback

Status: implemented and verified (2026-10-21; see §21). This document closes `METAL4-TODO.md` F5
and the remaining gap in `HLD-07` §4.3. The body below is the design; §21 records what the
implementation confirmed, the corrections the compiler and the tests forced, and the optimisations.

Scope: the Metal 4 backend (`rpcs3/Emu/RSX/MTL`). The design uses Metal 4 APIs only. Vulkan and
OpenGL are the behavioural reference. KosmicKrisp is implementation evidence for the Metal 4
argument-table and compiler APIs.

---

## 1. The result

The backend gets a generic RSX shader interpreter. The interpreter executes the guest vertex and
fragment ucode in a Metal render pipeline. The backend uses it when a guest pipeline is not ready.

The default mode (`shader_mode::async_with_interpreter`) then never skips a draw for a pending
shader. The draw renders with the interpreter until the real pipeline is ready. The next draw uses
the real pipeline.

The design has five decisions:

| # | decision | reason |
|---|---|---|
| D1 | Reuse the shared GLSL interpreter (`Program/GLSLInterpreter/*.glsl`) through the existing GLSL → SPIR-V → MSL route. Do not write a second interpreter in MSL. | One source of truth with Vulkan and OpenGL. The existing cross-compiler already handles the RSX ABI. |
| D2 | Map the 16 texture units to **compact per-type sampler arrays**, not four arrays of 16. | MSL has a hard limit of 16 sampler arguments per stage. The Vulkan form needs 64. |
| D3 | Use a separate pair of `MTL4::ArgumentTable` objects for interpreter draws. | Interpreter texture slots and sampler slots must not disturb the regular binder's cached state. |
| D4 | Link interpreter pipelines through the existing serialized link worker's synchronous path and compile mutex. | L18 forbids the Metal 4 completion-handler link path while the pipeline data set serializer is attached. A synchronous first-use link is the only way to guarantee no skipped draw; priority does not bound the wait. |
| D5 | Build the interpreter pipeline synchronously on first use when it is not prebuilt, and fall back to a synchronous real-pipeline build if the interpreter cannot represent the draw. | The draw must not be skipped. A bounded one-time link is better than missing geometry. |

The interpreter does not replace the real pipeline path. It is a fallback with a bounded lifetime
per draw. It is not a startup precompile and it has no progress bar.

---

## 2. The defect

The current draw path asks the shared program cache for a pipeline. On a miss the cache submits an
asynchronous job and returns null. `MTLGSRender` then calls `execute_nop_draw()`
(`MTLGSRender.cpp`, the `draw/skipped pipeline pending` branch near the pipeline lookup).

The result is a missing draw for every frame between the first use of a program and the end of its
compile. On a transition this is visible as missing geometry, flicker, or a black region. The
guest does not know that the host skipped the draw. The guest continues to present the frame.

Vulkan and OpenGL do not have this gap:

- `VKGSRender.cpp` selects the interpreter when the real pipeline is null and the shader mode
  allows it. It also reuploads the transform and fragment constants when it swaps between the
  interpreter and the recompiler.
- `GLShaderInterpreter.cpp` builds interpreter variants and uses the same swap rules.

Metal reads no `shadermode` value today. `METAL4-TODO.md` F5 records that gap.

The fix must also preserve the modes that do **not** want the interpreter:

| `shader_mode` | real pipeline on miss | interpreter | skip on miss |
|---|---|---|---|
| `recompiler` | synchronous link | no | never |
| `async_recompiler` | asynchronous link | no | until ready; this is the mode's contract |
| `async_with_interpreter` | asynchronous link | yes | never |
| `interpreter_only` | not built | yes | never |

`async_recompiler` keeps the current skip. The change must not turn that mode into a different
mode in silence.

---

## 3. Constraints

### 3.1 MSL sampler limit

MSL allows 16 sampler arguments per stage. `MTLArgumentBinds.h` already asserts this limit:

```
constexpr u32 argument_sampler_count = 16;
static_assert(argument_sampler_count <= 16, ...);
```

The Vulkan interpreter declares four arrays of 16 combined samplers:

```
sampler1D_array[16], sampler2D_array[16], sampler3D_array[16], samplerCube_array[16]
```

That is 64 sampler declarations. Vulkan accepts it. MSL does not. A direct port of the Vulkan
declaration to Metal fails at library creation.

### 3.2 Texture limit

MSL allows 128 texture arguments per stage. The current argument table reserves indices 12..50.
The interpreter needs at most 16 texture units, so the texture space is not a constraint.

### 3.3 Metal 4 pipeline link

Metal 4's asynchronous `newRenderPipelineState` completion-handler path corrupts
`MTL4::PipelineDataSetSerializer` and crashes in `MTLHashKey::getHashTools` (L18). The backend
already serializes all links through one worker. Interpreter links must use the same worker and the
synchronous API. They must not use the completion-handler form.

### 3.4 Argument table shape

The backend already has a fixed RSX argument-table map (`MTLArgumentBinds.h`):

| space | indices | use |
|---|---|---|
| buffer | 0,1,2,3,4,5,6,7,8,9,10,11,28,29,30 | context, constants, streams, draw params, state, stipple, instancing |
| texture | 12..27 | 16 fragment texture units |
| texture | 30..50 | stencil mirror, subpass, depth input |
| sampler | 0..15 | one sampler per unit |

The interpreter can reuse the buffer slots and the 12..27 / 0..15 range. It needs one new buffer
slot for its instruction block. Buffer slot 12 is unused by the regular map. Metal's buffer and
texture index spaces are separate, so buffer 12 and texture 12 do not collide **in MSL**. GLSL /
SPIR-V descriptor sets are not separate spaces: a storage buffer at `set=1, binding=12` and a
texture at `set=1, binding=12` are the same descriptor binding, and the module is invalid. The FS
instruction block therefore lives at `set=0, binding=12` (the FS buffers already use set 0); the VS
instruction block also uses `set=0, binding=12`. `spv_to_msl` ignores the set for the MSL index and
maps both to `[[buffer(12)]]`.

### 3.5 Vertex textures

The Vulkan interpreter does not implement vertex texture fetch; it logs a TODO. The Metal design
does the same, but it does not use the interpreter for a vertex program that references a vertex
texture. It builds the real pipeline synchronously instead. This is a correctness improvement over
the Vulkan TODO and it keeps the no-skip rule.

---

## 4. Architecture

```
guest RSX ucode
      |
      +-- real path:  GLSL -> SPIR-V -> MSL -> MTL::Library -> MTL::RenderPipelineState
      |
      +-- fallback:   interpreter GLSL (same shared bodies)
                      -> SPIR-V -> MSL -> MTL::Library -> MTL::RenderPipelineState
                      -> interpreter argument tables + instruction buffers
```

The interpreter has three layers:

| layer | object | key | lifetime |
|---|---|---|---|
| L1 shader library | `MTL::Library` for one VS or FS variant | compiler options + texture mapping | process |
| L2 pipeline | `MTL::RenderPipelineState` | L1 keys + `pipeline_properties` | process, LRU-bounded |
| L3 per-draw | instruction blocks, headers, argument tables | guest program + draw state | frame slot |

L1 and L2 never enter the guest shader disk cache. They are generic backend objects. L3 is rebuilt
when the guest program changes.

### 4.1 New objects

```
mtl::interpreter_mapping
    The canonical unit -> (type, compact index) map for one fragment texture state.

mtl::interpreter_library
    One MTL::Library, one entry point name, and the mapping that the source baked in.

mtl::interpreter_pipeline
    One MTL::RenderPipelineState plus shared references to its two libraries.

mtl::interpreter_cache
    Owns L1 and L2. Thread-safe. The RSX thread reads, inserts and (through the
    synchronous link path) publishes entries; the link worker may also publish a
    prebuilt library. Neither holds the map mutex across a compiler call.

mtl::interpreter_state
    Per-draw instruction blocks, headers, resident mask and dirty bits.
```

### 4.2 New argument tables

`command_recorder::frame_slot` gains two tables and a second binder:

```
MTL4::ArgumentTable* interpreter_vertex_table;
MTL4::ArgumentTable* interpreter_fragment_table;
argument_binder interpreter_binder;
```

The descriptors use the existing counts: 31 buffers, 51 textures, 16 samplers,
`initializeBindings = true`. The tables are separate from `vertex_table` and `fragment_table`
because the regular binder caches per-slot state. A separate table makes the interpreter bind
stream independent and removes the need to invalidate the regular binder. The second binder is
what makes the tables independent: a single binder's cache is per stage, not per table, so it would
skip one of the two tables (`MTLArgumentBinds.cpp`, `bind`).

### 4.3 New buffer slot

```
constexpr u32 interpreter_instruction_slot = 12;
```

The VS and FS each bind their instruction block at buffer index 12 in their own stage table. The
block is a `device` buffer, bound with `MTL4::ArgumentTable::setAddress`.

---

## 5. The compact texture mapping

This is the central design change. It is the part that makes the shared GLSL interpreter fit MSL.

### 5.1 Principle

Each RSX texture unit has exactly one dimension. A draw references a subset of the 16 units. The
interpreter therefore needs one sampler per **referenced unit**, not one sampler per unit per
dimension.

The interpreter declares one combined-sampler array per dimension type. The array size is the
number of referenced units of that type. A unit maps to an element in exactly one array. The total
number of sampler elements is the number of referenced units, which is at most 16.

Example: units 0 and 2 are 2D, unit 3 is cube.

```
sampler2D    sampler2D_array[2];     // units 0, 2 -> elements 0, 1
samplerCube  samplerCube_array[1];   // unit 3    -> element 0

unit_index_2d[16]   = {0, 0, 1, 0, ...};
unit_index_cube[16] = {0, 0, 0, 0, ...}; // only unit 3 is used
```

Total sampler declarations: 3. Total texture declarations: 3. Both are inside the MSL limits.

### 5.2 Single-type fast path

When every referenced unit has the same dimension, the mapping uses the **unit index** as the
array index and declares a 16-element array of that type:

```
sampler2D sampler2D_array[16];
unit_index_2d[unit] = unit;
```

Only the referenced units are bound. The shader never samples the others. This variant is keyed
only by the dimension type, not by the referenced mask. It therefore matches every 2D-only draw,
which is the common case. The backend prebuilds the four single-type variants (1D, 2D, 3D, cube).

### 5.3 Mixed-type path

When the referenced units have more than one dimension, the mapping is compact per type:

```
count[t] = number of referenced units of type t
base[0] = 0
base[1] = base[0] + count[0]
base[2] = base[1] + count[1]
base[3] = base[2] + count[2]
next[t] = base[t]
for unit in 0..15:
    if referenced(unit):
        t = dimension(unit)
        index[unit] = next[t]++
    else:
        index[unit] = 0
```

The FS shader declares, for each type with `count[t] > 0`:

```
layout(set=1, binding=12 + base[t]) uniform samplerTYPE samplerTYPE_array[count[t]];
```

The combined-sampler binding is `12 + base[t]`. The existing `spv_to_msl` rule maps a combined
sampler at binding `b` to `msl_texture = b` and `msl_sampler = b - 12`
(`texture_sampler_index`). The element `i` of the array therefore lands at:

| space | index |
|---|---|
| texture | `12 + base[t] + i` |
| sampler | `base[t] + i` |

The highest texture index is `12 + referenced_count - 1 <= 27`. The highest sampler index is
`referenced_count - 1 <= 15`. Both are inside the existing tables.

### 5.4 The shader macros

The shared interpreter calls `SAMPLER1D(unit)`, `SAMPLER2D(unit)`, `SAMPLER3D(unit)` and
`SAMPLERCUBE(unit)`. The Metal fragment builder defines them as:

```
#define SAMPLER1D(index)   sampler1D_array[unit_index_1d[index]]
#define SAMPLER2D(index)   sampler2D_array[unit_index_2d[index]]
#define SAMPLER3D(index)   sampler3D_array[unit_index_3d[index]]
#define SAMPLERCUBE(index) samplerCube_array[unit_index_cube[index]]
```

`unit_index_*` is a `const int[16]` in the generated GLSL. It is a constant array, so the
indexing is a normal dynamic array read. The MSL specification permits `operator[]` on a sampler
array with a runtime `size_t` (`Metal Shading Language Specification`, §2.12.1.1).

Only a type with `count[t] > 0` gets an array, a `unit_index_t[16]` constant and a `SAMPLERt`
macro. A type with no referenced unit is compiled out (§5.5), so its macro must not be referenced
by a compiled case.

The single-type fast path declares `samplerTYPE_array[16]` and sets `unit_index_TYPE[unit] = unit`.
It emits only that type's macro. The other three branches are compiled out by §5.5.

### 5.5 Empty types

If a dimension type has no referenced unit, the builder does not declare its array. The shared
`_texture` and `_textureLod` switch statements must not compile the dead case. The shared
`FragmentInterpreter.glsl` gains guards:

```
#ifndef MTL_INTERPRETER_NO_1D
case RSX_SAMPLE_TEXTURE_1D: ... break;
#endif
```

The Metal builder defines `MTL_INTERPRETER_NO_1D` (and the other three) when the count is zero.
Vulkan and OpenGL do not define these macros, so their builds keep every case. The change has no
effect on the other backends.

The `unit_index_*` arrays are still emitted with zeroes. They are not referenced by a compiled
case. Each switch must still have a `default: return vr_zero;` (or an equivalent `break` with
`vr0 = vec4(0.)`) because an unreferenced unit can carry stale dimension bits, and an omitted case
would otherwise fall through with an undefined result.

The guard must wrap the whole `case` label and body, not only the body: a label left in the switch
would still reference the undeclared sampler. The same guards apply to `_texture` and
`_textureLod`.

The same pattern trims the fragment outputs for a draw with fewer than four attachments:

```
#ifndef MTL_INTERPRETER_NO_OCOL1
layout(location=1) out vec4 ocol1;
#else
vec4 ocol1;
#endif
```

`ocol0` is always exported; `ocol1..3` become plain locals when their attachment is absent. This is
required for `interpreter_only`, where no real-program validation is available to mask the extras
(§6.4).

### 5.6 Key

```
struct interpreter_texture_key
{
    u16 referenced_mask;   // fp_metadata.referenced_textures_mask, zero for single_type
    u32 dimensions;        // current_fragment_program.texture_state.texture_dimensions
    u8  single_type;       // 0 = mixed, 1..4 = the one type
    bool operator==(...) const;
};
```

Canonicalisation:

- Mask the dimensions to the referenced units.
- If all referenced units have the same type, set `single_type`, store the type, and **clear
  `referenced_mask` and `dimensions`**. The shader's `unit_index` is the unit index for all 16
  slots and the resident mask travels in the per-draw header, so the shader does not depend on
  which units are live. Without this clearing the key would still vary per mask and the four
  prebuilt variants would not be reused.
- Otherwise store the masked `referenced_mask` and the masked `dimensions`, and set
  `single_type = 0`.
- Set every unreferenced unit's dimension bits to zero.

A change to an unreferenced unit's dimension does not change the key. A change to a referenced
unit's dimension does. The binder takes the live mask from the draw, not from the key.

---

## 6. Shader generation

### 6.1 Vertex shader

The builder mirrors `VKShaderInterpreter::build_vs` and reuses the Metal decompiler's declarations:

```
RSXVertexProgram null_prog;
null_prog.ctrl = instancing ? RSX_SHADER_CONTROL_INSTANCED_CONSTANTS : 0;
MTLVertexDecompilerThread comp(null_prog, shader_str);

std::stringstream b;
comp.emit_interpreter_header(b);             // public wrapper for insertHeader
comp.emit_interpreter_constants(b, { vec4 vc[468] });   // public wrapper: full RSX constant file
comp.emit_interpreter_inputs(b, {});          // public wrapper: persistent / volatile usamplerBuffer streams

b << "layout(location=16) out flat uvec4 draw_params_payload;\n\n";
b << "#define xform_constants_offset get_draw_params().xform_constants_offset\n";
b << "#define scale_offset_mat get_vertex_context().scale_offset_mat\n";
... point_size, z_near, z_far ...

b << "layout(std430, set=0, binding=12) readonly restrict buffer VertexInstructionBlock\n"
     "{\n"
     "    uint base_address;\n"
     "    uint entry;\n"
     "    uint output_mask;\n"
     "    uint control;\n"
     "    uvec4 vp_instructions[];\n"
     "};\n\n";

if (instancing) b << "#define _ENABLE_INSTANCED_CONSTANTS\n";

glsl::insert_glsl_legacy_function(b, properties);
glsl::insert_vertex_input_fetch(b, glsl::glsl_rules_vulkan);
comp.emit_fs_payload(b);                     // public wrapper: write_fs_payload()
b << program_common::interpreter::get_vertex_interpreter();
```

The source of truth is the same shared body that Vulkan uses. The Metal-specific declarations are
the vertex streams and the constants block. The VS output `dest[16]` occupies user locations
0..15. `draw_params_payload` occupies location 16. The FS reads the same two ranges.

The public wrapper sets `properties.has_indexed_constants = true` before `insertConstants`, as
Vulkan's builder does (`VKShaderInterpreter.cpp:187-193`), so `vc[]` is declared as an indexable
array. The MTL constructor is `(const RSXVertexProgram&, std::string&)`, not Vulkan's four-argument
form; do not copy the Vulkan construction line.

The vertex program options are `COMPILER_OPT_ENABLE_INSTANCING` and (if it ever lands)
`COMPILER_OPT_ENABLE_VTX_TEXTURES`. Vertex textures are rejected before the interpreter is chosen.

The vertex shader uses the full 468-vector constants file so an instruction operand can index any
`vc[]` register. This is confirmed by the current Metal path: `constants_heap` allocates
`alloc_transform_constants()` as `vertex_constant_count` (468) `vec4` blocks and
`argument_binds.vertex_constants` is `constants_heap::vertex_constants_address()`, the base of the
whole block. The regular path optionally compacts the block with the vertex program's relocation
table. The interpreter must therefore leave `frame.compiled_vertex` null for its draws, so
`fill_vertex_program_constants_data` receives an empty relocation table and writes the full file.
When the next regular draw follows, `transform_constants_valid` must be cleared or that draw will
reuse the interpreter's full block against a compacted index map (§8.4).

### 6.2 Fragment shader

The builder mirrors `VKShaderInterpreter::build_fs`:

```
#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_uniform_buffer_unsized_array : require
#extension GL_ARB_separate_shader_objects : enable

RSXFragmentProgram null_frag;
null_frag.ctrl |= RSX_SHADER_CONTROL_INTERPRETER_MODEL;
MTLFragmentDecompilerThread comp(null_frag, len, shader_str);

glsl::insert_subheader_block(b);
comp.emit_interpreter_constants(b);          // public wrapper for insertConstants; emits _fs_* and texture_base_index

b << "layout(location=16) in flat uvec4 draw_params_payload;\n\n";
b << "#define fog_param0 fs_contexts[_fs_context_offset].fog_param0\n";
... fog_mode, wpos_scale, wpos_bias ...

if (alpha_test_ge) b << "#define ALPHA_TEST_GEQUAL\n";
... other alpha test defines ...

if (!f32_export) b << "#define WITH_HALF_OUTPUT_REGISTER\n";
if (depth_export) b << "#define WITH_DEPTH_EXPORT\n";
if (flow_control) b << "#define WITH_FLOW_CTRL\n";
if (packing) b << "#define WITH_PACKING\n";
if (kil) b << "#define WITH_KIL\n";
if (stippling) b << "#define WITH_STIPPLING\n";

if (textures)
{
    b << "#define WITH_TEXTURES\n\n";
    b << "#define MTL_INTERPRETER_NO_1D\n";   // only when count[0] == 0
    ...
    b << "layout(set=1, binding=" << 12 + base[0] << ") uniform sampler1D sampler1D_array[" << count[0] << "];\n";
    ... four declarations, only for count[t] > 0 ...
    b << "const int unit_index_1d[16] = { ... };\n";
    ...
    b << "#undef TEX_PARAM\n";                  // insertConstants defines it; the interpreter does not use it
    b << "#define textures_resident reserved1\n";
    b << "#define IS_TEXTURE_RESIDENT(index) (((textures_resident) >> (index)) & 1u)\n";
    b << "#define SAMPLER1D(index) sampler1D_array[unit_index_1d[index]]\n";
    ...
    // insertConstants already defines _fs_constants_offset, _fs_context_offset,
    // _fs_texture_base_index, _fs_stipple_pattern_array_offset and texture_base_index.
    // Do not redefine them.
}

b << "layout(std430, set=0, binding=12) readonly restrict buffer FragmentInstructionBlock\n"
     "{\n"
     "    uint shader_control;\n"
     "    uint texture_control;\n"
     "    uint reserved1;          // textures_resident\n"
     "    uint reserved2;\n"
     "    uvec4 fp_instructions[];\n"
     "};\n\n";

b << "uint rop_control = fs_contexts[_fs_context_offset].rop_control;\n";
b << "float alpha_ref = fs_contexts[_fs_context_offset].alpha_ref;\n\n";

glsl::insert_glsl_legacy_function(b, properties);
b << program_common::interpreter::get_fragment_interpreter();
```

`textures_resident` is an alias for `reserved1`; the block has no field named `textures_resident`.
The alias must be emitted before the shared body, or the macro must use `reserved1` directly. The
`IS_TEXTURE_RESIDENT` macro makes a non-resident unit return zero instead of sampling an unbound
texture. The Vulkan interpreter returns `true` for every unit. The Metal value is the referenced
mask intersected with the units that have a valid cache binding. This is a deliberate, safer
divergence.

`MTLFragmentDecompilerThread::insertConstants` already emits the four `_fs_*` macros, the
`texture_base_index` macro, and a `TEX_PARAM` macro. The builder must not redefine
`texture_base_index`, and must `#undef TEX_PARAM` only if it needs a different definition. The
shared interpreter reads `texture_parameters[...]` directly, so it does not need `TEX_PARAM` at
all. The `INTERPRETER_MODEL` control bit is what suppresses `insertConstants`' own
`draw_params_payload` declaration; without it the builder would declare location 16 twice.

### 6.3 Cross-compilation

No new MSL translator is needed. The generated GLSL goes through:

```
spirv::compile_glsl_to_spv(spv, glsl, domain, glsl_rules_vulkan)
spv_to_msl(spv, is_vertex, msl, &color_output_mask)
```

The existing `spv_to_msl` binding rule is exactly what the compact map needs:

- combined sampler at binding `12 + base[t]` -> `[[texture(12 + base[t])]]` and
  `[[sampler(base[t])]]`;
- storage buffer at binding 12 -> `[[buffer(12)]]`;
- texture-buffer stream at binding 5 or 6 -> `[[texture(5)]]` / `[[texture(6)]]` with no sampler.

No `spv_to_msl` change is required for the mapping. The only shared-source change is the
`MTL_INTERPRETER_NO_*` guards.

The shader library keeps MSL 2.3, which is the current setting. Arrays of samplers require MSL 2.0
or later, so the version is sufficient.

### 6.4 Color outputs

The interpreter FS declares `ocol0..3` at locations 0..3, as the shared body does. The number of
exported outputs must be `min(4, current_fragment_program.mrt_buffers_count)`, not
unconditionally four. The builder emits `MTL_INTERPRETER_NO_OCOL1/2/3` for the higher outputs and
the shared body declares them as plain locals (§5.5).

This is required, not conditional on the target compiler:

- `interpreter_only` never calls `get_graphics_pipeline`, so `validate_pipeline_properties` has not
  masked the write masks. The draw path must mask `properties.write_mask[i]` for
  `i >= mrt_buffers_count` to `ColorWriteMaskNone` itself.
- The fallback path does call `get_graphics_pipeline`, and its validation masks the write masks by
  the real fragment program's live outputs. The interpreter can then export an output whose write
  mask is `None`; Metal ignores it. The trim still avoids declaring an output for an attachment
  that does not exist.
- A four-output fragment function linked against a one-attachment pipeline is not a specified
  Metal case. The standalone probe is a check, not the design's correctness argument.

`spv_to_msl` reports the output mask; the pipeline validation uses it for the real program. The
interpreter's own output mask is the trimmed set and is not fed back into `properties`, because
`properties` belongs to the guest draw state.

---

## 7. Metal 4 objects and the binding map

### 7.1 Argument-table layout

Both interpreter tables use the same descriptor shape as the regular tables. The fields are:

| space | index | binding | stage |
|---|---|---|---|
| buffer | 0 | vertex context | VS |
| buffer | 1 | full vertex constants `vc[468]` | VS |
| buffer | 2 | fragment constants (only if the generated MSL reads it) | FS |
| buffer | 3 | fragment state / `fs_contexts` | FS |
| buffer | 4 | fragment texture parameters | FS |
| buffer | 5 | persistent vertex stream (texture buffer) | VS |
| buffer | 6 | volatile vertex stream (texture buffer) | VS |
| buffer | 7 | draw parameters | both |
| buffer | 8 | conditional-render predicate | VS |
| buffer | 9 | polygon stipple | FS |
| buffer | 10 | instancing lookup | VS |
| buffer | 11 | instancing constants | VS |
| buffer | 12 | **interpreter instruction block** | both |
| texture | 12..27 | compact texture elements | FS |
| sampler | 0..15 | compact sampler elements | FS |

The descriptor asks for `maxBufferBindCount = 31`, `maxTextureBindCount = 51`,
`maxSamplerStateBindCount = 16`. These are the existing `argument_buffer_count`,
`argument_texture_count` and `argument_sampler_count`.

The table's indices are MSL indices. In GLSL the instruction block is a `set=0, binding=12`
storage buffer; the texture array starts at `set=1`. Metal's buffer and texture spaces are
separate, so MSL buffer 12 and texture 12 coexist. The CPU generator test asserts the GLSL set,
because the collision in R1 is a GLSL/SPIR-V error, not an MSL one.

The tables are created with `MTL::Device::newArgumentTable` and
`MTL4::ArgumentTableDescriptor`. `setInitializeBindings(true)` keeps an unbound slot
deterministic, as the regular tables already do.

### 7.2 Binding calls

For each interpreter draw:

```
encoder->setArgumentTable(table, stage);
table->setAddress(vertex_context, 0);
table->setAddress(vertex_constants, 1);
table->setAddress(fragment_state, 3);
table->setAddress(fragment_texture_params, 4);
table->setTexture(persistent_stream, 5);
table->setTexture(volatile_stream, 6);
table->setAddress(draw_parameters, 7);
table->setAddress(interpreter_instruction, 12);
table->setAddress(vertex_draw_params, 28);       // push-constant substitute

if (conditional_render) table->setAddress(predicate, 8);
if (stippling)          table->setAddress(stipple, 9);
if (instancing)
{
    table->setAddress(instancing_lookup, 10);
    table->setAddress(instancing_constants, 11);
}

for unit in referenced units:
    t = dimension(unit);
    slot = base[t] + index[t][unit];
    table->setTexture(texture_bindings[unit].texture, 12 + slot);
    table->setSamplerState(texture_bindings[unit].sampler, slot);
```

The `vertex_draw_params` address is easy to miss: `MTLVertexDecompilerThread::insertHeader` declares a
`push_constants_block` and `spv_to_msl` maps it to `vertex_draw_params_slot` (28), while
`MTLArgumentBinds` binds `binds.vertex_draw_params` there. Without it the VS's
`draw_parameters_offset` is uninitialised or zero and every context lookup is wrong. The same table
must bind the stipple slot for a stippled FS and the instancing slots for an instanced VS; the
interpreter does not read the fragment blend/stencil/depth-input slots because those features are
rejected (§11).

The `texture_bindings` array is filled by the texture cache in unit order. The cache already
returns `{texture ResourceID, sampler ResourceID}` per unit. The interpreter binder asks for the
referenced units and places them at `base[type] + index`, not at the loop counter. The shader's
`unit_index_TYPE[]` constant and the binder must use the same mapping function; a shared helper
returns the compact slot for a unit. `texture_parameters` stays indexed by the RSX unit, because
`texture_base_index + unit` is what the shared body reads.

The regular table and the interpreter table are not enough by themselves. `argument_binder` caches
the last-bound `argument_binds` per stage, so one binder instance would consider a regular table
bind already applied and skip the interpreter table (or the reverse). `frame_resources` therefore
gains a second `argument_binder` (`interpreter_binder`), reset at frame begin alongside the
regular one, and the interpreter draw calls `bind_interpreter(...)` through it. The instruction
address also needs two fields, `vertex_interpreter_instruction` and
`fragment_interpreter_instruction`, because one `argument_binds` object is consumed by both stage
binds and a single field would send the VS block to the FS stage (or the reverse).

The texture cache's existing null handling (B3/B4) is reused. A unit with no valid texture gets a
null resource ID and its resident bit is cleared.

### 7.3 Pipeline creation

The interpreter pipeline uses the existing `apply_pipeline_properties` helper with the current
`pipeline_properties`, so color formats, blend, depth/stencil, topology, rasterization, sample
count and write masks match the draw exactly. Only the two function descriptors differ:

```
MTL4::LibraryFunctionDescriptor* vs_fn = ...;
vs_fn->setLibrary(vertex_library->library);
vs_fn->setName(NS::String::string("main0", NS::UTF8StringEncoding));

MTL4::LibraryFunctionDescriptor* fs_fn = ...;
fs_fn->setLibrary(fragment_library->library);
fs_fn->setName(NS::String::string("main0", NS::UTF8StringEncoding));

descriptor->setVertexFunctionDescriptor(vs_fn);
descriptor->setFragmentFunctionDescriptor(fs_fn);
```

The library entry name comes from `spv_to_msl` (the existing code uses `main0`). The pipeline has
no vertex descriptor because the VS fetches vertices from the texture-buffer streams
(`HLD-05` §3.5).

The link call is the synchronous `newRenderPipelineState` on the link worker. It must not use the
completion-handler form (L18).

### 7.4 Library creation

`MTL4::Compiler::newLibrary` with `MTL4::LibraryDescriptor` and the generated MSL. The compiler
options match the regular path: fast math unless `disable_msl_fast_math` is set. Every compile is
inside an `NS::AutoreleasePool`, as `MTLShader.cpp` already does.

The library is owned by `interpreter_library`. The pipeline holds a `std::shared_ptr` to both
libraries, so a pipeline cannot outlive its code.

---

## 8. Per-draw data

### 8.1 Vertex instruction block

Layout, 16-byte aligned and 256-byte aligned start:

| offset | field | value |
|---|---|---|
| 0 | `base_address` | `current_vertex_program.base_address` |
| 4 | `entry` | `current_vertex_program.entry` |
| 8 | `output_mask` | `current_vertex_program.output_mask` |
| 12 | `control` | `two_side_light_en() ? 1 : 0` |
| 16 | `vp_instructions[]` | `current_vertex_program.data`, `ucode_length` bytes |

The block is allocated from a per-frame `byte_ring` with 256-byte alignment. The address is bound
at buffer 12 of the VS interpreter table.

### 8.2 Fragment instruction block

| offset | field | value |
|---|---|---|
| 0 | `shader_control` | `ctx->shader_control()` |
| 4 | `texture_control` | `current_fragment_program.texture_state.texture_dimensions` |
| 8 | `reserved1` | resident mask (`referenced & valid`) |
| 12 | `reserved2` | 0 |
| 16 | `fp_instructions[]` | `current_fragment_program.get_data()`, `ucode_length` bytes |

The block is allocated the same way and bound at buffer 12 of the FS interpreter table.

### 8.3 Upload triggers

The interpreter state tracks:

```
enum interpreter_dirty : u32
{
    interpreter_vertex_ucode   = 1 << 0,
    interpreter_fragment_ucode = 1 << 1,
    interpreter_header         = 1 << 2,
};
```

- `vertex_ucode` is set when the guest vertex program changes or the interpreter is entered.
- `fragment_ucode` is set when the guest fragment program changes or the interpreter is entered.
- `header` is set when the texture dimension mask or the resident mask changes.

The program identity is the current program object/hash that the draw path already uses. The
interpreter does not re-read the ucode every draw. A program change allocates a new block. A
header-only change allocates a new FS block (the header is part of the same buffer).

The ring is per frame slot and the recorder resets it at `begin_frame`. A slot is reused every
`frame_slot_count` frames, so a block is valid for one frame only, not "until a program changes".
The dirty bits must be forced at frame begin: the first interpreter draw of every frame uploads
both blocks even when the guest programs and the resident mask are unchanged. The "does not
re-read the ucode every draw" property is intra-frame only. A header-only resident-mask change
still allocates a new FS block because the 16-byte header shares the ucode buffer; splitting the
header into its own 16-byte buffer slot is the measured optimisation, not the first
implementation.

`byte_ring::alloc` aligns to 4 bytes. The argument table's `setAddress` path and the SPIR-V
`std430` block need at least the buffer's natural alignment; the implementation adds
`alloc_aligned(size, 256)` (or over-allocates and advances the address) and records the alignment
in the block. Do not rely on the default allocator's padding.

### 8.4 Constants and context

The interpreter uses the same per-frame context and constant buffers as the regular path:

- VS: `vertex_context`, full `vertex_constants`, `draw_parameters`, optional predicate and
  instancing buffers.
- FS: `fragment_state`, `fragment_texture_params`, optional stipple, optional fragment constants.

The Metal path does not have Vulkan's global `transform_constants_dirty` /
`fragment_constants_dirty` bits. It has `frame.transform_constants_valid` and the shared
`rsx::pipeline_state::transform_constants_dirty` flag. The interpreter integration is:

- set `frame.compiled_vertex = nullptr` for an interpreter draw. `emit_geometry` then passes an
  empty relocation table (`MTLGSRender.cpp:2579-2583`), `fill_vertex_program_constants_data`
  writes the full 468-vector file, and
  `fill_constants_instancing_buffer(..., nullptr)` performs a full re-upload of the instance
  tables. Both null paths already exist in current code.
- set `frame.compiled_fragment = nullptr`. The fragment constant path already guards it
  (`fragment_constant_count = frame.compiled_fragment ? ... : 0`,
  `MTLGSRender.cpp:2616`), so `params->fs_constants_offset` stays 0 and no relocation happens.
  There is no unguarded dereference in `emit_geometry`; the guard is a correctness requirement,
  not a new crash fix.
- set `frame.transform_constants_valid = false` whenever the draw changes between interpreter and
  regular, or the compiled vertex program changes. The flag caches "the block at
  `transform_constants_index` is the full/compacted file this draw needs"; reusing a regular
  compacted block for an interpreter draw (or the reverse) sends wrong `vc[]` values. Clearing
  the flag also invalidates the index, so the next draw allocates and rewrites.

The regular uploader consumes these fields directly; there is no separate interpreter constant
uploader.

---

## 9. Cache and compilation policy

### 9.1 Keys

```
struct interpreter_vertex_key
{
    u64 options;   // COMPILER_OPT_ENABLE_INSTANCING, and future VS bits
};

struct interpreter_fragment_key
{
    u64 options;             // FS mix-and-match bits + alpha test
    interpreter_texture_key textures;
};

struct interpreter_pipeline_key
{
    interpreter_vertex_key vertex;
    interpreter_fragment_key fragment;
    pipeline_properties properties;
};
```

`pipeline_properties` already has equality and a hash in `MTLProgramPipeline.h`. The interpreter
cache uses the same hash/equality helpers.

The library cache keys are `interpreter_vertex_key` and `interpreter_fragment_key`. The pipeline
cache key adds `pipeline_properties`.

### 9.2 Selection

The draw path computes the fragment options from the same RSX state as Vulkan:

```
alpha test -> COMPILER_OPT_ENABLE_ALPHA_TEST_*
depth export -> COMPILER_OPT_ENABLE_DEPTH_EXPORT
32-bit exports -> COMPILER_OPT_ENABLE_F32_EXPORT
uses KIL -> COMPILER_OPT_ENABLE_KIL
ROP remap -> COMPILER_OPT_ENABLE_ROP_REMAP
referenced textures -> COMPILER_OPT_ENABLE_TEXTURES
branch -> COMPILER_OPT_ENABLE_FLOW_CTRL
pack -> COMPILER_OPT_ENABLE_PACKING
polygon stipple -> COMPILER_OPT_ENABLE_STIPPLING
```

The VS options are `COMPILER_OPT_ENABLE_INSTANCING` when the guest uses instanced constants. A VP
with `referenced_textures_mask != 0` is rejected (see §11).

### 9.3 Prebuild

At backend init, after the `MTL4::Compiler` exists, build the base shader sources (GLSL → SPIR-V →
MSL) on the init thread and compile their libraries with the existing `compile_msl_async` path.
The link worker does not compile libraries; it only links pipelines with
`newRenderPipelineState`. Prebuild:

- VS: instancing off, instancing on.
- FS: no textures; textures with the single-type identity mapping for each of 1D, 2D, 3D and cube;
  common alpha-test/depth-export combinations as the metrics justify.

The library builds are asynchronous and have no progress dialog. They are not a startup precompile
of guest shaders. The first draw that needs a missing variant waits for the async library compile,
then links on the worker. A prebuild variant that has finished its library compile is still not a
pipeline; the pipeline link is per `pipeline_properties` and happens on first use.

### 9.4 First-use link

When the RSX thread needs an interpreter library or pipeline that is not in the cache:

1. Reserve a `compiling` entry, so a recursive/re-entrant path does not start a duplicate.
2. If the library is a prebuild that is still compiling, wait for its completion (bounded). If it
   was not prebuilt, compile it with `compile_msl` synchronously on the RSX thread inside an
   autorelease pool.
3. Link the pipeline with `pipeline_jobs::run_link_sync`, which takes the link worker's compile
   mutex. The call can wait for an in-flight guest link, **which is not preemptible**. There is no
   priority queue that reorders a link once it has started. On a cold transition the guest link
   can take hundreds of milliseconds, so the RSX thread can stall for that long even though the
   interpreter source is ready. This is a confirmed performance risk, not a correctness problem,
   and it is the main reason to measure the wait before extending the synchronization design.
4. Publish the ready library/pipeline before `run_link_sync` returns; there is no condition
   variable in the synchronous path. Use the condition variable only for the async prebuild
   library.
5. On failure, store the error, publish `failed`, and use the synchronous real-pipeline fallback
   for this draw.

This is the only place the RSX thread waits for an interpreter compile. The wait is a correctness
fallback, not the normal path. The probe records the wait time. If the wait is visible, the
escape hatches are, in order: prebuild the measured variants; give the interpreter a second
`MTL4::Compiler` with no `PipelineDataSetSerializer` (the L18 corruption is serializer-specific),
so interpreter links do not queue behind guest links; or accept the first-use stall.

If the worker is stopped, the device is lost, or shutdown begins, the wait uses a timeout and
fails the entry. It must not hang the RSX thread.

### 9.5 Eviction

- Libraries: keep all mappings; cap at 1024 with LRU. A library is small compared with a PSO.
- Pipelines: cap at 1024 with LRU. A pipeline is per render state and is the large object.
- Eviction releases the Metal object only. The archive may still hold a binary; a later link can
  use it.

The cache uses one mutex for the maps. With the synchronous interpreter link the RSX thread is the
writer for interpreter entries; the link worker is the writer for guest-pipeline entries. Neither
holds the map mutex across `newRenderPipelineState`. The cache is not described as "RSX reads,
worker writes" because that is only true for the guest cache.

Eviction releases the Metal object and the library references. The command buffer retains the
pipeline state it records, so releasing a cache entry after `setRenderPipelineState` does not
destroy an in-flight draw. This is the same lifetime rule as the regular cache; it is stated
because the interpreter cache is smaller and evicts more often.

---

## 10. Draw-path integration

### 10.1 Current path

`MTLGSRender` currently:

1. calls `get_graphics_pipeline(..., allow_async, ...)`;
2. if the result is null, calls `execute_nop_draw()` and returns.

### 10.2 New path

```
const auto mode = g_cfg.video.shadermode.get();
const bool use_interpreter = (mode == async_with_interpreter || mode == interpreter_only);
const bool build_real = (mode != interpreter_only);
const bool can_interpret = use_interpreter && interpreter_can_represent(...);
const bool allow_async = build_real && mode != recompiler && can_interpret;

// Decide the async policy before the first submission. A submitted placeholder
// cannot be converted into a synchronous build later.
std::tuple data{};
if (build_real)
    data = get_graphics_pipeline(..., allow_async, ...);

pipeline* program = std::get<0>(data);
bool is_interpreter = false;

if (!program && can_interpret)
{
    program = m_interpreter.get(...);   // synchronous first-use link
    is_interpreter = program != nullptr;
}

if (!program && build_real && !allow_async)
    program = get_graphics_pipeline(..., /*async*/ false, ...);   // sync, no placeholder exists

if (!program && build_real && allow_async)
{
    // A guest job is in flight and the interpreter failed. Waiting for the
    // placeholder is the only correct option; calling the sync path now returns
    // the placeholder, not a pipeline. Wait with a bound, or skip this draw.
}

if (!program)
{
    execute_nop_draw();
    return;
}
```

`can_interpret` is the §11 support predicate, not just the depth-range bit. It must be evaluated
before `get_graphics_pipeline`, because an unsupported draw has to start in synchronous mode: once
an async placeholder is inserted, a later `allow_async = false` call cannot turn it into a sync
build. This is the main ordering defect in the earlier draft.

`MTLTraits::build_pipeline` already honours `compile_async`
(`MTLProgramPipeline.cpp:1057-1059`). The defect is in the draw path, which currently passes
`true` unconditionally. The fix is to pass `allow_async` and to run the synchronous build through
the same compile mutex as the link worker (§13). Otherwise a `recompiler` miss can enter
`newRenderPipelineState` while the worker is inside one, which is the L18-sensitive path.

### 10.3 Interpreter branch

When `m_is_interpreter` is true:

1. `m_interpreter.get()` returns the pipeline and the current mapping. It uploads the instructions
   if the dirty bits say so.
2. The draw binds the interpreter pipeline and the two interpreter argument tables.
3. The draw uses the normal viewport, scissor, vertex buffers/indices, blend constants and draw
   call. Only the pipeline and argument tables differ.
4. The draw records the active kind (interpreter) in the frame slot and leaves the interpreter flag
   set.

When the real pipeline becomes ready, the next call to `get_graphics_pipeline` returns it. The
draw path detects a kind change (previous draw interpreted, this draw not, or the reverse) and
clears `frame.transform_constants_valid` before the constant upload.

### 10.4 Modes

`recompiler`:

- `allow_async = false`;
- `build_pipeline_sync` on every miss;
- no interpreter;
- no skip.

`async_recompiler`:

- `allow_async = true`;
- no interpreter;
- skip on miss, as today. The probe latch remains.

`async_with_interpreter`:

- `allow_async = true`;
- interpreter on miss;
- no skip.

`interpreter_only`:

- do not call the real cache on the normal path;
- interpreter always;
- if the interpreter cannot represent the draw or its link fails, build the real pipeline
  synchronously and use it. This is a safety divergence from Vulkan's interpreter-only mode. It
  keeps the no-skip guarantee and prevents a wrong interpreter result;
- because no real program was decompiled, clear `properties.write_mask[i]` for
  `i >= current_fragment_program.mrt_buffers_count` before creating the interpreter pipeline, and
  trim the interpreter's exported outputs to the same count (§6.4).

### 10.5 What the interpreter does not change

- The fixed-function state: viewport, scissor, blend, depth, stencil, cull, topology, index and
  vertex streams.
- The present path.
- The texture cache upload and surface resolve.
- The real pipeline cache and archive.

The interpreter is a shader-stage substitution plus its own argument tables. A failure in the
interpreter must not leave the encoder in a different fixed-function state.

---

## 11. Unsupported features

The interpreter is not used when it cannot express the guest program correctly. In those cases the
draw path builds the real pipeline synchronously. The draw still happens.

| feature | detection | action |
|---|---|---|
| Vertex texture fetch | `vp_metadata.referenced_textures_mask != 0` | synchronous real pipeline |
| Emulated depth range | `fp.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE` | synchronous real pipeline |
| Emulated depth compare | `fp.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE` | synchronous real pipeline |
| Texture alpha kill | `fp.ctrl & RSX_SHADER_CONTROL_TEXTURE_ALPHA_KILL` | synchronous real pipeline |
| Programmable blending | `fp.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING` | synchronous real pipeline |
| Logic op | `fp.ctrl & RSX_SHADER_CONTROL_LOGIC_OP` | synchronous real pipeline |
| Shadow texture compare | `fp.texture_state.shadow_textures != 0` | synchronous real pipeline |
| Redirected texture | `fp.texture_state.redirected_textures != 0` | synchronous real pipeline |
| Multisampled texture | `fp.texture_state.multisampled_textures != 0` | synchronous real pipeline |
| More than 16 referenced fragment units | impossible by RSX limits | — |
| A texture dimension outside 0..3 | impossible by the 2-bit field | — |
| A shader mode that forbids the interpreter | `recompiler`, `async_recompiler` | no interpreter |
| Interpreter library or pipeline link failure | cache state `failed` | synchronous real pipeline |
| Interpreted draw with no valid pipeline | fallback chain | synchronous real pipeline |

`RSX_SHADER_CONTROL_ATTRIBUTE_INTERPOLATION` is **not** a rejection condition. It is set for
ordinary triangle draws and the shared interpreter handles both flat and smooth inputs.
`RSX_SHADER_CONTROL_ROP_OUTPUT_REMAP` is supported through `COMPILER_OPT_ENABLE_ROP_REMAP` and is
also not a rejection condition.

These remain open and must be resolved by the probe, not assumed:

- texture format conversion (`TEXTURE_FORMAT_CONVERT`) — the interpreter samples the cache's native
  format; if the guest conversion is observable, reject;
- sRGB / 8-bit framebuffer output — the attachment format may do the conversion, but the
  interpreter has no packing path;
- polygon stipple and `KIL` beyond the shared options — the shared body implements them; verify the
  MSL translation.

The rule is: no silent wrong result. An open case falls back until the probe closes it.

---

## 12. Failure handling

The draw path has one ordered fallback chain:

```
async real pipeline (if the mode allows async)
    -> interpreter pipeline
    -> synchronous real pipeline
    -> execute_nop_draw() + fatal log
```

The order has one constraint that a plain chain hides: the synchronous step is only reachable if
no async placeholder was submitted for this draw. If `get_graphics_pipeline` was called with
`allow_async = true`, the placeholder owns the key and a later `allow_async = false` call returns
the placeholder (null), not a new synchronous build. The draw path must therefore decide
`allow_async` from `can_interpret` **before** the first call (§10.2). For a draw that cannot be
interpreted, the first call is already synchronous.

The last step is a real failure, not a normal miss. It must produce a log line with the program
hashes, the shader mode and the interpreter error. It is the only skip in
`async_with_interpreter` and `interpreter_only`.

Library and pipeline failures are cached per key, so a broken shader does not retry every draw.
The cache entry carries the Metal error text. A different key retries.

A device loss or a permanent compiler failure abandons the interpreter cache in the same way the
existing pipeline jobs abandon their work. The draw path does not submit new work after the
device-loss flag is set.

---

## 13. Threading and lifetime

- The `MTL4::Compiler` remains one per device (W11).
- Interpreter libraries use `compile_msl` synchronously for first use, and `compile_msl_async` for
  the §9.3 prebuild. Library compilation is not done on the link worker.
- Interpreter pipeline links call `pipeline_jobs::run_link_sync`, which takes the same
  `m_compile_mutex` that the link worker holds around its synchronous
  `newRenderPipelineState`. This serializes the L18-sensitive call without a second compiler. It
  can wait for the in-flight guest link; that is the confirmed stall in §9.4.
- `MTLTraits::build_pipeline(compile_async=false)` must also take `m_compile_mutex`, for the same
  reason. The current `build_pipeline_sync` does not.
- Descriptor, MSL string and library lifetime: a synchronous call copies what it needs and returns
  before the descriptors are destroyed. The async library path keeps the existing lifetime
  holders. The published library is retained by `interpreter_library`; the published pipeline
  retains its libraries.
- The cache mutex is not held across `newRenderPipelineState` or `newLibrary`.
- Per-draw instruction blocks live in the frame's byte ring. The recorder's existing ring lifetime
  covers the GPU read, subject to the per-frame reset in §8.3.
- Argument tables and the second `argument_binder` live in the frame slot and are released with
  the slot.
- A second `MTL4::Compiler` without a `PipelineDataSetSerializer` is the measured escape hatch for
  interpreter links. It is not the first implementation: two compilers double the driver-side
  state and the current L18 workaround already makes one compiler safe.

The worker must not run an interpreter job while it holds the cache mutex. The cache mutex protects
the maps; the link happens outside it.

---

## 14. Diagnostics

All diagnostics are compile-time gated and marked `TODO(excise)`.

Probe latches:

```
draw/interpreter
draw/interpreter/library miss
draw/interpreter/pipeline miss
draw/interpreter/compile wait
draw/interpreter/sync real fallback
draw/skipped pipeline pending
```

Notes at level 2:

- the texture key hash, referenced count and per-type counts;
- the library/pipeline cache result and the compile time;
- the fallback reason when the interpreter is rejected.

The acceptance signal for the default mode is `draw/skipped pipeline pending == 0` and
`draw/interpreter > 0` during a transition. `draw/interpreter/sync real fallback` must be zero for
ordinary draws; a non-zero value means the interpreter missed a feature.

The existing `MTL_PERF_ENABLED` scopes cover the interpreter link and bind path. No trace line sits
on a path that can spin.

---

## 15. Tests

### 15.1 CPU tests

Add `test_rsx_metal4_interpreter.cpp`:

- `Mapping.SingleTypeUsesUnitIndex`: all-2D referenced units keep their unit index; array size is
  16; texture slot `12+unit`; sampler slot `unit`.
- `Mapping.MixedTypesCompact`: 2D units 0,2 plus cube unit 3 produce `count2d=2`, `countcube=1`,
  `base2d=0`, `basecube=2`; arrays at bindings 12 and 14; sampler bases 0 and 2; total 3.
- `Mapping.MixedTypeBinderSlots`: the binder's unit-to-table-slot mapping equals the shader's
  baked `unit_index_*`; a loop-counter implementation fails this test.
- `Mapping.UnreferencedDimensionDoesNotChangeKey`: changing a unit outside the referenced mask
  leaves the key equal.
- `Mapping.SingleTypeKeyIgnoresMask`: two all-2D draws with different referenced masks share one
  key and one 16-element 2D variant.
- `Mapping.ReferencedDimensionChangesKey`.
- `Mapping.EmptyTypeIsGuarded`: count 0 emits `MTL_INTERPRETER_NO_*` and no array.
- `Generation.ArrayBindings`: the generated GLSL contains the four array bindings at
  `12+base[t]`, the `unit_index_*` constants and the `IS_TEXTURE_RESIDENT` macro.
- `Generation.InstructionSetZero`: the FS instruction block is declared at `set=0, binding=12`
  and the VS block at the same set; both map to MSL buffer 12.
- `Generation.NoDuplicateMacros`: `texture_base_index` is not redefined; `TEX_PARAM` is either
  absent or undefined first; `_fs_*` appear once.
- `Generation.OutputTrim`: `mrt_buffers_count` 1 emits `ocol1..3` as locals and only one exported
  output; 4 emits all four.
- `Generation.ResidentAlias`: the block field is `reserved1` and `textures_resident` is a macro
  alias, not an undeclared identifier.
- `Generation.MslBindings`: run `compile_glsl_to_spv` + `spv_to_msl` on a small generated FS and
  assert the MSL texture/sampler indices. This test is CPU-only and does not need a Metal device.
- `Policy.SyncFallbackBeforeAsyncSubmit`: an unsupported feature selects `allow_async=false` on the
  first call; an async placeholder cannot be followed by a sync build.
- `Policy.ModeTable`: every `shader_mode` × miss × availability case selects the expected action.
- `Policy.InterpreterOnlyWriteMask`: attachments above `mrt_buffers_count` are masked.
- `Header.Layout`: VS and FS header sizes, offsets and alignment, including 256-byte ring
  alignment.
- `Header.RingReset`: the first interpreter draw of a frame uploads both blocks even when the
  program and mask are unchanged.
- `Binder.TableIsolation`: an interpreter bind followed by a regular bind does not skip the second
  table write; the reverse also holds.
- `Binder.PushConstantSlot`: the interpreter VS table always receives buffer 28 from
  `binds.vertex_draw_params`; a table without 28 fails the test.

The existing 499 enabled tests must still pass.

### 15.2 Standalone Metal probe

A small metal-cpp program compiles one generated interpreter MSL pair, creates a library and a
render pipeline state, and binds a compact array set. It checks:

- four combined-sampler arrays with a total of 16 elements are accepted;
- runtime indexing of a sampler array is accepted;
- 16 sampler slots and 16 texture slots fit the argument table;
- `[[user(locn16)]]` coexist with `dest[16]` at locations 0..15.

The probe is a device probe, not a test in CI. Keep the source and the result in the research
folder per the probe rule.

### 15.3 Runtime validation

Requires explicit approval. One bounded instance at a time.

1. `MTL_PROBE=2`, 120 s, `async_with_interpreter`. Check:
   - no `Dead FIFO`;
   - `draw/skipped pipeline pending == 0`;
   - `draw/interpreter > 0` at a transition;
   - no interpreter link failure.
2. `interpreter_only`, same bound. Check the image against Vulkan on:
   - the race start and the car body;
   - the menu background video;
   - the Xevious panel;
   - the track preview.
   This run separates an interpreter-correctness fault from a texture/surface fault. If a region is
   still black under `interpreter_only`, the interpreter is not the cause.
3. `recompiler`: confirm the synchronous build removes the skip and measure the hitch.

The screenshots are compared with Vulkan and OpenGL. The RSX remains the specification; the other
backends are evidence.

---

## 16. Implementation plan

| phase | work | files |
|---|---|---|
| 0 | mapping, key, mode policy, header layout, tests | `MTLShaderInterpreter.*`, `test_rsx_metal4_interpreter.cpp` |
| 1 | VS/FS GLSL builders, shared guards, CPU MSL test | `MTLShaderInterpreter.cpp`, `FragmentInterpreter.glsl` |
| 2 | interpreter library/pipeline cache, `run_link_sync` + compile mutex | `MTLShaderInterpreter.*`, `MTLProgramPipeline.*` |
| 3 | interpreter argument tables and binder | `MTLCommandRecorder.*`, `MTLArgumentBinds.*` |
| 4 | instruction ring, header upload, draw branch, mode wiring | `MTLGSRender.cpp`, `MTLConstants.*` |
| 5 | prebuild, LRU, diagnostics, sync fallback, `recompiler` path | `MTLShaderInterpreter.*`, `MTLProgramPipeline.*`, `MTLGSRender.cpp` |
| 6 | standalone probe, runtime validation, documentation | tests, `LOG-FINDINGS.md` |

Phase 0 and 1 are CPU-only and can land before any game run. Phase 4 is the first phase that can
be tested with a draw. Phase 6 needs approval.

All phases of this plan have landed; §21 is the implementation record. Phase 6's runtime half
(the draw under `MTL_PROBE=2`, `interpreter_only` and the standalone probe) is still gated on
approval (§15.2/§15.3, §17).

---

## 17. Risks and open questions

| risk | impact | mitigation |
|---|---|---|
| MSL rejects runtime sampler-array indexing | interpreter cannot use the compact form | The MSL specification permits `operator[]`; the standalone probe is the gate. If it fails, generate a `switch(unit)` that selects a constant index. The shader body stays the same. |
| SPIRV-Cross does not remap array bindings as expected | wrong texture/sampler indices | The CPU `Generation.MslBindings` test asserts the emitted indices before any GPU work. |
| FS instruction block collides with a texture array in SPIR-V | module rejected | The block is at `set=0, binding=12`; the CPU generator test asserts it. Metal's buffer and texture spaces are separate, but GLSL sets are not. |
| Compact type branch still declares an unused sampler | MSL 16-sampler link failure | The `#ifndef` guard wraps the whole case; the generator test asserts the count. |
| One argument binder serves both tables | stale cache skips the second bind | `interpreter_binder` is separate and reset per frame; `Binder.TableIsolation` covers it. |
| Manually bound instruction ring is under-aligned | wrong ucode or validation failure | `alloc_aligned(..., 256)` and `Header.Layout` assert the address. |
| Per-frame ring reset invalidates the dirty bits | stale ucode | Force both blocks on the first interpreter draw of every frame; `Header.RingReset` covers it. |
| The full 468-vector vertex constant file is not bound by the regular path | wrong vertex constants | Confirmed present: `constants_heap` allocates 468 and `argument_binds.vertex_constants` is its base. The interpreter leaves `compiled_vertex` null to force the full upload. |
| `[[user(locn16)]]` conflicts with the 16 register varyings | pipeline link failure | The standalone probe checks it. If it fails, move the payload to a buffer slot and read it in the FS; this is a small generator change. |
| Interpreter PSO link stalls behind an in-flight guest link | a visible transition hitch | The wait is serialized on `m_compile_mutex`; priority cannot preempt. Measure, then prebuild or add a serializer-free second compiler. |
| Interpreter output count exceeds attachments | pipeline link failure or unspecified behaviour | Trim `ocol1..3` with the `MTL_INTERPRETER_NO_*` macros; mask `write_mask` in `interpreter_only`. This is required, not conditional. |
| Variant explosion | memory and compile time | Single-type identity mapping removes the common case from the key. Mixed-type maps are LRU-bounded and measured. |
| Texture dimensions change inside a draw | header and variant mismatch | The key includes the referenced dimensions; the FS block is re-uploaded with the header when the key changes. |
| Vertex textures | wrong result | Reject the interpreter and build the real pipeline synchronously. |
| Device loss or compiler failure | no fallback | Cache the failure, abandon new work, and keep the existing device-loss path. |
| 1D texture support on Apple silicon | link or sampling failure | Probe; if unsupported, reject 1D draws to the real pipeline. |
| Archive may capture interpreter pipelines | archive growth | Unverified: the serializer is attached to the device, not the task options, so `options=nullptr` does not prove exclusion. Measure the archive size; if it grows, skip interpreter links from the serializer or accept the cost. |

Open questions that need measurement, not design changes:

1. The exact first-use wait for a new mixed-type mapping on RR7, including a wait behind a guest
   link of a cold shader.
2. The number of distinct `interpreter_fragment_key` values in a normal menu/race transition.
3. Whether the MSL compiler accepts 16 samplers across four arrays at bases 0, 2, 4, 6 with a
   runtime index on the target OS.
4. Whether a fragment function with four color outputs links against a one-attachment pipeline on
   the target OS.
5. Whether interpreter pipelines appear in the device serializer's archive.

---

## 18. Why this fits the Metal 4 rule

Every new object uses the Metal 4 API:

| purpose | API |
|---|---|
| compile MSL | `MTL4::Compiler::newLibrary`, `MTL4::LibraryDescriptor` |
| describe entry points | `MTL4::LibraryFunctionDescriptor` |
| link a pipeline | `MTL4::Compiler::newRenderPipelineState` returning `MTL::RenderPipelineState`, `MTL4::RenderPipelineDescriptor` |
| hold arguments | `MTL4::ArgumentTable`, `MTL4::ArgumentTableDescriptor` |
| bind arguments | `MTL4::ArgumentTable::setAddress`, `setTexture`, `setSamplerState` |
| encode a draw | `MTL4::RenderCommandEncoder`, `setArgumentTable`, `setRenderPipelineState` |
| keep resources | `MTL::Texture::gpuResourceID`, `MTL::SamplerState::gpuResourceID` |
| archive | `MTL4::PipelineDataSetSerializer` (existing) |

There is no Metal 3 encoder, no MoltenVK object and no Vulkan descriptor set in the path.

KosmicKrisp confirms the shape:

- one `MTL4::ArgumentTable` per command buffer, with explicit buffer counts
  (`.kk-mesa/src/kosmickrisp/vulkan/kk_cmd_buffer.c`);
- `setAddress` for buffers and `gpuResourceID` for textures and samplers
  (`.kk-mesa/src/kosmickrisp/bridge/mtl_argument_table.m`);
- `MTL4::Compiler` for libraries (`.kk-mesa/src/kosmickrisp/bridge/mtl_compiler.m`).

The interpreter uses the same model with a larger table and a fixed RSX slot map.

---

## 19. References

- `rpcs3/Emu/RSX/VK/VKShaderInterpreter.cpp` — the reference interpreter build, options and draw
  integration.
- `rpcs3/Emu/RSX/GL/GLShaderInterpreter.cpp` — the reference variant and preload design.
- `rpcs3/Emu/RSX/Program/ShaderInterpreter.h` — the compiler options and shared GLSL bodies.
- `rpcs3/Emu/RSX/Program/GLSLInterpreter/VertexInterpreter.glsl`,
  `FragmentInterpreter.glsl` — the shared interpreter source.
- `rpcs3/Emu/RSX/MTL/MTLArgumentBinds.h` — the RSX argument-table map and the 16-sampler assert.
- `rpcs3/Emu/RSX/MTL/MTLShader.cpp` — the SPIR-V → MSL binding rule.
- `rpcs3/Emu/RSX/MTL/MTLProgramPipeline.h` — the serialized link worker, L18 workaround and keys.
- `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp` — the draw path, skip site and texture environment.
- `rpcs3/Emu/RSX/MTL/MTLCommandRecorder.cpp` — the per-frame argument tables.
- `docs/research/metal4/design/HLD-07-shader-pipeline-cache.md` §4.3 — the gap this document
  closes.
- `docs/research/metal4/design/METAL4-TODO.md` F5 — the defect row.
- Metal Shading Language Specification §2.12.1 — arrays of textures, texture buffers and samplers.
- KosmicKrisp `kk_cmd_buffer.c`, `mtl_argument_table.m`, `mtl_compiler.m` — Metal 4 binding and
  compiler evidence.

---

## 20. Design review — 2026-10-21

The review audited this design against the current `MTL*` sources, the shared GLSL interpreter, the
Vulkan interpreter, and the Metal 4/macOS 27 API contract. The body above already incorporates the
corrections. This section is the ledger: a row is listed only when the defect is confirmed in the
source or the API contract, not when it is a preference.

### 20.1 Blockers found before implementation

| # | defect | confirmation | correction |
|---|---|---|---|
| R1 | FS instruction block at `set=1, binding=12` collides with `sampler1D_array` at `set=1, binding=12` when `base[0]=0` | GLSL/SPIR-V descriptor bindings are unique per set; `spv_to_msl` maps by binding, not by set | FS block is at `set=0, binding=12`; the VS block is also set 0. MSL buffer 12 is unchanged. |
| R2 | `IS_TEXTURE_RESIDENT` references `textures_resident`, but the block field is `reserved1` | §6.2 block declaration | Emit `#define textures_resident reserved1` before the body, or use `reserved1` directly. |
| R3 | Builder redefines `TEX_PARAM` differently and redefines `texture_base_index`, both already emitted by `MTLFragmentDecompilerThread::insertConstants` | `MTLFragmentProgram.cpp:174-177, 328` | `#undef TEX_PARAM` only if needed (the shared body does not use it); do not redefine `texture_base_index`; add only the fog/wpos macros. |
| R4 | Empty-type guard removed the array but left the `case` label/body reachable | shared `_texture`/`_textureLod` switch | Wrap the whole `case` label and body in `#ifndef MTL_INTERPRETER_NO_*`; add a `default` return. |
| R5 | Four exported `ocol` outputs are declared unconditionally | shared FS body; `interpreter_only` has no real-program validation | Trim to `min(4, mrt_buffers_count)` with `MTL_INTERPRETER_NO_OCOL1/2/3`; mask `write_mask` in `interpreter_only`. |
| R6 | One `argument_binder` cannot serve two tables | `argument_binder` caches per stage, not per table | Add `interpreter_binder`, reset per frame; add separate VS/FS instruction address fields. |
| R7 | `byte_ring::alloc` aligns to 4, not 256 | `MTLConstants` ring allocator | Add `alloc_aligned(size, 256)` before `setAddress`. |
| R8 | The ring resets every frame, but the dirty bits survive | recorder `begin_frame` / slot reuse | Force both blocks on the first interpreter draw of every frame; the cache property is intra-frame only. |
| R9 | A later `allow_async=false` cannot replace an async placeholder | `get_graphics_pipeline` key/placeholder contract | Compute `can_interpret` and `allow_async` before the first submission; unsupported draws start synchronous. |
| R10 | The rejection list omitted cases the shared interpreter cannot express | §11 and the shared body | Add texture alpha kill, emulated depth compare, programmable blending, logic op, shadow/redirected/multisampled textures, vertex textures. |
| R11 | The builder calls protected/private decompiler helpers | `MTLFragmentProgram.h:39-58`, `MTLVertexProgram.h:44-58` | Add public wrappers (`build_interpreter_body`, `emit_fs_payload`) or a friend builder; do not make the members public wholesale. |
| R12 | `emit_geometry` was assumed to dereference a null `compiled_fragment` | current code guards it at `MTLGSRender.cpp:2616` | The guard already exists; the design still forces both `compiled_*` to null and keeps the guard as a contract. |
| R13 | `transform_constants_valid` can be reused across interpreter/regular draws | `MTLGSRender.cpp:2571-2585` | Clear it on every kind change or compiled-vertex change. |
| R14 | The design said `MTLTraits::build_pipeline` ignores `compile_async` | false: `MTLProgramPipeline.cpp:1057-1059` already branches | The defect is the draw path passing `true` unconditionally. Pass `allow_async`. |
| R15 | Synchronous builds can race the link worker | one `MTL4::Compiler`, L18 | Put `build_pipeline_sync` and `run_link_sync` behind the same compile mutex. |
| R16 | "Priority" was claimed to bound the first-use wait | the worker is a single serialized queue; priority only chooses the next job | Document the possible wait behind an in-flight guest link; measure, then prebuild or use a serializer-free second compiler. |
| R17 | The design asked to "verify" the full 468-vector constants | confirmed: `constants_heap` allocates `vertex_constant_count` and `argument_binds.vertex_constants` is its base | State it as confirmed; leave `compiled_vertex` null to force the full upload. |
| R18 | "Interpreter pipelines are not archived" was an assumption | the serializer is attached to the device descriptor, not to `CompilerTaskOptions` | Mark as unverified and measure the archive size. |
| R19 | `MTL4::RenderPipelineState` is not a type | the compiler returns `MTL::RenderPipelineState*` | Corrected throughout; the descriptor remains `MTL4::RenderPipelineDescriptor`. |
| R20 | Prebuild was described as link-worker work | libraries use `compile_msl_async`; the worker links pipelines | Build sources on init, compile libraries asynchronously, link pipelines on first use. |
| R21 | Binder pseudo-code used the loop counter as the compact slot | mixed-type slots are `base[type] + index[unit]` | Use the shared mapping helper in both the shader and the binder. |
| R22 | `interpreter_only` did not mask attachments | no `get_graphics_pipeline` validation | Clear `write_mask` above `mrt_buffers_count` and trim outputs. |
| R23 | `ATTRIBUTE_INTERPOLATION` was considered a rejection condition | it is set for ordinary triangles (`RSXThread.cpp:2429`) | Explicitly not a rejection test; the shared interpreter handles flat/smooth. |
| R24 | MSL 1D samplers, runtime sampler indexing and `[[user(locn16)]]` are unvalidated | no local Metal CLI; the runtime compiler is the only validator | Keep the standalone probe as the gate; the CPU MSL test is necessary but not sufficient. |
| R25 | The binder pseudo-code omitted the push-constant substitute at buffer 28 and the conditional stipple/instancing slots | `MTLVertexProgram.cpp:130`, `MTLArgumentBinds.cpp:98`; the VS reads `draw_parameters_offset` from slot 28 | Bind 28 from `binds.vertex_draw_params`, plus 8/9/10/11 when the draw uses them. |
| R26 | The single-type key still contained the referenced mask, so the four prebuilt variants were never reused | §5.2 says the variant is keyed by type only; §5.6 kept `referenced_mask` | Clear `referenced_mask`/`dimensions` and store `single_type`; the live mask lives in the header and the binder. The key test asserts two masks share one key. |

### 20.2 What the review changed in the body

- §3.4 now states the SPIR-V set rule and the `set=0` FS block.
- §5.4/§5.5 define the macro/array only for non-empty types, the full `case` guard, the default
  case, and the output-trim macros.
- §5.6 clears the mask/dimensions for the single-type key, so the prebuilt variants are reused.
- §6.2 corrects the binding, the resident alias, the `insertConstants` ownership of `_fs_*` /
  `texture_base_index` / `TEX_PARAM`, and sets `INTERPRETER_MODEL` before calling it.
- §6.4 makes output trimming a requirement.
- §7.2 corrects the compact-slot mapping and adds the second binder, the stage-specific instruction
  fields, and the push-constant/stipple/instancing binds.
- §8.3 adds per-frame invalidation and 256-byte alignment.
- §8.4 replaces Vulkan dirty-bit names with the actual `frame.compiled_*` /
  `transform_constants_valid` mechanics.
- §9.3/§9.4/§9.5/§13 correct the prebuild, the first-use stall, the writer ownership and the
  compile mutex.
- §10.2/§10.4/§12 correct the async-then-sync ordering, `interpreter_only` masking and the
  placeholder rule.
- §11 adds the confirmed unsupported features and removes `ATTRIBUTE_INTERPOLATION`.
- §15 adds the tests that lock each correction.
- §17 records the remaining measurement gates.

### 20.3 Implementation gate

The interpreter is not ready to implement until the following are true in the first commit:

1. The FS block is `set=0, binding=12`; no generated module contains duplicate `(set, binding)`.
2. Every generated shader's sampler count equals the referenced unit count.
3. Every generated shader's exported output count equals `min(4, mrt_buffers_count)`.
4. The binder and the shader use the same unit-to-slot mapping.
5. `interpreter_binder` exists and is reset per frame.
6. Instruction blocks are 256-byte aligned and forced on the first interpreter draw of a frame.
7. `can_interpret`/`allow_async` are decided before the first `get_graphics_pipeline` call.
8. Synchronous links and synchronous real builds share the compile mutex.
9. `frame.compiled_*` are null for interpreter draws and `transform_constants_valid` is cleared on
   kind changes.
10. The interpreter table binds buffer 28 (`vertex_draw_params`) and the conditional stipple and
    instancing slots.
11. The single-type key ignores the referenced mask, so two masks share one variant.
12. The CPU tests in §15.1 pass, and the standalone probe has answered R24 before any runtime
    claim.

---

## 21. Implementation record — 2026-10-21

The interpreter landed on `metal4`. This section is the ledger of what the implementation
confirmed, the corrections the compiler and the tests forced, the optimisations, and the
verification. It is the companion to §20: §20 corrected the design before coding, §21 corrects the
design after coding.

### 21.1 Corrections the compiler and the tests forced

| # | finding | evidence | correction |
|---|---|---|---|
| I1 | The FS builder's `#define VULKAN` collided with glslang's own `VULKAN` define for `glsl_rules_vulkan` (`'#define' : Macro redefined; different substitutions: VULKAN`) | `MTLOverlays.cpp` documents the same rule | The interpreter sources do not define `VULKAN`; glslang supplies it. |
| I2 | The VS prologue omitted `_builtin_lit` (`no matching overloaded function found`), which the shared body needs for LIT | `VKShaderInterpreter.cpp:180` | The VS builder sets `MTLVertexDecompilerThread::properties.has_lit_op = true` before the body. |
| I3 | Forcing `in_register_mask` for wpos/fogc made the body read `fetch_fog_value()`/`get_wpos()`, which reference the named varying `fog_c` the interpreter never declares (`fog_c undeclared identifier` / `missing #endif`) | `VKShaderInterpreter.cpp:286-291,318-323` | The FS emits the context macros (`fog_param0/1`, `fog_mode`, `wpos_scale`, `wpos_bias`) directly and does not set the wpos/fogc register bits. |
| I4 | `canonicalize()` clears `referenced_mask` for the single-type fast path, so `empty() == (referenced_mask == 0)` mis-reported a textured single-type program as empty and silently dropped `COMPILER_OPT_ENABLE_TEXTURES` | §5.2/§5.6 | `empty()` is `!is_single() && referenced_mask == 0`; `build_request` and `build_fragment_source` test `!mapping.empty()`. |
| I5 | The first cut reused the regular tables for interpreter draws | §4.2/§7.1 | Implemented as designed: `frame_slot` gains `interpreter_vertex_table`/`interpreter_fragment_table`. This is a correctness fix, not only isolation: a shared table let an interpreter bind clear the regular 12..27 slots while the regular binder's cache still believed they held textures, so the next regular draw in the same frame read null/stale textures. |

### 21.2 Performance work

| # | optimisation | where |
|---|---|---|
| P1 | A one-entry memo of the last `(request, pipeline_properties)` skips the two shader-cache probes, the pipeline hash and the `pipeline_properties` copy for a repeated clause. | `interpreter_cache::get`, `m_memo` (`MTLShaderInterpreter.*`). |
| P2 | `bind` gained `bind_textures`; the interpreter passes false, so it writes only the named slots and never walks the 16 texture + 16 stencil + depth units, and `bind_interpreter` no longer copies the whole `argument_binds`. | `MTLArgumentBinds.{h,cpp}`. |
| P3 | The ucode hash is validated once per program change and read from the program metadata, instead of re-hashing the ucode every draw for the instruction-block dedupe key. | `ProgramStateCache.h` (`ucode_hash`), `RSXThread.cpp`, `MTLGSRender.cpp`. |
| P4 | The single-type compact bind range is the highest referenced unit plus one (carried as `frame.interpreter_texture_count`), not a fixed 16. | `MTLGSRender.cpp`. |

In the default `async_with_interpreter` mode the interpreter only runs during a compile gap, so P1,
P2 and P4 matter mainly for `interpreter_only`; P3 removes a per-draw hash on the interpreter path.

### 21.3 Diagnostics removed

The bring-up dumps (`/tmp/mtl-interp-*.glsl`, `/tmp/mtl-interp-*.msl` and the test's `<fstream>`
include) and the `SPIRVCommon.cpp` `fprintf` were removed. The interpreter sources carry no
`TODO`/`FIXME`/`HACK`/workaround marker; the only reports are `rsx_log.error` failures.

### 21.4 Verification

`test_rsx_metal4_interpreter.cpp` carries five tests; all pass and the full suite is green
(504/504 enabled, 2 pre-existing disabled):

- `MTLInterpreter.SingleTypeMappingCanonicalises`
- `MTLInterpreter.MixedTypeMappingPacksByDimension`
- `MTLInterpreter.VertexSourceCompiles`
- `MTLInterpreter.SingleTypeFragmentSourceCompiles`
- `MTLInterpreter.MixedTypeFragmentSourceCompiles`

The MSL bindings were confirmed by a temporary dump: a single-type FS declares its texture array at
`[[texture(12..)]]` and its sampler array at `[[sampler(0)]]`; the mixed case places the textures at
12.. and the samplers at 0..`. (The dump itself was removed; §21.3.)

Still open, unchanged by this record: the runtime gates R24 and the §17 measurement questions.

### 21.5 Coverage probes (2026-10-21, after the 20:55 run)

The 20:55 probe-2 run reached the race with no skipped draw, so it could not say how much of the draw
stream the interpreter actually covers. Two families now answer that without text:

- `draw/interpreter` versus `draw/recompiler` — one latch per draw, split into two macros because a
  latch registers its name once per call site (`REVIEW-FINDINGS.md` M1).
- `interpreter/reject vertex textures` / `… fragment control` / `… texture state` / `… no output` /
  `… alpha never` — one latch per rejection reason in `interpreter_cache::build_request`, plus
  `interpreter/pipeline not ready` when the source is eligible but its link has not landed.

The draw path also records `draw vertex program` and `draw fragment program` by ucode hash, at level
3 so the level-2 performance numbers stay clean. The interpreted variant of a program carries the
same hash, so a level-3 capture pairs the coverage with the program identity. These are temporary
diagnostics under the same `TODO(excise)` excision rule as the rest of the probe.

