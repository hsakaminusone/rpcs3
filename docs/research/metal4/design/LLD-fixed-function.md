# LLD — `fixed-function` cluster (RSX lighting / fog / texgen residue)

Owner/Scope: RSX-first Metal 4 backend, `fixed-function` cluster | Status: draft | Date: 2026-10-16 (Fri)
Branch: `metal4`. Design only — nothing under `rpcs3/**` is modified.

Legend: `[V]` verified against this tree or a cited primary source; `[I]` inference from verified
facts; `[U]` could not verify. `file:line` is advisory (the branch moves); **symbol names are the
stable reference**. Difficulty scale per `inventory/SCHEMA.md` §4: **1** trivial · **2** mechanical ·
**3** real work · **4** hard / needs design · **5** unknown or impossible.

---

## 1. Scope

The shard is **exactly four ids** (`inventory/out/clusters/fixed-function.csv`; `SUMMARY.md:124-131`):
`M_NV4097_SET_FOG_MODE`, `M_NV4097_SET_FOG_PARAMS`, `M_NV4097_SET_SPECULAR_ENABLE`,
`M_NV4097_SET_TWO_SIDE_LIGHT_EN`. None is bound in the binding table; none has an NV47/HW handler
(`SUMMARY.md:128-131`, `binding` empty; `NV47/HW/nv4097.cpp` contains no match for any of them) `[V]`.

**The cluster name is a misnomer.** RSX has no fixed-function transform-and-lighting unit: every draw
runs a guest vertex program and a guest fragment program, and the "transform program" is the vertex
program itself (`10` §3.1; `README.md` §6 R32 "fixed VP/FP model"; `Program/` is VP+FP only) `[V]`.
Consequently the NV40-era registers the scoping note named alongside this shard —
`NV4097_SET_MATERIAL_*`, `NV4097_SET_TEXGEN_*` — **do not exist in RPCS3's method enum at all**
(`gcm_enums.h:560-760`; a repo-wide search finds `SET_MATERIAL`/`SET_TEXGEN` only in vendored GL
headers under `3rdparty/`) `[V]`. They are unreachable guest state, not emulated state, and the LLD's
only obligation toward them is to record their absence (§5 F6). The one *texgen-family* register RSX
does model — `NV4097_SET_TEX_COORD_CONTROL` (0x0b40), which feeds `texcoord_control_mask` into the
fragment program (`rsx_methods.h:1332-1342`, `RSXThread.cpp:2411`) — belongs to the `texture` cluster
(`SUMMARY.md:345`), not this one; cross-reference it there.

What is left is genuine and small: two fog registers and one two-sided-lighting select that RSX folds
into the **shader** (there is no Metal fixed-function object on the other side), plus one dead legacy
bit. This document is deliberately short because the correct disposition for three of the four ids is
"the value is already a member of a constant buffer the shader reads", and for the fourth it is
"nothing reads it".

---

