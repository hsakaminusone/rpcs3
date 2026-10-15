# 11 — Making MoltenVK and KosmicKrisp coexist (deep dive)

**Question:** can MoltenVK (MVK) and KosmicKrisp (KK) live in one RPCS3 build/process, and what
exactly breaks when they do? **Answer:** yes — the Vulkan **loader is explicitly designed for
multiple ICDs**. They do not fight inside the process. Every real problem is one of: *telling them
apart*, *gating MVK behind the portability flag*, *driver-specific code keyed on the wrong thing
(`__APPLE__` / `MVK`)*, or *packaging the loader + both ICDs*. This doc enumerates those precisely
and gives the fix for each.

Read this together with doc 04 (selection design), doc 10 (plan), and doc 09 (shadPS4 reference).

---

## 1. The loader's multi-driver model (authoritative)

Source: `KhronosGroup/Vulkan-Loader` `docs/LoaderDriverInterface.md` + `LoaderInterfaceArchitecture.md`.

- Each driver is a separate dylib with a **JSON ICD manifest**. Drivers are independent; the loader
  multiplexes them into one `VkInstance`.
- **Portability gate.** MVK's manifest declares `"is_portability_driver": true`. The loader
  **does not load portability drivers at all** unless the app sets
  `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` (and, for the extension to appear,
  `VK_KHR_portability_enumeration`). KK's manifest declares `false`/omits it ⇒ **KK is always
  loaded**. This is the single most important fact: *"no portability flag ⇒ only KK; with the flag ⇒
  MVK + KK."*
- **LDP_LOADER_9 — per-driver extension filtering:** during `vkCreateInstance` the loader
  **strips any enabled instance extension that a given driver does not support**, *per driver*.
  ⟹ Enabling `VK_KHR_portability_enumeration` (which only MVK advertises) **cannot break KK**.
- **LDP_LOADER_8 — instance-extension emulation:** for drivers that lack an instance extension, the
  loader emulates its entry points, so the app is unaware of who lacks what.
- **`vkEnumerateInstanceExtensionProperties` returns the union** across drivers (so MVK's
  portability extension is visible even though KK has no such thing).
- **Physical-device ordering is unspecified.** The loader only sorts on Windows
  (`vk_icdEnumerateAdapterPhysicalDevices`, LUID-based). On macOS the order is "indeterminate" ⇒
  **RPCS3 must never select by index; it must select by `driverID`.**
- **Duplicate-ICD hazard (called out verbatim in the docs):** *"MoltenVK is commonly placed inside
  application bundles. If there exists a system installed MoltenVK, the loader will load **both**
  the app bundled and the system installed MoltenVK, leading to potential issues or crashes."* Same
  applies to KK now that Homebrew/MacPorts/SDK install it system-wide.
- **Isolation knob:** `VK_LOADER_SEARCH_ONLY_IN_BUNDLE=1` (loader ≥ **1.4.359**, macOS/iOS only)
  restricts layer/driver discovery to the app bundle. **We have loader 1.4.363.0, so it is
  available.**
- **Runtime-switch knobs are trap doors:** `VK_DRIVER_FILES` / `VK_LOADER_DRIVERS_SELECT` /
  `VK_LOADER_DRIVERS_DISABLE` / `VK_LOADER_DRIVER_ID_FILTER` (≥1.4.326) are read during loader
  driver **discovery**, which happens once per process. They are fine as *diagnostic / launcher*
  overrides but **must not be the mechanism for an in-app runtime toggle.**

> **Verdict on squidbus's suggestion** ("just omit the portability flag"): that is the *KK-only*
> configuration. RPCS3 wants *both*, so we **keep** the portability flag and disambiguate by
> `driverID`. Per LDP_LOADER_9 that is safe for KK.

---

## 2. Coexistence problem catalogue

Legend: **[BUG]** = breaks today · **[GAP]** = missing feature · **[RISK]** = must verify.

