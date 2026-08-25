#include "UserWavetable.h"
#include "PresetManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>

const UserWaveSlot UserWaveBank::emptySlot;

namespace
{
    /** Four-point interpolation, for Full Sample mode.

        Linear interpolation is good enough for a single cycle, which is stored at
        2048 points per period and is already band limited. A whole sample is
        neither: it is stored at its own rate and gets transposed by however far
        the played note is from the note it was recorded at, so the read lands
        between samples by a wide margin and linear interpolation dulls it
        audibly.

        The caller guarantees three samples of clearance at both ends -- see how
        loopStart and loopLength are chosen in buildSlot. */
    inline float hermite (const float* data, double position) noexcept
    {
        const int i = (int) position;
        const float t = (float) (position - (double) i);

        const float xm1 = data[i - 1];
        const float x0  = data[i];
        const float x1  = data[i + 1];
        const float x2  = data[i + 2];

        const float c = 0.5f * (x1 - xm1);
        const float v = x0 - x1;
        const float w = c + v;
        const float a = w + v + 0.5f * (x2 - x0);
        const float b = w + a;

        return ((a * t - b) * t + c) * t + x0;
    }

    /** Scale a buffer so its loudest point sits at unity, so one imported
        waveform is not twice the volume of the next. */
    void normalise (std::vector<float>& data)
    {
        float peak = 0.0f;
        for (float v : data)
            peak = std::max (peak, std::abs (v));

        if (peak <= 1.0e-9f)
            return;

        const float gain = 1.0f / peak;
        for (float& v : data)
            v *= gain;
    }

    //==========================================================================
    // -- The audio a slot keeps --
    //
    // A slot carries the audio it was built from, compressed, and that is the only
    // place it is kept: nothing is copied into the user's folders, so importing a
    // hundred files leaves nothing behind but the slots that hold them.
    //
    // FLAC at 16 bits. It is about half the size of the raw samples, so fifteen
    // seconds -- the most a slot ever keeps -- costs roughly a megabyte inside a
    // preset instead of three, and a short file costs almost nothing. 16 bits
    // rather than 24 for the same reason; the noise floor it puts under an
    // imported sample is far below anything the synth then does to it.

    juce::MemoryBlock encodeSlotAudio (const std::vector<float>& mono, double sampleRate)
    {
        juce::MemoryBlock block;

        if (mono.empty() || sampleRate <= 0.0)
            return block;

        juce::FlacAudioFormat flac;

        // The writer takes the stream: released to it below, deleted with it.
        auto stream = std::make_unique<juce::MemoryOutputStream> (block, false);

        std::unique_ptr<juce::AudioFormatWriter> writer (
            flac.createWriterFor (stream.get(), sampleRate, 1, 16, {}, 0));

        if (writer == nullptr)
            return {};

        stream.release();

        const float* channels[1] = { mono.data() };

        if (! writer->writeFromFloatArrays (channels, 1, (int) mono.size()))
        {
            writer.reset();
            return {};
        }

        writer.reset();   // finishes the stream, so the block is complete here
        return block;
    }

    bool decodeSlotAudio (const juce::MemoryBlock& block, std::vector<float>& mono,
                          double& sampleRate)
    {
        if (block.getSize() == 0)
            return false;

        juce::FlacAudioFormat flac;

        // createReaderFor takes the stream: it holds it for as long as the reader
        // lives, and the trailing true has it deleted if opening fails instead. So
        // the raw new below is owned either way, and the reader itself is owned by
        // the unique_ptr on the next line.
        auto* stream = new juce::MemoryInputStream (block, false);
        std::unique_ptr<juce::AudioFormatReader> reader (flac.createReaderFor (stream, true));

        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
            return false;

        const auto cap = (juce::int64) (UserWave::maxSampleSeconds * reader->sampleRate);
        const int length = (int) juce::jmin (reader->lengthInSamples, cap);

        juce::AudioBuffer<float> buffer (1, length);

        if (! reader->read (&buffer, 0, length, 0, true, true))
            return false;

        sampleRate = reader->sampleRate;
        mono.assign (buffer.getReadPointer (0), buffer.getReadPointer (0) + length);
        return true;
    }
}

