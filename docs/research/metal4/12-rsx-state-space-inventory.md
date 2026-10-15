# 12 — RSX state-space inventory (the completeness spine)

> **Status:** mechanically generated + curated. This document is the narrative
> around `inventory/`. The *facts* are produced by
> `inventory/generate.py`; the *disposition* layer is curated per cluster
> (`inventory/out/dispositions/*.csv`) and gated by `inventory/validate.py`.
>
> Related: `10-rsx-native-metal4-architecture.md` (the design frame),
> `11-verification-loop.md` (how mappings get *verified*), and the
> `design/HLD-*` / `design/LLD-*` documents that consume this spine.

---

## 1. Purpose — why an inventory exists at all

The RSX → Metal 4 mapping is **not a bijection**. It is many-to-one and
semantic: some RSX state maps *directly* (`direct`), some *reshapes*
(`reshape`), some must be *emulated* (`emulate`), and some has **no Metal
path at all** (`impossible`). "We have mapped RSX → Metal 4" is therefore not an
assertion that can be made by writing enough code — it is a **coverage
statement**: *every* item the guest can put into RSX state has a recorded
disposition.

You cannot make that statement without first knowing the set you are ranging
over. That set is finite and enumerable, and it lives in the RPCS3 tree. This
document describes the mechanism that enumerates it, proves the enumeration is
complete against the tree, and records the disposition of every member.

The spine exists to make three failure modes impossible:

1. **Silent omission** — a register nobody noticed, never wired up, and never
   tested. With a generated spine, an unwired register is a visible `unbound`
   row, not an absence.
2. **Undocumented impossible** — a construct with no Metal equivalent that is
   quietly *approximated*. With a spine, `impossible` is a first-class value
   with a fallback recorded next to it.
3. **Rotten coverage claims** — "we support RSX" measured by vibes. With a spine,
   coverage is a fraction of a finite set, recomputable on every commit.

## 2. Method — generated facts, curated meaning

Three layers, strictly ordered:

| layer | owner | artifact | mutability |
|---|---|---|---|
| **FACT** | `generate.py` | `out/*.csv`, `out/*.json`, `out/SUMMARY.md` | regenerated, never hand-edited |
| **DISPOSITION** | humans / agents, per cluster | `out/dispositions/<cluster>.csv` | curated, one row per id |
| **NARRATIVE** | humans | `design/*.md`, this document | curated prose |

The generator parses — it never hand-lists — the authoritative sources:

| source | extracted | space |
|---|---|---|
| `rpcs3/Emu/RSX/gcm_enums.h` | the FIFO method enum | **COMMAND** |
| `rpcs3/Emu/RSX/gcm_enums.h` | `CELL_GCM_*` and other enums | **VALUE** |
| `rpcs3/Emu/RSX/rsx_decode.h` | `registers_decoder<>` bitfields | **DECODE** |
| `rpcs3/Emu/RSX/rsx_methods.cpp` | the dispatch table | dispatch |
| `rpcs3/Emu/RSX/VK/vkutils/pipeline_binding_table.h` | argument-table slots | binding |
| `rpcs3/Emu/RSX/NV47/HW/*.cpp` | hardware modules | implementation |
| `rpcs3/Emu/RSX/Program/RSX{Vertex,Fragment}Program.h` | shader instruction encoding | **SHADER** |

The method enum stores **word indices** (`NV4097_SET_SURFACE_FORMAT = 0x00000208
>> 2`, `gcm_enums.h:562`); the inventory reports **byte addresses**
(`byte = word * 4`), because that is what the hardware manuals and the FIFO use.

**Determinism is enforced.** `test_generate.py` builds the model twice and
asserts byte-identical output, so the spine can be diffed in review. **Completeness
is enforced** by sanity checks that fail the generator's exit code if the shape of
the tree changes materially: minimum register count, the load-bearing
`NV4097_SET_SURFACE_FORMAT == 0x208` identity, minimum decoder/binding counts, and
a minimum value-space size.

## 3. The state spaces

### 3.1 COMMAND space — `out/rsx_state_space.csv`

One row per FIFO method register. At this revision: **282 registers**.

Columns: `id, name, group, cluster, addr, word, has_decoder, n_fields,
binding_kind, handler, step, count, source`, then the curated columns
`disposition, metal4_target, difficulty, risk, confidence, doc_ref, notes`.

The method space is partitioned into 14 **clusters** (§5), which are the unit of
work for the low-level design docs.

### 3.2 DECODE space — `out/rsx_decode.h` → `out/rsx_decode_fields.csv`

