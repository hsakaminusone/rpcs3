# Prompt for the next agent — Metal 4 RSX backend, Ridge Racer 7

You are continuing an in-progress investigation inside `/Users/abhakash/PersonalWS/rpcs3` (branch
`metal4`). Read this whole file before touching anything. It is deliberately explicit: several past
sessions lost runs to mistakes described in §7.

---

## 1. Mission

Reach **RSX parity** for the Metal 4 backend on **Ridge Racer 7 (`BLUS30001`)**. "Parity" means the
Vulkan/OpenGL reference output, not "it looks plausible". The visible defects are:

1. **Black video/FMV** — intro, course-select and in-race videos are black. **This is the current
   priority.**
2. **Black car** — the car is drawn as a correct silhouette, entirely black.
3. **Black Xevious mini-game panel** — buttons answer with audio, panel is black.
4. **Presentation** — the presented surface holds three panels where a screenshot holds one.

The FIFO desync is **parked by the user** and must not be worked on until the picture is fixed, even
though it still occurs.

Approach: root-cause, evidence first, one discriminating measurement at a time. No speculative
rewrites.

---

## 2. Read these first, in this order

| file | why |
|---|---|
| `docs/research/metal4/design/HANDOFF.md` | current state, build loop, rules, continuing instructions |
| `docs/research/metal4/design/LOG-FINDINGS.md` | **the facts**. Open items are §1; `L<n>` ids are stable and cited everywhere |
| `docs/research/metal4/design/METAL4-TODO.md` | live work list and row states |
| `docs/research/metal4/design/INVESTIGATION-2026-10-22.md` | the recent investigation, especially §10.2 (the five fixes), §10.6 (run 2), §10.7 (next experiment) |
| `docs/research/metal4/design/REVIEW-RSX-NATIVE.md` | the design rule the fixes must satisfy |
| `docs/research/metal4/design/RR7-FREEZE-FORENSICS.md` | freeze evidence (parked; do not act) |

`NON-METAL-FINDINGS.md`, `CONFLICTS.md`, `REVIEW-FINDINGS.md` and the `HLD-*`/`LLD-*` documents hold
supporting detail. Do not re-derive what is already measured; cite the id.

---

## 3. Non-negotiable rules

1. **RSX is the specification.** Vulkan and OpenGL are evidence of RSX behaviour; they are not the
   design. Use the native Metal 4 mechanism (Apple docs, KosmicKrisp). No hacks, wrappers,
   compatibility aliases, or "make the test pass" special cases.
2. **Metal 4 APIs only.** Do not introduce older Metal APIs, and do not port Vulkan code literally.
3. **Temporary code is marked.** Every diagnostic or temporary change carries `TODO(excise)` (or
   `TODO(excise) <reason>`). Diagnostics must be removable at zero cost: compile-time
   `MTL_DEBUG_LOGS` / `MTL_PERF_ENABLED`, runtime `MTL_PROBE`.
4. **Diagnostics go through the probe facility**, not `rsx_log` in shared code. Levels: `1` counters,
   `2` per event, `3` per event with detail, `5` per draw + dumps. Per-draw text starts at 3 and is
   new at 5; prefer latches/counters/histograms. A probe output line must name its id.
5. **Bounded capture.** A window is capped at **64 draws** (`diagnostic_window_probe_limit`), reset per
   window by `diagnostic_reset_window_state()`. Violating this wedged the queue and cost a run (§7).
6. **Never run the emulator without explicit user approval.** One instance at a time. `pgrep -f
   rpcs3` before and after; kill leftovers. The user boots the game from the GUI — there is no
   `build/bin/rpcs3` executable and the assistant does not know the disc path.
7. **A shader-changing switch gets its own archive suffix.** Current suffixes: `-noalpha`,
   `-forcefp`. A diagnostic run must never read or overwrite the real pipeline archive.
