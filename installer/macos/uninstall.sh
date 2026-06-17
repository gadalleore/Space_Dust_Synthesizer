#!/bin/bash

# Space Dust Synthesizer - Uninstaller
# Double-click this .command file or run from Terminal.
# Removes the plugins and app. User presets are left alone unless you opt in.

echo "=========================================="
echo "  Space Dust Synthesizer Uninstaller"
echo "=========================================="
echo ""
echo "This will remove:"
echo "  - VST3 plugin from /Library/Audio/Plug-Ins/VST3/"
echo "  - Audio Unit from /Library/Audio/Plug-Ins/Components/"
echo "  - Standalone app from /Applications/"
echo "  - This uninstaller itself"
echo ""
echo "Your personal presets in ~/Documents/Space Dust will NOT be deleted"
echo "unless you choose to remove all user data at the end."
echo ""
read -p "Continue with uninstall? [y/N] " -r REPLY
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Uninstall cancelled."
    exit 0
fi

# Resolve the real user (this script runs as the logged-in user; sudo elevates
# only the privileged removals below).
REAL_USER="$(id -un)"
USER_HOME="$HOME"

# Acquire admin rights up front with a single prompt. If it fails, stop —
# do NOT pretend the uninstall succeeded.
echo ""
echo "Administrator access is required to remove the system plug-ins and app."
if ! sudo -v; then
    echo ""
    echo "ERROR: Could not obtain administrator access. Nothing was removed."
    echo "Re-run the uninstaller and enter your Mac password when prompted."
    echo ""
    read -p "Press Enter to close this window..." -r
    exit 1
fi

# Track whether every step actually succeeded so we can report honestly.
ALL_OK=1
remove_path() {  # $1 = path to remove with sudo; only reports failure if it truly remains
    local p="$1"
    if [[ -e "$p" ]]; then
        sudo rm -rf "$p"
        if [[ -e "$p" ]]; then
            echo "  FAILED to remove: $p"
            ALL_OK=0
        else
            echo "  removed: $p"
        fi
    else
        echo "  not present: $p"
    fi
}

echo ""
echo "Removing plug-ins and app..."
remove_path "/Library/Audio/Plug-Ins/VST3/Space Dust.vst3"
remove_path "/Library/Audio/Plug-Ins/Components/Space Dust.component"
remove_path "/Applications/Space Dust.app"

# Discard installer receipts so a future (re)install lays everything down
# cleanly instead of being skipped as "same version already installed".
echo ""
echo "Clearing installer receipts..."
for id in com.63c.SpaceDust.vst3 com.63c.SpaceDust.au com.63c.SpaceDust.standalone com.63c.SpaceDust.uninstaller; do
    sudo pkgutil --forget "$id" >/dev/null 2>&1 && echo "  forgot: $id" || echo "  (no receipt: $id)"
done

# Optional user data removal (presets, config). Use sudo because the install
# may have created these folders as root.
echo ""
read -p "Also remove your presets, config, and other user data? This cannot be undone. [y/N] " -r REPLY
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    sudo rm -rf "$USER_HOME/Documents/Space Dust"
    sudo rm -rf "$USER_HOME/Library/Application Support/Space Dust"
    sudo rm -rf "$USER_HOME/Library/SpaceDust"
    echo "User data removed."
else
    echo "User data left in place."
fi

echo ""
echo "Running final cleanup..."
killall -9 AudioComponentRegistrar 2>/dev/null || true

echo ""
if [[ $ALL_OK -eq 1 ]]; then
    echo "Uninstall complete. Space Dust has been removed."
else
    echo "Uninstall finished, but one or more items could NOT be removed (see above)."
    echo "You may need to remove them manually or re-run the uninstaller."
fi
echo "You may need to restart your DAW for the change to take effect."
echo ""

# Remove the uninstaller itself last (so it doesn't linger in /Applications).
# Scheduled after this script's file handle is already open, so the running
# shell finishes fine.
sudo rm -f "/Applications/Space Dust Uninstaller.command" 2>/dev/null || true

read -p "Press Enter to close this window..." -r
