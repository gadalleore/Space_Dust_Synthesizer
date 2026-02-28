# Reverb Research & Implementation Guide for Space Dust

Synthesized from the resources you provided. Use this as a reference when implementing the Reverb section.

---

## 1. Reverb Types Overview

| Type | Description | Character |
|------|-------------|-----------|
| **Spring** | Mechanical, springs | Twangy, metallic (guitar amps) |
| **Plate** | Vibrating metal plates (EMT 140) | Smooth, dense tails |
| **Room/Hall/Chamber** | Algorithmic spaces | Early reflections + late decay |
| **Convolution** | Impulse responses (IR) | Realistic emulation of real spaces |
| **Algorithmic** | DSP (Schroeder, Freeverb) | Delay lines, allpass, feedback |
| **Shimmer** | Pitch-shifted reverb | Ethereal, octave-up tails |

---

## 2. JUCE Built-in Reverb (Fastest Path)

JUCE provides `juce::Reverb` (FreeVerb-based) in `juce_audio_basics`. **Parameters:**

| Parameter | Range | Description |
|-----------|-------|-------------|
| `roomSize` | 0–1 | 0 = small, 1 = big |
| `damping` | 0–1 | 0 = bright, 1 = fully damped |
| `wetLevel` | 0–1 | Wet mix |
| `dryLevel` | 0–1 | Dry mix |
| `width` | 0–1 | Stereo width (1 = very wide) |
| `freezeMode` | 0–1 | <0.5 = normal, >0.5 = infinite hold |

**Usage:**
```cpp
#include <juce_audio_basics/juce_audio_basics.h>

juce::Reverb reverb;
reverb.setSampleRate(sampleRate);

juce::Reverb::Parameters params;
params.roomSize = 0.7f;
params.damping = 0.5f;
params.wetLevel = 0.33f;
params.dryLevel = 0.4f;
params.width = 1.0f;
reverb.setParameters(params);

// In processBlock:
reverb.processStereo(leftChannel, rightChannel, numSamples);
```

For `ProcessorChain` integration, use `juce::dsp::Reverb` (wrapper in `juce_dsp`).

---

## 3. Algorithmic Reverb Building Blocks (Valhalla DSP)

From Valhalla's "Getting Started" series:

- **Delay lines** – Read/write buffers, distance = delay time
- **Allpass filters** – Critical for high-density, colorless echoes
- **Modulators** – LFOs or band-limited noise to vary delay lengths (reduces metallic artifacts)
- **Filters** – In feedback path (e.g. lowpass for damping)

**Key resources:**
- Valhalla Part 1: [Dev Environments](https://valhalladsp.com/2021/09/20/getting-started-with-reverb-design-part-1-dev-environments) – JUCE recommended
- Valhalla Part 3: [Online Resources](https://valhalladsp.com/2021/09/23/getting-started-with-reverb-design-part-3-online-resources/) – Spin Semiconductor FV-1 notes, MusicDSP.org, DAFX

---

## 4. Schroeder Algorithm (Foundation)

- **Comb filters** (parallel): `FBCF = 1 / (1 - g*z^-N)` – colored echoes
- **Allpass filters** (series): `AP = (-g + z^-N) / (1 - g*z^-N)` – colorless density
- **Delay lengths**: Mutually prime (e.g. 1051, 337, 113 samples)
- **g**: ~0.7 for allpass

**C++ implementations:**
- [ruarim/Schroeder_FDN_Reverb](https://github.com/ruarim/schroeder_fdn_reverb) – FDN with Schroeder
- [erthal11/Schroeder-s-Reverb](https://github.com/erthal11/Schroeder-s-Reverb) – JUCE plugin with mix, size, decay, gain

---

## 5. Famous Hardware Emulations (Reference)

| Hardware | Character | Software Emulation |
|----------|-----------|--------------------|
| Lexicon 480L | Lush halls, plates | Relab LX480, Valhalla VintageVerb |
| EMT 140 Plate | Dense, natural | UAD EMT 140, Soundtoys SuperPlate |
| Eventide SP2016 | Versatile | Eventide SP2016 Reverb |
| Bricasti M7 | Transparent, natural | LiquidSonics Seventh Heaven |

---

## 6. Papers & Deep Dives

- **Schroeder (1962)** – Natural sound artificial reverberation (comb/allpass)
- **Dattorro (1997)** – Plate algorithm, CCRMA "Effect Design"
- **Valhalla Part 2** – [Best Papers](https://valhalladsp.com/2021/09/22/getting-started-with-reverb-design-part-2-the-foundations/)
- **CCRMA Stanford** – Schroeder, Dattorro docs
- **DAFX** – Modern reverb research archive

---

## 7. Space Dust Implementation Strategy

**Phase 1 (Quick win):** Use JUCE `juce::Reverb` with 5–6 params:
- Room Size, Damping, Wet/Dry, Width, (optional Freeze)

**Phase 2 (Custom):** Extend or replace with Schroeder-style comb + allpass. Add delay modulation for smoother tails.

**Phase 3 (Advanced):** Add reverb type selector (Room, Plate, Hall) by mapping params to different delay/allpass tunings.

---

## 8. Suggested Reverb Parameters for Space Dust UI

| Parameter | Range | Default | Notes |
|-----------|-------|---------|-------|
| On | Toggle | Off | Bypass |
| Room Size | 0–1 | 0.5 | Small → large space |
| Damping | 0–1 | 0.5 | Bright → dark |
| Mix | 0–1 | 0.33 | Wet amount |
| Width | 0–1 | 1.0 | Stereo width |

---

## 9. Signal Flow (Space Dust)

Current: `Synth → [Delay] → Output`

Add: `Synth → [Delay] → [Reverb] → Output` (or parallel: Delay + Reverb → mix)

---

*Generated from research resources. Last updated: Feb 2025*
