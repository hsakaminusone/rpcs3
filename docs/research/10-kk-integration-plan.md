# 10 — Plan: ship MoltenVK + KosmicKrisp selection

**Goal:** one macOS build that ships **both** ICDs, enumerates both, shows a macOS-only **Vulkan Driver** selector (Auto / MoltenVK / KosmicKrisp), and falls back automatically — solving "problem 3" (MVK for Intel / macOS ≤ 25) without breaking either driver.

**Design principles** (from doc 04):
1. Keep the portability enumeration flag **on** — without it MoltenVK is invisible.
2. Select the physical device by **`driverID`** (MVK `14`, KK `28`), never by env var at runtime.
3. Do **not** raise the deployment target; KK must be **runtime**-gated (RPCS3 ≥ 15.0).
4. Ship **arm64** (KK + MVK) and **x86_64** (MVK only) bundles separately, as today.

**Reference:** doc 09 (shadPS4) for the working mechanics; **doc 11 for the coexistence deep-dive**
(loader semantics, the C1–C10 problem catalogue, and the exact version pins below); RPCS3 files are as noted.

---

## Size at a glance

| Phase | Net LOC (rough) | Files | Risk |
|---|---|---|---|
| 0. Version bumps + coexistence groundwork | ~120–220 | 6–8 | medium |
| 1. Driver identification | ~100–140 | 4 | low |
| 2. Config + UI selector | ~110–150 | 6 | low |
| 3. Device select + fallback | ~130–180 | 3 | medium |
| 4. KK behavioural bring-up | ~100–350 + testing | 6–10 | **high** |
| 5. Packaging + CI | ~100–250 | 4–6 | medium |
| **Total** | **~650–1200 LOC + config** | **~20–25** | |

**Effort:** ~**2–4 focused weeks** for a working "both selectable" build (phases 1–3 + 5), **plus** open-ended phase-4 bring-up (days→weeks, title-dependent). Mostly plumbing; the cost concentrates in KK behaviour and packaging.

---

## Phase 0 — Version bumps + coexistence groundwork (do first)

Per doc 11:

- **MoltenVK → v1.4.2:** `3rdparty/MoltenVK/CMakeLists.txt` `GIT_TAG 4588705` → `db660224`. (`deploy-mac.sh` already fetches v1.4.2.)
- **KosmicKrisp → `af7dd0a3`** (`shadexternals/mesa-kosmickrisp`) or Mesa **26.2.5**; loader **1.4.363.0** already matches MacPorts.
- **Coexistence bugs to fix at the same time** (details in doc 11):
  - C1 – adapter list/selection must not key on `deviceName` alone (`render_creator.cpp`, `VKGSRender.cpp`).
  - C2/C3 – remove the `__APPLE__ ⇒ MVK` hard-codes in `device.cpp` and `chip_class.cpp`.
  - C4 – add `driver_vendor::KOSMICKRISP` cases at every MVK site (`VKCompute.cpp`, `VKRenderTargets.h`, `swapchain.cpp`, `VKHelpers.cpp`, `VKGSRender.cpp`).
  - C5/C6 – bundle a real Vulkan **loader** and set `VK_LOADER_SEARCH_ONLY_IN_BUNDLE=1` (blocking for KK).
- **Verify first** (doc 11 §5): whether a loader is bundled today, that MVK/KK enumerate with `driverID` 14/28, and that KK's ICD json is well-formed.

_Exit criterion:_ both ICDs load in one instance on macOS 26; each reports the right `driver_vendor`.

## Phase 1 — Driver identification (VK core)

Mirrors shadPS4 + RPCS3 draft PR #17902.

- `rpcs3/Emu/RSX/VK/vkutils/chip_class.h` — add `driver_vendor::KOSMICKRISP`; add predicate `is_KOSMICKRISP()` if needed.
- `rpcs3/Emu/RSX/VK/VulkanAPI.h` — add
  ```cpp
  #if VK_HEADER_VERSION < 287
  #define VK_DRIVER_ID_MESA_KOSMICKRISP 28
  #endif
  ```
