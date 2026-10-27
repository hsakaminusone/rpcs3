# Handoff — the RSX to Metal 4 backend

This document hands over the Metal 4 backend. Read it first. Then read `METAL4-TODO.md` for the live
work list, `REALISATION.md` for the per-cluster status, `REVIEW-FINDINGS.md` for the itemised
defects, and `CONFLICTS.md` for design/tree disagreements.

**This revision (2026-10-27, after run U).** The live state is in §0, the datapoints in
`INVESTIGATION-2026-10-22.md` §10.12–§10.20, and the findings in `LOG-FINDINGS.md` (open first).
The results since the last revision:

- **GET was licensing the guest to recycle a chunk the parser was still in (L145), and run U
  confirmed the fix on device.** Run T parked at the boot handover `A+0x9d4` with `word=0x30`
  after the parser spent 188 ms inside A on a label acquire. `sync_get()` published the end of the
  prefetched block, not the executed position, and the guest reused A's link word. The fix is the
  contract, not another sample: GET now publishes `m_fifo_pos`; `cellGcmCallback` writes the jump
  before publishing PUT; the L141/L144 publish-sample layer is deleted; and the stream frontier is
  enforced in fast mode too. Run U consumed the live `0x210a0000` at `A+0x9d4`, entered B, and made
  2,892 cross-buffer handovers with no `Dead FIFO`, desync or read-ahead. 12 `FifoStreams` cases,
  516/516 full suite.
- **The black video is not the alpha test.** Run 2 compiled the alpha test out of every program and
  captured the video draw itself (frame 879, draw 12262, `fp 100`): three non-empty B8 planes, a
  full-screen quad, every state field correct, and four colour patches left at exactly
  `(0,0,0,0)`. The remaining question is "wrote black" versus "never ran", which colour alone cannot
  answer (L110, L111).
- **The run-2 freeze was the capture, not the backend.** The 670-draw race-start window took eight
  synchronising readbacks per draw; queue lag went from 2.9 s to 94.8 s, the queue then raised 588
  `MTL4CommandQueueErrorDomain` failures, and the game stalled at the race start. The depth probe
  that was supposed to be the decisive second witness returned garbage and is removed; a window now
  probes at most 64 draws (L112, L113).

- **The boot stall is the label handshake, not the link.** Run K captured the boot chunk tail
  before it was recycled: it ends with a non-flip `NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE`
  (`0x75c`, `0x60300800`, `0x1`) and the next `NV406E_SEMAPHORE_ACQUIRE` (`arg=0`) reads the old
  `0` and takes the `sema == arg` fast path. The release was submitted (L134), but the acquire
  never waited for the GPU copy, so the parser parks on the genuine `0` link at `0xcb09d4`
  (L135). The fix is a targeted `sync_label(address)` hook: Metal commits the buffer that carries
  the write and waits only for that slot's feedback before the acquire re-reads the word. VK
  submits the label immediately in its own secondary command buffer and GL hands it to the host
  DMA writer, so neither needs this wait. The old `put == addr` guard was still a real hole for
  the healthy link case and stays fixed (L133).

- **Run L: the wait fires, but the guest word still reads zero.** With L135 in the binary, the
  boot release's slot is committed and its feedback delivered (`sync/label wait` latched 2,
  `recorder/re-enter slot/wait` max 235.7 ms, feedback lag max 18.7 ms), and `sync/label verify`
  still prints `sudo=0x0 guest=0x0`. **The destination was the fault:** the Metal device imports
  the guest window at 4.58 s, but the RSX reports page that holds the label is mapped at 18.96 s,
  and a no-copy import is fixed at creation (L136). A standalone remap experiment reproduces the
  failure exactly. Labels now write through a per-page import created after the page exists and
  cached for the process (L137), and the same-commit readback (`gpu=`) plus the acquire's re-read
  word are in the build for the confirming run (§10.19).

The `MTL_PROBE_FORCE_FP=<fp id>` witness remains available for the black video: it forces one
program's colour output to magenta after every ROP stage and clears its discard (L114), which
separates "wrote black" from "never ran" without a GPU debugger. The FIFO desync is fixed in code
(L132) and at the equality path (L133). Run L supplied the label wait but not the word; L136 found
the stale destination and L137 moves the label write to a per-page import. **Run M confirmed L137
on device:** `sudo=0x0 guest=0x0` at 13.304 becomes `sudo=0x1000000 guest=0x1000000` at 13.575, the
acquire reads `sema=0x1`, and the FIFO ran 5,060 chunks / 129,233 draws / 3,000 flips. The
`0xcb09d4` boot park is gone.

**Run M then froze at a later frontier (L138/M1).** After two minutes, at a track-load transition,
the parser parked at `addr=0xcbe890 frontier=0xcbe890 put=0x10ae994 word=0x0`: the acquire before
it returned, but the cross-buffer link word after the acquire was never written. The guest kept
running (allocations to 02:22, `CnuSound2Thread` at 02:24), and the parser polls an empty FIFO, so
the word really stayed zero. **Run P then showed the mechanism (L140):** the missing word is the
guest's park area, and the frontier rule parked the parser on its first NOP instead of reading it.
The fix is in the tree. The same run showed the reported
**flickering black patches**; a classification run (`MTL_PROBE_REPLACE_TEXTURES=1`, red one-texel
replacement for every sampled unit) is the next measurement: red patches mean texture content,
black patches mean the surface/clear path.

**Run N (classification attempt) never got to gameplay.** The same build plus
`MTL_PROBE_REPLACE_TEXTURES=1` passed the label handshake (`sudo=0x1000000 guest=0x1000000` at
13.106, acquire `sema=0x1`) and then hit L138 at boot instead of at a load transition: the parser
reached the boot chunk end `addr=0xcb09d4 frontier=0xcb09d4`, PUT had moved into the other buffer
(`put=0x10a0e20`), and the word there was `0x30`, a NOP-class data word, not a link. The run logged
1 subdraw, 3 vertices and 27 composed flips, so the display showed only the empty frame: the
complete black screen is the boot park, not a rendering regression and not the patch bug. It also
means the red/black classification still needs a run that reaches the track.

**Run O (clean `MTL_PROBE=2`) reached gameplay and then reproduced L138 there (15,882
subdraws).** It parked at `addr=0xcc01f8 frontier=0xcc01f8 put=0x10b01f8 word=0x10000` — PUT in
the other buffer — and the word at the parked frontier changed to `0x4025` 1.4 s later. `0x4025`
passes the RSX *new-jump* test, so the frontier link predicate accepted a data word and jumped to
`0x4024`, producing two `possible desync event` lines. The frontier test now accepts only the
encodings the guest emits; the probe built in this tree records the frontier word for the last 8
PUT moves and any change while parked, which separates "link published after the PUT move" from
"chunk recycled under the parser".

