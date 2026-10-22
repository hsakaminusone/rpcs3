# Metal 4 run-log findings — Ridge Racer 7, 2026-10-19 and 2026-10-20

Method: read `RPCS3.log` (14,150,098 bytes, 133,106 lines; run 2026-10-19 23:26:06 to 23:28:00, `BLUS30001` Ridge Racer 7) and four `*.ips` reports (2026-10-18 13:13:49 to 2026-10-19 23:28:05; 68,762 / 61,629 / 66,352 / 164,138 bytes).
Method: `RPCS3.log.gz` (824,650 bytes) is mode 000 and could not be read; `TTY.log` is 71 bytes; no timestamped logs or log files exist in `~/Library/Application Support/rpcs3/`.

Search counts in `RPCS3.log`: 94 `·E`, 1,388 `·W`, 21 `Metal`, 16 `Metal 4: access violation`, 1 `should_skip_draw`, 0 `MTL_PROBE`, 0 `MTL_DEBUG_LOGS`, 0 `feedback`, 0 runtime `timeout` (2 config lines), 0 `quarantine`, 0 `did not report`, 0 `dropped`, 0 `rebuild`, 0 shader or pipeline failure lines, 0 allocation failure lines.

Scope: L1, L3 and L4 are on the Metal or RSX path. L2 and L5–L9 are shared emulator, guest or host findings.

## Findings

| id | finding | log evidence (file + line) | inferred source site (`file:line`) | severity | confirmed vs hypothesis | suggested next step |
|---|---|---|---|---|---|---|
| L1 | Metal aborts on a texture descriptor. The RSX thread dies in `MTLTextureDescriptorInternal validateWithDevice` during a fragment-texture CPU upload from `MTLGSRender::end`. The abort occurs inside `heapTextureSizeAndAlign`, before any backend error log. The log stops at 0:01:53.881. The invalid field is not logged. Candidates are a zero width or height, a non-square cube, a bad mip count, or a format/type pair that Metal rejects. | `rpcs3-2026-10-19-232805.ips:52` (threads array): `"validateWithDevice"` ← `"heapTextureSizeAndAlign"` ← `"new_texture"` ← `"create_image"` ← `"create_new_texture"`; `RPCS3.log:133106` is the last line and holds no error. | `rpcs3/Emu/RSX/MTL/MTLTextureCache.h:218-229` (`make_descriptor`), `:426` (call); `rpcs3/Emu/RSX/MTL/MTLResourceManager.cpp:315-333` (`new_texture`). | H | confirmed crash; field is hypothesis | Log `format`, `type`, `width`, `height`, `depth` and `mipmaps` before allocation. Reject zero dimensions, a non-square cube and `mipmaps == 0`. Add a regression test with the saved guest texture. |
| L2 | LLVM JIT teardown crashes on quit. The main thread takes SIGSEGV inside `llvm::Module::~Module` while `jit_compiler::operator=(destroying_context)` resets the MCJIT engine. The fault address `0xddbce0b5b72586e9` is not in any region. | `rpcs3-2026-10-18-131349.ips:56` (threads array): `EXC_BAD_ACCESS`, `llvm::Module::~Module` ← `MCJIT::~MCJIT` ← `jit_compiler::operator=`. | `Utilities/JITLLVM.cpp:810-825` (`m_engine.reset()`); `rpcs3/Emu/Cell/PPUThread.cpp:3951` (`jit_module_manager::operator=`). | M | confirmed crash; cause is hypothesis | Reproduce with a debug or ASan build. Check module ownership and the engine-before-context reset order. |
| L3 | A draw after boot is refused. `should_skip_draw()` returns true at 0:00:28.131 and the backend replaces the draw with a NOP draw. A static latch prints the line once, so the log cannot show a repeat. The reason bits are not logged. | `RPCS3.log:21215` — `·E 0:00:28.131163 {RSX [0x0cb0638]} RSX: Metal 4: should_skip_draw refuses the draw` | `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:1580-1587`; `rpcs3/Emu/RSX/RSXThread.cpp:1012-1025`. | M | confirmed line; cause and repeat are hypothesis | Log the four predicate bits once. Add a skip counter. Check `rtt_config_valid` and the attachment mask at the first draw. |
| L4 | The fragment program reads ROP output r0 before any write. The decompiler gathers the final value. Four shader workers hit this between 0:00:10.602 and 0:00:10.799. The log does not name the shader. | `RPCS3.log:2246` — `·W 0:00:10.602566 {RSX Worker 6} RSX: ROP reads from r0 without writing to it. Final value will be gathered.` | `rpcs3/Emu/RSX/Program/FragmentProgramDecompiler.cpp:849`. | L | confirmed warning; impact is hypothesis | Dump the shader for the first occurrence. Compare r0 output with Vulkan and OpenGL. |
| L5 | The SPU JIT detects a channel loop and asks for a developer report. One occurrence at 0:00:15.242. The log does not show a hang. | `RPCS3.log:4164` — `·E 0:00:15.242461 {SPU Worker 2} SPU: Channel Loop Pattern Detected! Report to developers!` | `rpcs3/Emu/Cell/SPUCommonRecompiler.cpp:8811`. | L | confirmed; impact is hypothesis | Replay the image `7PiXnkUPiv7ZdGvUkndsHKRu6ZNZ`. Check the loop at `0xa7c`. |
| L6 | The VM cannot lock 15 guest ranges. The log asks for higher system limits. The run continues. Failures cluster at 29–32 s and repeat at 1:05 and 1:26. | `RPCS3.log:21318` — `VM: Failed to lock sudo memory (addr=0x30510000, size=0x1240000). Consider increasing your system limits.` | `rpcs3/Emu/Memory/vm.cpp:1179` (`lock_sudo`). | L | confirmed; performance impact is hypothesis | Check the host wired-memory limit. Repeat the run with the limit raised. |
| L7 | The SPU recompiler warns 671 times that MFC_EAH is not zero, and 85 times that MFC_Cmd is not constant. The same sites repeat. The log does not show a wrong result. | `RPCS3.log:2332` — `SPU: [0x39cc] MFC_EAH: $3 is not a zero constant`; `RPCS3.log:2333` — `SPU: [0x39dc] MFC_Cmd: $83 is not a constant` | `rpcs3/Emu/Cell/SPULLVMRecompiler.cpp:5157`, `:5517`. | L | confirmed warning; impact is hypothesis | Check the dynamic MFC address path. Add a test with a non-zero EAH value. |
| L8 | The guest game-data directory is missing. The guest asks for `/dev_hdd0/game/BLJS10001/USRDIR/RR7.DAT` and gets CELL_ENOENT. It falls back to `/dev_bdvd/PS3_GAME/USRDIR/RR7.DAT` (4.872 GB). The title ID is `BLUS30001`; the guest asks for `BLJS10001` game data. | `RPCS3.log:20487` — `'sys_fs_open' failed with 0x80010006 : CELL_ENOENT, “/dev_hdd0/game/BLJS10001/USRDIR/RR7.DAT”`; fallback at `:20490`. | not located (guest data path; HLE `sys_fs`). | L | confirmed | Install the game data under `BLJS10001`. Repeat the run. Compare load time and content. |
| L9 | Low Power Mode is on. The log warns that performance may drop. Timing results from this run are lower bounds. | `RPCS3.log:9` — `·E 0:00:00.000173 SYS: Low Power Mode is enabled, performance may be reduced.` | `rpcs3/rpcs3.cpp:793`. | L | confirmed | Disable Low Power Mode. Repeat the run before any timing comparison. |