### C1 — Both drivers report the same `deviceName` **[BUG]**
Both return e.g. `"Apple M4 Pro"`. RPCS3 currently:
- `rpcs3qt/render_creator.cpp`: `adapters->append(QString::fromStdString(gpu.get_name()))` ⇒ **two identical
  entries** in the adapter list.
- `VK/VKGSRender.cpp` (~444–466): `if (gpu.get_name() == adapter_name)` ⇒ picks **whichever device
  the loader listed first**, i.e. non-deterministic; the user cannot force KK vs MVK.

**Fix:** label with the driver, e.g. `"Apple M4 Pro (KosmicKrisp)"` / `"… (MoltenVK)"`, and make the
selection key `(driver_vendor, deviceName)` — or store the two in separate cfg fields. Never match
on `deviceName` alone.

### C2 — `get_driver_vendor()` is hard-coded to MVK on Apple **[BUG]**
`VK/vkutils/device.cpp:286–291`:
```cpp
#ifdef __APPLE__
    // moltenVK currently returns DRIVER_ID_MOLTENVK (0).
    return driver_vendor::MVK;
#endif
```
This makes **every** Apple device — including a KK device — report `MVK`, so every MVK/KK workaround
misfires. (The comment is also factually wrong: `VK_DRIVER_ID_MOLTENVK` is **14**, not 0.)

**Fix:** delete the Apple short-circuit and add to the existing `driver_properties.driverID` switch:
```cpp
case VK_DRIVER_ID_MOLTENVK:        return driver_vendor::MVK;          // 14
case VK_DRIVER_ID_MESA_KOSMICKRISP:return driver_vendor::KOSMICKRISP;  // 28
```
(`VK_DRIVER_ID_MESA_HONEYKRISP`=26 is already handled.)

### C3 — `chip_class()` is hard-coded to Apple-MVK **[BUG]**
`VK/vkutils/chip_class.cpp:~108`: on `__APPLE__` it returns `chip_class::APPLE_MVK` for **any**
Apple GPU (vendor `0x106B`). KK would be mis-classified.

**Fix:** choose based on the resolved `driver_vendor` (`APPLE_MVK` for MVK, `APPLE_HK_generic` — or a
new `APPLE_KK` if the M-series differences ever matter — for KK).

### C4 — MVK-only behaviour keyed on `__APPLE__`/`MVK` instead of the actual vendor **[BUG/RISK]**
Every Apple-specific site must be re-evaluated for KK so that the *MVK* path is preserved but KK is
not accidentally forced down it:

| Site | Today | Action |
|---|---|---|
| `VKGSRender.cpp:702` | `supports_hw_conditional_render = (vendor != MVK)` | KK gets `true`. **Verify** KK tolerates the "don't stop the renderpass" speedhack; if not, add `&& vendor != KOSMICKRISP`. |
| `VK/VKCompute.cpp:78` | `case MVK:` with `HONEYKRISP` → `unroll_loops=true, group_size=256` | Add `case KOSMICKRISP` (same as Honeykrisp/Apple). |
| `VK/VKRenderTargets.h:214` | `case MVK:` no-op fallthrough | Add `KOSMICKRISP` to the no-op list, else it logs **"Unknown driver vendor!"** and falls through. |
| `VK/vkutils/swapchain.cpp:125` | `case MVK:` no-op | Add `KOSMICKRISP`. |
| `VK/VKHelpers.cpp:~123` | `case MVK:` (comment only) | Add `case KOSMICKRISP:` ("needs testing"). |
| `VK/vkutils/device.cpp:135,592` | `#ifdef __APPLE__` for `VK_KHR_portability_subset` | **OK as-is**: KK doesn't advertise the extension, so the feature stays false and it is never requested. |
| `VK/vkutils/instance.cpp:108–137` | MVK layer settings (`MVK_CONFIG_RESUME_LOST_DEVICE`, `MVK_CONFIG_FAST_MATH_ENABLED`) enabled unconditionally on Apple | Inert for KK (they are `VkLayerSettingsCreateInfoEXT`, unknown to KK and — per LDP_LOADER — passed through). Keep, but consider gating on "MVK will be used". Note `disable_msl_fast_math` is an **MSL** knob: it has no KK meaning. |

