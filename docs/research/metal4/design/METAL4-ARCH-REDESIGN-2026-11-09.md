# METAL4 — backend architecture redesign (2026-11-09)

Status: **P1 implemented** (long-lived encoder, recorder boundary, feedback guard);
P2 identity and P3 store-op minimisation remain deferred — see §14.

Inputs: L201–L210, the 21:21:51 probe
run, and a read of `MTLGSRender.cpp`, `MTLFramebuffer.cpp`, `MTLCommandRecorder.cpp`,
`MTLPassScheduler.*`, `MTLBarrier.cpp`, `MTLTextureCache.*`, `Common/texture_cache.h`.
Device numbers are from `build/probe/probe-rca-20261109-212151.log` and are *counts*,
not timings (that run was instrumented to <1 fps).

---

## 0. TL;DR — the decision

There is no single Metal 4 call that replaces `glTextureBarrier()` or
`VK_EXT_attachment_feedback_loop_layout`. The correct answer is a **contract**, not an
API:

> **Batch draws into long-lived render encoders; put an explicit, conservative
> queue barrier at every encoder boundary; copy only when a draw samples an
> attachment the *current* encoder is rendering; bind the live attachment (plus a
> boundary barrier) for every cross-encoder cyclic read; and make guest-range
> identity monotonic so a sampled image can never be a stale view of an address that
> is currently an attachment.**

That is correct by construction: correctness never depends on the hazard oracle
guessing an edge. The oracle is demoted to an **optimiser** that decides where to
batch and how narrow a barrier may be. If the oracle is wrong, the result is
slower, not broken.

The performance consequence is larger than the copy-vs-split question: the backend
currently opens **~1 render pass per draw** (371 passes/frame vs 370 draws/frame in
the measured run). That is the dominant cost and the source of the WAW/tear class.
Batching is both the correctness fix and the performance fix.

**Correction to the earlier analysis:** copy-at-bind and pass-split are **not
alternatives**. They legalise disjoint cases:

| Case | Legalisation |
|---|---|
| A draw samples an attachment the **same encoder** is rendering (same-clause feedback) | **copy** (a pass split cannot split the draw) |
| A draw samples an attachment a **previous, still-open encoder** rendered (cross-clause) | **split** (close + reopen) or **copy** — a policy choice |
| A draw samples an attachment a **previous, already-closed pass** rendered (the common case today) | **no copy, no split** — bind the live image + a boundary queue barrier |
| Two draws write the same attachment | **no ordering needed inside one pass** (raster order is defined); a boundary barrier if they land in different passes |

The current per-draw pass model makes case 3 look like case 2, which is why the
cyclic path appeared to need a copy everywhere. It does not.

---

## 1. What the evidence actually says

Cumulative counters at teardown (2933 frames):

| counter | value | per frame |
|---|---:|---:|
| `framebuffer/pass opened` | 1,089,440 | **371.4** |
| `draw/record` | 1,086,099 | **370.3** |
| `present/displayed` | 234 per perf window | — |
| `frame witness` | 2933 | — |

**1.003 render passes per draw.** `MTLGSRender::end_draw()` calls
`frame.attachments.open_pass(...)` unconditionally (`MTLGSRender.cpp:4439`), and
`command_recorder::open_pass()` opens with `close_encoder()` first
(`MTLCommandRecorder.cpp:557`). The "clause-per-encoder" model was intended as an
ordering tool; it became the default granularity.

Every pass stores and reloads all its attachments (`MTLFramebuffer.cpp:170-200`:
`LoadActionLoad` + `StoreActionStore`, depth always `Store`). For a 1280×720–1408×768
colour target that is **≈3.7–4.3 MB store + 3.7–4.3 MB load per pass**, i.e.
≈2.7–3.2 GB of attachment traffic per frame *if the driver does not elide it*.
Whether Metal fuses identical consecutive passes is exactly what P0 must measure;
either way the design intent is wrong and it is the first thing to fix.

Two consequences:

1. **Performance**: attachment bandwidth, not the 128 KB feedback copy, is the
   thing to attack first.
2. **Correctness**: a pass boundary orders nothing by itself (the scheduler header
   says so correctly). The current design relies on the per-frame oracle to emit
   barriers across those 371 boundaries, and the oracle is known incomplete (WAW,
   StageTile, starved vertex consumer — L202/L203). Every boundary is an opportunity
   to drop an edge; batching removes ~97% of those opportunities.

---

## 2. Why the current architecture does this