**The label path follows the setting (L139).** Metal forced `supports_host_gpu_labels` true, so
every run paid the GPU write and the acquire's wait — the only block inside the FIFO loop that VK
and GL do not have. `MTLDevice::fill_backend_config` now reads
`g_cfg.video.host_label_synchronization`, default off: the release is a host store into the shared
guest page (`nv47_sync.hpp:83`), the acquire does not wait, and the per-page import plus
`sync_label` run only when the setting is on. The init log prints which path the run took.

**The enabled path now waits on a completion event (SY14).** Every commit takes the next value of
one `MTLSharedEvent` and `MTL4CommandQueue::signalEvent` marks it done; `begin_frame`,
`reenter_slot`, `wait_for_slot`, `wait_idle` and `shutdown` poll `signaledValue` with the existing
10 s bound. The feedback handler is left with commit diagnostics and the ZCULL counter, and the
recorder's mutex and condition variable are gone. A satisfied wait is one atomic load; a real wait
is a short pause spin and then a 50 µs sleep.

**Run P: the host-store path freezes the same way, so the label is not the cause of L138.** The run
started with `guest labels are written by the CPU (host store)`, no GPU label and no `sync_label`,
and still parked at 0:00:54.940: `addr=0xcb6264 frontier=0xcb6264 put=0x10ab0e0
word=0x2062000`. The probe shows every earlier PUT move left a real link at the frontier
(`0x20cb0000`/`0x210a0000`); only the last move left data (`put[7] old_word=0x2062000`). Both label
paths fail, which settles the setting as orthogonal.

**The park is the guest's park area, not garbage (L140).** At 54.957 the word at `0xcb6264` became
the jump `0x3020300`, 17 ms after the PUT move: the guest published the link late, and the parser
followed it correctly. `0x3020300` holds a count-0 NOP with PUT still in the other buffer, so it is
the zero-filled park the guest jumps to while it prepares the next chunk and patches afterwards.
`stream_tracker::on_jump` gave that unknown target a strict frontier equal to the target, so the
parser refused to read the NOP (`addr >= published_end`, word not a link) and never saw the patch;
`fetch at/after PUT fetch=0x3020300` repeats for the rest of the run. Hardware follows the jump and
reads whatever is there. **Fixed:** a target more than `max_put_gap` above PUT is read unbounded,
exactly as static code below the streams already was, while a target PUT names keeps its strict
frontier, so the published-end rule for the two command buffers is unchanged
(`FifoStreams.ParkAreaAboveStreamsIsReadUnbounded`).

**The 20:55 probe-2 run** is the first clean run of the final binary (L25–L33 in `LOG-FINDINGS.md`).
It exits `rc=0` after 2:19, so the graceful-shutdown and archive-flush paths ran. It reaches the
race with no `Dead FIFO`, no `should_skip_draw` refusal, no failed link and no skipped draw
(`draw/skipped pipeline pending == 0`, `draw/skipped pipeline failed == 0`), and all 97 links are
asynchronous. So the remaining visual failures are not the skip path, not a failed compile and not
the FIFO path. What the log does show is: DXT45 textures dominate (396,544 of 708 k bound formats),
the RSX thread is GPU-bound at 21.4 ms a frame and drains the queue once a frame
(`recorder/wait idle` 25.5 ms avg), the semaphore wait is a spin worth 12.6 % of wall, and 10.1 % of
flips compose with no display buffer and present a clear. The interpreter-eligibility, texture-format,
readback and depth probes that this pass adds are the next evidence step; they were not in the
binary under test.

**The diagnostics this pass.** The bulk probe text (the perf report, the coverage table and the
level-3/5 verbose family) now goes to its own file, `$TMPDIR/rpcs3-mtl-probe.log`, so a deep run can
be read without the HLE channels on top; warnings and errors stay in the shared log. While a probe
level is set, `sys_spu`, `sys_event`, `cellDmux` and `cellVdec` drop to `error` for the run, because
they were 93 % of a 397 k-line log; that block is marked `TODO(revert)` and is not part of the
backend. The new families are `draw/interpreter` vs `draw/recompiler`, five
`interpreter/reject …` reason latches, `texture host format`, `texture upload context`,
`texture/bind null fragment` and `texture/bind null vertex`, `texture readback format` and
`texture readback context`, `surface/depth plane decode` and `surface/depth plane encode`, and the
`draw vertex program` and `draw fragment program` hashes at level 3. The CPU tests are green; the
interpreter run gate (`interpreter_only`, the standalone MSL probe) still needs an approved run. The
remaining known visual failures are the black Xevious panel, the black race-selection background
video and the black FMVs.

---

## 0. Start here

**The state.** The backend builds, boots, renders the guest frame upright, and reaches the race. Run 6
(2026-10-24) closed the large visual defects: the intro FMV picture, road, car bodies with liveries,
guardrails, bridge, sky, HUD and the Xevious mini-game panel all render, after `capture_front_face`
stopped inverting the guest's winding a second time (L116, DEVIATIONS-DRAW D5, **device-confirmed**).
What is left visually is the colour the video program computes (FMV flicker/black patches, L110
invalidated by L116 and to be re-validated) and the L99/L105 car-probe placement. Runs C and D
(2026-10-25) quit in the known black title FMV (L12); run D repeated the boot stall. Run K
(2026-10-26) named the label half: the release is submitted, but the acquire read the pre-release
word and skipped the wait (L135); the per-page import (L137) fixed the boot handshake, and the
setting now selects the label path again (L139). **The L138 freeze is the guest's park area
(L140), fixed in the tree:** the parser followed the guest's late link correctly and then refused
to read the park's first NOP because it had given the target a strict frontier. **Run T then named
the recycle half (L145):** the boot handover parked at `A+0x9d4` with `word=0x30` because GET had
already acknowledged the prefetched block end, so the guest reused A while the parser spent 188 ms
inside it on the label acquire. `sync_get()` now publishes the executed position, `cellGcmCallback`
writes the jump before PUT, the L141/L144 publish-sample layer is deleted, and the frontier applies
in fast mode as well. **Run U confirmed L145 on device:** the live link at `A+0x9d4`, 2,892
cross-buffer handovers, no `Dead FIFO`, no desync. The run reached gameplay and the visual defects
of §7.1–§7.3 are unchanged, so they are not FIFO symptoms. The completion-event wait (SY14)
remains unmeasured.

