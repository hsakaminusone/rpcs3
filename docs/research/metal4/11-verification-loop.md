# 11. The verification loop: RRC replay harness, differential goldens & agent iteration

Owner/scope: Scope 11 (cross-cutting, implementation-facing) | Status: draft | Date: 2026-10-15

> **Scope.** Scope 07 asks *"how would we know the backend is correct, and fast enough"* and answers
> with an oracle ladder (§2), an automatable conformance ladder (§3), a determinism/divergence
> taxonomy (§4) and a three-tier CI plan (§9). **This document does not re-derive any of that.** It
> answers the next question: *what harness do we actually build, and what loop do we run against it,
> so that implementation can iterate toward correctness — including autonomous iteration, where each
> change must be validated in minutes, not weeks?*
>
> The single contribution here that 07 does not contain is the mechanism that makes the loop
> **runnable without a GPU** and **localisable per command**: a recording seam over the backend's
> decision points, and the fact that the RRC replay substrate is already **backend-agnostic** and
> **per-command annotated**.
>
> **Read-only research.** No RPCS3 source, CMake, test or `.ci` file was modified and no build was
> run to produce this note. All `file:line` citations were read on 2026-10-15 on branch `metal4`; the
> tree is point-in-time and another agent is editing `rpcs3/Emu/RSX/VK/**` and
> `docs/research/metal4/10-*.md` concurrently, so **symbol names are the stable reference; line
> numbers are advisory**.
>
> **Legend:** `[V]` = verified against a primary source (the RPCS3 tree, a pristine reference under
> `/tmp/metal4-refs/`, or a cited vendor doc reached in this session); `[I]` = inference / engineering
> judgement — **all harness designs below are `[I]`** unless a citation says otherwise; `[U]` = could
> not verify. Effort ratings reuse the scale fixed by the sibling docs: **1** trivial · **2**
> mechanical · **3** real work, well-trodden · **4** hard / needs design · **5** unknown.

---

## 1. Scope & relationship to 07 and 10

| Doc | Owns | Relationship |
|---|---|---|
| **07** | *Conformance theory*: the oracle ladder (rungs 0–5), the conformance levels (L0–L4), determinism taxonomy, debug tooling, perf model, CI tiers A/B/C. | This doc **consumes** it. Where 07 states a rung or a level, this doc says *which artifact implements it* and *what the loop does with it*. |
| **10** | *Architecture*: the RSX-native Metal 4 backend design. | This doc **constrains** it: §9 states the verifiability requirements that must be a stated design requirement in 10. |
| **11 (this)** | *The executable harness*: the build list, the differential-golden generator, the per-command localiser, and the fast/slow iteration loop. | — |

Three things this doc adds that 07 leaves at the level of principle:

1. **The RRC substrate is backend-agnostic and per-command annotated** (§3) — so the replay harness
   works *unchanged* for a future `MTL` backend and can localise a divergence to a single command.
2. **A recording seam** (§5, H2) that turns "run it and look at the picture" into "diff the backend's
   *decisions*" — GPU-free, deterministic, and the only thing that lets an agent iterate unattended.
3. **The loop itself** (§7–§8): what runs per change, what gates a merge, what runs nightly, and what
   "green" means.

---

## 2. What "RSX compatibility" means — the testable contract

A backend is a *mechanism* for satisfying the `rsx::thread` contract; "compatibility" is
**guest-observable equivalence under a replay**, not visual similarity. It decomposes into three
layers of sharply different testability (07 §2.2 reaches the same split; this restates it as a
harness target):

| Layer | Contract | Oracle | Determinism | Testable? |
|---|---|---|---|---|
| **A. Guest memory** | The regions the RSX *writes* (render targets, textures, index/vertex results, DMA) reach equivalent contents | snapshot RSX address-space regions after replay; diff vs reference | high (float tolerance) | yes — strong |
| **B. Command semantics** | Same draws, same pass boundaries, same bindings, same program keys, correct ordering; **no FIFO desync, no access violation** | `frame_debug` trace (§3); decision trace (§5); desync detector (§4.1) | exact | yes — strongest |
| **C. Host image** | The presented frame looks equivalent within tolerance | screenshot readback + tolerant diff | low | yes — weakest |

