# Demon's Souls strobe — live investigation

**Status: live document. Read §1, then §6, then §10. Append to §11 as runs
complete; never rewrite a past entry — correct it in a new one.**

This is the canonical context file for the Demon's Souls (BLUS30443) black/white
strobe on the Metal 4 backend. A fresh session can continue from this file alone;
everything else it points at is either in the repository or in `build/probe/`.
The capture mechanics are in `F12-CAPTURE-HISTORY-2026-11-18.md` (companion, not
duplicate: this file is the *why*, that one is the *how*).

---

## 1. Position as of 2026-11-18 (night)

* **Symptom now.** The full-screen black↔white strobe is **gone** (`ds23`:
  51 of 96 sampled presents are `mixed`, and the world renders). What remains
  is a smaller defect of the same family: the lit ground alternates white↔black
  every frame (t4194 vs t4195), the upper half has grey/dark flicker, and the
  world is blurred while the HUD is sharp (left half blurred / right half sharp
  in p4465; whole world blurred in t4195). The user reports the environment is
  visible and the run is "a LOT better". The blur/grey residual is **not yet
  attributed**; the next run's instrumentation targets it (§10, Phase 3).
* **Original symptom (ds21/ds22).** The 3D world alternated between black and
  white, with the correct image visible for milliseconds at a time. Geometry
  and depth were present and stable. The HUD's visibility is *not* a reliable
  class marker (F20). The user reports it as a Metal 4 backend defect.
* **Tree.** `metal4-dev` = `9c8bb94d2` (39 ahead / 0 behind `upstream/master`
  `fb6737b1c`). The RSX FIFO parser is upstream's, byte-for-byte; the branch
  parser plus its 9 tests are gone (`9c8bb94d2`).
* **Built binary.** `build-tests/bin/rpcs3.app` and `rpcs3_test` rebuilt
  2026-11-18 10:33 from the working tree (HEAD + the uncommitted items below),
  carrying the Phase-2 arms and the family-wide literal oracle. `rpcs3_test`
  687/687, 2 disabled. Release scrub clean (213 files deleted, 615 units
  excised, 0 review / 0 residual / 0 orphan).
* **Uncommitted.** (a) `MTLDevice.cpp` `import_guest_window()` uses
  `utils::memory_protect(rw)` instead of `memory_commit` (drops the 4 GiB
  `MADV_WILLNEED`); (b) the four arms in `MTLGSRender.cpp` /
  `MTLInstrumentation.h` (F16); (c) the diagnostic truthiness fix (F17).
  Rebuild + gates pass; (a) can be committed alone, (b)/(c) are `EXCISE`.
* **The strobe is now measured and its mechanism is identified.** `ds21` ran
  164 s with six F12 presses (frames 3008/3512/3834/3983/4038/4191) and produced
  the first pixel tape that contains the defect. The chain, all measured:

  1. The world epilogue is `clamp(h0 * fc[67].x / fc[68].y)` (F18).
  2. `fc[68].y` is the authored literal `00400000` → `2.0`. `fc[67].x` is the
     guest-animated multiplier, and it is the lane the fill delivers wrong:
     the shader fetches the **16-bit-half swap** of the value the guest
     animates (F19).
  3. This is not an edge case. Across the 1085 logged fills of the two world
     programs, the guest animates the multiplier over `[0.315, 2.222]`; the
     delivered f32 is `|v| >= 1e6` in 530 of them, `< 1e-6` in 430, and falls in
     the intended band only 5 times by accident (F22).
  4. Negative-huge → clamp 0 (black); positive-huge → clamp 1 (white); tiny
     (denormal-ish) → quantise to 0 (black). The guest's slow ramp walks the
     transposed sign bit, so multi-frame black runs alternate with 1–2 present
     white flashes (F20, F22). One lane, one transformation, both classes.