The FIFO desync is **no longer parked at the user's request**. It is now one class with an exact
proof — a guest *data* word read as a command header, always exactly `0x39C` after the last jump,
reported by the malformed-command branch and replayed by recovery until `Dead FIFO` (L117–L121,
`INVESTIGATION-2026-10-22.md` §10.11). The 2026-10-25 run (L122–L131, §10.12–§10.13) excluded the
two boundary events that were left: there is no phase slip and no jump past PUT in a whole 65 s run.
It also measured the producer: two command buffers of at least 136 KB at `0xcb0000` and `0x10a0000`, with `put`
alternating between them at the same offset. What fails is the event that layout creates: the app
moves `put` to the other buffer while the parser waits on it (50 times in 6 idle seconds), and in
the failing frame the chunk ended with a link into the other buffer `0x2B0C` past its published end
(L125–L127). The probes could not see that link because every test in them is numeric and the
buffer being read has the lower address (L130). Vulkan and OpenGL write the label GPU-side; Metal
drained the GPU on the RSX thread, which is why the race is reachable here (L129).

**The label write is on the GPU only when the setting is on, and the acquire then waits for it
(L131/L134/L135/L139, §10.13/§10.16).** The default is the CPU host store (`L139`): the word is
written at the release and the FIFO reader never waits, which is what VK and GL do. On the GPU
path, `release_GCM_label` stages the guest word and emits one compute-encoder `copyFromBuffer` per
batch at the next encoder boundary, under one queue-scope barrier, so the write is ordered after
the work ahead of it and never before it. The old drain survives only as the fallback for a label
outside the guest window. **Runs C/D found one hole:** the batch was committed only from
`on_semaphore_acquire_wait`, which this guest never calls — the profile has zero
`sync/semaphore waits`. The task parks in `run_FIFO`/`FIFO_EMPTY` instead of the `cpu_wait` path,
the staged writes were never submitted, and the producer stayed at `0xcb09d4`. The 2 GPU latches in
run C were staging events, not visible writes. VK submits the label in its own secondary command
buffer immediately (`VKGSRender.cpp:1744–1759`); GL hands it to the host DMA writer
(`GLGSRender.cpp:1445–1457`). `release_GCM_label` now commits the back-end batch at the release,
without waiting. **Run K found the second hole:** the release is submitted, but the adjacent
acquire reads the old guest word before the GPU copy and returns through `sema == arg` (L135).
`semaphore_acquire` now calls the backend `sync_label(address)`; the Metal recorder keeps the label
addresses of each command buffer, commits the buffer that carries the write, and waits only for
that slot's completion event (SY14). No acquire on the address means no wait. The next run must
show the wait, the acquire re-reading a non-zero word, and the producer's next chunk; the race's
larger label count remains the wider confirmation.

**The FIFO frontier fix is written and tested (L132, §10.14).** `FIFO_control` now tracks the
published end of each command stream instead of treating the single guest `put` as the end of the
buffer being read. A word at the frontier is consumed only if it is a link (jump/call/return); a
method packet is dispatched only when all of it is published; and the cache is capped at the
frontier, which removes the word-at-PUT over-read that produced the 2:36.620 failure. The boot
stream stays linear. Seven new unit cases model the boot advance, the mid-fill PUT, the stale-tail
park, the call/return path and the frontier-at-PUT link; the full suite is 511/511. **Run C found a
hole in the equality path (L133, §10.15):** the old `put == addr` guard returned `FIFO_EMPTY`
before the frontier rule, so the healthy link case could never consume the link at PUT. The equality
now takes the frontier path. **Run D then showed the run-C stall was not that path:** the word at
the park was `0x0`, so the parser was correctly waiting; the producer was stalled by the label
handshake. **Run K showed the release is submitted but the acquire skips its wait (L135).** The
next run must show the label wait, the acquire re-reading the release, the link jump and the next
chunk.

**The build and run loop.** `ninja rpcs3_emu` compiles the Metal work (no Qt, ~1 min);
`ninja rpcs3` links the app bundle and is only needed before a run. Diagnostics must be excised
before a commit; the variant switches get their own archive suffix (`-noalpha`, `-forcefp`).

**The first moves, in order.**

1. Read `METAL4-TODO.md`, then `INVESTIGATION-2026-10-22.md` §10.12 and `LOG-FINDINGS.md` §1.2.
   Nothing else is more current.
2. One approved run with `MTL_PROBE=2`, default config (host-store labels). The label probe is now
   confirmatory: run P already showed `guest labels are written by the CPU (host store)` with no
   `sync_label`, and still parked at `0xcb6264`. What this run must show is the completion-event
   wait (SY14): `recorder/begin frame` and `recorder/re-enter slot` near zero in steady state, no
   `did not complete in 10 s` line, and no `recorder/wait idle` spike that the old condition
   variable did not have. Then, once it reaches the race, the L138 probe again: `park put[*]` prints
   the frontier word at each of the last 8 PUT moves, and `park word change` prints any change under
   the parked parser. Run P's `old_word=0x2062000` is a link published 17 ms after the PUT move,
   and the jump it carries is the guest's park area (L140); the fix is in the tree.

   The FIFO handover is the health check: the end of the boot chunk, `jump cross-buffer
   from=0xcb09d4 target=0x10a0000` within milliseconds of the `CALL 0`, then continuous A/B chunks.
   A mid-fill `PUT move` with `parked=1` and no `desync` is healthy; `resume at former PUT` followed
   by `0x10000` is the old failure and must not appear. `NOP run`, `jump cross-buffer` and the
   32-entry jump chain are still logged. **The black FMV is L12:** wait through it (or skip it) and
   judge the parser on the 3D scene and the race. In run B the desync arrived after the title video
   closed, at 2:36. Read `~/Library/Caches/rpcs3/RPCS3.log` in time order and take the first
   `FIFO diag:` line before each `FIFO error`.

   F7 is decided and in the tree (L139): `MTLDevice::fill_backend_config` honours
   `g_cfg.video.host_label_synchronization`, off by default, so the release is a CPU store and the
   FIFO reader never waits, exactly as VK's default. The GPU write with the per-page import runs
   only when the setting is on, and its acquire now waits on the commit's completion event (SY14).
   A second run with the setting on is the A/B for that path; it is not needed to judge L138.

   Run R (L141 in the tree) passed the old boot freeze and ran to 1:18, then parked at `0x10b1b78`
   while PUT was in the other buffer and desynced at 1:22.24 (L142). The redirect is the fix for that
   park. The next run must reach the same movie/level transition and either consume the abandoned
   chunk without a park or log `FIFO diag: redirect from=… target=…`. A redirect at the transition
   is healthy; `park word change` followed by `CALL found inside a subroutine` is the old failure.
3. Once the desync event is named — or confirmed absent — the visual remainder is the video
   program's colour (L110 must be re-validated, it was invalidated by L116) and the L99/L105
   car-probe placement.
4. Do not rebuild the depth probe as it was; the readback never produced depth (L112).
5. Excise the diagnostics and commit only after a fix is confirmed. The winding fix is
   device-confirmed and is the next commit.

**The context that saves a day.**

