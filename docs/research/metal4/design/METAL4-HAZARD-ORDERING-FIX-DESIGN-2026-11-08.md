# Metal 4 — hazard-ordering fix design (2026-11-08 late)

Status: **implemented (code) 2026-11-08 late; device validation pending (E1/E2/E5).**
Nothing in this document was validated on hardware yet; every landed change is
GPU-free-test-pinned (674/674). Device results land as L204.

Implementation deltas since this document was drafted:
- The satisfied-mask dedup was redesigned to **per-pass scratch** (cleared in
  `begin_pass`) after the `ManyPassesManyFramesDoesNotCorrupt` test caught a
  cross-pass edge-suppression flaw in the per-record scheme: a write in a
  later pass was silenced by an earlier pass's identical-mask consumption.
  Per-pass clearing re-emits every edge a pass owes a record — strictly
  stronger, simpler, and it made the "exactly-once per (record, bucket,
  class)" invariant a per-pass invariant, which is the only sound form.
- One truncation defect in the redesign (64-bit `MTL::Stages` cast through
  `static_cast<u32>` into the missing-mask) was caught by the stage sweeps
  and fixed; the sweeps are now load-bearing, not decorative.
- emitted specs are per candidate (never unioned across candidates), zero
  consumer masks emit nothing, and `flush(nullptr)` carries edges into the
  next encoder instead of destroying them.
- L-A (WAW) remains unlanded, measure-gated (D1b). L-C (stencil view note)
  landed in record_pass_hazards. D2/D3 landed. D4 as written (stencil view)
  landed. D5 landed. D6 landed.


Goal: fix the ordering layer **correctly**, minimally, and measurably — the
class that plausibly produces the per-frame black-plate/trunk-block flicker —
without game-visible behaviour changes, without knob-surface growth, and
without regressing frame time.

--------------------------------------------------------------------------------
0. Executive summary
--------------------------------------------------------------------------------

Two defect families live in the ordering layer, both verified:

R2 (visibility edge may not cover the tile flush — the prime suspect for the
    frame-4572 class).
  Colour and depth attachment writes are noted `StageFragment` only
  (MTLGSRender.cpp:3032, :3055), but those attachments store with
  `LoadActionLoad/Clear` + `StoreActionStore` (MTLFramebuffer.cpp:~172,~187) —
  the store is a **tile-stage** operation on Apple silicon. The colour
  *resolve* target alone is noted `StageTile` (:3048), which means the authors
  believed tile-stage writes need tile-stage edges — the attachments do not
  follow the same rule. Depth-resolve target is never noted at all (its
  ordering rests entirely on the producer barrier in the depth resolve pass,
  MTLSurface.cpp:524-533). If a `StageFragment` queue barrier does not
  encompass the tile flush, then **every attachment sampled by a later clause
  is un-ordered against its own store** — exactly the cyclic display-RT
  (`0x50110000`/`0xc0000000`/`0xc0880000`) class the frame-4572 flicker lives
  on.

R1 (the oracle drops computed edges).
  `note_read` consumes the single write record wholesale — its consumer
  stages are destroyed for the whole frame, so a second consumer (vertex +
  fragment sampling one shared materialised image) finds `write_stages == 0`
  and produces **no edge** (reproducer:
  `MTLSchedulerHypothesis.SharedDeferredUploadOrdersFragmentButStarvesVertex`,
  L202 ii-2). Producer masks overwrite instead of OR (L202 ii-5). And
  `flush(nullptr)` silently destroys pending edges (one fragile call site,
  MTLGSRender.cpp:2106; not a live defect today). Same-pass TBDR feedback
  pairs are unorderable by construction and `split_requested()` has no
  consumer in the backend (L202 ii-3).

Plus two latent gaps and a missing edge (nothing depends on them being wrong
today; all cheap to close):
  L-A — WAW is entirely untracked: two clauses writing one attachment are
        never ordered by any barrier.
  L-B — the depth *resolve* target gets no scheduler note.
  L-C — the redirected depth unit's stencil view (the MSL-direct `read()`,
        MTLArgumentBinds.cpp:131-138) is never noted by the scheduler; it
        rides only on the depth-*view* read transitively.

