#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "TrimSession.h"
#include "WaveAnalysis.h"

#include <atomic>
#include <functional>
#include <map>
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

    /** Samples kept clear at each end of a Full Sample slot, and the shortest
        region the start and end markers may leave between them.

        Both belong to the geometry that works out what a slot plays, which lives
        in WaveAnalysis so it can be checked without a plugin host -- see
        WaveAnalysis::regionForTrim. Named here as well because the panel and the
        slot both reason in these terms. */
    inline constexpr int interpolationGuard = WaveAnalysis::interpolationGuard;
    inline constexpr int minPlayLength = WaveAnalysis::minPlayLength;

    /** The most leading silence a slot holding this many samples may carry.

        Silence costs exactly what sound costs -- it is stored as samples like
        everything else -- so it comes out of the same fifteen seconds a slot has
        in the first place. A short sample can be pushed a long way back; one that
        already fills a slot cannot be pushed back at all. */
    inline int maxPadSamples (int fileLength, double sampleRate) noexcept
    {
        if (sampleRate <= 0.0)
            return 0;

        return juce::jmax (0, (int) (maxSampleSeconds * sampleRate) - fileLength);
    }
}

//==============================================================================
/**
    One imported waveform, ready to play. Immutable once built.
*/
struct UserWaveSlot
{
    bool active = false;
    juce::String name;

    /** Name of the file this came from. Kept to show the user where a slot came
        from; nothing is ever looked up by it. Slots imported before the audio was
        carried in the slot itself name a copy in the Wavetables\Samples folder,
        and those are still read from there once, on load. */
    juce::String sourceFile;

    /** The audio this slot was built from: mono, no longer than
        UserWave::maxSampleSeconds, FLAC-compressed.

        This is the slot's own copy of its material, and the only one there is. It
        travels inside a preset and inside a saved song, so a waveform cannot be
        lost by deleting, moving or renaming the file it was imported from, and no
        copy of that file is left anywhere on the user's disk. It is also what a
        change of Mode is rebuilt from, since the two modes are two different
        readings of the same audio and only one is held ready to play at a time. */
    juce::MemoryBlock encodedAudio;

    UserWave::Mode mode = UserWave::Mode::SingleCycle;

    /** Whether a Full Sample slot repeats for as long as the key is held.

        On, a held note goes round and round the file -- which is what makes a
        sampled string or a pad hold. Off, the file is heard once and then the
        oscillator falls silent, which is what a hit, a stab or a whole recorded
        phrase wants: it plays out to its end and stops there instead of starting
        over on top of itself.

        Means nothing in Single Cycle mode, where one turn of the phase IS the
        waveform and repeating it is the whole point. */
    bool loop = true;

    /** Where the sound starts and ends inside the file, in samples of the file.

        These are the two markers the player drags across the picture, and they
        are the ONLY record of where the sound was cut: the audio in the slot is
        never shortened, so every cut can be taken back by dragging the marker
        out again. What plays is worked out from them each time the slot is built
        -- see playStart and playLength.

        Either marker can be dragged OFF the file, and out there it means silence.
        trimStart below zero puts silence in FRONT of the recording, so a sample
        can be made to begin late -- a hit that lands after the beat, a pad that
        arrives a moment after the key goes down. trimEnd past fileLength puts
        silence AFTER it, which is what holds a one-shot's key open past the end
        of the sound and what gives a loop a measured gap before it comes round
        again. There is nothing to reach for outside a recording, so the only
        thing a marker can mean out there is silence, and it makes it.

        Inside the file they are ordinary positions: trimEnd is one past the last
        file sample played. Zero, and anything at or before trimStart, means "to
        the end of the file" -- which is what every slot saved before the markers
        existed has, and what a freshly imported one gets.

        Meaningless in Single Cycle mode, which takes one period out of the
        middle of the sound and never sees its ends. Kept across a change of mode
        all the same, so switching to Single Cycle and back does not throw the
        markers away. */
    int trimStart = 0;
    int trimEnd = 0;