- `rpcs3/Emu/RSX/VK/vkutils/device.cpp` (`get_driver_vendor`, ~286–405) — **remove** the `#ifdef __APPLE__ return MVK;` hard-code; resolve from `VkPhysicalDeviceDriverProperties.driverID`:
  - `VK_DRIVER_ID_MOLTENVK` (14) → `MVK`
  - `VK_DRIVER_ID_MESA_KOSMICKRISP` (28) → `KOSMICKRISP`
  - `VK_DRIVER_ID_MESA_HONEYKRISP` (26) → `HONEYKRISP` (already in enum)
  - Keep the existing name-heuristic path for other vendors.
- `rpcs3/Emu/RSX/VK/VKHelpers.cpp` (~106–162) — add a `KOSMICKRISP` case to the global workaround switch (values TBD by testing; start with "no Apple-specific hacks", cf. merged PR #19311).

_Exit criterion:_ `driver_vendor` correctly reports `MVK` vs `KOSMICKRISP` on a Tahoe box with both ICDs present.

## Phase 2 — Config + UI selector

- `rpcs3/Emu/system_config_types.h` / `.cpp` — new enum + `fmt_class_string`:
  ```cpp
  enum class vulkan_apple_driver { auto_, moltenvk, kosmickrisp };
  ```
- `rpcs3/Emu/system_config.h` — `cfg::_enum<vulkan_apple_driver>` in `node_vk` (near `adapter`, ~197), `#ifdef __APPLE__`.
- `rpcs3/rpcs3qt/emu_settings_type.h` / `.cpp` — new `emu_settings_type` + mapping to `local_cfg.video.vk.*`.
- `rpcs3/rpcs3qt/settings_dialog.ui` — new `<QComboBox>` (driver selector) next to `renderBox`/`graphicsAdapterBox`.
- `rpcs3/rpcs3qt/settings_dialog.cpp` — populate via `EnhanceComboBox`, hide off-Apple (same pattern as `disableMslFastMath`, ~1468), disable when renderer ≠ Vulkan.

_Exit criterion:_ selector persists, appears only on macOS + Vulkan.

## Phase 3 — Device selection, adapter labelling, fallback

- `rpcs3/rpcs3qt/render_creator.cpp` — record the driver vendor per enumerated adapter so the list shows `"M4 Pro (KosmicKrisp)"` / `"M4 Pro (MoltenVK)"` (both report `"M4 Pro"` today). Filter out `lavapipe`/software unless it's the only device.
- `rpcs3/Emu/RSX/VK/VKGSRender.cpp` (~444–471) — selection policy:
  ```
  desired = cfg.vulkan_apple_driver
  candidates = devices matching desired driver   (auto => KK if present else MVK)
  pick candidates[adapter_name]  else candidates[0]  else other driver + warn
  ```
- `rpcs3/Emu/RSX/VK/vkutils/instance.cpp` (~108–145, 208–218) — keep portability on; **gate the MVK-only layer settings** (`MVK_CONFIG_RESUME_LOST_DEVICE`, `MVK_CONFIG_FAST_MATH_ENABLED`, `kMVKMoltenVKDriverLayerName`) on "MVK selected".

_Exit criterion:_ choosing each driver boots; "Auto" picks KK on Apple Silicon/≥26 and MVK elsewhere; broken preference falls back with a warning.

## Phase 4 — KosmicKrisp behavioural bring-up *(the risky part)*

Port the analogues of what shadPS4 needed, found empirically:
- `vkutils/swapchain.cpp` (~120–128) — present modes / formats per KK.
- `VKGSRender.cpp` (~653–816) `backend_configuration` — which features KK actually supports vs the MVK-assumed set.
- Candidate known-issue analogues (confirm which apply): `needs_unorm_fixup`, `needs_lds_barriers`, sampler-count cap (KK has a **1024 unique-sampler** limit before `VK_ERROR_OUT_OF_HOST_MEMORY`), primitive-list-restart (KK unrolls → slow).
- Ensure KK-specific behaviour is keyed off `driver_vendor::KOSMICKRISP`, **not** `__APPLE__`, so MVK keeps its old paths.

_Exit criterion:_ a small title set boots and renders correctly on KK; MVK regressions = none.

## Phase 5 — Packaging + CI

Current RPCS3 mac packaging: `.ci/deploy-mac.sh` downloads **MoltenVK v1.4.2**, copies `libMoltenVK.dylib` → `Contents/Frameworks/` and `MoltenVK_icd.json` → `Contents/Resources/vulkan/icd.d/` (rewriting `./`→`../../../Frameworks/`). `.ci/build-mac.sh` uses `-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DUSE_SYSTEM_MVK=ON VULKAN_SDK=/opt/local`. CI installs MacPorts ports incl. `MoltenVK` (`.github/parameters/setup-macports.yaml`).

Add (arm64 only):
```
rpcs3.app/Contents/Frameworks/libvulkan_kosmickrisp.dylib
rpcs3.app/Contents/Resources/vulkan/icd.d/<kosmickrisp>_icd.json
```
Options for the KK binary (pick one):
- **(a) MacPorts `mesa +kosmickrisp`** — lowest friction; matches the existing MacPorts CI. Copy `/opt/local/lib/libvulkan_kosmickrisp.dylib` + `/opt/local/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json`.
- **(b) Build from source as a submodule** (like shadPS4: `shadexternals/mesa-kosmickrisp` + `Vulkan-Loader`, `ENABLE_SYSTEM_VULKAN` escape hatch) — max control/versioning; heaviest.
- **(c) LunarG SDK binary** — easy, but version/reproducibility/licensing overhead.

Recommended: **(a)** for evaluation CI, move to **(b)** for a shipped bundle.

Also:
- Bundle a Vulkan **loader** in the app (`Contents/Frameworks/libvulkan.1.dylib`) if not already present — **verify**; shadPS4 bundles it; RPCS3's `deploy-mac.sh` currently does not.
- Keep `CMAKE_OSX_DEPLOYMENT_TARGET=15.0`; KK is not called directly, so no macOS-26 SDK requirement.
- CI: add KK to the **arm64** mac job only; leave the x86_64 job MVK-only.

_Exit criterion:_ the arm64 `.app` contains both ICDs and both load; x86_64 unchanged.

## Phase 6 — Validation

- Matrix: {Apple Silicon macOS 26/27, Intel macOS ≤ 26, Apple Silicon ≤ 25} × {Auto, MVK, KK} × {a few titles + a hardware test}.
- Log actual `driverID` at boot; confirm Auto chooses correctly on each host.
- Regression: MVK path identical to pre-change on Intel/Sonoma/Sequoia.

---

## Open questions / things to verify before coding

1. Does RPCS3's bundle ship the **Vulkan loader**, or rely on MacPorts `/opt/local/lib`? (deploy-mac.sh suggests the latter.)
2. Exact KK ICD filename + `library_path` from whichever packaging we choose.
3. Which of shadPS4's KK quirks actually apply to RPCS3's VK backend.
4. Whether KK works at all with RPCS3's current VK feature set (KK advertises some extensions only under `MESA_KK_EXPERIMENTAL`).
5. KS `VK_DRIVER_ID_MESA_KOSMICKRISP` header availability in RPCS3's vendored Vulkan headers.

## Upstreaming strategy

- **Phase 1** (driver-id) is aligned with merged **#17735** and open **#17902** ⇒ good candidate to upstream (or land via #17902).
- **Phases 2–3** (UI + selection) are plausibly upstreamable, but maintainers have been lukewarm on macOS-specific UX.
- **Phase 5** (bundling a second ICD + CI) is likely **fork-only** for now.

_Status: plan only. No code changes yet._
