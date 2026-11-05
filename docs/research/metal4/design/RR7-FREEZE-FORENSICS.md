# RR7 freeze — forensics of one Metal 4 run

Scope: one 18 MB run log, `RPCS3-rr7-video.log` (≈169 k lines), read-only.
Title: Ridge Racer 7 (BLUS30001), 1280×720. Build
`RPCS3 v0.0.43-20383-17c3c332`, renderer Metal 4, Apple A18 Pro, macOS 27.0.1.
Config: Write Color Buffers ON, Emulate BD-ROM/HDD read speed ON, trace channels
`cellVdec`/`cellDmux`/`cellDmuxPamf` at Trace, `MTL_PROBE=5`.
Symptom: the game reached the mode-selection screen; the image then froze.

Confidence tags follow the directory convention: `[V]` verified against the log or
source, `[I]` inference from primary evidence, `[U]` unverified.

Log line shape: `·<level> <timestamp> {<thread>} <message>`. The RSX tag is the **FIFO
GET pointer**, not a thread id: `fmt::format("RSX [0x%07x]", rsx->ctrl ? +rsx->ctrl->get : 0)`
(`RSXThread.cpp:1101`) `[V]`. This single fact drives §4 and §5.

Video-noise filter used throughout: lines containing `cellVdec:`, `cellDmux`, `PAMF
demuxer`, `CPamfDemuxer`, `ATRAC`, `Performance Sensor`.

---

## 1. Summary

1. The last RSX present is at `0:00:37.487259` (drawable `…19800e0`); the last RSX-thread
   draw is at `0:00:37.481` `[V]` (§2). The present pipeline is healthy in the last perf
   report at `0:00:36.250`: `present/flip` 300 calls, `present/displayed` 301,
   `present/flip/composed` 300 (`64817`, `64910`, `64911`) `[V]`.
2. `0:00:37.511836 {RSX [0x0cbeaac]} RSX: FIFO error: possible desync event (last cmd =
   0x80078000)` (`68869`) is the **only** FIFO error in the run `[V]` (§4).
3. From that instant the RSX thread executes **no draw and no present again** for the rest
   of the run (until shutdown at `0:01:12`) `[V]` (§5). It emits exactly four further
   lines, all `MTL: Metal 4: invalidate memory range`, all on GET `0x0cbe9fc`
   (`83465`@`0:00:44.588`, `90930`@`0:00:52.645`, `111639`@`0:00:56.064`,
   `132889`@`0:01:04.907`) `[V]`.
4. The malformed word `0x80078000` fails the non-method mask **and** all four flow-control
   masks, so `FIFO::read()` returns `FIFO_ERROR` and `run_FIFO()` calls `recover_fifo()`
   (`gcm_enums.h:842-867`, `RSXFIFO.cpp:477-493`, `RSXFIFO.cpp:762-767`) `[V]`. It has
   bit 31 set, a legal flag in no mask; it decodes as `count=1`, `method=0x2000`. It is the
   signature of a **data/argument word read as a command header** (GET off the header
   boundary), not of a jump/call/return `[I]`.
5. `recover_fifo()` does `set_get(restore_point)` then `abort()`
   (`RSXThread.cpp:3083-3086`). The post-recovery GET is `0x0cbe9fc`, i.e. **44 words
   (0xB0) below** the error GET `0x0cbeaac` `[V]`. GET then never changes for the rest of
   the run `[V]` (§5). The RSX thread is alive (it still services memory invalidations)
   but parked on the FIFO.
6. The guest keeps running after the freeze: video decode continues (1058 `Got picture` /
   1063 `AU decoding: done` lines after the error) and `_sys_ppu_thread_create` /
   `CnuSound2Thread:` create-and-exit cycles continue to `0:01:12` `[V]` (§3).
7. The guest `sys_mutex_lock` spin does **not** start at the freeze; it is huge *before* it
   and collapses *at* it — cumulative deltas are +26 946 104 (11→21 s), +19 525 480
   (21→31 s), +2 695 (31→41 s) `[V]` (§6.1). The stats are cumulative; the deltas are the
   measurement.
8. The RSX-thread probe `sync/semaphore wait` (the `NV406E_SEMAPHORE_ACQUIRE` spin,
   `MTLGSRender.cpp:3084-3104`, `nv406e.cpp:60-105`) grows 83× between the last two perf
   reports: 1 153 374 calls @`0:00:20.400` → 96 316 516 calls @`0:00:36.250` `[V]` (§6.2).
9. Top hypothesis `[I]`: the image froze because the **guest render loop stopped
   submitting frames while parked in a semaphore/label spin**, and `recover_fifo()` parked
   the RSX GET on the guest's inter-frame idle point, which the guest never overwrote. The
   desync is a marker at the boundary, not the sole cause. Metal-specific only where the
   present/flip and semaphore-wait hooks live; the FIFO decode and recovery are shared/core
   (§7).

---

## 2. Timeline (0:00:36.9 – 0:00:38.0, non-video lines only)

Lines are verbatim (abridged to the fields that matter). Full excerpt in §9.1.

