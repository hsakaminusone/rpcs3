# Demon's Souls — the three Metal 4 rendering defects

**Status: live handoff. This is the only investigation/handoff document for
Demon's Souls (BLUS30443) on the Metal 4 backend. Read §0, then §1, then §5,
then §10.**

Scope is exactly three symptoms. All three are now confirmed as pixels in the
`ds24d` present stream, not inferred from constants:

1. **White flashing floor / water** — the ground/water surface renders pure
   white while the rest of the scene is drawn.
2. **Whole-image black/grey flicker** — whole frames where the 3D scene is
   absent (HUD only, 85–99 % of pixels black), plus whole frames that are
   92–94 % pure white.
3. **Sharpness split** — only the right half of the image is sharp; the left
   half is blurred, with a vertical seam at exactly `x = 640`.

The backend design layer (`HLD-*`, `LLD-*`, `DEVIATIONS-*`, `CAPTURE-DESIGN.md`,
`CONFLICTS.md`, `DESIGN-REMAINING.md`, `REALISATION.md`, the parity audits and
`README.md`) is unchanged. The dated Demon's Souls investigation artifacts were
removed on 2026-11-24; their live conclusions are folded into this file.

---

## 0. Handoff in one page

### 0.1 Do we know the root cause?

**Not yet — but the question changed shape on 2026-11-25.** For the current
capture the fragment-constant chain is excluded on every symptom frame (F55,
E18), and the left-half seam is proven to be a screen-space boundary written in
place (F57). The leading family is now backend resource identity and contents —
stale or uninitialised surfaces, partial destination writes, and missing
cross-frame hazards (F61–F63) — not shader math. Both resource-lifetime gaps in
that family are now closed in code: the guest-memory re-dirty test (F61) and the
invalidated-surface frame clock (F62). Whether either produced these frames is
the next measurement. The backend difference that looked decisive, Metal's
viewport rectangle, is not a proven mechanism: Metal feeds it from the
pitch-corrected layout width while VK/GL use the raw surface clip, and the
scale > 1 case was genuinely broken and is now fixed (F60). What exists so far is a proven
*consumer* chain and a measured *negative* on every hooked producer path; that
is not an RCA.

| Question | Answer | Basis |
|---|---|---|
| Is the Metal fragment-constant ring/offset inventing the wrong number? | **No.** The fill applies one deterministic transform `T` to the live ucode image, the ring stores the result, the shader reads `fc[x + fs_constants_offset]`, and the probe reads the same shared buffer at the same index. The delivered word is `T(image word)` by construction. | F49; `ProgramStateCache.cpp:642-687`; `MTLGSRender.cpp:6250-6280`; `MTLFragmentProgram.cpp:253,264` |
| Is the defect already in the ucode image bytes? | **For the ds22/ds23 strobe, yes** (F19). **Not for the 2026-11-25 symptoms:** the convention gate fired 0 times in a run containing all four symptom classes. | F19, F49, F55 |
| Does a hooked CPU store write them? | **No.** PPU JIT/interpreter, SPU DMA and `vm::write` are live (positive control) and none lands in a defect quad, a `fixsrc` image, or carries a `fix.log` word. | E15 (F44) |
| Does the `nv308a` copy create them? | **No.** Every sampled line is `fmt=11 mode=memcpy same=1`, `dst_bytes == src_bytes`; the permuted word is already in the main-memory source. | F51 |
| Is the "guest writes a byte-reversed word" conclusion real? | **Unproven — and the usual proof is circular.** `fix.log` flags a lane only when `!plausible(raw) && plausible(byteswap(raw))`, so every flagged lane is a byte reversal by definition. | F50 |
| Is the fragment-constant producer the whole story for the white floor, the left-half blur and the world-absent frames? | **No.** 74 white-floor, 12 flash, 31 absent and 2 blur-left frames were captured in a run where `source=fill` never fired. | F55, E18 |
| Is the `x = 640` seam a texture size, pitch, tiling or UV error? | **No.** The edge is in place with continuous geometry across it: left blurred, right sharp, same wall and character. | F57 |
| Is the seam Metal's viewport rectangle? | **Not proven.** Metal's rectangle comes from the pitch-corrected layout width, VK/GL's from the raw surface clip, so the inputs can differ even at scale 1; Metal also failed to scale the rectangle at scale > 1 (now fixed). No measurement ties either to x=640. | F60 |
| Are the surfaces' contents and lifetimes safe? | **Both gaps are now closed in code; neither is proven to produce these frames.** The guest-memory re-dirty trigger (F61) and the invalidated-resource frame clock (F62) are implemented. | F61–F63 |
| Which frames present the full target? | Every symptom frame is the full member of a double-buffered duplicate pair; the 1-target frames are its duplicate, not a lost world. | F59 |
| Is `fp38` (the velocity-weighted blur) the producer of the seam? | **Untested.** The only skip run never matched fp38 — fragment-program ids are assigned by compile order and differ between runs, so id 38 did not exist in that capture — and the capture contained no blur-left frame. | ds24e; §11 2026-11-26 |

### 0.2 The three symptoms, with the frames that show them

All frames are `build/probe/run-20261124-132656-ds24d/dumps/host-present-*.bin`
(BGRA8, 1280×720, read as raw). They are the final composed image, HUD included.

The 2026-11-25 run (`build/probe/run-20261124-145124-ds24d`) adds frames 3116
(white ground, hard shadows, foreign map patch), 3118 (same camera, dark
ground), 3410 (sharp) and 3411 (left-half blur, seam at `x = 640`); its 335-frame
window classifies as `floor-white 74 / flash 12 / absent 31 / blur-left 2 /
stable 216`.

| Symptom | Frames | Measured signature |
|---|---|---|
| White floor / water | 3037, 3045, 3049, 3060, 3096 | scene drawn, 18–19 % of pixels ≥ 250, 1–2 % pure black; the white area is the water/ground surface |
| Whole-image white flash | 2967–2968, 3021–3022, 3068–3069, 3102–3105, 3128–3129, 3402–3404, 3539–3540, 3549–3550, 3597–3598, 3664–3665 | mean 245.9, 92–94 % of pixels pure white, HUD visible |
| Whole-image black / grey | 2712–2718, 2785–2814, 2955–2959, 3678–3680 | 85–99 % of pixels black, 3D scene absent, HUD present (2955–2959 is HUD-only on black) |
| Sharpness split | 3090–3091, 3399, 3415–3416, 3429–3430, 3455–3456, 3461–3462, 3477–3478, 3585–3590, 3652–3653, 3672–3675 | left/right high-frequency energy ratio **0.25–0.55** while the HUD stays sharp; seam at `x = 640` |

Reproduce the classification in ~25 s with no rebuild:

```bash
RUN=build/probe/run-20261124-132656-ds24d   # or $(cat build/probe/.last-run)
python3 build/probe/classify-present.py "$RUN"   # writes present-analysis.json + a frame table
```

The viewer used on 2026-11-24 is a ~30-line raw-BGRA→PNG snippet (see §10,
"Phase 5"); a PNG of any frame is enough to see all three symptoms directly.

### 0.3 The one experiment to run next

The next discriminator is a backend one, and it is already implemented:
`MTL_PROBE_VARIANT=serialise` (`MTLGSRender.cpp:1968`, one command per encoder,
wait-idle per draw). If a symptom goes under it, the defect is a missing GPU
edge across the ring; if it stays, the defect is content. `heaps` is the second
arm (tracked hazards).

```bash
MTL_PROBE_VARIANT=serialise ./build/probe/launch-ds24d.sh
MTL_PROBE_VARIANT=sentinel  ./build/probe/launch-ds24d.sh   # magenta = never painted
MTL_PROBE_VARIANT=poison    ./build/probe/launch-ds24d.sh   # unwritten word names its allocation
```

The logging the audit asked for is now in place, and two of its questions are
answered in code rather than only observed:

1. **Present source (H16).** The flip now picks the newest non-depth section
   and falls back to the texture cache; the overlap list is available at level 5.
2. **Surface bind/re-dirty (H17).** `surface::memory_barrier` re-tests a `ready`
   surface and sets `erase_bkgnd`; the bind stream is logged at level 2.
3. **Depth resolve (H20).** Resolve targets and their rects are logged at
   level 3; the half-written-depth premise is withdrawn, so this is now
   background data rather than a discriminator.

`literal-provenance`, `freeze-fc` and `freeze-fs-offset` remain available, but
the 2026-11-25 run already made the first two non-decisive for these symptoms
(F55). `freeze-present=120` is still the way to hold a symptom frame long enough
to dump it.

### 0.4 What is still unknown

* Whether the present still selects a wrong resource after the depth filter; the
  next capture logs the chosen section (H16).
* Whether the stale-contents path (F61) or the surface-lifetime path (F62)
  produces the foreign map patch and the noise block (H17–H18). Both paths are
  now guarded in code, so the next run is the test: if the symptoms persist, the
  cause is elsewhere; if they change, the guard moved the frame.
* The half-written-depth mechanism is withdrawn (H20); the seam still needs a
  pass-level explanation (F57/H11).
* Whether any symptom survives `serialise`/`heaps`; that separates a missing
  GPU edge from content (H19).
* Why whole frames lose the 3D scene (H14) — unchanged.
* The producer of the animated ucode lane is still unobserved, but it is no
  longer on the critical path for these symptoms (E18).

---

## 1. Position as of 2026-11-26

* **Tree.** `metal4-dev`, pushed to `origin/metal4-dev` on 2026-11-26. The
  code/fix commit is `3af46b3a9` (the doc and probe-corpus cleanup notes follow
  it). The F60/F61/F62 fixes, the instrumentation bounds, the DS document and
  the two new `RSXProducer*` headers are in that commit. `release/metal4`
  remains one squashed commit with no docs and no instrumentation; never
  force-push `origin/metal4`.
* **Binary.** `build-tests/bin/rpcs3.app` rebuilt after the fixes;
  `rpcs3_test` green (689/689, 2 disabled); `.metal4-release-scrub.py check`
  clean (653 units excised, 0 needs-review, 0 residuals, 0 orphans, 0 empty
  bodies).
* **Code changed on 2026-11-25, all in the leading family.**
  1. F61: `surface::memory_barrier` now re-tests a `ready` surface against guest
     memory when `force_data_load` or the read-buffers configuration asks, and
     forces `erase_bkgnd` on a mismatch, mirroring VK/GL.
  2. F62: invalidated surfaces carry `frame_tag`/`frame_slot`; `resource_manager`
     keeps a monotonic frame serial and a per-slot completion clock, and both
     reuse paths (`find_reusable_matching_surface` and the
     `surface_matches_properties(..., true)` path at `surface_store.h:554-580`)
     refuse a surface until its slot reports the frame complete.
  3. F60: `capture_encoder_state` carries a scaled `raster_width/height`, the
     viewport and scissor Y-flip use it, and `framebuffer::bind` stores and
     compares the scaled pass extent.
  4. Present: the flip takes the newest **non-depth** section of the overlap
     list instead of `back()`, falling back to the texture cache.
  5. Instrumentation: origin scans RSX local memory first, `note_defect` is
     needle-only, `record_copy`/`record_surface` no longer seed the store
     filter, and the harvest drain is capped at 1.5 M lines.
* **Last full run.** `build/probe/run-20261125-231354-ds24e-skip38`
  (`build/probe/.last-run`), 232 present frames 3261–3816, classes
  `stable 125 / absent 78 / floor-white 22 / flash 7 / blur-left 0`. It was
  meant to skip `fp38` but **did not** — that id is not compiled in this run —
  so it is a baseline, not a skip test (§11 2026-11-26). The previous reference
  run `build/probe/run-20261125-195520-ds24d` (96 frames 4685–4780,
  `stable 88 / blur-left 2 / flash 2 / absent 3 / floor-white 1`) still owns
  the decoded post chain (F65) and the persistent-blur measurement (F64).
* **Next action.** A capture that actually contains a blur-left frame, then a
  skip of the *same run's* last pre-HUD writer (read the id from that run's
  manifests; do not reuse `ds24d`'s `fp38`). If no id in the run selects the
  seam, change `MTL_PROBE_SKIP_FP` to match on the fragment ucode hash so the
  target survives id renumbering. Until a skip actually fires, the pass-level
  producer of the seam is unmeasured.
* **Why this took three weeks.** Four things, all now corrected:
  1. **The detector was circular.** `fix.log` records only lanes where the
     delivered word is implausible and its 32-bit reversal is plausible, so
     "100 % of corrected lanes are byte-reversed" is a tautology (F50). Three
     generations of analysis treated it as evidence about the guest.
  2. **The PPU store hook did not run for six days.** The JIT object cache key
     did not include the translator, so every run reloaded objects compiled
     before the hook existed (F43). Every PPU negative from 2026-11-18…24 was
     void, but was read as "the PPU is silent".
  3. **The probe caps saturated on exactly the world family.** `watch_range`
     held 2048 ranges and round-robin-evicted the persistent program images in
     favour of transient copy sources; `stage-prearm` hit its 1024-line cap;
     `fill.log` is capture-gated and capped at 4096. The second half of every
     run was blind by construction.
  4. **The pixel dumps were produced in every run and never read.** One
     `host-present-*` frame answers which symptom is which. A 20-line screen of
     the 623 present frames ranks them by brightness, black fraction and
     left/right high-frequency ratio (F47). That was done for the first time on
     2026-11-24.
  Root-causing was attempted from the top of the constant-file provenance chain
  downward; the visual output, which is one step from the symptom, was not
  used. That inversion is the process failure, not any single missing probe.
* **The symptom set, restated from the pixels.** Three separate frame classes,
  not one: white floor (scene drawn, water blown white), world-absent frames
  (HUD only, black), and blurred-left frames (scene drawn, left half at half
  the detail). Whole-image white frames also occur. See §0.2 for frames and
  metrics.
* **What the `ds24d` probe adds.** The copy-destination hook names `nv308a` as
  the writer of the 8 delivered defect quads, all in program images, from
  main-memory sources that already carry the permuted word (F51); 0 hooked CPU
  stores are within ±0x200 of any of those sources, and 0 stores land inside
  the 16-byte sources. The store-hook coverage hole (`note_mem` returns before
  the address test unless `in_ring || in_stage || probe`, and `probe` is only
  the first 16 stores per writer) means "0 stores near the source" is not
  evidence for a plain non-band, non-needle store (F46, E15 caveat).
* **What the code audit adds.** The consumer path is one deterministic
  transform plus one index (F49). `fs_constants_offset` is an index, not a
  presence bit (F17), and the shader macro is `fc[x + _fs_constants_offset]`
  (`MTLFragmentProgram.cpp:253,264`). The probe and the shader therefore read
  the same bytes at the same index, so the delivered `world tail` value is not
  a probe artifact. The remaining consumer-side failure mode is a stale or
  aliased block (per-frame-slot ring lifetime), which `literal-provenance`
  classifies.
* **What the pixels do not yet tell us.** The `host-world-target-*` dump fires
  on the first `ucode_length >= 3000` world draw (F41), so it shows the clear
  colour plus the first character draws, not the finished world (F48). The
  final image must be read from `host-present-*` (or from a world-target dump
  taken at the last world draw). The detailed `Metal 4: draw` trace is capped
  at 16 lines per run (F53), so the bloom-composite draws are not described in
  `ds24d` and the half-width question cannot be answered from that log.
* **The 2026-11-19 finding still stands and now has a picture.** The world
  target's feedback copy is exact (F38) and the present is a single compose
  after the HUD (F39); the blur is therefore added before the HUD, in the
  pre-HUD pass chain. The `3415` frame shows it directly: same scene, same
  camera, HUD sharp, left half blurred.

---

## 2. Branch, build, gates

| Item | Value |
|---|---|
| Branch | `metal4-dev` |
| HEAD | `metal4-dev` tip; the code commit for the current state is named in the newest §11 entry |
| Parent chain | `5b3870866` (parity + docs) → `2f3a52558` (FP/ROP port) → `0ed64a761` (writable mirror) → `9c8bb94d2` (FIFO) → … → `180a3056a` (proven, armed watch) → `6108fefcf` |
| Upstream base | `fb6737b1c` |
| Ahead / behind | 52 / 0 |
| Build | `build-tests` (ninja); `-DMACDEPLOYQT_EXECUTABLE=/usr/bin/true` |
| Tests | `build-tests/bin/rpcs3_test` — 687 passed, 2 disabled |
| Scrub | `python3 .metal4-release-scrub.py check` — 0 residuals / 0 orphans / 0 empty bodies |
| Commit identity | `hsaka -1.0 <340957754+hsakaminusone@users.noreply.github.com>` |

Rules in force (from the user, do not relitigate): do **not** run Vulkan/OpenGL
as a differential test; do **not** `pkill -f rpcs3`; F12 is the only capture
trigger; every diagnostic is `TODO(excise)` + `EXCISE-BEGIN/END`, default-off and
non-load-bearing; the scrubbed tree must build.

---

## 3. The symptom, as reported

> "The screen flickers and strobes from black and white. For a milisec it shows
> the proper image sometimes."

And the framing that governs the whole investigation:

> "think in terms of what is metal backend doing differently than vk backend that
> this issue will manifest itself"

Two consequences that are easy to lose:

1. **A component shared with VK/GL cannot be the differentiator.** The FIFO
   parser, the RSX interpreter, `write_fragment_constants_to_buffer`, the
   decompiler and the ROP epilogue GLSL are shared. A Metal-only strobe must come
   from `rpcs3/Emu/RSX/MTL/*`, from something the MTL decompiler emits that
   VK/GL do not, or from Metal 4 semantics (page 0 of this file, §4 F7).
2. **"Black and white" is not "dark".** The earlier dark-world investigation
   (2026-11-16; folded into this file) and this strobe are now known to be
   the same lane and the same transformation, seen at different sampling rates:
   the dark-world captures landed on the tiny-multiplier half of the ramp.

---

## 4. Confirmed facts

Each fact is a measurement or a code citation. Anything inferred is labelled.

