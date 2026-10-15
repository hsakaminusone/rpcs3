# RSX state-space inventory

The **completeness spine** for the RSX → Metal 4 design work: a mechanically-derived
enumeration of everything the guest can put into RSX state, with a curated mapping
layer that records how each item is handled in Metal 4.

## Quick start

```bash
cd docs/research/metal4/inventory
./run.sh                       # generate -> test -> validate -> lint
# or step by step:
python3 generate.py            # extract from the RPCS3 tree -> out/
python3 -m unittest -v         # conformance tests (must stay green)
python3 validate.py --strict   # fail unless the curated layer covers every id
cat out/SUMMARY.md             # the whole spine, grouped by cluster
```

`generate.py` parses — never hand-lists — the authoritative sources:

| source | extracted |
|---|---|
| `rpcs3/Emu/RSX/gcm_enums.h` | the FIFO method enum (COMMAND space) + `CELL_GCM_*` and other enums (VALUE space) |
| `rpcs3/Emu/RSX/rsx_decode.h` | `registers_decoder<>` bitfields (DECODE space) |
| `rpcs3/Emu/RSX/rsx_methods.cpp` | the dispatch table (`bind` / `bind_range` / `bind_array`) |
| `rpcs3/Emu/RSX/VK/vkutils/pipeline_binding_table.h` | argument-table slot layout |
| `rpcs3/Emu/RSX/NV47/HW/*.cpp` | hardware implementation modules |
| `rpcs3/Emu/RSX/Program/RSX{Vertex,Fragment}Program.h` | the shader instruction encoding (opcodes, bitfields, program fields) |

## What it proves

The spine is a **finite, enumerable** checklist. At the current revision it contains:

- **282** method registers across **14** clusters
- **158** with a bitfield decoder (**93** decoded fields)
- **112** with a dispatch binding
- **768** value-space constants across **10** enums
- **275** shader facets (VP/FP opcodes, register types/precisions, instruction bitfields, program fields)
- **12** argument-table slots

Because the space is finite, "we have mapped RSX → Metal 4" becomes a *coverage
statement* (every `id` has a disposition), not an assertion.

## Layout

- `SCHEMA.md` — ID scheme, disposition vocabulary, doc templates, rules.
- `generate.py` — the extractor. Deterministic; non-zero exit on sanity failure.
- `validate.py` — the curation gate: vocabulary checks, exact coverage, merge.
- `lint_docs.py` — design-doc linter (sentinel, table shape, cluster coverage).
- `run.sh` — one-shot pipeline (`generate` → tests → `validate` → `lint`).
- `test_generate.py` / `test_validate.py` — live conformance tests against the RPCS3 tree.
- `out/` — generated artifacts (`SUMMARY.md`, CSVs, JSON, per-cluster shards).
- `out/clusters/<cluster>.csv` — the unit of work for the low-level design docs.
- `out/dispositions/<cluster>.csv` — curated layer (one row per register id).

See `SCHEMA.md` for how the curated layer is filled and
`../12-rsx-state-space-inventory.md` for the design narrative built on top.

<!-- doc-end -->