Also port shadPS4's *KK* quirk analogues once identified (doc 09): `needs_unorm_fixup`,
`needs_lds_barriers`, primitive-list-restart disable (KK unrolls ⇒ slow), and the **1024 unique
sampler** cap (KK returns `VK_ERROR_OUT_OF_HOST_MEMORY` past it — check whether RPCS3's
`maxSamplerAllocationCount` usage can exceed this).

### C5 — Duplicate ICD discovery (bundled ⊕ system) **[BUG on dev machines / RISK]**
The moment a user (or CI runner) has MacPorts/Homebrew/SDK Vulkan installed **and** runs the bundled
app, the loader loads **both** the bundle ICDs and the system ICDs ⇒ duplicate physical devices and,
per the loader docs, potential crashes. This is exactly what PR #17902 testers hit
(`brew uninstall molten-vk` as a workaround).

**Fix (recommended):** ship the loader and, for release builds, set
`VK_LOADER_SEARCH_ONLY_IN_BUNDLE=1` at startup (before `vkCreateInstance`). Alternatives/escape
hatches: `VK_LOADER_DRIVERS_DISABLE=<glob>` on the system manifests, or `VK_LOADER_DRIVER_ID_FILTER`.

### C6 — Loader bundling / provenance **[GAP — hard prerequisite for KK]**
`deploy-mac.sh` copies **only** `libMoltenVK.dylib` + `MoltenVK_icd.json`; it does **not** copy a
Vulkan loader. Build wiring points at MacPorts (`VULKAN_SDK=/opt/local`, `-Wl,-rpath,/opt/local/lib`;
the MacPorts `vulkan-loader` port installs **`vulkan.framework`**).

Why this matters *specifically for coexistence*: **MoltenVK's dylib also exports the public `vk*`
API**, so historically an app could link/load it *as if* it were the loader (the old CI literally did
`ln -s libMoltenVK.dylib libvulkan.dylib`). **KosmicKrisp is a pure ICD — it does *not* export the
public Vulkan API.** It can *only* be driven through a real loader. Therefore:

> **Shipping KK forces RPCS3 to bundle a genuine Vulkan loader (`libvulkan.1.dylib` / `vulkan.framework`).**

If the current bundle really has no loader, KK cannot work at all on an end-user machine. This is a
blocking packaging item, not an optional nicety.

- **Verify (30 s on a mac build):**
  ```sh
  otool -L  RPCS3.app/Contents/MacOS/rpcs3 | grep -i vulkan
  ls        RPCS3.app/Contents/Frameworks | grep -i vulkan
  ls        RPCS3.app/Contents/Resources/vulkan/icd.d
  ```
- **Then:** bundle the loader into `Contents/Frameworks/`, add `@executable_path/../Frameworks` as an
  rpath, and point both ICD manifests at `../../../Frameworks/…`. (The rpath-fixup logic that used to
  live in `deploy-mac.sh` was removed in `4b9f2af41` when CI moved to MacPorts — it likely needs to
  come back.)

### C7 — ICD manifest details **[RISK]**
- MVK: `is_portability_driver: true`; `library_path` already rewritten to
  `../../../Frameworks/libMoltenVK.dylib` by `deploy-mac.sh`.
- KK: `kosmickrisp_mesa_icd*.json` (Mesa) with `library_path` → bundled
  `libvulkan_kosmickrisp.dylib`, likewise rewritten. Mesa fixed a bug where the KK ICD json
  advertised the wrong `api_version` (`kk: Fix icd json api version`); ensure the pin includes it.
- Bundle ICDs take priority over system ICDs on macOS (`Contents/Resources/vulkan/icd.d` searched
  first), so C5 is about *duplicates*, not about the bundle losing.