**The ordering is deliberate: C is the signal people reach for first and the one that should gate
least.** 07 §2.2 is explicit that *only* rung 3 (guest-observable) is objectively about RSX; B and A
above are the enforcement of that, and C is a proxy that inherits TBDR/MSAA/rounding differences
(README §6 R16). **A loop that optimises C will chase driver nondeterminism.**

The practical consequence: **the loop's primary signals are A and B, both of which can be made
deterministic; C is advisory and tolerance-gated.**

---

## 3. The substrate that already exists (and why it is the right foundation)

This is the most important empirical fact in the document: **capture and replay are implemented in
backend-agnostic core, not in `Emu/RSX/VK/`.** A future `MTL` backend inherits the entire mechanism
for free.

### 3.1 Capture hooks live in core `[V]`

The capture entry points are called from shared RSX code, never from the Vulkan backend:

| Hook | Called from |
|---|---|
| `capture::capture_draw_memory(this)` | `RSXThread.cpp:975` |
| `capture::capture_display_tile_state(this, …)` | `RSXThread.cpp:3478` |
| `capture::capture_image_in(this, …)` | `RSXFIFO.cpp:833` |
| `capture::capture_buffer_notify(this, …)` | `RSXFIFO.cpp:836` |

(`rsx_capture.h` declares exactly these four.) The producers are core too: `rsx::thread::capture_frame`
(`RSXThread.cpp:915`), the per-draw auto-capture at `RSXThread.cpp:994`, and the `.rrc.gz` writer at
`RSXThread.cpp:3484`; NV47 handlers arm it via the `RSX_CAPTURE_EVENT` macro (`nv4097.cpp:11`,
`context_accessors.define.h:3`). **No backend participates.** `[V]`

### 3.2 The capture is a *state-annotated* command stream `[V]`

`rsx::frame_capture_data` (`rsx_replay.h`; magic `"RRC"`, version `0x6`) is not a raw FIFO dump. Per
command it records (`replay_command`, `rsx_replay.h`):

```
rsx_command            // the (u32,u32) FIFO command
memory_state           // set of indices into memory_map: blocks to apply BEFORE this command
tile_state             // index into tile_map: RSX tile+zcull state at this command
display_buffer_state   // index into display_buffers_map
```

Supporting structures: `memory_block{offset, location, data_state}` (offset in **RSX address
space**), `tile_state{tile_info[15], zcull_info[8]}`, `display_buffers_state{buffer_state[8], count}`,
the interning maps (`memory_map`/`memory_data_map`/`tile_map`/`display_buffers_map`, deduped by a
`bitwise_hasher`), and `reg_state` (the full `rsx::rsx_state` at capture start). `[V]`

**This is the localisation substrate.** Every annotated command is a labelled point in the stream, so
"the frame is wrong" can be reduced to "command *k* produced the wrong state" — a unit an
implementer (or an agent) can fix directly.

### 3.3 Replay is stop-and-apply — i.e. already a determinisation harness `[V]`

`rsx_replay_thread::cpu_task` (`rsx_replay.cpp:174`; helpers `allocate_context` `:16`,
`alloc_write_fifo` `:59`, `apply_frame_state` `:104`) does *not* just fire the stream at the GPU. For
every command that carries a state change it:

1. waits until the RSX FIFO is **idle at the current stop** (`render->is_fifo_idle()`);
2. applies the captured memory/tile/display state (`apply_frame_state`);
3. advances the FIFO `put` pointer to the next stop.

So the replay is **stop-and-apply**: the device is quiesced at each annotated command. That is exactly
the synchronisation point a differential harness needs, and it is already in the tree. At the end, if
the capture contained no flip, the replay performs one manually (`rsx_replay.cpp`, tail of
`cpu_task`). `[V]`

### 3.4 The launch and observability seams already exist `[V]`