- Ordering was modelled as "a boundary the scheduler controls" (`HLD-03`), and the
  draw clause is the only place the backend reliably knows the full read/write set.
- `pass_scheduler::split_requested()` — the designed escape hatch — has **no
  consumer** (`MTLPassScheduler.h:80`; only tests read it). The scheduler computes
  "this edge cannot be ordered in this encoder" and nobody acts on it.
- Because a pass is opened per draw anyway, the scheduler's intra-pass problem is
  mostly imaginary: it never actually encodes two draws in one encoder. The same-pass
  feedback case it worries about is only reachable inside a multi-subdraw clause.
- The cyclic bind then takes a copy at bind time (`requires_feedback_loop_copy`,
  `MTLTextureCache.h:38`; `texture_cache.h:2893`) because Metal cannot sample the
  live attachment. That copy is correct and cheap, but it is compensating for a
  boundary model that should not have created the situation.

---

## 3. The three hazard classes

| Class | Definition | What Metal permits | Chosen legalisation |
|---|---|---|---|
| **H1 — intra-encoder feedback** | a draw samples a texture whose guest range overlaps an attachment of the **currently open** encoder | undefined/illegal on TBDR; no feedback primitive in MTL4 (headers contain zero mentions) | **copy at bind** (snapshot), keep the encoder open |
| **H2 — cross-encoder feedback** | a draw samples a range an **earlier encoder of this frame** wrote | legal if an explicit barrier orders the write before the read | bind the **live attachment image** + `barrierAfterQueueStages(StageAll, StageAll, Device)` at the boundary |
| **H3 — attachment WAW/WAR** | two draws write the same attachment, or one pass overwrites what an earlier one stored | **within a pass**: defined raster/submission order, no barrier; **across passes**: needs a barrier | batch them; if split, boundary barrier |

The important, non-obvious part is **H3-inside-a-pass**. Two draws writing the same
pixel in one render pass are ordered by the hardware (same guarantee GL and VK give
inside a render pass). The taillight/trunk-block tear (L201) is exactly what
*separate passes without a barrier* produce. **Batching fixes WAW for free.**

H1 cannot be fixed by a pass split. If a single draw samples what it writes, there
is no pass boundary that separates the read from the write without splitting the
draw. Copy (or a ping-pong attachment) is the only legalisation. That is why the
L204 copy stays.

H2 does not need a copy at all once the live image is bound and the boundary has a
barrier. The 114,209 samples of `0x50110000` (L208) fall here; today they resolve to
a stale all-zero 1408×768 view. Binding the live attachment fixes the content *and*
removes most of the copy traffic.

---

## 4. The contract

**C1 — Batch key.** A render encoder stays open across draws while all of the
following hold:
- `same_framebuffer(layout, previous)` (already exists, `MTLFramebuffer.cpp:19`);
- the same active colour subset (`surface_a` / `surface_b` / MRT set) and depth;
- no clear that requires a load action;
- no resolve, readback, blit, upload, present, or surface rebind of an attachment's
  guest range to a different surface object (alias epoch, C5);
- no H1 sample in the draw (if one appears, legalise it, do not close).

**C2 — Boundary rule.** Every close→open pair emits
`barrierAfterQueueStages(MTLStageAll, MTLStageAll, MTL4VisibilityOptionDevice)` on
the first encoder after the boundary, unless the boundary is proven independent.
`StageAll` (not `StageFragment`) is deliberate: it includes `StageTile`, which
settles L202's open question (a) without a device experiment. A handful of
boundaries per frame makes the conservative form affordable; narrowing is an
optimisation (C6).

**C3 — No self-sample.** At texture bind, if the descriptor's guest range overlaps
any attachment of the **open** encoder, never bind the live view:
- same clause → copy (current L204 path);
- cross clause → close the encoder, then take the H2 path (live bind + C2 barrier).

This check uses only the *current attachment set*, not the hazard oracle. It is
complete and cheap (≤4 colour + depth comparisons per bind, already range-keyed).

**C4 — Canonical identity.** For a guest range bound as an attachment this clause,
the sampled image is the attachment's own image, or a snapshot of it. The fast
texture search may never return a different host image over overlapping bytes. If
it would, force the surface view or the copy. One guest range → one answer per
clause; no per-frame choice.

**C5 — Alias epoch.** Every write to a guest range bumps a monotonic epoch. A view
records the epoch it was resolved at. A bind whose view epoch < the range's current
epoch is re-resolved (or copied). This covers `0xc0000000` being the scene RT, a
256×128 scratch, and a 1280×720 target in one frame, and every aliased
`0x50110000`/`0x50110800` case. Aliasing is identity by guest bytes, not by Metal
object.

