# Native CPU runtime: landed state and deviations from the design

Marker for branch `cpu-native-p1`. The design document
(`01-native-cpu-redesign.md`) is the specification and is **not** edited to match
the implementation. Where the code departs from it, this file is authoritative.
Date: 2026-11-16.

## What P0/P1 landed

| Area | Code | Note |
|---|---|---|
| W^X | `Utilities/JIT.h` (`jit_write_guard`), `JITLLVM.cpp`, `JITASM.cpp`, `Thread.cpp`, `PPUThread.cpp`, `SPUThread.cpp` | One guard owns the per-thread toggle; no raw `pthread_jit_write_protect_np` call sites remain outside it |
| icache publication | `finalizeMemory` in both LLVM memory managers, plus an explicit publish after `addObjectFile` | See "LLVM 23 measurements" below |
| Frame budget | `cpu_thread::gateway_slab_size`, `JITLLVM.cpp` `.stack_sizes` decoder | Modules with a frame above the budget are refused; the caller falls back to the interpreter |
| VM reset/decommit | `rpcs3/util/vm_native.cpp` | `mprotect(PROT_NONE)` + `MADV_FREE_REUSABLE`, no `munmap`; reset no longer returns the previous contents |
| Diagnostics | `Utilities/CPUStats.h`, `Utilities/fastmem.h` | Counters for faults, MMIO tier sites, frames, and the observed fault sites |
| Atomics | `rpcs3/util/atomic.hpp`, `atomic_storage<T, 16>` | 16-byte `load` and `observe` emit exactly the sequences clang emits for `__ATOMIC_SEQ_CST` and `__ATOMIC_RELAXED`; `store` and `release` already did |
| Wait engine | `rpcs3/util/atomic.cpp` | The `ext` walkers in `wait()` now bound their fixed-size stack arrays themselves instead of relying on a `static_assert` in `atomic_wait.hpp` |
| Tests | `rpcs3/tests/test_cpu_native_jit.cpp`, `test_cpu_native_atomics.cpp`, `test_cpu_native_vm.cpp` | JIT: builder runs, patch+flush is visible, guard nests, outer scope restores execute mode, frame budget fits the slab. ATOM: 16-byte store/load/observe/`compare_exchange`/`exchange` round-trip both halves; `notify_one`/`notify_all` wake within budget; red-zone + 16 KiB stack. VM: page size, decommit keeps the reservation, reset leaves the range usable |

## Deviations from the design, and why

| Item | Design says | Landed | Why |
|---|---|---|---|
| Gateway slab | 64 KiB, thunk for frames above 48 KiB (§8.4.3) | `gateway_slab_size = max_frame_size = 1 MiB`, no thunk | Supersedes §8.4.3 step 1. The slab is one `sub sp` into reserved address space, so sizing it to the supported maximum closes the whole corruption band without changing the GHC calling convention. Also retires risk A4 until an exact-frame thunk earns its cost. |
| Frames above the slab | log and fall back (§8.4.3) | `finalizeMemory`/`try_add` refuse the module; the caller falls back | Logging alone left the corrupting path runnable. Uses `cpu_thread::max_frame_size` rather than a second hard-coded constant. |
| Direct-tier volatility | non-volatile T1 at v1, with a per-access site record (§8.1.1) | volatile by default; non-volatile only under `core.ppu_nonvolatile_memory` | The site record does not exist yet, so a fault cannot be attributed to one re-executable access. Shipping non-volatile loads inverts the PR 7-before-PR 9 ordering in §9.3 and is a correctness bug on every host, not only Apple. The tier split and the MMIO counter are kept. |
| W^X | one RAII scope, no ad-hoc toggles (§8.6.2) | `jit_write_guard` is the only toggle point; `enable_execute()` and `recover()` cover the two out-of-band transitions | The LLVM compile path had raw toggles, so a nested `build_function_asm` scope closed the write window mid-codegen. Reproduced before the fix: the next store after the nested scope raised `SIGBUS`. |
| icache publication | flush on every code path, named in §8.6.3 | flushed in `finalizeMemory` **and** explicitly after `addObjectFile` | See measurements below; the cached-object path never reaches `finalizeMemory`. |
| Decommit | `mprotect(PROT_NONE)` + `MADV_FREE_REUSABLE` (§8.1.5) | as designed, for both `decommit` and `reset` | The Apple path had been applying the generic no-dump advice (`MADV_NOCORE`), which reclaims nothing, and `reset` left the previous contents readable. The advice is best effort: it runs over `MAP_JIT` ranges at teardown, where aborting would be worse than keeping the pages. |
| 16-byte ordering | align each sequence with the architecture's total order (§8.1.6) | matched to the compiler's lowering instead of hand-tuned | The audit found `load` carrying two `dmb ishld` around a `dmb ish`, and `observe` delegating to `load` even though `atomic_t::observe` is documented as a relaxed load. Removing the redundant barriers is not a relaxation: the remaining sequence is byte-identical to what clang emits for the same `__atomic_load`, and `observe` without LSE2 still takes the acquire path. |

