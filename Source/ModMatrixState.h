#pragma once

#include "ModMatrix.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <unordered_map>

namespace spacedust
{
    /** Every knob an LFO is allowed to reach, in a fixed order.

        Built once, by walking the APVTS parameter list, so a parameter added
        later becomes assignable with no extra work here. The ORDER is the slot
        order the compiled routings index into, and it is stable for one run of
        the plugin because the parameter list is. It is NOT stable across
        versions, which is why the patch stores parameter ids and not slots. */
    class DestinationTable
    {
    public:
        void build (juce::AudioProcessorValueTreeState& apvts);

        int size() const noexcept { return (int) ids.size(); }

        const std::string& idAt (int slot) const { return ids[(size_t) slot]; }

        /** -1 when this id is not a legal destination, which is what an old
            patch naming a parameter that no longer exists must produce. */
        int slotFor (const std::string& id) const;

        DestRange rangeAt (int slot) const { return ranges[(size_t) slot]; }

        juce::RangedAudioParameter* paramAt (int slot) const { return params[(size_t) slot]; }

        /** Whether this parameter may be modulated at all. Public so the editor
            can decide which knobs light up in assign mode. */
        static bool isLegalDestination (const juce::RangedAudioParameter& p);

        /** Which LFO owns this parameter, or -1 if it is not an LFO control.

            "lfo3Rate" gives 2. Used to stop an LFO reaching its own controls:
            legality alone cannot decide that, because it depends on which LFO is
            doing the reaching. Applied when routings are compiled, so a
            self-routing arriving from an edited patch is dropped rather than
            trusted, and again in assign mode so an LFO's own knobs never light
            up while it is the one being assigned. */
        static int lfoOwnerOf (const std::string& parameterId);

    private:
        std::vector<std::string>                 ids;
        std::vector<DestRange>                   ranges;
        std::vector<juce::RangedAudioParameter*> params;
        std::unordered_map<std::string, int>     lookup;
    };

    /** The routing list as a MODMATRIX element, for the saved patch. */
    std::unique_ptr<juce::XmlElement> toXml (const ModMatrix& matrix);

    /** Read a MODMATRIX element back. Replaces whatever the matrix held.
        Entries with an illegal LFO index or an empty id are skipped, so a patch
        from a build with different parameters loads rather than failing. */
    void fromXml (const juce::XmlElement& element, ModMatrix& matrix);
}
