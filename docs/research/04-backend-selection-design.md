# 04 — MoltenVK / KosmicKrisp selection design

Answers: *Is UI backend selection possible? Can the user choose? Can we keep both? How do we solve the MVK-fallback problem ("problem 3")?*

## Short answers

- **UI selection: yes, straightforward.** RPCS3 already has a renderer dropdown and an *adapter* list built by enumerating Vulkan devices (`rpcs3qt/render_creator.cpp`). A "Vulkan driver" choice is the same pattern.
- **Keep both: yes.** Bundle both ICDs in the `.app`; the loader enumerates both; pick by `driverID`.
- **Fallback: yes** — do it at the **physical-device** level, not with env vars. This is what makes it robust.

## Why physical-device selection (not env vars)

Two mechanisms were considered; the RPCS3 PR thread (schm1dtmac, shinra-electric, squidbus) essentially converged on the second:

**A. `VK_DRIVER_FILES` / `VK_LOADER_DRIVERS_SELECT` env vars** — must be set *before* the loader initializes (first Vulkan call). RPCS3 creates a Vulkan instance in **two** places: the UI probe (`render_creator.cpp:35`) and emulation init (`VKGSRender.cpp:422`). Fragile; loader caches; doesn't help UI enumeration.

**B. Enumerate all, pick a device by `driverID`** — ✅ preferred.
- Enable `VK_KHR_portability_enumeration` + `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` (required for MVK; ignored by KK). This is already done on Apple (`instance.cpp:193-218`).
- Then `vkEnumeratePhysicalDevices` returns **MVK + KK (+ lavapipe, see below)**.
- Read `VkPhysicalDeviceDriverProperties.driverID`: `VK_DRIVER_ID_MOLTENVK` (**14**) vs `VK_DRIVER_ID_MESA_KOSMICKRISP` (**28**). *(squidbus's alternative to "block MVK": simply don't add the portability flag — but that hides MVK entirely, so it can't be used when the user wants MVK.)*
- Report `driver_vendor` per device instead of the current hard-coded MVK-on-Apple.
- Choose the device by policy/user setting; **fall back to the other driver** if the preferred one is absent.

## Apple support matrix → fallback policy

Reality as of Oct 2026 (see doc 01): **macOS 26 Tahoe is the last Intel release; macOS 27 "Golden Gate" is Apple-Silicon-only.**

```
host = (arch, macOS version)
KK device present  ⇔  Apple Silicon  AND  macOS >= 26  AND  KK ICD loadable
MVK device present ⇔  an MVK ICD is loadable (bundled on all Apple builds)

policy (user setting; default "Auto"):
  Auto:
    if KK device present            -> KosmicKrisp
    else if MVK device present      -> MoltenVK
    else                            -> (no Vulkan; error)
    if the chosen driver yields no usable device -> use the other one + log a warning
  Explicit MoltenVK / KosmicKrisp:
    if present -> use it
    else       -> warn + fall back (and grey it out in the UI)
```

| Host | KK | MVK | Auto picks |
|---|---|---|---|
| Apple Silicon, macOS ≥ 27 | ✅ | ✅ (bundle) | KosmicKrisp |
| Apple Silicon, macOS 26 | ✅ | ✅ | KosmicKrisp |
| Apple Silicon, macOS ≤ 25 (Sonoma/Sequoia) | ❌ | ✅ | MoltenVK |
| Intel, macOS ≤ 26 | ❌ | ✅ | MoltenVK |

This covers every case the PR author called out (Intel, Sonoma/Sequoia) **and** the macOS-27 future where KK is always available.

## Packaging (keep both in one build)

`.ci/deploy-mac.sh` becomes "copy both ICDs" (mirroring LunarG's recommended layout):
```
rpcs3.app/Contents/Frameworks/
    libMoltenVK.dylib
    libvulkan_kosmickrisp.dylib
rpcs3.app/Contents/Resources/vulkan/icd.d/
    MoltenVK_icd.json
    <kosmickrisp>_icd.json
```
- Bundle ICDs take priority over system ICDs on macOS ⇒ this also neutralizes the "Homebrew `molten-vk` hijacks the loader" problem testers hit in PR #17902.
- KK is Apple-Silicon + macOS-26+ only, so the **arm64** bundle carries both; the **x86_64** bundle keeps MVK only.
- Wrinkle: if a user has **Homebrew `mesa`** installed on Tahoe, its `kosmickrisp` **and** `swrast` (lavapipe software Vulkan) ICDs sit in loader-scanned paths and can add a *software* device to the adapter list. The adapter sort (discrete > integrated > software) already deprioritizes it, but selection should filter by `driverID` so "Auto" never picks lavapipe.

## Required code changes (survey)

1. **`VK/vkutils/device.cpp:286-292`** — remove the unconditional `#ifdef __APPLE__ return MVK;`; detect by `driverID` (MVK=14, KK=28, Honeykrisp=26). *(PR #17902 does this.)*
2. **`VK/vkutils/chip_class.h`** — add `driver_vendor::KOSMICKRISP`; update all consumers (`VKGSRender.cpp`, `VKCompute.cpp`, `VKRenderTargets.h`, `VKHelpers.cpp`, `VKDMA.cpp`, shader paths, `swapchain.cpp`).
3. **`VK/VulkanAPI.h`** — add `VK_DRIVER_ID_MESA_KOSMICKRISP` (`#if VK_HEADER_VERSION < 287`).
4. **Instance creation** — keep portability enumeration on; **gate MVK-only settings** (`MVK_CONFIG_RESUME_LOST_DEVICE`, `MVK_CONFIG_FAST_MATH_ENABLED`, `kMVKMoltenVKDriverLayerName`) on "MVK is the chosen driver" (`instance.cpp:108-145`).
5. **Device pick** — in `VKGSRender` ctor (`VKGSRender.cpp:444-471`), select by `(driver, adapter name)` and fall back.
6. **UI / config**
   - New enum in `Emu/system_config_types.h` + `fmt_class_string` (e.g. `vulkan_apple_driver { auto_, moltenvk, kosmickrisp }`), `cfg::_enum` in `Emu/system_config.h` `node_vk` (Apple-gated), entry in `rpcs3qt/emu_settings_type.{h,cpp}`.
   - New `<QComboBox>` in `settings_dialog.ui`, wired via `EnhanceComboBox`, hidden off-Apple (same pattern as `disableMslFastMath` at `settings_dialog.cpp:1468`).
   - Extend `render_creator` to record driver vendor per adapter so the adapter list can read `"M4 Pro (KosmicKrisp)"` / `"M4 Pro (MoltenVK)"` (both currently report `"M4 Pro"`).

## UI mock (Graphics tab, macOS)

```
Renderer        : [ Vulkan ▾ ]
Vulkan Device   : [ M4 Pro (KosmicKrisp) ▾ ]      ← adapter + driver label
Vulkan Driver   : [ Auto ▾ ]                      ← Auto / MoltenVK / KosmicKrisp  (macOS only)
```

## Risks / caveats
- Both drivers may be loaded at once (both dylibs + both ICDs, since portability is on). Verify memory/timing, or optionally restrict loaded ICDs via `VK_LOADER_DRIVERS_SELECT` as a later optimization.
- `disable_msl_fast_math` must only apply when MVK is active.
- Adding an enum value changes config serialization-by-name ⇒ check `config_checker.cpp`.
- KK advertises advanced extensions (custom border color, etc.) only under `MESA_KK_EXPERIMENTAL`; the `driver_vendor::KOSMICKRISP` workaround switch must not assume MVK's quirks.

_Status: design only, no code written._