### C8 — macOS-version gating **[GAP]**
The KK dylib targets **macOS 26**; RPCS3's deployment target is **15.0**, and the arm64 app runs on
macOS 15 too. On <26 the loader will fail to `dlopen` KK — this **must be non-fatal** (the loader
logs and continues; verify). Keep KK **out of the x86_64 bundle entirely** (KK is arm64 + macOS 26).
For the `Auto` policy, prefer KK only when the OS is ≥ 26 *and* a KK device actually enumerated.

### C9 — Instance API version **[OK]**
RPCS3 requests `VK_API_VERSION_1_2`, KK exposes 1.4 — a higher driver version is fine. Just keep the
bundled KK ICD json's `api_version` ≥ the header version we compile against.

### C10 — lipo + codesign **[RISK]**
`optimize-mac.sh` `lipo -thin`s *every* file to the main binary's arch, then `deploy-mac.sh`
`codesign --deep`. Add the KK dylib **before** these steps, keep it arm64-only on the arm64 bundle,
and check `otool -L libvulkan_kosmickrisp.dylib` for extra dynamic deps (LLVM / spirv-tools). Mesa's
`--prefer-static` build avoids most of them (shadPS4 builds it that way).

---

## 3. Version bumps → absolute latest (as of 2026-10-15)

### 3.1 MoltenVK — latest release **v1.4.2** (2026-07-24)

| Where | Current | Bump to |
|---|---|---|
| `3rdparty/MoltenVK/CMakeLists.txt` (`GIT_TAG`) | `4588705` (**v1.4.0**, 2025-08-20) | `db660224` = **v1.4.2** |
| `.ci/deploy-mac.sh` download | `v1.4.2` | `v1.4.2` (**already latest**) |
| MacPorts `MoltenVK` (build-time ICD) | port = **1.4.1** | up to the maintainer; the *bundle* uses the 1.4.2 tarball regardless |

Notes: upstream HEAD is `5189f8de` (post-1.4.2 dev, untagged) — stick to the **v1.4.2 tag** for
reproducibility. There is **no** MoltenVK 1.4.3 upstream (the `v1.4.3-ryujinx.*` tags are a Ryujinx
fork, not upstream). MoltenVK is still **not** conformant (no `portability_subset` removal) — which is
exactly why the portability gate stays.

### 3.2 Vulkan headers / loader / SPIRV-Cross — **1.4.363.0** (SDK 2026-09-29)

`vulkan.lunarg.com/sdk/latest/*.txt` = **1.4.363.0** (confirmed: no 1.4.364 yet). MacPorts
`vulkan-loader` is already **1.4.363.0** ✅. This is the loader that carries the KK conformance
milestone, `VK_LOADER_SEARCH_ONLY_IN_BUNDLE` (≥1.4.359), and `VK_LOADER_DRIVER_ID_FILTER` (≥1.4.326).
If we vendor a loader for the bundle, pin **`vulkan-sdk-1.4.363.0`**.

### 3.3 KosmicKrisp — latest = **mesa 26.2.5** / shadPS4 fork **`af7dd0a3`**

| Source | Version / pin | Notes |
|---|---|---|
| Mesa stable (contains KK) | **26.2.5** (2026-10-14; 26.2.6 due 2026-10-28) | latest tagged Mesa; 26.3.0-rc1 also out (2026-10-14) — too new |
| Vulkan SDK | **1.4.363.0** | KK at Vulkan 1.4-conformant; Metal 4; macOS 26+ |
| `shadexternals/mesa-kosmickrisp` (shadPS4) | **`af7dd0a3`** (2026-10-02) | *newest KK*: adds geometry-shader support + mip-tail-offset & triangle-fan fixes; what shadPS4 ships |
| Homebrew `mesa` | 26.2.4 | `-Dvulkan-drivers=kosmickrisp,swrast` on Tahoe+; also pulls `molten-vk`, `llvm`, `spirv-llvm-translator`, `spirv-tools` |
| MacPorts `mesa` | **26.2.3** | `+kosmickrisp` → `libvulkan_kosmickrisp.dylib` + `kosmickrisp_mesa_icd.aarch64.json` (lags upstream) |

