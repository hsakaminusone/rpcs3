# BUILD-STATUS — macOS RPCS3.app (KosmicKrisp integration branch)

**Owner:** build/environment agent. **Do not commit.** This file is the shared
coordination log for the parallel agents (build vs. tests).

## Coordination protocol (IMPORTANT)

- Homebrew serializes on a **global lock**. Before running `brew`, check for a
  running `brew` (`pgrep -fl brew`). Before/after every `brew` run, append an
  entry to the **Log** section below.
- Disk is tight: **~40 GiB free** on `/System/Volumes/Data` (as of start).
- RAM is limited: **8 GiB, 6 CPUs** → build with a modest `-j`.

## Environment (verified)

| Item | Value |
|---|---|
| macOS | 27.0.1 (build 26A434), arm64 (Apple Silicon, T8140) |
| Kernel | Darwin 27.0.0 |
| Xcode CLT | `/Library/Developer/CommandLineTools` (present) |
| Apple clang | 21.0.0 (clang-2100.3.34.2) — satisfies RPCS3's clang ≥ 19 check |
| Homebrew | 7.0.9 at `/opt/homebrew` |
| CPUs / RAM | 6 / 8 GiB |
| Git branch | `kosmickrisp-integration` @ 17543d273 (identical to master) |

## Plan (decisions)

- Use **Homebrew** (not MacPorts) for all deps.
- Use **system LLVM** (Homebrew) instead of the LLVM submodule:
  `-DBUILD_LLVM=OFF -DWITH_LLVM=ON -DSTATIC_LINK_LLVM=OFF`, `LLVM_DIR=/opt/homebrew/opt/llvm/lib/cmake/llvm`.
- Use **builtin ffmpeg** (`USE_SYSTEM_FFMPEG=OFF`) — the `3rdparty/ffmpeg` submodule is
  `RPCS3/ffmpeg-core` (prebuilt downloader), which is what CI uses. Avoids the huge brew ffmpeg dep tree.
- `glew` is **not used on Apple** (`3rdparty/CMakeLists.txt` guards it with `NOT APPLE`) → not installed.
- OpenCV is optional in the CMake (`WARNING` not `FATAL` if missing) but we install brew `opencv`.
- `USE_SYSTEM_MVK=ON`, `USE_SYSTEM_SDL=ON`, `USE_SYSTEM_OPENCV=ON`.

## Log

### [entry 1] deps install started
- No `BUILD-STATUS.md` existed. Creating it now.
- Observed a background `brew update` (not ours) — waited for it to finish.
- Starting `brew install` of: `cmake ninja llvm qt sdl3 opencv vulkan-headers vulkan-loader molten-vk pkgconf googletest`.
  (Skipped: `glew` (unused on Apple), `ffmpeg` (builtin), `python@3.14` (punt until needed).)
- Log: `/private/var/folders/0x/tq2_tw6d7_lckb2kqwt23klc0000gn/T/opencode/rpcs3-build/brew-install.log`
- **BLOCKED on brew lock**: an interactive iTerm shell (not an opencode agent) is running
  `brew upgrade` over ~22 outdated formulae. Per protocol, NOT starting a concurrent `brew`.
  Doing the network-only submodule work while waiting.

### [entry 2] submodules initialized (network-only, no brew)
- Ran the CI submodule command (excluding llvm/opencv/libsdl-org/feralinteractive/curl/zlib):
  `git submodule -q update --init --depth=1 --jobs=8 <paths>` → 22 submodules.