| time | thread | event |
|---|---|---|
| 36.953023 | TID 214361 | MTL feedback slot 2 |
| 36.953809 | RSX `0x0cbe740` | display showed drawable `…18980e0` |
| 36.953897 | RSX `0x0cbe740` | **acquire at 15381.847 took 203.053 ms** — stall #42 |
| 36.953932 | RSX `0x0cbe740` | acquire stall 42, 203.1 ms, drawables in flight 1 |
| 36.988061 | RSX `0x10ae8d0` | present source from the surface, `0xc0000000`, 1280×720 |
| 36.988099 | RSX `0x10ae8d0` | acquire took 0.032 ms |
| 36.988191 | RSX `0x10ae8d0` | present at 15382.084 |
| 36.995882 | main_thread | MTL access violation `0x525fc000`, writing 1 |
| 37.001576 | TID 220865 | feedback slot 0/1 |
| 37.012942 | RSX `0x0cb1500` | sample 53 |
| 37.015441 | RSX `0x0cbe844` | present source `0xc0880000` |
| 37.015529 | RSX `0x0cbe844` | present at 15382.112 |
| 37.028906 | RSX `0x0cbe844` | slot 0 committed, **no drawable** |
| 37.045670 | TID 214380 | display showed drawable `…18c00e0` |
| 37.047852 | RSX `0x10ae8d0` | present at 15382.144 |
| 37.061877 | main_thread | MTL access violation `0x52830000` |
| 37.081136 | RSX `0x10ae95c` | present at 15382.177 |
| 37.095666 | TID 220865 | feedback slot 2 |
| 37.105093 | TID 214361 | display showed drawable `…18e00e0` |
| 37.114812 | RSX `0x10ae95c` | present at 15382.211 |
| 37.130638 | TID 214361 | display showed drawable `…18f00e0` |
| 37.148442 | RSX `0x0cbe9e8` | present at 15382.245 |
| 37.162777 | RSX `0x0cbe9e8` | slot 0 committed, no drawable |
| 37.184671 | RSX `0x10ae95c` | present at 15382.281 |
| 37.195392 | RSX `0x10ae95c` | **slot 0 is in flight, begin_frame waits for its feedback** |
| 37.323497 | TID 220865 | feedback slot 0, **start lag 0.161** (a ≈128 ms hole after 37.195) |
| 37.323660 | RSX `0x10ae95c` | slot 1 is in flight, begin_frame waits for its feedback |
| 37.371227 | RSX `0x0cbe95c` | present at 15382.467 |
| 37.398907 | RSX `0x10ae9e8` | present at 15382.495 |
| 37.418026 | RSX `0x0cbe970` | present at 15382.514 |
| 37.434874 | RSX `0x10ae970` | present at 15382.531 |
| 37.451345 | RSX `0x0cbe970` | present at 15382.547 |
| 37.470316 | RSX `0x10ae8e4` | present at 15382.566 |
| 37.481928 | RSX `0x0cb7080` | last draw (fragment/vertex context), xform 4680 |
| 37.487088 | RSX `0x0cbe8e4` | present source from the surface, `0xc0880000` |
| **37.487259** | RSX `0x0cbe8e4` | **last present** at 15382.583, drawable `…19800e0` |
| 37.498412 | TID 214361 | display showed drawable `…11cc0e0` |
| 37.500377 | TID 214380 | feedback slot 1 |
| 37.510242 | TID 214380 | display showed drawable `…19800e0` (the frame that stays frozen) |
| **37.511836** | RSX `0x0cbeaac` | **`RSX: FIFO error: possible desync event (last cmd = 0x80078000)`** |
| 37.512119 | main_thread | `cellVdecGetPicItem` |
| 37.512130 | main_thread | `cellVdecGetPicture(outBuff=*0x52750c80)` |
| 37.512470 | main_thread | MTL access violation `0x52830000`, writing 1 |
| 37.545514 | main_thread | MTL access violation `0x524adc80` |
| **37.545823** | main_thread | **last guest write into RSX memory** (`0x525c4000`) |
| 37.951464 | CnuSound2Thread `0x1000016` | `PPU: HLE/LV2 history: (0) … sys_mutex_unlock`; `'_sys_ppu_thread_exit' aborted (1.006067s)` |

Two stalls precede the desync: acquire #42 at 36.954 (203.1 ms) and the 37.195→37.323
hole (~128 ms) during which `begin_frame` waited on slot-0 feedback. Neither is itself
fatal (frames resume at 37.370) `[V]`.

---

## 3. Thread state at the freeze

Guest threads present after the error (line 68869), by appearance count and role `[V]`:

| id | role | after-freeze activity | verdict |
|---|---|---|---|
| `0x1000000` | main_thread | 3591 lines; `cellVdecGetPicItem`/`GetPicture(Ext)`, `_sys_ppu_thread_create`, `sys_memory_allocate/free` up to 0:01:12 | **runs** |
| `0x1000017`–`0x1000029` | HLE Video Decoder / HLE PAMF demuxer / CPamfDemuxer::ReadThread / ATRAC3plus | ≈1058 `Got picture`, 1063 `AU decoding: done` | **runs** |
| `0x1000007` | **Draw Thread** | 5 lines after the error, all shutdown bookkeeping | **silent from 0:00:26.100** |
| `0x1000016` | CnuSound2Thread | exits `0:00:37.951` | stops |
| `0x100001a/1e/22` | CnuSound2Thread (new) | created 43.586 / 52.427 / 55.580, each exits ≈0.8–1.0 s later | run/exit cycle |
| `0x1000010`, `0x1000008–0x100000f` | Initialize/File/auto_load etc. | shutdown bookkeeping only | stop |