**C6 — Oracle is an optimiser.** `MTLPassScheduler` may decide to *keep* a batch
longer, *narrow* a barrier, or *choose* copy vs split by bytes. It may never be the
reason a correct barrier is omitted. If its state is uncertain, the conservative
path wins.

---

## 5. Target pipeline shape

```
frame start
  └─ begin command buffer, residency set
     ┌─────────────────────────────────────────────────────────────┐
     │ batch: one render encoder per (framebuffer, no-boundary-run) │
     │   draw, draw, draw ...                                       │
     │   H1 at bind → copy into a temporary, bind the temporary     │
     │   C2 barrier at boundary (rare)                              │
     └─────────────────────────────────────────────────────────────┘
     boundary reasons: clear-as-load, resolve, readback, upload,
                       surface rebind/alias epoch, present, frame end
  └─ present / flip
```

Draws per pass should go from **~1** to the natural RSX run length (tens to
hundreds). Passes per frame should go from **~371** to **single digits plus one per
real event** — the exact number is what P0/P1 measure.

---

## 6. Identity model (the `0x50110000` bug)

Today `0x50110000` is simultaneously a 512² shadow RT, a 512² sample, a 1280×720
composite input, and a 1408×768 all-zero image (L172/L208). The fast path
(`process_framebuffer_resource_fast`, `test_if_descriptor_expired`) can return a
host image that is neither the current attachment nor a snapshot taken after the
last write to the range. Which image wins is a per-frame race — exactly the shape
of a nondeterministic grey/black blotch.

The fix is C4 + C5, enforced at the single place where a sampled descriptor is
resolved (`resolve_texture_bind` / the fast-search call sites):

```
if range overlaps an attached range of the current framebuffer:
    if that attachment is open in this encoder:      copy (C3/H1)
    else if view.epoch == range.epoch:               live bind (H2)
    else:                                            re-resolve / copy
else:
    normal fast search (epoch-checked)
```

Add a probe latch (`texture/bind alias`) whenever the attachment-overlap branch
runs. In a correct frame it should be stable per range, not alternating.

---

## 7. Native Metal 4 mapping

| Need | MTL4 primitive | Use |
|---|---|---|
| boundary ordering | `barrierAfterQueueStages:` | C2, producer `MTLStageAll` |
| cache flush | `MTL4VisibilityOptionDevice` | every boundary barrier |
| guest alias flush | `MTL4VisibilityOptionResourceAlias` | P3: measure on `0xc0000000` / `0x50110000` aliasing |
| legal intra-pass edge | `barrierAfterEncoderStages:` | rarely needed; draws do not write non-attachment resources |
| multi-pass stitching | `MTL4RenderEncoderOptions{Suspending,Resuming}` | P4 only, for parallel encoding; **cannot** span a compute/blit or an H1 copy, so not a feedback fix |
| avoid PSO recompiles for `surface_b` | `supportColorAttachmentMapping` + `setColorAttachmentMap:` + pipeline `colorAttachmentMappingState = Inherited` | P3: keep one pass across target-subset changes |
| resource binding | `MTL4ArgumentTable` snapshot-at-encode | makes the H1 copy the natural snapshot point |

The header audit is unambiguous: MTL4 has **no** feedback-loop support and **no**
automatic hazard tracking; `barrierAfterEncoderStages:` is an execution/cache
barrier, not a tile-memory visibility primitive. So C2/C3 are not optional.

---

## 8. Performance model

| Item | Now | After batching |
|---|---|---|
| render passes / frame | ~371 | ~5–20 (events only) |
| attachment store+load | ~2.7–3.2 GB/frame (if not elided) | ~1–3 round trips/frame |
| feedback copies | 1 / frame, 256×128 ≈ 128 KB (measured) | same; cross-pass copies mostly disappear |
| boundary barriers | ~371 + oracle edges | ~5–20 conservative `StageAll` barriers |
| WAW ordering | untracked across passes | defined inside a pass |

The feedback copy was never the bottleneck. The per-draw pass is. A split is
only worth it when a cross-clause sample region is larger than the attachment
round-trip it would avoid — and with batching that decision becomes rare enough to
measure (C6). Keep the copy as the default for H1; never make it the default for
H2.

---

## 9. Rollout