* **Producer of the transposed word: still open, but now narrowed to one
  byte-level statement.** The prior handoff's `src == after` result stands: the
  nv308a chunk copies the bytes faithfully, so the reversal is already in the
  guest's source buffer `src=`. The same 16-byte quad carries three correct
  lanes and one reversed lane, so whatever writes the animated lane is not
  whatever writes the static ones. New (this revision, F26): the game's own
  representation of the value is **big-endian** — `3f bd a1 27` (`1.48148` as
  a BE f32) is present 33× in each `frame-*-transform-constants.bin` — while
  the ucode slot holds `bd 3f 27 a1`. Those two differ by exactly `T`: the slot
  contains **`T(W)` stored big-endian** where the ucode format needs `T(W)`
  stored **little-endian** (`T(LE_bytes(W))`). So the producer applied the RSX
  half-word shuffle correctly and then stored the 32-bit word the wrong way
  round: **one missing `bswap32` at the store**, not a copy or format
  conversion error (F27). This is upstream of every backend; it is the
  remaining work, not the mechanism.
* **The constant is causally consumed — proven from the MSL.** `fp-71` (the
  world program at off `21642368`) reads `h0 * fc[67].x` into `_1714` and
  `precision_clamp(r1 / fc[68].y, 0, 1)` into `_1731` (`h0` is the lit colour).
  So the reversed lane is the epilogue multiplier itself, not a dead constant:
  huge → clamp 1 (white), tiny/negative → 0 (black). This closes the old
  handoff's open question 2 and replaces its "unproven" caveat (F28).
* **The Metal-vs-Vulkan question.** Nothing measured in `ds21` is a Metal 4
  semantic difference: the fill, the staging copy and the ucode image are
  shared code (F2). The Metal-only part remains F7 (heap OOB reads, flat
  varyings) and is not implicated by this path. If a VK/GL run shows the same
  world, the defect is shared; that is a prediction, not a test we may run
  (rule in §2).

* **The oracle has now been run (`ds22`), and it proves the mechanism is
  family-wide, not two programs.** With `RSX_FP_LITERAL_FIX=bswap32-all`,
  4031 lanes were corrected (fp 21642368: 580; fp 21442304: 3451), and the
  world draw's tail reads `1.48148` instead of `4.49e-15`. The user reports the
  character silhouette became visible — a real change — **but the black/white
  strobe survived** (140 black / 88 white / 81 dark / 11 mixed presents).
  The reason is in `ds22`'s `literal watch` lines: **49 fragment programs**
  carry a non-uniform trailing pair, and about **30 of them still deliver the
  same stray `[W,2,W,1]` word** (e.g. fp 17/53/40/118/77/155/11/121/26 with
  `ring0` from `1.1e21` to `-5.2e32`). They were outside the oracle because it
  required `ucode_length >= 3000`; the stray word is not limited to the two
  largest programs (F23).
* **The family-wide oracle has been run (`ds23`), and it is the run that
  removes the strobe.** `RSX_FP_LITERAL_FIX=bswap32-all` with the
  `ucode_length >= 3000` gate removed corrected **221 834 lanes across 41
  distinct fragment-program offsets** (the ds22 pair plus the ~30 family
  members: fp 5/8/11/17/20/21/24/26/34/39/40/41/42/48/53/57/63/77/78/86/88/
  99/104/108/118/120/121/134/137/142/145/149/153/155 and their siblings; the
  offsets are listed in §11). The world draw's tail now reaches the shader as
  `1.69832 2 1.69832 1` (ds22 `1.48148`; ds21 `4.49e-15`). `literal watch`
  sees 61 programs; **no remaining `[W,2,·,1]` program delivers an out-of-band
  value at draw time**. The one flagged program, fp 47, is a 2D program whose
  `-0.0777778` is a legitimate constant, not the stray word. Present classes:
  **32 black / 11 dark / 51 mixed / 2 white** (96 sampled presents) vs ds22's
  140 / 81 / 11 / 88 (320). → F24, F25.
