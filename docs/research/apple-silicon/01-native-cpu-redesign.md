# RPCS3 on Apple Silicon: A Native CPU Runtime Design

**Status:** Phase 1 design complete, implementation started; Phase 2 (§13) documented
**Date:** 2026-11-13
**Reference machine:** Apple A18 Pro (MacBook Neo, `hw.model Mac17,5`), macOS 27.2 (26B123), arm64, 16 KB pages
**Toolchain:** Homebrew LLVM 22.1.8, host CPU `apple-m4`, default target `arm64-apple-darwin27.2.0`
**Scope:** native ARM64 macOS only. No Linux/ARM64 or non-Apple platform changes.

**Evidence markers:** `[V]` verified from this tree, the hardware, or an official source; `[M]` measured on the reference machine (Appendix A); `[R]` researched from another emulator's source or release notes; `[U]` unverified — a hypothesis, estimate, or projection.

---

## 0. Executive summary

RPCS3 builds and runs natively on Apple Silicon today. It is not *designed* for Apple Silicon: it is an x86-first emulator whose ARM64 backend inherits the x86 execution shape. The port works, and on some titles it works well, but the largest performance levers are untouched and several correctness defects only exist on 16 KB-page hosts.

This document specifies a redesign of the CPU and memory runtime. It is not a rewrite of RPCS3 and not a fork. The target is narrow and testable:

1. **Correct first.** Fix the defects that turn working games into crashes, hangs, and silent interpreter fallbacks on Apple Silicon.
2. **Then faster than x86 under Rosetta.** Not by a few percent from LLVM tuning, but by adopting what the strongest Apple Silicon emulators already do: direct-access memory with fault backpatching, block linking, per-page code invalidation, and a much tighter IR for guest state.
3. **Upstreamable.** Every change is gated on `__APPLE__ && ARCH_ARM64`, keeps the shared translator path, and can be reviewed as a small, measured PR.

### 0.1 The thesis

The biggest wins are not in instruction selection. They are in **memory access, dispatch, and state shape**, in that order.

- **Memory.** RPCS3 already accesses guest RAM through a flat host mapping and handles faults in a signal handler. That is the right skeleton. What it lacks is a fast fault path — backpatching a recurring fault instead of re-decoding every occurrence — a way to represent 4 KB guest holes on a 16 KB host page for `sys_mmapper` notification regions, and page-granular invalidation of generated code.
- **Dispatch.** Direct guest branches already become GHC tail calls, but every conditional branch makes both successors new function entries, and indirect branches and returns pay a dependent load. Dolphin, PPSSPP, Dynarmic, and ARMSX2 all treat block linking and indirect-branch caching as mandatory.
- **State.** The translator keeps guest registers in SSA only within a block, then flushes them to `ppu_thread` at every boundary. LLVM already does register allocation; the fix is to give it a region of IR with real CFG edges, not to write a second allocator. This one has a measured caveat: straight-line regions are only 4–6 guest instructions long, so region IR must be **loop-first** or it does not amortise (§8.2.1, Appendix A.12).

LLVM's scheduling model is a distant fourth. Every Apple CPU currently binds LLVM's 2014 Cyclone model, and every strong Apple Silicon emulator got its wins from architecture, not from a better scheduler.

### 0.2 What changes

| Layer | Today | Target |
|---|---|---|
| Guest memory access | Direct host access; signal decode on every fault | Direct access + **fault-site backpatch**; sub-page map for notification regions; per-page code invalidation |
| Dispatch | GHC tail calls; both successors of a conditional branch are function entries | **Loop-first region IR**, direct link areas, **indirect branch target cache**, return stack buffer |
| Guest state | CR fields as separate `i8` locals; flush before every branch | **Packed CR + lazy flags**, SSA across region edges, live-out materialisation only at exits |
| Frames | Fixed 8 KB gateway slab; silent overflow | **64 KB slab + frame histogram** first, then `.stack_sizes` + per-function thunk, PPU and SPU |
| JIT lifetime | Module cache; module-wide invalidation; ad-hoc W^X toggles | **Code-space reuse**, per-page invalidation, batched RAII W^X, cache v10 |
| Runtime | `SCHED_RR` 99 on every emulation thread; QoS destroyed | Measured policy split; bounded `isb` + `os_sync_wait_on_address` parking; no WFE |
| Observability | Ad-hoc logs | Fallback, fault, block, compile, and frame histograms; one summary line per launch |
| Platform | `MAP_JIT` without an entitlement contract | Signing/entitlement matrix; per-thread W^X discipline; icache publication audit |

### 0.3 Scope, effort, confidence

- **Effort:** 10–16 engineer-months for a production-grade, upstreamable result. With two engineers splitting memory/dispatch and tests/CI, 6–9 calendar months. Roughly 12k–20k changed/added lines plus 4k–8k lines of tests.
- **Confidence:** high (>85%) that the P0 correctness work fixes real, reproducible failures; medium (~50–60%) of a >10% FPS win from the memory+dispatch+state work; low (<30%) of a large scheduler-driven win at any point. The fastmem prototype passed, so the remaining performance risk is concentrated in how much execution time sits inside long-enough regions, not in whether faults can be handled.
- **Biggest risk:** dynamic region coverage. Static analysis says straight-line regions are ~6 instructions and loop regions ~13, with 20% of loop regions above 40 (§8.2.1). Whether the hot 90% of execution lands in those few long regions is unmeasured and decides how much of the SSA work pays.
- **First go/no-go already passed:** fault → commit → retry costs ~5.7 µs and a batched site patch costs 1–2 ns, so a fastmem tier is practical with 64-bit guest pointers (Appendix A.12).

The rest of the document is the evidence, the comparative research, the design, the work breakdown, and the falsification criteria.

**Two phases.** Sections 1–12 specify Phase 1: an upstreamable, Apple-gated redesign that keeps `ghccc`, MCJIT, and stock LLVM. Section 13 specifies Phase 2: the performance-first architecture that removes those three constraints, adds background tiering and traces, and is allowed to carry a vendored LLVM patch series. Phase 1 is a prerequisite, not a compromise that Phase 2 throws away: it is the correctness baseline and the differential oracle.

---

## 1. Scope, non-goals, and the upstreamability contract

### 1.1 In scope

- Native ARM64 macOS on Apple Silicon, macOS 26 and newer.
- The PPU and SPU LLVM recompilers: memory access, frames, dispatch, state representation, and cache.
- The VM layer (`vm::`), signal handling, W^X/JIT publication, and thread policy where they affect the CPU runtime.
- Tests, instrumentation, and benchmarks needed to defend the changes.
- Apple-specific code paths, gated at compile time and/or runtime.

### 1.2 Out of scope

- Phase 1 only: a forked LLVM, a private scheduling model, or an LLVM dependency upstream cannot consume. Phase 2 deliberately revisits this trade (§13.4).
- SVE/SME codegen. Both are advertised but inert in user space on the reference OS (Appendix A.11).
- Linux/ARM64, Windows/ARM64, or non-Apple changes.
- Replacing the RSX/Metal/Vulkan renderer. GPU work is a separate document.
- Emulating a PS3 hypervisor. There is no guest/host ISA equivalence to exploit: the guest is 64-bit PowerPC, the host is AArch64.
- A from-scratch JIT emitter. RPCS3's LLVM translator is an asset; the redesign changes the IR it produces and how blocks are linked, not the fact that LLVM compiles them.

### 1.3 Upstreamability contract

Every PR must satisfy all of these:

1. **Gated.** Apple behavior lives behind `#if defined(__APPLE__) && defined(ARCH_ARM64)`, or behind a runtime capability check where the behavior is generic.
2. **Shared where possible.** Frame checks, `.stack_sizes` decoding, block metadata, and cache-key rules are architecture-neutral and live in shared code.
3. **No regression by construction.** x86_64 and Linux/ARM64 builds compile and pass the same tests before and after. CI includes a non-Apple ARM64 build.
4. **Measured.** Every performance PR carries a before/after on at least two titles and a synthetic microbenchmark, with the benchmark source in the tree.
5. **Reversible.** New behavior has a config flag for one release, defaulting to the measured-good value.
6. **Documented.** The PR description names the exact failure it fixes and the exact test that proves it.

---

## 2. Why now: the Rosetta endgame and the macOS JIT contract

### 2.1 Rosetta is leaving

Apple's published position, verified 2026-11-12:

- Rosetta is available on any Apple Silicon Mac running **macOS 27 or earlier**.
- Starting with **macOS 28**, Rosetta functionality is available only for **certain older, unmaintained games**, not as a general-purpose translation layer.
- macOS 27.2 no longer removes an existing Rosetta installation, but the direction is unambiguous.
- macOS 26.4+ notifies users when an Intel app depends on Rosetta.

Consequences:

- A native build is not a nice-to-have; it is the only long-term path for RPCS3 on macOS.
- The x86_64 build remains useful for older macOS and as a performance baseline, but it cannot be the primary path.
- "Wait for Rosetta to improve" is not a strategy. Rosetta is already excellent; any remaining gap is in RPCS3's native runtime.

### 2.2 The JIT contract on Apple Silicon

Apple Silicon enforces **per-thread W^X** for `MAP_JIT` regions:

1. `mmap(..., MAP_JIT, ...)` maps the region executable.
2. `pthread_jit_write_protect_np(false)` flips the *calling thread* to writable, non-executable.
3. The thread writes or patches code.
4. `pthread_jit_write_protect_np(true)` flips it back to executable.
5. `sys_icache_invalidate(addr, len)` publishes the new instructions.

Rules that follow:

- The write window is **per-thread**. A thread that writes while another executes the same region is a correctness and security bug.
- The window must be closed on **every** exit path, including exceptions and early returns.
- Instruction caches are not coherent with data caches. Every publication path must invalidate.
- `MAP_JIT` requires `com.apple.security.cs.allow-jit` when the app adopts the Hardened Runtime. A plain `mprotect(PROT_EXEC)` path requires `com.apple.security.cs.allow-unsigned-executable-memory`. The two are not interchangeable.
- macOS 26 tightened the rules: PPSSPP hit a silent `mprotect(PROT_EXEC)` failure under Hardened Runtime until it added `allow-unsigned-executable-memory`; `MAP_JIT` pages now behave like iOS and require the per-thread toggle at every compile/execute boundary.

RPCS3's macOS CI ad-hoc signs with `codesign --deep -fs -` (`.ci/deploy-mac.sh:69`), and the repository has no explicit entitlements file outside third-party code. That works only if the shipped app does not enable the Hardened Runtime, or if the signing pipeline adds the entitlement out of band. **This must be made explicit and tested.** A Debug/ad-hoc build is not proof that a notarized Developer ID build can map JIT memory.

### 2.3 16 KB pages are the default, not an edge case

All Apple Silicon Macs use 16 KB host pages. The guest (PS3) uses 4 KB pages. This changes what a page fault *is*:

- A 4 KB guest hole inside a 16 KB host page cannot be expressed by the host MMU.
- The VM layer explicitly bails out of native page mapping when the host page size exceeds 4096 (`vm.cpp:747`, `:981`).
- Regions registered for `sys_mmapper_enable_page_fault_notification` can therefore have no host holes, so no fault fires. That is #19881 (GT5): 0 faults native versus 164 under x86/Rosetta, with `flags=0x1020a`.

Any credible Apple Silicon design must solve this without giving up direct memory access for the pages that are fully mapped.

---

## 3. Reference machine, toolchain, and what is actually true

**Evidence: [V]** — local `sysctl`, `sw_vers`, `llc --version`, and the LLVM source files named in §14.

### 3.1 Hardware

| Property | Value | Evidence |
|---|---|---|
| SoC | Apple A18 Pro, 2P + 4E | `sysctl`; `hw.model Mac17,5` |
| Memory | 8 GB | `hw.memsize` |
| OS | macOS 27.2 (26B123), arm64 | `sw_vers` |
| Page size | 16384 | `pagesize` |
| Cache line | 128 B | `hw.cachelinesize` |
| Virtualization | not a VM (`kern.hv_vmm_present = 0`) | `sysctl` |
| LSE / LSE2 | 1 / 1 | `hw.optional.arm.FEAT_LSE*` |
| DotProd / I8MM / BF16 | 1 / 1 / 1 | `hw.optional.arm.FEAT_*` |
| SHA3 / SHA512 | 1 / 1 | `hw.optional.arm.FEAT_*` |
| SME / SME2 | 1 / 1, but inert in user space | measured, Appendix A.11 |
| SVE / SVE2 | absent | `hw.optional.arm.FEAT_SVE*` |

### 3.2 Toolchain

- Homebrew LLVM **22.1.8**.
- `llc --version` host CPU: **`apple-m4`**.
- Default target: **`arm64-apple-darwin27.2.0`**.
- `apple-a18` is accepted and aliases to `apple-m4`.
- `apple-m6` is **rejected** by LLVM 22; the recognized list stops at `apple-m5`.
- LLVM's Apple CPU models bind to `AArch64SchedCyclone.td` (2014 Cyclone: issue width 6, 192-entry ROB, 4-cycle load latency). There is no Firestorm/Icestorm scheduling model in LLVM main. `TuneAppleM4/M5/M6` exists, but tuning is not a scheduling model.

### 3.3 What this changes about the design

- **Use `apple-m4` for ISA features.** Do not trust `-mcpu` for scheduling. Do not build a performance claim on LLVM's scheduler.
- **Detect features at runtime.** A build may run on M1 through M5. LSE/LSE2, DotProd, I8MM, and SME all need probes or compile-time guards.
- **Treat 16 KB pages and per-thread W^X as invariants.** They are not optional configurations.
- **Do not design around SME/SVE.** They are advertised, but `SVCR` stays 0 after `smstart`, `CNTB/CNTW/CNTD/RDSVL` return 0, and ZA reads back zero. Plain SVE/SME without OS streaming mode traps.

### 3.4 Microarchitecture facts, and the ones we do not have

**Evidence: [R]** for the Firestorm/Icestorm numbers, from Dougall Johnson's black-box measurements; **[V]** for the platform constants, checked against the local SDK headers and `sysctl`.

Firestorm (the M1/A14-class P-core) is the closest publicly characterised proxy for the A18 Pro core:

```
8 uops/cycle, 1 taken branch/cycle
int pipes: 1 (alu+ubfm/flags/branch), 2 (alu+ubfm/flags/indirect/ptrauth),
           3 (alu+ubfm/flags), 4 (alu+ubfm), 5 (alu+mul+div), 6 (alu+mul+madd+crc+bfm/extr)
ld/st:     7 store, 8 load/store, 9 load, 10 load (128-bit, LSL#3)
SIMD:      11–13 fp/simd (+fcsel), 14 fp/simd+fdiv+sqrt+sha
ROB:       ~330 retire groups (~1100 uops), ~623 renames, ~380 int regs, ~432 FP regs

Icestorm (E): 4-wide, 3 int units, 1 load + 1 store, 2 FP/SIMD, 60 retire groups
```

Fusion into a single uop:

- `adds`/`subs`/`ands`/`cmp`/`tst` followed by `b.cc`;
- `add`/`sub`/`and`/`orr`/`eor`/`bic` followed by `cbz`/`cbnz` when the destination matches;
- `aese`+`aesmc`, `pmull`+`eor`.

Zero-uop elimination: `mov x0, #0`, 64-bit `mov x0, x1`, `nop`, `b`.

Latencies: `madd` with an addend 1 cycle, otherwise 3; load-to-load-base 3, load-to-ALU 4; integer-to-SIMD at least 7. Not fused: `adrp`+`add`, `mov`+`movk`, `mul`+`umulh`, `udiv`+`msub`.

Why this matters for a translator: **`cmp` + `b.cc` fusion is free, and `cmp` + `cset` + `strb` is not.** That is exactly the shape D9 forces on every CR field, and it is the microarchitectural justification for the CR design in §8.3.3. It also means a branch-dense block is limited by the one-taken-branch-per-cycle port long before it is limited by ALU width.

What is *not* known, and must not be encoded as fact:

- There is no public Firestorm-level counter breakdown for M4/A18 Pro. Issue width, ROB size, load latency, and the exact fusion set on A18 Pro are `[U]`.
- The 128-byte cache line and 16 KB page size are confirmed locally; the rest of the A18 Pro core is inferred from Firestorm.
- Use Firestorm as a conservative proxy. Do not hand-tune an instruction schedule to a guess, and do not claim a latency win from a model that does not exist.

Platform constants and the API that owns each one:

| Constant | API | Reference value |
|---|---|---|
| Page size | `getpagesize()`, `sysconf(_SC_PAGESIZE)`, `vm_page_size` | 16384 |
| Cache line | `sysctlbyname("hw.cachelinesize")` | 128 |
| P/E levels | `hw.nperflevels`, `hw.perflevel{0,1}.name/.physicalcpu/.logicalcpu` | 2 levels, 2P + 4E |
| Feature bits | `hw.optional.arm.FEAT_*` | §3.1 |
| JIT W^X | `pthread_jit_write_protect_np`, `pthread_jit_write_protect_supported_np` | macOS 11+ |
| Futex | `os_sync_wait_on_address`, `os_sync_wake_by_address_*` | macOS 14.4+ |
| QoS | `pthread_attr_set_qos_class_np`, `pthread_set_qos_class_self_np`, `pthread_get_qos_class_np` | macOS 10.10+ |
| Affinity hint | `thread_policy_set(THREAD_AFFINITY_POLICY)` | advisory only |
| Icache flush | `sys_icache_invalidate` (`libkern/OSCacheControl.h`) | required after code writes |

`apple-m4` feature implications, which the translator's target attributes must match:

- `AppleM4` uses `HasV8_7aOps`, which implies `HasV8_4aOps`, which includes `FeatureLSE2`. So `+lse2` is present even though the processor definition spells only `FeatureLSE`.
- `+dotprod`, `+i8mm`, `+bf16`, `+sha3`, `+fp16`, `+fp16fml`, `+crc`, `+aes`, `+sha2`, `+complxnum`, `+js`, `+pauth`, `+fpac`, `+ras`, `+rcpc`, `+rdm` are present.
- No `+sve`, no `+sve2`. SME is present in the feature list but inert (§3.3, Appendix A.11).

The design consequence is stated once and applies throughout: **subtract instructions, spills, branches, and memory traffic first; schedule second, and only when a model exists.** On a 6-wide out-of-order core, removing a store beats assigning it a better latency.

---

## 4. What the best Apple Silicon emulators do

**Evidence: [R]** — emulator source trees, pull requests, and release notes, cited in §14.2. No claim here was measured locally.

This is comparative research, not a claim that RPCS3 can copy any of it wholesale. The point is to identify which techniques produce wins across different guest architectures, and which are specific to hand-written JITs or hypervisors.

### 4.1 Dolphin — the reference AArch64 JIT port

Dolphin's GameCube/Wii JIT has a separate `JitArm64` backend and was one of the first emulators to run natively on M1.