### 3.1 The Draw Thread `0x1000007` — complete record (17 lines in 169 k)

Created at `0:00:10.862421`. Eleven RSX-fault writes between `0:00:25.350566` and
`0:00:26.100625` (targets `0xc2fd0008`, `0xc1bd0008`; guest PC `0x0022d53c` /
`0x0022cf34`). Then **nothing** until `0:01:12`:

```
0:01:12.198618 {PPU[0x1000007] Thread (Draw Thread) [0x107f4eac]} PPU: HLE/LV2 history:
(0) 0x107f21fc: sys_mutex_unlock, 0x0, r3=0x0, r4=0x0, r5=0x0, r6=0x0
0:01:12.198620 {PPU[0x1000007] Thread (Draw Thread) [0x107f4eac]} PPU: '_sys_ppu_thread_exit' aborted (0.000010s)
0:01:12.198623 {PPU[0x1000007] Draw Thread} SIG: Thread time: 0.000000s (0.000000Gc); Faults: 11 [rsx:11, spu:0]; [soft:0 hard:0]; Switches:[vol:0 unvol:0]; Wait:[60.765s, spur:0]
```

Interpretation `[I]`:
- Its **last HLE call before the freeze** is not visible: only `cellVdec`/`cellDmux`/
  `cellDmuxPamf` are traced, and the Draw Thread calls none of them. The only observed
  activity is 11 RSX-memory write faults (handled by `MTLGSRender::on_access_violation`,
  `MTLGSRender.cpp:3277-3344`); the last is `0:00:26.100625`.
- Its **last HLE call before shutdown** is the generic `sys_mutex_unlock` at `0x107f21fc`,
  the same stub every thread reports at exit — no discriminating wait is recorded.
- **What it is waiting on is not directly logged.** The only quantitative statement is
  `Wait:[60.765s]` against a 61.336 s lifetime (created 10.862, exit 72.198): it was
  blocked ≈99 % of its life and had produced no log line for ≈46 s by the end. This is
  consistent with a block on a `sys_*` synchronization object (mutex/semaphore/event) or
  a GCM flip/label, but the log does not name it `[I]`, `[U]` for the object identity.

A precise "Draw Thread blocked on X" claim needs the HLE trace extended to `cellGcmSys`
and the `sys_*` wait, or a savestate/thread dump: **not available in this log** `[V]`.

### 3.2 main_thread

Runs throughout. Its MTL access-violation writes into RSX memory (`HLE:0x107d54bc`,
`LR:0x002d323c`, i.e. inside `cellVdecGetPicture`, `gcm_enums` target `0x52750c80` etc.)
span `0:00:26.251052`–`0:00:37.545823` (632 events, 11 distinct addresses); they stop at
`37.545823` and never resume, while plain `cellVdec` calls continue past `0:00:38.012` `[V]`.
So after the freeze the decoder keeps running but its output is no longer written into the
protected RSX pages — consistent with the render path no longer consuming decoded frames
`[I]`.

---

## 4. FIFO desync analysis — `0x80078000`

### 4.1 Why it is malformed

`FIFO::read()` classifies each word (`RSXFIFO.cpp:477-494`). Apply the masks from
`gcm_enums.h:842-867` to `c = 0x80078000` `[V]`:

| test | mask | `c & mask` | expected | match |
|---|---|---|---|---|
| non-method? | `RSX_METHOD_NON_METHOD_CMD_MASK = 0xa0030003` | `0x80030000` | `≠ 0` → non-method | yes |
| old jump | `RSX_METHOD_OLD_JUMP_CMD_MASK = 0xe0000003` | `0x80000000` | `0x20000000` | no |
| new jump | `RSX_METHOD_NEW_JUMP_CMD_MASK = 0xe0000003` | `0x80000000` | `0x00000001` | no |
| call | `RSX_METHOD_CALL_CMD_MASK = 0x00000003` | `0x00000000` | `0x00000002` | no |
| return | `RSX_METHOD_RETURN_MASK = 0xffff0003` | `0x80070000` | `0x00020000` | no |

`c` enters the non-method branch (`RSXFIFO.cpp:477`) and matches none of the four
flow-control patterns (`:481-484`), so the code takes the "Malformed command, optional
recovery" path and returns `FIFO_ERROR` (`:491-493`). `run_FIFO()` then logs the error and
calls `recover_fifo()` (`RSXFIFO.cpp:762-767`). `[V]`

**What class of value is it?** `[I]` Bit 31 is set. Bit 31 is not part of any legal
command pattern: an increment/non-increment method header has bit 31 = 0 (the
non-increment flag is `0x40000000`), and no jump/call/return sets bit 31. As a header the
word decodes to `count = (c>>18)&0x7ff = 1`, `method = c & 0xfffc = 0x8000` (index `0x2000`)
— a plausible-looking count but a value that cannot be a header. `0x80000000` is exactly
`RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE` (`gcm_enums.h:480`), i.e. a real RSX *state/flag*
bit. The word therefore reads as a **data/argument word carrying a high-bit flag, sign or
mask, interpreted as a header** — the artefact expected when GET is not on the header
boundary (one or more words into the argument stream), or when a header word never landed
while its arguments did.

### 4.2 `read()` and `run_FIFO` cross-check

