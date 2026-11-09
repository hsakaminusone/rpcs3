# RCA status — frame 4572 glitch (black plate) — 2026-11-08 late (offline census)

Written for the user. It reflects everything verified this session against the
raw dumps in `build/probe/dumps-rca-20261108-194019/` plus the frame-1789
witness, **plus a fresh offline byte-level census done tonight** (exact-black
connected components vs a byte-identical clean reference, full-record multiset
comparison, census/hazard tables). It supersedes the "steady baseline ~250–270
pixels, growing at 4557→4572" reading in the earlier version of this file:
that measurement mixed the animated scene with the defect. It also records the
true state of the discriminator plan. This is deliberately explicit about what
is proven and what is not.

## What the defect is (measured tonight)

- The defect is **pure `#000000` pixels on the car's left taillight cluster**,
  and it **flickers per frame**. Connected components of newly-black pixels
  in `host-shot-4572` vs the clean reference `host-shot-4495` (`host-shot-4462`
  is byte-identical to `host-shot-4495` inside the defect window
  x[480..700] y[430..610]; whole frames differ by one byte in the sky —
  animation residue; `host-present-4463` vs `4496` likewise by one byte):
  - comp 876 px, bbox x[538..580] y[514..540] — the block on the lenses;
  - comp 232 px, bbox x[524..535] y[499..535] — the vertical bar to its left;
  - comp 18 px, x[523..530] y[536..539];
  - ≤8 px specks on the DEROTA band (x[587..656], y[500..509]).
- Per-frame flicker across shot/present pairs one frame apart (same stationary
  start-line camera, frames 4204–4573):
  - 4200 present 201 → 4204/4205 shot+present 0/0 → 4234 shot 6 → 4235 present
    689 → 4412/4413 72/70 → 4462/4463 0/0 → 4495/4496 0/0 → 4557 shot 148 →
    4558 present 0 → 4572 shot 1193 → 4573 present 70.
  - Not parity-locked, not monotone, not tied to the countdown ramp. An
    ordering/visibility race signature, not a static-content signature.
- It is the same pure-black signature family as the trunk-livery block
  (L193/L195) and the user's 19:42:26–34 series (73→116→773→886 px growth
  across those four shots — the packed frames 4557/4572 show the same
  "present can be clean while the shot 200 ms later is not" behaviour).

## What is eliminated (verified offline, this session)

- **Draw records (multiset level): identical between failing 4572 and clean
  4495.** The full 1005-record multiset of (vp, fp) matches 1:1. All 32 car-paint
  `fp 105` draws are byte-identical on every field including unit bindings.
  The only record-level differences are ~45 animated full-screen sky/particle
  draws (`vp4/fp129`, `vp27/fp34`, `vp5/fp10`, `vp2/fp17`, target `0xc0000000`)
  whose geometry varies per frame, plus fp129 ~1e-7 time-ramp constants.
  `fragment-context` identical for all 1005 draws.
- **Textures:** no uploads after frame ~3986 (window census stable at 1800
  uploads; static start-line scene). Never-uploaded-but-sampled entries are
  all RTTs: `0x50110000` (scene pyramid), `0xc0000000` / `0xc0880000` (flip
  buffers), `0xc1a00000` (512x128 mask, **uniform `00000000` in both clean
  and failing windows**, ~212k samples — not causal).
- **BC-zero blocks:** exactly two addresses, constant counts across all
  windows (`0xc440b080` 36/512; `0xc6881800` 128/32768) — not causal.
- **Census 4-address picture:** `0xc0000000` hazards == samples exactly
  (15.79M at fr. 4573; every sample is a unit-as-target); `0x50110000` hazards
  ≈ 64% of samples; `0xc0880000` ≈ 33%. Census `suppressed` (address beyond the
  384-entry cap — i.e. invisible to this instrument, not "hazard suppressed")
  grew 0 → 286k (fr. 3986) → 875k (fr. 4236) → 1.97M (fr. 4574).
- **PB double-blend fix is in this build; defect persists.** Async-link clean;
  screenshot/present path ordered; flip-blit input identity fine; the two
  binding identities of the canvas (tid 0x31d vs 0x3b) not the bug.
