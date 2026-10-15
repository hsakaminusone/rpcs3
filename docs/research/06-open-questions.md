# 06 — Open questions / TODOs

## Drivers & packaging
- [x] Does Homebrew `mesa` include KosmicKrisp? **Yes — but only on macOS ≥ 26** (`vulkan_drivers = (MacOS.version >= :tahoe) ? "kosmickrisp,swrast" : "swrast"`). It also builds **swrast (lavapipe)**.
- [x] Exact manifest names: SDK → `libkosmickrisp_icd.json`; Mesa/MacPorts → Mesa-generated `kosmickrisp_mesa_icd.<arch>.json`; MVK → `MoltenVK_icd.json`. Homebrew's exact filename still worth confirming on a real Tahoe box (`$(brew --prefix)/share/vulkan/icd.d/`).
- [x] `VK_DRIVER_ID_MOLTENVK` = **14** (RPCS3's in-tree comment saying `0` is wrong). Confirm the bundled v1.4.2 actually returns 14.
- [ ] KK runtime dylib deps to bundle (LLVM? libclc? SPIRV-Tools?).
- [ ] Verify KK + MVK (+ lavapipe) co-load cleanly when portability enumeration is enabled (memory, startup time).
- [ ] Decide KK version to ship (SDK 1.4.363.0 vs a Mesa/Homebrew build) and how to reproduce a reproducible KK binary in CI.

## RPCS3 integration (doc 04)
- [ ] Decide config surface: a new `vulkan_apple_driver` enum vs reusing the adapter dropdown with a driver label.
- [ ] Decide the "hard block global/brew MVK" idea vs relying on bundle priority + `driverID` filtering.
- [ ] Confirm `MVK_CONFIG_*` layer settings are safe to leave on when KK is active (or gate them).
- [ ] Does the UI probe (`render_creator`) need to enumerate both drivers, or just report supported renderers?
- [ ] Savestate/config compat for a new enum member (serialized by name; `config_checker.cpp:214`).
- [ ] x86_64 bundle: MVK only (correct?) — and does macOS 27 still ship an x86_64 slice of the loader? (Rosetta still present through 27.)

## Metal backend (doc 05 / 07)
- [ ] Shader strategy: **Option A (SPIR-V → SPIRV-Cross → MSL)** vs **Option B (native MSL decompiler)**.
- [ ] Is adding **SPIRV-Cross** to `3rdparty` acceptable (Apache-2.0; already used by MoltenVK)?
- [ ] Vertex strategy on Metal: vertex pulling vs stride clamping (Cemu's "no easy solution").
- [ ] Render-pass → `MTL4RenderCommandEncoder` mapping design (attachment map, load/store actions).
- [ ] Prototype `CAMetalLayer` + `MTL4CommandQueue` present path to validate the `GSFrameBase` seam.
- [ ] Assess which `backend_configuration` flags a Metal backend must advertise (many VK flags are driver workarounds).
- [ ] Estimate the RSX quirk surface to re-derive (format handling, RTT layouts, MSAA resolve, spilling).
- [ ] Plan for MSL/Metal bug workarounds (mirror KK's `KK_WORKAROUND_*` discipline).

## Process
- [ ] Repo layout for these docs (currently `docs/research/`; move to a scratch location if not upstreaming).
- [ ] Whether to push `kosmickrisp-integration` to the fork and/or open a tracking PR.
- [ ] Decide whether to lift KK's workaround list into RPCS3 test coverage.

## Resolved
- [x] Can both drivers coexist? Yes (bundle both; select by `driverID`).
- [x] Is UI selection possible? Yes (renderer/adapter dropdown pattern).
- [x] Fallback design: physical-device selection by `driverID`, "Auto" policy (doc 04).
- [x] Does macOS 27 support Intel? No — Apple Silicon only; KK is thus universally available on macOS 27.
- [x] Metal 4 API shape (doc 07).