## Already known

| log finding | existing row or note | mapping |
|---|---|---|
| `cellVdecGetPicItem` returns `0x80610104` at 1:26.503 and 1:41.303, with `cellDmuxResetEs` and `cellDmuxDisableEs` errors. | `HANDOFF.md:776` records the video blocker: the decoder releases AUs, no picture arrives, and the game aborts the decode. | Shared HLE path. No Metal code. Do not re-report. |
| `Metal 4: access violation` appears 16 times from 0:00:28.342 to 0:00:40.691. | Benign per the task. F8 fixed the flush loss in `on_access_violation` (`REVIEW-FINDINGS.md:957`). | Memory-protection diagnostic. Not a crash. Do not re-report. |
| `rpcs3-2026-10-18-144255.ips` and `rpcs3-2026-10-19-151439.ips` show SIGABRT after `Emulator::Quit`. | I6, the teardown abort (`REVIEW-FINDINGS.md:1564`; `rpcs3-2026-10-19-151439.ips:51`). | Frontend teardown defect. Already recorded and fixed. |
| `should_skip_draw` replaces a draw with a NOP draw. | DR8 (`DEVIATIONS-DRAW.md:38`) and DR7 (`METAL4-TODO.md:69`). | The mechanism is known. L3 reports only the missing reason and the hidden repeat count. |
| `_CellDmuxCoreOpReleaseAu` returns EPERM, and `cellDmuxResetEs` returns `CELL_DMUX_ERROR_SEQ`. | Benign per the task. | Do not report. |

## No evidence

- No Metal validation or API error line in `RPCS3.log`. The only validation abort is in the 23:28 crash report (L1).
- No `error:` line, assertion, `fatal` line or hang line. `fatal` appears only in the config text.
- No repeated per-frame Metal warning. The 16 access violations are the only repeated Metal line, and they are benign.
- No dropped commit, feedback timeout, `did not report` line, quarantine, slot rebuild or allocation failure.
- No shader compile or pipeline creation failure. The archive load at 0:00:08.503 is a cache hit.
- No GPU time, present time or FIFO stall value. The `PERF` lines report CPU use only.
- No surface or texture format named together with an error.
- `MTL_PROBE` latches: 0 lines. `MTL_DEBUG_LOGS` text: 0 lines. The probe level was 0, so the run gives no counts or signatures.
- No RSX line from 0:00:40.691 to the 0:01:53.881 end. This is not a hang: a successful draw does not log at level 0 (see `HANDOFF.md` §7.1).

## Run 2026-10-20 22:05

Artifact: `RPCS3.log`, 22,704,144 bytes, run 2026-10-20 22:01:37 to 22:05:17, and
`rpcs3-2026-10-20-220517.ips`. This run uses the 21:58 build, which includes batch 3, TX9 and the
earlier fixes. Probe level was 0.

| id | finding | evidence | site | severity | state |
|---|---|---|---|---|---|
| L10 | The process aborts with SIGABRT in `MTLTextureDescriptor validateWithDevice`, inside `heapTextureSizeAndAlign`, from `new_texture` → `create_new_texture` → `upload_image_from_cpu` → `upload_texture` → `load_texture_env` → `end`. The faulting thread is `rsx::thread`. The last flushed line is `0:03:35.278933`: a 256x256 cube with 7 mips and format 134. Root cause confirmed: `make_descriptor` set `arrayLength = 6` for `TextureTypeCube`; Metal requires 1. A local Metal probe reproduced the exact assertion (`MTLTextureTypeCube requires that arrayLength is 1`) and passed with 1; `depth` stays 1 and the six faces are addressed by the upload slice. | `.ips` thread 34; `RPCS3.log` last line; local cube probe | `MTLTextureCache.h` `make_descriptor` | H | fixed (probe-verified; on-device run pending) |
| L11 | The Xevious mini-game panel is black. The X button produces the correct audio feedback, so guest input and logic run. The panel is not rendered, is cleared after it is drawn, or is sampled stale. | user report; 22:05 session; earlier screenshot `21_14_46` | `MTLGSRender.cpp` skip path; `MTLFramebuffer.cpp`; `MTLTextureCache.cpp` | H | open; B2–B4 landed, needs run |
| L12 | The race-selection background video and the FMVs are black. This run decodes and samples video, so the earlier “no picture, shared HLE” reading is obsolete. The decode-to-sample format, pitch, invalidation, barrier and copy route are unproven. | user report; 22:05 session | `MTLGSRender.cpp` `load_texture_env`; `MTLTextureCache.cpp` | H | open; B2–B4 landed, needs run |

The crash report has `EXC_CRASH`/`SIGABRT`, termination code 6, and no memory-pressure or jetsam
signature. `vmSummary` shows a 24.9 GB writable region with 2.8 MB resident; the run is not
memory-limited.

## Resolution (2026-10-20)