- Non-`as_ps3` mode reads `put = read_put()`; `put == m_fifo_pos` ⇒ `FIFO_EMPTY`
  (`RSXFIFO.cpp:443-452`). Otherwise it reads the word at GET and classifies it
  (`:454-457`). There is no "resync by scanning" — an unclassifiable word is fatal.
- `run_FIFO()` handles `FIFO_EMPTY` (`:743-756`, sets `performance_counters.state`),
  `FIFO_NOP` (`:733-742`), and `FIFO_ERROR` (`:762-767` → `recover_fifo()`). A
  self-jump (`offs == get_pos`) is not an error: it sets `state = spinning` and releases
  `sync_point_request` (`:771-796`), leaving GET fixed by design. `[V]`

### 4.3 `recover_fifo` cross-check

`recover_fifo()` (`RSXThread.cpp:3054-3095`): if the recovered-command history is full
(20) and the last entry is <2 s old, it throws "Dead FIFO commands queue state"; otherwise
it does `fifo_ctrl->set_get(restore_point)`, restores `fifo_ret_addr = saved_fifo_ret`,
sleeps 2 ms, `fifo_ctrl->abort()`, and pushes `last_cmd()` into the history `[V]`.
`restore_point` is refreshed at every sync point while not inside a BEGIN/END:
`restore_point = fifo_ctrl->get_pos()` (`RSXThread.cpp:1304-1309`) `[V]`.

The run produced **exactly one** FIFO error, so the history never filled and no
throw fired `[V]`.

### 4.4 Torn write vs lost jump vs GET/restore-point

| candidate | what it predicts | observed? |
|---|---|---|
| **torn / partial guest write** | an inconsistent-but-header-like word, and a *second* desync soon after recovery (the restore point is itself mid-write), or a skipped tail of a batch | single error only; word is not header-like (bit 31) `[I]` |
| **lost FIFO jump/call** | `FIFO: CALL found inside a subroutine` (`:802`) or `RET found without corresponding CALL` (`:817`) | not logged anywhere `[V]` |
| **RSX GET / restore-point problem** | the buffer is coherent but GET points mid-command; after `set_get(restore_point)` GET parks and never advances | GET parks at 0x0cbe9fc and never moves; word is a data word `[I]` |

Verdict `[I]`: the word looks like a **data word consumed as a header**, i.e. a GET that is
off the header boundary (an RSX GET/restore-point-shaped failure), *not* a lost jump/call
(that would log a CALL/RET error, and none exists). A torn guest write is possible but is
the less economical reading, because a torn header would normally still be header-shaped
(count/method plausible, bit 31 clear). The three causes are not separable from this log
alone — see §8.

---

## 5. What the RSX thread did after `recover_fifo()`

The RSX tag is GET (`RSXThread.cpp:1101`), so the tag sequence *is* the GET timeline `[V]`:

```
0:00:37.511836  {RSX [0x0cbeaac]}  RSX: FIFO error: possible desync event (last cmd = 0x80078000)
0:00:44.588374  {RSX [0x0cbe9fc]}  MTL: Metal 4: invalidate memory range
0:00:52.645041  {RSX [0x0cbe9fc]}  MTL: Metal 4: invalidate memory range
0:00:56.064989  {RSX [0x0cbe9fc]}  MTL: Metal 4: invalidate memory range
0:01:04.907563  {RSX [0x0cbe9fc]}  MTL: Metal 4: invalidate memory range
```

After `set_get(restore_point)` GET is `0x0cbe9fc = 0x0cbeaac − 0xB0` (44 words) `[V]`, and
it never changes for the remaining 35 s of the run except for those four identical tags.
The only RSX-thread output in that window is `on_invalidate_memory_range`
(`MTLGSRender.cpp:3435-3453`, note at `:3445`) `[V]`.

Conclusions:
- The RSX thread is **not deadlocked on a host lock**: it still runs and services memory
  invalidations dispatched to it (`RSXThread.cpp:3386-3392`) `[V]`.
- It executes **no command that advances GET** — no draw, no present, no jump, no flip
  `[V]`. It is parked on the FIFO.
- Because `recover_fifo()` does not touch PUT, and the RSX had already read to
  `0x0cbeaac > restore_point`, a *live guest* would have had PUT past GET and the RSX
  would have resumed. It did not, therefore the guest stopped advancing PUT at/after the
  restore point `[I]`. The most coherent reading is that GET landed on the guest's
  inter-frame idle point (NOP + jump-to-self), which the guest overwrites at the start of
  the next frame; the next frame never came `[I]`.
- Whether the parked state is literally `FIFO_EMPTY` (put == get) or `spinning`
  (jump-to-self) **cannot be read from this log**: `FIFO::performance_counters.state`
  (`RSXFIFO.cpp:735-786`) is not printed, and `is_fifo_idle()` is not logged. Absence of a
  second `FIFO: CALL/RET` error and of any TDR/`nv406e` timeout message is the only
  negative evidence `[V]`.

So: **the RSX is idling on the FIFO because the guest stopped submitting**; it is not
blocked elsewhere. The image is frozen because no new frame is drawn or presented.

Two supporting facts bracket the freeze:
- `MTL: Metal 4 perf` reports exist only at `0:00:20.399` and `0:00:36.250`, i.e. once per
  300 frames; the next would fall ≈`0:00:52` and never appears `[V]`. No post-freeze frame
  counter exists at all.
