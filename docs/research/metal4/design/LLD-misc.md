# LLD-misc — the `misc` cluster: sparse residue registers

Backend: native Apple **Metal 4** (`MTL4*`) on Apple-silicon (M1+), macOS 26+ (`10` §10 `[V]`).
This LLD covers the **16 method registers in `inventory/out/clusters/misc.csv`** and nothing else.
It is a *low-level* document: it pins per-register disposition and the exact Metal 4 object (or the
reason there is none), and it surfaces the registers whose in-tree status is *not* fully determined.

Legend: `[V]` verified against the RPCS3 tree or a cited Apple doc; `[I]` inference; `[U]`
unverified, with the experiment that would discharge it named inline. Register IDs are taken
verbatim from `rsx_state_space.csv`; **no register or constant is invented here.** `file:line` is
advisory (the tree is a moving branch); symbol names are the stable reference.

---

## 1. Scope

`misc` is the **residue of the substring clusterer**: after every register hand-claimed by the
`surface`/`vertex`/`draw`/`texture`/`depth-rop`/`blend`/`raster`/`sync-flip`/copy-engine rules, these
16 remain (`12-rsx-state-space-inventory.md` §7, `misc` = "sparse residue; resolved case by case").
It is not a semantic family. It contains, in decreasing order of consequence:

- **3 registers with live backend-visible behaviour** — `SET_CONTROL0` (depth-float select),
  `SET_FREQUENCY_DIVIDER_OPERATION` (instanced attribute modulo), `INVALIDATE_L2` (program refresh).
- **13 inert registers** — written by `cellGcmInit`, printed by `gcm_printing.cpp`, and read by no
  code path: the `SET_ATTRIB_*` fixed-function source maps, `SET_ANISO_SPREAD`,
  `SET_EDGE_FLAG`, `SET_PERFORMANCE_PARAMS`, `NO_OPERATION`, `PM_TRIGGER`, the two vertex-file
  invalidates, and the `GCM_SET_DRIVER_OBJECT` handle.

**Shard-boundary note (disambiguation, `[V]`).** The brief for this shard anticipated object/context
registers such as `NV4097_SET_OBJECT`, `NV4097_SET_CYLINDRICAL_WRAP`, `NV4097_SET_REF`, or
`NV4097_SET_SHADER_PACKER`. **None of those are in `misc`.** The clusterer assigned
`M_NV4097_SET_OBJECT` and `M_NV4097_SET_CYLINDRICAL_WRAP`/`_WRAP1` to `surface`
(`out/SUMMARY.md:275`,`:300-301`), `M_NV406E_SET_REFERENCE` to `sync-flip` (`:320`), and
`M_NV4097_SET_SHADER_PACKER` plus the `SET_TRANSFORM_*` block to `vertex` (`:385-393`). There is no
`NV4097_SET_DAMPING` register in the enum (`gcm_enums.h`). This LLD therefore says nothing about
those registers; their owners do.

**Non-goals.** Not the FIFO decoder (`12` §5.3 / `08`), not the shader ABI (`10` §4.2), not the
pass scheduler (`10` §4.3 / RN1), and not the capture format (`11` §3).

---

## 2. Register model

### 2.1 The dispatch table is a *coverage* mechanism, not a state model (`[V]`)

All 16 registers are members of the `rsx::method_registers` singleton, and all 16 are registered in
the global dispatch table so the FIFO decoder stays aligned:

- Globals: `rsx_state method_registers`, `std::array<rsx_method_t, 0x10000/4> methods`,
  `state_signals` (`rsx_methods.cpp:15-18`).
- Default fill: `methods.fill(&invalid_method)` (`:1245`); `invalid_method` logs and may
  `recover_fifo()` (`:20-32`). Therefore **every** guest-writable register must have an entry, even
  when the entry is `nullptr` (`:1247-1260`).
