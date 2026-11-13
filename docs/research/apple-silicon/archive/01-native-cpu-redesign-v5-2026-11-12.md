# 01 — Apple Silicon native CPU: correctness + performance design

**Status:** v2 working design doc — first consolidated draft, will keep changing
**Date:** 2026-11-12 (v1 was 2026-11-07)
**Owner:** CPU/ARM bring-up
**Goal:** make the native `arm64` macOS build the primary RPCS3 path on Apple Silicon — correct first, then faster than the `x86_64` build under Rosetta.
**Target hardware:** Apple M-series and A-series on macOS 26+; reference machine is an **Apple A18 Pro (2P+4E, 8 GB, macOS 27.2 / 26B123, 16 KB pages, 128 B cache lines)**.
**Scope:** CPU emulation only. GPU/Metal work lives in `docs/research/metal4/`.

Evidence tags used below:

- `[V]` verified this session against this tree, a local SDK/header, `sysctl`, `llc`, or a primary URL.
- `[I]` inference from verified facts.
- `[U]` unverified, needs a measurement or an experiment.
- `[X]` explicitly out of scope.

---

## 0. What changed from v1

The first draft was directionally right but several claims were stale. Corrections are part of the design record:

1. **LLVM is 22.1.8**, not a pinned 21.1.7. `llc --version` reports `Homebrew LLVM version 22.1.8`, default target `arm64-apple-darwin27.2.0`, host CPU `apple-m4`. `[V]`
2. **`m_use_dotprod` and `m_use_i8mm` are not "off in-tree."** `cpu_translator::initialize()` enables them at runtime from `utils::has_dotprod()` / `utils::has_i8mm()` (`rpcs3/Emu/CPU/CPUTranslator.cpp:206-215`). Apple A18 Pro reports both = 1. `[V]`
3. **The naive stack-frame estimator is currently dead code for PPU and SPU.** Both set `.use_stack_frames = false` (`PPUTranslator.cpp:64`, `SPULLVMRecompiler.cpp:1653`). The real frame bug is the **8 KB gateway scratch**, not `AArch64JIT.cpp:82-91`. `[V]`
4. **`ARM_FEATURE_LSE2` is already forced on Apple**, and 16-byte atomics already have `ldp` load/store and `CASPAL` paths (`rpcs3/util/types.hpp:24-30`, `rpcs3/util/atomic.hpp:1062,1111,1205`). PR #19977 merged 2026-10-28. `[V]`
5. **The ARM64 JIT triple is `aarch64-unknown-linux-android`**, deliberately, to reserve x18 (`Utilities/JITLLVM.cpp:656,673`). It is not `arm64-apple-macos`. `[V]`
6. **`apple-m4` does imply LSE2 via `HasV8_7aOps → HasV8_4aOps → FeatureLSE2`** in LLVM TableGen, even though `AppleM4` does not spell out `FeatureLSE2`. `[V]`
7. **LLVM main now has `apple-m6`/`TuneAppleM6`; Homebrew LLVM 22 stops at `apple-m5`.** Do not assume "latest main" equals the toolchain we ship. `[V]`
8. **Rosetta's end is now confirmed by Apple**: macOS 27 is the final full release; macOS 28 keeps only a narrow path for certain older, unmaintained Intel games; macOS 26.4+ notifies users. `[V]` (Apple Developer News 2026-09-01, Apple support article 102147/102527 family)
9. **Current macOS thread setup is self-contradictory**: it sets `QOS_CLASS_USER_INTERACTIVE` and then `SCHED_RR` + `sched_priority = 99`. Apple's own `pthread/qos.h` says a subsequent `pthread_setschedparam()`-family call that conflicts with QoS **unsets** the QoS class. The intended QoS is therefore not in effect. `[V]`
10. **`memory_decommit`/`memory_reset` on Apple ARM64 still use `munmap` + `mmap` without `MAP_FIXED`** (the #18423 fix was reverted by #18656). That race is still live. `[V]`
11. **The `.stack_sizes` frame design is feasible, but only because the JIT triple is ELF.** `llc -stack-size-section` emits `.stack_sizes` for ELF targets and **not** for Mach-O. The JIT compiles as `aarch64-unknown-linux-android`, so MCJIT produces an ELF object; with `EmitStackSizeSection` + `setProcessAllSections`, the section really is delivered to `RTDyldMemoryManager::allocateDataSection`, and its `[u64 addr][ULEB128 size]` payload decodes exactly. Verified end-to-end in `measurements/2026-11-12/sstest2.cpp`. `[V]`
12. **`m_use_avx = true` on ARM is dead code.** Every `m_use_avx` use in `SPULLVMRecompiler.cpp` is inside `#ifdef ARCH_X64` (or the `#else` of an `ARCH_ARM64` block). The v2 draft's "remove the ARM `m_use_avx` hack to save compile time" item was wrong; there is nothing to remove. `[V]`
13. **`SCHED_RR` is not slower than QoS — it is faster in throughput.** Measured on the reference machine: 8 runnable threads for 2 s, `SCHED_RR`/99 = 1461 Miters/s total, `QOS_CLASS_USER_INTERACTIVE` = 929, `DEFAULT` = 958, `UTILITY` = 775. The QoS class is definitely unset by the current sequence, but "restore QoS" must be justified by latency/fairness, not throughput. `[V]`
14. **`LVLX`/`STVLX` are absent from the local corpus.** Decrypting and disassembling the real `EBOOT.BIN`s shows zero `lvlx/lvrx/stvlx/stvrx` in Demon's Souls and Ridge Racer 7, while `lwz/stw/ld/std` and CR branches dominate. The masked-store codegen bug is real but not a corpus regression. `[V]`
15. **WFE does not park on macOS 27.2/A18 Pro.** A pure `wfe` loop runs at 0.74 M ops/s and burns ~100% of a core; wake latency is worse than `isb`. The v3 plan to build a WFE spinlock is withdrawn. `os_sync_wait_on_address` is the only tested primitive that reaches zero CPU while waiting. `[V]`
16. **SME/SME2 are advertised but inert in user space.** `SVCR` stays 0, `CNTB/CNTW/CNTD/RDSVL` return 0, ZA reads back zeros, and plain SVE/SME instructions trap unless the OS enables streaming mode — which it does not. `[V]`
17. **Decommit without `munmap` is not more expensive.** On 64 MB, `mprotect(PROT_NONE)` decommits in 130 µs and `MADV_FREE_REUSABLE` in 207 µs, versus 285 µs for `munmap`+`mmap`, and all three return `phys_footprint` to baseline. `[V]`
18. **The masked-store fix is 31 branches → 6 instructions.** A read-modify-write merge-store compiles to `shl/cmlt/ldr/bif/str`, no branches. `[V]`
19. **SPU has no stale codegen cache.** Its `.dat` holds raw guest code; the `llvm/` objects are debug-only and never loaded. PPU's key already includes CPU + settings, but its `v9` prefix must be bumped when IR changes. `[V]`

Two claims from v1 survive and drive the design:

- **LLVM has no Apple scheduling model for any Apple CPU.** Even `apple-a7` through `apple-m6` bind to `CycloneModel` (2014), with `IssueWidth = 6`, `MicroOpBufferSize = 192`, `LoadLatency = 4`, and `WriteAtomic` marked unsupported. `[V]`
- **The GHC calling convention is the central codegen constraint**, not a detail: no callee-saved registers, no preserved hardware SP, tail-call-only control flow, and spills are placed relative to a scratch area RPCS3 owns. `[V]`

---

## 1. Executive summary

Native ARM64 is no longer optional on macOS. Apple has confirmed that general-purpose Rosetta ends with macOS 27; macOS 28 retains only a limited compatibility path for certain older games, and macOS 26.4+ already warns users when an app depends on it. The reference machine runs **macOS 27.2**, i.e. the last release where the x86 build works normally. RPCS3 must therefore make the native build at least as correct and as fast as the Rosetta build, and then deprecate the x86 macOS path.

The performance gap is **not** "ARM is slow." It is a small set of architectural mismatches that compound:

| # | Structural issue | Consequence | Workstream |
|---|---|---|---|
| 1 | Stackless GHC code assumes an 8 KB scratch slab | large SPU functions overwrite the gateway frame (#19939), crashes/hangs | A |
| 2 | 16 KB host pages vs 4 KB guest pages | guest page faults never fire (#19881), GT5 never progresses | B |
| 3 | Legacy memory decommit race on Apple | `memory_decommit`/`memory_reset` can lose the mapping (#18423 reverted) | B |
| 4 | LLVM schedules Apple CPUs with the 2014 Cyclone model | bad latency/issue assumptions; atomics modeled as unsupported | E |
| 5 | LLVM GHC codegen has no frame and limited registers | spills, LR clobbers, tail-call fixups, high compile-time pressure | A, D |
| 6 | macOS QoS is silently unset; everything runs `SCHED_RR` 99 | unfair latency/placement; realtime audio/offloader contention | F |
| 7 | `sse2neon`-derived lowering and x86-shaped IR | extra instructions on hot SIMD paths | C, D |
| 8 | JIT compile-time and W^X toggling | stutter, cache-build time, signal-handler recovery paths | G |

The plan is deliberately **upstreamable**: each item is a small, measured PR behind `#if defined(__APPLE__) && defined(ARCH_ARM64)` (or a generic ARM64 improvement that is also valid on Linux ARM64), with tests. No fork of `CPUTranslator`, no private LLVM branch as a merge dependency, no Linux ARM regression.

**North-star metrics**

1. Every title in the local corpus and the `CPU: arm64` issue corpus boots with the LLVM recompiler, not the interpreter.
2. Native median FPS ≥ Rosetta x86 median FPS on the same machine, same settings, same cache state, for the benchmark corpus.
3. Native p99 frame time ≤ Rosetta p99 frame time × 1.05.
4. Cold SPU/PPU cache build time ≤ Rosetta build time × 1.10.
5. No increase in crash rate; ASan/UBSan clean on the L0 test suite.

### 1.1 Ballpark expectations (design-stage estimates, not promises)

These are the ranges the design is built around. They are deliberately wide; each one is
tied to a measurement or a structural argument, and each must be replaced by a real number
before the PR lands.

| Change | Expected effect | Confidence | Basis |
|---|---|---|---|
| 8 KB gateway frame fix (A) | turns crashes/hangs into boots in affected titles; ~0% FPS elsewhere | high | #19939 reproduces with 9.8 KB / 10.7 KB frames; patch reported working |
| 16 KB page-fault map (B) | unblocks GT5-class titles that currently never fault | high | #19881: 0 faults native vs 164 under Rosetta |
| decommit/reset without `munmap` (B) | removes a class of random "Verification failed"/lost-mapping crashes | high | `vm_native.cpp:354,384`; #18423/#18656 history |
| 16-byte atomic audit (B) | 0–5% in RSX-heavy scenes | medium | `ldp`/`stp`/`casp` already emitted; remaining risk is fences |
| masked-store fast path (C) | 0% on current corpus, 2–15% in titles that use `stvlx/stvx` tails | medium | DS/RR7 contain zero `STVLX`; lowering measured at 31 branches vs 6 instructions |
| CR field packing (D) | 1–4% in PPU-bound titles | low–medium | 341 k CR branches in DS; each block-boundary flush is up to 4 `strb`; needs harness |
| spin-wait / parking (F) | removes wasted cores; latency win, not FPS | medium | `wfe` burns a core; `os_sync_wait_on_address` parks at 0 CPU, 9 µs p50 |
| QoS rework (F) | −5% to +5% FPS; the win is p99/audio, not mean | low | §measurements: `SCHED_RR` is 50% faster in synthetic throughput |
| LLVM Apple scheduling model (E) | 0–10%, not shippable in the near term | low | Cyclone model is from 2014; upstream dependency |
| SPU SIMD cleanup (`dotprod`/`i8mm`/`tbl2`) | 0–8% in SPU-bound titles | medium | features already enabled; wins are per-opcode |

The honest summary: **the correctness work is the bulk of the value.** Native-vs-Rosetta
FPS parity is plausible today for SIMD-heavy scenes and unlikely for titles that fall back
to the interpreter or take a page-fault path. Do not promise a global speedup before the
instrumentation in §17 exists.


---

## 2. Scope, non-goals, and the upstreamability contract

### 2.1 In scope

- Apple Silicon (`arm64`) on macOS **26+** (deployment target can stay at the project minimum; runtime features are probed).
- M1–M5 and A15–A19 families, including the A18 Pro reference machine.
- PPU LLVM, SPU LLVM, RSX/offloader interaction, VM layer, thread scheduler, JIT cache.
- Linux ARM64 only where a change is unambiguously generic (e.g. an LLVM feature attribute fix); Apple-specific behavior stays gated.

### 2.2 Out of scope

- `[X]` SVE/SVE2 codegen. Apple reports no `FEAT_SVE`; `utils::has_sve()` is 0 and `sve_length()` would be meaningless.
- `[X]` AVX-512 emulation. There is no Apple equivalent; the useful AVX-512 tricks already have NEON forms (table lookup, popcount, ternary select, dot product).
- `[X]` Rosetta tuning. The x86 build is the baseline, not a target.
- `[X]` GPU/Metal 4 changes. Separate docs.
- `[X]` Private AMX instructions. SME is the public matrix extension; see §14.
- `[X]` A custom LLVM fork as a merge requirement. Optional local experiment only.

### 2.3 Upstreamability contract

A change is mergeable only if all of these hold:

1. **No behavior change outside the gate.** Apple path behind `__APPLE__ && ARCH_ARM64`; generic fixes must be justified for Linux ARM64 too.
2. **Small.** Target ≤ 400 changed lines per PR, ≤ 800 for the frame pass.
3. **Tested.** Unit tests for pure logic; a golden-assembly or IR test where codegen changes; a named game or replay for a behavioral fix.
4. **Measured.** Perf claims carry before/after numbers from the benchmark protocol in §17.
5. **Reversible.** A config switch or compile-time guard can restore the old path for one release.
6. **Documented.** Every magic number (8 KB, 16 KB, 64 KB, QoS class) has a comment and a test.

---

## 3. Why now: the Rosetta endgame

Apple's September 1, 2026 developer note and support article say, in substance:

- Rosetta is available on Apple silicon Macs running **macOS 27 or earlier**.
- **macOS 27 is the final release with general-purpose Rosetta support.**
- Starting with **macOS 28**, Rosetta functionality is available **only for certain older, unmaintained games** that rely on Intel frameworks.
- macOS 26.4+ shows users a notification when an app depends on Rosetta.

Consequences for RPCS3:

1. An `x86_64` macOS build is a dead end for general emulation. Even if macOS 28's exception covered RPCS3, relying on an unmaintained compatibility shim is not a supportable baseline.
2. The native build must not merely "boot"; it must be the **fast** path. Today users explicitly choose the x86 build because it is faster in some titles.
3. The transition window is now. The reference machine is on the last full-Rosetta release, which makes A/B measurements possible; after macOS 28 they become impossible.
4. CI must keep an `arm64` macOS job and should add a **native-vs-Rosetta benchmark job** while Rosetta still exists.

`[U]` Exact behavior of macOS 28's "older games" exception for a general-purpose emulator. Do not design around it.

---

## 4. Verified baseline

### 4.1 Reference machine

| Property | Value | Source |
|---|---|---|
| SoC | Apple A18 Pro | `machdep.cpu.brand_string` |
| Cores | 2 performance + 4 efficiency | `hw.nperflevels=2`, `hw.perflevel0/1.physicalcpu` |
| Memory | 8 GB unified | `system_profiler` |
| OS | macOS 27.2 (26B123) | `sw_vers` |
| Kernel page size | **16384** | `hw.pagesize` / `getpagesize()` |
| Cache line | **128 B** | `hw.cachelinesize` |
| LSE / LSE2 | **1 / 1** | `hw.optional.arm.FEAT_LSE{,2}` |
| DotProd / I8MM / BF16 | **1 / 1 / 1** | `hw.optional.arm.FEAT_*` |
| SHA3 / SHA512 | **1 / 1** | `hw.optional.arm.FEAT_*` |
| SME / SME2 | **1 / 1** | `hw.optional.arm.FEAT_SME{,2}` |
| SVE / SVE2 | **absent** | `sysctl` key missing → 0 |
| CSSC | **0** | `hw.optional.arm.FEAT_CSSC` |
| LRCPC | **1** | `hw.optional.arm.FEAT_LRCPC` |
| FP16 / FHM / JSCVT | **1 / 1 / 1** | `hw.optional.arm.FEAT_*` |
| AES / PMULL / SHA1 / CRC32 | **1 / 1 / 1 / 1** | `hw.optional.arm.FEAT_*` |

`[V]` all of the above were read on the reference machine on 2026-11-12.

**Important:** the presence of SME/SME2 does **not** mean RPCS3 can casually use it. See §14.4.

### 4.2 Toolchain

| Item | Value |
|---|---|
| Compiler | Homebrew LLVM **22.1.8** (`/opt/homebrew/opt/llvm@22`) |
| `llc` host CPU | **`apple-m4`** |
| Default target | `arm64-apple-darwin27.2.0` |
| JIT target triple | `aarch64-unknown-linux-android` (deliberate: reserves x18) |
| Build arch | `CMAKE_OSX_ARCHITECTURES=arm64` |
| Build type | `Release` |
| `USE_NATIVE_INSTRUCTIONS` | `OFF` (good: JIT codegen must not depend on host C++ `-march`) |
| LLVM CMake | `LLVM_DIR=/opt/homebrew/opt/llvm@22/lib/cmake/llvm` |

`[V]` from `llc --version` and `build/CMakeCache.txt`.

Notes:

- CI may use a different LLVM. Any design that depends on a TableGen change must be version-gated or upstreamed first.
- `llvm::sys::getHostCPUName()` returns `apple-m4` on this machine, so `jit_compiler::cpu("")` resolves to `apple-m4` without the ARM `cortex-a78` fallback. The fallback only runs when LLVM returns `generic`.
- `AArch64Common::get_cpu_name()` is effectively **empty on Apple** because `read_MIDR_EL1()` returns 0 for every “CPU”, so the part lookup fails. The user-visible name comes from `get_cpu_brand()` → `"Apple A18 Pro (2P+4E)"`. `[V]`

### 4.3 Current code map

| Area | File(s) | What it does | Apple-specific? |
|---|---|---|---|
| CPU translator | `rpcs3/Emu/CPU/CPUTranslator.{h,cpp}` | PPU/SPU → LLVM IR, GHC helpers, NEON helpers, x86/ARM feature flags | shared |
| AArch64 backend | `rpcs3/Emu/CPU/Backends/AArch64/AArch64{JIT,Common,ASM,Signal}.{cpp,h}` | GHC frame pass, CPU detection, inline asm, Darwin signal decode | ARM64 |
| PPU frontend | `rpcs3/Emu/Cell/PPUTranslator.cpp` | PPC → IR, GHC config | shared |
| SPU frontend | `rpcs3/Emu/Cell/SPULLVMRecompiler.cpp` | SPU → IR, GHC config, SPU SIMD lowering | shared |
| JIT engine | `Utilities/JIT.{h,cpp}`, `Utilities/JITLLVM.cpp`, `Utilities/JITASM.cpp` | LLVM engine, memory manager, target CPU/attrs, W^X, ASM helpers | shared |
| Atomics | `rpcs3/util/atomic.hpp`, `rpcs3/util/types.hpp` | 16-byte LSE2/CASP, futex sync | shared |
| VM | `rpcs3/util/vm_native.cpp`, `rpcs3/Emu/Memory/vm.cpp` | reserve/commit/decommit/protect, 4 KB guest pages, page faults | shared |
| Threads | `Utilities/Thread.cpp`, `Utilities/sync.h` | Darwin threads, QoS/priority, futex via `os_sync` | shared |

### 4.4 What is already fixed — do not redo

| Date | Change | Reference | Why it matters |
|---|---|---|---|
| 2022-06 | PPU LLVM ARM64+macOS port | #12115 | baseline |
| 2022-07 | SPU LLVM ARM64+macOS port | #12338 | baseline |
| 2024-08/09 | Native macOS ARM64 support + CI | #15992, #16070, #16148 | baseline |
| 2025-12 | macOS futex via `os_sync_wait_on_address` (14.4+) | #17888, #17893 | stability/perf |
| 2025-12 | Force max pthread priority / avoid throttling | #17905 | FPS stability |
| 2026-02 | Detect ARM features and pass LLVM attributes | #18133 | correctness on SVE-less SoCs |
| 2026-02 | `ISB` instead of `YIELD` for x86 `PAUSE` | #18151 | spin-loop power/perf |
| 2026-04 | `MAP_JIT` only where needed; VSH memory fixes | #18425, #18707 | correctness |
| 2026-09 | `ARM_FEATURE_LSE2` on Apple; 16-byte loads/stores | #19611 | GTA IV 19→58 FPS |
| 2026-09 | Reset GHC transform state per compiled module | #19609, commit `965f4f5` | Far Cry 3 boot 1/8 → 6/6 |
| 2026-10 | `CASPAL` for 16-byte RMW | #19977 | RSX offload queue |

**Do not re-implement these.** Verify them with tests, then build on them.

### 4.5 What is still open

| Issue | State | Impact | Workstream |
|---|---|---|---|
| #19939 | open | SPU frames > 8 KB overflow gateway scratch; Prototype 2 crash, Max Payne 3 hang | A |
| #19881 | open | Guest page-fault notifications never fire on 16 KB hosts; GT5 hangs | B |
| #19801 | open | LLVM SPU spill failure (“Cannot scavenge … X9”) | A/D |
| #19369 | open | ARMSX3 portable ARM64 optimizations not evaluated | C/F/G |
| #18429 | open | ASan stack-buffer-underflow in `atomic_wait_engine::notify_all` | B |
| #16484 | open | Mega-issue: ARM-only game breakage | all |
| #17231 | closed | ARM 5–10 FPS vs x86/Rosetta 30 FPS (Skylanders) | perf baseline |

### 4.6 Verified defect register (exact locations)

These are code-level defects confirmed by reading this tree, not hypotheses. Each one is
small enough to fix independently and should get its own regression test.

| # | Defect | Location | Verified evidence | Severity |
|---|---|---|---|---|
| D1 | QoS class is immediately destroyed by `SCHED_RR` + `pthread_attr_setschedparam`; all emulation threads run `SCHED_RR` priority 99 | `Utilities/Thread.cpp:2854-2856` | local test: QoS readback = `UNSPECIFIED(0)`, policy `SCHED_RR`, prio 99 | medium |
| D2 | `pthread_setschedparam` after QoS unsets QoS and drops priority to `SCHED_OTHER` max (47) | `Utilities/Thread.cpp:3915-3946` via `Thread.h:349-364` | local test: QoS `UNSPECIFIED(0)` after the call | medium |
| D3 | Apple affinity call computes `countr_zero(full_mask)`, i.e. tag 0 = no-op; no thread ever gets a meaningful P/E hint | `Utilities/Thread.cpp:3982-3986`, `CPUThread.cpp:658-661` | `get_affinity_mask()` returns `process_affinity_mask` on generic layout; `countr_zero` = 0 | low (it is only a hint) |
| D4 | `memory_decommit` unmaps and re-maps without `MAP_FIXED` | `rpcs3/util/vm_native.cpp:354-355` | code inspection; `mmap` result is only `ensure`d to equal the hint | high |
| D5 | `memory_reset` has the same non-`MAP_FIXED` race | `rpcs3/util/vm_native.cpp:384-385` | same | high |
| D6 | `STVLX`/`STVRX` lower through generic AArch64 masked store: 16 conditional byte stores / 31 branches + mask reduction (merge-store: 6 instructions) | `rpcs3/Emu/Cell/PPUTranslator.cpp:3730,3758` | `llc -mcpu=apple-m4` output in `measurements/2026-11-12/mstore2.ll` | medium (latent) |
| D7 | `m_use_avx = true` on ARM has no effect (all uses are x86-only) | `CPUTranslator.h:3244`; all uses in `SPULLVMRecompiler.cpp` under `ARCH_X64` | macro-context audit | none (dead flag) |
| D8 | `lwsync` is lowered as full `seq_cst` because of `op.l10 && false` | `PPUTranslator.cpp:3710` | code inspection; on AArch64 `dmb ish` is also the correct acq_rel fence, so this is not a bug in practice | none (document it) |
| D9 | Each CR field is four independent i8 locals, and `UseCondition` flushes them to the thread struct before every branch, so a compare+branch at a block boundary can emit 1 `cmp` + 3 `cset` + 4 `strb` + a load in the next block | `PPUTranslator.cpp:5248` (`SetCrFieldSignedCmp`), `:845` (`UseCondition`), `:5214` (`GetCrb`) | `llc` repro in `cr.ll`: full-field materialisation is `cmp`+3×`cset`+3×`strb`; single-bit use folds to `cmp`+`b.cc` | medium (largest PPU codegen surface) |
| D10 | `wfe` does not park on macOS 27.2/A18 Pro; it burns ~100% of a core and is *slower* to wake than `isb` | proposed `arm64::spin_wait` (not present yet); `#18151` advice | local measurement: `wfe` 0.74 M ops/s at 100% CPU vs `isb` 38.9 M ops/s; `os_sync_wait_on_address` is the only primitive that reaches 0.0000 s CPU | high for the design section, none in shipping code |

D3 deserves a note: on Apple Silicon `THREAD_AFFINITY_POLICY` is advisory and, with tag 0,
does nothing. Combined with D1/D2, thread placement today is entirely the OS scheduler's
decision. That is not necessarily bad (see the measured `SCHED_RR` throughput in §14), but
it means the code comments about affinity are misleading.

D9 is the one that most changes priorities: the guest corpus is branch-dense
(`bt`/`bf` = 341 k in Demon's Souls), and the translator forces the expensive CR
materialisation at every block boundary. It is a code-shape bug, not a correctness bug,
and it needs a codegen-instruction-count harness before it becomes a PR.

D10 is a correction, not a defect in shipped code: the design must not build a WFE spin
helper on the assumption that it saves power on macOS 26+. Measured behaviour is the
opposite of the 2024 power-meter result.

---

## 5. Evidence corpus: what ARM-only failures actually look like

The permanent GitHub label `CPU: arm64` plus the mega-issue #16484 is the best available regression corpus. The pattern is consistent: **same title, same settings, x64 works, ARM64 fails**; fallback is interpreter or Rosetta.

| Issue | Symptom | Root cause (verified or strongly indicated) | Status |
|---|---|---|---|
| #16484 | Heavenly Sword black screen; MGS4 black screen; GT5 `Narrowing error`; Dark Souls II crash; Rayman Legends crash; TLOU stuck; GTA V crash/leak; Uncharted 3 stuck | multiple: frames, NEON lowering, page faults, atomics | open |
| #15982 | Unity games segfault in `UnityMain` | recompiler codegen; interpreter and x64 OK | fixed |
| #16081 | VSH crashes with PPU LLVM | `MAP_JIT` / write-protection state | fixed by #18707 |
| #17530 / #17634 | Demon's Souls `BUG IN CLIENT OF LIBMALLOC`; “every game crashes” | heap corruption, likely spill/mapping | partly fixed |
| #18470 / #18974 | Demon's Souls regression after #18395 | ARM-specific regression | fixed |
| #19609 | Far Cry 3 SPU spins on one `ret` | GHC pass visited set not reset per module | fixed |
| #19939 | Prototype 2 crash; Max Payne 3 hang | SPU frame > 8 KB gateway scratch | open |
| #19881 | GT5 never gets a page fault; x64/Rosetta gets 164 | 16 KB host pages cannot express 4 KB holes | open |
| #19759 | GTA IV 13–30 FPS with MT RSX, 60 FPS without | 16-byte LL/SC retry storm on RSX queue | open (CASPAL landed; verify) |
| #19801 | LLVM “Cannot scavenge register without an emergency spill slot” | GHC/no-frame codegen + TBL2 fallback | open |
| #18429 | ASan underflow in `atomic_wait_engine::notify_all` on Apple ARM64 | wait-engine stack/lifetime | open |
| #19369 | ARMSX3 has portable ARM64 optimizations | not upstreamed | open |

### 5.1 Performance evidence

| Case | Native ARM64 | x86_64 + Rosetta | Notes |
|---|---|---|---|
| GTA IV, indoor, 65 s window, #19611 | 18.7 FPS median (11–28) | — | before LSE2/CASP; 58.0 after |
| GTA IV, MT RSX on, #19759 | 13–30 FPS, boot-dependent | — | off → steady 60; CASPAL should fix |
| Skylanders, #17231 | 5–10 FPS in 2025 → **30 FPS** | 30 FPS | **Resolved Jan 2026** by the perf fixes tested by the reporter; do not cite as a live gap |
| Far Cry 3, #19609 | 1/8 boots reach gameplay | — | 6/6 after per-module reset |
| Prototype 2, #19939 | crash ~80 s in | — | patch works |
| Max Payne 3, #19939 | hang on first level | — | patch works |

`[V]` all numbers are from the issue reports, not measured locally.

**Correction (adversarial pass).** The 3–6× native-vs-Rosetta gap that earlier drafts used
as the primary motivation is **stale**. #17231 was closed in January 2026: after the
perf fixes, the reporter confirmed a steady 30 FPS on the ARM build of the same title
(issue comments, 2026-01-18/20). The table row is kept only as history. There is currently
**no verified, current, large native-vs-Rosetta gap** in the issue tracker; the surviving
performance problems are title-specific hangs/crashes (#19939, #19881, #19801) and
frame-rate outliers such as #19759. Any perf argument in this document must therefore rest
on the structural defects (D1–D10) and on local instrumentation, not on #17231.

### 5.2 Local corpus (available now)

The repository's `filesdump/` contains playable dumps:

| Title | ID | Use |
|---|---|---|
| Demon's Souls (USA) | BLUS30443 | PPU/SPU heavy, long load, good stability test |
| Ridge Racer 7 (USA) | BLUS30001 | high frame rate, good perf test |
| Mega Man 9 (PSN `.pkg`) | NPUB30036 | light 2D, boot/regression smoke test |

The RPCS3 config in `~/Library/Application Support/rpcs3` is already set up with the Metal renderer, LLVM PPU/SPU, 2 PPU threads, 6 SPURS threads, and caches. This corpus is small but sufficient for A/B native-vs-Rosetta measurements. `[V]`

**Protocol:** use the same game data, same `config.yml` (copy it per run), same shader/PPU/SPU cache state, same resolution, and a fixed input replay. Capture at least 3 runs of 120 s; report median and p99 frame time, not just average FPS. See §17.

### 5.3 Real instruction mix from the local dumps

`rpcs3 --decrypt <EBOOT.BIN>` produces a plain `ppc64` ELF; `llvm-objdump -d --triple=powerpc64`
disassembles it. This is the first time the design has been grounded in the actual guest
instruction mix rather than in issue reports. Details and raw counts are in
`measurements/2026-11-12/README.md`.

| Group | Demon's Souls | Ridge Racer 7 | Consequence |
|---|---:|---:|---|
| scalar loads/stores (`lwz`, `stw`, `ld`, `std`, `lbz`, `stb`, `lfs`, `stfs`) | ~2.05 M | ~340 k | the hot path is scalar memory, not vector |
| CR branches (`bt`, `bf`) | 341 k | 58 k | CR lowering and `cmp`+`b.cc` fusion matter |
| `cmpwi` | 210 k | 21 k | three `cset`s per field is the floor unless fused |
| `lvx`/`stvx` | 109 k | 11 k | 1–2% of instructions; already `ldr`/`str` q |
| `vmaddfp`/`vaddfp` | 42 k | 5 k | NEON `fmla`/`fadd`; already good |
| `vperm`/`vsel` | 14.5 k | 1.1 k | `tbl2`/`bsl`; already good |
| `lwarx`/`stwcx` | 66 | 0 | guest atomics are rare |
| `lvlx`/`lvrx`/`stvlx`/`stvrx` | **0** | **0** | the masked-store bug is latent, not hot |
| `sync`/`lwsync` | 1620 | 1 | fences are not a corpus-wide bottleneck |

Three conclusions that change priorities:

1. **Do not spend the first perf PRs on vector shuffles.** The ARM port already has good
   `tbl2`/`bsl`/`fmla` lowering, and vector ops are a small fraction of guest code.
2. **CR and branch lowering deserve a look.** `bt`/`bf` + `cmpwi` is the largest
   control-flow cost. LLVM should fuse `cmp` + `b.cc`; verify it does on `apple-m4` and
   check that `SetCrFieldSignedCmp` does not force three separate `cset`s around every
   branch.
3. **The masked-store fix is quality, not speed.** Keep it, but below the SPU and thread
   work; it currently has no measured corpus impact.

---

## 6. Performance model: where the time goes

### 6.1 Thread and workload topology on the reference machine

- 2 P-cores, 4 E-cores.
- PPU: 2 emulated threads (main + one), each a host thread running PPU LLVM.
- SPU: up to 6 emulated threads (config `Max SPURS Threads: 6`), each a host thread running SPU LLVM.
- RSX: 1 render thread + 1 offloader (multithreaded RSX).
- Audio, input, save, HTTP, shader compiler workers.

That is **more runnable emulation threads than P-cores** (2 PPU + 6 SPU + RSX + offloader = 10). Placement therefore matters as much as per-thread code quality.

### 6.2 Cost buckets (hypotheses to measure)

| Bucket | What to measure | Initial hypothesis |
|---|---|---|
| Guest PPU code | time in JIT functions, `perf_meter`/signposts | 20–35% |
| Guest SPU code | time in SPU JIT functions | 25–40% |
| RSX/offloader | queue contention, DMA copy | 10–25% (can spike to 90% on a bad boot) |
| Synchronization | futex/atomic waits, reservation loops | 5–15% |
| JIT compile | LLVM compile wall time, cache misses | 0–30% during first boot |
| Host/OS | Metal/Vulkan, audio, input | remainder |

`[U]` until instrumented. The point of the table is to force measurement before optimization.

### 6.3 Why native can currently lose to Rosetta

1. **Interpreter fallback.** Any LLVM failure that silently falls back to the interpreter turns a 60 FPS title into a 5 FPS title. Logs must be checked for `Interpreter` / `Recompiler (LLVM)` mismatches.
2. **Codegen constraints.** GHC/no-frame code spills more, reloads LR around tail calls, and uses fewer registers than the x86 build's normal ABI.
3. **Wrong LLVM schedule.** Cyclone's 6-wide/192-ROB/4-cycle-load model is conservative for Firestorm-class cores; the scheduler may serialize independent work or over-prioritize latency.
4. **16 KB pages.** More memory per mapping, different fault behavior, and a VM layer written around 4 KB guest pages.
5. **Atomics.** Even after CASPAL, fences and 16-byte loads/stores need auditing; `dmb ish` is not free.
6. **Thread policy.** `SCHED_RR` + priority 99 on every emulation thread is a blunt
   instrument. Measured, it is *faster* than QoS in synthetic throughput (§14.2), so this
   is a latency/fairness problem, not an obvious FPS problem. It does not guarantee
   P-core placement.
7. **W^X.** MAP_JIT toggling and signal-handler recovery add overhead and complexity, especially during cache build.
8. **Compiler version skew.** Reports mix LLVM 19, 21, and 22; behavior changes.

### 6.4 Microbenchmark datapoints (first measurements)

A synthetic kernel approximating an SPU hot loop (two-table byte shuffle + 8-bit widening multiply-add + byte sum, dependent chain) was compiled natively (`-mcpu=apple-m4`) and for `x86_64 -mavx2`, then run under Rosetta 2 on the reference machine:

| Kernel | Native arm64 | x86_64 + AVX2 under Rosetta | Ratio |
|---|---|---|---|
| shuffle + madd + sum, dependent chain | **5.86 ns/iter** | 15.34 ns/iter | native 2.6× faster |
| 64-byte copy loop | 1.54 ns/iter | **0.93 ns/iter** | native 0.6× (Rosetta faster) |

`[V]` measured 2026-11-12, 2,000,000 iterations, single run after warm-up.

Additional measured datapoints (all in `measurements/2026-11-12/`):

| Datapoint | Result | Meaning |
|---|---|---|
| 16-byte atomic load, `apple-m4` | `ldp` + `dmb ishld` | LSE2 path is real; no library call |
| 16-byte atomic store | `dmb ish` + `stp` | correct, one fence each way |
| 16-byte CAS | `casp` | no `WriteAtomic` scheduler failure |
| generic masked 16-byte store | 16 conditional `str`/`stur` + mask reduction | avoid `CreateMaskedStore` on AArch64 |
| 8-thread `SCHED_RR`/99 throughput | 1461 Miters/s | QoS is not a free throughput win |
| 8-thread QoS `USER_INTERACTIVE` | 929 Miters/s | −36% vs `SCHED_RR` in this synthetic |
| 8-thread QoS `UTILITY` | 775 Miters/s | E-core-biased as expected |

**Interpretation, with caveats:**

- Native NEON is excellent when the operation maps to one `tbl2`/`vmlal`/`vpadal`; the x86 shim needs several instructions to emulate a two-table byte shuffle.
- Native is **not** universally faster. The 64-byte copy result is dominated by store-to-load forwarding and loop shape; it is a warning against assuming “native wins.”
- Real emulator code has a different mix. Use these numbers only to justify measuring per kernel, not to promise a global speedup.

**Action:** turn this into a proper microbenchmark suite (`tests/cpu/micro/`) with 5 runs, median, and assembly inspection. See §17.

---

## 7. Microarchitecture facts that matter

### 7.1 Firestorm / Icestorm (M1/A14 class)

From Dougall Johnson's black-box measurements (`applecpu`):

```
Firestorm (P): 8 uops/cycle, 1 taken branch/cycle
  int pipes: 1 alu+ubfm/flags/branch, 2 alu+ubfm/flags/indirect/ptrauth,
             3 alu+ubfm/flags, 4 alu+ubfm,
             5 alu+mul+div, 6 alu+mul+madd+crc+bfm/extr
  ld/st: 7 store, 8 load/store, 9 load, 10 load (128-bit, LSL#3)
  SIMD: 11,12,13 fp/simd (+fcsel), 14 fp/simd+fdiv+sqrt+sha
  ROB ~330 retire groups (~1100 uops), ~623 renames, ~380 int regs, ~432 FP regs

Icestorm (E): 4-wide, 3 int units, 1 load + 1 store, 2 FP/SIMD, 60 retire groups
```

Fusion (2 instructions → 1 uop):

- `adds/subs/ands/cmp/tst + b.cc`
- `add/sub/and/orr/eor/bic + cbz/cbnz` when the destination matches
- `aese+aesmc`, `pmull+eor`, `amx+amx`

Elimination (0 uops): `mov x0,#0`, `mov x0,x1` (64-bit), `nop`, `b`.

Latencies: `madd` → addend 1c else 3c; load → load-base 3c, load → ALU 4c; int → SIMD ≥ 7c. Not fused: `adrp+add`, `mov+movk`, `mul+umulh`, `udiv+msub`.

### 7.2 What we actually know about M4/A18 Pro

There is no public Firestorm-level counter breakdown for M4/A18 Pro. Publicly documented/measured facts:

- 128-byte cache lines (confirmed locally).
- 16 KB pages (confirmed locally).
- Wide out-of-order core; Firestorm is the closest public proxy.
- SME/SME2 present; no SVE.
- LLVM's `AppleM4` feature list includes SME, SME2, SME F64F64, SME I16I64, but binds to **CycloneModel**.

`[U]` Exact issue width, ROB, load latency, and fusion set for A18 Pro. Do not encode guesses into the compiler. Use Firestorm as a conservative proxy and measure.

### 7.3 LLVM's Apple scheduling model is from 2014

`AArch64SchedCyclone.td` (LLVM main, fetched 2026-11-12):

```tablegen
def CycloneModel : SchedMachineModel {
  let IssueWidth = 6;
  let MicroOpBufferSize = 192;
  let LoadLatency = 4;
  let MispredictPenalty = 16;
  let CompleteModel = 1;
  ...
  // Atomic operations are not supported.
  def : WriteRes<WriteAtomic, []> { let Unsupported = 1; }
}
```

Every Apple processor binds to it:

```tablegen
def : ProcessorModel<"apple-m4", CycloneModel, ProcessorFeatures.AppleM4, [TuneAppleM4]>;
def : ProcessorAlias<"apple-a18", "apple-m4">;
def : ProcessorModel<"apple-m5", CycloneModel, ProcessorFeatures.AppleM5, [TuneAppleM5]>;
def : ProcessorAlias<"apple-a19", "apple-m5">;
def : ProcessorModel<"apple-m6", CycloneModel, ProcessorFeatures.AppleM6, [TuneAppleM6]>;
def : ProcessorAlias<"apple-a20", "apple-m6">;
```

Consequences:

1. `-mcpu=apple-m4` is still required — it gives the right **ISA features** and a few tuning flags — but it does **not** give a Firestorm-class schedule.
2. `WriteAtomic` is unsupported, so the machine scheduler has no model for atomics. With `CompleteModel = 1`, this can also produce “unsupported instruction” behavior in strict modes. Atomics should be kept out of the hottest straight-line regions where possible, and the model should eventually be fixed upstream.
3. `TuneAppleM4` only removes `FeatureStorePairSuppress` and adds `FeatureFuseAppleSMECompute` + `FeatureShiftMaddSlow`. It does not encode fusion, elimination, load latency, or issue width. `[V]`
4. `TuneAppleM5` adds `FeatureFuseFMinFMax`; `TuneAppleM6` exists in main but not in LLVM 22. `[V]`

**Design stance:** do not depend on LLVM's scheduler for the big wins. Win by reducing instruction count, spills, branches, and memory traffic. Then, separately, propose an upstream `AArch64SchedAppleFirestorm.td` (or extend `CycloneModel`) as a community contribution. RPCS3 can consume it when distros ship a new LLVM; it must not be a merge blocker.

### 7.4 Features implied by `apple-m4`

`AppleM4` uses `HasV8_7aOps`; `HasV8_7aOps` implies `HasV8_6aOps → … → HasV8_4aOps`, and `HasV8_4aOps` includes `FeatureLSE2`. So:

- `+lse2` is implied even though `AppleM4`'s list only spells `FeatureLSE`.
- `+lse` is explicit.
- `+dotprod` (v8.4) and `+i8mm` (v8.6) are implied/explicit.
- `+bf16`, `+sha3`, `+fp16`, `+fp16fml`, `+crc`, `+aes`, `+sha2`, `+complxnum`, `+js`, `+pauth`, `+fpac`, `+ras`, `+rcpc`, `+rdm` are present.
- No `+sve`, no `+sve2`.
- SME/SME2 are present, but see §14.4.

`[V]` from `AArch64Processors.td` / `AArch64Features.td` and local `sysctl`.

### 7.5 Platform constants and topology API

| Constant | API | Reference value |
|---|---|---|
| Page size | `getpagesize()` / `sysconf(_SC_PAGESIZE)` / `vm_page_size` | 16384 |
| Cache line | `sysctlbyname("hw.cachelinesize")` | 128 |
| P/E levels | `hw.nperflevels`, `hw.perflevel{0,1}.name`, `.physicalcpu`, `.logicalcpu` | 2 levels, 2P+4E |
| Feature bits | `hw.optional.arm.FEAT_*` | see §4.1 |
| JIT W^X | `pthread_jit_write_protect_np`, `pthread_jit_write_protect_supported_np` | macOS 11+ |
| Futex | `os_sync_wait_on_address`, `os_sync_wake_by_address_*` | macOS 14.4+ |
| QoS | `pthread_attr_set_qos_class_np`, `pthread_set_qos_class_self_np`, `pthread_get_qos_class_np` | macOS 10.10+ |
| Affinity hint | `thread_policy_set(THREAD_AFFINITY_POLICY)` | advisory only |
| icache flush | `sys_icache_invalidate` (`libkern/OSCacheControl.h`) | required after code writes |

`[V]` all APIs verified against the local macOS SDK headers.

---

## 8. Design principles

1. **Correctness before speed.** A 5 FPS title that boots is better than a 60 FPS title that crashes. Every perf PR keeps the regression suite green.
2. **Gate, don't fork.** Share `CPUTranslator`, PPU/SPU frontends, and the JIT. Apple behavior is a runtime/compile-time branch.
3. **Reduce work, then schedule it.** On an out-of-order core, eliminating a load or a spill beats a better latency assignment.
4. **The GHC ABI is fixed.** Do not change the register layout casually; it is intertwined with `ppu_thread`/`spu_thread` context save and tail escape.
5. **Never trust `-mcpu` alone.** Use it for ISA features; use explicit IR and passes for shape.
6. **Measure on the corpus.** Microbenchmarks justify a hypothesis; games decide.
7. **Keep the x86 build working until macOS 28.** It is the A/B baseline and the fallback for users on older macOS.
8. **No hidden heap/stack growth.** Every new per-thread buffer has a budget and a test.
9. **Make failure loud.** If a function exceeds a scratch budget or a feature is missing, log and fall back deterministically; do not silently corrupt.
10. **Document magic numbers.** 8 KB, 16 KB, 64 KB, 128 B, QoS class, priority 99.

---

## 9. Workstream A — GHC frames, gateway scratch, and spill correctness (P0)

### 9.1 The actual mechanism

AArch64 GHC functions are compiled with:

- all GPR arguments in `x19–x28` (`Base, Sp, Hp, R1..R6, SpLim`), vectors in `q4/q5`/`s8–s11`/`d12–d15`;
- no callee-saved registers in the usual sense;
- hardware `sp` treated as **scratch**, not as a preserved frame pointer;
- control flow as tail calls only (a GHC function that calls C++ never returns normally).

RPCS3 enters compiled code through a gateway that does:

```asm
sub sp, sp, #8192          ; SPU gateway scratch
blr <compiled code>
```

Compiled code spills at **positive offsets from the scratch base**: under the GHC calling
convention LLVM treats the stack as caller-owned and emits **no prologue `sub sp`**. If a
function's frame is larger than 8192, the spill writes past the scratch, over the
gateway's saved thread-pointer slot and into host frames. That is #19939: a 9.8 KB frame
in Prototype 2 and a 10.7 KB frame in Max Payne 3.

`[V]` mechanism confirmed three ways: reading `SPUCommonRecompiler.cpp:513,609-616`, the
#19939 patch, and an `llc` reproducer (`measurements/2026-11-12/ghccc2.ll`). Under
`ghccc`, a 12 KB alloca compiles to `str w8, [sp, #11996]` with no `sub sp`; a 1.2 KB
alloca to `str w8, [sp, #28]`. This rules out the "LLVM allocates its own frame" reading
of the bug.

### 9.2 Options

| Option | Change | Pros | Cons | Verdict |
|---|---|---|---|---|
| A. Enlarge the slab | `8192 → 65536` in both gateways | one-line; no recompilation; covers almost all real functions | wastes 64 KB per gateway entry; no proof for >64 KB; hides the bug | safety net |
| B. `.stack_sizes` + per-function thunk | compile once, read frame sizes, recompile only oversized functions with a thunk that lowers `sp` | exact; no cost for normal functions; upstreamable | two-pass compile for affected modules; needs `EmitStackSizeSection` + `setProcessAllSections` | **recommended v1** |
| C. MachineFunction pass | insert a real prologue/epilogue only when spills exceed the slab | most correct long-term; one pass | complex; interacts with GHC tail calls; high risk | v2 |
| D. Per-function frame always | change GHC config / emit `sub sp` in IR | simple concept | adds overhead to every function; LLVM may move spills across inline asm | reject |

### 9.3 Recommended design (B + A as guard)

**Data flow**

1. `spu_llvm_recompiler` constructs its `jit_compiler` with a new flag `0x4` meaning “emit `.stack_sizes`”.
2. `jit_compiler` sets `llvm::TargetOptions::EmitStackSizeSection = true` and `ExecutionEngine::setProcessAllSections(true)`.
3. `MemoryManager1/2::allocateDataSection` records `.stack_sizes` sections; `finalizeMemory` decodes them into `(function address, frame size)` pairs (address is `u64`, size is ULEB128).
   - **This only works because the JIT triple is ELF.** `llc -stack-size-section` emits nothing for Mach-O; `aarch64-unknown-linux-android` yields an ELF object, which is what MCJIT sees. Log an error and disable the frame check if `.stack_sizes` is absent; never assume the section exists.
4. After `m_jit.get_engine().getPointerToFunction(f)`, look up the address. If `frame_size > spu_gw_scratch_size`, record `{function_name → frame_size}` in `spu_llvm_compile_context::frame_overrides` and fail the compile (`return nullptr`).
5. The caller (`compile_spu_llvm_with_retry`) sees the override set grow, preserves it across the retry, and recompiles with `use_tbl2` unchanged.
6. In the second compile, `GHC_frame_preservation_pass::inject_frame_thunk` runs for each overridden function:
   - move the original body to `<name>-framed`;
   - emit a thunk that loads the gateway-saved `sp` from the hypervisor context, subtracts `align16(frame_size) + 16`, writes it to hardware `sp`, and tail-calls the body;
   - do **not** restore `sp`; the gateway and tail escape reload it from the context.

**Budgets and failure**

- `spu_gw_scratch_size = 8192` remains the default.
- `max_frame_size = 0x100000` (1 MB). Larger is a compiler bug; log an error and fall back to the interpreter path rather than overflow.
- Debug builds assert `frame_size <= 0x100000`.
- Option A is the emergency fallback: raise the slab to 32 KB or 64 KB behind a config flag if a title still overflows.

#### 9.3.1 Verified feasibility (do not re-litigate)

Measured on the reference machine with Homebrew LLVM 22.1.8:

- Mach-O output (`arm64-apple-macos26.0`) with `-fstack-size-section`: **no**
  `__LLVM,__stack_sizes` section.
- ELF output (`aarch64-linux-gnu`, same as the JIT's `android` environment): `.stack_sizes`
  exists, size 19 bytes for two functions, with `R_AARCH64_ABS64` relocations.
- A 30-line MCJIT program using the android triple, `EmitStackSizeSection = true`, and
  `setProcessAllSections(true)` receives `DATA name='.stack_sizes' size=30` in
  `allocateDataSection`, followed by `.rela.stack_sizes`. The payload decodes as
  `(addr, uleb size)` and reports `0x4c0` = 1216 bytes for a function with `int a[300]`.

So the plumbing is real. The remaining work is decoding, lookup, and the two-pass retry —
not discovering whether LLVM will cooperate.

**Decoder caveats (learned the hard way in `sstest2.cpp`).** Treat these as implementation
requirements, not trivia:

- **The payload is not a clean array of records.** The observed 30-byte section was one
  real record (`addr=0x10224c000`, `frame=1216`), followed by zero padding to the section
  size. A naive loop decodes phantom `addr=0, frame=0` records. Stop at `i == size`, ignore
  records with `addr == 0`, and tolerate a truncated tail rather than reading past the
  buffer.
- **Decode after relocation, not at emission time.** In the raw object the address field is
  a relocation target (`.rela.stack_sizes` is a separate section). The MCJIT
  `allocateDataSection` path sees the relocated payload; the `ObjectCache` path
  (`notifyObjectCompiled`) sees the unrelocated object and must apply
  `R_AARCH64_ABS64` itself or defer decoding until load.
- **The section covers every function in the module, not just the entry point.** The
  lookup must map all addresses, not only the compiled entry; a module can contain the
  oversized function plus dozens of normal ones. Addresses can also be duplicated after
  thunk/alias creation — dedupe before deciding to recompile.
- **Section presence is a build property.** If the JIT triple ever changes to Mach-O, or
  the flag is lost, the section silently disappears. Check for it explicitly, log once, and
  fall back to the slab — never assert it exists (CPU-FRAME-035).
- **Cap the value.** Anything above `max_frame_size` (1 MB) is a compiler bug or a decode
  error; log and fall back rather than calling `sub sp` with it.

An alternative that avoids `allocateDataSection` entirely: subclass the existing
`ObjectCache` (`Utilities/JITLLVM.cpp:842-857`) so `notifyObjectCompiled` parses
`.stack_sizes` from the emitted object before it is loaded. The object is already produced
for the on-disk cache, so this costs nothing extra. Either route is acceptable; the
memory-manager route is simpler because the relocations are already applied.

**Why two passes are acceptable**

- Only modules containing an oversized function pay the second compile. The first compile still produces `.stack_sizes`; the module is not cached until the second succeeds.
- The retry is deterministic and bounded (one extra compile per affected module).
- Cache files are written only after success, so users do not see a half-cached module.

**Alternative if two-pass compile is too slow:** always emit the thunk for every GHC function whose *estimated* frame is > 4 KB using `llvm::Function` size/IR heuristics, then verify with `.stack_sizes` in debug. This trades a cheap IR walk for a second compile. `[U]` measure.

### 9.4 PPU gateway

The PPU gateway also reserves 8192 (`PPUThread.cpp:380`). Apply the same frame check to PPU functions. PPU code is less vector-heavy, but a large function can still exceed 8 KB. The same `.stack_sizes` mechanism works; the PPU pass has `base_register_lookup` and `hypervisor_context_offset` already configured.

### 9.5 Tests for A

- Unit: ULEB128 decoder, `.stack_sizes` parsing, address lookup, frame size alignment.
- Unit: thunk IR shape (one `sub`, one `mov sp`, one tail call; no `add sp`).
- Unit: overridden function name suffix, no duplicate thunks.
- Golden: compile a synthetic GHC module with a 12 KB vector spill; assert a thunk and a `-framed` body.
- Integration: Prototype 2 and Max Payne 3 reach gameplay; SPU cache builds; no stack corruption under ASan.
- Regression: modules without oversized functions compile once (counter test).
- Error path: a synthetic 2 MB frame is rejected, logs, and falls back.
- PPU variant: synthetic PPU function with large vector spills.

See §17 for the full test matrix.

---

## 10. Workstream B — memory, 16 KB pages, atomics, and fault notifications (P0/P1)

### 10.1 The 16 KB problem

Guest memory is modeled in 4 KB pages (`g_pages`, `vm::block_t`), but the host enforces 16 KB pages. Current code explicitly bails out:

- `vm.cpp:747` — `// If native page size exceeds 4096, don't map native pages (expected to be always mapped in this case)`
- `vm.cpp:981` — `// If native page size exceeds 4096, don't unmap native pages (always mapped)`
- `vm.cpp:1369`, `1884` — `map_critical(..., block_size_4k && page_size > 4096 ? rw : no)`
- `vm.cpp:1920-1931` — block flags for 64 KB guest pages.

Result: a range registered for `sys_mmapper_enable_page_fault_notification` has **no host holes**, so touching an unmapped guest page never raises `SIGSEGV`/`SIGBUS`. GT5 registers `start_addr=0x60000000`, size `0x10000000`, flags `0x1020a`; on x64/Rosetta it gets 164 faults and reaches the menu; native gets 0 and hangs at the logo. `[V]`

### 10.2 Design: a software-checked sub-page map for notification regions

Do **not** try to make 4 KB host mappings. Apple's VM page size is 16 KB; `mmap`/`mach_vm_map` round to it. Instead:

1. Keep a per-4 KB **guest protection bitmap** for every guest page (already partially exists via `g_pages`).
2. For a region with page-fault notification enabled, compute which 16 KB host pages are:
   - **fully mapped** → map RW, no checks;
   - **fully unmapped** → map `PROT_NONE`, let the hardware fault;
   - **partially mapped** → map RW, but mark the 4 KB sub-pages in a fast bitmap.
3. In `ReadMemory`/`WriteMemory` codegen for addresses that may fall in a notification region, emit:
   ```text
   if (unlikely(notification_region_active)) {
       if (unlikely(!subpage_mapped[addr >> 12])) {
           call vm::handle_guest_page_fault(addr, is_write);
       }
   }
   ```
   The fast path is a predictable load + test + branch. Keep it out of the inline hot path by using a per-region flag and a separate slow helper.
   **Scope caveat:** this only covers PPU `ReadMemory`/`WriteMemory`. Instruction
   fetch, SPU MFC DMA, RSX transfers, and `mmapper`'s own copies are separate paths and
   must either perform the same check or be proven unable to target a notification
   region. Enumerate them before writing the patch (§22 A8).
4. `handle_guest_page_fault`:
   - validates the guest mapping;
   - if invalid, queues a `sys_mmapper` page-fault event and blocks the faulting thread exactly like the x64 path;
   - if valid but the host page was `PROT_NONE` due to a partial hole, `mprotect(RW)` the host page and return to retry.
5. Signal handler remains the fallback for fully unmapped 16 KB pages. It must distinguish a real guest fault from a host JIT fault (existing `AArch64Signal` code) and must not recurse.

This is more code than a one-line fix, but it is the only design that preserves guest 4 KB semantics on a 16 KB host without single-stepping.

**Narrower alternative:** if the 64 KB guest block path is the only one that matters for GT5, fix the `block_size_64k` path first: 64 KB is 4 × 16 KB, so holes are expressible at 16 KB granularity. Instrument `_page_map` to log `bflags` and test a 64 KB region before building the general bitmap. `[U]` The #19881 reporter could not determine why the 64 KB path still had no holes; that is the first experiment.

### 10.3 Decommit/reset race

Current Apple code:

```cpp
ensure(::munmap(pointer, size) != -1);
ensure(::mmap(pointer, size, PROT_NONE, MAP_ANON | MAP_PRIVATE | (can_be_jit ? MAP_JIT : 0), -1, 0) == pointer);
```

`mmap` without `MAP_FIXED` is a hint; another thread can claim the range. #18423 changed it to `MAP_FIXED` without `MAP_JIT`, but #18656 reverted it due to reported crashes.

**Design:** stop unmapping for decommit/reset on Apple.

- Non-JIT guest memory: `mprotect(ptr, size, PROT_NONE)` + `madvise(ptr, size, MADV_FREE_REUSABLE)`. The address stays reserved; no race; pages are reclaimable.
- Commit: `mprotect(ptr, size, prot)` + `madvise(ptr, size, MADV_WILLNEED)`.
- JIT code: keep `MAP_JIT`; never `munmap` a live code range. Reset via `mprotect(PROT_NONE)` + `madvise`, and reuse the range.
- `memory_reset` becomes `decommit` + `commit`, with the same semantics.
- If a range must be truly returned, do it only at shutdown or under the global VM lock, and verify with `mach_vm_region`.

This is upstreamable and removes a whole class of “Verification failed (object: 0x0)” crashes.

**Measured cost (64 × 1 MB touched regions, `mem.c`):**

| Strategy | Decommit | `phys_footprint` after | Recommit + touch | Race-free |
|---|---:|---:|---:|---|
| `munmap` + `mmap(PROT_NONE)` (today) | 285 µs | 1.0 MB | 3.8 ms | **no** |
| `mprotect(PROT_NONE)` | 130 µs | 1.0 MB | 2.5 ms | **yes** |
| `madvise(MADV_FREE_REUSABLE)` | 207 µs | 1.0 MB | 2.1 ms | **yes** |

All three return the physical footprint to baseline. `mprotect` is the cheapest
decommit, `MADV_FREE_REUSABLE` the cheapest recommit, so use both. There is no
memory-footprint reason to keep the unsafe path, which answers the old “will never
unmapping cost 8 GB of RAM?” objection: it does not, because `mprotect`/`madvise`
reclaim the pages while keeping the address reservation.

### 10.4 Atomics audit

Already fixed:

- LSE2 16-byte `ldp` loads and `stp` stores.
- `CASPAL` for 16-byte CAS/exchange (#19977).
- `atomic_storage<T,16>` selects LSE2 at runtime via `utils::g_atomic_lse2`.

Still to audit:

1. **Fences.** `atomic_fence_acquire/release/seq_cst` should lower to the weakest correct AArch64 fence. `dmb ish` on every 16-byte load is expensive; acquire loads can use `ldar`/`ldapr` (LRCPC present) where applicable. `[U]` measure.
2. **`observe()`** is `return load()` with a TODO. On ARM64, a true observe can be a relaxed `ldp`; using acquire semantics may over-fence.
3. **`store()`** currently does `dmb ish; stp; dmb ish` even for relaxed stores. A relaxed 16-byte store on LSE2 is just `stp`; only release needs a fence. Fix and test.
4. **`exchange`** fallback path: ensure it also uses `CASP`/`SWP` when LSE is available, not just when LSE2.
5. **Sub-16-byte atomics** should use `ldadd`/`swpal`/`casal` where the operation is a simple RMW; LLVM may already do this.
6. **`atomic_wait_engine`** on Apple: #18429 ASan underflow. The wait engine stores waiter info on the stack; with 16 KB pages and different stack alignment, a buffer underflow may be exposed. Review lifetimes and add a red-zone test.

### 10.5 JIT W^X and icache

- Keep MAP_JIT only for code pages. Guest RAM must not be MAP_JIT (#18425).
- Batch `pthread_jit_write_protect_np(false/true)` around a whole module compile, not per function. Use an RAII scope with a nesting counter.
- After writing code, call `sys_icache_invalidate(start, len)` (or `__builtin___clear_cache`) before executing it. Verify whether LLVM's `RTDyldMemoryManager` already does this; if not, add it in `finalizeMemory`.
- The signal handler's write-protection recovery (#18707) stays, but should be a last resort, not the normal path.

### 10.6 Tests for B

- 16 KB page-size unit tests: reserve/commit/decommit/protect with unaligned addresses.
- Decommit race: 64 threads repeatedly decommit/commit while another thread reads; no `Verification failed`.
- Guest page fault: synthetic `sys_mmapper` region with 4 KB holes; assert fault count and recovery.
- 64 KB block path: holes at 16 KB granularity.
- Signal handler: `SIGSEGV` in guest vs JIT vs host; no infinite recursion.
- Atomics: litmus tests for load/store/CAS/exchange/fence; compare against `std::atomic` reference.
- W^X: compile/execute/compile again; no `EXC_BAD_ACCESS`; count toggles.
- icache: write a function, execute, overwrite, execute; observe new behavior.

---

## 11. Workstream C — NEON lowering and SPU SIMD (P1)

### 11.1 Current state

- `m_use_dotprod` / `m_use_i8mm` are enabled at runtime and used in SPU LLVM for `GB`, `GBH`, `GBB`, `SUMB`, and multiply paths. `[V]`
- `m_use_tbl2 = true`; `tbl2`/`tbx2` are used for `VPERM` and SPU `SHUFB`. `[V]`
- `sse2neon.h` is vendored for x86-style intrinsics, but ARM paths mostly use `CPUTranslator` NEON helpers.
- `pshufb` lowering is a `movi/and/tbl1` sequence copied from sse2neon (`CPUTranslator.cpp:37-79`). `[V]`

### 11.2 Mapping table (verified NEON forms)

| SPU/VMX op | Current shape | Apple NEON target | Notes |
|---|---|---|---|
| `shufb` (two tables) | `tbl2` / `tbx2` | `tbl2`/`tbx2` | already good; keep `m_use_tbl2` |
| `selb` | `bsl`/`bit`/`bif` | `bsl`/`bit`/`bif` | 1 uop, 2c |
| `cntb` | `cnt` + narrowing | `cnt` | 16-byte popcount |
| `clz`/`cls` | `clz`/`cls` | `clz`/`cls` | vector forms |
| `mpya`/`mpyh` | `vmlal`/`vmlaq` | `sdot`/`udot` where legal | dotprod already used |
| `sumb` | `udot` | `udot`/`vpadalq_u8` | already optimized |
| `rot`/`roti` | shifts/or | `shl`+`sri` or `ext` | prefer `ext` for byte rotates |
| XFloat clamp | `fminnm` | `fminnm` | single instruction |
| `vperm` | `tbl2` | `tbl2` | good |
| unaligned `lvlx`/`lvrx` | `ldr`+`pshufb` | `ldr`+`ext`/`tbl` | measure; masked stores are the risk |
| `stvlx`/`stvrx` | masked store | `bsl`+`str` or `st1` | avoid per-byte lowering |
| `vrefp`/`vrsqrtefp` | `frecpe`/`frsqrte` | same | accuracy caveats |

### 11.3 Concrete opportunities

1. **~~Remove x86-shaped IR.~~ Withdrawn.** `m_use_avx = true` on ARM is dead: every use
   in `SPULLVMRecompiler.cpp` is inside `#ifdef ARCH_X64` or the `#else` of an
   `ARCH_ARM64` block. The block-hash/PTEST 256-bit paths never compile on ARM. Do not
   spend a PR on this. `[V]`
2. **Prefer `tbl`/`tbl2` over `pshufb`.** `VPERM` already does. Check `CPUTranslator::pshufb` and all callers; the `movi/and/tbl1` sequence is correct for 16-byte one-table shuffle but has extra `and`. If indices are known to be < 16, drop the mask.
3. **Masked stores — measured, then deprioritised.** `CreateMaskedStore` on AArch64
   without SVE lowers to **31 conditional branches** plus a `cmlt/and/zip1/addv` mask
   reduction (measured with `llc -mcpu=apple-m4`, `mstore2.ll`). The replacement
   read-modify-write merge-store is 6 instructions and zero branches:
   `shl.16b`, `cmlt.16b`, `ldr q`, `bif.16b`, `str q`, `ret`.
   `PPUTranslator::STVLX/STVRX` (`PPUTranslator.cpp:3730,3758`) use the slow form.
   **However:** the decrypted local titles contain zero `LVLX/STVLX`, so this is a
   latent quality bug, not a measured win. Keep it as a P2 cleanup with a golden
   assembly test, not as a headline optimization.
4. **Widen `sumb`/`avg`/sad paths** with `udot`/`vpadal` where not already.
5. **BF16.** `bfmmla` exists on Apple. SPU has no BF16 op, but PPU `vmaddfp`/`vmaddfp`-like sequences might benefit from `bf16` only if precision allows. Do not change guest semantics; skip unless a bit-exact mapping exists.
6. **`fmlal`/`fmlal2`** (FP16 widening) for PPU/SPU FP16 paths. LLVM may already select them; check.
7. **Constant folding / shuffle matching.** `CPUTranslator` has `match_vr` and constant-fold paths. Extend to more SPU idioms (`shufb` with a constant mask, `selb` with constant, `rot` with constant). Fewer runtime ops is the biggest lever.

### 11.4 Do not chase

- `sse2neon` “precise” modes as a blanket fix; they add instructions. Fix the specific opcode semantics instead.
- 256-bit IR on NEON. It splits; it does not give AVX2 width.
- `fdiv` on hot paths: 8c on P, 9c on E, one unit. Use reciprocal + Newton only if the guest allows approximation.

### 11.5 Tests for C

- Differential: run each SPU opcode against the interpreter for random inputs (millions of vectors).
- Golden assembly: `shufb`, `selb`, `sumb`, `mpya`, `gbb`, `rot`, `stvlx` produce the expected NEON op (or at most N instructions).
- Edge cases: index ≥ 16, index 0x80 (zeroing), signed/unsigned saturation, NaN/Inf, denormals.
- Performance: per-op microbenchmark on P and E cores.
- Compile-time: no regression in SPU cache build time.

---

## 12. Workstream D — PPU codegen (P1)

### 12.1 Hot patterns

Measured guest mix (§5.3): scalar loads/stores (~2 M in Demon's Souls) and CR branches
(`bt`/`bf` 341 k) dominate; vector ops are 1–3% of instructions. Order the work
accordingly — a 10% win on `vperm` is worth less than a 10% win on CR/branch lowering.

- `VPERM` → `tbl2` (already).
- `VSEL` → `bsl`/`bit`/`bif`.
- `LVLX`/`LVRX`/`STVLX`/`STVRX` → unaligned load/store with `pshufb` + masked store.
- `LVEBX`/`STVEBX` (endian-swapped element) → `rev`/`rbit`.
- `VADD`/`VSUB`/`VMUL`/`VMADD` → NEON.
- `VCFSX`/`VCTSX` → conversion.
- `VUPK*`/`VPKP*` → pack/unpack shuffles.
- CR operations → flag-setting instructions and `csel`.
- Branch/compare fusion: `cmp + b.cc` fuses on Firestorm; LLVM should form it, but check.

### 12.2 Opportunities

1. **`LVLX`/`LVRX`**: current `pshufb` on a full 16-byte load is 2–3 ops. For the common case where the address is 16-byte aligned, skip the shuffle. For unaligned, `ext` may beat `tbl`. Add a runtime `addr & 15` branch only if it predicts well; otherwise use a branchless `ext`.
2. **Masked stores**: same as SPU. Use `bsl` with a mask table indexed by `addr & 15`, or a branch for the aligned case.
3. **`VPERM` with `ra == rb`**: already `tbl` with `~c & 0xf`. Good.
4. **CR field updates**: ensure LLVM fuses `cmp`/`tst` with branches and `csel`; inspect assembly for `setcc`-style materialization.
5. **Addressing modes**: `ReadMemory`/`WriteMemory` should fold base+offset into `ldr`/`str` addressing. Check for redundant `add` before loads.
6. **`vm::g_base_addr` reloads**: the translator may reload the base pointer per access. Hoist into a register where the IR allows.
7. **Inlining**: PPU helpers (e.g. `__lv2_*`) are calls; force-inline small ones. Measure.

### 12.3 Tests for D

- Differential PPU vector tests against the interpreter.
- Golden assembly for `LVLX`, `STVLX`, `VPERM`, `VSEL`, CR ops.
- Address alignment matrix (0–15) for unaligned ops.
- Random instruction fuzz with a reference interpreter.

---

## 13. Workstream E — scheduling and LLVM target configuration (P2)

### 13.1 What can be done without patching LLVM

1. **Use `-mcpu=apple-m4`** (or the host CPU from `getHostCPUName()`), plus explicit `+lse2`, `+dotprod`, `+i8mm`, `-sve`, `-sve2` attributes. This is already the direction of #18133; extend it to LSE2/CSSC and log the final feature set once.
2. **Prefer `apple-m4` over `apple-m1`** on A18/M4-class machines because `apple-a18` aliases to `apple-m4`. Keep `apple-m1` for a floor test.
3. **Set `-mllvm` knobs deliberately** (via `EngineBuilder` target options or `TargetMachine` flags):
   - `-aarch64-enable-ccmp` (conditional-compare fusion): verify it is on; it reduces branch count. `-aarch64-ccmp-limit` caps speculation per block.
   - `-aarch64-enable-ldst-opt` / store-pair formation: on M4, `FeatureStorePairSuppress` is removed, so store pairing is allowed. But `LDP` followed by `STR` has a 2× penalty (LLVM #235875). Add a post-RA pass or a peephole to avoid the adjacency, or pass `-mllvm -aarch64-...` if a knob exists.
   - `-enable-misched` / `-misched-postra`: test whether post-RA scheduling helps or hurts. `[U]`
4. **Reduce reliance on scheduling** by reducing spills and memory ops. This is the main lever.
5. **Code layout**: order hot blocks and functions to improve i-cache/branch prediction. LLVM's block placement is generic; a post-pass could reorder based on profile data. `[U]`

### 13.2 MachineFunction pass (long-term)

A `AppleGHCPostRA` pass could:

- coalesce `stp`/`ldp` prologues;
- avoid `ldp`+`str` adjacency;
- sink/hoist spills;
- replace `mov`/`movk` pairs with literal pools where smaller;
- remove redundant `dmb` by fence strength;
- align hot loops to 128 B (cache line) but not over-align (i-cache pressure).

This is the natural home for the FIXME at `AArch64JIT.cpp:88-89`. It is also the riskiest change; do it after A/B/C.

### 13.3 Upstream LLVM contribution

Propose `AArch64SchedAppleFirestorm.td` with:

- `IssueWidth = 8`, `MicroOpBufferSize ≈ 600`, `LoadLatency = 3`, `MispredictPenalty ≈ 14`;
- 4 int pipes, 2 branch, 1 mul, 1 div, 2 ld/st, 3 FP/SIMD;
- fusion/elimination entries;
- `WriteAtomic` modeled with `CAS`/`CASP` latencies.

Do this upstream, not in RPCS3. Until it lands, RPCS3 must not depend on it. The design doc records the target numbers so the upstream patch has a spec.

### 13.4 Tests for E

- `llc` feature dump test: assert the final `-mcpu`/attributes include LSE2, DotProd, I8MM, and exclude SVE.
- Golden assembly for a spill-heavy function: assert no `ldp`+`str` adjacency, paired prologue, no redundant `dmb`.
- Benchmark: `-mcpu=apple-m1` vs `apple-m4` on the same binary, per machine; no universal constant.

---

## 14. Workstream F — threads, QoS, P/E placement, and spin waits (P2, measurement-gated)

### 14.1 Current state is contradictory

`Utilities/Thread.cpp` (Apple branch) does:

```cpp
pthread_attr_setstacksize(&attrs, 0x800000);
pthread_attr_set_qos_class_np(&attrs, QOS_CLASS_USER_INTERACTIVE, 0);
pthread_attr_setschedpolicy(&attrs, SCHED_RR);
pthread_attr_setschedparam(&attrs, &sp); // priority 99
```

Apple's `pthread/qos.h` says:

> Subsequent calls to interfaces such as `pthread_attr_setschedparam()` that are incompatible or in conflict with the QOS class system will unset the QOS class requested with this interface.

So the QoS class is **unset**, measured on macOS 27.2 (`qos.cpp`): the thread reports
`UNSPECIFIED(0)`, `SCHED_RR`, priority 99. The same experiment shows that
`pthread_setschedparam()` on a running QoS thread also unsets QoS and leaves
`SCHED_OTHER` priority 47. `thread_ctrl::set_native_priority()`
(`Utilities/Thread.cpp:3915-3946`) does this on macOS via `scoped_priority`
(`Thread.h:349-364`), so both the creation path and the dynamic-priority path destroy QoS.

This explains why #17905 “fixed throttling” — the thread really is running at the highest
realtime priority — but it also means all emulation threads, including the offloader and
audio, are realtime. `[V]`

### 14.2 Design (revised after measurement)

The naive version of this section said “replace `SCHED_RR` with QoS”. Measurement says
that would be a throughput regression in the synthetic worst case:

The measurements have two generations. The first (`place.cpp`) compared the full
RPCS3-style attribute set against QoS classes and reported `SCHED_RR` at 1461 Miters/s
versus 929–958 for QoS. That run conflated policy, priority, and QoS. The clean A/B
(`place2.cpp`) separates them, all four configurations in one process:

| 8 threads, 2 s, same binary warm | Total throughput | per-core |
|---|---:|---:|
| `SCHED_OTHER`, default priority | 1143 Miters/s | ~190 |
| `SCHED_RR`, priority 99 (today) | **1799 Miters/s** | ~300 |
| QoS `USER_INTERACTIVE` | 1180 Miters/s | ~197 |
| `SCHED_RR`, priority 99 (repeat) | 1791 Miters/s | ~299 |

The correction matters: **the win belongs to `SCHED_RR`/99, not to "having no QoS"**.
`SCHED_OTHER` and QoS are within noise of each other (1143 vs 1180), while `SCHED_RR` is
~55% faster than both and reproduces across the two runs. Absolute throughput also varies
~20% between process launches (the same `SCHED_RR` configuration measured 1461 in the
earlier binary and 1799 here), so **only ratios measured inside one run are valid**; do
not quote Miters/s as a stable number.

`SCHED_RR` behaves like a very low-preemption scheduler: with 8 runnable threads on 6
cores it delivers ~300 Miters/s per core, versus ~190 for `SCHED_OTHER`/QoS. That is not a
reason to keep it (latency and fairness are still wrong, and every emulation thread is
realtime), but it is a reason to **not** present QoS as a speed fix and to require a
real-title A/B before changing the default.

Revised plan, in order:

0. **Measure first.** Add a debug mode that logs the actual `pthread_getschedparam` and
   `pthread_get_qos_class_np` of every emulation thread at startup. This turns D1/D2 into
   a visible invariant and gives the A/B a baseline.
1. **Stop the accidental destruction.** Do not call `pthread_attr_setschedpolicy` /
   `pthread_attr_setschedparam` after setting QoS. Make `set_native_priority()` a no-op
   on Apple (or implement it via `pthread_set_qos_class_self_np` with a relative
   priority), because it silently drops QoS and the priority.
2. **Keep `SCHED_RR` behind a config flag for one release.** Default remains `SCHED_RR`
   until a real-title A/B (RR7, Demon's Souls, Mega Man 9) shows QoS is not worse in
   median and p99. The synthetic result above is the null hypothesis.
3. **Assign QoS classes only where they add information** (if the A/B passes):
   PPU `USER_INTERACTIVE`, SPU `USER_INITIATED`, RSX render `USER_INITIATED`, offloader
   `UTILITY`, background cache workers `UTILITY`. Audio stays out of the experiment until
   p99 glitching is measured.
4. **Affinity tags are not placement control.** With `countr_zero(full_mask) == 0` the
   current call is a no-op. If a hint is wanted, use a deliberate tag per SPU group, and
   document that Apple may ignore it.
5. **Spin waits — corrected by measurement.** `ISB` is the right pause replacement
   (#18151). **`WFE` is not a power win on macOS 27.2/A18 Pro:** a pure `wfe` loop
   runs at 0.74 M ops/s and burns a full core (`ps` 90–98%), and its wake latency is
   *worse* than `isb`'s (p50 1 µs vs 0 ns). The only primitive that actually parks is
   `os_sync_wait_on_address` (already used in `Utilities/sync.h:90`): 0.0000 s CPU over
   500 ms, p50 9 µs, p99 150 µs. The design is therefore:
   - a bounded `isb` spin (tens of microseconds) for the uncontended fast path;
   - then `os_sync_wait_on_address` with a timeout for the slow path;
   - never a hand-rolled WFE spinlock;
   - keep the existing futex path, which already does exactly this.
   The old plan (“WFE/SEVL with a futex fallback”) is withdrawn. `[V]`
   `measurements/2026-11-12/README.md` §6.
6. **Priority inversion.** Realtime threads that block on a futex held by a utility thread
   can invert. `SCHED_RR` does not prevent this; QoS does not either. The fix is to keep
   blocking critical sections short and to avoid holding locks across DMA waits.

If step 3 passes, the intended class assignment is:

- PPU threads: `QOS_CLASS_USER_INTERACTIVE` (latency-critical, needs P-cores).
- SPU threads: `QOS_CLASS_USER_INITIATED` (throughput + latency).
- RSX render: `QOS_CLASS_USER_INITIATED`.
- RSX offloader: `QOS_CLASS_UTILITY` (throughput, can use E-cores).
- Audio/input: `QOS_CLASS_USER_INTERACTIVE` or the audio workgroup API; do not starve.
- Background/cache workers: `QOS_CLASS_UTILITY` or `BACKGROUND`.

**Oversubscription policy.** On 2P+4E with ~10 runnable threads, the OS cannot pin the
split for us, and the affinity call does not. If a hint is ever added, the intended shape
is: 2 PPU + 2 hottest SPU on P, remaining SPU on E, offloader on E. The exact split must be
measured per title; expose a config knob (`Preferred SPU Threads` exists) and an
`Auto`/`PreferP`/`PreferE`/`Balanced` enum only if the measurement justifies it.

### 14.3 Tests for F

- QoS readback: `pthread_get_qos_class_np` after creation returns the intended class (catches the schedparam conflict).
- **QoS preservation**: create with QoS, call `set_native_priority(+1)`, assert QoS is still set and the relative priority changed, not the policy.
- **Policy invariant**: no emulation thread reports `SCHED_RR` when the "QoS" mode is selected; every thread reports `SCHED_RR` in the legacy mode.
- Affinity: N threads with tags land on the expected perf levels (best-effort; assert no crash).
- Spin wait: correctness under contention; power measurement (`powermetrics` or `perf` counters) before/after.
- Oversubscription: 6 SPU + 2 PPU synthetic load; no starvation; frame time p99.
- **Throughput A/B**: replicate the 8-thread `place.cpp` harness inside `rpcs3_test` and assert the chosen default is within 5% of the legacy mode, or document why it is not.
- Regression: audio does not glitch under full load.

### 14.4 SME/SME2: analyze, do not ship in v1

The A18 Pro reports SME/SME2. The public ARM SME extension provides:

- streaming SVE mode (vector length up to 2048 bits, implementation-defined);
- ZA tile storage (up to 512 bits per tile element? implementation-defined);
- outer-product / matrix multiply instructions.

Potential: a 512-bit streaming SVE could process four 128-bit SPU vectors at once, or accelerate PPU matrix-like sequences. But:

- macOS user code must opt into streaming mode (`__arm_streaming`, `SMSTART`/`SMSTOP`), and the OS must save/restore ZA state.
- RPCS3's guest model is 128-bit; widening changes semantics and requires shuffles.
- LLVM's SME support is evolving; `apple-m4` lists SME but the scheduler is Cyclone.
- Unknown whether macOS exposes streaming SVE to JIT-generated code without entitlements.

**Verified on the reference machine (2026-11-12).** The CPU advertises
`hw.optional.arm.FEAT_SME = 1` and `FEAT_SME2 = 1`, and SME instructions are accepted
(no `SIGILL`), but they are inert:

| Probe | Result |
|---|---|
| `mrs x0, svcr` inside/after `smstart` | `0x0` — streaming mode never becomes active |
| `cntb` / `cntw` / `cntd` / `rdsvl` after `smstart` | `0`, `0`, `0`, `0` |
| `zero {za}` + `mova za0h.s[…]` + `mova` back | reads back zeros |
| plain `cntb` with no `smstart` | `SIGILL` (4) |
| SVE `add z0.b` without `smstart` | `SIGILL`; with `smstart` it executes but is a 128-bit NEON-shaped op |
| SVE `add z0.b` after `smstart` | executes, but `SVCR` still reads 0 |

Interpretation: macOS 27.2 traps/emulates the SME state instructions and does not give
user space a real streaming vector length or ZA. There is nothing to exploit. This closes
the SME research question for the current OS.

**Decision:** keep SME out of the v1 design. Add a research spike (§20 Q6) that measures `svcntb()` in streaming mode and a simple ZA matmul. Revisit only if the spike shows a > 20% win on an SPU/PPU kernel and the OS permits it.

---

## 15. Workstream G — JIT compile time, cache, and code layout (P2)

### 15.1 Compile-time costs

- LLVM `Aggressive` opt level; PPU/SPU modules can be large.
- GHC/no-frame code has many tail calls and inline asm, which stresses register allocation.
- `.stack_sizes` adds a second compile only for oversized functions (A).
- (Removed in v3: the claim that `m_use_avx` widens ARM IR. All `m_use_avx` uses are
  x86-only.)

### 15.2 Opportunities

1. **Audit the pass pipeline.** `-O3` may be overkill for JIT; `-O2` with targeted vector combines can be faster to compile and produce similar code. Measure per-game.
2. **Function-level caching.** Already exists; ensure cache keys include the exact CPU/feature set (`apple-m4`, LSE2, DotProd, I8MM, LLVM version). A cache built with different features must not be reused.

   **Audited (2026-11-12):**
   - PPU key is `v9-kusa-<hash16>-<settings>-<resolved CPU>.obj`. It already includes
     the resolved CPU (`jit_compiler::cpu()` → `apple-m4` here) and the settings
     bitfield. It does not include the LLVM version, which is fine for machine-code
     objects, but **the `v9` prefix must be bumped in the first PR that changes IR
     generation** (frame checks, masked-store path, CR packing). Otherwise a stale
     object is silently reused.
   - SPU's persistent cache is `spu-<block_size>-v1-tane.dat` and contains raw guest
     code, not compiled artifacts; the `llvm/` object directory is written only under
     `spu_debug` and is never loaded. There is no SPU codegen-cache staleness bug.
   - Consequence: add a test that a cache written by one CPU-feature configuration is
     not read by another, and a test that the version constant changes when the IR
     changes (fail the build if the two are edited together without a bump).
3. **Parallel compilation.** `Max LLVM Compile Threads` exists; ensure it uses all P/E cores appropriately and does not thrash.
4. **Batch W^X toggles.** One toggle per module, not per function.
5. **Avoid recompiles.** The #19939 two-pass compile should be rare; log a counter. The #19801 TBL2 fallback recompiles without TBL2; fix the root cause (register scavenging) instead of relying on the fallback.
6. **Code layout.** Align hot functions to 128 B; avoid huge functions that exceed i-cache; consider `-mllvm -align-all-functions=...`? Measure.

### 15.3 Tests for G

- Cache key test: different feature sets produce different keys.
- Compile-time benchmark: cold cache for each corpus title; native vs Rosetta.
- Recompile counter: normal modules compile once; oversized modules at most twice.
- W^X toggle count per module.

---

## 16. Workstream H — experimental / later

| Idea | Why it might help | Risk | Gate |
|---|---|---|---|
| Custom LLVM schedule model | better latency/issue on Firestorm | upstream only; toolchain dependency | upstream first |
| `WFE`-based spin waits | power + wake latency | correctness if event register mishandled | F |
| 2 MB host pages | TLB reach | macOS does not expose `MADV_HUGEPAGE`; kernel may use superpages automatically | measure only |
| Block linking / superblocks | fewer dispatches | correctness, code size | later |
| Software pipelining of SPU loops | hide latencies | LLVM already does; hand pass risky | later |
| SME/streaming SVE | 4× vector width | OS/ABI/toolchain unknown | research spike |
| PGO from game replays | better layout/branching | build complexity, reproducibility | later |
| `os_signpost` instrumentation | precise profiling | none | now (tooling) |

---

## 17. Verification and test plan

### 17.1 Layers

| Layer | What | Runs where | Gate |
|---|---|---|---|
| L0 | Unit tests (`rpcs3_test`) for pure logic, decoders, atomics, bitmaps | CI, every PR | must pass |
| L1 | IR/assembly golden tests for codegen | CI, every codegen PR | must pass |
| L2 | Differential tests vs interpreter (random vectors/instructions) | CI/nightly | must pass |
| L3 | Game boot/regression on the local corpus | local/self-hosted Mac | every behavioral PR |
| L4 | Native-vs-Rosetta performance | local/self-hosted Mac | release gate |
| L5 | Soak/ASan/UBSan/TSan | nightly | no new findings |

### 17.2 Test ID scheme

`CPU-<area>-<nnn>`. Areas: `FEAT`, `FRAME`, `ATOM`, `VM`, `NEON`, `PPU`, `THR`, `JIT`, `FUZZ`, `GAME`, `PERF`.

The implementation phase must land **at least 250** tests. The matrix below is the contract; exact names can change, coverage cannot.

#### FEAT — feature detection and target attributes (24)

| ID | Test |
|---|---|
| CPU-FEAT-001 | `has_lse2()` true on Apple arm64 |
| CPU-FEAT-002 | `has_dotprod()` true on A18/M4 |
| CPU-FEAT-003 | `has_i8mm()` true on A18/M4 |
| CPU-FEAT-004 | `has_sve()` false on Apple |
| CPU-FEAT-005 | `has_sve2()` false on Apple |
| CPU-FEAT-006 | `sve_length()` is not called when SVE absent |
| CPU-FEAT-007 | `has_sha3()` true |
| CPU-FEAT-008 | `has_sha512()` true |
| CPU-FEAT-009 | `has_bf16()` true |
| CPU-FEAT-010 | `has_sme()` true but unused by default |
| CPU-FEAT-011 | `getHostCPUName()` maps A18 to `apple-m4` |
| CPU-FEAT-012 | `jit_compiler::cpu("")` returns `apple-m4` |
| CPU-FEAT-013 | explicit `-mcpu=apple-m1` still works (floor) |
| CPU-FEAT-014 | target attributes include `+lse2` via `apple-m4` |
| CPU-FEAT-015 | target attributes include `+dotprod` |
| CPU-FEAT-016 | target attributes include `+i8mm` |
| CPU-FEAT-017 | target attributes include `-sve` |
| CPU-FEAT-018 | target attributes include `-sve2` |
| CPU-FEAT-019 | unknown CPU falls back to `cortex-a78` only when generic |
| CPU-FEAT-020 | `get_cpu_brand()` includes `(2P+4E)` |
| CPU-FEAT-021 | `get_cpu_name()` on Apple does not crash (may be empty) |
| CPU-FEAT-022 | feature logging is stable/deterministic |
| CPU-FEAT-023 | cache key changes when feature set changes |
| CPU-FEAT-024 | no `arm_sve.h` code path on Apple |

#### FRAME — GHC frames and gateway scratch (34)

| ID | Test |
|---|---|
| CPU-FRAME-001 | `.stack_sizes` section is emitted when flag 0x4 |
| CPU-FRAME-002 | `setProcessAllSections(true)` is set |
| CPU-FRAME-003 | ULEB128 decode: 0, 1, 127, 128, 16384, 1 MB |
| CPU-FRAME-004 | address→size map handles duplicate addresses |
| CPU-FRAME-005 | frame size ≤ 8 KB → no thunk |
| CPU-FRAME-006 | frame size = 8 KB → no thunk (boundary) |
| CPU-FRAME-007 | frame size = 8 KB + 1 → thunk |
| CPU-FRAME-008 | frame size alignment 16 |
| CPU-FRAME-009 | thunk loads saved SP from hypervisor context |
| CPU-FRAME-010 | thunk subtracts `align16(frame)+16` |
| CPU-FRAME-011 | thunk does not restore SP |
| CPU-FRAME-012 | thunk tail-calls body |
| CPU-FRAME-013 | body suffix `-framed` is unique |
| CPU-FRAME-014 | no duplicate thunks on retry |
| CPU-FRAME-015 | overrides survive retry |
| CPU-FRAME-016 | compile fails then succeeds once |
| CPU-FRAME-017 | frame > 1 MB is rejected with log |
| CPU-FRAME-018 | frame == 1 MB is accepted |
| CPU-FRAME-019 | PPU gateway gets the same check |
| CPU-FRAME-020 | SPU gateway gets the same check |
| CPU-FRAME-021 | normal module compiles once (counter) |
| CPU-FRAME-022 | oversized module compiles at most twice |
| CPU-FRAME-023 | cache is not written after first failed pass |
| CPU-FRAME-024 | cache is written after successful pass |
| CPU-FRAME-025 | `use_stack_frames=false` path unchanged |
| CPU-FRAME-026 | old naive estimator remains compiled but unused |
| CPU-FRAME-027 | 12 KB vector-spill synthetic function survives |
| CPU-FRAME-028 | 64 KB vector-spill synthetic function survives |
| CPU-FRAME-029 | thunk + body under ASan |
| CPU-FRAME-030 | thunk + body under UBSan |
| CPU-FRAME-031 | tail escape resets SP correctly |
| CPU-FRAME-032 | repeated far jumps do not grow stack |
| CPU-FRAME-033 | gateway SP is 16-byte aligned |
| CPU-FRAME-034 | 6 SPU threads × 64 KB frames do not exhaust 8 MB stacks |
| CPU-FRAME-035 | missing `.stack_sizes` disables the check and logs, never crashes |

#### ATOM — atomics and fences (40)

| ID | Test |
|---|---|
| CPU-ATOM-001 | 16-byte load uses `ldp` when LSE2 |
| CPU-ATOM-002 | 16-byte load uses `ldaxp` loop when no LSE2 |
| CPU-ATOM-003 | 16-byte store uses `stp` when LSE2 |
| CPU-ATOM-004 | 16-byte store uses `stlxp` loop when no LSE2 |
| CPU-ATOM-005 | CAS 16 uses `caspal` when LSE2 |
| CPU-ATOM-006 | CAS 16 returns correct previous value on failure |
| CPU-ATOM-007 | CAS 16 success path |
| CPU-ATOM-008 | exchange 16 uses `caspal` |
| CPU-ATOM-009 | exchange 16 returns old value |
| CPU-ATOM-010 | relaxed load has no unnecessary `dmb` |
| CPU-ATOM-011 | acquire load has acquire semantics |
| CPU-ATOM-012 | release store has release semantics |
| CPU-ATOM-013 | seq_cst store has seq_cst semantics |
| CPU-ATOM-014 | `observe()` is at least relaxed |
| CPU-ATOM-015 | `atomic_fence_acquire` lowers to correct instruction |
| CPU-ATOM-016 | `atomic_fence_release` lowers to correct instruction |
| CPU-ATOM-017 | `atomic_fence_seq_cst` lowers to `dmb ish` |
| CPU-ATOM-018 | 8-byte CAS uses `casal` |
| CPU-ATOM-019 | 4-byte CAS uses `casal` |
| CPU-ATOM-020 | 8-byte exchange uses `swpal` |
| CPU-ATOM-021 | 4-byte exchange uses `swpal` |
| CPU-ATOM-022 | `ldadd` used for fetch_add where applicable |
| CPU-ATOM-023 | `ldclr` used for fetch_and where applicable |
| CPU-ATOM-024 | 16-byte `fat_ptr` queue push/pop under contention |
| CPU-ATOM-025 | RSX offload queue stress 10M ops |
| CPU-ATOM-026 | no reservation starvation with two threads |
| CPU-ATOM-027 | atomics on unaligned address are rejected |
| CPU-ATOM-028 | atomics across 16 KB page boundary |
| CPU-ATOM-029 | `atomic_wait_engine` notify/wait basic |
| CPU-ATOM-030 | `notify_all` with many waiters |
| CPU-ATOM-031 | waiter cancellation |
| CPU-ATOM-032 | ASan red-zone test for wait engine (#18429) |
| CPU-ATOM-033 | futex timeout path |
| CPU-ATOM-034 | futex wake-by-address-any/all |
| CPU-ATOM-035 | 16-byte atomic under TSan |
| CPU-ATOM-036 | compare_exchange ABA stress |
| CPU-ATOM-037 | memory order litmus (MP) |
| CPU-ATOM-038 | memory order litmus (SB) |
| CPU-ATOM-039 | memory order litmus (LB) |
| CPU-ATOM-040 | no `WriteAtomic` scheduler assertion with CASP |

#### VM — memory, 16 KB pages, page faults (36)

| ID | Test |
|---|---|
| CPU-VM-001 | `get_page_size() == 16384` on Apple |
| CPU-VM-002 | reserve/commit/decommit 4 KB unaligned |
| CPU-VM-003 | reserve/commit/decommit 16 KB aligned |
| CPU-VM-004 | decommit keeps address reserved |
| CPU-VM-005 | decommit returns pages (`vmmap`/`madvise`) |
| CPU-VM-006 | decommit/commit race with 64 threads |
| CPU-VM-007 | no `Verification failed` under race |
| CPU-VM-008 | `memory_reset` race-free |
| CPU-VM-009 | MAP_JIT only for code pages |
| CPU-VM-010 | guest RAM mapping is not MAP_JIT |
| CPU-VM-011 | JIT code page is MAP_JIT |
| CPU-VM-012 | JIT code executes after write-protect toggle |
| CPU-VM-013 | `sys_icache_invalidate` called after code write |
| CPU-VM-014 | overwrite + execute sees new code |
| CPU-VM-015 | 4 KB guest hole in 16 KB host page |
| CPU-VM-016 | fully unmapped 16 KB page faults |
| CPU-VM-017 | partial 16 KB page software check |
| CPU-VM-018 | valid access to partial page does not fault |
| CPU-VM-019 | invalid access queues page-fault event |
| CPU-VM-020 | `sys_mmapper_enable_page_fault_notification` registers |
| CPU-VM-021 | fault count matches x64 reference on synthetic region |
| CPU-VM-022 | `sys_ppu_thread_recover_page_fault` completes |
| CPU-VM-023 | 64 KB block size with 16 KB holes |
| CPU-VM-024 | 64 KB block size full mapping |
| CPU-VM-025 | 2 MB alignment/superpage behavior logged |
| CPU-VM-026 | 128 B cache-line padding for ring buffers |
| CPU-VM-027 | VM reservation lock stress |
| CPU-VM-028 | `g_exec_addr` write path |
| CPU-VM-029 | write-protection recovery signal path |
| CPU-VM-030 | false SIGSEGV in JIT does not loop |
| CPU-VM-031 | signal handler distinguishes guest/JIT/host |
| CPU-VM-032 | 16 KB page boundary vector access |
| CPU-VM-033 | 4 GB guest reservation |
| CPU-VM-034 | per-SPU 256 KB local store mapping |
| CPU-VM-035 | decommit under memory pressure |
| CPU-VM-036 | shutdown unmaps cleanly |
| CPU-VM-037 | `mprotect(PROT_NONE)` decommit returns `phys_footprint` to baseline (measured baseline: `mem.c`) |
| CPU-VM-038 | decommit leaves the address reserved; a concurrent reader never sees a different mapping |

#### NEON — SPU SIMD (50)

| ID | Test |
|---|---|
| CPU-NEON-001 | `shufb` index 0 |
| CPU-NEON-002 | `shufb` index 15 |
| CPU-NEON-003 | `shufb` index 16 (second table) |
| CPU-NEON-004 | `shufb` index 31 |
| CPU-NEON-005 | `shufb` index 0x80 → zero |
| CPU-NEON-006 | `shufb` all 256 index values |
| CPU-NEON-007 | `shufb` random vectors × 1M |
| CPU-NEON-008 | `selb` all masks |
| CPU-NEON-009 | `cntb` all byte values |
| CPU-NEON-010 | `cls` all byte values |
| CPU-NEON-011 | `clz` all byte values |
| CPU-NEON-012 | `mpya` signed saturation |
| CPU-NEON-013 | `mpyh` signed |
| CPU-NEON-014 | `mpyu` unsigned |
| CPU-NEON-015 | `sumb` saturation |
| CPU-NEON-016 | `sumb` random |
| CPU-NEON-017 | `avg` rounding |
| CPU-NEON-018 | `absdb` |
| CPU-NEON-019 | `rot` by 0..7 |
| CPU-NEON-020 | `roti` immediate |
| CPU-NEON-021 | `shl`/`shr` byte |
| CPU-NEON-022 | `shlh`/`shlqby` |
| CPU-NEON-023 | `gbb` bit gather |
| CPU-NEON-024 | `gbh` bit gather |
| CPU-NEON-025 | `gb` bit gather |
| CPU-NEON-026 | `fscrwr`/`fpscr` |
| CPU-NEON-027 | `fma` single precision |
| CPU-NEON-028 | `fms` single precision |
| CPU-NEON-029 | `fnms` |
| CPU-NEON-030 | `fdiv` |
| CPU-NEON-031 | `fsqrt` |
| CPU-NEON-032 | `frecf` |
| CPU-NEON-033 | `frsqef` |
| CPU-NEON-034 | `fceq` |
| CPU-NEON-035 | `fcgt` |
| CPU-NEON-036 | `fcmeq` |
| CPU-NEON-037 | XFloat clamp min/max |
| CPU-NEON-038 | XFloat NaN handling |
| CPU-NEON-039 | XFloat denormal handling |
| CPU-NEON-040 | float→int saturate |
| CPU-NEON-041 | int→float convert |
| CPU-NEON-042 | `vperm` same register |
| CPU-NEON-043 | `vperm` two registers |
| CPU-NEON-044 | `vsel` |
| CPU-NEON-045 | `lvsl`/`lvsr` |
| CPU-NEON-046 | `lvlx` 16 alignments |
| CPU-NEON-047 | `lvrx` 16 alignments |
| CPU-NEON-048 | `stvlx` 16 alignments |
| CPU-NEON-049 | `stvrx` 16 alignments |
| CPU-NEON-050 | all above match interpreter bit-exactly |
| CPU-NEON-051 | `STVLX`/`STVRX` lowering is branch-free merge-store (golden assembly, ≤ 8 instructions) |

#### PPU — codegen (30)

| ID | Test |
|---|---|
| CPU-PPU-001 | `VPERM` same register |
| CPU-PPU-002 | `VPERM` two registers |
| CPU-PPU-003 | `VSEL` |
| CPU-PPU-004 | `LVLX` 16 alignments |
| CPU-PPU-005 | `LVRX` 16 alignments |
| CPU-PPU-006 | `STVLX` 16 alignments |
| CPU-PPU-007 | `STVRX` 16 alignments |
| CPU-PPU-008 | `LVEBX` endian swap |
| CPU-PPU-009 | `STVEBX` endian swap |
| CPU-PPU-010 | `VUPK*` |
| CPU-PPU-011 | `VPKP*` |
| CPU-PPU-012 | `VADD`/`VSUB` |
| CPU-PPU-013 | `VMUL`/`VMADD` |
| CPU-PPU-014 | `VCFSX`/`VCTSX` |
| CPU-PPU-015 | CR compare + branch fusion |
| CPU-PPU-016 | `csel` generation |
| CPU-PPU-017 | `cmp`+`b.cc` fusion |
| CPU-PPU-018 | base+offset addressing fold |
| CPU-PPU-019 | no redundant base reload |
| CPU-PPU-020 | `lwarx`/`stwcx.` reservation |
| CPU-PPU-021 | `dcbz` |
| CPU-PPU-022 | `sync`/`lwsync` |
| CPU-PPU-023 | `mftb` |
| CPU-PPU-024 | `sc` syscall |
| CPU-PPU-025 | indirect branch |
| CPU-PPU-026 | tail call |
| CPU-PPU-027 | big function frame |
| CPU-PPU-028 | inline helper |
| CPU-PPU-029 | random vector fuzz × 1M |
| CPU-PPU-030 | all above match interpreter |
| CPU-PPU-031 | full CR field emits 4 byte-stores or one packed store; branch-only use folds to `cmp`+`b.cc` |

#### THR — threads/QoS (16)

| ID | Test |
|---|---|
| CPU-THR-001 | QoS readback for PPU = USER_INTERACTIVE |
| CPU-THR-002 | QoS readback for SPU = USER_INITIATED |
| CPU-THR-003 | QoS readback for offloader = UTILITY |
| CPU-THR-004 | `pthread_setschedparam` does not unset QoS |
| CPU-THR-005 | no `SCHED_RR` on emulation threads |
| CPU-THR-006 | affinity tag groups SPU+PPU |
| CPU-THR-007 | 6 SPU + 2 PPU synthetic load |
| CPU-THR-008 | no starvation |
| CPU-THR-009 | p99 frame time under load |
| CPU-THR-010 | `WFE` spin correctness |
| CPU-THR-011 | `WFE` wake latency |
| CPU-THR-012 | `WFE` power vs `ISB` |
| CPU-THR-013 | futex fallback after spin budget |
| CPU-THR-014 | audio under full emulation load |
| CPU-THR-015 | thread shutdown clean |
| CPU-THR-016 | no priority inversion in RSX queue |
| CPU-THR-017 | QoS preserved after `set_native_priority` |
| CPU-THR-018 | throughput A/B within 5% or documented |
| CPU-THR-019 | bounded spin then `os_sync_wait_on_address` parks (CPU ≈ 0 over 500 ms) |
| CPU-THR-020 | no WFE-based spinlock anywhere; missed-wakeup stress passes |

#### JIT — cache, compile, layout (20)

| ID | Test |
|---|---|
| CPU-JIT-001 | cache key includes CPU |
| CPU-JIT-002 | cache key includes LLVM version |
| CPU-JIT-003 | cache key includes LSE2/DotProd/I8MM |
| CPU-JIT-004 | cache miss recompiles |
| CPU-JIT-005 | cache hit loads |
| CPU-JIT-006 | cache corruption rejected |
| CPU-JIT-007 | cold compile PPU |
| CPU-JIT-008 | cold compile SPU |
| CPU-JIT-009 | compile time budget |
| CPU-JIT-010 | W^X toggle count |
| CPU-JIT-011 | icache flush |
| CPU-JIT-012 | code alignment 128 B |
| CPU-JIT-013 | module size |
| CPU-JIT-014 | parallel compile |
| CPU-JIT-015 | TBL2 fallback counter |
| CPU-JIT-016 | frame retry counter |
| CPU-JIT-017 | LLVM fatal-error recovery |
| CPU-JIT-018 | signal-handler recovery |
| CPU-JIT-019 | cache build resume |
| CPU-JIT-020 | no stale code after patch |
| CPU-JIT-021 | PPU object key changes with CPU/feature settings; `v9`→`v10` required when IR changes |

#### FUZZ — differential and randomized (20)

| ID | Test |
|---|---|
| CPU-FUZZ-001 | random SPU instruction stream vs interpreter |
| CPU-FUZZ-002 | random PPU instruction stream vs interpreter |
| CPU-FUZZ-003 | random vector shuffle |
| CPU-FUZZ-004 | random arithmetic saturation |
| CPU-FUZZ-005 | random memory addresses |
| CPU-FUZZ-006 | random atomics |
| CPU-FUZZ-007 | random branches |
| CPU-FUZZ-008 | random calls/tail calls |
| CPU-FUZZ-009 | random MFC commands |
| CPU-FUZZ-010 | random DMA sizes |
| CPU-FUZZ-011 | random page protections |
| CPU-FUZZ-012 | random signal timing |
| CPU-FUZZ-013 | random thread interleavings |
| CPU-FUZZ-014 | random cache states |
| CPU-FUZZ-015 | random feature toggles |
| CPU-FUZZ-016 | ASan run |
| CPU-FUZZ-017 | UBSan run |
| CPU-FUZZ-018 | TSan run |
| CPU-FUZZ-019 | 24 h soak |
| CPU-FUZZ-020 | no new crashes vs baseline |

#### GAME — corpus regression (18)

| ID | Test |
|---|---|
| CPU-GAME-001 | Demon's Souls boots to title |
| CPU-GAME-002 | Demon's Souls reaches gameplay |
| CPU-GAME-003 | Demon's Souls 30 min soak |
| CPU-GAME-004 | Ridge Racer 7 boots |
| CPU-GAME-005 | Ridge Racer 7 race |
| CPU-GAME-006 | Ridge Racer 7 30 min soak |
| CPU-GAME-007 | Mega Man 9 boots |
| CPU-GAME-008 | Mega Man 9 gameplay |
| CPU-GAME-009 | GT5 page-fault scenario (if dump available) |
| CPU-GAME-010 | Far Cry 3 module-reset scenario |
| CPU-GAME-011 | Prototype 2 frame scenario |
| CPU-GAME-012 | Max Payne 3 frame scenario |
| CPU-GAME-013 | GTA IV RSX queue scenario |
| CPU-GAME-014 | VSH boot |
| CPU-GAME-015 | Unity title boot |
| CPU-GAME-016 | 6 SPU title |
| CPU-GAME-017 | audio-heavy title |
| CPU-GAME-018 | save/load cycle |

#### PERF — performance gates (16)

| ID | Test |
|---|---|
| CPU-PERF-001 | RR7 native vs Rosetta median FPS |
| CPU-PERF-002 | RR7 p99 frame time |
| CPU-PERF-003 | Demon's Souls native vs Rosetta |
| CPU-PERF-004 | Demon's Souls p99 |
| CPU-PERF-005 | Mega Man 9 native vs Rosetta |
| CPU-PERF-006 | cold PPU cache build time |
| CPU-PERF-007 | cold SPU cache build time |
| CPU-PERF-008 | warm cache startup |
| CPU-PERF-009 | CPU package power |
| CPU-PERF-010 | E-core utilization |
| CPU-PERF-011 | P-core utilization |
| CPU-PERF-012 | memory bandwidth |
| CPU-PERF-013 | RSX offload queue latency |
| CPU-PERF-014 | WFE spin power |
| CPU-PERF-015 | regression vs previous build (5%) |
| CPU-PERF-016 | no regression on Linux ARM64 |

**Total: 24 + 35 + 40 + 38 + 51 + 31 + 20 + 21 + 20 + 18 + 16 = 314 tests.**

### 17.3 Benchmark protocol

1. **Same machine, same OS, same power state.** Plugged in; `pmset` AC; no Low Power Mode; close other apps.
2. **Same config.** Copy `config.yml` per run; same renderer, resolution, PPU/SPU decoders, thread counts.
3. **Same caches.** Either cold (delete `cache/`) for compile-time tests, or fully warm for steady-state tests. Never mix.
4. **Same input replay.** Use a recorded pad script or a deterministic idle scene.
5. **3 runs × 120 s.** Discard the first 10 s (shader/cache warm-up). Report median and p99 frame time, 1% low FPS, average FPS, CPU power.
6. **Native vs Rosetta**: build both with the same RPCS3 commit and same LLVM where possible. Run the x86 build with `arch -x86_64` or the official x86 app. Record Rosetta version.
7. **Microbenchmarks**: 5 runs, median, assembly inspected, on both P and E cores (QoS or `taskpolicy`).
8. **Store raw logs** in `docs/research/apple-silicon/measurements/YYYY-MM-DD/`.

### 17.4 What “pass” means

- Correctness: the test's expected result, or bit-exact vs interpreter.
- Performance: no regression > 3% on any corpus title; target improvement stated in the PR.
- Stability: 30 min soak with no crash/hang; ASan/UBSan clean for L0/L2.

---

## 18. Phasing and PR breakdown

### Phase 0 — baseline and guardrails (1–2 weeks)

- Add `docs/research/apple-silicon/measurements/` and the benchmark protocol.
- Add L0 test harness and the first 30 tests (FEAT, FRAME decode, ATOM basic).
- Capture native vs Rosetta baselines for RR7, Demon's Souls, Mega Man 9.
- Add a debug log line that prints the resolved CPU/features/triple once.
- Add the static instruction-mix scanner (`--decrypt` + `llvm-objdump`) for the corpus, so every optimization proposal can be checked against real guest code.
- Add the QoS/policy readback harness and the 8-thread throughput A/B as `rpcs3_test` cases.
- **No behavior change.**

### Phase 1 — correctness P0 (2–4 weeks)

- PR 0: **make fallback loud.** Count and log every interpreter fallback, TBL2 retry,
  frame retry, and failed module compile, with a one-line summary per game launch. The
  original justification (the #17231 5–10 vs 30 FPS gap) is stale — that issue was fixed
  in Jan 2026 — but the instrumentation is still the cheapest way to prove which decoder
  a title is actually running. The live ARM-only breakage list (#16484) contains titles
  that could be silently interpreting, and Phase 0 cannot capture a native-vs-Rosetta
  baseline without an x86_64 build (§20 Q17), so the log line is the only local signal.
  Do not quote a "3–6× gap" as fact.
- PR 1: `.stack_sizes` plumbing + frame thunk (A).
- PR 2: gateway scratch budget + PPU/SPU checks (A).
- PR 3: decommit/reset race fix (B).
- PR 4: JIT W^X batching + icache flush (B).
- PR 5: atomics fence/observe/store audit (B).
- Tests: FRAME, VM, ATOM.

### Phase 2 — 16 KB page faults (2–4 weeks)

- PR 6: instrument `_page_map` and 64 KB block path.
- PR 7: software sub-page map for notification regions (B).
- PR 8: signal-handler fallback and tests.
- Tests: VM-015..024.

### Phase 3 — codegen wins (3–6 weeks)

- PR 9: masked-store fast path for `STVLX`/`STVRX` (golden assembly + merge-store). (C/D)
- PR 10: `pshufb`/masked-store improvements (C/D).
- PR 11: PPU unaligned load/store fast paths (D).
- PR 12: constant-fold more SPU idioms (C).
- Tests: NEON, PPU.

### Phase 4 — threads/QoS (1–2 weeks, gated on measurement)

- PR 13: stop destroying QoS (`set_native_priority` no-op on Apple; no `schedparam` after QoS). Keep `SCHED_RR` as the default behind a flag. (F)
- PR 14: bounded `isb` spin + `os_sync_wait_on_address` parking helper; **no WFE** (measured to burn a core). (F)
- PR 15: deliberate affinity tags, or delete the misleading code path. (F)
- PR 16: only if the A/B shows a win, switch the default class mapping.
- Tests: THR.

### Phase 5 — scheduling and JIT (ongoing)

- PR 16: target-option audit, post-RA experiments (E).
- PR 17: code alignment/layout (G).
- Upstream: Apple Firestorm schedule model (separate repo/PR).

### Phase 6 — experimental

- SME spike, 2 MB page study, block linking. Only if Phase 1–5 leaves headroom.

---

## 19. Risks and mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Frame thunk changes break tail-call invariants | medium | high | extensive FRAME tests; keep slab fallback |
| Two-pass compile doubles cache build time | medium | medium | only oversized modules; counter + benchmark |
| Page-fault software checks slow GT5 | medium | medium | only notification regions; fast bitmap; measure |
| Decommit change increases memory footprint | medium | medium | `MADV_FREE_REUSABLE`; measure `vmmap` |
| QoS change regresses FPS | medium | high | A/B per title; config knob; keep SCHED_RR behind flag for one release |
| WFE spin assumption wrong (measured: WFE burns a core on macOS 27.2) | high | high | bounded `isb` spin + `os_sync_wait_on_address`; no WFE spinlock |
| LLVM version skew | high | medium | feature probes, cache keys, CI matrix |
| `.stack_sizes` disappears if the JIT triple changes to Mach-O | low | high | explicit runtime check + log + fallback to slab; test CPU-FRAME-035 |
| QoS switch regresses throughput (measured −36%) | medium | high | keep `SCHED_RR` default until real-title A/B; config flag |
| Upstream rejects Apple-specific code | medium | medium | small PRs, generic where possible, measurements |
| Rosetta removed before parity | medium | high | prioritize P0/P1; keep x86 build for older macOS |

---

## 20. Research closure

Status legend: **answered** (we have a measured/verified answer), **deferred** (requires
implementation or hardware we do not have), **rejected** (the premise was wrong).

| # | Question | Status | Answer / next step |
|---|---|---|---|
| 1 | Is the QoS class actually set today? | answered | No: `UNSPECIFIED(0)`, `SCHED_RR` 99. §14.1 |
| 2 | Would QoS be faster than `SCHED_RR`? | answered | No in synthetic throughput (−36%); the open part is real-title latency. §14.2 |
| 3 | Does `pthread_setschedparam` preserve QoS? | answered | No; it unsets QoS and drops to `SCHED_OTHER`/47. §14.1 |
| 4 | Is `.stack_sizes` available to the JIT? | answered | Yes, but only because the JIT triple is ELF. Decoder verified end-to-end. §9.3.1 |
| 5 | Is the masked store actually slow? | answered | 16 conditional byte stores / 31 branches; merge-store is 6 instructions, 0 branches. §11.3 |
| 6 | Do real titles use `LVLX/STVLX`? | answered | Zero occurrences in Demon's Souls and Ridge Racer 7. §5.3 |
| 7 | Is `m_use_avx` doing anything on ARM? | rejected | No; every use is under `ARCH_X64` or a non-ARM `#else`. §11.3 |
| 8 | Does WFE save power on macOS 26+? | rejected | No on macOS 27.2/A18 Pro: 100% core, 0.74 M ops/s. `os_sync_wait_on_address` parks. §14.2 |
| 9 | Does macOS expose SME/SVE to user space? | answered | Advertised but inert: `SVCR`=0, `CNTB`=0, ZA reads zero. §14.4 |
| 10 | Is there an Apple M-series LLVM scheduling model? | answered | No; every Apple CPU binds the 2014 Cyclone model. §7.3 |
| 11 | Do SPU caches miss a codegen-environment key? | rejected | SPU `.dat` is raw guest code; the `llvm/` objects are debug-only and never loaded. PPU cache already includes CPU+settings. §15.2 |
| 12 | What does a decommit cost on 16 KB pages? | answered | `mprotect` 130 µs, `madvise` 207 µs, `munmap+mmap` 285 µs per 64 MB; all reclaim the footprint. §10.3 |
| 13 | Is the CR representation expensive? | answered (structure) | Yes, structurally: 4 i8 locals + pre-branch flush. Sized by D9; the exact win needs a codegen-count harness. §4.6 |
| 14 | Frame-size distribution on real titles | deferred | Needs the `.stack_sizes` decoder in-tree; one histogram during boot. |
| 15 | Two-pass compile cost | deferred | Needs the frame thunk; measure cold-cache DS/RR7. |
| 16 | Exact 64 KB block behaviour in #19881 | deferred | Needs GT5; instrument `_page_map` and reproduce `flags=0x1020a`. |
| 17 | Real native-vs-Rosetta A/B | deferred | No x86_64 RPCS3 build locally; build one before macOS 28. |
| 18 | #19801 register scavenging | deferred | Requires a reduced IR repro from a live SPU compile. |
| 19 | Can a Firestorm-class schedule model be upstreamed? | deferred | Upstream dependency; not a merge requirement. §13.3 |
| 20 | Rosetta 28 exception coverage | deferred | Apple policy question; do not rely on it. §3 |

### 20.1 What is deliberately not in this design

- A forked LLVM or a private scheduling model as a merge requirement.
- SVE/SME codegen.
- A WFE-based spinlock.
- Replacing `SCHED_RR` by default before a real-title A/B.
- Linux/ARM64 or non-Apple platform changes.

### 20.2 Honest confidence statement

- **Stability / correctness:** high confidence (≥85%) that the P0 items fix real,
  reproducible failures; every one is tied to a live issue or a code-level defect.
- **Performance:** medium confidence (~60%) of >10% FPS on the corpus after P0/P1; low
  confidence (<30%) of a large LLVM-scheduler-driven win in the near term.
- **The dominant unknown is silent interpreter fallback — but there is no current gap
  number.** The #17231 3–6× figure is stale (fixed Jan 2026, see §5.1). We do not have a
  current native-vs-Rosetta datapoint because no x86_64 RPCS3 build exists on the
  reference machine (§20 Q17). Treat "native is slower" as an unproven hypothesis until
  the Phase 0 fallback counter and a real A/B say otherwise.



---

## 21. References

Primary sources fetched/verified 2026-11-07..12:

- `llvm/lib/Target/AArch64/AArch64SchedCyclone.td` (LLVM main, 2026-11-12): `CycloneModel`, `IssueWidth=6`, `MicroOpBufferSize=192`, `LoadLatency=4`, `WriteAtomic Unsupported`.
- `llvm/lib/Target/AArch64/AArch64Processors.td` (LLVM main): `TuneAppleM4/M5/M6`, `AppleM4/M5/M6` feature lists, `apple-a18 → apple-m4`, `apple-a19 → apple-m5`, `apple-a20 → apple-m6`.
- `llvm/lib/Target/AArch64/AArch64Features.td`: `HasV8_4aOps` implies `FeatureLSE2`; `FeatureFuseAppleSMECompute`, `FeatureShiftMaddSlow`, `FeatureStorePairSuppress`, `FeatureFuseFMinFMax`.
- `llvm/lib/Target/AArch64/AArch64CallingConvention.td`: `CC_AArch64_GHC`, x19–x28, q4/q5, s8–s11, d12–d15.
- Apple Developer News, 2026-09-01, “Upcoming changes to Rosetta support for Intel-based macOS apps”; Apple Support 102527, “Using Intel-based apps on a Mac with Apple silicon.”
- Local macOS SDK: `pthread/qos.h`, `pthread/pthread.h` (`pthread_jit_write_protect_np`), `libkern/OSCacheControl.h` (`sys_icache_invalidate`), `os/os_sync_wait_on_address.h`, `mach/thread_policy.h`.
- Local `sysctl` and `llc --version` output, 2026-11-12.
- Dougall Johnson, Apple CPU black-box measurements (`applecpu`: Firestorm/Icestorm, retirement, fusion, elimination, latencies).
- RPCS3 issues/PRs: #11922, #12115, #12338, #15982, #16081, #16484, #17231, #17530, #17888, #17893, #17905, #18133, #18151, #18423, #18425, #18656, #18707, #19369, #19609, #19611, #19759, #19801, #19881, #19939, #19977.
- RPCS3 commits: `965f4f5` (reset transform state per module), `f4e66ee35` (current tree context).
- WhatCookie, “Why is AVX-512 useful for RPCS3?” (2022-06-15): AVX-512 wins came from new 128-bit shapes (`VPLZCNTD`, `VPOPCNTB`, `VPTERNLOG`, `VPDPWSSD`, `VRANGEPS`, `VGF2P8AFFINEQB`), all with NEON equivalents.
- `lemire.me` 2024-04-29, “Careful with Pair-of-Registers instructions on Apple Silicon”; LLVM #235875 (LDP+STR 2× penalty on M4).
- Phoronix 2024-06-15, “Apple M4 Support Added To The LLVM Compiler” (commit `2b335913`).
- `docs/research/metal4/` for GPU-side constraints (not modified here).

New in v3 (2026-11-12 evening):

- `docs/research/apple-silicon/measurements/2026-11-12/`: `qos.cpp`, `place.cpp`,
  `place2.cpp`, `sstest.cpp`, `sstest2.cpp`, `mstore.ll`, `mstore2.ll`, `cr.ll`,
  `wfe3.cpp`, `wfecount.cpp`, `oswait.c`, `mem.c`, `ghccc.ll`, `ghccc2.ll`, and
  `README.md` with all raw numbers.
- Local measurement of QoS destruction: `UNSPECIFIED(0)` after the RPCS3 attribute
  sequence; `SCHED_OTHER` priority 47 after `pthread_setschedparam`.
- `llvm/Target/TargetOptions.h` (`EmitStackSizeSection`),
  `llvm/ExecutionEngine/ExecutionEngine.h` (`setProcessAllSections`, `setTargetOptions`).
- `llc -stack-size-section` on ELF vs Mach-O; MCJIT `allocateDataSection` receipt of
  `.stack_sizes`.
- `rpcs3 --decrypt` + `llvm-objdump -d --triple=powerpc64` instruction mix for
  Demon's Souls and Ridge Racer 7.
- Source audit: `Utilities/Thread.cpp:2854-2856,3915-3946,3982-3986`,
  `rpcs3/util/vm_native.cpp:354-355,384-385`,
  `rpcs3/Emu/Cell/PPUTranslator.cpp:3710,3730,3758`,
  `rpcs3/Emu/CPU/CPUTranslator.h:3244`,
  `rpcs3/Emu/Cell/SPULLVMRecompiler.cpp` `#ifdef ARCH_X64` guard audit.
- v4 additions: `wfe3.cpp`, `wfecount.cpp`, `oswait.c`, `mem.c`, `mstore2.ll`, `cr.ll`
  in `measurements/2026-11-12/`, plus README §6–§10.
- LLVM main file list for `AArch64Sched*` and `AArch64Processors.td` (`apple-m6`
  exists; no `AArch64SchedAppleM*`).

---

## 22. Adversarial review (what would make this design fail)

This section is deliberately hostile. It lists the places where the design is weakest, so
that a reviewer does not have to find them. Each item names the evidence we lack and the
experiment that would settle it.

### 22.1 Correctness risks that are not yet closed

| # | Weakness | Why it matters | What closes it |
|---|---|---|---|
| A1 | **#19881's 64 KB root cause is unknown.** The design assumes "4 KB holes cannot be expressed on 16 KB pages" and proposes a software sub-page map, but the issue body also mentions a 64 KB block path and `flags=0x1020a` that were never diagnosed. | If GT5's hang is actually a 64 KB `mmapper` bug, the sub-page map is wasted work and the patch will not fix the title. | GT5 dump + instrument `_page_map`; reproduce `flags=0x1020a`; do not ship the map before that. |
| A2 | **The software sub-page check has no cost model.** A check on every guest load/store in a notification region can add branches to the hottest code in the emulator. | A 10% tax on all memory ops to fix one title is a net loss; the design does not quantify it. | Per-region gating in the IR (only regions registered for notification), plus a synthetic `ldr/str` microbenchmark. Measure before/after on DS/RR7. |
| A3 | **Blocking/restart semantics in the JIT memory helper are unspecified.** If a fault helper blocks on the guest page and restarts the instruction, the translator must be able to reconstruct guest state (GPRs, CR, reservation, PC) at that point. | The current translator has no restartable-instruction mechanism; a naive helper either deadlocks or silently corrupts state. Worst case it forces interpreter fallback for the whole region. | Prototype one restartable `lwz` in a synthetic module; assert state equality against the interpreter under 10⁶ randomized faults; measure the IR cost. |
| A4 | **The framed-function thunk breaks the GHC tail-call chain.** GHC functions are compiled on the assumption that they never return and all share one slab; giving one function its own `sp` changes the contract for every caller and for far-return unwinding (`x30`, `x19–x28`, `q4–q15`). | A thunk that is correct in isolation can still corrupt a caller that assumed the shared slab, and the two-pass retry only recompiles the callee, not its callers. | A model test with a 3-deep call chain where only the middle function is oversized; ASan + canary; verify far-return path. |
| A5 | **`.stack_sizes` is an LLVM-internal contract.** It is emitted only on ELF, can disappear with a triple or flag change, and (as measured) contains padding/zero records and needs relocation. | Silent loss of the section turns the frame check off without a test failure; the gateway overflow returns. | CPU-FRAME-035 plus a startup log line; a CI test that asserts the section is present for the configured triple. |
| A6 | **The cache-key bump invalidates every user's PPU cache.** Bumping `v9` is correct but causes a one-time full recompile for all titles. | Users will report "first boot is much slower" and may pin an old build; the upstream PR must say so. | Release note + a cache-migration test; measure cold-cache DS/RR7 wall-clock before deciding to bundle the bump with the IR change. |
| A7 | **W^X batching interacts with signal handlers and backtraces.** `pthread_jit_write_protect_np` toggling around batched writes is not verified against the SIGSEGV handler that inspects JIT code, and batched flushes can leave stale icache after a signal-driven patch. | A rare stale-icache bug is a nightmare to debug and would be blamed on the emulator, not the JIT. | Stress test: 10⁶ patches with concurrent signal delivery; `sys_icache_invalidate` audit; assert every publish path flushes. |
| A8 | **The sub-page map only covers PPU `ReadMemory`/`WriteMemory`.** Weaker paths — instruction fetch, SPU MFC DMA, RSX/DMA transfers, and `mmapper`'s own copies — can touch the notification region without going through those helpers. | GT5 may rely on DMA or execute from the region; the software map would then be incomplete and the fault would still never fire. | Enumerate every access path; either route them through the check or prove they cannot target a notification region (§10.2). |
| A9 | **Page-fault events have re-arm/level semantics that the design does not specify.** The x64 path queues an event and blocks the thread; on 16 KB hosts a single host page can cover four guest pages, and the handler must not lose or duplicate events across `mprotect` transitions. | Lost events hang; duplicate events break games that count them. | Model test with 4 guest pages in one host page, randomized access/fault/re-arm; compare event trace against the x64 build. |

### 22.2 Claims that are weaker than the document implies

| # | Claim | Actual status | Fix |
|---|---|---|---|
| B1 | "`SCHED_RR` is ~55% faster" | True **only** in a synthetic dependent-multiply benchmark with 8 always-runnable threads and no memory or SIMD. Absolute numbers vary ~20% between runs. | Quote it as a within-run ratio; label it a worst-case scheduling microbenchmark; never present it as an FPS prediction. |
| B2 | "CR field packing is 1–4% in PPU-bound titles" | There is **no codegen-count harness** and no measurement; it is a structural argument from 341 k `bt`/`bf` in Demon's Souls. | Build the instruction-count harness first; if the win is < 0.5%, drop D9 from the roadmap. |
| B3 | "Masked stores are a P2 win" | The local corpus has **zero** `LVLX/STVLX`; the 31-branch → 6-instruction result is real but applies to titles we cannot test. | Keep as a code-quality fix with a golden assembly test; do not put it in a perf table. |
| B4 | "QoS will improve latency/audio" | Unmeasured. QoS is within noise of `SCHED_OTHER` in throughput, and no p99 frame-time or audio-glitch data exists. | Add a p99 frame-time and audio-underrun metric to the Phase 0 harness before any QoS PR. |
| B5 | "A Firestorm scheduling model would help" | No public Firestorm model exists, `apple-m4` binds the 2014 Cyclone model, and LLVM upstreaming is out of our control. | Keep it explicitly non-blocking (§20 Q19); do not size any expected win. |
| B6 | "SME/SVE are inert" | Verified on one OS build (27.2) and one chip (A18 Pro). Apple could enable it in 27.3/28. | Re-probe at startup in debug builds; keep the research spike, but do not design around it. |

### 22.3 Evidence and test-matrix weaknesses

- **The 314-test matrix is a plan, not coverage.** Many rows require hardware we do not
  have (M4 Max/Ultra, macOS 26, GT5, Prototype 2, Max Payne 3). The number should be read
  as "314 hypotheses to encode", not "314 passing tests". Add an explicit L0/L1/L2 tier
  and a CI column; a test that cannot run is documentation.
- **One machine, one chip, one OS.** All local measurements are A18 Pro / macOS 27.2 /
  16 KB pages / 8 GB. The design targets "newer Apple Silicon Macs", but nothing was
  measured on an M4 Pro/Max/Ultra, on a 24 GB+ machine, or under thermal load. The
  scheduler, cache, and bandwidth conclusions may not transfer.
- **The corpus is three titles, two of which are decrypted for instruction mix only.**
  The "zero `LVLX`" claim is true for Demon's Souls and Ridge Racer 7; it says nothing
  about the titles that actually use unaligned vector tails.
- **No native-vs-Rosetta A/B exists.** The benchmark protocol correctly says to capture
  one, but there is no x86_64 build on the reference machine, so the central performance
  question is unanswered. The design's perf claims are therefore all relative to an ARM
  baseline, not to the product users can run today.
- **#17231 is stale and was, until this pass, used as a load-bearing datapoint.** It is
  fixed (§5.1). This is exactly the kind of error the review is meant to catch; assume
  there are others in the issue table until each is re-checked.
- **The decommit numbers are microbenchmarks.** 64 MB, one process, no concurrent access,
  no `vmmap` soak. The claim "footprint returns to baseline" was measured immediately
  after the call, not after 30 minutes of guest churn. §20 Q11 still stands.

### 22.4 Upstream and product risks

- **Apple-specific code is a hard sell.** The design keeps the shared `CPUTranslator` and
  gates Apple behavior, but the frame thunk, sub-page map, and policy changes all touch
  common code. A reviewer can reasonably ask for an x86 parity argument for each one. The
  mitigation is small PRs with measurements, not a single mega-PR.
- **The Rosetta deadline is real but not urgent in the way the document sometimes
  implies.** macOS 27 is the last full-support release and 27.2 no longer auto-removes
  Rosetta; users on 26/27 are fine. The deadline argues for merging before macOS 28, not
  for shipping an unmeasured scheduler change.
- **Power and thermals are not modeled.** Running every emulation thread at `SCHED_RR`
  99 may win throughput while raising package power and fan noise; on a fanless MacBook
  Air that is a user-visible regression. No energy measurement exists.
- **Memory footprint on 8 GB.** Decommitting without unmapping is the right call for the
  race, but the design has not measured the long-run cost of never returning address
  space. On an 8 GB machine with a large guest, this can matter.

### 22.5 What would falsify the design

1. Instrumented fallback counts show no title silently interpreting → the Phase 1
   "make fallback loud" work is still useful but no longer P0.
2. Real-title A/B shows `SCHED_RR` is not faster than QoS on frame time → drop Phase 4's
   policy change, keep only the QoS-destruction fix.
3. `#19881` turns out to be a 64 KB `mmapper` bug → replace the sub-page map with a
   targeted fix.
4. The codegen-count harness shows CR packing < 0.5% → drop D9.
5. The frame-size histogram shows no function above 8 KB outside two titles → keep the
   slab at 8 KB and ship only the thunk for the exceptions, not a general frame redesign.

If any of these fire, the correct response is to shrink the design, not to explain the
result away.

---

## 23. Changelog

| Date | Change |
|---|---|
| 2026-11-07 | v1 draft: single-target Apple Silicon CPU redesign, Firestorm facts, first issue map. |
| 2026-11-12 | v2: corrected LLVM version/feature claims; verified live hardware; added Rosetta endgame; replaced naive-frame narrative with the 8 KB gateway scratch; added LSE2/CASP status, 16 KB page-fault design, QoS contradiction, 304-test matrix, phasing, risks, references. |
| 2026-11-12 (v3) | Measured QoS destruction and the surprising `SCHED_RR` throughput result; verified `.stack_sizes` end-to-end and documented the ELF-only dependency; measured masked-store lowering; withdrew the dead `m_use_avx` item; added real guest instruction mix from decrypted Demon's Souls and Ridge Racer 7; demoted QoS and masked stores; added defect register D1–D8 and ballpark expectations; test matrix 307. |
| 2026-11-12 (v4) | Research-complete. Measured that WFE does **not** park on macOS 27.2/A18 Pro and withdrew the WFE spinlock plan; measured `os_sync_wait_on_address` (0 CPU, 9 µs p50) and made it the parking primitive; measured decommit strategies (`mprotect` 130 µs vs `munmap+mmap` 285 µs, footprint equal) and used that to justify removing the race; verified SME/SVE are inert in user space; audited cache keys (PPU includes CPU+settings, SPU `.obj` is debug-only) and added the `v9` bump rule; quantified the masked-store fix (31 branches → 6 instructions); added CR flush analysis (D9) and the WFE correction (D10); replaced §20 with a research-closure table; test matrix 314. |
| 2026-11-12 (v5) | Adversarial pass. Corrected the stale #17231 premise (resolved Jan 2026; no current native-vs-Rosetta gap is proven); replaced the conflated scheduling table with the clean within-run `place2.cpp` A/B and forbade absolute Miters/s claims; added `.stack_sizes` decoder caveats (padding/zero records, relocation, all-functions mapping, 1 MB cap); added §22 adversarial review with falsification criteria; verified the GHC positive-offset spill mechanism with an `llc` reproducer (`ghccc2.ll`); fixed the withdrawn-WFE phase item and measurement references. Test matrix unchanged at 314. |

*Living document. Every research question that does not require implementation is now
closed; the remaining items are listed as `deferred` in §20 with the exact experiment
that closes them.*