Recommendation: pin **`af7dd0a3`** (fork) or **Mesa 26.2.5**. Conformance was reached 2026-09-25;
the SDK snapshot predates the fork's geometry-shader work, so **`af7dd0a3` is the freshest KK** and is
proven by shadPS4.

> Net: MVK is already effectively latest in the artifact we ship (bump the source-build pin 1.4.0 →
> 1.4.2); the loader is already 1.4.363.0; KK should be pinned to `af7dd0a3` (or Mesa 26.2.5).

---

## 4. Recommended coexistence architecture

1. **One instance, portability flag always set on macOS** (so MVK is enumerable). KK is unaffected
   (LDP_LOADER_9). 
2. **Enumerate**, then build an adapter list of `(physical_device, driverID → driver_vendor, name)`.
3. **Label** adapters `"<name> (<vendor>)"`; **select by `(vendor, name)`**, with `Auto` = KK if a KK
   device exists and OS ≥ 26, else MVK.
4. **Keep MVK and KK workarounds keyed on `driver_vendor`**, never on `__APPLE__` (C2–C4).
5. **Bundle the loader + both ICDs into the arm64 `.app`**; MVK only on x86_64 (C6–C8).
6. **Set `VK_LOADER_SEARCH_ONLY_IN_BUNDLE=1`** for release builds to stop system ICDs from
   double-loading (C5).
7. **Never** use env-var driver selection for the in-app toggle; the only runtime switch is which
   `VkPhysicalDevice` we bind to.

---

## 5. Verification checklist (run before/while coding)

- [ ] `otool -L RPCS3.app/Contents/MacOS/rpcs3 | grep -i vulkan` → is a loader linked, and from where?
- [ ] `ls RPCS3.app/Contents/Frameworks | grep -i vulkan` → is the loader bundled today? (C6)
- [ ] With both ICDs present, `vkEnumeratePhysicalDevices` on macOS 26 returns **2** devices with the
      portability bit set and **1** (KK) without. Confirm `driverID` = 14 / 28.
- [ ] Confirm MVK's v1.4.2 `MoltenVK_icd.json` has `is_portability_driver: true`.
- [ ] Confirm KK's `kosmickrisp_mesa_icd*.json` `library_path`/`api_version`.
- [ ] `otool -L libvulkan_kosmickrisp.dylib` → list extra dynamic deps (C10).
- [ ] On macOS 15 (arm64), confirm a loader failing to `dlopen` the macOS-26 KK dylib is non-fatal.
- [ ] `MESA_KK_DEBUG=msl` + RPCS3 to dump MSL and spot shader-translation gaps.

---

## 6. Sources

- Loader driver interface: <https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md>
- Loader interface architecture (env vars, `VK_LOADER_SEARCH_ONLY_IN_BUNDLE`, driver-id filter): <https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md>
- MoltenVK releases (v1.4.2): <https://github.com/KhronosGroup/MoltenVK/releases>
- MacPorts `vulkan-loader` Portfile (installs `vulkan.framework`): <https://github.com/macports/macports-ports/blob/master/graphics/vulkan-loader/Portfile>
- Vulkan SDK latest (`1.4.363.0`): <https://vulkan.lunarg.com/sdk/latest/windows.txt>
- Mesa release calendar (26.2.5 / 26.3.0-rc1): <https://docs.mesa3d.org/release-calendar.html>
- `shadexternals/mesa-kosmickrisp` (`af7dd0a3`): <https://github.com/shadexternals/mesa-kosmickrisp>
- Mesa KosmicKrisp docs: <https://docs.mesa3d.org/drivers/kosmickrisp.html>
