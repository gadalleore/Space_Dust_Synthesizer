#!/bin/bash

# Space Dust Synthesizer - Uninstaller
# Double-click this .command file or run from Terminal.
# Removes the plugins and app. User presets are left alone by default.

set -e

echo "=========================================="
echo "  Space Dust Synthesizer Uninstaller"
echo "=========================================="
echo ""
echo "This will remove:"
echo "  - VST3 plugin from /Library/Audio/Plug-Ins/VST3/"
echo "  - Audio Unit from /Library/Audio/Plug-Ins/Components/"
echo "  - Standalone app from /Applications/"
echo ""
echo "Your personal presets in ~/Documents/Space Dust/Presets will NOT be deleted"
echo "unless you choose to remove all user data at the end."
echo ""
read -p "Continue with uninstall? [y/N] " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Uninstall cancelled."
    exit 0
fi

echo ""
echo "Removing system plugins (you may be asked for your password)..."

/usr/bin/sudo rm -rf /Library/Audio/Plug-Ins/VST3/"Space Dust.vst3" 2>/dev/null || true
/usr/bin/sudo rm -rf /Library/Audio/Plug-Ins/Components/"Space Dust.component" 2>/dev/null || true
/usr/bin/sudo rm -rf /Applications/"Space Dust.app" 2>/dev/null || true

echo "Plugins and app removed."

# Optional user data removal
echo ""
read -p "Also remove your presets, config, and other user data? This cannot be undone. [y/N] " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    rm -rf ~/Documents/"Space Dust" 2>/dev/null || true
    rm -rf ~/Library/Application\ Support/"Space Dust" 2>/dev/null || true
    rm -rf ~/Library/SpaceDust 2>/dev/null || true
    echo "User data removed."
else
    echo "User data left in place."
fi

echo ""
echo "Running final cleanup..."
killall -9 AudioComponentRegistrar 2>/dev/null || true

echo ""
echo "Uninstall complete."
echo "You may need to restart your DAW or computer for all traces to disappear."
echo "If you reinstall later, your presets will still be there (unless you deleted them)."
echo ""
read -p "Press Enter to close this window..." -n 1 -r
