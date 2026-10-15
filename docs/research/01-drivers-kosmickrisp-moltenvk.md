# 01 — Apple Vulkan drivers: MoltenVK vs KosmicKrisp

## What each thing is

| | **MoltenVK** | **KosmicKrisp (KK)** |
|---|---|---|
| Origin | Brenwill Workshop → Khronos | LunarG (Google-sponsored), part of **Mesa** |
| Nature | Portability layer; **not conformant** | Full **Vulkan 1.4 conformant** ICD |
| Underlying API | Metal (3.x baseline) | **Metal 4** |
| Hardware | Intel + Apple Silicon | **Apple Silicon only** |
| Minimum OS | old macOS / iOS / tvOS / visionOS | **macOS 26+** |
| Shader path | SPIR-V → SPIRV-Cross → MSL | SPIR-V → Mesa **NIR** → MSL (no SPIRV-Cross) |
| `VkDriverId` | `VK_DRIVER_ID_MOLTENVK` = **14** | `VK_DRIVER_ID_MESA_KOSMICKRISP` = **28** |
| License | Apache-2.0 | MIT |
| iOS | shipping solution today | demoed internally (A14+), **not enabled in Mesa tree** |

> Correction to an in-tree assumption: RPCS3's `device.cpp` comment claims MVK returns driverID `0`. The Vulkan spec value is **14** (`VK_DRIVER_ID_MOLTENVK`). Verify against bundled v1.4.2.

## Versions relevant to this repo (as of 2026-10-15)

