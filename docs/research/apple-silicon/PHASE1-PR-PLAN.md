# Phase 1: the remaining PRs, split small

Counters, W^X, icache, `.stack_sizes`, decommit, atomics, the wait engine and the
observational site table have landed. This is what is left of §9.2, split so each
piece is reviewable and bisectable on its own. Order follows §9.3.

Sizes: S ≤ 150 LOC, M ≤ 500, L ≤ 1,500. "Gate" is what has to be observable
before the piece is called done.

## W2 — fault backpatch (PR 9 in the design)

| PR | Title | Files | Size | Gate |
|---|---|---|---|---|
| 9a | `jit: 4-byte AArch64 branch patch primitive` | `Utilities/jit_patch.h` | S | encoder matches the system assembler; a patched page redirects execution |
| 9b | `cpu: record a site for every direct access` | `PPUTranslator.*`, `CPUTranslator.*` | M | recorded sites equal the static access count of a known function |
| 9c | `cpu: emit the per-site stub island` | `PPUTranslator.cpp`, `JIT.h` | L | stub calls the helper and resumes after the access, with `x30` saved |
| 9d | `cpu: patch a recurring site after the threshold` | `Thread.cpp`, `fastmem.*` | M | the third fault at one site does not fault |
| 9e | `cpu: recompile a patched block so LLVM sees the helper` | `PPUTranslator.cpp`, `PPUThread.cpp` | M | patched site survives invalidation; no flapping |

9a is the only piece that needs no translator metadata, so it goes first. 9b has
to land before 9c: a stub needs the access width and direction at fault time and
those are known at translation time, not decodable safely from the patched
instruction.

## W3 — sub-page notification map (PR 10)

| PR | Title | Files | Size | Gate |
|---|---|---|---|---|
| 10a | `vm: track sub-page state per host page` | `vm.cpp`, `vm.h` | M | state transitions under mmap/unmap, unit tested |
| 10b | `cpu: emit the T1 sub-page guard for notification pages` | `PPUTranslator.cpp`, `CPUTranslator.*` | L | golden IR; #19881 narrowed or explained |

## W4 — dispatch (PR 11–14)

| PR | Title | Files | Size | Gate |
|---|---|---|---|---|
| 11a | `cpu: loop-first region formation` | `PPUTranslator.cpp` | XL | golden IR for straight-line and loop bodies |
| 11b | `cpu: real CFG edges between blocks` | `PPUTranslator.cpp`, `CPUTranslator.h` | L | differential on a block corpus |
| 12 | `cpu: link area for direct exits` | `PPUTranslator.cpp` | M | block-link tests |
| 13 | `cpu: monomorphic IBTC for indirect branches` | `PPUTranslator.cpp` | M | hit rate + correctness |
| 14 | `cpu: return stack buffer for bl/blr` | `PPUTranslator.cpp` | M | call/return stress |

## W5 — IR state (PR 15–16)

| PR | Title | Files | Size | Gate |
|---|---|---|---|---|
| 15 | `cpu: SSA across region edges` | `PPUTranslator.*` | XL | only for regions above the histogram threshold; differential |
| 16 | `cpu: packed CR fields and lazy flags` | `PPUTranslator.cpp` | L | golden assembly |

## W6–W8 — quality, lifetime, policy (PR 17–21)

| PR | Title | Files | Size | Gate |
|---|---|---|---|---|
| 17 | `cpu: merge-store and aligned LVLX/STVLX lowering` | `PPUTranslator.cpp` | M | golden assembly, alignment matrix |
| 18 | `jit: per-page invalidation and code-space reuse` | `JIT*.cpp` | L | invalidation stress |
| 19 | `jit: PPU cache v10 with sidecar metadata` | `PPUThread.cpp` | S | cold-cache measurement |
| 20 | `cpu: QoS policy split; bounded isb + park` | `Thread.cpp`, `sync.h` | M | p99, audio A/B |
| 21 | `build: PGO variant for host code` | CI | S | CI build + corpus run |

## Honest ordering note

9a–9d are the pieces whose correctness can be established without a title. From
9c on, the gate needs a running emulator: a stub that resumes at the wrong
address hangs the process, which is the failure the design's prototype hit.
Nothing above claims a title-level gain; the p99 and A/B gates in §11.2 still
have to be run on hardware with the corpus.