- **L1 partially fixed.** `normalize_texture_extent` (`MTLTextureCache.cpp`) produces a legal host
  extent: one texel minimum, a square cube face from the width, and a mip count capped at the chain
  the extent can hold. `make_descriptor` reports the first eight substitutions with the type, format
  and both extents. The unit tests cover zero fields, a non-square cube, an over-long chain and a
  volume depth. The 22:05 run aborts again in the same validation call, so a descriptor still
  bypasses the path; the open row is L10.
- **L3 fixed.** The skip path reports the four `should_skip_draw` predicates for the first 64
  refusals at probe level 2.
- **L10–L12 open.** The crash, the black mini-game and the black video. The read-only forensic pass
  is `SUBAGENT-RR7-2026-10-20.md` when it lands.
- **L2 closed as I6.** The teardown abort was already recorded and fixed.
- **L4–L9** are shared, guest or host findings with no Metal code. They stay in this document and
  are indexed in `METAL4-TODO.md` K.3.

Build and tests at that revision: 149/149 MTL, 496/496 full (2 disabled).


## Run 2026-10-21 00:01 — probe 2, cold and warm

Artifact: `RPCS3.log`, 15,078,221 bytes, 145,725 lines, two boots of `BLUS30001` Ridge Racer 7.
Preserved copy: `/private/var/folders/0x/tq2_tw6d7_lckb2kqwt23klc0000gn/T/opencode/rpcs3-probe2/RPCS3.log`.
The first boot ran 0:01:13 to 0:03:36. The second boot ran 0:03:45 to 0:06:12. Probe level 2.

| id | finding | evidence | site | severity | state |
|---|---|---|---|---|---|
| L13 | The guest recycles the FIFO before the GPU finishes the commands that a back-end semaphore release publishes. The RSX thread then reads a label value as a command, fails 21 FIFO desync recoveries in 0.16 s, and dies with `Dead FIFO commands queue state has been detected!`. The fence is `NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE` value `0x10000`, address `0x60300800`; the FIFO GET is `0xcbcf54` and reads `0x10000` as the next command. The Metal backend had `supports_host_gpu_labels = false`, so `write_gcm_label` wrote the label on the CPU after a no-op `thread::sync()` and before the Metal queue completed the preceding work. | `RPCS3.log:144415` FIFO state; `:144417` semaphore address; `:144223-144243` 21 desync lines; `:145415` fatal. First boot had two earlier desyncs at `:21608-21609`. | `MTLDevice.cpp:314` (old flag), `nv47_sync.hpp:13-63` (CPU write path), `MTLGSRender::release_GCM_label`, `MTLCommandRecorder::wait_idle` | H | fixed in code; on-device check pending |
| L14 | The first boot reaches the title video but stalls after frame 306 at 0:01:25. It logs only 3 submitted vertices, 1 draw record, 75 pipeline misses, 35 vertex builds and 40 fragment builds. 302 of 306 flips compose a black clear because no display buffer is configured yet (`present/source resolved` 4, `present/compose refused` 302). The second boot is warm and renders normally. This is a cold-start path, not a present-predicate bug. | `RPCS3.log:17480-17488`, `:64186-64219` | pipeline archive and synchronous compilation in `MTLProgramPipeline.cpp`; async design is P1 | H | open; async shader subagent is working |
| L15 | `make_descriptor` reported the legal cube form as an illegal request: `256x256x6 with 7 mips ... using 256x256x1`. The six faces are the RSX cube depth and are addressed through the upload slice; the report was noise. | `RPCS3.log:140406` | `MTLTextureCache.h:251-275` | L | fixed (diagnostic only) |
| L16 | Four `Resource was destroyed whilst holding a resource reference!` lines appear only at shutdown after the fatal. They do not occur before the race freeze and do not explain it. | `RPCS3.log:145600-145603` | `MTLResourceManager` teardown | L | open (shutdown only) |
| L17 | Twelve `Metal 4: access violation ... writing 0` reports and twelve host `VM: Failed to lock sudo memory` errors occur during the second boot. The access reports are the existing guest-write diagnostic (F8). The lock errors are the host wired-memory limit. Neither precedes the fatal. | `RPCS3.log:86066-86211`, `:86071-86095` | `MTLGSRender::on_access_violation`; `vm.cpp` `lock_sudo` | L | known; non-Metal lock failures are in `NON-METAL-FINDINGS.md` |

The second boot reaches the race start at 0:05:04, then the fatal stops the RSX thread. The PPU main
thread is in `sys_semaphore_wait` for 64.87 s until the user quits. The 21 desync recoveries exceed
the 20-in-2-s limit in `RSXThread::recover_fifo` (`RSXThread.cpp:3054-3090`); the limit is the
detector, not the cause.

Resolution in this pass (built, unit tests green, on-device check pending):

- `supports_host_gpu_labels = true` now routes every label release through
  `MTLGSRender::release_GCM_label`.
- `release_GCM_label` commits and waits for the current slot with
  `command_recorder::flush_for_readback`, or waits for the ring when the frame is closed with
  `command_recorder::wait_idle`.  The label write follows that wait.  The queue is serialized, so
  the current commit also proves the earlier commits complete.
- The 10 s feedback bound is reused.  A timeout logs an error and still writes the label, so the
  RSX thread cannot hang in the label path.
- Deferred texture subresources are materialized at bind (`resolve_texture_bind`), null texture
  slots are written on a transition (B3), and a disabled unit binds null (B4).  These three changes
  are the B2-B4 batch for the black panel and video paths.

Build and tests: 497/497 enabled cases pass on 499 tests (2 disabled).


## Run 2026-10-21 12:54 — probe 5, deep dumps

Artifact: `/private/var/folders/0x/tq2_tw6d7_lckb2kqwt23klc0000gn/T/opencode/rr7-probe5-20261021-125446/RPCS3.log`, 8.9 MB.
Dumps: `/tmp/mtl4-dump`. Command: `MTL_PROBE=5 MTL_PROBE_AT_SEC=105 MTL_PROBE_FRAME_COUNT=1`.
The run died before the capture window, so there is no race or menu capture at 105 s.