### MoltenVK (what RPCS3 uses now) — latest release **v1.4.2** (2026-07-24)
- **Source build is an `ExternalProject` (MoltenVK is no longer a git submodule):** `3rdparty/MoltenVK/CMakeLists.txt` → `GIT_TAG 4588705` (2025-08-20) = MoltenVK **1.4.0** → **bump to `db660224` (v1.4.2)**.
- **macOS deploy artifact:** `.ci/deploy-mac.sh` downloads **`v1.4.2`** (`MoltenVK-macos-privateapi.tar`) — already latest.
- **Homebrew `molten-vk`:** 1.4.2. **MacPorts `MoltenVK`:** 1.4.1 (lags; it's the *build-time* ICD only).
- MoltenVK is now **Vulkan 1.4** (layered), still **not conformant** (still needs the portability gate).
- Upstream HEAD `5189f8de` is post-1.4.2 dev (untagged) — pin the **v1.4.2 tag**. There is no upstream 1.4.3 (the `v1.4.3-ryujinx.*` tags are a Ryujinx fork).

### KosmicKrisp — **latest**
- **Conformance:** Vulkan **1.4** CTS passed **2026-09-25** (was 1.3 since Sep 2025). Khronos news 2026-10-01.
- **Shipped in:** **Vulkan SDK 1.4.363.0** (2026-09-29); earlier big jump in **1.4.357** (2026-08-02, "up to ~2.35× faster + full Vulkan 1.4 exposure"); KK first shipped ~SDK May 2026 (1.4.350).
- **In Mesa:** upstreamed for **Mesa 26.0**; current train **26.2** — latest tag **26.2.5** (2026-10-14; 26.2.6 due 2026-10-28), with 26.3.0-rc1 also out (2026-10-14). Homebrew `mesa` = 26.2.4, MacPorts `mesa` = 26.2.3.
- **Freshest KK source:** `shadexternals/mesa-kosmickrisp` @ **`af7dd0a3`** (2026-10-02) — geometry-shader support + mip-tail-offset/triangle-fan fixes; what shadPS4 ships. **Recommended pin** (newer than the SDK snapshot).
- **Perf:** LunarG says performance has **tripled** since 1.3 conformance and is **"not too far behind native Metal"** (Phoronix, Oct 2026).
- **Features added alongside 1.4:** tessellation, robustness2, multi-draw, etc.
- **Not in:** Vulkan SDK < ~1.4.350; not on Intel; not on macOS < 26.

### Build requirements (KK from source)
- Xcode + CLT; `meson` 1.9.1+; `cmake`; `pkg-config`; `libclc`; **llvm 20.1.8+**; `spirv-llvm-translator`; `spirv-tools`. Python: `mako`, `packaging`, `pyyaml`.
- Build: `meson setup <mesa> --buildtype=debug -Dplatforms=macos -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= -Dopengl=false -Dzstd=disabled --prefer-static`
- Runtime env: `MESA_KK_DEBUG={msl|force_robustness}`, `MESA_KK_GPU_CAPTURE`, `MESA_KK_GPU_CAPTURE_DIRECTORY`, `MESA_KK_DISABLE_WORKAROUNDS`, `MESA_KK_EXPERIMENTAL`.
  - `MESA_KK_DEBUG=msl` dumps every generated **MSL shader** — invaluable for debugging shader translation.

## Distribution / packaging

LunarG (Oct 2026) recommends bundling the loader **and both ICDs**, and **not** relying on a system-wide Vulkan loader:

```
YourApp.app/Contents/
  Frameworks/
    libvulkan.1.dylib            # Vulkan Loader
    libMoltenVK.dylib            # if shipping MVK
    libvulkan_kosmickrisp.dylib  # if shipping KK
  Resources/
    vulkan/icd.d/
      MoltenVK_icd.json
      libkosmickrisp_icd.json
```

The ICD json's `library_path` is relative, e.g. `"../../../Frameworks/libvulkan_kosmickrisp.dylib"`.

> **RPCS3 status (verified from tree):** `deploy-mac.sh` bundles `libMoltenVK.dylib` + `MoltenVK_icd.json` **but no loader**; the build links MacPorts' `vulkan.framework` with an rpath to `/opt/local`. Because **KosmicKrisp is a pure ICD and does NOT export the public `vk*` API** (unlike MoltenVK, which can be linked directly), shipping KK **requires** bundling a real loader. See doc 11 §C6.

**ICD manifest filenames seen in the wild**
- MoltenVK: `MoltenVK_icd.json`
- KK (LunarG SDK): `libkosmickrisp_icd.json` (also packaged as `MoltenVK_icd.json`-style names in samples)
- KK (Mesa, incl. MacPorts): Mesa-generated, e.g. `kosmickrisp_mesa_icd.aarch64.json`
- KK dylib: `libvulkan_kosmickrisp.dylib`

### Package-manager reality (important for the "MVK hijack" problem)

- **Homebrew `mesa` 26.2.4** *does* build KosmicKrisp, but only on macOS ≥ 26:
  ```ruby
  vulkan_drivers = (MacOS.version >= :tahoe) ? "kosmickrisp,swrast" : "swrast"
  [ ... "-Dvulkan-drivers=#{vulkan_drivers}", ... ]
  on_macos { depends_on "molten-vk" }
  ```
  So on Tahoe+ `brew install mesa` yields **two** ICDs in `$(brew --prefix)/share/vulkan/icd.d/`: **kosmickrisp** and **swrast** (lavapipe software Vulkan). On Sequoia/Sonoma it yields only swrast. It also depends on `molten-vk`, `llvm`, `spirv-llvm-translator`, `spirv-tools`.
- **MacPorts:** `sudo port install mesa +kosmickrisp` → `/opt/local/lib/libvulkan_kosmickrisp.dylib`, `/opt/local/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json` (this is what RPCS3 draft PR #17902 uses).
- **Vulkan SDK:** easiest prebuilt path; installer can also drop a system-wide ICD (not recommended for shipping).
- Because these install ICDs into loader-scanned system paths, they get picked up by RPCS3 even though the app bundle also ships MoltenVK — this is exactly what PR #17902 testers hit.

## How ICD selection works (macOS)

From the Vulkan Loader docs + LunarG:

- Loader search order puts the **app bundle first**: `<bundle>/Contents/Resources/vulkan/icd.d`, then user/home/`/etc`/`/usr/local/share/vulkan/icd.d`, etc.
- **System ICDs are ignored if the bundle supplies drivers of the same name** (macOS behavior; squidbus confirmed this in PR #17902).
- Force a driver: `VK_DRIVER_FILES=/path/xxx_icd.json` (preferred), `VK_ICD_FILENAMES` (deprecated), `VK_ADD_DRIVER_FILES` (prepend), `VK_LOADER_DRIVERS_SELECT` / `_DISABLE` (glob filters). Read by the loader at init.
- **Portability enumeration is the key lever:**
  - MoltenVK only appears in `vkEnumeratePhysicalDevices` when the app enables `VK_KHR_portability_enumeration` **and** sets `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`.
  - KosmicKrisp needs neither.
  - ⟹ **No portability flag ⇒ only KK (and swrast); with the flag ⇒ MVK + KK + swrast.** `squidbus` (PR #17902): "you can block MVK by just not adding the portability flag / extension."
- Both drivers report the **same `deviceName`** (e.g. `"M4 Pro"`) ⇒ disambiguate via `VkPhysicalDeviceDriverProperties.driverID` (14 vs 28).

## RPCS3 PRs on this

- **#17735 "(macOS) Support loading VK ICDs (in preparation for KosmicKrisp)" — MERGED** 2025-12-06, now commit **`b76ca8807`**: links the loader, uses `VK_EXT_metal_surface`, adds a `CAMetalLayer` Obj-C shim, forces ICD detection on macOS.
- **#17902 "[Mac OS X] [CI] Add KosmicKrisp builds (for evaluation/testing)" — OPEN draft** (schm1dtmac). Key thread points:
  - Author's stated goal (Feb 2026): *"using both KK & MVK when available, while also falling back to MVK only for users on Intel or on Mac OS X Sonoma/Sequoia."* Confirms this doc's target.
  - Dec 2025: KK was already rendering the GoW3 in-game main menu briefly before crashing.
  - Sep 2026 testers (`shinra-electric`): had to `brew uninstall molten-vk` or RPCS3 boots MVK. Author: global ICDs are prioritized; **backend selection is needed**; both drivers identify as "M4 Pro".
  - Oct 2026 (`squidbus`): bundle ICDs are prioritized; use `VK_DRIVER_FILES`; or **omit the portability flag** to hide MVK.
  - Related merged PR **#18897** raised the macOS deployment/runners to OS 15 (min Sequoia).
  - Related open issues: #19434 (black squares, TLoU), #19700 (Batman AO UE3 smearing on MVK).

## Known-issue surface (why KK is not "just a driver drop-in")

Mesa documents **18 `KK_WORKAROUND_*`** entries for Metal / MSL compiler bugs. Highlights that will also bite a *native* Metal backend:

- **Metal violates its own memory coherency model** for device memory without `coherent` (W6, fixed macOS 27).
- **Early-fragment `discard_fragment` with side effects is dropped prematurely** (W5, fixed macOS 27).
- **`simd_is_helper_thread()` wrong after discard** (W4, fixed macOS 27); **`simd_ballot` inside conditionals misbehaves** (W3, fixed macOS 27).
- **MSL compiler crashes**: infinite loops, uninitialized scratch (W1/W2, W14/W17; W2 re-enabled for M5 on macOS 27).
- **Stencil `sample_mask` ignored when no color attachment** with MSAA depth-stencil (W7) — directly relevant to RSX MSAA.
- **`MTL4Compiler` concurrency**: concurrent instances corrupt the heap → must keep **one compiler per `MTLDevice`** (W11, fixed macOS 27 b5). Relevant to RPCS3's async pipeline compilation.
- **Index-buffer OOB robustness** not guaranteed unless 32-bit aligned (W13); **M5 OOB LOD clamp** differs (W16).
- **`volatile`/`coherent` miscompile** (W15).
- **Rosetta size/`currentAllocatedSize` heap mismatch** breaks GPU capture (W8).

## Apple platform reality (Oct 2026)

- **macOS 26 "Tahoe"** is the **last release to support Intel Macs** (only 4 models) — announced WWDC 2025.
- **macOS 27 "Golden Gate"** (Sept 2026) runs **Apple Silicon only**; full Rosetta 2 remaining this cycle, largely gone in macOS 28 (legacy games excepted).
- So from macOS 27 on, "Apple Silicon + macOS ≥ 26" is universal ⇒ **KK is viable everywhere the emulator runs**, and MVK is only needed to support **Intel / macOS ≤ 25** and as a regression escape hatch.

## Sources
- LunarG "The State of Vulkan on Apple – Oct. 2026": <https://www.lunarg.com/the-state-of-vulkan-on-apple-oct-2026/>
- LunarG KosmicKrisp product page: <https://www.lunarg.com/products/kosmickrisp>
- Mesa KK docs: <https://docs.mesa3d.org/drivers/kosmickrisp.html>
- Mesa KK workarounds: <https://docs.mesa3d.org/drivers/kosmickrisp/workarounds.html>
- LunarG SDK 1.4.363.0 / 1.4.357: <https://www.lunarg.com/lunarg-releases-vulkan-sdk-1-4-363-0/>
- Phoronix (Oct 2026): <https://www.phoronix.com/news/KosmicKrisp-Vulkan-1.4-2026>
- XDC 2026 talk + slides: <https://indico.freedesktop.org/event/12/contributions/538/>
- Vulkan SDK macOS getting started (ICD paths): <https://vulkan.lunarg.com/doc/view/1.4.335.1/mac/getting_started.html>
- Loader driver interface: <https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md>
- Homebrew mesa formula: <https://github.com/Homebrew/homebrew-core/blob/HEAD/Formula/m/mesa.rb>
- Apple: macOS 26 last Intel release: <https://developer.apple.com/documentation/macos-release-notes/macos-26_4-release-notes>
