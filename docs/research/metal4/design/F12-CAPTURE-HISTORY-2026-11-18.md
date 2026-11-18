# What F12 does — the complete capture history

Date: 2026-11-18. Branch: `metal4-dev` (`9c8bb94d2`).
Scope: the Metal 4 backend only, with `MTL_DEBUG_LOGS=1` (the default) and
`MTL_PROBE=5`. Everything below is a reading of the code as it stands, not a
design proposal. The investigation this serves is
`DS-STROBE-INVESTIGATION.md` (live; read its §1 first).

The short answer: **F12 does not take a picture of the frame you just saw.** It
takes the ordinary PNG screenshot of the *next* present, and separately it
freezes two histories that were already being written — a 12-frame ring snapshot
in CPU memory and a rolling GPU pixel tape — and then dumps them plus a forward
burst. From the key event to the last byte on disk the chain is:

```
Qt key event
  → atomic flag
  → next present_frame()            (the flag is consumed here, not at the press)
  → arm + freeze                    (no evidence written yet, only decisions)
  → ordinary PNG capture             (the frame being presented)
  → shot / present / upscaled dumps  (GPU blit to a shared buffer, sync, hash)
  → tape flush                       (the N frames *before* the press)
  → compose, flip
  → ring dump                        (12 frames before + N after, CPU memory)
  → next frame(s): more images + rings
  → window close by frame index      (census report)
  → PNG delivered on slot completion
```

The important asymmetry, and the one that matters for Demon's Souls: **the
history before the press is real, but it is only kept while the probe is at
level 5 and only for the depth the ring and the tape were configured for.** The
defect the user photographed is the one *already in* those histories; the F12
frame itself is usually the first clean frame.

---

## 0. Preconditions that silently disable everything

| Condition | Where | Effect if absent |
|---|---|---|
| `MTL_PROBE=5` | `MTLInstrumentation.h:157` | `diagnostic_try_arm_capture()` returns at `MTLGSRender.cpp:352`; no window, no rings, no tape. |
| level 5 at the call site | `probe::at_least(5)` | The dump bodies, the tape copy and the per-draw records all no-op. |
| `MTL_DEBUG_LOGS=1` (compile time) | `MTLInstrumentation.h:118` | The `#if MTL_DEBUG_LOGS` present path is not compiled; `screenshot_requested` does not exist; F12 degrades to the release path. |
| `MTL_PROBE_TAPE>=8` | `MTLInstrumentation.h:489` | `diagnostic_tape_freeze()` returns at `MTLGSRender.cpp:2855`; no pre-press image timeline at all. |
| `MTL_PROBE_DUMP_DIR` writable | `MTLInstrumentation.h:432` | Default is `/tmp/mtl4-dump`; the machine purges it. |
| Unique `MTL_PROBE_LOG` | `MTLInstrumentation.h:361` | Default truncates on every run (`.../rpcs3-mtl-probe.log`). |

The one that has bitten this investigation: **`MTL_PROBE` below 5 still takes an
F12 screenshot**, so a run "with F12" can look successful while producing no
evidence. The press itself is not an error; the arming is just a no-op.

---

## 1. The key event

`rpcs3/rpcs3qt/shortcut_settings.cpp:78` binds `game_window_screenshot` to `"F12"`
(the only default binding). The Qt side dispatches it as
`gui::shortcuts::shortcut::gw_screenshot`; `rpcs3/rpcs3qt/gs_frame.cpp:301-305` is
the handler:

```cpp
case gui::shortcuts::shortcut::gw_screenshot:
{
    g_user_asked_for_screenshot = true;
    break;
}
```

That is the whole of the GUI's participation. `g_user_asked_for_screenshot` is an
`atomic_t<bool>` (`RSXThread.cpp:44`), and there is no queue, no callback, no
timestamp. The press is only a bit that the RSX thread will observe later.

