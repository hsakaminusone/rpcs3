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
| 10a | `vm: track sub-page state per host page` | `vm.cpp`, `vm.h` | M | **landed**: 7 tests, state derived from the mapped bits |
| 10b | `cpu: emit the T1 sub-page guard for notification pages` | `PPUTranslator.cpp`, `CPUTranslator.*` | L | **landed**: the guard is always emitted, and the slow path shares `mmapper_deliver_page_fault` with the hardware fault handler |

## W4 — dispatch (PR 11–14)

| PR | Title | Files | Size | Gate |
|---|---|---|---|---|
| 11a | `cpu: loop-first region formation` | `PPUTranslator.cpp` | XL | **scan landed** (`Utilities/ppu_region_scan.*`, 7 tests): loop headers by descending span, absorption only of single-predecessor blocks, budget and threshold; the translator does not consume regions yet, so the golden-IR gate is still open |
| 11b | `cpu: real CFG edges between blocks` | `PPUTranslator.cpp`, `CPUTranslator.h` | L | differential on a block corpus |
| 12 | `cpu: link area for direct exits` | `PPUTranslator.cpp` | M | block-link tests |
| 13 | `cpu: monomorphic IBTC for indirect branches` | `PPUTranslator.cpp` | M | **landed**: one cell per indirect site in the module's data, filled on the miss path and trusted only while the invalidation epoch matches; hit rate and the corpus A/B have not been measured |
| 14 | `cpu: return stack buffer for bl/blr` | `PPUTranslator.cpp` | M | call/return stress |

## W5 — IR state (PR 15–16)

| PR | Title | Files | Size | Gate |
|---|---|---|---|---|
| 15 | `cpu: SSA across region edges` | `PPUTranslator.*` | XL | only for regions above the histogram threshold; differential |
| 16 | `cpu: packed CR fields and lazy flags` | `PPUTranslator.cpp` | L | golden assembly |

## W6–W8 — quality, lifetime, policy (PR 17–21)

| PR | Title | Files | Size | Gate |
|---|---|---|---|---|
| 17 | `cpu: merge-store and aligned LVLX/STVLX lowering` | `PPUTranslator.cpp` | M | **lowering landed**: `STVLX`/`STVRX` are merge-stores (§8.3.4) and `LVLX`/`LVRX` stay branchless; the golden-assembly and alignment-matrix gates have not been run |
| 18 | `jit: per-page invalidation and code-space reuse` | `JIT*.cpp` | L | **landed**: page index + epoch (`Utilities/jit_page_index.*`, 9 tests) and epoch-gated 64 KB chunk reuse (`Utilities/jit_code_space.*`, 7 tests); the per-page recompile sweep is the part left, and it needs regions |
| 19 | `jit: PPU cache v10 with sidecar metadata` | `PPUThread.cpp` | S | **landed**: `v10` key, tier flag in the bits, and a `<obj>.meta` sidecar written at compile and consumed on a cache hit to seed the page index (7 tests) |
| 20 | `cpu: QoS policy split; bounded isb + park` | `Thread.cpp`, `sync.h` | M | **policy split landed**: threads request `SCHED_RR` and never layer it on a QoS class (D1), and the Apple affinity hint is gone (D3); spin-then-park is the existing bounded `busy_wait` (`isb`) + futex; the p99/audio A/B has not been run |
| 21 | `build: PGO variant for host code` | `CMakeLists.txt` | S | **option landed** (`USE_PGO=GENERATE`/`USE`); the CI job and the corpus run are not wired |

## Honest ordering note

9a–9d are the pieces whose correctness can be established without a title. From
9c on, the gate needs a running emulator: a stub that resumes at the wrong
address hangs the process, which is the failure the design's prototype hit.
Nothing above claims a title-level gain; the p99 and A/B gates in §11.2 still
have to be run on hardware with the corpus.

## What is still open, and what each one is waiting for

| PR | Waiting for |
|---|---|
| 9d, 9e | the site metadata of PR 8 to reach the fault handler, then a resume-address test on a running module |
| 11b | the translator consuming the region list: real CFG edges need region IR, which needs the entry checks and live-in/out split to land together |
| 12 | region IR: for a same-module direct target LLVM already emits the direct branch, so a link cell there is slower; the cell only pays once a callee body can be linked past its prologue, and that needs 11a's regions |
| 13, 14 | measurement and the invalidation epoch (now in the tree): the design says to ship the monomorphic cell only if it beats the table on the corpus, and the RSB needs per-thread state whose layout must be validated on a running module |
| 15, 16 | the differential corpus; both change the state carried across a region edge |
| 18 (second half) | a consumer: code-space reuse needs chunks and an epoch, and the epoch side is now in place |

Every row above changes either generated code or the state carried across a
block edge, so none of them can be closed by inspection, and none is claimed as
done here.