Design verdict up front:
  Stage 1 (E1, cheapest, highest density): two-line StageTile note change
  (D2) + depth-resolve note (D3). Measure. This is the falsifier for the
  frame-4572 hypothesis and needs no other design.
  Stage 2 (D1): the oracle redesign — consume per consumer class, OR producer
  masks, per-candidate same-pass specs, null-flush carries instead of
  destruction, comment/code truth fixes. Subsumes R1 and closes L-A-adjacent
  correctness; keeps WAW **opt-in behind a measure gate** (D1b).
  Stage 3 (D4/D5/D6): the small latent closures and the falsification gate
  (§6). L203/L204 land the rows.

Honesty clause, stated once and meant: **this design does not prove the
frame-4572 defect is ordering.** E2 (the serialise arm) can veto the ordering
theory outright. If the defect survives byte-identically in serialise, the
ordering theory dies and Stage 4 (content-class analysis) takes over — §6 Gate G.

--------------------------------------------------------------------------------
1. Evidence base (do not re-litigate)
--------------------------------------------------------------------------------

- L193/L195 — the car/livery group draws bind byte-identical units in failing
  and clean windows (u0 paint DXT45, u1 `c1a00000` reflection lookup, …); the
  defect pixels are pure `#000000`; window diffs name fp-119/fp-40/fp-113.
- L201 — frame-4572 offline census (dumps-rca-20261108-194019, clean reference
  host-shot-4495 ≡ 4462 inside the defect window, one byte sky difference
  whole-frame): newly-black components 876 px x[538..580] y[514..540] + 232 px
  bar x[524..535] y[499..535] + 18 px + ≤8 px specks; **per-frame flicker, not
  monotone, not parity-locked** (4200:201, 4204/4205:0, 4234:6 → 4235:689,
  4412/4413:~70, 4462..4496:0, 4557:148, 4558:0, 4572:1193, 4573:70); full
  1005-record (vp,fp) multiset identical; all 32 car-paint fp105 draws
  byte-identical incl. unit bindings; no texture uploads after frame ~3986;
  never-uploaded `0xc1a00000` zero in both windows; BC-zero blocks static;
  `0xc0000000` hazards == samples exactly (15.79M); `suppressed` grew
  0 → 1.97M by frame 4574 (384-address census full — addresses beyond it are
  invisible to the instrument, *not* "suppressed hazards").
- L202 — scheduler verdict **incomplete**: 114 new tests, 674/674 green;
  prologue hypothesis refuted as written; ii-1..ii-10 enumerated; comment/code
  disagreements pinned; device experiments enumerated (iii-a…iii-f).
- The current build ships the PB double-blend fix and the L199/L200 disk fixes;
  defect persists → not PB, not an upload gap, not content of any sampled
  texture the census can see.
- The menu-item flicker is **out of scope here**: the 22:05–22:07 photos are
  the ~5 fps diagnostic build stuck in the splash loop, not the menu; a light
  build is required to capture it (§9).

--------------------------------------------------------------------------------
2. Requirements and non-goals
--------------------------------------------------------------------------------

R1. RSX is the specification. No suppression, no "paint around it", no
    sentinel/poison in shipping behaviour; ordering fixes only.
R2. Performance first. Every added barrier is measured at the grid (stationary,
    worst-case for the static start scene; it is also the defect's home
    ground). Abort threshold for the whole change: present/frame time
    regression > 2% at the grid, or pass/hazards GPU cost up > 0.5 ms/frame.
R3. Zero new environment knobs. The 7-knob surface stays exactly as is
    (MTL_PROBE, MTL_PROBE_LOG, MTL_PROBE_DUMP_DIR, MTL_PROBE_FRAME_COUNT,
    MTL_PROBE_ONLY_DRAW, MTL_PROBE_PIXELS, MTL_PROBE_VARIANT).
R4. Diagnostics remain TODO(excise), bounded, default-off; none of the design
    depends on a probe being armed.
R5. GPU-free suite stays green; new semantics land with new tests in the same
    commit as the behaviour change. Target: ≥ 690/690 after D1 lands.
