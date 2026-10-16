# 13 — KosmicKrisp bundling ledger (pin, provenance, loader isolation)

This is the reproducible record for shipping **both** the MoltenVK and KosmicKrisp
ICDs inside `rpcs3.app` and for making the bundled Vulkan loader search the bundle
**only**.

Status legend: **[V]** = verified on this machine (macOS 27.0.1, arm64),
**[I]** = inferred / not directly exercised here.

---

## 1. Pin — KosmicKrisp

| Field | Value |
|---|---|
| Version | `1.4.363.0` (LunarG Vulkan SDK build) |
| Provenance | `https://github.com/maluoi/kosmic_krisp_mirror/releases/download/1.4.363.0/kosmickrisp.zip` |
| Archive `sha256` | `0daad483a931f0855582e7c254f06d24ff8d729e7d2adb8c59ead5f0ecc64394` |
| `libvulkan_kosmickrisp.dylib` `sha256` | `52d7b68364e19d744882533d663d69b898bfd294f4248b3313785ad801e23e58` |
| `libkosmickrisp_icd.json` `sha256` | `f6dced3fc889099bd155fdd8bca28c3465698962f1ceee71ba1c875b467715b7` (artifact; the bundled copy is rewritten to `19e57865bac9b042c69d07b47b17916fb1b6b17818f06cd83d973b69975f8066`) |
| Arch / OS | `arm64`, macOS 26+ (Metal 4) |
| `VkDriverId` | 28 (`VK_DRIVER_ID_MESA_KOSMICKRISP`) |

The archive is an automated extraction of the KosmicKrisp driver from the **LunarG
Vulkan SDK 1.4.363.0**. It is a third-party mirror, not a first-party Mesa release;
Mesa does not publish macOS binaries. The archive digest is pinned in
`.ci/bundle-kosmickrisp.sh` and re-checked on every deploy.

**Why this artifact:** its `otool -L` closure is **system-only** (see §3), so bundling
it needs no extra dylibs. The alternative (Homebrew `mesa` 26.2.4) works on this
machine but is only built with `-Dvulkan-drivers=kosmickrisp,swrast` on macOS ≥ 26,
so it cannot be produced on the CI runners (`macos-15`) — and its dylib pulls a
10-library non-system closure (libxcb/libX11/zstd/spirv-tools). Pinning the SDK
artifact is therefore both cheaper and portable.

Alternative pins considered:

| Source | Version | Note |
|---|---|---|
| Homebrew `mesa` (bottle `arm64_golden_gate`) | 26.2.4 | builds KK on macOS ≥ 26; bottle `sha256 a0b56573cf9b7662010f20160b880c6037175c37ac16e9b351d5108e1385eb7f`; dylib pulls xcb/X11/zstd/SPIRV-Tools |
| MacPorts `mesa +kosmickrisp` | 26.2.3 | lags upstream |
| Mesa source (`archive.mesa3d.org/mesa-26.2.4.tar.xz`) | 26.2.4 | `sha256 bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9`; requires a macOS 26 host + a full build |

## 2. MoltenVK / loader pins (unchanged, for context)

| Component | Pin | Source |
|---|---|---|
| MoltenVK | `v1.4.2` | `MoltenVK-macos-privateapi.tar` (KhronosGroup release), as before |
| Vulkan loader | `1.4.363.0` | bundled `libvulkan.1.dylib` (Homebrew `vulkan-loader` locally; MacPorts `vulkan.framework` in CI) |

## 3. KosmicKrisp dependency closure

`otool -L libvulkan_kosmickrisp.dylib` links only system libraries/frameworks:

```
/usr/lib/libz.1.dylib
/usr/lib/libSystem.B.dylib
/usr/lib/libexpat.1.dylib
/usr/lib/libc++.1.dylib
/usr/lib/libobjc.A.dylib
/System/Library/Frameworks/{QuartzCore,Metal,Foundation,CoreFoundation}.framework
```

No LLVM, no zstd, no xcb/X11, no SPIRV-Tools. The bundling script still walks and
vendors any non-system closure generically (and repoints references at
`@executable_path/../Frameworks/`), so a future artifact that adds deps stays correct.

## 4. Loader isolation mechanism

The bundled loader is the Khronos Vulkan-Loader. Two facts were taken **from the
loader binary and source**, not guessed:

1. `strings libvulkan.1.dylib` contains `VK_LOADER_SEARCH_ONLY_IN_BUNDLE` and the
   relative paths `vulkan/icd.d` / `vulkan/explicit_layer.d`.
2. `loader.c::read_data_files_in_search_paths` uses `CFBundleGetMainBundle()` +
   `CFBundleCopyResourcesDirectoryURL()` to search `<bundle>/Contents/Resources/vulkan/icd.d`
   **first**, then the system paths. If `VK_LOADER_SEARCH_ONLY_IN_BUNDLE` is set it
   sets `search_outside_of_bundle = false`, so **only** the bundle Resources dir is
   searched. Available since loader 1.4.359 (we ship 1.4.363.0).

RPCS3 sets this variable itself — no user configuration is required — at the very
start of `vk::instance::create()`, which is the first point that can reach the
loader. The decision is a pure function (`should_enable_bundle_only_search`) plus a
filesystem probe of the bundle's `icd.d`:

* bundle ships ≥ 1 `*.json` ICD manifest → `setenv("VK_LOADER_SEARCH_ONLY_IN_BUNDLE", "1", 0)`
  (system ICDs ignored);
* bundle ships no ICD → variable left unset → loader searches the system paths
  (fallback preserved);
* `overwrite = 0` (and an explicit `getenv` check) → a user-provided value always wins.

**Layer side effect [V, by design]:** the variable also restricts *layer* discovery to
the bundle, so a system-installed `VK_LAYER_KHRONOS_validation` is no longer found.
Users who need it can set `VK_LOADER_SEARCH_ONLY_IN_BUNDLE=0` (or unset it) before launch.

## 5. Bundle layout

```
RPCS3.app/Contents/
  MacOS/rpcs3
  Frameworks/libvulkan.1.dylib          # bundled loader
  Frameworks/libMoltenVK.dylib
  Frameworks/libvulkan_kosmickrisp.dylib
  Resources/vulkan/icd.d/MoltenVK_icd.json          -> ../../../Frameworks/libMoltenVK.dylib
  Resources/vulkan/icd.d/libkosmickrisp_icd.json    -> ../../../Frameworks/libvulkan_kosmickrisp.dylib
```

## 6. Verification

See the task report. Summary of what was exercised on macOS 27.0.1 / arm64:

* `rpcs3.app` contains the loader + both ICDs **[V]**.
* With the dev machine's system ICDs present
  (`/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json`,
  `/opt/homebrew/share/vulkan/icd.d/{kosmickrisp_mesa_icd.aarch64,lvp_icd.aarch64}.json`)
  a probe binary placed inside the bundle enumerates **exactly** the two bundled
  drivers and ignores the system ones **[V]**.
* Device enumeration yields one entry per `(driver_kind, device)` with
  `(MoltenVK)` / `(KosmicKrisp)` labels **[V]**.

## 7. Residual uncertainty

* Pre-macOS-26 arm64: the bundled KK dylib fails to `dlopen` (Metal 4 missing). The
  loader is expected to log and continue so MVK still works, but this was **not**
  exercised here (no <26 host) **[I]**.
* x86_64 bundles: KK is not bundled at all (the script skips non-arm64) **[V by script]**.