The same bit is also set by the home-menu screenshot entry
(`Overlay/HomeMenu/overlay_home_menu.cpp:128`), so everything below applies to
that path too.

---

## 2. Where the bit is consumed

`MTLGSRender::present_frame()` (`MTLGSRender.cpp:7409`). Two mutually exclusive
compile-time sites:

- `MTLGSRender.cpp:7775` — diagnostics build:
  `const bool screenshot_requested = g_user_asked_for_screenshot.exchange(false);`
- `MTLGSRender.cpp:7789` — release build: same exchange, no capture.

Because the exchange is `exchange(false)`, **the first present after the press
owns it and every other present sees false**. A press is therefore attributed to
exactly one frame, and if two presses land inside one present, one is lost. That
is deliberate: the press is a marker, not a queue.

The press is consumed at the *top* of the present path — after `open_frame()` and
after `++diagnostic_present_index` (`:7433`) — so by the time it is read:

- `diagnostic_frame_index` is already the frame being presented (`F`);
- `diagnostic_present_index` is already the present being built (`P`);
- the frame's draws are already encoded and the present source is already
  resolved (`get_present_source`, `:7510-7767`).

That ordering is why the frame the user photographed is captured as *history*
(m12..m1 plus the tape), not as the forward window.

---

## 3. Arming: `diagnostic_try_arm_capture("screenshot", true)`

`MTLGSRender.cpp:350-402`. Called from `:7785`, immediately followed by
`diagnostic_tape_freeze()`.

### 3.1 The gate

```cpp
if (!probe::at_least(5)) return;                      // :352
```

and for manual presses the window-limit / gap / monotonicity checks
(`:357-362`) are skipped — a press always arms, up to the fixed 5-window run
limit that only applies to automatic triggers.

### 3.2 The window

```cpp
const u64 first_frame = diagnostic_frame_index + (manual ? 1u : 0u);   // F+1
const u64 last_frame  = first_frame + probe::capture_frame_count() - 1; // F+1
```

`capture_frame_count()` is `MTL_PROBE_FRAME_COUNT`, default 1
(`MTLInstrumentation.h:446`).

The name `first_frame` is misleading: the only thing that reads the window is

```cpp
bool diagnostic_capture_active() { return probe::at_least(5) && diagnostic_frame_index <= diagnostic_capture_last_frame; }
```

(`:404-407`). It is a **watermark, not an interval**: with the default count the
window is `F .. F+1`, i.e. it includes the press frame itself. That is
intentional (the press frame has to be able to write `p1` into the ring dump),
but it is the reason a default F12 produces two frames of images and not one.

### 3.3 Fresh arm

```cpp
diagnostic_capture_last_frame   = last_frame;                 // F+1
diagnostic_capture_gap_until    = last_frame + 1200;          // auto-trigger only
diagnostic_capture_armed        = true;
diagnostic_capture_dump_pending = true;
++diagnostic_capture_windows;
rsx::diagnostic_capture_window() = true;                      // opens the level-5 gates
diagnostic_reset_window_state();                              // budgets, dedupe, patches
diagnostic_tape_freeze();
diagnostic_snapshot_extend_present = diagnostic_present_index + capture_frame_count(); // P+1
```

`diagnostic_capture_window()` is the switch the render path reads: it is what
turns on the world-draw lines, the patch probes, the target tally and the
index/vertex dumps (`:3628`, `:4409`, `:4908`, `:4941`, `:7693`). Arming a
window therefore changes the cost of the frame it covers; a window that never
closes leaves the backend in the expensive mode.

### 3.4 Press inside an open window

`:367-384`: extend `diagnostic_capture_last_frame` if the new press reaches
further, reset the window budgets, push `diagnostic_snapshot_extend_present`
forward, and **do not dump the rings again**. A second press is a marker; the
ring history is already on disk.

### 3.5 `diagnostic_tape_freeze()`

