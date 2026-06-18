#!/bin/bash
#==============================================================================
# Space Dust - Build + package macOS installer (.pkg / .dmg)
#
# 1. (Optional) Re-configures + builds a CLEAN Release universal binary with
#    logging explicitly disabled (mirrors the Windows package-installer.ps1
#    discipline). On macOS the relevant flags are the CMake options; there is
#    no equivalent to VLD but we still honor ENABLE_MEMORY_SAFETY_LOGGING=OFF.
# 2. Stages the three artefacts (VST3, AU, Standalone) from the chosen build
#    tree into a payload layout suitable for /Library and /Applications.
# 3. Stages the factory presets (shared with the Windows installer under
#    installer/Files/Presets/*.sdpreset) plus the user-facing README into the
#    package scripts bundle so the postinstall can copy them into the user's
#    ~/Documents/Space Dust/Presets without clobbering existing user presets.
# 4. Builds the component packages (VST3, AU, Standalone, Uninstaller) and
#    generates Distribution.xml. The UI exposes two user-visible choices
#    (VST3+AU and Standalone); the uninstaller is always installed via a
#    hidden choice (never a user option).
# 5. Assembles a product .pkg via productbuild (with Distribution.xml for the
#    choice UI). If signing identities are available (or passed), the inner
#    bundles are codesigned with the Application cert (hardened runtime) and
#    the final .pkg is signed with the Installer cert.
# 6. Optionally wraps the signed/unsigned .pkg in a .dmg for distribution.
#
# The resulting installer:
#   - Drops VST3 into   /Library/Audio/Plug-Ins/VST3/
#   - Drops AU into     /Library/Audio/Plug-Ins/Components/
#   - Drops Standalone into /Applications/ (if the user leaves the choice ticked)
#   - Runs postinstall which installs the 50+ factory .sdpreset files into the
#     user's Documents (only files that don't already exist) and writes the
#     config.xml that PresetManager reads.
#
# IMPORTANT (per project rules):
#   - This script only touches packaging / staging / signing of the *output*
#     artefacts. It never modifies Source/, CMakeLists.txt, or any runtime code.
#   - The same source tree produces the Windows build. No Mac-only branch.
#   - Notarization is deliberately left as a *subsequent* manual (or later
#     scripted) step. Run this script first to prepare the package, then
#     notarytool + stapler once Apple login / credentials are working.
#
# Usage examples:
#   ./package-macos.sh
#   ./package-macos.sh --skip-build
#   ./package-macos.sh --sign
#   ./package-macos.sh --build-dir build --skip-dmg
#
# Flags:
#   --skip-build           Reuse whatever is already in the build dir (faster
#                          iteration). You are still responsible for having
#                          produced a clean Release build with logging OFF.
#   --build-dir DIR        Explicit build tree (default: build-release if it
#                          contains artefacts, otherwise build).
#   --sign                 Attempt to sign. The script will look for
#                          "Developer ID Application" and "Developer ID
#                          Installer" certificates via security find-identity.
#                          Use the --*-identity flags to override.
#   --app-identity STR     Full "Developer ID Application: Name (TEAMID)" string.
#   --installer-identity   Full "Developer ID Installer: Name (TEAMID)" string.
#   --skip-presets         Do not touch the staged presets (rarely needed).
#   --skip-dmg             Only produce the .pkg, do not wrap it in a .dmg.
#   --out-dir DIR          Output directory for the final .pkg/.dmg
#                          (default: installer/Output).
#==============================================================================

set -euo pipefail

# --- Resolve project root (script may be run from anywhere) -----------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# --- Product version (single source of truth: CMakeLists.txt) ---------------
# Parse "project(SpaceDust VERSION X.Y.Z)" so the installer metadata and the
# output .pkg/.dmg filenames always track the real plugin version and never
# drift from the binary again.
VERSION="$(sed -n -E 's/^project\(SpaceDust VERSION ([0-9.]+)\).*/\1/p' CMakeLists.txt | head -1)"
if [[ -z "$VERSION" ]]; then
    echo "[Package] ERROR: could not parse version from CMakeLists.txt (project(SpaceDust VERSION ...))." >&2
    exit 1
fi

# Allow overriding cmake (this machine often needs a manually downloaded 3.30+ because
# the system/brew version is too old or broken). You can also set CMAKE_BIN env var.
if [[ -z "${CMAKE_BIN:-}" ]]; then
    for c in \
        "/tmp/cmake-3.30.5-macos-universal/CMake.app/Contents/bin/cmake" \
        "/Applications/CMake.app/Contents/bin/cmake" \
        "$(command -v cmake 2>/dev/null || true)"
    do
        if [[ -x "$c" ]]; then
            CMAKE_BIN="$c"
            break
        fi
    done