- **Launch:** `--rsx-capture <path>` with `--headless`/`--no-gui` (07 §2.3, `rpcs3.cpp:414,843,1228-1240`,
  `:391,448`); GUI equivalent "Select RSX Capture" (`main_window.cpp:742`, suffix check `:4341`);
  replay thread constructed at `Emu/System.cpp:971` from the capture built at `:870`.
- **Structural trace:** `rsx::frame_trace_data` (`rsx_trace.h:11-32`) records a `command_queue` of
  `(reg, value)` and a `draw_calls` vector of `draw_state{name, programs, rsx_state, colour/depth
  buffers, index, vertex_count}`, global instance `frame_debug` (`RSXThread.cpp:46`,
  `RSXThread.h:36`), armed by `capture_current_frame` (`RSXThread.cpp:921`, `RSXFIFO.cpp:819`), reset
  at frame start (`RSXThread.cpp:3468`). This is the **rung-2 oracle** (07 §2.3) and it is
  RSX-level, so it survives rasteriser differences. `[V]`
- **Pixel seam:** `g_user_asked_for_screenshot` (`RSXThread.cpp:43`) → readback (`VKPresent.cpp:662-779`,
  `GLPresent.cpp:331-387`) → virtual `take_screenshot(std::vector<u8>&&, w, h, is_bgra)`
  (`GSFrameBase.h:35`). A native backend must implement this seam; it is also what gives the harness
  an image to hash. `[V]`
- **Perf/stat oracle:** `frame_statistics_t` (`RSXDisplay.h:50-68`), `m_frame_stats`
  (`RSXThread.h:162`), reset `RSXThread.cpp:3568`, queued with the flip `:3524` (07 §2.3). `[V]`
- **Failure alarm:** FIFO desync detection — `FIFO::FIFO_ERROR` logs *"possible desync event"* and
  calls `recover_fifo()` (`RSXFIFO.cpp:710`); history in `recovered_fifo_cmds_history`
  (`RSXThread.h:328`, type `desync_fifo_cmd_info` `:100`). `[V]`
- **Invalidation oracle:** the RSX host memory-manager deferred-mprotect queue (`rsx::MM`,
  `Emu/RSX/Host/MM.cpp`) is backend-agnostic and **already unit-tested** (`test_rsx_mm_queue.cpp`, 138
  lines, `g_deferred_mprotect_queue` / `mm_protect` / `mm_flush`). A `MTL` backend must drive the same
  queue; any divergence here is catchable in a GPU-free test. `[V]`

**Verdict:** the pieces 07 §3.3 calls "the key enabler" all exist and are backend-agnostic. The new
work is **glue, a comparison tool, and one new seam (§5)** — not infrastructure. `[I]`

---

## 4. The oracle ladder in executable form

07 defines rungs 0–5 and levels L0–L4. Below is the harness layer that implements each, with the
information the build plan needs. **Harness layers `H*` are this doc's; the 07 column maps them.**

| Harness | Implements 07 | GPU? | Artifact | Exact? | Catches |
|---|---|---|---|---|---|
| **H0** unit logic | §3.1 L0 (rung 0/1 pre-req) | no | `rpcs3/tests/test_mtl_*.cpp` | exact | workaround/feature tables; format map; argument-table index math; cache keys |
| **H1** shader-translate | §3.1 L0 extension | **no** | corpus → `GLSL→SPIR-V→SPIRV-Cross MSL→xcrun metal` | exact (compile) | the *entire* shader-translation class — the README's make-or-break |
| **H2** decision/encoder golden | rung 2 (new mechanism, §5) | no\* | recording seam + golden decision traces | exact | wrong bindings, wrong pass structure, wrong draw encoding, wrong barrier placement |
| **H3** per-command state oracle | rung 2 + RRC annotations (§3.2) | no\* | replay + `frame_debug`/tile/display-state diff | exact | state-machine divergence, localised to a command index |
| **H4** validation gate | §3.2 L1 (rungs 0–1) | yes | `MTL_DEBUG_LAYER=1`, `MTL_SHADER_VALIDATION=1` | exact | illegal API use, shader OOB/nil/stack |
| **H5** guest-memory differential | rung 3 (§2 layer A) | yes | region snapshot + diff | tolerant | real rendering semantics (the *only* RSX-objective rung) |
| **H6** image differential | §3.4 L3 (rung 4) | yes | screenshot + tolerant diff + manifest | tolerant | visual regressions |
| **H7** soak + perf | §4 / §8 (rung 5) | yes | desync counter; `frame_statistics_t` A/B | exact / band | hazard & invalidation bugs; perf regressions |