**P0 — measure, no behaviour change.**
Add counters: `passes/frame`, `draws/pass`, `boundary barriers/frame`,
`attachment store bytes/frame`, `attachment load bytes/frame`, `copy bytes/frame`,
`copies/frame`. Run RR7 with `MTL_PROBE=0` first (static build) to get an honest fps
baseline, then with counters. This is the missing data point in every prior
comparison. Gate: baseline recorded in `LOG-FINDINGS.md`.

**P1 — batching (the main change).**
- `framebuffer::can_absorb(surface_state, clear_mask)` = C1.
- `end_draw()`: reuse `frame.encoder` when `can_absorb`; otherwise close and reopen.
- `record_pass_hazards` moves to the boundary only; the common path records no
  oracle state.
- Boundary emitter: C2.
- Bind-time guard: C3.
Gate: 674/674 tests green; passes/frame down ≥10×; no new visual regression; probe
witness unchanged (`live 0 copy 1` may become `live 1 copy 1` or `live 1 copy 0` —
record which).

**P2 — identity (C4/C5).**
Thread the guest range into descriptor resolution; add the epoch map; force the
surface view/copy for attachment overlaps; add the `texture/bind alias` latch.
Gate: `0x50110000` resolves to one image per clause; the 1408×768 all-zero view is
never bound while the range is an attachment; tests for alias/epoch.

**P3 — policy and narrowing.**
Copy-vs-split by bytes; narrow C2 with the epoch tracker (only when the oracle can
prove independence); `ResourceAlias` experiment; attachment map for `surface_b`.
Gate: measured improvements, each knob default-off and bounded.

**P4 — encoding throughput.**
Only if P1 shows CPU encoding is the next bottleneck: `MTL4RenderEncoderOptions`
suspend/resume for parallel encoding of independent passes. Never around a hazard.

---

## 10. Tests

- `framebuffer::can_absorb`: same layout / changed layout / clear / resolve /
  surface subset / alias rebind.
- Boundary rule: every close→open emits exactly one `StageAll` barrier by default;
  a proven-independent pair may omit it.
- C3: a bind that overlaps the open encoder's attachment takes exactly one copy;
  a bind that overlaps a closed pass's attachment takes zero copies and binds live.
- C5: epoch bump on write; stale view re-resolves; no per-frame flip.
- Keep `test_rsx_metal4_scheduler.cpp` as the pinning suite for the optimiser; it
  should no longer be able to make the backend *incorrect*.

---

## 11. Risks / what could go wrong

- **Resource lifetime in long encoders.** A surface cannot be destroyed mid-batch;
  the recorder's resource pool already defers releases, but the surface cache's
  eviction path must be checked against a still-open encoder.
- **Load actions.** A mid-batch clear must force a boundary; the scissored fill
  stays a draw and can stay in the batch.
- **Alias rebinds.** `0xc0000000` changes role inside a frame; C5 must force a
  boundary, otherwise a batch spans two surfaces over one address.
- **MSAA resolve.** The resolve happens at pass end; a same-batch sample of the
  resolve target is H1 → copy/split. Do not assume the resolve is visible.
- **Occlusion queries.** `VisibilityResultTypeAccumulate` is already per pass;
  batching changes the number of passes a query spans, not its result.
- **Depth mirror.** `EMULATE_DEPTH_COMPARE` copies depth before the draw; that is a
  boundary. It stays rare; do not try to hide it inside a batch.
- **Driver elision.** If Metal already fuses the per-draw passes, P1's win will be
  smaller than the arithmetic suggests. P0 decides this before P1 is committed.

---

## 12. Open measurements (ranked)

1. **P0**: real fps and attachment bytes/frame with diagnostics off. Decides
   whether P1 is worth 100× or 2×.
2. Does a StageAll queue barrier order a tile-store (`StageTile`) for a sampled
   attachment? (L202 (a).) C2 assumes yes; the `StageAll` producer makes it moot.
3. `ResourceAlias` vs `Device` on `0xc0000000` / `0x50110000` aliasing.
4. After P1: does the L201 tear disappear with batching alone (WAW in-pass)?
5. After P2: does `0x50110000` stop resolving to the stale image, and does the
   car-pass blotch change?

---

## 13. Relation to KosmicKrisp

KK is native MTL4 (`MTL4CommandBuffer`, `MTL4RenderPassDescriptor`,
`MTL4RenderCommandEncoder` — `bridge/mtl_encoder.m:271`) and already implements the
shape proposed here. The mapping is direct:

