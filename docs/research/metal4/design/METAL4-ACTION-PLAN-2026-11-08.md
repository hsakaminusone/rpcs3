# Metal 4 backend — line-by-line audit and action plan (2026-11-08)

Provenance: first-pass line reading by the maintainer this session (file:line below), plus two
background read-only audits dispatched for the pipeline-materiality and pass/hazard axes (results
to be appended when they return). No code was changed for this audit; no device was touched.
The goal, per the maintainer, is a host-of-issues closure plan, not a single-defect fix.

--------------------------------------------------------------------------------
Part 1 — what was checked, line by line, and what the code actually does
--------------------------------------------------------------------------------

### PSO materialization and cache (MTLProgramPipeline.cpp / MTLPipeline.cpp / ProgramStateCache.h)

- `m_storage` lookups match on **hash AND `operator==`** (ProgramStateCache.h:371-389, MTLPipeline.cpp:26-48). The whole-struct `hash_struct(pipeline_properties)` (fnv_hash.hpp hash_struct_base over the raw bytes) therefore produces a **duplicate link job** whenever the padding holes before/after `u8 attachment_count`, `bool` fields and the rule-of-4 alignment hold garbage — it can **never produce a wrong pipeline reuse**, because equality is checked after the hash. Verdict: **not a correctness defect, a duplicate-PSO leak** → fix P0-1.
- `same_blend_state` / `operator==` exclude `alpha_ref`/`alpha_func` (correct: runtime uniforms) but include `alpha_test_enabled`, `logic_op_enabled`, `logic_operation`, `shade_mode` — consistent; no field of the PSO lies about the descriptor built from it (`apply_pipeline_properties` mirrors the same clamp).
- The draw site (MTLGSRender.cpp:3452-3580) covers first-use misses with the interpreter; if neither interpreter nor a real pipeline is ready, **the draw is counted and skipped** (`yield` + `draw/skipped pipeline failed`); there is **no code path that binds a stale older-pipeline object** for a new state, and no path binds an unfinished descriptor. The "older allocator epoch (0x7753e22000)" fingerprint reported by the dump analysis is fully explained by "the fresh job compiled for the unique-rare state this frame", which is the *correct* behavior. Verdict: **suspect retired**.
- The link pipeline is sound on failure: jobs publish; failed ones land in `m_failures`; permanent-failure branch just skips the draw. Not a "black fill" producer by itself. Verdict: `permanently_failed` path is acceptable by design; the interpreter cover exists only when enabled.
- Bar on "hash collision pin-pointage": A 64-bit FNV collision is possible in principle; equality is checked afterwards, so no wrong reuse — only theoretical double-link. Drop from the list.

### ROP alpha-test / shader epilogue (MTLFragmentProgram.cpp, RSXROPEpilogue.glsl, RSXProgramCommon.glsl)