\* **H2/H3 are GPU-free only in the sense that they need no device to *diff*** — but running the
replay still constructs a backend, which needs a `MTLDevice`. Two ways to make them truly
Tier-A-ready (hosted CI): (a) a **null/recording device** behind the backend's own seam (§5.2); or
(b) accept that they run on the self-hosted Mac. **(a) is the accelerator; (b) is the baseline.** `[I]`

Two consequences worth stating plainly:

- **H1 is the highest-value GPU-free test in the set.** The shader path (`ucode→GLSL→SPIR-V→MSL`) is
  the documented make-or-break (README §2 verdict 2), and every shader in the corpus can be compiled
  to MSL with `xcrun metal` on any Mac or hosted runner that has the toolchain — no GPU, no game,
  seconds per shader. No other single test covers as much risk for as little cost.
- **H2–H3 are what make the loop autonomous.** They are exact, deterministic and fast, and they
  localise. H5–H7 need a self-hosted runner (07 §9.4: GitHub-hosted macOS runners have no `MTLDevice`)
  and run nightly.

---

## 5. The missing piece: a recording seam (H2/H3)

07's rung 2 asserts structural equivalence via `frame_debug` + capture replay, but does not say *how
the backend's own decisions get recorded*. This is the gap that decides whether the loop can run
unattended.

### 5.1 Record decisions, not API calls `[I]`

The tempting design — wrap the metal-cpp boundary and record every `MTL4*` call — is fragile:
metal-cpp is a header-only C++ façade over Objective-C message sends, and a faithful re-recording
means reimplementing the whole façade. **Do not do that.**

The design that works: the backend records **its own decision points**, as structured records, at the
places where correctness lives:

- pass begin/end and attachment load/store actions (the pass-boundary decision);
- every barrier emitted, with the two stage sets and the visibility option;
- every binding applied to the argument table (address/texture/sampler slot + value id);
- every draw: primitive, vertex/index counts, pipeline key, argument-table snapshot id;
- every copy/fill/resolve and its source/destination;
- residency commits and removals.

These are exactly the points 07 §2.4 says `frame_debug` can key against, and exactly the points the
RSX-native design (doc 10) makes *explicit* by construction. The record is a **struct per decision**
written to a binary/text trace, and the harness diffs two traces.

### 5.2 The null device `[I]`

For H2 to run on hosted CI (no `MTLDevice`), the backend needs a mode where the decision path runs
but the device is a null object: encode-and-record, never commit. This is a mechanical consequence of
routing all device access through the backend's own seam, and it is the single artifact that would
let the *structural* half of the loop run in Tier A. Effort 3. If skipped, H2/H3 are Tier B — still
correct, just not hosted.

### 5.3 What this buys

| Without the seam | With the seam |
|---|---|
| "the picture is wrong" | "command 4731 emitted a barrier missing `.fragment` before `.tile`" |
| bisect by bisecting *games* | bisect by *command index* in one replay |
| needs a GPU to observe | needs a GPU only to *render*; diffing is offline |
| human judgement per iteration | machine-checkable per iteration |

---

## 6. Differential goldens: how the reference is produced

RPCS3 has **no golden-image GPU suite today** (07 §3.4 `[V]`) — so the loop must *create* one. It does
not have to invent the reference: **the reference is the backend that already ships** (MoltenVK or
KosmicKrisp). Differential generation, not hand-authoring, is what makes the corpus bootstrap-able.