- The last perf report is fully healthy (`present/flip` 300, `present/displayed` 301,
  `present/flip/composed` 300, `64817/64910/64911`) `[V]`.

---

## 6. The spin: guest mutex vs RSX semaphore

### 6.1 Guest `sys_mutex_lock` (cumulative; deltas are the measurement)

`PPU Syscall Usage Stats` blocks print at 11 s intervals `[V]`. The values are **cumulative**
and monotonic; the first block (11.256) still shows the small boot numbers. Deltas between
consecutive identical blocks:

| interval | `sys_mutex_lock` | `sys_event_queue_receive` | `sys_semaphore_wait` | `sys_cond_wait` |
|---|---:|---:|---:|---:|
| 11.256 → 21.256 | +26 946 104 | +3 737 | +2 421 | +0 |
| 21.256 → 31.256 | +19 525 480 | +4 141 | +2 745 | +229 |
| **31.256 → 41.256** (spans the 37.5 freeze) | **+2 695** | +4 153 | +2 801 | +597 |
| 41.256 → 51.256 | +1 145 | +4 349 | +2 995 | +124 |
| 51.256 → 61.256 | +3 066 | +4 352 | +3 000 | +499 |
| 61.256 → 71.256 | +3 344 | +4 349 | +2 997 | +435 |
| 71.256 → 81.256 | +2 397 | +398 | +257 | +39 |

Reading `[V]`:
- The mutex spin is **not** a freeze-time event. It runs at ≈2.7 M and ≈2.0 M locks/s in the
  11–31 s window (the whole time the present pipeline is healthy) and **collapses to ≈0**
  from 31 s onward — the block that contains the freeze. So the spin **ends** at (or just
  before) the freeze; it does not begin there.
- `sys_event_queue_receive` and `sys_semaphore_wait` continue at a **steady** ≈350–450/s and
  ≈270–300/s through the freeze and past it, up to 71 s — the video/audio/event machinery is
  unaffected. This is the "other threads keep running" signal.
- Whole-run totals (`sys_mutex_lock [46484410]` / `sys_mutex_unlock [46484409]`) are almost
  perfectly balanced (one lock outstanding at exit), i.e. a tight lock/unlock loop, not a
  stuck holder `[V]`.

### 6.2 RSX-thread `sync/semaphore wait`

The Metal perf probe `sync/semaphore wait` is the scope around
`MTLGSRender::on_semaphore_acquire_wait()` (`MTLGSRender.cpp:3084-3104`), the hook the core
calls from `nv406e::semaphore_acquire` while the RSX spins for a guest label value
(`nv406e.cpp:60-105`, `spin_on_cacheline_once(..., 100)`). Both perf reports print it
`[V]`:

```
0:00:20.400973  [T]    56.227 ms   1153374 calls   0.05 us avg   2290.96 us max  sync/semaphore wait
0:00:36.250429  [T]  4351.549 ms  96316516 calls   0.05 us avg 154767.67 us max  sync/semaphore wait
```

Per-interval (the same report shows `local task` call counts *falling*, 92 152 → 32 717, so
the `[T]` rows reset each report) `[V]`. The spin grows **83×** in the 16 s before the
freeze — 300 frames carry ≈96.3 M semaphore-acquire waits (≈321 k/frame), each returning in
≈0.05 µs. `[V]`

This is a *guest-visible, label-backed* spin on the RSX thread, distinct from the guest
mutex spin of §6.1. It is the closest thing in the log to a **flip/label completion wait**:
the guest polls a GPU-released label; the RSX services each poll and, per the code comment,
must submit mid-frame so the GPU can release the label
(`MTLGSRender.cpp:3081-3083`, `:3112-3116`). It grows sharply in the run-up to the freeze.

---

## 7. Ranked hypotheses

Each row: hypothesis, tag (Metal-specific vs shared/core), and support/against.

| # | hypothesis | scope | support | against / gap |
|---|---|---|---|---|
| H1 | **Guest render loop stops submitting frames; `recover_fifo()` parks GET on the guest's idle point and the next frame never arrives.** The frozen image is a *guest-producer* stall; the desync is a boundary marker. | boundary (guest core + Metal present path) | GET frozen at the 44-words-earlier restore point to `0:01:04` and no frame counters after `36.250`; guest still alive (video/audio/thread churn) `[V]` | object the guest waits on is not logged `[U]` |
| H2 | **Guest stalls on a GPU label the backend never releases (flip/semaphore completion).** The `sync/semaphore wait` probe grows 83× (1.15 M→96.3 M) in the last interval. | **Metal-specific** (present/flip + `on_semaphore_acquire_wait` hooks) | huge label spin immediately before the freeze `[V]`; the mid-FIFO `recorder.submit()` on wait (`MTLGSRender.cpp:3114`) is a backend decision `[V]` | labels are core-emulated; the trace cannot show the label's address/expected/actual `[U]` |
| H3 | **Torn/partial guest write of a command header.** The RSX read a stale data word as a header. | shared/core (guest↔RSX write ordering) | word is non-header-shaped with bit 31, consistent with a data word `[I]` | only one desync; a torn header is usually still header-shaped; heavy spin makes ordering plausible but unproven `[U]` |
| H4 | **RSX GET/restore-point defect.** Buffer coherent, GET off-boundary; recovery to `restore_point` parks on an idle point. | shared/core (FIFO + recover) | GET never advances after `set_get(restore_point)` `[V]`; the observed word is a data word `[I]` | `restore_point` is core logic, unchanged from VK/GL; no second error `[V]` |
| H5 | **Lost FIFO jump/call.** | shared/core | — | contradicted: a lost jump/return logs `FIFO: CALL found inside a subroutine` (`RSXFIFO.cpp:802`) or `RET found without corresponding CALL` (`:817`); neither appears `[V]` |
| H6 | **Drawable starvation** (`nextDrawable()` blocks with all drawables held) as the cause. | Metal-specific | `acquire stall` occurs 43×, incl. 203.1 ms at `36.954` `[V]` | the last perf report shows `drawables in flight 1`, `present/flip` 300/300, and the desync is a FIFO error, not an acquire block `[V]` (HANDOFF §7.1 already records the acquire as a *symptom*) |

