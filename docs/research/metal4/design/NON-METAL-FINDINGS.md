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

## What is not here

- `Resource was destroyed whilst holding a resource reference!` is Metal teardown (L16).
- The 12 `Metal 4: access violation` lines are the Metal guest-write diagnostic (L17).
- The FIFO desync and fatal are Metal (L13).
- No decoder memory-pressure, jetsam, or `cellVdec` decode error appears.  The video output is not a
  decoder failure.

<!-- doc-end -->
