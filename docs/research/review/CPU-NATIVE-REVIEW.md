# RPCS3 CPU Emulation on Apple Silicon — `cpu-native-p1` Review and Upstream Gap Analysis

Date: 2026-11-27
Scope: recon only. Nothing in RPCS3 was built or run. The one exception is a 20-line SME probe compiled and run on this machine to check a claim in the branch's design doc.
Repo: `/Users/aa77982/PersonalWS/rpcs3`, fork branch `origin/cpu-native-p1` (67 commits ahead of master, merge-base with upstream `9b8974d32`, 2026-11-12). Compared against `upstream/master`.
Host used for probes: Apple M5 Pro (two performance levels: 6 + 12 cores), `hw.cachelinesize` = 128, 16 KB pages, macOS 27.
Companion doc: `METAL4-BACKEND-REVIEW.md` (RSX/Metal). Where the two overlap (fault path, texture-cache page protection), this doc points there.

---

## 1. Answer in one paragraph

Apple-specific fixes alone won't make CPU emulation vastly better. Upstream already handles the ARM basics: native 16-byte atomics (LSE2), TSO is not needed because the JIT emits explicit barriers, SPU LLVM has NEON paths, and LLVM picks the right host CPU. What's left falls into two groups.

1. **One large generic lever: PPU code shape.** Upstream translates PPU code block by block. Each basic block is its own function, every conditional branch flushes guest registers and tail-calls the next block, and there is no region IR, block linking, or return-stack prediction. This is the biggest CPU-side cost on any host. It hurts Apple a bit more because Apple cores reward long straight-line code and are hurt by indirect-branch chains. The branch measured a branch every ~8 instructions with a median block of 4, so this is where real double-digit gains could come from.
2. **Several Apple-specific defects that cost frame-time consistency more than average FPS.**
   - Every thread is forced to `SCHED_RR` priority 99, which wipes its QoS class. That is the opposite of Apple's guidance.
   - The app is missing the Game Mode plist keys.
   - Shared atomics are padded to 64 bytes, but the host has 128-byte cache lines, so they false-share.
   - Hot waits use `yield`, which on XNU can drop the thread to priority 0 for milliseconds.
   - Nothing distinguishes P cores from E cores.

   Each is cheap to fix. Together they should mostly reduce stutter on machines with few P cores (M1–M4 base, A18 Pro) rather than raise peak FPS.

The `cpu-native-p1` branch is careful, well-measured infrastructure work. It fixed real correctness bugs (W^X, decommit races, 16 KB sub-page mapping). But it has not landed the PPU region work that would move FPS, it has no in-game measurement, and its thread-policy change only made the bad default optional.

---

## 2. Review of `cpu-native-p1`

### 2.1 What landed (about 30 CPU commits, +3,265 lines in `Utilities`, `Emu/Cell`, `util`, `Emu/Memory`)