| id | finding | evidence | site | severity | state |
|---|---|---|---|---|---|
| L19 | The level-5 sampled-texture dump computed a DXT byte span as `width * height * block_size`, which is 16x the real span. The dump read past the texture and the scan statistics were meaningless. The length now uses block dimensions. | `sample-*` BC dumps were 16x their file names' block extents; `conv-1` 512x512 BC3 decodes to the Bandai Namco logo. | `MTLGSRender.cpp` sampled-image scan | L | fixed |
| L20 | The 0:00:52 FIFO desync reproduces the probe-2 failure earlier. The recovery loop reports the same command `0x2fd53cc`, then `0x3f800000` (float 1.0), and exhausts the 20-in-2-s limit. The GET pointer is reading data, not commands. | `RPCS3.log` `0:00:52.766248` onward; fatal in `RSXFIFO.cpp:765` / `RSXThread.cpp:3078`. | FIFO recovery; first suspect is a guest or backend write that corrupts the command stream. | H | open |
| L21 | The present path is sound for the early frames: `host-present-600` shows the ESRB notice correctly; `host-present-1` is white and is an early boot frame. | `/tmp/mtl4-dump/host-present-*`. | `MTLPresent.cpp` capture read-back | L | no action |
| L22 | The normal BC3 upload path is correct: `conv-1` 512x512 BC3 decodes to the Bandai Namco logo, and `conv-6`/`conv-7` 256x512 A8R8G8B8 decode to the Xevious panel. The panel is uploaded correctly, so its black screen is a draw, bind, blit or present issue, not an upload decode issue. | `/tmp/mtl4-dump/conv-1-*`, `conv-6-*`, `conv-7-*`. | `MTLTextureCache.cpp` upload | L | evidence |
| L23 | Video luma planes at ~52 s are very dark/flat (`sample-27` 1280x720 B8, `sample-5` 640x360 B8). This may be a genuine dark frame or fade, not proof of a sampling bug. | `/tmp/mtl4-dump/sample-*`; probe-5 log at 0:00:52. | video upload path | L | needs run |
| L24 | The level-5 upload histogram is 946 `Metal 4: upload` lines: 933 texture uploads (475 B8, 354 A8R8G8B8, 104 DXT5) and 13 vertex uploads. No DXT1/DXT23 texture appears after the first atlas, so DXT1/23 coverage is still thin. | `RPCS3.log` upload lines. | `MTLTextureCache.cpp` | L | evidence |

Resolution in this pass (built, 499/499 green):

- SF9 and TX12: both depth read-back sites stage the host depth and stencil planes, wait once,
  and CPU-encode the guest `z16_float` / `z24s8` pack.
- SF8 correctness: `scaled_image_from_memory` still returns false, but now flushes the source
  sections with a read invalidation and drops the destination sections with a write invalidation.
  The `nv3089` CPU fallback then reads current guest bytes and leaves no stale host copy. The GPU
  blit remains open.
- L19: the sampled-image byte span uses block dimensions for compressed formats.

The shutdown `Resource was destroyed whilst holding a resource reference!` lines remain open (L16).


## Run 2026-10-21 20:55 — probe 2, async interpreter variant, clean exit

Artifact: `~/Library/Caches/rpcs3/RPCS3.log`, 59,832,475 bytes, 397,107 lines.
Command: `MTL_PROBE=2` from `build/bin`; the process exited `rc=0` after 2:19, so the
graceful-shutdown and archive-flush paths ran for the first time.

Log composition: Metal probe 1,394 lines (0.35 %), `sys_spu` / `sys_event` / `sys_event_flag`
318,037 (80.1 %), `cellDmux` / `cellVdec` / PAMF 51,647 (13.0 %), remaining 25,629 (6.5 %). The
probe is not the bloat source. The coverage table printed twice (interval 10 and teardown), so the
latch totals below are run totals.

