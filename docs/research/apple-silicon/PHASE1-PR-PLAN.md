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
| 9d | `cpu: patch a recurring site after the threshold` | `Thread.cpp`, `fastmem.*` | M | blocked: needs T2 classification first (below) |
| 9e | `cpu: recompile a patched block so LLVM sees the helper` | `PPUTranslator.cpp`, `PPUThread.cpp` | M | patched site survives invalidation; no flapping |

9a is the only piece that needs no translator metadata, so it goes first. 9b has
to land before 9c: a stub needs the access width and direction at fault time and
those are known at translation time, not decodable safely from the patched
instruction.

### Why 9d is blocked, and on what

Decoding the instruction gives the width and the direction but not two things the
patch decision needs:

1. **Whether the address is RAM or a device register.** A patched stub re-executes
the access or calls a helper; doing that to an MMIO address bypasses the device
emulation and reads or writes the register file directly. The instruction cannot
tell us, and neither can the fault. T2 classification has to exist before any site
is patched, which means the translator-side metadata of PR 8 rather than the
decoder.
2. **The destination register, for a load.** Replacing a load with a call loses the
result: the helper would have to write the guest register, which is a host
register the stub does not name. Stores avoid this, so a store-only patch is the
smallest correct slice if 9d is pulled forward.

A `stub_provider` interface with no implementation was drafted and discarded: it
would have compiled, been tested nowhere, and looked like a working tier.

### A finding that reorders W2

Under the current VM, **a site that faults is by definition not a plain RAM
page.** Guest RAM is committed on first touch and stays committed; what keeps
faulting is an unmapped or device-backed page. So "patch the recurring site and
re-execute it" is never correct here: the recurring fault means the address must
not be accessed directly.

The only correct patch is therefore a call into the slow helper, and that needs
the full operand set of the instruction, not just its width and direction: a load
must write its destination guest register, a store must read its value register.
Those are host registers the stub would have to name, so they have to come from
metadata the translator recorded when it knew them (PR 8), or from decoding every
addressing and register field and emitting the register moves.

That makes 9d/9e an L-sized change, not M, and it moves the useful work back to
W3: the sub-page map fixes #19881 without patching anything, and the T1 guard it
adds is the thing that keeps a fault from being generated in the first place.

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