| fact | detail |
|---|---|
| the backend renders | fixed-function, surface, clear, 2-D fill, blit, present and 3-D geometry run end to end |
| the image is upright | draw viewport negates height, scissor mirrors, winding inverts (F24) |
| the present architecture is correct | drawable is the present image, output tier blits into it, layer presents; KK does the same |
| the freeze is fixed | a flip opens the frame and presents; acquire does not submit; feedback wait is bounded |
| the cube abort is fixed | `make_descriptor` sets `arrayLength = 1`; the six RSX cube faces are addressed through the upload slice. |
| the race-start fatal is fixed in code | the probe-2 log dies after 21 FIFO desync recoveries; the label value `0x10000` is read as the next command. `MTLGSRender::release_GCM_label` wrote the label on the CPU after a drain; it now writes it on the GPU, so the RSX thread never waits for a label (L131). **The 20:55 run is clean over 2:19 with no `Dead FIFO` (L25).** |
| the FIFO desync is **root-caused and fixed in code** | a guest data word read as a command header, always `0x39C` after the last jump, via the malformed branch; recovery replays the same word until `Dead FIFO` (L117–L121). The 2026-10-25 runs excluded boundary drift and named the trigger: the producer moves `put` to the other buffer under a waiting parser, and the word at the parked position is not written for this pass. The cache also served the word **at** PUT (2:36.620). The per-stream frontier, exact cache bound and packet atomicity are in the tree (L132). Run C showed that the old `put == addr` guard could block the healthy link at PUT; the equality now takes the frontier path (L133). Run D's park word was `0x0`, so the boot stall was not the link but the unsubmitted label (L134). One run after the label fix. |
| the guest's FIFO has **two buffers** | `0xcb0000` and `0x10a0000`, `0x3F0000` apart, each at least 136 KB; `put` alternates between them, normally naming the buffer the parser is *not* reading (L122, L123). |
| the two-buffer switch is a **handshake, not a fixed jump** | the guest publishes a chunk and then waits for GET to reach its end before it publishes the other buffer. The link at the chunk end is written late or not at all. When the parser is parked on that end and PUT names the other buffer, it must leave the chunk and read the published buffer from its base, not wait for a link (L142). Run R waited 15 s at `0x10b1b78` and desynced at 1:22.24. |
| the NOP path is **bounded now** | the frontier check runs before every fetch, so a count-0 header can no longer walk through unwritten zero memory; the old path reached the bad word through `0x00000000` at `0xcbf57c` (L127, L132). |
| the probes were **blind to the link** | read-ahead, jump-past-PUT and rewind are all numeric against `put`, and here the buffer being read has the lower address, so a jump from A down into B satisfies `target <= put`. The discriminator is a slab test, `(addr ^ put) & ~0xffff`, logged as `jump cross-buffer` (L130). |
| Metal parked the parser **because of the label drain** | VK/GL write the label GPU-side and keep work in flight; Metal drained the GPU on the RSX thread, which held the RSX at the GPU's rate. The park/catch-up cadence itself is normal on all three backends; the drain is why the race is reachable here, and L131 removes it (L129). |
| the boot stall was **the unsubmitted label** | `release_GCM_label` staged the back-end write in the open frame, and the batch was committed only from `on_semaphore_acquire_wait`; this guest parks in `run_FIFO` and never calls it, so the write never reached the GPU and the producer stayed at `0xcb09d4` (L134). VK's immediate secondary CB and GL's host DMA writer both submit without a frame commit. The fix commits the back-end batch at the release. |
| the remaining gaps vs VK/GL are **enumerated and ordered** | `GAP-PLAN.md` (2026-10-25) re-reads every deviation row in the code: D1–D3, D5–D7, D9, S1–S9/S11–S13, TX3/TX7/TX9, SY1–SY5/SY9 are fixed and the audit rows were stale. The open work is DR10 (pass per clause), SY10, VX4, SF14, TX10/TX11/TX20, F12, E7, the parked D4 and the capture-gated SY7/SY8/SY11. |
| `0x00010000` **is** non-method | `0x00010000 & 0xa0030003 = 0x10000`; L37's older claim was wrong. It is the argument of `reg 0x75C`, not a header (L37, L84, L118). |
| the parser now **stops at the published end** | `fetch_u32` refuses `addr >= frontier`; the word at the frontier is consumed only as a flow-control link, and the method packet must be fully published. This includes `addr == put`, where the link is the one published exception (L132, L133). |
| the black video is **not** an upload or format problem | the three B8 planes carry picture (L95), `B8 → R8Unorm` matches Vulkan (L96), and the video draw's state is complete (L101). With the alpha test compiled out it still leaves the target at `(0,0,0,0)` (L110), so the remaining split is "wrote black" versus "never ran" (L111, L114). |
| the black rectangle's texture is correct | the 8×8 DXT45 it samples is opaque white in the guest and converts byte-identically, and the write mask is `0xf` into an A8R8G8B8 target. The blackness is in the shading or the ROP, not the upload. |
| the async link crash is fixed | Metal 4's completion-handler `newRenderPipelineState` corrupts the pipeline data set serializer (L18). Links now run through one synchronous worker; the 10:21 scan reports 66/66 ready, 0 `MDB_*`, 0 failures. All 97 links in the 20:55 run are asynchronous and none fails (L25). |
| the desync history | the `Dead FIFO` of the 20:55/22:05 runs is the L52–L54 mechanism, intermittent; the 2026-10-22 runs reproduce it with no probe (L36). Run 2 adds one capture at 53.451 s (L115). |
| the decode is not the video's problem | frames decode and no draw error is raised; the log's PAMF/vdec traffic is real work, not a stall (L32, L95). |
| the texture blame is DXT | DXT45 396,544 + DXT1 171,588 + DXT23 8,530 of 708 k bound format instances, against A8R8G8B8 129,696 and B8 2,852 (L28) |
| TX9 is built, not proven | typeless copies stage through a shared buffer; the RR7 path has not been confirmed to use it |
| the diagnostics are switchable | `MTL_PROBE` levels 1–5; a probe run writes its bulk text to `$TMPDIR/rpcs3-mtl-probe.log`; `MTL_DEBUG_LOGS=0` / `MTL_PERF_ENABLED=0` remove all of it |
| Vulkan with KosmicKrisp loads the title | the guest stream is sound; a divergence is in this backend |
| the commit identity | `hsaka -1.0 <340957754+hsakaminusone@users.noreply.github.com>` |
| the language | ASD-STE100 Simplified Technical English |
| the scope | `metal4` only, pushed to `origin/metal4`, without breaking Vulkan or OpenGL |
| one instance, never detached | kill-before, one tracked child, a cap, kill-after |

---

## 1. The backend in one page

### 1.1 What it is

RPCS3 emulates the RSX. Vulkan reaches Metal through MoltenVK or the KosmicKrisp driver. The Metal 4
backend removes the translator and drives the Metal 4 queue, command buffer and encoders directly.
It keeps the shared RSX emulation behind `rsx::thread` and the backend capability struct. The shared
part is about 55,000 lines; the backend is a second implementation of the graphics resource layer.

