#!/bin/sh -e
# Package build/bin/rpcs3.app for macOS with Homebrew-provided Vulkan runtime.
#
# Adapted from .ci/deploy-mac.sh, but:
#   * no wget of the MoltenVK tarball — uses Homebrew's molten-vk keg directly;
#   * BUNDLES THE VULKAN LOADER (libvulkan.1.dylib). This is a hard prerequisite for KosmicKrisp
#     (a pure ICD that cannot be linked/loaded as if it were the loader) and is recommended by
#     LunarG even for MoltenVK-only bundles. See docs/research/11 §C6.
#
# Usage: bash .ci/deploy-mac-homebrew.sh [path/to/rpcs3.app]   (default build/bin/rpcs3.app)

APP="${1:-build/bin/rpcs3.app}"
BREW_PREFIX="$(brew --prefix)"
LOADER_SRC="$BREW_PREFIX/opt/vulkan-loader/lib/libvulkan.1.dylib"
MVK_SRC="$BREW_PREFIX/opt/molten-vk/lib/libMoltenVK.dylib"
MVK_ICD_SRC="$BREW_PREFIX/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json"
BIN="$APP/Contents/MacOS/rpcs3"

[ -x "$BIN" ] || { echo "ERROR: no executable at $BIN"; exit 1; }

mkdir -p "$APP/Contents/Frameworks" "$APP/Contents/Resources/vulkan/icd.d"

echo "== Bundling Vulkan loader =="
cp -f "$LOADER_SRC" "$APP/Contents/Frameworks/libvulkan.1.dylib"
chmod u+w "$APP/Contents/Frameworks/libvulkan.1.dylib"
install_name_tool -id "@rpath/libvulkan.1.dylib" "$APP/Contents/Frameworks/libvulkan.1.dylib" || true
# Point every reference in rpcs3 (and bundled libs) at the in-bundle loader.
for f in "$BIN" "$APP/Contents/Frameworks/"*.dylib; do
  [ -f "$f" ] || continue
  for old in $(otool -L "$f" 2>/dev/null | awk '/libvulkan/{print $1}'); do
    echo "  $f: $old -> @executable_path/../Frameworks/libvulkan.1.dylib"
    install_name_tool -change "$old" "@executable_path/../Frameworks/libvulkan.1.dylib" "$f"
  done
done

echo "== Bundling MoltenVK ICD =="
cp -f "$MVK_SRC" "$APP/Contents/Frameworks/libMoltenVK.dylib"
chmod u+w "$APP/Contents/Frameworks/libMoltenVK.dylib"
install_name_tool -id "@rpath/libMoltenVK.dylib" "$APP/Contents/Frameworks/libMoltenVK.dylib" || true
# library_path is relative to Contents/Resources/vulkan/icd.d/  =>  ../../../Frameworks/
sed 's#"library_path".*#"library_path": "../../../Frameworks/libMoltenVK.dylib",#' \
  "$MVK_ICD_SRC" > "$APP/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json"

echo "== Bundling KosmicKrisp ICD (arm64 only) =="
bash "$(dirname "$0")/bundle-kosmickrisp.sh" "$APP"

echo "== Pruning Qt bits RPCS3 does not use =="
rm -rf "$APP/Contents/Frameworks/QtPdf.framework" \
       "$APP/Contents/Frameworks/QtQml.framework" \
       "$APP/Contents/Frameworks/QtQmlModels.framework" \
       "$APP/Contents/Frameworks/QtQuick.framework" \
       "$APP/Contents/Frameworks/QtVirtualKeyboard.framework" \
       "$APP/Contents/Plugins/platforminputcontexts" \
       "$APP/Contents/Plugins/virtualkeyboard" \
       "$APP/Contents/Resources/git" || true

echo "== Thinning to host arch =="
if [ -x .ci/optimize-mac.sh ]; then
  bash .ci/optimize-mac.sh "$APP" >/dev/null 2>&1 || echo "  (optimize-mac.sh skipped/failed; non-fatal)"
fi

echo "== Ad-hoc signing =="
find "$APP" -type f \( -name "*.dylib" -o -name "*.framework" \) -print0 2>/dev/null | xargs -0 -n1 codesign --force -s - 2>/dev/null || true
codesign --force --deep -s - "$APP"

echo "== Done: $APP =="