- **The flip-blit / pyramid chain** (present hand-off, scene RT → flip
  buffer) remains the suspected boundary class: unit-as-target cyclic pattern
  on the display RT is the one channel that differs from every other target
  in the census.

## What is NOT yet explained (honest)

- **Which pass pair races.** No offline evidence names the exact missing edge.
  Everything failing-frame-side is deterministic in the dump; only execution
  timing differs. The defect needs a stage-mask/visibility verdict from the
  barrier layer, or the `heaps` discriminator, not more byte-diffing.
- **Why the user's 19:42:26→34 series looked like a ramp** (73→116→773). The
  packed presents oscillate; the screenshot series growth is real for those
  four frames — treat "ramp" as unproven.
- **The menu-item flicker** (user-visible today in normal builds; not the
  splash/FMV bug — the intro FMV replays fine in normal builds, the 22:05–22:07
  screenshots are the ~5 fps diagnostic run stuck in the splash loop, which is
  not representative). It has never been captured in a playable build; it needs
  a light run to reach the menu and an F12 on the flicker frame.
- **The heaps discriminator result.** Not run. See below.

## Discriminator plan (unchanged, blocked on your go)

The heavy diagnostic config (`heaps,poison,sentinel,serialise`,
`MTL_PROBE=5`, pid 29328) is dead — it never reached the menu/race at ~5 fps,
and its screenshots were splash frames. It cannot answer the plate question.

Light relaunch (playable, normal speed), to be played at the grid with F12 at
the countdown (00:26–00:29 was the failing window):

```
MTL_PROBE_VARIANT=heaps ./build/bin/rpcs3.app/Contents/MacOS/rpcs3  (BLUS30001)
```

`heaps` alone flips every heap to `HazardTrackingModeTracked`. If the defect
disappears → missing edge / heap-untracked ordering (case 1 or 2). If it
persists → the bug is not heap ordering; continue at the pass-scheduler level.
Optional: `heaps,poison` to also stamp unwritten resources; `heaps,dump-tex=0x50110000`
to capture the failing flip input.

## Verification verdict (2026-11-08 late, L202) — the mechanism narrowed

The scheduler-verification agent is done: 114 new tests, 674/674 green
(`rpcs3/tests/test_rsx_metal4_scheduler.cpp` +1 CMakeLists line, committed
nowhere). Its verdict on the hazard-ordering core is **incomplete**, and it
refutes the prologue hypothesis *as written*: every deferred/upload path ends
in a scheduler note, a producer barrier, or an explicit queue barrier — the
mechanism fires **one stage over** instead:

- **WAW is entirely unordered** — two clauses writing one attachment are never
  ordered by any barrier (the encoders may overlap on untracked heaps).
- **A materialised image sampled by both fragment and vertex stages gets its
  edge starved for the vertex consumer** — the fragment read consumes the one
  write record; the vertex read finds zero stages left and produces nothing
  (`MTLSchedulerHypothesis.SharedDeferredUploadOrdersFragmentButStarvesVertex`).
- **Same-pass TBDR feedback pairs are never ordered by this encoder**, and
  `split_requested()` has no consumer in the backend.
- **Colour and depth attachment writes are noted `StageFragment`, never
  `StageTile`** — while the attachment store is a tile-stage op
  (`StoreActionStore`). The resolve target alone is noted `StageTile` (:3048).
  Whether a `StageFragment` queue barrier encompasses the tile flush is the
  CPU-unverifiable question the agent flagged as (ii-8) — and it is exactly
  the cyclic-RT class this defect lives on (scene RT written as attachment,
  sampled later by shader work).
- `flush(nullptr)` destroys pending edges (one fragile call site,
  MTLGSRender.cpp:2106; not a live defect today).
- Comment/code disagreements documented; the null-flush destruction and the
  "pass boundary orders everything" comment are wrong in the tree.