| Concern | KK | This design | Relation |
|---|---|---|---|
| Encoder granularity | lazy `cs_get_render`, open across every draw until a pass change (`kk_cmd_buffer.c:298-312`) | C1 batch key | **same idea; ours is missing it today** |
| Boundary ordering | `end_encoder` emits `barrierAfterStages(ALL, ALL)` before every `endEncoding` (`kk_cmd_buffer.c:266`) | C2 | **KK is stronger and simpler; adopt it** |
| Split predicate | `kk_barrier_requires_encoder_split` (`:409`): any attachment write seen + any texture read → split; MSAA always splits | C3/C5 range+identity | **KK over-approximates; our scheduler under-approximates** |
| Split mechanics | `apply_store_ops` → `cs_end` → `cs_start_render` (`:470-478`) | close + reopen | same |
| Intra-pass barrier | blanket `Vertex→Fragment` (`mtl_encoder.m:39`) | rarely needed | equivalent |
| Store/load policy | per-attachment store op, `DONT_CARE` when possible (`:926`) | currently always Load+Store | **KK is better; adopt** |
| Same-draw feedback | impossible in Vulkan | H1 copy at bind | **ours only; no KK analogue** |
| Guest alias identity | Vulkan app supplies one view per image; aliasing is app-managed | C4/C5 epoch + canonical image | **ours only; no KK analogue** |
| Feedback-loop extension | `EXT_attachment_feedback_loop_layout` implemented as a layout, with optimisation disabled (`kk_image_layout.c:107`) | n/a | confirms there is no Metal primitive |

Two structural observations matter more than the table:

1. **KK's predicate is deliberately over-approximate, and its TODO is to make it
   less conservative** (`kk_cmd_buffer.c:395-406`: "track where the attachments lie
   in memory … to avoid splitting"). Our scheduler is the opposite: it consumes
   records and can drop edges, and L202/L203 were spent making the consumption
   correct. **That inversion is the defect class.** The right contract is KK's:
   the conservative answer is the default; the optimiser only removes work it can
   prove is unnecessary.

2. **KK makes correctness independent of the predicate** by ending *every* encoder
   with an `ALL→ALL` producer barrier. A missed split is then still ordered across
   the encoder boundary. We currently emit nothing in `close_encoder`
   (`MTLCommandRecorder.cpp:844`) and let the scheduler carry the edge. Moving the
   guarantee into the recorder is the cheapest reliability win in the whole plan.

The residual gap to KK is not design taste; it is **RSX semantics**:

- RSX permits a draw to sample the surface it renders (Vulkan forbids it), so no
  Vulkan driver — KK included — has code for H1. The copy stays.
- RSX aliases guest bytes freely (Vulkan's API forbids aliasing without explicit
  app barriers), so no Vulkan driver has an epoch/identity model. C4/C5 stay.
- RSX has no render-pass object, so load/store actions must be synthesised; KK gets
  them from `VkRenderPass`/dynamic rendering. Our always-Load+Store is a port
  regression, not a design choice.

Verdict: roughly **80% the same architecture**, with the remaining 20% forced by
RSX and not covered by KK at all. The three things worth lifting verbatim are the
lazy long-lived encoder, the unconditional encoder-end `ALL→ALL` barrier, and
per-attachment store-op minimisation. The two things that must stay ours are the
same-clause feedback copy and the guest-alias identity model.

### 13.1 Decision — adopt KK's three advantages, in this order

| # | KK practice | Why this position |
|---|---|---|
| 1 | **Long-lived render encoder** (lazy `cs_get_render` equivalent) | Biggest win, and a prerequisite: it is what makes 2 and 3 cheap |
| 2 | **Unconditional encoder-end `ALL→ALL`** (recorder-level, `close_encoder`) | The correctness backstop: the scheduler can no longer make the backend wrong. Affordable only after 1 (371 → ~15 barriers/frame) |
| 3 | **Per-attachment store-op minimisation** | Pays per pass; batching removes most of the redundant stores that make it attractive today. Do it last, and only with per-surface "will be read again" tracking |

Deliberately **not** adopted from KK: `ResourceAlias` as the default visibility (its
own TODO says it is overkill outside occlusion queries — use `Device`, promote
`ResourceAlias` only on measured alias cases); the coarse split predicate (ours is
finer and KK's TODO is exactly our C3/C5); the blanket `Vertex→Fragment` intra-pass
barrier (ours is more precise because a draw batch has no non-attachment writes).

The RSX-only two are not alternatives to this list. They are the parts KK cannot
supply: same-clause feedback copy and guest-alias epochs.
