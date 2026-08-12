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
    /** How many importable slots exist, per list. Fixed forever -- see the note
        above. */
    inline constexpr int numSlots = 8;

    /** Which waveform list a set of slots belongs to.

        Every list keeps its OWN eight slots. They used to share one set, so a
        drum imported for the Transient also became the sub oscillator's waveform
        and Oscillator 2's, all at once -- five controls that look independent
        moving together. They are separate now, and the Waveforms window's Update
        All button is what deliberately puts one slot into all five.

        The order is fixed: the tag below is written into every saved song, and
        the number behind it is never stored. */
    enum class Group
    {
        Osc1 = 0,
        Osc2,
        Sub,
        Noise,
        Transient
    };

    inline constexpr int numGroups = 5;

    /** Tag written into saved state, and into the name of the copied sample file
        so two lists can hold different files in the same numbered slot.

        Fixed forever: a song saved today finds its slots again by these. */
    inline const char* groupTag (Group group) noexcept
    {
        switch (group)
        {
            case Group::Osc1:      return "osc1";
            case Group::Osc2:      return "osc2";
            case Group::Sub:       return "sub";
            case Group::Noise:     return "noise";
            case Group::Transient: return "transient";
        }

        return "osc1";
    }

    /** What the panel calls this list, for the lines the player reads. */
    inline const char* groupName (Group group) noexcept
    {
        switch (group)
        {
            case Group::Osc1:      return "Waveform 1";
            case Group::Osc2:      return "Waveform 2";
            case Group::Sub:       return "Sub";
            case Group::Noise:     return "Noize";
            case Group::Transient: return "Transient";
        }

        return "Waveform 1";
    }

    /** The group a tag names. False when the tag is not one of ours, which is
        how state written before the lists were split is recognised -- it has no
        tag at all, and every list is meant to receive it. */
    inline bool groupFromTag (const juce::String& tag, Group& result) noexcept
    {
        for (int i = 0; i < numGroups; ++i)
        {
            const auto candidate = (Group) i;

            if (tag == groupTag (candidate))
            {
                result = candidate;
                return true;
            }
        }

        return false;
    }

    /** First dropdown index that means "user slot" for the two oscillators.
        0-3 are Sine, Triangle, Saw and Square.

        The sub oscillator shares this base, because it offers the same four
        shapes in the same order and plays them through the same code. */
    inline constexpr int oscUserBase = 4;

    /** First dropdown index that means "user slot" for the noise source.
        0-1 are White and Pink. */
    inline constexpr int noiseUserBase = 2;

    /** First dropdown index that means "user slot" for the Transient effect.
        0-9 are the ten built-in 808 and 909 drums.

        Written here rather than taken from SpaceDustTransient::NumTypes so that
        this header stays free of the effect; SpaceDustTransient.cpp holds a
        static_assert that the two agree. */
    inline constexpr int transientUserBase = 10;

    /** The largest of the bases above, so anything that has to be big enough for
        every list -- the Waveforms window's row area -- can be sized once. */
    inline constexpr int maxUserBase = transientUserBase;

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
    Every list's slots, as the audio thread sees them.

    One bank holds all five lists rather than there being one bank each, so the
    handover below stays a single atomic exchange and the audio thread keeps
    holding a single pointer -- which is what lets a voice look up its two
    oscillators, its sub and its noise source without five separate handovers to
    keep in step.

    Immutable and replaced whole rather than edited in place, so the audio thread
    never reads a slot that is half way through being rebuilt. UserWaveLibrary
    handles the handover.
*/
class UserWaveBank
{
public:
    UserWaveBank() = default;

    const UserWaveSlot& slot (UserWave::Group group, int index) const noexcept
    {
        return (index >= 0 && index < UserWave::numSlots)
                 ? slots[(int) group][index] : emptySlot;
    }

