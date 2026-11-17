# Verification of the DS "state summary" against ds20 raw evidence — 2026-11-17

The user pasted a summary of the DS investigation and said "I don't trust it."
This doc records which claims I verified against the raw ds20
(`build/probe/run-20261117-172400-ds20`) logs/dumps and which are wrong.
Read-only research.

## Verified TRUE

1. **Shader epilogue is a single scalar multiply.**
   `dumps/fp-64.metal` ends with exactly:
   ```
   float3 _1805 = (h0 * _229.fc[67u + draw_params_payload.x].xxxx).xyz;
   float4 param_57 = r0 / float4(_229.fc[68u + draw_params_payload.x].y);
   float4 _1824 = precision_clamp(param_57, 0.0, 1.0);
   ```
   Geometry/depth/rasterise/present are not in this path. CONFIRMED.

2. **Trailing literal layout is `[E, 2.0, E, 1.0]` and the 2.0/1.0 lanes are correct.**
   `dumps/fp-64.const.txt`: `[67] @3312 a1273fbd 00400000 a1273fbd 803f0000`.
   `T(0x00400000)=2.0`, `T(0x803f0000)=1.0`, `T(0xa1273fbd)=4.489e-15`. CONFIRMED.

3. **Frame-by-frame pixel predictions are exact.** From `mtl-probe.log`
   `host image present N, row 0 ...`:
   | frame | summary | actual row 0 |
   |---|---|---|
   | 4240 | black | `ff000000` ✓ |
   | 4242 | white | `ffffffff` ✓ |
   | 4244 | white | `ffffffff` ✓ |
   | 4246 | black | `ff000000` ✓ |
   | 4248 | black | `ff000000` ✓ |
   | 4254 | white | `ffffffff` ✓ |
   | 4268 | dim (2,2,2) | `ff020202` ✓ |
   | 4282 | dim (12,12,12) | `ff0c0c0c` ✓ |
   This is the strongest evidence in the summary and it is fully verified.

4. **Guest copy is byte-faithful.** `watch.log` ring writes at the trailing
   literal (`dst=71c14a4940` ×1313, `dst=71c14a4960` ×1314) all show
   `dst_bytes == src_bytes`, e.g. both `a1273fbd 00400000 a1273fbd 803f0000`.
   The source pointers are `0x79f6526xxx` — host heap (`fifo_span`), not guest RAM.
   CONFIRMED: no writer in the instrumented path reorders the E word.

5. **The code site is the shared fill.** `ProgramStateCache.cpp:644-706` applies
   T() to every constant offset. CONFIRMED.

## Verified WRONG / MISLEADING in the summary

1. **"E is a monotone ramp 2.150→2.221 across 40+ programs."**
   FALSE as a single ramp. The corrected E values from `fill.log` form **three
   clusters**: 1.48 (280 lines), 2.22 (265), 0.99 (140), plus a smaller
   2.17–2.21 ramp. The 0.99 cluster is a *different program* (word pattern
   `e8323f7c`/`e6593f7c` → corrected 0.988), not the same E. The story "one
   smooth ramp" is an oversimplification; there are at least two distinct
   guest-authored constants being patched into the same trailing slots.

2. **"the transposition is already in the guest's staging block; no Metal copy
   reorders it."** Half-true. The bytes are in the FIFO source (`fifo_span`,
   host heap). But WHO put `a1273fbd` (big-endian form of the correct RSX word
   `0xbd3f27a1`) into the FIFO is NOT established. The summary's earlier
   framing that "the guest is byte-faithful" conflates *FIFO→local-memory copy*
   (faithful, measured) with the *producer of the FIFO words* (unmeasured).

3. **"none of the parity findings explain DS."** CONFIRMED but incomplete —
   none of the 15 parity gaps touch this path, which actually *supports* the
   conclusion that this is not a VK/GL-vs-MTL rendering difference.

## What is genuinely NOT closed (summary is honest about these)

- **How hardware distinguishes the patched E lanes from authored literals in
  the same 16-byte block.** This is the crux of layer ownership. On real RSX,
  the trailing block `[E,2,E,1]` must decode E via a different convention than
  the authored 2.0/1.0. The mechanism (a per-constant "is patched" flag, a
  different constant bank, a `fc` vs `c` addressing mode) is unknown.
- **Visual confirmation of a functional fix.** None applied.
- The "previous DS screen" ghost is a separate stale-surface artefact.

## Four-subagent verdicts (this session)

- **H1 (Metal4 GPU-blit/write-back):** no Metal4-only path found that writes the
  reversed word; the GPU-blit `note("nv3089-gpu")` early-return gap (finding 9b)
  is the only uninstrumented route, but ds20's trailing-literal writes are all
  `nv308a`, byte-faithful.
- **H2 (dump forensics):** the world-program dump files in `dumps/` are texture
  uploads, not program images; no `conv-*.bin` matches the program address. The
  program-image bytes are only in `watch.log` ring records.
- **H3 (shader/interpreter byte transform):** Metal4's constant path is
  byte-identical to VK/GL — no Metal-only decode. Corroborates the shared-fill
  conclusion.
- **H4 (git history):** no Metal4 commit introduces or fixes a fragment-literal
  byte reversal. The two commits that rewrote the byte-moving paths are
  `8ef12357f` (GPU blit + shared nv3089/nv308a/nv0039 rewrite) and `281a806aa`
  (write-back/upload rewrite). Upstream PR #12450 shows a *different* wrong
  decoded scalar blacking the DS world (scaling_table/SNORM16), not byte order.

## H1 final report — the decisive no-interleave evidence

H1 (GPU-blit/write-back audit) returned with the strongest single argument
against a Metal4-specific writer:
- The watch hooks only `nv308a`/`nv3089`/`nv0039`. An uninstrumented Metal4
  byteswap write to the literal would **break the smooth `before`/`after` chain**
  between consecutive writes. Instead it advances cleanly
  `a11e3fbd → a11f3fbd → a1203fbd` — nothing interleaves. There is no hidden
  Metal4 write touching this address.
- Every one of the 22,802 writes to the literal is `writer=nv308a`, `src==dst`.
- The only full-32-bit-byteswap write in all of Metal4 is `MTLGSRender.cpp:7923`
  in `release_GCM_label` (GCM label address, via `recorder.write_label`) — not a
  program literal. `encode_r6g5b5_plane` (`MTLFormats.cpp:539`) has a u16 swap
  but is R6G5B5-texture-only. `supports_byteswap=false` at `MTLTextureCache.cpp:132`.
- The GPU blit (`textures.blit`/`MTLBlit.cpp`) is texture↔texture only and
  **never writes guest RAM**; on decline the shared nv3089 CPU fallback does a
  byte-exact memcpy.
- H1's proposed site: `nv308a.cpp:79-135` raw-copies FIFO 32-bit pixel data
  **without the endianness conversion real NV308A applies**, while the Cg upload
  path (`CgBinaryProgram.cpp:217-221`) does the `be_t<u32>` swap. This is shared
  code (affects all backends), not Metal4.

## Bottom line

The pasted summary's **core causal chain is verified** (E-byte-order → fill
produces pseudo-random scalar → clamp → black/white, with exact per-frame pixel
matches). Its **"single smooth ramp"** and **"guest staging is the whole story"**
framing are wrong. The genuinely open question is not *what* value reaches the
shader or *which* fill transforms it — it is **why the same 16-byte block has two
byte-order conventions, and which layer in RSX/hardware is supposed to tell them
apart**. Answering that requires knowing the guest's constant-patch mechanism,
which none of the instrumentation captured.
