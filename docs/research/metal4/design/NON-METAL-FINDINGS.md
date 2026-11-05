# Non-Metal findings — Ridge Racer 7 probe 2

Scope: failures in the 2026-10-21 probe-2 log that are not in the Metal backend.  Do not patch these
in `rpcs3/Emu/RSX/MTL`.  The Metal run-log findings are in `LOG-FINDINGS.md` (L13–L17).

Artifact: `RPCS3.log`, 15,078,221 bytes, 145,725 lines, two boots (0:01:13–0:03:36 and
0:03:45–0:06:12).  All line numbers refer to that file.

## HLE video and demuxer

| id | finding | count | evidence | severity | state |
|---|---|---|---|---|---|
| N1 | `_CellDmuxCoreOpReleaseAu` returns `CELL_EPERM` during video decode.  The decoder still produces pictures: 1,074 `cellVdecGetPicture` calls and about 1,960 decoded frames in the run. | 21 | `RPCS3.log:21926` first, `:142977` last | L | shared HLE; open |
| N2 | `_CellDmuxCoreOpDisableEs` returns `CELL_EPERM` when the video stream stops. | 6 | `RPCS3.log:35085` first, `:140161` last | L | shared HLE; open |
| N3 | `cellVdecGetPicItem` fails with `0x80610104` during stream teardown. | 5 | `RPCS3.log:35061` first, `:140130` last | L | shared HLE; open |
| N4 | `cellDmuxResetEs` returns `CELL_DMUX_ERROR_SEQ` (5) or `CELL_DMUX_ERROR_ARG` (4).  `cellDmuxDisableEs` returns `CELL_DMUX_ERROR_ARG` (4).  These follow the release failures and are likely teardown state, not independent defects. | 13 | `RPCS3.log:35071`, `:35099`, `:140171`, `:140176` | L | shared HLE; hypothesis |
| N5 | `sys_cond_destroy` returns `CELL_ESRCH` during the video teardown. | 6 | `RPCS3.log:35128` first, `:140202` last | L | shared HLE; open |
| N6 | `sys_ppu_thread_set_priority` returns `CELL_ESRCH` for a thread that has already exited. | 4 | `RPCS3.log:33522` first, `:110837` last | L | shared HLE; open |

These are not the race-start freeze.  The freeze is `L13`: the Metal label publication raced the
GPU and killed the RSX thread.  Recheck N1–N6 after the label fix, because a dead RSX thread also
stops the video consumer and makes the demuxer teardown errors more likely.

## Firmware, fonts and host limits

| id | finding | count | evidence | severity | state |
|---|---|---|---|---|---|
| N7 | `sys_mmapper_allocate_shared_memory_from_container` returns `CELL_ESRCH`.  The guest asks for a 4 MiB shared region. | 1 per boot | `RPCS3.log:19316`, `:64843` | L | shared `sys_mmapper`; open |
| N8 | The firmware font `SCE-PS3-RD-R-LATIN.ccd` is absent.  The guest falls back to `SCE-PS3-RD-R-LATIN.TTF`, which opens. | 1 per boot | `RPCS3.log:20793-20796` | L | firmware data; no action for Metal |
| N9 | `VM: Failed to lock sudo memory` reports 12 guest ranges between 0x30510000 and 0x323e0000.  The log asks for higher system limits.  The run continues. | 12 | `RPCS3.log:86071` first, `:86095` last | L | host wired-memory limit; open |
| N10 | `_sys_ppu_thread_exit` is aborted while the PAMF demuxer and draw threads are torn down.  The line is a timeout report, not a decode failure. | 3 | `RPCS3.log:51999`, `:64037`, `:140250` | L | shared thread teardown; open |

## Log volume (20:55 run)

Artifact: `~/Library/Caches/rpcs3/RPCS3.log`, 59,832,475 bytes, 397,107 lines, `MTL_PROBE=2`, `rc=0`
after 2:19.