```
                    ┌─────────────── corpus/N.rrc.gz ───────────────┐
                    │                                                │
        reference run (vulkan/MVK or KK)              candidate run (metal)
                    │                                                │
        ┌───────────┼───────────┐                    ┌───────────┼───────────┐
        ▼           ▼           ▼                    ▼           ▼           ▼
   frame_debug  decision    region snapshots     frame_debug  decision   region snapshots
     trace        trace      + screenshot          trace        trace     + screenshot
        │           │           │                    │           │           │
        └───────────┴─────► GOLDENS/ ◄───────────────┴───────────┴───────────┘
                               │
                    diff (exact for B, tolerant for A/C)
                               │
                       first divergent command index ──► localise ──► fix
```

**Manifest per golden** (`[I]`): capture hash + RRC version; backend id + version; GPU family; OS
build; Xcode/SDK build; the tolerance bucket set; and a `rebaseline_required` flag.

**Generation rules** (`[I]`):

1. Generate goldens **only** from the shipping backend on the *same machine* that will run the
   candidate — cross-machine goldens are invalid (07 §3.4, §9.3).
2. Store **raw artifacts** (trace + image + region hashes), never only a pass/fail bit, so a
   re-baseline is a diff, not a mystery.
3. A device/OS change marks goldens **stale**, not failed (07 §3.4; the GitHub mobile Intel→M1
   re-record precedent).
4. **Never** assert cross-backend bit equality on layer A or C. Exactness is reserved for layer B and
   H0–H4.

**Tolerance policy** (`[I]`, consistent with 07 §3.4/§4.3): bucket by region — RSX-observable
(readback targets, ZCULL) tight; host-presented loose; use per-region max/mean absolute error plus a
perceptual metric, and record the bucket set in the manifest so a tightened bucket is a deliberate
change.

---

## 7. Localisation: turning a failure into a fixable unit

The loop's throughput is set by **how fast a failure becomes a specific command**. The substrate
already supports this:

1. **Structural divergence (H2/H3, offline).** Diff the decision traces / `frame_debug` draw-call
   vectors element-wise. The first differing element carries a command index (RRC is per-command
   annotated, §3.2) and a decision kind. **This is usually the whole diagnosis.**
2. **Per-command replay (H3/H5, on device).** Because replay is stop-and-apply (§3.3), the harness
   can replay to command *k*, snapshot the RSX-visible regions, and compare — bisecting the stream
   in `O(log n)` replays, or `O(n)` with per-command snapshots.
3. **Offline GPU debug (H4/H6 escalation).** Emit a `.gputrace` and a `frame_debug` dump as CI
   artifacts; drive `gpudebug` non-interactively (`gpudebug -s <session> -c "…"`), which Apple
   documents as sufficient for autonomous investigation (07 §3.5/§5.2 `[V for the tool]`).

**Rule:** every failing test must emit (a) the first divergent command index and (b) the artifacts
that let a developer or agent inspect it without reproducing on interactive hardware. A test that
reports only "image differs" is a test that will be ignored. `[I]`

---

## 8. The loop: what runs when

07 §9.4 fixes the CI tiers (A hosted / B self-hosted per-PR / C self-hosted nightly) on the verified
fact that hosted macOS runners have no `MTLDevice`. This section says **what the loop does with each
harness layer** in those tiers.

| When | Where | Runs | Gate |
|---|---|---|---|
| **Per change (fast)** | dev machine / Tier B | build → **H0 + H1** (GPU-free, seconds) → **H2 + H3** on one or two capture(s) | must be exact-green before commit |
| **Per PR** | Tier A (hosted) | build + **H0 + H1** (both GPU-free) + skip-guarded stubs | required |
| **Per PR (Metal-touching)** | Tier B (self-hosted Apple silicon) | **H4** validation smoke + **H6** small subset + **H2/H3** full corpus | required for `MTL/**` |
| **Nightly** | Tier C | **H3 + H5 + H7** full corpus; perf baselines; `.gputrace` on failure | non-blocking, tracked |
| **On failure** | anywhere | localise (§7) → artifacts → offline `gpudebug` | — |

