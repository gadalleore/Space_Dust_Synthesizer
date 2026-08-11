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

    juce::String sanitiseFileName (const juce::String& name)
    {
        auto cleaned = juce::File::createLegalFileName (name);
        return cleaned.isEmpty() ? juce::String ("sample") : cleaned;
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

    for (int i = 0; i < UserWave::numSlots; ++i)
        copy->editableSlot (i) = source.slot (i);

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
    slot.loopStart = 0;
    slot.loopLength = 0;
    slot.crossfade = 0;
    slot.phaseIncrementScale = 1.0;

    const int numSamples = (int) mono.size();

    if (numSamples < 64)
    {
        slot.active = false;
        return;
    }

    // The pitch is wanted in both modes, but for different reasons: it sets the
    // length of the cycle to extract in one, and the playback speed in the other.
    const auto estimate = WaveAnalysis::detectFundamental (mono.data(), numSamples, sampleRate);

    slot.fundamentalHz = estimate.frequencyHz;
    slot.confidence = estimate.confidence;

    char label[32] = {};
    WaveAnalysis::describePitch (estimate.frequencyHz, label, sizeof (label));
    slot.pitchLabel = juce::String (label);

    if (mode == UserWave::Mode::SingleCycle)
    {
        const auto harmonics = WaveAnalysis::analyseHarmonics (mono.data(), numSamples,
                                                               sampleRate, estimate.frequencyHz);

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
    slot.sample = mono;

    // Reserve material at both ends. The crossfade at the end of the loop reads
    // the samples immediately BEFORE the loop start, and the interpolator reads
    // one sample back and two forward, so both ends need clearance or the loop
    // would read off the end of the buffer.
    constexpr int interpolationGuard = 3;

    int fade = (int) (0.010 * sampleRate);
    fade = juce::jlimit (0, (numSamples - 16) / 4, fade);

    slot.crossfade = fade;
    slot.loopStart = fade + interpolationGuard;
    slot.loopLength = numSamples - slot.loopStart - interpolationGuard;

    if (slot.loopLength <= 0)
    {
        slot.active = false;
        return;
    }

    // Whether to re-tune at all. A drum hit has no fundamental to put on middle
    // C, and forcing one on it only makes the sample play back at a speed the
    // user did not ask for and cannot explain.
    slot.retuned = estimate.confidence >= UserWave::minPitchConfidence;
    const double rootHz = slot.retuned ? estimate.frequencyHz : WaveAnalysis::middleCHz;

    // The whole of Full Sample mode, in one constant. The derivation, and the test
    // that proves middle C really does land on the recorded pitch, are both over
    // in WaveAnalysis where they can run without a plugin host.
    slot.phaseIncrementScale = WaveAnalysis::playbackPhaseScale (sampleRate, rootHz, slot.loopLength);

    slot.active = true;
}

//==============================================================================
bool UserWaveLibrary::importFile (const juce::File& file, int slotIndex, UserWave::Mode mode,
                                  juce::String& errorMessage)
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

    // Keep our own copy before anything else. A song saved today has to still
    // load when the user has tidied their Downloads folder.
    auto folder = wavetableFolder().getChildFile ("Samples");
    folder.createDirectory();

    const auto copyName = "slot" + juce::String (slotIndex + 1) + "_"
                        + sanitiseFileName (file.getFileNameWithoutExtension())
                        + file.getFileExtension();

    auto destination = folder.getChildFile (copyName);

    if (destination != file)
    {
        destination.deleteFile();

        if (! file.copyFileTo (destination))
        {
            errorMessage = "Could not copy the file into the Wavetables folder.";
            return false;
        }
    }

    auto& slot = editable->editableSlot (slotIndex);
    slot.name = file.getFileNameWithoutExtension();
    slot.sourceFile = copyName;

    buildSlot (slot, mono, sampleRate, mode);

    if (! slot.active)
    {
        errorMessage = "Nothing pitched could be found in that file.";
        clearSlot (slotIndex);
        return false;
    }

    saveToDisk();
    publish();
    return true;
}

//==============================================================================
bool UserWaveLibrary::setSlotMode (int slotIndex, UserWave::Mode mode, juce::String& errorMessage)
{
    if (slotIndex < 0 || slotIndex >= UserWave::numSlots)
    {
        errorMessage = "That waveform slot does not exist.";
        return false;
    }

    auto& slot = editable->editableSlot (slotIndex);

    if (! slot.active)
    {
        errorMessage = "That slot is empty.";
        return false;
    }

    if (slot.mode == mode)
        return true;

    // Both modes are built from the source audio, and only one of them is kept in
    // memory at a time, so switching means reading the copy again.
    auto source = wavetableFolder().getChildFile ("Samples").getChildFile (slot.sourceFile);

    std::vector<float> mono;
    double sampleRate = 0.0;

    if (! readFileAsMono (source, mono, sampleRate, errorMessage))
    {
        errorMessage = "The copy of \"" + slot.name + "\" is missing, so it cannot be rebuilt.";
        return false;
    }

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

void UserWaveLibrary::renameSlot (int slotIndex, const juce::String& newName)
{
    if (slotIndex < 0 || slotIndex >= UserWave::numSlots)
        return;

    auto& slot = editable->editableSlot (slotIndex);

    if (! slot.active)
        return;

    slot.name = newName.trim().isEmpty() ? slot.name : newName.trim();

    saveToDisk();
    publish();
}

void UserWaveLibrary::clearSlot (int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= UserWave::numSlots)
        return;

    editable->editableSlot (slotIndex) = UserWaveSlot();

    saveToDisk();
    publish();
}