| id | finding | evidence | site | severity | state |
|---|---|---|---|---|---|
| L25 | Shader compilation is not the visible-bug path. The only `program/*` timers land in the first interval: `program/compile vertex` 54 calls / 1,164.6 ms, `program/compile fragment` 71 calls / 323.0 ms, `program/pipeline` 97 calls / 6.45 µs avg. Every later report is empty of `program/*`. All 97 links are asynchronous (`program/async submitted` = `program/async link ready` = 97) and none fails. | probe-2 report at `0:00:16.093`; coverage latches | `MTLProgramPipeline.cpp` | L | evidence |
| L26 | No draw is skipped and no pipeline fails: `draw/skipped pipeline pending`, `draw/skipped pipeline failed` and `texture/bind dropped` are absent (zero), and `session/archive loaded` is 1. The remaining visual failures are therefore not the async skip path and not a failed link. | probe-2 coverage latch table | `MTLGSRender.cpp` skip path | L | evidence |
| L27 | The run is clean of Metal faults: no MTL error, no failed pipeline, no dropped bind. The only RSX warnings are the benign `ROP reads from rN without writing to it` lines. | `RPCS3.log` | — | L | evidence |
| L28 | The guest texture formats in play are DXT45 (136) 396,544, DXT1 (134) 171,588, A8R8G8B8 (133) 129,696, DXT23 (135) 8,530 and B8 (129) 2,852, in that order. **DXT45 dominates.** The 1,615 host resources resolve to BC3 802, BC1 528, BGRA8 259, R8 13, `Depth32Float_Stencil8` 12 and BC2 1. If a decoded texture is wrong, the BC (block-compressed) route is the first suspect, and the 12 depth-format resources are the first suspect for a depth-as-colour read. | probe-2 coverage families `texture format` and `resource texture format` | `MTLGSRender.cpp` upload; `MTLTextureCache.h` `create_image` | M | evidence |
| L29 | The RSX thread is GPU-bound and drains the queue once per frame. In the last interval `recorder/wait idle` and `sync/label release` are both 5,197 ms over 204 calls (25.5 ms avg, 342 ms max) across a 10.0 s wall window, against `frame/gpu us` 21.4 ms avg (41.7 ms max). The `L13` label fix's drain is on the frame-critical path: the native fix is a GPU-side label write into shared memory, which unified memory allows, instead of a CPU drain. | probe-2 report interval `0:02:18.921` | `MTLGSRender::release_GCM_label`; `MTLCommandRecorder::wait_idle` | M | open |
| L30 | The semaphore wait is a spin and is the largest CPU cost on the RSX thread: `sync/semaphore wait` is 1,259 ms over 38,793,305 entries in the same 10.0 s window (12.6 % of wall) and the `sync/semaphore waits` latch reaches 584,190,283 over the run — about 190k entries per frame. At level 2 the timing scope itself is part of that cost, but the entry count alone shows the loop re-enters per frame. | probe-2 report interval `0:02:18.921` | `MTLCommandRecorder` semaphore wait | M | open |
| L31 | 383 of 3,803 flips (10.1 %) took the `present/compose refused` path because no display buffer was configured, so they presented a clear: `present/flip delivered` 3,803 = `present/compose ran` 3,420 + `present/compose refused` 383. A refused compose is a black frame, so part of the missing picture is present-side and not draw-side. | probe-2 coverage latches | `MTLFramebuffer` / present compose | M | open |
| L32 | The boot black screen is the intro FMV: `cellVdec` / `cellDmux` decode occupies the same window (51,647 lines, 13.0 % of the log) and no draw or pipeline error accompanies it. The FMV blank is a video-texture upload, bind or compose issue, not a missing shader. | `RPCS3.log` PAMF traffic | `MTLTextureCache.cpp` blit upload; present compose | M | open; probes added |
| L33 | The probe could not see correctness at all: it had no interpreter-vs-recompiler counter, no host-format record, no per-draw program id and no readback or depth record. This pass adds them, splits the bulk probe text to a dedicated file (`$TMPDIR/rpcs3-mtl-probe.log`) and drops the four flooding HLE channels to `error` while a probe level is set. | this pass | `MTLInstrumentation.h`, `MTLGSRender.cpp`, `MTLTextureCache.cpp`, `MTLFormats.cpp`, `MTLDevice.cpp`, `MTLShaderInterpreter.cpp` | M | fixed |
| L34 | Fourteen `MTL_PROBE_LATCH` sites used a ternary name. A latch registers its name once per call site, so each site counted every event under the variant that ran first and the second name never appeared. Every split family in this run's coverage table is therefore a branch total, not a branch count; the affected families are listed in the correction below. Three readings survive because both names are absent (`draw/skipped pipeline *`, `present/flip skipped`). | probe-2 coverage table | `MTLInstrumentation.h` macro contract; the fourteen sites | M | fixed |
| L35 | The FMV handoff stops the emulator. `MTL_PROBE=3` on the same build reaches ~300 frames, then `display_manager::stop_audio` (`0:00:44.945`), an `NV3089_IMAGE_IN … NOPed out due to empty regions` (`0:00:44.966`) and `FIFO error: possible desync event (last cmd = 0xbf800000)` (`0:00:46.574`) follow, and the RSX thread produces no further work. The probe's last RSX line is at t=13.7 s and the next, an `invalidate memory range`, is at t=89.9 s. `0xbf800000` is float −1.0, so the GET pointer reads vertex or constant data as commands — the same shape as L20's `0x3f800000`. No `Dead FIFO` occurs this time: the recovery loop absorbs the desync and the RSX thread then waits on an empty FIFO, so the title hangs on the last frame instead of dying. The PAMF demuxer spins at the same moment (97,084 `cellDmuxPamf` lines, 81 % of the log). | probe file; `RPCS3.log:21678`, `:21680`, `:22480` | FIFO recovery (`RSXFIFO.cpp`); the write that corrupts the stream is the open question | H | open |

The shutdown `Resource was destroyed whilst holding a resource reference!` lines still appear (L16);
this run's clean `rc=0` confirms they are teardown-only and not the fatal path.

**Correction — the split latches of this run are unreliable (L34).** Fourteen `MTL_PROBE_LATCH` sites
used a ternary name (`cond ? "a" : "b"`). A latch registers its name once per call site in a
function-local `static`, so each of those sites attributes every later event to the variant that ran
first, and the second name never appears. The totals in the table above that come from a split latch
are therefore *totals of both branches*, not of the named branch:

| family | reads as | actually |
|---|---|---|
| `sync/barrier cross pass` 696,249 | cross-pass barriers | all barriers |
| `feature/depth attachment` 1,265,264 | passes with depth | all passes |
| `feature/zcull query` 1,265,264 | passes with a query | all passes |
| `texture/resolved` 1,602,517 | resolved units | all sampled units |
| `bind/vertex table written`, `bind/fragment table written` 1,248,683 | written tables | all binds |
| `draw/not indexed` 1,248,683 | non-indexed draws | all draws |
| `draw/nonempty` 1,248,683 | non-empty draws | all draw records |
| `present/flip delivered` 3,803 | delivered flips | all flips |
| `present/source resolved` 3,420 | resolved sources | all composes that reached the resolve |
| `sync/semaphore wait/frame closed` 584,190,283 | waits with the frame closed | all semaphore waits |
| `overlay/clip space flipped` 4,898 | flipped overlays | all overlays |
| `transfer/interpolated` | interpolated transfers | all transfers |

Three conclusions survive, because the second name is absent *and* the first name is also absent:
`draw/skipped pipeline pending` and `draw/skipped pipeline failed` never fire (L26), so no draw is
skipped; and `present/flip skipped` never fires, so `present/flip delivered` is the true flip count
in L31. The `feature/*`, `bind/*`, `draw/not indexed` and `texture/resolved` splits must be read
again after the fix (`REVIEW-FINDINGS.md` M1) before they are used as evidence.

## Run 2026-10-22 02:31 — probe 5, level-5 capture, EBOOT video screen

Artifacts: `~/Library/Caches/rpcs3/metal4-artifacts/20261022-023406-probe5-level5/` (probe log,
RPCS3.log, 26 `fp-*.metal` + 38 `vs-*.metal` shader sources, 150 upload pairs, 64 sampled
textures, 28 host-image readbacks) and
`~/Library/Caches/rpcs3/metal4-artifacts/20261022-023046-probe2-ebootscreen/` (level-2 run, all 57
in-app screenshots). The run reached the EBOOT video screen, hung there and was killed.