//==============================================================================
float UserWaveSlot::read (double phase01, double freqHz, double sampleRate) const noexcept
{
    // The caller wraps the phase before calling, but a modulated phase that
    // overshoots by a hair would index off the end of the table, so it is pinned
    // here as well. This is one compare per sample against a crash.
    if (! (phase01 >= 0.0))
        phase01 = 0.0;
    else if (phase01 >= 1.0)
        phase01 -= std::floor (phase01);

    if (mode == UserWave::Mode::SingleCycle)
    {
        if (tables.empty())
            return 0.0f;

        // Read the richest version of the waveform that will not fold back at
        // this pitch. High notes get fewer harmonics; that is the whole point.
        const int level = WaveAnalysis::levelForFrequency (freqHz, sampleRate);
        const float* table = tables.data() + (std::size_t) level * WaveAnalysis::tableSize;

        const double position = phase01 * (double) WaveAnalysis::tableSize;
        const int index = (int) position;
        const float fraction = (float) (position - (double) index);

        const int next = (index + 1 == WaveAnalysis::tableSize) ? 0 : index + 1;
        return table[index] + fraction * (table[next] - table[index]);
    }

    if (loopLength <= 0)
        return 0.0f;

    //==========================================================================
    // A slot that does NOT loop is read straight through, from the start marker
    // to the end marker, with no crossfade at the end of it.
    //
    // Both halves of that matter, and both were audible before:
    //
    //   where it starts   a looping slot begins at loopStart, which is a whole
    //                     crossfade past the start marker -- material
    //                     deliberately held back so the loop has something to
    //                     fade into. There is no loop here, so holding it back
    //                     would only throw away the first ten milliseconds of the
    //                     sound, which on anything struck or plucked IS the
    //                     sound.
    //
    //   the crossfade     it fades the end of the pass into the material before
    //                     loopStart -- that is, into the ATTACK. Inaudible going
    //                     round, because that is where the next pass begins; a
    //                     click going nowhere, because the oscillator then falls
    //                     silent from whatever the attack was doing
    //                     (Giuseppe, 2026-08-14).
    //
    // A one-shot covers playLength and a loop covers loopLength, and the phase
    // scale is worked out from whichever of the two this slot uses -- so the
    // sample comes out at the speed it went in either way, and the note still
    // plays at the pitch it was recorded at.
    if (! loop)
        return hermite (sample.data(), (double) playStart + phase01 * (double) playLength);

    const double position = (double) loopStart + phase01 * (double) loopLength;
    float value = hermite (sample.data(), position);

    // Fade the end of the loop into the material immediately before its start, so
    // the jump back is inaudible. Without this a sustained note ticks once per
    // pass, at a rate that follows the pitch and so sounds like distortion rather
    // than like a loop.
    if (crossfade > 0)
    {
        const double fadeStart = (double) (loopStart + loopLength - crossfade);

        if (position >= fadeStart)
        {
            const double weight = (position - fadeStart) / (double) crossfade;
            const float incoming = hermite (sample.data(), position - (double) loopLength);
            value = (float) ((1.0 - weight) * (double) value + weight * (double) incoming);
        }
    }

    return value;
}

//==============================================================================
UserWaveLibrary::UserWaveLibrary()
    : editable (std::make_unique<UserWaveBank>())
{
    formatManager.registerBasicFormats();
}

UserWaveLibrary::~UserWaveLibrary()
{
    // Anything the audio thread never collected is still owned here.
    if (auto* neverTaken = pending.exchange (nullptr))
        delete neverTaken;

    collectRetired();
}

//==============================================================================
void UserWaveLibrary::exchangeBank (UserWaveBank*& current) noexcept
{
    // Audio thread. One atomic exchange, no allocation, no lock, and no chance of
    // reading a bank that is being deleted: the message thread gives up ownership
    // here and does not take it back.
    if (auto* incoming = pending.exchange (nullptr, std::memory_order_acq_rel))
    {
        auto* previous = current;
        current = incoming;

        if (previous != nullptr)
        {
            for (auto& slot : retired)
            {
                UserWaveBank* expected = nullptr;
                if (slot.compare_exchange_strong (expected, previous, std::memory_order_release))
                    return;
            }

            // Every return slot is full, which needs thirty-two imports inside a
            // fifth of a second. Keeping the old bank rather than dropping it
            // costs some memory until the plugin closes; freeing it here could
            // free it out from under this very block.
        }
    }
}

void UserWaveLibrary::reclaimFromAudio (UserWaveBank*& current)
{
    // Message thread, after audio has stopped. Nothing is reading it now.
    delete current;
    current = nullptr;
}

void UserWaveLibrary::collectRetired()
{
    for (auto& slot : retired)
        if (auto* old = slot.exchange (nullptr))
            delete old;
}

//==============================================================================
void UserWaveLibrary::publish()
{
    collectRetired();

    auto fresh = cloneBank (*editable);

    // If the audio thread had not yet taken the last one, it never will -- this
    // thread still owns it and must delete it.
    if (auto* superseded = pending.exchange (fresh.release()))
        delete superseded;

    if (onChange)
        onChange();
}

std::unique_ptr<UserWaveBank> UserWaveLibrary::cloneBank (const UserWaveBank& source)
{
    auto copy = std::make_unique<UserWaveBank>();

    for (int g = 0; g < UserWave::numGroups; ++g)
    {
        const auto group = (UserWave::Group) g;

        for (int i = 0; i < UserWave::numSlots; ++i)
            copy->editableSlot (group, i) = source.slot (group, i);
    }

    return copy;
}

//==============================================================================
juce::File UserWaveLibrary::wavetableFolder()
{
    return PresetManager::appDataFolder().getChildFile ("Wavetables");
}

//==============================================================================
bool UserWaveLibrary::readFileAsMono (const juce::File& file, std::vector<float>& mono,
                                      double& sampleRate, juce::String& errorMessage)
{
    if (! file.existsAsFile())
    {
        errorMessage = "That file does not exist.";
        return false;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr)
    {
        errorMessage = "Cannot read " + file.getFileExtension()
                     + " files. Try WAV, AIFF, FLAC or OGG.";
        return false;
    }

    if (reader->sampleRate <= 0.0 || reader->lengthInSamples < 64)
    {
        errorMessage = "That file is too short to use.";
        return false;
    }

    sampleRate = reader->sampleRate;

    const juce::int64 cap = (juce::int64) (UserWave::maxSampleSeconds * reader->sampleRate);
    const int length = (int) juce::jmin (reader->lengthInSamples, cap);

    juce::AudioBuffer<float> buffer ((int) reader->numChannels, length);

    if (! reader->read (&buffer, 0, length, 0, true, true))
    {
        errorMessage = "That file could not be decoded.";
        return false;
    }

    // Summed to mono. An oscillator produces one signal and is panned afterwards
    // by the controls already on the panel, so keeping the stereo image here
    // would only fight them.
    mono.assign ((std::size_t) length, 0.0f);

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const float* source = buffer.getReadPointer (channel);
        for (int i = 0; i < length; ++i)
            mono[(std::size_t) i] += source[i];
    }

    if (buffer.getNumChannels() > 1)
    {
        const float scale = 1.0f / (float) buffer.getNumChannels();
        for (float& v : mono)
            v *= scale;
    }

    normalise (mono);
    return true;
}