juce::StringArray UserWaveLibrary::slotChoiceNames() const
{
    juce::StringArray names;

    for (int i = 0; i < UserWave::numSlots; ++i)
        names.add (choiceNameForSlot (i));

    return names;
}

juce::String UserWaveLibrary::choiceNameForSlot (int slotIndex) const
{
    const auto& slot = editable->slot (slotIndex);

    if (slot.active && slot.name.isNotEmpty())
        return slot.name;

    return "User " + juce::String (slotIndex + 1);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> UserWaveLibrary::createStateXml() const
{
    auto root = std::make_unique<juce::XmlElement> ("USERWAVES");

    for (int i = 0; i < UserWave::numSlots; ++i)
    {
        const auto& slot = editable->slot (i);

        if (! slot.active)
            continue;

        auto* element = root->createNewChildElement ("SLOT");
        element->setAttribute ("index", i);
        element->setAttribute ("name", slot.name);
        element->setAttribute ("source", slot.sourceFile);
        element->setAttribute ("mode", (int) slot.mode);
        element->setAttribute ("fundamental", slot.fundamentalHz);
        element->setAttribute ("confidence", slot.confidence);
        element->setAttribute ("pitchLabel", slot.pitchLabel);
        element->setAttribute ("retuned", slot.retuned);

        // Only the richest table is stored. The other ten are a transform away,
        // and eight kilobytes per slot is small enough to sit inside a preset --
        // which is what makes a Single Cycle waveform travel with the song.
        if (slot.mode == UserWave::Mode::SingleCycle && ! slot.tables.empty())
        {
            juce::MemoryOutputStream encoded;
            juce::Base64::convertToBase64 (encoded, slot.tables.data(),
                                           sizeof (float) * (std::size_t) WaveAnalysis::tableSize);
            element->setAttribute ("table", encoded.toString());
        }
    }

    return root;
}

void UserWaveLibrary::restoreFromStateXml (const juce::XmlElement& xml)
{
    if (! xml.hasTagName ("USERWAVES"))
        return;

    for (int i = 0; i < UserWave::numSlots; ++i)
        editable->editableSlot (i) = UserWaveSlot();

    for (auto* element : xml.getChildWithTagNameIterator ("SLOT"))
    {
        const int index = element->getIntAttribute ("index", -1);

        if (index < 0 || index >= UserWave::numSlots)
            continue;

        auto& slot = editable->editableSlot (index);

        slot.name = element->getStringAttribute ("name");
        slot.sourceFile = element->getStringAttribute ("source");
        slot.mode = element->getIntAttribute ("mode", 0) == 1 ? UserWave::Mode::FullSample
                                                              : UserWave::Mode::SingleCycle;
        slot.fundamentalHz = element->getDoubleAttribute ("fundamental", 0.0);
        slot.confidence = element->getDoubleAttribute ("confidence", 0.0);
        slot.pitchLabel = element->getStringAttribute ("pitchLabel");
        slot.retuned = element->getBoolAttribute ("retuned", false);

        if (slot.mode == UserWave::Mode::SingleCycle)
        {
            const auto encoded = element->getStringAttribute ("table");

            if (encoded.isEmpty())
                continue;

            juce::MemoryOutputStream decoded;

            if (! juce::Base64::convertFromBase64 (decoded, encoded))
                continue;

            if (decoded.getDataSize() != sizeof (float) * (std::size_t) WaveAnalysis::tableSize)
                continue;

            // Rebuild the ladder rather than storing it. Reading the harmonics
            // back out of the stored table is exact, so this loses nothing.
            std::vector<float> table ((std::size_t) WaveAnalysis::tableSize);
            std::memcpy (table.data(), decoded.getData(), decoded.getDataSize());

            const auto harmonics = WaveAnalysis::harmonicsFromTable (table.data());

            slot.tables.resize ((std::size_t) (WaveAnalysis::mipLevels * WaveAnalysis::tableSize));
            WaveAnalysis::renderMipmaps (harmonics, slot.tables.data());

            slot.active = true;
        }
        else
        {
            // A whole sample is too big to carry inside a song, so it is reloaded
            // from the copy kept in the Wavetables folder. A song moved to a
            // different machine needs that folder to come with it.
            auto source = wavetableFolder().getChildFile ("Samples").getChildFile (slot.sourceFile);

            std::vector<float> mono;
            double sampleRate = 0.0;
            juce::String ignored;

            if (readFileAsMono (source, mono, sampleRate, ignored))
            {
                const auto keptName = slot.name;
                const auto keptSource = slot.sourceFile;

                buildSlot (slot, mono, sampleRate, UserWave::Mode::FullSample);

                slot.name = keptName;
                slot.sourceFile = keptSource;
            }
        }
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