| id | finding | count | evidence | severity | state |
|---|---|---|---|---|---|
| N11 | The log is 80.1 % `sys_spu` / `sys_event` / `sys_event_flag` notices and 13.0 % `cellDmux` / `cellVdec` / PAMF trace, against 0.35 % Metal probe text. The two families bury any deep probe run. A probe run therefore drops those four channels to `error` and writes the bulk probe text to its own file. The channel drop is `TODO(revert)` and is not part of the backend. | 318,037 + 51,647 | `RPCS3.log`, composition counts | M | temporary measure in place; upstream noise open |

The `sys_spu` / `sys_event` notices are expected SPU thread lifecycle traffic, and the PAMF trace is
the video decode. Neither is a defect; the volume is the problem.

## Shared RSX FIFO (not Metal)

| id | finding | count | evidence | severity | state |
|---|---|---|---|---|---|
| N12 | **The accurate FIFO's empty-first-argument retry is off by one and misaligns the command stream.** `FIFO_control::read()` advances the position to the first argument and fetches it; when the fetch is empty it left the position on that argument and raised `m_remaining_commands`. The retry enters `read_unsafe()`, which advances by four **before** it fetches, so the retry reads the second argument as the first, consumes one word too many, reads the next command's header as the last argument and resumes the stream on the next command's first argument — a data word parsed as a command. A data word with low bits `0b10` is a legal call encoding, which produces the wild call in the Metal run's capture (LOG-FINDINGS L59/L64/L70). The path is not exotic: the producer may publish a header before its arguments, which is why the non-accurate path's `inc_get(true)` waits for exactly that one argument. Fixed in `read()` by rewinding to the header and clearing the remaining count. This is shared code, so it is not a Metal defect and it affects every backend that runs the accurate FIFO. | one shift per empty first argument; the Metal run hit it once in 99.7 s | `RSXFIFO.cpp:509-542`, `:335-385`; `INVESTIGATION-2026-10-22.md` §9.1, §10.2 | H | fixed (shared), build and run pending |

## Audio backend teardown

| id | finding | count | evidence | severity | state |
|---|---|---|---|---|---|
| N13 | **`CubebBackend::Open()` self-deadlocks when `cubeb_stream_start()` fails.** `Open()` holds `m_cb_mutex` for its whole body, and the start-failure path called `Close()`, which takes the same non-recursive `std::timed_mutex`. The cellAudio thread blocks there forever: the game runs with no audio and `Emulator::Kill()` can never join the thread, so closing the window hangs (and the forced-terminate path then aborts in JIT teardown). | 1 per run | `RPCS3.log:2520-2524`; `rpcs3-2026-10-28-224120.ips`, thread `cellAudio Thread`: `Close()+80` → `timed_mutex::lock()` | H | fixed: the failure path unlocks before `Close()` |

The stack is exact. `Close()` at `CubebBackend.cpp:209` is the `m_cb_mutex` lock; the `cellAudio`
thread reaches it from `Open()` (`:184`), which already holds that mutex from `:106`. The two log
lines that precede the stall are `cubeb_stream_start() failed: -1` and
`Cubeb stream (…) destroyed successful.` — i.e. the stream is torn down and then the lock never
returns. Everything after `0:00:05.565` is silence from that thread until `Emulation Join Thread`
starts reporting `Thread [cellAudio Thread] is too sleepy` at `0:03:17`.

The trigger on this host is `AudioOutputUnitStart` returning `kAudioHardwareIllegalOperationError`
(`'what'`, 2003329396) at `0:00:05.505`, so the backend never starts and the run has no sound. That
start failure is separate from the deadlock and remains open: the fix only makes the failure
non-fatal and joinable. Reproduce and fix the start failure before calling audio healthy.

## What is not here

- `Resource was destroyed whilst holding a resource reference!` is Metal teardown (L16).
- The 12 `Metal 4: access violation` lines are the Metal guest-write diagnostic (L17).
- The FIFO desync and fatal are **shared** FIFO code (N12), not Metal; the Metal run is where they were
  observed (L13).
- No decoder memory-pressure, jetsam, or `cellVdec` decode error appears.  The video output is not a
  decoder failure.

<!-- doc-end -->