Every `registers_decoder<REG>` specialisation is recorded (**158** of the 282
registers have one), with its literal bitfield extractions. There are **93**
extracted `bf_decoder<offset,width>` fields. Fields whose arguments are symbolic
templates (`bf_decoder<I,N>`) are a layout generator, not a concrete register
field, and are correctly excluded.

`rsx_decode.h` is the authoritative bit layout: a register is a 32-bit word whose
meaning is defined only by its decoder. **Any mapping that reads a register without
using the decoder is a bug.** This is why the spine records decoder coverage: a
register with `has_decoder=0` is either a pure pass-through (rare) or a register
nobody has decoded.

### 3.3 VALUE space — `out/rsx_value_space.csv`

The enums registers can carry. At this revision: **768 constants across 10 enums**.

| enum | constants | role |
|---|---|---|
| *(anonymous)* | 393 | unqualified `CELL_GCM_*`-family constants |
| `CellGcmMethod` | 282 | the method enum itself (mirror of COMMAND) |
| `CellGcmTexture` | 31 | texture format constants |
| `CellGcmLocation` | 26 | memory location constants |
| `Method` | 22 | PS3 SDK method enum |
| `texture_dimension_extended` | 4 | texture dimensionality |
| `CellGcmDefaultFifoMode` | 3 | FIFO mode |
| `texture_dimension` | 3 | texture dimensionality |
| `CellGcmSystemMode` | 2 | system mode |
| `CellRescTableElement` | 2 | rescale table elements |

The value space is where the *format/blend/compare* mapping lives. Every texture
format, blend factor, blend equation and comparison function the guest can write
becomes a row that must resolve to a Metal enum or a shader emulation. That
exhaustive table is `design/LLD-value-space.md`.

### 3.4 SHADER space — `out/rsx_shader_facets.csv`

The guest shader is a microcode program whose *encoding* is fixed RSX state. At
this revision: **275 facets**.

| kind | count | what |
|---|---|---|
| `opcode` | 70 | fragment program opcodes (`rsx_fp_op_names`) |
| `bitfield` | 129 | instruction bitfields (VP `D0`–`D3`,`Src`; FP `OPDEST`,`Src0`–`2`,`Src_Common`) |
| `enum` | 58 | VP scalar/vector opcodes, VP/FP register types, FP precision |
| `field` | 18 | program control fields (`ctrl`, `output_mask`, `texcoord_control_mask`, `mrt_buffers_count`, …) |

This is the ISA a Metal 4 backend must *translate*, and it is the input contract to
the shader work in `design/HLD-05`. Recording it as a finite facet space turns
"the decompiler handles RSX shaders" into a coverage statement too: every opcode,
every precision mode, every instruction bitfield is a row with a translation.

### 3.5 Supporting structures

- **Dispatch table** (`rsx_methods.cpp`): **112** of 282 registers carry a
  binding — `bind`=75, `bind_range`=29, `bind_array`=8; the remaining **170 are
  unbound** (state consumed indirectly, or not consumed at all). The binding names
  the actual handler (`nv4097::clear`, `nv4097::draw_arrays`, …) and, for ranges,
  the stride and count (e.g. the 16 fragment textures are a `bind_range` with
  `step=8, count=16`).
- **Argument-table slots** (`pipeline_binding_table.h`): 12 slots — the Metal 4
  binding contract is derived from these, not invented.
- **NV47/HW modules**: 6 implementation modules — `nv4097` (36 functions),
  `common` (6), `nv406e` (3), `nv3089` (3), `nv0039` (1), `nv308a` (0).

## 4. Stable IDs

IDs are the C++ symbol names, deliberately, so they survive regeneration and can be
grepped.

| prefix | meaning | example |
|---|---|---|
| `M_<NAME>` | a FIFO method register | `M_NV4097_SET_SURFACE_FORMAT` |
| `M_<NAME>.<field>` | a decoded bitfield | `M_NV4097_SET_SURFACE_FORMAT.color_fmt_raw` |
| `V_<NAME>` | a value-space constant | `V_CELL_GCM_TEXTURE_A8R8G8B8` |
| `S_<slot>` | an argument-table slot | `S_textures_first_bind_slot` |
| `N_<module>#<fn>` | an NV47/HW function | `N_nv4097#clear` |

**No ID is ever invented.** The spine CSV is the namespace. `validate.py` rejects
any disposition row whose `id` is not a spine id.

## 5. Coverage

### 5.1 Command space, by cluster