8. **Docs are part of the deliverable.** Keep `HANDOFF.md`, `METAL4-TODO.md`, `LOG-FINDINGS.md` and
   `INVESTIGATION-2026-10-22.md` current. Open items first; fold or compress resolved material rather
   than appending forever. `L` ids are permanent.
9. **Commit only after a fix is confirmed on device.** Identity: `hsaka -1.0
   <340957754+hsakaminusone@users.noreply.github.com>`. Nothing from this investigation is committed
   yet.

---

## 4. Where the work stands

### 4.1 Working tree (uncommitted)

Five correctness fixes, all built and covered by the existing suites (203/203 MTL, 499/499 full):

| fix | what it was |
|---|---|
| `MTLGSRender::begin()` framebuffer prep | `begin()` never prepared the framebuffer, so the first draws of a frame were refused (63 consecutive at one point) |
| `bind_framebuffer()` fast path | must test context, `rtt_config_dirty`, `attachments.valid()` and `ignore_change`, as VK/GL do |
| `argument_binder::address()` | Metal 4 snapshots the argument table at encode time; an unwritten slot kept the previous draw's binding. Writes every change, zero included |
| FIFO retry rewind (`RSXFIFO.cpp`) | on the empty first-argument path the position stayed on the argument, so the retry read the second argument as the first |
| `fetch_u32()` word-cache store | the control-command shortcut set cache bounds without storing the word, serving a stale word |

Plus the diagnostics: per-draw colour patches, `MTL_PROBE_NO_ALPHA_TEST` (own archive), the removed
depth probe, the 64-draw window cap, and `MTL_PROBE_FORCE_FP` (own archive).

### 4.2 What has been proven about the video (do not re-test)

- The video is **three B8 planes** (1280×720 luma + two 640×360 chroma) and they **carry picture**:
  luma 230 400/230 400 non-zero, 71 distinct bytes. Upload and decode are fine (`L95`).
- `B8 → R8Unorm` with view swizzle `{ONE, RED, RED, RED}` matches Vulkan (`L96`).
- The video draw is **frame 879, draw 12262, `vp 50 / fp 100`** in run 2: full-screen quad, correct
  scissor/clip/cull, blend `770/771` (SRC_COLOR / ONE_MINUS_SRC_COLOR), clear `(0,0,0,0)` (`L101`).
- **The alpha test is excluded**: run 2 compiled it out of every program and all four colour patches
  after the draw were still exactly `0x00000000`, `distinct 1` (`L110`).
- The two surviving explanations are indistinguishable from colour: the fragment **wrote
  `(0,0,0,0)`**, or **no fragment of that program ran** (`L111`).
- The depth-probe idea failed: 2 924 lines of garbage, no valid depth for either depth format. It is
  removed; **do not rebuild it the same way** (`L112`).
- The run-2 freeze was **the capture**, not the backend: the 670-draw race-start window did eight
  synchronising readbacks per draw, lag rose to 94.8 s, and 588 `MTL4CommandQueueErrorDomain`
  feedback failures followed. Fixed by the 64-draw cap (`L113`).

### 4.3 What is pending, in order

1. **P0 — the video discriminator.** One approved run with `MTL_PROBE_FORCE_FP=100`; read the patch
   transition. Exact command below.
2. **The car.** L99 must be settled first (three panels vs one). The run-1 car probes were on
   background geometry (`L105`). Do not reduce the car to "one missing texture": the paint texture is
   one repeated BC1 block and is copied faithfully (`L97`) — the question is what the shader does with
   it.
3. **The Xevious panel.** Order: does its own draw write (forced output), is its RTT cleared or
   overwritten before the sample, is the sampled surface invalidated when the guest writes it.
4. **Presentation** (`L14`, `L31`, `L40`): 10.1 % of flips present a clear, and the video surface
   comes from a path with only 2 readbacks in 300 frames.
5. **Performance** (`L38`, `L48`): the label drain is 57.9 ms/frame against an 8.68 ms GPU frame;
   the native fix is a GPU-side label write. Do not start this before the picture.
6. **FIFO** — parked (`L115`); do not spend a run on it.

---