### 1.2 Scope and the RSX-first rule

Scope: parity with the Vulkan backend's RSX feature set. There is no Metal 3 fallback; MoltenVK is
the pre-Metal-4 fallback. The RSX is the source of truth. The design asks what the RSX does and what
the smallest Metal 4 expression is. Performance centres are the encoder boundary, the pipeline
cache, per-draw CPU work and real hazard barriers.

### 1.3 Target and environment

| item | value |
|---|---|
| SoC | Apple A18 Pro |
| OS | macOS 27.0.1 |
| memory | 8 GB |
| deployment target | macOS 15.0 |
| Metal 4 API floor | macOS 26 |
| backend runtime floor | macOS 27 |
| build | `./build`, Ninja, one build at a time, `-j4` |

The deployment target stays 15.0 so the binary loads on older systems and the backend declines at
run time. Metal 4 exists on Apple silicon only.

### 1.4 Where the code is

| role | location | note |
|---|---|---|
| Metal subsystem | `rpcs3/Emu/RSX/MTL/` | about 16k lines, 67 files |
| Metal tests | `rpcs3/tests/test_rsx_metal4_*.cpp` | about 4k lines, 150 MTL cases |
| shared RSX | `rpcs3/Emu/RSX/Common`, `Core`, `Program` | unchanged; all backends call it |
| design docs | `docs/research/metal4/design/` | this file is the master state |

The five exception sites are real error paths, not stubs. The temporary diagnostics are named by
`TODO(excise)` and are removed before the commit.

---

## 2. The rules of the work

**Evidence.** RSX behaviour is the source of truth. Evidence order: the RSX state inventory and
headers, the two shipping backends, the shared emulation, the RSX documentation. A design/tree
disagreement goes into `CONFLICTS.md`; neither side is edited in silence.

**Completeness spine.** The inventory holds 282 rows, 203 live, 178 work dials (133 `reshape`, 45
`emulate`). A coverage test fails when a live row has no disposition. “Wired” means a C++ test
writes the register, reads the latch and checks the field; it proves decode, not correct use.

**Probes.** When a mechanism is not derivable, write a standalone device probe, run it with the
validation layer on and off, keep the probe and the log, and record the measurement. A result that
changes between the two runs is a tool artefact.

**Subagents.** Read-only, no build/run/commit/push, no edits to `.kk-mesa` or `filesdump/`, exactly
one report document, report `file:line` + severity + confirmed/hypothesis.

**Build and test.** One build, four jobs. Change; build app and tests; run tests; run the document
linter and the inventory validator. A change is complete only when both pass.

**One emulator instance, never detached.** Kill before, track one child, cap the run, kill after,
confirm none is left. Concurrent or orphaned instances freeze this 8 GB machine. The local patch
replaces the `macdeployqt` post-build step with `/usr/bin/true`; keep it, and never set
`-DMACDEPLOYQT_EXECUTABLE` to get the same effect.

**Diagnostics.** `MTL_PROBE=1` is the counters and periodic report; `2` (`trace`/`verbose`) also
raises the `MTL` channel. `MTL_PROBE_INTERVAL` and `MTL_PROBE_STALL_MS` tune two probes. Every site
is `TODO(excise)`. `MTL_DBG`/`MTL_DBG_ON` must not exist, and no trace line may sit on a path that
can spin.

**Kill cover.** `pkill -9 -f` self-matches. Use `pgrep -f 'rpcs3.app/Contents/MacOS/rpcs3'` and
`ps -p <pid>`. Never `pkill -f rpcs3`.

**Config.** `Renderer:` in `~/Library/Application Support/rpcs3/config.yml` selects the backend.
Copy and restore the file for a temporary Vulkan reference run. KosmicKrisp needs no environment
variable; the loader points at the bundle's own ICD.

**Capture.** Display capture and keystroke injection are refused by TCC. The user presses keys and
reports. F12 screenshots land in `~/Library/Application Support/rpcs3/screenshots/<title id>/`; a
black frame is a few KB, a content frame tens of KB.

---

## 3. What is built

**Substrate.** A placement-heap pool per storage class, a free-list suballocator, explicit
residency sets and a per-slot deferred-release list. The guest window is one no-copy buffer over
`vm::g_sudo_addr`. The command recorder has three frame slots, one allocator, one reusable command
buffer and one argument table per stage; it is the only way to open a pass, close an encoder or
submit. The barrier chokepoint carries producer, consumer and visibility; a `false` return asks the
pass scheduler to split. The surface store projects one guest range to an owned texture, a resolve
target and deferred data movement.

**Pipeline and binding.** The key is the shared properties struct. The argument table replaces
descriptor sets, layouts and push constants; its order is mutate, bind, draw. Cull, front face,
fill, depth clip, bias, viewport, scissor, stencil references, blend colour and the depth-stencil
object are encoder state and key nothing.

**Shaders.** Guest ucode → decompiled GLSL (Vulkan rules) → SPIR-V (vendored glslang) → MSL
(vendored SPIRV-Cross) → Metal library. The chain keeps about 5,000 lines of shared emulation. No
text post-passes. Programmable blending uses the subpass-input form. Both SPIRV-Cross and the shader
TU build with `-fexceptions`, so a malformed guest shader fails its pipeline instead of aborting.

**State, clear and draw.** Full clear is a load action; partial clear is a scissored fill. Each latch
field hashes at its own width. Attachments are compacted. Cull-both drops rasterisation only for
triangles. A two-sample guest request folds to four host samples, but the guest lattice and
`get_spp()` stay at two.

**Presenter and flip.** The drawable is the client area; one blit scales the RSX image into it. The
recorder signals the drawable on the frame's own submit. The flip calls both `GSRender::flip` and
`rsx::thread::flip`. Vsync off is no sync, adaptive is paced, full is display sync. The draw path
carries the same Y flip as the presenter: viewport height negation, mirrored scissor, inverted
winding.

**Reuse-only clusters.** `fixed-function`, `misc`, `sync-flip`, `nv0039-dma`, `nv2d-surface`,
`nv3089-image` and `nv308a-image` need no new Metal code; the shared CPU handlers do the work.

**Landed batches.** Batch 1: PF1–PF7, PF10, PF11, DR1, DR2, DR7, SF1, SF2, SF6, SF11–SF13, V1, F1.
Batch 2: SY1–SY3, SY5, SY9, S3–S5, S7, L1, TX1, TX2, TX4, TX6, F4, F6, TX9. Batch 3: VX1–VX3,
DR3, DR6, L3, TX3, F13, PF9.

---

## 4. The measurements

These are device facts on the floor device. The probes are not in the tree.

### 4.1 Shader limits