    /** What the analysis found, kept for display. */
    double fundamentalHz = 0.0;
    double confidence = 0.0;
    juce::String pitchLabel;

    /** Whether the pitch was trusted enough to re-tune the sample. */
    bool retuned = false;

    /** Whether re-tuning is allowed at all.

        False for a slot made by Resample. That sound was recorded from middle C,
        and middle C is the note a slot plays back unchanged, so the two already
        agree exactly -- and any reading the pitch detector takes of it can only
        disagree. A patch with a strong fifth in it, or an octave-down sub, is
        read a fifth or an octave out often enough to matter, and the result is a
        resample that plays back at the wrong speed for a reason the player cannot
        see.

        Kept in the slot and in saved state rather than worked out again, because
        a slot is rebuilt whenever its mode changes and whenever a song is opened,
        and either of those would otherwise re-tune it later. */
    bool allowRetune = true;

    //==========================================================================
    // -- Single Cycle --

    /** WaveAnalysis::mipLevels tables of WaveAnalysis::tableSize samples, richest
        first. Empty in Full Sample mode. */
    std::vector<float> tables;

    //==========================================================================
    // -- Full Sample --

    /** What the oscillator reads, at the rate of the file it came from. Empty in
        Single Cycle mode.

        NOT the file. It is the file with room built around it:

            [ guard ][ silence ][ the whole imported file ][ silence ][ guard ]

        The guards are what the interpolator reads past the ends (see
        interpolationGuard). The silence is whatever the markers were dragged out
        by, and it is real samples because that is the only way the phase can run
        through it.

        The WHOLE file is here whatever the markers say, and that is deliberate:
        trimming is a pair of numbers, not a cut, so material outside the markers
        is still there to be drawn behind them and still there to be taken back. */
    std::vector<float> sample;

    double fileSampleRate = 0.0;

    /** How many samples of it are the imported file, and where that file begins.

        Everything the player points at is a position in the FILE -- the markers,
        the picture, the times on screen -- while everything the oscillator reads
        is a position in the buffer above. These convert between the two. */
    int fileLength = 0;
    int padStart() const noexcept { return trimStart < 0 ? -trimStart : 0; }
    int padEnd() const noexcept { return trimEnd > fileLength ? trimEnd - fileLength : 0; }
    int fileOffset() const noexcept { return UserWave::interpolationGuard + padStart(); }

    /** The part that PLAYS: the silence in front, the file between the two
        markers, and the silence after. A slot with the markers where they were
        imported plays the file end to end, which is what every slot did before
        there were markers.

        A one-shot reads exactly this and stops. A looping one reads the loop
        below, which starts a crossfade further in. */
    int playStart = 0;
    int playLength = 0;

    /** The looped region, and the overlap that hides its seam.

        The loop deliberately starts a little way after the start marker and ends
        at the end marker, so the crossfade at the end of the loop has real
        material to fade into -- the samples immediately BEFORE the loop start.
        Without that reserve the loop would have to fade into silence, which is
        audible as a pulse once per pass. */
    int loopStart = 0;
    int loopLength = 0;
    int crossfade = 0;

    /** What the oscillator's phase increment is multiplied by.

        In Single Cycle mode this is 1: one turn of the phase is one period, which
        is what the increment already means.

        In Full Sample mode one turn has to cover the whole region being read
        instead of one period, and the ratio between those two is fixed per slot,
        so the whole mode costs one multiply.

        Which region that is depends on the loop -- a one-shot reads playLength
        and a looping slot reads loopLength -- so turning the loop on or off
        recomputes this. It has to: the point of the scale is that the sample
        comes out at the speed it went in whatever length the phase is covering,
        and a scale left over from the other length would transpose it. */
    double phaseIncrementScale = 1.0;

    /** The region one turn of the phase covers. See phaseIncrementScale. */
    int playSpan() const noexcept { return loop ? loopLength : playLength; }

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

