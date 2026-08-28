// =====================================================================
//  Chunked effects chain audit
//  ---------------------------------------------------------------------
//  Everything about per-sample modulation of an effect rests on one
//  claim: running the effects chain in 32-sample pieces produces the
//  same samples as running it whole. This renders the same notes both
//  ways and compares them bit for bit.
//
//  Build: cmake -B build -DENABLE_CHUNK_AUDIT=ON
//         cmake --build build --config Release --target SpaceDustChunkAudit
// =====================================================================
#include "PluginProcessor.h"
#include "PresetManager.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace
{
    /** Put the player's loaded patch back after this harness has trampled it.

        SpaceDustAudioProcessor starts PresetHotReload from its own CONSTRUCTOR,
        which publishes live parameter state to current.sdpreset. Merely building
        a processor here overwrites whatever patch the player had loaded. */
    struct LiveStateGuard
    {
        explicit LiveStateGuard (juce::File fileToGuard)
            : file (std::move (fileToGuard))
        {
            had = file.existsAsFile() && file.loadFileAsData (data);
        }

        ~LiveStateGuard()
        {
            if (had)
                file.replaceWithData (data.getData(), data.getSize());
        }

        juce::File       file;
        juce::MemoryBlock data;
        bool             had = false;
    };

    constexpr double sampleRate = 44100.0;
    constexpr int    blockSize  = 512;
    constexpr int    numBlocks  = 40;

    /** Render a fixed two-note phrase through an already-configured processor
        and return every sample. */
    std::vector<float> renderWith (SpaceDustAudioProcessor& sd)
    {
        auto* proc = static_cast<juce::AudioProcessor*> (&sd);

        proc->prepareToPlay (sampleRate, blockSize);

        juce::AudioBuffer<float> buffer (2, blockSize);
        std::vector<float> out;
        out.reserve ((size_t) (numBlocks * blockSize * 2));

        for (int b = 0; b < numBlocks; ++b)
        {
            buffer.clear();
            juce::MidiBuffer midi;

            if (b == 0)
            {
                midi.addEvent (juce::MidiMessage::noteOn  (1, 57, 0.9f), 0);
                midi.addEvent (juce::MidiMessage::noteOn  (1, 64, 0.9f), 8);
            }
            if (b == 30)
            {
                midi.addEvent (juce::MidiMessage::noteOff (1, 57), 0);
                midi.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
            }

            proc->processBlock (buffer, midi);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    out.push_back (buffer.getSample (ch, i));
        }

        proc->releaseResources();
        return out;
    }

    /** The same phrase on a fresh processor. forceChunking makes the effects
        chain run in 32-sample pieces even though nothing is modulated. */
    std::vector<float> render (bool forceChunking)
    {
        std::unique_ptr<juce::AudioProcessor> proc (createPluginFilter());
        auto* sd = dynamic_cast<SpaceDustAudioProcessor*> (proc.get());

        sd->setForceEffectChunkingForTests (forceChunking);

        return renderWith (*sd);
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const LiveStateGuard liveState (PresetManager::appDataFolder()
                                        .getChildFile ("current.sdpreset"));

    std::printf ("Chunked effects chain audit\n");
    std::printf ("===========================\n\n");

    const auto whole   = render (false);
    const auto chunked = render (true);

    if (whole.size() != chunked.size())
    {
        std::printf ("  FAIL  sample counts differ (%zu vs %zu)\n",
                     whole.size(), chunked.size());
        return 1;
    }

    size_t differing = 0;
    double worst = 0.0;

    for (size_t i = 0; i < whole.size(); ++i)
    {
        const double d = std::abs ((double) whole[i] - (double) chunked[i]);

        if (d != 0.0)
        {
            ++differing;
            worst = juce::jmax (worst, d);
        }
    }

    std::printf ("  samples compared : %zu\n", whole.size());
    std::printf ("  samples differing: %zu\n", differing);
    std::printf ("  largest gap      : %.12f\n\n", worst);

    if (differing != 0)
    {
        // Never the string "error :" -- MSBuild reads that as a build failure.
        std::printf ("  FAIL  chunking changed the audio. The chunking is wrong.\n");
        return 1;
    }

    std::printf ("  chunked output is bit-identical to whole-block output.\n");

    // -- the MODMATRIX round trip --
    // Task 2 added saving and loading of the routing list but could not exercise
    // it: this is the first harness in the plan that builds a real processor.
    // The failure it guards against is silent -- a list that saves but does not
    // come back leaves the patch sounding flat with no error anywhere.
    {
        std::unique_ptr<juce::AudioProcessor> proc (createPluginFilter());
        auto* sd = dynamic_cast<SpaceDustAudioProcessor*> (proc.get());

        sd->modMatrix.setRouting (0, "reverbWetMix", 0.5f);
        sd->modMatrix.setRouting (2, "filterCutoff", -0.75f);

        juce::MemoryBlock saved;
        sd->getStateInformation (saved);

        sd->modMatrix.clear();
        sd->setStateInformation (saved.getData(), (int) saved.getSize());

        const bool ok = sd->modMatrix.routings().size() == 2
                     && std::abs (sd->modMatrix.amountFor (0, "reverbWetMix") - 0.5f)  < 1.0e-6f
                     && std::abs (sd->modMatrix.amountFor (2, "filterCutoff") + 0.75f) < 1.0e-6f;

        std::printf ("\n  MODMATRIX round trip: %d routing(s) restored\n",
                     (int) sd->modMatrix.routings().size());

        if (! ok)
        {
            std::printf ("  FAIL  the routing list did not survive save and load.\n");
            return 1;
        }

        std::printf ("  the routing list survives save and load.\n");
    }

    // -- the destination sweep --
    // A routing that reaches an effect must audibly change the output, and it
    // must do so through the chunked path. Without this, a matrix that compiles
    // to nothing would pass every test above.
    //
    // Two FRESH processors rather than one rendered twice: the reverb tail, the
    // delay lines and the LFO phase all survive a render, so a second pass on
    // the same instance would start somewhere else and the difference measured
    // would be that, not the routing. The bit-identical check above already
    // proves two fresh processors render the same samples.
    {
        // Reverb on and wide open, so the knob under test has something to move.
        auto configure = [] (SpaceDustAudioProcessor& sd)
        {
            auto set = [&] (const char* id, float value)
            {
                if (auto* p = sd.getValueTreeState().getParameter (id))
                    p->setValueNotifyingHost (p->convertTo0to1 (value));
            };

            set ("reverbEnabled", 1.0f);
            set ("reverbWetMix", 0.5f);
            set ("lfo1Enabled", 1.0f);
            set ("lfo1Depth", 100.0f);
            set ("lfo1Sync", 0.0f);
            set ("lfo1Rate", 6.0f);
        };

        std::unique_ptr<juce::AudioProcessor> flatProc (createPluginFilter());
        auto* flatSd = dynamic_cast<SpaceDustAudioProcessor*> (flatProc.get());
        configure (*flatSd);
        const auto flat = renderWith (*flatSd);

        std::unique_ptr<juce::AudioProcessor> movedProc (createPluginFilter());
        auto* movedSd = dynamic_cast<SpaceDustAudioProcessor*> (movedProc.get());
        configure (*movedSd);

        movedSd->modMatrix.setRouting (0, "reverbWetMix", 1.0f);
        movedSd->rebuildCompiledRoutings();

        const auto moved = renderWith (*movedSd);

        double biggest = 0.0;
        for (size_t i = 0; i < flat.size() && i < moved.size(); ++i)
            biggest = juce::jmax (biggest, std::abs ((double) flat[i] - (double) moved[i]));

        std::printf ("\n  destination sweep, LFO 1 -> reverbWetMix\n");
        std::printf ("  largest difference: %.9f\n", biggest);

        if (biggest < 1.0e-6)
        {
            // Never the string "error :" -- MSBuild reads that as a build failure.
            std::printf ("  FAIL  an assigned routing changed nothing.\n");
            return 1;
        }

        std::printf ("  an assigned routing moves the sound.\n");
    }

    return 0;
}