R6. Minimal surface change; every fix lands at the layer that owns the fact
    (scheduler semantics in the scheduler, note masks at the note sites).

Non-goals:
- Metal 4 raster order groups / framebuffer fetch — wrong granularity (they
  order per-fragment inside one pass; our defect class is cross-pass visibility).
- HazardTrackingModeTracked heaps shipped by default — "perf" unmeasured; it is
  the E3 discriminator, not the fix (§5).
- Any frame-4572-specific special case, livery-specific or scene-specific.
- The menu-item flicker and the FMV/splash behaviour; the FMV is fine in
  shipping builds.

--------------------------------------------------------------------------------
3. Root-cause model (why the class flickers)
--------------------------------------------------------------------------------

Apple silicon render passes hold output in tile memory. `StoreActionStore`
flushes tile → memory at pass end. A **new render pass that samples that image
must see the flushed content**; with `HazardTrackingModeUntracked` heaps (all
heaps today, MTLResourceManager.cpp:54), Metal guarantees nothing without an
explicit barrier. The backend's lever for visibility is the barrier the pass
scheduler emits at the head of each pass encoder (or its carried split).

The producer mask in that barrier names the stages that wrote the resource.
For attachments we name Fragment only. Either (a) `barrierAfterQueueStages`
with a Fragment mask encompasses the tile flush (then the layer is complete
and the defect is elsewhere — §6 Gate G catches this), or (b) it does not
(then every sample of a just-stored attachment is un-ordered — R2 — and the
scheduler must name the tile stage). **This question is not CPU-decidable; it
is the E1 experiment.**

Why only some frames: the race is a timing race between the store of the pass
that wrote `0x50110000`-class pyramids/canvas and the first shader read of the
next pass that samples them. Most frames the tile flush lands in time; some
don't, and the content the car-paint samples (the reflection chain) comes
back stale — pure-#000000 regions exactly where the reflection lookup is
content-dependent (the lenses), benign elsewhere. The frame-to-frame
ping-ponging store (c0880000 ⇄ c0000000) and the full-screen env passes make
the window wide. This is a mechanism, not a proof — the falsifier is E2.

Why the flicker keeps the *same* bbox: the unwritten texels live at fixed
UV/mip addresses of a fixed texture set.

--------------------------------------------------------------------------------
4. The design (D1…D8)
--------------------------------------------------------------------------------

D1 — Oracle redesign: consume per consumer class; never destroy stage bits.
  m_note_read / note_write records gain:
    MTL::Stages write_satisfied_same = 0, write_satisfied_earlier = 0;
    MTL::Stages read_satisfied_same  = 0, read_satisfied_earlier  = 0;
  Rules:
  a. note_write OR-s its mask into entry.write_stages (no overwrite). Adding
     **new producer bits** resets both satisfied masks (re-arm) — correctness
     over convenience; old bits already covered keep their edges.
  b. note_read(resource, range, stages): for every conflicting write
     candidate, bucket by candidate.write_pass == m_pass; compute
     missing = stages & ~candidate->satisfied[bucket]; if non-zero, emit one
     spec { producer = candidate->write_stages, consumer = missing,
            visibility = Device, cross_pass = (bucket == earlier) };
     record the emitted bits into candidate->satisfied[bucket].
     Specs are emitted **per candidate, not unioned per bucket** — a
     legal-Vertex producer and an illegal-Fragment producer in the same bucket
     no longer drag each other into carried status (closes L202 ii-9 by
     construction; barrier count is bounded by candidate count ≤ ~6/frame).
  c. WAR mirrors (read side satisfied masks; per-candidate specs).
  d. Same-pass specs still split legal/illegal by `stages_legal_intra_pass`
     (legal → barrierAfterEncoderStages; illegal → carried + split latch).
     Unchanged semantics, per candidate instead of unioned.
  e. Cross-pass specs are always queue form, as today.
  f. Exactly-once per (write record, bucket, consumer-stage class); coverage
     is conserved across frames because satisfied rides the record and resets
     only on new producer bits or begin_frame.
  g. `split_requested()` keeps current meaning (a carried edge exists); its
     missing backend consumer is documented (D8), not silently "fixed".

  Performance: emitting per candidate adds ≤ (candidates−1) specs worst case;
  in the common frame where one RT dominates, unchanged. Measured at the
  grid per R2; abort threshold applies.