    /** Build a slot from audio that is already in memory -- what Resample does
        with the synth's own output.

        Everything from here on is the same as an import from disk: the audio is
        summed and normalised the same way, analysed the same way, and stored in
        the slot the same way. The slot names no file, because there is no file:
        the sound never existed anywhere but inside the plugin. */
    bool importAudio (std::vector<float> mono, double sampleRate, const juce::String& name,
                      UserWave::Group group, int slotIndex, UserWave::Mode mode,
                      juce::String& errorMessage);

    /** Rebuild a slot in the other mode, re-reading its stored copy. */
    bool setSlotMode (UserWave::Group group, int slotIndex, UserWave::Mode mode,
                      juce::String& errorMessage);

    /** Turn a Full Sample slot's loop on or off. The audio is not rebuilt -- the
        loop is read at playback time -- but the phase scale is, because the two
        settings read regions of different lengths. See phaseIncrementScale. */
    void setSlotLoop (UserWave::Group group, int slotIndex, bool shouldLoop);

    /** Move the start and end markers of a Full Sample slot.

        Both are in samples of the imported FILE. A negative start is silence put
        in front of it; see UserWaveSlot::trimStart. Nothing is thrown away, so
        any cut can be taken back by dragging the marker out again, and the
        request is clamped rather than refused: a start pushed further back than a
        slot has room for stops where the room runs out, and the two are never let
        closer together than UserWave::minPlayLength.

        Rebuilds the slot from its own stored copy of the audio, exactly as a
        change of mode does. */
    bool setSlotTrim (UserWave::Group group, int slotIndex, int trimStart, int trimEnd,
                      juce::String& errorMessage);

    //==========================================================================
    // -- Dragging a marker --
    //
    // setSlotTrim above is one finished decision: it decodes the slot's audio
    // again, rebuilds it, writes the index file and publishes. That is right for
    // a double-click reset and wrong thirty times a second.
    //
    // A session splits it up. The decode happens once at the start and the write
    // once at the end; only the rebuild and the publish happen in between. The
    // slot ends up in exactly the state setSlotTrim would have left it in.

    /** Begin a drag on a slot: decode its audio once and hold it.

        Refused, with a line for the player, on the same slots setSlotTrim
        refuses -- one that does not exist, one that is empty, and one that is
        not in Full Sample mode -- and on a slot that cannot be decoded. */
    bool beginTrimSession (UserWave::Group group, int slotIndex, juce::String& errorMessage);

    /** Put the markers where the mouse has them, and let the audio thread hear
        it. Rebuilds from the held audio and publishes. Writes nothing to disk.

        Does nothing, successfully, when the markers have not moved since the
        last call -- so a paused drag costs nothing at all. */
    bool updateTrimSession (int trimStart, int trimEnd, juce::String& errorMessage);

    /** End the drag: apply the last position if the mouse outran the timer, then
        write the index file and release the held audio.

        Safe, and successful, when no session is open. */
    bool endTrimSession (juce::String& errorMessage);

    bool trimSessionIsOpen() const noexcept { return trimSession.isOpen(); }

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

        Every slot carries its own audio, so a preset is self contained: it plays
        on a machine that has never seen the file it was imported from, and no
        copy of that file exists anywhere for the user to lose. A Single Cycle
        slot also carries its table, which is what it is rebuilt from -- the other
        ten levels are a transform away, and it saves decoding the audio on load.

        The size that costs is real: about a megabyte for a slot holding the full
        fifteen seconds, far less for a short one. It is the price of a preset
        that cannot be broken by tidying a Downloads folder. Slots holding the
        same sample -- what "Update All" makes -- pay it once between them: the
        first carries the audio under an id, the rest name that id. */
    std::unique_ptr<juce::XmlElement> createStateXml() const;

    /** Rebuild the slots from XML written by createStateXml().

        A slot with no group attribute was written before the lists were split,
        when there was one shared set, and so goes into all five -- which is
        exactly what that song sounded like when it was saved.

        A slot with no audio was written before the audio travelled inside it, and
        is read once from the copy that version left in the Wavetables folder;
        saving again makes it self contained. */
    void restoreFromStateXml (const juce::XmlElement& xml);

