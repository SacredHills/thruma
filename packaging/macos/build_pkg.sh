#!/usr/bin/env bash
# Build the macOS installer package (and a zip) for Thruma.vst3.
#
# Usage: packaging/macos/build_pkg.sh <path/to/Thruma.vst3> <version> <output-dir>
#
# Optional signing / notarisation (all skipped when the variables are empty):
#   APPLE_CODESIGN_IDENTITY    "Developer ID Application: Name (TEAMID)"   -> signs the plug-in (hardened runtime)
#   APPLE_INSTALLER_IDENTITY   "Developer ID Installer: Name (TEAMID)"     -> signs the .pkg
#   APPLE_ID, APPLE_APP_PASSWORD, APPLE_TEAM_ID                           -> notarises and staples the .pkg
set -euo pipefail

VST3="$1"
VERSION="$2"
OUT="$3"
mkdir -p "$OUT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/root"
cp -R "$VST3" "$WORK/root/"
BUNDLE="$WORK/root/$(basename "$VST3")"

if [[ -n "${APPLE_CODESIGN_IDENTITY:-}" ]]; then
    echo "Signing plug-in with: $APPLE_CODESIGN_IDENTITY"
    codesign --force --deep --options runtime --timestamp --sign "$APPLE_CODESIGN_IDENTITY" "$BUNDLE"
    codesign --verify --strict --verbose=2 "$BUNDLE"
else
    echo "No APPLE_CODESIGN_IDENTITY: ad-hoc signing (fine for local use; Gatekeeper will warn on other Macs)"
    codesign --force --deep --sign - "$BUNDLE"
fi

lipo -info "$BUNDLE/Contents/MacOS/Thruma" || true

# zip for manual installation
( cd "$WORK/root" && ditto -c -k --keepParent "$(basename "$VST3")" "$OUT/Thruma-$VERSION-macOS-VST3.zip" )

pkgbuild --root "$WORK/root" \
         --install-location "/Library/Audio/Plug-Ins/VST3" \
         --identifier "com.sacredhills.thruma.vst3" \
         --version "$VERSION" \
         "$WORK/Thruma-component.pkg"

cat > "$WORK/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Thruma $VERSION</title>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true"/>
    <choices-outline><line choice="vst3"/></choices-outline>
    <choice id="vst3" title="Thruma VST3"><pkg-ref id="com.sacredhills.thruma.vst3"/></choice>
    <pkg-ref id="com.sacredhills.thruma.vst3" version="$VERSION">Thruma-component.pkg</pkg-ref>
</installer-gui-script>
EOF

PKG="$OUT/Thruma-$VERSION-macOS-Installer.pkg"
if [[ -n "${APPLE_INSTALLER_IDENTITY:-}" ]]; then
    productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" --sign "$APPLE_INSTALLER_IDENTITY" "$PKG"
else
    productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" "$PKG"
fi

if [[ -n "${APPLE_ID:-}" && -n "${APPLE_APP_PASSWORD:-}" && -n "${APPLE_TEAM_ID:-}" && -n "${APPLE_INSTALLER_IDENTITY:-}" ]]; then
    echo "Notarising..."
    xcrun notarytool submit "$PKG" --apple-id "$APPLE_ID" --password "$APPLE_APP_PASSWORD" --team-id "$APPLE_TEAM_ID" --wait
    xcrun stapler staple "$PKG"
else
    echo "Notarisation skipped (credentials not provided)."
fi

echo "Built: $PKG"
