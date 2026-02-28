# Space Dust - JUCE Synthesizer Plugin

A pure CMake-based polyphonic synthesizer VST3 plugin built with JUCE 8. Cosmic particles of sound.

## Features

- **VST3 format** – VST3-only plugin
- **8-voice polyphony** – Dual oscillators, filter, and ADSR per voice
- **Effects chain** – Reverb, grain delay, phaser, flanger, parametric EQ
- **MIDI input** – Full MIDI note and control support
- **Custom UI** – SpaceDust look and feel
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

3. **Plugin output**: `build\SpaceDust_artefacts\Release\VST3\Space Dust.vst3`

## Project Structure

```
Source/
├── PluginProcessor.*       # Main audio processor
├── PluginEditor.*         # Plugin UI
├── SynthVoice.*           # Voice implementation
├── SynthSound.*           # Sound definition
├── SpaceDustSynthesiser.*  # Synth engine
├── SpaceDustReverb.*      # Reverb
├── SpaceDustGrainDelay.*  # Grain delay
├── SpaceDustPhaser.*      # Phaser
├── SpaceDustFlanger.*     # Flanger
├── SpaceDustParametricEQ.*# Parametric EQ
├── SexiconReverb.*        # Additional reverb
└── SpaceDustLookAndFeel.* # Custom UI styling
```

## References

- [JUCE Documentation](https://docs.juce.com/)
- [JUCE CMake API](https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md)

## License

MIT (or add your preferred license here)