* **What is still wrong, measured.** The 2 white presents are *HUD-only* frames
  on a white background: the world did not make it into that present
  (p4461 shows the HUD on pure white, no 3D). The remaining alternation is
  **ground white↔black** with the world otherwise present (t4194 white
  trapezoid with the character's shadow; t4195 the same frame with it black),
  plus a blurred world (p4465: left half blurred / right half sharp). Because
  the trailing-pair oracle is active and its delivered scale is in-band, the
  ground's saturated lit term must come from a lane **outside the trailing
  pair** — the likely shape is the same stray-word producer in a non-trailing
  constant quad, which neither the oracle nor `literal watch` covers. That is
  a hypothesis, not yet evidence (H7 in §6).
* **Next single action:** two runs. (1) `ds24b` — the Phase 3 producer census
  (`RSX_FP_STAGE_WATCH`), with the proven `bswap32-all` oracle still on so the
  picture is stable: it names the layer that stores the byte-reversed staging
  word. `ds24` crashed before the world because the watch's own shadow table
  was written out of bounds (fixed; see the §11 entry), and because the ring was
  armed with `ioAddress == 0` and so was never armed at all (now translated
  through the io page table). (2) `ds25` — the same watch armed as a *corrector*
  (`RSX_FP_STAGE_FIX=stage`) with the oracle **off**: the producer-side
  hypothesis test. Phase 3 is implemented as of this revision: one hook in
  `PPUTranslator::WriteMemory` (plus the two masked vector stores and the
  interpreter/HLE `vm::write`), one each in the SPU's `do_dma_transfer` and the
  HLE command builders, all feeding a shape-filtered `fp_stage_watch` whose
  range is armed from `cellGcmInit` and extended by every `nv308a` source
  range. The shape predicate (F31) is what keeps the log small; the exact quad
  is logged unconditionally so a partial or split producer cannot hide (§10,
  §11 entries F34/F35). Per-layer kill switches (`RSX_FP_STAGE_NO_JIT`,
  `_NO_SPU`, `_NO_VMW`) are wired for bisection.
* **The producer-side experiment (default off, ~50 % prior).** If the animated
  lane is a plain big-endian store of the word the fill will `T`, then the
  bytes are one endianness away from correct *at the producer*, and
  byte-reversing the word in guest memory right after the store should fix the
  picture without touching the fill. `RSX_FP_STAGE_FIX=stage` does exactly
  that (only inside an `nv308a` source range, only on the F31 shape, only when
  two quad siblings are plausible), and logs the original bytes first with
  `fix=1`. A single run with the oracle off decides it: the ground stops
  alternating if the producer is the whole story. If it does not, the log still
  names the writer, and the lane is produced correctly but consumed wrongly
  somewhere else (H7/H8).

---

## 2. Branch, build, gates

| Item | Value |
|---|---|
| Branch | `metal4-dev` |
| HEAD | `9c8bb94d2 metal4: return to upstream's RSX FIFO parser` |
| Parent chain | `5b3870866` (parity + docs) → `2f3a52558` (FP/ROP port) → `0ed64a761` (writable mirror) → `9c8bb94d2` |
| Upstream base | `fb6737b1c` |
| Ahead / behind | 39 / 0 |
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
2. **"Black and white" is not "dark".** The earlier dark-world work
   (`HANDOFF-2026-11-16-ds-world-literal.md`) and this strobe are now known to be
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
| F4 | sRGB packing is exonerated as the root cause. | Parity audit `BEHAVIOR-PARITY-AUDIT-2026-11-17.md`; format mapping matches VK. |
| F5 | Residency / eviction is exonerated. | `METAL4-SEMANTICS-MEASURED-2026-11-17.md`, heap tests. |
| F6 | Pass-transition barriers are covered: `close_encoder()` emits a producer `ALL→ALL` with `VisibilityOptionDevice`. | `MTLCommandRecorder.cpp:1051-1060`. |
| F7 | Measured Metal 4 semantics that differ from Vulkan and matter here: (a) an out-of-bounds heap read silently returns the neighbouring suballocation; (b) a flat `[[user]]` varying comes from the first vertex. | Harness runs #1/#2 in `METAL4-SEMANTICS-MEASURED-2026-11-17.md`. |
| F8 | The FP/ROP precision port (`2f3a52558`) is behaviour-neutral at the default `high` precision: `compareFunctionAlias` resolves to plain compares, and the ROP epilogue and `clamp16` paths now match VK/GL line-for-line. | Commit review; `ea602f81f`, `05a9d9605`, `94dbb7b58`, `906493bc1`. |
| F9 | `supports_denorm_preserve_f32` stays `false` for Metal. At `ultra` this emits the integer-key compare emulation, which is exactly correct — a perf nibble, not a defect. Do not flip without a measurement. | `MTLFragmentProgram.cpp` device_props; `FragmentProgramDecompiler.cpp` `_builtin_sxx`. |
| F10 | The DS evidence words for the world epilogue block are `[a1273fbd, 00400000, a1273fbd, 803f0000]`. | `build/probe/run-20261117-183052-ds20` (`fill.log`, `dumps/`). |
| F11 | (Inference from F10.) Under the 16-bit-half swap `T`, lanes 1/3 decode to `2.0` and `1.0`; lanes 0/2 decode to ~`4.5e-15`, but to ≈`1.4817` under `(raw<<16)|(raw>>16)`. One 16-byte block appears to carry two byte-order conventions. | Decode worked in `DS-SUMMARY-VERIFICATION-2026-11-17.md` (update it if a run contradicts). |
| F12 | The guest continuously patches the ucode tail: `a11e3fbd → a11f3fbd → a1203fbd` (a 1-ulp mantissa ramp). | `watch.log` in `ds20`. |
| F13 | The strobe mechanism, as decoded: the epilogue is `clamp(h0 * fc[67+off].x / fc[68+off].y)`. A word-order flip on the multiplier gives exact black; on the divisor (denormal → FTZ → +inf) gives clamped white. **Superseded by F18-F22:** only the multiplier lane is wrong; the divisor is always the authored `2.0`, and the multiplier's transposed sign/magnitude produces both classes. | Inference from the decompiled MSL + F10; F18-F22 replace the divisor half. |
| F14 | Metal's no-copy guest import covers the sudo mirror (`0x1'0000'0000` bytes from `vm::g_sudo_addr`) and only needs the range `PROT_READ|PROT_WRITE`; `memory_commit` added a pointless 4 GiB `MADV_WILLNEED`. | `MTLDevice.cpp:352-371`; `vm_native.cpp:315-330`. |
| F15 | A default F12 capture is 2 frames wide, not 1 (`capture_active` is a `<= last_frame` watermark), and the ring dump's forward half is `max(6, …)`-by-intent but `capture_frame_count()` in fact — so `p1`+`p2` only. | `MTLGSRender.cpp:350-440`, `MTLConstants.cpp:696-711`; finding 1 in `F12-CAPTURE-HISTORY-2026-11-18.md` §9. |
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

Two defects are known and **not** the strobe, but must not be confused with it:

- `pass_scheduler::split_requested()` is dead: a same-encoder RAW is carried and
  emitted at the next pass. Real, currently masked, unfixed.
- The LLVM `MCJIT` teardown segfault in `Emulator::Quit` is unrelated to rendering.

---

## 6. Open hypotheses

The mechanism is settled (F18-F22); what remains is *why the guest's staging
buffer holds the half-swapped word*. Ordered by what the next run can kill.

| # | Hypothesis | Discriminator |
|---|---|---|
| H1 | **The animated lane is written by a different path than the static lanes.** The quad arrives as one nv308a chunk, but its `src=` range was produced by a writer that stored the correct ucode word `T(W)` big-endian (F26/F27), while the assembler writes the same format little-endian. | Watch the `src=` range of the tail chunk (`watch.log` has `src_bytes`), not just the destination. The producer is one indirection further back than the fill. |
| H3 | **The fill is one patch stale** (the value it reads predates the guest's latest store). | The fill log's `fifo` cursor vs the patch's FIFO position. Low: the delivered value is not a *delayed* correct value, it is a different byte order, which staleness cannot produce. |
| H2 | ~~**The oracle changes the image but the strobe survives → the epilogue re-reads the lane elsewhere.**~~ **Answered by `ds23` (F24):** with every `[W,2,·,1]` family member corrected, the full-screen strobe is gone. The epilogue's trailing pair was the whole full-screen mechanism. | Closed. |
| H7 | **The residual ground/upper-half flicker is a stray lane outside the trailing pair.** The oracle corrects only the two trailing quads and `literal watch` only reads those, so a `[W,2,W,1]`-shaped stray in, say, the lighting or ambient quad of the ground shader would survive and would saturate/zero the lit term without touching the epilogue. F29 bounds it: no whole-quad transposition exists in 4096 fills, but `neither` quads do (max 14), so a two-lane stray in a non-trailing quad is still possible. | Whole-image per-lane census (every constant quad, not the tail), an extended `literal watch` that names the quad/lane that changed, and `bswap32-any` (see §10) as the functional test. |
| H8 | **The blur is introduced after the world draw.** The HUD is always sharp, so it is not the present path's scaling as a whole; the world image is blurred while the HUD overlay is not. Candidates: a half-resolution intermediate sampled by a post pass, a per-half UV/derivative defect, or a stale/ghosted copy. | Dump the world colour target and the present source of the *same* frame (both already exist separately); compare them by region. If the world target is sharp, the post/present pass adds the blur. |
| H9 | **The producer is the per-frame ucode patcher writing through an emulated store that loses a 32-bit byte reversal.** F26 says the word is `T(W)` with the endianness of a plain `stw`; the correct slot needs the `stwbrx` order. Candidates, in order: (a) the guest patcher's own store path is fine and the bytes are written through an RPCS3 helper that swaps at 16-bit granularity; (b) the game's inline FIFO packet is produced by an SPU MFC transfer whose element size is 2 instead of 4; (c) the packet builder applies `T` to a BE value and never reverses. | **Implemented (F34):** the `stage-store` line names the writer (`writer=`, `pc=`, `thread=`, or SPU `tag`/`lsa`) for the exact 16-byte source quad an `nv308a` later copies. **Separate test (F35):** `RSX_FP_STAGE_FIX=stage` with the oracle off; the ground stops alternating iff the producer store is the whole story. |

Hypotheses H3 (two pipelines), H5 (FTZ) and H6 (output-chain amplification)
from the previous revision are in §5. The slot/offset race is not required by
any measurement but was not tested directly; `freeze-fs-offset=0` is the arm
that would test it.

---

## 7. Evidence inventory

| Path | What is in it |
|---|---|
| `build/probe/run-20261118-103456-ds23/` | **The run that removes the strobe (family-wide oracle).** Five F12 presses at frames 3384/3927/4209/4389/4465. `fix.log`: 221 834 corrected lanes across 40 distinct fp offsets; `mtl-probe.log`: 61-program `literal watch`, `world tail` in-band (`1.69832 2 1.69832 1`), `tape class` 32 black / 11 dark / 51 mixed / 2 white; `fill.log`/`watch.log` present. `png/` holds decoded present frames (`p4461` HUD-only white, `p4465` lit world, `t4194`/`t4195` white↔black ground). |
| `build/probe/run-20261118-102603-ds22/` | **The first oracle run.** Four F12 presses (frames 4260/4688/4765/4915). `fix.log`: 4031 corrected lanes across the two large world programs; `mtl-probe.log` carries the 49-program `literal watch` census (F23) and the `tape class` timeline (still 140 black / 88 white / 81 dark / 11 mixed); `fill.log`/`watch.log` as in ds21. Present images, `fp-*`, manifests and fragment constants kept; the per-texture dumps were pruned for space. |
| `build/probe/run-20261118-095414-ds21/` | **The run that measured the strobe.** Six F12 presses at frames 3008/3512/3834/3983/4038/4191; 80 present-stream images (F20), `fill.log` with 1331 fill records (1085 of them the two world programs, F19/F22), `watch.log` with the 308 nv308a chunk records, `mtl-probe.log` (world draws, feedback witness, tape timeline), `dumps/` (`fp-70`/`fp-63` ucode+glsl+const, tape slots, ring snapshots). The per-texture dumps were pruned; logs and timeline kept. |
| `build/probe/run-20261117-183052-ds20/` | Last full DS capture: `dumps/` (host images, tape slots, ring snapshots), `fill.log` (fragment-constant fills, F10), `mtl-probe.log` (level-5 text), `watch.log` (guest ucode patches, F12). |
| `docs/research/metal4/design/DS-SUMMARY-VERIFICATION-2026-11-17.md` | The decode of F10/F11 and the verification of the earlier summary claims. |
| `docs/research/metal4/design/METAL4-SEMANTICS-MEASURED-2026-11-17.md` | Harness #1/#2: heap OOB, flat varying (F7). |
| `docs/research/metal4/design/BEHAVIOR-PARITY-AUDIT-2026-11-17.md` | The Metal-vs-Vulkan audit; F4/F5 rest on it. |
| `docs/research/metal4/design/FIX-PLAN-CONFIRMED-GAPS-2026-11-17.md` | The 15 landed parity fixes and the remaining list. |
| `docs/research/metal4/design/F12-CAPTURE-HISTORY-2026-11-18.md` | What F12 writes, in order, with line numbers and budgets. |
| `docs/research/metal4/design/HANDOFF-2026-11-16-ds-world-literal.md` | The dark-world thread (H1's ancestor). |
| `docs/research/metal4/design/METAL4-FIFO-PARK-HANDOFF-2026-11-10.md` | FIFO history; now historical (F1). |

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
order, budgets and filenames: `F12-CAPTURE-HISTORY-2026-11-18.md`.

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
2. **Blurred world, sharp HUD** (F25). The blur is a spatial/post-pass question,
   not a constant value (H8).

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
  of its left/right and top/bottom halves. The world colour target and the
  present of the same frame are already both on the tape and at the same
  resolution, so a sharp `sample` next to a flat `present` attributes the blur
  to the pass between them; both flat means the world draw itself is soft.
  CPU-only on bytes the flush already made visible.

Run: one F12 press with the ground in view (the white↔black alternation is
visible to the user before any press), then two more during the grey flicker.
Score: (`stray`/`bswap32-any`) does the ground stop alternating; (blur dumps)
which pass first contains the blur.

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

## 11. Running log

Newest first. One entry per run or code change: what was done, what was measured,
what it changes in §4/§5/§6.

### 2026-11-18 — `ds24`: crashed before the world; the crash was the watch's own shadow table

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

- Wrote `F12-CAPTURE-HISTORY-2026-11-18.md`: the full trigger → arming → dumps →
  freeze → window close → artefacts path, with the env switches and budgets.
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

See `HANDOFF-2026-11-16-ds-world-literal.md` (dark world, literal block),
`INVESTIGATION-2026-10-22.md` (Ridge Racer; the probe facility and method), and
`METAL4-FIFO-PARK-HANDOFF-2026-11-10.md` (FIFO; now historical).

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
| fragment-constant fill | `MTLGSRender.cpp` | 6100-6145 |
| `fs_constants_offset` latch | same | 6109, 6348, 6612 |
| `fc[]` declaration | `MTLFragmentProgram.cpp` | 258 |
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
