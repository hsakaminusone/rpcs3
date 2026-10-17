# 12 — Building RPCS3 on macOS (Homebrew-only)

Everything required to turn a fresh checkout of this branch into a signed, launchable
`RPCS3.app` on Apple Silicon **without MacPorts**, plus the packaging and test steps.
Written against the `kosmickrisp-integration` branch.

Scripts referenced here are the source of truth:

- `.ci/build-mac-homebrew.sh` — configure + build
- `.ci/deploy-mac-homebrew.sh` — bundle the Vulkan runtime, thin, sign

## 0. Reproduce

```sh
bash .ci/build-mac-homebrew.sh      # configure + ninja build   -> build/bin/rpcs3.app
bash .ci/deploy-mac-homebrew.sh     # bundle loader + MVK ICD, thin, ad-hoc sign
build/bin/rpcs3.app/Contents/MacOS/rpcs3 --version
```

## 1. Host prerequisites

| Item | Value used |
|---|---|
| macOS | 27.0.1 (Darwin 27), arm64 (Apple T8140) |
| RAM / CPU | 8 GiB / 6 cores |
| Xcode CLT | `/Library/Developer/CommandLineTools` — Apple clang 21 (satisfies RPCS3's `clang >= 19` check) |
| Homebrew | 7.x at `/opt/homebrew` (no MacPorts on this machine) |

RPCS3's upstream CI uses MacPorts. This guide replaces that with Homebrew; everything
below exists to make that substitution work.

## 2. Dependencies

```sh
brew install cmake ninja llvm@22 qtbase qtsvg qtmultimedia qttools \
             sdl3 vulkan-headers vulkan-loader molten-vk pkgconf googletest
```

Resolved versions: cmake 4.4.4 · ninja 1.13.2 · **llvm@22 22.1.8** · qt 6.12.0 ·
sdl3 3.4.18 · vulkan-headers/loader 1.4.363.0 · molten-vk 1.4.2 · pkgconf 3.0.8 ·
googletest 1.18.0. Installed size ≈ 2.2 GB (`llvm@22` alone ≈ 1.6 GB).

Deliberate omissions (each avoids a large dependency tree):

| Skipped | Why |
|---|---|
| `llvm` (23.x) | RPCS3 pins LLVM 22 (CI `LLVM_COMPILER_VER=22`, submodule 22.1.8). **Use `llvm@22`.** |
| `qt` meta | Pulls `qtwebengine` + ~47 deps. Only `qtbase qtsvg qtmultimedia qttools` are needed (`qttools` provides `macdeployqt`). |
| `opencv` | Optional — only used by the PS Move tracker behind `HAVE_OPENCV`. Homebrew OpenCV 5 pulls ~70 deps (gcc/vtk/openvino/…). CMake emits a harmless WARNING. |
| `glew` | Not used on Apple (`3rdparty/CMakeLists.txt` guards it with `NOT APPLE`). |
| brew `ffmpeg` | The builtin prebuilt (`USE_SYSTEM_FFMPEG=OFF`) is what CI uses and avoids brew's ffmpeg tree. |
| `curl`, `zlib` | System SDK/CLT versions are found by default (`USE_SYSTEM_CURL`, `USE_SYSTEM_ZLIB`). |

## 3. Submodules

```sh
git submodule -q update --init --depth=1 --jobs=8 \
  $(awk '/path/ && !/llvm/ && !/opencv/ && !/libsdl-org/ && !/feralinteractive/ && !/curl/ && !/zlib/ { print $3 }' .gitmodules)
```

Same exclusions as CI; the six excluded paths are replaced by system/cask builds.

Two pitfalls found:

1. **`--depth=1` checks some submodules out at the branch tip, not the pinned SHA**, and
   glslang's shallow fetch fails outright (`did not contain <sha>`). Fix: fetch the exact
   recorded SHA per submodule and `git checkout` it — GitHub serves arbitrary SHAs.
   Verify with `git submodule status | grep -c '^+'` == 0.
2. **`yaml-cpp` can end up with an empty worktree** after a shallow init. Fix:
   `git submodule update --init --force -- 3rdparty/yaml-cpp/yaml-cpp`.

## 4. Configure

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix);$(brew --prefix llvm@22);$(brew --prefix)/lib/cmake/Qt6/.." \
  -DWITH_LLVM=ON -DBUILD_LLVM=OFF -DSTATIC_LINK_LLVM=OFF \
  -DLLVM_DIR="$(brew --prefix llvm@22)/lib/cmake/llvm" \
  -DQt6_DIR="$(brew --prefix)/lib/cmake/Qt6" \
  -DSDL3_DIR="$(brew --prefix sdl3)/lib/cmake/SDL3" \
  -DUSE_SDL=ON -DUSE_SYSTEM_SDL=ON \
  -DUSE_SYSTEM_MVK=ON -DUSE_SYSTEM_OPENCV=ON -DUSE_SYSTEM_FFMPEG=OFF \
  -DUSE_AUDIOUNIT=ON -DUSE_NATIVE_INSTRUCTIONS=OFF \
  -DUSE_PRECOMPILED_HEADERS=OFF -DUSE_LTO=OFF \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Non-obvious flags and why:

