#!/bin/bash
# Builds the SkinFlaps macOS installer package.
#
# Produces installer/out/SkinFlaps-<version>.pkg containing a self-contained
# SkinFlaps.app (Homebrew dylibs bundled) that installs to /Applications,
# with the license and surgical disclaimer presented during installation.
#
# Usage:  installer/build_installer.sh [version]
#
# Signing is ad-hoc by default (first launch then needs the Gatekeeper
# bypass). For a package users can install with no security steps, set:
#   SKINFLAPS_APP_IDENTITY="Developer ID Application: <name> (<team id>)"
#   SKINFLAPS_PKG_IDENTITY="Developer ID Installer: <name> (<team id>)"
#   SKINFLAPS_NOTARY_PROFILE=<notarytool keychain profile>   (optional)
# With SKINFLAPS_NOTARY_PROFILE set, the signed package is submitted to
# Apple's notary service and the ticket is stapled, so it installs cleanly
# even offline.
set -euo pipefail

VERSION="${1:-1.1.5}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO/installer/out"
mkdir -p "$OUT"
# Stage in a fresh temp dir each run. A prior install of a relocatable build
# can leave root-owned copies behind that a normal rm cannot clear, so never
# reuse a fixed path. Cleaned up on exit.
STAGE="$(mktemp -d "$OUT/stage.XXXXXX")"
ROOT="$(mktemp -d "$OUT/root.XXXXXX")"
APP="$STAGE/SkinFlaps.app"
trap 'chmod -R u+w "$STAGE" "$ROOT" 2>/dev/null; rm -rf "$STAGE" "$ROOT"' EXIT

command -v dylibbundler >/dev/null || {
  echo "dylibbundler not found: brew install dylibbundler" >&2; exit 1; }

cmake --preset gui-perf -S "$REPO"
cmake --build "$REPO/build/gui-perf" -j --target SkinFlaps

cp -R "$REPO/build/gui-perf/SkinFlaps/SkinFlaps.app" "$APP"

# The bundle's version fields must agree with the package version the user
# asked for, whatever the build tree was configured with.
PL="$APP/Contents/Info.plist"
for kv in "CFBundleShortVersionString $VERSION" "CFBundleVersion $VERSION"; do
  k="${kv%% *}"; v="${kv#* }"
  /usr/libexec/PlistBuddy -c "Set :$k $v" "$PL" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Add :$k string $v" "$PL"
done

# Read-only data ships inside the bundle; the app seeds the user-writable
# ~/SkinFlaps/History from Resources/History on first launch.
mkdir -p "$APP/Contents/Resources"
cp -R "$REPO/Model" "$APP/Contents/Resources/Model"
mkdir -p "$APP/Contents/Resources/History"
cp "$REPO/History/"*.hst "$APP/Contents/Resources/History/"
# Only the upstream example procedures and models ship: refuse any extra or
# modified file rather than propagate it to users' History folders. The set is
# a subset of upstream's: FurlowPalateRepair.hst is absent because the palate
# scene it loads is not part of the model set.
VENDOR="$REPO/vendor/original_windows/SkinFlaps"
for f in "$APP/Contents/Resources/History/"*.hst; do
  cmp -s "$f" "$VENDOR/History/$(basename "$f")" || { echo "non-upstream history in package: $f" >&2; exit 1; }
done
for f in $(cd "$APP/Contents/Resources/Model" && find . -type f); do
  cmp -s "$APP/Contents/Resources/Model/$f" "$VENDOR/Model/$f" || { echo "non-upstream model file in package: $f" >&2; exit 1; }
done
# The license and disclaimer the installer shows travel inside the app too.
for f in License.rtf SurgicalDisclaimer.txt; do
  [ -f "$APP/Contents/Resources/$f" ] || cp "$REPO/installer/resources/$f" "$APP/Contents/Resources/"
done

# Bundle the Homebrew runtime (GLFW, TBB, SuiteSparse chain) so the app runs
# on machines without Homebrew.
dylibbundler -od -b \
  -x "$APP/Contents/MacOS/SkinFlaps" \
  -d "$APP/Contents/libs" \
  -p "@executable_path/../libs/" > /dev/null

# Sign inside-out (each bundled dylib, then the app). Notarization requires
# hardened runtime and secure timestamps on every Mach-O.
if [ -n "${SKINFLAPS_APP_IDENTITY:-}" ]; then
  for lib in "$APP/Contents/libs/"*.dylib; do
    codesign --force --options runtime --timestamp \
      --sign "$SKINFLAPS_APP_IDENTITY" "$lib"
  done
  codesign --force --options runtime --timestamp \
    --sign "$SKINFLAPS_APP_IDENTITY" "$APP"
else
  codesign --force --deep --sign - "$APP"
fi
codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | tail -2

# Disable bundle relocation. By default pkgbuild marks app components
# relocatable, so if a bundle with this identifier already exists elsewhere
# on disk (e.g. a build tree Spotlight has indexed) the installer targets
# that copy instead of /Applications. Pin it to /Applications.
ditto "$APP" "$ROOT/SkinFlaps.app"
cat > "$OUT/component.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<array>
  <dict>
    <key>BundleHasStrictIdentifier</key><true/>
    <key>BundleIsRelocatable</key><false/>
    <key>BundleIsVersionChecked</key><true/>
    <key>BundleOverwriteAction</key><string>upgrade</string>
    <key>RootRelativeBundlePath</key><string>SkinFlaps.app</string>
  </dict>
</array>
</plist>
PLIST
pkgbuild \
  --root "$ROOT" \
  --install-location /Applications \
  --component-plist "$OUT/component.plist" \
  --identifier io.github.christian-arcelona.skinflaps.app \
  --version "$VERSION" \
  "$OUT/SkinFlapsApp.pkg"

DIST="$OUT/distribution.xml"
sed -E 's/(<pkg-ref [^>]*version=")[^"]*"/\1'"$VERSION"'"/' \
  "$REPO/installer/distribution.xml" > "$DIST"

PRODUCT="$OUT/SkinFlaps-$VERSION.pkg"
if [ -n "${SKINFLAPS_PKG_IDENTITY:-}" ]; then
  productbuild --distribution "$DIST" \
    --resources "$REPO/installer/resources" \
    --package-path "$OUT" \
    --sign "$SKINFLAPS_PKG_IDENTITY" \
    "$PRODUCT"
else
  productbuild --distribution "$DIST" \
    --resources "$REPO/installer/resources" \
    --package-path "$OUT" \
    "$PRODUCT"
fi

if [ -n "${SKINFLAPS_NOTARY_PROFILE:-}" ]; then
  xcrun notarytool submit "$PRODUCT" \
    --keychain-profile "$SKINFLAPS_NOTARY_PROFILE" --wait
  xcrun stapler staple "$PRODUCT"
  spctl -a -vv -t install "$PRODUCT"
fi

rm -f "$OUT/SkinFlapsApp.pkg" "$OUT/component.plist" "$DIST"
echo "Built $PRODUCT"
shasum -a 256 "$PRODUCT"