**Directly testable against this defect class:** (iii-a) does a
`StageFragment` queue barrier order the tile flush of a `StoreActionStore`
attachment — sample an attachment in clause N+1 in a patched
`StageFragment|StageTile` experiment build vs the current build, and vs
`serialise`; (iii-d) the cyclic-RT request barrier on/off against `heaps`;
(iii-b) torn-half attachment readback between back-to-back write clauses.
No backend/scheduler behaviour has been modified; the one-line candidate
patch (attachment + depth notes carry `StageTile`, matching the colour
resolve-target note) is documented, not shipped — it is a behaviour change
and needs your explicit go before it lands in any build.

**Consequence for the discriminator:** the `heaps` run is still the cheapest
falsifier, but the engine evidence now says "attachment note mask vs tile
flush" is the prime suspect. A single experiment build could settle both.

**Stage-1 code landed (L203, 2026-11-08 late):** attachment/depth notes carry
`StageFragment|StageTile`, depth resolve target noted (StageTile), stencil
view noted, scheduler redesigned (OR producer masks, per-candidate edges,
per-pass satisfied scratch), null-flush carries instead of destroys; the
114-case suite re-pinned; **674/674 GPU-free green**. WAW measure-gated.
Device validation (E1 start-line F12 run) pending user go; L204 will carry
the verdict.

## Committed artifacts

- LOG-FINDINGS.md: **L201** — the offline census; **L202** — the scheduler
  verification verdict. The device discriminator result will land as L203.
- Evidence docs cite `evidence-20261108-sig/` and this pack.
- `build/probe/dumps-rca-20261108-194019/` and `dumps-serialise-20261108-220404/`
  (3 GB, decoded, no glitched frame) were deleted on 2026-11-09 to free
  space for the multi-press run; their findings are the log rows above.
  `rpcs3.pid` is stale (dead run).
- The 22:05–22:07 menu screenshots are in
  `~/Library/Application Support/rpcs3/screenshots/BLUS30001/` (splash loop,
  not the menu); 19:42:26–34 are the four countdown shots (defect witness).
- Scheduler-verification subagent (100+ permutation tests on `MTLPassScheduler`):
  done 2026-11-08 late — 114 new tests, 674/674 green; verdict **incomplete**;
  see L202 and the "Verification verdict" section above.

## 2026-11-09 — the feedback-loop copy landed in code; the pre-trigger frame is now taped

**Not a verdict — a new hypothesis with a falsifier, and an instrument that captures the frame the
user actually points at.** Nothing here claims the defect is fixed.

### Why this is the prime suspect (L204, device verdict pending)

- GL legalises a bound render target sampled by the same draw with `glTextureBarrier()`
  (`9cb58a47c`, PR 2918). Vulkan legalises it with `VK_EXT_attachment_feedback_loop_layout`
  (`71e35c8b4`, `102d30db2`; PRs 12526 / 12716; per-surface state machine `110c20d25` / PR 13136).
  Metal has no such primitive and no tile-in-tile or framebuffer-fetch story for the store-then-sample
  sequence — and the shared fast path bound the live attachment as a shader input anyway whenever
  `force_strict_fbo_sampling()` was false (strict rendering off, AA off: the default).
