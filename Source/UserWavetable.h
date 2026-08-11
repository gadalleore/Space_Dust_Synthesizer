#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "WaveAnalysis.h"

#include <atomic>
#include <functional>
#include <vector>

/**
    User waveforms -- samples the player drops in, played as oscillators.

    WHY THERE ARE EXACTLY EIGHT SLOTS

    The waveform dropdowns are automatable Choice parameters, and VST3 and AU both
    freeze the number of items in a Choice when the plugin loads. A host writes
    automation against that fixed list and a session recalls it by position, so a
    list that grew when the user imported a ninth sample would silently move every
    automation point written before it.

    So the parameter always offers the same eight User slots and only their NAMES
    change. Nothing the user does can alter the shape of the parameter.

    HOW A SLOT IS PLAYED

    Both modes are driven by the same phase accumulator the built-in waveforms use,
    which is what lets detune, glide, the sub oscillator, key tracking and the
    oscillator oversampler keep working untouched.

      Single Cycle  one period of the sample, held as a ladder of band-limited
                    tables. One turn of the phase is one period, exactly as for a
                    sine, so the note played is the note heard.

      Full Sample   the whole file. One turn of the phase is one pass through the
                    file instead of one period, which is arranged by scaling the
                    phase increment by a constant (see phaseIncrementScale). The
                    oscillator's existing wrap then loops the sample for free.

    In both modes the analysis has already found the pitch of the source, and the
    playback rate is set so that middle C plays it at concert pitch -- so a sample
    recorded at F#3 is in tune with the song without the player doing anything.
*/
namespace UserWave
{
    /** How many importable slots exist. Fixed forever -- see the note above. */
    inline constexpr int numSlots = 8;

    /** First dropdown index that means "user slot" for the two oscillators.
        0-3 are Sine, Triangle, Saw and Square. */
    inline constexpr int oscUserBase = 4;

    /** First dropdown index that means "user slot" for the noise source.
        0-1 are White and Pink. */
    inline constexpr int noiseUserBase = 2;

    /** Longest sample kept for Full Sample mode. Past this the file is truncated,
        because eight slots of unbounded audio would be unbounded memory in every
        instance of the plugin, in every project the user ever opens. */
    inline constexpr double maxSampleSeconds = 15.0;

    /** Below this the pitch reading is treated as meaningless and the sample is
        played at its recorded speed on middle C instead of being re-tuned.
        Re-tuning a snare hit to concert pitch is not a musical act; it just makes
        the drum play back at the wrong speed for no reason the user can see. */
    inline constexpr double minPitchConfidence = 0.6;

    enum class Mode
    {
        SingleCycle = 0,
        FullSample  = 1
    };
}

//==============================================================================
/**
    One imported waveform, ready to play. Immutable once built.
*/
struct UserWaveSlot
{
    bool active = false;
    juce::String name;

    /** File this came from, as copied into the user's Wavetables folder. The copy
        is what makes a saved song still work after the original is moved. */
    juce::String sourceFile;

    UserWave::Mode mode = UserWave::Mode::SingleCycle;

    /** What the analysis found, kept for display. */
    double fundamentalHz = 0.0;
    double confidence = 0.0;
    juce::String pitchLabel;

    /** Whether the pitch was trusted enough to re-tune the sample. */
    bool retuned = false;

    //==========================================================================
    // -- Single Cycle --

    /** WaveAnalysis::mipLevels tables of WaveAnalysis::tableSize samples, richest
        first. Empty in Full Sample mode. */
    std::vector<float> tables;

    //==========================================================================
    // -- Full Sample --

    /** Mono, at the rate of the file it came from. Empty in Single Cycle mode. */
    std::vector<float> sample;

    double fileSampleRate = 0.0;

    /** The looped region, and the overlap that hides its seam.

        The loop deliberately starts a little way into the file and ends a little
        way before its end, so the crossfade at the end of the loop has real
        material to fade into -- the samples immediately BEFORE the loop start.
        Without that reserve the loop would have to fade into silence, which is
        audible as a pulse once per pass. */
    int loopStart = 0;
    int loopLength = 0;
    int crossfade = 0;

    /** What the oscillator's phase increment is multiplied by.

        In Single Cycle mode this is 1: one turn of the phase is one period, which
        is what the increment already means.

        In Full Sample mode one turn has to cover loopLength file samples instead
        of one period, and the ratio between those two is fixed per slot, so the
        whole mode costs one multiply. */
    double phaseIncrementScale = 1.0;

    //==========================================================================
    /** One sample, from a phase in [0, 1). Audio thread: no allocation, no locks.

        freqHz and sampleRate choose how much bandwidth to read in Single Cycle
        mode and are ignored in Full Sample mode. */
    float read (double phase01, double freqHz, double sampleRate) const noexcept;

    /** Whether this slot can produce sound. */
    bool isPlayable() const noexcept
    {
        return active && (mode == UserWave::Mode::SingleCycle ? ! tables.empty()
                                                              : loopLength > 0);
    }
};

//==============================================================================
/**
    A complete set of slots, as the audio thread sees it.

    Immutable and replaced whole rather than edited in place, so the audio thread
    never reads a slot that is half way through being rebuilt. UserWaveLibrary
    handles the handover.
*/
class UserWaveBank
{
public:
    UserWaveBank() = default;

    const UserWaveSlot& slot (int index) const noexcept
    {
        return (index >= 0 && index < UserWave::numSlots) ? slots[index] : emptySlot;
    }

    UserWaveSlot& editableSlot (int index) noexcept { return slots[index]; }

