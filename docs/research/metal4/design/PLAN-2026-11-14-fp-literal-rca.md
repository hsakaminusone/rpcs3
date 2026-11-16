# Plan — RCA of the Demon's Souls world epilogue (2026-11-14)

Status: **plan open, probes P1–P15 below.**  Supersedes the informal thread in
L248.  The object under investigation is one 4-byte number, not the Metal
backend: the `.x` lane of the trailing inline literal of the world fragment
programs.

The rule this plan is built on:

> A value that renders the whole 3D scene must be explainable by RSX semantics.
> Until it is, every layer between the guest's store and the GPU's `fc[]` load is
> a suspect with a named probe that can clear or convict it.

## 0. Facts, with the measurement that produced them

| # | Fact | Evidence |
|---|---|---|
| F1 | The world programs end `MUL R0.xyz = \|h0.xyz\| * CONST0.xxxx`, `DIV H0.xyz.sat = \|R0.xyz\| / CONST0.yyyy`, then a `MOV` that stores `h0.w`. Net colour `sat(\|h0\| * X / 2)`. | `dec.py` over `fp-47.ucode.bin` (ds5) at 3296/3328; MSL confirms `float3 _1805 = (h0 * _229.fc[67u + …].xxxx).xyz;` then `param_57 = r0 / float4(_229.fc[68u + …].y);` then `clamp(…, 0, 1)`. |
| F2 | Both instructions read the *same* inline literal, emitted twice, 32 bytes apart: `(X, 2.0, X, 1.0)`. `.y` and `.w` never vary; `.x` and `.z` are always equal to each other and always vary together. | ds5 `fp-47.const.txt` [67] offset 3312 / [68] offset 3344; ds7 dumps for fp 58/60/77. |
| F3 | Sibling programs that do the same thing hold `(2.0, 2.0, 2.0, 1.0)` at the same slot, i.e. the authored value of `.x` is `2.0`. | ds5 `fp-9/14/19/35/40/42/43/44/94/97.const.txt` (grep for ` 00400000 803f0000$`). |
| F4 | With the literal's `.x` forced to `2.0` (`MTL_PROBE_VARIANT=epilogue-scale`) the world renders (user: "better but still glitchy"). Nothing else changes. | ds6 run, user observation; the variant only rewrites `constants[count-2].v[0]` and `.v[2]`. |
| F5 | Instruction bytes are identical between the compile-time and the draw-time image for **all** programs; every difference is inside an inline-literal block. | ds7: 48 programs dumped, 200 differing 4-byte words, **200/200** inside DQ literal blocks (script over `fp-*-live.ucode.bin`). |
| F6 | `X` changes between compile and draw for the same program, and the values are junk: `a1273fbd` (4.489e-15) → `a1123fbd` (1.021e-27) for fp 58; `52ed3fa7` (−4.07e27) → `a1273fbd` for fp 77. | ds7 `ucode image` lines + `fp-*-live.ucode.bin`. |
| F7 | The live ucode addresses are 21–22 MB (`21484160`, `22531456`, …), i.e. XDR in the game's own heap, not RSX local memory. | ds7 `ucode image` lines, `live offset`. |
| F8 | The fill is shared code (`write_fragment_constants_to_buffer`); Metal is a faithful port of the VK path. The only Metal-side difference found so far is MSL materialising unsized arrays (`fc[1]`, `texture_parameters[1]`). | `ProgramStateCache.cpp:638-700`, `MTLGSRender.cpp:5448`, ds5 `.metal`/`.glsl` dumps. |
| F9 | The epilogue *is* reached with the ring value at the right index: patching one CPU-side quad changes the image. | F4, plus ds5 source-probe lines showing `ring[…] == shuffle(guest[…])`. |

F3 + F4 are the strongest pair: the game authors `2.0` in that lane elsewhere,
and `2.0` renders the scene.  So `X` is *wrong at the moment it is read*; the
only open question is **which layer supplies the wrong bytes**.

## 1. Hypothesis tree

Each node has exactly one probe that can kill it.  A hypothesis that survives
must explain F2 (lane pairing), F5 (instructions stable) and F6 (value drifts).