`:2853-2863`. Sets `diagnostic_tape_frozen = true` and logs the frame and depth.
It does **not** copy, wait or write anything: the copies were already encoded by
every previous present (`:7662`), so freezing only stops the ring from being
reused and tells the next flush to read it.

---

## 4. The press frame: what is written during present `P`

The press is processed at `:7775-7787`, then the rest of `present_frame()` runs in
this order (line numbers are the diagnostics branch):

### 4.1 Ordinary PNG capture — `:7793-7799`

```cpp
if (source.image && (user_asked_for_screenshot || user_is_recording))
    resources.output.request_capture(recorder, constants, source, user_asked_for_screenshot, ...);
```

`MTLPresent.cpp:252-391` blits the **present source, before the upscaler and
before the drawable compose**, into a persistent shared buffer behind an
`ALL→BLIT` queue barrier, and parks it in `m_captures[slot]`. The pixels are not
read on the CPU here. Delivery is asynchronous: `on_slot_complete()`
(`MTLPresent.cpp:393-399`) marks the slot ready and `deliver()` (`:401`) is what
writes the user-visible PNG. So the PNG the user finds in the screenshot folder
is *this* frame, and it can arrive a frame or more after the F12.

`user_asked_for_screenshot` is also passed as `with_overlays`; the capture then
composes a private copy with the OSD/host overlays, because overlays must not
touch the RSX output (`MTLPresent.cpp:297-365`).

### 4.2 `host-shot` dump — `:7806-7811`

```cpp
MTL_NOTE(5, "Metal 4: screenshot surface %u, %ux%u", P, w, h);
dump_host_image(..., source.image, "shot", P, 0);
```

This is the diagnostics twin of the PNG: same pixels, different route.
`dump_host_image()` (`:2450-2609`):

1. reject unsupported/oversized formats (`:2453-2471`);
2. refuse past the 10 GiB run budget (`:2488-2502`);
3. allocate a 0x100-aligned shared buffer, `copyFromTexture` behind an
   `ALL→BLIT` device barrier (`:2504-2524`);
4. **`recorder.flush_for_readback()`** (`:2527`) — commits the partially built
   frame and waits on the GPU. This is a real stall inside the present;
5. hash the bytes (FNV-1a), count non-zero words and distinct byte values, keep
   the first four words of row 0 (`:2529-2577`);
6. write `host-<label>-<P>-addr<hex>-<w>x<h>-fmt<fmt>.bin` (`:2579-2584`);
7. log one line with the hash, `nonzero`/`words`, `distinct bytes` and the first
   row (`:2603-2606`).

`distinct bytes` is the flat-image detector: a stale, zeroed or never-refreshed
copy collapses to a handful of values, real content does not. That line is the
cheapest way to tell "the world went black" from "the image was never written".

### 4.3 Tape flush — `:7821`

```cpp
diagnostic_tape_flush(resources.recorder);
```

`diagnostic_tape_freeze()` at the arm already set the flag; the flush is the read
side, and it runs in the same present so the PNG and the tape describe the same
submitted stream.

### 4.4 Present / upscaled dumps — `:7693-7721`, `:7873-7895`

`window_present = diagnostic_capture_active()` is evaluated **after** the arm, and
with the default window `F <= F+1`, so the press present is already inside the
window:

- `dump_host_image(..., "present", P, address)` for the resolved source
  (`:7697`), bounded by `diagnostic_present_dump_budget = 2048` (`:7628`);
- `dump_host_image(..., "upscaled", P, 0)` when the FSR/upscaler produced a
  different image (`:7875`);
- one `present format` line naming the source, upscaled and drawable formats
  (`:7888-7894`).

So at the press present the disk gets `shot`, `present` and (if FSR is active)
`upscaled` for the same `P`, plus the ordinary PNG. Three of the four are the
same content by construction; comparing them localises the output chain, not the
world.

### 4.5 The ring dump — `:7992-8017`

