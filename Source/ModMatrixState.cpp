#include "ModMatrixState.h"

namespace spacedust
{
    bool DestinationTable::isLegalDestination (const juce::RangedAudioParameter& p)
    {
        // Floats only. A wobble between "on" and "off", or between two waveform
        // names, is a different feature and would need a different control.
        if (dynamic_cast<const juce::AudioParameterFloat*> (&p) == nullptr)
            return false;

        const auto id = p.paramID.toStdString();

        // An LFO's own controls ARE destinations now, so one LFO can wobble
        // another's Rate, Depth or Phase -- those three are the only floats an
        // LFO has, so the float rule above already picks exactly them and leaves
        // Waveform, Sync and Retrigger alone (Giuseppe, 2026-08-31).
        //
        // What is NOT allowed is an LFO reaching its OWN controls, and that
        // cannot be decided here: this function is asked "may anything modulate
        // this?", not "may LFO 2 modulate this?". The self rule is enforced
        // where the source is known -- lfoOwnerOf below, applied when routings
        // are compiled and again when assign mode decides what to light up.

        // The host drives this one.
        if (id == "pitchBend")
            return false;

        return true;
    }

    int DestinationTable::lfoOwnerOf (const std::string& parameterId)
    {
        // "lfo" then a single digit, so "lfo3Rate" gives 2. Anything else, and
        // anything numbered outside the LFOs that exist, is not an LFO control.
        if (parameterId.rfind ("lfo", 0) != 0 || parameterId.size() < 4)
            return -1;

        const char digit = parameterId[3];

        if (digit < '1' || digit > '9')
            return -1;

        const int index = digit - '1';

        return index < numLfos ? index : -1;
    }

    void DestinationTable::build (juce::AudioProcessorValueTreeState& apvts)
    {
        ids.clear();
        ranges.clear();
        params.clear();
        lookup.clear();

        auto& processor = apvts.processor;

        for (auto* raw : processor.getParameters())
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (raw);

            if (ranged == nullptr || ! isLegalDestination (*ranged))
                continue;

            const auto id = ranged->paramID.toStdString();
            const auto r  = ranged->getNormalisableRange();

            lookup.emplace (id, (int) ids.size());
            ids.push_back (id);
            ranges.push_back (DestRange { r.start, r.end });
            params.push_back (ranged);
        }
    }

    int DestinationTable::slotFor (const std::string& id) const
    {
        const auto it = lookup.find (id);
        return it == lookup.end() ? -1 : it->second;
    }

    std::unique_ptr<juce::XmlElement> toXml (const ModMatrix& matrix)
    {
        auto root = std::make_unique<juce::XmlElement> ("MODMATRIX");

        for (const auto& r : matrix.routings())
        {
            auto* e = root->createNewChildElement ("ROUTING");
            e->setAttribute ("lfo", r.lfoIndex);
            e->setAttribute ("dest", juce::String (r.destination));
            e->setAttribute ("amount", (double) r.amount);
        }

        return root;
    }

    void fromXml (const juce::XmlElement& element, ModMatrix& matrix)
    {
        matrix.clear();

        for (auto* e : element.getChildWithTagNameIterator ("ROUTING"))
        {
            const int   lfo    = e->getIntAttribute ("lfo", -1);
            const auto  dest   = e->getStringAttribute ("dest").toStdString();
            const float amount = (float) e->getDoubleAttribute ("amount", 0.0);

            // setRouting refuses an illegal index and an empty id on its own, so
            // a patch saved by a build with different parameters loads rather
            // than failing.
            //
            // Deliberate: a routing that names a parameter THIS build does not
            // have is KEPT, not dropped. Dropping it would mean that opening a
            // patch in a build lacking that knob and re-saving it destroys the
            // routing permanently, and the player would never be told. Keeping
            // it costs nothing and reaches no audio:
            // rebuildCompiledRoutings looks the id up, gets -1, and skips it.
            matrix.setRouting (lfo, dest, amount);
        }
    }
}
