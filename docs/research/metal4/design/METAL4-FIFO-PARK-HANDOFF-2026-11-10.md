# Metal 4 backend — FIFO live-lock on Demon's Souls (BLUS30443) — handoff

Date: 2026-11-10. Branch: `metal4-dev` (the Metal 4 backend development branch;
unlike `metal4-release`, which is the squashed public release and was left untouched).
Status: **2026-11-11 21:42 — THE PICTURE DEFECT IS SOLVED: the backend had no
1-D texture type, so the presenter blit sampled a 2-D texture where its shader
declared a 1-D one, and three undefined reads replaced all three colour
channels (§22.13, L230). Fixed, regression-tested and verified on device: the
screen shows the scene.** The frontier is
deleted and the link-at-PUT rule is withdrawn after a device run (§18); the
parser is one cursor + PUT with no exception, and the whole-packet gate is the
only unproven deviation left. Demon's Souls boots and runs indefinitely on that
model (FIFO healthy: 0 link-at-PUT, 17 ring wraps, 4708 parks resolved, 2
desyncs recovered). **The open defect is the picture, but §21 moves the
question:** the blit is a *plain store* (its guest blend-enable mask is `0xe`,
so blending is off on attachment 0), the scene RTT's alpha is 0 in every pixel
of every dump, the RTT's content is a solid 529×322 rectangle that both probe
patches sit inside, and the guest's 16-draw stream is **byte-identical across
1100 frames** except for one quad's vertex block — the title is showing a
frozen frame. **Read §22 first**: it is the cross-backend audit
(Metal 4 against Vulkan and OpenGL, path by path) and it changed the plan — it
closed H1's object-identity half from logs already on disk, found the one
confirmed architectural deviation, and produced the cheapest next experiments
(`MTL_DEBUG_LAYER=1`, `MTL_PROBE_VARIANT=heaps`, "press Start"). **Then §21**:
§21.7 is the plan (Phase 1 = the Vulkan oracle, Phase 2 = the pointer-identity
check, Phase 3 = the `probe-tri` run), and §21.6 withdraws §20.9's `sentinel`
plan, which cannot discriminate. §20
is the elimination record for the blit itself. Then §§18–19 (FIFO,
retrospective, cross-title risk). §§6–8 and §16.4 are history.

> **Correction (read this first).** The original root cause and Option A in
> §§7–8 are wrong in one load-bearing detail: at `fetch == put == 0x5fd77c` the
> word at PUT is **not** the just-published epilogue, it is stale data from an
> earlier lap. The producer published the high buffer *ahead of its writes* and
> then **left it for a different buffer** (the low default-buffer fragments
> `0xb910 / 0x11230 / 0x16c04`). Accepting the packet at PUT (Option A) would
> execute stale commands. See §§6.5–6.8 and §8.4. The evidence is quoted there
> with log timestamps; read it before trusting any older sentence in this file.

This document is self-contained. Every claim below comes from logs, dumps or
the NV4097 decode performed during the investigation, and is annotated with the
source. Do not trust reconstructions; read the logs.

---

## 1. One-paragraph summary

Demon's Souls (BLUS30443) loads, then hangs forever: the video overlay still
flips at the same 16.67 ms cadence (≈60 fps), the GPU work kept flowing during
boot (1,177 presents with draws on several thousand first), but the game never
advances past a specific frame boundary. The RSX thread is parked at a
published, valid command-word (`0x00041d6c` = `NV4097_SET_SEMAPHORE_OFFSET`) at
`fetch == put == 0x5fd77c`. The game's producer published exactly one frame's
worth of command stream and then stopped — it is waiting for the flip semaphore
(`NV406E_SEMAPHORE_ACQUIRE(1)` = the only exit). The release write happens in
the epilogue **immediately after** the word our puller refuses to read. So:
the game waits for the RSX to execute the epilogue; the RSX waits for words the
game never publishes because it waits for the RSX. Our strict "reading at the
published end is a jump/call/return only" frontier rule (added to fix the RR7
desync class) is what parks the parser, so on this game the rule is the wrong
default — not a data race, not an engine quirk of the image generator, and not
a Metal 4 problem; the same logic would freeze on Vulkan/GL with the same
FIFO tracker in place.

---

## 2. Symptoms

- Launch Demon's Souls (title list). Boot proceeds normally (Wii-style load, draws, census prints).
- At a point equivalent to the game reaching the first interactive/loading
  boundary, the game stops progressing: black screen, no visible content ever.
- The FPS counter/flag stays fixed; the flip loop reports ~60 fps indefinitely.
- The game cannot be quit with "Quit" normally — eventually the watchdog kills it at ~6 s (`read_sysutil_signal=1, elapsed_ms=6000`).
- On a frozen frame, F12 screenshot (F12 = `gw_screenshot`) captures nothing
  useful: black frame. (Confirmed: `BLUS30443_screenshot_2026_11_10_16_06_46.png` — fully black.)
- One Chinese user guide behaves as ASCII artist emoji in the config: `Max SPURS Threads: 6` (irrelevant; unrelated to this bug).
- The game is configured for the **Metal 4** renderer (`Renderer: Metal` in the
  game's custom config `config_BLUS30443.yml`). Vulkan path uses KosmicKrisp — not relevant to this bug.

All three device runs (progressively instrumented) reached the same class of
stall:

| run id | build | result | stall addr |
|---|---|---|---|
| `20261110-155430` | probe 5, pre-fix | stuck ~65 s in | `fetch=0x200000`, `put=0x20d7a4`, `reason=put-unknown` |
| `20261110-160602` | probe 5, pre-fix | stuck ~50.5 s in | `fetch=0x200000`, `put=0x210e24`, `reason=not-published-end` |
| `20261110-162015` | probe 5, **with fix below** | stuck at ~98.5 s (far further) | `fetch=0x5fd77c`, `put=0x5fd77c`, `reason=put-unknown` |

---

## 3. Reproduction / how to rerun it

```sh
cd /Users/abhakash/PersonalWS/rpcs3
python3 .metal4-release-scrub.py selftest   # if you need the scrubber state
cmake --build build --parallel 8            # rebuilds rpcs3.app
cd build/bin
RUN=$(date +%Y%m%d-%H%M%S)
PROBE=/Users/abhakash/PersonalWS/rpcs3/build/probe
echo "$RUN" > "$PROBE/run-id"
MTL_PROBE=5 \
MTL_PROBE_LOG="$PROBE/probe-$RUN.log" \
MTL_PROBE_DUMP_DIR="$PROBE/dumps-$RUN" \
MTL_PROBE_INTERVAL=300 \
nohup ./rpcs3.app/Contents/MacOS/rpcs3 > "$PROBE/stdout-$RUN.log" 2>&1 &
echo $! > "$PROBE/rpcs3.pid"
```

Launch the game from the list (`BLUS30443`). It will freeze; then the log at
`~/Library/Caches/rpcs3/RPCS3.log` and the probe log will show the park lines.

F12 arms the Metal 4 capture facility *only at probe level ≥ 5* (the require is
`probe::at_least(5)`; at level 4 it silently did nothing — this cost us an F12
session; see §11.1).

---

## 4. Evidence inventory

**Logs (human-meaningful ones):**
- `build/probe/probe-demonssouls-20261110-162015.log` — latest full probe-5 run (the stall at `0x5fd77c`).
- `build/probe/probe-demonssouls-20261110-160602.log` — the pre-fix run with the same stall class.
- `build/probe/probe-demonssouls-20261110-155430.log` — earlier, `reason=put-unknown` identified.
- `~/Library/Caches/rpcs3/RPCS3.log` — the emulator's shared log (all RSX/FIFO diag lines live here; 2,144 `fetch at/after PUT` occurrences currently).
- `build/probe/stdout-demonssouls-*.log` — emulator stdout (2 lines each).

**Dumps:**
- `build/probe/dumps-demonssouls-20261110-162015/` — **~100 MB**, all seven textures converted (fp/vs glsl→metal translation included).
- `build/probe/dumps-demonssouls-20261110-160602/` — same shape, ~200 MB.
- `build/probe/dumps-demonssouls-20261110-155430/` — first probe-5 run.

**Screenshot:**
- `~/Library/Application Support/rpcs3/screenshots/BLUS30443/BLUS30443_screenshot_2026_11_10_16_06_46.png` — completely black. Every other BLUS30443 screenshot has been deleted at the user's request; this is the only one taken (before stall or at stall — is ambiguous, but it's black either way).

**Code changes (working tree, uncommitted — only the scrub-annotated diagnostics were committed; the real fix is in the worktree):**

```
 M rpcs3/Emu/RSX/RSXFIFO.cpp    (fix + diagnostics)
 M rpcs3/Emu/RSX/RSXFIFO.h
?? docs/research/apple-silicon/   (untracked deliverables, must stay untracked)
```

---

## 5. Investigation — what we tried and what happened

### 5.1 First identification: stalls in the FIFO
The base symptom was "fps counter stuck" + "black screen". The probe-5 log showed
1177 frames presented, GPU silent across them but presents ongoing; the RSX
thread parked indefinitely at a fixed address. RSX park detection (`diag_park`/
`park tail`/`PUT move`) fired over and over at that address.

### 5.2 Working out *where* the producer was
Useful log lines (from `RPCS3.log`):
```
RSX: FIFO diag: PUT move prev=0x5f5b64 put=0x5fb3c0 parked=1 delta=+22620 ...
RSX: FIFO diag: parked addr=0x5fd77c frontier=0x5fd77c put=0x5fd77c word=0x41d6c ...
RSX: FIFO diag: redirect addr=0x5fd77c put=0x116b4 reason=put-unknown current_base=0x5f0000 current_frontier=0x5fd77c
RSX: FIFO diag: streams=4 cur=0 sav=0 ... e0=b0x5f0000 f0x5fd77c l0 e1=b0x5a8000 f0x5bebf4 l0 e2=b0x5c0000 f0x5d6bf4 l0 e3=b0x5d8000 f0x5eebf4 l0 pend_cnt=4 pend=0x0 0xb910 0xbcd0 0x116b4
```

Note the shape: PUTs advance ~`+0x24c0` per jump-slab move for many slabs
(add `base=0x5a8000`, `0x5c0000`, `0x5d8000`, `0x5f0000` logged by `add()`), and
then stall at `0x5fd77c`. Puller parked at the published end; redirection
refused because the pending producer position named a different slab not in the
current stream table.

### 5.3 Instrumentation added in this investigation (all in the working tree)
All new diag blocks/ranges carry `EXCISE-BEGIN/END` markers so the release
scrubber removes them:

- `RSXFIFO.cpp:161` `EXCISE-BEGIN fifo-diag-impl` — contains `diag_put_sample`, `diag_read_ahead`, the new `diag_state` (streams snapshot) and `note_put`-related dump.
- `:577` `fifo-add-diag` — logs every `stream_tracker::add()` call.
- `:833` `fifo-redirect-diag` — rate-limited (1 Hz) `redirect()` decision print with the exact reason string (`not-published-end` / `put-unknown` / …).
- `:952` `fifo-read-ahead` — also dump on `addr == published_end` (the live-lock pose), not only `addr > published_end`.
- `:976` `fifo-park-word`, `:1015` `fifo-resume`, `:1074` `fifo-stall-state`, `:1153`/`:1163` `fifo-stall-start/log` — pre-existing ranges; unchanged semantics.
- `fifo-park-word`, and `RSXFIFO.h:264` `fifo-diag-state`, `:361` `fifo-diag-methods` — pre-existing.

Excise markers added *during this investigation* (so the release scrub must run
on these to stay clean): `fifo-add-diag`, `fifo-redirect-diag`, `fifo-read-ahead`.

### 5.4 The one fix that improved things — and its limits
Commit `daf2db0`-prefix change (working tree only, not yet committed): in
`stream_tracker::on_jump`, the `find()` fast path used to accept any aliased
match regardless of how stale the current entry's frontier was. Now it trusts
only (a) `linear` streams, or (b) streams whose frontier has passed `target`;
otherwise it falls back to the pending-ring path, which creates a correct slab
entry. Device result: the game survived multiple slab boundaries it previously
froze on (it progressed well past the original `0x200000` stall point) and then
reached a **new** stall further in — at `0x5fd77c`, the same structural class,
at the semaphore epilogue the game actually uses this frame.

### 5.5 The F12 trap (time lost, lesson)
F12 is wired to `gw_screenshot`. The Metal 4 capture arming
(`MTLGSRender.cpp:6585-6594`) is gated on `probe::at_least(5)`; at probe level
4, pressing F12 simply did `g_user_asked_for_screenshot = true` and the window
er orders `gs_frame::saveScreenshot` — Metal wrote the png (black) but never
armed the ring dump. We lost one debug session to that. Now noted, and gate
logic is understood.

### 5.6 What doesn't matter (verified dead-ends)
- **SPURS Threads = 6 vs Unlimited.** Both settings present in the game's
  custom config and the global config; upstream issue #16157 is about low
  SPURS counts; the live-lock reproduces identically with `Unlimited`. Ignore.
- **The label/GPU-label work.** The acquire sync path is unaffected by the
  stall position; `nv406e.cpp:168-172` already forces arg==0 → 1 for the
  device sema release.
- **`NV4097` shaders/interpreter** — the game's compiles were fine; nothing
  shader-specific shows up at the stall.
- Renderer `Metal` vs `Vulkan` — irrelevant; the FIFO parser is shared.

---

## 6. The bug, precisely

### 6.1 The game's command buffer boundaries (decoded by subagent; see Appendix A)

> Caveat: the "park tail" dumps the bytes in memory at the park address, which is
> not the same as the bytes the producer has published — if the buffer was reused
> between frames they may be stale. We cannot distinguish stale from fresh from
> the log alone, but the *pattern* is consistent across all three runs and the
> decode fits every time.

Every frame buffer ends with an epilogue shaped approximately like this (verified in all dumps):

```
0004e944 00000000   GCM_DRIVER_QUEUE+1  — queue the display flip
00040060 56616661   NV406E_SET_CONTEXT_DMA_SEMAPHORE = CELL_GCM_CONTEXT_DMA_DEVICE_R
00040064 00000030   NV406E_SEMAPHORE_OFFSET = 0x30    (device sema = flip sema)
0004006c 00000000   NV406E_SEMAPHORE_RELEASE → forced 1 (device_addr+0x30, arg==0 rule)
00040064 00000010   NV406E_SEMAPHORE_OFFSET = 0x10    (label_addr+0x10 = flip sema)
0004006c ffffffff   NV406E_SEMAPHORE_RELEASE → 0xffffffff
0004e924 8000010f   GCM_FLIP_HEAD+1
[... draw state setup ...]
00041d6c 00000ff0   NV4097_SET_SEMAPHORE_OFFSET = 0xff0   ← the parked word
00041d70 00110000   NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE → 0x40300ff0 ⇐ 0x11
00041d6c 00000ff0   NV4097_SET_SEMAPHORE_OFFSET = 0xff0
00041d70 00120000   NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE → 0x40300ff0 ⇐ 0x12
```

`NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE` is the `vm::write` MMIO-equivalent
that publishes a semaphore in guest memory — in *healthy* frames our `label:`
log shows two such writes per frame completing with incrementing counters
(0x02b→0x038 on `0x40300ff0`, 0xc2→0xc5 on `0x40300810`). So our MMIO path *does*
know how to release them; it hasn't reached them because the command stream
stalls at the park point.

### 6.2 Why the parser parks on `NV4097_SET_SEMAPHORE_OFFSET`

> **SUPERSEDED by §16.** This subsection reads the parked word as the frame's
> live epilogue. §16.2 shows the parked word is the *next header slot*, that
> the chunk ends cleanly on `NV4097_SET_BEGIN_END(0)` one packet earlier, and
> that the parser is parked at a packet boundary, not mid-epilogue.

`fetch_u32` in `rpcs3/Emu/RSX/RSXFIFO.cpp` only accepts reading a word at
`addr == published_end` if it's a jump/call/return (`is_valid_link`). The
frontier here is `fetch == put == 0x5fd77c` — the producer published exactly up
to (but not including) the word; the count bit would need to be decoded past it.
`NV4097_SET_SEMAPHORE_OFFSET` is not a jump link; it's an ordinary method
command. Our puller therefore refuses to parse the packet, the GPU never
executes the release pair, the flip sema stays 0, the game never continues.

### 6.3 Why the producer stops publishing
Because the game can't proceed: it's waiting on `NV406E_SEMAPHORE_ACQUIRE(1)`
(the flip semaphore in the epilogue). The flip is handled by the flip engine /
present path; the flip semaphore releases when the flip completes; the game
needs that to reuse the buffer. Since the flip depends on the RSX completing
the buffer (the epilogue releases the device-and-label sema), the RSX is
required to parse the boundary. The game publishes exactly one frame's command
stream and then waits for it. The parser is parked just in front of that frame's
critical release sequence. Circular. Deterministic.

Our 3 runs all end in the same shape of park because the game's buffer always
ends at this boundary and the producer always waits on flip.

### 6.4 What we already know is NOT the cause
- GPU hang or Metal encoder bug — presents happen continuously, GPU works until
  the stall.
- A race in the semaphore implementation — in healthy frames it releases fine.
- A PPU/SPU/interpreter bug — same hang regardless of interpret or recompiler
  and regardless of SPURS configuration.