Most likely `[I]`: **H1 with H2 as its mechanism.** H3/H4 are the competing explanations
for the *marker* (the malformed word); H5 is ruled out; H6 is not supported by the last
report.

---

## 8. Discriminating experiment

Single decisive measurement: **re-run the identical input on Metal with targeted
instrumentation for the 2 s straddling the desync**, logging:

1. In `recover_fifo()` (`RSXThread.cpp:3054`): `restore_point`, `last_known_code_start`,
   `fifo_ctrl->get_pos()`, and the live PUT — and read back the 8 FIFO words at both
   `restore_point` and the error GET.
2. In `nv406e::semaphore_acquire` (`nv406e.cpp:60`): the label address, expected and
   actual values, and the wait count/elapsed for each acquire.
3. The FIFO state on entry to the wait: `FIFO::performance_counters.state` (`spinning` vs
   `empty`) and `is_fifo_idle()` (`RSXThread.cpp:2944`).

This one capture separates all live candidates at once:
- If the FIFO words at GET are coherent commands that GET skipped ⇒ **H4** (GET/restore-point).
- If the word at GET is stale/garbage while the neighbouring words are a valid command ⇒
  **H3** (torn write).
- If the parked word is a NOP+jump-to-self and PUT never advanced ⇒ **H1** (guest producer
  stopped), and the recorded label (address/expected/actual) tells whether the label was
  ever released ⇒ **H2** if not.

Companion control (cheap, one run): the identical scene on **Vulkan** with the same
settings. If the freeze reproduces there, the cause is shared/guest (H1/H3/H4); if only
Metal freezes, the cause is Metal-specific (H2/H6). `[I]`

---

## 9. Evidence appendix (verbatim)

### 9.1 The freeze window (filtered, 0:00:36.95 – 0:00:37.95)

```
·T 0:00:36.953023 {TID: 214361} MTL: Metal 4: feedback for slot 2: gpu 15382.033..15382.049, commit 15381.814, start lag 0.219, delivery lag 0.000
·T 0:00:36.953809 {RSX [0x0cbe740]} MTL: Metal 4: display showed drawable 00000079018980e0
·T 0:00:36.953897 {RSX [0x0cbe740]} MTL: Metal 4: acquire at 15381.847 took 203.053 ms, drawable 00000079018ac0e0
·! 0:00:36.953932 {RSX [0x0cbe740]} MTL: Metal 4: acquire stall 42, 203.1 ms, drawables in flight 1, present gap 16.4 ms, main gap 1.2 ms, since display 0.1 ms, view hidden 0, window visible 1 occluded 0, bounds 1408x754 drawable 2816x1508 max 3 sync 0
...
·T 0:00:37.481928 {RSX [0x0cb7080]} MTL: Metal 4: unit 0, base 2032, scale 1.000000 1.000000 0.000000, bias 0.000000 0.000000 0.000000, clamp 0.0000 0.0000 0.0000 0.0000, remap 0xaae4, control 0xe000000
·T 0:00:37.481933 {RSX [0x0cb7080]} MTL: Metal 4: fragment context, rop 0x204000, alpha ref 0.0000, fog 0.0000 0.0000 mode 2048, wpos -1.0000 0.0000 720.0000, fs context 127, vs context 127, xform 4680
·T 0:00:37.487088 {RSX [0x0cbe8e4]} MTL: Metal 4: present source from the surface, address 0xc0880000, 1280x720
·T 0:00:37.487110 {RSX [0x0cbe8e4]} MTL: Metal 4: acquire at 15382.583 took 0.015 ms, drawable 00000079019800e0
·T 0:00:37.487223 {RSX [0x0cbe8e4]} MTL: Metal 4: slot 1 committed at 15382.583, with drawable 00000079019800e0
·T 0:00:37.487259 {RSX [0x0cbe8e4]} MTL: Metal 4: present at 15382.583, drawable 00000079019800e0, paced 0
·T 0:00:37.498412 {TID: 214361} MTL: Metal 4: display showed drawable 00000079011cc0e0
·T 0:00:37.500377 {TID: 214380} MTL: Metal 4: feedback for slot 1: gpu 15382.584..15382.596, commit 15382.583, start lag 0.000, delivery lag 0.000
·T 0:00:37.510242 {TID: 214380} MTL: Metal 4: display showed drawable 00000079019800e0
·E 0:00:37.511836 {RSX [0x0cbeaac]} RSX: FIFO error: possible desync event (last cmd = 0x80078000)
·T 0:00:37.512470 {PPU[0x1000000] Thread (main_thread) [HLE:0x107d54bc, LR:0x002d323c]} MTL: Metal 4: access violation 0x52830000, writing 1
·T 0:00:37.512641 {PPU[0x1000000] Thread (main_thread) [HLE:0x107d54bc, LR:0x002d323c]} MTL: Metal 4: access violation 0x52868000, writing 1
·T 0:00:37.545514 {PPU[0x1000000] Thread (main_thread) [HLE:0x107d54bc, LR:0x002d323c]} MTL: Metal 4: access violation 0x524adc80, writing 1
·T 0:00:37.545753 {PPU[0x1000000] Thread (main_thread) [HLE:0x107d54bc, LR:0x002d323c]} MTL: Metal 4: access violation 0x5258c000, writing 1
·T 0:00:37.545823 {PPU[0x1000000] Thread (main_thread) [HLE:0x107d54bc, LR:0x002d323c]} MTL: Metal 4: access violation 0x525c4000, writing 1
·! 0:00:37.951464 {PPU[0x1000016] Thread (CnuSound2Thread:) [0x107f4eac]} PPU: HLE/LV2 history:
(0) 0x107f21fc: sys_mutex_unlock, 0x0, r3=0x0, r4=0x0, r5=0x0, r6=0x0
·W 0:00:37.952501 {PPU[0x1000016] Thread (CnuSound2Thread:) [0x107f4eac]} PPU: '_sys_ppu_thread_exit' aborted (1.006067s)
```