    UserWaveSlot& editableSlot (UserWave::Group group, int index) noexcept
    {
        jassert (index >= 0 && index < UserWave::numSlots);
        return slots[(int) group][index];
    }

    /** The slot a dropdown index selects, or nullptr if it selects a built-in
        waveform or an empty slot. userBase is oscUserBase, noiseUserBase or
        transientUserBase, and group says whose list is being read. */
    const UserWaveSlot* slotForChoice (int choiceIndex, int userBase,
                                       UserWave::Group group) const noexcept
    {
        const int index = choiceIndex - userBase;

        if (index < 0 || index >= UserWave::numSlots)
            return nullptr;

        const auto& entry = slots[(int) group][index];
        return entry.isPlayable() ? &entry : nullptr;
    }

private:
    UserWaveSlot slots[UserWave::numGroups][UserWave::numSlots];
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

    /** Names for one list's eight User entries, in slot order. */
    juce::StringArray slotChoiceNames (UserWave::Group group) const;

    //==========================================================================
    // -- Message thread --

    /** The bank as it currently stands, for the UI to read. Never null. */
    const UserWaveBank& bank() const noexcept { return *editable; }

    /** Read a file into one list's slot. Returns false and fills errorMessage on
        failure. Copies the file into the Wavetables folder first, so the slot
        survives the original being moved or deleted.

        Touches that list only. Use copySlotToAllGroups() to put the result into
        the other four. */
    bool importFile (const juce::File& file, UserWave::Group group, int slotIndex,
                     UserWave::Mode mode, juce::String& errorMessage);

    /** Rebuild a slot in the other mode, re-reading its stored copy. */
    bool setSlotMode (UserWave::Group group, int slotIndex, UserWave::Mode mode,
                      juce::String& errorMessage);

    void renameSlot (UserWave::Group group, int slotIndex, const juce::String& newName);
    void clearSlot (UserWave::Group group, int slotIndex);

    /** Put one slot into the SAME numbered slot of all four other lists -- the
        Update All button.

        The same number rather than the first free one in each, so the result is
        predictable: after this, slot 5 means the same waveform everywhere, and
        pressing it again after re-importing replaces the same five entries
        rather than filling the lists up. Anything already in those slots is
        overwritten. */
    bool copySlotToAllGroups (UserWave::Group group, int slotIndex,
                              juce::String& errorMessage);

    /** Empty every slot of every list.

        What Initialize Preset does: the parameters go back to their defaults, so
        every dropdown is back on a built-in shape, and leaving eight imported
        entries behind each of them would keep the last session's samples in a
        patch the player asked to be blank. The copies in the Wavetables folder
        are NOT deleted -- another saved song may still refer to them. */
    void clearAllSlots();

    /** Name to show in a dropdown for a slot -- the user's name, or "User n" when
        the slot is empty. */
    juce::String choiceNameForSlot (UserWave::Group group, int slotIndex) const;

    //==========================================================================
    // -- Persistence --

    /** The slots as XML, for embedding in a preset or a saved song.

        Single Cycle slots carry their table inline, so a preset is self contained
        -- eight kilobytes each, and the other ten levels are rebuilt on load.
        Full Sample slots carry only a reference to their copy in the Wavetables
        folder, because embedding fifteen seconds of audio eight times over would
        add tens of megabytes to every saved song. */
    std::unique_ptr<juce::XmlElement> createStateXml() const;

    /** Rebuild the slots from XML written by createStateXml().

        A slot with no group attribute was written before the lists were split,
        when there was one shared set, and so goes into all five -- which is
        exactly what that song sounded like when it was saved. */
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

    /** Read one SLOT element into a loose slot, doing whichever rebuild its mode
        needs. Kept apart from where the result is stored so that a slot with no
        group -- which belongs in all five lists -- is decoded once and copied,
        rather than being decoded five times. */
    void restoreSlotFromXml (const juce::XmlElement& element, UserWaveSlot& slot);

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