- **`-DCMAKE_OSX_ARCHITECTURES=arm64` is mandatory.** The root `CMakeLists.txt` only appends
  `/opt/homebrew` to the search path when this is exactly `arm64`. Omit it and dependencies
  are not found.
- **`-DQt6_DIR=$(brew --prefix)/lib/cmake/Qt6`** — Homebrew lays *every* Qt module's config
  under the unified prefix. Pointing at the `qtbase` keg makes component targets
  (`Qt6::Multimedia`, `Qt6::Svg`, …) unresolvable.
- **`-DLLVM_DIR`** — `llvm@22` is keg-only, so it is not on the default search path.
- **`-DUSE_LTO=OFF`** — LTO only affects `rpcs3_emu`, but on 8 GiB the LTO link can OOM.
- **`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`** — CMake 4 refuses the old `cmake_minimum_required`
  in some 3rdparty subprojects (protobuf, asmjit).
- `USE_SYSTEM_OPENCV=ON` with OpenCV absent is fine: `WARNING`, not `FATAL_ERROR`.

## 5. Build

```sh
ninja -C build -j4          # single build at a time on this box
```

- ~1258 targets, ~17 min cold on 6 cores.
- **Run only one build at a time.** Two parallel `-j4` builds on 8 GiB thrash swap
  (observed 3.5/4 GB swap, 134k pageouts) and can OOM a compiler.
- `build/compile_commands.json` is exported — useful for fast `clang++ -fsyntax-only`
  checks of individual TUs without a full build.

## 6. Required source fix — builtin ffmpeg needs Apple frameworks

The link fails without this patch:

```
FAILED: bin/rpcs3.app/Contents/MacOS/rpcs3
ld: symbol(s) not found for architecture arm64
  "_kCVPixelBufferWidthKey", referenced from: _vt_frames_init in libavutil.a[arm64](hwcontext_videotoolbox.o)
```

**Cause:** `3rdparty/CMakeLists.txt` links the builtin ffmpeg by raw path
(`FFMPEG_LIB_AVCODEC`, …) and never links the `ffmpeg` INTERFACE target that declares the
Apple frameworks. The framework list in `3rdparty/ffmpeg/CMakeLists.txt`
(`CoreServices CoreFoundation AudioUnit AudioToolbox CoreAudio CoreMedia VideoToolbox
CoreVideo Security`) therefore never reaches the link line, so the static `libavutil.a`
symbols from `hwcontext_videotoolbox.o` are undefined.

**Fix (already applied, `3rdparty/CMakeLists.txt`):** in the builtin-ffmpeg branch, add

```cmake
if (APPLE)
    target_link_libraries(3rdparty_ffmpeg INTERFACE
        "-framework CoreServices" "-framework CoreFoundation"
        "-framework CoreMedia" "-framework VideoToolbox"
        "-framework CoreVideo" "-framework Security")
endif()
```

## 7. Packaging (`.ci/deploy-mac-homebrew.sh`)

Adapted from the MacPorts `deploy-mac.sh`, with two changes:

1. **No MoltenVK tarball download** — uses the Homebrew `molten-vk` keg.
2. **Bundles the Vulkan loader** (`libvulkan.1.dylib`). This is a hard prerequisite for
   KosmicKrisp (a pure ICD that cannot stand in for the loader) and is recommended by LunarG
   even for MVK-only bundles.