| id | hypothesis | killed by |
|---|---|---|
| **H1** | The guest computes `X` and writes junk (upstream PPU/SPU mis-emulation). | P3 (a writer is observed, with a PC), P7 (VK reads the same bytes and is also black). |
| **H2** | The guest's buffer is transient; the emulator reads it later than hardware would. The FIFO-time image is sane. | P1 (prefetch image), P5 (3-way timeline). |
| **H3** | `X` is authored junk on purpose and hardware reads a different lane / order (our DQ decode or swizzle is wrong). | P10 (a program must exist that reads `.x` where `.x` is sane and distinct from `.y`), P7 (VK correct with the same `X`). |
| **H4** | The ring index is off (`draw_params_payload.x` base), so `.x` and `.y` come from different quads than the compile-time offset map says. | P4/P12 (the filled quads, and `fc[k].y == 2.0` consistency), P7. |
| **H5** | `constant_offsets` come from a different image than the fill reads (offset shift of a few words). | P4 (offsets at compile vs words at draw), F5 (offset shift would show up as differing instruction words). |
| **H6** | MSL declares `fc[1]` but indexes `fc[67]`; the Metal compiler is free to treat the load as out of bounds (0 or poison) and only *some* loads degrade. | P11 (declaration audit + raised bound experiment). VK/GL are unaffected by construction. |

H5 is already weak (F5), H3 is contradicted by F3 unless the decode is wrong in
a way that only shows on non-uniform quads (hence P10).

## 2. Probe catalogue

Conventions: every probe is default-off, bounded (a fixed maximum number of
lines or files), carries `TODO(excise)` and sits inside `EXCISE-BEGIN/END
<id>`.  Nothing is gated behind `diagnostic_capture_active()` unless it is
already a capture probe, because the defect reproduces without an F12 capture.

| id | probe | where | output | decides |
|---|---|---|---|---|
| **P1** | ucode image at FIFO time: raw `SET_SHADER_PROGRAM` value, decoded location, `vm::base`, offset, length, FNV-1a hash, last 16 words. | `RSXGraphEngine.cpp::prefetch_fragment_program` | `$MTL_PROBE_PREFETCH_LOG`, ≤128 lines | H2. If the prefetch tail has `2.0` in `.x`, the read is late; if it is already junk, the guest wrote junk or the buffer is stale at source. |
| **P2** | per-draw literal watch: for every program whose `constant_offsets` map a DQ literal, log the last two quads at **every** draw, with time, frame, draw id, `get_data()` pointer, and whether the value changed since the previous draw. | `MTLGSRender.cpp` after the fill | `MTL_NOTE(5)` ≤ 4096 lines | H2/H1 timing: a value that changes within a frame is written by someone we can attribute; a value constant within the frame is set once per upload. |
| **P3** | write attribution: when a watched literal changes, log the RSX-side observation point (draw id, `m_ctx->rsx_event_count`/subframe), and arm a page-level write watch on the containing 4 KB page so the guest store that changes it is caught with its PC/thread. | `MTLGSRender.cpp` + `vm` page fault path | `$MTL_PROBE_WRITE_LOG` | H1: names the writer. |
| **P4** | the ring, not the guest: log the exact `constants[]` quads the shader will read (`fs_constants_offset + count-2/-1`), beside the guest words they were built from. | `MTLGSRender.cpp` after the fill | `MTL_NOTE(5)` | H4/H5. |
| **P5** | 3-way image timeline: dump `fp-<id>-<seq>.ucode.bin` at prefetch, at compile, and at draw for the first N programs, so the *same* program can be diffed at three times in one run. | prefetch + `MTLProgramPipeline.cpp` + draw | dump directory, ≤48 programs | H2 with bytes, not a hash. |
| **P6** | epilogue trace: for the world programs, log `h0.xyz` before the epilogue is not observable on the CPU, so instead log the *input* to the fill (the literal) and the *output* after the draw via the pixel probe, at the same draw id. | pixel probe + P2 | existing probe files | Correlates the literal timeline with the framebuffer. |
| **P7** | cross-backend: the *shared* fill logs `offset`, `ucode_length`, source hash, count and the last four quads for every fill, backend-agnostic.  Run the same scene on VK/GL and diff. | `ProgramStateCache.cpp::write_fragment_constants_to_buffer` | `$RSX_FP_LITERAL_LOG`, ≤256 lines | H1/H3/H4: if VK is order-identical and renders, the fault is Metal-side; if VK reads the same tiny value, the fault is shared and upstream of the backend. |
| **P8** | screenshot differential: VK and Metal screenshots of the same spot, compared programmatically (mean luma in the world region). | harness | PNG | Scope: is this Metal-only? |
| **P9** | first-value history: for each watched literal, the first 32 *distinct* values with timestamps, so "was it ever 2.0?" is answered without a second run. | `MTLGSRender.cpp` | `MTL_NOTE(5)` | H1/H2. |
| **P10** | offline decode audit: for every DQ literal in every ds7 dump, tabulate the quad, whether the reading instruction uses `.x`, and whether the value is sane.  Find a program whose `.x` is sane, distinct, and used. | offline script | table | H3. |
| **P11** | MSL declaration audit: every `[1]` array in the generated MSL that is indexed dynamically; then the raised-bound experiment (declare the real ring size) as a *fix candidate*. | `MTLProgramPipeline.cpp` (MSL preamble) | generated MSL + rebuild | H6. |
| **P12** | ring layout: dump the whole `fc[]` block for one armed draw (bounded to 4 quads before/after each mapped literal) to see whether the quad *around* the literal is the compile-time array. | `MTLGSRender.cpp` | `MTL_NOTE(5)` | H4. |
| **P13** | FIFO/capture replay: rsx capture around the draw, replay on VK. | harness | `.cap` + VK replay | last resort for H1/H3. |
| **P14** | generalisation: scan other titles' dumps for `(X, 2, X, 1)` with tiny `X` to see whether this is a Demon's Souls-specific trick or a class of programs. | offline over existing dumps | table | Design of the fix (general or quirk). |
| **P15** | provenance: raw `NV4097_SET_SHADER_PROGRAM` word, decoded `location`, and whether the address is XDR or local, on every prefetch. | prefetch (same line as P1) | P1 file | Confirms F7 and rules out local-memory aliasing. |

