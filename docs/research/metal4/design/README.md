# Design documents — RSX → Metal 4

This directory is the **curated design layer** built on top of the mechanically
generated completeness spine. It is the second half of the RSX-first working set:

- **Facts** — `../12-rsx-state-space-inventory.md` + `../inventory/`
  (generator, tests, validator, per-cluster shards).
- **Design** — this directory: high-level design (HLD) and one low-level design
  (LLD) per cluster, each carrying that cluster's disposition shard.

Read `../inventory/SCHEMA.md` first: it defines the ID scheme, the disposition
vocabulary (`direct`/`reshape`/`emulate`/`impossible`/`not-recommended`/`moot`/`na`),
the difficulty/risk/confidence scales, and the document templates.

---

## High-level design (HLD)

| doc | topic |
|---|---|
| [`HLD-01-architecture.md`](HLD-01-architecture.md) | Component decomposition (C1–C7), the hazard-derived pass scheduler, reuse-vs-replace vs the VK/GL backends, the Metal 4 object inventory. |
| [`HLD-02-resource-model.md`](HLD-02-resource-model.md) | Guest memory → buffers, surfaces/textures, tiling/swizzle, aliasing, residency (`MTLResidencySet`), the format table. |
| [`HLD-03-synchronization-model.md`](HLD-03-synchronization-model.md) | The Metal 4 hazard mechanism, the RSX sync surface, producer/consumer edges, and the pass scheduler's barrier insertion. |
| [`HLD-04-command-encoding.md`](HLD-04-command-encoding.md) | Recording/submission, argument tables, pipeline state, indirect command buffers, the draw model, vertex fetch. |
| [`HLD-05-shader-binding-contract.md`](HLD-05-shader-binding-contract.md) | CgBinary → MSL, the shader facet surface, the canonical slot map, PSO assembly and cache keying. |
| [`HLD-06-presentation-flip.md`](HLD-06-presentation-flip.md) | GCM flips, `CAMetalLayer`/drawable presentation, frame pacing, the swapchain-equivalent, the copy engines. |

## Low-level design (LLD)

One per cluster; each carries a register table for its shard and a disposition
shard under `../inventory/out/dispositions/<cluster>.csv`.

| doc | cluster | registers |
|---|---|---|
| [`LLD-draw.md`](LLD-draw.md) | `draw` | 13 |
| [`LLD-vertex.md`](LLD-vertex.md) | `vertex` | 30 |
| [`LLD-texture.md`](LLD-texture.md) | `texture` | 16 |
| [`LLD-surface.md`](LLD-surface.md) | `surface` | 40 |
| [`LLD-depth-rop.md`](LLD-depth-rop.md) | `depth-rop` | 38 |
| [`LLD-blend.md`](LLD-blend.md) | `blend` | 15 |
| [`LLD-raster.md`](LLD-raster.md) | `raster` | 30 |
| [`LLD-fixed-function.md`](LLD-fixed-function.md) | `fixed-function` | 4 |
| [`LLD-sync-flip.md`](LLD-sync-flip.md) | `sync-flip` | 18 |
| [`LLD-misc.md`](LLD-misc.md) | `misc` | 16 |
| [`LLD-nv0039-dma.md`](LLD-nv0039-dma.md) | `nv0039-dma` | 12 |
| [`LLD-nv2d-surface.md`](LLD-nv2d-surface.md) | `nv2d-surface` | 13 |
| [`LLD-nv308a-image.md`](LLD-nv308a-image.md) | `nv308a-image` | 16 |
| [`LLD-nv3089-image.md`](LLD-nv3089-image.md) | `nv3089-image` | 21 |

## Conventions

- Every document ends with exactly `<!-- doc-end -->` (enforced by
  `../inventory/lint_docs.py`).
- Markdown tables must have a consistent column count (also enforced).
- Code claims carry `file:line`; confidence is tagged `[V]` verified / `[I]`
  inference / `[U]` unverified.
- Every LLD's disposition shard must cover its shard's ids **exactly once**
  (enforced by `../inventory/validate.py --strict`).

## Verification

```bash
cd ../inventory
./run.sh            # generate -> tests -> validate -> lint
./run.sh --strict   # additionally fail unless curation is complete
```

`lint_docs.py` reports which cluster docs/shards are still missing; `validate.py`
reports the curated fraction and every uncovered id.

<!-- doc-end -->
