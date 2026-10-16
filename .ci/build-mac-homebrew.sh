#!/bin/sh -e
# Local macOS build of RPCS3 using ONLY Homebrew dependencies (no MacPorts).
# Adapted from .ci/build-mac.sh for a Homebrew-only environment.
#
# Usage (from anywhere):
#   bash .ci/build-mac-homebrew.sh
#
# Environment knobs:
#   JOBS=<n>          parallel build jobs (default 4; this box has 6 CPU / 8 GiB RAM)
#   BUILD_TYPE=...    CMAKE_BUILD_TYPE (default Release)
#
# Notes / rationale (see docs/research/BUILD-STATUS.md):
#   * Uses Homebrew llvm@22 (NOT llvm 23) because RPCS3 pins LLVM 22 (CI LLVM_COMPILER_VER=22,
#     submodule 22.1.8). llvm@22 is keg-only, so LLVM_DIR is set explicitly.
#   * Qt: only the modules RPCS3 needs are installed (qtbase/qtSvg/qtmultimedia/qttools) to avoid
#     pulling qtwebengine et al. qttools provides macdeployqt.
#   * OpenCV is intentionally NOT installed: it is optional in RPCS3 (only used for the PS Move
#     camera tracker behind HAVE_OPENCV) and Homebrew's OpenCV 5 pulls ~70 deps (gcc/vtk/openvino...).
#     CMake emits a harmless "OpenCV not found" WARNING.
#   * ffmpeg is the builtin RPCS3 prebuilt download (USE_SYSTEM_FFMPEG=OFF), as CI does.
#   * USE_LTO=OFF: on 8 GiB RAM the LTO link of rpcs3_emu can OOM; it only affects that one lib.

WORKDIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$WORKDIR"

BREW_PREFIX="$(brew --prefix)"
LLVM_PREFIX="$(brew --prefix llvm@22 2>/dev/null || echo "$BREW_PREFIX/opt/llvm@22")"
# Homebrew lays out every Qt module under the *unified* $BREW_PREFIX/lib/cmake, so the package
# root must be that (not the qtbase keg), otherwise component targets (Qt6::Multimedia, ...) are
# not found.
QT6_CMAKE_DIR="$(brew --prefix)/lib/cmake/Qt6"
QT_BIN="$BREW_PREFIX/opt/qtbase/bin"
SDL3_PREFIX="$(brew --prefix sdl3)"

JOBS="${JOBS:-4}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

# Make macdeployqt/pkg-config visible; keep Apple clang as the compiler (llvm@22/bin last).
export PATH="$QT_BIN:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin:$LLVM_PREFIX/bin:$PATH"

# 1. Submodules (same exclusions as CI: system llvm/opencv/sdl/zlib/curl; feralinteractive is Linux-only)
git submodule -q update --init --depth=1 --jobs=8 \
  $(awk '/path/ && !/llvm/ && !/opencv/ && !/libsdl-org/ && !/feralinteractive/ && !/curl/ && !/zlib/ { print $3 }' .gitmodules)

COMM_TAG=$(awk '/version{.*}/ { printf("%d.%d.%d", $5, $6, $7) }' rpcs3/rpcs3_version.cpp)
COMM_COUNT=$(git rev-list --count HEAD)

# 2. Configure
cmake -S "$WORKDIR" -B "$WORKDIR/build" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)" \
  -DCMAKE_PREFIX_PATH="$BREW_PREFIX;$LLVM_PREFIX;$QT6_CMAKE_DIR/.." \
  -DMACOSX_BUNDLE_SHORT_VERSION_STRING="$COMM_TAG" \
  -DMACOSX_BUNDLE_BUNDLE_VERSION="$COMM_COUNT" \
  -DWITH_LLVM=ON -DBUILD_LLVM=OFF -DSTATIC_LINK_LLVM=OFF \
  -DLLVM_DIR="$LLVM_PREFIX/lib/cmake/llvm" \
  -DQt6_DIR="$QT6_CMAKE_DIR" \
  -DSDL3_DIR="$SDL3_PREFIX/lib/cmake/SDL3" \
  -DUSE_SDL=ON -DUSE_SYSTEM_SDL=ON \
  -DUSE_SYSTEM_MVK=ON \
  -DUSE_SYSTEM_OPENCV=ON \
  -DUSE_SYSTEM_FFMPEG=OFF \
  -DUSE_AUDIOUNIT=ON \
  -DUSE_NATIVE_INSTRUCTIONS=OFF \
  -DUSE_PRECOMPILED_HEADERS=OFF \
  -DUSE_LTO=OFF \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# 3. Build
cmake --build "$WORKDIR/build" --parallel "$JOBS"

echo "Built: $WORKDIR/build/bin/rpcs3.app"