| Area | Change | Verdict |
|---|---|---|
| JIT memory | `jit_write_guard` with nesting; icache publish, including after `addObjectFile` | Correct and needed. It fixes a real hazard: stale icache after LLVM object load. Keep. |
| JIT memory | 1 MiB gateway slab with `.stack_sizes` checks | Fine. Low risk. |
| Memory | Decommit via `mprotect(PROT_NONE)` + `madvise(MADV_FREE_REUSABLE)` instead of `munmap` | Correct. It removes the D4/D5 unmap/remap races, where another thread could get a fresh mapping at the same address. Upstream-worthy. |
| Atomics | 16-byte atomics matched to clang's LSE2 lowering (load 1.42 → 0.48 ns) | Good micro-win. It affects host-side reservation code, not JIT output. |
| Memory | Sub-page map plus T1 guard for mmapper notification pages (#19881) | Correctness fix for 16 KB host pages over 4 KB guest pages. Upstream-worthy. |
| PPU | Page index + invalidation epoch, 64 KB code-chunk reuse | Reasonable. It reduces invalidation cost. |
| PPU | PPU cache v10 with a sidecar file | Fine. It invalidates old caches; flag this in release notes. |
| PPU | Loop-first region scan | **Built, but the translator doesn't use it yet.** No effect today. |
| PPU | Monomorphic IBTC (indirect branch target cache) | Unmeasured. Could help or hurt. Needs a counter-based check. |
| PPU | STVLX/STVRX merge-store lowering | Fine, local win. |
| Threads | `thread_priority` = QoS or RR (commit `7e284f705`) | Direction right, not enough. See §3.2. |
| Build | PGO option | Good. Should be on for release builds. |
| PPU | Non-volatile direct tier (default off) | Experimental. Leave off until measured. |

### 2.2 What did not land

Fault-site backpatching (steps 9d/9e), region IR with CFG edges, block link areas, return stack buffer, SSA across regions, and packed CR. These are exactly the items that would change FPS. The doc's own estimate for the landed memory/dispatch/state work is **4–9% end to end**, with ~50–60% confidence of >10% FPS. That is honest, and the estimate should be believed rather than rounded up.

### 2.3 Quality and honesty of the design doc (`01-native-cpu-redesign.md`)

- **Strengths.** Measurements are concrete: branch density 8.2–8.4 instructions, median block 4, loop regions covering ~55% of static text, fault→commit→retry ~5.7 µs, batched site patch 1–2 ns. Upside claims are conservative.
- **Weakness 1: the reference machine is an A18 Pro** (2P+4E, 8 GB). It's a good worst case for scheduling, but the results don't carry over to M-series Pro/Max parts with 6–12 P cores. The QoS-versus-RR result (1.57× synthetic throughput for RR 99 under oversubscription) is an artifact of a 2-P-core machine. It measures oversubscription, not normal play.
- **Weakness 2: no title-level numbers.** Every gain is synthetic or per-op.
- **Factual error, Appendix A.11 ("SME and SVE in user space").** The doc says `SVCR` stays 0 after `smstart`, `RDSVL` returns 0, and the OS does not expose SME. On this M5 Pro / macOS 27, a minimal probe gives:

  ```
  smstart; mrs SVCR; rdsvl #1; smstop
  → streaming vector length = 64 bytes (512-bit), SVCR = 0x3 (SM and ZA set)
  ```

  So SME/SME2 **is** usable from user space. The doc's probe was most likely wrong (for example, reading after `smstop`, or the compiler dropping the asm). The conclusion "don't build SPU codegen on it" still holds, for different reasons. Streaming mode has costly entry and exit, and NEON is not available or is slow inside it. 512-bit streaming vectors also don't fit 128-bit SPU registers. A.11 should be corrected so nobody repeats the claim.

---

## 3. Upstream gaps, ranked by expected impact

Impact labels: **L** = likely a measurable FPS gain in CPU-bound titles; **S** = mainly frame-time stability or stutter; **C** = correctness/robustness. All are unmeasured unless noted.

### 3.1 [L, generic] PPU translation is block-per-function (finding D11)

**Evidence (upstream `rpcs3/Emu/Cell/PPUTranslator.cpp`):**
- `Translate()` (~line 185) handles one `block = (info.addr, info.size)` and emits an entry-state check when the block is ≥16 bytes or contains a branch/SC.
- `UseCondition` (~845–858):

  ```cpp
  FlushRegisters();
  m_ir->CreateCondBr(cond, local, next);
  m_ir->SetInsertPoint(next);
  CallFunction(m_addr + 4);
  ```

  Each conditional branch spills every cached guest register to the context and tail-calls the fall-through block.

**Why it matters on Apple silicon.** With a branch every ~8 instructions and median block 4, most PPU time goes to register spill/fill through `ppu_thread` and to call/return or indirect dispatch. The work itself is small by comparison. Apple's wide out-of-order cores can't hide a load→indirect-branch chain every few instructions.

**Fix, in order of payoff per effort:**
1. **Region functions over loops.** The branch already has the loop-first region scan; feed it to `PPUTranslator` so a loop body plus its exits becomes one LLVM function with internal `br` edges. Guest registers stay in SSA inside the region and are flushed only at region exits and calls.
2. **Block linking (direct chaining).** At region exits with a constant target, emit a patchable direct `b` to the target's entry and patch it once the target is compiled. Use the existing W^X guard and icache publish.
3. **Return stack buffer** for `bl`/`blr` pairs. Push the predicted return host address on `bl`, and compare-and-jump on `blr`. This avoids the generic `ppu_dispatch` lookup on returns.
4. **Packed CR.** Keep CR as 8 nibbles in one GPR inside regions instead of 32 bytes in memory.

**Expected gain.** This estimate is not verified for RPCS3. Other dynamic recompilers (Dolphin, Cemu, Xenia) treat block linking and multi-block regions as core features because large wins on branch-dense guest code are well known. For RPCS3 PPU-bound titles, a plausible target is **+15–40%** in PPU-limited scenes. That number is a guess until a prototype is measured. This is the only item in the doc that justifies "vastly better", and it is not Apple-specific.

**Risks.** SMC and invalidation (the page index + epoch from the branch is the right base), cache-format churn, and LLVM compile time going up with function size. Cap region size, and keep the per-block path as a tier-0 fallback.

### 3.2 [S, Apple] Thread policy contradicts Apple's guidance

**Evidence (upstream `Utilities/Thread.cpp`, `thread_base::start`, Apple path ~2780–2805):**

```cpp
sp.sched_priority = 99;                                            // 2797
pthread_attr_set_qos_class_np(&attrs, QOS_CLASS_USER_INTERACTIVE, 0); // 2801
pthread_attr_setschedpolicy(&attrs, SCHED_RR);                     // 2802
```

This applies to **every** thread: PPU, SPU, RSX, LLVM compile workers, log writer, audio, input, and so on. On XNU, setting `SCHED_RR` takes the thread out of the QoS system, so the QoS class set one line earlier is lost. `set_native_priority` (~3879) moves between RR max and min. Affinity (~3933) passes `THREAD_AFFINITY_POLICY` tags, which XNU on Apple silicon ignores for placement.

**Apple's guidance** (Tech Talk 110147, "Tune CPU job scheduling for Apple silicon games"):
- QoS is the main input to P/E placement.
- RR/fixed priority is for short periodic work (audio-style), not continuous compute.
- Avoid busy waits and `yield`.
- Games should use priorities roughly 5–47.
- Match worker count to core count.

**Why it hurts.**
- Compile workers and logging at RR 99 compete as equals with PPU/SPU threads.
- On machines with fewer P cores than hot threads, the scheduler has no QoS signal for which threads deserve P cores.
- RR 99 threads that spin can starve the WindowServer and audio threads, which shows up as hitching.

**What the branch did.** Commit `7e284f705` adds `core.thread_priority` = `qos` | `rr`. In QoS mode it maps priority >0 to `USER_INTERACTIVE`, 0 to `USER_INITIATED`, <0 to `UTILITY`, and removes the affinity hint. But it is still one policy for every role, and the default stays RR.

**Fix: role-based policy, QoS by default on Apple.**

| Thread role | QoS / policy |
|---|---|
| PPU, SPU (active groups), RSX FIFO | `QOS_CLASS_USER_INTERACTIVE`, default policy |
| Metal submit / presenter | `USER_INTERACTIVE` |
| Audio output | Short periodic RR is acceptable here (or Core Audio's own workgroup) |
| LLVM compile workers, PPU/SPU precompile | `USER_INITIATED` while booting, `UTILITY` during gameplay |
| Shader compile workers | `USER_INITIATED` |
| Log writer, cache writer, disc/file I/O, network | `UTILITY` or `BACKGROUND` |

Sketch, keyed off the existing `thread_class` and names:

```cpp
#ifdef __APPLE__
static qos_class_t qos_for(thread_class cls, std::string_view name)
{
	switch (cls)
	{
	case thread_class::ppu:
	case thread_class::spu:
	case thread_class::rsx: return QOS_CLASS_USER_INTERACTIVE;
	default: break;
	}
	if (name.starts_with("LLVM") || name.starts_with("PPU Worker") || name.starts_with("SPU Worker"))
		return Emu.IsRunning() ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED;
	return QOS_CLASS_UTILITY;
}
#endif
```

Set it with `pthread_attr_set_qos_class_np` at creation (no `SCHED_RR`, no `setschedparam`). Re-tune compile workers with `pthread_set_qos_class_self_np` when emulation starts. Implement `set_native_priority` as relative priority inside the QoS class (the `relpri` argument, 0 to −15), not as an RR priority.

If emulation threads need deadline-like scheduling, look at Apple's **`os_workgroup`** API (`os/workgroup.h`). It is the sanctioned way to tell the scheduler that a set of threads shares a per-frame deadline. Whether a display-link workgroup can be joined from non-audio threads on macOS 26+ still needs to be checked against the SDK headers. Either way, it is the right direction instead of RR 99.

**Expected gain.** Little on M-series Pro/Max with many P cores. On M1–M4 base and A-series (4 or fewer P cores), it should noticeably reduce frame-time spikes during shader/PPU compilation. Measure with 1% lows, not average FPS.

### 3.3 [S, Apple] Game Mode is never engaged

**Evidence.** Upstream `rpcs3/rpcs3.plist.in` has only `NSHighResolutionCapable` and `LSMinimumSystemVersion` (15.0). There is no `LSApplicationCategoryType` and no `LSSupportsGameMode`.

**What Game Mode gives** (when the app is in native fullscreen):
- Highest priority on CPU and GPU.
- Background work pushed onto E cores so it stays away from the game.
- Doubled Bluetooth sampling rate for controllers, which lowers input latency.

**Fix.** Two plist keys:

```xml
<key>LSApplicationCategoryType</key>
<string>public.app-category.games</string>
<key>LSSupportsGameMode</key>
<true/>
```

Game Mode only turns on in **native** fullscreen (`toggleFullScreen:`), so make sure RPCS3's fullscreen path uses that rather than a borderless window. Worth checking in the Qt window code.

**Expected gain.** Free stability win plus lower controller latency. It is the cheapest item in this doc.

### 3.4 [S/L, Apple] 64-byte padding on a 128-byte cache-line host (false sharing)

**Evidence.**
- `rpcs3/Emu/Memory/vm.cpp:61`: `alignas(4096) u8 g_reservations[65536 / 128 * 64];`
- `vm_reservation.h:30`: `reservation_acquire(addr)` = `*(atomic_t<u64>*)(g_reservations + (addr & 0xff80) / 2)`. That gives one 64-byte slot per 128-byte guest reservation line.
- `rpcs3/util/atomic.cpp:172`: `struct alignas(64) cond_handle`; `:641` `root_info` is also 64-aligned.
- Per-thread `rdata` in `PPUThread.h:265` / `SPUThread.h:712` is `alignas(64)`. That one is per-thread and much less important.

`sysctl hw.cachelinesize` is **128** on Apple silicon. Two adjacent guest reservation lines therefore share one host coherence line. When SPU A does `PUTLLC` on line N and SPU B spins on line N+1, they contend for the same host line. SPURS workloads hammer neighbouring reservation lines, so this is a real pattern.

**Fix.** Make the stride a host constant:

```cpp
#if defined(__APPLE__) && defined(ARCH_ARM64)
constexpr usz host_line = 128;
#else
constexpr usz host_line = 64;
#endif
alignas(16384) u8 g_reservations[65536 / 128 * host_line];
// reservation_acquire: g_reservations + (addr & 0xff80) / 128 * host_line
```

Do the same for `cond_handle` and `root_info` (`alignas(host_line)`). Memory cost is trivial: 64 KB instead of 32 KB for the reservation table.

**Expected gain.** Unknown and title-dependent. SPURS-heavy titles with many SPU threads polling neighbouring lines could see fewer reservation failures and less wasted retry work. Measure with the existing reservation-miss counters.

### 3.5 [S, Apple] `yield` and spin waits in hot paths

**Evidence.**
- `rpcs3/util/asm.hpp:211`: `busy_wait` spins on `pause()`, which is `isb` on ARM.
- `std::this_thread::yield()` appears in `SPUThread.cpp` (9), `vm.cpp` (4), `sys_ppu_thread.cpp` (4), `Thread.cpp` (3), `PPUThread.cpp` (2), `CPUThread.cpp` (2), `RSXThread.cpp` (2), among others.

On XNU, `sched_yield`/`swtch_pri` can depress the calling thread's priority to 0 for up to ~10 ms. A PPU or SPU thread that yields while waiting for a short event can come back many milliseconds late. That costs frame time, not average throughput.

**Fix.**
- Replace yields in wait loops with the existing atomic wait engine (`atomic_t::wait`, which uses `__ulock_wait` on macOS), with a short bounded spin first (~1–2 µs of `wfe`/`isb`).
- For the "yield to let another guest thread run" cases (SPU loop detection, PPU `sys_ppu_thread_yield`), use `thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0)` or a 0-timeout `__ulock_wait`, which doesn't depress priority. Better still, use a real guest-scheduler handoff.
- `busy_wait` on ARM: prefer `wfe` with a timer bound over `isb` spins. `wfe` saves power and stops a spinning P core from taking power budget from its neighbours.

**Expected gain.** Fewer multi-millisecond stalls, mostly visible as better 1% lows.

### 3.6 [S] No P/E core awareness

**Evidence.** Upstream only reads `hw.perflevel*` to print `"(%lluP+%lluE)"` in the CPU brand string (`Emu/CPU/Backends/AArch64/AArch64Common.cpp:285–297`). `thread_ctrl::get_affinity_mask` (`Thread.cpp:3653`) only has `generic` and `amd_ccx` arrangements. `Preferred SPU Threads`, `Max SPURS Threads`, and `Max LLVM Compile Threads` have no Apple-aware defaults.

**Fix.** Pinning is impossible on macOS, so don't try. Instead:
- Default `llvm_threads` during gameplay to `perflevel1` (E-core) count, or to `max(1, P/2)` on machines without E cores.
- When `PPU threads + active SPU threads + RSX` is greater than the P-core count, suggest (or auto-set) `Preferred SPU Threads` = `P − 3` (minimum 1) in the per-game config UI.
- Log both perflevel counts at boot so user bug reports show them.

**Expected gain.** Mostly on 4-P-core Macs. It works together with §3.2.

### 3.7 [C/S] Fault path and 16 KB page protection

The branch measured fault→commit→retry at ~5.7 µs, versus 1–2 ns for an already-patched site. Fault-site backpatching (9d/9e) is designed but not landed.

The bigger Apple cost is on the RSX side. With 16 KB host pages, write-protecting one 4 KB guest page protects four, so writes near (not into) protected texture or surface memory still fault, and each fault costs ~5.7 µs. This is finding F6 in `METAL4-BACKEND-REVIEW.md`.

**Fix.**
- Land backpatching for hot fastmem sites, so a site that keeps faulting is rewritten into a checked slow path.
- In the texture cache, count false-sharing faults per 16 KB host page. Above a threshold, switch that page from protect-on-write to hash-on-use.

### 3.8 [Minor, Apple-capable] Float-mode features the hardware has but RPCS3 doesn't use

`sysctl` on this M5 Pro reports `FEAT_AFP`, `FEAT_RPRES`, `FEAT_LSE2`, `FEAT_LRCPC2`, `FEAT_CSSC`, `FEAT_HBC`, `FEAT_FPAC`.

- **FPCR.DN (default NaN).** `gv_set_zeroing_denormals` (`util/simd.hpp:565–597`) sets only FZ (bit 24) on ARM. The PPU vector path pays for `VecHandleNan` (`fcmp uno` + select) on every VMX float result when `ppu_fix_vnan` is on. Setting FPCR.DN=1 on PPU threads gives VMX's "NaN results are the default NaN" behaviour for free. Caveat: scalar FPU code needs NaN propagation, so DN must only be on while vector code runs, or the scalar FPU path must avoid relying on propagation. Gate it behind the same option and verify with the existing PPU float tests.
- **FEAT_AFP (FPCR.AH / FIZ / NEP).** AH gives x86-style FMIN/FMAX and NaN behaviour, which RPCS3's SPU xfloat paths copy from SSE with extra compare/select. NEP keeps upper lanes on scalar ops. These features were added for x86 emulators. **Risk:** LLVM assumes IEEE semantics for the `minnum`/`maxnum` it emits, so AH can only be on around hand-emitted intrinsics or inline asm. This is research, not a quick win.
- **FEAT_RPRES.** With AH=1, `FRECPE`/`FRSQRTE` give 12-bit estimates instead of 8-bit. That is closer to SPU `FREST`/`FRSQEST` and PPU `vrefp`/`vrsqrtefp`, and could remove a Newton step in "approximate" accuracy modes.

**Expected gain.** Small (a few percent in VMX/xfloat-heavy code) and only after accuracy validation.

---

## 4. Checked and not gaps

| Suspected gap | Result |
|---|---|
| LLVM picks a generic/`cortex-a78` CPU on new Apple chips | **No.** `jit_compiler::cpu()` falls back to `fallback_cpu_detection()` (`cortex-a78`) only when LLVM returns `"generic"`. LLVM's macOS `getHostCPUName()` reads `hw.cpufamily` and **defaults to the newest Apple core it knows**; it never returns `generic`. LLVM 22 maps M5/M5 Pro (`CPUFAMILY_ARM_SOTRA` = `0xf76c5b1a`, this machine) to `apple-m5`, which includes LSE/LSE2. Bundled LLVM is 22.1. |
| 16-byte atomics | Native LSE2 single-copy atomicity is already used; the branch added a lowering micro-win. |
| x86 TSO / memory ordering | Not needed. The JIT emits explicit barriers, and guest PPC/SPU are weakly ordered anyway. |
| TSX / RTM | Removed upstream (`SPULLVMRecompiler.cpp:5502` hardcodes `/*!g_use_rtm*/ true`). No Apple disadvantage. |
| SPU tiering | Upstream has interpreter → fast LLVM → full LLVM with background `spu_llvm_worker` threads. The SPU ASMJIT tier is x86-only, but LLVM covers it. |
| SPU SIMD coverage | NEON fallbacks exist for every SVE2/AVX-512-only path (for example, MPYA via `smull` + shuffle). `dotprod`/`i8mm` are used for GB/GBB/GBH/SUMB. Apple has no SVE, so the SVE2 paths never run; the NEON paths are fine. |
| `cmp_rdata` on ARM | Already NEON-vectorised (`vmlaq_s16` accumulation). |
| SME for SPU | Usable in user space (see §2.3), but a poor fit for 128-bit SPU ops. Not a gap. |

---

## 5. Recommended plan

| # | Item | Effort | Impact | Upstreamable |
|---|---|---|---|---|
| 1 | Game Mode plist keys + native-fullscreen check (§3.3) | Hours | S | Yes |
| 2 | 128-byte padding for reservations/wait engine on Apple (§3.4) | 1 day | S/L | Yes |
| 3 | Role-based QoS, default on Apple; drop RR 99 (§3.2) | 2–3 days | S | Yes, with data |
| 4 | Replace hot-path `yield` with ulock wait / bounded `wfe` (§3.5) | 2–4 days | S | Yes |
| 5 | Apple-aware thread-count defaults (§3.6) | 1 day | S | Yes |
| 6 | Correct A.11 in the design doc (§2.3) | Minutes | — | — |
| 7 | Hook the region scan into `PPUTranslator`, then block linking + RSB (§3.1) | Weeks–months | **L** | Yes, large review |
| 8 | Fault backpatching + 16 KB texture-cache policy (§3.7) | 1–2 weeks | C/S | Yes |
| 9 | FPCR.DN for VMX; AFP/RPRES research (§3.8) | 1 week + validation | Minor | Maybe |

Do items 1–6 first. They're cheap, independent, and make later measurements cleaner because scheduling noise won't hide PPU gains. Item 7 is the real performance project.

---

## 6. Small PRs to send upstream

Each PR below is independent, touches one area, and can be reviewed in one sitting. They are ordered by how easy they are to get merged. All line numbers are `upstream/master`.

| # | PR title (suggested) | Files | Size | Evidence to attach |
|---|---|---|---|---|
| 1 | macOS: declare game category and Game Mode support | `rpcs3/rpcs3.plist.in` | 4 lines | Screenshot of the Game Mode menu-bar icon in fullscreen; Apple's Game Mode docs |
| 2 | JIT: invalidate the instruction cache after code is written on ARM | `Utilities/JITLLVM.cpp` (`finalizeMemory` at `:373`, `:435` currently `return false;`), `Utilities/JITASM.cpp` (asmjit commit, ~`:323-336`) | ~15 lines | Apple's "Porting just-in-time compilers to Apple silicon" (asks for `sys_icache_invalidate`); the risk is real once memory is reused (decommit + recommit, chunk reuse). Use `sys_icache_invalidate` on Apple, `__builtin___clear_cache` elsewhere. |
| 3 | vm: don't clobber an outer W^X scope when resetting JIT memory | `rpcs3/util/vm_native.cpp:341-359` (`handle_jit_memory_reset`) | ~10 lines | The function forces `pthread_jit_write_protect_np(true)` on exit. If a caller already enabled writes, it silently re-protects mid-scope. Save and restore the previous state, or use a nesting counter like the branch's `jit_write_guard` (`cb45f4cb4`, `0a80fff95`, `14350dea3`). |
| 4 | vm: use a 128-byte reservation stride on Apple silicon | `rpcs3/Emu/Memory/vm.cpp:61`, `vm_reservation.h:30` | ~10 lines | `sysctl hw.cachelinesize` = 128; adjacent guest lines currently share one host line (§3.4). Before/after reservation-miss counters from one SPURS title. |
| 5 | atomic: align wait-engine slots to the host cache line | `rpcs3/util/atomic.cpp:172` (`cond_handle`), `:641` (`root_info`) | ~5 lines | Same reasoning as PR 4. Can be folded into PR 4 if reviewers prefer one "128-byte line" PR. |
| 6 | macOS: stop forcing SCHED_RR 99 on every thread | `Utilities/Thread.cpp:2780-2805`, `set_native_priority` ~`:3879` | ~40 lines | Keep QoS `USER_INTERACTIVE` for PPU/SPU/RSX, `UTILITY` for everything else, no `SCHED_RR`. Attach an Instruments System Trace before/after on a 4-P-core Mac, with 1% lows. **Expect pushback without frame-time data.** The fuller role table (§3.2) can follow. |
| 7 | macOS: log P/E core counts and default LLVM compile threads to the E-core count | `Emu/CPU/Backends/AArch64/AArch64Common.cpp`, compile-thread default | ~20 lines | Boot log lines; compile-time vs stutter numbers. |
| 8 | util: match 16-byte atomic sequences to clang's LSE2 lowering | from branch commit `e2b1abaf1` (atomics part only) | small | The branch's microbenchmark (load 1.42 → 0.48 ns). Split out the wait-engine bound changes as a separate PR. |
| 9 | Replace `yield` in SPU wait loops with a bounded spin + atomic wait | `rpcs3/Emu/Cell/SPUThread.cpp` (9 sites) | medium | One PR per file, starting with SPU. Frame-time captures, because the gain is in stalls, not average FPS. |

**Not small, so propose these as an issue or discussion first:** PPU region translation and block linking (§3.1), fault-site backpatching (§3.7), the 16 KB texture-cache protection policy (§3.7 and the Metal doc's F6), the sub-page guard (#19881 territory), and the AFP/RPRES float-mode work (§3.8).

**Already handled upstream, so don't send:** the macOS decommit race (upstream `fb6737b1c`, "MacOS/Arm: Try to fix memory_decommit() race condition", plus `8d6b42e78` for reserve). The branch's `5cb0b8ca3`/`9904492c4` take a different approach (`mprotect` + `MADV_FREE_REUSABLE`). Only send it if you can show the upstream version still races or is slower (upstream's fallback `memset`s the whole range).

---

## 7. How to measure (needed before claiming any gain)

The branch's biggest gap is measurement, so every item above should ship with:

1. **Test machines:** one 4-P-core machine (M1/M2/M3/M4 base, or A18 Pro) and one Pro/Max. Results differ completely between the two.
2. **Titles:** one PPU-bound (for example, Demon's Souls or a GTA IV opening), one SPURS-heavy (Uncharted 2/3, Killzone 2/3), one RSX/texture-cache-heavy (Ratchet & Clank Future).
3. **Metrics:** average FPS, **1% and 0.1% lows**, frame-time histogram. Also the RPCS3 perf overlay counters (PPU/SPU utilisation, reservation misses), plus `powermetrics --samplers cpu_power` to see P/E residency.
4. **A/B discipline:** same cache state, so delete or warm the PPU/SPU caches on purpose. Run 3+ times, report median and spread.
5. **Instruments:** "Game Performance" and "System Trace" templates. These show thread state, priority, QoS, and which core each thread ran on, which directly confirms or rules out §3.2/§3.5/§3.6.

---

## 8. Confidence

- **High:** §3.2, §3.3, §3.4 are defects as described (code and Apple docs checked). §4 non-gaps verified in code; LLVM CPU detection verified against LLVM's `Host.cpp` and this machine's `hw.cpufamily`. A.11 is wrong (probe run here).
- **Medium:** the size of §3.1's gain (based on other emulators' experience, not RPCS3 data). §3.5's frame-time effect.
- **Low/unknown:** FPS impact of §3.4, §3.6, §3.8 on real titles. None of it was measured, because nothing could be built or run here.
