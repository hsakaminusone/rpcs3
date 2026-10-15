# `inventory/tests/` — RSX state-space contract suite

The second test layer over the inventory. `test_generate.py` / `test_validate.py`
test the *tools*; this package tests the *state space itself* — one case per dial
(every method register, every value constant, every shader facet, every binding
slot), asserted against the SCHEMA vocabulary and the curated disposition layer,
plus the cross-products of the combinable spaces and the generator's determinism.

The package is organised so each file maps 1:1 to a future Metal 4
implementation module: when the backend is written, the matching test file
already exists.

## Run

```bash
cd docs/research/metal4/inventory/tests
python3 -m unittest discover -p 'test_*.py'
# or from inventory/ (wired into run.sh):
python3 -m unittest discover -s tests -p 'test_*.py'
```

Deterministic: no GPU, no network, no randomness, no wall-clock. The whole
package runs in a couple of seconds.

## Modules

| test module | future implementation module (mirror) |
|---|---|
| `_corpus.py` | shared helper: typed dial corpus, contract checks, deviation registry, permutation spaces |
| `test_cluster_surface.py` | `surface` — render-pass/attachment synthesis (`design/LLD-surface.md`) |
| `test_cluster_depth_rop.py` | `depth-rop` — depth/stencil/ROP + ZCULL (`design/LLD-depth-rop.md`) |
| `test_cluster_raster.py` | `raster` — viewport/scissor/cull/clip rasterizer state (`design/LLD-raster.md`) |
| `test_cluster_vertex.py` | `vertex` — attribute fetch, constants, VTF, instancing (`design/LLD-vertex.md`) |
| `test_cluster_texture.py` | `texture` — sampler/format/swizzle decode (`design/LLD-texture.md`) |
| `test_cluster_blend.py` | `blend` — blend/alpha-test/logic-op/shade (`design/LLD-blend.md`) |
| `test_cluster_draw.py` | `draw` — begin/end, draw calls, inline arrays (`design/LLD-draw.md`) |
| `test_cluster_sync_flip.py` | `sync-flip` — semaphores, fences, FIFO barriers, flip (`design/LLD-sync-flip.md`) |
| `test_cluster_fixed_function.py` | `fixed-function` — legacy lighting/fog residue (`design/LLD-fixed-function.md`) |
| `test_cluster_misc.py` | `misc` — uncategorised residue (`design/LLD-misc.md`) |
| `test_cluster_nv0039_dma.py` | `nv0039-dma` — DMA copy engine (`design/LLD-nv0039-dma.md`) |
| `test_cluster_nv2d_surface.py` | `nv2d-surface` — 2D surface copy/swizzle (`design/LLD-nv2d-surface.md`) |
| `test_cluster_nv3089_image.py` | `nv3089-image` — scaled image blit (`design/LLD-nv3089-image.md`) |
| `test_cluster_nv308a_image.py` | `nv308a-image` — image-from-CPU engine (`design/LLD-nv308a-image.md`) |
| `test_value_space.py` | value translation tables: format/blend/compare enums (`design/LLD-value-space.md`, `design/HLD-05`) |
| `test_shader_facets.py` | shader decompiler ISA coverage (`design/HLD-05`) |
| `test_binding_slots.py` | argument-table binding contract (`design/HLD-04` §3.3, `design/HLD-05` §3.2) |
| `test_permutations.py` | combined-state representability (PSO/encoder synthesis) |
| `test_determinism.py` | generator reproducibility |
| `test_spine_contract.py` | spine-wide invariants (id uniqueness, shard partition, stats agreement) |

## Cases

Each dial is an individually-identified `subTest`. At the current revision:

- **1337 base cases** = 282 registers + 768 value constants + 275 shader facets
  + 12 binding slots.
- **10 enum-domain cases** (the value-space enums).
- **9496 permutation cases** = 120 (blend factor × equation) + 1080 (texture
  format × address × filter) + 8192 (depth func × stencil fail/zfail/zpass ×
  two-sided) + 64 (surface format × target count) + 40 (primitive × cull ×
  front face). Every space is < 10 000, so all are fully enumerated; no pairwise
  reduction is needed.

`test_spine_contract.test_dial_counts_are_pinned` fails if the shape changes, so
the count is auditable rather than asserted.

## Deviations

The contract is asserted for every dial; the handful of genuine deviations on the
current tree are pinned in the `*_DEVIATIONS` registries so a new deviation fails
and a fixed one also fails (the registries cannot go stale). `_corpus.deviations()`
returns the full list; `test_spine_contract` asserts it equals the pins.

On the current tree:

- `M_NV4097_SET_INDEXED_CONSTANT_READ_LIMITS` (`na`) has an **empty `doc_ref`**,
  so it resolves to no design decision.
- The binding table has **two names at index 12** (`textures_first_bind_slot`,
  `vertex_textures_first_bind_slot`) and leaves **6/7 unused**. The index reuse is
  intentional (argument tables are bound per stage, `design/HLD-04` §3.3) but the
  CSV does not encode the stage, so a global index-uniqueness contract fails.
- The value space is only *partly* enum-shaped: `CellGcmTexture` (`SZ`/`NR`),
  `CellGcmSystemMode` (`IOMAP_512MB`/`MASK`) and `Method` carry deliberate C++
  aliases (declared in `VALUE_ALIASES`), and the **anonymous bucket** collapses
  many anonymous `enum {}` blocks into one row set (393 names, 123 distinct
  values), so per-enum identity is lost for those constants. The four genuinely
  ordinal enums are asserted gap-free and duplicate-free.