//==============================================================================
void UserWaveLibrary::buildSlot (UserWaveSlot& slot, const std::vector<float>& mono,
                                 double sampleRate, UserWave::Mode mode)
{
    slot.mode = mode;
    slot.fileSampleRate = sampleRate;
    slot.tables.clear();
    slot.sample.clear();
    slot.fileLength = 0;
    slot.playStart = 0;
    slot.playLength = 0;
    slot.loopStart = 0;
    slot.loopLength = 0;
    slot.crossfade = 0;
    slot.phaseIncrementScale = 1.0;

    // NOT reset: trimStart and trimEnd say where the player put the markers, and
    // this is the function that puts their decision into effect. Clearing them
    // here would undo it on every rebuild -- including the rebuild a change of
    // mode does, and the one opening a song does.

    const int numSamples = (int) mono.size();

    if (numSamples < 64)
    {
        slot.active = false;
        return;
    }

    // The pitch is wanted in both modes, but for different reasons: it sets the
    // length of the cycle to extract in one, and the playback speed in the other.
    const auto estimate = WaveAnalysis::detectFundamental (mono.data(), numSamples, sampleRate);

    // What the period and the playback speed are actually measured against.
    //
    // A resample is KNOWN to be a middle C -- the plugin played it itself -- so
    // nothing is gained by asking the detector and there is a great deal to lose.
    // It reads a patch with a strong fifth, a detuned pair or an octave-down sub
    // an octave or a fifth out often enough to matter, and a period cut at the
    // wrong length is not a slightly wrong waveform: it is a different shape
    // every cycle, which is what a resample read as "G#1" looked like on screen
    // (Giuseppe, 2026-08-13).
    const double analysisHz = slot.allowRetune ? estimate.frequencyHz : WaveAnalysis::middleCHz;

    slot.fundamentalHz = analysisHz;
    slot.confidence = estimate.confidence;

    char label[32] = {};
    WaveAnalysis::describePitch (analysisHz, label, sizeof (label));
    slot.pitchLabel = juce::String (label);

    if (mode == UserWave::Mode::SingleCycle)
    {
        const auto harmonics = WaveAnalysis::analyseHarmonics (mono.data(), numSamples,
                                                               sampleRate, analysisHz);

        if (harmonics.isEmpty())
        {
            slot.active = false;
            return;
        }

        slot.tables.resize ((std::size_t) (WaveAnalysis::mipLevels * WaveAnalysis::tableSize));
        WaveAnalysis::renderMipmaps (harmonics, slot.tables.data());

        // A cycle is one period by construction, so the note played is the note
        // heard. Nothing needs re-tuning and the phase increment is untouched.
        slot.retuned = true;
        slot.active = true;
        return;
    }

    //==========================================================================
    // Full Sample.
    slot.fileLength = numSamples;

    // Where the markers stand, turned into what actually gets read. The maths is
    // in WaveAnalysis, where it is checked against known numbers -- a region that
    // reaches one sample too far is a read off the end of a buffer on the audio
    // thread, not a slightly wrong sound.
    const auto region = WaveAnalysis::regionForTrim (numSamples, sampleRate,
                                                     UserWave::maxPadSamples (numSamples,
                                                                              sampleRate),
                                                     slot.trimStart, slot.trimEnd);

    if (region.loopLength <= 0)
    {
        slot.active = false;
        return;
    }

    // Written back, so what the slot says and what it plays are the same thing --
    // the picture and the times on screen are drawn from these.
    slot.trimStart = region.trimStart;
    slot.trimEnd = region.trimEnd;

    slot.playStart = region.playStart;
    slot.playLength = region.playLength;
    slot.loopStart = region.loopStart;
    slot.loopLength = region.loopLength;
    slot.crossfade = region.crossfade;

    // The buffer: clearance, the silence the start marker asked for, the WHOLE
    // file, clearance. The whole file, not the part between the markers, because
    // a marker is a decision that can be taken back -- see UserWaveSlot::sample.
    slot.sample.assign ((std::size_t) region.bufferLength, 0.0f);
    std::copy (mono.begin(), mono.end(), slot.sample.begin() + region.fileOffset);

    // Whether to re-tune at all. A drum hit has no fundamental to put on middle
    // C, and forcing one on it only makes the sample play back at a speed the
    // user did not ask for and cannot explain. Neither has a resample, for a
    // different reason -- see UserWaveSlot::allowRetune.
    slot.retuned = slot.allowRetune && estimate.confidence >= UserWave::minPitchConfidence;

    // The whole of Full Sample mode, in one constant -- see updatePhaseScale.
    updatePhaseScale (slot);

    slot.active = true;
}

