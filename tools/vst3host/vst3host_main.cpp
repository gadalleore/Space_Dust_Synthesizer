// =====================================================================
//  Space Dust VST3-wrapper crash host  (ASan builds only)
//  ---------------------------------------------------------------------
//  The headless processor harness and the Standalone both bypass the VST3
//  wrapper — they talk to SpaceDustAudioProcessor directly. The live crash
//  happened in ABLETON, i.e. through the VST3 wrapper + host-driven threading.
//  This program closes that gap: it is ITSELF AddressSanitizer-instrumented
//  (so the ASan runtime is loaded first), then it HOSTS the real .vst3 via
//  juce::VST3PluginFormat. Loading the ASan plugin from an ASan host means
//  interceptors are installed before the dlopen — no "loaded too late" abort.
//
//  It then drives the user's repro THROUGH THE WRAPPER:
//    * apply each factory preset via setStateInformation (the host state path
//      a VST3 host uses — internally replaceState + updateVoicesWithParameters)
//    * render notes into each preset
//    * a cross-thread race: audio renders+notes while the message thread keeps
//      slamming preset state in (like flipping presets live)
//    * an editor + preset + offscreen-paint race
//
//  Run under ASan; any OOB write / use-after-free in the wrapper path faults
//  at the offending instruction.
// =====================================================================
#if JUCE_MAC || defined(__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
#endif

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstdlib>

#ifndef SPACEDUST_VST3_PATH
 #define SPACEDUST_VST3_PATH ""
#endif
#ifndef SPACEDUST_PRESET_DIR
 #define SPACEDUST_PRESET_DIR ""
#endif