    /** The slot a dropdown index selects, or nullptr if it selects a built-in
        waveform or an empty slot. userBase is oscUserBase or noiseUserBase. */
    const UserWaveSlot* slotForChoice (int choiceIndex, int userBase) const noexcept
    {
        const int index = choiceIndex - userBase;

        if (index < 0 || index >= UserWave::numSlots)
            return nullptr;

        return slots[index].isPlayable() ? &slots[index] : nullptr;
    }

private:
    UserWaveSlot slots[UserWave::numSlots];
    static const UserWaveSlot emptySlot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UserWaveBank)
};

//==============================================================================
/**
    Owns the user's waveforms, and hands them to the audio thread safely.

    THE HANDOVER

    Importing happens on the message thread and takes hundreds of milliseconds --
    reading a file, finding the pitch, building eleven tables. The audio thread
    cannot wait for any of that, and cannot take a lock to find out whether it
    finished.

    So a new bank is built off to one side and handed over through a single
    atomic exchange, and the OLD bank travels back the same way to be deleted on
    the message thread. The audio thread only ever swaps its own pointer, so a
    bank can never be freed while it is being read:

        message thread   builds bank, pending.exchange(newBank)
                         if that returned a bank, the audio thread never took it,
                         so this thread still owns it and deletes it itself

        audio thread     pending.exchange(nullptr) at the top of the block;
                         if it got one, its previous bank goes into retired[]

        message thread   empties retired[] and deletes what it finds

    No locks, no allocation on the audio thread, and no window in which a pointer
    is both published and deleted.
*/
class UserWaveLibrary
{
public:
    UserWaveLibrary();
    ~UserWaveLibrary();

    //==========================================================================
    // -- Audio thread --

    /** Take the newly published bank, if there is one, and hand back the old.
        Call once at the top of each block.

        current is the caller's own pointer, which this updates. */
    void exchangeBank (UserWaveBank*& current) noexcept;

    /** Give back the bank the audio thread was holding, after audio has stopped.

        The audio thread owns whatever exchangeBank() handed it, so something has
        to free it at shutdown. Call this from the message thread, from the
        destructor of whatever called exchangeBank(). */
    void reclaimFromAudio (UserWaveBank*& current);

    /** Names for the eight User entries in a waveform dropdown, in slot order. */
    juce::StringArray slotChoiceNames() const;

    //==========================================================================
    // -- Message thread --

    /** The bank as it currently stands, for the UI to read. Never null. */
    const UserWaveBank& bank() const noexcept { return *editable; }

    /** Read a file into a slot. Returns false and fills errorMessage on failure.
        Copies the file into the Wavetables folder first, so the slot survives the
        original being moved or deleted. */
    bool importFile (const juce::File& file, int slotIndex, UserWave::Mode mode,
                     juce::String& errorMessage);

    /** Rebuild a slot in the other mode, re-reading its stored copy. */
    bool setSlotMode (int slotIndex, UserWave::Mode mode, juce::String& errorMessage);

    void renameSlot (int slotIndex, const juce::String& newName);
    void clearSlot (int slotIndex);

    /** Name to show in a dropdown for a slot -- the user's name, or "User n" when
        the slot is empty. */
    juce::String choiceNameForSlot (int slotIndex) const;

    //==========================================================================
    // -- Persistence --

    /** The slots as XML, for embedding in a preset or a saved song.

        Single Cycle slots carry their table inline, so a preset is self contained
        -- eight kilobytes each, and the other ten levels are rebuilt on load.
        Full Sample slots carry only a reference to their copy in the Wavetables
        folder, because embedding fifteen seconds of audio eight times over would
        add tens of megabytes to every saved song. */
    std::unique_ptr<juce::XmlElement> createStateXml() const;

    /** Rebuild the slots from XML written by createStateXml(). */
    void restoreFromStateXml (const juce::XmlElement& xml);

    /** Write the slots to the user's Wavetables folder, and read them back. This
        is what makes an imported waveform available in the next session and in
        every other instance of the plugin. */
    void saveToDisk() const;
    void loadFromDisk();

    /** Where the copies and the index live. */
    static juce::File wavetableFolder();

    /** Called on the message thread whenever the slots change, so the editor can
        refresh its dropdowns. */
    std::function<void()> onChange;

private:
    /** Hand the current slots to the audio thread as a fresh bank. */
    void publish();

    /** Delete anything the audio thread has finished with.

        Called at the start of every publish and again at shutdown, rather than on
        a timer. A timer would have to be started from wherever the processor is
        constructed, which is not guaranteed to be the message thread in every
        host -- and it buys nothing here: collecting on publish means at most one
        superseded bank is ever waiting, and it goes as soon as the next import
        happens or the plugin closes. */
    void collectRetired();

    /** Build one slot from decoded mono audio. Does the analysis. */
    static void buildSlot (UserWaveSlot& slot, const std::vector<float>& mono,
                           double sampleRate, UserWave::Mode mode);

    /** Read any audio file the host can decode into mono, truncated to the cap. */
    bool readFileAsMono (const juce::File& file, std::vector<float>& mono,
                         double& sampleRate, juce::String& errorMessage);

    /** Deep copy, because a bank is immutable once published. */
    static std::unique_ptr<UserWaveBank> cloneBank (const UserWaveBank& source);

    std::unique_ptr<UserWaveBank> editable;

    /** Handed to the audio thread. Null once it has been taken. */
    std::atomic<UserWaveBank*> pending { nullptr };

    /** Handed back by the audio thread, waiting to be deleted here.

        Sized far beyond what a person can generate: it takes one import to fill
        one entry and the timer empties it several times a second. */
    static constexpr int retiredCapacity = 32;
    std::atomic<UserWaveBank*> retired[retiredCapacity] {};

    juce::AudioFormatManager formatManager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UserWaveLibrary)
};
