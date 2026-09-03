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

            // Only written when there is something to write. A straight shape
            // saves exactly the file it saved before bending existed, so a
            // patch does not change on disk just because this build opened it.
            if (p.bend != 0.0f)
                e->setAttribute ("bend", (double) p.bend);

            if (p.skew != 0.0f)
                e->setAttribute ("skew", (double) p.skew);
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

            // Absent in every file written before bending existed, and the
            // default is what those files meant: a straight line.
            const float bend      = (float) e->getDoubleAttribute ("bend", 0.0);
            const float skew      = (float) e->getDoubleAttribute ("skew", 0.0);

            curve.addPoint (t, semitones, bend, skew);
        }
    }
}