| limit | value |
|---|---|
| MSL buffer index | 0–30 (31 fails) |
| MSL sampler index | 0–15 (16 fails) |
| MSL texture index | 0–127 (128 fails) |
| argument table buffer bind count | accepts 100000 |
| argument-buffer escape hatch | no ceiling found up to 8000 entries |
| MSL language version | does not move the three limits |

A combined image sampler that is only fetched still emits a sampler argument. A bind past the
declared sampler count aborts on the validation layer.

### 4.2 Depth and stencil

| measurement | result |
|---|---|
| depth resolve, producer barrier, 12 configurations | 576/576 |
| depth resolve, consumer barrier | 46/48 |
| depth resolve, no barrier | 4/48 and 3/48 |
| combined depth-and-stencil resolve | 32/32 |
| stencil resolve, all barrier forms | 48/48 |
| named-plane combined-format copy | bit-exact, private and shared |
| un-named combined-format copy | invalid API |
| depth-only view of `Depth32Float_Stencil8` | rejected; only the stencil view casts |
| guest z24s8 | stencil bits 7:0, depth 31:8 |
| guest z16 | depth bits 15:0 |
| read-back encode | bit-exact at every texel |

### 4.3 Shadow compare

Directed functions: 32/72 mismatches. Swapped functions: 0/72. Linear compare gives
1.0/0.75/0.5/0.25/0.0. The native comparison sampler is correct with the directed functions swapped.

### 4.4 Presentation and frame probe

Drawable count is 3. An explicit producer-to-consumer barrier between two encoders changed no pixel
on this device for this encoder structure; this is a measurement, not a licence to drop barriers.
The full 2-D frame probe (upload, alpha blend, depth mirror, RTT resample, read-back, present)
passed every check. It proves encoder shapes, not the RSX translation.

### 4.5 Residency

A resource not in an `MTL::ResidencySet` on the queue is not resident. A compute copy to it writes
nothing, the commit reports success, and the queue wedges with `MTL4CommandQueueTimeout`. Residency
and barriers are separate.

### 4.6 Packed texture formats

The shared uploader keeps a packed GCM format packed on Apple silicon. Metal names a packed format
from the most significant field, so the read permutes it. KK proves the direction
(`kk_format.c:231-234`).

| guest format | native Metal format | read |
|---|---|---|
| `R5G6B5` | `B5G6R5Unorm` | `{One, Blue, Green, Red}` |
| `R5G5B5A1` | `BGR5A1Unorm` | `{Alpha, Blue, Green, Red}` |
| `A1R5G5B5` | `A1BGR5Unorm` | `{Alpha, Blue, Green, Red}` |
| `D1R5G5B5` | `A1BGR5Unorm` | `{One, Blue, Green, Red}` |
| `A4R4G4B4` (stored `R4G4B4A4`) | `ABGR4Unorm` | `{Red, Alpha, Blue, Green}` |
| `R6G5B5` | expansion planned | REVIEW R3 |

The earlier “the probe measured the channel order” claim was wrong: a shader write/read of the same
channel is invariant under any permutation.

### 4.7 Copy rules

A buffer-to-texture copy accepts a source row shorter than the destination row: a 512-byte source
row into a 1024-byte destination row wrote 1020 of 1024 bytes and put two source rows in the first
destination row. `bytesPerRow` is required for a linear texture. A `uchar` view is illegal. A wrong
source pitch fails silently, so the pitch is measured, never assumed.

---

## 5. The backend against Vulkan and OpenGL

### 5.1 Size

| backend | .cpp files | .cpp lines | headers |
|---|---|---|---|
| Metal 4 | 31 | ~11k | ~4.3k |
| Vulkan | 29 | ~18.5k | ~4.8k |
| OpenGL | 18 | ~9.1k | ~3.5k |

Line count is a weak proxy. The feature list is the real comparison.

### 5.2 Capability differences

| capability | Vulkan | OpenGL | Metal | consequence |
|---|---|---|---|---|
| `supports_last_provoking_vertex` | device | true | **false** | flat shading falls back to smooth |
| `supports_hw_conditional_render` | true | not set | **false** | CPU skip; correct but slower |
| `supports_native_logic_op` | true | true | **false** | emulated in the ROP |
| `supports_host_gpu_labels` | setting | true | **false** | guest writes stay CPU-side |
| `supports_extended_depth_range` | device | device | **false** | shared float-depth emulation |
| `supports_passthrough_dma` | device | not set | **false** | shared CPU handlers |
| `supports_multidraw` | true | true | **false** | inert; no reader |
| `supports_asynchronous_compute` | true | not set | true | — |
| `supports_programmable_blending` | true | not set | true | — |

### 5.3 Gaps

| feature | Vulkan | OpenGL | Metal |
|---|---|---|---|
| shader interpreter | yes | yes | yes (`LLD-shader-interpreter.md`; runtime gate pending) |
| on-disk pipeline cache | yes | yes | yes (`"metal"/"v1"`, driver archive) |
| multi-threaded pipeline build | pool | one | preload pool; draw-time build synchronous |
| async scheduler | yes | no | no (deliberate) |
| occlusion query pool | yes | yes | none (ZCULL exists) |
| upscalers (FSR1, RCAS, bilinear, nearest) | yes | yes | **none** |

The pipeline archive (`MTL4PipelineDataSetSerializer`) is the `VkPipelineCache` analogue. The
shader interpreter is implemented (`LLD-shader-interpreter.md` §21); only its runtime gate (an
approved draw run and the standalone MSL probe) is outstanding. A draw the chain cannot express on
the interpreter falls back to a synchronous real build, not a drop.

### 5.4 Settings Metal ignores

| setting | Metal |
|---|---|
| `read_color_buffers`, `write_color_buffers`, `read_depth_buffer`, `write_depth_buffer` | shared half inherited; backend half absent |
| `strict_rendering_mode`, `disable_zcull_queries`, `relaxed_zcull_sync` | inherited |
| `disable_hardware_texel_remapping` | **missing** |
| `disable_vertex_cache` | no-op by construction |
| `force_high_precision_z_buffer` | effectively always on |
| `precise_zpass_count` | shared half inherited; backend half absent |
| `host_label_synchronization` | matches false capability |
| `disable_msl_fast_math` | **missing**; F4 now selects safe math |
| `log_programs`, `debug_output` | debug-only; absent |
| `output_scaling`, `rcas_sharpening_intensity`, `min_scalable_dimension`, `resolution_scale_percent` | **missing** |

### 5.5 Workarounds and debug surface

Class A (vendor driver workarounds) mostly does not apply, except the Apple/Kosmickrisp entries:
KK unrolls loops, so Vulkan emulates primitive restart; Metal must check its own index path. Class B
(shared guest quirks) is inherited. Class C (OpenGL-only hacks) encodes guest behaviour Metal must
reproduce; it lives in the shared vertex prologue. Vulkan's debug callback, RenderDoc and GPU labels
are absent; Xcode GPU capture and `MTL_HUD_ENABLED` are available.

