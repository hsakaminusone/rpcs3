# Measurements — 2026-11-12

Machine: Apple A18 Pro, 2P+4E, 8 GB, macOS 27.2 (26B123), arm64, 16 KB pages, 128 B cache lines.
Toolchain: Homebrew LLVM 22.1.8 (`/opt/homebrew/opt/llvm@22`), host CPU `apple-m4`.

All raw outputs are in this directory. Every number below is reproducible with the
commands recorded next to it.

## 1. QoS vs `SCHED_RR` (thread policy)

Source: `qos.cpp`, `place.cpp` (copies kept in this directory).

### 1.1 What RPCS3 does today

`Utilities/Thread.cpp:2854-2856` sets `QOS_CLASS_USER_INTERACTIVE`, then calls
`pthread_attr_setschedpolicy(SCHED_RR)` and `pthread_attr_setschedparam(priority 99)`.

Measured on macOS 27.2:

```
main default                 qos_ret=0 qos=USER_INTERACTIVE(33) rel=0 | sched_ret=0 policy=SCHED_OTHER prio=31
attr sets: qos=0 policy=0 param=0
create=0
rpcs3-style thread           qos_ret=0 qos=UNSPECIFIED/OTHER(0) rel=0 | sched_ret=0 policy=SCHED_RR prio=99
qos-only thread              qos_ret=0 qos=USER_INTERACTIVE(33) rel=0 | sched_ret=0 policy=SCHED_OTHER prio=31
qos-initiated before         qos_ret=0 qos=USER_INITIATED(25) rel=0 | sched_ret=0 policy=SCHED_OTHER prio=31
pthread_setschedparam ret=0
qos-initiated after schedparam qos_ret=0 qos=UNSPECIFIED/OTHER(0) rel=0 | sched_ret=0 policy=SCHED_OTHER prio=47
```

Conclusions:

- The QoS class really is **unset** (`UNSPECIFIED(0)`) after the RPCS3 sequence.
  This is not header folklore; it is measured.
- `pthread_setschedparam()` on an already-running QoS thread also unsets QoS,
  and lowers the effective priority to 47 (max for `SCHED_OTHER`).
  `thread_ctrl::set_native_priority()` (`Utilities/Thread.cpp:3915-3946`) does this
  on every non-Windows platform, including macOS, so `scoped_priority` is destructive
  on Apple.
- `SCHED_RR` + priority 99 is accepted without privileges.

### 1.2 Throughput: `SCHED_RR` is not slower

8 threads, 2 s of a dependent-multiply chain each, `place.cpp` / `place2.cpp`:

```
2 threads RPCS3-style      total=  455.08 Miters/s  min= 227.54  max= 227.54
2 threads UI               total=  415.76 Miters/s  min= 207.62  max= 208.14
8 threads RPCS3-style      total= 1461.19 Miters/s  min= 176.69  max= 184.55
8 threads UI               total=  929.04 Miters/s  min= 113.77  max= 119.01
8 threads DEFAULT          total=  957.87 Miters/s  min= 118.49  max= 121.63
8 threads UTILITY          total=  775.42 Miters/s  min=  78.12  max= 217.06
8 mixed 2UI+6UTIL          total=  967.31 Miters/s  min=  83.89  max= 228.07
```

A second, cleaner run that isolates policy from QoS (`place2.cpp`, same binary warm):

```
8 SCHED_OTHER default prio   total= 1143.47 Miters/s  min= 137.36 max= 156.76  qos=21 pol=1 prio=31
8 SCHED_RR 99                total= 1798.83 Miters/s  min= 219.68 max= 237.50  qos=0  pol=2 prio=99
8 QoS UI                     total= 1179.65 Miters/s  min= 145.75 max= 148.90  qos=33 pol=1 prio=31
8 SCHED_RR 99 (again)        total= 1790.97 Miters/s  min= 221.77 max= 227.54  qos=0  pol=2 prio=99
```