### 9.2 RSX thread output after the error (complete)

```
0:00:37.511836 {RSX [0x0cbeaac]}  RSX: FIFO error: possible desync event (last cmd = 0x80078000)
0:00:44.588374 {RSX [0x0cbe9fc]}  MTL: Metal 4: invalidate memory range
0:00:52.645041 {RSX [0x0cbe9fc]}  MTL: Metal 4: invalidate memory range
0:00:56.064989 {RSX [0x0cbe9fc]}  MTL: Metal 4: invalidate memory range
0:01:04.907563 {RSX [0x0cbe9fc]}  MTL: Metal 4: invalidate memory range
```

### 9.3 MTL perf reports (only two; per-interval)

```
·! 0:00:20.399386 {RSX [0x0cb1344]} MTL: Metal 4 perf: 110 rows over 300 frames, drawables in flight 2, suppressed lines 0
·! 0:00:20.400973 {RSX [0x0cb1344]} MTL: Metal 4 perf: [T]    56.227 ms   1153374 calls     0.05 us avg  2290.96 us max  sync/semaphore wait
·! 0:00:36.250422 {RSX [0x0cbcd44]} MTL: Metal 4 perf: 99 rows over 300 frames, drawables in flight 1, suppressed lines 0
·! 0:00:36.250429 {RSX [0x0cbcd44]} MTL: Metal 4 perf: [T]  4351.549 ms  96316516 calls     0.05 us avg 154767.67 us max  sync/semaphore wait
·! 0:00:36.250555 {RSX [0x0cbcd44]} MTL: Metal 4 perf: [C]          301 total       301 calls         1.00 avg            1 max  present/displayed
·! 0:00:36.250556 {RSX [0x0cbcd44]} MTL: Metal 4 perf: [C]          300 total       300 calls         1.00 avg            1 max  present/flip/composed
```

### 9.4 `PPU Syscall Usage Stats` — block headers and key rows

```
·! 0:00:11.256433 {PPU Syscall Usage Thread} PPU: PPU Syscall Usage Stats:
	⁂ sys_mutex_lock [179]
	⁂ sys_mutex_unlock [178]
	⁂ sys_event_queue_receive [144]
	⁂ sys_semaphore_wait [21]
·! 0:00:21.256628 {PPU Syscall Usage Thread} PPU: PPU Syscall Usage Stats:
	⁂ sys_mutex_lock [26946283]
	⁂ sys_mutex_unlock [26946283]
	⁂ sys_event_queue_receive [3881]
	⁂ sys_semaphore_wait [2442]
·! 0:00:31.256084 {PPU Syscall Usage Thread} PPU: PPU Syscall Usage Stats:
	⁂ sys_mutex_lock [46471763]
	⁂ sys_mutex_unlock [46471761]
	⁂ sys_event_queue_receive [8022]
	⁂ sys_semaphore_wait [5187]
	⁂ sys_cond_wait [229]
·! 0:00:41.256184 {PPU Syscall Usage Thread} PPU: PPU Syscall Usage Stats:
	⁂ sys_mutex_lock [46474458]
	⁂ sys_mutex_unlock [46474456]
	⁂ sys_event_queue_receive [12175]
	⁂ sys_semaphore_wait [7988]
	⁂ sys_cond_wait [826]
·! 0:01:21.255833 {PPU Syscall Usage Thread} PPU: PPU Syscall Usage Stats:
	⁂ sys_mutex_lock [46484410]
	⁂ sys_mutex_unlock [46484409]
	⁂ sys_event_queue_receive [25623]
	⁂ sys_semaphore_wait [17237]
	⁂ sys_cond_wait [1923]
```

### 9.5 The Draw Thread `0x1000007` (complete)

