// =====================================================================
//  Space Dust -- Unison level and balance audit (offline, no ASan)
//  ---------------------------------------------------------------------
//  Giuseppe (2026-08-26): "when I push the width up, the volume goes down.
//  Also it is weirdly favoring one side?"
//
//  UnisonSpread.h on its own is symmetric to within 0.1 dB, so whatever he
//  hears is not the pan law by itself. This drives the WHOLE processor --
//  oversampler, filter, pan gains, the lot -- fires one note per setting and
//  measures each channel, so the numbers include everything the model left out.
//
//  Built by CMake target SpaceDustUnisonAudit when -DENABLE_UNISON_AUDIT=ON.
// =====================================================================
#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdio>
#include <cmath>
#include "PluginProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::unique_ptr<juce::AudioProcessor> proc(createPluginFilter());
    auto* sp = dynamic_cast<SpaceDustAudioProcessor*>(proc.get());
    if (sp == nullptr) { std::puts("FATAL: not a SpaceDustAudioProcessor"); return 2; }

    auto& apvts = sp->getValueTreeState();

    const double sampleRate = 48000.0;
    const int    blockSize  = 512;

    auto setRaw = [&](const char* id, float v)
    {
        if (auto* a = apvts.getRawParameterValue(id)) a->store(v);
        else std::printf("  (warn: no raw param '%s')\n", id);
    };
    auto setChoice = [&](const char* id, int index)
    {
        if (auto* cp = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(id)))
            *cp = index;
        else std::printf("  (warn: no choice param '%s')\n", id);
    };

    // Osc 1 alone, dry, wide open. Anything downstream that colours the two
    // channels differently would hide the effect we are looking for.
    setRaw("osc1Level", 0.8f);
    setRaw("osc2Level", 0.0f);
    setRaw("noiseLevel", 0.0f);
    setRaw("subOscLevel", 0.0f);
    setRaw("subOscOn", 0.0f);
    setRaw("osc1Pan", 0.0f);
    setRaw("osc2Pan", 0.0f);
    setRaw("filterCutoff", 20000.0f);
    setRaw("filterResonance", 0.0f);
    setRaw("filterEnvAmount", 0.0f);
    setRaw("modFilter1Show", 0.0f);
    setRaw("modFilter2Show", 0.0f);
    setRaw("transientEnabled", 0.0f);
    setRaw("delayEnabled", 0.0f);
    setRaw("reverbEnabled", 0.0f);
    setRaw("chorusEnabled", 0.0f);
    setRaw("softClipperEnabled", 0.0f);
    setRaw("lfo1Enabled", 0.0f);
    setRaw("lfo2Enabled", 0.0f);
    // Flat envelope, so the measurement window is steady state.
    setRaw("envAttack", 0.001f);
    setRaw("envDecay", 0.001f);
    setRaw("envSustain", 1.0f);
    setRaw("envRelease", 0.1f);

    struct Result { double rmsL, rmsR, peakL, peakR; };

    auto measure = [&](int shape, int voices, float detune, float width) -> Result
    {
        setChoice("osc1Waveform", shape);
        setRaw("osc1UnisonVoices", (float) voices);
        setRaw("osc1UnisonDetune", detune);
        setRaw("osc1UnisonWidth", width);

        sp->setRateAndBufferSizeDetails(sampleRate, blockSize);
        sp->prepareToPlay(sampleRate, blockSize);

        juce::AudioBuffer<float> buf(2, blockSize);
        double sumL = 0.0, sumR = 0.0, peakL = 0.0, peakR = 0.0;
        long long counted = 0;

        // Long enough to cover several beats. Copies five cents apart at this
        // pitch beat once every 1.6 s, so a shorter window lands wherever it
        // happens to land in the cycle and reports a trough as a level loss --
        // which is exactly what a 1.5 s window did on the first run of this.
        const int numBlocks = (int) (12.0 * sampleRate / blockSize);
        const int skipBlocks = (int) (0.25 * sampleRate / blockSize);

        for (int b = 0; b < numBlocks; ++b)
        {
            buf.clear();
            juce::MidiBuffer midi;
            if (b == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 57, (juce::uint8) 100), 0);

            sp->processBlock(buf, midi);

            if (b < skipBlocks) continue;

            auto* l = buf.getReadPointer(0);
            auto* r = buf.getReadPointer(1);
            for (int i = 0; i < blockSize; ++i)
            {
                sumL += (double) l[i] * l[i];
                sumR += (double) r[i] * r[i];
                peakL = juce::jmax(peakL, (double) std::abs(l[i]));
                peakR = juce::jmax(peakR, (double) std::abs(r[i]));
            }
            counted += blockSize;
        }

        if (counted == 0) counted = 1;
        return { std::sqrt(sumL / counted), std::sqrt(sumR / counted), peakL, peakR };
    };

    auto db = [](double a, double b) { return 20.0 * std::log10((a + 1e-12) / (b + 1e-12)); };

    const char* shapeNames[] = { "Sine", "Saw" };
    const int shapeIds[] = { 0, 1 };

    std::printf("\n================ UNISON AUDIT (whole processor) ================\n");

    for (int s = 0; s < 2; ++s)
    {
        for (float detune : { 0.0f, 0.10f, 0.30f, 0.60f })
        {
            Result ref = measure(shapeIds[s], 1, detune, 0.0f);
            const double refMono = 0.5 * (ref.rmsL + ref.rmsR);

            std::printf("\n--- %s, detune %.2f (1 voice = %.5f) ---\n", shapeNames[s], detune, refMono);
            std::printf("  width :   rmsL     rmsR    L vs 1v   R vs 1v   L-R imbalance\n");
            for (float w = 0.0f; w <= 1.001f; w += 0.25f)
            {
                Result r = measure(shapeIds[s], 7, detune, w);
                std::printf("  %.2f  : %.5f  %.5f   %+6.2f dB %+6.2f dB   %+6.2f dB\n",
                            w, r.rmsL, r.rmsR,
                            db(r.rmsL, refMono), db(r.rmsR, refMono), db(r.rmsL, r.rmsR));
            }
        }
    }

    std::printf("\n--- Saw, detune 0.30, width 1.00, voices sweep ---\n");
    {
        Result ref = measure(1, 1, 0.30f, 0.0f);
        const double refMono = 0.5 * (ref.rmsL + ref.rmsR);
        for (int v = 1; v <= 7; ++v)
        {
            Result r = measure(1, v, 0.30f, 1.0f);
            std::printf("  voices %d : rmsL=%.5f rmsR=%.5f  %+6.2f dB vs 1 voice   imbalance %+6.2f dB\n",
                        v, r.rmsL, r.rmsR, db(0.5 * (r.rmsL + r.rmsR), refMono), db(r.rmsL, r.rmsR));
        }
    }

    // --- Velocity ---
    // Velocity was read at note-on and thrown away before this existed, so what
    // matters is that it now moves the note at all -- and that at FULL velocity
    // the knob changes nothing, which is what keeps saved presets as they were.
    std::printf("\n--- Velocity (Saw, 1 voice, dB re. same amount at v127) ---\n");
    {
        setChoice("osc1Waveform", 1);
        setRaw("osc1UnisonVoices", 1.0f);
        setRaw("osc1UnisonDetune", 0.0f);
        setRaw("osc1UnisonWidth", 0.0f);

        auto atVelocity = [&](float amountPercent, int vel) -> double
        {
            setRaw("velocityAmount", amountPercent);

            sp->setRateAndBufferSizeDetails(sampleRate, blockSize);
            sp->prepareToPlay(sampleRate, blockSize);

            juce::AudioBuffer<float> buf(2, blockSize);
            double sum = 0.0; long long counted = 0;
            const int numBlocks  = (int) (1.0 * sampleRate / blockSize);
            const int skipBlocks = (int) (0.25 * sampleRate / blockSize);

            for (int b = 0; b < numBlocks; ++b)
            {
                buf.clear();
                juce::MidiBuffer midi;
                if (b == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 57, (juce::uint8) vel), 0);
                sp->processBlock(buf, midi);
                if (b < skipBlocks) continue;
                auto* l = buf.getReadPointer(0);
                for (int i = 0; i < blockSize; ++i) sum += (double) l[i] * l[i];
                counted += blockSize;
            }
            return std::sqrt(sum / (counted > 0 ? counted : 1));
        };

        const double reference = atVelocity(0.0f, 127);

        for (float amount : { 0.0f, 50.0f, 100.0f })
        {
            std::printf("  amount %5.1f%% : ", amount);
            for (int vel : { 127, 100, 64, 32 })
                std::printf("v%-3d %+6.2f dB   ", vel,
                            20.0 * std::log10((atVelocity(amount, vel) + 1e-12) / (reference + 1e-12)));
            std::printf("\n");
        }
    }

    // --- Velocity to resonance ---
    // A differential test. Level and cutoff already move with velocity, so a plain
    // "is it quieter" reading cannot tell whether resonance moved as well. Running
    // the SAME velocity sweep at two resonance settings does: the level and cutoff
    // terms are identical in both, so any difference between the two columns is
    // the resonance scaling and nothing else.
    std::printf("\n--- Velocity to resonance (v32 vs v127, amount 100%%) ---\n");
    {
        setChoice("osc1Waveform", 1);
        setRaw("osc1UnisonVoices", 1.0f);
        setRaw("velocityAmount", 100.0f);
        setRaw("filterCutoff", 800.0f);          // low enough that resonance shows

        auto ratioAtResonance = [&](float resonance) -> double
        {
            setRaw("filterResonance", resonance);

            auto level = [&](int vel) -> double
            {
                sp->setRateAndBufferSizeDetails(sampleRate, blockSize);
                sp->prepareToPlay(sampleRate, blockSize);

                juce::AudioBuffer<float> buf(2, blockSize);
                double sum = 0.0; long long counted = 0;
                const int numBlocks  = (int) (1.0 * sampleRate / blockSize);
                const int skipBlocks = (int) (0.25 * sampleRate / blockSize);

                for (int b = 0; b < numBlocks; ++b)
                {
                    buf.clear();
                    juce::MidiBuffer midi;
                    if (b == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 57, (juce::uint8) vel), 0);
                    sp->processBlock(buf, midi);
                    if (b < skipBlocks) continue;
                    auto* l = buf.getReadPointer(0);
                    for (int i = 0; i < blockSize; ++i) sum += (double) l[i] * l[i];
                    counted += blockSize;
                }
                return std::sqrt(sum / (counted > 0 ? counted : 1));
            };

            return 20.0 * std::log10((level(32) + 1e-12) / (level(127) + 1e-12));
        };

        const double flat = ratioAtResonance(0.0f);
        const double sharp = ratioAtResonance(0.9f);

        std::printf("  resonance 0.0 : v32 is %+.2f dB below v127\n", flat);
        std::printf("  resonance 0.9 : v32 is %+.2f dB below v127\n", sharp);
        std::printf("  difference    : %+.2f dB  (zero would mean resonance never moved)\n",
                    sharp - flat);

        setRaw("filterResonance", 0.0f);
        setRaw("filterCutoff", 20000.0f);
    }

    // --- The sub oscillator's own shaping and unison ---
    //
    // The sub had neither until now, so there is no "did it change" to measure
    // against a previous build -- what is measured here is that each control
    // reaches the sub's DSP and does the thing it says, and that the two faults
    // already found in the oscillators' unison are not repeated on it:
    //
    //   * Voices up with Detune at zero must NOT be quiet. Copies spread evenly
    //     around one cycle sum to zero; that was silence at -157 dB before, and
    //     a level near the one-voice level here is what proves the sub seeds its
    //     copies stacked instead.
    //   * Width must move the copies apart and NOT off to one side. The side
    //     signal (L-R)/2 is zero for anything centred, so it rises with Width
    //     from nothing; the L-R imbalance stays near zero throughout, because
    //     spreading is not the same as leaning.
    std::printf("\n================ SUB OSCILLATOR (whole processor) ================\n");
    {
        setRaw("osc1Level", 0.0f);
        setRaw("osc2Level", 0.0f);
        setRaw("noiseLevel", 0.0f);
        setRaw("subOscOn", 1.0f);
        setRaw("subOscLevel", 1.0f);
        setRaw("subOscCoarse", 0.0f);
        setRaw("filterCutoff", 20000.0f);
        setRaw("filterResonance", 0.0f);
        setRaw("velocityAmount", 0.0f);

        auto zeroSubShaping = [&]
        {
            setRaw("subOscBendPlus", 0.0f);
            setRaw("subOscBendMinus", 0.0f);
            setRaw("subOscBendPlusMinus", 0.0f);
            setRaw("subOscSpectrum", 0.0f);
            setRaw("subOscSync", 0.0f);
        };
        zeroSubShaping();

        // rmsS is the SIDE signal, (L-R)/2. Zero for anything in the middle
        // however loud it is, so it separates "spread across the field" from
        // "louder", which a per-channel level cannot.
        struct SubResult { double rmsL, rmsR, rmsS; };

        auto measureSub = [&](int shape, int voices, float detune, float width) -> SubResult
        {
            setChoice("subOscWaveform", shape);
            setRaw("subOscUnisonVoices", (float) voices);
            setRaw("subOscUnisonDetune", detune);
            setRaw("subOscUnisonWidth", width);

            sp->setRateAndBufferSizeDetails(sampleRate, blockSize);
            sp->prepareToPlay(sampleRate, blockSize);

            juce::AudioBuffer<float> buf(2, blockSize);
            double sumL = 0.0, sumR = 0.0, sumS = 0.0;
            long long counted = 0;

            // The same 12 s window the oscillator sweep uses, and for the same
            // reason: the sub runs an octave DOWN, so its beat period is twice
            // as long and a short window is even more likely to sample a trough
            // and report it as a level loss.
            const int numBlocks  = (int) (12.0 * sampleRate / blockSize);
            const int skipBlocks = (int) (0.25 * sampleRate / blockSize);

            for (int b = 0; b < numBlocks; ++b)
            {
                buf.clear();
                juce::MidiBuffer midi;
                if (b == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 57, (juce::uint8) 100), 0);

                sp->processBlock(buf, midi);

                if (b < skipBlocks) continue;

                auto* l = buf.getReadPointer(0);
                auto* r = buf.getReadPointer(1);
                for (int i = 0; i < blockSize; ++i)
                {
                    const double side = 0.5 * ((double) l[i] - (double) r[i]);
                    sumL += (double) l[i] * l[i];
                    sumR += (double) r[i] * r[i];
                    sumS += side * side;
                    }
                counted += blockSize;
            }

            if (counted == 0) counted = 1;
            return { std::sqrt(sumL / counted), std::sqrt(sumR / counted),
                     std::sqrt(sumS / counted) };
        };

        for (float detune : { 0.0f, 0.30f })
        {
            SubResult ref = measureSub(0, 1, detune, 0.0f);
            const double refMono = 0.5 * (ref.rmsL + ref.rmsR);

            std::printf("\n--- Sine sub, detune %.2f, 1 voice = %.5f ---\n", detune, refMono);
            std::printf("  voices width :   rmsL     rmsR     side    vs 1v     L-R\n");

            for (int v : { 1, 3, 5, 7 })
            {
                for (float w : { 0.0f, 0.5f, 1.0f })
                {
                    SubResult r = measureSub(0, v, detune, w);
                    std::printf("    %d    %.2f  : %.5f  %.5f  %.5f  %+6.2f dB  %+6.2f dB\n",
                                v, w, r.rmsL, r.rmsR, r.rmsS,
                                db(0.5 * (r.rmsL + r.rmsR), refMono), db(r.rmsL, r.rmsR));
                }
            }
        }

        // --- Shaping ---
        // A sine, because a sine has nothing but its fundamental: any harmonic
        // that appears when a knob is turned up came from the shaping and could
        // not have come from anywhere else. Each knob is measured alone, against
        // the same sine with all five at zero.
        std::printf("\n--- Sub shaping, one knob at a time (Sine, 1 voice) ---\n");
        {
            setRaw("subOscUnisonVoices", 1.0f);
            setRaw("subOscUnisonDetune", 0.0f);
            setRaw("subOscUnisonWidth", 0.0f);
            setChoice("subOscWaveform", 0);

            // Harmonic 2 of the sub. The sub runs an octave below the played
            // note, so at MIDI 57 (220 Hz) it is at 110 Hz and this is 220 Hz.
            const double subFundamental = 110.0;

            auto harmonicAt = [&](const char* knob, float amount, double freq) -> double
            {
                zeroSubShaping();
                if (knob != nullptr) setRaw(knob, amount);

                sp->setRateAndBufferSizeDetails(sampleRate, blockSize);
                sp->prepareToPlay(sampleRate, blockSize);

                juce::AudioBuffer<float> buf(2, blockSize);
                double re = 0.0, im = 0.0, sumSq = 0.0;
                long long n = 0;

                const int numBlocks  = (int) (2.0 * sampleRate / blockSize);
                const int skipBlocks = (int) (0.25 * sampleRate / blockSize);

                for (int b = 0; b < numBlocks; ++b)
                {
                    buf.clear();
                    juce::MidiBuffer midi;
                    if (b == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 57, (juce::uint8) 100), 0);
                    sp->processBlock(buf, midi);
                    if (b < skipBlocks) continue;

                    auto* l = buf.getReadPointer(0);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const double t = (double) n / sampleRate;
                        const double w = 2.0 * juce::MathConstants<double>::pi * freq * t;
                        re += (double) l[i] * std::cos(w);
                        im += (double) l[i] * std::sin(w);
                        sumSq += (double) l[i] * l[i];
                        ++n;
                    }
                }

                if (n == 0) return 0.0;
                const double mag = 2.0 * std::sqrt(re * re + im * im) / (double) n;
                const double rms = std::sqrt(sumSq / (double) n);
                return 20.0 * std::log10((mag + 1e-12) / (rms + 1e-12));
            };

            const double clean = harmonicAt(nullptr, 0.0f, subFundamental * 2.0);
            std::printf("  2nd harmonic, all knobs at zero : %+7.2f dB re. total\n", clean);

            for (const char* knob : { "subOscBendPlus", "subOscBendMinus",
                                      "subOscBendPlusMinus", "subOscSpectrum", "subOscSync" })
            {
                const double withKnob = harmonicAt(knob, 1.0f, subFundamental * 2.0);
                std::printf("  %-20s at 1.00      : %+7.2f dB   (%+.2f dB vs clean)\n",
                            knob, withKnob, withKnob - clean);
            }

            // Spectrum, on a SAW.
            //
            // It reads +0.00 dB in the table above and that is the right answer,
            // not a dead knob: Spectrum fades a waveform TOWARDS a plain sine,
            // and the test above starts from a sine, so there is nothing for it
            // to take away. Measured here on a saw, where there is -- and where
            // turning it up must take the 5th harmonic DOWN, the opposite
            // direction from every Bend above.
            std::printf("\n--- Sub Spectrum, on a Saw (5th harmonic) ---\n");
            {
                setChoice("subOscWaveform", 1);

                const double fifth = subFundamental * 5.0;
                const double sawClean = harmonicAt(nullptr, 0.0f, fifth);

                std::printf("  Spectrum 0.00 : %+7.2f dB re. total\n", sawClean);

                for (float amount : { 0.50f, 1.00f })
                {
                    const double v = harmonicAt("subOscSpectrum", amount, fifth);
                    std::printf("  Spectrum %.2f : %+7.2f dB re. total   (%+.2f dB vs 0.00)\n",
                                amount, v, v - sawClean);
                }

                setChoice("subOscWaveform", 0);
            }

            zeroSubShaping();
        }

        setRaw("subOscOn", 0.0f);
        setRaw("subOscLevel", 0.0f);
        setRaw("osc1Level", 0.8f);
    }

    // --- The noise source's unison ---
    //
    // Built-in noise is not an oscillator and its unison is not the oscillators':
    // Detune has no pitch to act on, and what Voices and Width buy is a stereo
    // field rather than a thicker note. So the thing to measure is not level --
    // it is CORRELATION between the two sides.
    //
    //   corr = mean(L*R) / sqrt(mean(L*L) * mean(R*R))
    //
    // 1.00 is mono: the two sides carry the same signal, however loud. 0.00 is
    // fully decorrelated -- a real stereo noise field. The claims being checked:
    //
    //   * Width 0 stays at 1.00 whatever Voices says. Independent streams summed
    //     with equal gains both sides is still the same number both sides.
    //   * Width up drops it, and further at higher Voices, because there are more
    //     independent streams to spread.
    //   * The LEVEL barely moves through all of it: 1/sqrt(N) is the right
    //     compensation for streams that never line up, and it is used flat rather
    //     than the detune-dependent one the oscillators take.
    //   * White and Pink both do it. Pink is the one that could go wrong -- its
    //     rows ARE the generator, so copies sharing one would run it N times too
    //     fast and the noise would get brighter with Voices instead of wider.
    std::printf("\n================ NOIZE UNISON (whole processor) ================\n");
    {
        setRaw("osc1Level", 0.0f);
        setRaw("osc2Level", 0.0f);
        setRaw("subOscOn", 0.0f);
        setRaw("subOscLevel", 0.0f);
        setRaw("noiseLevel", 1.0f);
        setRaw("lowShelfAmount", 0.0f);
        setRaw("highShelfAmount", 0.0f);
        setRaw("filterCutoff", 20000.0f);
        setRaw("filterResonance", 0.0f);
        setRaw("velocityAmount", 0.0f);

        struct NoiseResult { double rms, corr, centroid; };

        auto measureNoise = [&](int type, int voices, float detune, float width) -> NoiseResult
        {
            setChoice("noiseType", type);
            setRaw("noiseUnisonVoices", (float) voices);
            setRaw("noiseUnisonDetune", detune);
            setRaw("noiseUnisonWidth", width);

            sp->setRateAndBufferSizeDetails(sampleRate, blockSize);
            sp->prepareToPlay(sampleRate, blockSize);

            juce::AudioBuffer<float> buf(2, blockSize);
            double sumLL = 0.0, sumRR = 0.0, sumLR = 0.0;
            // A crude brightness reading: the mean absolute first difference
            // against the mean absolute sample. It rises with the spectral
            // centroid and needs no FFT, which is all that is wanted here --
            // whether Pink got brighter as Voices went up.
            double sumAbs = 0.0, sumDiff = 0.0;
            float previous = 0.0f;
            long long counted = 0;

            const int numBlocks  = (int) (4.0 * sampleRate / blockSize);
            const int skipBlocks = (int) (0.25 * sampleRate / blockSize);

            for (int b = 0; b < numBlocks; ++b)
            {
                buf.clear();
                juce::MidiBuffer midi;
                if (b == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 57, (juce::uint8) 100), 0);

                sp->processBlock(buf, midi);

                if (b < skipBlocks) continue;

                auto* l = buf.getReadPointer(0);
                auto* r = buf.getReadPointer(1);
                for (int i = 0; i < blockSize; ++i)
                {
                    sumLL += (double) l[i] * l[i];
                    sumRR += (double) r[i] * r[i];
                    sumLR += (double) l[i] * r[i];
                    sumAbs += std::abs((double) l[i]);
                    sumDiff += std::abs((double) l[i] - (double) previous);
                    previous = l[i];
                    ++counted;
                }
            }

            if (counted == 0) counted = 1;
            const double rmsL = std::sqrt(sumLL / counted);
            const double rmsR = std::sqrt(sumRR / counted);
            const double corr = (sumLR / counted) / (rmsL * rmsR + 1e-15);
            const double centroid = sumDiff / (sumAbs + 1e-15);

            return { 0.5 * (rmsL + rmsR), corr, centroid };
        };

        const char* typeNames[] = { "White", "Pink" };

        for (int t = 0; t < 2; ++t)
        {
            NoiseResult ref = measureNoise(t, 1, 0.0f, 0.0f);

            std::printf("\n--- %s noise, 1 voice = rms %.5f, brightness %.4f ---\n",
                        typeNames[t], ref.rms, ref.centroid);
            std::printf("  voices width : correlation   level vs 1v   brightness\n");

            for (int v : { 1, 3, 5, 7 })
            {
                for (float w : { 0.0f, 0.5f, 1.0f })
                {
                    NoiseResult r = measureNoise(t, v, 0.0f, w);
                    std::printf("    %d    %.2f  :    %+6.3f      %+6.2f dB      %.4f\n",
                                v, w, r.corr, db(r.rms, ref.rms), r.centroid);
                }
            }
        }

        // Detune, on built-in noise, must change NOTHING. It has no pitch to pull
        // apart, and a knob that moved the sound here would be moving it by
        // accident.
        std::printf("\n--- Detune on built-in White (7 voices, width 1.00) ---\n");
        for (float d : { 0.0f, 0.30f, 1.00f })
        {
            NoiseResult r = measureNoise(0, 7, d, 1.0f);
            std::printf("  detune %.2f : rms %.5f   correlation %+6.3f\n", d, r.rms, r.corr);
        }

        setRaw("noiseUnisonVoices", 1.0f);
        setRaw("noiseUnisonWidth", 0.0f);
        setRaw("noiseLevel", 0.0f);
        setRaw("osc1Level", 0.8f);
    }

    std::printf("\n================================================================\n");
    return 0;
}