void UserWaveLibrary::updatePhaseScale (UserWaveSlot& slot)
{
    if (slot.mode != UserWave::Mode::FullSample)
    {
        slot.phaseIncrementScale = 1.0;
        return;
    }

    // Not the fundamental as read: a file whose pitch was read with too little
    // confidence keeps its recorded speed even though re-tuning WAS allowed for
    // it. A resample reaches the same answer by the other road -- allowRetune is
    // false, so retuned is false and this is middle C, which is where it was
    // recorded.
    const double rootHz = slot.retuned ? slot.fundamentalHz : WaveAnalysis::middleCHz;

    // The derivation, and the test that proves middle C really does land on the
    // recorded pitch, are both over in WaveAnalysis where they can run without a
    // plugin host.
    slot.phaseIncrementScale = WaveAnalysis::playbackPhaseScale (slot.fileSampleRate, rootHz,
                                                                 slot.playSpan());
}

//==============================================================================
bool UserWaveLibrary::storeSlot (const std::vector<float>& mono, double sampleRate,
                                 const juce::String& name, const juce::String& sourceFile,
                                 bool allowRetune, UserWave::Group group, int slotIndex,
                                 UserWave::Mode mode, juce::String& errorMessage)
{
    auto& slot = editable->editableSlot (group, slotIndex);
    slot.name = name;
    slot.sourceFile = sourceFile;

    // Set before the build, because the build is what reads it.
    slot.allowRetune = allowRetune;

    // A newly imported sample loops, whatever the slot's last occupant did. The
    // slot object is reused, so without this a slot the player had set to
    // one-shot would silently impose that on the next thing dropped into it.
    // Changing MODE goes another way (setSlotMode) and deliberately keeps it.
    slot.loop = true;

    // And it plays end to end, for the same reason. The markers belong to the
    // sound that was in the slot, not to the slot, so a new sample must not
    // arrive with somebody else's start and end already cut into it.
    slot.trimStart = 0;
    slot.trimEnd = 0;

    buildSlot (slot, mono, sampleRate, mode);

    if (! slot.active)
    {
        errorMessage = "Nothing pitched could be found in that sound.";
        clearSlot (group, slotIndex);
        return false;
    }

    // The slot keeps the audio, not the user's disk: an imported file is never
    // copied anywhere, and this is what a preset, a saved song and a later change
    // of Mode are all built from. Encoded after buildSlot, which does not touch it.
    slot.encodedAudio = encodeSlotAudio (mono, sampleRate);

    if (slot.encodedAudio.getSize() == 0)
    {
        errorMessage = "That sound could not be stored in the slot.";
        clearSlot (group, slotIndex);
        return false;
    }

    saveToDisk();
    publish();
    return true;
}

bool UserWaveLibrary::importFile (const juce::File& file, UserWave::Group group, int slotIndex,
                                  UserWave::Mode mode, juce::String& errorMessage)
{
    if (slotIndex < 0 || slotIndex >= UserWave::numSlots)
    {
        errorMessage = "That waveform slot does not exist.";
        return false;
    }

    std::vector<float> mono;
    double sampleRate = 0.0;

    if (! readFileAsMono (file, mono, sampleRate, errorMessage))
        return false;

    return storeSlot (mono, sampleRate, file.getFileNameWithoutExtension(), file.getFileName(),
                      true, group, slotIndex, mode, errorMessage);
}

bool UserWaveLibrary::importAudio (std::vector<float> mono, double sampleRate,
                                   const juce::String& name, UserWave::Group group,
                                   int slotIndex, UserWave::Mode mode,
                                   juce::String& errorMessage)
{
    if (slotIndex < 0 || slotIndex >= UserWave::numSlots)
    {
        errorMessage = "That waveform slot does not exist.";
        return false;
    }

    if (sampleRate <= 0.0 || mono.size() < 64)
    {
        errorMessage = "That sound is too short to use.";
        return false;
    }

    // The same cap a file gets. The caller's history is already this long at most,
    // so this only ever holds for a caller that has not read that far.
    const auto cap = (std::size_t) (UserWave::maxSampleSeconds * sampleRate);

    if (mono.size() > cap)
        mono.resize (cap);

    // And the same normalisation, so a waveform taken from the synth sits at the
    // same level as one dropped in from disk. What that costs -- the level the
    // sound was captured at -- is given back by Resample + Init, which puts the
    // master volume where it has to be to undo this exactly.
    normalise (mono);

    // No re-tuning: this was recorded from middle C, and middle C plays a slot
    // back unchanged. See UserWaveSlot::allowRetune.
    return storeSlot (mono, sampleRate, name, {}, false, group, slotIndex, mode, errorMessage);
}

//==============================================================================
bool UserWaveLibrary::loadSlotAudio (UserWaveSlot& slot, std::vector<float>& mono,
                                     double& sampleRate, juce::String& errorMessage)
{
    if (decodeSlotAudio (slot.encodedAudio, mono, sampleRate))
        return true;

    // Only reachable for a slot imported before the audio travelled with it,
    // whose file in the Wavetables folder may since have been removed.
    auto legacy = wavetableFolder().getChildFile ("Samples").getChildFile (slot.sourceFile);

    if (! readFileAsMono (legacy, mono, sampleRate, errorMessage))
    {
        errorMessage = "The audio for \"" + slot.name + "\" is missing, so it cannot be rebuilt.";
        return false;
    }

    // Take it into the slot while we have it, so this is the last time it is ever
    // looked for on disk.
    slot.encodedAudio = encodeSlotAudio (mono, sampleRate);
    return true;
}

