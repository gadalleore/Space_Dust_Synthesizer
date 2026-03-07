# Space Dust - JUCE Synthesizer Plugin

A pure CMake-based polyphonic synthesizer VST3 plugin built with JUCE 8. Cosmic particles of sound.

## Features

- **VST3 format** – VST3-only plugin
- **8-voice polyphony** – Dual oscillators (with sub oscillator), filter, and ADSR per voice
- **Effects chain** – Reverb, delay (with filter), phaser, flanger, trance gate (post-effect), grain delay, parametric EQ, bitcrusher
- **Modulation** – Two LFOs with On toggles; LFO1 targets filter, LFO2 targets pitch (25% depth default)
- **Master volume** – 0–2.0 range for headroom
- **MIDI input** – Full MIDI note and control support
- **Custom UI** – SpaceDust look and feel with compact tabbed layout (Main, Modulation, Effects, Saturation Color, Spectral) and meter-linked glow
- **Real-time safe** – Parameter updates without allocations in the audio thread

## Prerequisites

- **Visual Studio 2022 or newer** with "Desktop development with C++" workload
- **CMake 3.22 or newer**
- **JUCE 8** – [juce.com](https://juce.com/get-juce) or [GitHub](https://github.com/juce-framework/JUCE)

## Setup

1. **Install JUCE** and point the project to it (choose one):
   - Copy `juce_path.local.example` to `juce_path.local` and put your JUCE path in it, or
   - Set the `JUCE_DIR` environment variable, or
   - Pass `-DJUCE_DIR=...` when running CMake

2. **Generate and build**:
   ```powershell
   cmake -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ```

   Or use the convenience script (builds, copies VST3, and optionally launches Ableton):
   ```powershell
   .\build-and-launch.ps1
   # Use -NoLaunch to skip launching Ableton
   .\build-and-launch.ps1 -NoLaunch
   ```

3. **Plugin output**: `build\SpaceDust_artefacts\Release\VST3\Space Dust.vst3`

## Project Structure

```
Source/
├── PluginProcessor.*        # Main audio processor
├── PluginEditor.*           # Plugin UI (tabbed: Main, Modulation, Effects, Saturation Color, Spectral)
├── SynthVoice.*             # Voice implementation
├── SynthSound.*             # Sound definition
├── SpaceDustSynthesiser.*   # Synth engine
├── Goniometer.*             # Stereo correlation meter
├── OscilloscopeComponent.*  # Lissajous / waveform display
├── SpectrumAnalyserComponent.*  # Spectrum analyzer
├── SpaceDustReverb.*        # Reverb
├── SpaceDustGrainDelay.*    # Grain delay
├── SpaceDustPhaser.*        # Phaser
├── SpaceDustFlanger.*       # Flanger
├── SpaceDustTranceGate.*    # Trance gate (post-effect)
├── SpaceDustBitCrusher.*    # Bitcrusher effect
├── SpaceDustParametricEQ.*   # Parametric EQ
├── SexiconReverb.*          # Additional reverb
└── SpaceDustLookAndFeel.*   # Custom UI styling
```

## References

- [JUCE Documentation](https://docs.juce.com/)
- [JUCE CMake API](https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md)

## License

MIT License - see [LICENSE](LICENSE) for details.