## 5. The next experiment, exactly

Build first. `ninja rpcs3_emu` is enough for Metal changes; **`ninja rpcs3` is required before a
launch** (it links the app bundle; the binary lives at
`build/bin/rpcs3.app/Contents/MacOS/rpcs3`). The tree is already built and linked, including
`MTL_PROBE_FORCE_FP`.

Ask the user for approval, then run **one instance**, with this environment, from `build/bin`:

```sh
cd build/bin && \
MTL_PROBE=5 \
MTL_PROBE_FORCE_FP=100 \
MTL_PROBE_LOG=/tmp/mtl4-forcefp.log \
MTL_PROBE_SITE_CAP=20000 \
MTL_PROBE_RUN_BUDGET=600000 \
MTL_PROBE_INTERVAL_BUDGET=120000 \
MTL_PROBE_PIXELS='120,400,200,180;480,400,180,140;660,420,180,140;520,560,240,120' \
./rpcs3.app/Contents/MacOS/rpcs3
```

Wait for the intro video (the first window arms on the video-plane bind, ~47 s in run 2). Then quit
and kill leftovers. **Preserve** `/tmp/mtl4-forcefp*.log`, `/tmp/mtl4-dump/` and
`archive-…-forcefp.bin` before the next run.

Interpretation:

| the video patch after draw 12262 | conclusion | next step |
|---|---|---|
| magenta | the draw reaches the write stage; attachment, pipeline, raster and viewport are sound | Test B: sample-only output (or `MTL_PROBE_REPLACE_TEXTURES` for the coarse answer) to separate the bound view from the YUV math |
| unchanged `(0,0,0,0)` | no fragment of that program wrote | raster/clip side: the vertex stream and which pass the draw lands in. The state fields already look right, so do not re-audit them first |

Two cautions:

- Read the **transition** after the target draw, not the end-of-frame value; later draws in the same
  frame repaint the patches.
- Program ids are compile-order artefacts. Confirm from the log that `fp 100` is the video draw
  before trusting the id; if the id moved, use the printed id of draw 12262's program and rebuild
  only if the switch has to be re-aimed.

If magenta appears and Test B is needed: `MTL_PROBE_REPLACE_TEXTURES=1 MTL_PROBE_REPLACE_UNITS=n`
binds a one-texel red image to every referenced unit at bind time (no rebuild). It answers "is
sampling wired up at all", **not** "did this draw write" — every draw sampling that unit turns red,
so it cannot isolate one draw inside a frame (`L91`).

---

## 6. Method that has worked (and why)

- **Make the effect unmistakable instead of inferring from absence.** The colour probe could not
  distinguish black from nothing; the forced magenta output can. When a measurement is ambiguous,
  add a positive marker rather than a second negative one.
- **Instrument at the narrowest scope that still isolates the draw.** Global overrides are useless in
  a frame whose other draws already paint; target one compiled program.
- **Every diagnostic has a cost.** Four patches × two attachments per draw × 670 draws wedged the
  queue. Cap the window, count the copies, and check `start lag` in the same log.
- **Readback hazards are real.** Use `vm::get_super_ptr` for guest memory in diagnostics; `vm::read32`
  faults a `PROT_NONE` page and flushes it, perturbing the frame (`L92`).
- **Validate the observation path before trusting a negative.** The depth probe returned garbage and
  would have produced a confident wrong answer. If a probe's numbers look impossible (`min -3.7e19`),
  stop and fix the probe, do not interpret it.
- **Keep the reference honest.** When a Metal behaviour looks wrong, check Vulkan and OpenGL first:
  several "Metal bugs" were shared or upstream (`L43a`–`L46`).
- **The five fixes are the floor.** The current binary contains them; a regression to the old
  symptoms (draws refused, stale bindings) means one was lost.

---

## 7. Traps that cost runs — read before every run

1. **Never run without approval**, never two instances. Check `pgrep -f rpcs3` before and after; kill
   leftovers (`pkill -f 'rpcs3.app/Contents/MacOS/rpcs3'`), wait, then proceed.