| cluster | registers | with decoder | bound | the shape of the work |
|---|---|---|---|---|
| `surface` | 40 | 25 | 21 | render-pass synthesis; MRT; clears |
| `depth-rop` | 38 | 30 | 21 | depth/stencil/ROP + **ZCULL** |
| `raster` | 30 | 25 | 3 | viewport/scissor/cull/clip; coordinate systems |
| `vertex` | 30 | 6 | 23 | attribute fetch, constants, VTF, instancing |
| `nv3089-image` | 21 | 14 | 1 | scaled image blit engine |
| `sync-flip` | 18 | 7 | 13 | semaphores, fences, FIFO barriers, flip |
| `texture` | 16 | 3 | 13 | sampler state, formats, swizzle |
| `nv308a-image` | 16 | 1 | 0 | image-from-CPU engine |
| `blend` | 15 | 14 | 4 | blend/alpha-test/logic-op/ROP |
| `misc` | 16 | 5 | 2 | uncategorised / sparse |
| `draw` | 13 | 9 | 10 | begin/end, draw calls, inline arrays |
| `nv2d-surface` | 13 | 7 | 0 | 2D surface copy / swizzle engine |
| `nv0039-dma` | 12 | 9 | 1 | DMA copy engine |
| `fixed-function` | 4 | 3 | 0 | legacy lighting/fog remnants |

The distribution is itself a finding: **half the command space is not draw
submission.** ~40% of the registers belong to the *resource and raster* families
(`surface`, `depth-rop`, `raster`, `texture`), and a further ~20% belong to the
**copy/blit engines** (`nv3089`, `nv308a`, `nv2d-surface`, `nv0039`) that a
naive "just map the draw path" plan would miss entirely. The spine makes that
impossible to miss.

### 5.2 The unbound 170