Steps performed:

- copy the loader into `Contents/Frameworks/`, `install_name_tool -id` it, and rewrite every
  `libvulkan` reference in the binary and bundled dylibs to
  `@executable_path/../Frameworks/libvulkan.1.dylib`;
- copy `libMoltenVK.dylib` and write
  `Contents/Resources/vulkan/icd.d/MoltenVK_icd.json` with
  `library_path: ../../../Frameworks/libMoltenVK.dylib`;
- prune unused Qt frameworks (`QtPdf/QtQml/QtQuick/…`), then thin to the host arch via
  `.ci/optimize-mac.sh`, then ad-hoc sign (`codesign --force --deep -s -`).

Verify: `codesign --verify --deep <app>` and `otool -L` shows no `/opt/homebrew` references.

**Dev-machine caveat:** if Homebrew `molten-vk` is installed, the loader *also* scans
`/opt/homebrew/etc/vulkan/icd.d`, loading a second copy of `libMoltenVK.dylib` and printing a
duplicate-class Obj-C warning. It does not happen on a machine without Homebrew MVK. Pin the
ICD to silence it:

```sh
VK_ICD_FILENAMES="$PWD/build/bin/rpcs3.app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json" \
  build/bin/rpcs3.app/Contents/MacOS/rpcs3
```

`VK_LOADER_SEARCH_ONLY_IN_BUNDLE=1` (loader ≥ 1.4.359) is the release switch to prevent
bundle + system ICD double-loading. See `11-mvk-kk-coexistence.md` §C5/§C6.

## 8. Tests

```sh
cmake -S . -B build-tests -G Ninja \
  -DBUILD_RPCS3_TESTS=ON -DRUN_RPCS3_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DSTATIC_LINK_LLVM=ON \
  -DLLVM_DIR="$(brew --prefix llvm@22)/lib/cmake/llvm" \
  -DUSE_SYSTEM_SDL=ON -DUSE_SYSTEM_MVK=ON -DUSE_SYSTEM_FFMPEG=OFF \
  -DUSE_DISCORD_RPC=OFF -DUSE_PRECOMPILED_HEADERS=OFF -DUSE_NATIVE_INSTRUCTIONS=OFF \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5

ninja -C build-tests rpcs3_test
build-tests/bin/rpcs3_test --gtest_color=no
```

`rpcs3_test` requires `google_test` (brew `googletest`). A separate build directory is used so
it does not disturb `build/`.

## 9. KosmicKrisp status on this branch

- The MoltenVK/KosmicKrisp **runtime selection** is implemented (`docs/research/10`, `11`).
- **Only the MoltenVK ICD is bundled** by the deploy script. Bundling a KosmicKrisp ICD
  requires an `arm64` KK build (`shadexternals/mesa-kosmickrisp` or brew `mesa` once it
  packages `libvulkan_kosmickrisp`) and is a separate step; the loader is already in the
  bundle so dropping the ICD in is the only remaining packaging work.
- KosmicKrisp-specific quirks (unorm fixup, 1024-sampler cap, list-restart) are **not** wired
  yet — KK currently inherits Honeykrisp behaviour.

## 10. Gotcha checklist

| Symptom | Cause / fix |
|---|---|
| `/opt/homebrew` not on the search path | missing `-DCMAKE_OSX_ARCHITECTURES=arm64` |
| `Qt6::Multimedia` / `Qt6::Svg` not found | `Qt6_DIR` pointed at the `qtbase` keg; use `$(brew --prefix)/lib/cmake/Qt6` |
| LLVM not found | `llvm@22` is keg-only → set `LLVM_DIR` |
| LLVM ABI/API mismatch | used `llvm` 23 instead of **`llvm@22`** |
| `ld: symbol(s) not found … hwcontext_videotoolbox.o` | builtin-ffmpeg Apple frameworks missing (§6) |
| CMake 4 error on old subprojects | `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` |
| Submodule at wrong commit / empty `yaml-cpp` | shallow-init pitfalls (§3) |
| Compiler OOM / extreme slowness | too many parallel builds on 8 GiB → one `-j4` build at a time |
| Duplicate `MVKBlockObserver` class warning | Homebrew MVK + bundled MVK both load; pin `VK_ICD_FILENAMES` |