- **W^X from the start.** The M1 port (PR #9441) added `MAP_JIT` allocation, entitlements, and the machine-context plumbing, and reported a **30–50% speedup over Rosetta**, plus fewer hitches. The port was not "compile for ARM"; it was a JIT-memory redesign.
- **Fastmem with backpatching.** `EmitBackpatchRoutine` has three modes: always slow, always fast, and `Auto`. In `Auto`, the first access runs fast; if it faults, the handler patches the site to call the slow C++ path. This avoids re-decoding the same fault forever.
- **Block linking as a first-class mechanism.** `JitArm64BlockCache::WriteLinkBlock` emits a fixed **3-instruction link area** (downcount check, direct branch, checked entry). Exits are backpatched to the target. Near/far code spaces handle AArch64's branch range.
- **Register cache.** `Arm64GPRCache` / `Arm64FPRCache` track guest registers in host registers, with dirty bits, flush modes (`Full`, `Undirty`, `MaintainState`), and immediate tracking. "JitArm64: Implement CR cache" extends the same idea to condition registers.
- **Codegen space reuse.** Instead of flushing the whole JIT cache on invalidation, Dolphin marks freed code space reusable. Metroid Prime and N64 Virtual Console titles that previously flushed every few minutes stopped flushing.
- **MMU support in the JIT.** The AArch64 JIT initially lacked it and interpreted those games. Adding it moved Rogue Squadron 2/3 and Spider-Man 2 from interpreter to JIT on ARM.
- **Fault handling on macOS.** Dolphin's `HandleFault` has an explicit macOS/ARM64 branch because the exception handler runs on a separate thread there, so the usual "must be the CPU thread" assumption does not hold.

**Lesson for RPCS3:** direct access plus backpatching is a proven shape; block linking is not optional; code-space reuse removes a class of stutter; a hand-written register cache is powerful but not directly transferable to an LLVM-based JIT.

### 4.2 PPSSPP — compile time and W^X discipline

- **Fastmem and block linking** are standard: the block cache links direct branches, and memory access uses a fast path with a safe fallback.
- **W^X-compatible JIT** (PR #8937) made the JIT work on iOS; block linking was initially disabled in that mode and later restored.
- **Compile time is a performance metric.** PR #23049 ("IR JITs: Compile blocks faster") took block compile time on a Galaxy S8 from ~20 µs to ~15.5 µs, and from 3.63 µs to ~3.3 µs on x86. The changes were mostly *not* codegen: read cache-line sizes once instead of per icache flush, time blocks by phase only when profiling, skip IR passes a block has nothing for, avoid clearing 1 KB of constants per block, keep exits/links in flat vectors instead of `unordered_multimap`, avoid copying the register cache on the bad-address path. The PR logs an IR/code hash to prove codegen did not change.
- **macOS 26 entitlement trap.** A 2026 PPSSPP/OpenEmu-Silicon fix documents that macOS 26 blocks `mprotect(PROT_EXEC)` on anonymous pages under Hardened Runtime unless `com.apple.security.cs.allow-unsigned-executable-memory` is present. `allow-jit` only covers `MAP_JIT`; the two entitlements differ. On macOS 26, `MAP_JIT` pages enforce hardware W^X like iOS, so `pthread_jit_write_protect_np` must be used at every compile/execute boundary.

**Lesson for RPCS3:** JIT compile time and cache-line reads are gameplay-visible (stutter), and the entitlement/signing matrix is a real failure mode, not a packaging detail.

### 4.3 DuckStation — invalidation and interpreter fallback

DuckStation's PS1 recompiler supports x86-64, ARM32/64, RISC-V, and LoongArch. The ARM64 backend uses Vixl and keeps guest state in `x19`, memory base in `x20`.

- **Fastmem** maps guest RAM into the host address space and generates direct accesses; faults go to `HandleFastmemException`, which emulates the access and resumes.
- **Block invalidation heuristics** are explicit: if a block is recompiled more than 3 times within 15 frames, fall back to the interpreter; if a page is invalidated more than 4 times within 60 frames, switch that page to `ManualCheck`, where the recompiled block verifies the original instructions before executing.
- **MAP_JIT** is used for code space on Apple Silicon (PR #1840), with `pthread_jit_write_protect_np` around writes.

**Lesson for RPCS3:** self-modifying code and repeated invalidation need a policy, not just a flush. A per-page/manual-check fallback prevents recompile storms.

### 4.4 Dynarmic / yuzu — the clean fastmem model

Dynarmic is the ARM JIT used by yuzu. Its A64 backend is the clearest public description of the fastmem contract.

- **Arena + page table.** Guest memory is mapped into a host arena (39 bits for the Switch), with a page table for fallback. With fastmem, an access is "3 instructions to check the pointer is in range, plus one load"; out-of-range accesses call callbacks.
- **Fault patching.** Each fastmem instruction records its offset from the block entry (`FastmemPatchInfo`). On a fault, the handler finds the block and patch site, marks the instruction "do not fastmem", and invalidates/recompiles the block. `recompile_on_fault` is a configuration choice.
- **Exclusive access.** Fastmem-exclusive uses direct atomics where the architecture allows; otherwise a global monitor coordinates exclusive operations.
- **Optimization flags.** `GetSetElimination`, `ConstProp`, `MiscIROpt`, `BlockLinking`, and `ReturnStackBuffer` are explicit, separately measurable optimizations. Block linking and RSB are baseline for performance.
- **Page table fallback.** When fastmem is unusable, the page table is the safe middle tier: faster than callbacks, portable, no fault handling.

**Lesson for RPCS3:** the value is not "map memory" (RPCS3 already does); it is the *patch metadata* that turns a fault into a one-time fix, and the explicit separation of fastmem, page-table, and callback tiers.

### 4.5 Ryujinx / ARMeilleure — two JIT generations and "lightning"

- **ARM64 JIT** (`ARMeilleure`) was added for cases the hypervisor cannot run, especially 32-bit titles.
- **Hypervisor** runs 64-bit Switch code natively with near-zero overhead, because guest and host are both ARMv8. It is *not* applicable to RPCS3's PowerPC guest; the design says so explicitly rather than implying a similar shortcut exists.
- **"Lightning" recompilation.** The new JIT still translates every block but first checks whether anything actually needs recompiling. In the best case it is a zero-cost passthrough; at worst a fraction of the x86 work. The January 2024 progress report: on Neptunia, the old JIT took 4.5 minutes and produced 633 MB in HighCq mode; the new JIT took 4.64 seconds and produced 348 MB.
- **W^X-safe memory manager.** `IJitMemoryBlock` exposes `MapAsRw`/`MapAsRx`/`MapAsRwx`, `ReservedRegion` granularity, and a `JitSupportDarwin` shim for `armeilleure_jit_memcpy`. W^X is an interface, not a set of ad-hoc `mprotect` calls.
- **PPTC** persists translated code across runs.

**Lesson for RPCS3:** compile time and code size are first-class; "only recompile what changed" is a valid architecture; W^X belongs behind an interface; do not pretend a hypervisor shortcut exists for a non-ARM guest.

### 4.6 PCSX2 and ARMSX2 — the closest analogue

PCSX2 is a PS2 emulator with a complex VM and 4 KB guest pages, closer to RPCS3's problem than Dolphin or PPSSPP.

- **Upstream PCSX2** merged "Native Apple Silicon Support — Part 1" (PR #11364) in June 2024. It added the native ARM64 build and platform work, but explicitly **no EE/VU/IOP recompilers** at that point; the estimate was a 2–3× improvement once they landed. The build still warns that Apple Silicon support is incomplete.
- **ARMSX2** is a fork that finished the ARM64 JITs. Its release notes are a catalogue of the techniques this document proposes:
  - **Direct block chaining**: patchable direct-branch tails replace LUT-indirect dispatch between blocks.
  - **SIGSEGV-backpatch fastmem**, extended from integer loads/stores to FP and vector loads/stores (`LWC1/SWC1`, `LQ/SQ`).
  - **Register cache coverage** across `MULT/DIV/MTHI/MTLO`, reducing full register spills.
  - **PGO**: shipping cores built with a profile trained on God of War 2, Jackie Chan, Valkyrie Profile 2, LEGO Batman, and GT4.
  - **Eliminating single-stepping** for `SYNC`/`CACHE` and other instructions that previously trapped every occurrence.
  - Later releases report tighter ARM64 code, ~10% faster recompilers, and instruction-level tests for every change.

**Lesson for RPCS3:** the closest analogue confirms the priorities — block chaining, fastmem backpatching for all widths, register-cache coverage, PGO, and removing per-instruction single-stepping.

### 4.7 Cemu and the rest

- **Cemu** merged "General aarch64 improvements & Apple Silicon support" (PR #1255) in June 2025 and ships `cemu-bin-macos-arm64` action builds. The Apple Silicon community builds pair the native recompiler with a Metal backend. CPU and GPU ports must land together; a native CPU with a Rosetta GPU path gives back much of the win.
- **Dolphin, PPSSPP, DuckStation, and Ryujinx** all ship universal or ARM64 macOS builds and all treat JIT memory, fastmem, and block linking as core architecture.
- **No emulator** in this survey reports a large win from an LLVM scheduling model. The wins are memory, dispatch, state, compile time, and GPU integration.

### 4.8 Cross-cutting lessons

| # | Lesson | Evidence | RPCS3 implication |
|---|---|---|---|
| 1 | Native ARM is worth 30–50% over Rosetta by itself | Dolphin M1 port | The native build is necessary but not sufficient |
| 2 | Direct access + fault backpatching beats re-decoding | Dolphin, Dynarmic, ARMSX2, DuckStation | Add patch metadata to RPCS3's fault sites |
| 3 | Block linking/direct chaining is mandatory | Dolphin link area, ARMSX2 direct tails, Dynarmic `BlockLinking` | Add link areas, IBTC, RSB |
| 4 | Register/CR caching matters more than instruction selection | Dolphin GPR/FPR/CR caches, ARMSX2 register cache | Improve IR shape; let LLVM allocate |
| 5 | Code-space reuse and per-page invalidation prevent stutter | Dolphin codegen reuse, DuckStation policy | Add per-page code bits and reuse |
| 6 | Compile time is gameplay-visible | PPSSPP #23049, Ryujinx 4.5 min → 4.64 s | Instrument and optimize the JIT pipeline |
| 7 | W^X and entitlements are a design surface | Dolphin, PPSSPP macOS 26, Ryujinx `IJitMemoryBlock` | Make RAII + signing matrix explicit |
| 8 | Remove per-instruction single-stepping | ARMSX2 `SYNC`/`CACHE` work | Audit RPCS3's trap/single-step paths |
| 9 | PGO and corpus-driven tuning help | ARMSX2, PCSX2 | PGO the emulator binary; use corpus metrics for JIT heuristics |
| 10 | Hypervisor shortcuts require ISA equality | Ryujinx | Not available for PowerPC; do not design around it |
| 11 | Measure p99/stutter, not just average FPS | Dolphin, PPSSPP, Ryujinx | Add frame-time and compile-time histograms |
| 12 | Keep interpreter fallback loud | DuckStation fallbacks, Dolphin parity work | Count and log every fallback |

---

## 5. RPCS3 today: the execution model, and where it leaves performance

This section is a code-level description of the current ARM64 path, not a summary of the documentation. Every claim was read from this tree. The purpose is to separate what is already good — and must be preserved — from what is structurally expensive.

### 5.1 One host function per branch target

RPCS3 does not compile a guest function as one host function. It compiles **one host function per guest entry point**, where an entry point is any address that can be branched to. `PPUTranslator::CallFunction` (`rpcs3/Emu/Cell/PPUTranslator.cpp:554`) is the single mechanism for control flow:

1. For a direct, in-module target, it obtains (or creates) an LLVM `Function` for that guest address.
2. It emits a call with the GHC calling convention and `TCK_Tail`:
   ```cpp
   CreateCall(callee, {m_exec, m_thread, seg0, m_base,
                       GetGpr(0), GetGpr(1), GetGpr(2)});
   c->setCallingConv(CallingConv::GHC);
   c->setTailCallKind(CallInst::TCK_Tail);
   CreateRetVoid();
   ```
3. For an indirect branch, it loads a host function pointer from the execution table (`m_exec + addr * 2`) and a 16-bit segment base, then makes the same GHC call.
4. For an out-of-module target, it calls through the module link table.

There is no fall-through **across a branch**. A conditional branch is implemented by `UseCondition` (`:845`) like this:

```cpp
FlushRegisters();                       // store every dirty guest register to ppu_thread
CreateCondBr(cond, taken, next, hint);  // taken = branch target path
// next block:
CallFunction(m_addr + 4);               // the *sequential* successor is a tail call
// taken block:
CallFunction(target);                   // the branch target is another tail call
```

After a `bt`/`bf`, **both** successors leave the function through the GHC ABI. Sequential code after a branch is not an inlined continuation; it is a new function entry. This is the most important structural fact in this document, and it is not visible from the source comments.

When a function needs it, `Translate` (`:241-265`) emits an entry check before the body. `need_check` is true when the block is at least 16 bytes or contains any branch, `BCCTR`/`BCLR`, or `SC` (`:200-230`). The check loads `ppu_thread::state`, tests for zero, and on a nonzero value raises `cpu_flag::wait` and tail-calls `__check`. Most nontrivial blocks pay it. (`TestAborted`, `:865`, is a different helper used around MMIO paths.)

**Why this matters.** A `cmpw; bt` pair at a block boundary costs, on the host:

- one compare and one conditional branch (unavoidable);
- a store of every dirty guest GPR to the thread struct (`FlushRegisters`);
- a GHC tail call with seven live arguments;
- at the callee: a `ppu_thread::state` load and test;
- reload of every guest GPR the callee needs (`RegLoad`, `:683`).

The guest executed two instructions. The host executed a function call, a state check, and a register spill/reload cycle. This is not an instruction-selection problem; it is an **IR state-shape and dispatch problem**, and it is the largest single opportunity in the PPU runtime.

### 5.2 Guest register state lives in `ppu_thread`

`m_locals` is an array of SSA values; `m_globals` holds the corresponding struct field pointers. `RegStore` (`:699`) records a value and marks the struct pointer dirty. `FlushRegisters` (`:705`) walks the array and emits a store for every dirty local. `RegLoad` (`:683`) emits a load from the struct when a value is needed but was not produced in the current function.

Consequences:

- LLVM cannot keep a guest register live across a block boundary. The boundary is a function boundary, so the value must go through memory.
- Every flush is a chain of stores; every entry is a chain of loads. On a 32-GPR guest with a branch every few instructions, this is a large fraction of the dynamic instruction count.
- Condition-register fields are four separate `i8` locals (`SetCrFieldSignedCmp`, `:5248`), so a flushed CR field is up to four byte stores. `GetCrb` (`:5214`) reloads one byte from the struct. Measured codegen: full-field materialisation is `cmp` + 3×`cset` + 3×`strb`; a single live bit folds to `cmp` + `b.cc` (Appendix A.8).
- `UseCondition` flushes **before** the branch, so even a branch that needs one CR bit pays for the whole field, and the successor cannot reuse the compare.

The fix is not a hand-written register allocator. It is to give LLVM a region of guest code with real CFG edges, keep guest state in SSA across those edges, and materialise to the thread struct only at true exits (calls, syscalls, faults, block-link misses). That is the region design in §8.3.

### 5.3 Memory: direct host access, signal decode on every fault

Guest memory is a flat host mapping. The translator receives a base pointer as its fourth GHC argument (`m_base`, set around `:233`) and lowers an access to:

```cpp
CreatePtrAdd(m_base, addr)                       // GetMemory, :860
CreateAlignedLoad(type, ptr, MaybeAlign{align})  // ReadMemory, :946
```

with `->setVolatile(true)` on both loads and stores (`:927`, `:947`, `:987`). There is no bounds check, no software TLB, and no per-access permission test on the fast path. That is the correct starting point: RPCS3 already has the "direct access" half of a fastmem design.

What it does not have is the other half:

- **No fault-site metadata.** When the host raises `SIGSEGV`/`SIGBUS`, `handle_access_violation` (`Utilities/Thread.cpp:1555`) re-decodes the faulting host instruction. On ARM64 it calls `decode_a64_mem_inst` (`~:1838`) to recover the operation, size, and register. On the SPU path it can emulate the access and advance `PC`. On the PPU path it validates `vm::check_addr`, maps or protects the page, and returns to retry. **Every** fault pays a full decode. A recurring, expected fault is never converted into a cheaper form.
- **Volatile everywhere.** Every guest load and store is `volatile`, so LLVM cannot merge, widen, sink, hoist, or eliminate them. That is conservative for MMIO and device registers, but it also blocks the most basic optimisation on ordinary RAM.
- **A 4 KB guest bitmap on a 16 KB host.** `vm::g_pages` is `std::array<atomic_t<u8>, 0x100000000 / 4096>` (`rpcs3/Emu/Memory/vm.cpp:87`), one byte per guest 4 KB page. `check_addr` (`rpcs3/Emu/Memory/vm.h:82-93`) is the bitmap test used by the interpreter and slow paths, not by generated code. When the host page size exceeds 4096, `vm.cpp:747` and `:981` skip native page mapping and unmapping:
  ```cpp
  // If native page size exceeds 4096, don't map native pages (expected to be always mapped in this case)
  const bool is_noop = bflags & block_size_4k && utils::get_page_size() > 4096;
  ```
  A 4 KB guest hole therefore cannot be represented by the host MMU, and no fault can fire for it. That is the structural cause of #19881.
- **One generic slow path.** There is no fastmem page-table tier, no "patch this site to a slow call", and no per-page invalidation of generated code.

### 5.4 Dispatch and the execution table

`vm::g_exec_addr` is a 4 GiB reservation (`vm.cpp:49`) holding two words per guest address: a host function pointer and a segment tag. An indirect branch loads both (`PPUTranslator.cpp:648-658`):

```cpp
const auto pos = CreateShl(indirect, 1);
const auto val = CreateLoad(u64, CreatePtrAdd(m_exec, pos));
callee = FunctionCallee(type, CreateIntToPtr(val, u8*));
```

This is a flat, constant-time indirect branch target table. It is better than a hash lookup, but it has no inline cache: the load is a dependent 8-byte load on the critical path of every indirect call, followed by an indirect branch whose target is unknown to the predictor. Guest return addresses are also indirect branches, so **every guest `blr` and `ret` is a predicted-indirect branch through memory**. There is no return-stack buffer and no monomorphic site cache.

Direct branches are already "linked" in the sense that they are direct calls, but they are not **chained**: the callee re-enters through the GHC ABI and re-checks state. SOTA emulators treat a linked direct branch as a few instructions and a fall-through, not a function call.

### 5.5 JIT module lifecycle and invalidation

PPU modules are compiled as a unit by `ppu_initialize` (`PPUThread.cpp:5109`), cached on disk as:

```
ppu-<sha1(EBOOT)>/v9-kusa-<hash16>-<settings bitfield>-<resolved CPU>.obj
```

(`PPUThread.cpp:5816`) and loaded with `jit_compiler::load_object` (`:5850`). Compiled modules are owned by `jit_module_manager` (`:3901`) keyed by module name and segment pointer.

The invalidation model is **module-level**: unloading a PRX or overlay destroys the whole `jit_module`. There is no page-granular code invalidation, no code-space reuse, and no "recompile only the changed page" path. On a PS3 this is mostly fine — retail games do not self-modify hot code — but it has three costs:

1. Any code change (overlay load, patch, PRX reload) forces a full module recompile and a visible hitch.
2. There is no cheap way to invalidate a single block after a fastmem patch or a page-protection change.
3. The cache key does not include the LLVM version. Machine-code objects are stable across LLVM versions *if the IR is stable*, but this design adds new IR (frame checks, fastmem sites, CR packing), so the first such PR must bump `v9` or silently load stale objects (Appendix A.9).

### 5.6 W^X, icache, and entitlements

The code paths are present and mostly correct:

- `Utilities/JIT.h:441-452` defines `jit_write_guard`, an RAII guard around `pthread_jit_write_protect_np(false/true)` with `atomic_fence_acq_rel` / `atomic_fence_seq_cst`. It is used by `build_function_asm`.
- `Utilities/JITLLVM.cpp:73-81` toggles write protection around LLVM code emission.
- `Utilities/JITASM.cpp:315-333` toggles around runtime code generation.
- `PPUThread.cpp:5982` and `SPULLVMRecompiler.cpp:4218` toggle around module load/relocation.

What is missing is a **contract**:

- No explicit entitlements file in the repository (only a third-party wolfSSL one). macOS CI ad-hoc signs (`.ci/deploy-mac.sh:69`), which does not prove a notarized Hardened Runtime build can map JIT memory.
- No `sys_icache_invalidate` audit. LLVM's `RTDyldMemoryManager` may publish code, and `JITASM` uses `__builtin___clear_cache` in places, but there is no single choke point that proves every path invalidates before execution.
- No nesting discipline. Nested guards toggle the thread state repeatedly; a counter-based guard is cheaper and safer.
- No stress test that patches code while another thread executes an adjacent region.

### 5.7 What is already good and must not be broken

| Property | Where | Why it matters |
|---|---|---|
| Flat host mapping with direct `m_base + addr` | `PPUTranslator.cpp:860` | The fast memory path already exists; no interpreter-style dispatch is needed |
| Signal-based fault handling with a real instruction decoder | `Thread.cpp:1838` | Faults can be classified and resumed; the missing piece is caching the result |
| GHC tail calls with a shared 8 KB scratch slab | `PPUThread.cpp:380`, `SPUCommonRecompiler.cpp:513` | Cheap block entry; the redesign keeps the ABI and adds frames only where needed |
| Constant-time indirect branch table | `PPUTranslator.cpp:648` | A solid base for an inline cache |
| Per-module disk cache with settings + CPU in the key | `PPUThread.cpp:5816` | Deterministic recompilation; only the version bump is missing |
| LSE2/CASP atomics | `atomic.hpp:1111,1170`, #19611/#19977 | 16-byte guest atomics are already native |
| `os_sync_wait_on_address` futex path | `Utilities/sync.h:90` | The only parking primitive that works on macOS; do not replace it |
| RAII W^X guard | `Utilities/JIT.h:441` | Correct discipline; needs batching and an audit, not a rewrite |

### 5.8 The gap, precisely

| Capability | SOTA Apple Silicon emulator | RPCS3 today | Gap |
|---|---|---|---|
| Guest memory fast path | Direct access with a page-table/arena guard | Direct access, no guard | **Small** — keep it |
| Fault handling | One-time patch to a slow call, or page-table fallback | Re-decode every fault | **Large** |
| 4 KB guest holes on 16 KB host | Software sub-page map or per-page table | Bailout; no fault | **Large (correctness)** |
| Code invalidation | Per-page, with code-space reuse | Per-module | **Medium** |
| Direct branch | 3-instruction link area / direct tail | GHC tail call + state check | **Medium–large** |
| Indirect branch | Inline cache / IBTC | Dependent load from a table | **Medium** |
| Return | Return-stack buffer | Indirect branch through the table | **Medium** |
| Guest state | Register/CR cache in host registers | SSA per block, flushed at every boundary | **Large** |
| Compile time | Phase-level profiling, IR pass skipping | Not instrumented | **Unknown** |
| W^X | Interface with batching | Ad-hoc toggles, no entitlement test | **Medium** |

The rest of this document designs the four "large" rows and the three "medium" rows, in that order.

---

## 6. Performance model: where the time actually goes

### 6.1 Thread and workload topology

The reference machine is 2 P-cores + 4 E-cores, 8 GB, 16 KB pages, 128 B lines. A typical RPCS3 session runs:

| Role | Threads | Notes |
|---|---:|---|
| PPU | 2 | main + one; each runs PPU LLVM |
| SPU | up to 6 | `Max SPURS Threads: 6`; each runs SPU LLVM |
| RSX | 1 render + 1 offloader | multithreaded RSX |
| Audio, input, save, HTTP | 1 each | low CPU, latency-sensitive |
| Shader compiler workers | N | bursty, competes for P-cores |

That is **ten or more runnable emulation threads on two P-cores**. Placement and preemption matter as much as per-thread code quality. The measured `SCHED_RR` result (Appendix A.1) is a symptom of this oversubscription, not a general scheduling win.

### 6.2 Cost buckets

The design does not assume a distribution. It specifies how to measure one, because every prior performance argument in the issue tracker was title-specific.

| Bucket | Instrumentation | Hypothesis | Confidence |
|---|---|---|---|
| Guest PPU code | `perf_meter` around the PPU gateway; signposts | 20–35% | low |
| Guest SPU code | `perf_meter` around the SPU gateway | 25–40% | low |
| RSX + offloader | queue wait, DMA copy, Metal/Vulkan submit | 10–25%, spikes to 90% on a bad boot | low |
| Synchronization | futex/atomic wait counts and wait time | 5–15% | low |
| JIT compile | per-module wall time, cache hit/miss, frame histogram | 0–30% on first boot | medium |
| Host/OS | audio, input, compositor | remainder | high |

The only defensible claim is that PPU + SPU together dominate steady-state time on the local corpus, because the GPU is not the bottleneck at 720p on an A18 Pro. That is a hypothesis with a measurement plan (§10), not a result.

### 6.3 Per-block cost decomposition

Take a PPU block of *N* guest instructions ending in a conditional branch. The host work is:

```
entry:   load ppu_thread::state ; test ; branch        (if need_check)
         reload live guest GPRs from ppu_thread         (RegLoad)
body:    N translated instructions
         compare + conditional branch
exit:    store every dirty guest GPR                   (FlushRegisters)
         GHC tail call: 7 register arguments
callee:  load state ; test ; branch
         reload live GPRs
```

If *N* is large, the fixed cost is amortised. If *N* is small — and PS3 code is branch-dense: 341 k `bt`/`bf` and 210 k `cmpwi` in Demon's Souls alone (Appendix A.5) — the fixed cost dominates. A branch every 5–8 instructions with 10–20 host instructions of entry/exit overhead is a **2–4×** instruction-count tax on control-flow-heavy code before any guest work is counted. `[M]` The static half is now measured: 11.9–12.2% of all guest instructions end a basic block, so a branch occurs every 8.2–8.4 instructions; the block median is 4 and the mean 5.6, and the current compiler inlines a median of only 4–5 instructions into one GHC function (Appendix A.12). The multiplier itself still needs a dynamic instruction-count profile.

The point is not the exact multiplier. The point is that the dominant term is **per-block fixed cost**, and it is addressable by:

1. keeping guest state in SSA across CFG edges instead of flushing at every edge;
2. chaining direct branches instead of re-entering through the ABI;
3. caching indirect targets instead of a dependent load;
4. checking `ppu_thread::state` once per region, not once per block.

### 6.4 Why native can lose to Rosetta — and why that framing is now suspect

The honest list of mechanisms by which a native build can underperform Rosetta:

1. **Silent interpreter fallback.** An LLVM failure that falls back to the interpreter turns 60 FPS into 5 FPS with no error in the UI.
2. **GHC codegen constraints.** No real frame, fewer callee-saved registers, LR reloads around tail calls, and a shared scratch slab constrain LLVM compared with the x86 build's normal ABI.
3. **Block-boundary state traffic.** §6.3. Rosetta runs x86 code produced by the same translator, but the x86 backend's register allocation and call shape differ; the comparison is not apples to apples.
4. **Fault decode cost.** Every guest page fault re-decodes a host instruction; the x86 build has a different (not necessarily cheaper) path.
5. **Thread policy.** `SCHED_RR` 99 on every emulation thread is a blunt instrument; measured, it wins synthetic throughput but says nothing about frame-time p99.
6. **W^X and cache build.** MAP_JIT toggles and module recompiles add first-boot and overlay-load hitches.

What the evidence does **not** support is a current, large, general native-vs-Rosetta gap. #17231 — the 5–10 FPS versus 30 FPS report — was closed in January 2026 after the reporter confirmed steady 30 FPS on ARM. Every performance claim in this document is therefore relative to an **ARM baseline**, and the first implementation milestone is to build an x86_64 RPCS3 on the reference machine and capture a real A/B (Appendix A.13).

### 6.5 Measured microbenchmarks

**Evidence: [M]** — measured 2026-11-12 on the reference machine.

| Kernel | Native arm64 | x86_64 + AVX2 under Rosetta | Ratio | Caveat |
|---|---:|---:|---:|---|
| two-table byte shuffle + widening madd + byte sum, dependent chain | 5.86 ns/iter | 15.34 ns/iter | native 2.6× faster | single run after warm-up; synthetic |
| 64-byte copy loop | 1.54 ns/iter | 0.93 ns/iter | native 0.6× (Rosetta faster) | store-to-load forwarding and loop shape dominate |

The second row is the important one. "Native wins" is not a law; it is a property of the kernel. Any optimisation proposal must name the kernel it improves and show the before/after assembly, not assert a global speedup.

### 6.6 The ceiling

An optimistic but defensible model for the PPU runtime:

- If per-block fixed cost is 30–50% of PPU time today (hypothesis), and SSA-across-edges plus chaining removes half of it, PPU time drops 15–25%.
- If PPU is 25–35% of frame time, that is a **4–9%** end-to-end win.
- Fastmem backpatching mostly affects fault-heavy titles and first-boot/overlay paths; it is a **stutter and correctness** win more than a steady-state FPS win.
- CR packing is smaller still: 1–4% of PPU-bound time, i.e. well under 1% end-to-end.

That is the realistic shape of the prize: **single-digit to low-double-digit percent**, concentrated in CPU-bound titles, plus the removal of hangs and crashes that no FPS number captures. It is worth doing because the alternative is losing Rosetta with no native path at all, not because there is a hidden 3×.

---

## 7. Defect register and open correctness risks

**Evidence: [V]** for code-audited defects; **[M]** for measured behaviour (Appendix A); **[U]** for the open risks in §7.2, which are explicitly unproven.

### 7.1 Confirmed defects

| # | Defect | Location | Evidence | Severity | Fix in |
|---|---|---|---|---|---|
| D1 | The QoS class is destroyed by the RPCS3 attribute sequence: `QOS_CLASS_USER_INTERACTIVE` then `SCHED_RR` + `pthread_attr_setschedparam(99)` leaves QoS `UNSPECIFIED(0)`, `SCHED_RR`, priority 99 | `Utilities/Thread.cpp:2854-2856` | measured on macOS 27.2/A18 Pro: `qos=0 pol=2 prio=99` | medium | §8.5.3 |
| D2 | `pthread_setschedparam` on a running QoS thread also unsets QoS and drops effective priority to `SCHED_OTHER` max (47); `set_native_priority` does this on macOS via `scoped_priority` | `Utilities/Thread.cpp:3915-3946`, `Thread.h:349-364` | measured: QoS `UNSPECIFIED(0)` after the call | medium | §8.5.3 |
| D3 | The Apple affinity path computes `countr_zero(full_mask)`; tag 0 is a no-op, so no thread receives a P/E placement hint. The comments imply otherwise | `Utilities/Thread.cpp:3982-3986`, `CPUThread.cpp:658-661` | code audit: `get_affinity_mask()` returns the process mask; `countr_zero` = 0 | low (advisory API) | §8.5.3 |
| D4 | `memory_decommit` does `munmap` then `mmap` **without** `MAP_FIXED`; the address is only a hint and another thread can claim it | `rpcs3/util/vm_native.cpp:354-355` | code audit; `mmap` result merely `ensure`d equal to the hint | high | §8.1.5 |
| D5 | `memory_reset` has the same non-`MAP_FIXED` race | `rpcs3/util/vm_native.cpp:384-385` | same | high | §8.1.5 |
| D6 | `STVLX`/`STVRX` lower through the generic AArch64 masked store: 31 conditional branches + 16 stores; the merge-store form is 6 instructions, 0 branches | `rpcs3/Emu/Cell/PPUTranslator.cpp:3730,3758` | `llc -mcpu=apple-m4`, `measurements/2026-11-12/mstore2.ll` | medium (latent) | §8.3.4 |
| D7 | `m_use_avx` is set on ARM but every use is under `ARCH_X64` or an x86-only `#else`; it is a dead flag | `CPUTranslator.h:3244`; uses in `SPULLVMRecompiler.cpp` under `ARCH_X64` | macro-context audit | none (document) | §8.3.5 |
| D8 | `lwsync` is lowered as `seq_cst` because of `op.l10 && false`; on AArch64 `dmb ish` is also a correct acq_rel fence, so this is conservative, not wrong | `PPUTranslator.cpp:3710` | code audit + ISA check | none (document) | — |
| D9 | Each CR field is four independent `i8` locals; `UseCondition` flushes them **before** the branch, so a compare+branch at a block boundary can emit `cmp` + 3×`cset` + 3×`strb` plus a byte load in the successor | `PPUTranslator.cpp:5248`, `:845`, `:5214` | `llc` repro `cr.ll` | medium (largest PPU codegen surface) | §8.3.3 |
| D10 | `wfe` does not park in user space on macOS 27.2/A18 Pro; it burns ~100% of a core at 0.74 M ops/s and wakes no faster than `isb` | proposed spin helper (not in tree); #18151 advice | measured, `wfe3.cpp`/`wfecount.cpp`; `os_sync_wait_on_address` reports 0.0000 s CPU parked | high for design, none shipping | §8.5.3 |
| D11 | Every basic block is a host function. A conditional branch tail-calls **both** successors; the sequential successor `m_addr + 4` is a new function entry with an entry state check (unless the block is <16 bytes and has no branch) and register reload | `PPUTranslator.cpp:845-858`, `:554-666`, `:241-265` | code audit; the emitted IR is unambiguous | high (largest structural cost) | §8.2, §8.3 |
| D12 | Every guest load and store is emitted as `volatile`, blocking merging, widening, sinking, hoisting, and dead-store elimination even for ordinary RAM | `PPUTranslator.cpp:927,947,987` | code audit | medium | §8.1.1 |
| D13 | No page-granular code invalidation and no code-space reuse: invalidation is module-level, and an overlay reload recompiles the whole module | `PPUThread.cpp:3901-3970`, `:4137` | code audit | medium | §8.1.4 |
| D14 | The frame slab is a fixed 8192 bytes and overflow is silent; LLVM under GHC writes positive offsets from `sp` with no prologue, so a >8 KB frame corrupts the caller | `PPUThread.cpp:380`, `SPUCommonRecompiler.cpp:513` | `llc` repro `ghccc2.ll`: 12 KB alloca → `str w8, [sp, #11996]`, no `sub sp` | high (#19939) | §8.4 |
| D15 | The PPU cache key includes CPU and settings but not the LLVM version, and there is no IR-version bump discipline beyond the literal `v9` prefix | `PPUThread.cpp:5816` | Appendix A.9 | medium (silent stale code) | §8.5.1 |

Three notes.

**D11 is the load-bearing defect.** It is not in any issue report because it is not a bug; it is the architecture. It explains why the PPU translator's output is so sensitive to branch density, why inlining the sequential successor is the highest-value structural change, and why "just tune the scheduler" cannot work.

**D9 and D11 compound.** `UseCondition` flushes registers *because* the successors are separate functions. Fix D11 (SSA across edges) and D9 becomes a local codegen improvement instead of a per-branch tax.

**D12 is a correctness/performance trade.** Volatile protects MMIO and device registers. The design keeps volatile for regions marked MMIO and for the `m_may_be_mmio` heuristic (`ReadMemory`, `:888`), and removes it only for ordinary guest RAM, with the fastmem guard making the access re-executable.

### 7.2 Open correctness risks

These are the places where the design could be wrong. Each names the experiment that settles it; they reappear as falsification criteria in §12.2.

| # | Risk | Why it matters | What closes it |
|---|---|---|---|
| A1 | #19881's root cause is not proven to be "4 KB holes on 16 KB pages". The issue also mentions a 64 KB block path and `flags=0x1020a` that were never diagnosed | If GT5's hang is a 64 KB `mmapper` bug, the general sub-page map is wasted work and may not fix the title | GT5 dump; instrument `_page_map` and the 64 KB path; reproduce `flags=0x1020a` before shipping the map |
| A2 | A software sub-page check on every access in a notification region has no cost model | A 10% tax on all memory ops to fix one title is a net loss | Per-region gating, a synthetic `ldr`/`str` benchmark, DS/RR7 before/after |
| A3 | Restart semantics for a blocking fault helper are unspecified. If the helper blocks and restarts, the translator must reconstruct GPRs, CR, reservation, and PC | A naive helper either deadlocks or silently corrupts state; worst case it forces interpreter fallback for the region | Prototype one restartable `lwz`; 10⁶ randomised faults with state equality vs the interpreter |
| A4 | A per-function frame thunk changes the GHC tail-call contract for every caller and for far-return unwinding (`x30`, `x19–x28`, `q4–q15`) | A thunk correct in isolation can corrupt a caller that assumed the shared slab; the two-pass retry recompiles the callee, not its callers | 3-deep call-chain model test where only the middle function is oversized; ASan + canary; far-return path |
| A5 | `.stack_sizes` is an LLVM-internal ELF-only contract | A triple or flag change silently disables the frame check, and the gateway overflow returns | Startup log + CI test asserting the section exists (CPU-FRAME-035); fall back to the slab, never assert |
| A6 | Bumping the PPU cache version invalidates every user's cache | Users report "first boot is much slower" and may pin an old build | Release note + cold-cache DS/RR7 measurement; bundle the bump with the IR change |
| A7 | Batching W^X writes interacts with signal handlers and backtraces; a batched patch can leave stale icache after a signal-driven patch | A rare stale-icache bug is a nightmare to debug and gets blamed on the emulator | 10⁶ patches with concurrent signal delivery; `sys_icache_invalidate` audit; assert every publish path flushes |
| A8 | The sub-page map only covers PPU `ReadMemory`/`WriteMemory`; instruction fetch, SPU MFC DMA, RSX transfers, and `mmapper` copies are separate paths | GT5 may rely on DMA or execute from the region; the map would be incomplete and the fault still would not fire | Enumerate every access path; route it through the check or prove it cannot target a notification region |
| A9 | Page-fault events have re-arm/level semantics the design does not specify; one 16 KB host page covers four guest pages | Lost events hang; duplicate events break titles that count them | Model test with four guest pages in one host page, randomised access/fault/re-arm; compare the event trace against the x86 build |

### 7.3 Issue map

The permanent `CPU: arm64` label plus mega-issue #16484 is the regression corpus. The pattern is consistent: same title, same settings, x64 works, ARM64 fails, and the fallback is the interpreter or Rosetta.

| Issue | State | Symptom | Verified or indicated cause | Design response |
|---|---|---|---|---|
| #16484 | open | Heavenly Sword black screen; MGS4 black screen; GT5 `Narrowing error`; Dark Souls II crash; Rayman Legends crash; TLOU stuck; GTA V crash/leak; Uncharted 3 stuck | multiple: frames, NEON lowering, page faults, atomics | all workstreams |
| #19939 | open | Prototype 2 crash ~80 s in; Max Payne 3 hang on the first level | SPU frame > 8 KB overflows the gateway slab | D14, §8.4 |
| #19881 | open | GT5 never gets a page fault on ARM; x64/Rosetta gets 164; hangs at the logo | 4 KB guest holes cannot be expressed on 16 KB host pages; 64 KB path undiagnosed | D13, §8.1.3, A1 |
| #19801 | open | LLVM "Cannot scavenge register without an emergency spill slot" | GHC/no-frame codegen + TBL2 fallback | §8.3.5; needs a reduced IR repro |
| #19369 | open | ARMSX3's portable ARM64 optimisations not evaluated | not upstreamed | §8.2, §8.3 |
| #18429 | open | ASan stack-buffer-underflow in `atomic_wait_engine::notify_all` | wait-engine stack lifetime; 16 KB stack alignment | §8.5.3 |
| #19759 | open | GTA IV 13–30 FPS with MT RSX, 60 FPS without | 16-byte LL/SC retry storm on the RSX queue; CASPAL landed | verify with 16-byte counters, §8.1.6 |
| #17231 | closed | Skylanders 5–10 FPS native vs 30 FPS Rosetta | **resolved Jan 2026**; reporter confirmed 30 FPS on ARM | do not cite as a live gap |
| #19611 | closed | GTA IV 18.7 FPS median | fixed by LSE2 16-byte loads/stores (19 → 58 FPS) | keep, add tests |
| #19609 | closed | Far Cry 3 SPU spins on one `ret`; 1/8 boots reach gameplay | GHC transform visited-set not reset per module | keep, add test |
| #17905 | closed | FPS instability from thread throttling | force max pthread priority | revisit with D1/D2 |
| #18151 | closed | spin-loop power/perf | `isb` instead of `yield` | keep; **do not** add WFE (D10) |

`[V]` Issue states and bodies checked 2026-11-12. Performance numbers in the table are from the reporters, not measured locally, except where Appendix A says otherwise.

---

## 8. Design: ASNR — Apple Silicon Native Runtime

The design has a name because it needs a boundary. **ASNR** is the CPU and memory runtime RPCS3 uses on Apple Silicon. It is not a fork and not a second emulator. It is a set of interfaces and code paths that replace the current per-block dispatch, fault handling, and state materialisation, gated on `__APPLE__ && ARCH_ARM64`, with shared code underneath wherever the change is architecture-neutral.

### 8.0 Principles

1. **Correctness is a gate, not a phase.** Every workstream lands with the tests that prove it, including negative cases. A perf PR that weakens an invariant is rejected regardless of its number.
2. **Keep the existing fast paths.** Direct `m_base + addr` access, the GHC ABI, the exec table, and `os_sync_wait_on_address` are assets. ASNR changes what surrounds them.
3. **One new choke point per concern.** Faults go through one site table. Dispatch goes through one link area. State goes through one materialisation function. No second ad-hoc path.
4. **Every optimisation is reversible.** A config flag for one release, defaulting to the measured-good value.
5. **Measure the corpus, then generalise.** Demon's Souls, Ridge Racer 7, and Mega Man 9 decide; microbenchmarks only justify a hypothesis.
6. **No hidden budgets.** Every new table, cache, or stub area has a size, an eviction policy, and a test.
7. **Fail loud.** A missing `.stack_sizes`, an unpatched fault site, or an interpreter fallback produces a counter and a log line, not silence.

### 8.1 Memory layer v2

#### 8.1.1 Three tiers

The current design has one memory path: direct access, and a generic fault decoder when it fails. ASNR defines three tiers and chooses per access at translation time and per page at runtime.

| Tier | When | Generated code | Fault behaviour |
|---|---|---|---|
| **T0 — direct** | Ordinary guest RAM, site has run fault-free N times, not MMIO, not a notification region | Bare `ldr`/`str` (non-volatile), no guard | Treat as a bug, but recover: the site record still exists in the module, so the handler can demote it to T1 |
| **T1 — guarded** | Every access that is not statically MMIO and has not been promoted | Bare access plus a site record; optional sub-page check (§8.1.3) | Fault is expected; map/queue/retry, patch only if it recurs |
| **T2 — helper** | MMIO, device registers, RSX-mapped memory, addresses statically known to be special | Direct call to a C++ helper | The helper handles everything; no host fault |

**The tier cannot be chosen purely at translation time.** The translator sees a guest PC and a base register, not a dynamic address, so it cannot prove that a load targets ordinary RAM. Any rule that claims otherwise is wrong the first time a legitimate `munmap` of an overlay, a `mmapper` remap, or a lazily committed page changes the mapping under compiled code. The resolution is to separate *code shape* from *promotion*:

- **v1 emits T1 for every data access** — a bare load/store plus a site record. The record is module metadata, not instructions; it costs bytes in the module, not cycles in the block. There is no codegen branch and no runtime range check.
- **T0 is a promotion, not a translation-time decision.** After a site has executed without faulting N times, the site table marks it `trusted`; the next recompile of the region emits the same instruction without the record (or with a flag that skips the handler on fault). The page index already knows how to invalidate the region if the page's mapping changes.
- **Demotion is automatic.** A fault at a trusted site is a legitimate event (remap, overlay) or a bug. Either way the handler has the guest address and the register file; it fixes the mapping, clears the trusted bit, and recompiles at the next entry. It never crashes and never silently continues.
- **T2 is decided by the existing MMIO heuristic plus the region map**, and it is the only tier that must be chosen before the first execution, because calling the helper is a different instruction shape.

This keeps the fast path zero-overhead in steady state, keeps the first execution safe, and removes the impossible "prove it is ordinary RAM" step. The cost is one site record per access in the module — roughly 16 bytes per memory instruction, which is bounded by the module size and discarded with it.

The volatile question (D12) follows the same split:

- T0 and T1 accesses are non-volatile. The guest memory model is ordinary RAM; LLVM may merge and reorder them within a region, subject to the barriers emitted for guest `sync`/`lwsync` and for atomics.
- T2 accesses and any access in an MMIO region remain volatile.
- The translator must not merge accesses across a guest fence or across a possible fault. The site record gives the fault handler enough information to re-execute one access; it does not need every access to be a compiler barrier.

This is the largest codegen change in ASNR and the one that needs the most differential testing, because it changes the meaning of generated IR, not just its shape.

#### 8.1.2 Fault-site backpatching

A fault site is a generated memory instruction that can raise a host fault. For every T1 site, the module records:

```cpp
struct fastmem_site
{
    u32 host_offset;   // byte offset of the instruction within the module code block
    u32 guest_page;    // guest 4 KB page of the last fault, 0 if never faulted
    u8  size_log2;     // 0..4 (1..16 bytes)
    u8  flags;         // is_write | is_paired | patched | mmio
    u8  reserved[2];
};

struct code_block
{
    u32 guest_start;         // first guest instruction
    u32 guest_end;           // one past the last guest instruction
    u8* host_start;          // executable code
    u8* host_end;
    u32 invalidation_epoch;
    std::vector<fastmem_site> sites;   // sorted by host_offset
};
```

The fault path becomes:

```text
SIGSEGV/SIGBUS
  -> addr = si_addr
  -> if addr is not inside the guest reservation: host bug, report and die
  -> pc = ucontext PC
  -> block = module_index.find_block(pc)          // binary search over host ranges
  -> if !block: unknown code, fall back to the old decoder
  -> site = block.find_site(pc)                   // binary search over sorted sites
  -> if !site: the fault is not a guest access (stack, JIT data); old path
  -> classify(addr, site):
       mapped and permitted      -> unprotect/map, retry (no patch)
       notification region       -> queue mmapper event, block, retry
       MMIO                      -> patch site to T2 stub, retry
       unmapped and not guest    -> guest fault semantics, queue or abort
       repeated fault on a page  -> patch site to T2 stub after N hits
```

Two things make this better than today:

1. **The block lookup is a binary search in a per-module sorted array**, not a scan of all guest addresses. With a few thousand blocks per module, that is ~12 comparisons, and it happens only on a fault.
2. **Recurring faults patch the site.** After the first fault on a site, the handler records it. On the second fault at the same site within a window, it overwrites the instruction with a branch to a **per-site stub**:

   ```asm
   ; before
   ldr  w8, [x20, x9]
   ; after (4-byte b, ±128 MB, near code space)
   b    <site_stub_N>
   ```

   Each site has its own stub in the module's stub island. The stub knows its site index and its resume address as link-time constants, loads them from a nearby literal (or from its own code), calls the C++ helper with `(site_index, guest_addr, width, is_write)`, and branches to the resume address — the instruction after the original access. It does not use the link register as a data pointer, because the GHC ABI does not preserve it across a tail call. It *must* still save and restore `x30` around the helper call: the 4-byte `b` that enters the stub leaves the region's return address in `x30`, and `bl` overwrites it. The prototype's first stub omitted that save and returned to itself, hanging the process; the fixed stub is `str x30,[sp,#-16]! ; bl helper ; ldr x30,[sp],#16 ; ret`. If the stub is out of `b` range (far code space), use a 16-byte patch island:

   ```asm
   ldr  x16, #8
   br   x16
   .quad <site_stub_N>
   ```

   The 16-byte form needs 16 bytes of patchable space per T1 site, or a per-site trampoline in a near island. The design reserves a near island per module (`module_code::stub_area`) so the 4-byte form is always available; the 16-byte form is the fallback for far code space.

Patching rules:

- Take the W^X write guard, patch, `sys_icache_invalidate` the 4 or 16 bytes (plus the cache line), release the guard.
- Patching is per-thread state on Apple; the executing thread must not be inside the instruction being patched. Use a per-module `quiesce` epoch: the faulting thread holds the site; other threads are at a block boundary or in the handler. Document the protocol and test it (§7.2 A7).
- A patched site stays patched until the page is mapped and the block is recompiled. Do not auto-unpatch on unmap; that would flap.

Policy knobs:

- `fastmem_patch_threshold` (default 2): how many faults a site takes before patching.
- `fastmem_recompile_on_fault` (default on for T1, off for T0): whether the block is recompiled after a patch so LLVM can see the helper call.

**Why not patch on the first fault?** The first fault is usually a legitimate lazy-commit or mmapper event that resolves to a valid mapping. Patching then would slow the common case. The second fault is evidence of recurrence.

**Prototype result (Appendix A.12).** A one-site prototype on this machine exercises the whole path: `ldr w0,[x0]` in a JIT page, a 64 MB `PROT_NONE` arena, a `SIGSEGV` handler that commits the faulting 16 KB page, and a retry of the same instruction.

| Step | Measured |
|---|---|
| Fault → commit → retry, per fresh 16 KB page | mean 5.6–5.8 µs, p50 5.1–5.3 µs, p99 14–17 µs |
| Steady-state bare load | 1.10–1.15 ns |
| Patch one site: `mprotect` RW + store + `mprotect` RX + `sys_icache_invalidate` | mean 3.0–3.3 µs |
| Batched patch, one W^X scope, 4096 sites | 5.0–7.2 µs total, 1–2 ns/site |
| Demoted site: `b` → save `x30` → `bl` helper → restore → `ret` | 1.85 ns |

Consequences for the design:

- The fault path is ~5,000× a load. That is fine once per page and unacceptable per access, which is exactly why T0 promotion exists and why a site must be patched after the second fault rather than re-faulting forever.
- The two `mprotect` calls dominate a single patch. The RAII W^X scope is therefore a prerequisite for patching more than a handful of sites, not an optimisation; individual `mprotect` toggles per site would cost more than the fault they prevent.
- `sys_icache_invalidate` is not the expensive part; the W^X transition is.
- Nothing in the path cares that the guest pointer is 64-bit: the handler reads the exact fault address from the exception state and the site from the host PC. The range-check thesis in §12.2 is answered.

#### 8.1.3 The sub-page notification map

This is the #19881 fix. The host page is 16 KB; the guest page is 4 KB. Four guest pages share one host page. A notification region needs a fault when the guest touches an *unmapped guest page*, even if the host page is mapped because a sibling guest page is valid.

Within a notification region, a host page has one of three states:

| Host-page state | Condition | Host protection | Generated-code behaviour |
|---|---|---|---|
| **Full** | all four guest pages mapped | `RW` | T0 access, no check |
| **Empty** | all four guest pages unmapped | `PROT_NONE` | hardware fault; handler queues event |
| **Partial** | some mapped, some not | `RW` | T1 access with sub-page check |

The check, emitted only for T1 accesses that can reach a notification region:

```text
if (unlikely(g_mem_subpage_active)) {          // one load + predictable branch
    if (unlikely(g_host_page_state[addr >> 14] == partial)) {
        if (unlikely(!g_subpage_mapped[(addr >> 12) & 3])) {
            call vm::handle_guest_subpage_fault(addr, is_write);
        }
    }
}
```

`g_host_page_state` is a byte per 16 KB host page in the guest reservation (256 K entries for 4 GiB, 256 KB of memory), with values `full`, `partial`, `empty`, `not_notify`. `g_subpage_mapped` is the existing 4 KB bitmap (`vm::g_pages`) or a dedicated 4-bit field per host page.

Cost control:

- The outer `g_mem_subpage_active` flag is 0 unless at least one notification region exists. In that state the cost is one load and one predicted-not-taken branch per T1 access. The design targets **≤1%** on DS/RR7; if it exceeds that, gate the check at block level (a per-block flag set when any address in the block statically falls in a notification range) and re-measure.
- `Partial` pages are rare, only at the boundary of a mapped region. The second branch is almost never taken.
- Fully empty host pages use the hardware fault, which costs a signal but requires no generated check.

The slow path, `handle_guest_subpage_fault`:

1. Validate the guest page against `vm::g_pages`.
2. If mapped: `mprotect`/state-fix the host page, update `g_host_page_state`, return to retry. This is a race or a stale partial state.
3. If unmapped: queue a `sys_mmapper` page-fault event exactly like the x64 path (`Thread.cpp:2070-2145`), mark the guest page "fault pending", and block the faulting thread until the game re-arms or maps it.
4. On `sys_mmapper` map/unmap/notification changes, recompute the host-page state for the affected 16 KB pages and update `g_subpage_mapped`. This is the re-arm path; test it with four guest pages in one host page (§7.2 A9).

Scope rule: the check is only for **data accesses through `ReadMemory`/`WriteMemory`**, and every other path must be enumerated (§7.2 A8):

| Path | Can it touch a notification region? | Handling |
|---|---|---|
| PPU/SPU data loads/stores | yes | T1 check |
| Guest instruction fetch | yes, if code executes from the region | route through the same check; or prove PS3 never executes from `mmapper` regions and assert |
| SPU MFC DMA | yes | the DMA helper validates the 4 KB map |
| RSX/DMA transfers | yes | the RSX access-violation handler already exists; add the sub-page test |
| `mmapper` own copies | yes | validate and update state |

The first experiment is **not** to build the general map. It is to instrument `_page_map` and reproduce `flags=0x1020a` on a 64 KB region (§7.2 A1). If the 64 KB path is the whole bug, 64 KB is four 16 KB pages and holes are expressible; the fix is much smaller. The general map ships only if the 64 KB fix does not cover the title.

#### 8.1.4 Per-page code invalidation and code-space reuse

Today, invalidation is module-level. ASNR adds a page index:

```cpp
struct module_code_index
{
    std::shared_mutex lock;
    std::vector<code_block> blocks;                          // sorted by host_start
    std::unordered_map<u32, std::vector<u32>> guest_page_map; // 4K guest page -> block ids
    std::vector<u8*> free_space;                            // reusable code ranges
};
```

Triggers:

| Event | Today | ASNR |
|---|---|---|
| PRX unload | destroy whole module | unchanged (correct) |
| Overlay replaced | destroy whole module | unchanged initially; page invalidation later |
| Guest writes to a page containing code | not detected for JIT code | invalidate blocks covering that page |
| `sys_mmapper` unmap of a notification region | module-wide? | invalidate affected pages |
| Fastmem site patched | n/a | recompile one block, reuse its space |
| Cache version bump | full recompile | unchanged (correct, one-time) |

Invalidation procedure:

1. Look up `guest_page_map[page]`; collect block ids.
2. For each block, patch its entry (the first instruction) to `b <stale_stub>` and clear the corresponding `g_exec_addr` entries so an indirect branch cannot enter the middle.
3. Bump the block's `invalidation_epoch`; an in-flight thread already inside the block finishes it, exits through the stale path, and recompiles.
4. Return the host range to `free_space` **after** a quiescence point. Do not unmap; do not reuse immediately.
5. Recompile lazily on the next entry.

Why "patch the entry, don't unmap":

- Another thread may be executing the block. Unmapping its code is a guaranteed crash.
- A `b` to a stale stub is 4 bytes, atomic at instruction granularity on AArch64, and safe: a thread sees either the old instruction or the branch, both valid.
- Code-space reuse avoids the JIT-cache-flush stutter Dolphin eliminated. Keep a per-module free list of fixed-size chunks; reuse only chunks whose epoch is older than all active blocks.

The exec table is part of the contract. `g_exec_addr` maps guest address → host function pointer; an invalidated block must not be reachable through it. On invalidation, write the module's `compile_on_entry` stub into every covered exec entry, so invalidation becomes a lazy recompile instead of a correctness hazard.

#### 8.1.5 Decommit, reset, and the `munmap` race

Replace the Apple `munmap`/`mmap` pair with protection changes, as measured in Appendix A.7:

```cpp
// decommit (Apple)
mprotect(base, size, PROT_NONE);
madvise(base, size, MADV_FREE_REUSABLE);

// commit / reset
mprotect(base, size, prot);
madvise(base, size, MADV_WILLNEED);
```

Measured, 64 × 1 MB touched regions:

| Strategy | Decommit | Footprint after | Recommit + touch | Race-free |
|---|---:|---:|---:|---|
| `munmap` + `mmap(PROT_NONE)` (today) | 285 µs | 1.0 MB | 3.8 ms | **no** |
| `mprotect(PROT_NONE)` | 130 µs | 1.0 MB | 2.5 ms | **yes** |
| `madvise(MADV_FREE_REUSABLE)` | 207 µs | 1.0 MB | 2.1 ms | **yes** |

All three return `phys_footprint` to the pre-allocation baseline. There is no memory-footprint argument for keeping the unsafe path.

JIT code ranges follow the same rule with one addition: never `munmap` a `MAP_JIT` range while any thread can execute it. Reset via `mprotect(PROT_NONE)` + `madvise`, keep the reservation, and reuse the space through the free list. `memory_reset` becomes `decommit` + `commit`.

Guard rails:

- A debug assertion that `mprotect`/`madvise` return 0; the current code `ensure`s the `mmap` result equals the hint, which is the wrong invariant.
- A startup check that the address is still inside a `mach_vm_region` with the expected protection after reset.
- A soak test that runs 30 minutes of guest churn and samples `vmmap` every minute; the microbenchmark's "returns to baseline" claim must hold in situ.

#### 8.1.6 Atomics, fences, and the RSX retry storm

Already fixed and must not regress:

- LSE2 16-byte loads/stores: `ldp` + `dmb ishld`, `dmb ish` + `stp` (Appendix A.3).
- 16-byte CAS: `casp`/`caspal` (`atomic.hpp:1111,1170`).
- Runtime selection via `utils::g_atomic_lse2`.

Still to do:

1. **Fence strength.** Audit `atomic_fence_acquire/release/seq_cst` and the 16-byte `store` path. A relaxed 16-byte store on LSE2 is `stp`; only release needs a fence. The current pattern can emit `dmb ish; stp; dmb ish` for a relaxed store, two unnecessary fences per store on the RSX queue.
2. **`observe()`.** It is `load()` with a TODO. Use a relaxed `ldp` where the caller does not need acquire semantics, and document which callers do.
3. **`exchange` fallback.** Ensure it uses `casp`/`swpal` when LSE is available, not only when LSE2 is.
4. **Sub-16-byte RMW.** Prefer `ldadd`/`swpal`/`casal` for simple operations; check what LLVM already emits before hand-writing.
5. **Retry-storm instrumentation.** Count LL/SC retries and `casp` failures per queue. #19759's 13–30 FPS with MT RSX should show up as a retry count; if it does not, the hypothesis is wrong and the fix is elsewhere.
6. **`atomic_wait_engine` on Apple.** #18429 is an ASan stack-buffer-underflow. Review waiter-storage lifetime and alignment under 16 KB stacks, and add a red-zone test.

### 8.2 Dispatch v2

Dispatch is the second-largest lever after state shape, and the two are coupled: you cannot chain blocks without deciding where state lives across the edge.

#### 8.2.1 Regions instead of blocks

The unit of translation changes from "one function per branch target" to "one **region** per entry, containing as many directly connected blocks as fit a budget." A region:

- has exactly one entry (the guest address the exec table or a caller targets);
- contains real CFG edges: conditional branches are `br`, fall-through is fall-through;
- may contain loops whose back-edge targets a block inside the region;
- exits through the GHC ABI only at region boundaries: indirect branches, calls to unknown targets, syscalls, faults, and budget exhaustion;
- materialises guest state to `ppu_thread` **only at exits**.

**Measured region shape.** Before designing the formation walk, the corpus was measured. Details and raw output are in Appendix A.12; the numbers that drive the design are:

| Measure | Demon's Souls | Ridge Racer 7 |
|---|---:|---:|
| Basic block, median / mean / p90 | 4 / 5.55 / 12 | 4 / 5.63 / 11 |
| Current inline chain (unconditional `b` only), median | 4 | 5 |
| Region, "any branch target is an entry" rule, median | 6 | 6 |
| Region, ideal multi-predecessor rule, median / p90 | 10 / 32 | 11 / 41 |
| Region ≥40 instructions, ideal rule | 6.6% | 10.6% |
| Loop-header region, median / p90 | 13 / 63 | 14 / 79 |
| Loop regions ≥40 instructions | 19.8% | 21.8% |
| Instructions inside some loop region | 55.5% | 54.6% |
| Blocks with >1 predecessor | 8.1% | 7.4% |
| Blocks targeted by a direct branch | 49.4% | 48.9% |

Two conclusions follow, and they change the design rather than merely decorate it:

1. **A straight-line region is too short to amortise anything.** The median is 6 instructions under a strict "any branch target is an entry" rule, and 10–11 under a multi-predecessor rule. The per-region fixed cost — entry check, GHC argument moves, live-in reload, live-out store — cannot disappear at that length. Region IR that walks forward from an arbitrary entry is not worth building.
2. **The only long unit is a loop.** The median back-edge spans 98–108 guest instructions, 20–22% of loop-header regions absorb at least 40 instructions, and those regions cover roughly 55% of the static text. Execution time in every emulator is concentrated in loops; this is the unit to compile as a region.

Therefore formation is **loop-first**, and it is the same two-pass frontend:

```text
pass 1 (cheap scan of decoded instructions):
    for every direct branch whose target < its own address:
        record target as a loop-header candidate
        record the back-edge address

pass 2 (build regions, loop headers first, then the remaining entries):
    for each loop header, in descending order of loop span:
        region = walk(header, limit = envelope of its back edges)
    for each remaining entry (exec table / symbol / indirect target):
        region = walk(entry, limit = budget)

walk(seed, limit):
    worklist = [seed]; region = {}
    while worklist not empty and budget not exhausted:
        bb = pop worklist
        if bb in region: continue
        if bb is an indirect branch, syscall, or unknown target: mark as exit; continue
        add bb to region
        for each direct successor s:
            if s in region: record an edge (back-edge if it targets an earlier block)
            else if s is inside limit and s is not an entry: push s
            else: mark edge as exit
cap: 256 guest instructions or 8 KB of IR per region (tunable)
```

**What "entry" means, and why the old rule was wrong.** The previous text said any block targeted by a branch is an entry, which is why the measured region median collapsed to 6. That rule is unnecessarily strong. A block needs its own exec-table entry only if it can be reached from **outside** the region being built. The frontend can compute this statically over the decoded module:

- compute in-degree for every block from all direct branches and fall-through edges;
- a block with **more than one predecessor** is an entry;
- a block with **zero known predecessors** is an entry (return target, jump-table case, indirect target, or data-in-text);
- a block whose single predecessor is outside the candidate region is an entry;
- a symbol start is an entry;
- anything the frontend cannot classify is an entry.

A single-predecessor block whose only predecessor is inside the region has exactly one host copy by construction, so absorbing it duplicates nothing. That relaxation is what moves the median from 6 to 10–11. It is still conservative: if self-modifying code later creates a new branch into an absorbed block, the page invalidation from §8.1.4 discards the whole region and the next formation sees the new in-degree.

**Regions shorter than the threshold stay as linked blocks.** Formation computes the region and then compares it with `region_min_instructions` (default 32). Below the threshold, emit the existing per-block function plus a link area (§8.2.2) instead of a region. This is not a fallback for correctness; it is a decision that the fixed cost is cheaper than the region machinery for a 4-instruction block. It also makes the design robust to the measurement: if dynamic profiling later shows that short regions dominate, lowering the threshold is a tuning change, not a rewrite.

**Loops are the target, but they are not unconditional.** A loop header region is only formed when the loop has a single dominant entry and its body does not contain an irreducible join. The scan can see both: a back edge gives the header, and the in-degree gives the joins. If the body is irreducible or has multiple external entries, fall back to linked blocks for that loop.

A small direct successor is absorbed into the region rather than called; that is the "small-block inlining" in §0, and it falls out of formation rather than being a separate optimisation.

**Entry registration is the constraint that keeps this correct.** A region formed by walking forward from one entry can absorb a block that is *also* reachable from outside the region — a function's second basic block targeted by a `b` from another function, a return target, a jump-table case. If that address has no exec-table entry, a later caller compiles a second copy of the same guest code. That is not a wrong answer, but it is duplicated code, duplicated invalidation, and two different host implementations of one guest address.

Rules:

- Region formation **does not absorb an entry block other than its own start**. It stops the walk at that block and marks the edge as an exit. The cost is a smaller region; the benefit is one host implementation per guest entry.
- Every entry block gets a normal exec-table entry and a normal state check. Internal blocks do not.
- The page index (§8.1.4) records every block, entry or internal, so invalidation of a page still covers the whole region.

The alternative — register a per-block entry stub into the middle of a region, so an external branch can join the shared code — is faster in theory and much harder to make correct: the stub must materialise the GHC arguments and the SSA state expected at that point. Do not build it in v1. If profiling later shows that regions are being cut too small by conservative entry detection, relax the detector (e.g., use the symbol table plus a two-pass reachability analysis) before adding mid-region entries.

Budget rationale: a region is compiled once and lives in the JIT cache. A 256-instruction cap keeps compile time and code size bounded while capturing the common case (a loop body with a handful of branches). The cap is a measured parameter; start at 128 and tune on the corpus. The measured loop p90 is 63–79 instructions, so 128 covers most loops and the cap only binds on the long tail.

The PPU frontend already has the control-flow information (`ppu_itype`, `BC`, `B`, `BCTR`, `BCCTR`, `SC`). Region formation is a frontend pass over the same decoded instruction stream; it does not require a new decoder.

#### 8.2.2 Direct chaining and the link area

For a direct branch whose target is **outside** the region, emit a link area rather than an unconditional tail call:

```asm
; link area, 4 instructions (16 bytes), patched per target
adrp x16, target_cell@page
ldr  x16, [x16, target_cell@off]   ; host pointer, 0 = unresolved
cbz  x16, resolver
br   x16
```

`target_cell` is a per-site data word in the module's writable data area. When the target region is compiled, the compiler writes its host entry into the cell. When the target is invalidated, the cell is cleared, so the branch falls into the resolver and recompiles.

Why a cell and not a direct `b` patched at compile time:

- The target may not be compiled yet (lazy per-entry compilation).
- The target may be invalidated and recompiled at a different address.
- A `b` patched in place needs ±128 MB range management and W^X write access to code; a data cell needs neither.
- The cell load is a single L1 hit and is predictable; a direct `b` is faster still, so the design allows a **second stage**: after a cell has resolved N times and the target has not moved, rewrite the link area to a direct `b` under W^X. Invalidate back to the cell form when the target moves. This is the same two-stage idea as Dolphin's link area and Dynarmic's block linking.

The current tail call has a fixed cost the link area removes: the callee's entry check, the seven GHC argument moves, and the register reload. The link area jumps directly to the callee's body past the prologue, with the callee's live-in registers already in place from the region's SSA state. That is the actual win, not the branch itself.

Safety: a direct link may only skip the entry check if the callee does not need it. Conservatively, link to the callee's **body** only when the callee is in the same module and its entry check is proven redundant (no syscall/fault between). Otherwise link to the entry. The design starts with entry linking (safe) and adds body linking behind a flag with tests.

#### 8.2.3 Indirect branch target cache

Replace the dependent load from `g_exec_addr` with an inline cache at each indirect site:

```asm
; monomorphic IBTC
adrp x16, ibtc_cell@page
ldr  x17, [x16]                ; cached guest target
cmp  x_target, x17
b.ne miss
ldr  x16, [x16, #8]            ; cached host entry
br   x16
miss:
  ; call resolver(guest_target, site); resolver updates the cell
```

Cell layout:

```cpp
struct ibtc_entry
{
    u32 guest_target;   // 0 = empty
    u32 guest_epoch;    // bumped on any invalidation that could affect the target
    u8* host_entry;
};
```

The resolver:

1. Looks up `g_exec_addr` for the guest target.
2. If the entry is a compile stub, compiles the region, then re-reads.
3. Validates the target's page is still executable.
4. Writes the cell if the site is monomorphic within a window; otherwise leaves it in "megamorphic" mode and always calls the resolver, or switches to a 2-way cell if profiling shows polymorphism.

Start monomorphic. PS3 indirect calls (`bctrl`, virtual dispatch) are often monomorphic at a site; returns are handled by the RSB. A 2-way cell is a follow-up gated on a miss-rate counter.

Cost: one extra load and compare on the hit path versus today's single dependent load. The win is that the compare is register-only and the branch target is known after the compare, so the predictor can learn it; today's load-to-branch is a pointer-chase with no stable predictor input. Measure both; if the monomorphic cell is not faster on the corpus, keep the table and add only the RSB.

#### 8.2.4 Return stack buffer

Guest `bl` pushes a return address; guest `blr` to that address is a return. Maintain a small shadow stack per emulation thread:

```cpp
struct return_stack
{
    u32 guest_pc[16];
    u8* host_entry[16];
    u8  depth;          // 0..16, saturating
};
```

Codegen for `bl`:

```text
rsb[depth] = {guest_pc + 4, host_entry_of(guest_pc + 4)}
depth = min(depth + 1, 15)
tail-call target
```

Codegen for `blr`:

```text
if (depth > 0 && rsb[depth-1].guest_pc == target) {
    depth--
    br rsb[depth].host_entry          // direct, predicted
} else {
    depth = 0                          // underflow: clear, use IBTC
    ibtc_or_table(target)
}
```

The RSB lives in `ppu_thread` (or a per-thread side struct) and is saved/restored with the thread context. It must be cleared on any context switch, syscall, or exception; a stale RSB entry is a correctness bug, not a perf bug. Depth 16 covers real call chains; the saturating behaviour degrades to the IBTC, not to corruption.

#### 8.2.5 Eliding the state check

The entry check is the inline state test in `Translate` (`:241-265`), not `TestAborted`. With regions:

- Check at **region entry** only.
- Check at **backward branches** (loop backedges), where a long-running guest loop can be interrupted.
- Check before and after any call that can block (syscall, MFC, RSX queue).
- Do **not** check at internal forward edges.

This preserves the `cpu_flag::again` / `cpu_flag::exit` contract: the flag is observed no later than one loop iteration or one blocking call, the same guarantee the interpreter provides. Add a test that sets `cpu_flag::exit` from another thread while a guest loop is running and asserts exit within one iteration (CPU-PPU-0xx).

#### 8.2.6 What this does not do

- It does not inline across indirect calls. That needs whole-program knowledge and is not worth the complexity.
- It does not eliminate the GHC ABI at true boundaries. The gateway, far-return, and C++ interop depend on it.
- It does not change the exec table format. The IBTC sits in front of it.

### 8.3 State v2: IR shape

#### 8.3.1 SSA across region edges

Inside a region, `m_locals` values flow through CFG edges as SSA values. At a merge point, insert `phi` nodes. At a region exit, materialise only the **live-out** values:

```text
live_out = values used by any successor region or by the exit helper
for each live_out local:
    store local -> ppu_thread field
```

This replaces the unconditional `FlushRegisters()` before every branch. Two rules keep it correct:

1. **A region exit is a barrier.** Everything live at the exit is stored, because the successor is a separate function and may be entered from anywhere.
2. **A fault site is a barrier.** If a T1 access can fault and the handler may re-enter the guest, the values needed to reconstruct the instruction must be materialised or reconstructible from the site record. The site record names the base register, index register, and width; the handler reads them from the host context, not from `ppu_thread`. This is why the fastmem design must land before non-volatile accesses: the fault contract defines what state is live.

Implementation shape:

- A single `m_locals` array cannot represent a merge: two predecessors may hold different SSA values for the same guest register. Replace it with a **per-block value map** — `std::array<Value*, 32>` (plus CR/VR/LR/CTR) attached to each `BasicBlock` while the region is built. `m_globals` keeps only the struct-field pointers used for materialisation.
- At a merge, for every local that is live on entry, compare the incoming values from each predecessor. If they agree, reuse the value; if they differ, create a `phi` and install it in the block's map. A missing incoming value means the value was never defined on that path — materialise it with a load from `ppu_thread` before the edge, which is exactly what `RegLoad` does today.
- Replace `UseCondition`'s `FlushRegisters()` with that phi-aware merge. The conditional branch becomes a real `br` to two blocks in the same function.
- `CallFunction` remains for exits; it materialises live-out registers first.
- Add a debug assertion that no local is used after a store without a reload, that every merge has one incoming value per predecessor, and that every exit stores all live-out values.

The per-block map is the actual work. `PPUTranslator` currently reads and writes one global array from every helper, so the change touches every `GetGpr`/`SetGpr`/`GetCrb`/`SetCrb` call site or their implementations. That is why §11.2 estimates W5 at 8–12 weeks and why it must be flag-gated and differentially tested rather than reviewed by inspection.

The translator is a large state machine (`PPUTranslator.cpp` is 5,644 lines); this is the riskiest change in the document. It must be developed behind a flag (`ppu_region_ir`) and validated by differential execution against the current translator on the whole test corpus, not just the three games.

#### 8.3.2 Register materialisation budget

The target is not "never store registers"; it is "store each live register once per region exit."

| Metric | Today | Target |
|---|---|---|
| Stores per conditional branch | up to 32 (all dirty GPRs) + 4 per dirty CR field | 0 at internal edges; live-out only at exits |
| Loads per block entry | one per used register | one per live-in at region entry |
| State checks per block | 1 (when `need_check`) | 1 per region entry + 1 per backedge |
| GHC calls per conditional branch | 2 (both successors) | 0 internal; 1 at region exit |

These are compile-time counts, not runtime measurements, and they are testable with the golden-IR harness (§10). A region with no exits except a return should compile to one function with a handful of stores at the end.

#### 8.3.3 CR packing and lazy flags

Guest CR is eight 4-bit fields. Today: four `i8` locals per field, flushed before each branch, reloaded in the successor.

ASNR:

- Represent each CR field as one `i32` (or pack all eight into one `i64`, 8 bits per field, with the low nibble holding the condition bits and the upper bits free for SO/OV).
- Keep the **last compare operands and predicate** as SSA values (`lhs`, `rhs`, `pred`). Do not materialise LT/GT/EQ until a CR bit is read or the region exits.
- A branch on one bit compiles to `cmp` + `b.cc` directly from the operands, with no `cset` and no store.
- A branch on a multi-bit condition (`cror`/`crand`/`crxor` combinations) compiles to `ccmp` chains or a short `csetm`/`and` sequence; the current code goes through `GetCrb` loads.
- At a region exit, store **one 4-byte word per dirty CR field**, not four bytes. Eight dirty fields are 32 bytes in eight stores instead of 32 byte-stores.

Measured baseline (Appendix A.8): full-field materialisation is `cmp` + 3×`cset` + 3×`strb`; single-bit use already folds to `cmp` + `csel`/`b.cc`. The saving is concentrated in the full-field paths and the pre-branch flush, which is exactly where `UseCondition` forces it. The upper bound is 1–4% of PPU-bound time; the lower bound is under 1%. Build the instruction-count harness before promising either.

Three LLVM knobs interact with this and are checked once, with the resolved value logged:

- `-aarch64-enable-ccmp` must be on; it is what turns a two-bit guest predicate into a `ccmp` pair instead of two independent `cset`s.
- Store-pair formation is legal on M4 (`FeatureStorePairSuppress` is removed by `TuneAppleM4`), but a `ldp` immediately followed by a `str` carries a 2× penalty (LLVM #235875). Verify the CR store sequence does not land in that adjacency; if it does, prefer two `str`s or reorder.
- Post-RA scheduling (`-misched-postra`) is tested, not assumed. If it does not improve the corpus, it is left at the default rather than tuned to a model that does not exist (§3.4).

Knob changes are part of a PR only when they move a golden-assembly test or a corpus measurement. A flag that "might help" and cannot be measured is left alone.

#### 8.3.4 Masked and unaligned stores

`STVLX`/`STVRX` currently use `CreateMaskedStore`, which lowers to 31 branches + 16 stores (D6). Replace with a merge-store:

```asm
; v = value, m = byte mask (from the existing shl/cmlt pair)
ldr   q2, [x0]          ; aligned 16-byte load
bif   v.16b, v2.16b, m.16b
str   q0, [x0]          ; aligned 16-byte store
```

6 instructions, 0 branches, measured. The pattern applies to PPU unaligned scalar and vector tails, and to SPU shuffle code that currently branches per byte. Keep the generic intrinsic as a correctness fallback when the base is not 16-byte aligned and the read-modify-write would cross a page boundary; the translator must prove alignment or use the safe path.

This is a code-quality fix, not a corpus win: the local titles contain **zero** `LVLX`/`LVRX`/`STVLX`/`STVRX` (Appendix A.5). Ship it with a golden-assembly test; do not put it in a performance table.

#### 8.3.5 The volatile/MMIO contract and the SPU fallback

The non-volatile change (D12) needs a precise contract:

- **Ordinary RAM** (guest pages with `page_readable | page_writable`, not MMIO, not a notification region): non-volatile, may be merged within a region, subject to guest fences.
- **MMIO** (`m_may_be_mmio`, RSX-mapped, device ranges): volatile, never merged, never reordered across a fence.
- **Notification regions**: non-volatile for mapped sub-pages, but the sub-page check is a side-effecting call; LLVM must not sink or duplicate it. Mark the call `convergent`/`nounwind` as appropriate, or use an `llvm.sideeffect` barrier.
- **Atomics**: use the existing atomic intrinsics; do not model them as volatile loads/stores.

The SPU side has a separate constraint: `TBL2` and the `g_spu_llvm_compile_context` retry path can fail with "Cannot scavenge register without an emergency spill slot" (#19801). The design does not guess at the fix. It requires a reduced IR repro from a live compile, then a targeted change (likely a reserved scratch register or a spill-slot reservation in the GHC frame pass). Track it as correctness, not perf.

#### 8.3.6 IR shape tests

Every change in §8.3 is testable without running a game:

- **Golden IR**: a fixed guest input produces a canonical LLVM IR dump; assert instruction counts for stores, loads, branches, and CR materialisations.
- **Golden assembly**: `llc -mcpu=apple-m4` output for a synthetic region; assert no `cset` in a one-bit branch, one `stp`/`str` per dirty CR field, and a merge-store for `STVLX`.
- **Differential**: run the same guest code through the old and new translators, compare architectural state after N instructions.
- **Fault injection**: every T1 site is faulted deliberately; assert the handler reconstructs state and the retry produces the same result as an unfaulted run.

### 8.4 Frames v2: gateway scratch and the GHC contract

#### 8.4.1 The mechanism

Under the GHC calling convention, LLVM treats the stack as caller-owned. It emits **no prologue `sub sp`** and writes spills and allocas at positive offsets from whatever `sp` the caller provided. Verified with `llc -mtriple=aarch64-unknown-linux-android -mcpu=apple-m4 -O2` (Appendix A.10):

```asm
big:
    mov  w8, #1
    str  w8, [sp]           ; no prologue, no sub sp
    mov  w8, #2
    str  w8, [sp, #11996]   ; a 12 KB alloca, written above sp
    mov  x0, sp
    bl   g
    ret
```

A 1.2 KB alloca behaves the same: `str w8, [sp, #28]`, no `sub sp`.

RPCS3 enters compiled code through two gateways with a fixed slab in the current tree:

| Gateway | Slab | Location |
|---|---:|---|
| SPU | 8192 | `SPUCommonRecompiler.cpp:513`, released at `:609` |
| PPU | 8192 | `PPUThread.cpp:380` |

A function whose frame exceeds 8192 writes past the slab into the gateway's saved thread pointer and host frames. That is #19939: a 9.8 KB frame in Prototype 2 and a 10.7 KB frame in Max Payne 3.

The mechanism is settled. The remaining question is the cheapest correct fix.

#### 8.4.2 Options

| Option | Change | Pros | Cons | Verdict |
|---|---|---|---|---|
| **A. Enlarge the slab** | 8192 → 65536 in both gateways | one line; no recompile; covers every frame seen in the issue reports; cannot break the `sp` contract | still no proof for >64 KB; hides the exact size; 64 KB of an 8 MB thread stack | **ship first** |
| **B. `.stack_sizes` + thunk** | compile once, read frame sizes, recompile only oversized functions with a thunk that lowers `sp` | exact; zero cost for normal functions; upstreamable | two-pass compile; depends on an LLVM-internal ELF section; requires a gateway `sp`-reload fix (§8.4.3) | **target, after A** |
| **C. MachineFunction pass** | insert a real prologue/epilogue only when spills exceed the slab | most correct long-term; one pass | complex; interacts with GHC tail calls and far-return unwinding; high risk | v2 |
| **D. Always emit a frame** | add `sub sp` to every function | simple concept | overhead on every function; LLVM may move spills across inline asm | reject |

The ordering is deliberate. Option A is a correctness fix that lands in days and cannot regress the calling convention. Option B is the exact solution, but it changes the stack discipline of every oversized function, and an adversarial read of the two gateways (below) shows that the current epilogues are not written for a permanently lowered `sp`. Do not ship B before the gateway fix, and do not make the crash fix wait for B.

#### 8.4.3 Recommended design: A now, B as the exact follow-up

**Step 1 — enlarge the slab (P1, immediately).**

- Both gateways reserve 65536 bytes instead of 8192: `PPUThread.cpp:380`, `SPUCommonRecompiler.cpp:513`.
- A debug-only frame histogram decodes `.stack_sizes` and logs any function above 48 KB, so the next overflow is visible before it corrupts memory.
- No other change. This covers the 9.8 KB Prototype 2 frame and the 10.7 KB Max Payne 3 frame with a 6× margin.

**Step 2 — exact frames (P1, after the gateway fix).**

**Data flow**

1. The SPU/PPU recompiler constructs its `jit_compiler` with a flag meaning "emit stack-size section."
2. `jit_compiler` sets `llvm::TargetOptions::EmitStackSizeSection = true` and `ExecutionEngine::setProcessAllSections(true)`.
3. `MemoryManager1/2::allocateDataSection` records `.stack_sizes`; `finalizeMemory` decodes it into `(function address, frame size)` pairs.
4. After `getPointerToFunction(f)`, look up the address. If `frame_size > 48 KB`, record `{function_name → frame_size}` in the compile context and fail the compile.
5. The caller sees the override set grow, preserves it across the retry, and recompiles once.
6. On the second compile, `GHC_frame_preservation_pass::inject_frame_thunk` runs for each overridden function:
   - move the original body to `<name>-framed`;
   - emit a thunk that subtracts `align16(frame_size) + 16` from the current `sp` and tail-calls the body.

**The `sp`-restore contract is not optional.** The current gateways unwind relatively, and only one of them then restores `sp` absolutely:

| Gateway | Prologue | Epilogue | Safe if `sp` stays lowered? |
|---|---|---|---|
| PPU (`PPUThread.cpp:300-410`) | saves `sp` into `hv_ctx`, `sub sp,#16`, `str x20,[sp]`, `sub sp,#8192` | `add sp,#8192`; `ldr x20,[sp]`; `add sp,#16`; restore context; `mov sp,x15`; `ret x30` | **by luck** — the relative add overshoots and the `x20` load reads outside the slab, but `x20` and `sp` are overwritten from `hv_ctx` afterwards |
| SPU (`SPUCommonRecompiler.cpp:505-630`) | `sub sp,#16`, `str thread,[sp]`, `sub sp,#8192` | `add sp,#8192`; `ldr x19,[sp]`; `add sp,#16`; `ldr x30,[x19,#...]`; `ret` | **no** — `x19` (the thread pointer) is reloaded from the overshot address and then used to fetch the return address; the SPU gateway never restores `sp` or `x19` from an absolute slot |

So a thunk that leaves `sp` lowered corrupts the SPU return path. Two fixes are possible; take the second:

- *Thunk restores `sp`* on every path that can reach an epilogue. In a tail-call-only GHC model there is no return point, so this means a continuation and a real return — which is the thing the design is trying to avoid.
- *Gateway reloads `sp` absolutely* before the relative arithmetic. PPU: move `mov sp, x15` before `add sp,#8192`, or delete the add and the `[sp]` load entirely (`x20` comes from `hv_ctx` anyway). SPU: store the pre-slab `sp` (and the thread pointer) in `spu_thread` or a fixed TLS slot, and restore both from there. Two instructions per gateway entry, and a future thunk cannot forget them.

This is a prerequisite PR to the thunk, not part of it. Test: enter a thunked SPU function with a 40 KB frame, force the gateway epilogue, and assert `sp`, `x19`, and `x30` against the pre-entry values. Then repeat with a function that escapes through the far-return path.

**Why the section works here.** `.stack_sizes` is emitted for ELF only. The JIT triple is `aarch64-unknown-linux-android` (`Utilities/JITLLVM.cpp:656,673`), so MCJIT sees ELF and the section exists. Mach-O emits nothing. This is an Apple-on-ELF dependency and must be documented as such.

**Decoder requirements** (verified end-to-end; see Appendix A.2 for the multi-function experiment):

- Decode **after** relocation. In the raw object the address field is an `R_AARCH64_ABS64` relocation target; `allocateDataSection` sees the relocated payload.
- Records are **packed back-to-back**, not 8-byte aligned: advance by `8 + ULEB_length`. An aligned decoder misparses a multi-function section.
- The address is the function's code entry, and it matches `getFunctionAddress()` exactly.
- Stop at the section end; ignore `addr == 0`; tolerate a truncated tail. A 30-byte single-function section is 10 bytes of record plus 20 bytes of zero padding, and a 3-function section is 29 bytes plus 60 bytes; the tail is padding, not phantom functions.
- The section covers **every function in the module**, not just the entry point. Map all addresses; dedupe after thunk/alias creation.
- Section presence is a build property. If the triple or flag changes, the section disappears. Check explicitly, log once, fall back to the slab. Never assert.
- Cap the value. Anything above `max_frame_size` (1 MB) is a compiler or decode bug; log and fall back rather than `sub sp` with it.

**Budgets**

- `slab_size = 65536` after step 1. In Appendix C, FRAME-005/006/007 state the 8 KB boundary from the original slab; read them as "the configured slab size", with the boundary parameterised so they still test the right comparison after the resize.
- `max_frame_size = 1 MB`.
- Debug builds assert `frame_size <= max_frame_size` and log anything above 48 KB.
- Option B's thunk is only applied above 48 KB, so normal functions pay nothing and the slab absorbs the rest.

**PPU.** Apply the same mechanism, but note that the PPU gateway is the one whose epilogue happens to survive a lowered `sp`; do not use that as the reason to skip the gateway fix. A future change to the `[sp]` load would turn the luck into a crash.

**What this does not fix.** A frame larger than 1 MB, or a function whose spills are written below `sp` by a pattern LLVM does not report in `.stack_sizes`, still corrupts memory. The histogram is the detector; the cap is the backstop.

#### 8.4.4 Why two passes are acceptable

- Only modules containing an oversized function pay the second compile.
- The first compile still produces `.stack_sizes`; nothing is cached until the second succeeds.
- The retry is deterministic and bounded: one extra compile per affected module.
- Cache files are written only after success, so users never see a half-cached module.

Alternative if two passes are too slow: emit the thunk for every GHC function whose **estimated** frame is >4 KB from IR size heuristics, then verify with `.stack_sizes` in debug. This trades a cheap IR walk for a compile. `[U]` measure both on cold-cache DS/RR7.

#### 8.4.5 Tests

- ULEB128 decoder, section parsing, address lookup, alignment.
- Thunk IR shape: one `sub`, one `mov sp`, one tail call, no `add sp`.
- No duplicate thunks for aliased functions.
- Golden module with a 12 KB vector spill produces a thunk and a `-framed` body.
- Modules without oversized functions compile once (counter).
- A synthetic 2 MB frame is rejected, logged, and falls back.
- 3-deep call chain with only the middle function oversized; ASan + canary; far-return path.
- PPU variant with large vector spills.

### 8.5 Runtime v2

#### 8.5.1 JIT cache, codegen reuse, and PGO

**Cache key discipline.** The PPU object name is `v9-kusa-<hash>-<settings>-<cpu>` (`PPUThread.cpp:5816`). Any IR change in ASNR invalidates the meaning of cached machine code. The first PR that changes IR bumps `v9` to `v10`; every subsequent IR change bumps again or is co-landed with the previous bump. A stale object is a silent correctness bug: the emulator runs old code with new semantics.

**What to persist.** The on-disk object is machine code. Also persist a small sidecar:

```text
<obj>.meta
  codegen_version: u32
  region_count: u32
  site_table_offset: u32     // layout only, not host addresses
  frame_sizes: [(u32 guest_addr, u32 frame_size)]
  block_length_histogram: [u16 x 8]
```

Host addresses are not stable across runs; do not persist them. The sidecar lets a new process pre-size its tables and skip the `.stack_sizes` decode for cached modules, and it gives the instrumentation a baseline.

**Code-space reuse.** With per-page invalidation (§8.1.4), a recompiled block needs a home:

- Allocate code in fixed 64 KB chunks per module.
- A free list holds chunks whose `invalidation_epoch` is older than the minimum active epoch.
- Reuse never unmaps; it re-protects and rewrites in place under W^X.
- A chunk returns to the OS only at module unload.

**PGO.** Two separate things, often conflated:

1. **Host PGO.** Build RPCS3 with `-fprofile-generate`, run DS/RR7/Mega Man 9 for a fixed script, then rebuild with `-fprofile-use`. This improves the emulator's own C++ (signal handler, VM, queues, dispatcher). Cheap, safe, measurable, and it does not change JIT output. Ship it as a CI build variant first.
2. **JIT-guided codegen.** Persist per-site counters (indirect monomorphism, branch bias, block execution counts) in the sidecar and use them to guide region formation and IBTC sizing on the second run. A real optimisation, but larger; do it only after the instrumentation exists and the corpus shows stable patterns.

Do **not** ship a profile trained on one title as a universal default. ARMSX2 trains on a handful of games; that is reasonable, but the profile must be validated outside the training set or it is overfitting.

#### 8.5.2 Compile-time pipeline

PPSSPP's block-compile work (PR #23049) is the template: the wins were mostly **not codegen**.

- read the cache-line size once, not per icache flush;
- time blocks by phase only when profiling;
- skip IR passes a block has nothing for;
- avoid clearing 1 KB of constants per block;
- keep block exits/links in flat vectors, not `unordered_multimap`;
- avoid copying the register cache on the bad-address path;
- log an IR/code hash to prove codegen did not change.

Apply the same audit to `PPUThread.cpp`/`SPULLVMRecompiler.cpp`:

| Item | What to measure | Target |
|---|---|---|
| Per-phase timers | parse, IR build, optimisation, codegen, relocation, icache | identify the top phase |
| Cache-line size | `sysctlbyname("hw.cachelinesize")` once per process | remove per-flush syscalls |
| Pass pipeline | IR passes that run on every module regardless of content | skip when provably empty |
| Constant tables | per-block clearing/copying | reuse |
| Exit/link containers | `unordered_multimap` in the hot compile path | flat vectors |
| `.stack_sizes` | decode cost, two-pass count | bounded, logged |
| Hash logging | IR/code hash per module | prove codegen unchanged |

Compile time is gameplay-visible: the overlay-load hitch and the first-boot stall. Treat a 20% compile-time reduction as a real win even with zero FPS change.

#### 8.5.3 Threading, QoS, and parking

The measured facts (Appendix A.1, A.6):

- The RPCS3 attribute sequence leaves QoS `UNSPECIFIED(0)`, `SCHED_RR`, priority 99.
- `pthread_setschedparam` after QoS unsets QoS and drops effective priority.
- `SCHED_RR`/99 beats `SCHED_OTHER` and QoS by ~55% in an 8-thread synthetic; absolute throughput varies ~20% between runs, so only within-run ratios are valid.
- `wfe` does not park on macOS 27.2/A18 Pro; it burns a core.
- `os_sync_wait_on_address` parks at 0 CPU with p50 9 µs / p99 150 µs wake.

Design:

1. **Stop destroying QoS (D1/D2).** On Apple, `set_native_priority` must not call `pthread_setschedparam` after a QoS class has been set. Either set QoS only, or set `SCHED_RR` only, never both. Default stays `SCHED_RR`/99 behind a config flag until a real-title p99 A/B says otherwise.
2. **Bounded spin then park.** Keep `isb` for a short window (tens of microseconds), then `os_sync_wait_on_address`. Do not build a WFE spinlock (D10). RPCS3 already has this primitive in `Utilities/sync.h:90`; reuse it and delete ad-hoc spin loops.
3. **Affinity.** Either assign deliberate P/E tags or delete the misleading `countr_zero` path (D3). The API is advisory; a wrong hint is worse than none.
4. **Metrics.** The policy decision needs p99 frame time, audio underruns, and input latency, not average FPS. Add them to the harness before changing the default.
5. **Power.** `SCHED_RR`/99 on every emulation thread may raise package power and fan noise. Measure energy on a fanless machine before shipping it as the only mode.

#### 8.5.4 Instrumentation and observability

The current logs are ad hoc. ASNR defines a small always-on counter set and an opt-in detailed mode.

Always on (atomic increments, negligible cost):

| Counter | Purpose |
|---|---|
| `interpreter_fallbacks{ppu,spu}` | prove a title is not silently interpreting |
| `faults{ppu,spu,by_page}` | fault rate, hot pages |
| `patched_sites` | how often fastmem patches |
| `block_compiles`, `block_invalidations` | recompile storms |
| `frame_overrides` | oversized functions seen |
| `llsc_retries`, `casp_failures` | RSX/atomic contention |
| `jit_compile_ns` (per module) | compile-time regressions |

Opt-in (`--cpu-stats`):

- block-length histogram (guest instructions per region);
- host instructions per guest instruction;
- fault-site histogram (guest page, count, patched);
- frame-size histogram from `.stack_sizes`;
- IBTC hit/miss, RSB hit/miss;
- per-phase compile timers;
- `os_signpost` intervals around PPU/SPU gateways, RSX submit, and compile.

The first launch after enabling stats prints one summary line. That line is the artifact every optimization PR attaches.

### 8.6 Platform integration: W^X, entitlements, and icache

#### 8.6.1 The entitlement matrix

Apple exposes two different executable-memory mechanisms, and they are not interchangeable:

| Mechanism | API | Entitlement | Notes |
|---|---|---|---|
| `MAP_JIT` | `mmap(..., MAP_JIT, ...)` + `pthread_jit_write_protect_np` | `com.apple.security.cs.allow-jit` | per-thread W^X; hardware toggle on macOS 26+ |
| Anonymous `mprotect(PROT_EXEC)` | `mmap(RW)` then `mprotect(RX)` | `com.apple.security.cs.allow-unsigned-executable-memory` | blocked under Hardened Runtime without the entitlement on macOS 26 |

RPCS3 uses `MAP_JIT` in `vm_native.cpp` and the JIT allocators, and relies on the toggle. That is the right choice. The missing pieces:

1. An explicit entitlements file in the repository with `allow-jit` and (if any path still uses `mprotect(PROT_EXEC)` on anonymous pages) `allow-unsigned-executable-memory`.
2. A CI job that signs a **Hardened Runtime** build with a Developer ID identity (or an ad-hoc identity plus `--options runtime`) and runs a minimal JIT smoke test. The current `codesign --deep -fs -` does not test the failure mode.
3. A startup check: if `pthread_jit_write_protect_supported_np()` is false, log and refuse to use MAP_JIT paths that require the toggle.

The PPSSPP/OpenEmu-Silicon macOS 26 failure is the cautionary tale: `mprotect(PROT_EXEC)` silently failed under Hardened Runtime, the emulator stalled waiting for a VBLANK, and the symptom was a black screen, not an error. RPCS3 must not rely on "it works in a debug build."

#### 8.6.2 RAII W^X with batching

Replace ad-hoc toggles with one guard:

```cpp
class jit_write_scope
{
    static thread_local u32 depth;
public:
    jit_write_scope()
    {
        if (depth++ == 0)
        {
            pthread_jit_write_protect_np(false);
            atomic_fence_acq_rel();
        }
    }
    ~jit_write_scope()
    {
        if (--depth == 0)
        {
            atomic_fence_seq_cst();
            pthread_jit_write_protect_np(true);
        }
    }
};
```

Rules:

- One scope around a whole module compile, not one per function.
- A scope around each fastmem patch, with an icache flush before release.
- Never hold the scope across a call that can block or throw.
- Assert `depth == 0` at thread exit.

#### 8.6.3 Instruction cache publication

AArch64 instruction and data caches are not coherent. Every path that writes code must publish:

1. Write the bytes (under the W^X scope).
2. `sys_icache_invalidate(start, len)` (or `__builtin___clear_cache`) on the **cache-line-aligned** range.
3. Release the scope.

Audit every path: LLVM `RTDyldMemoryManager::finalizeMemory`, `JITASM` runtime builders, `PPUThread` module load, `SPULLVMRecompiler` load, fastmem patching, exec-table updates (data only, no icache needed). Add a debug mode that fills freed code with `brk #0` and asserts no execution after invalidation.

#### 8.6.4 Mapping policy

- `MAP_JIT` only for code, never for guest RAM (already true; keep the test).
- Guest RAM: `MAP_PRIVATE | MAP_ANON`, `PROT_NONE` until committed.
- No `MAP_FIXED` with `MAP_JIT` on macOS: the comment in `vm_native.cpp:348-350` is correct, and it is why the `munmap`/`mmap` pair exists. ASNR removes the need for the pair rather than fighting the restriction.
- Large reservations: one contiguous guest reservation, sub-ranges protected/unprotected. Do not fragment.

### 8.7 SIMD lowering and SPU shuffle code

ASNR is a CPU/memory runtime, not a vector-codegen rewrite, but the SPU and VMX paths share the same translator and the same NEON helpers. Three facts decide how much of this belongs in scope.

**What already works, and must not be disturbed:**

- `m_use_dotprod` and `m_use_i8mm` are enabled at runtime and used in SPU LLVM for `GB`, `GBH`, `GBB`, `SUMB`, and the integer multiply paths.
- `m_use_tbl2 = true`; `tbl2`/`tbx2` implement `VPERM` and SPU `SHUFB`. This is the right lowering.
- `pshufb` lowers to a `movi`/`and`/`tbl1` sequence adapted from `sse2neon` (`CPUTranslator.cpp:37-79`). It is correct; the `and` is only redundant when indices are provably below 16.
- `sse2neon.h` is vendored for x86-shaped intrinsics, but most ARM paths use the `CPUTranslator` NEON helpers.

**Verified NEON forms and their current shapes:**

| Guest op | Current shape | Target on Apple NEON | Note |
|---|---|---|---|
| `shufb`, two tables | `tbl2`/`tbx2` | `tbl2`/`tbx2` | Already optimal; keep `m_use_tbl2` |
| `selb` | `bsl`/`bit`/`bif` | `bsl`/`bit`/`bif` | One uop, 2 cycles |
| `cntb` | `cnt` + narrowing | `cnt` | 16-byte popcount |
| `clz`/`cls` | `clz`/`cls` | `clz`/`cls` | Vector forms |
| `mpya`/`mpyh` | `vmlal`/`vmlaq` | `sdot`/`udot` where legal | DotProd already used |
| `sumb` | `udot` | `udot`/`vpadalq_u8` | Already optimised |
| `rot`/`roti` | shifts + `orr` | `ext` for byte rotates | Prefer `ext` where the amount is byte-aligned |
| XFloat clamp | `fminnm` | `fminnm` | Single instruction |
| `vperm` | `tbl2` | `tbl2` | Good |
| unaligned `lvlx`/`lvrx` | `ldr` + `pshufb` | `ldr` + `ext`/`tbl` | Measure; page-crossing is the risk |
| `stvlx`/`stvrx` | generic masked store | `bsl` merge-store + `str` | §8.3.4; avoid per-byte lowering |
| `vrefp`/`vrsqrtefp` | `frecpe`/`frsqrte` | Same | Accuracy caveats; never change guest semantics |

**In scope, because they are cheap and testable:**

1. **Constant-fold shuffles and selects.** `CPUTranslator` already has `match_vr` and constant-fold paths. Extending them to `shufb` with a constant mask, `selb` with a constant, and `rot` with a constant removes runtime work rather than rescheduling it. This is the largest SIMD lever and it is architecture-neutral.
2. **`pshufb` mask elimination** when the index range is provably below 16. Small, local, covered by a golden-assembly test.
3. **Aligned fast paths for `LVLX`/`LVRX`.** When `addr & 15 == 0`, the shuffle is a no-op; when it is not, `ext` can beat `tbl`. A runtime branch is only worth it if it predicts; otherwise use the branchless form. Both variants get an alignment-matrix test over offsets 0–15.
4. **Address folding.** `ReadMemory`/`WriteMemory` should fold base+offset into the `ldr`/`str` addressing mode and hoist repeated `m_base` materialisation out of a region. This falls out of region IR (§8.2.1) and is measured by golden assembly.
5. **`fmlal`/`fmlal2` and `bf16` only with a bit-exact mapping.** If no bit-exact mapping exists, skip; do not approximate guest FP.

**Out of scope, and why:**

- **256-bit IR on NEON.** It splits into pairs; it does not provide AVX2 width. The AVX-512 win in RPCS3 came from new 128-bit *shapes* (`VPLZCNTD`, `VPOPCNTB`, `VPTERNLOG`, `VPDPWSSD`, `VRANGEPS`, `VGF2P8AFFINEQB`), all of which have NEON or DotProd equivalents that RPCS3 already uses. Chasing width is chasing the wrong half of that result.
- **`sse2neon` "precise" modes as a blanket fix.** They add instructions. Fix the specific opcode's semantics instead.
- **The `m_use_avx` path.** It is dead on ARM (D7); do not spend a PR on it, and do not add a second dead flag.
- **A hand-written software-pipelining pass for SPU loops.** LLVM already schedules these; a hand pass is high risk for an unmeasured gain.

**Tests:** differential SPU/VMX opcode tests against the interpreter over millions of random vectors, including index ≥ 16, the 0x80 zeroing case, signed and unsigned saturation, NaN/Inf, and denormals; golden assembly for `shufb`, `selb`, `sumb`, `mpya`, `gbb`, `rot`, `LVLX`, and `STVLX`; an alignment matrix for every unaligned op; and a compile-time regression check on the SPU cache build. These are the `CPU-NEON-*` rows in Appendix C.

---

## 9. Workstreams and the PR plan

### 9.1 Workstream map

| WS | Name | Priority | Depends on | Primary files |
|---|---|---|---|---|
| **W0** | Instrumentation, corpus harness, CI on Apple hardware | P0 | — | `Utilities/Thread.cpp`, new `tests/cpu/`, `.github/workflows/` |
| **W1** | Correctness: frames, decommit, atomics, W^X | P0 | W0 | `SPULLVMRecompiler.cpp`, `PPUThread.cpp`, `vm_native.cpp`, `JIT*.cpp` |
| **W2** | Fastmem + fault backpatch | P1 | W1 (fault contract) | `CPUTranslator.{h,cpp}`, `PPUTranslator.cpp`, `Thread.cpp`, new `fastmem.{h,cpp}` |
| **W3** | Sub-page notification map | P1 | W2, A1 resolved | `vm.cpp`, `sys_mmapper.cpp`, `Thread.cpp` |
| **W4** | Dispatch: regions, link area, IBTC, RSB | P1 | W2 (state at exits) | `PPUTranslator.cpp`, `CPUTranslator.h` |
| **W5** | IR state: SSA across edges, CR packing | P1 | W4 | `PPUTranslator.cpp`, `CPUTranslator.h` |
| **W6** | Codegen quality: SIMD lowering, masked store, unaligned paths, shuffle folding | P2 | W5 | `PPUTranslator.cpp`, `SPULLVMRecompiler.cpp`, `CPUTranslator.cpp` |
| **W7** | JIT lifetime: page invalidation, code reuse, cache v10, PGO | P2 | W2, W4 | `PPUThread.cpp`, `JIT*.cpp` |
| **W8** | Threading/parking/policy | P2 | W0 | `Thread.cpp`, `sync.h` |
| **W9** | Platform: entitlements, RAII W^X, icache, CI signing | P0 | — | new `.entitlements`, `JIT.h`, `.ci/`, `.github/` |

### 9.2 PR sequence

Each PR is independently reviewable, carries its own tests, and names the defect it fixes. The order ensures each PR's correctness does not depend on a later one.

| PR | Title | WS | Size | Gate |
|---|---|---|---:|---|
| 0 | `ci: sign a Hardened Runtime build and run a JIT smoke test` | W9 | S | new CI job green |
| 1 | `cpu: add fallback/fault/compile counters and a stats summary` | W0 | M | no behavior change; counters match logs |
| 2 | `cpu: fix decommit/reset to use mprotect+madvise (Apple)` | W1 | S | VM tests, `vmmap` soak |
| 3 | `jit: single RAII W^X scope with nesting + icache audit` | W9 | M | patch stress test |
| 4 | `spu/ppu: raise gateway scratch to 64 KB; log frames above 48 KB` | W1 | S | #19939 repro no longer corrupts; histogram visible |
| 5 | `spu/ppu: reload sp absolutely in both gateway epilogues` | W1 | S | forced-epilogue test, SPU and PPU |
| 6 | `spu/ppu: emit and decode .stack_sizes; per-function frame thunk` | W1 | L | FRAME tests, 3-deep chain, ASan call chain |
| 7 | `cpu: non-volatile T0 accesses with an explicit MMIO tier` | W2 | L | differential + MMIO tests |
| 8 | `cpu: fastmem site table and first-fault retry (no patch)` | W2 | L | fault-injection |
| 9 | `cpu: patch recurring fault sites to a per-site stub` | W2 | M | patch stress + p99 |
| 10 | `vm: sub-page notification map for 16 KB hosts` | W3 | L | #19881 repro or documented narrowing |
| 11 | `cpu: loop-first region formation and real CFG edges` | W4 | XL | golden IR + differential |
| 12 | `cpu: link area for direct exits` | W4 | M | block-link tests |
| 13 | `cpu: monomorphic IBTC for indirect branches` | W4 | M | hit-rate + correctness |
| 14 | `cpu: return stack buffer for bl/blr` | W4 | M | call/return stress |
| 15 | `cpu: SSA across region edges; live-out materialisation` | W5 | XL | differential + IR counts; only for regions above the threshold |
| 16 | `cpu: packed CR fields and lazy flags` | W5 | L | golden assembly |
| 17 | `cpu: merge-store and aligned LVLX/STVLX lowering; fold constant shuffles` | W6 | M | golden assembly + alignment matrix |
| 18 | `jit: per-page invalidation and code-space reuse` | W7 | L | invalidation stress |
| 19 | `jit: bump PPU cache to v10; persist sidecar metadata` | W7 | S | cold-cache measurement |
| 20 | `cpu: QoS/priority policy split; bounded isb + os_sync park` | W8 | M | p99 + audio A/B |
| 21 | `build: PGO variant for host code` | W7 | S | CI build + corpus run |

Sizes: S ≤ 150 LOC, M ≤ 500, L ≤ 1,500, XL ≤ 4,000 (including tests in the same PR). The XL PRs (11, 15) are deliberately isolated; review may split them by translation feature (branches first, then memory, then calls).

### 9.3 Dependencies that are easy to get wrong

- **PR 4 first, and independently of the rest of W1.** A 64 KB slab fixes the known crashes without touching the calling convention. It can ship in the same week it is written.
- **PR 5 before PR 6.** The thunk leaves `sp` lowered. The PPU epilogue survives that by accident; the SPU epilogue reloads the thread pointer from the overshot address and then fetches a return address from it. Fix the epilogues before any thunk exists.
- **PR 6 after PR 4 and PR 5.** Exact frames are an optimisation over a working slab, not the first line of defence.
- **PR 7 before PR 9.** Non-volatile accesses change the fault contract. Patch metadata must exist before an access can be re-executed safely.
- **PR 11 before PR 15.** SSA across edges requires real edges. Doing 15 first means writing phi logic on a call graph.
- **PR 15 only after the executed-region histogram.** Static shape says straight-line regions are ~6 instructions and loop regions ~13, with 20% of loop regions above 40 (Appendix A.12). PR 15 earns its cost only if the executed-region histogram shows most dynamic instructions inside regions at or above the threshold; otherwise PR 15 shrinks to CR packing (PR 16).
- **PR 10 after A1.** If #19881 is a 64 KB bug, PR 10 shrinks or disappears.
- **PR 19 with any IR PR.** A cache bump that lands after an IR change leaves stale objects in the wild.

### 9.4 Upstreamability notes per workstream

| WS | Upstream concern | Mitigation |
|---|---|---|
| W2/W3 | Memory-layer changes are the least Apple-specific and the most invasive | Keep the site table and handler generic (`ARCH_ARM64` host fault), gate only the Apple sub-page parts; land on Linux/ARM64 too if CI allows |
| W4/W5 | Translator changes affect x86 builds | The region IR is architecture-neutral; gate it behind a flag, run both translators in differential tests, and keep the block-per-function path until parity |
| W1 | `.stack_sizes` is ELF-specific but the JIT triple is shared | Implement in `jit_compiler` for all platforms that use an ELF triple; Apple is the only consumer initially |
| W7 | Code-space reuse is generic | No Apple-specific API; land shared |
| W8 | QoS is Apple-only | `#ifdef __APPLE__` around the policy split; no change to Windows/Linux priority semantics |
| W9 | Entitlements are Apple-only | New file + CI job; no source impact |

---

## 10. Verification

### 10.1 Layers

| Layer | What | Runs where | Gate |
|---|---|---|---|
| **L0** | Unit tests (`rpcs3_test`) for pure logic: decoders, bitmaps, site tables, ULEB128, CR helpers | CI, every PR | must pass |
| **L1** | IR/assembly golden tests: instruction counts, no unexpected `cset`/`strb`, link-area shape, merge-store | CI, every codegen PR | must pass |
| **L2** | Differential tests vs the interpreter: random instruction streams, state equality after N steps | CI/nightly | must pass |
| **L3** | Game boot/regression on the local corpus: Demon's Souls, Ridge Racer 7, Mega Man 9 | local/self-hosted Mac | every behavioral PR |
| **L4** | Native-vs-Rosetta performance A/B | local/self-hosted Mac | release gate (requires an x86_64 build) |
| **L5** | Soak/ASan/UBSan/TSan, patch stress, fault injection | nightly | no new findings |

A test that cannot run in CI is documentation. Appendix C groups tests by area; §10.5 records which areas run on hosted CI, which need the self-hosted reference machine, and which are deferred because the required dump or title is not available. A deferred row is not counted as passing.

### 10.2 Test ID scheme

`CPU-<area>-<nnn>`. Areas: `FEAT`, `FRAME`, `ATOM`, `VM`, `NEON`, `PPU`, `THR`, `JIT`, `FUZZ`, `GAME`, `PERF`.

The implementation must land **at least 250** of the 314 tests in Appendix C. The matrix is the contract; names may change, coverage may not. Every PR in §9.2 names the IDs it turns green.

### 10.3 Benchmark protocol

The protocol exists because the corpus is small and the machine is one laptop. It is designed to make a false win hard.

**Setup**

- Same game data, same `config.yml` (copy per run; never edit in place), same PPU/SPU/shader cache state, same resolution, same renderer.
- Fixed input replay per title, at least 120 s, covering a scene with stable load.
- One warm-up run discarded; three measured runs.
- Record median and p99 frame time, average FPS, CPU time per thread role, fault counts, compile counts, and package power where available.

**Controls**

- Compare **within a run pair** (A/B/A/B) to cancel thermal drift. Absolute throughput on this machine varies ~20% between runs; the only valid statistic is a within-session ratio.
- Never compare against a number quoted from an issue or a different machine.
- For microbenchmarks, pin to a P-core, disable background load, run 5 times, report median and the assembly.

**Pass criteria**

- A correctness PR passes if L0–L3 are green and no new counter regresses by more than noise.
- A performance PR passes only if it improves the target metric on at least two titles or one title plus a synthetic kernel, and does not regress p99 frame time on any of the three.
- A "no change" result is valid and must be reported; it shrinks the design.

### 10.4 Differential fuzzing

The highest-value test in the plan, because it covers translator changes that unit tests cannot.

- Generate random but valid guest instruction sequences (PPU and SPU) with constrained registers and memory.
- Run each sequence through the interpreter and the JIT.
- Compare GPRs, FPRs, VRs, CR, LR/CTR, memory, and reservation state after every instruction.
- Inject faults at random T1 sites: unmap a page, remap it, force an MMIO path, force a notification event.
- Run 10⁶ sequences per night; any mismatch is a JIT bug, not a fuzzer bug.

The existing `rpcs3_test` infrastructure can host this; the new part is fault injection and the state comparator. Start with PPU scalar + CR + branches, then SPU, then vector.

### 10.5 Corpus and CI hardware

- **CI:** GitHub-hosted `macos-14`/`macos-15` runners are Apple Silicon; use them for L0/L1 and a boot smoke test. They are not suitable for performance numbers.
- **Self-hosted:** the reference machine (A18 Pro, macOS 27.2) for L3/L4. A second machine (M-series Pro/Max, 16 GB+) is required before any conclusion about cache, bandwidth, or scheduler generalises.
- **Titles:** DS, RR7, Mega Man 9 locally. #19939 (Prototype 2, Max Payne 3), #19881 (GT5), and #19801 need either a dump or a reduced repro; until then those rows are deferred.

### 10.6 What "done" means

A workstream is done when:

1. Its PRs are merged with tests.
2. Its counters are visible in the stats summary.
3. It has a before/after on the corpus (or a documented reason why not).
4. Its failure modes are logged, not silent.
5. Its config flag has a measured default.

---

## 11. Phasing, effort, and what would shrink the design

### 11.1 Phase plan

| Phase | Duration | Content | Exit criterion |
|---|---:|---|---|
| **P0 — baseline** | 2–3 weeks | W9 + W0: entitlements/CI signing, counters, stats summary, benchmark protocol, instruction-mix scanner, first ~40 tests | CI signs and runs a JIT; counters visible; DS/RR7/Mega Man 9 baselines captured |
| **P1 — correctness** | 6–8 weeks | W1: decommit/reset, W^X batching + icache, `.stack_sizes`, frame thunk, atomics audit, `atomic_wait_engine` | #19939 repro passes; no `munmap` race; VM/FRAME/ATOM tests green |
| **P2 — memory v2** | 8–12 weeks | W2 + W3: non-volatile tiers, site table, fault retry, patch-on-recurrence, sub-page map (gated on A1) | fault injection 10⁶; #19881 fixed or narrowed with evidence; ≤1% tax on DS/RR7 |
| **P3 — dispatch + state** | 10–14 weeks | W4 + W5: regions, link area, IBTC, RSB, SSA across edges, CR packing | golden IR/asm; differential fuzz green; measurable instruction-count reduction |
| **P4 — lifetime + runtime** | 4–6 weeks | W7 + W8: per-page invalidation, code reuse, cache v10, PGO, QoS/parking | invalidation stress; p99 A/B; cold-cache measurement |
| **P5 — polish** | 3–5 weeks | W6, docs, upstream review cycles, rebases | all tests green; PRs merged or queued |

Phases overlap: P2's fault contract can be designed while P1 lands, and P3's region formation can be prototyped against the current translator behind a flag.

### 11.2 Effort

| Workstream | Effort | New/changed LOC | Risk |
|---|---:|---:|---|
| W0 Instrumentation, corpus harness, CI | 3–5 weeks | 2k–4k | low |
| W1 Correctness (frames, decommit, atomics, W^X) | 8–12 weeks | 3k–5k | medium |
| W2 Fastmem + fault backpatch | 10–16 weeks | 4k–7k | **high** |
| W3 Sub-page notification map | 4–6 weeks | 1k–2k | medium |
| W4 Dispatch (regions, link area, IBTC, RSB) | 6–10 weeks | 2k–4k | medium–high |
| W5 State (SSA edges, CR packing) | 8–12 weeks if the histogram supports regions; 4–5 weeks for CR packing alone | 2k–4k | **high** |
| W6 Codegen quality (SIMD, masked store, unaligned) | 4–6 weeks | 1.5k–3k | low–medium |
| W7 JIT lifetime, cache, PGO | 4–6 weeks | 1k–2k | medium |
| W8 Threading/parking policy | 2–3 weeks | 500–1k | low |
| W9 Platform/entitlements/CI | 1–2 weeks | 200–500 | low |
| Tests (314 matrix, fuzzing, corpus) | 8–12 weeks (parallel) | 4k–8k | medium |
| Upstreaming, review, rebases, docs | 6–10 weeks | — | medium |

**Phase 1 total: 10–16 engineer-months**, or 12–18 with integration slack. Phase 2 adds 8–12 engineer-months (§13.9). With two engineers splitting memory/dispatch and tests/CI, **6–9 calendar months**. Roughly **12k–20k changed/added lines** plus **4k–8k lines of tests**.

The estimate is dominated by W2 and W5, which are also the two items with the least public prior art for a PS3-shaped guest.

### 11.3 What would shrink it

| If this happens | Effect on design | Effect on effort |
|---|---|---|
| A1: #19881 is a 64 KB `mmapper` bug | Drop the general sub-page map; fix 64 KB (4 × 16 KB) | −4 to −6 weeks, −1.5k LOC |
| FAT0 (answered): fastmem prototype shows backpatching is practical | Keep the patching tier; the ceiling risk moves to region coverage | no change; removes the largest feasibility risk (Appendix A.12.2) |
| Executed-region histogram shows sub-40-instruction regions dominate | W5 shrinks to CR packing; W4 keeps link areas and IBTC | −6 to −8 weeks, −1.5k LOC |
| Region IR (W4/W5) fails differential parity after a bounded attempt | Keep block-per-function, ship dispatch-only improvements | −8 to −10 weeks, −2k LOC |
| Frame histogram shows few functions >8 KB | Keep the 64 KB slab; thunk only for exceptions | −1 to −2 weeks |
| CR packing measures <0.5% | Drop D9/CR packing | −2 to −3 weeks |
| No real-title A/B is ever captured | Performance claims remain relative to ARM baseline; design is still correct-first | no effort change, lower confidence |

The correct response to any of these is to shrink the design, not to explain the result away.

### 11.4 Staffing shape

Two engineers is the minimum sensible team:

- **Engineer A:** memory + JIT lifetime (W2, W3, W7) — the highest-risk, most novel work.
- **Engineer B:** translator + dispatch + state (W4, W5, W6) — the largest code surface.
- Both share W0/W1/W8/W9 and the test matrix.

One engineer can do it serially in 14–20 months. Three or more has diminishing returns until P3, because W2 and W5 are hard to parallelise across files.

---

## 12. Risks, falsification, and honest confidence

### 12.1 Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Frame thunk breaks tail-call/far-return invariants (§7.2 A4) | medium | high | gateway `sp` reload first (PR 5), then FRAME tests, 3-deep chain, ASan; keep the 64 KB slab as the fallback |
| Two-pass compile doubles cache-build time | medium | medium | only oversized modules; counter + cold-cache benchmark |
| Sub-page check taxes all memory ops (§7.2 A2) | medium | high | per-region/block gating; ≤1% gate; measure before ship |
| Fastmem is impractical with 64-bit guest pointers (FAT0) | medium | high | prototype first; page-table fallback; do not commit to the big number before it |
| Region IR changes semantics subtly | medium | high | differential fuzzing, flag, keep old translator until parity |
| Decommit change increases footprint | low–medium | medium | measured `MADV_FREE_REUSABLE`; `vmmap` soak |
| QoS/policy change regresses FPS or power | medium | high | flag; keep `SCHED_RR` default until p99 A/B; energy measurement |
| `.stack_sizes` disappears if the triple changes (§7.2 A5) | low | high | explicit check + log + slab fallback; CI test |
| W^X batching + signal handler stale icache (§7.2 A7) | low | high | patch stress, icache audit, assert every publish path |
| Cache bump causes a one-time full recompile (§7.2 A6) | high (certain) | low–medium | release note; measure cold cache; bundle with IR change |
| Upstream rejects Apple-specific code | medium | medium | small PRs, generic where possible, measurements, Linux/ARM64 CI |
| One machine, one chip, one OS | high | medium | add a second Apple Silicon machine before generalising; mark conclusions |
| Rosetta removed before parity | medium | high | prioritize P0/P1; keep the x86 build for macOS ≤27 |

### 12.2 What would falsify the design

1. **Fallback counters show no title silently interpreting.** Then "make fallback loud" is still useful, but it is not the explanation for any performance gap, and the performance thesis rests entirely on §6.3.
2. **Fastmem prototype shows a 64-bit guest pointer cannot be range-checked or patched cheaply.** Then W2 collapses to a page-table tier, the ceiling drops, and the effort estimate drops with it.
3. **Sub-page check costs >1% on DS/RR7.** Then W3 is narrowed to the 64 KB path or dropped; #19881 gets a targeted fix.
4. **Region IR fails differential parity after a bounded, well-defined attempt.** Then W4/W5 reduce to dispatch-only (link area + IBTC + RSB) on the existing block model.
5. **Codegen-count harness shows CR packing <0.5%.** Then D9 is dropped from the roadmap.
6. **Frame histogram shows no function above 8 KB outside two titles.** Then the 64 KB slab alone is sufficient and only the debug histogram ships; the thunk waits until a title proves it is needed.
7. **Real-title A/B shows `SCHED_RR` is not faster than QoS on p99 frame time.** Then W8 keeps only the QoS-destruction fix and the parking helper.
8. **The measured region length is under ~40 guest instructions.** Half-triggered by measurement. Static analysis finds a median of 6 instructions under the old entry rule and 10–11 under the multi-predecessor rule, but loop-header regions have a median of 13 and 20–22% exceed 40, covering ~55% of the static text. The consequence is already applied: formation is loop-first, regions below 32 instructions stay as linked blocks, and the SSA rewrite (PR 15) is gated on an executed-region histogram. If that histogram shows execution dominated by sub-40 regions, W5 shrinks to CR packing and W4 to link areas. Appendix A.12.
9. **The frame thunk cannot be made safe without the gateway rewrite, and the gateway rewrite regresses a title.** Then the 64 KB slab is the permanent answer and D14 is closed as "bounded by a large slab plus a loud histogram", not by exact frames.
10. **A guest block with external predecessors turns out to be common enough that splitting regions cuts them below the budget.** Then entry detection must be relaxed or mid-region entry stubs built; otherwise the region design is correct but not profitable.

### 12.3 Honest confidence

- **Correctness/stability:** high (≥85%) that P1 fixes real, reproducible failures. Every item is tied to a live issue or a code-level defect.
- **Performance:** medium (~50–60%) of a >10% FPS win on the local corpus from P2/P3 combined; low (<30%) of a large scheduler-driven win at any point.
- **The fastmem go/no-go passed.** Fault → commit → retry is ~5.7 µs and a batched patch is 1–2 ns/site (Appendix A.12), so the remaining performance unknown is dynamic region coverage, not fault handling.
- **There is no current, verified, large native-vs-Rosetta gap.** #17231 is stale. The design is justified by structural defects and the Rosetta deadline, not by a made-up multiplier.
- **All local numbers are one machine.** A18 Pro, macOS 27.2, 16 KB pages, 8 GB. Nothing was measured on an M4 Pro/Max/Ultra, on a larger machine, or under sustained thermal load.

### 12.4 Deliberately not in this design

- A forked LLVM or a private scheduling model as a **Phase 1** merge requirement. Phase 2 carries a vendored patch series instead (§13.4).
- SVE/SME codegen (inert in user space on the reference OS).
- A WFE-based spinlock (measured to burn a core).
- Replacing `SCHED_RR` by default before a real-title p99 A/B.
- Linux/ARM64 or non-Apple platform changes.
- Emulating a PS3 hypervisor; there is no ISA equivalence to exploit.
- A from-scratch JIT emitter. LLVM stays; the IR and the linking change.

---

## 13. Phase 2 — performance-first architecture

Phase 1 is constrained by what upstream will merge. Phase 2 is not. It keeps the same guest semantics, the same VM, and the same test oracle, and replaces the parts of the runtime whose shape was chosen for reviewability rather than speed.

Phase 2 is a separate branch and a separate configuration (`cpu_native_abi = v2`), not a rewrite of the tree. Every Phase 1 mechanism survives as the fallback and as the differential oracle.

### 13.1 What Phase 2 optimizes for, and what it gives up

| Dimension | Phase 1 | Phase 2 |
|---|---|---|
| Calling convention | LLVM `ghccc`, seven arguments per call | `ppu_abi_v2`: two pinned registers, zero argument moves |
| JIT engine | MCJIT + `RuntimeDyld` | ORCv2 + JITLink, background compile |
| LLVM | Stock, `-mcpu=apple-m4`, Cyclone scheduling | Vendored patch series with a Firestorm/M4 model |
| Codegen | One pass from guest op to LLVM IR | Guest-level liveness, alias metadata, non-volatile T0, machine peepholes |
| Dispatch | Static loop-first regions, link areas, IBTC, RSB | Hotness tiering, guarded traces, jump-table specialization |
| Compilation | Synchronous on first entry | Tier 0 interpret, tier 1 baseline, tier 2 optimize, all off-thread |
| SIMD | NEON only | NEON only (SVE absent, SME inert — §13.5) |
| Upstreamability | Hard requirement | Not a requirement; a patch series with a written rationale |

The costs are real: a forked LLVM build, a bigger test surface, and two JIT engines to keep alive during migration. Phase 2 is justified only if the gates in §13.9 pass; otherwise the corresponding piece is reverted, not defended.

### 13.2 PPU ABI v2

#### 13.2.1 The register contract

The AArch64 PCS gives 31 general registers. `x18` is the platform register on Apple and is never touched. The Phase 2 contract pins exactly two:

| Register | Holds | Why pinned |
|---|---|---|
| `x28` | `cpu_state*` | Every helper, fault path, and exit needs the guest state without a load |
| `x27` | `mem_base` | Guest address = `x27 + u32 addr`; removes a load per access |
| `x0`–`x26`, `x29`, `x30` | LLVM allocation, calls, frame | Guest GPRs live here inside a region |

`cpu_state` is cache-line aligned and its first line is the hot header:

```cpp
struct alignas(64) cpu_state
{
    u64 gpr[32];        // offset 0x000: canonical guest GPR home
    u64 cr;             // offset 0x100: packed 32-bit CR in the low half
    u64 lr, ctr, xer;   // offset 0x108
    u32 cia;            // offset 0x120
    u32 state;          // offset 0x124: stopped / waiting / exited
    u8* mem_base;       // offset 0x128: mirrors x27 for the C++ side
    u8* exec_base;      // offset 0x130
    u32 fastmem_epoch;  // offset 0x138
    u32 reserved;
    u8  v[32][16];      // offset 0x140: vectors, 16-byte aligned
    u64 fpr[32];        // offset 0x340
};
```

Guest GPRs are **not** assigned fixed host registers. A fixed assignment wastes registers on guest code that does not use them and forces moves at every boundary. Instead:

- inside a region, SSA values live wherever LLVM puts them;
- at a region boundary, only the values the successor can read are stored to `cpu_state.gpr[i]`;
- the successor reloads only what it reads before writing.

That is the entire difference from Phase 1, where every boundary stores every dirty register and every entry reloads a fixed set.

#### 13.2.2 Region entry, exit, and call

A direct region call carries no arguments. The callee is compiled to expect `x28`/`x27` already set, which they are, because they are invariant for the life of the thread.

```asm
; direct call from the end of region A to the body of region B
; x28 = cpu_state*, x27 = mem_base, both callee-saved
b     B_body                 ; no argument moves, no stack traffic
```

An indirect call needs only a target:

```asm
; IBTC hit
ldr   x16, [x24, x9, lsl #3] ; x24 = IBTC base, x9 = hashed target
br    x16
```

The region entry stub is two instructions, not a prologue:

```asm
B_body:
ldr   w9, [x28, #0x124]      ; cpu_state::state
cbnz  w9, B_exit             ; stopped, waiting, or exiting
; body
```

A link area that has been proven safe may jump past that check (§8.2.2). Phase 2 makes the proof cheap: the callee exports `entry_flags`, and a caller links to `B_body` only when the callee has no syscall and no faulting access before its first state test.

Exit to C++ uses one stub, not a per-site sequence:

```asm
B_exit:
str   x0, [x28, #0x120]      ; optional: reason / guest pc
mov   x0, x28                ; the only argument the C++ side needs
ret                          ; x30 is untouched by region calls
```

The escape path still needs `pc` and `sp`. Phase 2 changes what is saved: because `x28` is callee-saved and invariant, the signal handler reads `cpu_state` directly from `uc_mcontext->__ss.__x[28]`; only `pc`/`sp` need the hypervisor slot. That removes the whole `x18`–`x30` save/restore block from the PPU gateway, and with it the `sp`-relative unwinding bug class in §8.4.

#### 13.2.3 Liveness, live-out, and clobber masks

Every compiled region exports a descriptor:

```cpp
struct region_desc
{
    u32 guest_start, guest_end;
    u8* host_entry;          // includes the state check
    u8* host_body;           // skips the state check
    u64 live_in;             // guest GPRs read before written
    u64 live_out;            // guest GPRs written and read by a successor
    u64 clobber;             // guest GPRs written before read (caller-saved)
    u16 frame_size;          // from .stack_sizes
    u8  entry_flags;         // has_syscall | has_fault | needs_state
    u8  tier;
};
```

The caller stores `dirty & callee.live_in`, not `dirty & all`. On real code `live_in` is small: a function that takes three arguments and a stack pointer reads four guest registers before writing them. This is the single largest instruction-count reduction in Phase 2, and it does not require a guest IR.

The same descriptor drives the reverse direction. A region that returns a value stores only `live_out`; the caller reloads only what it reads. In the common case of a leaf helper, both sets are under eight registers instead of thirty-two.

#### 13.2.4 Interop with C++ and the Phase 1 path

- C++ enters compiled code through `ppu_abi_v2_entry(cpu_state*)`, which sets `x27`/`x28` and calls `host_entry`. One stub, one place.
- Compiled code calls C++ through `ppu_abi_v2_call`, which saves the caller-saved set, sets `x0` = `cpu_state*`, and calls the helper.
- A region compiled with `ghccc` can be called from a v2 region through a six-instruction adapter; the adapter exists only during migration and only for modules not yet recompiled.
- The differential oracle runs both ABIs on the same guest code and compares `cpu_state` after every region. This is how the ABI is validated, not by review.

#### 13.2.5 What a branch costs after the change

| Work | Phase 1 | Phase 2 |
|---|---:|---:|
| Argument moves into GHC registers | 7 | 0 |
| Entry state check | 2–3 instructions | 2 instructions, skippable by link |
| Live-in reload | fixed set | `live_in` only |
| Live-out store | all dirty GPRs | `live_out` only |
| Virtual call / tail call | `blr` + GHC ABI | `b` to body or link cell |
| Guest state in memory per branch | 32 GPRs worst case | typically 4–8 |

### 13.3 ORCv2 and JITLink

#### 13.3.1 Why MCJIT is a tax

MCJIT + `RuntimeDyld` was designed for `lli`, not for an emulator:

- symbol resolution is a string map and a linear relocation pass;
- `finalizeObject()` is a global barrier;
- memory is allocated in a few large slabs, so freeing one invalidated module is coarse;
- sections are delivered to a `RTDyldMemoryManager` only after relocation, which is why `.stack_sizes` needs `setProcessAllSections` and a custom manager (§8.4);
- there is no supported way to compile concurrently into the same JIT.

None of that is fatal; all of it is avoidable.

#### 13.3.2 The Phase 2 engine

```cpp
auto jtbp = llvm::orc::JITTargetMachineBuilder::detectHost();
auto lljit = llvm::orc::LLJITBuilder()
    .setJITTargetMachineBuilder(std::move(jtbp))
    .setObjectLinkingLayerCreator([&](ExecutionSession& es, const Triple&) {
        auto layer = std::make_unique<ObjectLinkingLayer>(es, std::make_unique<Rpcs3JITLinkMemoryManager>());
        return layer;
    })
    .create();
```

The custom `JITLinkMemoryManager`:

- reserves one RW arena and one RX arena per module, 64 KB-aligned;
- returns a `FinalizedAlloc` whose deleter returns pages to the code-space reuse pool;
- toggles the RX alias with `pthread_jit_write_protect_np` inside the RAII W^X scope, not with `mprotect` per patch (measured: 1–2 ns/site batched versus 3 µs single-site, Appendix A.12);
- splits `.text`, `.rodata`, `.stack_sizes`, and stub islands into separate `LinkGraph` blocks, so a patch touches one page and invalidation frees one block;
- installs a `JITLink` pass that places per-module stub islands within ±128 MB of `.text`, which makes the 4-byte `b <stub>` patch universal and removes the 16-byte fallback.

`JITLink` also replaces the `.stack_sizes` decoder's dependence on `allocateDataSection`: the section arrives as a `LinkGraph` block with its relocations already applied, and the manager records `(symbol, size)` while walking it.

#### 13.3.3 Background compilation

Compilation moves to a `std::jthread` pool with a priority queue:

1. First entry to an unknown guest address runs the interpreter for that block and enqueues a tier-1 compile.
2. Tier-1 compilation is small and fast (no SSA, no regions); it publishes through an atomic pointer, so the dispatch path never blocks.
3. A region that crosses the hotness threshold enqueues a tier-2 compile. The tier-1 code keeps running until the tier-2 code is published, then the entry pointer is swapped in one atomic store.
4. Invalidation marks the descriptor dead, clears the entry pointer, and returns code space after the last executing thread leaves the epoch.

This is the frame-time fix that Phase 1 does not attempt. It is independent of the ABI and can ship before it.

#### 13.3.4 Migration

- `jit_engine = mcjit | orc` config, default `mcjit` for one release.
- The ORC path must pass the same 314-test matrix plus a module-level differential test that compiles every module in DS/RR7 both ways and compares every function's machine-code instruction count and every exported descriptor.
- Debug info and `perf` symbolization are allowed to regress in the first ORC release; correctness is not.
- LLVM version is pinned to the submodule already in the tree. Phase 2 carries a patch series, not a moving `main`.

### 13.4 A real scheduler model for Firestorm and M4

#### 13.4.1 The model

LLVM binds every Apple CPU to `AArch64SchedCyclone.td` (2014, Cyclone). `-mcpu=apple-m4` selects the right *features* and the wrong *scheduling model*. Phase 2 adds `AArch64SchedFirestorm.td`:

- issue width, dispatch groups, and retirement limits;
- per-port resource cycles for ALU, branch, load, store, FP/NEON, and crypto pipes;
- load-to-use and store-to-load forwarding latencies;
- the pair-register penalty (`LDP`/`STP`) and the `MADD`/`SMADDL` fusion rules that are already feature-gated in LLVM;
- read/write resources for `CAS`, `LDADD`, `SWP`, and the rest of LSE.

The numbers come from public black-box measurements and are validated with `llvm-mca` against microbenchmarks built from the same instruction mix as Appendix A.5. Anything not measured on the reference machine is marked `[U]` in the table and is not allowed to change a scheduling decision until it is.

#### 13.4.2 Build and versioning

Three options, in order of preference:

1. **Patch the in-tree LLVM submodule** with the new `.td` plus the one-line mapping in `AArch64Processors.td`. This is a vendored patch series, applied by the build, with a `README` that states the exact LLVM revision and the rebase cadence.
2. **A separate `llvm-project` fork** only if upstream rejects the model. The fork carries no other changes, so rebasing is mechanical.
3. **No fork**: keep Cyclone and accept the loss. This is the Phase 1 answer and the fallback if the gate fails.

The patch series is tested by `llvm-mca` output diffs, not by "it compiles".

#### 13.4.3 What else to fix in codegen

The scheduler is the least valuable part of this section. Phase 2 also adds `MachineFunction` passes that LLVM will not do for a JIT:

- avoid `LDP`/`STP` where the pair form has a measured 2× penalty on M4;
- fold `add`+`load` into `[base, index, lsl]` addressing when the index is already scaled;
- sink guest-state stores to the region exit instead of the branch;
- duplicate the return block when it removes a taken branch in a hot loop;
- align loop headers to 128 bytes (the measured cache-line size, Appendix A.3).

These are small, local, and measurable with the codegen-count harness from Phase 1.

### 13.5 SIMD: SVE and SME

This one is not an upstreamability sacrifice. It is a hardware fact:

- SVE and SVE2 are **absent** from every Apple CPU; there is no `hw.optional.sve`.
- SME and SME2 are advertised, but on macOS 27.2 `SVCR` stays zero after `smstart`, the count registers read zero, and ZA reads zero (Appendix A.11). User space cannot use them.

Phase 2 therefore does **not** generate SVE or SME. It adds a startup probe and a single, contained exception: if a future OS enables streaming mode, use SME only for bulk `memset`/`memcpy`-shaped helpers, never for translator output. The translator keeps NEON, which is what the hardware actually executes.

The performance work in SIMD is elsewhere: full-width `LD1/ST1` for `lvx/stvx`, `TBX`-based shuffle lowering, `CNT`/`UADDLV` for popcount and sum, and `AES`/`SHA` for the crypto instructions that the guest uses. Those are in Phase 1 because they do not need a fork; Phase 2 only widens them where the ISA allows.

### 13.6 Dynamic optimization

#### 13.6.1 Tiering

Each region descriptor carries a per-thread execution counter. The dispatch path increments it with a plain `ldrh`/`add`/`strh`; no atomics, no shared cache line between threads.

| Tier | Code | When |
|---|---|---|
| 0 | Interpreter | First execution of an unknown address; also after invalidation |
| 1 | Baseline JIT: block-per-function, no SSA, no regions | Published within microseconds; runs while tier 2 compiles |
| 2 | Optimized: loop-first region, SSA, CR packing, traces | After `tier2_threshold` executions (default 64) |

The interpreter is already in the tree. What is new is using it as a *cold tier* instead of a failure mode, and never blocking a guest thread on a compile.

#### 13.6.2 Guarded traces

A hot region is promoted to a trace by following its dominant edge, not by static layout:

- the descriptor records a saturating counter per side exit;
- the hottest path is duplicated into a trace of up to 512 guest instructions;
- each side exit gets a guard: compare the guest PC (and the CR/CTR bits the trace assumed) and branch to the original region entry on mismatch;
- the guard is one `cmp`+`b` pair on the hot path; the cold path stays in the region.

This is the dynamic answer to the static measurement in Appendix A.12. If the executed-region histogram shows that short regions dominate, traces recover the length that static formation cannot.

#### 13.6.3 Jump-table specialization

PPU switch code has a recognizable shape:

```text
lis   rX, hi
addi  rX, rX, lo
lwzx  rY, rX, rZ
mtctr rY
bctr
```

Phase 2 pattern-matches it, recovers the table base and the index, and emits a real `switch` with a bounds check. If the table length is not recoverable, it emits the IBTC path plus a specialization cache of the four hottest targets as a compare chain. This removes a dependent load and an indirect branch from every switch, which is a large class of PPU code.

### 13.7 Guest state without volatile

Phase 1 keeps every guest load/store `volatile` because the signal handler can change memory behind the compiler's back. That is correct, and it also prevents LLVM from eliminating redundant guest-state traffic.

Phase 2 changes the contract instead of the optimism:

- `cpu_state` is accessed through a pointer marked `!noalias` against guest memory, and guest memory is marked `!alias.scope` per region.
- T0 accesses are non-volatile. The fault path can still change a mapping, but a mapping change is followed by an epoch bump and an invalidation, so no compiled region observes a stale value across the boundary.
- The only volatile operations left are the state word, the epoch, and MMIO.
- LLVM can then keep `cpu_state.gpr[i]` in a register across a block when no call or fault intervenes, which is exactly the Phase 1 SSA goal without a second IR.

A dedicated guest IR remains an option, but it is a last resort: the measured win in Dynarmic and Ryujinx comes from removing redundant state traffic, and alias metadata plus liveness gets most of that without a new IR.

### 13.8 Profile-guided JIT heuristics

Phase 1 PGOs the host binary. Phase 2 also PGOs the *JIT*:

- a corpus run records block and edge counts for DS, RR7, Mega Man 9, and the PPU library;
- the profile sets `region_min_instructions`, `region_budget`, inline budget, IBTC associativity, trace threshold, and branch layout;
- the profile is a versioned data file shipped with the build, not a hard-coded table, so it can be regenerated when titles change;
- a title with no profile uses the static defaults.

### 13.9 Work plan, effort, and gates

| Piece | Effort | Depends on | Gate (revert if it fails) |
|---|---:|---|---|
| ABI v2 + descriptors | 8–12 weeks | Phase 1 PR 11 | ≥5% fewer host instructions and ≥3% FPS on DS/RR7 |
| ORCv2 + JITLink | 6–10 weeks | Phase 1 PR 19 | ≥20% lower p99 compile latency, no correctness delta |
| Background tiering | 6–10 weeks | ORCv2 | ≥30% lower p99 frame-time stutter, p50 not worse |
| LLVM scheduler model | 4–8 weeks | vendored LLVM | ≥1% on three titles, else drop |
| Machine peepholes | 3–4 weeks | scheduler patch | ≥1% each, measured by codegen counts |
| Guarded traces | 6–10 weeks | ABI v2 + tiering | ≥5% on branch-heavy titles |
| Jump-table specialization | 3–4 weeks | ABI v2 | ≥3% on titles with switches |
| Non-volatile T0 + alias metadata | 4–6 weeks | Phase 1 PR 7 | ≥5% fewer state accesses |
| JIT-heuristic PGO | 3–4 weeks | corpus harness | ≥2% |
| Phase 2 tests | 6–8 weeks (parallel) | — | new ABI/TIER/TRACE/ORC rows green |

**Total: 8–12 engineer-months on top of Phase 1.** With two engineers, 4–6 calendar months after Phase 1 stabilizes.

The order is deliberate. Background tiering and ORCv2 come before the ABI because they fix stutter without changing semantics. The ABI comes before traces and jump tables because both need the descriptor. The scheduler model comes last because it is the smallest expected win and the largest maintenance cost.

### 13.10 Falsifiers

1. **ABI v2 saves <3% FPS.** Then the seven-argument tax was not the bottleneck; keep `ghccc` and delete the ABI work.
2. **ORCv2 lowers compile latency but not frame-time stutter.** Then the stutter is in the guest-side compile queue, not the JIT engine; keep MCJIT and fix the queue.
3. **Traces do not beat static loop regions on the executed histogram.** Then the static region design was right and traces are deleted.
4. **The scheduler model changes `llvm-mca` output but not measured time.** Then scheduling is noise on this core; the patch series is dropped, not carried.
5. **Non-volatile T0 produces a correctness failure that alias metadata cannot explain.** Then the volatile contract stays and the state-traffic win is abandoned.
6. **Tiering raises p50 while lowering p99.** Then stutter moved rather than disappeared; the thresholds are wrong or the interpreter is too slow.
7. **A title regresses under any Phase 2 piece.** The piece ships behind a per-title override or not at all.

### 13.11 What Phase 2 still does not do

- No SVE codegen (hardware absent).
- No SME codegen (OS-inert), only an optional bulk-memory helper if a future OS enables it.
- No hypervisor, no guest/host ISA equivalence.
- No GPU work.
- No replacement of LLVM with a hand-written backend. The backend is not the problem; the IR shape and the ABI are.

### 13.12 Test surface added by Phase 2

The 314-row matrix in Appendix C is the Phase 1 contract and does not change. Phase 2 adds a separate matrix:

| Prefix | Count | What it covers |
|---|---:|---|
| ABI | 24 | register contract, liveness masks, adapter, signal recovery, differential v1/v2 |
| ORC | 16 | section placement, stub islands, deallocation, W^X, symbol lookup |
| TIER | 18 | promotion, demotion, in-flight compile, invalidation during compile |
| TRACE | 20 | guard correctness, side exits, CR/CTR assumptions, trace invalidation |
| JTBL | 12 | table recovery, bounds, fallback, specialization cache |
| SCHED | 10 | `llvm-mca` diffs, peephole golden assembly |
| PGO | 8 | profile versioning, missing profile, corpus reproducibility |

That is 108 additional tests, for 422 total across both phases.

---

## 14. References

### 14.1 Primary sources, fetched or verified 2026-11-07..13

- `llvm/lib/Target/AArch64/AArch64SchedCyclone.td` (LLVM main, 2026-11-12): `CycloneModel`, `IssueWidth=6`, `MicroOpBufferSize=192`, `LoadLatency=4`, `WriteAtomic Unsupported`.
- `llvm/lib/Target/AArch64/AArch64Processors.td`: `TuneAppleM4/M5/M6`, `AppleM4/M5/M6` feature lists, `apple-a18 → apple-m4`, `apple-a19 → apple-m5`, `apple-a20 → apple-m6`.
- `llvm/lib/Target/AArch64/AArch64Features.td`: `HasV8_4aOps` implies `FeatureLSE2`; `FeatureFuseAppleSMECompute`, `FeatureShiftMaddSlow`, `FeatureStorePairSuppress`, `FeatureFuseFMinFMax`.
- `llvm/lib/Target/AArch64/AArch64CallingConvention.td`: `CC_AArch64_GHC`, x19–x28, q4/q5, s8–s11, d12–d15.
- `llvm/include/llvm/Target/TargetOptions.h` (`EmitStackSizeSection`); `llvm/include/llvm/ExecutionEngine/ExecutionEngine.h` (`setProcessAllSections`, `setTargetOptions`).
- Apple Developer News, 2026-09-01, "Upcoming changes to Rosetta support for Intel-based macOS apps"; Apple Support 102527, "Using Intel-based apps on a Mac with Apple silicon."
- Local macOS SDK: `pthread/qos.h`, `pthread/pthread.h` (`pthread_jit_write_protect_np`), `libkern/OSCacheControl.h` (`sys_icache_invalidate`), `os/os_sync_wait_on_address.h`, `mach/thread_policy.h`.
- Local `sysctl`, `llc --version`, `sw_vers`, `getconf PAGESIZE`, 2026-11-12.
- Dougall Johnson, Apple CPU black-box measurements (`applecpu`: Firestorm/Icestorm retirement, fusion, elimination, latencies).
- `lemire.me`, 2024-04-29, "Careful with Pair-of-Registers instructions on Apple Silicon"; LLVM #235875 (LDP+STR 2× penalty on M4).
- Phoronix, 2024-06-15, "Apple M4 Support Added To The LLVM Compiler" (commit `2b335913`).
- WhatCookie, "Why is AVX-512 useful for RPCS3?" (2022-06-15): the wins came from new 128-bit shapes (`VPLZCNTD`, `VPOPCNTB`, `VPTERNLOG`, `VPDPWSSD`, `VRANGEPS`, `VGF2P8AFFINEQB`), all with NEON equivalents.
- `docs/research/apple-silicon/measurements/2026-11-12/`: `qos.cpp`, `place.cpp`, `place2.cpp`, `sstest.cpp`, `sstest2.cpp`, `mstore.ll`, `mstore2.ll`, `cr.ll`, `wfe3.cpp`, `wfecount.cpp`, `oswait.c`, `mem.c`, `ghccc.ll`, `ghccc2.ll`, `README.md`.
- `docs/research/apple-silicon/measurements/2026-11-13/`: `blocks.py`, `blocks-ds.txt`, `blocks-rr7.txt`, `fastmem2.cpp`, `README.md` (region shape and fastmem fault path).
- `rpcs3 --decrypt` + `llvm-objdump -d --triple=powerpc64` instruction mix for Demon's Souls and Ridge Racer 7.
- `docs/research/metal4/` for GPU-side constraints (not modified here).

### 14.2 Emulator sources

- Dolphin: PR #9441 (M1 JIT port), `JitArm64BlockCache::WriteLinkBlock`, `EmitBackpatchRoutine` in `Jit.h`, `Arm64GPRCache`/`Arm64FPRCache`, "JitArm64: Implement CR cache", codegen-space reuse progress reports, macOS/ARM64 `HandleFault` branch.
- PPSSPP: PR #8937 (W^X-compatible JIT on iOS), PR #23049 ("IR JITs: Compile blocks faster", ~20 µs → ~15.5 µs on Galaxy S8), `Arm64Jit`, `Arm64IRJit`.
- DuckStation: ARM64 backend (Vixl, guest state in `x19`, memory base in `x20`), `HandleFastmemException`, invalidation heuristics (3 recompiles/15 frames → interpreter; 4 invalidations/60 frames → `ManualCheck`), PR #1840 (MAP_JIT).
- Dynarmic / yuzu: `FastmemPatchInfo`, `recompile_on_fault`, 39-bit arena + page table, `BlockLinking`, `ReturnStackBuffer`, `GetSetElimination`/`ConstProp`/`MiscIROpt`.
- Ryujinx / ARMeilleure: `IJitMemoryBlock` (`MapAsRw`/`MapAsRx`/`MapAsRwx`), `JitSupportDarwin`, PPTC, "lightning" recompilation (Neptunia: 4.5 min/633 MB → 4.64 s/348 MB).
- PCSX2: PR #11364 ("Native Apple Silicon Support — Part 1", June 2024; no EE/VU/IOP recompilers at that point).
- ARMSX2: direct block chaining, SIGSEGV-backpatch fastmem for integer/FP/vector, register-cache coverage, PGO on a fixed corpus, elimination of `SYNC`/`CACHE` single-stepping.
- Cemu: PR #1255 ("General aarch64 improvements & Apple Silicon support", merged 2025-06-18), `cemu-bin-macos-arm64` action builds, Metal backend pairing.
- PPSSPP/OpenEmu-Silicon PR #471 (2026-05-12): macOS 26 blocks `mprotect(PROT_EXEC)` on anonymous pages under Hardened Runtime without `com.apple.security.cs.allow-unsigned-executable-memory`; `allow-jit` only covers `MAP_JIT`.

### 14.3 RPCS3 issues, PRs, and commits

#11922, #12115, #12338, #15982, #16081, #16484, #17231, #17530, #17888, #17893, #17905, #18133, #18151, #18423, #18425, #18656, #18707, #19369, #19609, #19611, #19759, #19801, #19881, #19939, #19977.

Commits: `965f4f5` (reset GHC transform state per module), `f4e66ee35` (tree context), `9514dcd0c` (RR7 freeze RCA).

---

## Appendix A — Measurements

**Evidence: [M]** — every number in this appendix was produced on the reference machine.

The reference machine is an Apple A18 Pro (MacBook Neo, `hw.model Mac17,5`), 2 P-cores + 4 E-cores, 8 GB, macOS 27.2 (26B123), 16 KB host pages, 128 B cache lines, not virtualised. Raw sources, build commands, and the unfiltered log are in `measurements/2026-11-12/`; this appendix is the summary of record. A number that appears in the body without a citation traces to a subsection here.

Two rules apply to every table:

- Only **within-run** ratios are meaningful. Absolute throughput on this machine varies by roughly 20% between runs, so a headline number from two different sessions is not a comparison.
- Synthetic results justify a hypothesis. They do not predict FPS. No table here is an end-to-end RPCS3 measurement unless it says so.

---

### A.1 Thread policy: QoS versus `SCHED_RR`

Sources: `qos.cpp`, `place.cpp`, `place2.cpp`.

Observed attributes, read back with `pthread_get_qos_class_np` and `pthread_getschedparam`:

```
main default                 qos=USER_INTERACTIVE(33) | policy=SCHED_OTHER prio=31
rpcs3-style thread           qos=UNSPECIFIED(0)        | policy=SCHED_RR    prio=99
qos-only thread              qos=USER_INTERACTIVE(33) | policy=SCHED_OTHER prio=31
after pthread_setschedparam  qos=UNSPECIFIED(0)        | policy=SCHED_OTHER prio=47
```

The RPCS3 sequence is the second row: `Utilities/Thread.cpp:2854-2856` sets `QOS_CLASS_USER_INTERACTIVE`, then `SCHED_RR` with priority 99. The QoS class does not survive.

Throughput, 8 threads, 2 s dependent-multiply chain, one clean run:

| Configuration | Total Miters/s | Within-run ratio |
|---|---:|---:|
| `SCHED_OTHER` default | 1143 | 1.00× |
| QoS `USER_INTERACTIVE` | 1180 | 1.03× |
| `SCHED_RR` 99 | 1799 | 1.57× |
| `SCHED_RR` 99 (repeat) | 1791 | 1.57× |

A second full run reproduced the ordering but not the absolute values: the `SCHED_RR` figure came in around 1.5× again, while the `SCHED_OTHER` and QoS figures moved by up to 20%. That is the entire supportable claim.

Interpretation:

- QoS is destroyed by the RPCS3 sequence (D1 in §7.1).
- `pthread_setschedparam` on a running QoS thread also unsets QoS and drops effective priority to the `SCHED_OTHER` maximum, 47 (D2 in §7.1).
- `SCHED_RR` wins *synthetic throughput under oversubscription*, which is exactly the state described in §6.1 — ten or more runnable threads on two P-cores.
- It says nothing about p99 frame time, audio underruns, input latency, or power. It is not an FPS prediction, and §8.5.3 keeps `SCHED_RR` behind a flag until a real-title latency A/B exists.

---

### A.2 `.stack_sizes` on the MCJIT path

Sources: `sstest.cpp`, `sstest2.cpp`.

- `llc -stack-size-section` emits `.stack_sizes` for **ELF only**. `arm64-apple-macos` produces no `__LLVM,__stack_sizes` section.
- The JIT triple is `aarch64-unknown-linux-android` (`Utilities/JITLLVM.cpp:656,673`), so MCJIT's object format is ELF and the section is present.
- With `TargetOptions::EmitStackSizeSection = true` and `ExecutionEngine::setProcessAllSections(true)`, `allocateDataSection` is called with `DATA name='.stack_sizes'`.
- Payload shape is `[u64 address][ULEB128 frame_size]` repeated, terminated by the section end — not by a sentinel. **Records are packed back-to-back; there is no alignment padding between them.** A decoder must advance by `8 + ULEB_length`.
- The address field arrives **post-relocation**; in the raw object it is an `R_AARCH64_ABS64` target. `allocateDataSection` sees the relocated payload.
- The address is the function's code entry and matches `getFunctionAddress()` exactly. Verified on a 3-function module containing `int a[3]`, `int a[300]`, and `int a[3000]`: the decoder recovered addresses `0x…c000`, `0x…c018`, `0x…c038` and frames **16, 1216, 12016**, matching the three `getFunctionAddress()` values. The same module built with the GHC calling convention produced **16, 1200, 12000** — the GHC frame omits the 16 bytes of C-ABI callee-save space.
- Section sizes include a large zero tail, and the tail is padding, not phantom functions: the 3-function section is 89 bytes, of which only 29 are records; the single-function `sstest2` section is 30 bytes, of which 10 are the record.
- A single-function section and a multi-function section therefore look different to a naive parser. The multi-function case is the one that matters, and it is the one that proves the "one record per function" contract.

Requirements this imposes on the decoder, all of which are test rows in Appendix C:

- decode after relocation;
- advance by `8 + ULEB_length`, never by a fixed stride;
- stop at the section end, ignore `addr == 0`, tolerate a truncated tail;
- assert that the record count equals the module's defined-function count in a debug build, then map every address and dedupe after thunk/alias creation;
- cap the value at 1 MB and treat anything above it as a decode or compiler bug;
- check for the section explicitly and fall back to the slab, never assert.

---

### A.3 16-byte atomics

Source: `mstore.ll`, compiled with `-mcpu=apple-m4` on LLVM 22.1.8.

```asm
_ldp16:  ldp  x0, x1, [x0]
         dmb  ishld
         ret

_stp16:  dmb  ish
         stp  x1, x2, [x0]
         ret

_casp:   casp x2, x3, x2, x3, [x0]
         ret
```

- LSE2 is implied by `HasV8_4aOps` in LLVM's feature model, so 16-byte loads/stores lower to `ldp`/`stp` plus the architectural fence.
- 16-byte compare-and-swap selects `casp`, and the release form selects `caspal`.
- `CycloneModel` declares `WriteAtomic Unsupported`; LLVM does not crash, it simply has no scheduling information for the atomic. That is a scheduling gap, not a correctness one, and it is another reason not to build the design on LLVM's scheduler (§3.2).

The mapping to RPCS3 is direct: `atomic.hpp:1111` and `:1170` already use this path, and #19611 recorded 19 → 58 FPS when the 16-byte GTA IV case stopped falling back.

---

### A.4 The generic masked store on AArch64 without SVE

Sources: `mstore.ll`, `mstore2.ll`.

- `llvm.masked.store.v16i8` with a runtime mask lowers to 16 conditional branches, 16 stores, and 7 mask-building instructions.
- With the exact shape `PPUTranslator::CreateMaskedStore` produces, the count is **31 branches + 16 stores**.
- The merge-store replacement is **6 instructions and 0 branches**:

```asm
shl.16b  v1, v1, #7
cmlt.16b v1, v1, #0
ldr      q2, [x0]
bif.16b  v0, v2, v1
str      q0, [x0]
ret
```

The replacement is only valid when the aligned 16-byte load and store cannot cross into an unmapped page, or when the surrounding tier can tolerate the fault. §8.3.4 keeps the generic form as the fallback for the unaligned and page-crossing cases.

`STVLX`/`STVRX` use the generic form today (D6 in §7.1), but the local corpus contains zero occurrences (Appendix A.5), so this is a code-quality fix with a golden-assembly test, not a performance claim.

---

### A.5 Static PPU instruction mix on real dumps

Method: `rpcs3 --decrypt`, then `llvm-objdump -d --triple=powerpc64`. Counts cover the whole file, including data that happens to disassemble as instructions, so the absolute numbers are inflated. The relative weights are what matter.

| Mnemonic | Demon's Souls | Ridge Racer 7 |
|---|---:|---:|
| `lwz` | 890,418 | 125,095 |
| `ld` | 448,358 | 60,896 |
| `std` | 396,301 | 50,854 |
| `li` | 349,387 | 61,069 |
| `stw` | 348,595 | 58,012 |
| `mr` | 338,475 | 58,113 |
| `clrldi` | 308,256 | 52,396 |
| `addi` | 292,130 | 69,884 |
| `bl` | 222,886 | 45,160 |
| `bt` | 210,683 | 35,719 |
| `cmpwi` | 209,789 | 20,898 |
| `bf` | 130,836 | 21,964 |
| `lvx` | 56,981 | 6,358 |
| `stvx` | 52,407 | 4,357 |
| `vmaddfp` | 28,451 | 4,495 |
| `vaddfp` | 13,175 | 778 |
| `vperm` | 8,037 | 517 |
| `vsel` | 6,525 | 582 |
| `lwarx` / `stwcx` | 31 / 35 | 0 / 0 |
| `sync` | 1,603 | 1 |
| `lwsync` | 17 | 0 |
| `lvlx` / `lvrx` / `stvlx` / `stvrx` | **0** | **0** |

What follows from it:

- Scalar loads, stores, register moves, and CR-forming compares dominate. Vector instructions are 1–2% of the static stream.
- `bt` + `bf` total 341,519 in Demon's Souls against 209,789 `cmpwi`: control flow is dense, and a large fraction of those branches sit near a block boundary. This is the static basis for the per-block-fixed-cost argument in §6.3.
- `LVLX`/`STVLX` are absent from both titles, so the masked-store fix is quality work.
- Guest atomics are rare (`lwarx`/`stwcx` in the tens, not thousands), so the atomic cost that matters on these titles is RPCS3's own queues, not guest LL/SC.
- `sync`/`lwsync` are not corpus-wide bottlenecks; the fence audit in §8.1.6 is about correctness margin and the RSX queue, not these counts.

The corpus is two titles for instruction mix and three for execution. "Zero `LVLX`" is a statement about Demon's Souls and Ridge Racer 7, not about the PS3 library.

---

### A.6 Spin waits

Sources: `wfe3.cpp`, `wfecount.cpp`, `oswait.c`.

| Primitive | Rate | CPU over 500 ms | `ps %cpu` | Wake p50 / p99 |
|---|---:|---:|---:|---:|
| `isb` | 38.9 M ops/s | 0.498 s (100%) | 97.8% | 0 ns / 1 µs |
| `yield` | 58.0 M ops/s | 0.498 s (100%) | — | 0 ns / 1 µs |
| `wfe` (no `sevl`) | 0.75 M ops/s | 0.498 s (100%) | 90.5% | 1 µs / 2 µs |
| `wfe` (`sevl` once) | 0.74 M ops/s | 0.498 s (100%) | 95.5% | — |
| `os_sync_wait_on_address` | parks | 0.0000 s | — | 9 µs / 150 µs |

- `wfe` does **not** park in user space on macOS 27.2/A18 Pro. It burns approximately a full core at 0.74 M operations per second, which is worse than `isb` on both axes — a lower spin rate and the same power draw (D10 in §7.1).
- `os_sync_wait_on_address` is the only tested primitive that actually parks: zero CPU time over the measurement window, with a p50 wake of 9 µs and a p99 of 150 µs.
- RPCS3 already uses it at `Utilities/sync.h:90`. The design in §8.5.3 is a bounded `isb` window followed by this park, never a WFE spinlock.
- The p99 is the number that matters for a guest spin loop. 150 µs is slower than the guest can tolerate for some handshakes, which is why the bounded spin window exists rather than parking immediately.

---

### A.7 Decommit and reset cost on 16 KB pages

Source: `mem.c`. 64 regions of 1 MB each, fully touched, `phys_footprint` read from `task_info(TASK_VM_INFO)`.

| Strategy | Decommit (64 MB) | Footprint after | Recommit + touch | Race-free |
|---|---:|---:|---:|---|
| `munmap` + `mmap(PROT_NONE)` (today) | 285 µs | 1.0 MB | 3.8 ms | **no** |
| `mprotect(PROT_NONE)` | 130 µs | 1.0 MB | 2.5 ms | **yes** |
| `madvise(MADV_FREE_REUSABLE)` | 207 µs | 1.0 MB | 2.1 ms | **yes** |

`mprotect` is the cheapest decommit. `MADV_FREE_REUSABLE` gives the cheapest recommit. All three return the physical footprint to the pre-allocation baseline, so there is no memory argument for keeping the unsafe pair.

The race is architectural, not theoretical: `rpcs3/util/vm_native.cpp:354-355` unmaps and then remaps **without `MAP_FIXED`**, so the returned address is only a hint and another thread in the same process can claim the range in between. `memory_reset` at `:384-385` has the same shape. Both are D4 and D5 in §7.1, and §8.1.5 replaces them with protection changes.

---

### A.8 CR and branch codegen

Source: `cr.ll`, `-mcpu=apple-m4 -O2`.

```asm
_cmp_branch:            ; one CR bit live
    cmp  x0, x1
    csel x0, x2, x3, lt
    ret

_cmp_fullcr:            ; full field materialised
    cmp  x0, x1
    csel x0, x2, x3, lt
    cset w8, lt
    cset w9, gt
    strb w8, [x4]
    cset w8, eq
    strb w9, [x4, #1]
    strb w8, [x4, #2]
    ret
```

- LLVM already folds `cmp` plus a one-bit use into `cmp` + `csel`, or `cmp` + `b.cc` when the result feeds a branch.
- Full-field materialisation is `cmp` + 3×`cset` + 3×`strb`, plus a fourth byte for the SO bit in the real translator.
- RPCS3 makes the expensive shape common. `SetCrFieldSignedCmp` (`PPUTranslator.cpp:5248`) writes four `i8` locals; `UseCondition` (`:845`) flushes before every branch; `GetCrb` (`:5214`) reloads one byte in the successor (D9 in §7.1).

The design consequence is in §8.3.3: keep eight fields as eight `u32` (or one packed `i64`), hold the last compare operands in SSA, materialise one word per dirty field at a region exit, and use `ccmp` chains where a guest predicate combines bits.

The expected win is **1–4% of PPU-bound time** and therefore well under 1% end-to-end on a GPU-bound scene. It is included because the change is small and testable, not because it is large.

---

### A.9 PPU and SPU cache keys

- **PPU** (`PPUThread.cpp:5816`): `ppu-<sha1(EBOOT)>/v9-kusa-<hash16>-<settings>-<resolved CPU>.obj`. The key includes the CPU model and the settings bitfield. It does **not** include the LLVM version, and the `v9` prefix is a literal with no documented bump discipline.
- Any IR change in this design invalidates the meaning of cached machine code. The first IR-changing PR bumps `v9` to `v10`; later IR changes co-land with the bump (D15 in §7.1, §8.5.1).
- **SPU** persistent cache (`SPUCommonRecompiler.cpp:855`): `spu-<block_size>-v1-tane.dat`, containing **raw guest code**, keyed by block size and a hardcoded version. This is correct as-is: it stores input, not codegen output.
- SPU `llvm/` objects are written only when `spu_debug` is enabled and are never loaded — `load_object` is PPU-only. There is no stale-object risk on the SPU side.

---

### A.10 GHC frames

Sources: `ghccc.ll`, `ghccc2.ll`. Command: `llc -mtriple=aarch64-unknown-linux-android -mcpu=apple-m4 -O2`.

```asm
big:
    mov  w8, #1
    str  w8, [sp]
    mov  w8, #2
    str  w8, [sp, #11996]   ; a 12 KB alloca, above sp
    mov  x0, sp
    bl   g
    ret
```

- Under the GHC calling convention LLVM emits **no prologue `sub sp`**. Spills and allocas go at positive offsets from whatever `sp` the caller supplied.
- A 12 KB alloca becomes `str w8, [sp, #11996]` with no stack adjustment.
- A 1.2 KB alloca behaves identically: `str w8, [sp, #28]`, no `sub sp`.
- RPCS3 supplies that `sp` from a fixed 8192-byte slab at each gateway (`PPUThread.cpp:380`, `SPUCommonRecompiler.cpp:513`). A 9.8 KB frame in Prototype 2 and a 10.7 KB frame in Max Payne 3 therefore write 1.6–2.5 KB past the slab into the gateway's saved registers and host frames. That is #19939 and D14 in §7.1.

This measurement rules out the assumption that "LLVM allocates its own frame." It does not, and it cannot be made to without changing the calling convention or inserting the thunk described in §8.4.

---

### A.11 SME and SVE in user space

Probe results on the reference machine:

- `SVCR` remains 0 after `smstart`.
- `CNTB`, `CNTW`, `CNTD`, and `RDSVL` all return 0.
- Reads from the ZA array return zero.
- Plain SVE or SME instructions without OS streaming mode trap.

The hardware advertises `FEAT_SME` and `FEAT_SME2`, and the OS does not expose them to user space. Therefore: do not design around them, do not generate them, and re-probe at startup in debug builds in case a future OS enables streaming mode. SVE and SVE2 are absent from the feature set entirely.

---

### A.12 Region shape and the fastmem fault path

Two experiments close Appendix B items 24, 33, and 34. Sources:
`measurements/2026-11-13/`, containing `blocks.py`, the raw analyzer output for
both titles, and `fastmem2.cpp`.

#### A.12.1 Static block and region shape

Corpus: the decrypted `EBOOT.elf` of Demon's Souls and Ridge Racer 7,
disassembled with `llvm-objdump --triple=powerpc64 -d`. Blocks are cut at
branch targets, after every branch, and at address gaps. Three units are
measured: a basic block; the **current inline chain** (follow unconditional `b`
only, because `UseCondition` and `BC` end a compiled function at every
conditional branch); and a **region** under the design's formation rules. The
loop-header region starts at every back-edge target and absorbs what is
reachable inside `[header, back-edge]`.

| Metric | Demon's Souls | Ridge Racer 7 |
|---|---:|---:|
| Instructions | 5,939,384 | 1,065,025 |
| Basic blocks | 1,070,410 | 189,058 |
| Branch density (blocks per instruction) | 11.9% | 12.2% |
| Block length mean / median / p90 / p99 | 5.55 / 4 / 12 / 26 | 5.63 / 4 / 11 / 29 |
| Blocks ≤10 instructions | 65.6% | 65.5% |
| Blocks with more than one predecessor | 8.1% | 7.4% |
| Blocks targeted by a direct branch | 49.4% | 48.9% |
| Current inline chain mean / median / p90 | 6.34 / 4 / 13 | 6.41 / 5 / 13 |
| Region, "any branch target is an entry" rule: mean / median / p90 | 8.40 / 6 / 18 | 9.50 / 6 / 20 |
| Region, multi-predecessor rule: mean / median / p90 | 15.53 / 10 / 32 | 18.55 / 11 / 41 |
| Regions ≥40 instructions, old rule / new rule | 1.2% / 6.6% | 2.3% / 10.6% |
| Back-edge / forward direct branches | 44.7% / 55.3% | 44.0% / 56.0% |
| Loop body span, median / p90 / p99 | 108 / 517 / 1,759 | 98 / 668 / 3,204 |
| Loop-header region mean / median / p90 / p99 | 29.9 / 13 / 63 / 270 | 33.2 / 14 / 79 / 291 |
| Loop regions ≥40 instructions | 19.8% | 21.8% |
| Loop regions ≥100 instructions | 4.8% | 7.3% |
| Instructions inside some loop region (overlapping) | 55.5% | 54.6% |

Interpretation:

- Straight-line regions are short: median 6 under the strict entry rule, 10–11
  with a multi-predecessor rule. A per-region fixed cost cannot be amortised
  there, which is why §8.2.1 forms regions loop-first and leaves short regions
  as linked blocks.
- The entry rule mattered more than the walk. Only 7.4–8.1% of blocks have more
  than one predecessor, while 49% are branch targets; replacing "any target" by
  real in-degree roughly doubles the median region at no duplication cost.
- Loops are the only long unit, and they are long enough to matter: 20–22% of
  loop-header regions exceed 40 instructions and together cover ~55% of the
  static text.
- The measured branch density confirms §6.3's static estimate: one block-ending
  branch every 8.2–8.4 instructions, with a median block of 4 and a median
  current inline chain of 4–5. The per-block fixed cost is therefore paid on
  nearly every branch, which is the quantitative case for link areas, IBTC, and
  the return stack buffer independent of SSA.

Caveats: this is static, not executed, coverage; the PPU library is not in the
corpus; and two titles are not all titles. The dynamic executed-region
histogram remains a P3 deliverable.

#### A.12.2 Fastmem prototype

`fastmem2.cpp` JITs `ldr w0,[x0]; ret` into an executable page, maps a 64 MB
`PROT_NONE` arena, and installs a `SIGSEGV` handler that commits the faulting
16 KB page and returns to re-execute the same instruction.

| Measurement | Result |
|---|---|
| Fault → commit 16 KB → retry, 4,095 fresh pages | mean 5.6–5.8 µs, p50 5.1–5.3 µs, p90 6.7–7.1 µs, p99 14–17 µs, max 142–178 µs |
| Steady-state bare load | 1.10–1.15 ns/op |
| Patch one site: `mprotect` RW + 4-byte store + `mprotect` RX + `sys_icache_invalidate` | mean 3.0–3.3 µs, p50 2.8–3.0 µs |
| Batched patch, one W^X scope, 4,096 sites | 5.0–7.2 µs total, 1–2 ns/site |
| Demoted site: `b` → save `x30` → `bl` helper → restore → `ret` | 1.85 ns/op |

Interpretation:

- A fault costs ~5,000× a load. Commit-on-fault is acceptable once per page and
  unacceptable per access; patch-on-second-fault is the correct policy.
- Two `mprotect` calls dominate a single-site patch. The RAII W^X scope is a
  prerequisite for multi-site patching, not a nicety.
- `sys_icache_invalidate` is cheap relative to the W^X transition.
- The handler needs only the fault address and the host PC, so 64-bit guest
  pointers are not a complication. The fastmem range-check thesis is answered.
- The per-site stub must preserve `x30`. The first prototype stub used
  `bl helper; ret` and hung: `bl` overwrites the region's return address, so
  `ret` returned to itself. The design's stub description now states the
  save/restore explicitly.

### A.13 What was not measured

Stated explicitly so that no reader infers more than the evidence supports:

- **No native-versus-Rosetta RPCS3 A/B.** There is no x86_64 RPCS3 build on the reference machine. Every RPCS3 performance claim in this document is relative to an ARM baseline, and building that comparison is the first milestone of P0 (§11.1).
- **No dynamic instruction-count or executed-region profile.** Static block and region shape is measured (Appendix A.12), but not how often each block executes. The 2–4× fixed-cost tax in §6.3 has a measured static half and an unmeasured dynamic half.
- **No frame-size distribution on real titles.** This needs the `.stack_sizes` decoder in-tree; it is a P1 deliverable.
- **No two-pass compile cost.** This needs the thunk; measure cold-cache Demon's Souls and Ridge Racer 7.
- **No GT5, Prototype 2, or Max Payne 3 dump.** #19881 and #19939 are verified from issue bodies and from the `.stack_sizes` mechanism, not reproduced locally.
- **No p99 frame-time, audio-underrun, or energy data.** All three are required before the QoS default changes.
- **One machine, one chip, one OS.** No M4 Pro/Max/Ultra, no sustained thermal load, no fanless power measurement beyond the spin-wait test.
- **Three titles, two decrypted for instruction mix only.** Conclusions about instruction mix do not generalise to the library, and "zero `LVLX`" says nothing about titles that use unaligned vector tails heavily.
- **The fastmem prototype is one site on one page.** Fault, commit, retry, patch, and a demoted stub are measured (Appendix A.12), but not multi-threaded patching, module-scale W^X scopes, or interaction with the real VM layer.

---

## Appendix B — Research closure

Every research question this document opened is listed below with its disposition. **Answered** means measured or verified. **Deferred** means it needs implementation, hardware, or a dump that does not exist locally. **Rejected** means the premise was wrong and the design does not include it.

| # | Question | Status | Answer or next step |
|---|---|---|---|
| 1 | Is the QoS class actually set today? | answered | No. `UNSPECIFIED(0)` with `SCHED_RR` 99. Appendix A.1 |
| 2 | Would QoS beat `SCHED_RR` on throughput? | answered (synthetic) | No: 1180 versus 1799 Miters/s, about a third lower. Real-title latency is still open. Appendix A.1 |
| 3 | Does `pthread_setschedparam` preserve QoS? | answered | No. It clears QoS and drops the effective priority to `SCHED_OTHER`/47. Appendix A.1 |
| 4 | Is `.stack_sizes` available to the JIT? | answered | Yes, because the JIT triple is ELF. Decoder verified end-to-end, including the padding and relocation behaviour. Appendix A.2 |
| 5 | Is the generic masked store actually slow? | answered | Yes: 31 branches and 16 stores, against 6 instructions for the merge-store form. Appendix A.4 |
| 6 | Do real titles use `LVLX`/`STVLX`? | answered | Zero occurrences in Demon's Souls and Ridge Racer 7. Appendix A.5 |
| 7 | Does the guest use `lwarx`/`stwcx` heavily? | answered | No: tens of static occurrences against hundreds of thousands of plain loads. Appendix A.5 |
| 8 | Is `m_use_avx` doing anything on ARM? | rejected | No. Every use is under `ARCH_X64` or an x86-only branch; it is a dead flag. D7 in §7.1 |
| 9 | Does WFE save power on macOS 26+? | rejected | No. On macOS 27.2/A18 Pro it burns about a full core at 0.74 M ops/s. Appendix A.6 |
| 10 | Does macOS expose SME or SVE to user space? | answered | Advertised but inert: `SVCR`, the count registers, and ZA all read zero. Appendix A.11 |
| 11 | Is there an Apple-silicon LLVM scheduling model? | answered | No. Every Apple CPU binds the 2014 Cyclone model; `TuneAppleM4/M5/M6` is tuning, not scheduling. §3.2 |
| 12 | Do the SPU caches miss a codegen-environment key? | rejected | No. The `.dat` file holds raw guest code and the `llvm/` objects are debug-only. Appendix A.9 |
| 13 | What does a decommit cost on 16 KB pages? | answered | 130 µs for `mprotect`, 207 µs for `madvise`, 285 µs for `munmap`+`mmap`, per 64 MB. Appendix A.7 |
| 14 | Is the CR representation expensive? | answered (structure) | Yes: four `i8` locals plus a pre-branch flush. The size of the win needs the instruction-count harness; bound is 1–4% of PPU-bound time. Appendix A.8 |
| 15 | Does the entry state check exist in the current translator? | answered | Yes, inline in `Translate` (`:241-265`), not in `TestAborted`. One check per nontrivial block. §5.1 |
| 16 | How does the existing fault path identify the faulting access? | answered | It re-decodes the host instruction on every fault, including an AArch64 decoder at `Thread.cpp:1838`. §5.3 |
| 17 | Frame-size distribution on real titles | deferred | Needs the decoder in-tree; one boot-time histogram per module. |
| 18 | Two-pass compile cost | deferred | Needs the thunk; measure cold-cache DS and RR7. |
| 19 | Exact 64 KB behaviour in #19881 | deferred | Needs a GT5 dump; instrument `_page_map`, reproduce `flags=0x1020a`, decide whether the general sub-page map is warranted. §8.1.3 |
| 20 | Real native-versus-Rosetta A/B | deferred | Build an x86_64 RPCS3 on the reference machine before macOS 28. |
| 21 | #19801 register scavenging | deferred | Needs a reduced IR repro from a live SPU compile before any fix is proposed. §8.3.5 |
| 22 | Can a Firestorm-class scheduling model be upstreamed? | deferred | Upstream dependency, explicitly not a merge requirement. §1.2 |
| 23 | Rosetta exception coverage under macOS 28 | deferred | Apple policy question. Do not design around it. §2.1 |
| 24 | Can fastmem work with 64-bit guest pointers? | answered | Yes. Fault → commit 16 KB → retry is 5.6–5.8 µs (p99 14–17 µs); a batched 4-byte patch is 1–2 ns/site; a demoted stub is 1.85 ns versus 1.10 ns bare. The stub must save/restore `x30`. §8.1.2, Appendix A.12.2 |
| 25 | Does region IR beat block IR on the corpus? | deferred | Differential fuzz plus codegen counts; keep the block path until parity. §8.2.1, §12.2 |
| 26 | Does non-volatile T0 access break anything? | deferred | Differential and MMIO tests; flag for one release. §8.1.1, §12.2 |
| 27 | Does a software sub-page check cost less than 1%? | deferred | Synthetic `ldr`/`str` benchmark, then DS/RR7 with a notification region active. §8.1.3, §12.2 |
| 28 | Is the per-thread `isb`-then-park policy better than pure `SCHED_RR`? | deferred | Needs p99 frame time and audio underruns, not throughput. §8.5.3 |
| 29 | Do 2 MB host pages reduce TLB pressure? | deferred | macOS exposes no `MADV_HUGEPAGE`; the kernel may already use superpages. Measure only, do not force. Appendix C `CPU-VM-025` |
| 30 | Does a hand-written software-pipelining pass help SPU loops? | rejected | LLVM already schedules them; a hand pass is high risk for an unmeasured gain. §8.7 |
| 31 | Does `.stack_sizes` emit one record per function, and is the address the code entry? | answered | Yes. A 3-function module produced three records at the exact `getFunctionAddress()` values, with frames 16/1216/12016 (C ABI) and 16/1200/12000 (GHC ABI). Records are packed with no alignment padding. Appendix A.2 |
| 32 | Do the gateway epilogues survive a permanently lowered `sp`? | answered | No for SPU: it reloads `x19` from `[sp]` after `add sp,#8192` and uses it to fetch the return address. PPU survives only because it restores `x20` and `sp` from `hv_ctx` afterwards. Gateway fix is a prerequisite to the frame thunk. §8.4.3 |
| 33 | How long is a typical region, in guest instructions? | answered (static) | Median 6 under the old entry rule, 10–11 with multi-predecessor entry, 13–14 for loop-header regions; 20–22% of loop regions exceed 40 instructions and cover ~55% of static text. Formation is now loop-first and PR 15 is gated on an executed-region histogram. Appendix A.12.1, §8.2.1 |
| 34 | How often does a guest block have an external predecessor? | answered (static) | 7.4–8.1% of blocks have more than one predecessor, versus 48.9–49.4% that are direct branch targets. Real in-degree is the correct entry test and doubles the median region. Appendix A.12.1, §8.2.1 |
| 35 | Does replacing `ghccc` with a two-register ABI save ≥3% FPS? | deferred (Phase 2) | The gate is §13.9; measure with the v1/v2 differential oracle and the codegen-count harness. §13.2 |
| 36 | Does ORCv2 lower p99 compile latency by ≥20%? | deferred (Phase 2) | Compare MCJIT and ORC on the same modules and the same corpus, measuring compile time and frame-time stutter. §13.3 |
| 37 | Does a Firestorm scheduling model change real execution time? | deferred (Phase 2) | Build the model, diff `llvm-mca`, then measure. Drop the patch series if the answer is under 1%. §13.4 |
| 38 | Do guarded traces beat static loop regions? | deferred (Phase 2) | Needs the executed-edge histogram. If they do not, the static loop-first design is kept. §13.6.2 |
| 39 | Does non-volatile T0 with alias metadata preserve correctness? | deferred (Phase 2) | Fault injection plus the v1/v2 differential; abandon if a failure is not explained by the memory model. §13.7 |
| 40 | Will macOS ever expose SME to user space? | deferred | Re-probe `SVCR` and the count registers at startup; no codegen until the probe succeeds. §13.5, Appendix A.11 |

---

## Appendix C — Test matrix (314 tests)

The matrix is the coverage contract. Names may change; coverage may not. Each row names the layer where it can run.

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

#### FRAME — GHC frames and gateway scratch (35)
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

#### VM — memory, 16 KB pages, page faults (38)
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

#### NEON — SPU SIMD (51)
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

#### PPU — codegen (31)
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

#### THR — threads/QoS (20)
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

#### JIT — cache, compile, layout (21)
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

---

---

## Appendix D — File and line index

Line numbers are from the `metal4-dev` tree at `9514dcd0c` and will drift with every rebase. The file paths and symbol names are the durable reference; treat a line number as a hint, not a citation.

### D.1 Translation and code generation

| File | Symbol / line | Relevance |
|---|---|---|
| `rpcs3/Emu/Cell/PPUTranslator.cpp` | `CallFunction` :554 | Direct, indirect, and far control flow; GHC tail call. The core of D11 and of §5.1 |
| | `UseCondition` :845 | Flushes registers, splits the block, tail-calls both successors |
| | `TestAborted` :865 | MMIO-path abort helper; not the entry state check |
| | `Translate` :241-265 | Inline entry state check; `need_check` decision at :200-230 |
| | `GetMemory` :860 | `m_base + addr`; the existing direct-access fast path |
| | `ReadMemory` :884, `WriteMemory` :951 | Volatile aligned loads/stores, MMIO heuristic, endian swap |
| | `RegLoad` :683, `RegStore` :699, `FlushRegisters` :705 | Guest-state materialisation; the target of §8.3.1 |
| | `BC` :2313 | Conditional branch lowering |
| | `SC` :2333 | Syscall path; flush and state barrier |
| | `lwsync` :3710 | `seq_cst` lowering (D8, conservative but correct) |
| | `STVLX` :3730, `STVRX` :3758 | Generic masked store (D6), replaced in §8.3.4 |
| | `GetCrb` :5214, `SetCrFieldSignedCmp` :5248 | CR representation (D9), redesigned in §8.3.3 |
| `rpcs3/Emu/CPU/CPUTranslator.h` | `m_use_avx` :3244 | Dead on ARM (D7) |
| | GHC and NEON helper declarations | Shared translator infrastructure; the surface region IR must not disturb |
| `rpcs3/Emu/CPU/CPUTranslator.cpp` | Shared memory helpers | Architecture-neutral; where the site-table hook belongs |
| `rpcs3/Emu/Cell/SPULLVMRecompiler.cpp` | GHC config :1650-1660 | `use_stack_frames = false`; gateway-owned scratch |
| | cache hash :1733 | `__spu-0x%05x-%s` |
| | W^X toggles :4218-4316 | Module load and relocation |
| `rpcs3/Emu/Cell/SPUCommonRecompiler.cpp` | gateway :513, release :609 | 8192-byte scratch reservation and release (D14) |
| | SPU block cache :855 | `.dat` holds raw guest code, not codegen output |
| | `TBL2` and compile-context retry | #19801 surface (§8.3.5) |

### D.2 JIT engine and memory

| File | Symbol / line | Relevance |
|---|---|---|
| `Utilities/JITLLVM.cpp` | triple :656, :673 | `aarch64-unknown-linux-android`; why `.stack_sizes` exists at all |
| | W^X :73-81 | LLVM emission guard |
| | `ObjectCache` :842-857 | Alternative `.stack_sizes` decode point |
| `Utilities/JIT.h` | `jit_write_guard` :441-452 | Existing RAII W^X; the batching target of §8.6.2 |
| | `jit_compiler` :535-597 | `add`, `try_add`, `load_object`, `cpu` |
| `Utilities/JITASM.cpp` | :315-333 | Runtime codegen W^X |
| `rpcs3/util/vm_native.cpp` | `get_page_size` :181 | Host page size; 16384 here |
| | `memory_decommit` :354-355 | `munmap`/`mmap` without `MAP_FIXED` (D4) |
| | `memory_reset` :384-385 | Same race (D5) |
| | `memory_protect` :424 | Protection changes; the replacement primitive |
| | `MAP_JIT` comment :348-350 | Why `MAP_FIXED` is unavailable on macOS JIT mappings |
| `rpcs3/Emu/Memory/vm.cpp` | `g_pages` :87 | One byte per guest 4 KB page |
| | 4 KB bailout :747, :981 | Native page mapping skipped when host page > 4096; root of #19881 |
| | `g_exec_addr` :49 | 4 GiB execution-table reservation |
| | 64 KB block flags :1920-1931 | The #19881 path to instrument first (§7.2 A1) |
| `rpcs3/Emu/Memory/vm.h` | `check_addr` :82-93 | Inline bitmap test used by slow paths |
| | `block_size_4k/64k/1m` | Guest block granularity |
| `rpcs3/util/atomic.hpp` | 16-byte CAS :1111, exchange :1170 | LSE2 `casp`/`caspal`; must not regress (§8.1.6) |
| `rpcs3/Emu/Cell/PPUThread.cpp` | PPU gateway :380 | 8192-byte slab (D14) |
| | exec helpers :475-480 | Guest address to execution-table entry |
| | `jit_module_manager` :3901-3970 | Module lifetime; module-level invalidation (D13) |
| | `ppu_initialize` :5109 | Module compile entry |
| | cache key :5816 | `v9-kusa-…` (D15) |
| | `load_object` :5850 | Disk cache load |
| | W^X :5982 | Module load and relocation |
| `rpcs3/Emu/Cell/lv2/sys_mmapper.cpp` | page-fault notification :178-208 | Event queue and re-arm; the semantics A9 must match |

### D.3 Faults, threads, and platform

| File | Symbol / line | Relevance |
|---|---|---|
| `Utilities/Thread.cpp` | `g_tls_fault_*` :112-114 | Existing fault counters; the base for §8.5.4 |
| | `handle_access_violation` :1555 | Single fault entry point |
| | ARM64 decode :1838 | `decode_a64_mem_inst`; the per-fault cost that §8.1.2 removes |
| | mmapper event :2070-2145 | Guest page-fault semantics on the x64 path |
| | W^X recovery :2719-2723 | `pthread_jit_write_protect_np` in the handler |
| | QoS / `SCHED_RR` :2854-2856 | D1 |
| | `set_native_priority` :3915-3946 | D2 |
| | affinity :3982-3986 | D3; `countr_zero` on a process mask |
| `Utilities/Thread.h` | `scoped_priority` :349-364 | D2 wrapper |
| `rpcs3/Emu/CPU/Backends/AArch64/AArch64Signal.cpp` | `decode_fault_reason` :71 | ESR and exception-class decode |
| `rpcs3/Emu/CPU/Backends/AArch64/AArch64JIT.cpp` | GHC frame preservation pass | Thunk insertion point for §8.4 |
| `Utilities/sync.h` | `os_sync_wait_on_address` :90 | The only parking primitive that works |
| `.ci/deploy-mac.sh` | :69 | Ad-hoc `codesign --deep -fs -`; no entitlement proof (§8.6.1) |
| `.github/workflows/` | macOS jobs | Where the Hardened Runtime JIT smoke test belongs (PR 0) |

### D.4 Documentation and measurements

| Path | Contents |
|---|---|
| `docs/research/apple-silicon/01-native-cpu-redesign.md` | This document; the canonical design |
| `docs/research/apple-silicon/archive/01-native-cpu-redesign-v5-2026-11-12.md` | Preserved prior version. History, not normative |
| `docs/research/apple-silicon/measurements/2026-11-12/README.md` | Raw measurement log, §1–§11 |
| `docs/research/apple-silicon/measurements/2026-11-12/*.cpp`, `*.c`, `*.ll` | Reproducers for every number in Appendix A |
| `docs/research/metal4/` | GPU-side constraints; unchanged by this work and out of scope |