This corrects the first interpretation: the win belongs to `SCHED_RR`/99 itself, not
to "having no QoS". `SCHED_OTHER` and QoS are within noise of each other (1143 vs 1180),
while `SCHED_RR` is ~55% faster than both. Absolute throughput also varies ~20% between
runs (1461 vs 1799 for the same configuration), so **only within-run ratios are valid**;
the doc should not quote absolute Miters/s as if they were stable.

Interpretation:

- With 8 runnable threads on 6 cores, `SCHED_RR` at 99 delivers ~300 Miters/s per core,
  versus ~190 for `SCHED_OTHER`/QoS. It behaves like a low-preemption scheduler.
- This is a synthetic worst case (no blocking, no latency requirements). It destroys the
  assumption that "QoS will obviously be faster", but it does **not** prove `SCHED_RR`
  is better for a real emulator.
- The case for changing thread policy must be **latency, fairness, and audio/UI
  responsiveness**, not raw throughput. Any QoS PR needs an A/B on real titles and
  must keep `SCHED_RR` behind a flag.

## 2. `.stack_sizes` on the JIT path

Source: `sstest.cpp` (section receipt), `sstest2.cpp` (end-to-end decode/relocation), `mstore.ll`, `ss.c`.

`llc -stack-size-section` emits `.stack_sizes` **for ELF only**. Mach-O (`arm64-apple-macos`)
does not get the section at all:

```
ss.o: file format mach-o arm64
Sections: __text, __literal16, __compact_unwind      # no __LLVM,__stack_sizes
```

The RPCS3 JIT compiles with triple `aarch64-unknown-linux-android`
(`Utilities/JITLLVM.cpp:656,673`), so MCJIT emits an **ELF** object and the section
does exist. With `TargetOptions::EmitStackSizeSection = true` and
`ExecutionEngine::setProcessAllSections(true)`, the section is delivered to the custom
`RTDyldMemoryManager`:

```
CODE   name='.text' size=32 align=4
DATA   name='.stack_sizes' size=30 align=1 ro=1
DATA   name='.eh_frame' size=80 align=8 ro=1
DATA   name='.rela.stack_sizes' size=24 align=8 ro=1
...
```

Format, decoded from the **raw object** (`llvm-objdump -s -j .stack_sizes ss_elf.o`) —
the address is still a relocation target here:

```
0000 00000000 00000000 c0090000 00000000
0010 000000
```

`[u64 address][ULEB128 frame_size]` repeated. `c0 09` = `0x4c0` = 1216 bytes, which is
exactly the `int a[300]` frame of `big()`. The trailing `0x30` = 0 is **padding**, not a
real function.

After `RuntimeDyld` applies the `R_AARCH64_ABS64` relocations, `sstest2.cpp` receives
30 bytes in `allocateDataSection`:

```
00 c0 24 02 01 00 00 00  c0 09  00 00 00 00 00 00 ...
addr=0x10224c000         frame=1216   zero padding
```

Two decoding requirements fall out of this: (a) decode **after** relocation, and (b) stop
at the section end and ignore zero-address records — a naive loop reports phantom
`addr=0, frame=0` functions. Full caveat list in the design doc §9.3.1.

**Consequence for the design:** the `.stack_sizes` approach works, but only because the
JIT triple is ELF. It must be documented as an Apple-on-ELF dependency, and the code must
fail loudly if the section is missing (e.g. a future triple change).

## 3. 16-byte atomics (`-mcpu=apple-m4`, LLVM 22)

`llc -mcpu=apple-m4 -O2 mstore.ll`:

```asm
_ldp16:
    ldp  x0, x1, [x0]
    dmb  ishld
    ret

_stp16:
    dmb  ish
    stp  x1, x2, [x0]
    ret

_casp:
    casp x2, x3, x2, x3, [x0]
```

- `apple-m4` really does enable LSE2 (implied by v8.7), so 16-byte loads/stores are
  `ldp`/`stp` plus the required fence.