```
·W 0:00:10.862421 {PPU[0x1000000] Thread (main_thread) [0x107f4d60]} sys_ppu_thread: _sys_ppu_thread_create(): Thread "Draw Thread" created (id=0x1000007, func=*0xbd77c, rtoc=0x4de880, user-tls=0x30007df0)
·T 0:00:25.350566 {PPU[0x1000007] Thread (Draw Thread) [0x0022d53c]} MTL: Metal 4: access violation 0xc2fd0008, writing 1
·T 0:00:25.366851 {PPU[0x1000007] Thread (Draw Thread) [0x0022d53c]} MTL: Metal 4: access violation 0xc1bd0008, writing 1
·T 0:00:25.503608 {PPU[0x1000007] Thread (Draw Thread) [0x0022d53c]} MTL: Metal 4: access violation 0xc2fd0008, writing 1
·T 0:00:25.533225 {PPU[0x1000007] Thread (Draw Thread) [0x0022d53c]} MTL: Metal 4: access violation 0xc1bd0008, writing 1
·T 0:00:25.700238 {PPU[0x1000007] Thread (Draw Thread) [0x0022d53c]} MTL: Metal 4: access violation 0xc2fd0008, writing 1
·T 0:00:25.733858 {PPU[0x1000007] Thread (Draw Thread) [0x0022d53c]} MTL: Metal 4: access violation 0xc1bd0008, writing 1
·T 0:00:25.767307 {PPU[0x1000007] Thread (Draw Thread) [0x0022d53c]} MTL: Metal 4: access violation 0xc2fd0008, writing 1
·T 0:00:25.800565 {PPU[0x1000007] Thread (Draw Thread) [0x0022d53c]} MTL: Metal 4: access violation 0xc1bd0008, writing 1
·T 0:00:25.834075 {PPU[0x1000007] Thread (Draw Thread) [0x0022d53c]} MTL: Metal 4: access violation 0xc2fd0008, writing 1
·T 0:00:25.867325 {PPU[0x1000007] Thread (Draw Thread) [0x0022d53c]} MTL: Metal 4: access violation 0xc1bd0008, writing 1
·T 0:00:26.100625 {PPU[0x1000007] Thread (Draw Thread) [0x0022cf34]} MTL: Metal 4: access violation 0xc2fd0008, writing 1
·! 0:01:12.198618 {PPU[0x1000007] Thread (Draw Thread) [0x107f4eac]} PPU: HLE/LV2 history:
(0) 0x107f21fc: sys_mutex_unlock, 0x0, r3=0x0, r4=0x0, r5=0x0, r6=0x0
·W 0:01:12.198620 {PPU[0x1000007] Thread (Draw Thread) [0x107f4eac]} PPU: '_sys_ppu_thread_exit' aborted (0.000010s)
·! 0:01:12.198621 {PPU[0x1000007] Thread (Draw Thread) [0x107f4eac]} PERF: PPU thread perf stats are not available.
·! 0:01:12.198623 {PPU[0x1000007] Draw Thread} SIG: Thread time: 0.000000s (0.000000Gc); Faults: 11 [rsx:11, spu:0]; [soft:0 hard:0]; Switches:[vol:0 unvol:0]; Wait:[60.765s, spur:0]
```

### 9.6 Shutdown

```
·! 0:01:11.999108 GUI: Game window close event issued
·! 0:01:11.999450 SYS: Requesting game to exit...
·! 0:01:12.204483 {PPU[0x1000000] Thread (main_thread) [0x000bd878]} SYS: 'sys_semaphore_wait' failed with 0x80010005 : CELL_ESRCH [1]
·! 0:01:14.095130 {Pad Thread} Input: Stopping pad threads...
·! 0:01:17.261663 {TID: 222016} SYS: Threads were terminated.. increasing termination timeout (threads=8)
```

### 9.7 Source citations

| claim | source |
|---|---|
| RSX log tag is the FIFO GET | `rpcs3/Emu/RSX/RSXThread.cpp:1101` |
| Non-method / jump / call / return masks | `rpcs3/Emu/RSX/gcm_enums.h:842-867` |
| Malformed word → `FIFO_ERROR` | `rpcs3/Emu/RSX/RSXFIFO.cpp:477-494` |
| `FIFO_ERROR` → log + `recover_fifo()` | `rpcs3/Emu/RSX/RSXFIFO.cpp:762-767` |
| jump-to-self sets `spinning`, GET fixed | `rpcs3/Emu/RSX/RSXFIFO.cpp:771-796` |
| `put == get` → `FIFO_EMPTY` | `rpcs3/Emu/RSX/RSXFIFO.cpp:443-452` |
| `recover_fifo()` body (`set_get(restore_point)`, `abort()`) | `rpcs3/Emu/RSX/RSXThread.cpp:3054-3095` |
| `restore_point = get_pos()` at sync points | `rpcs3/Emu/RSX/RSXThread.cpp:1304-1309` |
| invalidation dispatch to the RSX thread | `rpcs3/Emu/RSX/RSXThread.cpp:3386-3392` |
| `on_semaphore_acquire_wait` (probe + mid-frame submit) | `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:3084-3117` |
| `on_access_violation` (guest vs RSX-thread logging) | `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:3277-3344` |
| `on_invalidate_memory_range` + `invalidate memory range` note | `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:3435-3453` |
| `nv406e::semaphore_acquire` spin loop | `rpcs3/Emu/RSX/NV47/HW/nv406e.cpp:60-105` |

<!-- doc-end -->