- The alpha test runs **inside the fragment program**, not fixed function; fp-119/fp-45-class carry `comparison_passes(col0.a, alpha_ref, alpha_func)` with `alpha_func = bits[12:14] of the runtime `rop_control` uniform and `alpha_ref` the sibling float.
- CMP_FIXUP is f32-exact; the boundary-equality texel is a measure-zero case and cannot produce persistent flat plates. The mechanism that produces a *flat* fill is uniform-wide: one fragment state entry feeding every draw of a class.
- Handled correctly on the quantized path (round_to_8bit before the compare when rounding is enabled) — the same ordering VK emits internally through the fixed-function test; only the texel/ref comparison differs (f32 col0.a vs 8-bit ref), which flips only exact-equal texels. The trunk-vs-6150 byte A/B (L-rows / FINDINGS) showed identical rop/blend class in both windows, which retires the alpha-test theory **for the trunk instance specifically** at current evidence; the *class* stays resident with the new `no-alpha-test` confirmation run (P1-B).

### Texture upload (MTLTextureCache.cpp, common texture_cache.h / TextureUtils.h)

- `subresource_layout::data` is a mapped guest view (vm::_ptr) — the upload converts bytes **in the same RSX FIFO decode call** that added the range (synchronous CPU work inside `upload_texture_data` at MTLTextureCache.cpp:83). There is **no stale-CPU-copy window** anywhere on this call path; it cannot read "a fill that hasn't happened yet" from the CPU side.
- The **copy-back class** is where ghost zeros can exist: a surface the GPU renders into exists only in device memory; a later `upload_texture_data` for the same guest range needs that content back on the CPU side, which must go through an encoded blit + **a completed fence read wait** (`copy_texture_to_buffer` → `flush_for_readback`). If that fence is missing/async, the CPU reads stale bytes. That path serves the livery/paint-family class.
- The renderer's live `poison` arms do exist; fresh textures fill with an address-hashed word (MTLTextureCache.h:536/683, MTLResourceManager.cpp:335), fresh shared buffers with an index-hashed word. Therefore P1-A is decisive:
  · zero channels stay zero ⇒ the upload *content* was legitimately zero from the guest (or the copy-back ran before fence);
  · channels show the address-hashed word ⇒ the region was never filled in via the upload path that run (allocation-garbage leak).
- The `0xc679f780` zero-atlas anomaly (ZERO on frames 1557/1561/1715-1719, FULL elsewhere, `staging == guest`) is **not** yet classified as guest-zero vs back-end race; only P1-A separates them (the dump contains no discriminating bytes — FINDINGS verdict already says this).

### The compose / present / pass path

- Not line-audited yet at file:line in this pass (outside this audit's scope so far; covered by the background pass/hazard audit). Known facts: `host-shot` == `host-present` byte-for-byte in all captured frames; resolve order identical in fine/failing; nothing in compose can introduce plates absent in the scene render.

--------------------------------------------------------------------------------
Part 2 — interacting symptoms and their likely shared root
--------------------------------------------------------------------------------

The capture class spans: (a) tunnel FIERA plate 1789 = glow-paint draws under a blend descriptor unique to the frame;
(b) trunk livery plate 6189 = flat (8,8,8,255), texture bytes identical both windows; (c) 1158800 full-background black
scenes; (d) the 0xc679f780 atlas-zero-flicker anomaly with five consecutive frames; (e) menu glyph loss / black minimap
reported historically (1076xxx logs).

The common shape is **"a guest-visible content taken at the wrong time"** rather than "one wrong math table entry":
the backend never decodes RSX registers differently than VK/GL in ranges they agree on (state tables audited this
session — all 1:1), and the common delta Metal02 carries is **ordering** and **allocator/recycling timing**.
The pass/hazard audit must pin (d): fence completion before any CPU read of device-materialised content.

--------------------------------------------------------------------------------
Part 3 — action plan
--------------------------------------------------------------------------------

### P0 — do now, zero device, zero knob surface change

P0-1. MTLProgramPipeline.h `key_hash`: replace the whole-struct `hash_struct` with the field-wise hash VK uses
   (vertex_id ^ fragment_id ^ a small field-wise mix of `pipeline_properties`). Kills the duplicate link jobs every release;
   zero behavioural change otherwise; keeps MTLKey equality unchanged. Placeholder PSO churn is the only known cost being paid today.

### P1 — device work, needs the user's explicit go; ordered by information density

P1-A (the decider). Run with `MTL_PROBE=5 MTL_PROBE_FRAME_COUNT=1 … MTL_PROBE_VARIANT=poison,dump-tex=0xc1a00000`,
   one F12 in the tunnel. Decisions it buys:
   · never-uploaded / BC-zero census at window close prints free of charge (L200 path);
   · `dump-tex` byte pair (guest + converted) for the glow scratch is captured at every upload and every sample;
   · `poison` classifies zero regions in both, in one line each;
   · closes or orders the upload path vs copy-back path hypothesis without interpretation disputes.
   Do **only** after the disk call (13 GB pack prune) is made.
P1-B (trunk confirmation). Same setup, arm `no-alpha-test`, one F12 at the trunk window. If the trunk plate remains:
   the epilogue owns nothing for that instance (a real falsification, not a vibe). If it's gone: the alpha-discard epilogue
   is confirmed for the trunk, and the fix is one conditional, not one ucode patch.
P1-C (already free). The window-close census from any P1 run prints the `hazard` tally (a unit sampling its own
   bound target). No run required beyond the P1 runs themselves; if it names 0x50110000-class identity splits, the
   trunk's broken link is confirmed without a further launch.

### P2 — depends on P1 verdicts (committed only after P1-B/A returns evidence)

P2-A. If P1-A shows ghost zeros **introduced by the backend** (poison word absent, guest region demonstrably healthy):
   the copy-back path needs a completed fence before any synchronous CPU read of the mapped range, or it needs to refuse
   the read until it holds one. Patch in MTLTextureCache.h's copy_texture_to_buffer readback (find the flush site,
   upgrade to a real `waitUntilCompleted` on the timeline that encoded it, or reroute through a staging buffer with its own
   channel). Also add ONE level-3 probe to publish the wait stall that the fix removes. The small `0xc679f780` anomaly
   closes or confirms by the same mechanism.
P2-B. If P1-A shows content was already zero in guest memory in those frames, the backend is faithful for that case;
   the poison verdict for the `0xc1a00000` scratch decides the tunnel attribution end-to-end with no new code.
P2-C. The duplicate-PSO fix (P0-1) additionally serves as groundwork for lowering link lag that the rare-state windows show.
P2-D. Log-only items from this audit (no code change): none beyond L-rows already kept current.

### P3 — hygiene, cheap, can ride any build

P3-1. `cmd.get()`-style audit of PNG-pixel handoff paths worth a second pass is *out of scope* until P1-B/A lands.
P3-2. Keep the seven-knob surface. Nothing in Parts 1-3 needs a new env var; the poison dump-tex arm IS an existing knob.

--------------------------------------------------------------------------------
Part 4 — explicit non-goals (to avoid churning green code for no signal)
--------------------------------------------------------------------------------

- No blend/factor table edits, no ROP math edits, no PSO-key identity changes beyond P0-1.
- No fixes inferred from a single clean/failing frame pair; every falsification decision lands via P1.
- No deleting the failing-frame evidence; no devs may prune `dumps-final` until P1 lands (byte reproducibility).
- No new knobs; no MTL_PROBE_AUTO_CAPTURE/ARM_TEXTURE/INTERVAL re-adding; no `MTL_PROBE_PIXELS` changes.
- The trunk instance is not patched by this plan until P1-B returns evidence class.

--------------------------------------------------------------------------------
Part 5 — reading order and exit criteria
--------------------------------------------------------------------------------

1. P0-1 lands → build green (560/560) → docs touched only to note L-ids as usual.
2. P1-A/B/C run → decode coverage → FINDINGS.md append → the verdict's missing fact gets its name.
3. P2 lands (bounded, ≤2 files) → build green → one P1 re-run closes the loop (evidence goes in the next window capture).
4. L-ids and docs in LOG-FINDINGS.md, §1.6/§1.9 and the §3.1 table are updated at every landed step, not retroactively.