- 16-byte CAS selects `casp`.
- No scheduler crash despite `WriteAtomic Unsupported` in `CycloneModel`; the model is
  only a scheduling gap, not a correctness one.

## 4. Generic masked load/store on AArch64 without SVE

`llvm.masked.store.v16i8` with a runtime `<16 x i1>` mask lowers to a branch per byte:

```
shl.16b / cmlt.16b / and.16b / ext.16b / zip1.16b / addv.8h / fmov   ; mask -> GPR
tbnz w8,#0 -> str b0,[x0]
tbnz w8,#1 -> mov b1,v0[1]; stur b1,[x0,#1]
... 16 times ...
```

That is **16 conditional branches + 16 stores + 7 mask instructions** for one store,
and a mirrored sequence for a masked load. `PPUTranslator::STVLX`/`STVRX`
(`rpcs3/Emu/Cell/PPUTranslator.cpp:3730,3758`) use `CreateMaskedStore`, so any title that
executes those opcodes pays this.

Static analysis of the decrypted `EBOOT.BIN` of two local titles shows **zero**
`LVLX`/`LVRX`/`STVLX`/`STVRX` occurrences (see §5), so this is a latent code-quality bug
rather than a measured corpus regression. Fix it with a merge-store fast path
(`load aligned 16 -> bsl -> store 16`) or an overlapping-store sequence, not with the
generic masked intrinsic.

## 5. Static PPU instruction mix (real dumps)

`rpcs3 --decrypt` on the local `EBOOT.BIN`s, then
`llvm-objdump -d --triple=powerpc64`. Counts are over the whole file, including any
data that disassembles as code, so treat them as relative weights.

| mnemonic | Demon's Souls | Ridge Racer 7 |
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
| `lwarx`/`stwcx` | 31 / 35 | 0 / 0 |
| `sync` | 1,603 | 1 |
| `lwsync` | 17 | 0 |
| `lvlx`/`lvrx`/`stvlx`/`stvrx` | 0 | 0 |

Implications:

- PPU hot code is dominated by scalar load/store and CR-based branches, not by the
  vector ops the ARM port has spent effort on. `lvx`/`stvx` are 1–2% of instructions;
  `vmaddfp` is ~0.5%.
- `LVLX`/`STVLX` are absent from both titles. Prioritise them below SIMD and scheduling.
- Atomics (`lwarx`/`stwcx`) are rare in PPU code; the atomic cost that matters is in
  RPCS3's own queues, not guest code.
- A18 Pro has no `dcbz`-equivalent problem; `dcbz` did not appear in the top list.

## 6. Spin waits: WFE does **not** park on macOS 27.2

Sources: `wfe3.cpp`, `wfecount.cpp`, `oswait.c`.

This is a correction to the folklore in #18151. That issue measured WFE near idle
current on an older machine; on the reference machine (A18 Pro, macOS 27.2) user-space
`wfe` **burns a full core**.

Pure loop, 500 ms wall, `getrusage(RUSAGE_SELF)` and `ps %cpu` agree:

| Primitive | Rate | CPU time / 500 ms | `ps %cpu` |
|---|---:|---:|---:|
| `isb` | 38.9 M ops/s | 0.498 s (100%) | 97.8% |
| `yield` | 58.0 M ops/s | 0.498 s (100%) | — |
| `wfe` (no `sevl`) | 0.75 M ops/s | 0.498 s (100%) | 90.5% |
| `wfe` (`sevl` once) | 0.74 M ops/s | 0.498 s (100%) | 95.5% |
| `wfe` (`sevl` each iteration) | 0.75 M ops/s | — | — |

Notes:

- The loop rate above is dominated by the `clock_gettime()` in the loop condition
  (~1.3 µs). A separate inner loop without the clock call shows `wfe` costs ~57 ns
  per instruction, while `isb`/`yield` are ~26 ns. Either way the core stays busy.
