# macOS Apple Silicon "alpha" survey — open issues ↔ M-series changes (2026-10-16)

Scope: upstream `RPCS3/rpcs3`. Two datasets: (A) **66 open, mac-relevant issues**
(Oct 2022 → today), Apple-Silicon-only focus; (B) **94 commits in the last 12
months** whose message is mac/ARM-related on `upstream/master`.

Method: issues = label `OS: macOS` (37) + dedup of 5 content searches
(`macOS in:title`, "Apple Silicon", "Apple M", MoltenVK, aarch64|arm64) → 77
unique → 66 mac-relevant. 55 are curated into the clusters below; the remainder
are generic macOS items (cursor invisibility, audio devices, savedata dialogs,
cursor/camera) where the Apple-Silicon axis is not the deciding factor. Apple-Silicon classification is heuristic (keywords in
title/body: Apple Silicon, Apple M1–M4, aarch64, MacBook/Mac mini/Mac Studio,
MoltenVK, Metal); items marked `BOTH?` mention Intel too. Commit set = git grep
on message (mac|osx|darwin|metal|moltenvk|apple|M1-4|aarch64|arm64, -i), 2025-10-16 → 2026-10-16.

---

## Part A — Open macOS issues, grouped by failure cluster

### A1. Rendering corruption on MoltenVK (tile-GPU / TBDR hazards) — 12 open
The biggest visible cluster. All are Vulkan-via-MoltenVK symptoms; none exist on
Windows/Linux Vulkan drivers.