## 2. Register model

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV4097_SET_FOG_MODE` | `0x08cc` | `to_fog_mode(u32) → rsx::fog_mode` (`rsx_decode.h:2760-2773`; `gcm_enums.h:1438-1460`) | unbound | `emulate` | `MTL4ArgumentTable::setAddress(fragment-state buffer, 3)` — `fog_mode` field of `fragment_context_t`; MSL `fetch_fog_value()` |
| `M_NV4097_SET_FOG_PARAMS` | `0x08d0` | `std::bit_cast<f32>` on `FOG_PARAMS` and `FOG_PARAMS+1` (`rsx_decode.h:1048-1089`) | unbound | `emulate` | same slot 3 — `fog_param0`/`fog_param1` fields |
| `M_NV4097_SET_SPECULAR_ENABLE` | `0x1428` | **none** — `has_decoder=0` (`SUMMARY.md:130`) | unbound | `moot` | none — write-only dead state |
| `M_NV4097_SET_TWO_SIDE_LIGHT_EN` | `0x142c` | `two_sided_lighting_enabled() → bool` (`rsx_decode.h:979-993`) | unbound | `emulate` | MSL `[[front_facing]]` select in the FS; `MTL4Compiler::newRenderPipelineState` variant |

### 2.1 State and lifetime

All four are plain `rsx_state.registers[]` slots (dword-indexed from the byte addresses above:
`gcm_enums.h:624-625,653-654`), not objects with dirty bits of their own. Writes land through the
null method entry (`methods[...] = nullptr`: `rsx_methods.cpp:1375-1376` for fog, `:1412-1413` for
specular / two-sided) `[V]`; the register array is the value store and the dirty signalling is a
separate `state_signals[]` table:

- `NV4097_SET_FOG_MODE` → `rsx::fragment_state_dirty` (`rsx_methods.cpp:725`) `[V]`
- `NV4097_SET_FOG_PARAMS + 0/+1` → `rsx::fragment_state_dirty` (`rsx_methods.cpp:730-731`) `[V]`
- `NV4097_SET_TWO_SIDE_LIGHT_EN` → `rsx::fragment_program_state_dirty` (`rsx_methods.cpp:713`) `[V]`
- `NV4097_SET_SPECULAR_ENABLE` → **no signal declared** `[V]`

The distinction that matters here is *dirty-class*, not dirty-bit: FOG_MODE/FOG_PARAMS changing
invalidates **encoder** fragment state (a buffer whose contents must be rewritten this draw);
TWO_SIDE_LIGHT_EN changing invalidates the **program** (a different FP is compiled). SPECULAR_ENABLE
invalidates nothing. Reset defaults are `FOG_MODE = 0x800` (`CELL_GCM_FOG_MODE_EXP`) and
`FOG_PARAMS = 0` (`rsx_methods.cpp:269-272`, mirrored `:906-909`) `[V]` — the backend must seed the
same default, or the exponent fog branch changes on the first frame.

### 2.2 Decode

FOG_MODE is the only enum-valued reg: `fog_mode` has six states — `linear`, `exponential`,
`exponential2`, `exponential_abs`, `exponential2_abs`, `linear_abs` (`gcm_enums.h:1438-1460`), and
`to_fog_mode` returns `expected`/error on an unknown raw value `[V]`. FOG_PARAMS carries two `f32`
across two consecutive dwords; `fog_param_0()`/`fog_param_1()` are `bit_cast<f32>` (`rsx_decode.h:1058`,
`:1081`) `[V]`. TWO_SIDE_LIGHT_EN decodes the whole dword as a bool (no bitfield) `[V]`.
SPECULAR_ENABLE has no decoder at all — the generator's `has_decoder` is 0, which is the first
mechanical signal that nothing consumes it.

### 2.3 Consumption — where "affects output" is actually decided

**Fog.** The values are gathered once per draw into a packed `fragment_context_t`
(`RSXDrawCommands.cpp:668-677`; `fill_fragment_state_buffer` at `:665`) and emitted as
`payload.fog_param0 / fog_param1 / fog_mode` (`RSXDrawCommands.cpp:714-716`) `[V]`. That struct is the
`fragment_state` payload bound at slot 3 (`vkutils/pipeline_binding_table.h:12`). Fog is *conditional*:
the guest FP only sees it if its `in_register_mask` includes `in_fogc` (`FragmentProgramDecompiler.h:146`),
which sets `require_fog_read` (`VKFragmentProgram.cpp:404`) and in turn `_ENABLE_FOG_READ` and the six
`FOG_*` defines (`GLSLCommon.cpp:384-397`) `[V]`. On the shader side the equation lives entirely in
`fetch_fog_value()` (`RSXFragmentPrologue.glsl:67-103`): linear, `exp(11.084·(…))`, `exp(-pow(4.709·…,2))`,
and their `abs()` variants, clamped `[0,1]`.

**Two-sided lighting.** `two_side_light_en()` is stored into `current_fragment_program.two_sided_lighting`
(`RSXThread.cpp:2412`; field `RSXFragmentProgram.h:269`) and drives two things `[V]`:

1. FS codegen: when set, front and back colours are separate varyings and the fragment selects on
   `gl_FrontFacing` — `GLSLCommon.cpp:700-727` (varying list) and `VKFragmentProgram.cpp:528-535`
   (`diff_color = gl_FrontFacing ? diff_color0 : diff_color1`) `[V]`.
2. VP config: `vp_config[3] = two_side_light_en()` (`VKGSRender.cpp:2263`; GL mirror `GLGSRender.cpp:1090`),
   i.e. the vertex program is told whether to emit back-side outputs `[V]`.

The bit is part of the FP program cache key (`ProgramStateCache.cpp:727,771-772,906-907`), so it already
selects a distinct compiled FP rather than being a runtime uniform `[V]`.

**Specular.** Never read. `NV4097_SET_SPECULAR_ENABLE` appears only in the method enum
(`gcm_enums.h:653`), the null-method table (`rsx_methods.cpp:1412`), the dumper (`gcm_printing.cpp:323`),
and a GL comment (`GLDraw.cpp:320`) `[V]`. No accessor, no decoder, no shader input, no state signal.

---

## 3. Metal 4 mapping

There is **no Metal fixed-function object** for any of this; the shape change is "guest register →
shader constant / shader variant", not "guest register → Metal state". Concretely:

### 3.1 Fog (FOG_MODE, FOG_PARAMS) — argument-table constant, not encoder state

The three fog values already live inside the fragment-state payload. Under the RSX-first binding
contract the payload is bound per draw by `MTL4ArgumentTable::setAddress(gpuAddress + fs_context_offset, 3)`
(HLD-05 §3.2, slot 3) `[V]`/`[I]`, exactly as the VK path binds it today; the equation is MSL text
produced by `Program/` + SPIRV-Cross with no Metal-4 awareness (HLD-05 §2.2) `[V]`. So there is **no**
`MTL4RenderCommandEncoder` setter and **no** `MTL4RenderPipelineDescriptor` field for fog — a design
that reached for one would be inventing state Metal does not have. The register pair's whole Metal-4
obligation is: the value is present in the right `fs_context` slot, at the same offset the shader was
compiled against.

### 3.2 Two-sided lighting (TWO_SIDE_LIGHT_EN) — shader/pipeline variant

Selecting front vs back colour is MSL `[[front_facing]]` (SPIRV-Cross's rendering of `gl_FrontFacing`),
baked into the fragment function at `MTL4Compiler::newRenderPipelineState` time `[I]`. The bit must be
folded into the **shader cache key** (not merely the PSO key), because it changes emitted MSL — the same
program id compiled with the bit off must not be reused with it on (HLD-05 §3.5, invariant 8) `[V]`/`[I]`.
The vertex side is the same story: `vp_config[3]` selects the VP variant that emits back-side outputs,
so it is part of that variant's key too.

### 3.3 SPECULAR_ENABLE — no mapping

No object, no field, no key bit. The register is retained only as opaque shadow state for capture
fidelity; it is never read.

### 3.4 Slot-table note

None of the four ids has a binding-table entry (`SUMMARY.md:128-131`); the only binding artefact they
touch is the pre-existing fragment-state buffer at slot 3 (`pipeline_binding_table.h:12`). No slot-map
growth (HLD-05 §4 invariant 2, §5 F16) is required for this cluster.

---

## 4. Impossible / not-recommended

Nothing in this cluster is *impossible* and nothing needs a fallback: fog and two-sided lighting are
fully expressible in MSL and are already realised there, and the fourth register is not lost because
nothing ever read it. The dispositions are `emulate` ×3 and `moot` ×1.

Not-recommended, explicitly:

- **Do not invent Metal state for fog** (`MTL4RenderCommandEncoder` fog setter, a PSO fog field, a
  function constant per draw). The value is a per-draw constant buffer field; duplicating it as
  pipeline state would over-create pipelines per fog value.
- **Do not add a `fogc`-independent fog path.** The prologue snippet is gated on the guest FP actually
  reading `fogc` (`_ENABLE_FOG_READ`); emitting the equation unconditionally would add cost and, worse,
  change the shader cache key for programs that never needed it.
- **Do not attempt to emulate a fixed-function T&L / material / texgen stage.** It does not exist on
  RSX; the registers that would drive it are absent from the method space (§1).

---

## 5. Invariants & failure modes

**Invariants.**

- **I1 — Offset fidelity.** `fog_mode`/`fog_param0`/`fog_param1` are written at the `fs_context_offset`
  the compiled shader reads; the values are per-draw, not baked into the shader (HLD-05 §4 inv. 7,
  `ProgramStateCache.h:449-459`) `[V]`.
- **I2 — Variant key completeness.** TWO_SIDE_LIGHT_EN is a component of both the FP shader cache key
  and the VP/PSO variant key (`ProgramStateCache.cpp:727,771-772`; `VKGSRender.cpp:2263`). Two programs
  with equal ucode but different two-sided bits must not share MSL/PSO `[V]`/`[I]`.
- **I3 — Default parity.** The reset defaults `FOG_MODE=0x800`, `FOG_PARAMS=0` are reproduced in the
  Metal fragment-state payload `[V]`.
- **I4 — Dead-state isolation.** SPECULAR_ENABLE enters no cache key and no argument-table value; if it
  ever did, it would spuriously invalidate state `[V]`.

**Failure modes.**

- **F1 — Fog-offset drift.** The fragment-state buffer offset and the shader's `_fs_context_offset`
  diverge → fog reads another draw's values. Detectable in H2 (binding trace) and H3 (per-command
  state diff) `[I]`.
- **F2 — Two-sided variant aliasing.** The bit omitted from the shader key → a front-capable shader is
  reused where back colours were expected (or vice-versa); symptom is wrong sidedness on
  two-sided-lit geometry. Same class as HLD-05 §5 F6/F8 `[I]`.
- **F3 — Fog default mismatch.** Backend zero-inits the state → the default `EXP` branch is lost on
  first draw; symptom is fog appearing for a frame or fog equation wrong at boot `[I]`.
- **F4 — Conditional-fog overreach.** Emitting `fetch_fog_value` when `_ENABLE_FOG_READ` is off adds a
  spurious uniform read and changes the key; detectable as an H1 (MSL translate) golden diff `[I]`.
- **F5 — Unknown raw fog mode.** `to_fog_mode` returns an error value for an unrecognised raw dword
  (`gcm_enums.h:1448-1460`); a title writing junk would take the `expected` path. Preserve RPCS3's
  behaviour rather than clamping `[I]`.
- **F6 — Phantom registers.** A reviewer or generator change that assumes `SET_MATERIAL_*`/`SET_TEXGEN_*`
  exist would invent emulation for unreachable state. The cluster owner must keep them out `[V]`.

---

## 6. Open questions

- **Q1 `[U]`** — Does `NV4097_SET_SPECULAR_ENABLE` (0x1428) have *any* observable hardware effect that
  RPCS3 happens to ignore? It is `moot` here on the strength of RPCS3 never reading it (`rsx_methods.cpp:1412`)
  `[V]`, not on a hardware reference. If a title is shown to depend on it, the row moves to `emulate`
  (likely as a front-specular output gate in the VP), which would touch the VP variant key. Needs a
  hardware/reference experiment, not more source reading.
- **Q2 `[I]`** — Fog and two-sided state are carried in the fragment-state buffer (fog) and the shader
  cache key (two-sided). Should either be promoted to a `setBytes`-style inline constant instead?
  Blocked on the unresolved `MTL4ArgumentTable::setBytes` surface question (HLD-05 §3.3, Q4); the
  current answer (constant buffer) is the one that does not depend on it.
- **Q3 `[I]`** — Does the VP need to emit back colours unconditionally when two-sided lighting is on,
  or only when the FP reads them? RPCS3 gates the VP via `vp_config[3]` and the output mask
  (`VKGSRender.cpp:2263`); the Metal VP/PSO variant should mirror that gating to avoid dead varyings.
- **Q4 `[I]`** — Is fog's `fogc` input genuinely a fixed-function coordinate unit on RSX, or guest
  shader output? RPCS3 models it as a fixed-function computation surfaced in the `fogc` input register
  (`FragmentProgramDecompiler.h:146`, `RSXFragmentPrologue.glsl:67-103`) `[V]`; the LLD assumes that
  model is correct and needs no change — flagged only so the assumption is auditable.

---

## 7. Provenance

**Cluster shard and generated inventory** `[V]`
- `docs/research/metal4/inventory/out/clusters/fixed-function.csv` — the four ids, addresses, `has_decoder`.
- `docs/research/metal4/inventory/out/SUMMARY.md:124-131` — `fixed-function` (4 regs, 3 with decoder, 0 bound); `:345` `TEX_COORD_CONTROL` in `texture`.
- `docs/research/metal4/inventory/SCHEMA.md` §4 (disposition vocabulary), §6 (LLD template).

**RPCS3 register model** `[V]`
- `rpcs3/Emu/RSX/gcm_enums.h:624-625` (`FOG_MODE`, `FOG_PARAMS`), `:653-654` (`SPECULAR_ENABLE`, `TWO_SIDE_LIGHT_EN`), `:1438-1460` (`fog_mode` enum + `to_fog_mode`).
- `rpcs3/Emu/RSX/rsx_decode.h:979-993` (`TWO_SIDE_LIGHT_EN`), `:1048-1089` (`FOG_PARAMS`, `FOG_PARAMS+1`), `:2760-2773` (`FOG_MODE`).
- `rpcs3/Emu/RSX/rsx_methods.h:338-346` (`fog_params_0/1`), `:894-902` (`two_side_light_en`, `fog_equation`), `:1332-1342` (`texcoord_control_mask`).
- `rpcs3/Emu/RSX/rsx_methods.cpp:269-272,906-909` (defaults); `:713,725,730-731` (`state_signals`); `:1375-1376,1412-1413` (`methods[] = nullptr`).
- `rpcs3/Emu/RSX/NV47/HW/nv4097.cpp` — no handler for any of the four `[V]`.

**Consumption sites** `[V]`
- `rpcs3/Emu/RSX/Core/RSXDrawCommands.cpp:665-677` (`fill_fragment_state_buffer`, `fragment_context_t`), `:714-716` (fog fields).
- `rpcs3/Emu/RSX/RSXThread.cpp:2411-2412` (`texcoord_control_mask`, `two_sided_lighting` capture).
- `rpcs3/Emu/RSX/Program/RSXFragmentProgram.h:266,269` (`texcoord_control_mask`, `two_sided_lighting`).
- `rpcs3/Emu/RSX/Program/GLSLCommon.cpp:384-397` (fog defines, `_ENABLE_FOG_READ`), `:700-727` (two-sided varyings).
- `rpcs3/Emu/RSX/Program/GLSLSnippets/RSXProg/RSXFragmentPrologue.glsl:67-103` (`fetch_fog_value`).
- `rpcs3/Emu/RSX/Program/FragmentProgramDecompiler.h:146` (`in_fogc`).
- `rpcs3/Emu/RSX/VK/VKFragmentProgram.cpp:404` (`require_fog_read`), `:528-535` (`[[front_facing]]`-equivalent select).
- `rpcs3/Emu/RSX/VK/VKGSRender.cpp:2263` (`vp_config[3]`); `GL/GLGSRender.cpp:1090` (GL mirror).
- `rpcs3/Emu/RSX/Program/ProgramStateCache.cpp:727,771-772,906-907` (FP cache key / compare incl. two-sided).
- `rpcs3/Emu/RSX/GL/GLDraw.cpp:320` (comment only).

**Target contract (in-repo)** `[V]`
- `rpcs3/Emu/RSX/VK/vkutils/pipeline_binding_table.h:12` — `fragment_state_bind_slot = 3`.
- `docs/research/metal4/design/HLD-05-shader-binding-contract.md` §2.6 (cluster→object table, this cluster = *shader*), §3.2 (slot map; `setAddress(..., 3)`), §3.5 (cache key), §4 inv. 2/7/8.
- `docs/research/metal4/10-rsx-native-metal4-architecture.md` §3.1 (fixed VP/FP, no GS/TS), §4.2 (binding model), §4.6 (shader/pipeline model).
- `docs/research/metal4/README.md` §6 R32 (geometry/tess/TF unreachable); `09` D11 (`draw_parameters` SSBO design).

<!-- doc-end -->