## Atomic 16-byte sequences

The four sequences in `atomic_storage<T, 16>` are hand-written asm, so they were
compared against the compiler's own lowering for the equivalent `__atomic_*`
call rather than judged by argument. Target: `arm64`, `-march=armv8.6-a` (LSE2),
clang 23.

| Operation | clang (`__atomic_*`) | `atomic_storage` | Result |
|---|---|---|---|
| `load` seq_cst | `ldp; dmb ish` | `ldp; dmb ish` | was `dmb ishld; ldp; dmb ish; dmb ishld` |
| `observe` relaxed | `ldp` | `ldp` | was the full seq_cst load |
| `store` seq_cst | `dmb ish; stp; dmb ish` | identical | unchanged |
| `release` | `dmb ish; stp` | identical | unchanged |

Microbenchmark, 16-byte aligned operand, L1-resident, 450M iterations, median of
three runs on this machine:

| Operation | Before | After |
|---|---:|---:|
| `load` (seq_cst) | 1.42 ns | 0.48 ns |
| `observe` | 1.42 ns | 0.41 ns |
| `store` (seq_cst) | 1.12 ns | 1.12 ns |
| `release` | 0.68 ns | 0.64 ns |

This is a microbenchmark of the primitive, not of a title. It establishes that
the redundant barriers are gone; it does not establish a frame-time gain. The
A/B on the game corpus in §11.2 is still required before any title-level claim.

The x86_64 16-byte `observe` still delegates to the seq_cst `load`. That is a
missed optimization, not a correctness issue (x86 loads are already ordered), and
it was left alone to keep this change to the architecture it targets.

## LLVM 23 measurements used for the publish path

/tmp/ probe, MCJIT, one module:

| Operation | `notifyObjectLoaded` | `finalizeMemory` |
|---|---:|---:|
| `generateCodeForModule` + `finalizeObject` | 1 | 1 |
| `addObjectFile` (cached module load) | 1 | 0 |
| `addObjectFile` then `finalizeObject` | 2 | 1 |

`notifyObjectLoaded` is documented as running *before* relocations, so it cannot
be the publish point. A compiled module is published by `finalizeMemory`; a
cached module is not, so the compiler asks the memory manager for an explicit
publish after `addObjectFile`. `JITASM` already publishes in `_add`.

## Still open (not claimed as done)

- Fastmem site table is observational only: no patch, no first-fault retry.
- T2/MMIO helper recognises RawSPU MMIO only; RSX-mapped registers are not covered.
- Sub-page notification map (W3) and every workstream from W4 on are untouched.
- No `--cpu-stats` and no config flag gating the diagnostics.
- No differential test for the non-volatile tier, which is why it defaults off.
- Atomics: only the 16-byte sequences were audited against the compiler, because
  that is where hand-written barriers exist. The narrower paths still carry the
  conservative fences they had, and nothing was re-measured after the change on a
  title.
- `memory_reset` does not promise zero-filled pages, and the VM test measured
  that it does not deliver them: `MADV_FREE_REUSABLE` is a lazy free, so with no
  memory pressure the old contents survive the call. Decommit plus commit was in
  the same position before this change, so nothing regressed, but any caller that
  needs zeroed guest memory has to zero it explicitly. If that is required, the
  fix is a zeroing commit path, not a different advice.
- Unaligned decommits are still rejected by `ensure` (an unaligned `mprotect`
  fails on a 16 KiB host). `memory_commit` rounds the range outward; decommit
  cannot, because extending it would decommit a neighbour page that may be live.
  No caller below the VM layer asks for one today, and CPU-VM-002 cannot pass on
  a 16 KiB host until sub-page decommit is defined.
- `atomic_wait_engine` (#18429): the arrays in `wait()` and the ring buffer in
  `notify_all()` are bounded by `atomic_wait::max_list` and by the `count >= 128`
  check respectively, and no failure from the current source could be
  reproduced. The review finding was the unbounded `ext` walk, now fixed, plus the
  documented absence of a lifetime hazard: `atomic_wait::info` is caller-owned
  stack storage and is never published to another thread.
- The macOS CI smoke test still exercises only the host's MAP_JIT API. The guard
  and publish paths are covered by `rpcs3_test` instead.
