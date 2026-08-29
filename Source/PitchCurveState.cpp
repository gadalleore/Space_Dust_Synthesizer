#include "PitchCurveState.h"

namespace spacedust
{
    std::unique_ptr<juce::XmlElement> pitchCurveToXml (const PitchCurve& curve)
    {
        auto root = std::make_unique<juce::XmlElement> ("PITCHCURVE");

        for (int i = 0; i < curve.pointCount(); ++i)
        {
            const auto p = curve.pointAt (i);
            auto* e = root->createNewChildElement ("POINT");
            e->setAttribute ("t", (double) p.t01);
            e->setAttribute ("semitones", (double) p.semitones);
        }

        return root;
    }

    void pitchCurveFromXml (const juce::XmlElement& element, PitchCurve& curve)
    {
        curve.clear();

        for (auto* e : element.getChildWithTagNameIterator ("POINT"))
        {
            const float t         = (float) e->getDoubleAttribute ("t", 0.0);
            const float semitones = (float) e->getDoubleAttribute ("semitones", 0.0);
            curve.addPoint (t, semitones);
        }
    }
}