bool UserWaveLibrary::setSlotMode (UserWave::Group group, int slotIndex, UserWave::Mode mode,
                                   juce::String& errorMessage)
{
    if (slotIndex < 0 || slotIndex >= UserWave::numSlots)
    {
        errorMessage = "That waveform slot does not exist.";
        return false;
    }

    auto& slot = editable->editableSlot (group, slotIndex);

    if (! slot.active)
    {
        errorMessage = "That slot is empty.";
        return false;
    }

    if (slot.mode == mode)
        return true;

    // Both modes are built from the same audio and only one of them is held ready
    // to play, so switching means decoding the slot's own copy again. It is in the
    // slot, so this works with the imported file long gone.
    std::vector<float> mono;
    double sampleRate = 0.0;

    if (! loadSlotAudio (slot, mono, sampleRate, errorMessage))
        return false;

    buildSlot (slot, mono, sampleRate, mode);

    if (! slot.active)
    {
        errorMessage = "That file cannot be used in this mode.";
        return false;
    }

    saveToDisk();
    publish();
    return true;
}

void UserWaveLibrary::setSlotLoop (UserWave::Group group, int slotIndex, bool shouldLoop)
{
    if (slotIndex < 0 || slotIndex >= UserWave::numSlots)
        return;

    auto& slot = editable->editableSlot (group, slotIndex);

    if (! slot.active || slot.loop == shouldLoop)
        return;

    slot.loop = shouldLoop;

    // A one-shot and a loop cover regions of different lengths, so the scale that
    // keeps the sample at its recorded speed is not the same for both. Nothing
    // else is rebuilt -- the audio has not changed.
    updatePhaseScale (slot);

    saveToDisk();
    publish();
}

bool UserWaveLibrary::setSlotTrim (UserWave::Group group, int slotIndex, int trimStart,
                                   int trimEnd, juce::String& errorMessage)
{
    if (slotIndex < 0 || slotIndex >= UserWave::numSlots)
    {
        errorMessage = "That waveform slot does not exist.";
        return false;
    }

    auto& slot = editable->editableSlot (group, slotIndex);

    if (! slot.active)
    {
        errorMessage = "That slot is empty.";
        return false;
    }

    if (slot.mode != UserWave::Mode::FullSample)
    {
        errorMessage = "The start and end can only be moved on a whole sample.";
        return false;
    }

    if (slot.trimStart == trimStart && slot.trimEnd == trimEnd)
        return true;

    // The audio in the slot is the whole file and always was, so moving a marker
    // that was dragged inwards back OUT again brings the sound back. Nothing here
    // is ever a cut.
    std::vector<float> mono;
    double sampleRate = 0.0;

    if (! loadSlotAudio (slot, mono, sampleRate, errorMessage))
        return false;

    const int keptStart = slot.trimStart;
    const int keptEnd = slot.trimEnd;

    slot.trimStart = trimStart;
    slot.trimEnd = trimEnd;

    buildSlot (slot, mono, sampleRate, UserWave::Mode::FullSample);

    if (! slot.active)
    {
        // Cannot happen for a request that came through the clamps in buildSlot,
        // but a slot that fell out of the list would take the player's waveform
        // with it -- so it is put back rather than left broken.
        slot.trimStart = keptStart;
        slot.trimEnd = keptEnd;
        buildSlot (slot, mono, sampleRate, UserWave::Mode::FullSample);

        errorMessage = "There is not enough of the sample left between those two points.";
        return false;
    }

    saveToDisk();
    publish();
    return true;
}

//==============================================================================
// -- Dragging a marker --
//
// setSlotTrim above decodes, rebuilds, writes and publishes on every call. The
// three below are the same work, taken apart so the expensive ends happen once
// per gesture and only the rebuild happens in between.

bool UserWaveLibrary::beginTrimSession (UserWave::Group group, int slotIndex,
                                        juce::String& errorMessage)
{
    // A session left open by a gesture that never finished would hold the wrong
    // slot's audio. Close it before opening another.
    if (trimSession.isOpen())
    {
        juce::String ignored;
        endTrimSession (ignored);
    }

    if (slotIndex < 0 || slotIndex >= UserWave::numSlots)
    {
        errorMessage = "That waveform slot does not exist.";
        return false;
    }

    auto& slot = editable->editableSlot (group, slotIndex);

    if (! slot.active)
    {
        errorMessage = "That slot is empty.";
        return false;
    }

    if (slot.mode != UserWave::Mode::FullSample)
    {
        errorMessage = "The start and end can only be moved on a whole sample.";
        return false;
    }

    // The one decode of the gesture.
    if (! loadSlotAudio (slot, trimAudio, trimAudioRate, errorMessage))
    {
        trimAudio.clear();
        trimAudioRate = 0.0;
        return false;
    }

    trimSession.open ((int) group, slotIndex,
                      TrimSession::Points { slot.trimStart, slot.trimEnd });
    return true;
}