- **Important finding:** `--depth=1` leaves many submodules checked out at the **branch tip**
  instead of the pinned SHA (and glslang's fetch outright failed: *"did not contain <sha>, direct
  fetching failed"*). Fix: re-fetch + checkout the exact recorded SHAs
  (`git -C <p> fetch --depth=1 origin <sha> && git checkout <sha>`) — GitHub serves arbitrary SHAs,
  so this works. Log: `.../rpcs3-build/pin-submodules.log`.
- **All submodules now match the recorded SHAs** (`git submodule status | grep -c '^+'` == 0).
  Uninitialized = the 6 CI-excluded ones (curl/feralinteractive/libsdl-org/llvm/opencv/zlib) — all
  intentionally unused (system LLVM/OpenCV/SDL/zlib/curl; feralinteractive is Linux-only).

### [entry 3] configure-time findings (pre-empting failures)
- `rpc3/Emu/CMakeLists.txt:25` uses `USE_LTO` → `INTERPROCEDURAL_OPTIMIZATION ON` for `rpcs3_emu`
  only. On an 8 GiB box, LTO link may OOM, so we pass **`-DUSE_LTO=OFF`** (only affects that lib).
- **curl**: `USE_SYSTEM_CURL` defaults ON → `find_package(CURL REQUIRED)`. macOS CLT provides
  `/usr/bin/curl-config` and `MacOSX.sdk/usr/include/curl/curl.h` → system curl works, no brew `curl`.
- **zlib**: `USE_SYSTEM_ZLIB` ON → SDK `libz` works.
- **`CMAKE_OSX_ARCHITECTURES=arm64` must be passed explicitly**: the root CMakeLists only appends
  `/opt/homebrew` to the search path when `CMAKE_OSX_ARCHITECTURES STREQUAL "arm64"`.
- Homebrew `llvm` is keg-only → pass `LLVM_DIR=/opt/homebrew/opt/llvm/lib/cmake/llvm`.
- `MACDEPLOYQT_EXECUTABLE` is referenced in `rpcs3/CMakeLists.txt:134` but never defined in-tree →
  expected to be provided by the Qt6 CMake package (will verify post-install).

### [entry 4] Homebrew dependency set chosen (trimmed)
First `brew install` attempt was killed after it resolved the dep trees, because two formulas pull
absurd trees:
- **`opencv` (5.0.0) → 70 deps** (gcc, vtk, openvino, tesseract, ffmpeg, python@3.14, ...). **Skipped**:
  RPCS3 only uses OpenCV for the PS Move camera tracker (`rpcs3/Input/ps_move_tracker.cpp`, guarded by
  `#ifdef HAVE_OPENCV`) — optional, CMake emits a WARNING when absent.
- **`qt` meta → 47 deps incl. `qtwebengine`.** Replaced with only the modules RPCS3 requires:
  **`qtbase qtsvg qtmultimedia qttools`** (Widgets/Concurrent/DBus/Gui/GuiPrivate, Svg/SvgWidgets,
  Multimedia/MultimediaWidgets; `qttools` provides `macdeployqt`). This still pulls qtdeclarative/
  qtquick3d via qtmultimedia but NOT qtwebengine.
- **`llvm` (23.1.3) → replaced with `llvm@22` (22.1.8)**: CI pins `LLVM_COMPILER_VER=22` and the
  LLVM submodule is 22.1.8; LLVM 23 is likely ABI/API-incompatible with RPCS3's JIT code.
- Also skipped `glew` (unused on Apple) and brew `ffmpeg` (builtin prebuilt), `curl` (system).

Final install list:
`cmake ninja llvm@22 qtbase qtsvg qtmultimedia qttools sdl3 vulkan-headers vulkan-loader molten-vk pkgconf googletest`
Resolved versions: cmake 4.4.4, ninja 1.13.2, llvm@22 22.1.8, qt 6.12.0, sdl3 3.4.18,
vulkan-headers 1.4.363.0, vulkan-loader 1.4.363.0, molten-vk 1.4.2, pkgconf 3.0.8, googletest 1.18.0.
- Note: the aborted first run logged a Homebrew sandbox `EPERM` on `var/homebrew/tmp/.cellar/verify-*`
  (likely a race with the concurrent interactive `brew upgrade`); the retry (single brew process)
  proceeded normally.
- Helper script written: `.ci/build-mac-homebrew.sh` (documents the exact configure/build commands).

### [entry 5] dependencies installed + configure OK
- `brew install` completed (exit 0): cmake 4.4.4, ninja 1.13.2, llvm@22 22.1.8, qtbase/qtsvg/
  qtmultimedia/qttools 6.12.0, sdl3 3.4.18, vulkan-headers/vulkan-loader 1.4.363.0, molten-vk 1.4.2,
  pkgconf 3.0.8, googletest 1.18.0. Installed size ~2.2 GB (llvm@22 alone 1.6 GB).
- **Submodule fixes required for configure:**
  * `3rdparty/yaml-cpp/yaml-cpp` had an **empty worktree** after the shallow init (only a `.git`
    pointer) → fixed with `git submodule update --init --force -- 3rdparty/yaml-cpp/yaml-cpp`.
- **Configure gotcha:** do **not** set `Qt6_DIR` to the `qtbase` keg. Homebrew lays out *all* Qt
  module configs under the unified `$(brew --prefix)/lib/cmake/Qt6`; pointing at the keg makes the
  component targets (`Qt6::Multimedia`, `Qt6::Svg`, …) unresolvable. Use
  `Qt6_DIR=/opt/homebrew/lib/cmake/Qt6`.
- **CMake 4.x:** passed `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` for the older 3rdparty subprojects.
- Configure exit 0. Only benign warnings (OpenCV missing → built without it; old cmake_minimum in
  protobuf/asmjit).
- Build launched: `ninja -C build -j4` (log: `.../rpcs3-build/build.log`).

### [entry 6] packaging script written
- `.ci/deploy-mac-homebrew.sh`: bundles the **Vulkan loader** (`libvulkan.1.dylib` from brew
  `vulkan-loader`) into `Contents/Frameworks/`, rewrites rpcs3's `libvulkan` reference to
  `@executable_path/../Frameworks/libvulkan.1.dylib`, copies `libMoltenVK.dylib` + rewrites
  `MoltenVK_icd.json`'s `library_path` to `../../../Frameworks/libMoltenVK.dylib`, thins to arm64,
  and ad-hoc signs. This addresses doc 11 §C6 (loader must be bundled for KK) — see the verification
  section once built.
- Notes: brew loader install_name is `/opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib`;
  brew MVK install_name `/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib` (system-frameworks only);
  brew MoltenVK ICD has `is_portability_driver: true`, `api_version: 1.4.0`.

### [entry 7] build compiled 1258/1258; final link failed (CoreVideo)
- `ninja -C build -j4` compiled **all 1258 objects with 0 compile errors**, but the
  **final link of `build/bin/rpcs3.app/Contents/MacOS/rpcs3` FAILED** (`ld: symbol(s) not found for architecture arm64`).
- All undefined symbols came from the builtin ffmpeg static lib **`libavutil.a[hwcontext_videotoolbox.o]`**:
  `_CVPixelBuffer*`, `_CVBuffer*`, `_kCVImageBuffer*`, `_CVColorPrimaries/TransferFunction/YCbCrMatrix…`.
  i.e. the **CoreVideo / VideoToolbox** frameworks were never linked.
- **Root cause:** in `3rdparty/CMakeLists.txt` the builtin-ffmpeg branch manually `find_library()`s the
  five ffmpeg core libs and links only those. The `ffmpeg` INTERFACE target defined by
  `3rdparty/ffmpeg/CMakeLists.txt` — which *does* add `-framework CoreVideo/VideoToolbox/CoreMedia/…`
  (lines 71–76) — is created via `add_subdirectory(ffmpeg …)` but **never linked** into `3rdparty_ffmpeg`.
  So the system-ffmpeg branch would have been fine, but the builtin branch dropped the frameworks.

### [entry 8] link fixed → BUILD SUCCEEDED
- **Fix (allowed path `3rdparty/*`):** added an `APPLE` guard in the builtin-ffmpeg branch of
  `3rdparty/CMakeLists.txt` that links the Apple frameworks onto the `3rdparty_ffmpeg` interface:
  ```cmake
  if (APPLE)
      target_link_libraries(3rdparty_ffmpeg INTERFACE
          "-framework CoreServices" "-framework CoreFoundation"
          "-framework CoreMedia" "-framework VideoToolbox"
          "-framework CoreVideo" "-framework Security")
  endif()
  ```
  (Mirrors the set already declared in `3rdparty/ffmpeg/CMakeLists.txt`.)
- Re-ran `ninja -C build -j4` → reconfigure + relink succeeded (exit 0). Only `ld` deprecation
  warnings remain (`built for macOS-15.0, but linking with … built for newer version 27.0` — harmless).
- `build/bin/rpcs3.app/Contents/MacOS/rpcs3` produced: **Mach-O 64-bit executable arm64**, 42 MB.
  The `POST_BUILD` `macdeployqt` step ran and bundled Qt (`QtCore/Gui/Widgets/Svg/SvgWidgets/
  Network/DBus/Concurrent/Multimedia/MultimediaWidgets` + support dylibs).

### [entry 9] packaged + verified + launches
- Ran `bash .ci/deploy-mac-homebrew.sh` (bundles loader + MVK ICD, thins, ad-hoc signs) — exit 0.
- **Verification:**
  * `otool -L …/MacOS/rpcs3` → `@executable_path/../Frameworks/libvulkan.1.dylib` (bundled loader). ✅
  * No `/opt/homebrew` refs remain in the main binary or in any bundled `Contents/Frameworks/*.dylib`. ✅
  * `Contents/Frameworks/`: `libvulkan.1.dylib` (loader, 679 KB) + `libMoltenVK.dylib` (5.3 MB). ✅
  * `Contents/Resources/vulkan/icd.d/MoltenVK_icd.json` → `library_path: ../../../Frameworks/libMoltenVK.dylib`,
    `is_portability_driver: true`, `api_version: 1.4.0`. ✅
  * `codesign -dv` → adhoc, app bundle thin arm64; `codesign --verify --deep` → **valid on disk / satisfies DR**. ✅
- **Launch test:** `build/bin/rpcs3.app/Contents/MacOS/rpcs3 --version` → prints
  `RPCS3 0.0.43-20311-17543d27 Alpha`, exit 0. GUI launch (no args) stays up, initializes
  `LLVM version: 22.1.8`, then reports `Missing Firmware` (expected — no PS3 firmware installed). ✅
  Also verified from a clean env (`env -i`) and from a different CWD (`/tmp`) — all bundled dylibs resolve.
- **Known caveat (dev machine only):** with Homebrew `molten-vk` installed, the loader scans BOTH the
  app-bundle ICD and the global `/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json`, so two copies of
  `libMoltenVK.dylib` load and the Obj-C runtime prints
  *"Class MVKBlockObserver is implemented in both …"*. Benign warning; on a clean machine (no
  Homebrew Vulkan/MoltenVK) only the bundled ICD exists and there is no duplicate. Mitigation one-liner:
  `VK_ICD_FILENAMES="$PWD/build/bin/rpcs3.app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json" build/bin/rpcs3.app/Contents/MacOS/rpcs3`
  (verified: silence the duplicate). No code/RSX change made.

## Result

- **Launchable app produced: YES** — `build/bin/rpcs3.app` (arm64, ad-hoc signed, self-contained Qt + Vulkan loader + MoltenVK ICD).
- **Bundled:** loader `Contents/Frameworks/libvulkan.1.dylib` (brew `vulkan-loader` 1.4.363.0);
  ICD `Contents/Frameworks/libMoltenVK.dylib` (brew `molten-vk` 1.4.2) + `Contents/Resources/vulkan/icd.d/MoltenVK_icd.json`.
  (KosmicKrisp ICD **not** bundled/selected — out of scope.)
- **Reproduce:**
  1. `bash .ci/build-mac-homebrew.sh`   (deps already installed; configure + `ninja` build)
  2. `bash .ci/deploy-mac-homebrew.sh`  (bundle Vulkan loader + MoltenVK ICD, thin, sign)
- **Dry-run (one line):** `build/bin/rpcs3.app/Contents/MacOS/rpcs3 --version`
- **GUI:** `open build/bin/rpcs3.app`
- **Blockers:** none. Only the benign duplicate-MoltenVK warning when Homebrew `molten-vk` is also
  installed (see entry 9). `docs/research/BUILD-STATUS.md` and the app are uncommitted working-tree changes.

_(entries appended as work proceeds)_
