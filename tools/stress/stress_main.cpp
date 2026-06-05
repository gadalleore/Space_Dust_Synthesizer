// =====================================================================
//  Space Dust parameter-flood stress harness  (ASan builds only)
//  ---------------------------------------------------------------------
//  Reproduces a host flooding parameter changes from the message thread
//  while the audio thread renders — the "draw automation on every element"
//  scenario the editor/pluginval cannot replicate. Run under AddressSanitizer
//  so any heap-corruption / use-after-free / race-induced overwrite faults
//  AT the offending instruction with a Space Dust stack.
//
//  Built by CMake target SpaceDustStress when -DENABLE_ASAN=ON.
// =====================================================================
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <thread>
#include <cstdio>

// Defined in the plugin's shared code (PluginProcessor.cpp).
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;  // MessageManager for any JUCE internals

    std::unique_ptr<juce::AudioProcessor> proc (createPluginFilter());
    if (proc == nullptr) { std::puts("FATAL: createPluginFilter() returned null"); return 2; }

    const auto& params = proc->getParameters();
    std::printf("Loaded processor with %d parameters.\n", params.size());
    if (params.isEmpty()) { std::puts("FATAL: no parameters"); return 2; }

    const double sampleRate = 44100.0;

    auto runPass = [&] (int blockSize, int floodIters, bool playNotes)
    {
        proc->setRateAndBufferSizeDetails (sampleRate, blockSize);
        proc->prepareToPlay (sampleRate, blockSize);

        std::atomic<bool> running { true };

        // ---- audio thread: render continuously, with notes optionally held ----
        std::thread audioThread ([&]
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            juce::Random rng (0x5eed);
            int counter = 0;
            while (running.load (std::memory_order_relaxed))
            {
                buf.clear();
                juce::MidiBuffer midi;
                if (playNotes)
                {
                    // Hold a rotating cluster of notes so voices/grains are live
                    // while parameters are reconfigured underneath them.
                    if ((counter % 8) == 0)
                        midi.addEvent (juce::MidiMessage::noteOn (1, 36 + rng.nextInt (60),
                                                                 (juce::uint8) 100), 0);
                    if ((counter % 8) == 4)
                        midi.addEvent (juce::MidiMessage::noteOff (1, 36 + rng.nextInt (60)), 0);
                }
                proc->processBlock (buf, midi);
                ++counter;
            }
        });

        // ---- this thread: flood EVERY parameter with random values ----
        juce::Random rng (0xC0FFEE);
        for (int i = 0; i < floodIters; ++i)
        {
            auto* p = params[rng.nextInt (params.size())];
            if (p != nullptr)
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (rng.nextFloat());
                p->endChangeGesture();
            }
        }

        running = false;
        audioThread.join();
        proc->releaseResources();
    };

    // Sweep block sizes; run with and without notes (idle-synth matched the user's crash).
    for (int bs : { 64, 128, 256, 512, 1024 })
    {
        std::printf ("=== flood @ block %d (notes) ===\n", bs);   runPass (bs, 200000, true);
        std::printf ("=== flood @ block %d (idle)  ===\n", bs);   runPass (bs, 200000, false);
    }

    std::puts ("STRESS DONE - no AddressSanitizer error.");
    return 0;
}