D1b — WAW, measure-gated, not default.
  note_write additionally scans m_write_records for earlier writers of
  overlapping footprint and emits { producer = earlier.write_stages,
                                    consumer = stages, Device, queue }.
  Rationale: Metal-4 untracked heaps promise nothing; two full-screen fills
  to one attachment with no intervening read are formally unordered.
  Apple silicon executes command-buffer encoders in submission order, so this
  may be a no-op in practice — it is included for spec correctness, and it
  rides the same dedup machinery. Ship only if the E4 perf window is flat
  (or ≤ 1 barrier/clause in the grid measurement). If it regresses, the row
  lands as "documented gap, not shipped".

D2 — Attachment/depth notes carry the tile stage (stage-1 fix).
  MTLGSRender.cpp:3032 (colour attachments) and :3055 (depth attachment):
      scheduler.note_write(id, range,
          access_stages(rsx::surface_access::shader_write, false) | MTL::StageTile);
  Colour resolve target stays StageTile-only (:3048 — correct as is).
  Net effect: every attachment store is an edge naming tile stages; a later
  pass sampling that attachment orders the tile flush (or carries the edge
  to the next encoder when a producer is same-pass).

D3 — Depth *resolve* target note (the missing mirror).
  Mirror the colour branch immediately after the depth attachment note:
      if (texture* depth_resolve = frame.attachments.depth()->resolve_surface.get())
          scheduler.note_write(depth_resolve->resource_id(),
              frame.attachments.depth()->get_memory_range(), MTL::StageTile);
  This closes L-B with the same HLD-02 §2.2 reasoning already documented for
  colour. The depth resolve pass itself already emits its own producer
  barrier (MTLSurface.cpp:532) — the note makes that edge visible to the
  scheduler for *later* passes, same as colour.

D4 — Stencil view read note.
  In record_pass_hazards' fragment-unit loop, the redirected depth/stencil
  inputs read by MSL via `read()` (line ~4910) have no scheduler note; the
  depth **view** read is noted (`fragment_textures[i]`), which transitively
  orders most clauses (every such clause also notes the depth attachment
  write at :3054, so it still gets one queue edge). The stencil slot
  (`fragment_stencil[i]`) is bound at `fragment_stencil_slot_base + i`
  (MTLArgumentBinds.cpp:131-138) purely as an MSL-direct read.
  Note it with StageFragment, exactly like a fragment texture. Conservative,
  one note per referenced redirected unit. If frame.fragment_stencil is
  empty/unused, the note is a no-op by the same mask test used for textures.
  (If the redirect turns out to bind *only* the depth view and never stencil —
  confirm at code-time — this note collapses; leave the note, it documents
  the contract either way.)

D5 — flush(nullptr) never destroys computed edges.
  Today: MTLPassScheduler.cpp:250-254 clears m_pending unconditionally on a
  null encoder, permanently consuming the write/read records that produced
  them. The two callers that can hand it a null encoder are the clear paths
  (MTLGSRender.cpp:2106 whole-attachment load-action clear; :2134 scissored
  fill with a failed open_pass), i.e. passes that encode nothing.
  Fix: flush(nullptr) moves m_pending into m_carried unchanged (they are
  emitted at the next encoder in queue form by emit_split_barriers, the safe
  form) and sets nothing else. Frame boundary still retires them via
  begin_frame. Comment updated. The behaviour is pinned by re-running and
  extending the null-flush tests (the suite already characterises it today;
  the expectation flips from "destroys" to "carries").
  Alternative considered and rejected: "skip notes entirely when no encoder" —
  loses the records' contribution to *later* passes of the frame for WAR at
  the clear site; carrying preserves it.