| # | opened | cm | AS | title (compressed) |
|---|--------|----|----|--------------------|
| 19815 | 2026-10-12 | 1 | ✓ | BioShock Infinite: flickering black blocks on moving objects — **Z-prepass + LEQUAL pass over same depth, translated oddly** |
| 19814 | 2026-10-12 | 5 | ? | BioShock Infinite: dark colours turn green — colour-grading LUT, `fract()` of an interpolated coordinate (precision on assertive translation) |
| 19791 | 2026-10-10 | 0 | ✓ | Fallout: New Vegas: shadow flickers in square blocks — user clip planes + LEQUAL re-draw over same buffers |
| 19700 | 2026-10-04 | 2 | ✓ | [MoltenVK] Batman Arkham Origins: UE3 post-process smearing, horizontal streaks |
| 19615 | 2026-09-28 | 2 | ✓ | aliased MRT slots attach same image twice → tile mosaic on **tile-based GPUs** (Far Cry 3) |
| 19434 | 2026-09-08 | 6 | ✓ | TLoU: black squares — **reporter confirms NOT present on KosmicKrisp builds (#17902)** |
| 18563 | 2026-04-12 | 3 | ✓ | NBA 2K11: vertex explosions / texture stretch arm64 |
| 18551 | 2026-04-11 | 3 | ✓ | LBP2 stickers regression (renders wrong on current builds) |
| 18273 | 2026-03-04 | 3 | ✓ | LBP pixelation + missing stickers (M1) |
| 18694 | 2026-05-08 | 11 | ? | God of War Ascension: FidelityFX artifacts [Regression] |
| 16534 | 2025-01-09 | 10 | ✓ | Scott Pilgrim: black screen |
| 16424 | 2024-12-18 | 11 | ✓ | Simpsons Game menu doesn't work |

Key evidence: **#19434 — TLoU black squares appear under MoltenVK but not under
KosmicKrisp.** First field example of KK already beating MVK on a real title.

### A2. ARM64 JIT correctness (SPU/PPU LLVM) — 7 open, incl. cross-platform
| # | opened | cm | AS | title |
|---|--------|----|----|-------|
| 19790 | 2026-10-10 | 0 | ✓ | MGS V SPU crash: ARM64 code-verification checksum can't detect equal shifts (regression 35f65c224). Fine under Rosetta. |
| 19609 | 2026-09-28 | 1 | ✓ | SPU LLVM AArch64 GHC frame pass skips recompiled modules → spin forever (Far Cry 3 load hang) |
| 18501 | 2026-04-03 | 9 | ✓ | crash after loading any game (arm64 *Windows*) — same ARM64 JIT surface |
| 18530 | 2026-04-07 | 16 | ✓ | crash loading firmware/any game — M3 Pro, **macOS 26.4+** |
| 18828 | 2026-06-02 | 4 | ✓ | TLoU non-deterministic hard freeze (RSX/SPURS) linux-aarch64 |
| 19340 | 2026-08-28 | 0 | ✓ | preferred SPU Threads Auto stutter (linux aarch64 SDX2E) |
| 15861 | 2024-07-31 | 12 | ✓ | Motorstorm illegal instruction (macOS) |
| 16484 | 2024-12-31 | 34 | ✓ | **META: list of games broken on ARM64 but fine on x64** — the acceptance yardstick |

### A3. Boot / firmware / gameplay crashes & freezes, macOS 26 era — 10 open
| # | opened | cm | AS | title |
|---|--------|----|----|-------|
| 17640 | 2025-10-29 | 25 | ✓ | Freeze upon game boot — M4 Pro (25 comments, several sub-issues since fixed) |
| 17659 | 2025-11-02 | 13 | ✓ | still crashes on any game, arm macOS (related 17640/17530) |
| 17689 | 2025-11-13 | 2 | ✓ | crash after loading PPU and SPU — **macOS 26.1** |
| 16993 | 2025-04-03 | 12 | ✓ | crash on firmware install |
| 16454 | 2024-12-24 | 4 | ✓ | firmware install failed, MBP M1, macOS 15.2 |
| 16884 | 2025-03-17 | 3 | ✓ | all games and firmware crash on M1 |
| 19773 | 2026-10-08 | 2 | ✓ | crash when quitting without launching a game |
| 19487 | 2026-09-14 | 1 | ✓ | full app freeze creating any lobby (Minecraft PS3, RPCN) — macOS/AS only |
| 18744 | 2026-05-15 | 2 | ✓ | Sacred 2 stuck on loading screen |
| 16435 | 2024-12-20 | 10 | ✓ | LEGO Indiana Jones 2 / LEGO Star Wars III stuck after boot |

### A4. Atomics, threading, locks/perf — 4 open
| # | opened | cm | AS | title |
|---|--------|----|----|-------|
| 19759 | 2026-10-07 | 1 | ✓ | 16-byte compare_exchange still LL/SC loops; RSX offload contention halves GTA IV fps (13-30 vs 60) |
| 18429 | 2026-03-22 | 3 | ✓ | ASan stack-buffer-underflow `atomic_wait_engine::notify_all` (M3 Max) — heap corruption in normal play |
| 18769 | 2026-05-20 | 7 | ✓ | hanging SPU compilation threads ("is too sleepy") — dedicated spin-off of #17640 |
| 18347 | 2026-03-14 | 0 | ✓ | NHL 14: L1 soft-freeze (input-driven game-state freeze) |

### A5. ZCULL / report-path (RSX core, surfaces on macOS first) — 3 open
| # | opened | cm | AS | title |
|---|--------|----|----|-------|
| 19010 | 2026-07-09 | 0 | ✓ | Uncharted 2: RSX-thread segfault `NV4097_GET_REPORT`, deterministic (AS + Rosetta) |
| 19610 | 2026-09-28 | 2 | ✓ | ZCULL declined late fault deadlocks RSX thread on **16K-page hosts** (GTA IV freeze) |
| 17961 | 2026-01-04 | 5 | ✓ | Uncharted 2 crashes (macOS) |

### A6. Input / SDL on macOS — 6 open
| # | opened | cm | AS | title |
|---|--------|----|----|-------|
| 19844 | 2026-10-16 | 0 | ✓ | DJ Hero turntable seen as Guitar Hero 5 controller (SDL) |
| 19717 | 2026-10-05 | 0 | ? | PDP RiffMaster not detected as SDL controller |
| 17038 | 2025-04-13 | 4 | ✓ | PS2 wired Buzzers visible but dead |
| 19181 | 2026-08-09 | 5 | ✓ | DS3 missing hidapi calls |
| 17174 | 2025-05-06 | 3 | ✓ | DS4 bluetooth crash on GOW HD Collection |
| 15819 | 2024-07-21 | 8 | ✓ | mouse in fullscreen regression |

### A7. macOS UX / lifecycle / config — 9 open
| # | opened | cm | AS | title |
|---|--------|----|----|-------|
| 13319 | 2023-01-29 | 13 | ✓ | saves land on system drive even when app on external drive |
| 16734 | 2025-02-20 | 8 | ? | multiple microphone/audio device support |
| 17348 | 2025-06-30 | 6 | ? | invisible cursor regression |
| 15092 | 2024-01-25 | 7 | ? | crash on savedata dialogs |
| 15196 | 2024-02-11 | 8 | ✓ | no fine-grain resolution-scale-threshold control |
| 19037 | 2026-07-15 | 4 | ✓ | Disgaea D2 missing text |
| 17228 | 2025-05-20 | 2 | ✓ | Bakugan audio out of sync |
| 11421 | 2022-01-21 | 4 | ✓ | DiRT: error reading disc |

### A8. Meta / strategy
| # | opened | cm | title |
|---|--------|----|-------|
| 11414 | 2022-01-20 | 63 | **META: macOS Vulkan issues** — historical MVK landmine ledger (swizzle workarounds vs new MVK, etc.) |
| 19369 | 2026-08-31 | 3 | ARMSX3 portable ARM64/Vulkan optimization proposals |
| 16390 | 2024-12-10 | 3 | Snapdragon X Elite (Oryon-1) build request — perf-heterogeneity driver |

Excluded per scope: Intel-only reports (#12404 iGPU segfault, #951d… etc.).

---

## Part B — 94 mac/ARM commits on upstream/master, last 12 months, by bucket

Top authors of the mac set: **schm1dtmac 31, Malcolm 11, Megamouse 9, Elad 9**,
then kd-11, FlexBy420, qurious-pixel, RipleyTom, others 1-3. 30 of 94 are
ARM-only concerns. Notably upstream already contains
`b76ca8807 (macOS) Support loading VK ICDs (in preparation for KosmicKrisp)` —
KK groundwork is flowing to upstream through schm1dtmac, who is the same author
as draft PR #17902.

### B1. ARM64 JIT correctness/perf (≈32)
`be39b3b9f` atomic LSE2 runtime detect · `615f529e6` force LSE2 Apple AArch64 ·
`e13ee1579`/`6161ecd7a` stop inverting float-to-int saturation (SPU/PPU) ·
`ec4b1ae65` TBL byteswaps · `2d1be0918` TBL ROTQBY · `4542020c8` UDOT SUMB ·
`a87d17529` TBL2 scavenger retry · `320e8d634` FCGT→BSL workaround ·
`21d533675` NaN ruling-out disabled (Reduced Loop) · `d25972e19` advertise i8mm ·
`b2469039a` runtime ARM feature detection into LLVM attributes ·
`6647c5a2b` fallback SPU decoder for unsupported DB · `5e8ba021a` RawSPU MMIO ·
`5758ab11f` exclusion_callback type · `6a398f994` arm64 build fix.
**Perf-critical:** `0f85e9123` busy_wait scaling to ARM timer frequency
(27→37 FPS Metal Gear Rising, "almost all games benefit"), `eaebd3426` ARM FMA
unconditional, `93fd33a19` ISB instead of YIELD, `7cfe96a1d`
hw.optional.neon fallback.
**JIT/W^X/memory:** `6850e2e5a` W^X RAII consolidation, `f42b09d1f` SPRX MAP_JIT
exclusion fix, `88f81381e` jit_write_protect recovery, `5bbce42ad` MAP_JIT
decommit leak, `11b57268e`/`99d15190` symbol resolver + write-protection
guards, `4cac76caa`/`f3cf1da7b`(revert)/`e1734b51c`(superseded by
`3c9a4417f` revert) decommit-race churn, `e075170cc` JITASM mutex.

### B2. macOS threading/atomics (≈4)
`1147f47a8` futexes switch, min ver 14.4 · `5507078bd` native `os_sync` ·
`3819b9d57` force max pthread priority (anti-throttling).

### B3. MoltenVK / RSX-GPU surface (≈10)
`4c7d19fc6` MVK 1.4.1 + private API (fixes panics) · `8a9b15dee` MVK 1.4.0 ·
`155883ea2` MVK bump + Game Mode restore · `ee37ef277` MVK 1.4.2 + re-disable
Game Mode · `4ffeee034` Game Mode opt-out (throttling) · `e8961edf4` **remove
Apple-specific format hacks now MVK supports them on Apple GPUs + enable async
compute** · `2a6f1cf35` posix_memalign quirks (kd-11) · **`b76ca8807` VK ICD
loading (KK prep)**.

### B4. macOS 26 / lifecycle / crashes (≈14)
`bd9c4705f` PPU LLVM compile regression hackfix · `45a1da2d8` disable trap ·
`fb1c1eeae` restart-loops fix · `a0f2f006b`/`a0e0a6c6a` app-bundle cleanup on
update · `081c094c5`/`a543f3870` translation + compat/config DB updates ·
`050d2d3b2` Qt translations · `7a12e8066` plist version · `a646a66ea` release
naming · `d9f913016` `utils::bless` ambiguity (#17757).

### B5. Input (≈3)
`8c89adf17` DJ Hero turntable basis (#18957, now partially regressed per
#19844) · `94977d354` arrow-key roundtrip on macOS ARM.

### B6. Build / CI / deploy (≈24, includes FlexBy420's aarch64 deploy setup)
`4b9f2af41` **MacPorts switch after Homebrew "final straw"** (Intel CI concern)
· `e65755d68`/`c83769e79`/`44bd0568e` unified build/deploy/CI scripts ·
`77aa5d4bb` no GL code/GLEW compile-in · `056a8ba1b` brew rpath hack ·
`951d47c94` system protobuf · `f2ac83f5e`/`bc6e01c42`/`84277f41a`/`46ddcc`/
`1d464eeab` mac build repairs · `ec70c9691` ccache · `860e2a82b` runners on OS
15 · `133b19f20`/`fce393024` Qt 6.9.3→6.10.1 bumps + `7d3cf831d` Qt-6.8+
GUI-freeze fix · `54206c62b` OpenCV.

### B7. Misc (≈8)
`3c2815e89` MAC from PSID · `a99fde6f5` BD drive support · `c19594254`
sys_ss_random_number_generator (#19382) · `eb61fc1fb` ISO-verification crash ·
`aff645272` cyrillic fallback fonts · `9fb7c8f52` multi-selection menu ·
`5ecfc95c1` guest memory dump.

---

## Part C — Cross-analysis: cluster → recent work → residual state

| Cluster (A) | Last-year work (B) | Residual (still open because…) |
|---|---|---|
| A1 tile-GPU rendering | MVK 1.4.x bumps, removal of swizzle/format hacks (`e8961edf4`), async compute | Semantics gaps are *translation-layer* (aliasing, clip planes, LUT `fract()`, prepass depth reuse) — no driver fix; needs backend control of pass structure |
| A2 ARM64 JIT | ~32 commits of SPU/PPU LLVM ARM64 fixes | Regressions keep coming from new optimisations (`35f65c224` checksum); meta #16484 still long |
| A3 boot crashes, macOS 26 | W^X RAII, MAP_JIT exclusions, PPU-LLVM hackfix, trap disable | W^X ↔ LLVM interplay on macOS 26 is still unstable; multiple unresolved reports on 26.1/26.4 |
| A4 atomics/perf | futex switch, os_sync, LSE2, pthread priority | 16-byte CAS remains LL/SC (`19759`), notify_all bug (`18429`), SPU-compile hangs persist |
| A5 ZCULL | (no commit ids obviously targeting it) | RSX-core bugs; macOS users just hit them most (16K pages are Apple default) |
| A6 input | DJ Hero, arrow keys | SDL macOS gaps upstream |
| A7 UX | updater/bundle/DB updates | long-tail |

Two structural observations:

1. **The macOS pain is concentrated exactly where a native backend pays off**:
   translation-layer semantics (A1) + TBDR pass-structure assumptions (A1's
   MRT aliasing, clip-plane re-draws) + first-class 16K-page and W^X
   assumptions leaking out of the JIT (A3). MoltenVK can only paper over the
   first class; it cannot fix the second or third.
2. **`schm1dtmac` is already landing KK preparation upstream** (`b76ca8807` VK
   ICD loading), in parallel with our branch work. Convergence risk/opportunity:
   their draft PR #17902 (MacPorts-based KK CI) vs our from-source build are
   independent targets for the same seam.

## Part D — Implications for our alpha (Metal 4 + KosmicKrisp)

Ranked takeaways:

1. **KK's first proven win already exists**: #19434's TLoU black squares
   vanish on KK builds. Our alpha's headline test should be a controlled A/B
   MVK-vs-KK on the A1 titles (TLoU, LBP2, NBA 2K11, Batman Arkham Origins,
   BioShock Infinite, Fallout NV).
2. **A1 is the design brief for the Metal 4 backend's pass planner**: explicit
   store/load actions per pass, no aliased-attachment ambiguity, honest clip
   planes, and precision-matched `fract()` semantics. Every A1 issue is a
   concrete acceptance case for the LLD-surface / pass-structure work.
3. **A3 (W^X / boot crashes on macOS 26) is the stability blocklist**: our KK
   bundle must not perturb JIT/W^X paths at all; it is driver-plug-in scope
   only. Any KK work touching memory=nx regions is a hard no.
4. **A4 gives the KK perf matrix**: 16-byte atomics and offload-queue
   contention are code-level, not driver-level — KK inherits all B1/B2 fixes
   automatically, which is a talking point but not added work.
5. **A5 ZCULL (16K pages)**: Apple's 16K page size needs to be a first-class
   assumption in our backend resource model (HLD-03 territory).
6. **Acceptance yardstick**: use #16484 (ARM64-broken games list) + #11414
   (historical MVK landmines) as the regression-matrix skeleton; mark each
   entry MVK-status vs KK-status.
7. **Intel is out of scope per decision;** Intel-only reports excluded above.

Caveats: issue classification is heuristic; `BOTH?` rows mention Intel too;
some numbers (19589/18501) are Windows-ARM but share the ARM64 JIT surface.

---

## Part E — Why ARM64 is the macOS bug epicenter (structural analysis)

Short answer: there is no single broken architecture. There are **four structural
mismatches that all land on ARM64 at the same time**, plus one Apple-specific
geometry problem, plus an organisational amplifier. Each is evidenced below.

### E1. The codebase is x86-first; ARM64 is an override layer, not a peer backend

- `rpcs3/util/types.hpp:19,21` define `ARCH_X64` and `ARCH_ARM64`.
- `ARCH_ARM64` occurs **360 times in 35 files**; `ARCH_X86_64` occurs **0 times**
  — x86 is the implicit default path, ARM64 is the `#ifdef` branch.
- Concentration: **108** ARM64 branches in `rpcs3/Emu/Cell/`, **44** in
  `SPULLVMRecompiler.cpp` alone, 36 in `Emu/CPU` + `Emu/RSX`.

Consequence: every shared algorithm has an x86 reference implementation and a
hand-written ARM64 fast path that must stay *semantically* identical with no
mechanism enforcing it. Divergence is therefore a correctness bug generator, and
that is exactly what #19790 is: `35f65c224` ("SPU LLVM: Add ARM checksum UABA
path", 2026-05-26) replaced the SPU **code-integrity checksum** — the thing that
detects modified/stale recompiled SPU code — with a NEON `UABA`/`UABD`
accumulation. `UABD` gives pairwise `|a − b|`, which **cannot distinguish equal
shifts**, so the checksum collides, modified code is not detected, and stale
recompiled code runs → MGS V crashes ~30 s after boot. The x86 path was correct;
the ARM64 fast path was weaker.

Same shape elsewhere:
- `0f85e9123` — `busy_wait` was written for a ~3 GHz x86 timer; ARM hardware
  timers run <100 MHz, so the profiler's top entry was `busy_wait`. "27→37 FPS in
  Metal Gear Rising, almost all games benefit."
- `atomic.hpp` — LSE2 is **partially** adopted: the 16-byte `load()` has an
  `g_atomic_lse2` fast path (`ldp`+`dmb`), but the 16-byte
  `compare_exchange()` is a hand-written `ldaxp`/`stlxp` **LL/SC retry loop**
  with no LSE2 path at all. That is #19759: 16-byte CAS under contention with the
  RSX offload queue livelocks/starves → GTA IV at 13–30 fps instead of 60.

### E2. Apple's hardened-runtime JIT model is structurally hostile to an emulator

An emulator rewrites executable pages constantly; Apple Silicon requires W^X.

- Apple **rejects `MAP_FIXED | MAP_JIT`**, so *atomic* replacement of a JIT page
  is not expressible. The workaround (`4cac76caa`) uses `MAP_FIXED` without
  `MAP_JIT`, which **silently drops the JIT attribute** and leans on the
  `allow-unsigned-executable-memory` / `disable-executable-page-protection`
  entitlements instead. Between the old `munmap`→`mmap` pair another thread could
  claim the range → `Verification failed (object: 0x0)` crashes across PPUW
  threads.
- The area has an unresolved **revert war**: `4cac76caa` (Daniel Risto) →
  `f3cf1da7b` revert (schm1dtmac) → `e1734b51c` (Daniel Risto) → `3c9a4417f`
  revert (schm1dtmac). Two maintainers reverting each other's fixes in the same
  function means there is no agreed model of the page lifecycle on Apple.
- **21** `pthread_jit_write_protect_np` call sites across
  `SPULLVMRecompiler.cpp`, `PPUThread.cpp`, `SPUThread.cpp`,
  `SPUCommonRecompiler.cpp`, with `88f81381e` ("attempt recovery if
  pthread_jit_write_protect_np state is invalid") — the toggle is **per-thread
  state** that must be correct at every JIT write. A mistake surfaces as SIGILL
  or SIGSEGV in guest code, which is precisely #15861 ("Illegal instruction crash
  on Motorstorm") and #17890.

### E3. Host ordering is weaker than the model the code was written under

x86 host and x86 guest are both TSO; the PowerPC guest is weakly ordered, so on
an x86 host many guest orderings are satisfied *by accident* and the JIT/emulator
can be sloppy without failing. ARM64 offers no such accident. There is **no
explicit design record of the ordering contract** in the tree (no TSO/ordering
commentary in `util/`), and the atomics are hand-rolled inline asm (`dmb ish`
sequences in `atomic.hpp`). The failures appear exactly in the layers that
re-implement synchronisation: the futex/`os_sync` migration (`1147f47a8`,
`5507078bd`) and `atomic_wait_engine::notify_all` stack-buffer-underflow /
libmalloc corruption (#18429, M3 Max).

This also explains a recurring red herring: **"fine under Rosetta"** (#19790).
Rosetta executes the *x86* JIT. Such reports say nothing about ARM64 — they say
the x86 backend is the mature one.

### E4. LLVM's ARM64 codegen is immature, and the compensating workarounds are themselves bug sources

Evidence of fighting the backend: TBL2 register-scavenger retries (`a87d17529`),
an inline-asm workaround for LLVM emitting a bad select for FCGT (`320e8d634`),
i8mm advertising (`d25972e19`), heuristic ARM CPU feature detection because ARM
detection is far weaker than x86's (`b2469039a`), and an AppleClang-vs-vanilla-
LLVM macro mismatch for LSE2 (`615f529e6`). Every one of these is ARM64-only, and
each new optimisation can regress — the MGS V crash came from a 2026-05-26
optimisation, i.e. a *new* ARM64 bug from *new* ARM64 work.

### E5. Guest page geometry vs Apple's 16K host pages (Apple-specific)

Guest bookkeeping was designed against 4K pages. Apple Silicon hosts have **16K**
pages, so `mprotect`-based fault tracking has 4× coarser granularity. ZCULL's
report-page state machine uses host `get_page_size()` for protection granularity,
and the "declined late fault" path has no way to observe faults the coarser
granularity hides → RSX-thread deadlock (#19610, GTA IV) and the
`NV4097_GET_REPORT` segfault (#19010, Uncharted 2). This is the one mismatch that
is genuinely *Apple-only* rather than ARM64-generic.

### E6. The organisational amplifier

31 of the 94 mac commits are one author (schm1dtmac); the ARM64 JIT is carried by
kd-11 / Whatcookie / RipleyTom. ARM64-only regressions are reported late and
linger: #16484 ("games broken on ARM64 but work on x64") has been open since
2024-12-31 with 34 comments and keeps growing. x86 absorbs the test mass.

### Verdict

Nothing here says "the emulator is architecturally wrong". It says the ARM64
port carries **five simultaneous penalties** — ISA backend duplication, Apple's
JIT security model, a weaker host memory model, an immature codegen target, and
16K page geometry — none of which x86 pays, all of which are concentrated in
exactly the code an emulator is made of (JIT, page lifecycle, atomics, fault
handling). That is why ARM64 issues dominate the macOS tracker even though
macOS-on-Intel is the same OS.

### What this means for our alpha (constrains Part D)

1. **Do not touch JIT, W^X, page lifecycle, or atomics.** The A2/A3/A4 clusters
   are upstream-track work with an active revert war in progress; a driver-level
   change that perturbs them turns our alpha into a stability liability.
2. **A1 (rendering) is the only cluster that is ours to win**, and it is the
   cluster where the failure cause is *translation semantics*, not ISA or OS.
   KK already has one proven win there (#19434).
3. **E5 (16K pages) does land in our design**: the Metal 4 resource/HLD-03 model
   must treat 16K host granularity as first-class, because it is the one ARM64
   penalty that a new backend can neutralise rather than inherit.
4. **Do not claim ARM64 JIT fixes in the alpha.** The honest alpha promise is:
   same JIT, same atomics, better rendering path and presentation — measured as
   MVK-vs-KK A/B on the A1 title set.