| # | Fact | Evidence |
|---|---|---|
| F1 | The FIFO parser and `cellGcmCallback` are now byte-identical to `upstream/master`; the branch parser, recovery rules and `test_rsx_fifo_streams.cpp` are removed. | `git diff upstream/master -- rpcs3/Emu/RSX/RSXFIFO.* rpcs3/Emu/Cell/Modules/cellGcmSys.cpp` is empty; `9c8bb94d2`. |
| F2 | The FIFO is shared RSX code, not Metal code. If the strobe is Metal-only, FIFO cannot be the differentiator. | Same files are compiled for every backend; `RSXFIFO.cpp` has no Metal guard. |
| F3 | `fs_constants_offset = 0` is not a defect: the MSL declares `fc[]` only when `constant_offsets` is non-empty, and that is the same vector the fill iterates. | `MTLFragmentProgram.cpp:258`, `MTLGSRender.cpp:6100-6145`. |
| F4 | sRGB packing is exonerated as the root cause. | Metal-vs-Vulkan parity audit, 2026-11-17 (folded into this file): the format/channel mapping matches VK and no sRGB or gamma divergence was found. |
| F5 | Residency / eviction is exonerated. | Metal 4 heap semantics measured on device, 2026-11-17 (folded into this file): a live binding is not evicted; heap harness #1. |
| F6 | Pass-transition barriers are covered: `close_encoder()` emits a producer `ALL→ALL` with `VisibilityOptionDevice`. | `MTLCommandRecorder.cpp:1051-1060`. |
| F7 | Measured Metal 4 semantics that differ from Vulkan and matter here: (a) an out-of-bounds heap read silently returns the neighbouring suballocation; (b) a flat `[[user]]` varying comes from the first vertex. | Device harness runs #1/#2, 2026-11-17 (folded into this file; the harness source is `EXPERIMENT-metal4-semantics.mm`). |
| F8 | The FP/ROP precision port (`2f3a52558`) is behaviour-neutral at the default `high` precision: `compareFunctionAlias` resolves to plain compares, and the ROP epilogue and `clamp16` paths now match VK/GL line-for-line. | Commit review; `ea602f81f`, `05a9d9605`, `94dbb7b58`, `906493bc1`. |
| F9 | `supports_denorm_preserve_f32` stays `false` for Metal. At `ultra` this emits the integer-key compare emulation, which is exactly correct — a perf nibble, not a defect. Do not flip without a measurement. | `MTLFragmentProgram.cpp` device_props; `FragmentProgramDecompiler.cpp` `_builtin_sxx`. |
| F10 | The DS evidence words for the world epilogue block are `[a1273fbd, 00400000, a1273fbd, 803f0000]`. | `build/probe/run-20261117-183052-ds20` (`fill.log`, `dumps/`). |
| F11 | (Inference from F10.) Under the 16-bit-half swap `T`, lanes 1/3 decode to `2.0` and `1.0`; lanes 0/2 decode to ~`4.5e-15`, but to ≈`1.4817` under `(raw<<16)|(raw>>16)`. One 16-byte block appears to carry two byte-order conventions. | Decode worked in the 2026-11-17 verification (folded into this file); update if a run contradicts. |
| F12 | The guest continuously patches the ucode tail: `a11e3fbd → a11f3fbd → a1203fbd` (a 1-ulp mantissa ramp). | `watch.log` in `ds20`. |
| F13 | The strobe mechanism, as decoded: the epilogue is `clamp(h0 * fc[67+off].x / fc[68+off].y)`. A word-order flip on the multiplier gives exact black; on the divisor (denormal → FTZ → +inf) gives clamped white. **Superseded by F18-F22:** only the multiplier lane is wrong; the divisor is always the authored `2.0`, and the multiplier's transposed sign/magnitude produces both classes. | Inference from the decompiled MSL + F10; F18-F22 replace the divisor half. |
| F14 | Metal's no-copy guest import covers the sudo mirror (`0x1'0000'0000` bytes from `vm::g_sudo_addr`) and only needs the range `PROT_READ|PROT_WRITE`; `memory_commit` added a pointless 4 GiB `MADV_WILLNEED`. | `MTLDevice.cpp:352-371`; `vm_native.cpp:315-330`. |
| F15 | A default F12 capture is 2 frames wide, not 1 (`capture_active` is a `<= last_frame` watermark), and the ring dump's forward half is `max(6, …)`-by-intent but `capture_frame_count()` in fact — so `p1`+`p2` only. | `MTLGSRender.cpp:350-440`, `MTLConstants.cpp:696-711`; measured on 2026-11-18 and recorded in §9. |
| F16 | The four Phase-2 arms are implemented, default-off, bounded and `EXCISE`-wrapped: `literal-provenance` (fill-vs-draw class per fp id), `freeze-fc` (replay the first fill of each fp), `freeze-fs-offset[=n]` (every draw reads block n), `freeze-present[=n]` (hold the present source for n presents, default 120). `freeze-present` also suppresses the automatic present/upscaled dumps, so the archive cannot show a live frame the screen was not showing. | `MTLGSRender.cpp:3194-3606`, hooks `:6580-6656`, present `:8270-8290`; arm docs in `MTLInstrumentation.h`. |
| F17 | **Diagnostic defect, fixed:** `params->fs_constants_offset` and `params->xform_constants_offset` were used as truthiness tests at four sites, so block 0 — a valid block and the first allocation of a frame — made `hash_fragment_constants`, `hash_xform`, the `world draw` tail and the `world fc` head read as zero. The test is now the pointer/count only. This is not a renderer defect (F3 still holds); it is why block-0 evidence was missing. | `MTLGSRender.cpp:6798-6812`, `:6859`, `:6902`. |
| F18 | **The epilogue consumes exactly the suspect lane.** `fp-70.glsl:1023` is `r1.xyz = (h0 * _fetch_constant(67).xxxx).xyz;` and `:1024` is `h0.xyz = precision_clamp(_builtin_div(r1, _fetch_constant(68).yyyy.x), 0., 1.).xyz;`. So the multiplier is `fc[67].x` and the divisor is `fc[68].y`. The two quads are byte-identical, and the same pair is the tail of both world programs: fp 70 (3312 B, quads at 3264/3296) and fp 63 (3376 B, quads at 3312/3344). | `build/probe/run-20261118-095414-ds21/dumps/fp-70.glsl:1023-1024`, `fp-70.const.txt` `[67]`/`[68]`, `fp-63.const.txt` `[67]`/`[68]`, `fp-70-live-1.ucode.bin`. |
| F19 | **The delivered multiplier is the 16-bit-half swap of the animated value.** Block layout is `[W, 2.0, W, 1.0]` in guest words `[a1273fbd, 00400000, a1273fbd, 803f0000]`. The fill applies `T` (swap the two bytes within each 16-bit half); `T(00400000) = 2.0` and `T(803f0000) = 1.0` — correct — but `T(a1273fbd) = 4.48917e-15` where the half-swap `(a1273fbd<<16)|(a1273fbd>>16) = 3fbda127 = 1.48148`. Equivalently, the intended value is `bswap32` of the f32 the shader fetches. Lane 0/2 are wrong in every block; lanes 1/3 are always right. | `fill.log` (`[67]`/`[68]` decode columns), `mtl-probe.log` `world draw … tail`, `fp-*.const.txt`. |
| F20 | **tape classification of the strobe.** 80 distinct present-stream images survived across the six presses (frames 2993-4038): 62 black-class, 8 white-class, 10 transitional. Black-class: ≥ 96 % of sampled pixels are exactly `#000`, mean luma 1.0, and the 100 %-black frames are whole-image (mean 0.0). White-class: 93.7 % exactly `#fff`, mean 246.3. White runs are 1–2 presents; black runs 1–12. The newly-black bbox of a transition is scene-sized (up to 807×596), not a band or a sprite. HUD visibility is independent of the class: absent from the 100 %-black frames, present in others. | `host-tape-f*-present-s0-*.bin` and the `tape present` lines in `run-20261118-095414-ds21/`. |
| F21 | **The feedback path and the present chain are not adding the flip.** Every `frame witness` is `feedback live 0 copy N`, and every `feedback witness` is `path copy`. The unit-2 feedback sample at `0xc0b50000` (the world draw's depth target) is byte-stable across black and white presents (50 % black, mean 96), so geometry/depth are identical in both classes and the difference is generated in the world draw, not after it. | `mtl-probe.log` `feedback witness` / `frame witness`; `host-tape-*-sample-*-u2-addrc0b50000-*.bin`. |
| F22 | **Quantified over every world fill.** fp 70: 217 logged fills of the trailing pair, intended multiplier in `[0.3151, 2.2220]` every time; delivered 43 positive-huge (>1e6), 63 negative-huge, 86 <1e-6, 25 in `1e-6..1e6` — of which exactly 1 (`0.6836`) is in the intended band by accident. fp 63: 868 fills, intended also `[0.3151, 2.2220]`, 836/868 delivered implausible (172 positive-huge, 252 negative-huge, 344 tiny), 4 accidental in-band. No non-finite. Positive-huge is the white class; negative-huge and tiny are black. The existing oracle's `|v| > 1e6` gate therefore misses the 86+344 tiny cases, which are the majority of black frames. | `fill.log` (all `fill fp off …` lines), decoded against `T` and the half-swap. |
| F23 | **The stray word is family-wide; the oracle covered two programs.** `ds22`'s per-draw `literal watch` reports 49 compiled fragment programs with a non-uniform trailing pair. About 30 carry exactly `[W,2,W,1]` with `W` still huge or tiny at draw time — e.g. fp 17/53/40/118/153 `W=71623fb6` → `1.1147e21`, fp 11/121/24/104/99/20/88/108/8/41/63/34/5/42/142/86/57 `W=3ee13ff6` → `-2.20164e20`, fp 26 `a1183fbd` → `4.18e-24`, fp 48/120 `04fe400e` → `-4.3883e37`. Only fp 70 and fp 64 (the two `ucode_length >= 3000` images) were corrected to `1.48148`. The unrelated 2D programs in the same set (fp 19/33/47/89/90/96/31/140/83/93/51/6) have different tail shapes and are excluded by the `[W,2,·,1]` guard. | `ds22/mtl-probe.log` `literal watch fp … guest … ring …`; oracle `fix.log`. |
| F24 | **The family-wide oracle removes the full-screen strobe.** `ds23` (length gate removed, 5 F12 presses at frames 3384/3927/4209/4389/4465) corrected 221 834 lanes across **41 distinct fragment-program offsets** (fp 21439232 … 22560000; the list is in `fix.log` `$4`). `world tail` reaches the shader as `1.69832 2 1.69832 1`; the 61 programs the per-draw watch sees include **no `[W,2,·,1]` program still delivering an out-of-band value**. Present classes 32 black / 11 dark / 51 mixed / 2 white out of 96 (ds22: 140 / 81 / 11 / 88 of 320). The world is visible; the user confirms "a LOT better". | `run-20261118-103456-ds23/fix.log` (`awk '{print $4}' | sort -u | wc -l` = 41), `fill.log`, `mtl-probe.log` (`literal watch`, `world tail`, `tape class`); `png/p4465.png`. |
| F25 | **The two white presents are world-absent, not a shader flash.** `host-present-4461` is HUD-only on a pure white background: the 3D world did not reach that present. So the residual white class is a missing world draw/present, not the epilogue saturating. What remains in the world itself is the ground alternating white↔black with the world otherwise drawn (t4194 vs t4195), plus a blurred world (p4465: left half blurred, right half sharp; t4195: whole world blurred, HUD sharp). | `run-20261118-103456-ds23/png/` (`p4461`, `t4194`, `t4195`, `p4465`), decoded from `host-tape-f*-present-s0-*.bin` (BGRA8, 1280x720). |
| F26 | **The slot holds the correct ucode word in the wrong endianness.** The game's own representation of the value is big-endian: `3f bd a1 27` (BE f32 `1.48148`) appears 33× in each `frame-*-transform-constants.bin` (and in `frame-*-vertex-context` feed data), and 122× as the **little-endian** host f32 `27 a1 bd 3f` in each post-fill `frame-*-fragment-constants.bin`. The ucode slot holds `bd 3f 27 a1`. Writing `W = 0x3fbda127`, the ucode's correct slot is `LE_bytes(T(W)) = a1 27 3f bd`; the observed slot is `BE_bytes(T(W)) = bd 3f 27 a1`. So the producer applied the RSX half-word shuffle (`T`) correctly and then stored the 32-bit word big-endian instead of little-endian: **exactly one missing `bswap32`**, no other byte scrambling. | Byte-pattern scan of `ds23/dumps` (10 145 files ≤ 200 KB): BE pattern in 251 files (33× each in `frame-3565-p*-transform-constants.bin`), LE pattern in 168 files (`frame-*-fragment-constants.bin`), observed `bd 3f 27 a1` in 43 `fp-*.ucode.bin`, and the correct `a1 27 3f bd` in **0** files. |
| F27 | **The static lanes and the animated lane have different producers.** `2.0` / `1.0` / `0.9` in the same quads are `LE_bytes(T(W))`; the animated lane alone is `BE_bytes(T(W))`. A whole-image convention cannot produce that mixture, so the animated lane is written by a second writer (the per-frame patcher) after the assembler wrote the image. The *assembler* format is confirmed by the instruction stream itself: `decode_instruction` (`FPToCFG.cpp:27`, "RSX's weird half-word shuffle") applies the same `T` to every instruction word, and those work. | `fill.log` `[65]..[68]` words; `fp-71.const.txt`; `FPToCFG.cpp:27-40`; F26 scan. |
| F28 | **The constant is causally consumed (closes the handoff's "unproven").** `fp-71.metal` (id from `literal watch fp 71 … off 21642368`) computes `_1714 = (h0 * fc[67u + base].xxxx).xyz` and `_1731 = precision_clamp(r1 / fc[68u + base].y, 0, 1)`, where `h0` is the lit colour. So the reversed word *is* the epilogue multiplier: huge → clamp 1, tiny/negative → 0. It is not written to a dead temp. | `ds23/dumps/fp-71.metal:547,551` (and `:530` for `fc[66]`); `ds23/mtl-probe.log` `literal watch fp 71 … off 21642368`. |
| F29 | **No quad in the 4096-line census is uniformly half-swapped.** `conv` lines: `c2 = 0` in every logged fill (0 of 4096), and `lanesH` is only ever the two animated lanes of a trailing pair. So the stray is lane-local to the trailing pair in every program the census saw; a whole-quad transposition (`c2 > 0`) never occurs. `neither` quads (no convention gives 4 plausible lanes) exist — max 14 in one 30-constant program — and are the candidate set for H7. | `fill.log` `conv` lines parsed by column; see §11. |
| F30 | **The JIT's byte-reverse stores are not the defect.** `PPUTranslator::WriteMemory` is the single chokepoint every guest store passes through; `m_is_be` is `false` (`PPUTranslator.cpp:32` passes `false` to `cpu_translator`), so a `stw` swaps and a `stwbrx` (`WriteMemory(..., is_be=false)`, `STWBRX` at 3743) does not. On a little-endian host that yields the correct BE bytes for `stw` and the correct LE bytes for `stwbrx`, and the interpreter agrees (`PPU_WRITE(le_t<u32>)`). So the observed `BE_bytes(T(W))` is what a **plain `stw` of `T(W)`** writes; it is not a missing swap in `stwbrx`. | `PPUTranslator.cpp:32,951-988,3743-3746`; `PPUInterpreter.cpp:5425-5434`; `CPUTranslator.cpp:25-27`. |
| F31 | **The producer has a cheap, exact signature the store watch can key on.** Let `word` be the u32 a 4-byte store passes to `WriteMemory` (i.e. the LE read of the bytes written); the fill delivers `slot = T(word)`. The defect is exactly `!plausible(slot) && plausible(bswap32(slot))`: observed `word = 0xa1273fbd` → `slot = 0x27a1bd3f` (4.49e-15, implausible), `bswap32(slot) = 0x3fbda127` (1.48148); the correct `word = 0xbd3f27a1` → `slot = 1.48148` (not suspicious); static `2.0` → `slot = 2.0` (not suspicious). So a store hook can log only the writes that carry this shape and stay near-silent on a normal run. | Derived from `ProgramStateCache.cpp:655-668` (fill applies `T` to every word) and the observed tail bytes; numerically checked against ds23. |
| F32 | **Every writer layer has a single chokepoint, so one hook set covers all of them.** Guest PPU stores: `PPUTranslator::WriteMemory` (JIT) and `PPU_WRITE` (interpreter); SPU DMA: `spu_thread::do_dma_transfer` (SPUThread.cpp:2125) and `do_list_transfer` (2927, where a 16-bit element size would be visible); HLE: `vm::write`; RSX: the existing `program_write_watch`. The command ring is announced by `cellGcmInitInternal` (`cellGcmSys.cpp:446`, `ioAddress`/`ioSize`, `g_defaultCommandBufferBegin`/`cmdSize` at 513-518) and every submit crosses `set_rsx_dmactl` (`sys_rsx.cpp:151`), which can name the PPU thread and its PC. | Source read; see §10 Phase 3. |
| F33 | **The JIT already has a supported way to call a C++ hook from generated code.** `s_link_table` (`PPUThread.cpp:5168`) resolves `__write_maybe_mmio32` (5188); a `__ppu_stage_watch` entry plus one `m_ir->CreateCall` in `WriteMemory` uses the same mechanism, so no ABI or symbol work is needed. | `PPUThread.cpp:5168-5195`; `PPUTranslator.h:324-335`. |
| F34 | **The producer watch is implemented on every chokepoint, default off.** `rsx::fp_stage_watch` (`RSXProgramWriteWatch.h`) holds the `cellGcmInit` io ring plus every `nv308a` source range (merged), and is fed after the store by: `PPUTranslator::WriteMemory` (all guest JIT stores, with `is_be` per opcode and the guest PC), `STVLX`/`STVRX` (masked vector stores that bypass `WriteMemory`), `vm::write` (interpreter `PPU_WRITE` and HLE builders), `spu_thread::do_dma_transfer` (all list element PUTs included; the guard runs on every exit path), `rsx::make_command`/`make_jump` and `cellGcmCallback`'s raw jump store, and the two `nv308a` copy forms (`watch_range`). `set_rsx_dmactl` logs every submit with the PPU PC; `cellGcmCallback` logs get/put. A 4-byte word is logged if it is inside an `nv308a` source range, or it matches the F31 shape, or it equals `RSX_FP_STAGE_NEEDLE`; 1/2-byte stores inside a staging range are logged as `stage-partial`. Dedupe is per (site, address, word), with a 65536-line cap and 4096 per site. Inert unless `RSX_FP_STAGE_WATCH` names a file. | `RSXProgramWriteWatch.h` (`fp-stage-store-watch`), `PPUTranslator.cpp` (`WriteMemory`, `STVLX`, `STVRX`), `PPUThread.cpp` (`__ppu_stage_watch`), `vm.h`/`vm.cpp` (`fp_stage_watch_write`), `SPUThread.cpp` (`stage_guard_t`), `cellGcmSys.cpp` (arm + builders), `sys_rsx.cpp` (`log_submit`), `nv308a.cpp` (`watch_range`). |
| F35 | **The producer-side corrector is a separate, falsifiable arm.** `RSX_FP_STAGE_FIX=stage` byte-reverses a suspect word *in guest memory after the store*, when all of: the F31 shape holds, two words of the 16-byte quad are plausible, and the address is inside an `nv308a` source range (`io`/`all` extend it to the whole ring). The log line is written first and carries the original word with `fix=1`, so the run remains self-describing. With the fill oracle off, a strobe-free ground confirms the producer is the whole story; a surviving strobe falsifies it even though the writer is named. | `RSXProgramWriteWatch.h` `fix_level()`/`quad_sibling_plausible()`/`note_mem()`; launch script `build/probe/launch-ds25.sh`. |
| F36 | **A complete in-order draw list of a real frame is decoded.** `dumps/frame-6637-m9-manifest.txt` (header `present 6628`) holds 293 records, all `frame 6628`, in submission order: 51 shadow-map draws (`fp 103`, no colour target, 2048² depth `0xc4860000`, scissor 1024² quadrants at three corners), then **115 world draws** into `0xc07a0000` (`fp 86, 71, 62, 26, 20, 7, 118, 75, 16, 89, 101, 104, 23, 105, 4, 9, 36, 43, 64, 40`; all `prim 6` triangle lists); `fp 141` (52-vertex strip, no textures, constant colour); **copy 1** `fp 110`; 25 draws (`fp 155` 52-v strip sampling 2D+cube, `fp 142` 36 v, `fp 113` strips, `fp 87` 4 v, `fp 140` 4 v, `fp 134` 5 × `prim 6` 4–526 v) sampling `0xc5870000` + depth; **copy 2** `fp 110`; `fp 57` (2 strips); 16 bloom passes into their own targets; `fp 88` bloom composite into `0xc07a0000`; `fp 14` into `0xc4260000`; 70 × `fp 28` + 7 × `fp 8` + 1 × `fp 31` (HUD sprites/text/overlay, 4 v each) into `0xc07a0000`; `fp 95` present compose into `0xc03d0000`. | Manifest file; log `capture geometry` for the same draws; §11 2026-11-19 entry. |
| F37 | **The world target has no half-width scissor or viewport.** All **222** draws that write `0xc07a0000` in frame 6628 use `scissor 0,0,1280,720` and the full viewport; zero are narrower. The only sub-full scissors in the frame are the shadow atlas (`1024,0,1024,1024`, `0,1024,1024,1024`, `1024,1024,1024,1024`) and the bloom targets (`640x360`, `320x180`, `160x90`, `64x64`, `16x16`, all at origin `0,0`). | Manifest parsed per record: 222 target-`0xc07a0000` draws, 0 non-full. |
| F38 | **The world feedback copy is exact, and the bloom chain is the only blurred source.** `fp 110` is a 4-vertex full-screen draw `0xc07a0000 → 0xc5870000` (no depth). In frames 6642/6732/6734 the copied image is byte-identical to its source (`cmp` clean; the same FNV hash and row 0 `00040404`). The bloom chain then downsamples the **full** world target: `fp 56/96` → `0xc4610000` 640x360, `fp 91` → `0xc4710000` 640x360, `fp 10` → `0xc4260000` 1280x720, `fp 102` → `0xc4810000` 320x180, `fp 50` → `0xc41d1fc0`, `fp 66` → `0xc420a400` 320x180, `fp 102/66/47` → `0xc4250980`/`0xc4242840` 160x90, `fp 83` → 64x64, plus 16x16 targets; `fp 88` composites `0xc4260000` + `0xc4242840` back into `0xc07a0000`. | `cmp` on the three sample pairs; manifest texture-unit lines; `host image sample` hashes. |
| F39 | **The present is a single 4-vertex compose, after the HUD.** `fp 95` is one `prim 8` draw, full scissor, sampling `0xc07a0000` into `0xc03d0000`. The 78 HUD/overlay draws (`fp 28/8/31`) write `0xc07a0000` immediately before it. A spatial filter at the present would therefore blur the HUD too; the HUD is sharp in the seam frames, so the blur is added before the HUD (or by the world draws). | Manifest records 249888–249966; `capture geometry frame 6643 … fp 95` with unit 0 = `0xc07a0000`; seam crops. |
| F40 | **The seam is intermittent per frame and the statistics are content-dependent.** One metric (mean absolute green-channel step per 2 px, y stride 4; band ratio = left/right within each 120-px y band) over the ten `host-sample-*-addrc07a0000` files: 6638/6640 black-frame signature (L/R mean 3.74/0.80); 6639 black (0.03/0.00); 6642 L/R mean 10.09/15.37, HF 1.19/2.10, band ratios 0.74/0.54/0.45/0.58/0.55/0.47; **6643 19.19/25.10, HF 2.24/4.20, bands 0.59/0.21/0.09/0.16/0.85/1.38 (strong seam)**; 6731 17.66/22.47, HF 2.18/1.67, bands 0.79/0.41/0.23/0.47/2.14/2.17; 6732 8.89/13.56, HF 1.05/1.85, bands 0.74/0.54/0.45/0.59/0.56/0.47; 6733 58.58/65.67, HF 2.96/2.43, bands 0.94/0.76/0.49/0.78/2.10/1.79; 6734 8.68/13.24, HF 1.02/1.80, same 0.74/0.54/0.44/0.59/0.56/0.47; 6735 16.99/20.85, HF 2.15/1.82, bands 0.81/0.61/0.31/0.48/1.95/2.23. 6642/6732/6734 are the same scene at three exposures (ratios agree to two decimals); 6733 is the brightest sample and has no seam, so the seam does not simply track brightness. The conclusive evidence of a blur is the visual crop (the character's head crossed at `x = 640`); the ratios are a locator, not a proof. | The ten samples, measured with the stated metric; `seam-6643-5x.png`. |
| F41 | **The existing capture cannot bracket one frame's post chain.** The capture dumps a bound texture image at the draw that binds it, once per host image per capture frame (dedupe key is the `MTL::Texture*`, serial `frame*100 + unit`). Which stage that is depends on where the capture window opens: `0xc07a0000` is first bound at `fp 141` (pre-effects) in frame 6642, but at the present compose (`fp 95`) in frames 6638/6643. A frame therefore carries **one** of the two stages, never both. The separate `world-target` dump is gated to 8 per capture and `ucode_length >= 3000`, which fires on the first world draws only (target still the uniform `0x202020` clear); `host-census` is a 5-second periodic present dump (24/29/34 s …). The level-5 `capture frame … unit …` site also reaches its 4096-line cap around frame 6648 in a 120-frame window, so the later pressed windows have image files but no stage labels in the log. | `dumps/` inventory; `mtl-probe.log` `host image sample` / `world target draw` / `host image census`; `MTLGSRender.cpp:659-700, 4780-4880, 5814-5841`. |
| F42 | **What the seam evidence leaves open.** The seam is in the present source (F39). The only *lit* pre-effects sample (6642, at `fp 141`) is clean and the only *lit* present-source sample (6643, at `fp 95`) is seamed; the other labelled samples (6638 present-source, 6639 pre-effects) are black-class, and no single frame has both stages. Candidates, all before the HUD: the 115 world draws, `fp 141` (no textures, cannot resample), the exact copy (F38), the 25 effects + `fp 57` sampling `0xc5870000` + depth, the bloom chain, `fp 88`, `fp 14`. The next capture must produce the pre-effects and present-source samples of the same frame. | F36–F41; §10 Phase 2.6. |
| F43 | **The PPU JIT hook has never executed: the object cache was never invalidated.** The cache file name is `v9-kusa-<base57(module-hash)>-<base57(settings)>-<cpu>.obj` (`PPUThread.cpp:5833`); the hash covers the guest module bytes and settings, not `PPUTranslator`'s output. `cb7b3c811` (2026-11-18 12:29) added the `__ppu_stage_watch` calls to `WriteMemory`/`STVLX`/`STVRX` and did **not** bump the literal, so `jit_compiler::load_object` returned the old objects and nothing recompiled. Measured: all **233** `v9-kusa-*.obj.gz` under `~/Library/Caches/rpcs3/cache/BLUS30443/ppu-…-EBOOT.BIN/` are dated 2026-11-12 20:36 and `gzcat … \| grep -c __ppu_stage_watch` is **0** for every one; across the whole cache, **0 of 372** `.obj.gz` are newer than 2026-11-14; `spu-safe-v1-tane.dat` was rewritten 2026-11-18 13:57, so the SPU cache did recompile. Fixed by bumping the literal to `v10-kusa` (§11, 2026-11-19 cache entry); the rebuilt `rpcs3` contains `v10-kusa` and no `v9-kusa`, `rpcs3_test` 687/687. Consequence: the zero `ppu-stw`/`ppu-stwbrx` result in every run before this fix says nothing about PPU stores, and the PPU layer is the most likely producer. | `PPUThread.cpp:5832`; `Utilities/JITLLVM.cpp:963`; cache inventory; F34. |
| F44 | **`ds24c` is the first run with every store layer proven live, and it excludes them all for the residual ground flash.** Positive control (F43 fixed): `ppu-stw` 1122 stores (991 `ring=1`, 115 `stage=1`), `ppu-stwbrx` 32, `spu-put` 4081 (3933 `ring=1`), `spu-putllc` 37, `spu-list-put` 2, `vm-write` 11. The fill corrected 192 662 lanes across **93 distinct defect quads** and **68 distinct program images** (`fixsrc`), with **677 distinct guest words** behind the defects, all in `c1472e70..c1583e10`. Decisive negatives: **0** stores land inside a defect quad; **0** of the 115 `stage=1` stores land inside any `fixsrc` image; **0** store `new` values equal any `fix.log` guest word; `stage-flip` 0. Local-memory coverage works (5064 stores in `c0000000..c2000000`). The `nv308a` copies are faithful: every sampled line has `dst_bytes == src_bytes` and `same=1`, so the transposed bytes are already in guest RAM when the copy reads them. **The producer is therefore not any hooked CPU store path.** | `run-20261123-223946-ds24c/{stage,fix,watch,stage-nv308a}.log`, `summarize-stage.py`. |
| F45 | **The residual has no confirmed root cause yet; one class cannot be excluded.** The unhooked path is the RSX-side copy destination itself (`nv308a`/`nv3089`/`nv0039` DMA write) and the guest buffer that feeds it. `nv3089`/`nv0039` never fired in `ds24b`/`ds24c`; the `nv308a` bulk copy is instrumented (`program_write_watch`) but its destination was not scored by the stage watch, so a copy into a program image could not be joined to a defect by address. The `nv308a` *source* is watched (`watch_range`), and no store into a watched source explains the defect. So the remaining candidates, in order: (a) the copy destination, now hooked in the next build; (b) the guest source buffer, written by a path the four hooked layers do not see; (c) a defect that is already present at first sight of the range (`stage-prearm` covers this). | F44; `nv308a.cpp`; §10 Phase 3. |
| F46 | **Four probe defects were found by scoring `ds24c`, and all four could have hidden the producer.** (1) `watch_range`'s merge loop treated an exact duplicate as a merge: it deleted the matching range and re-appended it, so the common "same image prefetched again" call re-emitted `stage-watch` and churned the array; with the range last in the array it is an **infinite loop** (latent, not observed because a new image is usually appended in between). (2) The 128-line `stage-watch` cap was spent on 128 copies of the same 80-byte range, so the 68 real images were invisible and the watch could not be audited. (3) `stage-store`'s `needle=` field carried `needle OR band`, so 4924 lines read as needle hits and the band signal was indistinguishable. (4) `summarize-stage.py` parsed `fixsrc`/`fix` positionally, so `fp=None`, `ea=None`, `defect quads=0` and the join was not evidence. Also `suspicious_word`'s weak `plausible` shape fired on ~13% of random data words (false positives at `0x3919…`, `0xc14a…`). All five are fixed in the next commit; the array churn preserved the range *set*, so F44's `stage=1` negative stands. The first rewrite of the merge loop then kept a stale loop bound and indexed `ranges()[0 - 1]`, a hard crash in `ds24d` at the profile screen (segfault in `watch_range` via `nv308a`); fixed in `e6932cc06` by assigning the new count and guarding `now == 0`. | Code read of `watch_range`/`note_mem`/`log_word`; `summarize-stage.py` output; `run-20261124-132203-ds24d/stdout.log`. |
| F47 | **All three symptoms are present in the present-stream dumps and each has a distinct pixel signature.** White floor: scene drawn, 18–19 % of pixels >= 250, the white area is the water/ground surface (3037, 3045, 3060, 3455). Whole-image white: mean 245.9, 92–94 % >= 250 (3021, 3102–3105, 3402–3404, 3539, 3664). World-absent: 85–99 % black with only the HUD (2712–2718, 2955–2959, 3678–3680). Sharpness split: left/right high-frequency energy ratio 0.25–0.55 while the HUD stays sharp (3415, 3455, 3090, 3399, 3429, 3477, 3585–3590). | `run-20261124-132656-ds24d/dumps/host-present-*.bin`; `build/probe/present-analysis.json`; frames listed in §0.2. |
| F48 | **`host-world-target-*` cannot show the finished world.** The dump fires on the first `ucode_length >= 3000` world draw of the capture (F41), so it contains the clear colour plus the first character draws. `host-world-target-82840` is a dark field with one character silhouette and no ground or architecture. The seam and the floor must be read from `host-present-*`, or from a world-target dump taken at the last world draw. | F41; `dumps/host-world-target-82840-*.bin`; §0.2. |
| F49 | **The fragment-constant consumer path is deterministic end to end.** `write_fragment_constants_to_buffer_sse2` loads 16 bytes from the live ucode image and applies one transform `T` (swap the two bytes inside each 16-bit half; fallback `((w & 0x00ff00ff) << 8) \| ((w & 0xff00ff00) >> 8)`). `constants_heap::alloc_fragment_constants` copies that into the per-frame-slot ring, `params->fs_constants_offset = fragment_constants_index()`, and the MSL reads `fc[x + _fs_constants_offset]`. The probe reads the same StorageModeShared buffer at the same index, so `world tail` is the shader's input by construction, and a defect it shows is already in the image bytes. | `ProgramStateCache.cpp:642-687`; `MTLConstants.h:536-566`; `MTLGSRender.cpp:6250-6280`, `:6757`, `:6775`; `MTLFragmentProgram.cpp:253,264`. |
| F50 | **The `fix.log` "100 % byte-reversed" invariant is circular.** The oracle changes a lane only when the delivered value is not plausible and its 32-bit reversal is, then logs it. The logged population is therefore selected for byte reversals; the invariant says nothing about what the guest wrote. The non-circular statements are: on a defect frame the image word differs from the fill's expected convention by a full 32-bit reversal, and no hooked store writes it. | `ProgramStateCache.cpp:856-898` (the `broken`/`sane`/`forced` gates); `fix.log`. |
| F51 | **The permuted word is already in the main-memory copy source, and the copy is byte-faithful.** All 16 `nv308a-dst` defect lines are `fmt=11 mode=memcpy same=1`; the 8 delivered defect quads are written from sources at `0x3910f244 … 0x39112d60`; `stage-prearm` at first sight already shows the defect word; 0 hooked CPU stores land in any 16-byte source or within ±0x200 of one. | `ds24d/{stage.log,stage-nv308a.log,watch.log}`; F44/F45. |
| F52 | **The blur is added before the HUD, so the present compose cannot add it.** `fp 95` composes `0xc07a0000` into the present surface after the 78 HUD/overlay draws (F39); the HUD is sharp in every blur frame. The blur frames differ only in the world region, left of `x = 640`. | F39; F47; frames 3415/3455. |
| F53 | **The detailed `Metal 4: draw` trace is capped at 16 lines per run.** `diagnostic_draw_trace_limit` is 16 (64 under `draws-all`), so `ds24d` describes 16 draws and none of them is a bloom composite. Target/scissor/texture-size questions about the pre-HUD chain cannot be answered from that log until the cap is raised or the pass is named explicitly. | `MTLGSRender.cpp:4582`; 16 `Metal 4: draw <n>, command …` lines in `ds24d/mtl-probe.log`. |
| F54 | **The producer ledger is live and complete on every instrumented path.** `run-20261124-145124-ds24d` wrote 9 704 208 lines (722 MB) in `ranges` mode from 12 writers: `ppu-stw` 855 741 253 calls, `ppu-stwbrx` 293 549 943, `vm-write` 12 029 209, `spu-list-put` 5 138 432, `spu-putllc` 2 975 623, `spu-put` 2 596 103, `nv308a` 55 997, `render-target` 11 226, `depth-target` 501. Line counts are per call, not per distinct store. | `run-20261124-145124-ds24d/ledger.log` census; `launch-ds24d.sh` env. |
| F55 | **The literal convention gate fired zero times on a run that contains every symptom.** `source=fill` = 0 for the whole run, while the same run's present stream classifies `floor-white=74 flash=12 absent=31 blur-left=2 stable=216`. The needle `a1273fbd` appears in no `fill.log`, `stage.log` or `fix.log` line — only as the configured needle echoed in `mtl-probe.log`. So none of these captured symptom frames is produced by an out-of-band fragment-constant lane. This does not overturn F18–F24; it means the residual has a second producer, or that session's program set never carried the defect. | `ds24d/present-analysis.json`; `ledger.log` census; needle grep over `fill.log`/`stage.log`/`fix.log`. |
| F56 | **The origin report is invalid as delivered.** The 72 quads it scored are stage-watch `band` hits — 16-byte groups of guest pointers such as `c13e05e0` in RSX local memory — not ucode lanes: `note_defect` was wired to the band, and the static scan covers only main RAM `0x10000–0x40000000`, so it cannot see a `0xc0…`/`0xc1…` quad. Every `constructed-in-place` verdict is unearned and the 64-defect budget was spent on noise. | `ds24d/origin.log`; `RSXProducerLedger.h` `note_defect`; `RSXProducerOrigin.h` scan range. |
| F57 | **The seam is screen-space and written in place.** Frame 3411 cropped to `x 560–740, y 300–540` at 4× shows a hard vertical edge at `x = 640` with the geometry continuous across it — the same wall and the same character, left blurred, right sharp. A texture size, pitch, tiling or UV error cannot produce that; a pass that writes (or fails to write) one half of the destination can. Frame 3410, same scene and camera, is sharp on both halves. | `ds24d/dumps/host-present-3411-*.bin`, `host-present-3410-*.bin`; crop rectangle above; raw BGRA8 1280×720. |
| F58 | **The floor is a lighting saturation with correct shadowing, and a foreign image is composited into the world.** Frame 3116: the ground is pure white with hard-edged shadow silhouettes while the rest of the world is near-black. Frame 3118: identical camera and HUD, ground dark. The lit term flips per frame and the shadow term still multiplies it, so this is not a colour inversion. Frame 3116 also carries a top-down "map"-like rectangle at `x ≈ 630–815, y ≈ 0–80` and a dithered noise block below it, neither belonging to the scene — the signature of a resource bound or aliased wrongly, not of shader arithmetic. | `ds24d/dumps/host-present-3116-*.bin`, `host-present-3118-*.bin`. |
| F59 | **The per-frame target alternation is duplicate presentation, not the defect.** Joining the present stream to `frame targets` (147 of 335 frames have a tally) shows alternating full frames (12 targets including the world target `0xc07a0000`) and present-only frames (1 target, `0xc0010000` or `0xc03d0000`). Both members of a pair present the same image, and every symptom frame that has a tally is the full member or its duplicate. So the 1-target frame is the odd half of an ordinary double-buffered duplicate, not a lost world. | `ds24d/present-analysis.json` joined to `mtl-probe.log` `frame targets`. |
| F60 | **The Metal viewport rectangle is not the same input as VK/GL's, even at scale 1.** VK and GL scale `draw_state.viewport.clip_width/height`, which `RSXCoreRenderer` sets from `surface_clip_width()/height()` (`RSXCoreRenderer.cpp:174-175`, `VKGSRender.cpp:1312-1326`, `GLGSRender.cpp:76-84`). Metal reads `framebuffer_layout.width/height` (`MTLSurfaceState.cpp:53-54`, `MTLEncoderState.cpp:49-50`), and the layout width is *pitch-corrected*: `RSXGraphEngine.cpp:398-414` lowers it to `color_pitch / texel_size` when the raster width exceeds the pitch, and `:448` does the same for a colour-pitch fixup. So Metal's rectangle can differ from VK's at any scale, and the earlier "equal at 100 %" wording was too strong. Separately, Metal did not scale the rectangle at all, so at scale > 1 it stayed at guest size while the attachment grew; that is fixed by scaling `raster_width/height` in `capture_encoder_state` and by scaling the framebuffer extent. Neither difference is proven to cause this run's x=640 seam. | `RSXGraphEngine.cpp:398-414,448`; `VKGSRender.cpp:1315`; `GLGSRender.cpp:80`; `MTLEncoderState.cpp` `capture_encoder_state`; `MTLFramebuffer.cpp` `bind`. |
| F61 | **Metal's `memory_barrier` had no guest-memory re-dirty test (fixed).** `surface::memory_barrier` only acted on `old_contents`/`erase_bkgnd`. VK and GL re-sample guest memory on the same call and force `erase_bkgnd` (reload/clear) when a `ready` surface no longer matches: `last_use_tag && state_flags == ready && !test()` (`VKRenderTargets.cpp:960-974`, `GLRenderTargets.cpp:496-510`). Metal's `initialize_memory` and `needs_cpu_upload()` were already correct (`MTLSurface.cpp`); the missing piece was the trigger, which now mirrors VK/GL for `force_data_load` and the read-buffers configuration. | Code read; VK/GL citations above; `MTLSurface.cpp` `surface::memory_barrier`. |
| F62 | **Metal reused invalidated surfaces without a frame tag; now fixed with a frame clock.** The resource handle defers release to the frame slot that last referenced it (`handle::~handle` → `defer_release`, `on_commit_complete(index)`), and `surface_store::invalidate` keeps the surface in `invalidated_resources` (`surface_store.h:685-690`). The gap was reuse: `find_reusable_matching_surface` scans `invalidated_resources` and matched on size/format/samples only (`surface_store.h:100-120`; `MTLSurfaceCache.cpp:50-55`), so a surface a still-in-flight slot samples could be handed to a new allocation. VK stamps `frame_tag = get_current_frame_id()` and refuses to destroy until `frame_tag < last_finished_frame` (`VKRenderTargets.h:557-560`, `VKRenderTargets.cpp:188-204`). Metal now has the same guard: `resource_manager` keeps a monotonic frame serial and a per-slot completion clock, `notify_surface_invalidated` stamps `frame_tag`/`frame_slot`, and `is_reusable_surface` refuses a surface until that slot has completed that frame (`MTLResourceManager.*`, `MTLSurface.h`, `MTLSurfaceCache.cpp`). The earlier "releases immediately" wording was wrong. | `MTLSurfaceCache.cpp` `notify_surface_invalidated`/`is_reusable_surface`; `MTLResourceManager.*` frame clock; `surface_store.h:100-120,685-690`; `VKRenderTargets.cpp:188-204`. |
| F63 | **Metal snapshots a cyclic read instead of ordering it.** `requires_feedback_loop_copy` is `true` for Metal and `false` for VK and GL (`MTLTextureCache.h:40`, `VKTextureCache.h:33`, `GLTextureCache.h:33`). VK/GL insert a texture barrier and sample the live attachment (`VKRenderTargets.cpp:864-891`, `GL/glutils/barriers.h`); Metal copies the surface instead (`MTLSurface.cpp:190-198`). The copy itself is full size, so it is not a half-width mechanism, but it changes both the image a same-encoder pass sees and when the contents must be valid. | Citations above. |
| F64 | **The left-half blur is a steady-state property of the fully recorded frames, not a two-frame class.** In `ds24d`, every frame with a full draw list is left-blurred: p8 (present 4691, 329 draws), p20 (4703, 336) and p32 (4715, 340) all measure a left/right detail ratio of 0.54–0.55 in every 120-px band (y 0…600), and the 3× crop of 4703 shows the hard `x = 640` edge through the character with continuous geometry across it. The classifier calls only the ≤0.55 ones `blur-left`, so its count understates the defect. Frames 4685–4690 are capture pre-roll fragments (p6 33 draws, p7 one draw; `m*` manifests are pre-arm), so their sharper ratios are not counter-evidence. | `ds24d/present-analysis.json`; `build/probe/pngseam.py` on `host-present-4691/4703/4715`; `build/probe/view/crop/c4703.png`; `frame-4684-p{8,20,32}-manifest.txt` headers. |
| F65 | **The post chain of a real frame is decoded in submission order, and only three pre-HUD writers touch `0xc07a0000`.** Present 4703 (`frame-4684-p20`, 336 draws): 260 scene draws → `0xc07a0000` (all `scissor 0,0,1280,720`); `fp164` → `0xc07a0000` (fog mask, reads `0xc5870000` + depth); `fp69` → `0xc4610000` 640×360; `fp108` → `0xc4710000` 640×360 (reads `0xc4610000` + depth); `fp118` → `0xc4610000`; `fp10` → `0xc4260000` 1280×720 (reads `0xc07a0000` + `0xc4610000`); `fp129`/`fp102`/`fp29`×2/`fp116`/`fp41`/`fp64`/`fp75`/`fp129`/`fp75`/`fp60`×2 → the 320×180/160×90/64×64/16×16 bloom chain, all full-target scissor; `fp107` → `0xc07a0000` (reads `0xc07a0000` + `0xc4610000`); `fp25` → `0xc4260000` (reads `0xc07a0000` + depth; writes scene RGB with alpha = clamp(15·|reprojection offset|) in fp25's math); `fp38` → `0xc07a0000` (reads `0xc4260000` + depth; ~20 velocity-weighted taps, output = Σtaps + (1−Σw)·centre); `fp40`×3 → `0xc07a0000` (HUD); `fp121` → `0xc03d0000` (present copy). So on this frame the only passes that can put a screen-space half-split into the pre-HUD image are `fp164`, `fp107` and `fp38` (plus the 260 world draws); the bloom chain and `fp10`/`fp25` write other targets. | `ds24d/dumps/frame-4684-p20-manifest.txt` records 238810–238924 and their `t0…` lines; `fp-{10,23,25,38,69,107,108,118,121,164}.{metal,const.txt}`. |

---

## 5. Falsified or excluded

| # | Claim | Why it is out |
|---|---|---|
| E1 | sRGB / gamma mismatch | F4; the same packing path as VK. |
| E2 | Residency eviction / heap churn | F5. |
| E3 | Missing pass-transition barrier | F6. |
| E4 | The FIFO parser | F1 + F2: reverted to upstream, and shared code cannot make Metal differ. Keep only if a run re-implicates it with evidence. |
| E5 | `fs_constants_offset == 0` sentinel | F3. |
| E6 | The FP/ROP precision port caused it | F8: behaviour-neutral at `high`; the strobe predates it. |
| E7 | `supports_denorm_preserve_f32` should be `true` | F9: the emulation is exact; flipping it would be the regression. |
| E8 | "The dump is fine, the screen is fine" → capture artefact | A capture artefact cannot strobe the live window; but the reverse (the capture path alters timing) is real — F12 flushes inside the present (`MTLGSRender.cpp:2527`). |
| E9 | **H6 — output-chain amplification.** | F21: the tape's present stream carries the flip, the feedback witness is `live 0` everywhere, and the sampled depth image is byte-identical in both classes. Nothing after the world draw changes between black and white. |
| E10 | **H3 — two compiled pipelines alternate.** | F19/F22: one fp 70 image, one pipeline, one quad layout; only the guest's animated word changes between fills. The class tracks that word, and a second variant is not needed to explain either class. |
| E11 | **H5 — denormal/FTZ as the *cause*.** | F19: the shader's input is already out of range (measured up to `1e33` in magnitude, or below `1e-6`) before any arithmetic, so neither class requires a flush-to-zero. FTZ may still act on a *corrected* value at `ultra`; it is not the defect. |
| E12 | **A half-width scissor or viewport on the world target.** | F37: all 222 draws into `0xc07a0000` in a real frame use `scissor 0,0,1280,720`; the only sub-full scissors are the shadow atlas quadrants and the bloom targets. |
| E13 | **The `fp 110` world feedback copy adds the blur.** | F38: `0xc5870000` is byte-identical to its `0xc07a0000` source at 6642/6732/6734 (`cmp` clean, same hash); it is a plain 1-texture full-screen copy. |
| E14 | **An RSX blit engine writes the world.** | The `73d1e6ad4` run (`run-20261119-095732-ds24b`) logs **zero** `nv3089-host`, `nv0039-host` and `stage-partial` lines; the world target is written only by draws (F36). |
| E15 | **A hooked CPU store path (PPU JIT/interpreter, SPU DMA, `vm::write`) produces the residual ground flash.** | F44: with the PPU object cache rebuilt (F43), `ds24c` sees 5285 stores across those layers and none lands in a defect quad, in a `fixsrc` image, or carries a `fix.log` guest word. The layer is live, not silent, so this is a measured negative, not a missing instrument. |
| E16 | **"The guest writes the animated lane byte-reversed", as a measured fact.** | F50: the oracle logs a lane only when its reversal is plausible, so the population is selected for byte reversals. What is measured is that the image bytes on a defect frame are a full 32-bit reversal of the fill's expected convention, and that no hooked store writes them. Whether the guest is correct and RPCS3 mis-decodes the lane is still open (H9). |
| E17 | **"The Metal constant ring or `fs_constants_offset` is the cause."** | Not excluded, but F49 shows the probe reads the same index and the same bytes the shader reads; a ring/offset bug would have to move both identically. `literal-provenance` (ring-replaced / offset-mismatch) and `freeze-fs-offset` are the discriminators. |
| E18 | **The fragment-constant producer is the whole story for the white floor, the left-half blur and the world-absent frames.** | F55: in a run whose present stream contains 74 white-floor, 12 flash, 31 absent and 2 blur-left frames, `source=fill` fired **0** times and the needle appears only in the probe echo. The constants were the mechanism of the ds22/ds23 full-screen strobe (F18–F24); they are not the producer of the 2026-11-25 symptom set. |
| E19 | **`MTL_PROBE_SKIP_FP=38` tested `fp38`, so "skipping fp38 does not remove the seam" is a measured negative.** | Withdrawn. In `ds24e` fragment program 38 was never compiled (the run's ids run `…36, 37, 39…` and the post chain ends `fp 37`), so the skip check never matched and the log contains zero `skip fragment program` lines. That run is an unmodified baseline. Fragment-program ids are run-local; a skip target must be read from the same run's manifests. |

Two defects are known and **not** the strobe, but must not be confused with it:

- `pass_scheduler::split_requested()` is dead: a same-encoder RAW is carried and
  emitted at the next pass. Real, currently masked, unfixed.
- The LLVM `MCJIT` teardown segfault in `Emulator::Quit` is unrelated to rendering.

---

## 6. Open hypotheses

The full-screen strobe's mechanism is settled (F18–F24); the residual three
symptoms are not. Since 2026-11-25 the leading family is backend resource
identity and contents (F61–F63), not shader data. Ordered by what the next run
can kill.

| # | Hypothesis | Discriminator |
|---|---|---|
| H1 | **The animated lane is written by a different path than the static lanes.** The quad arrives as one nv308a chunk, but its `src=` range was produced by a writer that stored the correct ucode word `T(W)` big-endian (F26/F27), while the assembler writes the same format little-endian. | Watch the `src=` range of the tail chunk (`watch.log` has `src_bytes`), not just the destination. The producer is one indirection further back than the fill. |
| H3 | **The fill is one patch stale** (the value it reads predates the guest's latest store). | The fill log's `fifo` cursor vs the patch's FIFO position. Low: the delivered value is not a *delayed* correct value, it is a different byte order, which staleness cannot produce. |
| H2 | ~~**The oracle changes the image but the strobe survives → the epilogue re-reads the lane elsewhere.**~~ **Answered by `ds23` (F24):** with every `[W,2,·,1]` family member corrected, the full-screen strobe is gone. The epilogue's trailing pair was the whole full-screen mechanism. | Closed. |
| H7 | **The residual ground/upper-half flicker is a stray lane outside the trailing pair.** The oracle corrects only the two trailing quads and `literal watch` only reads those, so a `[W,2,W,1]`-shaped stray in, say, the lighting or ambient quad of the ground shader would survive and would saturate/zero the lit term without touching the epilogue. F29 bounds it: no whole-quad transposition exists in 4096 fills, but `neither` quads do (max 14), so a two-lane stray in a non-trailing quad is still possible. | Whole-image per-lane census (every constant quad, not the tail), an extended `literal watch` that names the quad/lane that changed, and `bswap32-any` (see §10) as the functional test. |
| H8 | **The blur is introduced after the world draw but before the HUD.** The HUD is always sharp, so the present compose is excluded (F39); the world target is blurred while the HUD drawn into the same target is not. Candidates: a half-resolution intermediate sampled by a pre-HUD post pass, a per-half UV/derivative defect, or a stale/ghosted copy. The copy is excluded (F38); the bloom chain is the only blurred source (F38); the remaining passes are the 25 effects + `fp 57` + `fp 88`. | Dump the world colour target both pre-effects (`fp 141`) and at the present compose (`fp 95`) **in the same frame**; if the pre-effects image is already blurred the world draws add it, if only the present source is blurred a pre-HUD pass adds it. Then bracket the ~28-draw pre-HUD chain. |
| H9 | **The producer is the per-frame ucode patcher writing through an emulated store that loses a 32-bit byte reversal.** F26 says the word is `T(W)` with the endianness of a plain `stw`; the correct slot needs the `stwbrx` order. Candidates, in order: (a) the guest patcher's own store path is fine and the bytes are written through an RPCS3 helper that swaps at 16-bit granularity; (b) the game's inline FIFO packet is produced by an SPU MFC transfer whose element size is 2 instead of 4; (c) the packet builder applies `T` to a BE value and never reverses. **Update 2026-11-19: (b) is refuted (accurate-SPU-DMA run identical), and no store hook that actually ran has seen the word in 4 runs / 288 distinct words — but F43 shows the PPU hook never ran, so that negative covers only the SPU, `vm::write` and RSX-blit layers.** | **Implemented (F34):** the `stage-store` line names the writer (`writer=`, `pc=`, `thread=`, or SPU `tag`/`lsa`) for the exact 16-byte source quad an `nv308a` later copies. **Separate test (F35):** `RSX_FP_STAGE_FIX=stage` with the oracle off; the ground stops alternating iff the producer store is the whole story. |
| H10 | **The whole-image grey flicker is the same reversed constant in a fullscreen pass.** A ground lane cannot dim the HUD or the whole frame; a 2D post/tonemap/exposure program whose constant arrives out of band can. | List, from `fill.log`/`fix.log` of a `ds24b` run, every constant of every 2D program whose delivered value is out of the authored band at draw time; a program whose value tracks the grey phase is the candidate. Functional check: correct only that program's lane and see whether the grey goes. |
| H11 | **The sharpness split is a tiled/regional pass that covers exactly one half.** The boundary is exactly `x = 640` (half of 1280) and only the right half is sharp, while the HUD is sharp; the seam is in the present-source image. A half-width scissor/viewport is excluded (E12), and so is the copy (E13) and any blit (E14). Remaining forms: a pre-HUD pass that samples a half-width region or a low-resolution source on one side, or the world draws themselves. | F42's one-frame pre-effects/present comparison first; then the manifest draw list names every pre-HUD candidate and its sampled addresses. |
| H12 | **The world draws themselves generate the left half at a lower effective resolution.** If the pre-effects sample (`fp 141`) is already blurred, then the 115 world draws / their texture sampling produce it — e.g. a mip/LOD or texture-cache selection that changes at `x = 640`, or an attribute interpolation split (F7(b) flat-varying rule) that only affects one half. | Same one-frame comparison. If confirmed, diff the per-half sampled mip/LOD and the interpolated UVs of the world programs; dump one world texture pair at 1:1 and at the mip the left half selects. |
| H13 | **The left-half blur and the white floor share one cause: a half-width bloom/post composite.** The bloom chain is the only blurred source in the frame (F38) and its intermediates are half- and quarter-resolution (`0xc4610000` 640x360, `0xc4810000` 320x180, …). If the composite that adds bloom into `0xc07a0000` is issued with a half-width viewport, or samples the 640-wide source with a full-res UV scale (`x / 640` instead of `x / 1280`), then the left half receives the blurred, over-bright bloom and the right half samples the clamped edge and stays sharp. That reproduces the seam at exactly `x = 640`, the 0.25–0.55 left/right detail ratio, and a water surface blown to white while the rest of the scene is correct. | Parse the composite draw: target, scissor, **viewport**, sampled texture sizes and the UV varyings (`Metal 4: draw` / `capture geometry`). The current cap hides them (F53). Functional arm: bind the bloom source as 1x1 (or skip the composite) and see whether both the blur and the white floor disappear; `freeze-present=120` makes the frame stable to screenshot. |
| H14 | **The world-absent frames are a presentation/target problem, not a dimming.** Those frames are 85–99 % black with only the HUD, and the `frame targets` tally alternates between a frame with ~12 targets including `0xc07a0000` and a frame whose only target is the present surface. Either the 3D draws were not submitted for that buffer, or the present sampled a buffer that was not written this frame. | For one captured black frame, join the present index to `frame targets` and check (a) whether `0xc07a0000` was written that frame, and (b) whether the present source equals the clear colour. Then read the buffer-flip path (HLD-06 / `LLD-sync-flip`) against the tally. |
| H15 | **The white floor is a non-trailing constant lane in the water/ground program, not the epilogue pair.** The `bswap32-all` oracle fixes `fc[count-2].x` and `fc[count-1].x` for the `[W,2,W,1]` family and the strobe is gone, but the floor still goes white, so a *different* lane of the same program (specular power, light intensity, ambient) may arrive out of band. F29 bounds this: no whole-quad transposition exists, but `neither` quads do (max 14 in one program). | `bswap32-any` (functional) plus the per-lane census over every constant quad of the water/ground program, in the same `literal-provenance` run. |
| H16 | **The present samples `overlap_info.back()` without checking `is_depth`, so a depth surface overlapping the display range can be presented as colour.** `surface_store` appends colour sections before depth sections (`surface_store.h:1433-1443`) and the Metal present takes the last one (`MTLGSRender.cpp:7973`) without reading `section.is_depth` (populated at `surface_store.h:1408-1413`, used only for diagnostics at `:4921`). Far-plane depth read as colour is white; near geometry is dark — the 3116/3118 pair. The tape already proves the backend samples `0xc0b50000` as a texture. | Log `is_depth`, `base_address`, `src_area` and `is_clipped` for `.back()` at the flip address. Refute if the display range has no depth overlap and the chosen section is a colour surface. |
| H17 | **Stale surface contents: a `ready` surface is reused after its guest memory changed (F61).** The predicted shape is exactly what 3116/3118 show — same camera and HUD, ground white one frame and dark the next, a foreign image and a noise block — because the attachment keeps its old GPU image instead of reloading or clearing. The trigger is now implemented: `surface::memory_barrier` re-tests a `ready` surface when `force_data_load` or the read-buffers configuration asks for it and sets `erase_bkgnd` on a mismatch. Still unproven for these frames until a run with the trigger logs a hit. | Log every bind whose attachment has `last_use_tag` set and `!test()`, and whether `erase_bkgnd` was set. Functional arm: force `erase_bkgnd` on every bind and see whether the alternation goes. |
| H18 | **A surface was recycled while an in-flight slot still samples it (F62). Fixed, not yet proven to cause these frames.** Three slots share one cache, and `find_reusable_matching_surface` could hand an `invalidated_resources` surface to a new allocation without checking whether the slot that invalidated it had completed. Surfaces are now stamped with the invalidating frame/slot and the store refuses reuse until that slot's command buffer reports completion. The foreign "map" patch and the dithered noise block are the predicted signature of sampling memory that now holds something else. | `MTLSurfaceCache.cpp` `notify_surface_invalidated`/`is_reusable_surface`; `MTLResourceManager.*` `is_frame_complete`; `MTLContract.InvalidatedSurfaceWaitsForItsSlotToComplete`. Functional arm still `MTL_PROBE_VARIANT=serialise`. |
| H19 | **Missing cross-frame GPU edges across the 3-slot ring.** `open_frame` waits only for the same slot's previous commit; the pass scheduler is per frame; heaps are untracked. The present is ordered only by `memory_barrier(transfer_read)`, which emits no edge for a clean surface. | `MTL_PROBE_VARIANT=serialise` (defect goes → missing edge) then `heaps` (tracked hazards). Defect stays under both → content, not ordering. |
| H20 | **Withdrawn: the half-written-depth premise is not supported by the dumps.** The `sample-47-draw54210-addrc0b50000-fmt85-1280x720.bin` dump is zero in **both** halves (one unique value per half), which is an empty/cleared dump, not a half-written depth image; the `host-tape-*c0b50000*` samples have 35 k–120 k distinct values in *each* half. No half-uniform depth sample exists in this run, so the "right half never written" claim is withdrawn. The seam still needs a pass-level explanation (F57/H11); the depth-half mechanism is not established. | `ds24d/dumps/sample-47-*`; `host-tape-f3332/3334/3336-*` half histograms. |

Hypotheses H3 (two pipelines), H5 (FTZ) and H6 (output-chain amplification)
from the previous revision are in §5. The slot/offset race is not required by
any measurement but was not tested directly; `freeze-fs-offset=0` is the arm
that would test it.

---

## 7. Evidence inventory

| Path | What is in it |
|---|---|
| `build/probe/run-20261125-231354-ds24e-skip38/` | **Latest run** (`build/probe/.last-run`): 232 presents 3261–3816, classes `stable 125 / absent 78 / floor-white 22 / flash 7 / blur-left 0`. Meant to skip `fp38`, but id 38 was not compiled, so it is a baseline (§11 2026-11-26). Kept: `host-present-*`, `frame-*` manifests, `fp-*` text/ucode, `mtl-probe.log`, the analysis JSON. |
| `build/probe/run-20261125-195520-ds24d/` | The F64/F65 reference: 96 presents 4685–4780, the decoded post chain, and the persistent-blur crops. Kept: presents, manifests, the cited `sample-47-*`, `fp-*`, logs. |
| `build/probe/run-20261124-132656-ds24d/` | The §0.2 symptom corpus: 623 presents including the frames cited there, plus manifests, logs and the `host-sample-*-addrc07a0000` set behind F40. |
| `build/probe/launch-ds24d.sh`, `launch-ds24e.sh` | The launchers and every probe switch in one place. `launch-ds24e.sh <ids>` sets `MTL_PROBE_SKIP_FP`; `none` is a baseline. |
| `build/probe/summarize-stage.py`, `build/probe/classify-present.py` | The two scorers: store/defect join, and present-frame classification. The `build/probe/mine-*.txt` files are mined outputs. |
| `docs/research/metal4/design/CAPTURE-DESIGN.md` | What a capture should contain, by design. |
| `docs/research/metal4/design/EXPERIMENT-metal4-semantics.mm` | The Metal 4 semantics harness behind F7. |

**Pruned 2026-11-26 for space (24 GB → 4.1 GB).** The raw directories for
`run-20261123-223946-ds24c` (the live-PPU-hook positive control behind E15),
`run-20261124-145124-ds24d` (the 335-frame classification run),
`run-20261124-132203/144357/144950-ds24d` (partial captures) and
`run-20261125-230336-ds24e-c4260000` were deleted. In the three kept runs every
`.bin` except `host-present-*`, `fp-*`, `sample-47-*` and
`host-sample-*-addrc07a0000-*` was deleted, keeping all `*.txt` manifests and
`*.log`. Conclusions drawn from the deleted dumps live in §4/§5/§11; those
entries are now the evidence. Anything that needs a deleted dump must be
re-captured, not re-read.

The dated artifacts removed on 2026-11-24 are superseded by this file. They were
`DS-SUMMARY-VERIFICATION-2026-11-17`, `F12-CAPTURE-HISTORY-2026-11-18`,
`FIX-PLAN-CONFIRMED-GAPS-2026-11-17`, `HANDOFF-2026-11-16-ds-world-literal`,
`FINDINGS-TRIAGE-2026-11-16`, `PLAN-2026-11-14-fp-literal-rca`,
`PLAN-2026-11-16-ds-world-literal-rca`, `UPSTREAM-RESEARCH-2026-11-16`,
`METAL4-RCA-STATUS-2026-11-08`, `METAL4-ACTION-PLAN-2026-11-08`,
`METAL4-FIFO-PARK-HANDOFF-2026-11-10`, `NEXT-AGENT-PROMPT`,
`BEHAVIOR-PARITY-AUDIT-2026-11-17`, `METAL4-SEMANTICS-MEASURED-2026-11-17` and
`SUBAGENT-SHADER-2026-10-21`. Their live conclusions are F4-F7, §8 and §11.

`build/probe/` is per-run and not in the release scrub's scope. A run that matters
gets its own directory; never overwrite `ds20`.

---

## 8. Repro and capture recipe

The `ds21` recipe (headless, one run, six presses):

```
MTL_PROBE=5 \
MTL_PROBE_TAPE=16 \
MTL_PROBE_FRAME_COUNT=120 \
MTL_PROBE_LOG="$PWD/build/probe/run-<stamp>-ds/probe.log" \
MTL_PROBE_DUMP_DIR="$PWD/build/probe/run-<stamp>-ds/dumps" \
build-tests/bin/rpcs3.app/Contents/MacOS/rpcs3 --no-gui
```

`MTL_PROBE_FRAME_COUNT=120` keeps the level-5 window open for 120 frames after
each press, which is what produces the per-draw `world draw` lines and the
`world fc` heads; the tape itself is independent of the window. The launcher
`build/probe/launch-ds21.sh` (untracked) wraps this and appends `RSX_FP_LITERAL_LOG`.

Then: reach the world, wait until the strobe is visible, and press F12 **four
times about a second apart**. One press answers "what is in the frame"; four
presses answer "what is the sub-frame pattern". Do not arm anything automatically.

Useful variants, one per run:

- `MTL_PROBE_PIXELS="x,y,w,h"` — per-draw patch readback inside the window.
- `MTL_PROBE_VARIANT=literal-provenance` — classify each fp id's block as
  fill-fresh or draw-replayed (F16).
- `MTL_PROBE_VARIANT=freeze-fc` — replay the first fill of each fp (F16).
- `MTL_PROBE_VARIANT=freeze-fs-offset=0` — every draw reads block 0 (F16).
- `MTL_PROBE_VARIANT=freeze-present` — hold the present source for 120 presents
  and suppress the automatic present/upscaled dumps (F16).
- `MTL_PROBE_VARIANT=serialise` — one command per encoder, wait-idle per draw
  (removes inter-frame overlap).
- `RSX_FP_LITERAL_FIX=bswap32-all` + `RSX_FP_LITERAL_FIX_LOG=<path>` — the
  fragment-literal oracle (F22). It restricts to lanes 0/2 of an image whose
  trailing quads are `[W,2,W,1]`, and reverses a lane when it is either huge
  (`>1e6`) or tiny (`<1e-6`); the authored lanes and any already-plausible value
  are untouched. `bswap32` is the older all-lane form, which misses the tiny
  class. `bswap32-any` (used by `ds24`) accepts any quad whose lane is
  implausible under `T` and plausible under the half swap. The `ds22` launcher
  is the oracle run.
- `RSX_FP_STAGE_WATCH=<path>` — the Phase 3 producer watch (F34). Writes
  `stage-arm`, `stage-submit`, `stage-store`, `stage-partial` and
  `stage-limit`. Inert unless set; every store the guest, SPU or HLE makes in the
  io ring or an `nv308a` source range is filtered to the F31 shape (or a
  matching needle, default `a1273fbd`) and logged once per
  (site, address, word).
- `RSX_FP_STAGE_LOG=<path>` — the `nv308a` source re-read: logs any 4-byte lane
  that differs between two commands that consumed the same source address, with
  the FIFO `put`/`get` and the destination. A lane that changes was written by a
  layer other than the copy.
- `RSX_FP_STAGE_FIX=stage|io|all` — the producer-side corrector (F35). `stage`
  only inside an `nv308a` source range, `io`/`all` also in the rest of the ring.
  Requires `RSX_FP_STAGE_WATCH`; use with the fill oracle **off** to test the
  producer hypothesis.
- `RSX_FP_STAGE_NEEDLE=<hex>` — override the exact word the watcher always
  keeps (default `a1273fbd`).
- `RSX_FP_STAGE_NO_JIT`, `RSX_FP_STAGE_NO_SPU`, `RSX_FP_STAGE_NO_VMW` — kill
  switches for the three hook families (guest JIT stores, SPU DMA puts, `vm::write`).
  Set one to `1` to remove that family, so a crash or a hot-path cost can be
  attributed without a rebuild.

The instrumentation the next run reads (all `EXCISE`-fenced, bounded,
default-off):

- `fill.log`: `fill … img <ptr> fifo <pos> put <put> fix <n>`; `conv …` the
  per-image convention census (`lanesT`, `lanesH`, `c2`, `neither`, named
  `c2q<quad>`); `xfp …` the delivered vs intended value of every transposed
  lane of the trailing pair.
- `watch.log`: `claim` (image pointer, offset/length, tail hash), `ring` with a
  monotonic `seq`, and `fill-tail` / `fill-world` scans. Every nv308a record now
  carries `fifo=`, `put=`, `span=`, `le=`/`be=` source words, and a
  guest-memory comparison (`guest=<addr> same=<0|1> g=…`): `same=1` proves the
  transposed bytes are in guest RAM, not made by the FIFO cache copy.
- `mtl-probe.log`: `tape class frame … black|white|dark|mixed mean …` per
  presented frame, plus the existing `world draw` / `world tail` / `world fc` /
  `world state` lines.

---

## 9. F12, in five lines

Press → atomic bit → next `present_frame()` consumes it → arms a window
(`F..F+1` by default), freezes the tape, dumps the pre-press ring history
(`m12..m1`), writes `host-shot`/`host-present`/`host-upscaled`, flushes the tape
(per-frame newly-black/newly-dark timeline), then `p1` and `p2` follow. Full
order, budgets and filenames are in `MTLGSRender.cpp:350-440` and
`MTLConstants.cpp:696-711` (measured 2026-11-18).

---

## 10. Plan

Phases are strictly ordered: each one either produces the next phase's input or
kills a hypothesis. No phase edits shipping behaviour.

### Phase 0 — measure the strobe — **done** (`ds21`)

Six F12 presses, `MTL_PROBE=5 MTL_PROBE_TAPE=16 MTL_PROBE_FRAME_COUNT=120`.
Result: 62 black / 8 white / 10 transitional presents, black runs 1–12, white
runs 1–2, whole-image classes, scene-sized transition bbox (no band). → F20.

### Phase 1 — join pixels to state — **done** (`ds21`)

fp 70 and fp 63 are both present in every captured frame (block index 0 and 69
respectively), so the class cannot follow the fp id or the block index alone.
The only thing that changes between fills is the guest's animated word, and the
class tracks that word. Slot/offset correlation was not directly tested; it is
also not needed, because the delivered lane is wrong in every fill. → F19/F22, E10.

### Phase 2 — four substitution arms (implemented, not yet run)

Each is one `MTL_PROBE_VARIANT` string, `MTL_PROBE=5`, own run, `EXCISE`-wrapped
and removable without behaviour change. Run them only if the oracle below fails;
the mechanism is already measured, so their value is now diagnostic breadth, not
the answer.

- `MTL_PROBE_VARIANT=literal-provenance` — per fp id, is the draw reading the
  fill's own block or a replayed one?
- `MTL_PROBE_VARIANT=freeze-fc` — replay the first fill of each fp. If the
  strobe becomes a single steady class, the fill/source side is the driver.
- `MTL_PROBE_VARIANT=freeze-fs-offset=0` — every draw reads block 0. Tests the
  slot/offset-race hypothesis directly.
- `MTL_PROBE_VARIANT=freeze-present` — hold the present source for 120 presents
  and suppress the automatic present dumps. If the strobe stops here, the world
  is not alternating and the present chain is adding it (E9 says it is not).

### Phase 2.5 — the family-wide oracle (`ds22`, `ds23`)

The existing `RSX_FP_LITERAL_FIX=bswap32` arm is the right transform but the
wrong predicate: `broken = !isfinite(v) || |v| > 1e6` skips the tiny values that
produce most black frames (F22). Change it to:

- restrict to lanes 0 and 2 (the animated lanes; lanes 1/3 are authored `2.0`
  and `1.0` and must not be touched), and only inside an image whose trailing
  quads decode to `[W,2,W,1]` (the world epilogue shape);
- gate on plausibility, not a single threshold: reverse when the delivered
  value is `>1e6` **or** a non-zero `<1e-6`, and when the reversal itself is a
  plausible float. In-band values (the five accidental fills) are preserved;
- keep the `RSX_FP_LITERAL_FIX_LOG` line so the number of corrected lanes per
  fill is recorded.

Implemented as `RSX_FP_LITERAL_FIX=bswap32-all`; the `ds22` and `ds23` launchers
select it.

`ds22` ran it with a `ucode_length >= 3000` gate: the two large world programs
were corrected, the character silhouette appeared, but the strobe survived, and
the `literal watch` census showed ~30 more programs carrying the same word
(F23). `ds23` removes that gate: the oracle now applies to **every** image, with the
`[W,2,W,1]` shape guard as the only discriminator. Offline replay of `ds22`'s
first sighting of each of the 49 non-uniform programs predicted 36 of them
would be corrected (fp 155 only in its first trailing quad), and none of the
unrelated 2D programs touched. **Result (F24):** 41 distinct fp offsets and
221 834 lanes corrected; the full-screen strobe is gone (51 of 96 sampled
presents `mixed`, world visible). **Phase 2.5 is closed as a success.**

### Phase 2.6 — the residual, the producer, and the blur — **in progress**

`ds23` leaves two effects that the trailing-pair oracle cannot see:

1. **Ground white↔black alternation** with the world otherwise drawn (F25).
   The epilogue scale at draw time is in-band, so the saturated lit term is
   most likely a stray word in a constant quad the oracle never visits (H7).
   F29 bounds it: no quad is uniformly transposed in 4096 fills, so the stray
   would be lane-local in a `neither` quad.
2. **Blurred world, sharp HUD** (F25). The blur is a spatial question, not a
   constant value (H8). The 2026-11-19 manifest already places it inside the
   present-source world target, i.e. before the HUD and the present compose
   (F36–F42), so the only split left is the world draws vs the ~28 pre-HUD post
   draws.

And one open producer question, now with a precise byte-level target (H9):
the patched lane is the correct ucode word `T(W)` stored big-endian instead
of little-endian (F26/F27), so the write-watch must name the writer of the
*staging* 16 bytes, not the copier.

Instrumentation implemented in the tree (all `EXCISE`-fenced, bounded,
default-off; built 2026-11-18 late night, `rpcs3_test` 687/687):

- **`stray` census** (`RSX_FP_STRAY_LOG`) in `write_fragment_constants_to_buffer`:
  for *every* constant quad, classify each lane under `T` and `bswap32(T)`;
  when a quad has at least one `T`-plausible lane and at least one
  `T`-implausible lane whose `bswap32(T)` is plausible, log
  `stray fp <off> quad <i> lane <l> raw <bits> alt <bits> t <f> b <f> qp <n> of <q>`
  (capped at 4096 lines / 8192 fills, deduplicated by fp+quad+lane+alt). This is
  the direct evidence for H7.
- **`literal scan`**: `literal watch` now reads *all* `fragment_constant_count`
  quads, not only the trailing pair, and reports the quad/lane whose ring value
  changed since the previous sample of the same pipeline, with the guest image
  word, the ring word and its `bswap32` decode. One line per changed lane (up to
  8 per sample, 4096 total) plus one baseline line per program.
- **Oracle arm `bswap32-any`**: reverse a lane when it is `T`-implausible but
  `bswap32(T)`-plausible **and** its quad has at least one `T`-plausible lane
  (proof the quad otherwise arrived in `T`). Unlike `bswap32-all` it has no
  `[W,2,·,1]` shape requirement, so it reaches the ground shader's non-tail
  quads. If the ground flicker stops, H7 is confirmed and the producer question
  is the same as Phase 3; if not, the residual is not a transposed constant.
- **Staging-range watch** (`RSX_FP_STAGE_LOG`, level 5): on the `nv308a` copy
  path, keep the last 16 bytes of up to 32 distinct guest source addresses,
  re-read them at every later command, and log the first time each 4-byte lane
  changes (`stage addr <a> lane <l> was <b> now <b> seq <n> put <p> get <g>`).
  A lane that changes between commands was written by a layer other than this
  copy: the measurement that can name the writer layer for H9 without a CPU
  breakpoint. 32 addresses / 4096 lines, inert unless the variable is set.
- **Blur evidence** (`tape detail`, level 5, uses the existing tape): every
  taped slot, sample and present, is scanned for the mean absolute luma gradient
  of its left/right and top/bottom halves. **Correction (2026-11-19):** the tape
  does not carry the world target and the present of the same frame at the same
  stage — per frame it keeps only the first-bound world-target image (F41) —
  so this scan locates the blur but cannot split the pre-HUD chain. The
  one-frame two-sample dump below is the discriminator.

Run: one F12 press with the ground in view (the white↔black alternation is
visible to the user before any press), then two more during the grey flicker.
Score: (`stray`/`bswap32-any`) does the ground stop alternating; (blur dumps)
which pass first contains the blur.

**Draw-list result (2026-11-19).** The 293-draw manifest of one real frame
(F36) replaces the guesswork with a list, and four of the old candidates are
now out: no half-width scissor (F37/E12), an exact feedback copy (F38/E13), no
RSX blit (E14), and the present compose is applied *after* the sharp HUD
(F39), so it cannot blur only the world. The seam is in the present-source
image; the only remaining split is "the 115 world draws did it" (H12) vs
"one of the ~28 pre-HUD post draws did it" (H8/H11). The bloom chain is the only
blurred source in the frame, and the effects that sample `0xc5870000` + depth
are the depth-tested overlays.

**Next measurement (one frame, two samples).** *Corrected 2026-11-22:* the
`present-*` dump already exists (`MTLGSRender.cpp:8376`, taken at the present
compose from `get_present_source`, i.e. pre-FSR), and `upscaled-*` (`:8568`)
covers post-FSR, so no new present-source dump is needed. What F41 observed is
the per-frame sampled-image table (`diagnostic_sampled_image[8]`, `:2392`),
which dedupes the *world target* by guest address across the draw path; the
present dump is a separate call and is not deduped there. The open question is
therefore narrower: for one captured frame, does the seam exist in the
`world-target` dump (`:5825`) and in `present-*` at the same time?

- seam in both → the world draws produce it (H12). Next: per-half sampled
  mip/LOD and UVs of the world programs, and a 1:1 versus mip dump of the
  world texture the left half selects.
- seam only in the present source → a pre-HUD pass produces it (H8). Next:
  dump `0xc07a0000` after each of the 25 effects, `fp 57`, `fp 88` and
  `fp 14` (a bounded ring of ~30 small dumps per captured frame; each dump
  costs a commit+wait, so cap the captured frames, not the draws).

If the present dump is itself suppressed for a frame because the sampled-image
table already recorded the address, the fix is a one-line bypass for the
`present` label, not a new dump. Keep everything default-off, `EXCISE`-fenced
and bounded.

**`ds24c` closes the CPU-store half and opens the copy half (2026-11-23).**
With the PPU hook finally compiled in (F43), every hooked writer is live and
none of them lands in a defect quad, in a `fixsrc` image, or carries a
`fix.log` guest word (F44/E15). That is the measured end of the "a CPU store
wrote the animated lane" branch. The remaining branch is the RSX-side copy
itself and its guest source (F45): the `nv308a` bulk and non-increment
destinations now call `fp_stage_watch::note_mem(..., "nv308a-dst", …)` and
carry `src_ea=`/`dst_ea=` in the detail, so the next run scores a copy into a
program image with the same address join as a store. If the destination is
clean and its source is clean, the defect was already present at first sight
and `stage-prearm` names it. The seam split is unchanged and still needs the
one-frame `world-target` + `present-*` comparison above.

### Phase 3 — producer of the transposed staging word — **one-run plan**

The fill and the nv308a chunk are faithful (`src_bytes == dst_bytes`), so the
reversed word is already in the guest's staging source at chunk time. The same
quad has three correct lanes and one reversed lane, so the producer is not the
chunk — it is whatever wrote the animated lane before it. F30 rules out the
JIT's `stwbrx`: the observed bytes are what a plain guest `stw` of `T(W)`
writes, so the question is *which store in which guest/SPU/HLE code* produced
them, not whether a byte-reverse instruction is mis-emulated.

The one-run plan was a **store watch on every layer that can write the command
ring**, filtered by the exact shape in F31 so the log stays small. It is now
implemented (F34); this section records what each hook does and how to score it.

1. **Shared watcher** (`rsx::fp_stage_watch`, `EXCISE`-fenced, env-gated,
   bounded, `RSXProgramWriteWatch.h`): holds the watched set (the `cellGcmInit`
   io ring plus every `nv308a` source range, merged within 64 bytes), applies
   `!plausible(T(word)) && plausible(bswap32(T(word)))` to 4-byte writes, and
   logs
   `stage-store writer=… addr=… new=… old=… slot=… alt=… suspect=… needle=… stage=… ring=… fix=… pc=… thread=… detail=…`.
   The `old` field is the previous word the same site wrote at that address, so
   an alternating lane is readable in one line. Dedupe is per (site, address,
   word); caps are 65536 lines and 4096 per site, and the cap is checked before
   the mutex so a saturated run stays cheap. 1/2-byte stores inside a staging
   range print `stage-partial` regardless of shape.
2. **Guest PPU (JIT)** — `PPUTranslator::WriteMemory` emits
   `__ppu_stage_watch(addr, size, is_be, pc)` after the store; the entry in
   `s_link_table` forwards to `vm::fp_stage_watch_write` (F33). `STVLX`/`STVRX`
   bypass `WriteMemory`, so they get the same call on the aligned 16-byte block.
   `is_be` is a compile-time constant at each site, so the line separates a
   plain `stw` (which reverses the bytes) from `stwbrx`; `pc` is the guest
   instruction. The interpreter's `PPU_WRITE` and all HLE stores go through
   `vm::write`, which calls the same hook.
3. **SPU** — `spu_thread::do_dma_transfer` arms a scope guard that reports the
   written region on every exit path (plain, atomic, MMIO), with `tag`/`lsa` of
   the transfer. List transfers PUT each element through this function, so a
   16-bit element (`H9(b)`) shows up as the element's own PUT; no separate
   per-element hook is needed.
4. **HLE builders** — `rsx::make_command`, `rsx::make_jump` and
   `cellGcmCallback`'s raw jump store report the packet they wrote (these use
   raw pointers, not `vm::write`).
5. **Arming and ordering** — `cellGcmInitInternal` arms the ring with
   `ioAddress`/`ioSize`/`cmdSize`, so a patch written during load is caught
   before the first `nv308a`; each `nv308a` copy adds its source range via
   `watch_range`; `set_rsx_dmactl` and `cellGcmCallback` log each submit with the
   PPU PC, so a store is ordered against the frame that carried it.
6. **Startup coverage** — the ring is armed at `cellGcmInit`, not first
   `nv308a`. After one frame every address the game uses as an `nv308a` source
   has been added.

**Run.** `build/probe/launch-ds24.sh` (oracle on, watch on) writes `stage.log`
(the store lines), `stage-nv308a.log` (a per-command re-read of the source
ranges, which shows a lane that changed between two commands), `fill.log`,
`watch.log`, `stray.log`, and the level-5 probe log. F12 at the ground and twice
during the grey flicker. `build/probe/launch-ds25.sh` is the same run with
`RSX_FP_STAGE_FIX=stage` and the oracle off.

**Success criterion (one run):** a `stage-store` line whose `addr` lies in the
16-byte quad `nv308a` later copies and whose `new` bytes equal the observed
`bd 3f 27 a1`. That line's `pc` + thread (PPU) or `tag` + `lsa` (SPU) or the
HLE `writer` names the writer. Then disassemble at `pc`:
- `stw` of `T(W)` → the producer is guest code writing the ucode in BE order;
  the question becomes why the hardware/emulator conventions differ (F26/F27).
- `stwbrx`/`sthbrx` → re-check the emitted swap against F30; a mistake here
  would be a genuine RPCS3 store bug shared by every backend.
- SPU PUT with a 2-byte step → H9(b) confirmed.
- HLE builder → the packet is produced in the emulator, not the game.

**Scoring `ds25`.** Same presses, oracle **off**, corrector on. If the ground
stops alternating while the fill is untouched, the producer store is the whole
story (F35); if it still alternates, the log still names the writer, and the
wrong value is being *consumed* elsewhere (H7/H8). Take a screenshot pair at the
same spot in both runs; do not compare across the level-5 window closing.

**Fallbacks if no line appears:** (a) the ring was written before
`cellGcmInit` or through a raw `be_t` pointer, so add the check to the specific
`cellGcm*` builder that emits `nv308a` packets; (b) an earlier RSX blit wrote
the ring, so extend the watch to the destination range of `nv308a` itself;
(c) the value is produced by a DMA engine RPCS3 implements outside the three
paths above — then the submit-boundary PC is the only lead left. Do not guess a
fix before one of these names the writer.

**Finally:** only after the producer is named, decide whether the correct fix is
at the producer, in the shared fill, or (last resort) a per-lane repair in the
fill. Do not ship the oracle or the corrector as a fix.

### Phase 4 — generalise and regress

The fix must hold for any game that uses the same seam, not just DS. Add a unit
test in the style of `test_rsx_metal4_*` for the pure part (byte order, quantise,
routing), then re-run `rpcs3_test` + scrub before any commit.

---

### Phase 5 — the built classifier, and a pixel-first loop — **next**

The two arms designed on 2026-11-18 have never been run. They are the shortest
path from the current state to an RCA.

```bash
# one run, three F12 presses: white floor, black/grey frame, blur frame
MTL_PROBE_VARIANT=literal-provenance ./build/probe/launch-ds24d.sh
```

`literal provenance` classifies every draw against its fill:
`stable` (the fill's own bytes), `image-patched` (the guest rewrote the ucode
between fill and draw), `ring-rewritten` (something wrote the ring block after
the fill), `ring-replaced` (the ring grew and the bound buffer is not the fill's)
and `offset-mismatch` (the draw reads a block the fill did not write). The
histogram is the consumer/producer verdict. Then:

```bash
MTL_PROBE_VARIANT=freeze-fc          ./build/probe/launch-ds24d.sh
MTL_PROBE_VARIANT=freeze-present=120 ./build/probe/launch-ds24d.sh
```

`freeze-fc` holds the first fill of every program, so the per-frame patch cannot
reach the shader: if the artifact freezes or disappears it is patch data, if it
survives it is not. `freeze-present` makes one frame stable enough to dump and
screenshot; use it for the seam and the floor.

Rules for this phase:

- **Read the pixels first.** Score every run with `classify-present.py` before
  parsing any other log. A run without a classified present frame is not scored.
- **Instrument the pre-HUD chain before adding more constant provenance**
  (H13). Raise the draw-trace cap (F53) and log, for every draw that writes
  `0xc07a0000` and samples a 640/320/160-wide source, the target, scissor,
  viewport, source size(s) and the UV varyings.
- **Dump the world target at the last world draw**, not the first (F48), or read
  the seam from `host-present-*`.
- **Do not re-derive the guest's intent from `fix.log`** (F50). Use the image
  bytes and the copy source.

The scorer is `build/probe/classify-present.py <run-dir>`. It samples every 2nd
column and every 4th row, computes mean, sd, the fraction of pixels > 250 and
< 5, and the left/right accumulated |Δlum| across the `x = 640` seam, then
prints and writes `present-analysis.json`. Classes, first match wins:

| class | rule | meaning |
|---|---|---|
| `flash` | white ≥ 0.25 | whole image blown white |
| `absent` | black ≥ 0.85 | HUD only on black |
| `blur-left` | mean > 20, ratio < 0.55, white < 0.25 | scene drawn, left half soft |
| `floor-white` | mean ≥ 20, white ≥ 0.03, black < 0.5 | scene drawn, water/ground blown out |
| `stable` | otherwise | no classified defect |

On `ds24d` it reports `absent=76 blur-left=25 flash=23 floor-white=52 stable=447`
and reproduces the frame lists in §0.2 exactly. Run it before any log parsing: a
run without a classified present frame is not scored.

Convert any single frame to PNG with the same loop (BGRA input, RGBA output)
and a 20-line `zlib` PNG writer; `build/probe/view/` holds the 2026-11-24 crops.

---

## 11. Running log

Newest first. One entry per run or code change: what was done, what was measured,
what it changes in §4/§5/§6.

### 2026-11-26 — the skip-38 run was a no-op: fragment-program ids are run-local

Run: `build/probe/run-20261125-231354-ds24e-skip38` (one F12 press,
`MTL_PROBE_SKIP_FP=38`, 232 present frames 3261–3816, exit 0). Classification:
`stable 125 / absent 78 / floor-white 22 / flash 7 / blur-left 0`; the detail
ratio ranges 0.78–3.86, so this window contains **no** blur-left frame and
cannot test the seam at all.

**The skip never applied.** `grep -c "skip fragment program"` is 0, while the
level-5 log is live (123,725 `Metal 4:` lines). The reason is in the dumps:
fragment program **38 is never compiled in this run** (`fp-38.*` is absent; the
run's ids run `… 36, 37, 39 …`), and the post chain of a 294-draw frame ends
`fp 37 → 0xc07a0000`, `fp 120 → 0xc0010000`. RPCS3 assigns fragment-program ids
by compile order, so ids are not comparable across runs. The skip instrument
itself is fine; the id was wrong.

**What this changes.** The earlier "skipping fp38 does not remove the seam"
observation is **withdrawn**: that run never skipped anything. `fp38` is
neither excluded nor implicated. The next skip must take its ids from the
*same* run's manifests (or the instrument should match on the ucode hash, not
the id). As a baseline, this run does repeat the white-floor, flash and absent
classes on the F60–F62 binary; that is consistent with F61/F62 not producing
those classes, but it is weak evidence because there is no pixel-matched
before/after window.

**Housekeeping.** The fixes, the consolidated document and the two
`RSXProducer*` headers were committed as `3af46b3a9` and pushed to
`origin/metal4-dev` (fast-forward, no force). The probe corpus was then pruned
from 24 GB to 4.1 GB: six superseded run directories were deleted, and in the
three kept runs only the present stream, manifests, logs, `fp-*` artifacts, the
F40 `host-sample-*-addrc07a0000` set and `sample-47-*` were retained. See §7.

### 2026-11-25 (latest) — the blur is persistent, the post chain is named, and the origin scan no longer crashes teardown

Run: `run-20261125-195520-ds24d` (one F12 press, 96 present frames 4685–4780,
109 manifests, exit 134). It is the first run with F60+F61+F62 in the binary.

**The run itself.** `classify-present.py` gives
`stable 88 / blur-left 2 / flash 2 / absent 3 / floor-white 1`. The classes are
unchanged in kind from the previous run; the interesting change is that the
manifests now carry full draw lists (329–340 draws) instead of three, so the
post chain is readable (§11 F65).

**New measurement — the blur is not a two-frame class.** The classifier flags
`blur-left` only below a 0.55 detail ratio, but every *fully recorded* frame of
this run is left-blurred: p8 (present 4691), p20 (4703) and p32 (4715) sit at
0.54–0.55 in every 120-px band, and the 3× crop of 4703 shows the hard `x = 640`
edge through the character with continuous geometry across it (same finding as
F57, now on a second run). Frames 4685–4690 are pre-roll fragments (p6 has 33
draws, p7 one), so their "sharp" ratios are not counter-evidence. The defect is
therefore a **steady-state half-image low-pass**, not a transient.

**New measurement — the post chain of one frame.** F65 lists it draw by draw.
The parts that matter: the last full-screen writer before the HUD is `fp38`, and
it reads `0xc4260000` (written by `fp25` from the scene plus depth) and the depth
target `0xc0b50000`. The HUD (`fp40`) is drawn after it, which is why the HUD is
sharp. `fp108` in this frame is a 640×360 step in the bloom chain, not the
full-resolution blur the previous note assumed; the earlier description of the
chain was from a different frame's draw list.

**New instrument — `MTL_PROBE_SKIP_FP`.** `MTL_PROBE_ONLY_DRAW` drops all but a
contiguous id range, which cannot isolate a post pass because the present copy
is the last draw of the frame. A fenced level-5 diagnostic now drops the draws of
named fragment programs instead, so the flip still presents a whole image and the
window stays usable while one pass is subtracted; the manifest of that frame
simply omits the dropped records. Default off. Used by
`build/probe/launch-ds24e.sh <ids>`. (An earlier draft pointed the flip at a
named guest address instead; it worked as a readback but made the window black,
so it was replaced.)

**Crash fixed — the async origin scan outlived teardown.** The run aborted in
`rsx::producer_origin::scan_guest_range` reading `0x73c0010000` on the detached
`scan_async` thread: the 4.5 s delayed scan was still walking `0xc0000000+` after
the emulator unmapped guest memory. `scan_guest_range` now checks
`Emu.IsStopped()` per page and returns incomplete. The per-1-MiB budget tick was
too coarse: the host mapping is gone before the page table is cleared, so
`vm::check_addr` can still say "allocated" for a page that will fault.

**Gates.** `rpcs3` and `rpcs3_test` rebuild green; `rpcs3_test` 689/689 (2
disabled); `.metal4-release-scrub.py check` clean (200 files deleted, 651 units
excised, 0 residuals).

**What changes.** E12 (half-width scissor) still holds, E13/E14 unchanged. H8
and H11 narrow to the three remaining pre-HUD full-screen writers (`fp164`,
`fp107`, `fp38`) plus the world draws; H13 is weakened because `fp108` is not a
full-resolution blur on this frame. F64/F65 are added to §4.

### 2026-11-25 — the two resource-lifetime guards are implemented

**What was done.** The two confirmed backend gaps were closed, not just
documented.

* **F61.** `surface::memory_barrier` now runs the same guest-memory re-dirty
  test as VK/GL: when `force_data_load` or the read-buffers configuration asks,
  a `ready` surface with a `last_use_tag` whose 16-byte samples no longer match
  guest memory gets `erase_bkgnd`, so it reloads or clears instead of serving a
  stale image.
* **F62.** `resource_manager` now keeps a monotonic frame serial and a per-slot
  completion clock (`on_frame_begin` assigns the serial, `on_commit_complete`
  records the finished frame before the serial advances; the recorder calls
  completion first). `notify_surface_invalidated` stamps the surface with
  `frame_tag`/`frame_slot`, and both reuse paths refuse it until
  `is_frame_complete(frame_tag, frame_slot)`: `is_reusable_surface`
  (`find_reusable_matching_surface`) and
  `surface_matches_properties(..., check_refs=true)` (the
  `invalidated_resources` scan at `surface_store.h:554-580`). The second path
  was found while implementing the first; a guard on only one of them would
  have left the hole open.
* **F60.** `capture_encoder_state` now carries `raster_width/height` scaled by
  the resolution config; the viewport and scissor use it, and
  `framebuffer::bind` stores the scaled extent and includes it in the early
  return key.
* **Present (H16).** The flip picks the newest non-depth overlap section and
  falls back to the texture cache when there is none.
* **Instrumentation.** Origin scans local memory `0xc0000000–0xd0000000` first;
  `note_defect` is needle-only; `record_copy`/`record_surface` no longer seed the
  store filter; the harvest drain is capped at 1.5 M lines and the pre-arm dump
  is bounded by the ring.

**Tests.** `rpcs3_test` is green (689/689, 2 disabled), including
`MTLContract.EncoderStateScalesTheRasterExtentNotTheGuestClip` and
`MTLContract.InvalidatedSurfaceWaitsForItsSlotToComplete`. The scrub check is
clean.

**What changes.** H17/H18 move from "code gap" to "guarded, cause unproven".
The next capture is now the discriminator: if the floor alternation, the
foreign map patch, the noise block or the left-half seam changes, the guard
touched the mechanism; if the frame classes are identical, F61/F62 are excluded
for these frames and the search moves on.

### 2026-11-25 (later) — backend audit: the seam is a rectangle, and the constants are not the producer

**What was done.** The Metal backend was read against the Vulkan and OpenGL
references, operation by operation: render-target identity and reuse, the
validity gate on a texture read, write→sample ordering, present source
selection, invalidated-surface lifetime, and the full-screen post chain. Every
citation in F60–F63 was re-read in this tree; the two audit reports were treated
as leads, not findings.

**Four concrete divergences (corrected after re-reading the code).**

* Metal's `surface::memory_barrier` had no guest-memory re-dirty test (F61).
  VK/GL re-sample and force `erase_bkgnd` → reload/clear; Metal only acted on
  `old_contents`/`erase_bkgnd`, so a `ready` surface whose memory changed kept
  its stale GPU image. The trigger is now implemented.
* Metal does **not** release an invalidated surface immediately: the handle
  defers release to the frame slot that referenced it. The real gap was that
  `find_reusable_matching_surface` could reuse an `invalidated_resources` surface
  without a `frame_tag`, so an in-flight slot's sample could be handed to a new
  allocation (F62). This is now fixed with a per-slot frame clock: an invalidated
  surface carries the serial of the frame that dropped it, and the store will not
  reuse it until that slot reports the frame complete.
* Metal snapshots a cyclic read (`requires_feedback_loop_copy = true`, F63);
  VK/GL insert a texture barrier and sample the live attachment.
* The present took `overlap_info.back()` without checking `is_depth` (H16); the
  shared store appends colour sections before depth sections. Metal now picks
  the newest non-depth section and falls back to the texture cache when none
  exists.

**The candidate that was refuted, then partly reopened.** The audit's top
mechanism was that Metal builds the viewport from `framebuffer_layout` while
VK/GL use `draw_state.viewport`, so a half-width viewport could produce the
`x = 640` edge. Both are driven by `surface_clip` (`RSXCoreRenderer.cpp:174-175`),
but Metal's `layout.width` is *pitch-corrected* (`RSXGraphEngine.cpp:398-414,448`)
while VK's `draw_state.viewport.clip_width` is not, so the rectangles are not
guaranteed equal even at scale 1. Metal also did not scale the rectangle at
scale > 1; that is now fixed. No measurement ties either difference to this
run's seam (F60).

**What the pixels add.** The seam is in place (F57) and the ground is a lighting
saturation with the shadow term intact (F58), not a colour inversion. The
"right half of the depth surface is never written" reading (H20) is withdrawn:
the `sample-47` dump is zero in both halves (an empty dump), and the tape
samples have tens of thousands of distinct values in each half.

**What changes.** §0 now leads with backend resource identity and contents, not
shader data; E18 records that the constant producer is not the story for these
frames; H16–H20 are the ranked backend hypotheses. The next run is `serialise`
→ `heaps` → `sentinel`/`poison`, after three bounded logging additions (§0.3).
This is not an RCA yet: it is a ranked, falsifiable family, and the first run
decides which half of it is real.

### 2026-11-25 — the producer ledger ran, and the literal path is measured silent on every symptom frame

Run: `run-20261124-145124-ds24d` (4 F12 presses, 335 present frames 3104–3438,
exit 134 at teardown as before, data intact).

**What was built.** Two new diagnostic units, both excised families, both
default-off, both wired into the F12 path:

* `rpcs3/Emu/RSX/Core/RSXProducerLedger.h` — a 1 M-record ring of instrumented
  stores, a per-writer census, a page-seeded range filter, a defect registry and
  copy/surface notes. It is independent of `RSX_FP_STAGE_WATCH`; every hooked
  store reaches `record_store` even when the stage watch is off. F12 dumps the
  whole pre-press ring, then harvests for `RSX_FP_LEDGER_MS` (4000).
* `rpcs3/Emu/RSX/Core/RSXProducerOrigin.h` — the post-F12 static-vs-runtime
  report. For every registered defect quad it searches guest RAM and the
  decrypted boot executable for the exact 16 bytes and prints
  `static-executable` / `data-in-ram` / `rsx-write` / `constructed-in-place` /
  `inconclusive`.

Writers are recorded at `vm::fp_stage_watch_write` (PPU JIT, PPU interpreter,
`vm::write`), all four SPU DMA guards, both copy engines (`nv308a`, `nv3089`
host and GPU paths) and the Metal render/depth-target selection. The fill calls
`note_defect` for a lane its convention gate finds out of band.

**Measured (F54–F59).** The ledger is live and complete: 9 704 208 lines in
`ranges` mode (722 MB), 12 writers, `ppu-stw` 855 741 253 calls,
`ppu-stwbrx` 293 549 943, `vm-write` 12 029 209, `spu-list-put` 5 138 432,
`spu-putllc` 2 975 623, `spu-put` 2 596 103, `nv308a` copies 55 997,
`render-target` 11 226, `depth-target` 501.

The decisive number is the one that is zero. **The fill's literal gate fired
0 times in the whole run** (`source=fill` = 0), while the same run's present
stream contains `floor-white=74 flash=12 absent=31 blur-left=2 stable=216`.
The needle `a1273fbd` appears in no `fill.log`, `stage.log` or `fix.log` line;
it appears only as the configured needle echoed in `mtl-probe.log`. So on every
captured symptom frame of this session the symptom was **not** produced by an
out-of-band fragment-constant lane (F55). This does not overturn F18–F24 — the
oracle did remove the strobe in `ds22`/`ds23` — but it means the strobe has a
second producer, or this session's program set never contained the defect.

**The origin report is invalid as delivered (F56).** The 72 quads it scored are
not ucode lanes. They are stage-watch `band` hits — 16-byte groups of guest
pointers such as `c13e05e0`, in RSX local memory — registered by the
`note_defect` hook on the stage-watch band. Two defects in the instrument:
the hook should be needle-only, and the scan range is main RAM
(`0x10000–0x40000000`) so it cannot see the `0xc0…`/`0xc1…` quads at all; every
`constructed-in-place` verdict is therefore unearned. The 64-defect budget was
also exhausted by this noise.

**The present stream was then read as pixels.** Three observations that change
the shape of the problem:

* *The seam is screen-space and in place.* Frame 3411 cropped at
  `x 560–740, y 300–540` (4×) shows a hard vertical edge at `x = 640` with the
  geometry continuous across it: left blurred, right sharp, same character, same
  wall. A texture-size, pitch, tiling or UV error cannot produce that; a pass
  that writes (or fails to write) one half of the destination can (F57).
  Frame 3410, same scene and camera, is sharp on both halves.
* *The floor is a lighting saturation, not a colour inversion.* Frame 3116 shows
  the ground pure white with hard-edged shadow silhouettes while the rest of the
  world is near-black; frame 3118 is the same camera and HUD with the ground
  dark. The lit value flips per frame, and the shadow term still multiplies it
  (F58).
* *A foreign render appears inside the world.* Frame 3116 also carries a
  rectangular top-down "map"-like image at `x ≈ 630–815, y ≈ 0–80` and a
  dithered noise block below it — neither belongs to the scene. That is the
  signature of a resource bound or aliased wrongly, not of a shader arithmetic
  error (F58).

**What the target tally does and does not say (F59).** The per-frame tally
alternates between a full frame (12 targets including the world target
`0xc07a0000`) and a present-only frame (1 target). The pair presents the *same*
image, so this is ordinary double-buffered duplicate presentation, not the
defect; every symptom frame is the full member of its pair.

**What this changes.** The fragment-constant producer hunt is no longer the
main line for the floor, the blur or the absent frames (E18). The next
discriminator is a backend one, and the probe already has the arms for it
(§0.3). The instrument itself needed four fixes, now landed: origin scans RSX
local memory `0xc0000000–0xd0000000` first and main RAM with the rest of the
budget; `note_defect` is needle-only; `record_copy`/`record_surface` no longer
seed the store filter (that is what made 722 MB); and the harvest caps its
continuous drain at 1.5 M lines while the pre-arm dump is bounded by the ring.
The earlier claim that the draw manifest never reaches a symptom frame is
withdrawn: `frame-3103-p14-manifest.txt` (present 3116), `p16` (3118) and
`p309` (3411) each carry 292/292/313 draws. The geometry *trace* was the thing
that stopped early, not the manifest.

### 2026-11-24 (later) — offline: the present stream read as pixels, and the consumer path audited

Run: `run-20261124-132656-ds24d` (exit 134 at teardown, data intact).

**The present dumps were read as images for the first time.** All three symptoms
are in `host-present-*` and each has a distinct signature (F47, §0.2):

- white floor: 3037, 3045, 3049, 3060, 3096 - scene drawn, water surface pure white;
- whole-image white: 2967, 3021, 3068, 3102-3105, 3128, 3402-3404, 3539, 3549,
  3597, 3664 - 92-94 % pure white;
- world absent: 2712-2718, 2785-2814, 2955-2959, 3678-3680 - 85-99 % black, HUD only;
- sharpness split: 3090, 3399, 3415, 3429, 3455, 3461, 3477, 3585-3590, 3652,
  3672-3675 - left/right detail ratio 0.25-0.55, HUD sharp, seam at `x = 640`.

The scorer is `build/probe/classify-present.py` (written this session): on this
run it reports `absent=76 blur-left=25 flash=23 floor-white=52 stable=447`.

`host-world-target-*` is an early-frame snapshot (first `ucode_length >= 3000`
draw), so it shows clear colour + character only and cannot show the seam (F48).

**The consumer path was audited (F49).** The fill is one deterministic `T`, the
ring copy and `fs_constants_offset` are one index, the MSL reads
`fc[x + _fs_constants_offset]`, and the probe reads the same shared bytes at the
same index. The delivered word is therefore `T(image word)` by construction. The
`fix.log` invariant used since 2026-11-18 is circular (F50):
the oracle logs a lane only when its reversal is plausible, so "100 % of
corrected lanes are byte reversals" is a tautology. E16 records the correction.

**The detailed draw trace is capped at 16 lines** (F53), so the bloom-composite
draws are not described in `ds24d`; the half-width hypothesis (H13) needs the cap
raised or the pass named.

**Probe state at the end of the run:** `stage-watch` 2048/2048 and `stage-prearm`
1024/1024 (both caps saturated); `fill.log` 4096 (capture-gated); `nv308a-dst`
logging stops at ms 110 534 because the range table is full. A plain store
outside a ring/stage range and outside the first 16 per writer is invisible by
construction (F46); "0 stores near the source" is therefore not a negative.

**Docs:** 15 dated investigation artifacts deleted; this file is the only Demon's
Souls investigation/handoff doc. Their live conclusions are F4-F7, §8 and §11.

**Next:** §10 Phase 5 - `MTL_PROBE_VARIANT=literal-provenance`, then `freeze-fc`,
then instrument the pre-HUD bloom composite.


### 2026-11-24 — code: the copy destination is scored, and the first merge rewrite crashed

The `ds24d` run is the first with `nv308a` bulk/non-increment destinations
scored by `fp_stage_watch::note_mem` (`allow_ring=false`, `src_ea`/`dst_ea` in
the detail), so a copy into a program image joins to a fill defect by address
and names the guest buffer upstream.

The first launch died before the world: the rewritten merge loop in
`watch_range` stored `count-1` but left the loop bound at the pre-decrement
value, so after merging the last entry it re-entered, reloaded a count of zero
and indexed `ranges()[0 - 1]`. Backtrace:
`fp_stage_watch::watch_range ← nv308a::color4::impl ← thread::run_FIFO`, at the
profile screen (`run-20261124-132203-ds24d/stdout.log`). Fixed in `e6932cc06`
by assigning the new count and guarding `now == 0`; `rpcs3_test` 687/687. This
is exactly why the flags are tested before a run: the crash is in the new
probe, not in rendering.

Even in that aborted run the destination hook fired, and it fired on a
program image: three `nv308a-dst` lines wrote 16 bytes at `0xc14b4b30` —
offset `0x130` inside `0xc14b4a00` len 1680, which is a `fixsrc` image in
`ds24c` — with source EA `0x392ff2f8`. So the copy path does write inside
fragment-program images, which is exactly the join `ds24d` must score; `stage=0`
on those lines only means the image range was not armed yet (prefetch had not
run at the profile screen).

### 2026-11-23 — `ds24c` scored: the PPU layer is live and every hooked writer is excluded; the probe defects it exposed are fixed

**Run.** `run-20261123-223946-ds24c`, `LEVEL=2`, `PROBE=8`, `HIST=4`, `SCAN=1`,
`BASE=1`, oracle `bswap32-all`, `RSX_FP_STAGE_FIX` unset, F12-only. The one-time
`v10-kusa` PPU cache rebuild (233 modules) finished before the world, and a
fresh object contains `__ppu_stage_watch`, so the instrument is compiled in.
The emulator exited through the known unrelated LLVM `MCJIT` teardown
segfault in `Emulator::Quit`; the logs are intact.

**Measured.** 192 662 fill corrections in 93 distinct defect quads across 68
program images, 677 distinct guest words, EA range `c1472e70..c1583e10`.
5285 stores: `ppu-stw` 1122, `spu-put` 4081, `ppu-stwbrx` 32, `spu-putllc`
37, `spu-list-put` 2, `vm-write` 11. `stage-flip` 0. Every `nv308a` sample is a
faithful copy (`dst_bytes == src_bytes`, `same=1`). **Decisive negatives: 0
stores inside a defect quad, 0 of the 115 `stage=1` stores inside a `fixsrc`
image, 0 store values equal to a `fix.log` guest word.** The positive control
proves those layers are live, so this is an exclusion (E15), not a silent
instrument — the F43 failure mode is now ruled out.

**Probe defects found while scoring (all fixed in this commit).** The merge
loop in `watch_range` treated an exact duplicate as a merge, deleting the
matching range and re-appending it; with the range last in the array that is an
infinite loop (latent — a new image was usually appended in between). The same
path re-emitted `stage-watch` on every prefetch and spent the 128-line cap on
128 copies of one 80-byte range, so the 68 real images were invisible. The
`needle=` field carried `needle OR band`, conflating 4924 band hits with needle
hits; it is now two fields (`needle=`, `band=`). `suspicious_word` no longer
includes the weak `plausible` shape (it fires on ~13 % of random data words and
produced the `0x3919…`/`0xc14a…` prearm false positives); the band shape
remains, and the weak shape stays per-store as `suspect=1` for steering only.
`summarize-stage.py` now parses the space-separated `fixsrc`/`fix` lines, prints
the positive control first, the per-writer census flags, the quad join, the
`fixsrc` image join and the `nv308a` destination lines.

**New depth.** The `nv308a` bulk and non-increment destinations are now scored
with `fp_stage_watch::note_mem(..., "nv308a-dst", …)` and carry the guest
source EA in the detail, so a copy into a program image joins to a defect by
address and names the buffer one hop upstream. The destination call sets
`allow_ring=false`: a copy destination is not a command-stream producer, so
only a program image (`in_stage`) or the positive control can log it —
otherwise every chunk would scan and log its whole destination as `ring=1` and
spend the line cap on data that cannot be the animated ucode lane. That closes
the one store path F44 could not see.

**Answer to "do we have the root cause": no.** The residual ground/water flash
is not produced by any hooked CPU store path (E15), and the seam and the grey
flicker are still bracketed but not localized. The remaining class is the
RSX-side copy destination and its guest source (F45); the next run scores it
with the same address join.

**Changes to §4/§5/§6.** Added F44 (the `ds24c` positive control and negatives),
F45 (the remaining class), F46 (the probe defects), and E15. §6 stays open on
the same three branches, with the producer branch narrowed to (a) copy
destination, (b) unwatched guest source, (c) already wrong at first sight.

### 2026-11-22 — code: the probes are rebuilt around a positive control, levelled, and the trace is written

No run yet in this entry; it records the code change, the gates, and the code
trace that ds24c is meant to falsify.

**What was wrong with the probes.** The F43 lesson generalises: a hook that
never runs and a hook that runs and sees nothing produce the same log. The old
`note_mem` was entered only when `interesting(addr, size)` was true, so the
per-writer positive control could not fire for a layer whose gate never opened;
`probe_allow` was also called twice per word, once for the log test and once for
the shape test. Both are fixed:

- every layer calls `note_mem` unconditionally and only the per-layer kill
  switch (`RSX_FP_STAGE_NO_JIT/_NO_SPU/_NO_VMW`) gates the call;
- `note_mem` computes `probe = probe_allow(writer)` first, then cheap-returns
  before the address lookup and word scan when `!in_ring && !in_stage && !probe`;
- `probe_allow` uses a 256-slot table with a thread-local cached slot, so the
  steady state is one relaxed load; `probe_available(writer)` exposes the
  remaining budget without spending it, for call sites that must format a
  detail string.

**New depth.** `watch_range` is now called from
`RSXGraphEngine::prefetch_fragment_program` on the program image itself
(`vm::get_addr(image)`, `ucode_length`), so a store into any prefetched program
is `in_stage` even if its address was never a ring or `nv308a` source.
`watch_range` also takes a first-sight sample (`stage-prearm`), merges ranges
within 64 bytes, and publishes a 4 KiB-page bitmap (128 KiB BSS) that
`range_hit` checks in O(1) before the exact interval scan. `fix.log` gained the
guest EA and the pre-fill guest word, and `fixsrc` names each image, so a
delivered defect joins to a store by address. The `nv308a` re-read hardened
`guest_read` against unmapped pages and logs a `stage-base` first-sight sample
with the decoded slots and a band mask. `stage-store` now carries `seq`, `ms`
and the 16-byte `quad` context; `stage-partial` carries the composed `word`,
`slot`, `alt` and `band`. `stage-flip` is an address-keyed history so an A/B
alternation between two writers is visible.

**Levels.** `RSX_FP_STAGE_LEVEL` 0–3 (default 1; 2 adds the flip history, 3
disables per-site dedupe for a per-store trace); `RSX_FP_STAGE_PROBE` (8),
`RSX_FP_STAGE_HIST` (4), `RSX_FP_STAGE_SCAN` (1), `RSX_FP_STAGE_BASE` (1). Caps
unchanged: 65536 lines, 4096/site, 2048 ranges, 1024 prearm, 8192 flip lines.
`build/probe/summarize-stage.py` now parses key=value (so added fields do not
break it), prints the positive control first (`LIVE`/`DEAD` per expected
writer), then the `fixsrc`/`fix.log` address join, `stage-flip`, `stage-prearm`
and `stage-base`.

**Gates.** `ninja rpcs3` green; the binary carries `stage-prearm` and
`stage-flip` and still carries `v10-kusa`/`__ppu_stage_watch`; `rpcs3_test`
687/687 (2 disabled); `.metal4-release-scrub.py check` exit 0 with 0
needs-review, 0 residuals, 0 orphans.

**Code trace (measured vs inferred).** Read against the tree:

- *White flash — the delivered word is wrong, not the binding.* Measured: the
  fill is a CPU-side snapshot (`_mm_stream_si128` is a plain store,
  `sse2neon.h:6028`), so a guest patch after the fill cannot reach this frame's
  block; `fs_constants_offset` is `fragment_constants_index()`
  (`MTLGSRender.cpp:6511`, `:6775`), and the `= 0` at `:6251` is overwritten
  before encode; the buffer base never changes in a frame
  (`constant_capacity = 65536`), so the ring-identity test at `:6254` cannot
  misfire; a program change forces a refill (`:5105`). Inferred: the fill's
  unconditional 16-bit-half swap turns a lane the guest wrote *pre-swapped*
  into a finite out-of-band value, and `h0 * fc[67].x / 2` then clamps to white
  — which is why the defect is a bright flash and not garbage.
- *Grey flicker — upstream of the compose.* Measured: the compose is one
  full-aspect draw (`MTLPresent.cpp:151+`); `present-*` (pre-FSR),
  `upscaled-*` (post-FSR) and the screenshot (post-compose) already bracket it;
  `fp-ucode-image` (`MTLGSRender.cpp:6488`) prints compiled vs live ucode hash
  and a match bit. Inferred: a whole-frame grey at frame granularity is the
  wrong fragment program bound for that frame, or a present source that is a
  cleared/stale surface.
- *Seam — before the HUD.* Measured: the present compose runs after the sharp
  HUD, and `pass_scheduler::split_requested()` has no callers. Inferred: a
  pre-HUD pass that writes the 1280x720 world target from the 640x360 bloom
  chain with a 640-wide viewport/scissor (H11), or a world draw that samples
  that chain across the full viewport (H12).

**Run.** `build/probe/launch-ds24c.sh` launched at 22:39
(`run-20261123-223946-ds24c`) with `LEVEL=2`, `PROBE=8`, `HIST=4`, `SCAN=1`,
`BASE=1`, oracle `bswap32-all`, and `RSX_FP_STAGE_FIX` unset. F12 only; nothing
is armed automatically.

**First-run cost: the PPU cache rebuilds (F43, measured).** The `v10-kusa`
bump forces every PPU object to be recompiled once, so the game holds at the
"shader compilation" screen while LLVM builds 233 modules. This is expected, not
a hang: the process sat at ~520 % CPU, the cache directory gained
`v10-kusa-*.obj.gz` at a steady rate (124 at 22:42, 131 at 22:43, heading for
233), and `gzcat` of a fresh one contains `__ppu_stage_watch`. The `v9-kusa`
set (233 files) is still on disk beside it. The `stage.log` positive control
already fired for `vm-write` (eight `seq=1..8` lines at `ms=0`), so the
in-process instrument is live before the first frame.

Scoring order: first the `v10-kusa-*.obj.gz` for `EBOOT.BIN` contains
`__ppu_stage_watch`, and `summarize-stage.py` shows `LIVE` for `ppu-stw` /
`ppu-stwbrx`; then the `fixsrc`/`fix.log` EA join, `stage-flip`, `stage-prearm`,
`stage-base`. A `DEAD` writer is a broken instrument and voids its layer for the
run.

### 2026-11-19 (later) — code: the PPU store hook had never run; the object cache was stale

One line changed, and it invalidates a headline negative from the previous four
runs.

**What was found.** `PPUThread.cpp` names each cached PPU object
`v9-kusa-<base57(module-hash)>-<base57(settings)>-<cpu>.obj`. The hash covers
the guest module bytes and the settings, **not** the translator, so the literal
`v9-kusa` is the only thing that invalidates the cache when generated code
changes. `cb7b3c811` added the `__ppu_stage_watch` calls to `WriteMemory`,
`STVLX` and `STVRX` on 2026-11-18 12:29 and left the literal alone.

Measured, with no emulator running:

- every `v9-kusa-*.obj.gz` for `EBOOT.BIN` (233 files) is dated
  **2026-11-12 20:36**;
- `gzcat <obj> | grep -c __ppu_stage_watch` is **0** for all 233;
- across the whole cache, **0 of 372** `.obj.gz` are newer than 2026-11-14,
  while `spu-safe-v1-tane.dat` was rewritten on 2026-11-18 13:57 — so the SPU
  cache did rebuild and the SPU hook did run.

`jit_compiler::load_object` validates only the object-file format
(`Utilities/JITLLVM.cpp:963`), so the stale objects were accepted and linked.
The PPU JIT layer — the most likely writer of a per-frame ucode patch — was
therefore **not watched in any run so far**, and "zero `ppu-stw`" is expected
output, not a negative result.

**Fix.** The literal in `PPUThread.cpp:5833` is now `v10-kusa`, with a comment
stating the rule. Rebuilt `rpcs3` (the first `ninja rpcs3_test` only relinked
the test binary; the app needed its own target): `strings` finds `v10-kusa` and
`__ppu_stage_watch`, and no `v9-kusa`. `rpcs3_test` 687/687 (2 disabled).
Scrub still clean (the literal is not `EXCISE` code; a one-time recompile in
the scrubbed release is harmless and correct for a changed build).

**What it changes.** §1's "no store hook has seen the word" is narrowed to the
layers that ran (SPU, `vm::write`, RSX blit/partial); §4 gains F43; H9's
2026-11-19 update is corrected. The next `ds24c` must first prove the cache
recompiled (`v10-kusa-*.obj.gz` exists and contains the symbol) and that
`stage.log` now has `ppu-stw`/`ppu-stwbrx` lines at all, before any store is
compared to the defect words.

### 2026-11-19 (day) — offline: the seam lives in the present-source world target, and every half-width explanation is excluded

No run, no code change: this entry reads the archive left by `ds24b`
(`run-20261118-132833-ds24b`), decodes the first complete draw manifest of a
frame that reaches the world, and re-measures the ten world-target samples with
one metric.

**The manifest.** `dumps/frame-6637-m9-manifest.txt` (header `present 6628`)
has 293 records, all `frame 6628`, in submission order. Grouped:

| records | draws | into | what |
|---|---|---|---|
| 249674–249724 | 51 | depth `0xc4860000` 2048² | shadow map, `fp 103`, 1024² quadrant scissors |
| 249725–249839 | 115 | `0xc07a0000` | world geometry, `prim 6`, `fp 86/71/62/26/20/7/118/75/16/89/101/104/23/105/4/9/36/43/64/40` |
| 249840 | 1 | `0xc07a0000` | `fp 141`, 52-v strip, no textures, constant colour (fog/atmosphere) |
| 249841 | 1 | `0xc5870000` | `fp 110` copy 1 |
| 249842–249866 | 25 | `0xc07a0000` | `fp 155` (2D+cube), `fp 142`, `fp 113` strips, `fp 87`, `fp 140`, `fp 134`; all sample `0xc5870000` + depth |
| 249867 | 1 | `0xc5870000` | `fp 110` copy 2 |
| 249868–249869 | 2 | `0xc07a0000` | `fp 57` strips, sample `0xc5870000` + depth |
| 249870–249885 | 16 | bloom targets | `fp 56/96` 640x360, `fp 91`, `fp 10` 1280x720, `fp 102` 320x180, `fp 83` 64x64, `fp 50/66` 320x180, `fp 102/66/47` 160x90 |
| 249886 | 1 | `0xc07a0000` | `fp 88` bloom composite (`0xc4260000` + `0xc4242840`) |
| 249887 | 1 | `0xc4260000` | `fp 14` |
| 249888–249965 | 78 | `0xc07a0000` | HUD: 70 × `fp 28`, 7 × `fp 8`, 1 × `fp 31`, all 4-v |
| 249966 | 1 | `0xc03d0000` | `fp 95` present compose, unit 0 = `0xc07a0000` |

**What the manifest settles.** (a) All **222** draws into `0xc07a0000` use
`scissor 0,0,1280,720` and the full viewport; the only sub-full scissors in the
whole frame are the shadow quadrants and the bloom targets. A half-width
scissor cannot be the seam (F37/E12). (b) The `fp 110` copy is a 4-vertex
full-screen 1-texture draw, and the destination sampled at the copy draw is
byte-identical to the source sampled one draw earlier (same hash, row 0
`00040404`; `cmp` clean at 6642/6732/6734), so the copy adds no blur
(F38/E13). (c) The present is one 4-vertex `fp 95` draw sampling `0xc07a0000`,
made *after* the HUD and overlay draws write that same target — so any spatial
filter at the present would blur the HUD too; the HUD is sharp, so the blur is
added before the HUD (F39). (d) No RSX blit is logged anywhere in the
`73d1e6ad4` run (E14). (e) The bloom chain is the only blurred source in the
frame and it downsamples the full world target; the depth-tested effects sample
the copy, not the world target.

**The seam, re-measured.** The ten `host-sample-*-addrc07a0000` files, one
metric (mean |Δgreen| per 2 px, y stride 4; band ratio = left/right inside each
120-px band):

| sample | stage | mean L/R | HF L/R | band ratios (top→bottom) |
|---|---|---|---|---|
| 6638 | present source | 3.74 / 0.80 | 1.59 / 0.55 | 1.04 1.64 0.00 0.00 0.00 5.84 |
| 6639 | pre-effects `fp 141` | 0.03 / 0.00 | 0.06 / 0.00 | black frame |
| 6640 | stage line ambiguous | 3.74 / 0.80 | 1.59 / 0.55 | same as 6638 |
| 6642 | pre-effects `fp 141` | 10.09 / 15.37 | 1.19 / 2.10 | 0.74 0.54 0.45 0.58 0.55 0.47 |
| 6643 | present source `fp 95` | 19.19 / 25.10 | 2.24 / 4.20 | **0.59 0.21 0.09 0.16 0.85 1.38** |
| 6731 | stage line capped | 17.66 / 22.47 | 2.18 / 1.67 | 0.79 0.41 0.23 0.47 2.14 2.17 |
| 6732 | stage line capped | 8.89 / 13.56 | 1.05 / 1.85 | 0.74 0.54 0.45 0.59 0.56 0.47 |
| 6733 | stage line capped | 58.58 / 65.67 | 2.96 / 2.43 | 0.94 0.76 0.49 0.78 2.10 1.79 |
| 6734 | stage line capped | 8.68 / 13.24 | 1.02 / 1.80 | 0.74 0.54 0.44 0.59 0.56 0.47 |
| 6735 | stage line capped | 16.99 / 20.85 | 2.15 / 1.82 | 0.81 0.61 0.31 0.48 1.95 2.23 |

6643 is the strong seam; 6642/6732/6734 are the same scene at three exposures
(identical ratios to two decimals); 6733 is the brightest and has no seam, so
the seam does not simply track brightness. The ratios are content-dependent and
therefore a locator, not a proof; the proof is still the crop that crosses the
character's head at `x = 640` (F40). The stage column is label-exact only where
the level-5 capture trace survives (`6638`, `6639`, `6642`, `6643`); the
`673x` windows lost their stage lines to the per-site cap (F41), and `6640`'s
bind sits astride the frame boundary.

**Why it is still open.** The capture dumps a bound image once per host
`MTL::Texture*` per frame, at the first stage that binds it — `fp 141` in
6642, the present compose in 6643 — so one frame carries only one of the two
stages. The labelled pre-effects sample (6642, clean) and the labelled present
source (6643, seamed) are different frames. The stage split needs one frame
with both (F42).

**Next.** One unconditional, bounded dump of the present compose's
`source.image` (`present-source`), so the same frame carries the pre-effects
and post-HUD images. Then: seam in both → world draws (H12, inspect per-half
mip/LOD/UVs); seam only at present → bracket the ~28 pre-HUD post draws (H8).

§4 gains F36–F42; §5 gains E12–E14; §6 narrows H8/H11 and adds H12; §10
Phase 2.6 records the one-frame discriminator.

### 2026-11-18 (afternoon–evening) — `ds24b` ×4 and the seven commits: the producer watch is live, and it is silent

What was done (all committed, in order):

1. `cb7b3c811` — the Phase-2.6/3 instrumentation and the producer watch; also
   `MTLDevice::import_guest_window()` now imports with
   `utils::memory_protect(rw)` instead of `memory_commit`, dropping a pointless
   4 GiB `MADV_WILLNEED`. **Shipping fix, independent of the strobe.**
2. `12b786b25` — `MTLCommandRecorder::flush_for_readback()` checks
   `slot.buffer_open` before `endCommandBuffer()`; ring-arm dedupe and a
   fallback. **Shipping crash fix**: reachable since ZCULL sync landed
   (2026-11-05), reproduced 2026-11-18 with Apple's
   `_commandAllocator && _storage` assertion.
3. `7d17787e9` — watch the SPU writes that bypass `do_dma_transfer`
   (inline list PUT, PUTLLC, PUTLLUC).
4. `1746f0d03` — arm at `gcmMapEaIoAddress` with unioned ranges; shape-based hit.
5. `ffb39986a` — accept a single animated lane in the shape.
6. `6fb67f31e` — generous detector: any store whose half swap lands in the
   authored band, logged anywhere.
7. `033f2fb8f` — watch `ppu_write_mmio_aware_u32` raw stores (MMIO-classified
   PPU stores bypassed `WriteMemory`'s hook).

Runs: four `ds24b` (13:28, 13:36, 13:43, 13:55), all with the proven
`bswap32-all` oracle, all reaching the world (frames 4000–6800). Every shutdown
crash was the known, unrelated LLVM `MCJIT` teardown.

Measured:
- **The producer watch is silent on the defect.** 4000–5750 stores per run
  (`spu-put`, `spu-putllc`, `vm-write`; zero `ppu-stw`, zero `spu-list-put`),
  and **not one of them carried any of the 288 distinct defect words** the fills
  delivered. The ring is armed (`stage-arm ring=38000000 len=02000000`), and
  ring stores are logged, so this is a real negative for every hooked layer.
- A fixed needle (`9d263fbd`) was present in one run and absent the next: the
  word animates, so a literal needle is a moving target; only the shape filter
  is stable.
- The accurate-SPU-DMA configuration changed nothing → the SPU inline-PUT
  variant of H9 is refuted.
- The generous detector produced 4486 hits, all noise (e.g. `0x3f003f00` from
  Havok); cross-referenced against the 52 real words, none matched.
- **Residual quantified, and it is the oracle's gate.** 154 of 1260 world draws
  (12.2 %) deliver finite but out-of-band transposed values (−0.0397, 51.9,
  3533.9). They pass the `[1e-6, 1e6]` plausibility window, so `bswap32-all`
  leaves them alone, while the authored band is `[0.315, 2.222]`. This is why
  the ground still flashes even with the oracle on.
- **The half-screen seam was localized** to the world colour target *before*
  the present copy. The only quadrant-split passes are the shadow-map ones
  (2048², four 1024² scissors); the colour passes are not split. Not
  root-caused. The user's later observation sharpens it: plotted at `x = 640`,
  exactly half of 1280, **only the right half is sharp**.

What it changes: §4 gains the negative result (no store hook has ever seen the
word); §6 gains H10 (whole-image grey flicker = the same reversed constant in a
fullscreen pass) and H11 (the exact-half sharpness split). The next step is not
another store hook — it is (a) the partial-store extension, (b) the RSX blit
writers, both already written, and (c) reading the tape's per-region energy for
the seam.


What ran: the phase-3 census with the (untested) `bswap32-any` oracle, the full
stray census, and `RSX_FP_STAGE_WATCH` on. Run dir
`build/probe/run-20261118-120111-ds24`.

What was measured:
- `stage.log`: 4042 stores, **all** `spu-put` in `0x390bc000`, no `ppu-*`,
  no `vm-write`, no `gcm-*`; 522 matched the F31 shape; **no needle hit**.
- One `stage-submit` (`get_put=0x100000001000`, `PPU[0x1000005] HighGraphics`)
  and **no `stage-arm`**: the ring was never armed.
- No `fill.log`, no `fx` window, no `png/`: the world was never reached. The
  only staging traffic is boot-time SPU noise.
- Crash at ~152 s: `Segfault reading location 0000000100000000` inside
  `logs::message::broadcast`, annotated `cellSysutilGetSystemParamInt`; exit 134.

Two defects in the instrumentation, both now fixed:
1. **Out-of-bounds table write.** `dedupe_insert`/`shadow_peek`/`shadow_store`
   indexed with `(key * 0x9e3779b97f4a7c15) >> 48`, which reaches 65535, while
   `shadows()` has 16384 entries. The write ran 2 KB past the array into
   adjacent statics — the corruption surfaced later as a wild read inside a log
   call. Replaced with `hash_slot(key, count)`: mix, then mask with the table
   size. This is the most likely cause of the crash, and it would have hit any
   run that logs more than a handful of words.
2. **Arming with `ioAddress == 0`.** DS passes `ioAddress = 0` and reaches the
   ring through the io page table, so `arm_ring(0, ioSize, …)` armed nothing
   (and would have armed guest low memory had `ioSize` been nonzero). The arm
   now translates io offset 0 through `render->fifo_ctrl->translate_address(0)`
   and uses `ioSize` (fallback 32 MiB).

Hardening added at the same time:
- `note_mem` reads the store back only when
  `vm::check_addr(addr, page_readable, size)`; `quad_sibling_plausible` checks its
  16-byte quad. A range that names an uncommitted page is now skipped, never
  read.
- `RSX_FP_STAGE_NO_JIT` / `_NO_SPU` / `_NO_VMW` are wired to the three hook
  families (the JIT/vm shim decides by its `from_jit` argument), so a recurrence
  can be bisected without a rebuild.
- `probe_allow` takes the first 8 stores of each writer regardless of shape, so
  a silent layer is distinguishable from a quiet one.

Gates after the fix: build links, `rpcs3_test` 687/687, scrub `check` clean.

Next: `ds24b` (`launch-ds24b.sh`) — proven `bswap32-all` oracle, no stray
census, stage watch on. The success criterion is unchanged: a `stage-store` in
an `nv308a` source range whose `new` is `a1273fbd`/`bd3f27a1`, naming
writer/`pc`/thread or SPU `tag`/`lsa`; plus a `stage-arm` line proving the ring
window is live. If it crashes again, relaunch with `RSX_FP_STAGE_NO_SPU=1`
(that is the only family ds24 exercised) and report, not hide, the regression.

### 2026-11-18 — code: the producer watch and the producer-side corrector are implemented

What changed:
- `rsx::fp_stage_watch` (`RSXProgramWriteWatch.h`, `fp-stage-store-watch`):
  ring arm, merged `nv308a` source ranges, F31 shape predicate, needle word,
  per-(site,address,word) dedupe with a shadow of the previous word, partial
  store logging, submit log, and a default-off corrector
  (`RSX_FP_STAGE_FIX=stage|io|all`) that byte-reverses a suspect word in guest
  memory after the store, logging the original first with `fix=1`.
- Hooks: `PPUTranslator::WriteMemory` + `STVLX`/`STVRX` →
  `__ppu_stage_watch` in `s_link_table`; `vm::write` → `vm::fp_stage_watch_write`
  (defined in `vm.cpp` with a cached per-thread name); `do_dma_transfer`
  scope guard; `cellGcmInitInternal` arm; `make_command`/`make_jump`/
  `cellGcmCallback` notes; `set_rsx_dmactl` submit log; `nv308a`
  `watch_range` on both copy forms.
- All fenced, default off, bounded; no behaviour change without the env vars.

Why: H9 needs the writer named, and the 50 % hypothesis is that the writer is a
plain big-endian store that the fill faithfully `T`s. `ds25` tests that directly.

Next: build (`ninja -C build-tests rpcs3 rpcs3_test`), gates, then `ds24`
(census) and `ds25` (corrector). Score §10 Phase 3.

### 2026-11-18 (late night, second) — offline: the producer watch is designed

- Re-read the store path rather than assuming. `PPUTranslator::WriteMemory` is
  the single guest-store chokepoint, `m_is_be` is `false`, and `STWBRX` passes
  `is_be=false`; the emitted store is therefore correct for both `stw` and
  `stwbrx`. **The observed `BE_bytes(T(W))` is what a plain `stw` of `T(W)`
  writes**, not a mis-emulated byte-reverse store. → F30.
- Derived the store-watch predicate from the fill's own transform:
  `word` = the u32 a 4-byte store writes; the fill delivers `slot = T(word)`;
  the defect is exactly `!plausible(slot) && plausible(bswap32(slot))`.
  Checked numerically on the observed word, the correct word and a static
  `2.0`; only the observed one matches. This is what keeps the producer log
  near-silent. → F31.
- Mapped one chokepoint per writer layer (PPU JIT/interpreter, SPU normal and
  list DMA, HLE `vm::write`, RSX write watch), the ring announcement at
  `cellGcmInit`, and the submit boundary at `set_rsx_dmactl`. → F32. Confirmed
  the JIT can call a C++ hook through `s_link_table`, the same route as
  `__write_maybe_mmio32`. → F33.
- Rewrote §10 Phase 3 as a one-run plan with a concrete success criterion and
  fallbacks. No code for it is in the tree yet; unlike Phase 2.6, this is a
  plan, not an implementation. Next is the Phase 2.6 `ds24` run; if it confirms the
  residual is the same producer, the Phase 3 hooks go in one build.

### 2026-11-18 (late night) — offline: the producer is narrowed to one missing `bswap32`

- Re-derived the byte relation from the ds23 artefacts instead of guessing:
  the game's feed carries the value as a BE f32 (`3f bd a1 27`) in every
  `frame-*-transform-constants.bin`; the post-fill host buffer carries the
  LE f32 (`27 a1 bd 3f`); the ucode slot carries `bd 3f 27 a1`. For
  `W = 0x3fbda127` the correct slot is `LE_bytes(T(W)) = a1 27 3f bd`; the
  observed slot is `BE_bytes(T(W))`. So the patcher applied `T` and then stored
  the word big-endian — one missing `bswap32`, no other scrambling. The correct
  pattern occurs in **0** dumped files; the observed one in 43 (`fp-*.ucode.bin`).
  → F26/F27.
- Confirmed the constant is consumed: `fp-71.metal` uses `h0 * fc[67].x` and
  `ratio / fc[68].y`. This closes the handoff's "is the literal causal?" open
  question. → F28.
- Parsed the 4096 `conv` census lines: `c2 = 0` everywhere (no uniformly
  half-swapped quad); `neither` quads exist (max 14). So H7 is bounded to
  lane-local strays in non-trailing quads. → F29.
- Doc updated: §1 position, F24 40→41, F26-F29, §6 (H1/H3/H7/H8/H9 in one
  table, H2 closed), §10 Phase 2.6, §11.
- **Phase 2.6 instrumentation implemented and built.** Four `EXCISE`-fenced,
  default-off additions: the `stray` census (`RSX_FP_STRAY_LOG`), the
  all-quads `literal scan`, the `bswap32-any` oracle arm, the `nv308a`
  staging-range watch (`RSX_FP_STAGE_LOG`), and the tape's per-region
  `tape detail` energy. `ninja -C build-tests rpcs3 rpcs3_test` succeeded;
  `rpcs3_test` 687/687 (2 disabled); `.metal4-release-scrub.py check` clean
  (213 files deleted, 617 units excised, 0 review / residual / orphan / empty
  body). No shipping path reads any of the new variables.
- Next: the instrumentation listed in §10 Phase 2.6, then `ds24` with F12 at
  the ground.

### 2026-11-18 (night) — `ds23`: the family-wide oracle removes the strobe

- Ran `RSX_FP_LITERAL_FIX=bswap32-all` with the `ucode_length >= 3000` gate
  removed (5 F12 presses at frames 3384/3927/4209/4389/4465). `fix.log`:
  **221 834 lanes across 41 distinct fp offsets**; `world tail` reaches the
  shader as `1.69832 2 1.69832 1`. Present classes 32 black / 11 dark /
  51 mixed / 2 white (96 sampled) versus ds22's 140 / 81 / 11 / 88 (320).
  **The full-screen strobe is gone; the world is visible.** → F24.
- Offline replay of ds22's 49-program census predicted exactly this set: 36 of
  49 would pass the `[W,2,·,1]` guard, fp 155 only in its first trailing quad,
  and none of the unrelated 2D programs would be touched.
- Residual measured (F25): the 2 white presents are HUD-only frames with no 3D
  world (p4461); the world itself still alternates **ground white↔black**
  (t4194/t4195) and is **blurred while the HUD is sharp** (p4465 left half
  blurred / right half sharp). The delivered epilogue scale is in-band, so the
  ground flicker is most likely a stray word in a non-trailing constant quad
  (H7); the blur is a post/present question (H8).
- Code: `RSX_FP_LITERAL_FIX=bswap32-all` now applies to every image with the
  per-quad `[W,2,·,1]` guard; the fill log logs every program with `>= 2`
  constant quads (4096-line cap); the expensive ring scans stay on
  `length >= 3000`. Build green, `rpcs3_test` 687/687, scrub `check`
  0 review / 0 residual / 0 orphan.
- Decoded present frames were produced with `build/probe/tape2png.py`
  (BGRA8 1280x720 → PNG, plus per-half gradient energy).

### 2026-11-18 (later) — `ds22`: the oracle ran; the defect is family-wide

- Ran the unconditional oracle `RSX_FP_LITERAL_FIX=bswap32-all` (4 F12 presses,
  frames 4260/4688/4765/4915, 4.3 GB). `fix.log`: 4031 lanes corrected, both
  world programs (21642368: 580, 21442304: 3451). `world tail` at draw time now
  reads `1.48148 2 1.48148 1` (was `4.49e-15`). The user saw the character
  silhouette — the multiplier correction is real.
- **The strobe survived**: 140 black / 88 white / 81 dark / 11 mixed present
  classes. The multiplier was not the only driver.
- Root of the survival: `literal watch` names **49** fragment programs with a
  non-uniform trailing pair; ~30 still carry the stray `[W,2,·,1]` word at draw
  time because the oracle had a `ucode_length >= 3000` gate. The unrelated 2D
  programs (fp 19/33/47/…) have other shapes. → F23.
- Code: removed the `ucode_length >= 3000` gate from the oracle and from the
  fill log (the shape guard is the discriminator); the expensive ring scans stay
  on `length >= 3000`. `build-tests` rebuilt, `rpcs3_test` 687/687, scrub clean.
- Space: deleted the per-texture `conv-*`/`sample-*`/`frame-*-persistent.bin`
  dumps and the `gui-probes` run; `ds20`/`ds21`/`ds22` kept with all logs, the
  present timeline, `fp-*`, manifests and fragment constants (~0.5 GB each).

### 2026-11-18 (evening) — `ds21`: the strobe is measured, the mechanism named

- Ran `build/probe/launch-ds21.sh` (headless, `MTL_PROBE=5 MTL_PROBE_TAPE=16
  MTL_PROBE_FRAME_COUNT=120`, `RSX_FP_LITERAL_LOG`). 164 s, six F12 presses
  (frames 3008/3512/3834/3983/4038/4191), exit via the known MCJIT teardown
  segfault after the evidence was written.
- Artefacts: `build/probe/run-20261118-095414-ds21/` — 6.2 GB, 80 present-stream
  tape images, 1331 fill records (1085 of the two world programs), 308 nv308a
  chunk records. → F20.
- Tape classification: 62 black / 8 white / 10 transitional; black ≥96 % pure
  `#000`, mean 1.0; white 93.7 % pure `#fff`, mean 246.3; black runs 1–12
  presents, white 1–2; whole-image classes with scene-sized transition bboxes,
  no band; HUD visibility independent of class. → F20.
- The world programs are fp 70 (ucode 3312 B) and fp 63 (3376 B), both ending in
  `[W, 2.0, W, 1.0]` with `W` guest-animated. The epilogue reads `fc[67].x` as
  the multiplier and `fc[68].y` (authored `2.0`) as the divisor. → F18.
- The shader's multiplier is `T(raw)` (16-bit-half byte swap); the intended value
  is the half-swap `(raw<<16)|(raw>>16)`, i.e. `bswap32` of what the shader
  fetches. Measured: `a1273fbd` → shader `4.48917e-15`, intended `1.48148`;
  `d6ee3f7c` → shader `-3.319e28`, intended `0.987655`; `52733ea1` → shader
  `+1.66878e31`, intended `0.315082`. → F19.
- Quantified: fp 70 217 fills, fp 63 868 fills; intended multiplier always in
  `[0.3151, 2.2220]`; delivered is ≥1e6 in 530 fills and <1e-6 in 430, landing
  in the intended band only 5 times by accident (fp 70: 43 +huge / 63 −huge /
  86 tiny, 1 in-band; fp 63: 172/252/344, 4 in-band). Positive-huge → white,
  negative-huge and tiny → black. → F22, E11.
- Killed H3 (one pipeline, one image, only the word changes → E10), H5 (input is
  already out of range → E11), H6 (feedback `live 0` on every witness, depth
  sample byte-identical across classes → E9, F21).
- No code change in this entry; the tree is unchanged from the 09:53 build.

### 2026-11-18 — F12 path documented, tree cleaned

- Wrote the F12 capture-history artefact (2026-11-18; folded into §9): the full
  trigger → arming → dumps → freeze → window close → artefacts path, with the
  env switches and budgets.
- Found and recorded (F15) that `probe::dump_frame_count()` is dead code and the
  default window is 2 frames wide, not 1 and not 6.
- `import_guest_window()` switched to `utils::memory_protect(rw)`; duplicated
  comment merged (F14). Uncommitted, needs rebuild.
- No DS run.

### 2026-11-17 — FIFO reverted, FP/ROP port landed, DS evidence decoded

- `9c8bb94d2`: upstream FIFO parser restored byte-for-byte; branch parser,
  recovery rules and `test_rsx_fifo_streams.cpp` removed. Test count 696 → 687.
  → F1, F2, E4.
- `2f3a52558`: four backend-specific upstream FP/ROP changes ported. Reviewed as
  behaviour-neutral at `high`. → F8.
- `0ed64a761`: guest mirror made writable before the no-copy import. → F14.
- `ds20` decode: the world block is `[a1273fbd, 00400000, a1273fbd, 803f0000]`,
  two conventions in one block, guest ramps the tail 1 ulp. → F10-F13.
- Harness #1/#2 measured Metal 4 heap and varying semantics. → F7.

### Earlier

See the earlier dark-world and FIFO handoffs (removed 2026-11-24; their live
conclusions are F1–F7, §8 and §11) and `INVESTIGATION-2026-10-22.md` (Ridge
Racer; the probe facility and method).

---

## 12. File and line map

| Seam | File | Lines |
|---|---|---|
| F12 → bit | `rpcs3/rpcs3qt/shortcut_settings.cpp`, `gs_frame.cpp` | 78, 301-305 |
| bit consumed / armed | `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp` | 8288-8298 |
| arming policy, window | same | 350-440 |
| Phase-2 arms (definitions) | same | 3194-3606 |
| Phase-2 arms (fill / draw hooks) | same | 6580-6656 |
| arm at present (`freeze-present`) | same | 8270-8290 |
| tape copy / freeze / flush | same | 2791-2851, 2853-2863, 2865-3133 |
| host image dump | same | 2450-2609 |
| present / upscaled dumps | same | 8197, 8390 |
| ring dump hand-off | same | 8521-8532 |
| ring snapshot / dump | `MTLConstants.cpp` | 605-711, 359-559 |
| fragment-constant fill (call) | `MTLGSRender.cpp` | 6250-6280 |
| `fs_constants_offset` latch | same | 6251 (default), 6511, 6775 |
| `fc[]` declaration / fetch macro | `MTLFragmentProgram.cpp` | 253, 264 |
| the fill transform `T` | `ProgramStateCache.cpp` | 642-687 |
| fill oracle / `fix.log` | `ProgramStateCache.cpp` | 753-898 |
| constants ring (`m_sets[m_slot]`) | `MTLConstants.h` | 536-566 |
| world draw / tail / fc / state | `MTLGSRender.cpp` | 7010-7110 |
| present source dump | `MTLGSRender.cpp` | 8376 |
| world-target dump (first world draw) | `MTLGSRender.cpp` | 5825 |
| draw-trace cap | `MTLGSRender.cpp` | 4582 |
| `literal-provenance` classifier | `MTLGSRender.cpp` | 3337-3480, 6759, 6807 |
| `freeze-fc` / `freeze-present` | `MTLGSRender.cpp` | 3534-3670 |
| nv308a dst note_mem + write watch | `nv308a.cpp` | 191-232, 293-370 |
| PPU store chokepoint (JIT) | `PPUTranslator.cpp` | 992-1006 |
| ROP properties | same | 360-424 |
| `_builtin_sxx`, `clamp16` | `FragmentProgramDecompiler.cpp` | 924-1050 |
| 16-bit-half swap + literal arms | `ProgramStateCache.cpp` | 700-1168 (oracle 711-853, stray census 855-959, fill log 961-1168) |
| nv308a chunk detail / guest compare | `nv308a.cpp` | 98-195 (stage watch), 202-370 (write watch) |
| per-draw literal scan (all quads) | `MTLGSRender.cpp` | 6278-6392 |
| world draw / tail / write ring | same | 6857-6960, 2960-3135 |
| tape class verdict | same | 3035-3098 |
| tape per-region detail energy | same | 2936-3011 |
| epilogue usage of `fc[67]`/`fc[68]` | `build/probe/run-20261118-095414-ds21/dumps/fp-70.glsl` | 1023-1024 |
| guest import | `MTLDevice.cpp` | 352-371 |
| PPU store chokepoint (JIT) | `PPUTranslator.cpp` | 951-988 |
| `m_is_be` (host LE) | `PPUTranslator.cpp`, `CPUTranslator.cpp` | 32, 25-27 |
| `stwbrx` / `stwx` emitters | `PPUTranslator.cpp` | 3743-3746, 3219-3226 |
| interpreter store macro / `stwbrx` | `PPUInterpreter.cpp` | 41-51, 5425-5434 |
| JIT symbol table (hook route) | `PPUThread.cpp` | 5168-5195 |
| SPU normal / list DMA | `SPUThread.cpp` | 2125, 2927 |
| producer watch (shared) | `RSXProgramWriteWatch.h` | `fp-stage-store-watch` (arm 525-610, note 612-700, `interesting`, fixer, `fix_level`, `quad_sibling_plausible`) |
| JIT store hooks / symbol | `PPUTranslator.cpp`, `PPUThread.cpp` | `WriteMemory` ~989, `STVLX`/`STVRX` ~3748/3784, `s_link_table` ~5188 |
| HLE store hook | `vm.h`, `vm.cpp` | `write` ~330, `fp_stage_watch_write` at end of `namespace vm` |
| SPU put guard | `SPUThread.cpp` | `do_dma_transfer` ~2125 |
| ring arm / builders / callback | `cellGcmSys.cpp` | 446, 513-518, `make_command` ~39, `make_jump` ~52, `cellGcmCallback` ~1578 |
| submit log | `sys_rsx.cpp` | `set_rsx_dmactl` 151-210 |
| `nv308a` source ranges | `nv308a.cpp` | `watch_range` in both copy forms (~105, ~326) |
| producer runs | `build/probe/launch-ds24.sh`, `launch-ds25.sh` | census / corrector |
| command-ring announcement | `cellGcmSys.cpp` | 446, 513-518 |
| submit boundary (`put`) | `sys_rsx.cpp` | 151-199 |

---

## 13. How to update this file

- **After every run:** append one entry to §11 with the run directory, the probe
  env, what was measured, and which of F/E/H entries changed. Do not delete the
  old entry; supersede it.
- **When a fact is measured:** add to §4 with the evidence path, not just the
  conclusion. When a hypothesis dies, move it to §5 with the discriminator that
  killed it.
- **At the top of a new session:** update §1 (tree, binary, last run, next
  action) before doing anything else. That is the only section a compacted
  session needs to read.

<!-- doc-end -->
