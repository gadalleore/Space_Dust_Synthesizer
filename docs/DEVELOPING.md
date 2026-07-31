# Developing Space Dust

The build-and-listen loop on macOS, plus the Claude connector that edits presets. For
signing and notarising a release, see [`MAC_BUILD.md`](../MAC_BUILD.md) — this document is
about day-to-day development, where none of that applies.

## One-time setup

- **Xcode** plus command line tools: `xcode-select --install`
- **CMake**: `brew install cmake`
- **JUCE 8.0.x** somewhere on disk. On this machine: `/Users/alvin/code/JUCE` (8.0.15).

### Apply the JUCE patches — do this before your first build

Space Dust needs local modifications to the JUCE tree itself, so they are **lost whenever
JUCE is re-cloned or updated**. Nothing warns you: the build succeeds, and the damage only
shows up in a host.

```bash
python3 patches/apply-juce-mpe-patch.py --juce /Users/alvin/code/JUCE
```

Without it the VST3 exposes 2080 phantom "MIDI CC" parameters and buries the host's
automation list. The script is idempotent, and fails loudly rather than half-patching, so
re-running it is always safe. See [`patches/README.md`](../patches/README.md); the second
patch there is Windows-only and irrelevant on macOS.

## Build

```bash
cmake -B build -DJUCE_DIR=/Users/alvin/code/JUCE -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 8
```

Artefacts land in `build/SpaceDust_artefacts/Release/` as VST3, AU and Standalone.

### Architectures — the one that will bite you

CMakeLists defaults macOS builds to universal (`arm64;x86_64`). **Leave it that way for
anything you install.**

Building `arm64` only roughly halves build time, which is tempting for a quick iteration:

```bash
cmake -B build -DJUCE_DIR=/Users/alvin/code/JUCE -DCMAKE_OSX_ARCHITECTURES=arm64
```

But **Ableton Live 10 is an x86_64 application** and runs under Rosetta even on Apple
Silicon. An arm64-only plugin is invisible to it — it does not appear, and Live gives no
reason why. The same applies to any Intel-only host. Use arm64-only for compile checks and
the test harness; build universal for anything that goes in a plug-ins folder.

`CMAKE_OSX_ARCHITECTURES` is cached, so switching back needs it passed explicitly:

```bash
cmake -B build -DJUCE_DIR=/Users/alvin/code/JUCE -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
```

Always confirm what you actually produced before installing it:

```bash
lipo -info "build/SpaceDust_artefacts/Release/VST3/Space Dust.vst3/Contents/MacOS/Space Dust"
```

## Install a local build

Both formats live in system folders, so this needs admin rights. Remove before copying —
copying over a bundle leaves stale files behind:

```bash
sudo rm -rf "/Library/Audio/Plug-Ins/VST3/Space Dust.vst3" "/Library/Audio/Plug-Ins/Components/Space Dust.component"
sudo cp -R "build/SpaceDust_artefacts/Release/VST3/Space Dust.vst3" /Library/Audio/Plug-Ins/VST3/
sudo cp -R "build/SpaceDust_artefacts/Release/AU/Space Dust.component" /Library/Audio/Plug-Ins/Components/
```

Then in Live: **Preferences → Plug-Ins → Rescan**, or restart it. macOS caches AU
registrations separately; if the AU looks stale, `killall -9 AudioComponentRegistrar`.

Local builds are unsigned. That is fine on your own machine — Gatekeeper only quarantines
downloaded files — but they cannot be distributed. That is what `MAC_BUILD.md` is for.

For quick DSP work, the **Standalone** build in
`build/SpaceDust_artefacts/Release/Space Dust.app` skips the install and rescan entirely
and has its own MIDI keyboard.

## Test harnesses

Headless, off by default:

```bash
cmake -B build -DJUCE_DIR=/Users/alvin/code/JUCE -DSPACEDUST_BUILD_TESTS=ON
cmake --build build --parallel 8 --target SpaceDustHotReloadTest
./build/SpaceDustHotReloadTest_artefacts/Release/SpaceDustHotReloadTest
```

`tools/hotreload/hotreload_test.cpp` instantiates the real processor, writes and edits a
preset in the real configured preset folder, pumps the run loop, and asserts the live
parameters followed. It uses the real folder rather than a mock deliberately — a mock is
exactly what would have hidden the app-data path bug described below.

To test a universal build the way an Intel host will run it, run each slice:

```bash
arch -arm64  ./build/SpaceDustHotReloadTest_artefacts/Release/SpaceDustHotReloadTest
arch -x86_64 ./build/SpaceDustHotReloadTest_artefacts/Release/SpaceDustHotReloadTest
```

`tools/` also holds `stress`, `transienttest` and `vst3host`, wired the same way.

## Where things live at runtime

| Path | What |
|---|---|
| `~/Documents/Space Dust/Presets` | Default preset folder (`.sdpreset`, plain XML) |
| `~/Library/Application Support/Space Dust/config.xml` | Per-user settings, including a custom preset folder |
| `~/Library/Application Support/Space Dust/current.sdpreset` | Live parameter state, republished ~1 Hz by `PresetHotReload` |

**A trap worth knowing:** JUCE maps `userApplicationDataDirectory` to `~/Library` on
macOS, *not* `~/Library/Application Support`. `PresetManager::appDataFolder()` is now
explicit about this; anything else reaching for app data should use it rather than the JUCE
special location directly.

## Hot reload

`Source/PresetHotReload.{h,cpp}`, owned by the processor so it keeps working with the
plugin window closed. One message-thread timer at 1 Hz publishes the live sound and
re-applies the loaded preset when its file changes on disk.

If you touch it, the constraint to preserve is that it does **not** use
`replaceState()` — that swaps the tree out from under the host and the editor's attachments
without telling either. Each changed parameter goes through `beginChangeGesture` /
`setValueNotifyingHost` / `endChangeGesture`, matching `PresetManager::loadInitPreset`,
whose comment records that unbalanced bursts corrupt FL Studio's "Last Tweaked" tracking.
Unchanged parameters are skipped so a two-knob edit costs two gestures, not 206.

## The Claude connector

`spacedust-mcp` lets a musician reshape presets by describing the sound. It edits
`.sdpreset` files, so it needs no plugin build to work — but the two live tools
(`read_current_sound`, `adjust_current_sound`) need a plugin containing `PresetHotReload`.

```bash
cd /Users/alvin/code/tmp/spacedust-mcp
uv run pytest
uv tool install --force .          # then: claude mcp add --scope user space-dust -- spacedust-mcp
```

Parameter ranges, defaults and choice names ship as a generated `data/schema.json`, so end
users never need this repo. **Regenerate it whenever `createParameterLayout()` changes:**

```bash
uv run python -m spacedust_mcp.source_parser Source/PluginProcessor.cpp
```

That generator regex-parses the C++, which is the most fragile part of the setup. Replacing
it with a build-time target that instantiates `createParameterLayout()` and dumps JSON
would make the schema an artefact rather than a guess — the next worthwhile job.