---

## 6. The findings register

`REVIEW-FINDINGS.md` holds 97 findings: **80 fixed, 3 partial, 0 blocked, 1 open, 9 limitations, 2
duplicates, 2 won't fix**. The parity sweep added J1–J10 (VX1–VX3, DR3, DR6, TX3, TX9, PF9, F13,
L3). The open finding is I7, the content of the two present buffers; it needs a reproducer. The
partials are B16, E7 and E8.

The register sweep closed I4 and I6 and added I9 and G3. G1 (flat shading) is a limitation, not an
open defect. G2 (`supports_multidraw`) is inert.

**Current high-severity state.** The descriptor abort is open and reproduced. The black mini-game,
the black background video and the black FMV are open visual defects. SF9/TX12 (depth read-back),
TX11 (depth-as-colour view), TX14 (null texture), SY4 (null resource unbind) and PF8 (drawable
without texture) are deferred with a recorded reason. DR5 (winding) needs an on-device cull A/B.

---

## 7. The pending work

### 7.1 The black video and FMV (current blocker)

The hypothesis chain is closed down to one binary question. Decode and upload are faithful (L88,
L94, L95); the B8 view swizzle matches Vulkan (L96); the draw's state is complete and the probe is
on the video area (L101); the alpha test is compiled out and changes nothing (L110). For draw 12262
in run 2 the target stays at exactly `(0,0,0,0)` while all three planes hold picture.

Two outcomes remain, and colour cannot separate them:

1. **The fragment ran and wrote zero** — the sampler returns zero (view, coordinate transform,
   binding) or the YUV math/sRGB/channel remap collapses the value.
2. **No fragment of that program ran** — raster/clip/attachment path, even though the state fields
   read correctly.

`MTL_PROBE_FORCE_FP=<id>` (§10.7) decides it in one run by writing magenta after every ROP stage.
If magenta appears, Test B is a sample-only output (or `MTL_PROBE_REPLACE_TEXTURES`, which answers
the coarse question but cannot isolate one draw inside a frame, L91).

Do not re-derive the depth witness: the readback returned garbage for both depth formats and was
removed (L112). If a depth witness is ever needed again, it needs a working readback first.

### 7.2 The black Xevious mini-game

The panel is the Xevious window on the startup screen. Input and audio work, so the guest runs. It
may be an RTT sampled by a quad or a presented surface. With L111/L114 as the method, the order is:
does the panel's own draw write (forced output), then is its RTT cleared or overwritten before the
sample, then is the sampled surface invalidated when the guest writes it. The 8×8 DXT45 rectangle
is already excluded as a texture-content problem (L42).

### 7.3 The black car

The car is a correct **silhouette** (L98), so geometry, cull and depth accept it; the colour is lost
in the fragment inputs or their math. The paint texture is one repeated BC1 block (L97), so the
question is whether that colour is what the shader expects, not whether the upload is right.

The car-select probes of run 1 are on background geometry (L105), so no existing probe touches the
car body. L99 must be settled first: the presented surface holds three panels at x = 0/427/853 while
the F12 screenshot holds one, so the probe coordinates depend on which layout the screen shows.

### 7.4 The open TODO rows

Order: SF8, SF9, TX5, TX7, TX10, TX11, TX12, TX14, DR9, DR10, PF8, PF12, S5, S6, F2, F3, F5, F7–F12,
F14, SY4, SY6–SY8, SY10, SY11, Q7, VX4, SF14, TX20. The live list and severity are in
`METAL4-TODO.md`; this section does not duplicate them.

### 7.5 Coverage and dials

203 live dials, 51 wired. The four latch clusters are depth-rop, raster, blend and draw; the other
ten write straight into a resource and need a device observation. 65 live dials have no decoder;
`vertex` (4 of 25) and `texture` (3 of 13) are the sharpest. 71 dials are difficulty 3 or 4 and need
a design decision.

### 7.6 Code that has never executed

The samplerless declarations (A9), the C1 logic-op shader, the shadow-compare fallback, the
cyclic-reference texture barrier (I4), the adaptive-vsync approximation, the stereo second eye, the
CPU read-back wait and the Metal 3 blit encoder inside a Metal 4 command buffer. Each needs a title
or a probe; none is a confirmed defect today.

### 7.7 Gap list

1. Flat shading: limitation (G1). No native provoking vertex; index rotation or attribute replicate
   are the costed expressions. No title on this install uses it.
2. No shader interpreter (Gap 2).
3. No occlusion query pool (Gap 3).
4. No upscalers (Gap 4).
5. `disable_hardware_texel_remapping` ignored (Gap 5).
6. Backend half of surface read/write flags absent (Gap 6).
7. `precise_zpass_count` backend half absent (Gap 7).
8. `disable_vertex_cache` and `force_high_precision_z_buffer` are silent no-ops (Gap 8).
9. Primitive restart against the Apple driver (Gap 9) — DR3 widens 16-bit strips.
10. `disable_msl_fast_math` (Gap 10) — F4 selects safe math.
11. On-disk shader cache landed (Gap 11).
12. Pipeline build is asynchronous at draw time (Gap 12 closed); the warm preload submits and drains one bounded queue (F6).

### 7.8 Guest quirks

Pitch conversion is unimplemented (`surface_store.h`), so overlapping surfaces of differing pitch
are truncated. The depth-to-colour `force_convert` bitcast needs the resample path. ZCULL titles that
never drain zpass and the `fdiv` subpixel bias are shared behaviour. The three Class C OpenGL hacks
encode guest behaviour; reproduce the behaviour, not the hack.

### 7.9 Performance

The run-4 steady window (300 frames, `MTL_PROBE=1`, F21/F22 in): 60 fps, `frame/gpu` 2,016 µs with
840 µs GPU wait, `present/flip` 413 µs, `present/acquire` 231 µs, `present/display` 59 µs,
`frame/end` 58 µs, `texture/upload` 39 µs, display gap 16.89 ms, latency 15.75 ms, 1.38 drawables in
flight. The boot window is the one to watch: `present/flip` 4,876 µs average with 879 ms maximum,
`present/acquire` 2,021 µs average with 361.8 ms maximum, `program/pipeline` 5,108 µs over four
builds. The 3-D title has no level-1 window yet. Owed: encoder boundaries per frame, pipeline cache
hit rate, per-draw CPU time, hazards per frame and a Vulkan differential.

### 7.10 Hygiene and the commit

**Uncommitted today** (all in the working tree): the five correctness fixes of
`INVESTIGATION-2026-10-22.md` §10.2 (framebuffer prep in `begin()`, framebuffer fast path, binder
zero write, FIFO retry rewind, FIFO cache store), the probe families below, and the run-2
diagnostics (`MTL_PROBE_NO_ALPHA_TEST`, `MTL_PROBE_FORCE_FP`, their archive suffixes). Nothing is
committed until a fix is confirmed.

