#!/bin/sh -e
# Bundle the KosmicKrisp Vulkan ICD into an RPCS3 .app.
#
# KosmicKrisp (VkDriverId 28) is a pure ICD: it does not export the public vk* API,
# is Apple-Silicon only and needs macOS 26+. The pinned artifact is the KosmicKrisp
# driver extracted from the LunarG Vulkan SDK, published as a release asset.
# Pin, provenance and sha256: docs/research/13-kosmickrisp-bundle-ledger.md
#
# Usage: .ci/bundle-kosmickrisp.sh [path/to/rpcs3.app]   (default build/bin/rpcs3.app)
#
# Skipped (non-fatally) for non-arm64 bundles, where KosmicKrisp cannot load at all.

APP="${1:-build/bin/rpcs3.app}"
FRAMEWORKS="$APP/Contents/Frameworks"
ICD_DIR="$APP/Contents/Resources/vulkan/icd.d"
BIN="$APP/Contents/MacOS/rpcs3"

KK_VERSION="1.4.363.0"
KK_URL="https://github.com/maluoi/kosmic_krisp_mirror/releases/download/${KK_VERSION}/kosmickrisp.zip"
KK_ARCHIVE_SHA256="0daad483a931f0855582e7c254f06d24ff8d729e7d2adb8c59ead5f0ecc64394"
KK_LIBRARY="libvulkan_kosmickrisp.dylib"
KK_MANIFEST="libkosmickrisp_icd.json"

[ -x "$BIN" ] || { echo "ERROR: no executable at $BIN"; exit 1; }

case " $(lipo -archs "$BIN" 2>/dev/null) " in
  *" arm64 "*) ;;
  *) echo "== Skipping KosmicKrisp: bundle is not arm64 =="; exit 0 ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== Fetching KosmicKrisp $KK_VERSION =="
curl -fsSL --retry 3 "$KK_URL" -o "$TMP/kosmickrisp.zip"
echo "$KK_ARCHIVE_SHA256  $TMP/kosmickrisp.zip" | shasum -a 256 -c - > /dev/null
(cd "$TMP" && unzip -oq kosmickrisp.zip)

mkdir -p "$FRAMEWORKS" "$ICD_DIR"

echo "== Bundling KosmicKrisp ICD =="
cp -f "$TMP/$KK_LIBRARY" "$FRAMEWORKS/$KK_LIBRARY"
chmod u+w "$FRAMEWORKS/$KK_LIBRARY"
install_name_tool -id "@rpath/$KK_LIBRARY" "$FRAMEWORKS/$KK_LIBRARY"

# Copy the non-system dependency closure and repoint every reference at the bundle.
# The pinned artifact only links system frameworks/libs, so this is normally empty,
# but it keeps the step correct if the artifact is ever rebuilt with extra deps.
CLOSURE="$TMP/closure.txt"
: > "$CLOSURE"
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

# Rewrite the manifest for the bundled layout, keeping the artifact's api_version.
KK_API_VERSION="$(sed -n 's/.*"api_version"[^"]*"\([^"]*\)".*/\1/p' "$TMP/$KK_MANIFEST" | head -n 1)"
[ -n "$KK_API_VERSION" ] || KK_API_VERSION="1.4.0"

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
