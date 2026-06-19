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
#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

#include <juce_audio_processors/juce_audio_processors.h>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>   // CFRunLoopRunInMode — message pump for the editor race
#endif
#include <atomic>
#include <thread>
#include <cstdio>
#include <map>

// Defined in the plugin's shared code (PluginProcessor.cpp).
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

// Recursively find the editor's TabbedComponent so the stress can switch pages
// (the Trance Gate / effects controls only lay out when their tab is active).
static juce::TabbedComponent* findTabbed (juce::Component* c)
{
    if (auto* t = dynamic_cast<juce::TabbedComponent*> (c)) return t;
    for (auto* child : c->getChildren())
        if (auto* r = findTabbed (child)) return r;
    return nullptr;
}

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

    // Map parameter IDs -> parameter pointers (for targeted grain-delay torture).
    std::map<juce::String, juce::AudioProcessorParameter*> byId;
    for (auto* p : params)
        if (auto* wid = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
            byId[wid->paramID] = p;
    auto setId = [&] (const char* id, float v)
    {
        auto it = byId.find (id);
        if (it != byId.end() && it->second != nullptr) it->second->setValue (v);
    };
    auto setIdNotify = [&] (const char* id, float v)
    {
        auto it = byId.find (id);
        if (it != byId.end() && it->second != nullptr) it->second->setValueNotifyingHost (v);
    };

    // ---------------------------------------------------------------------
    //  EDITOR RACE: the user crashed while EDITING the Trance Gate with the plugin
    //  WINDOW OPEN under heavy automation. The headless harness never had an editor,
    //  so editor parameter-attachment / resized() callbacks on the message thread
    //  never ran against the audio thread. Recreate that: real editor open, audio on
    //  its own thread, message thread flooding params (Trance Gate heavily) AND
    //  pumping the UI dispatch loop so attachments + combo onChange/resized fire.
    // ---------------------------------------------------------------------
    auto runEditorRace = [&] (int blockSize, int seconds)
    {
        proc->setRateAndBufferSizeDetails (sampleRate, blockSize);
        proc->prepareToPlay (sampleRate, blockSize);

        juce::AudioProcessorEditor* editor = proc->createEditorIfNeeded();
        if (editor == nullptr) { std::puts ("(no editor — skipping editor race)"); proc->releaseResources(); return; }
        editor->setSize (editor->getWidth() > 0 ? editor->getWidth() : 900,
                         editor->getHeight() > 0 ? editor->getHeight() : 700);

        // The Trance Gate lives on the Effects tab; its components only lay out when
        // that tab is active. Find the tab strip so we can switch pages in the loop.
        juce::TabbedComponent* tabs = findTabbed (editor);
        const int numTabs = tabs != nullptr ? tabs->getNumTabs() : 0;
        std::printf ("    (editor %dx%d, tabs=%d)\n", editor->getWidth(), editor->getHeight(), numTabs);

        std::atomic<bool> running { true };
        std::thread audioThread ([&]
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            juce::Random arng (0xA1);
            int ctr = 0;
            while (running.load (std::memory_order_relaxed))
            {
                buf.clear();
                juce::MidiBuffer midi;
                if ((ctr++ % 4) == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 48 + arng.nextInt (24), (juce::uint8) 100), 0);
                proc->processBlock (buf, midi);
            }
        });

        juce::Random rng (0x7ACE);
        const auto end = juce::Time::currentTimeMillis() + (juce::int64) seconds * 1000;
        int tabTick = 0;
        while (juce::Time::currentTimeMillis() < end)
        {
            // Cycle through ALL editor pages so every section (incl. the Trance Gate
            // on the Effects tab) is laid out and live under the automation flood.
            if (tabs != nullptr && numTabs > 0)
                tabs->setCurrentTabIndex ((tabTick++) % numTabs);

            // Heavy automation from the message thread (like the host + UI both moving params).
            for (int c = 0; c < 20; ++c)
                if (auto* p = params[rng.nextInt (params.size())]) p->setValueNotifyingHost (rng.nextFloat());

            // Hammer the Trance Gate specifically (steps count cycles 4/8/16 -> fires
            // the combo onChange -> resized(); step toggles flip the 16 step buttons).
            setIdNotify ("tranceGateEnabled", 1.0f);
            setIdNotify ("tranceGateSteps", rng.nextFloat());      // -> 4/8/16, triggers resized()
            for (int s = 1; s <= 16; ++s)
                setIdNotify (("tranceGateStep" + juce::String (s)).toRawUTF8(), (rng.nextFloat() < 0.5f) ? 1.0f : 0.0f);
            setIdNotify ("tranceGateRate", rng.nextFloat());
            setIdNotify ("tranceGateSync", (rng.nextFloat() < 0.5f) ? 1.0f : 0.0f);

            // Hammer the Reverb FILTER specifically — the user's live crash trigger.
            // reverbFilterShow toggles the filter UI's visibility, driving the editor's
            // resized()/child show-hide on the message thread WHILE the audio thread
            // runs reverb_.process() reading the same params: the exact editor race.
            setIdNotify ("reverbEnabled", 1.0f);
            setIdNotify ("reverbType", rng.nextFloat());                                  // VoidVerb <-> Freeverb
            setIdNotify ("reverbFilterShow", (rng.nextFloat() < 0.5f) ? 1.0f : 0.0f);     // -> resized()
            setIdNotify ("reverbFilterWarmSaturation", (rng.nextFloat() < 0.5f) ? 1.0f : 0.0f);
            setIdNotify ("reverbFilterHPCutoff", rng.nextFloat());
            setIdNotify ("reverbFilterHPResonance", rng.nextFloat());
            setIdNotify ("reverbFilterLPCutoff", rng.nextFloat());
            setIdNotify ("reverbFilterLPResonance", rng.nextFloat());
            setIdNotify ("reverbDecayTime", rng.nextFloat());
            setIdNotify ("reverbWetMix", rng.nextFloat());

            // Pump the native message queue so JUCE's async attachment updates +
            // combo onChange/resized() actually run (this thread is the message thread).
           #if JUCE_WINDOWS
            MSG msg;
            while (PeekMessageW (&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage (&msg);
                DispatchMessageW (&msg);
            }
           #elif JUCE_MAC
            // macOS: pump the CFRunLoop on this (the message) thread so JUCE's async
            // updaters / parameter-attachment updates + combo onChange/resized()
            // actually fire, racing the audio thread exactly as in the live editor
            // session. (Previously the pump was Windows-only, so the headless macOS
            // run never exercised the editor-thread path — and never reproduced the
            // crash.) runDispatchLoopUntil() is unavailable here: it needs
            // JUCE_MODAL_LOOPS_PERMITTED, off by default in modern JUCE.
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.002, true);
           #endif
        }

        running = false;
        audioThread.join();
        proc->editorBeingDeleted (editor);
        delete editor;
        proc->releaseResources();
    };

    for (int bs : { 64, 128, 256, 512 })
    {
        std::printf ("=== editor race (Trance Gate) @ block %d ===\n", bs);
        runEditorRace (bs, 12);
    }

    // ---------------------------------------------------------------------
    //  Block-size violation: a host calls processBlock with MORE samples than it
    //  declared to prepareToPlay (Ableton does this during some operations). Any
    //  buffer sized to the prepared block then overflows. The harness never did
    //  this before — prepare and process were always matched.
    // ---------------------------------------------------------------------
    auto runBlockSizeViolation = [&] (double sr, int prepBlock, int maxProcBlock)
    {
        proc->setRateAndBufferSizeDetails (sr, prepBlock);
        proc->prepareToPlay (sr, prepBlock);          // declares prepBlock as the max
        juce::Random rng (0xB10C);
        // Turn the LFOs on (they fill per-sample buffers sized to prepBlock).
        setId ("lfo1Enabled", 1.0f);
        setId ("lfo2Enabled", 1.0f);
        for (auto& kv : byId) if (kv.first.contains ("Enabled")) kv.second->setValue (1.0f);

        // Double the process block each step (prep*2, *4, ...) so we quickly reach
        // very large blocks and fault on the FIRST buffer past its capacity.
        for (int proc_bs = prepBlock * 2; proc_bs <= maxProcBlock; proc_bs *= 2)
        {
            juce::AudioBuffer<float> buf (2, proc_bs);   // BIGGER than prepared
            for (int rep = 0; rep < 120; ++rep)
            {
                for (int c = 0; c < 16; ++c)
                    if (auto* p = params[rng.nextInt (params.size())]) p->setValue (rng.nextFloat());
                for (auto& kv : byId) if (kv.first.contains ("Enabled")) kv.second->setValue (1.0f);
                // Force the grain delay hot (the real-world trigger).
                setId ("grainDelayEnabled", 1.0f);
                setId ("grainDelayDensity", 0.9f);
                setId ("grainDelayMix",     0.8f);
                setId ("grainDelayDecay",   0.8f);
                setId ("grainDelaySize",    rng.nextFloat());
                buf.clear();
                juce::MidiBuffer midi;
                midi.addEvent (juce::MidiMessage::noteOn (1, 48 + rng.nextInt (24), (juce::uint8) 100),
                               rng.nextInt (proc_bs));
                proc->processBlock (buf, midi);          // overflows any prepBlock-sized buffer
            }
        }
        proc->releaseResources();
    };

    for (double sr : { 44100.0, 48000.0 })
        for (int pb : { 32, 64, 128, 256, 512 })
        {
            std::printf ("=== block-size violation @ sr %.0f prep %d -> up to 65536 ===\n", sr, pb);
            runBlockSizeViolation (sr, pb, 65536);
        }

    // ---------------------------------------------------------------------
    //  Grain-delay torture: the crash log was wall-to-wall grain SPAWN/RETIRE.
    //  Force the grain delay ON and sustain heavy grain generation while
    //  automating its params and playing dense notes, single-threaded like a
    //  host. Grains accumulate over many blocks exactly as in the real session.
    // ---------------------------------------------------------------------
    auto runGrainTorture = [&] (double sr, int blockSize, int numBlocks, juce::uint32 seed)
    {
        proc->setRateAndBufferSizeDetails (sr, blockSize);
        proc->prepareToPlay (sr, blockSize);
        juce::AudioBuffer<float> buf (2, blockSize);
        juce::Random rng (seed);
        for (int b = 0; b < numBlocks; ++b)
        {
            // Automate EVERYTHING (full chain under heavy automation).
            for (int c = 0; c < 24; ++c)
                if (auto* p = params[rng.nextInt (params.size())]) p->setValue (rng.nextFloat());

            // Force EVERY effect ON so the grain delay feeds a fully-loaded chain
            // (the grain delay's feedback can emit extreme values a downstream
            // effect might index a buffer/table with). Done AFTER the flood so the
            // random writes can't switch them back off.
            for (auto& kv : byId)
                if (kv.first.contains ("Enabled")) kv.second->setValue (1.0f);

            // Force the grain delay into a sustained hot state.
            setId ("grainDelayEnabled", 1.0f);
            setId ("grainDelayDensity", 0.7f + 0.3f * rng.nextFloat()); // high density
            setId ("grainDelayMix",     0.5f + 0.5f * rng.nextFloat());
            setId ("grainDelaySize",    rng.nextFloat());      // sweep grain size
            setId ("grainDelayTime",    rng.nextFloat());      // sweep delay time
            setId ("grainDelayPitch",   rng.nextFloat());      // sweep pitch
            setId ("grainDelayJitter",  rng.nextFloat());      // sweep jitter
            setId ("grainDelayDecay",   0.5f + 0.5f * rng.nextFloat()); // long feedback
            setId ("grainDelayPingPong", (rng.nextFloat() < 0.5f) ? 1.0f : 0.0f);
            setId ("grainDelayFilterShow", (rng.nextFloat() < 0.5f) ? 1.0f : 0.0f);

            buf.clear();
            juce::MidiBuffer midi;
            // Dense notes feed the grain buffer continuously.
            if ((b % 2) == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 36 + rng.nextInt (60),
                                                          (juce::uint8) (60 + rng.nextInt (60))),
                               rng.nextInt (blockSize));
            if ((b % 5) == 0)
                midi.addEvent (juce::MidiMessage::noteOff (1, 36 + rng.nextInt (60)),
                               rng.nextInt (blockSize));
            proc->processBlock (buf, midi);
        }
        proc->releaseResources();
    };

    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (int bs : { 32, 64, 128, 256, 512, 1024 })
        {
            std::printf ("=== grain torture @ sr %.0f block %d ===\n", sr, bs);
            runGrainTorture (sr, bs, 80000, (juce::uint32) (bs + (int) sr));
        }

    // ---------------------------------------------------------------------
    //  Live-like pass: a VST3 host applies automation ON THE AUDIO THREAD in
    //  the SAME processBlock as the incoming MIDI note. This is single-threaded
    //  (no cross-thread race) and matches "added a MIDI note while there was
    //  super heavy automation" far more closely than the flood race above.
    // ---------------------------------------------------------------------
    auto runLivePass = [&] (int blockSize, int numBlocks, juce::uint32 seed)
    {
        proc->setRateAndBufferSizeDetails (sampleRate, blockSize);
        proc->prepareToPlay (sampleRate, blockSize);

        juce::AudioBuffer<float> buf (2, blockSize);
        juce::Random rng (seed);
        const auto& ps = proc->getParameters();

        for (int b = 0; b < numBlocks; ++b)
        {
            // Heavy automation: a batch of parameter writes applied on THIS
            // (audio) thread before rendering, exactly as the host drains its
            // parameter queue ahead of processBlock.
            const int changes = 8 + rng.nextInt (40);
            for (int c = 0; c < changes; ++c)
                if (auto* p = ps[rng.nextInt (ps.size())])
                    p->setValue (rng.nextFloat());

            buf.clear();
            juce::MidiBuffer midi;
            // Drop notes mid-stream, at random sample offsets inside the block,
            // so a voice starts rendering through the just-automated filter.
            if ((b % 3) == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 36 + rng.nextInt (60),
                                                          (juce::uint8) (40 + rng.nextInt (80))),
                               rng.nextInt (blockSize));
            if ((b % 7) == 0)
                midi.addEvent (juce::MidiMessage::noteOff (1, 36 + rng.nextInt (60)),
                               rng.nextInt (blockSize));

            proc->processBlock (buf, midi);
        }
        proc->releaseResources();
    };

    for (juce::uint32 seed : { 0x1u, 0xBEEFu, 0x5151u })
        for (int bs : { 64, 128, 256, 512, 1024 })
        {
            std::printf ("=== Live-like @ block %d seed %u ===\n", bs, seed);
            runLivePass (bs, 60000, seed);
        }

    // Sweep block sizes; run with and without notes (idle-synth matched the user's crash).
    for (int bs : { 64, 128, 256, 512, 1024 })
    {
        std::printf ("=== flood @ block %d (notes) ===\n", bs);   runPass (bs, 200000, true);
        std::printf ("=== flood @ block %d (idle)  ===\n", bs);   runPass (bs, 200000, false);
    }

    std::puts ("STRESS DONE - no AddressSanitizer error.");
    return 0;
}