D6 — Comment/code truth fixes (no behaviour).
  - MTLPassScheduler.h:38-44 — replace "A pass boundary orders everything
    already queued" with: a pass boundary orders **nothing**; only barriers
    do. begin_pass clears per-pass state only; records survive.
  - MTLPassScheduler.h:63-67 — fix the request_texture_barrier wording (the
    carried edge is emitted on the *current* pass's encoder at pass-open
    before its draws; it is not "the next encoder" as a rule, it is the
    encoder that opens after the request).
  - MTLPassScheduler.cpp:88-97 — update the "same unit is noted on every
    draw" comment: the read index is per-footprint; satisfied masks replace
    the overwrite discussion.
  - MTLPassScheduler.cpp:250 — remove/replace the comment; the code now
    carries.
  - MTLBarrier.cpp:27-28 — "the tile is already flushed when the barrier
    runs" → hedge honestly: on Metal 4 the tile flush is ordered by the queue
    barrier's stage set on this backend (tile stage), which is what D2 adds;
    keep the comment consistent with the post-D2 contract.
  - MTLPassScheduler.h — class doc keeps the guest-range identity rationale;
    append the satisfied-mask invariant (satisfied[bucket] ⊆ consumer stages
    for which an edge naming the *current* producer mask was emitted).

D7 — Performance and observability (no knobs).
  Existing instruments suffice: `pass/hazards` (MTL_PERF_SCOPE(1) around
  barrier batches), the level-2 barrier latches/histograms, census prints,
  and the present/`frame time` logs. The measurement harness for this design
  is the defect metric itself — newly-black pixels in the plate bbox vs the
  host-shot-4495 reference:
      newly_black(x in [515..700], y in [490..555]) vs 4495 -> target 0
  plus `Metal 4 perf: [T] ... pass/hazards` per-frame cost and the
  present/logging frame time. Reported per launch: defect count across ≥3
  consecutive packed pairs (shot↔present, one frame apart).

D8 — Documentation debt and lifecycle.
  Each landed stage closes with: LOG-FINDINGS row(s) (L203 stage-1/fix or
  falsification; L204 D1 redesign + suite count; L205 measure/WAW outcome),
  this document's status header updated ("implemented — results"), and the
  RCA-status doc updated. The TODO(excise) list gains no entries.

--------------------------------------------------------------------------------
5. Experiments and gates (ordered; cheapest first)
--------------------------------------------------------------------------------

E1. Stage-1 build (D2+D3), `MTL_PROBE_VARIANT=heaps`, at the start line,
    stationary; F12 at 00:26–00:29 (the failing window); quit ~2 min.
    Scoring: newly-black census per shot/present pair vs 4495-class reference;
    perf counters from the log.
    PASS := 0 newly-black across ≥3 consecutive pairs; perf within R2.
    FAIL := defect persists → proceed to E2 before any wider change.
E2. Serialise arm, same build: `MTL_PROBE_VARIANT=serialise` (one command per
    encoder, wait-idle per draw).
    If defect vanishes under serialise → order confirmed, E1/D1 proceed.
    **Gate G (falsification):** if the defect *persists* under serialise, or
    if frames remain byte-divergent in the defect pixels under full
    wait-idle, the ordering theory is false for this defect; the ordering
    fixes still proceed (they close verified latent bugs) but the frame-4572
    investigation reverts to content-class analysis (poison dumps, the
    `0xc1a00000` mask chain, and the pyramid/canvas pair) — that fallback is
    deliberately not designed here.
E3. Heaps-vs-default A/B at the grid (discriminator only, not a fix): runs
    with default vs `heaps` in the same build; defect-rate comparison on
    ≥10 F12s each. Informative for the shipping question, not required for
    Stage 1.
E4. With D1 landed: WAW measure window; ship D1b only if flat (see D1b).
E5. Full re-baseline capture: stationary start-line window, ≥8 shots/presents,
    census (hazard counts should drop to emitted-edges-only; `suppressed`
    should stop growing across windows ≥ 3986 → stays flat), perf, and the
    defect census.

Effort (staff estimate): E1/D2/D3 ≈ 0.5 day incl. build + run + scoring;
D1 ≈ 1–2 days with tests; D5+D6 ≈ 0.5 day; E5 ≈ half a day; total ≤ 4 days.

