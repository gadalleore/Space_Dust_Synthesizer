#pragma once

#include <string>
#include <vector>

/** The modulation matrix.

    Deliberately free of JUCE so its arithmetic can be tested in seconds without
    building a plugin (Source/NoteLockGrid.* is the same idea). The JUCE side --
    finding which parameters may be destinations, and saving the list into the
    patch -- lives in ModMatrixState.h.

    There is no limit on how many routings exist. That is why the list is saved
    in the patch rather than exposed as parameters: one parameter per possible
    pair would be four LFOs times about 150 knobs. */
namespace spacedust
{
    inline constexpr int numLfos = 4;

    /** The destinations that are applied INSIDE a per-sample loop, by hand.

        These predate the matrix: each was one entry of the LFO Destination
        drop-down, and each has its own formula written into
        SynthVoice::renderNextBlock (or, for the master gain, into
        SpaceDustAudioProcessor::processBlock). The formulas are PROPORTIONAL --
        a filter cutoff is multiplied by (1 + m/2), an oscillator level by
        (1 + m), a pitch by 2^m -- and the sound of every patch saved before
        assign mode existed depends on exactly those shapes.

        So the matrix decides WHICH of them an LFO reaches and by how much, and
        the formulas themselves are left alone. An amount of +1.0 reproduces the
        old drop-down character for character.

        Because of that they do NOT read the voice scratch, and they are not in
        the block-rate voice parameter list either: either one would apply the
        same modulation a second time. */
    enum PerSampleModDest
    {
        psm_osc1Pitch = 0,    // "osc1CoarseTune"
        psm_osc2Pitch,        // "osc2CoarseTune"
        psm_filterCutoff,     // "filterCutoff"
        psm_osc1Level,        // "osc1Level"
        psm_osc2Level,        // "osc2Level"
        psm_noiseLevel,       // "noiseLevel"

        /** The six above are the voice's. The master gain is applied after the
            voices, in the processor, so it is kept out of what a voice is
            handed. */
        numVoicePerSampleMod,

        psm_masterVolume = numVoicePerSampleMod,   // "masterVolume"
        numPerSampleMod
    };

    /** One LFO reaching one knob. */
    struct ModRouting
    {
        int         lfoIndex = 0;    // 0..numLfos-1
        std::string destination;     // an APVTS parameter id
        float       amount = 0.0f;   // -1..+1
    };

    /** A destination knob's legal range, as the APVTS reports it. */
    struct DestRange
    {
        float start = 0.0f;
        float end   = 1.0f;

        float halfRange() const noexcept { return (end - start) * 0.5f; }
    };

    /** A routing with the strings and the lookups already taken out of it.

        The audio thread walks these, never the ModRouting list: no string
        compare, no allocation, and the scale is pre-multiplied. destSlot is an
        index into whatever array of destinations the caller compiled against. */
    struct CompiledRouting
    {
        int   destSlot = 0;
        int   lfoIndex = 0;
        float scale    = 0.0f;   // amount * range.halfRange()
    };

    class ModMatrix
    {
    public:
        //======================================================================
        // -- Editing. Message thread only. --

        /** Set how far one LFO moves one knob.

            An amount of zero REMOVES the routing rather than leaving a dead
            entry, so a drag back through the middle undoes the assignment. An
            illegal LFO index is refused. Assigning a pair that already exists
            replaces its amount. */
        void setRouting (int lfoIndex, const std::string& destination, float amount);

        /** Remove one LFO from one knob, leaving any other LFO on it alone. */
        void clearRouting (int lfoIndex, const std::string& destination);

        void clear();

        //======================================================================
        // -- Reading --

        float amountFor (int lfoIndex, const std::string& destination) const;

        /** Whether any LFO at all reaches this knob. This is what decides
            whether the indicator bar is drawn beside it. */
        bool hasAnyRouting (const std::string& destination) const;

        const std::vector<ModRouting>& routings() const noexcept { return list; }

        //======================================================================
        // -- The value --

        /** Where the knob actually sits.

            lfoValues points at numLfos floats, each -1..+1 and ALREADY scaled by
            that LFO's own Depth knob. The amount here is a further trim, so
            Depth is the master level for one LFO and the amount is the balance
            between the knobs it reaches. Nothing may apply Depth twice.

            Convenience for tests and for the editor. The audio thread uses the
            compiled form instead -- this one compares strings. */
        float applyByName (const std::string& destination, float base,
                           DestRange range, const float* lfoValues) const noexcept;

        /** The same arithmetic, for one already-compiled destination.

            out is indexed by destSlot. bases and ranges are too. Walks the
            compiled list once, adds every contribution, then clamps. */
        static void applyCompiled (const CompiledRouting* compiled, int numCompiled,
                                   const float* bases, const DestRange* ranges,
                                   int numDests, const float* lfoValues,
                                   float* out) noexcept;

    private:
        std::vector<ModRouting> list;

        const ModRouting* find (int lfoIndex, const std::string& destination) const;
    };
}
