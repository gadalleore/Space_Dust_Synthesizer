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

    std::printf("\n================================================================\n");
    return 0;
}
