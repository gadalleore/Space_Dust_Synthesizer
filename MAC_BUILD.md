# Space Dust — macOS Build, Sign & Test

Goal on the Mac: **build → get it Apple-certified (sign + notarize) → test**, with the
least possible work on the machine. Everything below is command-line so an older Mac
isn't doing heavy GUI lifting.

> Windows code signing (Azure Trusted Signing) is a **separate** process done on the PC —
> not covered here. This file is macOS only.

Plugin identifiers (already set in `CMakeLists.txt`, don't change them):

| Field | Value |
|-------|-------|
| Manufacturer code | `YcPl` |
| Plugin code | `SpDs` |
| AU type | `aumu` (music device / instrument) |
| Bundle ID | `com.63c.SpaceDust` |
| Company | `63C` |

---

## 1. One-time setup

Install only what's missing:

- **Xcode** (from the App Store) + command-line tools: `xcode-select --install`
- **CMake**: `brew install cmake` (or the CMake.app installer)
- **JUCE 8** somewhere on disk (same version family as the Windows build: 8.0.x)
- **Apple Developer Program** membership ($99/year) — required to sign & notarize for
  distribution. Sign up at <https://developer.apple.com/programs/>.

---

## 2. Build (universal binary)

```bash
git clone https://github.com/gadalleore/Space_Dust_Synthesizer.git
cd Space_Dust_Synthesizer
cmake -B build -G Xcode -DJUCE_DIR=/path/to/JUCE
cmake --build build --config Release
```

`CMakeLists.txt` already forces a **universal arm64 + x86_64** binary and a **10.13
deployment target** on Mac hosts, so one build runs on Apple Silicon *and* Intel — no
extra flags needed.

Outputs land in:

```
build/SpaceDust_artefacts/Release/
├── VST3/Space Dust.vst3
├── AU/Space Dust.component
└── Standalone/Space Dust.app
```

> Old-Mac tip: a universal Release build is the slowest step. If you just want to
> iterate/test on *this* Mac's architecture, drop the arch override to halve build time:
> `cmake -B build -G Xcode -DJUCE_DIR=... -DCMAKE_OSX_ARCHITECTURES=$(uname -m)`
> — but do the **final** shippable build as universal (the default).

---

## 3. Validate the AU (do this before opening any DAW)

Logic won't load an AU that fails Apple's validator. Check it:

```bash
auval -v aumu SpDs YcPl
```

A clean run ends with **"AU VALIDATION SUCCEEDED."** If it fails here, fix that before
testing — the DAW will only hide the error.

To install the plugins for local testing (user-scope, no admin):

```bash
cp -R "build/SpaceDust_artefacts/Release/AU/Space Dust.component"  ~/Library/Audio/Plug-Ins/Components/
cp -R "build/SpaceDust_artefacts/Release/VST3/Space Dust.vst3"     ~/Library/Audio/Plug-Ins/VST3/
killall -9 AudioComponentRegistrar 2>/dev/null   # force AU re-scan
```

---

## 4. Apple certification (sign + notarize)

Needed once per release for distribution (so Gatekeeper doesn't block buyers).

**a. Create a "Developer ID Application" certificate** (once): Xcode → Settings →
Accounts → Manage Certificates → "+" → *Developer ID Application*. Confirm it's installed:

```bash
security find-identity -v -p codesigning
# note the line: "Developer ID Application: Your Name (TEAMID)"
```

**b. Sign each plugin with the hardened runtime** (`--options runtime` is required for
notarization):

```bash
SIGN="Developer ID Application: Your Name (TEAMID)"
codesign --force --deep --options runtime --timestamp --sign "$SIGN" "build/SpaceDust_artefacts/Release/AU/Space Dust.component"
codesign --force --deep --options runtime --timestamp --sign "$SIGN" "build/SpaceDust_artefacts/Release/VST3/Space Dust.vst3"
# verify
codesign --verify --strict --verbose=2 "build/SpaceDust_artefacts/Release/VST3/Space Dust.vst3"
```

**c. Notarize** (Apple scans the binary; ~1–5 min). First store credentials once with an
[app-specific password](https://support.apple.com/en-us/102654):

```bash
xcrun notarytool store-credentials "spacedust-notary" \
  --apple-id "you@example.com" --team-id "TEAMID" --password "abcd-efgh-ijkl-mnop"
```

Then per release, zip → submit → staple:

```bash
ditto -c -k --keepParent "build/SpaceDust_artefacts/Release/VST3/Space Dust.vst3" /tmp/vst3.zip
xcrun notarytool submit /tmp/vst3.zip --keychain-profile "spacedust-notary" --wait
xcrun stapler staple "build/SpaceDust_artefacts/Release/VST3/Space Dust.vst3"
# repeat the three lines for the .component
```

> When you build the macOS **installer** (.pkg/.dmg) later, you notarize *that* final
> artifact too — same `notarytool submit … --wait` + `stapler staple` flow.

---

## 5. Test checklist

Load the AU in **Logic/GarageBand** and the VST3 in a VST3 host (Live, Reaper). Verify:

- [ ] Plugin appears under vendor **63C** and loads without error
- [ ] **Preset save → reload round-trip** — especially: turn on the Modulation-tab
      Filter 1, **unlink** it, set it to **High Pass** (master on Low Pass), save, load in
      a fresh instance → it stays High Pass. *(This was the bug we fixed; confirm it holds
      on Mac.)*
- [ ] Reverb dropdown shows **"Void Verb"** (not the old name) and sounds right
- [ ] Audio plays cleanly, no crashes on add/remove/reload
- [ ] MPE: in Logic, confirm per-note expression behaves (Live needs manual MPE enable)
- [ ] Factory presets load from the installed location

---

## 6. Package the installer (bundle factory presets + choice UI)

This step produces a professional macOS installer (`.pkg` + optional `.dmg`) that:

- Installs the **VST3** and **AU** plug-ins to the standard system locations
  (`/Library/Audio/Plug-Ins/...`) when that choice is selected.
- Optionally installs the **Standalone** app to `/Applications` when that choice
  is selected.
- **Always** installs the easy "Space Dust Uninstaller.command" to /Applications
  (background / not a user-visible choice).
- Bundles **all 50+ factory presets** (the exact set that ships with the Windows
  installer, taken from the shared `installer/Files/Presets/` folder).
- Runs a `postinstall` script that copies the factory `.sdpreset` files into the
  user's `~/Documents/Space Dust/Presets` (never overwriting existing user
  presets) and writes the `config.xml` that `PresetManager` reads on first launch.
- Presents two user-visible choices (the Windows `.iss` offers similar optionality).

The packaging script (`package-macos.sh`) is the direct macOS counterpart to
`package-installer.ps1`. It shares the preset source tree and the same
release-discipline mindset (clean build with logging off).

### Run the packager (after a successful Release build)

```bash
# Fast path — reuse the artefacts you just built (the ones in build-release/
# or build/ after the steps above)
./package-macos.sh --skip-build

# Full clean path (forces a logging-OFF Release build first, like the Windows side)
./package-macos.sh

# With signing (once you have the certs installed — see section 4)
./package-macos.sh --sign

# Other useful flags
./package-macos.sh --skip-build --skip-dmg --out-dir /tmp
```

Outputs land in:

```
installer/Output/
├── SpaceDust-Synthesizer-1.0-Mac.pkg
└── SpaceDust-Synthesizer-1.0-Mac.dmg   (unless --skip-dmg)
```

The `.pkg` presents two user-visible selectable choices (the uninstaller is
always installed in the background and is never shown as an option):

- **VST3 + Audio Unit plug-ins** (selected by default)
- **Standalone application** (selected by default)

The choices are independent:
- Standalone checked + plugins unchecked → only the app (+ uninstaller) is installed.
- Plugins checked + standalone unchecked → only the plugins (+ uninstaller) are installed.
- Both checked → both are installed (+ uninstaller).

The postinstall always runs for whichever visible component(s) the user installs,
so even a "standalone only" install still gets the factory presets and a working
config.

### Local testing of the installer (no notarization needed on your own Mac)

```bash
open installer/Output/SpaceDust-Synthesizer-1.0-Mac.pkg
```

After installation:

- Plugins: `/Library/Audio/Plug-Ins/VST3/Space Dust.vst3` and
  `.../Components/Space Dust.component`
- Standalone: `/Applications/Space Dust.app`
- Your personal presets + config: `~/Documents/Space Dust/Presets/` and
  `~/Library/Application Support/Space Dust/config.xml`
- Run `auval -v aumu SpDs YcPl` again (or just launch your DAW) to confirm the
  installed AU is visible.

### Notarization (do this *after* package preparation)

As you requested: prepare the package first, notarize afterwards.

Once your Apple Developer login / app-specific password is working:

```bash
# One-time credential storage (use an app-specific password, not your account password)
xcrun notarytool store-credentials "spacedust-notary" \
  --apple-id "you@example.com" --team-id "TEAMID" --password "abcd-efgh-ijkl-mnop"

# Notarize + staple the .pkg (recommended) or the .dmg
xcrun notarytool submit "installer/Output/SpaceDust-Synthesizer-1.0-Mac.pkg" \
  --keychain-profile "spacedust-notary" --wait
xcrun stapler staple "installer/Output/SpaceDust-Synthesizer-1.0-Mac.pkg"

# If you prefer to ship the .dmg, notarize the .dmg (it will include the already-built .pkg)
```

See the end of the script output for the exact commands it prints.

### Sharing the preset set between Windows and macOS

- Factory presets live in `installer/Files/Presets/*.sdpreset` (plus the
  developer-oriented `README.md` that explains the workflow).
- Both `package-installer.ps1` (Windows) and `package-macos.sh` (macOS) pull from
  this single source when they run.
- The user-facing `installer/Support/README-Presets.txt` is installed as
  `README.txt` inside the user's preset folder on both platforms.

This keeps the "all of the presets that are on the Github page" in one place and
guarantees the two installers ship an identical factory library.

---

## Quick reference

```bash
# Build (universal) → validate → (optionally sign the raw plugins) → package
cmake -B build -G Xcode -DJUCE_DIR=/path/to/JUCE
cmake --build build --config Release
auval -v aumu SpDs YcPl

# Prepare the installer (presets bundled, choices for plugins vs. standalone)
./package-macos.sh --skip-build

# Later, when notarization is ready
xcrun notarytool submit "installer/Output/SpaceDust-Synthesizer-1.0-Mac.pkg" \
  --keychain-profile "spacedust-notary" --wait
xcrun stapler staple "installer/Output/SpaceDust-Synthesizer-1.0-Mac.pkg"
```