bool UserWaveLibrary::updateTrimSession (int trimStart, int trimEnd,
                                         juce::String& errorMessage)
{
    if (! trimSession.isOpen())
    {
        errorMessage = "There is no drag in progress.";
        return false;
    }

    trimSession.pending (TrimSession::Points { trimStart, trimEnd });

    // A still mouse. Not a failure -- there is simply nothing to do.
    if (! trimSession.wants())
        return true;

    const auto group = (UserWave::Group) trimSession.group();
    auto& slot = editable->editableSlot (group, trimSession.slot());

    const int keptStart = slot.trimStart;
    const int keptEnd = slot.trimEnd;

    slot.trimStart = trimStart;
    slot.trimEnd = trimEnd;

    buildSlot (slot, trimAudio, trimAudioRate, UserWave::Mode::FullSample);

    if (! slot.active)
    {
        // Same rollback setSlotTrim does: a slot that fell out of the list would
        // take the player's waveform with it mid-drag.
        slot.trimStart = keptStart;
        slot.trimEnd = keptEnd;
        buildSlot (slot, trimAudio, trimAudioRate, UserWave::Mode::FullSample);

        errorMessage = "There is not enough of the sample left between those two points.";
        return false;
    }

    trimSession.applied();

    // No saveToDisk(). That is what the end of the gesture is for.
    publish();
    return true;
}

bool UserWaveLibrary::endTrimSession (juce::String& errorMessage)
{
    if (! trimSession.isOpen())
        return true;

    // The last position FIRST, while the session and the held audio are both
    // still alive. A fast drag moves the markers after the final timer tick, and
    // without this the position the player actually let go at would be lost.
    bool ok = true;

    if (trimSession.wants())
    {
        const auto last = trimSession.pendingPoints();
        ok = updateTrimSession (last.start, last.end, errorMessage);
    }

    trimSession.close();

    trimAudio.clear();
    trimAudio.shrink_to_fit();
    trimAudioRate = 0.0;

    // The one write of the gesture. Done even when the last apply was refused:
    // the slot still holds the last trim that worked, and that is worth keeping.
    saveToDisk();
    return ok;
}

void UserWaveLibrary::renameSlot (UserWave::Group group, int slotIndex,
                                  const juce::String& newName)
{
    if (slotIndex < 0 || slotIndex >= UserWave::numSlots)
        return;

    auto& slot = editable->editableSlot (group, slotIndex);

    if (! slot.active)
        return;

    slot.name = newName.trim().isEmpty() ? slot.name : newName.trim();

    saveToDisk();
    publish();
}

void UserWaveLibrary::clearSlot (UserWave::Group group, int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= UserWave::numSlots)
        return;

    editable->editableSlot (group, slotIndex) = UserWaveSlot();

    saveToDisk();
    publish();
}

bool UserWaveLibrary::copySlotToAllGroups (UserWave::Group group, int slotIndex,
                                           juce::String& errorMessage)
{
    if (slotIndex < 0 || slotIndex >= UserWave::numSlots)
    {
        errorMessage = "That waveform slot does not exist.";
        return false;
    }

    // By value, not by reference: the loop below writes into the same array the
    // reference would point into, and the source would then be a reference to a
    // slot that had already been assigned over.
    const UserWaveSlot source = editable->slot (group, slotIndex);

    if (! source.isPlayable())
    {
        errorMessage = "That slot is empty, so there is nothing to copy.";
        return false;
    }

    // The copy carries sourceFile with it, so all five lists point at the one
    // file in the Wavetables folder. Nothing is duplicated on disk, and Full
    // Sample slots in every list rebuild from the same copy.
    for (int g = 0; g < UserWave::numGroups; ++g)
    {
        const auto target = (UserWave::Group) g;

        if (target != group)
            editable->editableSlot (target, slotIndex) = source;
    }

    saveToDisk();
    publish();
    return true;
}

void UserWaveLibrary::clearAllSlots()
{
    for (int g = 0; g < UserWave::numGroups; ++g)
    {
        const auto group = (UserWave::Group) g;

        for (int i = 0; i < UserWave::numSlots; ++i)
            editable->editableSlot (group, i) = UserWaveSlot();
    }

    saveToDisk();
    publish();
}

juce::StringArray UserWaveLibrary::slotChoiceNames (UserWave::Group group) const
{
    juce::StringArray names;

    for (int i = 0; i < UserWave::numSlots; ++i)
        names.add (choiceNameForSlot (group, i));

    return names;
}