2. **The pipeline archive hides shader dumps.** Move `archive-…-<suffix>.bin` aside before a capture
   run; the dump limit is 512 (`L93`). Archive path:
   `~/Library/Caches/rpcs3/cache/BLUS30001/ppu-oLi0iYdyVSU7WCRo9VaF5kpZVP71-EBOOT.BIN/shaders_cache/metal/archive-10000033e-27[-noalpha|-forcefp].bin`.
3. **`MTL_SHADER_VALIDATION=1` crashes Apple's Metal 4 debug compiler.** Use
   `MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog` instead (`L78`).
4. **A failed commit is a feedback failure, not a `commit()` failure.** Look at
   `MTLCommandRecorder.cpp` `on_feedback` (~line 475); a failed commit leaves the frame ring stalled.
   Run 2's 588 failures all came from the oversized capture window.
5. **Log volume.** Run 1 was 183 919 lines; run 2 was 16.4 MB. Always `MTL_PROBE_LOG` to a named file
   and grep it; never read it whole. The bulk probe text normally goes to `$TMPDIR/rpcs3-mtl-probe.log`.
6. **The game boots from the GUI.** Do not try to find an EBOOT; `dev_bdvd` is empty and the disc
   path is the user's. The user starts the run; you supply the env and the interpretation.
7. **`USE_PRECOMPILED_HEADERS` does nothing for Qt** (`rpcs3_ui` has no PCH). Do not propose it as a
   build-speed fix.
8. **Teardown segfaults on quit** (`SYS: Segfault … Deleting old game window`) are the known LLVM/JIT
   class (`L2`/I6), not a new bug, and not a reason to distrust the run data.
9. **Low Power Mode / battery** was on for earlier measurements; timings are lower bounds (`L61`).
10. **Do not "fix" the FIFO** while it is parked, and do not report it as fixed: it still occurs with
    both fixes in (`L115`).

---

## 8. Build and run loop

```sh
cd /Users/abhakash/PersonalWS/rpcs3/build
ninja rpcs3_emu          # Metal iteration, ~1 min, no Qt
ninja rpcs3              # before ANY launch: links the app bundle
```

Verify `build/bin/rpcs3.app/Contents/MacOS/rpcs3` mtime after linking, and that
`pgrep -f 'ninja rpcs3'` is empty before launching. Run from `build/bin` so the app finds its
plugins.

---

## 9. Definition of done for the current phase

- The video discriminator is answered and written into `LOG-FINDINGS.md` (new `L` id) and
  `INVESTIGATION-2026-10-22.md` §10.8.
- Whichever branch is taken is followed to a root cause with a measurement, then a fix that is native
  Metal 4 and matches RSX behaviour.
- The fix is confirmed on device with the same bounded probe methodology, and the car/panel work is
  re-planned from the answer.
- Diagnostics that were only for this phase are excised (or kept behind `MTL_DEBUG_LOGS`/`MTL_PROBE`
  with `TODO(excise)`), the suites pass, and only then is anything committed as `hsaka -1.0`.
- Docs updated with open items first and resolved material compressed.

## 10. First 30 minutes checklist

1. Read §2 files, especially `LOG-FINDINGS.md` §1.1 and `INVESTIGATION-2026-10-22.md` §10.6–§10.7.
2. `git status` and `git diff --stat`: confirm the five fixes and the diagnostics are still there and
   still uncommitted.
3. Check the binary mtime; rebuild `rpcs3_emu` and `rpcs3` if the tree changed.
4. Confirm the forced-output switch is present (`grep -rn MTL_PROBE_FORCE_FP rpcs3/Emu/RSX/MTL`) and
   that `_FORCE_OUTPUT` is guarded out by default.
5. Ask the user for approval for exactly one run (§5), and confirm the game is at the intro video.
6. After the run: kill leftovers, preserve logs, answer the discriminator, update docs, then decide the
   next single experiment. Do not chain experiments in one session.
