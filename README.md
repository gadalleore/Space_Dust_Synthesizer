# Space Dust - JUCE Synthesizer Plugin

A pure CMake-based polyphonic synthesizer VST3 plugin built with JUCE 8. Cosmic particles of sound.

**Repository:** [github.com/gadalleore/Space_Dust_Synthesizer](https://github.com/gadalleore/Space_Dust_Synthesizer)

```bash
git clone https://github.com/gadalleore/Space_Dust_Synthesizer.git
cd Space_Dust_Synthesizer
```

**Known Good With:** Ableton Live 10, 11, and 12 (heavily tested for stability as of 2026).

## Features

- **VST3 format** – VST3-only plugin
- **8-voice polyphony** – Dual oscillators (with sub oscillator), filter, and ADSR per voice; **voice modes** Poly / Mono / Legato (last-note priority stack)
- **Effects chain** – Reverb, delay (with filter), phaser, flanger, trance gate (post-effect), grain delay, parametric EQ, bitcrusher, soft clipper, compressor, transient shaper, lo-fi (with **Analog Drift**: subtle per-note pitch/filter variation and slow wander when Lo-Fi is enabled)
- **Modulation** – Two LFOs with On toggles; LFO1 targets filter, LFO2 targets pitch (25% depth default)
- **Master volume** – 0–2.0 range for headroom
- **MIDI input** – Full MIDI note and control support, including **MPE** (per-note pitch bend, pressure, and timbre via Lower Zone or Legacy modes — see [MPE Support](#mpe-support))
- **Custom UI** – SpaceDust look and feel with compact tabbed layout (Main, Modulation, Effects, Saturation Color, Spectral) and meter-linked glow
- **Real-time safe** – Parameter updates without allocations in the audio thread
- **Ableton stability focus** – Significant hardening against common VST3 crash patterns in Ableton (state restore, automation, editor lifecycle). Includes automated QA checks and runtime safety logging (optional).

## MPE Support

Space Dust is a fully MPE-aware synth: per-note pitch bend goes to the oscillators, channel pressure / aftertouch modulates amplitude, and CC74 / slide ("timbre") sweeps the filter cutoff. The MPE controls live on the Modulation tab (Mode: Legacy / Lower Zone, Bend Range, Pressure depth, Timbre depth).

**Important:** No DAW currently auto-enables MPE for third-party VST3 instruments — you have to flip a per-track toggle once per project. The plugin handles the rest.

### Ableton Live 11 / 12

1. Right-click the **Space Dust** title bar in the device chain → **Enable MPE Mode**. The header will show **"MPE"** on the right side once enabled.
2. (Optional) Right-click again → **MPE Channel Settings** to configure zone (Lower / Upper) and member-channel count.
3. In Space Dust's **Modulation tab → MPE strip**, set **Mode = Lower Zone** for the standard MPE spec, or leave on **Legacy** for non-MPE keyboards / classic pitch-wheel + aftertouch.
4. Drive it from a Seaboard/LinnStrument, the **Attila Magyar MPE Emulator** VST3 (MIDI effect placed *before* Space Dust on the track), or Live's per-clip MPE envelopes ("E" button → category **MPE** → Pitch / Pressure / Slide).

### Bitwig Studio

1. Click the Space Dust device's small triangle → **Enable Note Expressions** (or use the per-track MPE toggle on the track header).
2. In Space Dust set **Mode = Lower Zone**.
3. Per-note PB/AT/timbre from a Seaboard/LinnStrument or Bitwig's note expression editor will flow through.

### Cubase / Studio One

- External MPE controllers (Seaboard, LinnStrument, Sensel, etc.) plug in and work out of the box — Space Dust accepts MIDI on all 16 channels and routes per-note expression internally.
- Cubase's native "Note Expression" inspector does not surface per-note curves for Space Dust because the plugin does not expose `INoteExpressionController` (a separate VST3 interface, not the same as MPE-over-MIDI). Hardware MPE controllers and MPE-emulator plugins still work fully.

### Verifying it's working

Hold a note and move your controller's pitch wheel — pitch should glide; move pressure / aftertouch — amplitude should swell; move CC74 / slide — the filter should brighten or darken. Lower / raise the **Pressure** and **Timbre** knobs in the Modulation tab to confirm the attenuation.

## Prerequisites

- **Visual Studio 2022 or newer** with "Desktop development with C++" workload
- **CMake 3.22 or newer**
- **JUCE 8** – [juce.com](https://juce.com/get-juce) or [GitHub](https://github.com/juce-framework/JUCE)

## Setup

`juce_path.local` is **gitignored**—create it only on your machine (it may contain a local filesystem path to JUCE). Never commit it.

1. **Install JUCE** and point the project to it (choose one):
   - Copy `juce_path.local.example` to `juce_path.local` and put your JUCE path in it, or
   - Set the `JUCE_DIR` environment variable, or
   - Pass `-DJUCE_DIR=...` when running CMake

2. **Generate and build** (recommended):
   The easiest and most reliable way is to use the provided build script:
   ```powershell
   .\build-and-launch.ps1
   # Use -NoLaunch to skip launching Ableton
   .\build-and-launch.ps1 -NoLaunch
   ```

   This script will:
   - Configure with the correct Visual Studio generator (with fallback between 2022 and 2026)
   - Build the Release VST3
   - Automatically copy the plugin to all common VST3 locations (system folder + Ableton User Library, etc.)

   Manual alternative:
   ```powershell
   cmake -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ```
   (Use `Visual Studio 18 2026` if you have VS 2026 installed.)

3. **Plugin output**: `build\SpaceDust_artefacts\Release\VST3\Space Dust.vst3` (when building manually)

## Building the Windows installer

A signed-and-sealed Inno Setup script ships in [`installer/SpaceDust-Setup.iss`](installer/SpaceDust-Setup.iss).

1. Install [Inno Setup 6](https://jrsoftware.org/isdl.php) (one time).
2. Save the factory presets you want to ship from inside the plug-in UI. They land in
   `%USERPROFILE%\Documents\Space Dust\Presets\` (or wherever you configured via the
   installer wizard / `%ProgramData%\Space Dust\config.xml`).
3. Run the packaging script:
   ```powershell
   .\package-installer.ps1
   # Skip the VST3 rebuild for fast iteration on installer-only tweaks:
   .\package-installer.ps1 -SkipBuild
   ```
   The script rebuilds the Release VST3, syncs your `*.xml` presets into
   `installer/Files/Presets/`, and compiles
   `installer/Output/SpaceDust-Synthesizer-1.0-Setup.exe`.
4. Test the produced `.exe`. Customer-side, the installer:
   - Prompts for a VST3 folder and a presets folder.
   - Writes `%ProgramData%\Space Dust\config.xml` so the plug-in finds the chosen
     presets folder on first run.
   - Ships the factory presets with `onlyifdoesntexist`, so re-installing or
     upgrading **never overwrites** a user's modified copies.

The installer's UAC prompt shows "Publisher: Unknown" because the `.exe` isn't
code-signed; that's expected for indie distributions. See
`installer/Files/Presets/README.md` for more on the preset workflow.

## Project Structure

```
Source/
├── PluginProcessor.*        # Main audio processor
├── PluginEditor.*           # Plugin UI (tabbed: Main, Modulation, Effects, Saturation Color, Spectral)
├── SynthVoice.*             # Voice implementation
├── SynthSound.*             # Sound definition
├── SpaceDustSynthesiser.*   # Synth engine
├── MemorySafetyLogger.*     # Runtime safety & crash diagnostics logging (optional)
├── PresetManager.*          # Preset saving/loading system
├── Goniometer.*             # Stereo correlation meter
├── OscilloscopeComponent.*  # Lissajous / waveform display
├── SpectrumAnalyserComponent.*  # Spectrum analyzer
├── SpaceDustReverb.*        # Reverb
├── SpaceDustGrainDelay.*    # Grain delay
├── SpaceDustPhaser.*        # Phaser
├── SpaceDustFlanger.*       # Flanger
├── SpaceDustTranceGate.*    # Trance gate (post-effect)
├── SpaceDustBitCrusher.*    # Bitcrusher effect
├── SpaceDustSoftClipper.*   # KClip-style soft clipper (Saturation Color tab)
├── SpaceDustCompressor.*    # SSL/1176/LA-2A style compressor (Saturation Color tab)
├── SpaceDustTransient.*     # 808/909 transient shaper (Saturation Color tab)
├── SpaceDustLofi.*          # Lo-fi effect (Saturation Color tab)
├── SpaceDustParametricEQ.*  # Parametric EQ
├── SpaceDustFinalEQ.*       # Final mastering EQ (DSP)
├── FinalEQComponent.*       # Final EQ UI component
├── SexiconReverb.*          # Additional reverb
├── SpaceDustLookAndFeel.*   # Custom UI styling
└── CheezeGuyGame.h          # Easter egg (optional)
```

## Recent fixes

- **Ableton stability hardening (2026):** Major pass to eliminate classes of crashes that could occur during session restore, preset loading, and rapid automation. Key changes:
  - Introduced `safeGetParam()` helper on the editor (and equivalent helpers in the processor) to safely read parameters without risking null dereferences when IDs are missing or during state restore.
  - Replaced dozens of direct `*getRawParameterValue()` and `getParameter()->getValue()` calls throughout the UI with the safe wrappers.
  - Hardened Grain Delay read buffer against floating-point index drift (common with pitch shifting + automation), preventing potential out-of-bounds reads into the delay buffer.
- **Build & deployment reliability:** `build-and-launch.ps1` now reliably copies the VST3 to all common locations (system VST3, Ableton User Library, etc.) and includes fallback logic between Visual Studio 2022 and 2026 generators.
- **Host automation vs. Link to Master (filter):** When LFO filter sections follow the main filter, the editor previously mixed normalized APVTS values with `convertTo0to1` as if they were Hz, and called `setValueNotifyingHost` on the master when mod-filter parameters changed (including from host automation). That could corrupt cutoff and fight automation on the main filter. Link toggles now copy master→mod using normalized `getValue()`; while linked, the mod page mirrors the main filter visually without extra host notifications, and user edits on the mod page update the master through normal control gestures.
- **Pink noise generator:** The Voss–McCartney implementation uses 16 filter rows; the row index must stay in range. The counter is now wrapped so it cannot produce an out-of-bounds row (which could cause rare harsh digital glitches after sustained play, for example on long chords with pink noise). Non-finite oscillator frequencies and filter outputs are also clamped or zeroed; developers can read `SpaceDustAudioProcessor::getDspSanitizeEventCount()` if investigating edge cases.

## Additional documentation

- [docs/FIX_SUMMARY.md](docs/FIX_SUMMARY.md) — detailed history of stability hardening, crash prevention work, and assertion fixes
- [docs/REVERB_RESEARCH.md](docs/REVERB_RESEARCH.md)

## Development & Testing

This project includes significant tooling focused on stability and quality:

- `run-full-qa.ps1` — Runs static analysis checks, pluginval validation, installer smoke tests, and crash-marker hygiene.
- `enable-safety-logging.ps1` / `disable-all-logging-for-release.ps1` — Toggles the built-in `MemorySafetyLogger` for detailed runtime diagnostics (voices, grains, buffer access, audio-thread safety, etc.).
- The plugin has undergone extensive hardening specifically against common Ableton Live VST3 crash patterns (state restore crashes, dangling parameter listeners, etc.).

See `QA-README.md` for details on the QA suite.

## References

- [JUCE Documentation](https://docs.juce.com/)
- [JUCE CMake API](https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md)

## License

MIT License - see [LICENSE](LICENSE) for details.