**Definition of "green" for the loop** (`[I]`):

- H0, H1, H2, H3 pass **exactly**;
- H4 reports **no** validation errors;
- H5 within region tolerance; H6 within bucket tolerance;
- **zero new FIFO desyncs / access violations** (the built-in alarm, §3.4) on the replay corpus;
- H7 performance within the documented noise band (07 §9.3).

**On a single scalar.** For fully autonomous optimisation you need a number to move, and the honest
one is *"fraction of the corpus passing at the strictest layer it can pass"*. Emit it, **but treat it
as a smoke alarm, not a fitness function** — a scalar can be gamed (e.g. degrading tolerance buckets
or skipping captures), and the moment it is optimised directly it stops measuring correctness. Hard
gates per class, with the scalar for trend-watching only, is the defensible design. `[I]`

**Re-anchoring (mandatory).** The proxies (H5/H6 tolerance, the desync counter) are *proxies* for
"it renders correctly", and the goal itself is not a number. Periodically **human-review a fixed
sample of the corpus** against the reference. A loop that never re-anchors will eventually pass all
its tests while looking wrong. This is the one step that cannot be automated away. `[I]`

---

## 9. Design-for-verifiability: a requirement on doc 10

The RSX-native design (doc 10) is, by construction, *more* verifiable than a Vulkan translation —
because it makes the right things explicit. The following should be stated as **architectural
requirements**, not test afterthoughts:

1. **Every hazard decision is explicit and recorded.** Metal 4 forces explicit barriers; the design
   must also force them to be *emitted through one chokepoint* that H2 can record. A barrier
   computed inline and handed straight to an encoder is invisible to the loop.
2. **Pass boundaries are derived and recordable.** The RSX-native pass model derives boundaries from
   surface hazards; that derivation must be a function whose inputs and outputs H2/H3 can diff — not
   an emergent property of encoder plumbing.
3. **Bindings go through one argument-table layer.** One place to record, one place to test the index
   math (H0).
4. **All device access is behind the backend's own seam** — the precondition for the null device
   (§5.2) and thus for hosted-CI structural testing.
5. **Determinism is a design property, not a runtime hope:** RSX thread scheduling, clock/vsync,
   frame count and allocator behaviour must be pinned in replay mode (07 §4.4 governs the policy) so
   H2/H3 are stable run-to-run.
6. **The capture corpus is a first-class deliverable**, versioned with the code, with the manifest of
   §6 — not an ad-hoc set of files someone had lying around.

**Captures must cover the quirk surface**, or the loop has blind spots: (a) trivial 2D/overlay;
(b) VP/FP-heavy; (c) RTT + MSAA resolve; (d) format/deswizzle stress; (e) ZCULL/occlusion-heavy;
(f) a >2000-draw/frame scene (for H7 and to exercise the FIFO flattener, `RSXFIFO.cpp:532`). `[I]`

---

## 10. Limits, failure modes & open questions

**Where the loop cannot help** (`[I]`, consistent with 07 §4):

- **Per-title quirks** (RTT layouts, spilling, format edge cases) are only as covered as the corpus.
  A weak corpus produces a green loop and a broken backend. Corpus quality *is* the harness.
- **Driver/toolchain bugs** (the KK workaround surface, README §6 R10) are not detectable as
  correctness failures — they present as wrong pixels with no reference disagreement, or as crashes.
  They need the workaround register and per-OS gates, not the loop.
- **Human judgement** for "looks right" is irreducible (§8, re-anchoring).
- **Harness bugs**: a differential harness that shares code with the candidate can hide a shared bug.
  Keep the reference path on the *shipping* backend and the comparison tool independent. `[I]`

**Open questions for the human:**

- **Q1.** Is the *null device* (§5.2) worth ~effort-3 to bring H2/H3 into hosted CI, or is Tier B
  self-hosted enough? (Recommendation: build it — it is the difference between a loop that runs
  everywhere and one that needs a specific Mac.) `[I]`
- **Q2.** Who owns the corpus, and what is the minimum set that gates a merge? A corpus that grows
  only when someone remembers is not a gate. `[I]`