| id | finding | evidence | site | severity | state |
|---|---|---|---|---|---|
| L36 | **The hang is not the probe.** A run with `MTL_PROBE` unset reproduced it exactly: `0:01:12` six `FIFO error: possible desync event (last cmd = 0xbf800000)` then `Dead FIFO commands queue state has been detected!`. The probe is exonerated; the hang is an ordinary Metal-independent bug (L37). | RPCS3.log of the interpreter-only run | FIFO / label path | H | open |
| L37 | **The desync has three distinct mechanisms, and the word that raises the error is not the one that broke the stream.** `FIFO_ERROR` has four sources: an unmapped FIFO address (RSXFIFO.cpp:115, :460), a malformed command word (:492), and two argument-fetch failures (:526, :551). `0xbf800000` and `0x80078000` both take the **malformed** path — the mask arithmetic in RR7-FREEZE-FORENSICS.md §4.1 applies unchanged, so that document's mechanism is confirmed, not displaced. But `0x10000` is **not** malformed: `0x10000 & 0xa0030003 == 0`, so it parses as a *valid* method header (count 0, method 0x10000) and desyncs the stream **silently**, with no error, until a later word trips the check. `0x10000` is exactly the `NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE` label value of L13. **A label value in the command stream is a mechanism none of the three ranked hypotheses in RR7-FREEZE-FORENSICS.md §7 covers.** | RSXFIFO.cpp:115, :460, :492, :526, :551; §4.1 mask table; the `0x10000` words in the 23:04 run | FIFO GET publication / the label write | H | open |
| L38 | **The label drain is the frame rate.** `sync/label release` costs 17,381 ms over 300 calls — **57.9 ms average, 470 ms maximum, one per frame** — while `present/display gap` averages 82.8 ms and `frame/gpu us` averages 8.68 ms. The GPU is idle 74 ms of every 83 ms frame. This is the L13 workaround running in production. | level-5 perf report, 300-frame interval | `MTLGSRender::release_GCM_label` | H | open |
| L39 | **A single texture readback blocked the RSX thread for 134.8 ms** (`texture/readback wait` and `texture/readback wait/flush`, one call each) to move 6,291,456 bytes. | level-5 perf report | `cached_texture_section::copy_texture` / `map_synchronized` | H | open |
| L40 | **The video plane is delivered by a path that almost never runs.** The 1408×768 A8R8G8B8 video surface is sampled with `upload_context = framebuffer_storage` (8) on 223 of 669 binds; every one of the 14 readbacks in the run was A8R8G8B8 from context 8. But only **2** readbacks occurred in the last 300-frame interval, and 6.29 MB in total. The content the sampler sees is therefore whatever the last readback left behind. | coverage families `texture bind context`, `texture readback *`; perf report | `texture_cache::copy_texture` | H | open |
| L41 | **The video surface is never written and never initialised.** The host-image readback of the surface at 0xc0880000 is `0xff` for every pixel except one row — an allocation that was never rendered into. The blit that should fill it is `NV3089_IMAGE_IN`, and it NOPs: "Operation NOPed out due to empty regions" (nv3089.cpp:43-47). The register dump shows the truth: `clip width: 0 height: 0`, `output width: 0 height: 0`, `DS DX: 0`, `DT DY: 0` — and the register file is the executing context's own file (`REGS(ctx)` is `ctx->register_state`, and the single static context owns the global `method_registers`), so the dump is accurate. **The guest's blit carries no geometry at all**, so the NOP is truthful and the "empty regions" wording is not the bug. The bug is that the surface is then *sampled anyway*: it is bound 223 times with `upload_context = framebuffer_storage`, and a Metal image that was never written is `0xff` (L43). | RPCS3.log `NV3089` dump; `host-target-4-...-0xc0880000-1280x720-fmt50.bin` | `NV47/HW/nv3089.cpp`; `texture_cache::create_new_texture` | H | fixed |
| L42 | **The black rectangle is not a texture-content problem.** The 8×8 DXT45 texture the black quad samples is opaque white in the guest and converts byte-identically (`guest-0-addrc483e080-fmt88-sw1-8x8-sz64.bin` == `conv-0-...`). The blackness is in the shading or the ROP, not in the upload. | `guest-0` / `conv-0` dump pair; draw-state trace | shader / ROP | H | open |
| L43 | **Metal never clears a reused image; OpenGL does.** The OpenGL reference clears a reused image that is not uploaded wholly from the CPU (`GLTextureCache.h:751-761`, `fast_clear_image`) and the Vulkan reference wipes the image when `initialize_image_contents` is set (`VKTextureCache.cpp:1096-1112`). Metal does neither, and a Metal image that was never rendered into is not even zeroed — the `host-target` readback of the video surface is `0xff` for every pixel but one row. **This is the real divergence, and it is the black-content bug.** | `host-target-4-...-0xc0880000-1280x720-fmt50.bin`; GLTextureCache.h:751-761 | `texture_cache::create_new_texture` / `upload_image_from_cpu` | H | fixed |
| L43a | **The create flags are a Vulkan-only concept, and Metal ignoring them is not a bug.** `create_new_texture` takes `rsx::flags32_t flags`; Vulkan uses `do_not_reuse`, `shareable`, `initialize_image_contents` and `mutable_format`, while **OpenGL discards them as well** (`GLTextureCache.h:724`). Metal matching OpenGL is correct, and the flag that mattered for Vulkan (`initialize_image_contents`) is the *clear* of L43, which Metal now does directly. | GLTextureCache.h:724, VKTextureCache.cpp:1013-1112 | `texture_cache::create_new_texture` | L | closed |
| L44 | **Neither Metal nor OpenGL checks the extent, the mip count or the format of a reused image**, and neither resets the guest pitch; `set_dimensions` updates the section metadata only, never the allocation. Vulkan checks the type and the depth and calls `set_rsx_pitch` (`VKTextureCache.cpp:1076-1082`). A section re-created with another extent or a longer mip chain therefore keeps the old allocation. Metal now discards an image that does not fit, so the hole is closed there; **OpenGL still has it.** | MTLTextureCache.h vs GLTextureCache.h:735-745 vs VKTextureCache.cpp:1076-1082 | `texture_cache::create_new_texture` | M | fixed (Metal) |
| L45 | **Metal's `create_nul_section` ignores `memory_load` and the tile reference, and so does OpenGL** (`GLTextureCache.h:833`). Only Vulkan uses them, because only Vulkan has the host-GPU DMA controller they describe (`VKTextureCache.cpp:1218-1222`). **Metal matching OpenGL is correct; this is not a Metal bug.** | GLTextureCache.h:833, VKTextureCache.cpp:1204-1230 | `texture_cache::create_nul_section` | L | closed |
| L46 | **Metal overrides neither `on_section_destroyed` nor `handle_memory_pressure`, and neither does OpenGL.** Vulkan overrides the first because its image is owned by the cache rather than by the section (`VKTextureCache.cpp:393-400`); the Metal section releases its own image in `destroy()` (`MTLTextureCache.h:135-149`), so the missing override is correct. **This is not a Metal bug.** The shutdown `Resource was destroyed whilst holding a resource reference!` is therefore not explained by it. | MTLTextureCache.h:135-149, GLTextureCache.h:446-466, VKTextureCache.cpp:393-400 | `texture_cache` | L | closed |
| L47 | **836 guest access violations in one run**, every one on a guest thread and every one handled, but 600 µs average and **136.5 ms maximum**. The Metal handler documents the RSX-thread case as a known stall ("Thread [rsx::thread] is too sleepy") and flushes in place (MTLGSRender.cpp:4234-4238). | level-5 perf report `fault` rows; `access violation 0xc1bd0008 / 0xc2fd0008, writing 1` | `MTLGSRender::on_access_violation` | M | open |
| L48 | **The semaphore wait is a spin**: 65,297,798 iterations in 300 frames (218k/s), 1.93 s of CPU. | level-5 perf report | semaphore wait path | M | open |
| L49 | **A texture is bound at guest address 0x00000000** — DXT1, 8×8, all-zero content (`sample-2-draw30-addrc0000000-fmt84-8x8.bin`). The `texture/bind null` latch is 0, so the null check did not catch it: the descriptor carried an image handle. | sample dump; coverage latch | `texture_cache::resolve_texture_bind` | M | open |
| L50 | **The video surface is allocated at the screen size and sampled at the video size.** The host image at 0xc0880000 is 1280×720 while the guest's texture descriptor for that address is 1408×768. | `host-target-4-...-1280x720-fmt50.bin` vs the draw-state trace `u0=0xc0880000/a5/1408x768` | surface allocation | M | open |
| L51 | The new level-2 texture probes are clean: `texture/create` 3, `texture/reuse` 0, `texture/upload partial` 0, no format, extent or mip mismatch. The reuse and coverage hypotheses are ruled out for this title's boot path. | level-2 coverage table | the new probes | L | evidence |

