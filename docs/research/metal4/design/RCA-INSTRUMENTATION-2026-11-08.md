# RCA instrumentation audit — 2026-11-08

Scope: every logging, probing, ring and dump surface the Metal 4 backend carries as of
commit 4862a1700 plus the RCA pack added in this pass.  Goal: a capture that can take a
lauded-but-unidentified defect class — the Ridge Racer 7 (BLUS30001) livery/plate family —
from symptom to a chain of attributions whose weakest link is a file on disk.

Every diagnostic below is temporary (`TODO(excise)`), level-gated (`MTL_PROBE`), bounded,
and kept out of the shipping archive.  The knob surface is **7 vars**
(`MTL_PROBE`/`_LOG`/`_DUMP_DIR`/`_FRAME_COUNT`/`_ONLY_DRAW`/`_PIXELS`/`_VARIANT`); every
experiment arm rides `MTL_PROBE_VARIANT=<arm>[=<arg>]`.

--------------------------------------------------------------------------------
Part 1 — what each level sees (invocable by MTL_PROBE=n)
--------------------------------------------------------------------------------

Level 1 — the counters: block timers (`MTL_PERF_SCOPE`), totals and gauges
  (`MTL_PERF_COUNT/GAUGE`), frames and present cadence.  Aggregate only; safe to run
  in front of any title, prints at teardown (`MTL_PERF_REPORT(1)`).

Level 2 — coverage: latches (`MTL_PROBE_LATCH`), histograms (`MTL_PROBE_HISTOGRAM`),
  the periodic + final coverage table (`MTL_PROBE_DUMP(2)`).  Answers "what does this
  title ask the backend for at all".

Level 3 — the per-event verbose stream:
  · per-draw state block (draw, command, prim, targets, ROP u32, blend factors, cull,
    depth, scissor, layout, pass, clear) — every draw of the first 24; in a capture
    window also for all its draws.
  · the sampler unit audit (format, swizzle, dims, mips, remap, control, wrap, filter,
    LOD, bias, aniso, border, compare) per draw.
  · census of distinct draw signatures (target, program, units/target@addr per line).
  · the draw-state interpreter/compiler decision lines.
  Diagnostic volume: bounded by file-sink + site caps (4096/site, 40000/interval,
  200000/run shared budget).

Level 4 — the machine: layer walk, stall probe (`MTL_PROBE_STALL_MS`), heartbeat and
  residency churn.  Not needed for the plate defect; listed for completeness.

Level 5 — every dump:
  · capture windows: armed by F12 (`MTL_PROBE_CAPTURE`/screenshot key) — never by
    itself; `MTL_PROBE_FRAME_COUNT` frames (default 1).  At most 5 windows per run,
    1200-frame gap between automatic windows; manual presses bypass the gap.
  · per-window image dumps: ≤4 frames dump patterned images, ≤384 image files per run,
    ≤24 present blobs per run; every longer stream is counted not written.
  · per-capture-frame ring dumps (`frame-N-m1..m4/p1..p4-*`): draw records, geometry,
    bind state, vp/fp constants, staging buffers — `Frame_buffer_dump` helpers,
    `dump_frame_consts` for fp/vp constants executed per capture frame.
  · `target` readback after the window's draws + `resolve` readback (deferred op dump).
  · sampled-image dumps for units drawn with the window (`sample-*.bin`, 4-frame / per-run
    bounded), host image readback with FNV hash + nonzero/distinct counts (`host-*`),
    present flips (`host-present-*`, ≤24/run), F12 screenshots (`host-shot-*`).
  · sampled byte scan: every sampled unit's guest region, first 1024 words nonzero
    counted; full region byte dump (≤16 MB) for BC/DXT formats in the window.
  · upload path dumps (`guest-*`raw, `conv-*` converted): dedupe keys, 512 uploads,
    256 MB/run, level 0 only.
  · shader source dumps (`-id.glsl`/`.metal`), id < 512; program archive suffix arms.
  · target patch readback (`MTL_PROBE_PIXELS`): fixed screen patches after every draw,
    the draw-level discriminating instrument (cleared-magenta/alpha, written-black).
  · `poison` arm: fresh allocations carry a self-describing word (tex = f(1, address),
    buffer = allocation index) — an unwritten resource samples as its own word.

--------------------------------------------------------------------------------
Part 2 — the RCA pack added 2026-11-08 (closes the gaps found in this audit)
--------------------------------------------------------------------------------

P1. **Per-address upload/sample census** (`census_*` in MTLInstrumentation.h; noted from
    the upload path in MTLTextureCache.cpp and from every per-draw fragment-unit loop
    in MTLGSRender.cpp).  Per guest address: first/last frame, uploads + bytes, samples,
    BC all-zero blocks (8/16-byte, DXT only), last format, unit-as-target hazard flags.
    Report prints: (a) when a capture window disarms, (b) at renderer teardown, as the
    `Metal 4: census …` lines plus the `census never-uploaded` list (sampled this run,
    zero uploads — the flat-black plate smoking gun).  Bounded at 384 addresses,
    fixed line order, sink() anti-flood safe (report path, not MTL_NOTE).

P2. **BC flat-zero block accounting per upload** — for every DXT1/23/45 subresource,
    the converted staging is scanned once and the zero-block count is recorded with the
    census entry and carries into the report.  Cheap (one pass over ≤16 MB), catches
    "uploaded, content all zero" even when no dump is written.

P3. **`MTL_PROBE_VARIANT=dump-tex=<hex>`** — one-shot bypass arm.  Every upload of that
    address (raw + converted bytes, level 0) **and** every per-draw sample of it writes
    the full region, ignoring dedupe keys, the per-window frame budgets and the run
    image budgets.  16 files and 64 MB per run, clamped by the arm's own budget.