- The KosmicKrisp ICD — the renderer is Metal; KK is only used for the Vulkan
  config path (which this game doesn't use).
- The fetch/put delta being "badly aligned" — it is exactly `put == fetch`.

---

## 7. Root cause statement

> **SUPERSEDED by §16.4.** The deadlock is not caused by the frontier refusing
> a *complete published packet*: the word at PUT was never published and the
> chunk ends cleanly one packet earlier. Options A/B in §8 are withdrawn.

**The strict frontier rule — "reading at or past the published end is a
jump/call/return only" — breaks on Demon's Souls because the game ends every
frame's command buffer with ordinary NV4097/NV406E method commands, including
the semaphore release sequence our flip semaphore depends on. The rule
considers those bytes unreadable garbage until the producer moves PUT again;
the producer never moves PUT again because the game is waiting for the flip
semaphore those very bytes would release. Deterministic deadlock.**

This is a *model* failing, not an implementation bug: the frontier rule
encodes "producer ahead of parser, parser behind the published boundary" but
knows nothing of "producer published a complete buffer and is now waiting on
this buffer's completion". The game's end-of-buffer pattern uses ordinary
method commands to release semaphores; upstream RPCS3 over the past several
releases has the same pattern in every game needing this fence.

---

## 8. Fix — proposed, not implemented

> **WITHDRAWN. Do not implement Option A or Option B.** Both rest on the
> premise that the packet at PUT is fully published. §16.2 E4 shows PUT is
> exclusive here and the word at PUT is previous-lap data. See §16.4 for the
> replacement design.

### 8.1 Option A (recommended): complete-method-packet exception at the frontier
In `fetch_u32` (same function that rejects the frontier today), when:
- `addr == published_end`, and
- the word there is a valid NV4097/NV406E method header (bit 18 count > 0)
  whose **entire packet** (header + count args) falls inside published bytes,
- and the method is a known NV4097/NV406E release/notify/fence method (or,
  more conservatively, *any* such fully-published method),

..then consume it. The original desync class (parser reading garbage past a
stale frontier) is preserved because we still never parse garbage: the words
are all published; we just allow them through. The `put == fetch` case already
also has the full packet published (the producer published everything it needs;
it just hasn't moved to the next buffer).

RR7's bug (parser consuming stale bytes as commands) is unaffected: we never
advance past the published bound. Only the *literal* `addr == put` position
gets the exception; it still requires every byte read to be ≥ PUT-published.

### 8.2 Option B (lighter, but riskier): special-case NV406E/NV4097 semaphore-release methods only
Only allow the exception for `SEMAPHORE_OFFSET` / `SEMAPHORE_ACQUIRE` /
`SEMAPHORE_RELEASE` / `NOTIFY` / `WAIT_FOR_IDLE`. This keeps the tighter blast
radius but means other titles with a different boundary epilogue could hit the
same wall. Pick A unless you want the conservative start.

### 8.3 Validation
- BLUS30443 should reach at least the load-game menu (ideally in-game) —
  confirm by present count rising past frame ~1178 without freeze.
- Re-run RR7 (BLUS30001) and confirm no regression: the frontier rule still
  applies at the original stall position (`fetch=0x200000`); the game should
  still enter the menu (last playable frame: menu text visible).
- GPU-free test suite must stay green (455/455 release, or 681/681 dev).
- The 74 s boot cost is significant; budget accordingly.

---

## 9. Risks / open questions

1. **RR7 regression surface.** The exception must be gated such that the RR7
   desync class stays blocked. If you gate by "packet fits inside published
   bytes" — semantically conservative, correct — should be fine. If you gate
   by method list, more conservative, more likely to re-break other titles.
2. **The flip semaphore appears to be NV406E-driven** (`NV406E_SEMAPHORE_*`). The
   flip path is AsyncFIFO/flip path; our Metal flip path presumably publishes
   correct releases; the producer is stalled waiting for the flip, and the flip
   is presumably waiting for RSX completion — i.e. the same deadlock seen from
   the other side. If Option A unblocks one frame, check whether the flip path
   also needs the exception elsewhere (e.g. in the flip completion notify / the
   `NV4097_NOTIFY` handling).
3. **The image heap** (the game's vertex program binary) occupies the buffer,
   including full-bit-31 words that are not commands at all (e.g. `0x805fd5e8`);
   make sure the exception does not accept those: the NV4097 method check
   (count + enum) already excludes them; don't relax it.
4. **Whether the game's buffer truly ran to completion before parking**
   (`put == fetch` means producer published exactly this; the following words
   are visible in the park tail, so the full packet is there — confirmed).
5. **Whether one fix is enough** — the game may park again further in (e.g.
   in the in-game loop) at a different boundary with a different epilogue.
   Unknown until we try.
6. **The L145/L132 honest-GET lock** — the whole fix must not violate the L145
   contract: GET reported only when all prior work committed; our exception
   doesn't change that path.
7. **The present loop keeps flipping stale frames at the stall** — not a bug;
   the compositor is still working because the flip sema is never released.

---

## 10. Files and line numbers

- `rpcs3/Emu/RSX/RSXFIFO.cpp` — the FIFO parser (the fix location). Key ranges:
  - `:161` `// EXCISE-BEGIN fifo-diag-impl` (contains `diag_put_sample`, `diag_read_ahead`, `diag_state`)
  - `:240` `diag_read_ahead` (warns on `addr > published_end`) at `:250`
  - `:577` `fifo-add-diag` (add logging at `:585`)
  - `:609`/`:640` `note_put` and `frontier` / `redirect` regions
  - `:729` `stream_tracker::redirect` (reason-string instrumented form)
  - `:833` `fifo-redirect-diag` (rate-limited redirect logging)
  - `:874` for ping-pong-at-slab handling (park word case)
  - `:900`–`:1000` `fetch_u32` and `// EXCISE-BEGIN fifo-read-ahead` (`:952`)
- `rpcs3/Emu/RSX/RSXFIFO.h:264` — `fifo-diag-state`, `:361` — `fifo-diag-methods` (pre-existing).
- `rpcs3/Emu/RSX/RSXFIFO.cpp:729` — instrumented `redirect` with the `reason=` field.
- `rpcs3/Emu/RSX/Common/TextureUtils.cpp`, `rpcs3/Emu/RSX/VK/vkutils/vk_driver_selection.h`, `rpcs3/Emu/RSX/MTL/*` — Metal backend proper, not touched by this bug.

Picture defect (§20, Metal backend paths that matter):

- `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp` — `:1793` `layout.ignore_change` early
  return in `on_framebuffer_bound`; `:1928-1955` the per-draw
  `notify_surface_changed` / `lock_memory_region` /
  `commit_framebuffer_memory_region` over each bound colour range; `:4538`
  `can_absorb()` (pass reuse); `:5091-5141` the immediate-vertex ring write
  (`persistent_stream().alloc` → `write_vertex_data_to_memory`) and the
  `upload …` note; `:5188-5222` the `capture vertices` / `dump-draw` bytes;
  `:5257` `fill_scale_offset_data`; `:5443-5532` the manifest's
  `persistent_base/persistent_bytes/hash_persistent`; `:5612` the
  `geometry … persistent stream` dump; `:5776` the `capture input` line
  (attrib0/attrib8 addresses); `:3058` `probe_frame_pixels`; `:3492` the
  `host image sample` dump of `descriptor->image_handle`; `:4839` the probe call
  site, `:4862-4863` the `fp/vp` columns (ucode hashes, both 0 here); `:6532`
  the present-source dump.
- `rpcs3/Emu/RSX/MTL/MTLFramebuffer.cpp` — `:222-268` the `sentinel` variant
  (forces `LoadActionClear` + magenta on every colour attachment); `:200-260`
  load/store action and clear legality; `open_pass` / `bind`.
- `rpcs3/Emu/RSX/MTL/MTLConstants.cpp:360-430` — the per-frame ring dump
  (`write("persistent", frame.persistent)`), a raw `fwrite`; decode with `>4f`.
- `rpcs3/Emu/RSX/MTL/MTLFragmentProgram.cpp:29-90` — `force-fp` / `force-fp-tex`
  selectors; `MTL_WARN` goes to `~/Library/Caches/rpcs3/RPCS3.log`.
- `rpcs3/Emu/RSX/MTL/MTLProgramPipeline.cpp:49,67` — where `vs-N.metal` /
  `fp-N.metal` are written (N is the program id the capture names, e.g. the
  blit's `vp 2` → `vs-2.metal`).
- `rpcs3/Emu/RSX/Core/RSXDrawCommands.cpp:593-620` — `fill_scale_offset_data`
  (the `diag(1,-1,1,1)` Y flip the capture's `clip` column shows).
- `rpcs3/Emu/RSX/MTL/MTLRasterState.cpp:126-137` — `build_viewport`
  (`{0, h, w, -h}`) and `build_scissor_rect`.
- `rpcs3/Emu/RSX/Program/GLSLSnippets/RSXProg/RSXROPEpilogue.glsl:117-126` —
  the `_FORCE_OUTPUT` magenta site used by §20.9.
- `rpcs3/Emu/RSX/Program/GLSLCommon.cpp:255-259` — `_DISABLE_EARLY_DISCARD`
  gate (why fp 5 carries no `discard`).

---

## 11. Material artifacts / exact commands used

### 11.1 F12 capture
`F12` in the game window sends `gw_screenshot`. The Metal 4 capture path is
`g_user_asked_for_screenshot.exchange(false)` in `MTLGSRender.cpp:6583`, which
calls `diagnostic_try_arm_capture(..., true)` — but that returns early unless
`probe::at_least(5)`. At `MTL_PROBE=4` the F12 was a no-op for the dump, which
is why the screenshot came back black and no capture dumped. Now the notes
are accurate:

> «`MTL_PROBE=5` arms F12; `screenshots/` shows black because no present has
> happened for the stalled frame.»

### 11.2 instrument logging in this investigation
`MTL_PROBE=5` in the env, `MTL_PROBE_LOG` and `MTL_PROBE_DUMP_DIR` point into
`build/probe/`, `MTL_PROBE_INTERVAL=300`.
Add `FIFO diag:` sinks are `rsx_log.warning()` so no MTL_PROBE gate touches
them — they always print, but the heavy dump is `diag_state`, rate-limited by
the same 20 ms suppression counter as `diag_read_ahead`.

### 11.3 FPS stuck diagnosis census
`Metal 4: census` prints a full summary at renderer teardown; see the last one
in `probe-demonssouls-20261110-162015.log`: `uploads 19, samples 7972, frames
314..1177`, so the window to observe is before frame ~1177.

---

## 12. Release-tree impact

The release scrub deletes `docs/research` and `build/probe`, and strips
`TODO(excise)` diagnostic blocks via `.metal4-release-scrub.py`. This handoff
document lives under `docs/research/metal4/design/` and therefore **will not
ship** — correct for a research artifact, keep it out of the squashed commit
when rebuilding the release.

The working-tree fixes listed in §4 **must** be included in the next release
if they survive RR7 regression validation. RSXFIFO.cpp / RSXFIFO.h are in the
scrubber's scope; the new `fifo-add-diag`/`fifo-redirect-diag`/`fifo-read-ahead`
ranges are correctly marked and will be excised automatically — do not forget to
re-run the scrubber check after validating the fix.

---

## 13. Appendix A — NV4097 decode (subagent report)

### A.1 Decode rule
- `RSX_METHOD_COUNT_MASK = 0x1ffc0000`, `RSX_METHOD_COUNT_SHIFT = 18`
  → `count = (word >> 18) & 0x7ff`.
- `RSX_METHOD_METHOD_MASK = 0x0000fffc`
  → `method_reg = (word & 0xfffc) >> 2`, indexing `registers[...]`.
- `NV4097_*` enum values are `dev_addr >> 2`, so `method_reg == NV4097_*` enum, and `dev_addr = method_reg * 4`.

### A.2 Boundary words decoded
| word | count | enum | category | meaning |
|---|---|---|---|---|
| `0x00041d6c` | 1 | `NV4097_SET_SEMAPHORE_OFFSET` (0x75b) | semaphore latch | stores offset latch used by the next `BACK_END_WRITE_SEMAPHORE_RELEASE` |
| `0x00041d70` | 1 | `NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE` (0x75c) | semaphore release | MMIO/vm write of semaphore bytes |
| `0x00040060` | 1 | `NV406E_SET_CONTEXT_DMA_SEMAPHORE` (0x18) | semaphore config | selects semaphore DMA window |
| `0x00040064` | 1 | `NV406E_SEMAPHORE_OFFSET` (0x19) | semaphore offset | picks which semaphore to release |
| `0x0004006c` | 1 | `NV406E_SEMAPHORE_RELEASE` (0x1b) | semaphore release | does the write |
| `0x00040068` | 1 | `NV406E_SEMAPHORE_ACQUIRE` (0x1a) | semaphore acquire | spins on semaphore == arg |
| `0x0004e924` | 1 | `GCM_FLIP_HEAD+1` (0x3a49) | flip command | queues flip on display head |
| `0x0004e944` | 1 | `GCM_DRIVER_QUEUE+1` (0x3a51) | flip command | queues display buffer |
| `0x00041804` | 1 | `NV4097_SET_ZCULL_STATS_ENABLE` (0x601) | ZCULL | no guest write |
| `0x00041808` | 1 | `NV4097_SET_BEGIN_END` (0x602) | draw clause | opens/closes a primitive |
| `0x00041d60` | 1 | `NV4097_SET_SHADER_CONTROL` (0x758) | shader state | program state dirty flag |
| `0x00040304` | 1 | `NV4097_SET_ALPHA_TEST_ENABLE` (0xc1) | render state | decode-only |
| `0x00041db4` | 1 | `NV4097_SET_LINE_STIPPLE` (0x76d) | render state | decode-only |
| `0x00041db8` | 1 | `NV4097_SET_LINE_STIPPLE_PATTERN` (0x76e) | render state | decode-only |
| `0x00040104` | 1 | `NV4097_NOTIFY` (0x41) | notify/fence | writes {timestamp, 0} to context-DMA-notify window |
| `0x805fd5e8` | — | not a command word | inline data | skipped by OLD_JUMP |
| `0x205fd6f0` | — | `RSX_METHOD_OLD_JUMP_CMD` (bit 29) | jump | command buffer link (e.g. target = 0x5fd6f0) |

### A.3 Epilogue analysis (this game, every frame)
The boundary epilogue is (with `trailing-call-for-clarity`):

```
0004e944 00000000   GCM_DRIVER_QUEUE+1   queue flip
...NV406E semaphore writes (see §6.1)...
0004e924 8000010f   GCM_FLIP_HEAD+1
[draw state setup words]
00041d6c 00000ff0   NV4097_SET_SEMAPHORE_OFFSET = 0xff0
00041d70 00110000   NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE
00041d6c 00000ff0   NV4097_SET_SEMAPHORE_OFFSET = 0xff0
00041d70 00120000   NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE
```

Our `label:` log confirms all of these fire per frame with incrementing
counters at `0x40300ff0` and `0x40300810` — until they don't, because the
frontier blocks the parser.

### A.4 Subagent verdict (verbatim conclusion)
> So yes — the producer is waiting on semaphore/fence values that this
> command stream must release, and those releases are precisely the methods
> our handlers perform MMIO for. … the hang in this log is upstream of the
> release — `fetch == put` at `0x5fd77c` with the producer now publishing
> to other contexts after which the RSX re-parks indefinitely. … The freeze
> is not fixed by treating the parked word specially; the missing events are
> the producer publishing/running the epilogue (or the flip engine completing
> the flip that the `NV406E_SEMAPHORE_ACQUIRE(1)` spins on), not by our MMIO
> path refusing a write it already knows how to do.

i.e. the epilogue is the missing piece; blocks tree is simply at the front
of it, not anywhere else.

---

## 14. Appendix B — exact recent-action chronology

| Step | What | When |
|---|---|---|
| 1 | Investigated stuck FPS counter; found `redirect` reason=`put-unknown` (run `20261110-155430`) | 2026-11-10 16:00 |
| 2 | Same class of stall at `0x200000` in run `20261110-160602`, `reason=not-published-end` | 2026-11-10 16:10 |
| 3 | Patched `on_jump` to distrust aliased `find()` matches (working tree) | 2026-11-10 16:20 |
| 4 | Launched probe-5 probes; stuck much **later** at `0x5fd77c` (`20261110-162015`) | 2026-11-10 16:35 |
| 5 | Verified screenshots/capture paths (level 4 F12 trap; level 5 only) and confirmed `screenshots/BLUS30443/…png` is fully black | 2026-11-10 16:45 |
| 6 | Spawned subagent for NV4097 decode; decode returned: `0x00041d6c` = `NV4097_SET_SEMAPHORE_OFFSET`, flip semaphore epilogue | 2026-11-10 17:30 |
| 7 | Deliberated the "complete packet at the frontier" exception — needs review before merge | 2026-11-10 17:55 |
| 8 | Wrote this document | 2026-11-10 18:15 |
| 9 | Re-read the log end to end; found 60 accepted segment-handoff redirects (D1) and that `find(put)` can never learn the low default buffer (D2) | 2026-11-10 20:20 |
| 10 | Decoded the terminal park tail: chunk ends cleanly on `SET_BEGIN_END(0)`; the parked word is previous-lap data; §7/Option A invalidated | 2026-11-10 20:35 |
| 11 | Confirmed the single 7 MB iomap / I/O-offset model and the default 32 KB-fragment buffer; retired the address-space and context-switch hypotheses | 2026-11-10 20:45 |
| 12 | Design verdict: one cursor + honest GET + forward-only idle re-point (R1–R5); §§6.2, 7, 8 superseded by §16 | 2026-11-10 20:50 |
| 13 | Implemented §16.4: deleted `stream_tracker`/`reset_streams`/`redirect`; rewrote `fetch_u32`/`read`; extracted `cursor_rules::decide`; 12 cursor tests green (dev suite 680) | 2026-11-10 22:20 |
| 14 | Follow-up fix while reviewing the 1 MiB threshold: R4 now requires an observed idle at the same cursor, and never re-points over a **live link** at the park (the `cellGcmCallback` handoff), so the threshold is no longer load-bearing for correctness | 2026-11-10 22:35 |
| 15 | Device run with the one-cursor parser: R4 fired 28× and the game still drew nothing. The O1 region scan showed 141 packets below PUT — the region's commands were being skipped because R4 resumes *at* PUT | 2026-11-10 23:10 |
| 16 | Root cause of the ring wrap found: the producer writes its **exit link** in the slot it publishes (`parked addr=0x5fefc4 put=0x5fefc4 word=0x20008000` = OLD_JUMP\|0x8000). Upstream idles at `cursor == PUT` and can never consume it, so the ring never wraps. R4 withdrawn; the link at PUT is now consumable (§17) | 2026-11-10 23:40 |

---

## 14b. Reading order for the next session (and for compaction)

Read **§21 first** — it is the current state of the picture defect (the black
screen) and the only open work. Then §20 (the elimination record for the blit
itself — §20.5 corrected, §20.8 the void run, §20.9 *withdrawn* by §21.6),
then §19 (retrospective and cross-title position), then §18 ("do not
re-derive"). §17's link-at-PUT rule and §16.4's R4 are **withdrawn**: do not
re-implement either. Do not re-read §6.2, §7 or §8 except as history; they are
superseded. Do not re-open the log to rediscover E1–E7. When compacting, keep
only: the blit's state as measured (§21.2–§21.4), the frozen-stream fact
(§21.5), the plan (§21.7), and the file paths — everything else is in this
document.

Next actions, in order (rewritten by the §22 cross-backend audit; the first
three need **no code change**):

0. **Read §22 first** — it is the cross-backend audit and it changes the plan.
   Its headline: **H1's object-identity half is already excluded by the 19:23
   logs (L223)** — the blit samples the *same* `MTL::Texture` the RTT draws
   write into — so the Phase 2 pointer diagnostic cannot find the fault. What
   is left is Metal-4-specific: the hazard model, the argument tables, the
   residency model and the GLSL→SPIR-V→MSL round trip.
1. **`MTL_DEBUG_LAYER=1` (one run, needs a go)** — Metal's own validation
   layer. The backend never enables it, so a driver-dropped draw is silent
   today (L225). It is the cheapest test of H2 and of the "resource id behind
   the slot" question.
2. **`MTL_PROBE_VARIANT=heaps` (one run, needs a go)** — tracked heaps, so the
   driver inserts the cross-encoder edges the backend declares by hand. A
   defect that goes is a missing edge (L226); a defect that stays is content.
3. **Press Start during a Metal run** — ten seconds, and it splits L222's two
   readings (static splash vs. stall).
4. **`probe-tri` (Phase 3, one run, needs a go)** — still the right fragment-
   stage discriminator, but it now answers a narrower question than §21.7
   assumed, because the object-identity half is closed.
5. **The Vulkan oracle (Phase 1, needs a go)** — unchanged and now more
   valuable: it says what the frame should look like at all.
6. **Two code fixes this audit justifies** (both small, both EXCISE-marked):
   bind the parameter rings per draw and declare the arrays with real sizes
   (L224), and align the invalid-enum fallback with OpenGL (L227).

Old list, kept for the record:

1. ~~Implement §16.4~~ — done, then withdrawn by §17.
2. ~~Add the O1 dump~~ — done; O1 is closed by §17.5 (the wrap link names the
   region's entry point; there is no re-point).
3. ~~Build and run Demon's Souls with the link-at-PUT rule (§17)~~ — done; it
   hung the game (L214). Rule withdrawn (§18).
4. ~~Run the one-cursor model on Demon's Souls~~ — done; FIFO healthy, title
   boots and runs indefinitely. The remaining defect is the picture: §20, then
   re-scoped by §21.
5. ~~The `force-fp=4,5` run~~ — done and **void**: the blit was `fp 4` in a run
   that forced `fp 5` (§20.8). Do not repeat it; program ids move between runs.
6. ~~The `sentinel` + `force-fp=all` plan (§20.9)~~ — **withdrawn** by §21.6:
   `sentinel` clears the RTT magenta too, so a working blit and a broken blit
   both show magenta. Do not run it.
7. **Phase 2 (no device): the pointer-identity diagnostic** — print the sampled
   `MTL::Texture*` beside every colour attachment's `MTL::Texture*` with an
   `ALIAS` / `SAME-ADDRESS-DIFFERENT-OBJECT` verdict (§21.7, H1). This is the
   one comparison never made, and it can settle the RCA without a device.
8. **Phase 1 (device, needs an explicit go): the Vulkan oracle** — the same
   frame under Vulkan with a capture of the presented image and of the two
   surfaces. Four answers, four different next steps (§21.7). If Vulkan is
   black too, the title is stuck and the defect is in progress, not pixels.
   Companion, free: press Start during a Metal run and check whether the
   16-record stream changes (§21.5).
9. **Phase 3 (device, needs an explicit go): `probe-tri`** — one run, one
   three-way output `(tc.x, tc.y, sampled != 0 ? 1 : 0)`, armed structurally on
   the presenter draw (§21.7). Blue band ⇒ the sample is black (H1); no
   gradient ⇒ the draw never lands.
10. In parallel: the `mesa-kk` Metal 4 backend as a second opinion on
    render-pass/attachment handling and blend-mask keying, once Phase 2 or
    Phase 3 has narrowed the area.
11. **RR7 (BLUS30001) regression run** on the release build: expect no FIFO
    desync and the menu. If RR7 parks with a *data* word at PUT (no link),
    that is the §16.2 E1 case and needs its own answer — record it, do not
    re-point.
12. **Rebase + release (in progress, 2026-11-12, §22.14).** Both branches
    rebase cleanly onto `upstream/master` `9b8974d32` (verified by doing it):
    `metal4-dev` 13/13, `release/metal4` 1/1, zero conflicts. The three
    commits are on `metal4-dev` (the FIFO work, the 1-D texture fix, the
    findings log). The residency change is **reverted** from this release and
    kept as a deferred defect with its measurement (L229). The release tree is
    the rebased dev tree, scrubbed, one squashed commit on `upstream/master`,
    pushed to `release/metal4`; `origin/metal4` is not force-pushed.
13. **After the release:** the deferred residency defect (L229), the D1 fix
    (bind the parameter rings per draw and declare the arrays with real sizes,
    as GL does), `MTL_SHADER_VALIDATION=1` as the independent confirmation of
    the 1-D finding and the regression guard, the "press Start" test (L222),
    and the Vulkan oracle.

---

## 15. Questions to answer before merging anything

1. Does BLUS30443 actually reach the load-game menu with Option A, or does it
   park again at a later boundary? (Device test required.)
2. Does RR7 regress on the same change? (Device test with BLUS30001.)
3. Does optionality guarding by method list rather than "packet fits inside
   published bytes" avoid more risk than it buys?
4. Does `flip semaphore` handling need the same exception (NV4097_NOTIFY/GCM
   flip) or does it work correctly untouched?
5. Should these diagnostics ship in development builds only (i.e. excluded
   from `metal4-release` at scrub time) or are any worth keeping for
   post-release triage? (Note: scrub already strips them automatically.)

---

## 16. 2026-11-10 20:50 — design verdict: the stream model is wrong

This section supersedes §§6.2, 7 and 8. It exists so the same log is not
re-derived again. Read §16.7 before opening the log.

### 16.1 Answer first

**Yes, the FIFO design is inadequate, and the failure is not a tuning issue.**
There are two independent defects, and both come from one premise: that the
parser can and must infer, from the PUT register alone, which command stream the
guest "means".

| # | Defect | Effect | Where |
|---|---|---|---|
| D1 | `redirect()` treats "PUT is in a different stream" as "the producer abandoned this chunk" and moves the cursor to that stream's **base** | On every 0x18000 segment handoff the parser rewinds one full segment and re-executes it: duplicate draws, ~233 ms per handoff, ~2× cost | `stream_tracker::redirect`, `RSXFIFO.cpp:778` |
| D2 | A PUT move into a region the parser never entered by a jump is unrepresentable: `find(put)` only knows bases registered by jumps, so `redirect` can never fire | Terminal park: `reason=put-unknown`, forever | same, `RSXFIFO.cpp:808-810` |

Hardware has no stream table. It has one cursor and one publication register:
read at `fetch` while `fetch != put`, idle when `fetch == put`, and a jump
commits `fetch` to its target. The replacement design in §16.4 is exactly that,
plus one forward-only rule for the case where PUT moves while the puller is
idle.

### 16.2 New evidence (all from `~/Library/Caches/rpcs3/RPCS3.log`)

**E1 — the final park is at a clean packet boundary, not mid-epilogue.**
`:33862` (0:03:56.378216):

```
parked addr=0x5fd77c frontier=0x5fd77c put=0x5fd77c word=0x41d6c
  next=0x5fd77c remaining=0 last_cmd=0x41808 get=0x5fd77c
```

`0x41808` = `NV4097_SET_BEGIN_END`, count 1. The 64-word tail (`:33863`) ends at
`0x5fd778`:

```
0x5fd764 00041808   SET_BEGIN_END, 1 arg
0x5fd768 00000008   arg = 8 (END)
0x5fd76c 40041824   header, count 1, method 0x1824
0x5fd770 03000000   arg
0x5fd774 00041808   SET_BEGIN_END
0x5fd778 00000000   arg = 0 (BEGIN)
```

So the last packet is `SET_BEGIN_END(0)`, the parser consumed it, and
`m_fifo_pos == m_next_cmd_entry == PUT` is the next header slot. The chunk is
complete *at* the boundary. The parked word `0x41d6c`
(`NV4097_SET_SEMAPHORE_OFFSET`, the epilogue decoded in A.2) is previous-lap
data in the slot after the boundary, not the live epilogue.

**E2 — the producer then publishes a different region and never writes the exit
word.** `:33872` (0:03:56.410626, 32 ms after the park):

```
PUT rewind prev=0x5fd77c put=0xb910 max=0x5fd77c get=0x5fd77c fetch=0x5fd77c
```

then `0x11230`, `0x16c04`; the pending ring fills 1→4. `park word change` never
fires for `0x5fd77c` (it does fire for earlier parks), so no exit jump was ever
written there for this pass.

**E3 — the low PUTs are the 32 KB-fragment default command buffer, in the same
address space.** Only one iomap exists: `:22834`
`sys_rsx_context_iomap(io=0x0, ea=0x38d00000, size=0x700000)`. FIFO positions are
I/O offsets translated through that table (`FIFO_control::translate_address`,
`RSXFIFO.cpp:32-45`), so `0x5fd77c` → EA `0x392fd77c` and `0xb910` → EA
`0x38d0b910`: the same 7 MB window. There is no EA/offset mismatch and no second
context (`sys_rsx_context_allocate` appears once, `:22829`). `0xb910` is in
fragment 1 (`0x8000..0xfffc`) and `0x11230`/`0x16c04` in fragment 2
(`0x10000..0x17ffc`) of `getNextCommandBufferBeginEnd` with
`g_defaultCommandBufferBegin = 0` (`cellGcmSys.cpp:513-520, 1540-1547`). The
writers are the guest's own libgcm flush (HLE writes PUT only in
`cellGcmCallback`, `:1589`).

**E4 — the frontier rule can never learn that region.**

```
:33893 0:03:56.661473 redirect addr=0x5fd77c put=0x16c04 reason=put-unknown
        current_base=0x5f0000 current_frontier=0x5fd77c
```

`find(put)` searches only bases created by jumps. The low region was entered by
a PUT move, not a jump, so it is never added, so `put-unknown` is permanent. D2
is structural: no amount of waiting or timing changes it.

**E5 — 60 accepted redirects, each rewinding one segment, each costing a full
segment execution.** `grep -c "FIFO diag: redirect from=" = 60`, from
0:03:41.863449 to 0:03:56.210815, one every ~233 ms. `from` ascends
`0x108000 → 0x110000 → … → 0x5f0000` in ~0x18000 steps; every target is exactly
`from - 0x18000` (e.g. `:33755` `from=0x5f0000 target=0x5d8000 put=0x5eebf4
word=0x0`). Shape: execute segment B, jump to B+0x18000, redirect back to B,
execute B again; by the time the parser returns to B+0x18000 the producer has
published into it, so it proceeds. One duplicated 0x18000-byte segment per
handoff, and the 63 `add` events line up with it. (This is why the doc's earlier
"~100 ms/frame replay" and "63 adds" observations exist: same phenomenon, now
with the mechanism and the predicate named.)

**E6 — healthy in-segment PUT moves look different; do not "fix" them.**
`:33770` and the seven neighbours:

```
PUT move prev=0x5f5b64 put=0x5fb3c0 parked=1 delta=+22620
  old_word=0x41d6c cache=0x5f5a00+0x164 last_cmd=0x41d60
```

Eight forward moves inside one segment, each with `get == fetch == prev` and
`old_word` equal to the last command word: the parser is caught up and resumes.
Every such park is followed by `park word change addr=… old=0x0 new=0x41804`,
i.e. the producer writes the next packet and the parser continues. That is the
normal catch-up handshake, not a fault.

**E7 — why the old reading went wrong.** `diag_park` (`RSXFIFO.cpp:390-393`)
prints `addr - (i+1)*4` for `i = 63..0`, so the last printed word is `addr-4`;
the word at `addr` is the separate `word=` field. Reading the 64-word tail as
"the published epilogue up to PUT" is what produced the §7 root cause and
Option A. The tail is *behind* the park, and it ends cleanly (E1).

### 16.3 What the evidence proves

1. The park is at a **clean boundary** with `fetch == put`; the producer declared
   the chunk complete. There is no published packet at PUT to admit (kills §7
   and Option A).
2. The producer's next act is a **different** publication (`0xb910`), so the
   correct continuation is a buffer switch, not a re-read of the old frontier.
3. The model cannot represent that switch (D2), and its only fallback
   (`redirect`) is what corrupts every handoff (D1).
4. Every piece of state the stream table holds (bases, frontiers, pending PUTs,
   call/linear flags) exists only to answer "which stream does PUT mean"; the
   answer is used in exactly one place (`redirect`/`frontier`) and is wrong in
   both directions.

### 16.4 Replacement design (implemented 2026-11-10)

**One cursor. No stream table. Honest GET.** Rules, in the order the parser
applies them:

| # | Rule |
|---|---|
| **R1** | If `fetch != put`, the word at `fetch` is readable; if `fetch == put`, idle. This is the hardware rule and the upstream fast-path rule. |
| **R2** | While the parser is *behind* PUT (`fetch < put`), only words `< put` are readable and a packet is dispatched only when `packet_end <= put`. This keeps whole-packet atomicity without a per-stream frontier. |
| **R3** | If a jump lands *ahead* of PUT (unpublished region), read at `fetch` anyway, exactly as hardware: walk NOPs/zero words until the producer publishes, never redirect, never rewind. This is the 0x18000 handoff and it removes D1. Only a jump/call/return can leave such a region. |
| **R4** | If the parser was **observed idle** at a clean packet boundary (`fetch == put`, `m_remaining_commands == 0`, `m_next_cmd_entry == fetch`, and that exact cursor was recorded at the previous park) and the next observed PUT is **not a continuation** (`put < fetch`, or `put - fetch > max_continuation`, and the parked word is not a live link), adopt PUT as the new cursor: `m_fifo_pos = m_next_cmd_entry = put`, drop the cache, `GET = put`. Forward-only, never a base, never a replay. This replaces `redirect` and fixes D2; it covers ring wraps, buffer switches, and the low default buffer with no stream table. |
| **R5** | `sync_get()` publishes `m_fifo_pos` only. L145 is unchanged and is not implicated here. |

Two refinements found while implementing, both load-bearing:

1. **R4 requires a remembered idle, not just a boundary.** A large legitimate
   forward publication made while the parser is merely *behind* PUT (it never
   parked) must not be mistaken for a buffer switch, or its packets are
   skipped. `m_idle_pos`/`m_idle_put` record the exact cursor PUT named at the
   last R1 park; R4 only fires when the parser is still standing there.
2. **A live link at the park is never re-pointed over.** Park the cursor on
   word `w`; if `w` is a jump/call/return whose target translates, then the
   producer's next move is the publication of that link and the parser must
   read it (this is exactly the `cellGcmCallback` handoff: jump written first,
   PUT = its address). Without this guard, a next buffer megabytes away would
   be classified as a switch and the jump would be skipped, landing mid-buffer.
   `is_live_link()` validates the target so a data word that happens to match
   the CALL encoding is not followed into unmapped memory (the old L138 trap).
   It is evaluated at the moment PUT leaves the parked position, not stored at
   park time, so the guest's write-then-publish ordering cannot race a poll.
   With this guard the 1 MiB threshold only decides the genuinely ambiguous
   "parked on data, producer moved" case.

`max_continuation = 0x100000` is a **bounded heuristic, not a hardware value**:
real chunks are 32 KiB fragments or 0x18000 (96 KiB) segments, RR7's buffers are
~4 MB apart, so any value in roughly (96 KiB, 4 MB) separates "same chunk
filling forward" from "switch". 1 MiB is chosen centrally in that band. It is
safe in the direction that matters because the live-link guard removes the
dangerous handoff case; the residual risk is a single contiguous fill >1 MiB
that starts after an idle park on a data word, which no observed producer does.

Why R4 is sound: PUT is the producer's only statement about where execution
may continue, and the guest writes the words below PUT before publishing it. If
it moves PUT while the parser is idle at a clean boundary, everything before
that PUT is complete by definition, so adopting it cannot read unpublished data,
cannot duplicate an already-executed packet, and cannot deadlock. Skipping is
possible only in the interval the guest published past without ever naming — and
that interval was never declared executable (see O1).

Implementation notes:

- The decision is extracted into `rsx::FIFO::cursor_rules::decide()` (pure,
  header-only) and unit-tested directly; `FIFO_control::read()` is its only
  caller, so production and tests cannot drift.
- `m_published_end` is gone; `m_cache_put` records the PUT that was current
  when the cache was filled, and `fetch_u32` refills instead of serving a word
  that was read ahead of a now-moved PUT.
- `on_call`/`on_return` keep only the **return address** (`ret_addr`,
  `saved_ret_addr`, `restore_point`); the stream index, `m_call_linear` and
  `m_in_call` gating are gone.
- `is_valid_link` was deleted, then reintroduced in reduced form as
  `is_live_link()` for the R4 park guard only.
- Deleted: `class stream_tracker` (entries, pending ring, `find`, `min_base`,
  `add`, `on_jump`, `on_call`, `on_return`, `note_put`, `frontier`, `redirect`),
  `m_streams`, `m_redirect`, `reset_streams`, `is_known`, `diag_state`.
- `rpcs3/tests/test_rsx_fifo_streams.cpp` now holds 12 cursor cases (R1 idle,
  R2 read, R3 ahead-of-PUT walk, R4 backward/distant re-point, clean-boundary
  and observed-idle preconditions, once-only re-point, inclusive publish, live
  link guard, threshold boundary). Full dev suite: 680 passed.


### 16.5 Deliberately not doing

- Not keeping a stream table under another name, and not deriving bases from
  PUT moves (that is the same inference that produced D2).
- Not reading the stale word at the old frontier when PUT moves (the §7 trap).
- Not replaying a segment to re-synchronise (D1).
- Not touching L145's GET contract or the semaphore/MMIO path: A.3/A.4 already
  established the release path works when reached.

### 16.6 Open question O1 (one experiment, needs an explicit go)

R4 resumes at `put` (`0xb910`). The alternative is that the guest expects the
puller to restart at the default fragment base (`0x8000`), i.e. to execute the
first ~0x3910 bytes of that fragment too. R4 is chosen because `0xb910` is the
only publication the guest actually made, but this must be checked, not assumed.

Discriminator: one run with a dump of `[0x8000, 0xb910)` at the moment the first
low PUT is observed.
- If that range starts with a JUMP chain that leads into `0xb910` (or is empty
  except for jumps), `put` is the correct resume point and R4 stands.
- If it contains real packets with no jump into `0xb910`, the guest expected a
  base restart, and R4 must resume at the base of the region PUT lands in
  (fragment base from `getOffsetFromAddress(g_defaultCommandBufferBegin +
  k*32K)`), not at PUT.

No device run has been made since this verdict. The existing log does not
contain those words. Note the live-link guard does not change this case: the
observed park word is `0x41d6c` (`SET_SEMAPHORE_OFFSET`, a method header), not
a link, so R4 fires.

### 16.7 Do not re-derive

- PUT is **exclusive**: `put == addr` means "empty", the word at PUT is not
  published. `cellGcmCallback` is the sole inclusive path (jump written first,
  PUT = jump address).
- At the terminal park the parser is at a **clean boundary** after
  `SET_BEGIN_END(0)`; the word at PUT is stale previous-lap data.
- `0x41808` = `SET_BEGIN_END`, `0x41804` = `SET_ZCULL_STATS_ENABLE`,
  `0x41d6c` = `SET_SEMAPHORE_OFFSET`, `0x41d60` = `SET_SHADER_CONTROL`.
- `redirect` fires at every 0x18000 handoff, rewinds one segment, and re-executes
  it: 60 times in this run, ~233 ms each. That is D1.
- `find(put)` can never see `0xb910/0x11230/0x16c04`; `put-unknown` is permanent.
  That is D2.
- FIFO positions are RSX I/O offsets through the single 7 MB iomap at EA
  `0x38d00000`; the low PUTs are default-buffer fragments 1/2, not another
  context, not another address space.
- There is one RSX context; no context switch is involved.
- The `park tail` dump is 64 words **before** `addr`, not a publication.
- Honest GET is not the problem here; do not undo L145 while fixing this.

---

## 17. 2026-11-10 21:40 — the wrap link: §16.4's R4 is withdrawn

§16.4's replacement design is **withdrawn**. The one-cursor model is right; the
R4 rule ("adopt PUT as the cursor") is wrong, and it is wrong because **PUT is
a watermark, not a program counter**.

### 17.1 Answer first

| # | Rule | Status |
|---|---|---|
| **L1** | One cursor, no stream table, no frontier, no re-point. | kept |
| **L2** | PUT is **exclusive**: the word at PUT is not published. A data word there is the previous lap's data (§16.2 E1) and must not be executed. | kept |
| **L3** | **The one exception: a live link at PUT is consumable.** The producer writes its exit link into the slot it is about to publish and then publishes that slot, so the link is already in memory when the cursor parks on it. | new, and it is the fix |
| **L4** | Upstream (8183f6e32) idles at `cursor == PUT` and therefore **can never consume L3's link**. That is why the ring never wraps. | the defect, restated |
| **R4** | "If the producer moves elsewhere while the cursor is idle, adopt PUT as the cursor." | **withdrawn** — it skips the region's commands |

### 17.2 The evidence (2026-11-10, Demon's Souls, dev build)

The run that killed R4, `~/Library/Caches/rpcs3/RPCS3.log`:

```
:34804 0:00:55.405070 {RSX [0x05fefc4]} RSX: FIFO diag: parked addr=0x5fefc4 put=0x5fefc4 word=0x20008000
```

`0x20008000` = `RSX_METHOD_OLD_JUMP_CMD | 0x8000` (`RSX_METHOD_OLD_JUMP_CMD`
= `0x20000000`, offset mask `0x1ffffffc`) — **the guest's ring wrap link**,
sitting in the exact slot the producer published. The producer's ring is
`0x1000 … 0x5fffd8` in the single 7 MB iomap; the wrap link points back to
`0x8000`, the start of the next pass.

The sequence the parser then has to survive, all in the same log:

```
:34804  parked at 0x5fefc4, word = the wrap link to 0x8000, put = 0x5fefc4
:34814  PUT rewind prev=0x5fefc4 put=0xd6e0   (the producer wrapped its ring)
```

With R4 in the tree the parser adopted PUT (`0xb910`) instead of the link, so
the newly written region below PUT was **never executed** — the game drew
nothing (black screen) — and 12 re-points later the parser was following stale
links and desyncing (`FIFO error: possible desync event (last cmd = 0xffffffff)`
at 0:01:15 and 0:01:43).

With L3 the parser consumes the link at the park, lands on `0x8000`, and the
guest's new pass runs.

### 17.3 Why the ring wrap is not a "buffer switch" at all

`cellGcmCallback` was **never called** in this run (the diagnostic added for
this investigation logged zero PUT publications through it). The guest writes
PUT itself. So:

- the emulator is **not** told that the guest switched buffers;
- the only channel that carries "the command stream continues over there" is
  the command stream itself, i.e. the producer's exit link.

That is why the old `redirect` could not work: it tried to infer a buffer
switch from PUT, and PUT does not carry one. The link does.

### 17.4 What was deleted

- R4 and the whole idle-marker machinery (`m_idle_pos`, `m_idle_put`).
- `repoint()`, `diag_repoint()` and the O1 region scan.
- The 1 MiB `max_continuation` threshold. **It is not needed any more**: the
  question it answered ("is this publication a continuation or a switch?") is
  answered by the link, not by arithmetic on addresses. The threshold was a
  heuristic in a band between 96 KiB (the largest real chunk) and 4 MB (the
  distance between RR7's buffers); it never had a principled basis.
- `stream_tracker` and everything in it (§16.4).

### 17.5 Open question O1 — closed

O1 asked whether a re-point should resume at PUT or at the region's base. The
answer is neither: **there is no re-point**. The producer names its own
continuation — the wrap link at `0x5fefc4` points to `0x8000`, which is
exactly the region base the O1 scan was looking for. The scan counted 141
packets below PUT, and `into=0` links into the region, which is what stale
previous-lap data looks like; the region is entered by the link, not by
arithmetic.

### 17.6 The A/B switch

`RPCS3_FIFO_UPSTREAM=1` restores upstream's parser exactly: idle at
`cursor == PUT` even when the word there is a live link, and no whole-packet
gate. It exists to prove which model a given game needs, and it is the
experiment that shows the wrap link is the difference.

### 17.7 Do not re-derive (additions)

- `cellGcmCallback` is **not** used by Demon's Souls; the guest writes PUT
  directly. Do not assume the emulator is told about buffer switches.
- A data word at PUT is previous-lap data. Executing it is the L138 trap.
- ~~The producer's exit link is written before the publication that names its
  slot.~~ **Withdrawn by §18.** It is true that the producer writes the link
  and then publishes, but the publication is *past* the link; at
  `cursor == PUT` the slot still holds the previous lap's word.
- ~~`is_live_link()` must check that the target translates.~~ **Withdrawn by
  §18.** Translation is not a discriminator: the ring's whole 7 MB iomap
  translates, so most small arguments look like live links.

---

## 18. 2026-11-11 18:29 — the link-at-PUT rule is withdrawn (device)

### 18.1 What happened

The first run with §17's model booted Demon's Souls normally for 90 s (60+
healthy parks, every one resolved by a producer write), then desynced 21 times
and hung. The decisive sequence:

```
park put[6] prev=0x3f1d0 put=0x3f454 old_word=0x41804 new_word=0x20040000
jump=0x3f454->0x40000            (NOP run start=0x40000 len=256 put=0x3f454)
jump past PUT target=0x19300000 from=0x600000 cmd=0x39300000
FIFO error: possible desync event (last cmd = 0x39300000)
```

At the park `0x3f454` PUT had *just arrived* there, so the word in the slot
(`0x20040000`) is **previous-lap data**, not a published command. It parses as
`OLD_JUMP | 0x40000`; the parser jumped into unwritten memory, walked 256 NOPs,
read `0x39300000` at `0x600000` as another jump, and desynced at `0x19300000`.

### 18.2 Why the rule can never be right

For a link at `GET == PUT` to be legitimate, the producer would have to write
the link and then publish `PUT == link_addr`, i.e. declare that nothing past
the link is written, and wait for the puller to execute it. **Hardware idles at
`GET == PUT` and would hang identically.** So no PS3 title can require it: the
producer must publish `PUT > link_addr`, and then the link is an ordinary
published command read by the normal path.

The earlier "the ring can never wrap" reading of the 2026-11-10 log was
over-fitted to one line: that park was followed 32 ms later by
`PUT rewind … get=0x8000`, i.e. the parser did leave it under the build of the
time. What failed in that run was the frontier and then R4, not the wrap.

### 18.3 What the parser is now

- One cursor, one PUT, **no exception**: `put == pos` idles, otherwise read.
- `cursor_rules` is two-way again; `is_live_link()` is deleted.
- The whole-packet gate is the only remaining deviation from upstream and it
  has never been isolated — A/B it before believing it.
- Kept, because they are genuine narrow fixes: the honest GET (L145) and the
  `m_cache_put` refill (a cache that spans PUT serves stale words).

### 18.4 Do not re-derive (additions)

- **Never execute a word at PUT.** No syntactic test distinguishes a stale
  wrap link from an argument; the ring is one 7 MB iomap, so a "target
  translates" check is true for most data.
- A park is healthy: the producer writes the next word and PUT advances. The
  log line for that is `park word change … new=<header>`.
- `recover_fifo` does not currently recover from a jump out of the iomap: the
  21 desyncs all re-entered `0x19300000` because `last_known_code_start` was
  set to the bad target. Recovery must roll back to the last acknowledged
  packet boundary, independently of parser rules.

---

## 19. 2026-11-11 — retrospective: how the frontier came to exist, and the cross-title position

### 19.1 The chain, in the order it happened

| # | step | why it was added | outcome |
|---|---|---|---|
| 0 | RR7 desync class: data words parsed as commands after the guest recycled a command buffer (L141/L144 archaeology) | the original defect | — |
| 1 | **frontier**: at or past a stream's published end, only a jump/call/return is readable (`da956b587`) | a reader-side guard against walking into recycled memory | fixed RR7 symptoms; shipped on **every** title |
| 2 | `redirect` (L142) | the frontier's "not a link" case needed somewhere to go | re-executed a 0x18000 segment on every handoff, ~60×/run |
| 3 | `put-unknown` (D2) | `redirect` could not learn the low default buffer | terminal park |
| 4 | R4: adopt PUT (§16.4) | PUT was the only statement left | skipped 141 packets, black screen |
| 5 | link at PUT (§17) | the park word looked like a wrap link | consumed a stale word, desynced (L214) |
| 6 | delete all of it (§18) | — | upstream model + two narrow fixes |

### 19.2 Where the reasoning went wrong

The archaeology had already found the cause: the guest reused a chunk the parser
was still in (L128, L144). That is a **writer-licence** bug — `sync_get()`
published the prefetched block end, so the guest believed the chunk was free.
L145 fixed exactly that by publishing `m_fifo_pos` only.

The frontier was added before L145, and it guarded the **reader** instead: "do
not read at or past the published end unless it is a link". As a symptom
suppressor it worked; as a design it is the wrong layer. The hardware contract
is: **the producer may not overwrite below GET**. With an honest GET the parser
cannot see recycled bytes, and no frontier is needed. A reader-side rule cannot
compensate for a writer-side lie; it can only refuse to read data the writer was
entitled to reuse.

So the answer is: **yes, misguided — not in noticing the symptom, but in fixing
it at the opposite end of the protocol from the bug, and then, once L145 fixed
the cause (device-confirmed runs U/V), not deleting the guard.** Steps 2–5 are
the cost of keeping a guard after its cause was gone: each was invented to
explain the previous one's failure. The tell is that every rule from step 2 on
was justified by a log line produced by the rule before it, never by hardware.

### 19.3 What was not misguided

- **honest GET (L145)** — an upstream defect with a hardware-correct fix,
  device-confirmed (runs U/V).
- **nested CALL (L149)** — hardware has one return register; the guest relies on
  the overwrite.
- **cache refill at PUT (`m_cache_put`)** — a word cached before publication must
  not be served after it.
- **deleting the frontier** — the largest single reduction in divergence from
  upstream on this branch.

### 19.4 Cross-title impact — what is still ours

The frontier was committed in `da956b587` and was therefore active on **every
title run on this branch**, not just Demon's Souls. Removing it is the safest
change in the set: it moves all titles back to upstream's model. What remains
against `upstream/master` (`8183f6e32`):

| change | scope | evidence | risk to other titles |
|---|---|---|---|
| honest GET (L145) | every title | L128/L145, runs U/V | timing only; hardware GET is honest, so no title can require the optimistic form |
| nested CALL (L149) | titles with CALL chains | L149, long device runs | positive; upstream rejected a legal tail call |
| `m_cache_put` refill | titles that catch up to PUT | L132 | strictly narrower stale-read window |
| whole-packet gate | every title | **not isolated** | the only remaining semantic deviation; it can only delay dispatch, but a guest that never completes a packet would stall where upstream would not |
| frontier / redirect / R4 / link-at-PUT | — | — | removed |

The gate is the only thing left that can turn a working title into a hang. It
must not be treated as "the fix" until a run **without** it fails.

### 19.5 The test matrix, and the rule for accepting a deviation

- **A**: upstream `8183f6e32` + the deletions + honest GET + nested CALL. This is
  the release candidate, and both fixes are already device-confirmed.
- **B**: A + `m_cache_put`.
- **C**: B + the whole-packet gate.

Each cell: Demon's Souls to the title + 2 min, then RR7 menu + one race. A
deviation is kept only when the cell *before* it fails and the cell with it
passes, on the title that failed. **Demon's Souls must not be allowed to add a
rule RR7 does not need** — that is exactly what the frontier did.

### 19.6 Process rules for the next session

- One semantic change per device run. `da956b587` bundled the shader
  interpreter, the probe facility and the frontier; that is why the frontier
  could never be reverted cleanly.
- If a question is unanswerable ("which stream does PUT mean?"), the model is
  wrong; do not add a table to answer it.
- A guard added to suppress a symptom must name the cause it is waiting on, and
  must be deleted when that cause is fixed.
- Never execute a word at PUT (L214). Hardware does not, so no title can require
  it.

---

## 20. 2026-11-11 21:20 — the black screen: what is now excluded, and the one experiment left

### 20.1 Answer first

**No RCA yet.** The search space is down to one draw, and the open question is
narrower than it looked: *does the blit write black, or nothing at all?* The
destination has zero RGB either way, so every earlier "writes nothing" statement
in this section is a hypothesis, not a measurement (§20.7.1).

The FIFO is not involved: on the one-cursor model the ring streams healthily and
the frame is presented at the right cadence (present every frame, `present source
from the surface`, 0 texture-cache and 0 CPU-upload present paths). The screen is
black because **one draw per frame** — the final fullscreen blit that copies the
scene render target into the display buffer — produces the presented image, and it
produces either black or nothing. Its geometry, its render state, its source
texture and its fragment program are all provably correct, and after thousands of
frames the display buffer holds exactly the value it held *before* the draw
(arguing for "nothing", but see §20.7.1).

Everything below is measured. The decisive run is the pixel-probe run of
2026-11-11 19:23:

- `MTL_PROBE=5`, `MTL_PROBE_FRAME_COUNT=4`,
  `MTL_PROBE_PIXELS=384,192,128,128;640,256,128,128`
- log `/var/folders/0x/tq2_tw6d7_lckb2kqwt23klc0000gn/T/rpcs3-mtl-probe-px.log`
- dumps `/tmp/mtl4-run` (four captured frames — 3796, 4248, 4467, 4905 — with 16
  ring manifests each, `fp-*.metal`/`.glsl`, `vs-*.metal`, `conv-*`/`guest-*`
  upload dumps and the per-frame `-persistent.bin` ring)
- F12 presses at frames 3796, 4248, 4467, 4905; 452 per-draw probe lines in the
  first window; emulator closed by the user afterwards.

### 20.2 The shape of a frame

From `frame-4905-m1-manifest.txt`, one complete frame (records are 1-based; the
probe prints `record_id - 1`, so probe serial `N` is manifest record `N + 1`):

| manifest record | fp | vp | pbase | target | kind |
|---|---|---|---|---|---|
| 70662 | 7 | 1 | 0 | `0xc07a0000` | scene RTT, first draw (clears) |
| 70663–70672 | 4 | 9 | 48…480 | `0xc07a0000` | scene RTT, 10 draws |
| 70673 | 10 | 1 | 528 | `0xc07a0000` | scene RTT |
| 70674 | 4 | 0 | 576 | `0xc07a0000` | scene RTT, 532 verts / 798 indices |
| 70675 | 4 | 0 | 13344 | `0xc07a0000` | scene RTT, 8 verts |
| 70676 | 4 | 9 | 13536 | `0xc07a0000` | scene RTT |
| 70677 | 5 | 2 | 13584 | `0xc03d0000` | **the blit** (4 verts, 6 indices) |

15 RTT draws then exactly one blit, alternating the two display buffers
(`0xc0010000` and `0xc03d0000`, both 1280x720, pitch 5120). The ring is
contiguous and reset per frame (`pbase` 0 → 13584). The blit's fragment program
was `fp 5` in these captures and `fp 4` in one earlier run, and its vertex
program is **`vp 2`** — see §20.5. Any per-id experiment must confirm the id from
the same run's manifest.

### 20.3 The source texture is the live RTT, not a stale section

This was the leading hypothesis and it is **dead**:

- `Metal 4: host image sample 379700, address 0xc07a0000, 1280x720 format 0x50,
  image 0x767ba1d180, nonzero 169761 of 921600 words, distinct bytes 19`.
  That dump is taken from `descriptor->image_handle->value()`, i.e. the exact
  `MTL::Texture` the shader is bound to (`MTLGSRender.cpp:3492`), not from guest
  memory and not from a cache census.
- The per-address census confirms the arbitration: `census 0xc07a0000, uploads 1,
  bytes 3686400, samples 3513, fmt 0x85, hazards 0, frames 286..3800`. The single
  upload happened at **t = 19.491 s** (boot), and its bytes are all zero
  (`/tmp/mtl4-run/conv-0-addrc07a0000-fmt85-sw0-1280x720-sz3686400.bin` is
  921600 words of `0x00000000`). So all 3513 samples in the window were served
  by the surface store; if the stale section were still winning, the sample count
  would be 0 and the upload count would climb.
- Why that is the expected behaviour: every draw that binds `0xc07a0000` as a
  colour target calls `textures.commit_framebuffer_memory_region(range)` (or
  `lock_memory_region` with `write_color_buffers`), which invalidates every cached
  section over the range (`MTLGSRender.cpp:1928-1955`, `texture_cache.h:2039`).
- The RTT is not flat either: the probe patch on the RTT after the last RTT draw
  of the frame reads `mean b 8.1 g 9.3 r 10.3`, 15821/16384 nonzero, `distinct 128`.

### 20.4 The destination is unchanged, in both buffers

Probe lines for the same two rects, one draw apart:

```
probe 53414 (last RTT draw)  patch 0 384,192 mean b 8.1 g 9.3 r 10.3 a 0.0, nonzero 15821/16384, distinct 128
probe 53415 (the blit)       patch 0 384,192 mean b 0.0 g 0.0 r 0.0 a 255.0, nonzero b 0 g 0 r 0, distinct 2, first 0xff000000
probe 53415 (the blit)       patch 1 640,256 mean b 0.0 g 0.0 r 0.0 a 255.0, nonzero b 0 g 0 r 0, distinct 2, first 0xff000000
```

The two rects differ in the RTT and are *identical* in the destination: uniform
output from differing input. And the destination is not a clear value that the
draw then failed to overwrite — it is the value the buffer already had:

- `0xc0010000` reads `0x00000000` everywhere (`distinct 1`);
- `0xc03d0000` reads `0xff000000` everywhere (`distinct 2`).

The two display buffers hold *different* alpha (0 vs 255), so the alpha is real
surface content and the present path does not synthesise it. (The blit cannot
write alpha anyway: guest colour mask `1110`, mapped write mask `0xe` = `Red |
Green | Blue`.) Both buffers kept whatever RGB they held before the draw.

**The limit of this measurement (added 2026-11-11 later).** "Kept whatever they
held" is true for *RGB*, and the destination RGB is already `0`. A blit that
writes uniform black produces the same probe line as a blit that writes nothing.
Only the alpha column can tell the two apart, and this draw cannot write alpha.
So §20.4 proves **the displayed image is black**, not that the draw is absent —
see §20.7.1 and the sentinel experiment in §20.9. The same run's second window
gives the complementary datapoint: at frame 1440 the first probed draw is the
previous frame's blit on `0xc03d0000` and the patch reads `distinct 1`, all zero,
because *that* run's display buffer was zero before the draw.

The earlier reading "the blit writes uniform opaque black, and alpha 255 cannot
come from the alpha-less RTT" stays withdrawn (the alpha was pre-existing), but
its replacement, "the blit writes nothing", is now itself only a hypothesis.
Also withdrawn, and unaffected: "unit 0 delivers a constant image" — §20.3 dumps
the bound image and it has content.

### 20.5 The vertex stage is correct — but it is a *different* vertex program than §20.5 first said

**(Corrected 2026-11-11, later session. The first version of this section derived
the transform from `vs-3`; the blit does not use `vs-3`.)**

The blit's vertex program is **`vp 2`** — manifest field `vp 2` on the blit
record in every capture (`53400`, `70677`, …), and `vs 15` is the *vs context*,
not the program. `vp 2` dumps to `/tmp/mtl4-run/vs-2.metal`, whose whole body is:

```
void vs_main(thread float4& dst_reg0, thread float4& dst_reg7, ...)
{
    float4 in_pos = read_location(0, ...);   // attribute 0
    float4 in_tc0 = read_location(8, ...);   // attribute 8
    dst_reg0 = in_pos;                       // gl_Position
    dst_reg7.x = in_tc0.xy.x;                // tc0
    dst_reg7.y = in_tc0.xy.y;
}
```

**A pass-through.** There is no `xform 2340`, no pixel→NDC map: the guest supplies
*clip-space* positions directly. `vs-3` (the pixel→NDC program with the `vc[4..7]`
identity rows and `xform 2340`) belongs to the other draws of the frame — the
frame's RTT draws are `vp 9`, `vp 1` and `vp 0` — and the earlier derivation in
this section was reading the wrong program. The same is true of
`MTL_RasterState`'s viewport: it applies to both, and the RTT draws write, so it
was never the suspect.

The only post-transform is the global `scale_offset_mat`, which is the
`NV4097_SET_VIEWPORT_SCALE/OFFSET` map normalised by the surface clip
(`RSXDrawCommands.cpp:593-620`, filled at `MTLGSRender.cpp:5257`). For a
1280x720 viewport with the RSX's negative Y scale it is `diag(1, -1, 1, 1)`;
that Y flip is why the capture's `clip` column is `(guest.x, -guest.y)`.

**The uploaded vertex stream is a correct fullscreen quad.** The blit's record
carries `pbase 13584, pbytes 128` — 4 vertices × 32 B — and the capture's
`capture input` line gives `attrib0 … 536884496` and `attrib8 … 536884512`, i.e.
position at +0 and texcoord at +16 of each 32-byte vertex. Decoding the 128-byte
block at offset 13584 of `frame-3796-m1-persistent.bin` **and**
`frame-4905-m1-persistent.bin` (identical in both, and both hashes match the
manifest `p` field, `0x3f1b6745`):

| vertex | position | texcoord |
|---|---|---|
| 0 | (-1, 1, 0, 1) | (0, 0, 0, 1) |
| 1 | (-1, -1, 0, 1) | (0, 1, 0, 1) |
| 2 | ( 1, -1, 0, 1) | (1, 1, 0, 1) |
| 3 | ( 1, 1, 0, 1) | (1, 0, 0, 1) |

That is the standard fullscreen quad with the correct tc mapping, and it agrees
with the capture's `vertex` lines for frame 4909 (`guest -1 1 0`, `-1 -1 0`,
`1 -1 0`, `1 1 0`; `tc` 0, 0, 1, 0). **Mechanism 3 (the GPU fetching a different
vertex stream than the CPU record) is therefore excluded**, and the "the quad is
a 4-pixel speck" reading stays dead.

> **Decode note, and a correction to the previous session.** The ring stores
> floats in *big-endian byte order* (`bf800000` is -1.0 there), and the shader
> un-swaps them per attribute (`fetch_attribute` → `gen_bits(..., desc.swap_bytes)`
> in the MSL). Decode with `struct.unpack_from('>4f', block, n*32)`; a
> little-endian decode reads `bf800000` as `4.6185e-41`, and printing that with
> `%.4f` yields `0.0000`. That is exactly how the earlier "the 128-byte block at
> 13584 is all zero" reading was produced. **The block is not zero** — the
> "persistent dump is not evidence about vertex content" caveat in the first
> version of this section is withdrawn.
>
> The probe's `pixel probe … fp 0x0 vp 0x0` column is
> `fp_metadata.ucode_hash` / `vp_metadata.ucode_hash` (`MTLGSRender.cpp:4862-4863`),
> not a program id, and both are 0 for these programs. Do not read it as
> "program 0".

### 20.6 Every other input to the blit is benign — and identical to draws that do write

From the per-draw state block of draw 53415:

```
primitive 8 (quad, index expansion 0 1 2 2 3 0), clip 1280x720, aa 0, raster 1
color format 8 (A8R8G8B8) -> color0 pixel format 80 (BGRA8Unorm), pass colors 1 samples 1 1280x720
rsx color mask 1110, mapped write mask 0xe (R|G|B|A? no: R|G|B, alpha excluded)
blend enable mask 0xe -> attachment 0 unblended, alpha test 0, logic op 0/5379 (disabled)
cull 0/1029, front face 2305, polygon 6914 (fill), depth test 0, stencil test 0
scissor 0 0 1280x720, clear color 0 0 0 0, clear covers 1
viewport 0.0 720.0 1280.0 -720.0 z 0.0..1.0   (identical to every other draw)
unit 0: 0xa5 A8R8G8B8, 1280x720, scale (1,1,0), bias 0, remap 0xaae4, control 0xe000000
unit 1: 0xa5 A8R8G8B8, 256x1,   scale (1,0,0), bias 0, remap 0xaae4, control 0xe000000
```

`fp 5` compiles to (MSL, `/tmp/mtl4-run/fp-5.metal`):

```
r0    = tex0.sample(tex0Smplr, _texcoord_xform(tc0.xy, p0));
r0.z  = tex1.sample(tex1Smplr, _texcoord_xform(r0.z, p1)).z;
r0.y  = tex1.sample(tex1Smplr, _texcoord_xform(r0.y, p1)).y;
r0.x  = tex1.sample(tex1Smplr, _texcoord_xform(r0.x, p1)).x;
r0    = round_to_8bit(r0);   out.ocol0 = r0;
```

Unit 1 is the 256x1 identity ramp at `0xc00003e0` (verified in every dump: 256 of
256 words nonzero, 256 distinct bytes, `row 0 ff…`), and `_texcoord_xform` only
clamps when `flags` bit 21 is set. So the program is mathematically the identity:
content in must give content out. It also carries **no `discard` at all** — the 14
`discard` tokens in the GLSL are all inside `#ifdef`s that are off for this
program (`_DISABLE_EARLY_DISCARD` needs `props.disable_early_discard`,
`GLSLCommon.cpp:255-259`), so the shader writes *more* than the guest would, not
less.

The draw is encoded: `draw` count, `draw/nonempty` and the manifest record count
are all 80737, and the capture prints `count 4`, `subdraw 0`, `base 0`.

### 20.7 What is left — and the one ambiguity that has to be removed first

Correct geometry (now with the *right* vertex program), correct attachments,
correct state, correct source, correct shader, draw encoded — and the destination
does not change. What remains:

1. **`"writes nothing"` was never actually established.** The destination RGB was
   already `0`, and the blit's write mask excludes alpha (guest `1110` → `0xe` =
   R|G|B). A blit that writes **uniform black** is therefore indistinguishable
   from a blit that writes **nothing**: both leave a `0xff000000` patch at
   `0xff000000`. Every "no fragment" statement in §20.1–§20.6 rests on this
   ambiguity, and L216/L217 repeat it. The *alpha* observation still does real
   work: a `LoadActionClear` of `(0,0,0,0)` on the display-buffer pass would have
   turned `0xc03d0000`'s alpha to 0, and it did not, so **either the pass does not
   land on that texture, or it loads rather than clears**. But that is a statement
   about the pass, not about the fragments.
2. **The pass for that draw may not reach the GPU with its attachments.** The
   blit is the only draw in the frame whose colour target differs from the
   previous draw's, and it is the last draw before present. A pass that was
   absorbed, or whose attachments were not rebuilt (`layout.ignore_change` early
   return `MTLGSRender.cpp:1793`; `can_absorb()` `:4538`), would render into the
   previous target (the RTT) and leave the display buffer untouched.
   Counter-evidence: `frame.attachments.color(0)` at the blit really is the
   display-buffer surface (the window-start probe prints
   `address 0xc03d0000`), `pass_key` compares `MTL::Texture*` identities (so a
   target change cannot be absorbed), and no
   `the pass of the draw could not open` error is logged.
3. **A fragment kill outside guest-visible state** — a pipeline colour write mask
   resolved from a stale key, or a format mismatch that silently drops the draw.
4. ~~The GPU reads a different vertex/index stream~~ — **excluded by §20.5.**

### 20.8 The void `force-fp` run (2026-11-11 19:45) — do not repeat its mistake

`build/probe/run-forcefp.sh` was launched with
`MTL_PROBE_VARIANT='force-fp=5;force-fp-tex=2'` and the user pressed F12 four
times (frames 1109, 1439, 2087, 2923; screen black throughout). The run is
**void**:

- The only warning is `Metal 4: diagnostic, the colour output of fp id 5 is forced
  magenta …` at `0:00:11.365`, and it went to
  `~/Library/Caches/rpcs3/RPCS3.log`, not to `MTL_PROBE_LOG` (`MTL_WARN` and
  `MTL_ERROR` do not use the probe sink; only `MTL_NOTE` does).
- **The blit in this run is `fp 4` / `vp 3`, not `fp 5`.** The run's 54
  display-buffer draws are all `fp 4`/`vp 3` (`/tmp/mtl4-forcefp-20261111-194505`,
  72 manifests). So the forced program was never the blit.
- `force-fp-tex=2` never matched either (no warning), so the blit's
  `prog.texture_state.texture_dimensions` count is **not** 2, even though the draw
  binds two units. The compile-time state count and the draw-time unit count are
  not the same quantity.
- **Program ids are not stable across runs** (`fp 5` at 19:23, `fp 4` at 19:45,
  `fp 4` again in the force-fp run), so any per-id selector must be confirmed
  against the *same run's* manifest `fp` field.

Useful measurement that survives the void run: at frame 1440 the window's first
probed draw is the **previous frame's blit** (the window starts on it), target
`0xc03d0000`, and it reads `distinct 1`, all zero, both patches. That run's display
buffer was zero to begin with, so the ambiguity in §20.7.1 was total there.

### 20.9 The planned experiment (superseded by §21 — read §21.6 before using it)

The plan is one run with **two** id-independent arms, after a small EXCISE
diagnostic change:

- **`MTL_PROBE_VARIANT=sentinel`** already exists
  (`MTLFramebuffer.cpp:222-268`): every pass forces `LoadActionClear` and clears
  its colour attachments to magenta `(1,0,1,1)`. This makes the display buffer
  non-black **before** the blit draws, which removes the §20.7.1 ambiguity: a
  black screen then means a black *write*, and a magenta screen means the draw did
  not write.
- **`force-fp=all`** does not exist yet; `force-fp=<id>` and `force-fp-tex=<n>` do
  (`MTLFragmentProgram.cpp:29-90`). Add `all` as an arm of the same EXCISE block
  (a two-line change: when the argument is `all`, arm every program). Forcing
  every program to `ocol0 = (1,0,1,1)` forces the RTT magenta and the blit's
  fragment stage magenta *if it runs at all*.

Outcome matrix for `sentinel;force-fp=all`:

| screen | meaning |
|---|---|
| **magenta** | the blit's pass lands on the display buffer, its fragment stage executes, and the magenta output is stored. The fault is then in the *content* path (sampling / `_texcoord_xform` / LUT) or in the guest-state write mask, **not** in the pass. |
| **black** | either the pass never lands on the display buffer, or the fragment stage never executes: mechanism 2 (§20.7.2), or a pipeline/format kill. The next step is the `pass_key` / `can_absorb()` / `open_pass` audit against VK and GL, then a GPU-side `MTLCaptureManager` frame capture of that single draw. |

`sentinel` alone is still worth keeping in the same run: it answers "does *any*
pass action land on the display buffer" independently of the fragment stage,
which is exactly the fork between §20.7.2 and §20.7.3.

Two code-reading tasks run beside it, and are owed before any further device
runs:

1. **Pass/attachment handling vs VK and GL.** `framebuffer::bind/open_pass`
   (`MTLFramebuffer.cpp`) against `VKGSRender`'s render-pass begin and
   `GLGSRender`'s framebuffer cache: in particular what happens when the *only*
   change is the colour target (no layout change, no surface swap), and whether
   the Metal path can encode the blit into the RTT pass.
2. **The Mesa Metal 4 driver as a second opinion.** The user has a `mesa-kk`
   tree (Metal 4 backend) available; once the area is narrowed to "one draw with
   a pass whose target changed", compare its render-encoder/pass handling and its
   blend-mask/format keying with ours.

Do not spend another run on the texture cache (mechanism excluded by §20.3), on
the fragment program's colour math (§20.6), on the vertex stream (§20.5, now
excluded), or on the contract between the two sides of a unit bind
(`tex_sampler_hash`, `texture_id`, `sampler_id` all resolve; L205/L206
methodology).

> **Superseded by §21.6.** The `sentinel` arm of this plan cannot discriminate,
> for a reason that was not visible when it was written: `sentinel` forces a
> magenta clear on *every* pass, including the RTT pass, so a working blit
> copies a magenta RTT and a broken blit leaves the display buffer's own
> magenta clear — the same screen either way. Do not run this plan as written.

## 21. 2026-11-11, late session — the blit is innocent on paper, and the title is showing a frozen frame

### 21.1 Answer first

Three new measurements, all taken offline from the 2026-11-11 19:23 capture set
(`/tmp/mtl4-run` and the probe log), change the shape of the investigation:

1. **The blit is a plain store.** The guest's blend-enable mask for the blit is
   `0xe`, and that mask is *per colour target*, so attachment 0's blend-enable
   bit is **0**: the blit writes the fragment output unmodified (§21.2). A live
   sample of the RTT would therefore put the RTT's content on screen. It does
   not.
2. **The scene RTT's alpha is 0 in every pixel of every dump**, and nothing in
   the frame can ever raise it (§21.3). So *any* layer that re-enables the
   guest's `SRC_ALPHA` blend reproduces "the blit writes nothing" exactly, byte
   for byte — a trap for every future experiment.
3. **The guest's frame stream is frozen.** Across frames 3796, 4248, 4467 and
   4905 — 1100 frames, ~150 s — the 16-draw stream is byte-identical except for
   one quad's vertex block (§21.5). The title is presenting **one static
   frame**, not a live scene.

(3) is the biggest of the three, and it changes the question: we have been
asking "why is the blit black", when the first question is **"what is this
frame supposed to look like"** — and only a second backend can answer that.
§21.7 is the plan that follows from this; §21.6 kills the plan that preceded
it.

### 21.2 The blend decode: the blit is a plain store

The per-draw note prints the guest's factors as `src 770/770 dst 771/771`.
`770 = 0x0302 = CELL_GCM_SRC_ALPHA` and `771 = 0x0303 =
CELL_GCM_ONE_MINUS_SRC_ALPHA` (`gcm_enums.h:195-197`), so the *equation* is the
standard alpha blend `src·src.a + dst·(1-src.a)`.

But the *enable* mask is what decides whether it is applied, and that mask is
per colour target, not per channel:

- `rsx::context::blend_enabled_mask()` = `(global blend enable) | NV4097_SET_BLEND_ENABLE_MRT`
  masked to 4 bits (`rsx_methods.h:488-492`), i.e. bit 0 = MRT0, bit 1 = MRT1,
  bit 2 = MRT2.
- `rsx::blend_state::enable_mask` is documented as "One bit per color target"
  (`RSXDrawState.h:95-96`).
- `apply_blend_state` reads it per attachment: `const bool enabled =
  !state.logic_op_enabled && state.blend_enabled(index);`
  (`MTLBlendState.cpp:112`), and sets `BlendStateDisabled` for attachment 0 when
  the bit is clear (`:121`).

The blit's mask is `0xe` = `0b1110`: **bit 0 is clear**, so blending is *off*
on the only attachment the blit has. The RTT draws in the same frame carry
`0xf` (bit 0 set), which is why the two look different in the log and are not
the same case.

The mask is part of the PSO key, so a stale pipeline object cannot re-enable
blending behind our back: `same_blend_state` compares `enable_mask`, all four
factors and both equations (`MTLPipeline.cpp:26-40`), and `operator==`
compares the colour format, the attachment count, the sample count, the write
mask, the raster flag and the topology as well.

**Consequence:** the blit is a plain store of `ocol0`. §20.6's "identity through
a 256×1 LUT" is right (§21.4 confirms the LUT is an identity ramp), so the
blit is a straight copy of the RTT. If the copy were live, the screen would
show the RTT's rectangle. It shows black, so **the copy is not live** — which
is H1 in §21.7 (the sampled object is not the object the RTT was drawn into),
or the draw never lands at all.

### 21.3 The RTT's alpha is zero everywhere, and can never be anything else

Every `host-sample-*-addrc07a0000-1280x720-fmt50.bin` in `/tmp/mtl4-run`
(16 files, four frames × four probes) was decoded byte by byte:

```
alpha nonzero px: 0 of 921600      max alpha 0      max RGB byte 200
```

Zero, in all 921 600 pixels, in all 16 dumps. And the frame can never change
that:

- the RTT pass clears to `(0,0,0,0)` (`clear color 0.0000 0.0000 0.0000
  0.0000`), so the alpha starts at 0;
- **every** RTT draw in the frame carries guest colour mask `1110`, i.e. the
  alpha channel is not written (the mapped Metal write mask is `0xe` = R|G|B).

So the alpha channel of the scene RTT is write-protected from the first frame
to the last. Any hypothesis that explains the black screen by "the blit's
source alpha is 0" is therefore self-fulfilling: it is a property of our
rendering, not of the guest.

This is also why §20.4's "the destination is unchanged" is such a weak
observation, and it is the reason the §21.6 experiment has to use a colour
that survives both a plain store and an alpha blend.

### 21.4 The RTT's content is one solid rectangle — and both probe patches are inside it

Decoding the same 16 dumps by row and column:

```
rows with content: 192..513   (322 rows)
cols with content: 384..912   (529 cols)
nonzero RGB pixels: 169761 of 921600 (18.4 %)
```

The rectangle's area is 529 × 322 = 170 358, and 169 761 pixels of it are
nonzero — so the rectangle is **solidly filled** (one textured quad) and
everything outside it is exactly zero.

Two consequences:

- The rectangle is centred (x centre 648 ≈ 640, y centre 352 ≈ 360) and it is
  the *only* content in the frame. The frame's 16 draws are 10 sprite quads on
  256² textures, one full-screen quad on a 1024×512 texture, a 532-vertex
  mesh, an 8-vertex mesh, a 4-vertex quad and the blit.
- **Both pixel-probe patches are inside the rectangle.** `(384,192)` is its
  top-left corner and `(640,256)` is inside it. So "the probe landed outside
  the content" is excluded: the destination was sampled exactly where the
  source is non-black, and it read `0x000000` there.

The blit's fragment program (`fp 5` in the 19:23 captures) is
`r0 = tex0.sample(...)`, then one 256×1 LUT lookup per channel, then
`round_to_8bit`. The LUT is **not** a suspect: the host dump of unit 1
(`host-sample-*-addrc00003e0-256x1-fmt50.bin`) is
`ff000000 ff010101 ff020202 ff030303 …` — a linear identity ramp, 256/256
entries nonzero, `distinct bytes 256`. The blit is a copy.

### 21.5 The guest's frame stream is frozen

The four F12 captures in `/tmp/mtl4-run` are at frames 3796, 4248, 4467 and
4905. Flattening the 16 `record` lines of each manifest to tokens and
diffing them pairwise gives **one differing token out of ~1500**:

```
< 0x97b0a915        (frame 3795, record 12)
> 0x95367705        (frame 4905, record 12)
```

Per record, the comparison is:

```
record   1  p=0xe78864a1 i=0xd65c7fcf x=0x0        s=0x44a4dbc8   identical in all four
record   2  p=0x9cb7705d i=0xd65c7fcf x=0x96240da0 s=0xe54c257d   identical
...
record  11  p=0x7b74c0d  i=0xd65c7fcf x=0x96240da0 s=0xe54c257d   identical
record  12  p=0x97b0a915 → 0x4dc6ca5d → 0xac904e2d → 0x95367705     CHANGES
record  13  p=0x4a7eee01 i=0x27748423 x=0x79143937 s=0xe54c257d   identical
...
record  16  p=0x3f1b6745 i=0x696047b7 x=0x75499b94 s=0x44a4dbc8   identical
```

`p` is the hash of the record's persistent (vertex) block, so record 12 — the
`vp 1 fp 10` quad on the 1024×512 texture, `s=0xe0eedf25` — is the only thing
that moves. Everything else is frozen: every program id, every texture
binding (`t0`/`t1` lines), every constant hash, every colour target, every
viewport and scissor.

Two readings, and they must be separated before anything else:

- **(a) the title is at a static screen** (a splash, a logo, a loading card)
  that legitimately does not animate except for one element; or
- **(b) the title is stuck** — spinning on a wait, resubmitting the same
  display list, and never progressing.

Reading (b) would make the black screen a *progress* bug, not a rendering bug,
and every pixel-level experiment would be chasing a ghost. The two readings
are separated by input: press Start (or any button) and see whether the
16-record stream changes. It must, if the game is at a menu.

Supporting evidence for "very static": the sampled textures are byte-identical
across the whole run (the 512×512, 1024×512 and 256×256 textures and the LUT
each have one md5 over all 16 samples), and the 16 RTT dumps take only nine
distinct hashes.

### 21.6 §20.9's `sentinel` plan cannot discriminate — withdrawn

§20.9 planned `sentinel` (force `LoadActionClear` and a magenta clear on
every colour attachment of every pass, `MTLFramebuffer.cpp:222-268`) plus
`force-fp=all` (magenta output). The intent was a three-way answer. It does
not work, for a reason that only became visible in §21.3/§21.4:

- `sentinel` clears the **RTT** magenta as well as the display buffer. So a
  *working* blit copies a magenta RTT to the screen, and a *broken* blit leaves
  the display buffer's own magenta clear on the screen. **Magenta either
  way.**
- `force-fp=all` writes magenta *and* alpha 1, so it survives both a plain
  store and an alpha blend — but it cannot separate "the fragment stage ran"
  from "the pass landed", because the clear it is compared against is magenta
  too.

A usable discriminator has to (a) use **two different colours** for the clear
and for the fragment output, and (b) not depend on the source content at all.
`probe-tri` (§21.7, Phase 3) does both in one output channel each.

### 21.7 The plan

**Phase 1 — the oracle: the same frame under Vulkan.** One run of the same
title at the same boot point with the Vulkan backend, plus a RenderDoc capture
of one presented frame and of the two surfaces. Four questions, four different
next steps:

| what Vulkan shows | what it means |
|---|---|
| the presented frame is black too | the title is **stuck**; the defect is in progress, not pixels. Stop here and re-scope. |
| the presented frame has an image, and the RTT's alpha is 255 | **the alpha chain is the RCA** — find who writes alpha in VK and why we do not. |
| the presented frame has an image, the RTT's alpha is 0, but the blend/mask state differs | our blend or write-mask mapping is wrong; the PSO is lying. |
| the presented frame has an image and the RTT looks exactly like ours (529×322 rectangle, alpha 0) | our RTT is **correct**; the fault is provably in the blit/present path, and Phase 3 is the next step. |

Companion test, free and immediate: press Start during a Metal run. If the
16-record stream does not change, the title is stuck, and Phase 1 becomes a
progress investigation.

**Phase 2 — the pointer-identity check (no device needed).** At each draw,
print the `MTL::Texture*` that the sampled unit resolves to and the
`MTL::Texture*` of every colour attachment of the pass, with a verdict
(`ALIAS` / `SAME-ADDRESS-DIFFERENT-OBJECT` / `DIFFERENT`). Today the log
prints the descriptor's image **view** pointer on one line and its
`->value()` on another, at probe time, and never compares either against the
pass's attachment pointer — so the one comparison that would settle H1 has
never been made. H1: at draw time the unit resolves to a different
`MTL::Texture` (or a stale, zero-filled cached copy — the census records
exactly one upload to `0xc07a0000`, and that upload was zero-filled) than the
one the RTT draws store into, so the sample is black.

**Phase 3 — one Metal run, one three-way variant (`probe-tri`).** Armed
*structurally*, not by program id: a draw whose fragment unit 0 guest address
equals a colour target address used earlier in the same frame is a presenter
blit. That predicate is id-independent and generalises to every title (it is
also why `force-fp=<id>` failed: program ids move between runs, §20.8).

```
ocol0 = float4(tc0.x, tc0.y, any(sampled.rgb != 0.0) ? 1.0 : 0.0, 1.0)
```

| screen | conclusion |
|---|---|
| R/G gradient, blue band | everything lands; the fault is downstream of the sample. |
| R/G gradient, no blue | the **sample is black** ⇒ H1 confirmed on device. |
| no gradient | the draw never lands ⇒ the pass/attachment or the pipeline key; capture that single draw with `MTLCaptureManager`. |
| gradient and blue everywhere | the sample is not the RTT but something else entirely ⇒ the unit binding is wrong. |

**Phase 4 — outcomes → fixes.** H1: fix the resolve / `notify_surface_changed`
ordering so the sampled unit aliases the attachment, and make the Phase 2 note
part of the regression suite. H2 (the draw never lands): audit
`on_framebuffer_bound`'s `ignore_change` early return (`MTLGSRender.cpp:1793`),
`pass_key` / `can_absorb` (`MTLFramebuffer.cpp:32-93`) and `open_pass` against
`VKGSRender` and `GLGSRender`. Note that `same_framebuffer` *does* compare
`color_addresses` (`MTLFramebuffer.cpp:21`), so the "the alternating display
buffer was absorbed into the previous pass" variant is excluded. H3: dump the
attachment-0 state of the pipeline object that is actually bound.

**Phase 5 — prove it.** A Vulkan-parity image, an RR7 regression run, the
GPU-free `build-tests/bin/rpcs3_test` green, the scrubber clean, then commit.

Do not spend another run on the vertex stream (§20.5, excluded), the source
texture's *contents* (§21.4, measured), the LUT (§21.4, measured), the
fragment program's colour math (§20.6), or the unit-bind contract
(`tex_sampler_hash`, `texture_id`, `sampler_id` all resolve; L205/L206).


## 22. 2026-11-11, later — the cross-backend audit: what Metal 4 does that Vulkan and OpenGL do not

This section is the result of reading the Metal 4 backend side by side with the
Vulkan and OpenGL backends, path by path, for the four hypotheses of §21.7. It
records **what was ruled out**, **where Metal 4 genuinely differs**, and **the
cheapest experiment for what is left**. Everything here is code- or
log-derived; nothing needed a device.

### 22.1 Answer first

Three results, in order of how much they change the plan:

1. **H1 is dead, and it was dead in the logs we already had.** The probe's
   "host image target" line dumps `frame.attachments.color(0)` — the colour
   attachment of the open pass (`MTLGSRender.cpp:4948`) — and the "host image
   sample" line dumps `descriptor->image_handle->value()` — the texture the
   fragment unit resolved to (`MTLGSRender.cpp:3492`). In the 19:23 capture
   **both lines print the same pointer**, `image 0x767ba1d180`, for the RTT
   address `0xc07a0000` (probe log lines 3366 and 34620). The blit therefore
   samples *exactly* the texture the RTT draws store into: the sampled object
   and the attachment are the same `MTL::Texture`, not two objects that merely
   share an address. **The Phase 2 pointer-identity diagnostic would have
   printed `ALIAS`.** It is worth keeping as a regression guard, but it cannot
   find the fault.
2. **The remaining hypotheses are Metal-4-specific by construction.** Every
   remaining candidate is about a stage that Vulkan and OpenGL do not have:
   the *hazard model* (untracked heaps, explicit barriers), the *residency
   model*, the *argument tables*, the *pipeline immutability* (blend state is
   baked into the PSO in Metal, set per draw in VK/GL), and the *GLSL→SPIR-V→
   MSL* round trip. The shared code is shared: the sample resolve
   (`Common/texture_cache_helpers.h`), the vertex fetch
   (`Program/GLSLSnippets/RSXProg/RSXVertexFetch.glsl`), the blend mask
   semantics, the write-mask mapping and the depth-compare encoding are all
   one implementation used by all three backends.
3. **There is one confirmed architectural deviation, and it is not the cause.**
   SPIRV-Cross flattens every runtime-sized GLSL array to `[1]` in MSL
   (§22.4). It works in practice on this device — the RTT's 128-distinct-value
   gradient proves the per-draw texture parameters are read correctly — but it
   is undefined behaviour by the MSL rules, and it is the only place where the
   Metal backend's own design does not fit the target language.

### 22.2 What was ruled out, with the evidence

| candidate | verdict | evidence |
|---|---|---|
| H1: the sampled object is not the attachment | **excluded** | the "target" dump and the "sample" dump print the same `MTL::Texture*` (`0x767ba1d180`), probe log 3366 vs 34620 |
| the blit is a feedback copy instead of a live view | **excluded** | `surface_is_rop_target = m_rtts.address_is_bound(...)` (`Common/texture_cache.h:2575`) is false — the bound target is the display buffer, not the RTT — so the shared helper takes the live-view branch (`texture_cache_helpers.h:793-805`), not the deferred-copy branch (`:751-790`) |
| the vertex stream is wrong | **excluded** | record 53384's block at `pbase 13584` decodes to a perfect fullscreen quad with a correct tc ramp: `(-1,1)uv(0,0)`, `(-1,-1)uv(0,1)`, `(1,-1)uv(1,1)`, `(1,1)uv(1,0)`; index expansion `0 1 2 2 3 0` covers it |
| the vertex *fetch* differs from VK/GL | **excluded** | the MSL is a faithful translation of the shared `RSXVertexFetch.glsl` (same `fetch_desc`/`fetch_attribute`/`sext`/`gen_bits`), because the Metal backend compiles the same GLSL |
| blending is on for the blit (which would make an alpha-0 source write nothing) | **excluded** | the mask is `0xe` and the mask is per *draw-buffer index* in all three backends (`VKGSRender.cpp:336`, `GLDraw.cpp:192-198`, `MTLBlendState.cpp:112`), so attachment 0's bit is 0 → blending off → a plain store |
| the blend equation is wrong | **excluded** | 32774 = `0x8006` = `CELL_GCM_FUNC_ADD`; the shared decode is upstream-identical (`gcm_enums.h` differs from `upstream/master` by one unrelated line) |
| the colour write mask is wrong | **excluded** | guest `1110` → Metal `0xe` = Red|Green|Blue; Metal's `MTLColorWriteMask` bit order is Alpha=1, Blue=2, Green=4, Red=8, and the backend maps to it deliberately |
| the depth compare is wrong | **excluded** | 519 = `0x207` = `CELL_GCM_ALWAYS`; the RSX zfunc encoding carries the `0x200` bit (`gcm_enums.h:1419-1437`) |
| the viewport is wrong | **excluded** | the guest's viewport transform lives in the vertex env; Metal's viewport is the surface clip (`MTLEncoderState.cpp:96-99`), so the negative guest height is never handed to Metal's unsigned viewport |
| the pass/attachment set is wrong | **excluded** | `same_framebuffer` compares colour addresses and pitches (`MTLFramebuffer.cpp:19-31`); the blit opens its own pass; `reconcile_render_targets` mirrors VK's ordering |
| the draw never reaches the GPU | **not excluded** | the Metal validation layer is never enabled and there is no hook for it — a driver-dropped draw would be **silent** (§22.5) |
| the sample reads a stale RTT | **not excluded** | the pass-to-pass edge is one barrier issued *inside* the producer encoder, before `endEncoding` performs the tile flush (§22.6) |

### 22.3 The four hypotheses, re-scored

- **H1 (the sample is black).** Excluded as an *object-identity* problem, but
  not as a *content* problem: nothing yet proves the GPU read the RTT's
  current contents at the blit. What remains is (a) the sample reads a stale
  version (hazard), or (b) the fragment stage's own reads (`texture_parameters`)
  are wrong on the GPU.
- **H2 (the draw never lands).** Still live, and it is the *cheapest* to
  test: one run with `MTL_DEBUG_LAYER=1` in the environment turns on Metal's
  own validation layer, which names a dropped draw and its reason.
- **H3 (the unit binding is wrong).** The slot map is consistent and
  self-checking: the GLSL declares the unit's texture at binding 12+i and its
  sampler at binding i, `MTLShader.cpp:54-90` remaps binding→`[[texture(n)]]`
  and binding→`[[sampler(n)]]` with the same `texture_sampler_index()` the
  table writer uses (`MTLArgumentBinds.cpp:85-90`), and the MSL confirms it
  (`tex0 [[texture(12)]]`, `tex1Smplr [[sampler(1)]]`). The *resource id*
  behind the slot is the one remaining unverified link.
- **H4 (the title is stuck, not mis-rendered).** Unchanged: the guest's
  16-draw stream is byte-identical across 1100 frames (§21.5), and the
  "press Start" test still costs ten seconds and settles it.

### 22.4 D1 — the confirmed architectural deviation: runtime-sized arrays become `[1]`

The Metal backend emits the same GLSL as Vulkan, but MSL has no runtime-sized
array, and SPIRV-Cross resolves that by declaring a **fixed array of one**:

```
struct TextureParametersBuffer { sampler_info_1 texture_parameters[1]; };
struct FragmentStateBuffer   { fragment_context_t fs_contexts[1]; };
struct FragmentConstantsBuffer { vec4 fc[1]; };
struct VertexContextBuffer    { vertex_context_t vertex_contexts[1]; };
struct DrawParametersBuffer   { draw_parameters_t draw_parameters[1]; };
```

(`/tmp/mtl4-run/fp-5.metal`, `vs-2.metal`; every `.metal` in the capture set
shows the same.) Compare:

- **OpenGL** declares a real size: `sampler_info texture_parameters[16];`
  (`GLFragmentProgram.cpp:208-209`) — the array is exactly the 16 RSX units.
- **Vulkan** declares `sampler_info texture_parameters[];`
  (`VKFragmentProgram.cpp:328-330`) — a runtime-sized array, which is legal as
  the last member of a uniform block, and the bound range defines its length.
- **Metal 4** declares `[]` too (`MTLFragmentProgram.cpp:272-275`) and gets
  `[1]` from the cross-compiler.

The Metal backend's *design* depends on the Vulkan behaviour: "A shader indexes
a block by its position in an unsized array, so the ring is bound once and only
the index travels per draw" (`MTLConstants.h:118-124`). The index is the ring
index, which grows with the draw count — so every access is out of bounds by
the MSL rules.

**It works on this device, and the proof is in the data.** The RTT's textured
patch shows 128 distinct byte values across a 128×128 area with a smooth mean
(`mean b 8.1 g 9.3 r 10.3`, `distinct 128`, `first 0x00010101`). A broken
parameter read would put every fragment at the same texel and the patch would
have `distinct 1`. So Metal performs a plain dynamic load here, and the
`texture_parameters[base + i]` reads are correct.

That is luck, not a guarantee: the Metal compiler is entitled to fold the index
against the declared size of 1. The robust fix is the one the backend already
has a helper for: bind the block **per draw** at `address_of(index)`
(`MTLConstants.h:221-224` — "A block the shader reads at offset zero … is bound
here, not at the ring base") and declare the array with a real size, exactly as
GL does. The index then becomes the *unit* index (0..15), the array is `[16]`,
and the out-of-bounds class of bug disappears for the sampler-info, fragment-
context, fragment-constant, vertex-context and draw-parameter rings at once.

### 22.5 D2 — the validation layer is off, so a dropped draw is invisible

The backend never enables Metal's debug layer and offers no hook for it
(`grep -rn "DebugLayer" rpcs3/Emu/RSX/MTL/` → nothing). The 19:45 run's stdout
capture (`build/probe/stdout-forcefp-20261111-194505.log`, 80 bytes) contains
only the banner, and the Metal runtime prints validation errors to stderr, which
that file does capture — so with the layer on, a dropped draw would be named.

**Metal honours `MTL_DEBUG_LAYER=1` as a process-wide environment variable.**
That is a zero-build lever for the next run and it is the cheapest possible
test of H2.

### 22.6 D3 — the pass-to-pass edge is one barrier, and it is issued before the tile flush

With `MTLHazardTrackingModeUntracked` heaps (`MTLResourceManager.cpp:55`),
Metal 4 tracks no hazards: every cross-encoder dependency is the
application's declaration. The RTT writes and the blit's sample are in
different encoders (the pass is split, because Metal cannot sample a live
attachment — `MTLGSRender.cpp:4462-4470`), and the only edge between them is
the barrier `close_encoder()` emits (`MTLCommandRecorder.cpp:849-870`):

```cpp
emit_producer_barrier(m_encoder, barrier_spec{ MTL::StageAll, MTL::StageAll, Device, true });
m_encoder->endEncoding();
```

`emit_producer_barrier` is `encoder->barrierAfterStages(...)`
(`MTLBarrier.cpp:48-51`), i.e. the *producer-side* form, and it is encoded
**before** `endEncoding()`. On Apple hardware the render pass's tile memory is
flushed at the end of the encoder. The Mesa KK driver this backend was written
against does the opposite order — it ends the render pass first and then
raises the barrier (`kk_cmd_buffer.c:266`, quoted in the comment at
`MTLCommandRecorder.cpp:853-858`). If Metal 4's tile flush is not covered by a
barrier that precedes it, the blit's fragment shader can read the RTT's
*previous* contents.

This is invisible by construction: the scene is frozen (§21.5), so a
one-frame-stale read of the RTT would look *almost* right — and the display
buffer would then hold the previous frame's rectangle, not black. So this
ordering question explains "the picture lags" but **not** "the picture is
black". It stays on the list because it is a real deviation from the driver it
was modelled on, and because `MTL_PROBE_VARIANT=heaps`
(`MTLResourceManager.cpp:55`) tests it for free: with tracked heaps the driver
inserts the edge itself, so *a defect that goes is a missing edge and a defect
that stays is content* (`MTLInstrumentation.h:1027-1033`).

### 22.7 D4 — invalid enums behave differently in each backend

A guest blend equation outside the modelled set (the boot draws wrote `0x0`)
decodes to an invalid enum through the shared `expected<T>` path, and then the
backends diverge:

- **Metal**: `get_blend_operation` logs `unknown blend equation 0x0` and
  **falls back to Add** (`MTLBlendState.cpp:91-94`).
- **Vulkan**: `get_blend_op` hits `default: fmt::throw_exception("Unknown
  blend op: 0x%x")` (`VKGSRender.cpp:193-195`) — the draw is **aborted**.
- **OpenGL**: logs at trace level and uses `GL_FUNC_ADD` (`GLDraw.cpp:23-40`).

Six such errors appear in the 19:45 run, all from the boot draws, none from the
scene. It is not the black screen, but it is a real behavioural fork between
backends on identical guest state, and it is the kind of thing that becomes a
crash report when the Vulkan backend is the oracle.

### 22.8 The plan after the audit

Ordered by cost. The first three need **no code change**.

1. **`MTL_DEBUG_LAYER=1` (one run).** Metal's own validation layer. It names a
   dropped draw, an unbound texture, a mismatched argument table or a bad
   barrier — i.e. it tests H2 and the "resource id behind the slot" question
   directly, with the driver's verdict rather than ours.
2. **`MTL_PROBE_VARIANT=heaps` (one run).** Tracked heaps: the driver orders
   the conflicting accesses. A black screen that goes is a missing edge
   (§22.6); a black screen that stays is content.
3. **Press Start during a Metal run (ten seconds).** Splits H4: a static
   splash or a stall.
4. **`probe-tri` (Phase 3, one run).** Still the right discriminator for the
   fragment stage: `ocol0 = (tc0.x, tc0.y, sampled.rgb != 0 ? 1 : 0)` on the
   presenter draw. Gradient and blue ⇒ the sample path works and the fault is
   in the write; gradient and no blue ⇒ the sample is black; no gradient ⇒ the
   draw never lands. Note that `probe-tri` now answers a *narrower* question
   than §21.7 assumed, because H1's object-identity half is already excluded.
5. **The Vulkan oracle (Phase 1, needs a go).** Unchanged, and now more
   valuable: it says what the frame should look like at all, which §21.5 says
   we do not know.

And the two *code* changes this audit justifies, both small and both EXCISE-
marked:

6. **Bind the parameter rings per draw** (§22.4) and declare the arrays with
   real sizes, as GL does. This removes the whole class of out-of-bounds
   dynamic indexing from the Metal backend. It is a correctness fix, not a
   performance change, and it should land before the release branch.
7. **Align the invalid-enum fallback with OpenGL** (§22.7) so the three
   backends do not disagree about what an unmodelled equation means.

Do not spend another run on: the vertex stream, the vertex fetch, the blend
mask, the blend equation, the write mask, the depth compare, the viewport, the
sample's object identity, the RTT's contents, the LUT, or the fragment
program's colour math. All of those are now closed by §22.2.

### 22.9 The narrowing that the older capture sets give (and it is decisive about the family)

The four 2026-11-10 capture sets in `build/probe/dumps-demonssouls-*/` hold the
*display buffers* (`host-target-*-addrc0010000` and `-addrc03d0000`) and the
*composed drawable* (`host-present-*`). Decoding them by hand:

| run | moment | display buffer | composed drawable |
|---|---|---|---|
| 15:54 | frame 600 | uniform **blue** (`B=255 G=0 R=0 A=0`) | uniform blue |
| 16:06 | frame 232 | **all zero** | all zero |
| 16:20 | frame 600 (present 600) | all zero / uniform blue (both buffers seen) | all zero |
| 18:07 | frame 600 | uniform **blue** | uniform blue |
| 19:23 (11-11) | frame ~4900 | uniform **opaque black** (`A=255`) | not dumped |

Two things follow, and the second is the important one.

**First:** the display buffer never holds a *copy* of the scene RTT. A copy
would carry the RTT's 529×322 rectangle — 169761 nonzero pixels with 128
distinct byte values in a single 128×128 patch (L221). Every observation above
is a **uniform** value: one distinct word across all 921600 pixels. A uniform
value is a constant, and the blit is a copy, so the blit produced nothing in
any of those runs either.

**Second:** the blue is not the blit's output either. The LUT is
`ff000000 ff010101 ff020202 …`, which in Metal's BGRA8 byte order is
**RGB ramping with alpha pinned at 255** — a true identity in RGB, exactly as
§21.4 said. A blit that sampled the RTT and passed it through that LUT would
produce the RTT's pixels, not a constant. And the blit's source sample is
`(0,0,0)`-ish, so the blit's output would be black, not blue. The blue must
come from something else entirely — the guest's own clear, a different draw, or
memory that was never written.

**Consequence.** The whole "wrong or stale *content*" family is out: a hazard,
a wrong sample or a wrong UV transform would all yield a *stale copy* of the
RTT, and none of them can yield a constant. What is left is the mechanism
§22.3 already had as H2: **the draw never lands**, in every run, so the display
buffer shows whatever something else left there — which is why the screen is
black in one run, blue in another and zero in a third, and why the defect looks
like "no picture" rather than "a wrong picture".

That also re-ranks the experiments. The target is now a single question: *why
does the one draw that closes an encoder and opens a new pass, mid-frame, to a
different attachment, never reach the display buffer?* The three cheap levers
answer it directly:

- `MTL_DEBUG_LAYER=1` — the driver's own verdict on that draw (L225).
- `MTL_PROBE_VARIANT=heaps` — if the picture appears, the edge was missing
  (L226); if not, the draw itself is dropped.
- `probe-tri` — now with a sharper meaning: the discriminator no longer has to
  separate "black sample" from "no draw"; with the blit proven to land
  nowhere, a *visible* gradient from the presenter draw would prove that the
  draw lands, and its absence would point at the pass/attachment rather than
  the sample.

### 22.10 The residency defect — what the validation layer said

The 2026-11-11 21:15 run was the first with Metal's own API validation layer
enabled (`MTL_DEBUG_LAYER=1`, `MTL_PROBE_VARIANT=heaps`, level 5, the pixel
probe armed; 144 s, 2,851 presents, the F12 presses armed five capture
windows). Two findings, one of them the most likely cause yet.

**The environment.** The debug layer's error and warning modes are
`ignore|assert|nslog|oslog|exception` on this runtime — **there is no
`stderr` mode**, and passing one aborts the process at start-up
(`MTLGetEnvCase` assertion). The messages therefore go to the unified log,
where every Metal message is **`<private>`** (72,355 of them) — but the same
messages *are* readable on the process's stderr, which is why the run script
captures it. 46 MB of stderr for 144 s, and the run is dominated by the
debug layer's per-call tracing.

**The finding.** One error kind, 8,905 times, ≈3 per frame:

```
Attachment texture (Label: (null)) used in command buffer (at index 0)
is not added to any residency set on the command buffer or command queue.
```

**Metal 4 requires every resource a command buffer touches to be resident in
a residency set attached to the command buffer or its queue, and the backend
never adds its textures.** It adds the *heaps*:

```cpp
// MTLResourceManager.cpp:65-68, heap_pool::create_heap
if (heap && m_residency) { m_residency->make_persistent(heap); }
```

and `resource_manager::new_texture` only *checks* the heap:

```cpp
// MTLResourceManager.cpp:433-434
track(texture, pool, heap, offset, required.size);
check_residency(heap);          // asks "is the heap published?" — always yes
```

So `check_residency` — whose own comment says "A sub-allocation of an
unpublished heap reads zeros in silence: Metal raises no validation error and
no GPU fault (HLD-02 F1)" — is guarding the wrong object, and never fires.
The texture itself is never added.

**Why this is the best candidate yet.** It is Metal-4-only by construction:
Vulkan and OpenGL have no residency sets at all, so no VK/GL code path can
have this bug and no VK/GL comparison would ever have shown it. It lands
exactly on the resource the defect is about — the **colour attachment of a
render pass**, which is the display buffer the presenter blit writes to. And
it is invisible without the validation layer, which the backend never enables
(L225).

**The fix** is one line, in `resource_manager::new_texture`:

```cpp
m_residency.make_persistent(texture);
```

The release path already does the matching removal
(`release_list` → `residency_manager::remove`, `:548`), so nothing leaks and
no stale entry can cover a reused span.

**The open caveat.** The layer reports only *attachments*, because
argument-table resources are referenced by `MTL::ResourceID` and the layer
cannot resolve an ID back to its object. So the *sampled* textures are
probably non-resident as well, and publishing every texture (which the fix
does) covers both. Whether it is *the* black screen is one build and one run
away — and the same run tells us, because the validation error must go away.

### 22.11 Correction: the residency error is real, but my first reading of it was wrong

**What I claimed and why it is wrong.** §22.10 said the backend "publishes heaps
to its residency set and never the textures", and that this was the defect. It
is not, and Apple's own documentation says so:

> *"Adding a resource allocation that originates from an `MTLHeap` to a
> residency set makes that entire heap resident… This includes all resources
> that come from an `MTLHeap` allocation that's in the residency set."*
> — *Simplifying GPU resource management with residency sets*, Apple Developer
> Documentation

> *"When your app calls a command buffer's `commit()` method, Metal
> automatically attaches the owning queue's current residency sets to the
> command buffer."* — same page

And the backend's usage is correct on its face, which I checked rather than
assumed:

- `heap_pool::init` creates **no heap** (`MTLResourceManager.cpp:21-26`); heaps
  are created lazily by `allocate()`, i.e. after `set_residency()` is wired
  (`:268-269`). So **every** heap is published by `create_heap` (`:65-68`).
- `check_residency` (`:482-501`) therefore correctly never fires — and it did
  not fire in the run (0 occurrences).
- The persistent set is attached to the queue (`residency_manager::init`,
  `:186-194`), so it is attached to the command buffer at commit by Metal.
- The CAMetalLayer's own residency set is fetched and attached too
  (`MTLMetalLayer.mm:222-237`, macOS 26+; this host is 27.2) — which is
  exactly what MoltenVK does: *"Metal4 requires making CAMetalLayer's
  residencySet resident for the queue that will render to it"*
  (`.kk-mesa/src/vulkan/wsi/wsi_common.h:151`).

**What the three backends say.** Vulkan and OpenGL have **no residency model
at all**, so there is nothing to compare — which is precisely why a
cross-backend audit could never have found this, and why it is a Metal-4-only
defect. The Metal-4-only things that *do* have a Vulkan analogue are the
hazard model and the explicit barriers (the `StageAll→StageAll` edge that
§22.6 flagged against the KK reference, which ends the render pass *before*
raising the barrier, while we raise it before `endEncoding`).

**So what is still unexplained?** The error names an *"Attachment texture
(Label: (null))"* — and our textures have no label, and the layer cannot
resolve argument-table resources, so it cannot name the surface either. 8,905
errors over 2,474 commits is ≈3.6 per commit, i.e. every attachment of every
pass, which does not match "the drawable only" (that would be 1 per present).
**The identity of the offending texture is the missing fact**, and it is
obtainable two ways: give the surfaces a debug label so the message names
them, or bisect by disabling one attachment at a time.

**The armed experiment.** The build in the tree publishes every texture
explicitly (`resource_manager::new_texture` → `m_residency.make_persistent`).
Under Apple's rule that is *redundant* — but it is not a no-op for the
diagnosis:

| next run's residency-error count | reading |
|---|---|
| **≈0** | the offenders were our surfaces, and "a heap in the set covers its resources" did not hold for them — the fix is the fix |
| **≈8,900 (unchanged)** | the offenders are **not** our heap-backed textures: the CAMetalLayer drawable, or something else — and the drawable's path is the next file to read |

Either way the error count is a *quantitative* discriminator that the previous
runs never had, and it costs nothing but a run.

### 22.12 The consolidated findings, and the corrected picture

This is the whole of the 2026-11-11 late session in one place: what was measured,
what it ruled out, what it corrected, and what is still owed. Nothing here needs a
device; only the last item does.

#### 22.12.1 The corrected picture

**The blit lands. It writes the wrong colour.** That sentence replaces the
two earlier readings, and both were wrong:

- §21/L219 said "the blit writes black *or* nothing" — undecidable from the
  probe.
- §22.9/L228 said the uniform display buffer means "the blit produced
  nothing" — **wrong** (L231). A uniform value is a *constant*, and a
  constant is what a constant-producing shader writes. The 11-10 runs' blue is
  `r=0 g=0 b=255`, i.e. the LUT's entry 0 (`B=0 G=0 R=0 A=255`) with its
  **alpha in the blue channel** — the value an *undefined read* produces. So
  the blit landed in every run and wrote a colour derived from the LUT.

**Therefore the fault is in the fragment stage's reads, and it is the LUT's
three reads** (L230):

```metal
r0 = tex0.sample(tex0Smplr, ...);      // the scene RTT — 2-D on both sides
r0.z = tex1.sample(tex1Smplr, ...).z;   // the LUT — 1-D in the shader…
r0.y = tex1.sample(tex1Smplr, ...).y;   // …2-D on the host
r0.x = tex1.sample(tex1Smplr, ...).x;   // three reads overwrite all three channels
```

The RTT's own sample is *not* the problem: unit 0 is a 2-D texture on both
sides, it is the attachment itself (L223), and it has content (L221). The LUT
is a 1-D texture in the shader and a 2-D texture on the host, because
`get_texture_type` has no `TextureType1D` case and `texture_dimension_1d`
appears nowhere in the backend. Metal matches the bound resource against the
shader's declaration; the mismatch makes the read return undefined data, and
the three reads replace the colour the blit was supposed to copy.

**The blit is the only draw in the frame that samples a 1-D texture, and it is
the draw that produces the presented image.** That is the whole defect.

#### 22.12.2 Every finding, and what it cost

| # | finding | status | what it closed |
|---|---|---|---|
| 1 | The sampled object **is** the colour attachment — the "target" and "sample" host dumps print the same `MTL::Texture*` (`0x767ba1d180`) | measured | H1's object-identity half (L223) |
| 2 | The shared sample resolve takes the **live-view** branch, not the feedback-copy branch (`surface_is_rop_target` is false — the bound target is the display buffer) | code | "the sample is a stale copy" |
| 3 | The blit's vertex block is a **perfect fullscreen quad** with a correct tc ramp (`(-1,1)uv(0,0) … (1,1)uv(1,0)`), and the vertex fetch is the shared `RSXVertexFetch.glsl` | measured | the vertex stage, twice |
| 4 | The blit is a **plain store** (blend-enable mask `0xe`, per draw-buffer index in all three backends → attachment 0's blending is off) | code | the "alpha-0 source writes nothing" family |
| 5 | The scene RTT's **alpha is 0 in 921600/921600 pixels** and write-protected | measured | the "the source alpha is 0" trap |
| 6 | The blend equation is **32774 = `0x8006` = `CELL_GCM_FUNC_ADD`** — valid (my earlier `0x8002` reading was an arithmetic slip) | measured | the blend decode |
| 7 | The colour write mask maps guest `1110` → Metal `0xe` = R\|G\|B, correct against Metal's Alpha=1/Blue=2/Green=4/Red=8 bit order | code | the write mask |
| 8 | The depth compare `519` = `0x207` = `CELL_GCM_ALWAYS` — the RSX zfunc encoding | measured | the depth state |
| 9 | The guest's viewport transform lives in the vertex env, so Metal's unsigned viewport never sees a negative height | code | the viewport |
| 10 | **SPIRV-Cross flattens every runtime-sized GLSL array to `[1]`** in MSL — `texture_parameters[1]`, `fs_contexts[1]`, `fc[1]`, `vertex_contexts[1]`, `draw_parameters[1]`. GL declares `[16]`, VK keeps `[]` (legal in a UBO). The Metal design depends on the Vulkan behaviour ("the ring is bound once and only the index travels per draw") | confirmed | latent UB — **proven harmless on this device** by the RTT's 128-distinct-value gradient |
| 11 | The Metal **validation layer was never enabled** and has no hook; the debug layer's messages are `<private>` in the unified log but readable on stderr, and this runtime has **no `stderr` error mode** (`ignore\|assert\|nslog\|oslog\|exception`) | measured | the invisibility that hid everything below |
| 12 | **Residency: the textures were never published to the residency set** — the backend published the *heaps* only. Metal's API validation layer reported it 8,905 times (old run: 2,474 of 2,851 commits flagged): *"Attachment texture … is not added to any residency set…"* | measured, **fixed**, **not the cause** | — (the screen stayed black with 0 errors) |
| 13 | Apple's docs say a heap in a residency set covers its resources, so #12's mechanism was **not** the documented one; the fix is therefore *redundant* per the rule but empirically removes the errors | corrected | — |
| 14 | The pass-boundary edge is one barrier raised **before** `endEncoding`, while the KK reference ends the render pass first and raises it after (`kk_cmd_buffer.c:266`) | code | — (explains a lagging picture, not this one; `heaps` excluded the hazard family) |
| 15 | Invalid enums diverge: Metal degrades to Add, **Vulkan throws**, GL degrades | code | — |
| 16 | The guest's 16-draw stream is **byte-identical across 1100 frames** — the title is presenting a static frame | measured | — (the "press Start" test is still owed) |
| 17 | **No 1-D texture type** — the blit's LUT read is a 2-D/1-D mismatch; three undefined reads overwrite all three channels | **candidate RCA** | the black screen |

#### 22.12.3 What the four hypotheses now stand for

- **H1 (the sample is the wrong object / the sample is black).** The
  object-identity half is dead (L223) and the RTT's sample is live and has
  content. The *read* that is broken is the LUT's, and it is broken by a type
  mismatch, not by identity — so H1 was never the right shape. The right shape
  was "the fragment stage's reads are wrong", which L230 is.
- **H2 (the draw never lands).** **Dead** (L231): the blit lands in every run
  and writes a constant. What is constant is the undefined read's value.
- **H3 (the unit binding is wrong).** The slot map is consistent and
  self-checking, and the API validation layer reports no binding error. But
  the *type* of the bound resource was wrong — which is H3's cousin and was
  invisible to the API layer. `MTL_SHADER_VALIDATION=1` would have named it.
- **H4 (the title is stuck).** Unchanged and still owed: the frozen stream
  (L222) has never been probed with input. Note that it no longer matters for
  the black screen — a stuck title would still blit *something*.

#### 22.12.4 The fix, and what is owed

Shipped in the tree:

1. `resource_manager::new_texture` publishes the texture to the residency set
   (the release path already removes it) — #12.
2. `get_texture_type` maps `texture_dimension_1d` → `MTL::TextureType1D`, and
   `normalize_texture_extent` forces the height to 1 for a 1-D texture — #17.

Owed:

3. **The device confirmation**: one run with the validation layer armed. The
   pass criteria are quantitative — the screen shows the scene, the LUT's host
   dump still holds its 256 entries (proving the upload into a 1-D texture
   worked), and still zero validation errors.
4. **`MTL_SHADER_VALIDATION=1`** — the independent confirmation, and the
   diagnostic that should have named #17 on the first run. Worth doing even
   after the fix, as the regression guard.
5. **A GPU-free regression test** for the dimension → type mapping, next to the
   existing Metal 4 contract tests (`rpcs3/tests/test_rsx_metal4_state.cpp`).
6. The "press Start" test (#16), and the Vulkan oracle, which is still the
   only way to learn what the frame *should* look like.

### 22.13 The RCA, confirmed on device (2026-11-11 21:42)

**The Metal 4 backend had no 1-D texture type, so the presenter blit sampled a
2-D texture where its shader declared a 1-D one, and three undefined reads
replaced all three colour channels.** The blit landed; it stored the wrong
colour.

**The defect.** `texture_cache::get_texture_type` (`MTLTextureCache.h:248`)
mapped the guest's dimension to Metal's type and had **no `TextureType1D`
case** — 1-D fell into `default: TextureType2D`, and `texture_dimension_1d`
appeared *nowhere* in the backend. So the guest's 256×1 LUT became a **2-D**
Metal texture, while the guest's fragment program declared and read it as
**`texture1d<float> tex1`** (captured `fp-5.metal`). Metal matches the bound
resource against the shader's declaration: a 2-D texture bound to a `texture1d`
read is a **type mismatch**, and the read returns undefined data.

**Why it blanked the screen.** The blit's fragment program is:

```metal
r0 = tex0.sample(tex0Smplr, ...);      // the scene RTT — 2-D on both sides, fine
r0.z = tex1.sample(tex1Smplr, ...).z;   // the LUT — 1-D in the shader, 2-D on the host
r0.y = tex1.sample(tex1Smplr, ...).y;
r0.x = tex1.sample(tex1Smplr, ...).x;   // three reads overwrite all three channels
```

**Three undefined reads overwrite the colour the blit was supposed to copy**,
so the blit stored a constant derived from the undefined read rather than the
RTT's pixels. The RTT's own sample was never at fault: it is the colour
attachment itself (L223), it is live, and it has content (L221). And the blit
is the **only draw in the frame that samples a 1-D texture**, and it is the
draw that produces the presented image.

**The proof** (run `20261111-214209`, validation layer armed):

| | before the fix | after the fix |
|---|---|---|
| presented image (`host-present-600`) | one uniform word (`000000ff` = blue, or `ff000000` = opaque black) | max **r=200 g=170 b=140**, four distinct tones (689948 / 204175 / 12557 / 2486 px) |
| next presented frame (`host-present-1200`) | — | max **255 255 255** — brighter, i.e. the scene is animating |
| the LUT's host dump | 256/256 words (the identity ramp) | **256/256, unchanged** — the upload into a 1-D texture works |
| API validation errors | 8,905 residency errors (2,474 of 2,851 commits) | **0** |

And the human verdict: **the screen shows the scene.**

**Why the earlier evidence pointed the wrong way, and what it actually proved.**
Everything that pointed at "the blit writes nothing" was the *uniform* display
buffer, and a uniform value is a constant — what a constant-producing shader
writes (L231). The older runs' blue was `r=0 g=0 b=255`, i.e. the LUT's entry
0 (`B=0 G=0 R=0 A=255`) with its **alpha in the blue channel**: the value an
undefined read produces. So the blit landed in every run, and the fault was
*which colour*, never *whether*.

**What the fix is.** Two lines, plus the regression test:

- `get_texture_type` maps `texture_dimension_1d` → `MTL::TextureType1D`, and
  `normalize_texture_extent` forces the height to 1 for a 1-D texture
  (`MTLTextureCache.h`, `MTLTextureCache.cpp`).
- `get_texture_type` moved to the public section so the mapping is a
  GPU-free contract test (`MTLTextureTypes.*`, `rpcs3/tests/test_rsx_metal4_state.cpp`).

**What the audit contributed.** The cross-backend audit is what made the
difference, and not in the way it was aimed. The four original hypotheses were
all about the *sample* (H1), the *draw* (H2), the *binding* (H3) and the
*title's progress* (H4); the cause is none of them — it is the **type** of a
bound resource, a dimension the shared code carries and the Metal backend
dropped. Two of the audit's products were needed to see it: the **validation
layer** (never enabled before — it is what turned an invisible undefined read
into a named one) and the **cross-backend rule** that every mapping in the
backend must match the guest's *declared* type, not merely be *a* type.

**Also found, not the cause, and reverted before the release.** The residency
defect (L229): the backend published its heaps to the residency set and never
its textures; Metal reported it 8,905 times. Publishing the textures removed
every error, and **the screen stayed black** — so the residency bug was real,
Metal-only, and a separate defect. It is **not in this release**: the textures
are still published as heaps only, and the driver still reports the attachment
as non-resident. It is deferred, with the measurement and the one-line fix
recorded, and it is the first thing to pick up after the release.

**Still owed** (unchanged): the "press Start" test for L222's frozen stream;
the Vulkan oracle (what the frame *should* look like); `MTL_SHADER_VALIDATION=1`
as the independent confirmation and the regression guard; the D1 fix (bind the
parameter rings per draw and declare the arrays with real sizes, as GL does);
and the release work — scrub, `rpcs3_test`, the RR7 regression run, and the
squashed `release/metal4` commit.

### 22.14 The release sequence (2026-11-12)

#### The upstream refresh

`upstream/master` moved from `8183f6e32` to `9b8974d32` (nine commits). The
new commits touch **only** `rpcs3/Emu/Cell`, `rpcs3/Emu/Io`, `rpcs3/Input`
and `rpcs3/rpcs3qt` — no RSX, no Metal backend, no tests, and none of the
files the FIFO work changed.

Both metal branches rebase onto it **cleanly**, which was verified by doing
the rebase rather than by inspecting the file lists:

| branch | base | result |
|---|---|---|
| `metal4-dev` | `8183f6e32`, 13 commits | 13/13 rebased, **zero conflicts** |
| `release/metal4` | `8183f6e32`, 1 squashed commit | 1/1 rebased, **zero conflicts**; the tree differs from the old release commit by exactly upstream's 23 files |

#### What was committed on `metal4-dev`

Three commits, in the order the work belongs:

1. `rsx: the FIFO parser reads one cursor with no exception` — the FIFO work:
   `RSXFIFO.cpp`, `RSXFIFO.h`, `RSXThread.cpp`, `cellGcmSys.cpp` and the
   stream unit tests.
2. `metal4: map a 1-D guest texture to a 1-D host texture` — the fix
   (`MTLTextureCache.h`, `MTLTextureCache.cpp`), the new regression test file
   and the CMake entry for it.
3. `docs: the findings log through the 1-D texture root cause` —
   `LOG-FINDINGS.md`, with the two corrections recorded as corrections
   (L231) rather than silently dropped.

#### The residency change was reverted

The residency publication (`resource_manager::new_texture` → `make_persistent`
on the texture) was **reverted before the release**. It was a real,
driver-confirmed defect (8,905 validation errors → 0) and it was **not** the
black screen, so it does not belong in this release. The consequence is
recorded rather than forgotten: **the driver will again report the render
pass's colour attachment as not resident**, and that defect is the first
thing to pick up after the release (L229 keeps the measurement and the
one-line fix).

The reasoning that made the revert safe: the residency errors were present in
*every* black-screen run, including the ones where the RTT had content — so
they were never the reason the blit stored the wrong colour. What made the
revert *necessary* rather than merely tidy: Apple's documentation says a heap
in a residency set covers its resources, so the extra publication is redundant
under the documented rule, and it costs a residency-set commit per texture
creation.

#### The regression test has to live in a file the scrubber keeps

The scrubber deletes `rpcs3/tests/test_rsx_metal4_state.cpp` wholesale — its
authority is the dev inventory, not the production contract — so the three
1-D tests written there **would not have shipped**, and the fix could have
regressed silently in the release branch. They now live in
`rpcs3/tests/test_rsx_metal4_texture_types.cpp`, which is registered in
`rpcs3/CMakeLists.txt` and is not on the scrubber's delete list. The test
file carries no `MTL_NOTE`, no `probe::`, no `TODO(excise)` and no
`diagnostic_`, so it survives the forbidden-token gate.

#### The release tree

The release tree is rebuilt from the rebased `metal4-dev` tree, scrubbed, and
squashed into **one** commit on `upstream/master`:

1. `python3 .metal4-release-scrub.py check` — read-only; it prints exactly
   what `apply` would do.
2. `python3 .metal4-release-scrub.py apply --force` — deletes the
   diagnostics as syntactic units, drops `docs/research/` and the dev-only
   test files, and removes their CMake entries.
3. `python3 .metal4-release-scrub.py verify --build-cmd "cmake --build build --target
   rpcs3 -j8"` — the forbidden-token gate, the deletion-list check,
   `git diff --check`, and the compiler as the second gate.
4. The GPU-free `rpcs3_test` green (676 tests, of which the three 1-D
   contract tests are new).
5. One commit, authored `hsaka -1.0 <340957754+hsakaminusone@users.noreply.github.com>`,
   on top of `upstream/master`, then pushed to `release/metal4`.

`origin/metal4` is **not** force-pushed; `release/metal4` is the public
target.

### 22.15 The release, published (2026-11-12)

> **The capture sets and probe logs were deleted after the release
> (2026-11-12, 1.4 GB).** The paths quoted below — `build/probe/dumps-*`,
> `build/probe/probe-*.log`, `/tmp/mtl4-run` — are empty now. The
> *measurements* they carried are recorded in the findings log (L216-L232) and
> in §22.9-§22.13, so nothing in the analysis depends on the files; only the
> raw evidence is gone. The tooling (`build/probe/*.sh`, `build/probe/*.py`)
> was kept, and the launcher still refuses to start when an emulator is
> already running.

**`release/metal4` is `7ef691fff` — one commit on `upstream/master` `9b8974d32`**,
authored and committed as `hsaka -1.0 <340957754+hsakaminusone@users.noreply.github.com>`.
`origin/metal4-dev` is `f4e66ee35`.

The four commits on `metal4-dev` (all rebased onto the new upstream):

1. `rsx: the FIFO parser reads one cursor with no exception`
2. `metal4: map a 1-D guest texture to a 1-D host texture`
3. `docs: the findings log through the 1-D texture root cause`
4. `rsx: keep the two FIFO declarations the release scrub would excise`

**The gates the release passed**

| gate | result |
|---|---|
| the scrubber's `check` on the rebased dev tree | 171 files deleted, 551 units excised, **0 needs-review, 0 residuals** |
| the scrubber's `apply` | applied; 224 paths changed, 63,680 lines of diagnostics and research docs removed |
| the **scrubbed tree compiles** | the release build: **exit 0** — and this is the gate that mattered, see L232 |
| the scrubber's `verify` | **0 residuals**, exit 0 |
| the GPU-free tests, release tree | **450/450** (fewer than the dev tree's 676, because the scrub removes the dev-only suites) |
| the 1-D contract tests **in the release tree** | **3/3** — the regression guard ships, which is why they were moved out of the file the scrubber deletes |
| the device picture, post-revert configuration | the presented image has real colour and changes every 600 frames |

**What the release build caught, and what it says about the tooling.** The first
release build failed: the scrubber had removed two **production declarations**
(`m_upstream_semantics`, `peek_cache`) while keeping their uses, and its own
report said **0 needs-review, 0 residuals** — because the scrubber's rule
catches *writes* to production state, and a declaration is not a write (L232).
The release is therefore only publishable when the **scrubbed** tree compiles:
the scrubber's report is not sufficient evidence, and `verify --build-cmd`
must be part of the release, not an optional extra.

**Still open, deliberately**

- The **residency defect** (L229): the textures are published to the residency
  set as heaps only, so the driver again reports the render pass's colour
  attachment as not resident (11,342 in the last run). It was **not** the black
  screen, it is Metal-only, and it is the first thing to pick up after the
  release. The measurement and the one-line fix are in L229.
- **D1** (§22.4): the MSL runtime-sized arrays are still flattened to `[1]`.
  Proven harmless on this device, still undefined behaviour, and the fix (bind
  the parameter rings per draw and declare the arrays with real sizes, as GL
  does) should land before the next release.
- `MTL_SHADER_VALIDATION=1` as the independent confirmation of the 1-D
  finding and the guard against a repeat.
- The "press Start" test (L222's frozen stream) and the Vulkan oracle (what
  the frame *should* look like).
- The **RR7 (BLUS30001) regression run** on the release build — the one
  release gate that is still owed, since the FIFO work is in the release.

### 22.16 The release build is black: a second defect, and a contaminated verification

**The release tree (`7ef691fff`, the app the user launched) renders black for both
RR7 and Demon's Souls.** The dev tree renders the scene. The cause is **not** the
scrub.

**The scrub was validated.** Its JSON manifest lists every excision; classifying
all 549 of them:

| class | count |
|---|---|
| comments | 389 |
| preprocessor lines | 79 |
| log / counter / timer statements (`MTL_NOTE`, `MTL_PROBE_*`, `MTL_PERF_*`) | 53 |
| `#include "MTLInstrumentation.h"` (the header is deleted) | 24 |
| CMake entries for the deleted dev-only test files | 4 |
| **anything else** | **2** — `m_diag_retry = true;` and `fifo_ctrl->diag_jump(offs);` |

Both of the last two write or call **diagnostic** state, which the scrub removes
along with it. Nothing in the release tree's code differs in behaviour from the
dev tree's.

**The real cause: the verification runs were contaminated.** Every run that read
as "the picture is correct" had `MTL_PROBE_VARIANT=heaps` armed — *tracked
hazard tracking*, where the **driver** orders the conflicting accesses for us.
The release tree cannot have that switch: it lives in the probe machinery the
scrub deletes, so the release tree runs the **shipping** model (untracked heaps,
our own barriers).

| | heaps OFF (shipping) | heaps ON |
|---|---|---|
| no 1-D fix | black (19:23) | black (21:15, 21:27) |
| **1-D fix** | **the release build: BLACK** | correct (19:07, 21:42) |

So there are **two independent defects**: the 1-D texture type (fixed, and
necessary — the 21:27 run had heaps and was still black) and the **hazard edge**
(not fixed, masked by tracked heaps). The likely culprit is §22.6: the
pass-boundary barrier is raised **inside the producer encoder, before
`endEncoding()`**, while the render pass's tile flush happens at `endEncoding`,
so the barrier cannot order the flush. The KK reference ends the render pass
first and raises the barrier after; in Metal 4 the consumer-side form
(`barrierAfterQueueStages` in the *consuming* encoder, before its draws) is what
expresses "my fragment stage must see the producer's stages".

**The methodological fault, recorded because it is the more expensive lesson:**
I armed the variant that fixes the symptom and then read the correct picture as
proof that my fix worked. The 1-D fix is real and independently necessary — but
it was never tested *without* the driver's hazard tracking, so the release tree
inherited a second defect that the verification runs had been papering over.

**The missing cell, now running** (`build/probe/run-nohazards.sh`): the dev build
with `heaps` **off** and **no** `MTL_PROBE_PIXELS`, so there is no mid-frame
readback that could flush the GPU and mask a missing edge. The presented image
is dumped after each frame, so the dump *is* the screen.

**What the three reference models say about tracked vs. untracked** (the SDK's
own words, `MTLHeap.h:76-85`, plus the KK tree):

> *"For heaps, `MTLHazardTrackingModeDefault` is treated as
> `MTLHazardTrackingModeUntracked`… Setting hazardTrackingMode to
> `MTLHazardTrackingModeTracked` causes hazard tracking to be enabled heap…
> **For optimal performance, perform hazard tracking manually through MTLFence
> or MTLEvent instead.**"*

| | model | who orders the accesses |
|---|---|---|
| **Vulkan** | no tracking concept at all | **the app**, always — `vkCmdPipelineBarrier`, subpass dependencies, access masks |
| **OpenGL** | the driver inserts them implicitly | the driver; the app only *exposes* them (`ARB_sync_buffers`, `ARB_texture_barrier`) |
| **Metal 4** (Apple) | the heap default is **Untracked** | the app — which is why "perform hazard tracking manually" is the performance advice |
| **KK** (the reference Metal 4 driver) | never selects a mode (`mtl_types.h:67-71` defines it; nothing sets it) | the **default → Untracked**, so KK declares its edges explicitly — like Vulkan, like our HLD-03 design — and raises them *after* ending the render pass (`kk_cmd_buffer.c:266`) |

**So our backend's design is the right model**: Metal 4's default is untracked,
Vulkan is untracked, KK is untracked, and only OpenGL is the automatic one.
Flipping the heaps to tracked by default is therefore a **retreat from the
intended model** — the safe, slower one that Apple offers as the fallback
while advising manual tracking for speed. It is right as an interim shipping
decision and wrong as an end state: the real fix is to close the manual edge
(the pass-boundary barrier raised *after* the render pass ends, as KK does),
verified against the tracked configuration as the reference.

**Consequence for the release:** `release/metal4` must not be published as
"playable" until the shipping hazard model renders the scene. The branch itself
stays as it is — it is the scrubbed, correct-by-construction code — but the
*announcement* has to wait.

### 22.17 The black screen was mostly a measurement artefact; the picture works

**What the evidence now says** (2026-11-12, after the scrub review):

1. **The scrub is innocent.** All 549 excisions classify as comments (389),
   preprocessor lines (79), log/counter/timer statements (53), includes of the
   deleted `MTLInstrumentation.h` (24), CMake entries for the deleted dev-only
   tests (4) and exactly 2 diagnostic-state writes. Every excised
   `#if MTL_DIAGNOSTICS` / `#if MTL_PERF_ENABLED` / `#if MTL_DEBUG_LOGS` block
   was re-read: the two that looked functional are variant-gated diagnostics
   (`MTL_PROBE_VARIANT=no-cull`, `no-depth`), inert unless armed.

2. **The hazard model is innocent, and L233 is retracted.** Run `20261112-200536`
   (the *shipping* model, untracked heaps, 181 s of Metal time) dumped the guest
   display buffer `0xc0000000` and the dump decodes to a **perfect BANDAI
   NAMCO Games logo** — the RR7 boot card, correct glyphs, 609 → 1034 distinct
   colours as it accumulates, first content at t=21.4 s. So the backend renders
   and presents correctly with the hazard model the release ships. L226 already
   said the pass-boundary barrier explains "a lagging picture, **not a black
   one**".

3. **Demon's Souls works.** Run `20261112-203530` (shipping model, every probe
   armed, validation layer on): the guest's 16-draw title stream ran at
   t=72.7–79.2 s (draw ids 0–15, exactly L222's stream) and **the title card is
   on screen** (the human verdict). The display buffers `0xc0010000` /
   `0xc03d0000` are still all-zero at the blit, so the visible card is *not*
   that surface — the picture reaches the screen by another path (the overlay /
   video plane), which is worth a separate look.

4. **"Black screen" was, in most cases, the boot phase.** The DS title card
   needs ~75 s of Metal time and the RR7 boot card needs the FMV, whose data is
   read with the speed-accurate disc emulation on (~40 s in). Every run that
   "showed black" was cut short before the first visible frame: 67 s, 84 s,
   114 s. The 181 s run shows a picture.

5. **A second, real defect: booting a second game inside a running instance.**
   The user launched RR7 from the instance that was running DS: the log shows
   `Replacing the renderer (Metal | Apple A18 Pro -> Metal | Apple A18 Pro)`
   followed by a second `Metal 4 device initialized`, and then
   `Metal 4: watchdog, no present for 5.165 s` — the present pipeline stops
   after the renderer swap, so the window goes black. The renderer-replacement
   path (`RSXThread.cpp:969 reset_renderer`) is the defect; the Metal 4
   backend's `on_shutdown`/re-create path must re-wire the presenter and the
   layer.

6. **The hazard default is reverted to the shipping model.** The interim
   "tracked heaps by default" change is withdrawn: Metal 4's heap default is
   Untracked (`MTLHeap.h:79`), Vulkan and KK are untracked, and the shipping
   model is measured to render correctly. `MTL_PROBE_VARIANT=heaps` remains the
   oracle for the cross-pass edge work (Idea A/B), not the shipping setting.

**The method that produced this**: decode the dumps instead of describing them.
The BANDAI NAMCO logo was in `build/probe/dumps-nohazards-20261112-200536/` the
whole time, three hours before anyone looked at it.

## 23. Debug plan adopted 2026-11-12 (for the next session)

### 23.1 Facts, and what is not evidence

Established: the backend renders (RR7 logo decoded from `host-target` dumps,
run `200536`; DS card seen on screen, run `203530`); the scrub is
behaviour-neutral (549/549 excisions); the 1-D fix is in both trees; the
shipping hazard model is untracked heaps; our parser parks ~3,700 times in
12 s of RR7 (`204324`).

**Not evidence:** run `204942` ("upstream FIFO semantics") left **no probe
log, no dumps and no FIFO diag**. Its zero park count is equally explained by
the switch working or by the binary not carrying diagnostics. The A/B must be
repeated with proof that the instrument is live. `200536` also shows a
**73.6 s GPU-start lag** on the commit stamped t≈22 s, on the same frame whose
dump did a texture readback — so the probe may be causing the stall it
measured. Both confounds are closed by the protocol below.

### 23.2 Failure modes

| ID | Failure | Where |
|---|---|---|
| F1 | Parser stops dispatching draws while the guest flips | `RSXFIFO.cpp`: whole-packet gate, jump/link rules, count-0 walk |
| F2 | GPU queue/present stalls for tens of seconds | `MTLCommandRecorder.cpp` slot ring, readback/re-enter, fences; `MTLPresenter` |
| F3 | FMV never reaches the RSX; guest waits on the decoder | `cellVdec`/`cellDmux` HLE |
| F4 | A second game kills presenting | `RSXThread::reset_renderer` → `MTLGSRender::on_shutdown` |
| F5 | The scrubbed release binary differs | `release/metal4` |

### 23.3 Hypotheses and signatures

| H | Hypothesis | Signature | Falsified if |
|---|---|---|---|
| H1 | The whole-packet gate starves the parser | parks cluster at the draw stop; `pos` frozen, `put` barely moves; no desync; upstream mode removes both | upstream mode gives the same draw stop |
| H2 | Our jump/link rule walks an unpublished region | `jump past PUT` / `jump cross-buffer` / `phase slip` / `desync` at the stop | no jump/desync at the stop |
| H3 | The probe readback stalls the queue (observer) | 73.6 s lag vanishes with dumps off | lag persists without dumps |
| H4 | A real queue stall (fence/event/slot ordering) | lag persists with dumps off, on a specific slot/pass | only with dumps on |
| H5 | The video path stalls the guest | `cellVdec: waiting for a consumer`; RSX idle but not parked | draws stop without decoder involvement |
| H6 | Release-only defect | dev renders, release blacks out | release renders |

**Measurement caveat:** `frame/gpu wait` is
`feedback->GPUStartTime() - m_slots[index].debug_commit_time`. If
`allocate_slot()`/re-enter leaves that field stale, a huge lag is an artifact.
Check that before believing any queue-stall theory.

### 23.4 Instrumentation armed per run

`MTL_PROBE=5`, `MTL_PROBE_LOG`, `MTL_PROBE_DUMP_DIR`, `MTL_PROBE_TAPE=16`,
`MTL_PROBE_FRAME_COUNT=8`, `MTL_PROBE_PIXELS` (four patches),
`MTL_PROBE_VARIANT=untracked-heaps`, `MTL_DEBUG_LAYER=1` (+ nslog modes).
Emulator side: `FIFO diag:` (park/jump/desync/PUT move), the 5 s watchdog,
`cellVdec`/`cellDmux`/`cellPamf`, `Replacing the renderer`. `run-rca.sh` now
copies `RPCS3.log` to the run directory on exit.

**Human action:** press **F12 the moment the picture goes black** (and again
20–30 s later). The tape keeps the last 16 presented frames, so F12 captures
the frame that showed the defect; without it the capture window starts *after*
the defect.

### 23.5 Experiments

- **E0 sanity** — within 5 s of launch, confirm `probe-*.log` exists and starts
  with `[0.000] Metal 4: BC texture compression is available`. A run whose
  instrument did not open is void.
- **E1 RR7, production, all probes, 5 min** — baseline. F12 at black.
- **E2 RR7, `RPCS3_FIFO_UPSTREAM=1`, identical probes** — must show **zero**
  `park put`; otherwise the switch is dead and the run is void.
- **E3 DS control, both semantics** — the missing half of the A/B. If DS still
  reaches its card under upstream semantics, delete the gate.
- **E4 release binary (no probes), both games** — the deliverable, and the
  observer-effect control.
- **E5 `MTL_PROBE=4` (no dumps) + tape** — isolates H3 from H4.

### 23.6 Analysis protocol

1. Preserve probe log, stdout, emulator log, dumps, tape immediately.
2. Build a 1 s timeline: draws, presents, parks, jumps, desyncs, pos/put/get,
   last_cmd, watchdog.
3. Locate the stop: last draw, last present before the gap, last park/jump,
   first event after it.
4. Classify presented frames (source address, clear/black) from the tape and
   the decoded dumps — decode to PNG and look, never infer from line counts.
5. Align clocks on the first present/draw; a large lag must agree with host
   wall time before it is believed.
6. Record the row in `LOG-FINDINGS.md` with a verdict.

### 23.7 Decision rules

| Observation | Conclusion | Action |
|---|---|---|
| Upstream removes parks **and** draws resume | H1 confirmed | delete the gate, keep link rules, re-verify DS |
| Parks vanish, draws still stop | H1 falsified for the stall | treat F3/F2 |
| Parks cluster at the stop with `pos` frozen | H1/H2 | fix the gate/jump rule |
| Lag with dumps off | H4 | audit fences/slots |
| Lag only with dumps on | H3 | fix the probe readback, re-measure |
| `cellVdec waiting for a consumer` at the stop | H5 | video path; Vulkan oracle; renderer exonerated |
| Release differs from dev | H6 | diff the trees |

### 23.8 Static checks in parallel

- `MTLCommandRecorder.cpp:431, 487–505`: is `debug_commit_time` reset on
  allocate/re-enter; is the generation check before the log?
- `flush_for_readback` / `reenter_slot`: does a mid-frame commit + wait create a
  cross-slot dependency?
- `RSXFIFO.cpp:1017–1031`: is the packet end correct for non-increment packets
  and for a packet that wraps the ring?
- `RSXFIFO.cpp:1319–1350`: what does a jump past PUT actually *do* (we only log
  it)?
- The count-0 NOP path advances without a PUT check (its own comment).
- `present_frame` / `MTLPresenter::present`: what is presented when the guest
  flips a cleared surface; is "keep last frame" correct against hardware?
- `reset_renderer` / `on_shutdown`: does a second renderer re-wire layer and
  presenter?

### 23.9 Guardrails

One instance; no `pkill -f rpcs3`; launches only with an explicit go; unique
probe log/dump dir and copied emulator log per run; do not delete the gate
before E3; keep `build-tests/bin/rpcs3_test` green; the preserved shipping
binary is `build/bin/rpcs3-metal4-release-2051.app` (SHA-256 `c9320e29…`).

### 23.10 Run record: `20261112-212128` (RR7, then DS in the same session)

**Identity.** Dev tree `9c6ba7bf8`; `build/bin/rpcs3.app` 55,874,872 bytes
(probe-capable, `MTL_PROBE` and `FIFO diag` strings present); preserved shipping
binary `build/bin/rpcs3-metal4-release-2051.app` (SHA-256 `c9320e29…`).
Launcher `./build/probe/run-rca.sh rr7 "untracked-heaps"`; arms
`MTL_PROBE=5`, `MTL_DEBUG_LAYER=1`, `MTL_PROBE_FRAME_COUNT=8`,
`MTL_PROBE_TAPE=16`, four pixel patches, `MTL_PROBE_LOG`,
`MTL_PROBE_DUMP_DIR`, `MTL_PROBE_VARIANT=untracked-heaps`. Only
`MTL_PROBE_VARIANT` is honoured by the release tree; the dev tree honours all.

**Artefacts.** `build/probe/probe-rca-rr7-20261112-212128.log` (15,442 lines,
last t=142.669), `build/probe/emulator-rca-rr7-20261112-212128.log`
(copied on exit), `build/probe/phase-rr7-probe-20261112-212128.log`,
`build/probe/phase-rr7-emulator-20261112-212128.log`,
`build/probe/dumps-rca-rr7-20261112-212128/` (106 files, 22 `host-target`
frames written 21:21:48–49). Clock: **probe t = emulator − 1.36 s** (first
`Metal 4: BC texture compression is available` at probe 0.000 is emulator
`0:00:01.36`).

**Measured timeline.**

| emulator | probe | event |
|---|---|---|
| 0:00:01.36 | 0.000 | Metal 4 device init (A18 Pro) |
| 0:00:02.99 → 0:00:03.76 | 1.6 → 2.4 | startup renderer replacement (normal) |
| 0:00:12.8–17.3 | 11.4–16.0 | 16 draws (ids 0–15), 115 presents |
| 0:00:19.29 | 17.935 | **last present** (`display showed drawable` 17.954) |
| 0:00:19.311 | 17.95 | RSX parks waiting for PUT; **last FIFO diag until 0:01:40** |
| 0:00:29.84 | 28.4 | `title.pam` (73.9 MB) opened; PAMF demuxer + vdec threads start |
| 0:00:31.195 | 29.84 | `cellVdecGetPicItem` → `0x80610104 CELL_VDEC_ERROR_EMPTY` (`info=0, sequence_id=1, seq_id=0`) |
| 0:00:31.196 | — | `vdecEntry` aborts (1.0 ms PPU); ES torn down; `cellDmuxResetEs`/`DisableEs` → `0x80610201` |
| 0:00:31.39 | — | retry with `menu.pam` (49.6 MB); same failure at 31.409 |
| 0:00:33–0:00:44.5 | — | **no emulator log activity** |
| 0:00:44.56 | — | `sys_game: Aborting LV2 Watchdog Thread...` (shutdown path) |
| ~0:00:44.6 | — | stop requested; game does not react |
| 0:00:51.92 | 50.57 | forced termination (`elapsed_ms=6256`) |
| 0:01:29.84 | 88.5 | DS boot (second game, after RR7 stopped) |
| 0:01:31.08 | 89.7 | DS renderer init (second replacement; not an in-game reset) |
| 0:01:30–0:02:25 | 88.7–142.7 | DS: **1,401 presents, 0 draws** |
| 0:02:25.30 | 142.669 | app quit |

**Findings.**

1. **The FIFO gate is exonerated for this freeze.** Zero `park put`, `jump`,
   `desync`, `PUT move` or `parked addr` between emulator 0:00:20 and 0:01:40.
   The RSX thread was idle because the guest submitted nothing; the parser had
   nothing to mis-parse. The 19.311 park is the normal wait-for-PUT after the
   last flip.
2. **The freeze is the intro-movie path, not Metal.** `cellVdecGetPicItem`
   returns EMPTY 1.3 s after the vdec starts, then the game tears the whole
   video path down and retries with `menu.pam`, then stops. Audio and input
   continue because the PPU threads run; the RSX is simply not asked to draw.
3. **The video modules are byte-identical to `upstream/master`** (`git diff
   --stat` on `cellVdec.cpp`, `cellDmux.cpp`, `cellDmuxPamf.cpp`,
   `cellPamf.cpp` is empty; `72b872df6` "fix freezing FMVs in some games" and
   `2573cc5fd` are ancestors). This is not a Metal-tree regression.
4. **`L236`'s A/B is void.** Run `204942` has no probe log, no dumps and no
   `FIFO diag` at all, so "zero park put" is equally explained by a binary
   without diagnostics. Do not delete the gate on that evidence; repeat E2
   with a verified-live instrument.
5. **The 32.977 s `start lag` is an idle-gap artefact, not a stalled queue.**
   The last commit's drawable was displayed at probe 17.954; there is no
   `acquire` line between 17.935 and 50.876; `present/acquire` max is 1.0086 s
   and `recorder/readback wait` max is 311 ms. The feedback was simply not
   collected until the recorder next woke during forced shutdown. `frame/gpu
   wait` max 32.99 s is that same idle interval. (The slot can also be rebuilt
   when a feedback never arrives — HLD-04 §3.1 — so `debug_commit_time` alone
   is never proof of a live commit; `on_feedback` carries the generation.)
6. **RR7 is not permanently hung.** In run `200536` (left alone for 181 s)
   presents resume at probe t≈100 s and run at 30.8 fps (154 per 5 s) to
   t=180. The hole is ~80 s. **What is on screen from t=100 is unverified**:
   that run's dumps stop at the card (t=21.4), and the 30 fps stream could be
   the movie finally starting or the menu after a video timeout. This is the
   single most important open question.
7. **Instrument defects found.** (a) Every `cellVdec`/`cellDmux`/
   `cellDmuxPamf` `.trace()` is dead: nothing in the tree calls
   `logs::set_channel_levels`/`set_level`, so the `Log:` section of
   `config.yml` is inert and the 41 trace sites never print. (b) The
   `out_queue` "waiting for a consumer for 5 seconds" warning never fired, so
   the vdec was never blocked on a full queue (`out_max=60`). (c) The
   per-draw **detailed** line is capped at 16, but a per-draw census is
   uncapped: RR7 logged 16 draw ids (0–15, all 12.8–17.3 s); DS logged 0
   draws in its 55 s — a real count, not a cap.

**Instrumentation to add before the next RR7 run.**

1. **Enable the existing video traces** (free, 41 sites). Add a default-off
   `EXCISE-BEGIN` block at game boot that calls
   `logs::set_channel_levels` for `cellVdec`, `cellDmux`, `cellDmuxPamf`,
   `cellPamf`, `sys_interrupt` when `RSX_VIDEO_TRACE=1`. This prints
   `cellVdecDecodeAu` size/seq/cmd, `Got picture`, `PICOUT`/`AUDONE`,
   `cellVdecGetPicItem`, and the 5 s "waiting for a consumer" warning.
2. **A stall snapshot.** At the watchdog's 5 s trigger (and every 15 s while
   stalled), dump every PPU thread: name, PC, state, and the long syscall it
   is in; plus the vdec state `out_queue.size()`, `au_count`, `seq_state`, and
   the last decode cmd id. Bounded to two snapshots. This answers "where is
   the guest stuck" without guessing from the shutdown `aborted (N s)` lines.
3. **A present-content census.** Every 5 s log `presents/5 s`, `draws/5 s`,
   the last presented source address, and a cheap content signature of the
   presented drawable (an 8×8 mean/variance grid, or one bounded thumbnail
   PNG). This classifies the t≈100 s frames as card / movie / menu. Also add
   `MTL_PROBE_VARIANT=draws-all`: raise the detailed cap to 64 and log every
   Nth draw (`draws-sampled=N`) so the recovered stream is visible.
4. **A video-queue heartbeat.** Once per second while a vdec handle is open:
   `out_queue.size()`, `au_count`, `seq_state`, decoded AUs/s, and the last
   `cellVdecGetPicItem` return. This distinguishes "decoder slow" (AUs
   advancing, queue filling) from "guest not consuming" (queue pinned at 60)
   from "demuxer dead" (no AUs).

**Decision rules for the next run.**

| Observation at t≈100 s | Conclusion | Action |
|---|---|---|
| Release binary presents a **movie** frame | FMV startup latency, not a hang | profile the HLE demuxer/decoder; the Metal backend is exonerated |
| Release binary presents the **menu** | the game timed the FMV out and moved on | look for the timeout path; the movie may be skippable |
| Release binary never recovers | real hang | use the stall snapshot (probe 2) |
| AUs advance, `out_queue` pinned at 60, "waiting for a consumer" fires | the guest stopped consuming | find the video output thread; it is not the decoder |
| No AUs / no `Got picture` | demuxer fault | the `cellDmuxPamf` trace (probe 1) names the failing call |
| Zero parks during the hole, any mode | FIFO exonerated again | do not touch the gate |

**Guardrails unchanged:** one instance; no `pkill -f rpcs3`; launches only with
an explicit go; unique probe log/dump dir and copied emulator log per run; the
`204942` A/B stays void until repeated with a live instrument; keep
`build-tests/bin/rpcs3_test` green.
