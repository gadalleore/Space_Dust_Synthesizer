// =====================================================================
//  Space Dust — Transient Pre/Post filter routing test (offline, no ASan)
//  ---------------------------------------------------------------------
//  Verifies that the "Post Effect" toggle actually changes whether the
//  Main-tab filter colours the transient:
//    - Post OFF (Pre):  transient runs through the filter mirror  -> cut
//    - Post ON  (Post): transient added at end of chain           -> NOT cut
//
//  Method: silence the oscillators/noise (so the ONLY sound is the
//  transient), pick a bright hi-hat transient, close the Main filter to a
//  low low-pass, fire a note, render, and measure peak output. A working
//  toggle gives a much LOUDER peak with Post ON than Post OFF.
//
//  Built by CMake target SpaceDustTransientTest when -DENABLE_TRANSIENT_TEST=ON.
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

    const double sampleRate = 44100.0;
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

    // --- Silence the synth body so we measure ONLY the transient ---
    setRaw("osc1Level", 0.0f);
    setRaw("osc2Level", 0.0f);
    setRaw("noiseLevel", 0.0f);
    setRaw("subOscLevel", 0.0f);
    setRaw("subOscOn", 0.0f);

    // --- Close the Main filter: low-pass at 150 Hz (kills a bright hi-hat) ---
    setRaw("filterMode", 0.0f);       // 0 = Low Pass
    setRaw("filterCutoff", 150.0f);
    setRaw("filterResonance", 0.0f);

    // Make sure the Mod filters are NOT interfering with this test.
    setRaw("modFilter1Show", 0.0f);
    setRaw("modFilter2Show", 0.0f);

    // --- Transient: bright closed hat (HP-filtered, high freq) ---
    setRaw("transientEnabled", 1.0f);
    setRaw("transientMix", 1.0f);
    setRaw("transientKaDonk", 0.0f);
    setRaw("transientCoarse", 0.0f);
    setRaw("transientLength", 1.0f);
    setChoice("transientType", 2);    // 2 = ClosedHat808 (bright, high frequency)

    auto measurePeak = [&](bool postEffect) -> float
    {
        setRaw("transientPostEffect", postEffect ? 1.0f : 0.0f);

        // Fresh prepare each pass so filter/transient state is clean.
        sp->setRateAndBufferSizeDetails(sampleRate, blockSize);
        sp->prepareToPlay(sampleRate, blockSize);

        juce::AudioBuffer<float> buf(2, blockSize);
        float peak = 0.0f;

        const int numBlocks = static_cast<int>(0.5 * sampleRate / blockSize) + 1; // ~0.5 s
        for (int b = 0; b < numBlocks; ++b)
        {
            buf.clear();
            juce::MidiBuffer midi;
            if (b == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)110), 0);

            sp->processBlock(buf, midi);

            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            {
                auto* d = buf.getReadPointer(ch);
                for (int i = 0; i < blockSize; ++i)
                    peak = juce::jmax(peak, std::abs(d[i]));
            }
        }
        return peak;
    };

    const float prePeak  = measurePeak(false); // Post OFF -> filtered
    const float postPeak = measurePeak(true);  // Post ON  -> unfiltered

    std::printf("\n=== Transient Pre/Post filter routing ===\n");
    std::printf("Main filter: LP @ 150 Hz, transient = ClosedHat808 (bright)\n");
    std::printf("Post OFF (Pre,  through filter mirror): peak = %.5f\n", prePeak);
    std::printf("Post ON  (Post, end of chain)        : peak = %.5f\n", postPeak);

    if (postPeak > prePeak * 2.0f)
        std::printf("RESULT: toggle WORKS — Post ON is much louder (transient bypasses the filter).\n");
    else if (prePeak < 1.0e-4f && postPeak < 1.0e-4f)
        std::printf("RESULT: INCONCLUSIVE — both near silent (transient may not have triggered).\n");
    else
        std::printf("RESULT: SUSPECT — Post ON is NOT clearly louder; the filter may be cutting it in both modes.\n");

    return 0;
}