fi
if [[ -z "$CMAKE_BIN" || ! -x "$CMAKE_BIN" ]]; then
    echo "[Package] ERROR: No working cmake 3.30+ found." >&2
    echo "          Set CMAKE_BIN=/full/path/to/cmake or install one." >&2
    exit 1
fi
echo "[Package] Using cmake: $CMAKE_BIN"

# --- Defaults ---------------------------------------------------------------
SKIP_BUILD=0
BUILD_DIR=""
SIGN=0
APP_IDENTITY=""
INSTALLER_IDENTITY=""
SKIP_PRESETS=0
SKIP_DMG=0
OUT_DIR="installer/Output"

# --- Parse args -------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1; shift ;;
        --build-dir)  BUILD_DIR="$2"; shift 2 ;;
        --sign)       SIGN=1; shift ;;
        --app-identity)       APP_IDENTITY="$2"; shift 2 ;;
        --installer-identity) INSTALLER_IDENTITY="$2"; shift 2 ;;
        --skip-presets) SKIP_PRESETS=1; shift ;;
        --skip-dmg)   SKIP_DMG=1; shift ;;
        --out-dir)    OUT_DIR="$2"; shift 2 ;;
        -h|--help)
            sed -n '1,/^#===/p' "$0" | tail -n +2
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            echo "Run with --help for usage." >&2
            exit 1
            ;;
    esac
done

mkdir -p "$OUT_DIR"

echo "======================================================================"
echo "Space Dust macOS Packaging (version $VERSION)"
echo "======================================================================"

# --- Locate build tree (prefer build-release for "release" discipline) ------
if [[ -z "$BUILD_DIR" ]]; then
    if [[ -d "build-release/SpaceDust_artefacts/Release/VST3" ]]; then
        BUILD_DIR="build-release"
    elif [[ -d "build/SpaceDust_artefacts/Release/VST3" ]]; then
        BUILD_DIR="build"
    else
        echo "[Package] ERROR: No build artefacts found in build-release/ or build/." >&2
        echo "          Run a Release build first (see MAC_BUILD.md) or pass --build-dir." >&2
        exit 1
    fi
fi

ARTEFACTS="$BUILD_DIR/SpaceDust_artefacts/Release"
VST3_SRC="$ARTEFACTS/VST3/Space Dust.vst3"
AU_SRC="$ARTEFACTS/AU/Space Dust.component"
SA_SRC="$ARTEFACTS/Standalone/Space Dust.app"

echo "[Package] Using build tree : $BUILD_DIR"
echo "[Package] Artefacts        : $ARTEFACTS"

# --- (Optional) Clean re-build with logging forced OFF ----------------------
# This mirrors the Windows package-installer.ps1 behaviour. On macOS the
# important thing is that a prior dev build with ENABLE_MEMORY_SAFETY_LOGGING=ON
# does not accidentally leak into the shipped universal binaries.
if [[ $SKIP_BUILD -eq 0 ]]; then
    echo "[Package] Re-configuring CMake for clean Release (logging OFF)..."
    "$CMAKE_BIN" -B "$BUILD_DIR" -G Xcode \
        -DENABLE_MEMORY_SAFETY_LOGGING=OFF \
        -DENABLE_VLD=OFF \
        -DENABLE_ASAN=OFF \
        -DCMAKE_BUILD_TYPE=Release

    echo "[Package] Building Release (VST3 + AU + Standalone)..."
    "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --target SpaceDust_VST3 SpaceDust_AU SpaceDust_Standalone --parallel
else
    echo "[Package] --skip-build: reusing existing artefacts from $ARTEFACTS"
fi

# Check artefacts exist (after build if not skipped)
for p in "$VST3_SRC" "$AU_SRC" "$SA_SRC"; do
    if [[ ! -e "$p" ]]; then
        echo "[Package] ERROR: Required artefact missing: $p" >&2
        echo "          The build did not produce it. Check for errors above or run a manual build." >&2
        exit 1
    fi
done

# --- Resolve signing identities (only if --sign) ----------------------------
APP_SIGN_ID=""
INST_SIGN_ID=""