- `requires_feedback_loop_copy` is now a per-backend trait (`MTL = true`; GL/VK `false`, byte-identical
  pass). When the sampled image is a currently-bound ROP target,
  `process_framebuffer_resource_fast` takes the pre-pass copy branch it already had
  (`memory_barrier(transfer_read)` + `create_copy`) instead of the live attachment;
  `test_if_descriptor_expired` honours it too. The copy descriptor does not claim
  `is_cyclic_reference`, so `insert_texture_barrier` is correctly skipped on that path.
  `TextureUtils.h` grew `is_feedback_loop_read` (the witness's structural signal).
- The cyclic-RT class is exactly the class the census flags: `0xc0000000` hazards == its samples
  exactly (15.8M). This fix removes a guaranteed-undefined combination from Metal. Whether it
  removes *the* defect is unknown until the device run.

### What the next F12 run measures (L205)

The captured window never contains the frame the user photographed — F12 arms the window on the
*next* frame and the user's reaction lands later than that. The 2026-11-08 dumps confirm it: 3889,
3901 and 3902 are clean; `host-present-3600` holds a 416×416 black window. Two level-5 probes now
exist for that run, both default-off and `MTL_PROBE_TAPE`/`MTL_PROBE_PIXELS`:

1. a rolling **pixel tape** (`MTL_PROBE_TAPE=16`): every flip copies the present source GPU-side into a
   persistent shared-memory ring (no readback, no serialization), and each draw copies up to eight
   feedback-sample images the same way. A slot is a byte-for-byte BGRA8/RGBA8, sample-count-1 image up
   to 2048², so the full 1280×720 post-chain input is now inside the cap; a slot grows by replacing
   its buffer and deferring the old one, never under a queued copy, and the tape's own allocations are
   capped at 448 MB. F12/automation freezes the ring and the next present reads it back once and
   writes present slots first, then samples, against a 512 MB run file budget.
2. the flush is itself the **verdict reading**, not only a file write: every taped sample gets a
   pre-draw black/dark tally and `path copy|live`; every consecutive pair of taped presents gets
   `newly-black`/`newly-dark` counts, a bounding box and the busiest 16×16 cell (a 128×128-cell map, so
   the whole frame is covered). A flickering menu glyph or blotch is named by these lines without any
   configured rectangle.
3. a per-draw **feedback witness** (`Metal 4: feedback witness ...`, `path live|copy`) and a per-frame
   summary (`Metal 4: frame witness ...`): live-vs-copy counts of bound-target samples plus the
   running black tally of the `MTL_PROBE_PIXELS` probes. The witness has its own 400000-line budget
   (`MTL_NOTE_WITNESS`) instead of the 4096-line per-site cap, which used to silence it in about two
   minutes — before the race. A fixed run must show `copy` for the post chain (live 0); a nonzero
   `live` count names the unit the copy did not reach.

`MTL_PROBE_PIXELS` widened to eight patches and now batches all patches of one draw into a single
encoder and a single `flush_for_readback`, so a three-patch window does not pay three queue waits per
draw. `build/probe/run-rca-20261109.sh` passes three rectangles — `500,480,120,90` (presented-space
blotch), `480,500,420,120` (render-target band and taillights) and `0,0,256,128` (the post-chain
scratch, whose x is past the old patch width) — keeps the 4 s / 120-frame forward window, and stays
`MTL_PROBE=5` with `VARIANT`/`ONLY_DRAW` unset. The intended run is one F12 in the menu (flickering
text) and two or three in the race; each press re-arms and records its own 4 s window. Disk is bounded
per run: 1 GiB of image dumps plus 512 MB of tape, with present dumps every eighth captured frame
(budget 64) and a spent budget still leaving its counts in the log. The decoded 2026-11-08
`dumps-rca-*` and `dumps-serialise-*` directories (3 GB, no glitched frame) were deleted. GPU-free
tests 674/674, GL/VK byte-identical on the fast path.

### What is NOT established

- Whether the defect disappears on this build (device run pending; requires the user to launch with F12
  on a blotch).
- Whether it is the *same* bug as the flickering blocks and the menu glyph loss, or merely one
  undefined thing among several. The witness and the tape are the instruments that separate those.
- The L203 side (WAW ordering; the Tile-vs-Fragment hazard question) remains unverified by device, as
  does the attachment `StageFragment|StageTile` fix.

## 2026-11-09 late — the device verdict landed: the copy holds, the glitch persists

Written for the user, honestly. Two runs happened:

1. The F12-armed probe run `probe-rca-20261109-212151.log` (21:21:51 launch, pid 22654, game played to ~21:26; race at <1 fps). `path live` is **0** for the whole log; every frame is `feedback live 0 copy 1`. The serialised frame (frame 2835) contains exactly one true feedback loop: record 1032009, a 256×128 self-feedback at `0xc0000000`, covered by the copy. GPU-free tests 674/674, binary rebuilt before the run.
2. The user then launched the app directly and played; the in-game glitches are still present. The diagnostic build was too slow (<1 fps) to see the race blotches, and the direct run left no log file (rpcs3 logs to stdout only).

**What this settles**

- The feedback-loop copy (L204) is correct and necessary; it is **not** the RCA for the reported symptoms. The menu flicker resumed seconds after the F12 press.
- The menu-glyph loss is reproduced in the probe run: `BLUS30001_screenshot_2026_11_09_21_22_51.png` shows the description line as "variety of race events at the home of motor sports, F…" with the leading "A" missing — the same per-glyph, moving-between-frames loss as L147/L155/L175.
- The tape present timeline at <1 fps cannot separate the animated background from the glitch; the race blotches were not captured at playable speed.

**Probe gaps in that run (L207)**

- The 1 GiB image byte budget was exhausted at 75.3 s; all `black-target`/`black-sample` attribution images after that were counted but never written — the 99 `black patch draw` events after 75 s have log numbers only.
- Per-draw `pixel probe draw` lines are capped to frames 556–657; `capture frame` lines only for frames 556–657; the census 384-address cap is full (`suppressed` 445,409).
- The `0x50110000` 1408×768 sampled image is all-zero (L172, unchanged) but its surface is cleared and never drawn in that window — expected, not a defect.

**What is NOT established**

- The actual mechanism of the race black blotches (L201: per-frame visibility/ordering race, not content, not uploads, not draw order) — the device run neither captured a glitched frame at playable speed nor isolated the failing pass.
- The menu-glyph mechanism (L155/L175/L181/C5): alpha test vs ordering vs a different class — never separated on device; the alpha test is the cheapest falsifier (`MTL_PROBE_VARIANT=no-alpha-test`, C5).
- Whether the L203 side (WAW; Tile-vs-Fragment ordering) is the mechanism — never device-verified.

**Next discriminating runs (in order of cost)**

1. A menu-only run with the alpha test compiled out (`MTL_PROBE_VARIANT=no-alpha-test`, cold pipeline archive) — if the glyph loss stops, C5 is the mechanism.
2. `MTL_PROBE_VARIANT=heaps` (HazardTrackingModeTracked) — if the race blotches stop, the bug is heap-untracked ordering; if not, it is not heap ordering.
3. `MTL_PROBE_ONLY_DRAW` bisect from the level-3 quad trace for the menu glyph draws, or `MTL_PROBE_PASS_SENTINEL=1` to separate "no draw painted this" from "a draw painted this".
4. A playable-speed capture (no `serialise`, minimal probe) of the menu and the race with F12 on the flicker frame, so the failing frame is actually in the dumps.

## 2026-11-09 latest — the menu tape confirms the missing glyph, the alpha test is exonerated

The 15 consecutive taped presents of the menu press (frames 969-983, capture 2, press at 983) show the description line ("...variety of race events at the home of motor sports, F") with the leading "A" missing in **all 15 frames** — the shot (`host-shot-983`) and the taped present are byte-identical (MD5 `c1a6859c...`). The 15 frames differ only by a subtle brightness shimmer (~3.3-3.4k pixels/frame, never black-involving), so the "A" is a consistently missing glyph, not a per-frame pop. The glyph cell is not reserved (the visible text starts at x~268 with "variety").

The font path was fully traced: fp-76 is `r0 = TEX2D(0, tc0.xy) * saturate(diff_color)`; `diff_color` is `vs-32`'s `_fetch_constant(0)` (vertex constant 0), and the transform constant at index 0 is `(1,1,1,1)` for every font draw this run; `alpha_func = (rop>>12)&7 = 4` (greater), `alpha_ref = 0.0`. The alpha test therefore passes for any atlas alpha > 0 and never kills a glyph — **C5 is exonerated for the fp-76 font draws**. The missing glyph is not an alpha-test kill; the atlas is byte-identical every frame (L155) and the failing quads' recorded state matches the passing ones. The mechanism is not the alpha test and not the atlas; it is either a dropped/ mis-bound draw or a vertex-data/ordering fault (L201 class). The next discriminator is a menu-only run with the font draws' `ONLY_DRAW` isolated, or `PASS_SENTINEL=1` to separate "no draw painted this" from "a draw painted it".

Screenshot map corrected: `…_21_22_51.png` = the menu press (frame 983); `…_21_25_08.png` and `…_21_25_47.png` = the race (14/14, lap 00:31.450 / 01:26.133).
