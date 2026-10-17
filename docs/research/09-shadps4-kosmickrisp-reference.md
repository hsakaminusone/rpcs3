# 09 — shadPS4: a working KosmicKrisp integration (reference)

shadPS4 (PS4 emulator) has already shipped a **working KosmicKrisp** integration, authored by **squidbus** — the same person who advised RPCS3 in PR #17902. Their tree is the closest real-world reference we have. Cloned at `/tmp/shadPS4`.

**How to reproduce locally:**
```
git clone --filter=blob:none https://github.com/shadps4-emu/shadPS4.git /tmp/shadPS4
cd /tmp/shadPS4
git show 3590426a          # mac: Migrate to KosmicKrisp (#4566)
git show 7de583b4          # build: Switch to build KosmicKrisp from source (#4580)
git show 9fdaeb03          # vk_instance: Disable list restart on KosmicKrisp (#5161)
```

## The big architectural decision: **full swap, not dual-driver**

shadPS4 **removed MoltenVK entirely** and replaced it with KosmicKrisp:

- Raised `CMAKE_OSX_DEPLOYMENT_TARGET` from `15.4` → **`26.0`**.
- README: *"macOS users need at least macOS 26.0… Intel Macs are not supported."*
- `src/main.cpp` refuses to start on a real Intel Mac at runtime:
  ```cpp
  #if defined(__APPLE__) && defined(ARCH_X86_64)
      // KosmicKrisp only supports Apple Silicon.
      int sysctl_ret = 0; size_t n = sizeof(sysctl_ret);
      sysctlbyname("sysctl.proc_translated", &sysctl_ret, &n, nullptr, 0);
      if (sysctl_ret != 1) { /* message box: only Apple Silicon */ return -1; }
  #endif
  ```

> This is exactly the **opposite** of the RPCS3 requirement ("keep both, let users pick"). shadPS4 could do a clean swap because they dropped Intel and pre-26 macOS. **We can reuse their mechanics, but not their "single driver" policy.**

## Commit timeline (as read from git)

