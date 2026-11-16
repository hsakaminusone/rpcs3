# Upstream research — DS world-literal / black-white flicker (2026-11-16)

Question: has the Demon's Souls "world renders black/white" bug, or the
fragment-literal byte-order mechanism we found, come up upstream before?
Answer: **the symptom yes, famously; our exact byte-order mechanism, no.**

## 1. The symptom is a famous 2018–2019 upstream bug

- **Issue #4857 "Demon's Souls with v7077"** (opened 2018-07-07, closed 2019-03-19 by kd-11 "Closing as fixed"). Huge thread. Reports of "flashing and flickering screen", "black flickering when travel from Nexus to first Boletaria archstone ... screen goes white flash", "flickering in every stage, training, nexus, boletaria". Users on Vulkan and OpenGL, Windows and Linux.
  https://github.com/RPCS3/rpcs3/issues/4857
- **PR #5013 "Fix section reuse"** (kd-11, merged 2018-08-22, `f3d3a1a4`), body: "Probable fix for #4298 — Returns things to the old way of handling false positives in the texture cache ... Hopefully this fixes flickering in other games like #4857."
  https://github.com/RPCS3/rpcs3/pull/5013
- kd-11's `des_fbo_fixes` / `fbo_fixes` branch (Dec 2018–Jan 2019) — "I finally isolated the black flickering, but it needs a lot of work to fix properly." Merged to master around 2019-01-06 ("Fbo fixes is merged into master now"). This fixed the historical flicker for most users; a few residual reports, closed 2019-03-19 with build 7881.
- **Root cause upstream was texture-cache / ZCULL / FBO section reuse**, not fragment-literal byte order. Users' workarounds: toggle "Write Color Buffers", disable ZCull Occlusion Queries, force VSync in the NVIDIA driver, warp to a different area. None of those are byte-order remedies.
- **Crucially, a 2018 forum report** (https://forums.rpcs3.net/showthread.php?tid=199398, "Black/White Screen flashing (only HUD elements visible)") says: **"The problem doesn't appear if OpenGL renderer is used."** That was taken as evidence the historical bug was driver/renderer-specific (section reuse / FBO handling), not guest program data.

So the *visible symptom* we are chasing was reportable and was fixed years ago by work in the texture-cache/FBO area.

## 2. Relevant upstream work on the exact code area (fragment-constant decoding)

- **PR #15915 "rsx: Fix fragment constants decoding for non-x86 platforms"** (kd-11, merged 2024-08-09, `3ef8046f5c3778d3f66e78f3ce28a957b099fec9`). The RPCS3 blog (2024-12-09 arm64 announcement) says: "Some RSX functionality was broken when running on non-x86 CPUs ... high performance code had been rewritten in SSE and AVX for x86 and the cross-platform fallback was untested." It moved `write_fragment_constants_to_buffer` into `ProgramStateCache.cpp` and made **ARCH_ARM64 use the SSE2/sse2neon path** (the `#if defined(ARCH_X64) || defined(ARCH_ARM64)` we have today). **The byte transform did not change** — both paths are the 16-bit-half shuffle `((v>>8)&0xff00ff)|((v<<8)&0xff00ff00)`. This fix is already an ancestor of our HEAD.
- **PR #19669 "RSX: Keep infinite fragment constants"** (merged 2026-10-02, `f2b7edff`). Upstream changed the sanitizer so Inf is passed through (real RSX does); our branch predates it and still zeroes Inf (`shuffled & 0x7fffffff > 0x7f800000`). Fixed "armored core booster flickers" (#19555). Unrelated to our world literal (finite values), but a known trap in this exact fill.
- **PR #16652 "Fix graphics corruption when switching between shader interpreter and recompiler at runtime"** (merged 2025-02-02) — corrupted visuals in hybrid async-recompiler + interpreter mode (#15851). Our config uses "Async Recompiler with Shader Interpreter"; worth keeping in mind but not the literal byte order.
- **Issue #7713 / PR #7716 "Facepalm"** (2020-03-07, `149d550f`/`d64f0ffb`) — "after #6744 OpenGL only renders HUD elements, with constant black/white frame flickering; on vulkan there are no such graphical issues." PR body: *"Byte order step was disabled for debugging and not restored."* This is the source of the "byte order" search hit, but the diff only re-enables `pack_settings.swap_bytes(pack_unpack_swap_bytes)` in **`GLTextureCache.h`** — a **GL-only texture-readback pack swap**, not the shared fragment-constant decode. Cannot affect all backends.
- **Blog 2017-03-11 "Demon's Souls Nearly Playable"** — *"The value of a uniform fragment shader variable responsible for overall scene brightness was found to be calculated to negative zero"* — fixed via a shader-cache rewrite + PR #2482 (decompiler fix). Closest historical *fragment-constant value* cause, but a wrong value (negative zero), not byte-order corruption of a literal word.
- **PR #15915 closed issue #15914 "aarch64/rsx: Extreme visual flickering and visual corruption in 3D games"** — the ARM64 fallback was genuinely broken; already fixed in our branch.
- **Commit `47202d583` (2017-09-25) "rsx: Set up patch functionality for program coefficients"** — added a `patch_table` keyed by constant word → replacement value, the closest thing upstream ever had to "patch a specific fragment constant." Removed by #15915; no evidence it was ever used for a byte-order fix or for Demon's Souls.
- Older: PR #1318 (2015) "Gl: Use common code for fragment constants" — established the shared fill all backends still use.

**Wiki:** the official [Demon's Souls page](https://wiki.rpcs3.net/index.php?title=Demon%27s_Souls) lists *"Known Issues: There are no reported issues with this title."* GPU notes are only "Write color buffers: On — Fixes missing graphics ingame" and "Resolution scale threshold 640x640". No mention of shader literals or byte order.

## 3. What this means for our RCA

Our empirical RCA says the trailing literal word in **guest RAM** is `bswap32(correct_rsx_word)` at **first prefetch**, and all instrumented blit copies were byte-faithful (`src == dst` in 22,802 writes). Three independent code reviews confirmed **VK, GL, and KosmicKrisp (VK→Metal4) apply the same shared `T()` transform and no backend performs any byte swap on program words**. So:

1. **Our byte-order mechanism does not match any upstream report or fix.** No issue/PR/commit describes fragment literals being byte-reversed in guest RAM, nor a `bswap32` patch of inline literals. The historical DS flicker was ZCULL/FBO section reuse.
2. If the reversed bytes are genuinely in guest RAM (as measured), **every backend would compute the same wrong float** from the shared fill — so this would be a *shared/guest-data* problem, not a Metal4 regression. That contradicts the historical "OpenGL is fine" observation, unless:
   - (a) our Metal4 branch introduced an upstream-of-writer path that reverses these bytes (e.g. in the GPU-blit / surface write-back work — note local commit `281a806aa` "fix the byte-range write-back"), or
   - (b) the historical OpenGL observation concerned the ZCULL/FBO bug, a different cause of the same symptom, and our byte-order case is a second, independent defect.
3. Upstream never landed a byte-order fix for DS, so there is no upstream patch to port. The W4 oracle's `(w<<16)|(w>>16)` is the only transform we know corrects it, and it lives in gated diagnostics.

## 4. Practical consequences

- Do **not** land the W4 `bswap32`/halves transform as a shipping fix on the theory that upstream would have one — upstream does not.
- Before fixing anything, reproduce the differential the doc avoided: render the same DS scene on Vulkan/KosmicKrisp and OpenGL with `RSX_FP_LITERAL_LOG` and compare the trailing word in `fill.log`. If VK/GL show the same reversed word, this is shared/guest data (and the fix must be generic, in shared code, with an upstream bug report). If they show the correct word, the reversal is Metal4-specific and the search should focus on the GPU-blit / write-back paths introduced by commits `281a806aa` and `8ef12357f`.
- Two upstream fixes are **not** in our branch and may matter for overall DS correctness independently: #19669 (keep Inf constants) and #16652 (interpreter/recompiler switch corruption).

## Sources
- https://github.com/RPCS3/rpcs3/issues/7713 , https://github.com/RPCS3/rpcs3/pull/7716 (GL texture-readback byte swap — not our bug)
- https://blog.rpcs3.net/2017/03/11/des (negative-zero fragment constant — closest value-level match)
- https://github.com/RPCS3/rpcs3/issues/15914 (ARM64 flicker fixed by #15915)
- https://wiki.rpcs3.net/index.php?title=Demon%27s_Souls
- https://github.com/RPCS3/rpcs3/issues/4857
- https://github.com/RPCS3/rpcs3/pull/5013
- https://github.com/RPCS3/rpcs3/pull/15915
- https://blog.rpcs3.net/2024/12/09/introducing-rpcs3-for-arm64/
- https://github.com/RPCS3/rpcs3/pull/19669
- https://github.com/RPCS3/rpcs3/pull/16652
- https://forums.rpcs3.net/showthread.php?tid=199398
- https://github.com/RPCS3/rpcs3/issues/13087 (VK `vkCmdFillBuffer` black-screen regression, fixed by #13090 — a different black-screen cause)