- Wake latency (producer store → consumer observes) is sub-microsecond for all three:
  `isb` p50 0 ns / p99 1 µs, `yield` p50 0 / p99 1 µs, `wfe` p50 1 µs / p99 2 µs.
  WFE is *not* faster here.
- Conclusion: `wfe` is not a power or latency win on this OS. The existing `isb`
  choice (#18151) is correct.

### 6.1 The primitive that actually parks: `os_sync_wait_on_address`

RPCS3 already uses it in `Utilities/sync.h:90`. Measured:

```
os_sync wake ns: p50=9000 p90=12000 p99=150000 max=559000
os_sync idle-wait CPU over 500 ms wall: 0.0000 s
```

- Median wake latency ~9 µs, p99 ~150 µs (scheduler tail).
- CPU while waiting is **zero**. This is the only tested primitive that parks.
- Design consequence: spin with `isb` for a bounded window (tens of microseconds)
  and then park on `os_sync_wait_on_address`, exactly as `Utilities/sync.h` already
  does for futexes. Do not build a WFE-based spinlock.

## 7. Decommit/reset cost on 16 KB pages

Source: `mem.c`. 64 × 1 MB regions, all touched, `phys_footprint` from
`task_info(TASK_VM_INFO)`.

| Strategy | Decommit (64 MB) | Footprint after | Recommit + touch | Race-free |
|---|---:|---:|---:|---|
| `munmap` + `mmap(PROT_NONE)` (current) | 285 µs | 1.0 MB | 3.8 ms | **no** (no `MAP_FIXED`) |
| `mprotect(PROT_NONE)` | 130 µs | 1.0 MB | 2.5 ms | **yes** |
| `madvise(MADV_FREE_REUSABLE)` | 207 µs | 1.0 MB | 2.1 ms | **yes** |

- All three return the physical footprint to the pre-allocation baseline; `mprotect`
  and `madvise` are not more expensive in memory.
- `mprotect` is the cheapest decommit; `MADV_FREE_REUSABLE` gives the cheapest
  recommit. Using both (mprotect then madvise) is a reasonable combination.
- This is the measured justification for removing the `munmap`/`mmap` race: there is
  no performance reason to keep it.
- Caveat: this is a microbenchmark of fully-touched anonymous memory. RPCS3's
  `memory_decommit` is called on guest ranges; the relative ordering should hold, but
  re-measure in situ with `vmmap` on a real title.

## 8. CR / branch codegen

Source: `cr.ll`.

`llc -mcpu=apple-m4 -O2`:

```asm
_cmp_branch:                 ; compare + branch, one CR bit
    add  x8, x2, x3
    sub  x9, x2, x3
    cmp  x0, x1
    csel x0, x8, x9, lt
    ret

_cmp_fullcr:                 ; full CR field (lt/gt/eq) stored, branch on lt
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

LLVM fuses `cmp` + branch when only one CR bit is live. The cost appears when the CR
field is fully materialized: 1 `cmp` + 3 `cset` + 3 `strb`.

RPCS3's translator makes that the common case, not the exception:

- `PPUTranslator::SetCrFieldSignedCmp` (`PPUTranslator.cpp:5248`) always creates
  three `icmp`s and writes four separate i8 locals (`m_cr[]`).
- `PPUTranslator::UseCondition` (`:845`) calls `FlushRegisters()` **before** the
  conditional branch, so every dirty CR byte is stored to the `ppu_thread` struct
  before each branch. The successor blocks are tail calls, so LLVM cannot prove the
  stores dead.
- `GetCrb` (`:5214`) is one i8 load from the struct when the bit was produced in a
  previous block.

So a `cmpw` + `bc` sequence at a block boundary pays up to 4 byte stores + the
compare, plus a load in the next block. On Demon's Souls this path is reached ~341 k
times statically (`bt`/`bf`), the largest control-flow cost in the corpus.

Hypothesis to test (not yet a promise): pack the eight CR fields as eight `u32`
values so a flush is one 4-byte store per field, and teach the branch path to use
`ccmp`/`csetm` for multi-bit conditions. Expected win: 1–4% PPU-bound, needs the
codegen-count harness before it becomes a PR.

## 9. Masked store: 31 branches vs 6 instructions

Source: `mstore2.ll`.

Current `CreateMaskedStore` (what `STVLX`/`STVRX` use): the `_masked` function in
`mstore2.ll` compiles to **31 conditional branches** (`tbnz`/`tbz`) plus 16 stores.

Read-modify-write replacement:

```asm
_merge:
    shl.16b  v1, v1, #7
    cmlt.16b v1, v1, #0
    ldr      q2, [x0]
    bif.16b  v0, v2, v1
    str      q0, [x0]
    ret
```

6 instructions, 0 branches. `bif.16b` is the byte-select; the byte mask comes from
the existing `shl`/`cmlt` pair. On an aligned base this is safe because the
read-modify-write covers exactly the 16 bytes that the vector store would touch.

## 10. Cache keys and codegen-environment coherence

Read from this tree, not from issues.

**PPU** (`PPUThread.cpp:5816`):

```
ppu-<sha1 of EBOOT>/v9-kusa-<hash16>-<settings bitfield>-<resolved CPU>.obj
```

- Includes the resolved CPU (`jit_compiler::cpu()`, which returns `apple-m4` here) and
  the settings bitfield, so a cache built with different feature or accuracy settings
  is not reused.
- Does **not** include the LLVM version. That is acceptable for machine-code objects,
  but any change to IR generation must bump `v9`. **The Apple work adds new IR**
  (frame checks, masked-store fast path, CR packing), so its first PR must bump the
  prefix; otherwise a stale object from a previous build is loaded silently.
- `jit_compiler::load_object()` is called only by the PPU path.

**SPU** (`SPUCommonRecompiler.cpp:855`, `SPULLVMRecompiler.cpp:1733`):

- The persistent cache is `spu-<block_size>-v1-tane.dat`, containing **raw guest
  code** (`spu_program`), not compiled artifacts. It is keyed only by block size and a
  hardcoded version; that is correct because it is input data, not codegen output.
- The LLVM object directory `llvm/` is written **only when `g_cfg.core.spu_debug` is
  on**, and RPCS3 never calls `load_object()` for SPU. It is a debug dump, not a
  functional cache. There is no SPU stale-object risk to fix.

Conclusion: the earlier hypothesis that SPU caches are missing a codegen-environment
key was wrong. The actionable item is the PPU version bump.

## 11. GHC stack frames: LLVM really does write past `sp`

This closes the "is the 8 KB gateway scratch actually the mechanism?" question with a
reproducer rather than an inference. `ghccc.ll` / `ghccc2.ll` declare `define ghccc`
functions, which is what RPCS3 emits. `llc -mtriple=aarch64-unknown-linux-android
-mcpu=apple-m4 -O2` produces:

```
big:
        mov     w8, #1
        str     w8, [sp]           ; no prologue, no sub sp
        mov     w8, #2
        str     w8, [sp, #11996]   ; 12 KB frame, written above sp
        mov     x0, sp
        bl      g
        ret
```

A 1200-byte alloca behaves the same way: `str w8, [sp, #28]` with no `sub sp`. Under the
GHC calling convention LLVM treats the stack as caller-owned and does **not** allocate a
frame, so every byte of spill/alloca space is written at a positive offset from the `sp`
the gateway provided. The SPU gateway reserves 8192 bytes, so a 9.8 KB or 10.7 KB function
writes 1.6–2.5 KB into the caller's stack — exactly the corruption in #19939.

This also rules out the alternative hypothesis ("LLVM allocates its own frame, so the
gateway slab cannot overflow"). It does not. The fix must either enlarge the slab or give
the oversized function its own `sp` via a thunk, which is what §9.3 proposes.