| Commit | Date | Title |
|---|---|---|
| `3590426a` | 2026-06-13 | mac: Migrate to KosmicKrisp (#4566) |
| `7de583b4` | 2026-06-15 | build: Switch to build KosmicKrisp from source (#4580) |
| `f29c0269` | 2026-06-15 | build,ci: Minor clean-up (#4583) |
| `d392abed` | 2026-07-24 | externals: Update mesa-kosmickrisp to latest (#4606) |
| `545e1adb` | 2026-09-20 | externals: Update mesa-kosmickrisp (#5063) |
| `05b51d22` | 2026-09-26 | externals: Update mesa-kosmickrisp (#5138) |
| `916dea43` | 2026-09-27 | externals: Update mesa-kosmickrisp (#5152) |
| `9fdaeb03` | 2026-09-28 | vk_instance: Disable list restart on KosmicKrisp (#5161) |
| `3912336f` | 2026-10-01 | externals: Update mesa-kosmickrisp (#5214) |

For comparison, RPCS3's equivalents are PR **#17735** (merged; ICD loading) and **#17902** (open draft; driver-id + CI-only KK build by schm1dtmac).

## What `3590426a` (source migration) changed

**Removed the entire portability path** (KK is conformant, doesn't need it):
- `vk_platform.cpp`: dropped `VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME` and `vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR`.
- `vk_instance.cpp/.h`: dropped `VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME`, `PhysicalDevicePortabilitySubsetFeaturesKHR`, `portability_subset`, `portability_features`, and the `IsTessellationIsolinesSupported()` / `IsTessellationPointModeSupported()` helpers.
- Removed the MVK-only `MVK_CONFIG_DEBUG` layer setting.

**Re-pointed MoltenVK-specific workarounds at KosmicKrisp**:
- `vk_pipeline_cache.cpp`: `needs_lds_barriers = … || eMesaKosmickrisp` (was `… || eMoltenvk`); `needs_unorm_fixup = … == eMesaKosmickrisp` (was MoltenVK).
- Removed `has_incomplete_fragment_shader_barycentric` (an MVK quirk) from `profile.h`, `vk_pipeline_cache.cpp`, and simplified `shader_recompiler/frontend/translate/vector_interpolation.cpp`.

**Driver identification** (unchanged, but the mechanism to copy):
```cpp
// vk_instance.cpp ~664
const auto property_chain = physical_device.getProperties2<
    vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties>();
const vk::PhysicalDeviceDriverProperties driver =
    property_chain.get<vk::PhysicalDeviceDriverProperties>();
driver_id = driver.driverID;   // then compare against vk::DriverId::eMesaKosmickrisp
```

## What `7de583b4` (build) added

- **Submodules**: `externals/mesa-kosmickrisp` (`https://github.com/shadexternals/mesa-kosmickrisp`) and `externals/vulkan-loader` (`KhronosGroup/Vulkan-Loader`).
- **CMake option** `ENABLE_SYSTEM_VULKAN` (default `OFF` on Apple) to opt out of building/shipping the bundled driver.
- **Bundle three files** (copied into the build dir next to the executable):
  ```
  libvulkan.dylib               # Vulkan Loader (built from source)
  libvulkan_kosmickrisp.dylib   # the KK ICD (built from source)
  kosmickrisp_mesa_icd.json     # ICD manifest
  ```
  via a `CopyVulkanDriver` custom target; paths come from `externals/CMakeLists.txt`:
  `…/mesa-kosmickrisp/outputs/libvulkan_kosmickrisp.dylib` and `…/outputs/kosmickrisp_mesa_icd.json`.
- **CI** (`.github/workflows/build.yml`): runner `macos-26`, Xcode latest, deps
  `brew install cmake meson pkg-config llvm spirv-tools spirv-llvm-translator libclc` +
  `pip3 install mako packaging pyyaml`.
- **Docs** (`documents/building-macos.md`): Xcode **26.0+**; same brew/pip deps; `-DENABLE_SYSTEM_VULKAN=ON` to use a system setup.

## What `9fdaeb03` (post-migration fixup) shows

```cpp
// vk_instance.cpp — KosmicKrisp tuning
list_restart = driver_id != vk::DriverId::eMesaKosmickrisp &&
               add_extension(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
// comment: "Games excessively leave restart enabled with lists… it slows down
// KosmicKrisp due to unrolling. Revisit if KosmicKrisp moves this to mesh shaders."
```
And a latent quirk in `vk_instance.h`:
```cpp
u32 GetMaxSamplerAllocationCount() const {
    if (driver_id == vk::DriverId::eMesaKosmickrisp) {
        // KK has an internal 1024 unique sampler limit before
        // vkCreateSampler returns VK_ERROR_OUT_OF_HOST_MEMORY.
        return 1024;
    }
    return properties.limits.maxSamplerAllocationCount;
}
```

## ICD discovery

```cpp
#if defined(__APPLE__)
    // point the loader at the directory containing the bundled ICD
    static const auto icd_path = [] {
        char path[PATH_MAX]; u32 n = PATH_MAX; _NSGetExecutablePath(path, &n);
        return std::filesystem::path(path).parent_path();
    }();
    setenv("VK_DRIVER_FILES", icd_path.c_str(), true);
#endif
```
`VK_DRIVER_FILES` accepts a **directory**; the loader scans it for `*_icd.json`.

## Reusable vs not

| shadPS4 pattern | Reuse in RPCS3? |
|---|---|
| Identify driver via `VkPhysicalDeviceDriverProperties.driverID` | ✅ directly |
| `VK_DRIVER_FILES=<exe dir>` + bundle loader/driver/ICD | ✅ directly |
| Build KK from source as a submodule + `ENABLE_SYSTEM_VULKAN` | ✅ likely (option) |
| KK-specific workarounds (`unorm_fixup`, `lds_barriers`, list-restart, sampler cap) | ✅ as analogues in RPCS3's VK backend |
| **Drop portability extension/subset** | ❌ **No** — RPCS3 needs portability to keep MVK enumerable |
| **Raise deployment target to 26.0 / drop Intel** | ❌ **No** — RPCS3 must keep MVK on Intel & macOS ≤ 25 |
| Single-driver swap | ❌ No — RPCS3 wants both selectable |

## Key takeaway

shadPS4 proves the **mechanics** work end-to-end on macOS 26 + Metal 4 (bundle loader + KK, `driverID`-based quirks, build-from-source). It also shows the **cost**: a real migration needed several KK-specific workarounds and a build/CI overhaul. RPCS3's variant is *more* complex because it must preserve MoltenVK simultaneously — see doc 10.