This is the last thing before `m_frame->flip()`, and it is the piece that makes
the capture a history:

```cpp
if (diagnostic_capture_dump_pending) {                 // set by the arm
    diagnostic_capture_dump_pending = false;
    resources.constants.begin_dump(diagnostic_present_index);   // P
}
if (diagnostic_capture_armed)
    resources.constants.extend_dump((u32)diagnostic_snapshot_extend_present); // P+1
resources.constants.snapshot_frame(diagnostic_present_index);   // P
resources.constants.continue_dump();
```

`constants_heap::begin_dump()` (`MTLConstants.cpp:696-711`):

- iterates the 12-slot ring history from `m_history_pos` (the oldest entry) and
  writes **presents `P-12 .. P-1`** as `m12 .. m1`;
- sets `m_dump_index = P`, `m_dump_until = P + capture_frame_count()` (= `P+1`),
  `m_dump_active = true`.

`snapshot_frame()` (`MTLConstants.cpp:605-680`) runs on **every** present, all
run long, not only during a window. It memcpy's all 18 rings plus the retired
buffers and the per-draw records into `m_history[m_history_pos]` and advances the
position. This is the pre-press history: it exists before F12 for the same reason
the tape does.

Rings copied: persistent, volatile, index, instructions, overlay-vertices,
overlay-uniforms, vertex-context, draw-parameters, fragment-context,
sampler-info, stipple, blend-values, fill-values, vertex-draw-params,
fragment-constants, transform-constants, instancing-constants,
instancing-lookup (`MTLConstants.cpp:618-636`).

Then `continue_dump()` (`:727-745`) writes the entry just snapshotted — present
`P` — as **`p1`**. Forward frames are `p2, p3, …` as long as
`frame.present <= m_dump_until`.

Each snapshot writes:

- one `frame-<P>-<age>-<ring>.bin` per non-empty ring (`MTLConstants.cpp:382-418`);
- `frame-<P>-<age>-retired-<ring>-<n>.bin` for every ring that grew mid-frame
  (`:420-434`);
- `frame-<P>-<age>-draws.bin`, the binary `draw_record` array (`:439-448`);
- `frame-<P>-<age>-manifest.txt`, the human-readable map: present index, age,
  draw count, retired count, `grew_mask`, per-ring generation, per-ring element
  counts, one `draw` line per subdraw with stream offsets and 16 attribute
  pairs, and one `record` line per draw with pipeline, descriptor, viewport,
  targets, formats, scissor, all four hashes and all 16 texture units
  (`MTLConstants.cpp:450-559`).

Note the naming: **every `m*` and `p*` file of one press carries the same
`frame-<P>` prefix**, where `P` is the present index *at the press*. The age
suffix, not the frame number, is what orders them. `m12` is the oldest, `m1` the
present immediately before the press, `p1` the press present.

---

## 5. The frame after the press: `F+1` / present `P+1`

`open_frame()` (`:1924-1971`) is called by whichever comes first — a draw-path
hook (`begin_draw :1986`, `on_framebuffer_bound :1998`) or the present
(`:7426`). It is idempotent (`m_frame_open` guard), so `diagnostic_frame_opened()`
runs exactly once per frame.

`diagnostic_frame_opened()` (`:409-440`):

1. `++diagnostic_frame_index` → `F+1`;
2. `probe::census_note_frame()`;
3. disarm check — not yet (`F+1 <= last`);
4. `diagnostic_reset_frame_state()` — clears the per-frame sampled-image dedupe,
   the per-frame budgets and the black-patch baseline, because the frame that
   carries the defect is the one whose images must be dumped.

During `F+1` every level-5 gate is open: world-draw lines, the fixed-patch
readbacks from `MTL_PROBE_PIXELS`, the per-frame target tally, and the
index/vertex-stream dumps (bounded per window) all run.

At its present `P+1`:

- the tape copy writes `F+1 % depth` (the press frame is still in the ring);
- `dump_host_image("present", P+1, ...)`, and `"upscaled"` if any;
- `snapshot_frame(P+1)`; `continue_dump()` writes **`p2`** (because
  `P+1 <= m_dump_until = P+1`).

---

## 6. Window close: `F+2`

`diagnostic_frame_opened()` on `F+2`:

```cpp
if (diagnostic_capture_armed && diagnostic_frame_index > diagnostic_capture_last_frame) {
    diagnostic_capture_armed = false;
    rsx::diagnostic_capture_window() = false;
    probe::census_print("capture window closed at frame %llu");   // :431
}
```

The census is the only report generated at close: one `census total` line, up to
16 busiest rows, and the `census never-uploaded` list — every allocation that was
sampled this run with zero uploads (`MTLInstrumentation.h:690-733`). It is the
run's texture-level verdict and it is written once per window.

At the same present, `continue_dump()` sees `frame.present (P+2) > m_dump_until
(P+1)` and sets `m_dump_active = false` (`MTLConstants.cpp:736-740`). The ring
dump is finished; the last forward file was `p2`.

The tape is not frozen any more (`diagnostic_tape_flush` cleared the flag at
`:2872`), so slots resume being overwritten and a later press works normally.

---

## 7. What is on disk after one F12

Assume one press at present `P`, `MTL_PROBE=5`, `MTL_PROBE_TAPE=8`,
`MTL_PROBE_FRAME_COUNT=1`, level-5 dumps enabled, no FSR, no overlays.

| File | Produced at | Contents |
|---|---|---|
| `host-shot-<P>-addr0-<w>x<h>-fmt<f>.bin` | press present, `:7811` | the present source the user photographed |
| `host-present-<P>-addr<a>-…` | press present, `:7697` | same frame, present-path label |
| `frame-<P>-m12-*.bin` … `-m1-*.bin` | press present, `begin_dump` | 12 frames of all 18 rings, `P-12 .. P-1` |
| `frame-<P>-p1-*.bin` | press present, `continue_dump` | the press frame `P` |
| `frame-<P>-m*/p*-manifest.txt` | same | the read map for each frame |
| `frame-<P>-*-draws.bin` | same | `draw_record` per subdraw |
| `host-tape-f<F-8..F>-present-s0-…` | press present, `tape_flush` | up to 8 presented frames before and including the press |
| `host-tape-f<frame>-sample-u<unit>-…` | press present, `tape_flush` | the feedback-sample images those frames read |
| `host-present-<P+1>-…`, `frame-<P>-p2-*` | present `P+1` | the one forward frame |
| PNG screenshot | slot completion | the ordinary user screenshot |
| `Metal 4: pixel tape dumped …` | press present | count, bytes, allocation |
| `Metal 4: tape present frame a vs b, newly-black …, bbox …` | press present | **flicker verdict** |
| `Metal 4: census …` | frame `F+2` | never-uploaded / hazard report |

Budgets, all compile-time except the tape depth:

- sampled-image files: `diagnostic_capture_frame_budget = 6` frames per window,
  `diagnostic_capture_run_budget = 8192` images per run (`:672-673`);
- `dump_host_image`: 10 GiB of image files per run (`:2488`);
- tape: 512 MiB written and 448 MiB allocated per run (`:2710-2715`);
- present dumps: 2048 per run (`:7628`);
- patch probes: 400 000 draws per run (`MTLInstrumentation.h:479`).

---

## 8. The tape: the only pre-press *image* history

The ring dump is pre-press *state* (constants, streams, draw records). The tape is
pre-press *pixels*, and for a strobe it is the one that matters.

Every present, before the press check (`:7660-7665`):

```cpp
if (probe::tape_depth())
    diagnostic_tape_copy(recorder, resources, image, address, diagnostic_current_draw,
                         0, diagnostic_frame_index, 0, /*present*/ true, /*copy*/ false);
```