static juce::MidiMessage noteOn (int n)  { return juce::MidiMessage::noteOn (1, n, (juce::uint8) 100); }
static juce::MidiMessage noteOff (int n) { return juce::MidiMessage::noteOff (1, n); }

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::String vst3Path = (argc > 1) ? juce::String (argv[1]) : juce::String (SPACEDUST_VST3_PATH);
    std::printf ("Hosting VST3: %s\n", vst3Path.toRawUTF8());

    juce::VST3PluginFormat vst3;
    juce::OwnedArray<juce::PluginDescription> descs;
    vst3.findAllTypesForFile (descs, vst3Path);
    if (descs.isEmpty()) { std::puts ("FATAL: no VST3 types found at path"); return 2; }

    juce::AudioPluginFormatManager fm;
    fm.addDefaultFormats();
    juce::String err;
    std::unique_ptr<juce::AudioPluginInstance> plugin (
        fm.createPluginInstance (*descs[0], 44100.0, 512, err));
    if (plugin == nullptr) { std::printf ("FATAL: createPluginInstance: %s\n", err.toRawUTF8()); return 2; }
    std::printf ("Loaded '%s' with %d parameters via VST3 wrapper.\n",
                 plugin->getName().toRawUTF8(), plugin->getParameters().size());

    // Pre-convert every factory preset (.sdpreset = raw APVTS XML) into the
    // host state-blob format the wrapper's setStateInformation expects.
    juce::Array<juce::MemoryBlock> presetBlobs;
    juce::StringArray presetNames;
    {
        juce::File presetDir { juce::String (SPACEDUST_PRESET_DIR) };
        juce::Array<juce::File> files;
        presetDir.findChildFiles (files, juce::File::findFiles, false, "*.sdpreset");
        files.sort();
        for (auto& f : files)
        {
            auto xml = juce::XmlDocument::parse (f);
            if (xml != nullptr)
            {
                juce::MemoryBlock mb;
                juce::AudioPluginInstance::copyXmlToBinary (*xml, mb);
                presetBlobs.add (mb);
                presetNames.add (f.getFileNameWithoutExtension());
            }
        }
    }
    std::printf ("Prepared %d preset state blobs from %s\n", presetBlobs.size(), SPACEDUST_PRESET_DIR);
    if (presetBlobs.isEmpty()) { std::puts ("FATAL: no presets"); return 2; }

    const double sampleRate = 44100.0;

    // (1) Lockstep through the wrapper: setStateInformation(preset) then render notes.
    auto runLockstep = [&] (double sr, int blockSize, int blocksPerPreset, juce::uint32 seed)
    {
        plugin->setRateAndBufferSizeDetails (sr, blockSize);
        plugin->prepareToPlay (sr, blockSize);
        juce::AudioBuffer<float> buf (2, blockSize);
        juce::Random rng (seed);
        for (int pi = 0; pi < presetBlobs.size(); ++pi)
        {
            const auto& mb = presetBlobs.getReference (pi);
            plugin->setStateInformation (mb.getData(), (int) mb.getSize());
            for (int b = 0; b < blocksPerPreset; ++b)
            {
                buf.clear();
                juce::MidiBuffer midi;
                if ((b % 4) == 0) midi.addEvent (noteOn (36 + rng.nextInt (60)), rng.nextInt (blockSize));
                if ((b % 9) == 0) midi.addEvent (noteOff (36 + rng.nextInt (60)), rng.nextInt (blockSize));
                plugin->processBlock (buf, midi);
            }
        }
        plugin->releaseResources();
    };

    // (2) Cross-thread race: audio renders+notes; message thread flips preset state.
    auto runRace = [&] (int blockSize, int rounds, juce::uint32 seed)
    {
        plugin->setRateAndBufferSizeDetails (sampleRate, blockSize);
        plugin->prepareToPlay (sampleRate, blockSize);
        std::atomic<bool> running { true };
        std::thread audioThread ([&]
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            juce::Random arng (seed ^ 0xA17D);
            int ctr = 0;
            while (running.load (std::memory_order_relaxed))
            {
                buf.clear();
                juce::MidiBuffer midi;
                if ((ctr % 3) == 0) midi.addEvent (noteOn (36 + arng.nextInt (60)), arng.nextInt (blockSize));
                if ((ctr % 7) == 0) midi.addEvent (noteOff (36 + arng.nextInt (60)), arng.nextInt (blockSize));
                plugin->processBlock (buf, midi);
                ++ctr;
            }
        });
        juce::Random rng (seed);
        for (int r = 0; r < rounds; ++r)
            for (int pi = 0; pi < presetBlobs.size(); ++pi)
            {
                const auto& mb = presetBlobs.getReference (rng.nextInt (presetBlobs.size()));
                plugin->setStateInformation (mb.getData(), (int) mb.getSize());
            }
        running = false;
        audioThread.join();
        plugin->releaseResources();
    };

    // (3) Editor + preset + offscreen-paint race (window-open repro).
    auto runEditorRace = [&] (int blockSize, int seconds)
    {
        plugin->setRateAndBufferSizeDetails (sampleRate, blockSize);
        plugin->prepareToPlay (sampleRate, blockSize);
        juce::AudioProcessorEditor* editor = plugin->createEditorIfNeeded();
        if (editor == nullptr) { std::puts ("(no editor — skipping)"); plugin->releaseResources(); return; }
        editor->setSize (editor->getWidth()  > 0 ? editor->getWidth()  : 900,
                         editor->getHeight() > 0 ? editor->getHeight() : 700);
        std::printf ("    (editor %dx%d)\n", editor->getWidth(), editor->getHeight());

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
                if ((ctr++ % 4) == 0) midi.addEvent (noteOn (48 + arng.nextInt (24)), 0);
                plugin->processBlock (buf, midi);
            }
        });

        juce::Random rng (0x7ACE);
        const auto end = juce::Time::currentTimeMillis() + (juce::int64) seconds * 1000;
        int tick = 0;
        while (juce::Time::currentTimeMillis() < end)
        {
            if ((tick++ % 2) == 0)
            {
                const auto& mb = presetBlobs.getReference (rng.nextInt (presetBlobs.size()));
                plugin->setStateInformation (mb.getData(), (int) mb.getSize());
            }
            if (editor->getWidth() > 0 && editor->getHeight() > 0)
            {
                juce::Image img (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
                juce::Graphics g (img);
                editor->paintEntireComponent (g, false);
            }
           #if JUCE_MAC || defined(__APPLE__)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.002, true);
           #endif
        }
        running = false;
        audioThread.join();
        plugin->editorBeingDeleted (editor);
        delete editor;
        plugin->releaseResources();
    };

    for (double sr : { 22050.0, 44100.0, 48000.0, 96000.0 })
        for (int bs : { 32, 64, 128, 256, 512, 1024 })
        {
            std::printf ("=== VST3 lockstep @ sr %.0f block %d ===\n", sr, bs);
            std::fflush (stdout);
            runLockstep (sr, bs, 300, (juce::uint32) (bs + (int) sr));
        }

    for (juce::uint32 seed : { 0x9001u, 0x5050u, 0xF00Du })
        for (int bs : { 64, 128, 256, 512 })
        {
            std::printf ("=== VST3 preset RACE @ block %d seed %u ===\n", bs, seed);
            std::fflush (stdout);
            runRace (bs, 150, seed);
        }

    for (int bs : { 64, 128, 256, 512 })
    {
        std::printf ("=== VST3 editor+preset+paint race @ block %d ===\n", bs);
        std::fflush (stdout);
        runEditorRace (bs, 20);
    }

    std::puts ("VST3 HOST DONE - no AddressSanitizer error.");
    return 0;
}