juce::String UserWaveLibrary::choiceNameForSlot (UserWave::Group group, int slotIndex) const
{
    const auto& slot = editable->slot (group, slotIndex);

    if (slot.active && slot.name.isNotEmpty())
        return slot.name;

    return "User " + juce::String (slotIndex + 1);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> UserWaveLibrary::createStateXml() const
{
    auto root = std::make_unique<juce::XmlElement> ("USERWAVES");

    // Slots holding the same audio store it once. "Update All" puts one sample in
    // all five lists, and that used to write five copies of it into every preset
    // and every saved song. The first slot to carry a block gets an id and the
    // bytes; the others name the id and carry nothing.
    struct WrittenAudio { const juce::MemoryBlock* bytes; int id; };
    std::vector<WrittenAudio> written;
    int nextAudioId = 1;

    for (int g = 0; g < UserWave::numGroups; ++g)
    {
        const auto group = (UserWave::Group) g;

        for (int i = 0; i < UserWave::numSlots; ++i)
        {
            const auto& slot = editable->slot (group, i);

            if (! slot.active)
                continue;

            auto* element = root->createNewChildElement ("SLOT");
            element->setAttribute ("index", i);

            // Which list this belongs to. Its absence is what marks state
            // written before the lists were split -- see restoreFromStateXml.
            element->setAttribute ("group", UserWave::groupTag (group));
            element->setAttribute ("name", slot.name);
            element->setAttribute ("source", slot.sourceFile);
            element->setAttribute ("mode", (int) slot.mode);
            element->setAttribute ("fundamental", slot.fundamentalHz);
            element->setAttribute ("confidence", slot.confidence);
            element->setAttribute ("pitchLabel", slot.pitchLabel);
            element->setAttribute ("retuned", slot.retuned);

            // Only written when it is false, which is only ever a resample. Every
            // slot saved before this existed was imported from a file and is meant
            // to be re-tuned, which is what its absence means on the way back in.
            if (! slot.allowRetune)
                element->setAttribute ("allowRetune", false);

            // The same rule, for the same reason: every slot saved before the loop
            // could be turned off had one, so absent means on.
            if (! slot.loop)
                element->setAttribute ("loop", false);

            // And again: absent means the sample plays end to end, which is what
            // every slot saved before the markers existed does.
            if (slot.trimStart != 0)
                element->setAttribute ("trimStart", slot.trimStart);

            if (slot.trimEnd != 0)
                element->setAttribute ("trimEnd", slot.trimEnd);

            // Only the richest table is stored. The other ten are a transform
            // away, and eight kilobytes per slot is small enough to sit inside a
            // preset -- which is what makes a Single Cycle waveform load without
            // having to decode the audio below.
            if (slot.mode == UserWave::Mode::SingleCycle && ! slot.tables.empty())
            {
                juce::MemoryOutputStream encoded;
                juce::Base64::convertToBase64 (encoded, slot.tables.data(),
                                               sizeof (float) * (std::size_t) WaveAnalysis::tableSize);
                element->setAttribute ("table", encoded.toString());
            }

            // The audio itself, so the waveform travels whole. This is what makes a
            // preset self contained -- in either mode, on any machine, with the
            // imported file long since deleted.
            if (slot.encodedAudio.getSize() > 0)
            {
                int sharedWith = 0;

                // Compares the bytes, not a hash: a slot's audio is compressed the
                // same way every time, so two slots holding one sample hold two
                // identical blocks, and MemoryBlock's == checks the size first.
                for (const auto& seen : written)
                {
                    if (*seen.bytes == slot.encodedAudio)
                    {
                        sharedWith = seen.id;
                        break;
                    }
                }

                if (sharedWith != 0)
                {
                    element->setAttribute ("audioRef", sharedWith);
                }
                else
                {
                    const int id = nextAudioId++;

                    juce::MemoryOutputStream encoded;
                    juce::Base64::convertToBase64 (encoded, slot.encodedAudio.getData(),
                                                   slot.encodedAudio.getSize());
                    element->setAttribute ("audioId", id);
                    element->setAttribute ("audio", encoded.toString());

                    // The slot outlives this function, so the pointer stays good.
                    written.push_back ({ &slot.encodedAudio, id });
                }
            }
        }
    }

    return root;
}

void UserWaveLibrary::restoreSlotFromXml (const juce::XmlElement& element, UserWaveSlot& slot,
                                          const std::map<int, juce::MemoryBlock>& sharedAudio)
{
    slot = UserWaveSlot();

    slot.name = element.getStringAttribute ("name");
    slot.sourceFile = element.getStringAttribute ("source");
    slot.mode = element.getIntAttribute ("mode", 0) == 1 ? UserWave::Mode::FullSample
                                                         : UserWave::Mode::SingleCycle;
    slot.fundamentalHz = element.getDoubleAttribute ("fundamental", 0.0);
    slot.confidence = element.getDoubleAttribute ("confidence", 0.0);
    slot.pitchLabel = element.getStringAttribute ("pitchLabel");
    slot.retuned = element.getBoolAttribute ("retuned", false);

    // Read BEFORE the rebuild below, which is what consults it. Absent means true:
    // everything written before Resample existed came from a file.
    slot.allowRetune = element.getBoolAttribute ("allowRetune", true);
    slot.loop = element.getBoolAttribute ("loop", true);

    // Read before the rebuild for the same reason: the rebuild is what puts the
    // markers into effect, and both default to the whole file.
    slot.trimStart = element.getIntAttribute ("trimStart", 0);
    slot.trimEnd = element.getIntAttribute ("trimEnd", 0);

    // The slot's own audio, written by everything saved from this version on. It
    // is kept whatever the mode is, because it is what a later change of mode is
    // rebuilt from. Either it is written here, or another slot holding the same
    // sample carries it and this one names it by id.
    if (const auto stored = element.getStringAttribute ("audio"); stored.isNotEmpty())
    {
        juce::MemoryOutputStream decoded;

        if (juce::Base64::convertFromBase64 (decoded, stored))
            slot.encodedAudio.append (decoded.getData(), decoded.getDataSize());
    }
    else if (const int shared = element.getIntAttribute ("audioRef", 0); shared != 0)
    {
        const auto found = sharedAudio.find (shared);

        if (found != sharedAudio.end())
            slot.encodedAudio = found->second;
    }

    if (slot.mode == UserWave::Mode::SingleCycle)
    {
        const auto encoded = element.getStringAttribute ("table");

        if (encoded.isNotEmpty())
        {
            juce::MemoryOutputStream decoded;

            if (juce::Base64::convertFromBase64 (decoded, encoded)
                && decoded.getDataSize() == sizeof (float) * (std::size_t) WaveAnalysis::tableSize)
            {
                // Rebuild the ladder rather than storing it. Reading the harmonics
                // back out of the stored table is exact, so this loses nothing.
                std::vector<float> table ((std::size_t) WaveAnalysis::tableSize);
                std::memcpy (table.data(), decoded.getData(), decoded.getDataSize());

                const auto harmonics = WaveAnalysis::harmonicsFromTable (table.data());

                slot.tables.resize ((std::size_t) (WaveAnalysis::mipLevels * WaveAnalysis::tableSize));
                WaveAnalysis::renderMipmaps (harmonics, slot.tables.data());

                slot.active = true;
                return;
            }
        }

        // No usable table: fall through and rebuild the whole slot from the audio.
    }

    // Rebuild from the slot's own audio. Nothing on disk is consulted, so this
    // works on a machine that has never seen the file the slot came from.
    std::vector<float> mono;
    double sampleRate = 0.0;

    if (! decodeSlotAudio (slot.encodedAudio, mono, sampleRate))
    {
        // Written before the audio travelled in the slot: the only place left to
        // look is the copy that version kept in the Wavetables folder.
        auto legacy = wavetableFolder().getChildFile ("Samples").getChildFile (slot.sourceFile);
        juce::String ignored;

        if (! readFileAsMono (legacy, mono, sampleRate, ignored))
            return;

        // Take it into the slot, so re-saving this preset makes it self contained
        // and that folder is never needed again.
        slot.encodedAudio = encodeSlotAudio (mono, sampleRate);
    }

    // buildSlot rewrites what it plays from and takes the mode as an argument, so
    // the mode read out of the XML is taken first.
    const auto keptMode = slot.mode;
    const auto keptName = slot.name;
    const auto keptSource = slot.sourceFile;

    buildSlot (slot, mono, sampleRate, keptMode);

    slot.name = keptName;
    slot.sourceFile = keptSource;
}

void UserWaveLibrary::restoreFromStateXml (const juce::XmlElement& xml)
{
    if (! xml.hasTagName ("USERWAVES"))
        return;

    for (int g = 0; g < UserWave::numGroups; ++g)
    {
        const auto group = (UserWave::Group) g;

        for (int i = 0; i < UserWave::numSlots; ++i)
            editable->editableSlot (group, i) = UserWaveSlot();
    }

    // Audio written once and named by the other slots that share it. Gathered up
    // front, because the slot that names a block can come before the one that
    // carries it -- and decoded once here rather than once per slot sharing it.
    std::map<int, juce::MemoryBlock> sharedAudio;

    for (auto* element : xml.getChildWithTagNameIterator ("SLOT"))
    {
        const int id = element->getIntAttribute ("audioId", 0);
        const auto stored = element->getStringAttribute ("audio");

        if (id == 0 || stored.isEmpty() || sharedAudio.count (id) != 0)
            continue;

        juce::MemoryOutputStream decoded;

        if (juce::Base64::convertFromBase64 (decoded, stored))
            sharedAudio[id].append (decoded.getData(), decoded.getDataSize());
    }

    for (auto* element : xml.getChildWithTagNameIterator ("SLOT"))
    {
        const int index = element->getIntAttribute ("index", -1);

        if (index < 0 || index >= UserWave::numSlots)
            continue;

        UserWaveSlot rebuilt;
        restoreSlotFromXml (*element, rebuilt, sharedAudio);

        UserWave::Group group;

        if (UserWave::groupFromTag (element->getStringAttribute ("group"), group))
        {
            editable->editableSlot (group, index) = std::move (rebuilt);
            continue;
        }

        // No tag: written when all five lists shared one set of slots. Putting
        // it into all five is what that song sounded like when it was saved.
        for (int g = 0; g < UserWave::numGroups; ++g)
            editable->editableSlot ((UserWave::Group) g, index) = rebuilt;
    }

    publish();
}

//==============================================================================
void UserWaveLibrary::saveToDisk() const
{
    auto folder = wavetableFolder();
    folder.createDirectory();

    if (auto xml = createStateXml())
        xml->writeTo (folder.getChildFile ("index.xml"));

    pruneUnusedSampleCopies();
}

void UserWaveLibrary::pruneUnusedSampleCopies() const
{
    auto folder = wavetableFolder().getChildFile ("Samples");

    if (! folder.isDirectory())
        return;

    // What is still wanted there. A slot that carries its own audio -- every slot
    // imported by this version -- wants nothing, so this list is empty on any
    // library built from here on, and the folder empties itself.
    juce::StringArray stillWanted;

    for (int g = 0; g < UserWave::numGroups; ++g)
    {
        for (int i = 0; i < UserWave::numSlots; ++i)
        {
            const auto& slot = editable->slot ((UserWave::Group) g, i);

            if (slot.active && slot.encodedAudio.getSize() == 0 && slot.sourceFile.isNotEmpty())
                stillWanted.addIfNotAlreadyThere (slot.sourceFile);
        }
    }

    for (const auto& file : folder.findChildFiles (juce::File::findFiles, false))
        if (! stillWanted.contains (file.getFileName()))
            file.deleteFile();

    // The plugin keeps no copies any more, so an empty Samples folder is a
    // leftover of its own.
    if (folder.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
        folder.deleteFile();
}

void UserWaveLibrary::loadFromDisk()
{
    auto index = wavetableFolder().getChildFile ("index.xml");

    if (! index.existsAsFile())
    {
        publish();
        return;
    }

    if (auto xml = juce::XmlDocument::parse (index))
        restoreFromStateXml (*xml);
    else
        publish();
}