`diagnostic_tape_copy()` (`:2791-2851`) records a GPU `copyFromTexture` into a
persistent shared buffer, one slot per `(frame % depth, stream)`. Nothing is read
back. That is why a press can reach frames the user has already seen: the copies
were encoded *before* the press, and freezing only prevents the ring from
recycling the slots.

At the flush (`:2865-3133`) `flush_for_readback()` makes all queued copies
visible, and the present stream is walked as a timeline:

- per adjacent pair, newly-black and newly-dark counts, a bounding box of the
  new black, and the worst 16×16 cell (`:3002-3050`) — this is the strobe
  fingerprint: a full-screen flip to black/white with a bbox covering the frame
  is a present-level collapse, a small bbox is a draw-level defect;
- each sample stream gets a black/dark tally;
- present slots are written **before** sample slots (`:3114-3128`) so a full-size
  sample cannot exhaust the byte budget before the timeline is saved.

`tape_depth()` floors `MTL_PROBE_TAPE` at 8 and clamps at 64
(`MTLInstrumentation.h:489-503`). Below 8 a slot could be reused while its frame
is still in flight.

---

## 9. Verified inconsistencies in the path (not defects in the picture)

These are real and were confirmed while tracing the code. None changes rendering;
all affect what a capture proves.

1. **`probe::dump_frame_count()` is dead.** It is defined as `max(6, window+1)`
   with a comment promising "a floor of the six-frame burst a single-frame defect
   needs" (`MTLInstrumentation.h:457-467`), but nothing calls it. `begin_dump`
   uses `capture_frame_count()` instead (`MTLConstants.cpp:708`), so the default
   press yields `p1` and `p2` only — two forward frames, not six. Either the call
   site should use `dump_frame_count()` or the helper should go.
2. **The window is a watermark, not `[first, last]`.** `first_frame` is computed
   and never read; `diagnostic_capture_active()` compares only against
   `last_frame`, so the press frame is inside the window. Harmless for the ring
   (`p1` needs it) but it means the default window covers 2 frames, not 1.
3. **`m_dump_until` mixes counters.** It is built from a *present* index and
   `capture_frame_count()`, while `m_dump_active` is ended by comparing a
   *present* index. The window beside it is counted in *frames*. The two happen
   to advance together on this backend (one present per frame), but nothing
   enforces it.
4. **`dump_host_image` flushes inside the present.** `flush_for_readback()` at
   `:2527` commits and waits, so an F12 capture alters the timing of the frame it
   records. Evidence only — never run a timing experiment with a press in it.
5. **`diagnostic_capture_active()` gates dumps but not the tape.** The tape is
   independent of the window (`probe::tape_depth()` only), so a run can have a
   full tape timeline with no rings, or rings with no tape.

---

## 10. The one-screen summary

```
press          F12 bit set (Qt)
               |
next present   flag.exchange(false)                     MTLGSRender.cpp:7775
               arm: window F..F+1, pending, freeze tape  :350
               PNG capture (async)                       :7797
               host-shot dump + GPU sync                 :7811
               tape flush (pre-press timeline)           :7821
               host-present / host-upscaled              :7697, :7875
               begin_dump: m12..m1 rings                 MTLConstants.cpp:696
               snapshot(P) + continue_dump -> p1         :8015
present P+1    host-present/upscaled, snapshot, p2        :7697, :8016
frame F+2       disarm + census_print                     :420
slot done       PNG delivered                             MTLPresent.cpp:393
```

For Demon's Souls the useful reading is: if the tape timeline shows the
black/white flips in the presents *before* the press, the defect is already in
the presented image and the ring dump of the same `m*` frame carries the state
that produced it. If the tape is clean and only the F12 frame is wrong, the defect
is in the press frame's own path (capture, compose or a race introduced by the
flush). Those two outcomes point at different halves of the backend, and one
`MTL_PROBE_TAPE=16` run separates them without a rebuild.