if [[ $SIGN -eq 1 ]]; then
    # Allow explicit overrides
    if [[ -n "$APP_IDENTITY" ]]; then
        APP_SIGN_ID="$APP_IDENTITY"
    else
        # Pick the first "Developer ID Application" we find
        APP_SIGN_ID="$(security find-identity -v -p codesigning 2>/dev/null \
            | grep -i 'Developer ID Application' | head -1 | sed -E 's/.*"([^"]+)".*/\1/' || true)"
    fi

    if [[ -n "$INSTALLER_IDENTITY" ]]; then
        INST_SIGN_ID="$INSTALLER_IDENTITY"
    else
        INST_SIGN_ID="$(security find-identity -v -p codesigning 2>/dev/null \
            | grep -i 'Developer ID Installer' | head -1 | sed -E 's/.*"([^"]+)".*/\1/' || true)"
    fi

    if [[ -z "$APP_SIGN_ID" ]]; then
        echo "[Sign] WARNING: No Developer ID Application certificate found." >&2
        echo "       The staged bundles will be left with their current signature (ad-hoc from Xcode)." >&2
        echo "       For distribution you will need a real cert (see MAC_BUILD.md)." >&2
    else
        echo "[Sign] Application identity : $APP_SIGN_ID"
    fi
    if [[ -z "$INST_SIGN_ID" ]]; then
        echo "[Sign] WARNING: No Developer ID Installer certificate found." >&2
        echo "       The final .pkg will be built unsigned (you can sign it manually later)." >&2
    else
        echo "[Sign] Installer identity  : $INST_SIGN_ID"
    fi
fi

# --- Stage payloads ---------------------------------------------------------
echo "[Package] Staging payloads..."

# Clean previous staging (but keep the scripts/ content we will (re)populate).
# Use sudo if a prior run (or polluted build artefact) left root-owned files inside the staged bundles;
# this is the same class of permission issue that has appeared before on this machine.
echo "[Package] Cleaning previous staging area..."
/usr/bin/sudo rm -rf installer/macos/payload 2>/dev/null || rm -rf installer/macos/payload || true
mkdir -p installer/macos/payload/Library/Audio/Plug-Ins/VST3
mkdir -p installer/macos/payload/Library/Audio/Plug-Ins/Components
mkdir -p installer/macos/payload/Applications

# Copy the bundles using ditto (preferred over cp -R for bundles on macOS
# to preserve metadata, extended attributes, and proper bundle structure).
ditto "$VST3_SRC" "installer/macos/payload/Library/Audio/Plug-Ins/VST3/Space Dust.vst3"
ditto "$AU_SRC"   "installer/macos/payload/Library/Audio/Plug-Ins/Components/Space Dust.component"
ditto "$SA_SRC"   "installer/macos/payload/Applications/Space Dust.app"

echo "[Package]   VST3      -> installer/macos/payload/Library/Audio/Plug-Ins/VST3/Space Dust.vst3"
echo "[Package]   AU        -> installer/macos/payload/Library/Audio/Plug-Ins/Components/Space Dust.component"
echo "[Package]   Standalone-> installer/macos/payload/Applications/Space Dust.app"

# --- (Re)sign the staged bundles if we have a real Application cert ---------
if [[ -n "$APP_SIGN_ID" ]]; then
    echo "[Sign] Signing staged bundles with hardened runtime..."
    codesign --force --deep --options runtime --timestamp --sign "$APP_SIGN_ID" \
        "installer/macos/payload/Library/Audio/Plug-Ins/VST3/Space Dust.vst3"
    codesign --force --deep --options runtime --timestamp --sign "$APP_SIGN_ID" \
        "installer/macos/payload/Library/Audio/Plug-Ins/Components/Space Dust.component"
    codesign --force --deep --options runtime --timestamp --sign "$APP_SIGN_ID" \
        "installer/macos/payload/Applications/Space Dust.app"
    echo "[Sign] Bundles signed."
else
    echo "[Package] Bundles left with existing signature (ad-hoc or previous)."
fi

# --- Stage factory presets + README into the scripts bundle -----------------
# These travel inside the .pkg and are used by postinstall.
if [[ $SKIP_PRESETS -eq 0 ]]; then
    echo "[Package] Staging factory presets (shared with Windows installer)..."
    rm -rf installer/macos/scripts/Presets
    mkdir -p installer/macos/scripts/Presets

    # Only the real preset files (exclude the developer README.md that lives in Files/Presets)
    # shellcheck disable=SC2035
    find installer/Files/Presets -maxdepth 1 -name "*.sdpreset" -exec cp -p {} installer/macos/scripts/Presets/ \;

    # The user-facing README (will be installed as README.txt inside the user's preset folder)
    cp -p installer/Support/README-Presets.txt installer/macos/scripts/README.txt

    preset_count=$(find installer/macos/scripts/Presets -name "*.sdpreset" | wc -l | tr -d ' ')
    echo "[Package]   Staged $preset_count factory presets + README.txt for postinstall"
else
    echo "[Package] --skip-presets: leaving installer/macos/scripts/Presets untouched."
fi

# Ensure postinstall is executable (git may have dropped the bit)
chmod +x installer/macos/scripts/postinstall 2>/dev/null || true

# --- Build the component packages (VST3, AU, Standalone, Uninstaller) ---------
PKG_DIR="$(mktemp -d -t spacedust-pkgs.XXXXXX)"
trap 'rm -rf "$PKG_DIR"' EXIT

echo "[Package] Building component packages into $PKG_DIR..."

# Use --component (not --root) for each bundle. This is the recommended way
# to package VST3 / AU / .app bundles so the Installer properly registers
# them as bundles (better metadata, code signing handling, etc.).
# We attach the postinstall scripts to the VST3 pkg (and also to Standalone)
# so that presets + config are set up whenever the user picks "plugins" or
# "standalone only".

# VST3 (scripts attached here → presets will be installed for the plugins choice)
pkgbuild \
    --component "installer/macos/payload/Library/Audio/Plug-Ins/VST3/Space Dust.vst3" \
    --scripts "installer/macos/scripts" \
    --identifier "com.63c.SpaceDust.vst3" \
    --version "$VERSION" \
    --install-location "/Library/Audio/Plug-Ins/VST3" \
    "$PKG_DIR/SpaceDust-VST3.pkg"

# AU (no scripts needed; the VST3 one above will have already run postinstall
# when the "plugins" choice is selected, since it includes this pkg-ref)
pkgbuild \
    --component "installer/macos/payload/Library/Audio/Plug-Ins/Components/Space Dust.component" \
    --identifier "com.63c.SpaceDust.au" \
    --version "$VERSION" \
    --install-location "/Library/Audio/Plug-Ins/Components" \
    "$PKG_DIR/SpaceDust-AU.pkg"

# Standalone (scripts attached so "standalone only" still gets presets/config).
# Using --root (instead of --component) + explicit tree to ensure the bundle
# payload is *always* laid down when the choice is selected. --component can
# skip placement on repeated same-version installs (even after preinstall rm)
# due to bundle update / receipt logic on dev machines.
mkdir -p "$PKG_DIR/standalone-root"
ditto "installer/macos/payload/Applications/Space Dust.app" "$PKG_DIR/standalone-root/Space Dust.app"

# Disable bundle relocation so the Installer always places Space Dust.app in
# /Applications rather than following an existing copy (e.g. the Xcode build dir).
COMP_PLIST="$PKG_DIR/standalone-component.plist"
cat > "$COMP_PLIST" <<'PLIST_EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
    <dict>
        <key>BundleHasStrictIdentifier</key>
        <true/>
        <key>BundleIsRelocatable</key>
        <false/>
        <key>BundleIsVersionChecked</key>
        <false/>
        <key>BundleOverwriteAction</key>
        <string>upgrade</string>
        <key>RootRelativeBundlePath</key>
        <string>Space Dust.app</string>
    </dict>
</array>
</plist>
PLIST_EOF

pkgbuild \
    --root "$PKG_DIR/standalone-root" \
    --component-plist "$COMP_PLIST" \
    --scripts "installer/macos/scripts" \
    --identifier "com.63c.SpaceDust.standalone" \
    --version "$VERSION" \
    --install-location "/Applications" \
    "$PKG_DIR/SpaceDust-Standalone.pkg"

# Build the uninstaller component package. It is always installed (via hidden
# choice in Distribution.xml) so a friendly .command is present no matter which
# visible options the user selects.
mkdir -p "$PKG_DIR/uninstaller-root"
cp "installer/macos/uninstall.sh" "$PKG_DIR/uninstaller-root/Space Dust Uninstaller.command"
chmod +x "$PKG_DIR/uninstaller-root/Space Dust Uninstaller.command"
pkgbuild \
    --root "$PKG_DIR/uninstaller-root" \
    --identifier "com.63c.SpaceDust.uninstaller" \
    --version "$VERSION" \
    --install-location "/Applications" \
    "$PKG_DIR/SpaceDust-Uninstaller.pkg"

echo "[Package] Component packages ready."

# --- Assemble the final product .pkg (with choice UI) -----------------------
DIST_XML="installer/macos/Distribution.xml"

# (Re)write Distribution.xml with exactly two visible choices.
# The uninstaller is attached to *both* visible choices so it is installed
# whenever the user selects anything (no hidden choice, to ensure the
# sub-package is always properly embedded by productbuild).
cat > "$DIST_XML" <<DISTRIBUTION_EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>Space Dust Synthesizer</title>
    <organization>63C</organization>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <options customize="always" require-scripts="false" hostArchitectures="x86_64,arm64"/>

    <choices-outline>
        <line choice="plugins"/>
        <line choice="standalone"/>
    </choices-outline>

    <choice id="plugins"
            title="VST3 + Audio Unit plug-ins"
            description="Install the plug-ins for use inside your DAW (Ableton Live, Logic Pro, FL Studio, Reaper, Cubase, etc.). Both formats are included so the synth works in almost any host."
            selected="true">
        <pkg-ref id="com.63c.SpaceDust.vst3"/>
        <pkg-ref id="com.63c.SpaceDust.au"/>
        <pkg-ref id="com.63c.SpaceDust.uninstaller"/>
    </choice>

    <choice id="standalone"
            title="Standalone application"
            description="Install the standalone desktop synthesizer (Space Dust.app) into your Applications folder. Lets you play without a DAW."
            selected="true">
        <pkg-ref id="com.63c.SpaceDust.standalone"/>
        <pkg-ref id="com.63c.SpaceDust.uninstaller"/>
    </choice>

    <pkg-ref id="com.63c.SpaceDust.vst3" version="$VERSION">SpaceDust-VST3.pkg</pkg-ref>
    <pkg-ref id="com.63c.SpaceDust.au" version="$VERSION">SpaceDust-AU.pkg</pkg-ref>
    <pkg-ref id="com.63c.SpaceDust.standalone"
             version="$VERSION">SpaceDust-Standalone.pkg</pkg-ref>
    <pkg-ref id="com.63c.SpaceDust.uninstaller" version="$VERSION">SpaceDust-Uninstaller.pkg</pkg-ref>
</installer-gui-script>
DISTRIBUTION_EOF

# Note: we don't actually ship a welcome.html in this first cut; productbuild will
# just show the standard title + choice UI. You can add a Resources/ folder with
# welcome.html later for a richer first screen.

echo "[Package] Assembling final product installer..."

PRODUCT_PKG="$OUT_DIR/SpaceDust-Synthesizer-$VERSION-Mac.pkg"

if [[ -n "$INST_SIGN_ID" ]]; then
    productbuild \
        --distribution "$DIST_XML" \
        --package-path "$PKG_DIR" \
        --sign "$INST_SIGN_ID" \
        --timestamp \
        "$PRODUCT_PKG"
else
    productbuild \
        --distribution "$DIST_XML" \
        --package-path "$PKG_DIR" \
        "$PRODUCT_PKG"
fi

echo "[Package] Product .pkg created: $PRODUCT_PKG"

# --- Optional .dmg wrapper --------------------------------------------------
if [[ $SKIP_DMG -eq 0 ]]; then
    DMG_PATH="$OUT_DIR/SpaceDust-Synthesizer-$VERSION-Mac.dmg"
    echo "[Package] Creating .dmg wrapper..."
    # Simple read-only dmg containing the .pkg (and optionally a symlink or alias later)
    hdiutil create -volname "Space Dust Synthesizer $VERSION" \
        -srcfolder "$PRODUCT_PKG" \
        -ov -format UDZO -o "$DMG_PATH" >/dev/null
    echo "[Package] .dmg created: $DMG_PATH"
else
    echo "[Package] --skip-dmg: .pkg only."
fi

# --- Final report -----------------------------------------------------------
echo ""
echo "======================================================================"
echo "[Package] macOS installer ready"
echo "======================================================================"
ls -lh "$OUT_DIR"/SpaceDust-Synthesizer-"$VERSION"-Mac.* 2>/dev/null || true

preset_count=$(find installer/macos/scripts/Presets -name "*.sdpreset" 2>/dev/null | wc -l | tr -d ' ')
echo "  Factory presets bundled : $preset_count"
echo ""
echo "To test locally (no notarization required on your own machine):"
echo "  open \"$PRODUCT_PKG\""
echo ""
echo "For distribution (run this *after* you have notarization credentials working):"
echo "  xcrun notarytool store-credentials \"spacedust-notary\" \\"
echo "      --apple-id \"you@yourmail.com\" --team-id \"TEAMID\" --password \"app-specific-password\""
echo ""
echo "  xcrun notarytool submit \"$PRODUCT_PKG\" --keychain-profile \"spacedust-notary\" --wait"
echo "  xcrun stapler staple \"$PRODUCT_PKG\""
echo ""
echo "  # If you built a .dmg, notarize + staple the .dmg instead (or notarize the .pkg then re-wrap)."
echo "======================================================================"

exit 0