170 registers carry no dispatch binding. These are not dead: many are consumed
*indirectly* (a register whose value is read by another handler's decoder), and
some are genuinely inert. The distinction matters and is exactly what the
disposition layer records (`direct`/`reshape` vs `na`/`moot`). The rule: an
`unbound` register must still have a disposition; "unbound" is not a disposition.

### 5.3 Orphan bindings

Three bindings in `rsx_methods.cpp` target addresses that are not named constants
in the method enum, and are therefore *outside* the spine:

| address | handler | note |
|---|---|---|
| `0x2100 >> 2` | `trace_method` | trace-only hook |
| `0x171c >> 2` | `trace_method` | trace-only hook |
| `FIFO::FIFO_DRAW_BARRIER >> 2` | `fifo::draw_barrier` | FIFO-namespace barrier |

These are recorded as `orphan_bindings` in `rsx_inventory.json`. They are a
*warning*, not a coverage hole: they are hooks the backend contract must still
honour even though they are not guest-writable RSX state.

### 5.4 Completeness gate

`validate.py` computes coverage against the spine and fails (under `--strict`)
unless every id is curated exactly once. Coverage is a number, not an opinion:

```
$ python3 inventory/validate.py --strict
spine=282 curated=282 (100.0%) shards=14
```

## 6. Disposition vocabulary

Every curated row assigns one `disposition`:

| value | meaning | obligation it creates |
|---|---|---|
| `direct` | maps ~1:1 to a Metal 4 construct | may still need glue; record it |
| `reshape` | same need, different shape | record the *shape change* |
| `emulate` | no native path; realised in shader/compute/host | record the fallback |
| `impossible` | no Metal API path exists | record what the emulator does instead |
| `not-recommended` | expressible but a trap | record the trap |
| `moot` | never appears under the RSX-first frame | record why |
| `na` | no state / not applicable | none |

with `difficulty` 1–5, `risk` (low/med/high), `confidence` (`[V]`/`[I]`/`[U]`),
and `doc_ref` pointing at the design doc that owns the decision.

The `confidence` tag is the honesty mechanism. `[U]` is not a failure — it is a
recorded *experiment* that `11-verification-loop.md`'s ladder (H0–H7) exists to
discharge. A spine with no `[U]` rows would mean the team stopped looking.

## 7. Cluster notes

Brief orientation; the low-level design is in `design/LLD-<cluster>.md`.

- **`draw`** — `SET_BEGIN_END` primitive-mode state machine, `DRAW_ARRAYS`,
  `DRAW_INDEX_ARRAY`, `INLINE_ARRAY` (vertex data through the FIFO),
  index-array DMA, primitive restart, render-enable (conditional rendering). The
  shape change is RSX's *stateful* begin/end vs Metal's per-draw calls.
- **`vertex`** — attribute format/offset decode (type/size/stride/frequency),
  attribute input/output masks, transform constants, vertex textures (VTF),
  indexed constants for instancing. Low decoder count (6) but very high binding
  count (23) — the value lives in the argument table, not in bitfields.
- **`texture`** — sampler state and format decode. Note the inversion vs `vertex`:
  **only 3 decoders but 13 bindings**. Texture state is mostly *encoded in the
  argument table*, so the risk is binding-index correctness, not bit layout.
- **`surface`** — the render-target family and the clear path. This cluster is
  where the **hazard-derived pass scheduler** gets its inputs (see
  `design/HLD-03`); `SET_SURFACE_FORMAT` alone drives format, MRT count, sample
  count and dimensions.
- **`depth-rop`** — depth/stencil/ROP state plus **ZCULL**, the tile-level depth
  cull whose statistics are *guest-observable*. ZCULL is the canonical
  high-risk cluster (see `design/LLD-depth-rop`).
- **`blend`** — blend and the **alpha test**, which no modern GPU has; it must be
  lowered into the shader. Logic ops have no Metal blend path either.
- **`raster`** — viewport, scissor, cull, and the coordinate-system shape changes
  (clip-space range, Y orientation), plus user clip planes → `[[clip_distance]]`.
- **`sync-flip`** — semaphores/references, `SET_WAIT_FOR_IDLE`, FIFO barriers and
  the flip path. The producer/consumer edges of the whole emulator live here.
- **`fixed-function`** — a small residue of legacy lighting/fog registers; mostly
  `moot` or folded into the shader.
- **copy engines (`nv3089-image`, `nv308a-image`, `nv2d-surface`, `nv0039-dma`)** —
  62 registers of standalone blit/convert/copy hardware. These are frequently
  forgotten and are a real correctness surface (guest-visible memory writes).
- **`misc`** — sparse residue; resolved case by case.

## 8. How to use the spine

```bash
cd docs/research/metal4/inventory
python3 generate.py              # regenerate facts; non-zero on sanity failure
python3 -m unittest -v           # 37 conformance tests
python3 validate.py --strict     # fail unless every id is curated
cat out/SUMMARY.md               # the whole spine as tables, by cluster
```

To extend: add a shard, fill `out/dispositions/<cluster>.csv`, re-run
`validate.py`. The merge is mechanical and id-keyed.

## 9. Open questions

1. **[V] Is the method enum the whole command space?** The FIFO can also carry
   non-NV4097 packets (the `NV0039`/`NV3062`/`NV308A`/`NV3089` families are
   present, and the three orphan bindings are outside the enum). Whether the enum
   is *provably* exhaustive of guest-writable state, or merely *observed*
   exhaustive, is unresolved. Experiment: instrument the FIFO decoder to log any
   method word with no enum name, and run a corpus.
2. **[I] Are the 170 unbound registers all reachable?** Some may be write-only or
   vestigial. Experiment: instrument the dispatcher and measure the reachable set
   over the corpus.
3. **[I] Is the VALUE space complete?** Formats can arrive indirectly (e.g. via
   `SET_TEXTURE_FORMAT` decoded into a `color_format` subsystem) rather than as a
   literal constant. The 393 anonymous constants suggest categories not yet
   labelled. Experiment: cross-check `rsx_detail`/`utils` format tables against
   `rsx_value_space.csv`.
4. **[U] Cluster boundaries** are heuristic (name-substring rules). They are
   *good enough* to assign ownership but are not a hardware taxonomy. If a shard
   turns out to straddle concerns, the rule set in `generate.py` should be
   adjusted rather than the cluster worked around.
5. **[U] Decode coverage for `nv308a-image`** is 1/16 and deep-nested decoders
   (`bf_decoder<I,N>` templates) are excluded. Whether those registers have
   concrete layouts the extractor cannot see is unresolved.
6. **[I] Does the spine need a fourth space** for *memory* (the guest address
   map, DMA contexts) rather than just registers? `SET_CONTEXT_DMA_*` is command
   state but the object it names is memory. Experiment: fold `GcmTileInfo` and
   the address map into the inventory.

## 10. Provenance

- Generator: `inventory/generate.py` (deterministic; parses the tree, hand-lists
  nothing).
- Tests: `inventory/test_generate.py` (26), `inventory/test_validate.py` (11).
- Curator gate: `inventory/validate.py`.
- Generated artifacts: `inventory/out/` (`SUMMARY.md`, `rsx_state_space.csv`,
  `rsx_decode_fields.csv`, `rsx_value_space.csv`, `rsx_binding_slots.csv`,
  `clusters/`, `COVERAGE.md`).
- Source-of-truth files: `gcm_enums.h`, `rsx_decode.h`, `rsx_methods.cpp`,
  `VK/vkutils/pipeline_binding_table.h`, `NV47/HW/*.cpp`.
- Design frame: `10-rsx-native-metal4-architecture.md`; verification:
  `11-verification-loop.md`; consumers: `design/HLD-*.md`, `design/LLD-*.md`.

<!-- doc-end -->