- A `nullptr` handler is *not* "unhandled": the register's raw argument is still latched into
  `registers[id]` by the FIFO write machinery, and consumers read it through the `REGS()` accessors
  (`rsx_methods.h`), not through handler callbacks.
- `bind_array(id, step, count, nullptr)` white-lists a multi-word range as a single logical block
  (`:1252-1260`); `misc` uses it once, for `SET_ANISO_SPREAD` ×16 (`:1593`).

This is the single most important fact for a Metal backend: **most RSX state is value-only**. A
backend that only reacts to handler callbacks would observe 3 of these 16 registers.

### 2.2 Per-register decode and lifetime

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_NO_OPERATION` | 0x0100 | `(nop)` dump only (`rsx_decode.h:3958-3968`) | `nullptr` (`rsx_methods.cpp:1271`) | `na` | — |
| `M_NV4097_PM_TRIGGER` | 0x0140 | none | `nullptr` (`:1274`) | `na` | — |
| `M_NV4097_SET_CONTROL0` | 0x03b0 | `depth_float` @bit 12 (`rsx_decode.h:4410`) | `nullptr` (`:1369`) | `reshape` | depth `MTLPixelFormat` selection via `surface_depth_fmt()` |
| `M_NV4097_SET_ANISO_SPREAD` | 0x03c0 | none | `bind_array` ×16 `nullptr` (`:1372`,`:1593`) | `na` | — |
| `M_NV4097_SET_PERFORMANCE_PARAMS` | 0x1450 | none | `nullptr` (`:1415`) | `na` | — |
| `M_NV4097_SET_EDGE_FLAG` | 0x145c | none | `nullptr` (`:1417`) | `na` | — |
| `M_NV4097_INVALIDATE_VERTEX_CACHE_FILE` | 0x1710 | none (dump only, `rsx_decode.h:3971-3983`) | `nullptr` (`:1423`) | `na` | — |
| `M_NV4097_INVALIDATE_VERTEX_FILE` | 0x1714 | none (dump only, `rsx_decode.h:3985-3997`) | `nullptr` (`:1424`) | `na` | — |
| `M_NV4097_SET_FREQUENCY_DIVIDER_OPERATION` | 0x1fc0 | raw value (`rsx_decode.h:538-552`) | `nullptr` (`:1497`) | `reshape` | packed vertex-layout word → `RSXVertexFetch` modulo index math |
| `M_NV4097_SET_ATTRIB_COLOR` | 0x1fc4 | none | `nullptr` (`:1498`) | `na` | — |
| `M_NV4097_SET_ATTRIB_TEX_COORD` | 0x1fc8 | none | `nullptr` (`:1499`) | `na` | — |
| `M_NV4097_SET_ATTRIB_TEX_COORD_EX` | 0x1fcc | none | `nullptr` (`:1500`) | `na` | — |
| `M_NV4097_SET_ATTRIB_UCLIP0` | 0x1fd0 | none | `nullptr` (`:1501`) | `na` | — |
| `M_NV4097_SET_ATTRIB_UCLIP1` | 0x1fd4 | none | `nullptr` (`:1502`) | `na` | — |
| `M_NV4097_INVALIDATE_L2` | 0x1fd8 | none | `nv4097::set_shader_program_dirty` (`:1707`) | `reshape` | `fragment_program_ucode_dirty` → `MTL4RenderPipelineState` rebuild |
| `M_GCM_SET_DRIVER_OBJECT` | 0xe000 | none | `nullptr` (`:1585`) | `na` | — |

Counts: **13 `na`, 3 `reshape`, 0 `emulate`/`impossible`/`not-recommended`/`moot`/`direct`.**
Lifetime of every value is "sticky in `registers[]` until context reset" (`rsx_state::reset()`,
`rsx_methods.cpp:797-…`); none of the 16 is a latch-compared dirty-bit register, and none is
consumed by `state_signals` (`:700-785`) — i.e. **none of them can trigger a dirty-bit-driven
cache invalidation on its own.**

### 2.3 The three live registers (`[V]`)

**`SET_CONTROL0` (`depth_float`, bit 12).** The decoder exposes one field
(`rsx_decode.h:4410-4413`); the accessor `depth_buffer_float_enabled()` (`rsx_methods.h:1327-1330`)
is folded into `surface_depth_fmt()` (`rsx_methods.h:934-937`). Consumers are the backends'
surface-format paths (`VKGSRender.cpp:1449`, `VKDraw.cpp:247`, `GLGSRender.cpp:674`,
`GLDraw.cpp:106`) and the framebuffer layout (`RSXThread.cpp:1505`, `:2225`). Its RSX meaning is
"interpret the zeta surface as 32-bit float depth, not fixed-point". Every other bit of the register
is undecoded — see Q1.

**`SET_FREQUENCY_DIVIDER_OPERATION`.** The decoder returns the raw value
(`rsx_decode.h:538-552`); the accessor `frequency_divider_operation_mask()` (`rsx_methods.h:914-917`)
is read while building the vertex input layout (`RSXDrawCommands.cpp:62`, `:363`). It selects, per
attribute, whether an instanced attribute index **wraps modulo** the divisor or **divides**
(`:460-479`): the modulo case sets bit 31 of the second layout word (`:468`) and is decoded back in
the vertex-fetch shader as `desc.modulo` (`RSXVertexFetch.glsl:177`) →
`vertex_id = (gl_VertexID + offset) % frequency` (`:198`). It is genuinely functional despite the
`nullptr` handler.

**`INVALIDATE_L2`.** Bound to `nv4097::set_shader_program_dirty` (`rsx_methods.cpp:1707`), which sets
`pipeline_state::fragment_program_ucode_dirty` (`nv4097.cpp:19-22`). RSX semantically asks to flush
the L2 texture/program cache; RPCS3 realises this conservatively as "force the fragment program to
be re-resolved". It is also declared always-ignorable in the FIFO (`RSXFIFO.h:90`) and skipped in
register dumps (`RSXThread.cpp:3195`).

### 2.4 The thirteen inert registers (`[V]`)

Each of the following is proven inactive in-tree by the union of three facts: (a) it has no
non-`nullptr` handler in `rsx_methods.cpp`; (b) its only value writes are the `cellGcmInit`
defaults in `rsx_state::init()`/`reset()`; (c) a full-tree search finds no reader of
`registers[id]` or of a decode accessor. The specific citations per register are the `notes`
column of `out/dispositions/misc.csv`, and in aggregate:

- `NO_OPERATION` — decoder is a `(nop)` printer (`rsx_decode.h:3965`); the only caller that treats it
  specially is the register dumper's skip list (`RSXThread.cpp:3194`). Its one meaningful use is as
  the **capture anchor** that carries the first tile state (`RSXThread.cpp:3476-3478`).
- `PM_TRIGGER` — no handler (`:1274`), no decoder, skipped in the dumper (`:3200`).
- `SET_ANISO_SPREAD` — 16-word default block (`:277-292`,`:914-929`), `bind_array` `nullptr`
  (`:1593`); anisotropy is actually sourced from `SET_TEXTURE_CONTROL0` bits 4-6
  (`rsx_decode.h:4738-4740`, consumed at `RSXTexture.cpp:230`).
- `SET_PERFORMANCE_PARAMS` — default `0x80003` (`:249`), no handler (`:1415`).
- `SET_EDGE_FLAG` — default `0x1` (`:251`), no handler (`:1417`); one comment in `GLDraw.cpp:323`.
- `INVALIDATE_VERTEX_FILE` / `_CACHE_FILE` — dump-only decoders (`rsx_decode.h:3971-3997`), no
  handler (`:1423-1424`), declared always-ignore (`RSXFIFO.h:88-89`). The NV40 "vertex file" is a
  post-transform cache with no Metal analogue; RPCS3 re-reads guest vertex data per draw.
- The five `SET_ATTRIB_*` maps — no handlers (`:1498-1502`), only defaults (`:225-229`) and printer
  entries (`gcm_printing.cpp:819-823`). They describe *fixed-function* vertex input generation;
  RPCS3 always runs a decompiled VP, whose input mask comes from the program, not from these words.
- `GCM_SET_DRIVER_OBJECT` — a PS3 GCM handle; stored as the sentinel `0xcafebabe` at reset
  (`:964`), no handler (`:1585`), no reader.

---

## 3. Metal 4 mapping

### 3.1 Inert registers (`na`) — the shape is "do nothing, read nothing"

There is no Metal 4 object for any of these, and there must not be: the correct backend behaviour is
to **let the shared FIFO machinery latch the value into `registers[]` and never consult it.** The
Metal backend inherits this for free because the dispatch table and `rsx_state` are in shared core
(`rsx_methods.cpp:15-18`), and the MTL backend reads state via `REGS()` accessors exactly as the VK
and GL backends do. Concretely, none of these 13 registers appears in the MTL shader ABI, the
argument table (`10` §4.2), or the vertex-layout blob; there is nothing to bind.

The one non-obvious `na` is `NO_OPERATION`, because it is also a **control token for the replay
substrate**, not just dead state: `on_frame_end` seeds the capture with a synthetic
`(NV4097_NO_OPERATION, 0)` command and attaches the first tile-state record to it
(`RSXThread.cpp:3476-3478`). That use is backend-agnostic (`11` §3.2) and must survive any MTL
backend; a backend that treats a NOP specially (e.g. skips latching) would be indistinguishable on
VK/GL but would corrupt an RRC re-capture on MTL.

### 3.2 `SET_CONTROL0` (`reshape`) — bit → `MTLPixelFormat`

**RSX requirement.** A single bit chooses float vs fixed-point depth interpretation for the zeta
surface. **Metal 4 shape.** Metal has no depth-mode toggle: the mode *is* the pixel format, so the
bit collapses into the depth entry of the `VkFormat → MTLPixelFormat` table (`04` §2.3) that GLDraw
and VKDraw already drive through `surface_depth_fmt()` (`VKDraw.cpp:247`, `GLDraw.cpp:106`). The
shape change is "a mode bit on one surface descriptor" → "a discrete `MTLPixelFormat` chosen once
per surface". The backend work is to route float depth to `Depth32Float`
(and to the D32+X8 combined format when stencil is present, `04` §2.3) rather than
`Depth16Unorm`/`Depth32Float`-with-fixed-point-emulation. No MTL4 *object* is created here; the
target is the format table entry. Difficulty 2 (mechanical map), risk med (a wrong choice silently
misreads depth in an RTT), confidence `[V]` for the in-tree behaviour.

### 3.3 `SET_FREQUENCY_DIVIDER_OPERATION` (`reshape`) — hardware divider → software modulo

**RSX requirement.** Per-attribute instance-frequency semantics: for a divisor `f`, attribute data
either **wraps** every `f` instances or is **divided** by `f`. **Metal 4 shape.** Metal has only
`instanceCount`; there is no per-attribute frequency divider (`10` §4.5). RPCS3 already lowers the
whole thing to a **software vertex pull**: `frequency_divider_operation_mask()` is read while the
vertex layout is built (`RSXDrawCommands.cpp:62`,`:363`) and its per-attribute modulo flag is packed
into the layout word (bit 31, `:468`) that the vertex-fetch shader decodes
(`RSXVertexFetch.glsl:177`,`:198`). Under MTL the register maps to **no MTL4 object**; it is an
input to the shared layout blob and then to MSL index arithmetic. The MTL backend's obligation is
simply to keep the packing intact end-to-end; difficulty 2, risk low, confidence `[V]`.

### 3.4 `INVALIDATE_L2` (`reshape`) — cache flush → program-dirty

**RSX requirement.** Force cached program/texture state to be re-read. **Metal 4 shape.** Metal 4
has no user-visible L2 invalidate and `MTL4CommandQueue` does no hazard tracking (`01` §1.3), so
there is nothing to flush; correctness is maintained by the backend's own caches. RPCS3's existing
lowering is exactly this: the method sets `fragment_program_ucode_dirty` (`nv4097.cpp:19-22`),
which causes the next draw to rebuild the `MTL4RenderPipelineState` from the (possibly changed) VP
ucode. The shape change is "invalidate a hardware cache" → "mark a host pipeline-state cache
entry dirty"; the MTL4 target is the pipeline-state rebuild, **not** any barrier or
`MTL4VisibilityOption`. Difficulty 1, risk low, confidence `[V]`.

---

## 4. Impossible / not-recommended

**No `impossible` and no `not-recommended` rows.** Every register here is either inert (`na`) or
resolvable with existing shared machinery (`reshape`). This is the honest outcome for the residue
cluster: the "hard" registers were claimed by the semantic clusters.

Two positions worth stating so they are not re-litigated:

1. **`INVALIDATE_VERTEX_FILE` / `_CACHE_FILE` are `na`, not `emulate`.** Emulation would mean
   modelling a device-resident post-transform vertex cache that a flush must invalidate. RSX-first
   has no such cache — vertex data is read from guest memory on every draw — so there is no cache to
   emulate. The alternative (a decoded-vertex cache) is a *performance* optimisation, not a
   correctness requirement (`10` §4.5), and if it were added the invalidation would become a host
   cache-flush inside the MTL vertex path, still with no MTL4 object. `[I]`
2. **`INVALIDATE_L2` must not be promoted to a real barrier.** Treating it as a
   `barrierAfterQueueStages` or a residency event would over-synchronise and break the RSX-first
   "sync is anchored on surface hazards" model (`10` §4.4). Its only correct effect is the dirty
   flag. `[V]`

---

## 5. Invariants & failure modes

**I1 — Dispatch coverage is exact.** Every one of the 16 registers must be present in `methods[]`
with either a handler or `nullptr` (`rsx_methods.cpp:1245-1510`). Failure mode: dropping a `nullptr`
entry makes `invalid_method` fire (`:20-32`) and, in non-`as_ps3` FIFO accuracy, calls
`recover_fifo()` — a visible desync. The capture anchor (`RSXThread.cpp:3476`) makes this worse in
replay. `[V]`

**I2 — Value-only registers are still state.** The 13 inert registers and the three reshape inputs
are consumed via `REGS()` accessors, not handler calls. Failure mode: an MTL backend that assumes
"no handler ⇒ no state" and omits these from a snapshot/savestate diverges from VK/GL. Detectable at
H2/H3 (decision/state diff) but not at H6 (image) if the title never exercises them. `[V]`

**I3 — `depth_float` must be honoured before the first depth read *and* across a surface-format
change.** `surface_depth_fmt()` (`rsx_methods.h:937`) is evaluated per framebuffer layout
(`RSXThread.cpp:1505`) and per float-depth test (`:2225`). Failure mode: selecting a fixed-point
`MTLPixelFormat` when `depth_float=1` misreads zeta and breaks depth tests without a pipeline error.

**I4 — The vertex-layout modulo bit is load-bearing.** Bit 31 of the second layout word
(`RSXDrawCommands.cpp:468`) must reach `RSXVertexFetch.glsl:177`. Failure mode: silently wrong
instance indices — geometry corrupts with no draw-count change and no validation error; the shader
still compiles.

**I5 — `INVALIDATE_L2` must not invalidate more than the fragment program.** It is also
FIFO-ignorable (`RSXFIFO.h:90`) and skip-listed in dumps (`RSXThread.cpp:3195`); a backend that
weaponises it into a global barrier or a full pipeline-cache purge would over-synchronise or
re-compile needlessly.

**I6 — The `NO_OPERATION` anchor is a substitution, not a NOP, in replay.** In `capture_current_frame`
mode the method is emitted synthetically with tile state attached (`RSXThread.cpp:3476-3478`). A
backend must not filter it out of the replayed stream.

---

## 6. Open questions

**Q1 `[U]` — `SET_CONTROL0` beyond bit 12.** RPCS3 decodes only `depth_float`; the NV40
`NV4097_SET_CONTROL0` register carries other control bits that this tree never reads. Are any of
them guest-observable or backend-relevant (e.g. an sRGB/round-mode bit)? *Discharge:* instrument
`methods[]`/`registers[]` across the H7 corpus and diff non-`depth_float` bits against a VK
reference trace; check the RSX debugger views. Until then the MTL backend must copy the full word
faithfully even though it reads only bit 12.

**Q2 `[U]` — `SET_ANISO_SPREAD` really never read?** The `aniso_bias` field lives in the texture
address word (`rsx_methods.h:54`) and `max_aniso()` comes from `SET_TEXTURE_CONTROL0`
(`RSXTexture.cpp:230-230`, `rsx_decode.h:4738`); the 16 `ANISO_SPREAD` words are assumed dead.
*Discharge:* count writes to `registers[NV4097_SET_ANISO_SPREAD + i]` on the corpus; a non-zero hit
rate with visual anisotropy differences would falsify the `na`.

**Q3 `[U]` — fixed-function `SET_ATTRIB_*` maps on titles with no VP.** RPCS3 always synthesises a
VP from ucode, so these maps are never consulted. Is there a real title that emits no vertex program
(pure fixed-function T&L) whose attribute routing depends on them? *Discharge:* search the RRC corpus
for draws with an empty/absent VP and diff `attribute_placement` against the VK backend.

**Q4 `[I]` — the two vertex-file invalidates as scheduling hints.** They are `na` because there is no
device-resident vertex cache. If the MTL backend ever introduces a decoded-vertex cache for
performance, they must become a host-side flush. Is that cache in scope? (Currently: no — `10` §4.5
keeps vertex pull software-only.) Recorded so the `na` is revisited, not forgotten.

**Q5 `[I]` — `GCM_SET_DRIVER_OBJECT` fidelity.** The reset sentinel is `0xcafebabe`
(`rsx_methods.cpp:964`). It is stored in `registers[]` and cannot be read back by the guest through
the FIFO (RSX registers are write-only from the guest's perspective). Confirm no host-side consumer
(a GCM context switch / `sys_rsx` path) ever compares it before treating it as `na`.

---

## 7. Provenance

**`[V]` Primary — this tree** (paths under `rpcs3/Emu/RSX/`; line refs advisory).

- `rsx_methods.cpp` — globals `:15-18`; `invalid_method` `:20-32`; `init()` defaults
  `SET_CONTROL0` `:221`, `ANISO_SPREAD` `:277-292`, `PERFORMANCE_PARAMS` `:249`, `EDGE_FLAG` `:251`,
  `ATTRIB_*` `:225-229`, `GCM_SET_DRIVER_OBJECT` `:327`; `reset()` `:797-…` (GCM sentinel `:964`,
  `ANISO_SPREAD` `:914-929`, `ATTRIB_*` `:862-866`); dispatch table `:1241-1746` — `NO_OPERATION`
  `:1271`, `PM_TRIGGER` `:1274`, `SET_CONTROL0` `:1369`, `ANISO_SPREAD` `:1372`, `PERFORMANCE_PARAMS`
  `:1415`, `EDGE_FLAG` `:1417`, `INVALIDATE_VERTEX_CACHE_FILE` `:1423`, `INVALIDATE_VERTEX_FILE`
  `:1424`, `SET_FREQUENCY_DIVIDER_OPERATION` `:1497`, `SET_ATTRIB_COLOR` `:1498`,
  `SET_ATTRIB_TEX_COORD` `:1499`, `SET_ATTRIB_TEX_COORD_EX` `:1500`, `SET_ATTRIB_UCLIP0` `:1501`,
  `SET_ATTRIB_UCLIP1` `:1502`, `INVALIDATE_L2` `:1503` (rebound `:1707`), `GCM_SET_DRIVER_OBJECT`
  `:1585`; `bind_array(ANISO_SPREAD)` `:1593`.
- `rsx_decode.h` — `SET_FREQUENCY_DIVIDER_OPERATION` `:538-552`; `NO_OPERATION` `:3958-3968`;
  `INVALIDATE_VERTEX_CACHE_FILE` `:3971-3983`; `INVALIDATE_VERTEX_FILE` `:3985-3997`;
  `SET_CONTROL0.depth_float` `:4400-4420`; texture `max_aniso()` `:4738-4740`.
- `rsx_methods.h` — `frequency_divider_operation_mask()` `:914-917`; `surface_depth_fmt()` `:934-937`;
  `depth_buffer_float_enabled()` `:1327-1330`; texture `aniso_bias` `:54`.
- `rsx_methods.cpp:700-785` — `state_signals` table (none of the 16 appear: they cannot drive a
  dirty bit on their own).
- `NV47/HW/nv4097.cpp:19-22` — `set_shader_program_dirty` (the `INVALIDATE_L2` target).
- `Core/RSXDrawCommands.cpp` — `frequency_divider_operation_mask()` reads `:62`,`:363`; modulo pack
  `:460-479` (bit 31 at `:468`); layout-blob write `:498-500`.
- `Program/GLSLSnippets/RSXProg/RSXVertexFetch.glsl:177`,`:198` — `desc.modulo` decode and
  `% frequency` index math.
- `RSXFIFO.h:85-105` — always-ignore ranges (`INVALIDATE_VERTEX_FILE` `:88`,
  `INVALIDATE_VERTEX_CACHE_FILE` `:89`, `INVALIDATE_L2` `:90`).
- `RSXThread.cpp` — register-dump skip list `:3194-3202`; `surface_depth_fmt()` consumers `:1505`,
  `:2225`; capture anchor `:3476-3478`.
- Other `surface_depth_fmt()` consumers: `VK/VKGSRender.cpp:1449`, `VK/VKDraw.cpp:247`,
  `GL/GLGSRender.cpp:674`, `GL/GLDraw.cpp:106`, `GL/GLDraw.cpp:323` (edge-flag comment).
- `RSXTexture.cpp:230` — `max_aniso()` sourced from `SET_TEXTURE_CONTROL0`.
- `gcm_printing.cpp:78,81,155-173,326,328,426,427,818-824,893` — the printer table (the only reader
  of most of these registers).
- `gcm_enums.h:541,544,618,621,656,658,664,665,737-743,824` — method enum (addresses).

**`[V]` Sibling docs / spine.**

- `inventory/out/clusters/misc.csv` — the 16 ids and their `has_decoder`/`binding_kind` facts.
- `inventory/out/rsx_decode_fields.csv:36` — `M_NV4097_SET_CONTROL0.depth_float`.
- `docs/research/metal4/12-rsx-state-space-inventory.md` §5.2, §6, §7 (`misc` note).
- `10-rsx-native-metal4-architecture.md` §4.2 (argument table / no descriptors), §4.5 (draw →
  software vertex pull), §4.6 (shader pipeline), §10 (open decisions).
- `11-verification-loop.md` §3.2-§3.3 (RRC per-command annotation / stop-and-apply), §5 (recording
  seam), §9 (design-for-verifiability).
- `04-images-formats-samplers-rtt.md` §2.3 (`VkFormat → MTLPixelFormat` table).

<!-- doc-end -->
