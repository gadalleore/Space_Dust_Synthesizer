#pragma once

#include "PitchCurve.h"

#include <juce_core/juce_core.h>

#include <memory>

namespace spacedust
{
    /** The drawn shape as a PITCHCURVE element, for the saved patch.

        Kept out of PitchCurve.h/.cpp -- like ModMatrix's toXml/fromXml are kept
        out of ModMatrix.h/.cpp -- so the curve itself stays free of JUCE and its
        test keeps building in seconds. */
    std::unique_ptr<juce::XmlElement> pitchCurveToXml (const PitchCurve& curve);

    /** Read a PITCHCURVE element back. Replaces whatever the curve held. */
    void pitchCurveFromXml (const juce::XmlElement& element, PitchCurve& curve);
}