P4. **`MTL_PROBE_VARIANT=dump-draw=<id>`** — the isolated-draw input capture: the
    named draw's raw persistent and raw volatile vertex streams dump (bypassing the
    capture-window gate), 16 MB per run.  The counterpart to the existing byte-level
    `MTL_PROBE_ONLY_DRAW` mute test (mute proves attribution; dump proves inputs).

P5. **Unit-as-target hazard check every draw** — a fragment unit sampling an address
    this draw is also *bound to write* (color/zeta) is counted per address into the
    census as `hazards`.  The paint/reflection `0x50110000` identity-split class is now
    visible as counts at the window close and teardown lines, not only as frame
    coincidence ranges.

P6. **`dump_frame_opened` census frame keying** — the census knows which frame each
    upload/sample lands on, so a hazard or zero-BC upload can be pinned to a frame id.

--------------------------------------------------------------------------------
Part 3 — audit findings (gaps found, action)
--------------------------------------------------------------------------------

A1. **"Sampled but never uploaded this run" was unobservable.**  The upload dumps
    (512/run, dedupe) and the seam lift between guest memory and the GPU meant the
    last hop had no key.  Closed by P1: every sample notes the address; the report
    prints `samples>0, uploads==0` straight out.

A2. **DXT flat-zero content was only catchable by reading dumped blobs.**  Nothing
    summarized at upload time.  Closed by P2 (scan at the upload edge); complementary to
    the sampled nonzero-word scan already at MTLGSRender.cpp:3190 (which reads the guest
    side, not the converted staging).

A3. **The one unidentified texture could always lose the budget race.**  In 17:21 run,
    livery target/frame counters showed `0x200fxxxx`-class addresses never dumped in
    either window — the budget decides by arrival order, and the defect's texture may
    never arrive.  Closed by P3 (`dump-tex`).

A4. **The isolated draw's inputs were text-logged but never byte-captured.**  For a
    shading/texture-id question the fp constants, strides and stream-qualifying are
    enough, but the raw input bytes were not.  Closed by P4 (`dump-draw`).

A5. **Hazard (sample ∩ bound target) had no tally.**  The 123 `0x50110000` hits in the
    last run had to be correlated by hand.  Closed by P5 (per-draw hazard flag +
    census tally, first/last frame per address).

A6. **Upload census frame attribution was missing.**  uploads noted without frames
    could not be tied to a scene phase.  Closed by P6.

A7. **Every writer into the dump dir was already bounded before this pass**
    (4862a1700): per-window image ≤4 frames, run images ≤384, presents ≤24, shader
    source ids <512, upload dumps 512/run by dedupe + 256 MB, vertex-ring ≤16/window,
    index-ring ≤16/window, screenshot F12 ≤ budget, log-writer ENOSPC degrade.  No
    writer survived the audit unbounded.

A8. **Known limits, documented not closed (deliberate):**
    · guest FIFO mmap fault tracing (which mapped range paid which write) — far too hot
      for an always-on probe; the `poison` arm covers the "was it written at all" half.
    · 100% image dumps of a window — doubles today's 512-image flood again; the
      census + dump-tex cover the named-address case instead.
    · RSX-thread micro-interrupts — out of scope; this defect class is texture-content,
      not timing.
    · display-side re-acquisition cost — level 4, orthogonal.

--------------------------------------------------------------------------------
Part 4 — what a failing-frame RCA now needs, end to end
--------------------------------------------------------------------------------

1. Launch, MTL_PROBE=5 MTL_PROBE_FRAME_COUNT=1 MTL_PROBE_VARIANT unset or poison,
   MTL_PROBE_DUMP_DIR + MTL_PROBE_LOG named for the session; get to the failing frame.
2. F12 over the failing frame.  Window captures rings, sampled images, target/resolve
   readback, present flip, F12 shot.
3. Read the teardown `Metal 4: census` lines: the never-uploaded list, zero-BC upload
   table, hazard tallies.  Any address with samples>0 and uploads==0 is the defect
   candidate; its unit/binds are already in the per-draw rows (`draw N`, `unit N, …`).
4. If the candidate is a name, delete the guess: `MTL_PROBE_VARIANT=dump-tex=<hex>` the
   single failing address, re-run, byte-compare `dump-tex-*-conv.bin` against the guest
   upload bytes — a flat-zero DXT payload on one side and lossless on the other proves
   the decode/upload path owns the defect; empty on both sides proves the guest never
   wrote it (game data, not backend).
5. Attribution to a draw: `MTL_PROBE_ONLY_DRAW=<lo>-<hi>` counts-drops the rest; the
   plate disappears or stays by draw site.  `dump-draw=<id>` then captures that draw's
   raw input streams.  `replace-textures=checker` (or `uv`, or `replace-units=`) closes
   slot-vs-image on the same frame.
6. Verdict classes: (i) never-uploaded ⇒ texture-cache promote/miss binding bug;
   (ii) converted-staging ≠ raw ⇒ uploader/decode defect (find the fmt path);
   (iii) raw flat-zero ⇒ the guest's own data (out of backend scope, document and stop);
   (iv) draws bind it fine but shade black ⇒ vp/fp compile defect (dump-draw + the
   shader arms decide).

--------------------------------------------------------------------------------
Part 5 — test status
--------------------------------------------------------------------------------

GPU-free suite: 560/560 green after this pass (commit 4862a1700 → HEAD).
Device confirmation of the RCA pack itself is pending the next launch; nothing here
launches on its own.