1. Excise every `TODO(excise)` site, `MTLInstrumentation.h`, the acquire-stall probe and the
   main-thread heartbeat. The markers span `MTLCommandRecorder.cpp`, `MTLDevice.cpp`,
   `MTLFragmentProgram.cpp`, `MTLProgramPipeline.cpp`, `MTLGSRender.cpp`,
   `MTLInstrumentation.h`, `MTLMetalLayer.h`, `MTLMetalLayer.mm` and `MTLPresenter.cpp`.
2. Run the tests and the linter after excision.
3. Commit as `hsaka -1.0 <340957754+hsakaminusone@users.noreply.github.com>`, author and committer.
4. Push `origin/metal4`. Do not touch Vulkan or OpenGL.
5. Split the commits where a test exists: present/flip, F24, barrier/copy ordering, frontend exit,
   shader preload, batch 1–3, excision.

### 7.11 The rear window and menu text (run U)

The user reports both defects unchanged after the FIFO fix, so they are not FIFO symptoms. L146
(the rear window) has no capture yet; the first candidate is a content-transfer hazard on the
resolve or copy that produces the window (the unconfirmed `MTLSurface.cpp` barrier fix). L147
(the menu text) has two text paths: the title-screen line is correct and the menu line drops or
substitutes glyphs. A stale atlas would still show glyphs; a null or invalid bind shows black, so
the first measurement is the atlas bind at the failing menu draw. Both need a level-5 capture at
the car/menu/rear-window scene.

---

## 8. Accepted limitations

1. Point polygon mode collapses to fill.
2. Per-primitive polygon-offset enables collapse to the fill enable.
3. Guest dither is dropped.
4. Hardware conditional render is not implemented; CPU skip is used.
5. Level-of-detail bias needs a fragment injection.
6. Draw-state latch is a decode proof, not hot-path state.
7. Pipeline key carries inert fields.
8. Border colour is one of four fixed colours.
9. The drawable stretches a source whose aspect ratio differs from the window.
10. Stereo second eye is a deviation.
11. The stencil plane of a combined image cannot be uploaded.
12. Pitch conversion is unimplemented.
13. Flat shading falls back to smooth (G1).
14. Typeless/scaled copies are refused rather than emulated (I5); TX9 now stages the equal-texel
    typeless case.
15. `supports_multidraw` is inert (G2).
16. `R6G5B5` needs a format expansion (TX20).
17. A 16-bit surface uses an identity layout; raw write-back swaps R/B. An output permutation or a
    32-bit staging path is planned (R2).

---

## 9. Roadblocks

**Blocking.** The descriptor abort. The black mini-game and black video are visual blockers for
RR7; they do not stop the process.

**Environment.** No new dependencies; 8 GB memory; four-job builds; one emulator instance; about
39 GB free disk.

**Tooling.** `ripgrep` and `timeout` are absent. zsh; BSD `sed`. `MoltenVKShaderConverter` takes
SPIR-V only. `glslangValidator`/`glslc` are absent; SPIRV-Tools is present. The unit suite stays
GPU-free because a `verify()` dialog can kill it.

**Platform.** Vendored Metal headers carry no availability annotations; the runtime gate is the only
guard. A depth-only view of `Depth32Float_Stencil8` is rejected. `[[position]]` cannot be both a
stage-in and a fragment parameter. A bind past the declared sampler count aborts on validation.

**Measurement.** A probe that ignores residency reports a false defect. A result that changes with
validation on/off is a tool artefact. Only an on-screen present measures.

---

## 10. The verification ladder

1. GPU-free unit tests. These run anywhere.
2. Shader-translate check. Compiles the emitted shader without a GPU.
3. Guest-observable read-back. The only objective oracle.
4. Differential comparison against the shipping backend.
5. Golden images.

RPCS3 has no golden-image suite and the hosted runners have no dependable GPU, so the GPU half is
manual. A differential run compares a tile-based deferred renderer with a non-tile-based one; use a
classified tolerance model, not an exact match. Rung 2 is the cheapest unused rung: decompile a
logic-op and a redirected-depth program and compile the result.

---

## 11. How to continue

1. Read `NEXT-AGENT-PROMPT.md` (the full continuation brief), then `METAL4-TODO.md` and this
   file; the datapoints are in `INVESTIGATION-2026-10-22.md` §10.6–§10.7 and the findings in
   `LOG-FINDINGS.md` §1.1.
2. Build the loop: `ninja rpcs3_emu` for the Metal work, `ninja rpcs3` only before a launch.
3. One approved run with `MTL_PROBE_FORCE_FP=100 MTL_PROBE=5` and the run-2 capture env. Copy
   `/tmp/mtl4-forcefp*.log` and `/tmp/mtl4-dump/` before the next run.
4. Read the video patch: magenta ⇒ sampling/view (Test B); clear ⇒ raster/pass (L114, §10.7).
5. Keep the probe rule (bounded windows, ≤64 draws per window), the archive-suffix rule for any
   switch that changes a shader, and the one-instance rule.
6. Leave the FIFO desync parked until the black picture is closed.
7. Excise, test, commit as `hsaka -1.0`, push `origin/metal4`.

---

## 12. Datapoint index

| datapoint | value |
|---|---|
| branch | `metal4` |
| commits ahead of `origin/metal4` | 55 |
| Metal subsystem files | 67 |
| Metal subsystem lines | ~16k |
| Metal test cases | 150 |
| full test cases | 498 |
| Metal test suites | 38 |
| TODO rows | 60 fixed, 40 pending |
| review findings | 97 (80 fixed, 3 partial, 1 open, 9 limitations, 2 duplicates, 2 won't fix) |
| inventory rows | 282 |
| live rows | 203 |
| work dials | 178 (133 reshape, 45 emulate) |
| live dials without a decoder | 65 |
| dials at difficulty 3 or 4 | 71 |
| design documents | 26 |
| commits | uncommitted since `17c3c332e` |
| last build | green, with the asynchronous shader path |
| last RR7 run | 2026-10-21 00:01–00:06, probe 2; label race at race start |
| crash thread | `rsx::thread` (probe-2 fatal) |
| crash site | `RSXThread::recover_fifo` after dead FIFO |
| MSL buffer cap | 30 |
| MSL sampler cap | 15 |
| MSL texture cap | 127 |
| argument-buffer highest test | 8000 entries |
| depth resolve, producer barrier | 576/576 |
| depth resolve, consumer barrier | 46/48 |
| combined depth/stencil resolve | 32/32 |
| stencil resolve, all forms | 48/48 |
| shadow compare, swapped | 0/72 |
| drawable count | 3 |
| Metal 4 API floor | macOS 26 |
| runtime floor | macOS 27 |
| deployment target | 15.0 |
| code written (estimate) | ~75% |
| code verified (estimate) | ~70% |

<!-- doc-end -->