| L52 | **The §8 capture ran, and it rules out H3 and H4.** The capture (added to `thread::recover_fifo`) logs GET, PUT, the restore point, the code start, the guest address of the GET, the last label write and the eight FIFO words at the error. It fired seven times in one run: `get=0x10ae8ac put=0xcbf0dc get_guest=0x511ae8ac restore_point=0x10ae8ac code_start=0x10ae510 last_label=0x60300800/0x1 words: 02062000 00041a68 10030303 00041a7c 00000000 00041a6c 80078000 00041a74`. **The words around the error are all data, not commands**: read as headers they would be method indices 0x69A, 0x69B, 0x69D and 0x69F, and *none of those is a defined method* (RPCS3's NV4097 register map ends at 0x1980 and the attribute masks are at 0x1ff0), so the host is executing a **data stream as commands**. `0x10030303` and `0x80078000` in the same window are unmistakably register *values*, not headers. That rules out **H4** (GET off the header boundary), **H3** (a torn write) and **H1/H2** as the mechanism: the guest was writing commands normally right up to the error, and the host went off into data. | `RSX: FIFO desync capture` lines in `RPCS3.log`; `gcm_enums.h` for the method map | FIFO command stream | H | open |
| L53 | **Superseded by L56. The label write *is* the corrupting event.** L53 concluded that the label write was not the cause because the label address `0x60300800` is outside the FIFO's guest window `0x50100000..0x53200000`. **That reasoning was wrong: the iomap maps one guest page per FIFO page** (`rsx_iomap_table::ea[offs >> 20]`, `RSXIOMap.hpp:55-58`), so the FIFO's ring is **not** one contiguous guest range, and a guest address can fall inside it. The capture of 2026-10-22 12:0x shows the last commands the host dispatched were `NV4097_SET_SEMAPHORE_OFFSET` = `0x800` and `NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE` = `0x10000` (method indices 0x75b and 0x75c, `gcm_enums.h`), and `0x60300800 & 0xFFF == 0x800` — the offset *is* the label's low bits. | the dispatch history in the capture | `util::write_gcm_label` | H | superseded |
| L56 | **The host writes the label into the middle of its own pending command stream.** The dispatch history (the last eight commands the host executed before the desync) is: `NV4097_SET_ZCULL_STATS_ENABLE` 0x0, `NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE` 0x0, `NV406E_SEMAPHORE_OFFSET` 0x10, `NV406E_SEMAPHORE_ACQUIRE` 0x0, `NV4097_SET_SEMAPHORE_OFFSET` 0x800, `NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE` 0x10000, `NV406E_SEMAPHORE_OFFSET` 0x800, `NV406E_SEMAPHORE_ACQUIRE` 0x0. The last command before the desync is the label release. The label's guest address is `0x60300800`, its low twelve bits are `0x800`, and the host's `NV4097_SET_SEMAPHORE_OFFSET` argument is `0x800`. **The iomap maps one guest page per FIFO page** (`rsx_iomap_table::ea[offs >> 20]`, `RSXIOMap.hpp:55-58`), so the FIFO's ring is **not** one contiguous guest range, and a guest address can fall inside it. The write of the value `0x10000` then lands on a pending command header — and `0x10000` is exactly the rejected word in the 2026-10-21 runs. **The fix: the label write must be ordered against the FIFO.** The host knows the mapping, so it can test whether the label address falls in a mapped FIFO page and defer the write until the GET has passed that page; on hardware the write is ordered on the GPU timeline, so it lands after the ring has been consumed. | the dispatch history in the capture; `RSXIOMap.hpp:55-58` | `util::write_gcm_label` | H | open |
| L56a | **L56's mechanism is withdrawn, and L53 is confirmed.** The iomap is *not* scattered: the guest made one mapping, `sys_rsx_context_iomap(io=0x0, ea=0x50100000, size=0x3100000)`, and the arithmetic agrees — `0x50100000 + 0xcb5814 = 0x50db5814` is the capture's `get_guest`, and `0x50100000 + 0x1490000 = 0x51590000` is the other capture's. The ring's guest window is therefore `0x50100000..0x53200000`, and the label address `0x60300800` is **outside** it. **The host's label write does not land in the command ring.** The capture's new `label_in_ring` and `label_offset` fields measure this directly and will confirm it on the next run. The label release is the last *dispatched* command in the capture because the guest stopped there, not because it wrote into the ring. | `RPCS3.log` iomap line; the two `get_guest` values; `RSXIOMap.hpp:55-58` | `util::write_gcm_label` | L | closed |
| L58 | **The corruption is a fixed distance after the jump target.** In three of the four captures the error is exactly **0x39c bytes (231 words)** after `code_start`, the last jump target: `code_start=0x10ae510` / `get=0x10ae8ac`, `code_start=0xcbf13c` / `get=0xcbf4d8`, `code_start=0xcb5478` / `get=0xcb5814`. The fourth capture (`code_start=0x10af3b0`, `get=0x1490000`) is 0x3e050 bytes after its target and is the one whose window is eight floats. **The same jumped-to block is mis-parsed at the same word every time**, so the fault is in how the host reads that block, not in a race. The capture's dispatch history is widened to 256 entries and filtered to the commands from the jump target onwards, so the command at the fault can be named. | the four captures' `code_start` and `get` | the FIFO command stream | H | open |
| L57 | **The data words in the desync window are the guest's vertex attribute data and format registers.** The window `bf800000 3f800000 3f800000 3f800000 3f800000 3f800000 bf800000 3f800000` is eight IEEE-754 floats alternating −1.0 and +1.0, and the window `10030303 00041b7c 00000000 00041b6c 80078000 00041b74 02062000 00041b88` holds a packed colour and four words whose method indices (0x6db, 0x6dd, 0x6df, 0x6e2) are **not defined** in RPCS3's method table. The earlier window `02062000 00041a68 10030303 00041a7c 00000000 00041a6c 80078000 00041a74` names indices 0x69a–0x69f, also undefined. Registers 0x1A00–0x1BFF are the PS3's vertex attribute registers, and RPCS3's NV4097 map stops at `SET_VERTEX_DATA4S_M` (0x1980) with the attribute masks at 0x1ff0, so the host is reading the guest's attribute data and attribute format words as commands. | the capture windows; `gcm_enums.h` | the FIFO command stream | H | open |
| L54 | **The hang is a recovery loop that makes no progress, and the FIFO dies when the GET runs off the end of the map.** Every capture in the failing run has `restore_point == get`, so `recover_fifo()`'s `set_get(restore_point)` puts the host back on the *same* bad word; it retries seven times and then the GET reaches `0x3100000` — the exact end of the iomap window — where `get_guest=0xffffffff` and the address is unmapped, so the FIFO error becomes fatal (`Dead FIFO commands queue state`). **The freeze is therefore the recovery failing to advance, not a deadlock, and the root cause is the data word at a command boundary (L52).** | the capture sequence in `RPCS3.log` | `thread::recover_fifo` | H | evidence |
| L55 | **The FIFO's position accounting is self-consistent, and an earlier "one-word drift" reading of the capture was wrong.** `FIFO::read()` consumes the header and the first argument and leaves `m_fifo_pos` on the last word read; `read_unsafe()` advances by four per argument *and* its fall-through (`m_fifo_pos += 4; return false;`) supplies the final word when the argument block is exhausted; `skip_methods()` lands on the last argument and the same fall-through then lands on the next header. The count field is the number of arguments, so a command occupies `count + 1` words, which the capture's header spacing (two words per command) confirms. **No drift bug exists in this arithmetic.** | RSXFIFO.cpp:335-403, :495-520; the capture's word spacing | `FIFO_control` | L | closed |
| L59 | **The host follows a wild jump.** The capture of 2026-10-22 12:2x shows `cmd=0x3f48c8ca`, and `0x3f48c8c8 & RSX_METHOD_OLD_JUMP_CMD_MASK == RSX_METHOD_OLD_JUMP_CMD` (`0xe0000003` against `0x20000000`), so the word the host read was taken as an **old jump** and its offset field, `0x1f48c8c8`, was followed. That address is 300 MB past the PUT (`0xd3732c`) and outside the ring's window (`0x50100000..0x53200000`), so `get_guest` is `0xffffffff` and the next read is the unmapped error. The capture repeats 21 times with the same values, and `restore_point` is a *sane* `0x10a0e20`, so the recovery restores the GET and the host walks into the same jump again. **The wild jump is the most dramatic symptom of the same fault: the host takes a data word for a command.** | the capture's `cmd`, `get`, `put`, `get_guest`, `restore_point` | the FIFO command stream | H | open |
| L60 | **63 consecutive draws were refused at 55.461-55.528 s, 44 s before the desync, and they are a separate fault.** The probe's skip report gives the reason: `config valid 0` for 63 of the 64 refusals. The shared scissor setter clears `rtt_config_valid` when the rectangle is empty or outside the window clip (`RSXThread.cpp:2142-2149`) and only a later valid scissor restores it, so the guest drew 63 times with a scissor the RSX considers empty. This is shared behaviour, not a Metal divergence, but it is the cause of the **flashing** the operator reported, and it is 44 s away from the desync, so the two must not be conflated. | the probe's `should_skip_draw refuses the draw` lines and their timestamps | the shared scissor path | M | evidence |
| L61 | **macOS Low Power Mode was on and the machine was on battery for every measurement in this window.** RPCS3 logs it at boot: `SYS: Low Power Mode is enabled, performance may be reduced`, and `pmset -g` reports `lowpowermode 1` with `Now drawing from 'Battery Power'` at 40 % charge. **Every performance number recorded here - the 82.8 ms frame period, the 57.9 ms label release, the 20 fps - was measured on a throttled machine.** They are the numbers to beat, but they are not the machine's numbers, and they must be re-measured on mains power with Low Power Mode off before any performance work is judged. | the boot log line; `pmset -g batt` | the measurement environment | M | evidence |
| L62 | **54 guest memory ranges could not be pinned.** `VM: Failed to lock sudo memory (addr=0x..., size=0x...)` - the first is `addr=0x50100000, size=0x3300000`, which is the FIFO ring's own base and extent. `vm::lock_sudo` is `mlock` (`vm.cpp:1170`, `util/vm_native.cpp:458`), so the guest's pages are not pinned. On Metal with unified memory the guest memory is directly addressable, so the failure is benign for this backend; on a discrete GPU it would not be, because the host DMA needs pinned pages. It is an error line that clutters the log and it is the guest's memory, not the backend's. | the 54 log lines | the VM, not the backend | L | closed |
