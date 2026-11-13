# Measurements, 2026-11-13

Two experiments that close Appendix B items 24, 33, and 34 of
`../../01-native-cpu-redesign.md`. Machine: Apple A18 Pro (`Mac17,5`), macOS
27.2 (26B123), 16 KB host pages, Homebrew LLVM 22.1.8.

Sources in this directory:

| File | What it is |
|---|---|
| `blocks.py` | Static basic-block and region analyzer over `llvm-objdump` PPC64 output |
| `blocks-ds.txt`, `blocks-rr7.txt` | Raw output for the two decrypted EBOOTs |
| `fastmem2.cpp` | Fastmem go/no-go prototype (fault, commit, retry, patch, stub) |

## 12. Static block and region shape

Corpus: the decrypted `EBOOT.elf` of Demon's Souls (DS) and Ridge Racer 7
(RR7), disassembled with `llvm-objdump --triple=powerpc64 -d`. Both are the
real shipped executables, not synthetic code.

Method: instructions are grouped into basic blocks at branch targets, at the
instruction after any branch, and at any address gap. Code is then measured
three ways:

1. **Basic block** — straight-line run ending at a branch.
2. **Current inline chain** — what `CallFunction` compiles into one function
   today: follow unconditional `b` only, because a conditional branch ends the
   function (`UseCondition` emits the fall-through call and `BC` emits the taken
   call).
3. **Region** — the design's formation walk. Two variants: with the current
   "any branch target is an entry block" rule, and with an ideal
   multi-predecessor rule. A third, **loop-header region**, starts at every
   back-edge target and absorbs everything reachable inside
   `[header, back-edge]`; that is the shape a loop-first formation would use.

| Metric | DS | RR7 |
|---|---:|---:|
| Instructions | 5,939,384 | 1,065,025 |
| Basic blocks | 1,070,410 | 189,058 |
| Branch density (blocks per instruction) | 11.9% | 12.2% |
| Block length mean / median / p90 / p99 | 5.55 / 4 / 12 / 26 | 5.63 / 4 / 11 / 29 |
| Blocks ≤10 instructions | 65.6% | 65.5% |
| Blocks with >1 predecessor | 8.1% | 7.4% |
| Blocks targeted by a direct branch | 49.4% | 48.9% |
| Current inline chain mean / median / p90 | 6.34 / 4 / 13 | 6.41 / 5 / 13 |
| Region, current entry rule: mean / median / p90 | 8.40 / 6 / 18 | 9.50 / 6 / 20 |
| Region, ideal multi-pred rule: mean / median / p90 | 15.53 / 10 / 32 | 18.55 / 11 / 41 |
| Region ≥40 instructions, current rule | 1.2% | 2.3% |
| Region ≥40 instructions, ideal rule | 6.6% | 10.6% |
| Back-edge branches / forward direct branches | 44.7% | 44.0% |
| Loop body span median / p90 / p99 | 108 / 517 / 1759 | 98 / 668 / 3204 |
| Loop-header region mean / median / p90 / p99 | 29.9 / 13 / 63 / 270 | 33.2 / 14 / 79 / 291 |
| Loop regions ≥40 instructions | 19.8% | 21.8% |
| Loop regions ≥100 instructions | 4.8% | 7.3% |
| Instructions inside some loop region (overlapping) | 55.5% | 54.6% |

### What this says

- **The straight-line region premise is false.** Under the document's own
  entry-block rule, half of all regions are six guest instructions long, and
  98.8% are under 40. Even with an ideal multi-predecessor rule the median is
  10–11. A per-region fixed cost cannot be amortised over that.
- **Loop bodies are the only long unit.** The median back-edge spans 98–108
  guest instructions; 20–22% of loop-header regions absorb at least 40
  instructions, and those regions cover about 55% of the static text. A
  loop-first formation is the only shape that makes region IR pay.
- **The entry-block detector is far too conservative.** 49% of blocks are
  branch targets, but only 7.4–8.1% have more than one predecessor. Using real
  CFG in-degree instead of "any branch target" roughly doubles the median
  region (6 → 10–11) at no correctness cost, because a single-predecessor block
  has exactly one host copy either way.
- **Dispatch is the first-order win, not SSA.** The current compiler emits one
  GHC call-and-entry-check per 4–5 guest instructions. Block linking, an IBTC,
  and an RSB remove that cost without needing long regions.

## 13. Fastmem prototype

`fastmem2.cpp` builds a one-page AArch64 JIT body (`ldr w0,[x0]; ret`), a
64 MB `PROT_NONE` arena, and a `SIGSEGV` handler that commits the faulting
16 KB page with `mprotect` and returns to re-execute the same instruction.

| Measurement | Result |
|---|---|
| First fault → commit 16 KB → retry (n = 4095 pages) | mean 5.6–5.8 µs, p50 5.1–5.3 µs, p90 6.7–7.1 µs, p99 14–17 µs, max 142–178 µs |
| Steady-state bare `ldr` | 1.10–1.15 ns/op |
| Single-site patch: `mprotect` RW + 4-byte store + `mprotect` RX + `sys_icache_invalidate` | mean 3.0–3.3 µs, p50 2.8–3.0 µs |
| Batched patch: one W^X scope, 4096 sites | 5.0–7.2 µs total, 1–2 ns/site |
| Demoted site: `b` → save `x30` → `bl` helper → restore → `ret` | 1.85 ns/op |

### What this says

- **64-bit guest pointers are not the problem.** A fault gives the exact guest
  address and PC; committing one page and re-executing costs ~5.7 µs. That is
  acceptable for lazy commit (once per page) and unacceptable per access, which
  is exactly the T0/T1 promotion rule.
- **Patching must be batched.** One site costs ~3 µs because it pays two
  `mprotect` calls. A scope that covers a batch costs 1–2 ns/site. This
  validates the RAII W^X scope in §8.1.2 and makes PR 3 a prerequisite for the
  multi-site work, not a nicety.
- **The per-site stub must preserve `LR`.** The prototype's first stub used
  `bl helper; ret` and hung forever: `bl` overwrites `LR`, so `ret` returned to
  itself. The stub is entered by a 4-byte `b` from the middle of a region,
  where `LR` still holds the region's return address. It must save and restore
  `x30` (and any other caller-saved register it uses) around the helper call.
- **A demoted site is not free but is cheap.** 1.85 ns against 1.10 ns bare is
  a 0.75 ns penalty, versus 5.7 µs for a re-fault. Patching on the second fault
  is correct policy.

### Caveats

- Static analysis, not dynamic. It measures what code exists, not what runs.
  A loop-first design should still be validated with an executed-block
  histogram during P3 (Appendix B item 34).
- Two executables; the PPU library is not in the corpus. The library is the
  same compiler and the same idioms, but this is not a proof for every title.
- One machine. The absolute fault cost is Apple-silicon-specific; the shape of
  the conclusion (fault ≈ 5000× a load, patch ≈ 2 `mprotect`s) is not.
- The prototype patches a single 4 KB code page. Real code spans many pages and
  the W^X scope must cover the module, not one page.