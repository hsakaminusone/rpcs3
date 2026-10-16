#!/bin/sh -e
# Build and bundle the KosmicKrisp Vulkan ICD into an RPCS3 .app.
#
# KosmicKrisp (VkDriverId 28) is a pure ICD: it does not export the public vk*
# API, is Apple-Silicon only and needs macOS 26+ at runtime. RPCS3 ships no
# KosmicKrisp binary of its own, so the driver is built here from the same source
# as the official LunarG macOS Vulkan SDK and bundled next to MoltenVK.
#
# A driver built by an earlier run is reused from KK_CACHE_DIR, so CI can cache it
# between runs. The cache holds the two files Mesa produces, so deleting it forces
# a rebuild.
#
# Usage: .ci/build-kosmickrisp.sh [path/to/rpcs3.app]   (default build/bin/rpcs3.app)
#
# Skipped (non-fatally) for non-arm64 bundles, where KosmicKrisp cannot load at all.

APP="${1:-build/bin/rpcs3.app}"
FRAMEWORKS="$APP/Contents/Frameworks"
ICD_DIR="$APP/Contents/Resources/vulkan/icd.d"
BIN="$APP/Contents/MacOS/rpcs3"

KK_CACHE_DIR="${KK_CACHE_DIR:-/tmp/rpcs3-kosmickrisp}"
KK_MESA_REPO="https://gitlab.freedesktop.org/aitor/mesa.git"
KK_MESA_TAG="vulkan-sdk-1.4.363.0"
KK_LIBRARY="libvulkan_kosmickrisp.dylib"
KK_MANIFEST="kosmickrisp_mesa_icd.aarch64.json"

[ -x "$BIN" ] || { echo "ERROR: no executable at $BIN"; exit 1; }

case " $(lipo -archs "$BIN" 2>/dev/null) " in
  *" arm64 "*) ;;
  *) echo "== Skipping KosmicKrisp: bundle is not arm64 =="; exit 0 ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ -f "$KK_CACHE_DIR/$KK_LIBRARY" ]; then
  echo "== Reusing KosmicKrisp from $KK_CACHE_DIR =="
else
  echo "== Building KosmicKrisp $KK_MESA_TAG =="
  LLVM_VER="${LLVM_COMPILER_VER:-22}"
  sudo port -N install meson ninja pkgconfig "libclc-$LLVM_VER" spirv-tools "spirv-llvm-translator-$LLVM_VER"
  export PATH="/opt/local/libexec/llvm-$LLVM_VER/bin:/opt/local/bin:/opt/local/sbin:$PATH"
  export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"

  git clone --depth 1 --branch "$KK_MESA_TAG" "$KK_MESA_REPO" "$TMP/mesa"
  meson setup "$TMP/build" "$TMP/mesa" \
      --buildtype=release \
      --prefer-static \
      -Dplatforms=macos \
      -Dvulkan-drivers=kosmickrisp \
      -Dgallium-drivers= \
      -Dopengl=false \
      -Dzstd=disabled
  meson compile -C "$TMP/build"

  mkdir -p "$KK_CACHE_DIR"
  cp "$TMP/build/src/kosmickrisp/vulkan/$KK_LIBRARY" "$KK_CACHE_DIR/$KK_LIBRARY"
  cp "$TMP/build/src/kosmickrisp/vulkan/$KK_MANIFEST" "$KK_CACHE_DIR/$KK_MANIFEST"
fi

echo "== Bundling KosmicKrisp ICD =="
mkdir -p "$FRAMEWORKS" "$ICD_DIR"

cp -f "$KK_CACHE_DIR/$KK_LIBRARY" "$FRAMEWORKS/$KK_LIBRARY"
chmod u+w "$FRAMEWORKS/$KK_LIBRARY"
install_name_tool -id "@rpath/$KK_LIBRARY" "$FRAMEWORKS/$KK_LIBRARY"

# Copy the non-system dependency closure and repoint every reference at the bundle.
# Mesa links the driver against the prefix its dependencies came from, so this
# normally picks up libSPIRV-Tools.dylib, which the packages only ship as a dylib.
CLOSURE="$TMP/closure.txt"
echo "$FRAMEWORKS/$KK_LIBRARY" > "$CLOSURE"

line=1
while [ "$line" -le "$(wc -l < "$CLOSURE" | tr -d ' ')" ]; do
  current="$(sed -n "${line}p" "$CLOSURE")"
  line=$((line + 1))

  for dep in $(otool -L "$current" | tail -n +2 | awk '{print $1}'); do
    case "$dep" in
      /usr/lib/*|/System/*|@*) continue ;;
    esac
    [ -e "$dep" ] || { echo "  WARN: missing dependency $dep"; continue; }
    grep -qxF "$dep" "$CLOSURE" || echo "$dep" >> "$CLOSURE"
  done
done

while IFS= read -r dep; do
  if [ "$dep" = "$FRAMEWORKS/$KK_LIBRARY" ]; then
    continue
  fi
  base="$(basename "$dep")"
  cp -f "$dep" "$FRAMEWORKS/$base"
  chmod u+w "$FRAMEWORKS/$base"
  install_name_tool -id "@rpath/$base" "$FRAMEWORKS/$base"
done < "$CLOSURE"

while IFS= read -r target; do
  for dep in $(otool -L "$target" | tail -n +2 | awk '{print $1}'); do
    case "$dep" in
      /usr/lib/*|/System/*|@*) continue ;;
    esac
    install_name_tool -change "$dep" "@executable_path/../Frameworks/$(basename "$dep")" "$target"
  done
done < "$CLOSURE"

# Rewrite the manifest for the bundled layout, keeping Mesa's api_version.
KK_API_VERSION="$(sed -n 's/.*"api_version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$KK_CACHE_DIR/$KK_MANIFEST" | head -n 1)"
[ -n "$KK_API_VERSION" ] || { echo "ERROR: no api_version in $KK_CACHE_DIR/$KK_MANIFEST"; exit 1; }

cat > "$ICD_DIR/$KK_MANIFEST" <<EOF
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../../Frameworks/$KK_LIBRARY",
        "api_version": "$KK_API_VERSION"
    }
}
EOF

echo "== KosmicKrisp vendored artifacts (sha256) =="
shasum -a 256 "$FRAMEWORKS/$KK_LIBRARY" "$ICD_DIR/$KK_MANIFEST"