## 3. Run protocol

1. One Metal run with `MTL_PROBE=5`, `MTL_PROBE_VARIANT=epilogue-scale`,
   `MTL_PROBE_PREFETCH_LOG`, `RSX_FP_LITERAL_LOG`, `MTL_PROBE_WRITE_LOG`,
   `MTL_PROBE_PIXELS`, `RSX_SURFACE_DIAG=1`.  Everything is bounded, so one run
   answers P1/P2/P4/P5/P6/P9/P15.
2. One VK run of the same spot with `RSX_FP_LITERAL_LOG` and a screenshot
   (P7/P8).
3. Offline: P10/P11/P14 over the dumps of both runs.

## 4. Decision table

| observation | conclusion | next action |
|---|---|---|
| P1 prefetch `.x` = `2.0`, later reads junk | H2: the read is late | Snapshot the ucode at prefetch (`clone_data()` into an RSX-owned buffer) and fill from the snapshot; verify with P2. |
| P1 prefetch `.x` already junk, and P3 names a guest store | H1: guest data | Find why the guest computes it: trace the store's source; if it is a fixed-point/float reinterpretation, fix that path. |
| P1 prefetch `.x` junk, no writer ever observed | H1' : the value is stale memory at the source, or the guest never writes it | Identify the previous tenant of those 8 bytes (offset in a known upload) and make the emulator supply the canonical `2.0` from the *compile-time* image only when the DQ slot was never written. |
| P7 VK reads `2.0` for the same draw | Metal-side supply bug | Diff the two fills (index, base, buffer contents); fix Metal. |
| P7 VK reads the same `X` and renders | H3/H6 | Audit the decode (P10) and the MSL bounds (P11); test raised-bound build. |
| P7 VK is also black | shared/upstream bug | Escalate to a shared fix; Metal release can proceed only with the shared fix in. |

## 5. Deep dives (each must end in a written finding)

- **D1 — DQ literal semantics.**  Re-derive the inline-literal layout from the
  encoder (`RSXFragmentProgram.h` bitfields, `FPToCFG.cpp` `addr + 16`), the
  decoder, and a round-trip through `rsx::assembler`; check the repo's hwtests
  for a program that reads non-uniform lanes.  Outcome: a statement of which
  lane hardware reads for `CONST0.x`, with a test that proves it.
- **D2 — ucode lifetime.**  Who owns the XDR buffer at `SET_SHADER_PROGRAM`
  time, how long the guest keeps it alive, and what hardware guarantees.  If
  hardware latches at FIFO time, the emulator must latch; if hardware reads at
  execution time, the guest must keep it alive and the value is guest data.
- **D3 — cross-backend differential.**  Same draw, three backends, same
  numbers.  Anything that differs is a Metal deviation and gets a DEVIATIONS
  entry.
- **D4 — ring addressing.**  `draw_params_payload.x` semantics, `fc[]` bound,
  and the alignment of `constant_offsets` quads to the ring.  Outcome: the
  binding contract is either proven or amended (HLD-05).
- **D5 — the remaining "still glitchy".**  With `X` corrected, capture a
  screenshot and enumerate what is wrong, then map each artefact to a probe
  (that may be a second, independent defect).

## 6. Acceptance

- The world renders with **no variant** and the literal left at its guest value,
  *or* the literal is proven guest data and the real defect is fixed elsewhere.
- One written RCA in `LOG-FINDINGS.md` (L249+) naming the layer, the mechanism,
  the general fix, and the evidence, with no open question left in the chain.
- `rpcs3_test` 687/687, scrub `check` clean, scrubbed tree builds.
- Probes removed or kept only where the EXCISE policy allows.