--------------------------------------------------------------------------------
6. Validation, rollout, rollback
--------------------------------------------------------------------------------

GPU-free: existing test_rsx_metal4_scheduler.cpp re-pinned for D5/D1 semantics;
additions for: satisfied-mask reset on producer growth; per-candidate
same-pass legal/illegal split emission; exactly-once per
(write, bucket, consumer-class); null-flush carry; D2 note mask presence
(the attachment note exists with tile bits — source-level assert); D3 note
mirrors colour; D4 note exists. Target ≥ 690/690 (R5).

Commit plan (each standalone-revertable; author `hsaka -1.0`; no fixes without
your explicit go; GFX stays out of every commit):
  1. D6 comments + D5 null-flush carry + tests.
  2. D2 + D3 + tests (+ L203 row with E1 result).
  3. D4 + tests.
  4. D1 oracle redesign + tests (+ L204 row).
  5. E5 baseline + perf window (+ L205 row, this doc → "implemented").

Rollback: revert the commit; the backend has no saved-state/ABI impact; the
diagnostics keep working (they are unrelated); census/hazard accounting is
observational only.

Gates: G1 = GPU-free green incl. new tests; G2 = E1 PASS; G3 = E5 census flat;
G4 = perf within R2. Any gate failure halts rollout with the evidence logged,
not with silent drift.

--------------------------------------------------------------------------------
7. Risks and what we are accepting
--------------------------------------------------------------------------------

- Over-barriering / frame time: queue barriers at pass heads are dependency
  edges, not full pipeline flushes; Apple silicon executes encoders in
  submission order; the measured cost is expected ≈ 0. R2 aborts if not.
- Same-pass edges carried everywhere: D2 turns same-pass legal/illegal unions
  into per-candidate decisions (D1b-a), so legal producers still emit intra-pass.
- Per-candidate emission count: bounded by ≤ ~6 write candidates per frame;
  no path emits quadratic edges.
- Apple-silicon-only assumptions (tile memory store visibility, submission-
  order command buffers): this backend is Metal4-only; if a future backend
  lands on other silicon the notes stay valid (tile bits are no-ops elsewhere
  on Apple hardware); document as constraint, revisit then.
- RSX's within-pass RTT feedback semantics are formally undefined for Metal;
  the cyclic split path (request_texture_barrier → carried queue edge) is the
  backend's best-effort contract. This design orders what can be ordered and
  does not claim more.
- E2 may never exercise a failing frame (if the drive never reaches green):
  evidence is then "defect not observed in N captures", recorded as such —
  not as a fix confirmation.

--------------------------------------------------------------------------------
8. Open questions for the user
--------------------------------------------------------------------------------

1. Go for the Stage-1 build + E1 launch (D2+D3, light `heaps`, F12 at the
   countdown)? This is the single highest-value experiment available.
2. D1b (WAW): measure-only with intent to ship if flat — or record as
   documented gap without shipping?
3. Commit packaging per §6 — separate commits as listed, or one?
4. The menu-item flicker: scoped to this series (it may share the cyclic-RT
   mechanism; capture after G2 lands) or a separate RCA track?
5. New test file `test_rsx_metal4_scheduler.cpp` (untracked, subagent's work):
   fold into commit 1 or commit separately?
6. Keep `build/probe/dumps-rca-20261108-194019` (2.5 GB) and the
   `dumps-serialise-20261108-220404` pack (~750 MB) as-is until E-series
   results, or prune now?

--------------------------------------------------------------------------------
9. Out of scope, deliberately
--------------------------------------------------------------------------------

- Menu-item flicker capture and RCA (needs a light build reaching the menu;
  separate track; see §8-4).
- The intro splash / FMV sequence (fine in shipping builds; the magenta run
  was a diagnostic artifact).
- The `0x50110000` canvas/pyramid "two images, one address" identity question
  (L172/L173 family) — observable, not a blocker for ordering.
- The 2026-11-09 sponsor-atlas / `c679f780` zero-atlas anomaly — unchanged by
  this design (not demonstrated to be ordering).
- Any change to the 7-knob probe surface or the diagnostic TODO(excise) set.
