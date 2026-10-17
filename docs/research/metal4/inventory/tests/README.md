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
| `test_value_space.py` | value translation tables: format/blend/compare enums (`design/LLD-texture.md`, `design/LLD-blend.md`, `design/HLD-05`) |
| `test_shader_facets.py` | shader decompiler ISA coverage (`design/HLD-05`) |
| `test_binding_slots.py` | argument-table binding contract (`design/HLD-04` §3.3, `design/HLD-05` §3.2) |
| `test_permutations.py` | combined-state representability (PSO/encoder synthesis) |
| `test_determinism.py` | generator reproducibility |
| `test_spine_contract.py` | spine-wide invariants (id uniqueness, shard partition, stats agreement) |

## Cases

Each dial is an individually-identified `subTest`. At the current revision:

- **1337 base cases** = 282 registers + 768 value constants + 275 shader facets
  + 12 binding slots.
- **10 enum-domain cases** and **49 value-block cases**: an anonymous `enum {}`
  is a block, identified by its `gcm_enums.h:<line>`.
- **9496 permutation cases** = 120 (blend factor × equation) + 1080 (texture
  format × address × filter) + 8192 (depth func × stencil fail/zfail/zpass ×
  two-sided) + 64 (surface format × target count) + 40 (primitive × cull ×
  front face). Every space is < 10 000, so all are fully enumerated; no pairwise
  reduction is needed.

`test_spine_contract.test_dial_counts_are_pinned` fails if the shape changes, so
the count is auditable rather than asserted.

## Deviations

Every dial must satisfy its contract; `_corpus.deviations()` returns any dial that
does not, and `test_spine_contract` asserts that set equals the pins. The
registries are **empty**: the tree is contract-clean, and a new deviation fails the
suite. Three earlier deviations were resolved rather than tolerated:

- `M_NV4097_SET_INDEXED_CONSTANT_READ_LIMITS` now cites its real `nullptr` handler
  (`rsx_methods.cpp:1464`), like its sibling `na` registers.
- The argument-table index reuse is a **declared placeholder**: the source marks
  `vertex_textures_first_bind_slot` invalid/derived, so the extractor flags it and
  index uniqueness counts only real fields. The 6/7 gap stays declared.
- The anonymous value bucket now has **per-block identity**: every constant records
  the `gcm_enums.h:<line>` block it came from, validated against the header, so an
  anonymous `enum {}` is no longer lost in a single lump. C++ aliases
  (`CellGcmTexture` `SZ`/`NR`, `CellGcmSystemMode`, `Method`) stay declared in
  `VALUE_ALIASES` and are asserted exactly.