    /** Write the slots to the user's Wavetables folder, and read them back. This
        is what makes an imported waveform available in the next session and in
        every other instance of the plugin. One index file, holding the slots and
        their audio -- imported files are never copied anywhere. */
    void saveToDisk() const;
    void loadFromDisk();

    /** Where the index lives. Older versions also kept a copy of every imported
        file in a Samples folder here; see pruneUnusedSampleCopies(). */
    static juce::File wavetableFolder();

    /** Called on the message thread whenever the slots change, so the editor can
        refresh its dropdowns. */
    std::function<void()> onChange;

private:
    /** Delete the file copies that no slot needs any more.

        Earlier versions copied every imported file into Wavetables\Samples and
        never removed any of them, so re-importing a slot ten times left ten files
        behind for good. Nothing is copied there now, and a copy is only still
        wanted by a slot that was imported before the audio travelled inside the
        slot; everything else in that folder is a leftover and goes. When the last
        one goes, so does the folder.

        Run from saveToDisk(), which means when the user changes a slot -- not on
        load. A song saved by an older version can still be pointing at one of
        those copies, and opening that song is what bakes its audio into the slot
        for good; pruning at startup would take the file away first. */
    void pruneUnusedSampleCopies() const;

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

    /** Build one slot from decoded mono audio. Does the analysis.

        The audio handed in is the WHOLE imported file, every time. Where the
        markers stand is read out of the slot, so this is also what rebuilds a
        slot after they move. */
    static void buildSlot (UserWaveSlot& slot, const std::vector<float>& mono,
                           double sampleRate, UserWave::Mode mode);

    /** Work out phaseIncrementScale for a slot as it now stands.

        Apart because the loop flag changes it without anything being rebuilt --
        the length the phase has to cover is the loop's or the whole trimmed
        region's, and those are not the same number. */
    static void updatePhaseScale (UserWaveSlot& slot);

    /** Get a slot's own audio back, for a rebuild. False, with a line for the
        player, when there is none to be had.

        Slots imported before the audio travelled inside them keep a copy in the
        Wavetables folder instead, and this is where that copy is taken into the
        slot for good -- so a rebuild is the last time it is ever looked for. */
    bool loadSlotAudio (UserWaveSlot& slot, std::vector<float>& mono, double& sampleRate,
                        juce::String& errorMessage);

    /** Put mono audio into a slot and make it permanent: analyse it, keep the
        audio inside the slot, write the index and publish a fresh bank.

        Shared by importFile() and importAudio(), which differ only in where the
        audio came from and in whether there is a file to name. The slot is left
        empty on any failure, so a slot that says it holds something always does. */
    bool storeSlot (const std::vector<float>& mono, double sampleRate,
                    const juce::String& name, const juce::String& sourceFile,
                    bool allowRetune, UserWave::Group group, int slotIndex,
                    UserWave::Mode mode, juce::String& errorMessage);

    /** Read one SLOT element into a loose slot, doing whichever rebuild its mode
        needs. Kept apart from where the result is stored so that a slot with no
        group -- which belongs in all five lists -- is decoded once and copied,
        rather than being decoded five times.

        sharedAudio holds the blocks written once and named by id, for the slots
        that share a sample rather than carrying their own copy of it. */
    void restoreSlotFromXml (const juce::XmlElement& element, UserWaveSlot& slot,
                             const std::map<int, juce::MemoryBlock>& sharedAudio);

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

    /** The drag in progress, and the audio it is rebuilding from.

        The audio is the whole imported file and is decoded once, when the
        session opens. Holding it is the entire point: decoding is what made a
        marker too expensive to move while a note was sounding. */
    TrimSession trimSession;
    std::vector<float> trimAudio;
    double trimAudioRate = 0.0;

    juce::AudioFormatManager formatManager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UserWaveLibrary)
};