- **Q3.** `[U]` Does the RRC format record enough state for a *pixel-complete* replay of every title
  (spilled surfaces, MSAA resolve history)? 07 §2.4 flags this as unresolved and says re-check before
  depending on it; **H5/H6 depend on it.** Resolve before promising image-level gates.
- **Q4.** Does the reference backend run *headless* well enough to generate goldens unattended
  (including on the self-hosted Mac)? Generation cost is the loop's floor. `[U]`

**Not-recommended:** building a bespoke in-process test framework before the RRC glue exists (the
substrate is already there, §3); and asserting any cross-backend bit equality (07 §2.4, §4).

---

## 11. Sources

### Primary — verified against this tree `[V]`

- `rpcs3/Emu/RSX/Capture/rsx_replay.h` — `frame_capture_data` (magic `"RRC"`, version `0x6`),
  `replay_command`, `memory_block`, `tile_state`/`tile_info`/`zcull_info`, `display_buffers_state`/
  `buffer_state`, interning maps, `reg_state`, `memory_indexer`; `rsx_replay_thread` + `cpu_task`.
- `rpcs3/Emu/RSX/Capture/rsx_replay.cpp` — `allocate_context:16`, `alloc_write_fifo:59`,
  `apply_frame_state:104`, `cpu_task:174` (stop-and-apply loop, manual flip tail).
- `rpcs3/Emu/RSX/Capture/rsx_capture.h` — `capture_draw_memory`, `capture_image_in`,
  `capture_buffer_notify`, `capture_display_tile_state`.
- `rpcs3/Emu/RSX/Capture/rsx_trace.h:11-32` — `frame_trace_data`, `draw_state`.
- `rpcs3/Emu/RSX/RSXThread.cpp` — capture hooks `:975`, `:3478`; `capture_frame:915`,`:994`; `.rrc.gz`
  writer `:3484`; `frame_debug` `:46`; arm `:921`; reset `:3468`; `g_user_asked_for_screenshot:43`;
  `m_frame_stats` reset `:3568`, queued `:3524`.
- `rpcs3/Emu/RSX/RSXFIFO.cpp` — `capture_image_in:833`, `capture_buffer_notify:836`; `frame_debug`
  population `:819`; desync detection `:710`; flattener threshold `:532`.
- `rpcs3/Emu/RSX/RSXThread.h` — `frame_debug` decl `:36`; `desync_fifo_cmd_info:100`;
  `recovered_fifo_cmds_history:328`; `m_frame_stats:162`.
- `rpcs3/Emu/RSX/Host/MM.cpp` + `rpcs3/tests/test_rsx_mm_queue.cpp` (138 L) — deferred-mprotect queue,
  `g_deferred_mprotect_queue`/`mm_protect`/`mm_flush`.
- `rpcs3/tests/` — `test_rsx_fp_asm.cpp` (761 L), `test_rsx_cfg.cpp` (279 L) as existing GPU-free RSX
  test patterns; `rpcs3_test` target `CMakeLists.txt:180-225`.
- `rpcs3/Emu/RSX/GSFrameBase.h:35` — `take_screenshot` seam; `rpcs3/Emu/RSX/Core/RSXDisplay.h:50-68`
  — `frame_statistics_t`.
- `rpcs3/Emu/System.cpp:870,971,2918` — replay thread construction/lookup; `rpcs3/rpcs3qt/main_window.cpp:742,4341`.

### Sibling scope `[V]`

- `docs/research/metal4/07-verification-and-performance.md` — oracle ladder §2.2, structural oracles
  §2.3, conformance levels §3, determinism §4, debug tooling §5, CI tiers §9.4 (this doc consumes all
  of it).
- `docs/research/metal4/README.md` §2 verdict 2 (shader path), §4 (barrier/residency facts), §6 R10
  (workaround surface), R16 (TBDR/MSAA divergence), R37 (cache namespace).
- `docs/research/metal4/09-draw-submission-and-batching.md` — draw-path metrics for H7.
