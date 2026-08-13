#include "FinalEQComponent.h"
#include "SpaceDustLookAndFeel.h"

//==============================================================================
// Static helpers

juce::String FinalEQComponent::freqId(int band) { return "finalEQB" + juce::String(band + 1) + "Freq"; }
juce::String FinalEQComponent::gainId(int band) { return "finalEQB" + juce::String(band + 1) + "Gain"; }
juce::String FinalEQComponent::qId  (int band) { return "finalEQB" + juce::String(band + 1) + "Q";    }
juce::String FinalEQComponent::typeId(int band) { return "finalEQB" + juce::String(band + 1) + "Type"; }

FinalEQComponent::BandType FinalEQComponent::getBandType(int band) const
{
    return SpaceDustFinalEQ::typeFromChoiceIndex(cachedType_[static_cast<size_t>(band)]);
}

//==============================================================================
FinalEQComponent::FinalEQComponent(juce::AudioProcessorValueTreeState& apvts, double sampleRate)
    : apvts_(apvts), sampleRate_(sampleRate > 0.0 ? sampleRate : 44100.0)
{
    // Seed cached values from APVTS
    const float defaultFreqs[5] = { 80.0f, 250.0f, 1000.0f, 4000.0f, 10000.0f };
    const float defaultQs[5]    = { 0.707f, 1.0f, 1.0f, 1.0f, 0.707f };
    const BandType defaultTypes[5] = {
        BandType::LowShelf, BandType::Bell, BandType::Bell, BandType::Bell, BandType::HighShelf
    };

    for (int b = 0; b < numBands; ++b)
    {
        // Already null-guarded (safe pattern). These are only read at construction
        // and on parameterChanged, so the risk is much lower than the unguarded
        // direct dereferences that used to exist in the main editor.
        if (auto* p = apvts_.getRawParameterValue(freqId(b)))
            cachedFreq_[b] = p->load();
        else
            cachedFreq_[b] = defaultFreqs[b];

        if (auto* p = apvts_.getRawParameterValue(gainId(b)))
            cachedGain_[b] = p->load();
        else
            cachedGain_[b] = 0.0f;

        if (auto* p = apvts_.getRawParameterValue(qId(b)))
            cachedQ_[b] = p->load();
        else
            cachedQ_[b] = defaultQs[b];

        if (auto* p = apvts_.getRawParameterValue(typeId(b)))
            cachedType_[b] = static_cast<int>(p->load());
        else
            cachedType_[b] = static_cast<int>(defaultTypes[b]);

        apvts_.addParameterListener(freqId(b), this);
        apvts_.addParameterListener(gainId(b), this);
        apvts_.addParameterListener(qId(b),    this);
        apvts_.addParameterListener(typeId(b), this);
    }

    setOpaque(false);

    // Spectrum first, curve second: children paint in the order they are added, so
    // this is what puts the bars behind the response curve and the band dots.
    spectrum_.setDrawBackground(false);          // the EQ paints its own panel
    spectrum_.setEdgePadding(0.0f);              // bars must span the exact freq axis
    spectrum_.setDisplayAlpha(0.45f);            // a backdrop, not the main subject
    spectrum_.setInterceptsMouseClicks(false, false);
    spectrum_.setAccessible(false);
    spectrum_.setSampleRate(sampleRate_);
    addAndMakeVisible(spectrum_);
    addAndMakeVisible(curveOverlay_);
}

FinalEQComponent::~FinalEQComponent()
{
    // Remove listeners FIRST so no further parameterChanged can trigger an async update,
    // then cancel any update already queued, so handleAsyncUpdate() can't run against a
    // half-destroyed object.
    for (int b = 0; b < numBands; ++b)
    {
        apvts_.removeParameterListener(freqId(b), this);
        apvts_.removeParameterListener(gainId(b), this);
        apvts_.removeParameterListener(qId(b),    this);
        apvts_.removeParameterListener(typeId(b), this);
    }
    cancelPendingUpdate();
}

//==============================================================================
void FinalEQComponent::setSampleRate(double sr)
{
    const double newRate = (sr > 0.0) ? sr : 44100.0;
    if (std::abs(newRate - sampleRate_) < 1.0)
        return;   // called from the editor timer; recompute only on a real change

    sampleRate_ = newRate;
    spectrum_.setSampleRate(sampleRate_);
    recomputeResponse();
}

void FinalEQComponent::setSampleSource(std::function<void(float*, int)> fillSamples)
{
    spectrum_.fillSamplesCallback = std::move(fillSamples);
    spectrum_.start();   // self-driven 60 fps, idle unless this tab is showing
}

void FinalEQComponent::setClipping(bool isClipping)
{
    spectrum_.setClipping(isClipping);
}

void FinalEQComponent::setSelectedBand(int band)
{
    const int b = juce::jlimit(0, numBands - 1, band);
    if (b == selectedBand_)
        return;

    selectedBand_ = b;
    curveOverlay_.repaint();
}

//==============================================================================
void FinalEQComponent::resized()
{
    // Leave a small inset so dots near the edges are fully visible
    displayArea_ = getLocalBounds().reduced(static_cast<int>(dotRadius_) + 2);
    const int w = displayArea_.getWidth();
    if (w > 0)
        responseMag_.assign(w, 0.0f);

    // The analyser fills the display area exactly, and draws with no inset of its
    // own, so its log x-axis is the same map freqToX() uses.
    spectrum_.setBounds(displayArea_);
    curveOverlay_.setBounds(getLocalBounds());

    recomputeResponse();
}

//==============================================================================
// Coordinate helpers

float FinalEQComponent::freqToX(float freq) const
{
    const float logMin = std::log10(freqMin_);
    const float logMax = std::log10(freqMax_);
    const float t = (std::log10(juce::jlimit(freqMin_, freqMax_, freq)) - logMin) / (logMax - logMin);
    return static_cast<float>(displayArea_.getX()) + t * static_cast<float>(displayArea_.getWidth());
}

float FinalEQComponent::gainToY(float gainDb) const
{
    const float t = 0.5f - juce::jlimit(-gainRange_, gainRange_, gainDb) / (2.0f * gainRange_);
    return static_cast<float>(displayArea_.getY()) + t * static_cast<float>(displayArea_.getHeight());
}

float FinalEQComponent::xToFreq(float x) const
{
    const float logMin = std::log10(freqMin_);
    const float logMax = std::log10(freqMax_);
    const float t = (x - static_cast<float>(displayArea_.getX()))
                    / static_cast<float>(juce::jmax(1, displayArea_.getWidth()));
    return std::pow(10.0f, logMin + juce::jlimit(0.0f, 1.0f, t) * (logMax - logMin));
}

float FinalEQComponent::yToGain(float y) const
{
    const float t = (y - static_cast<float>(displayArea_.getY()))
                    / static_cast<float>(juce::jmax(1, displayArea_.getHeight()));
    return gainRange_ * (1.0f - 2.0f * juce::jlimit(0.0f, 1.0f, t));
}

juce::Point<float> FinalEQComponent::getBandDotPos(int band) const
{
    // A cut has no gain, so its dot rides the 0 dB line: dragging it up and down
    // would otherwise move a number that changes nothing you can hear.
    const float gain = SpaceDustFinalEQ::typeUsesGain(getBandType(band)) ? cachedGain_[band] : 0.0f;
    return { freqToX(cachedFreq_[band]), gainToY(gain) };
}

int FinalEQComponent::getBandNearPos(juce::Point<float> pos) const
{
    int    closest     = -1;
    float  closestDist = dotHitRadius_ * dotHitRadius_;

    for (int b = 0; b < numBands; ++b)
    {
        const float d = getBandDotPos(b).getDistanceSquaredFrom(pos);
        if (d < closestDist)
        {
            closestDist = d;
            closest     = b;
        }
    }
    return closest;
}

//==============================================================================
// Response computation

void FinalEQComponent::recomputeResponse()
{
    const int w = static_cast<int>(responseMag_.size());
    if (w <= 0 || sampleRate_ <= 0.0)
        return;

    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    // Build one set of coefficients per band (message thread – allocation is fine).
    // These mirror SpaceDustFinalEQ::updateCoefficients() exactly, so the drawn
    // curve is the filter the audio actually goes through.
    juce::ReferenceCountedObjectPtr<Coeffs> coeffs[numBands];
    for (int b = 0; b < numBands; ++b)
    {
        const float freq = juce::jlimit(freqMin_, freqMax_, cachedFreq_[b]);
        const float gain = juce::jlimit(-gainRange_, gainRange_, cachedGain_[b]);
        const float q    = juce::jlimit(0.1f, 10.0f, cachedQ_[b]);
        const float A    = std::pow(10.0f, gain / 20.0f);

        switch (getBandType(b))
        {
            case BandType::LowShelf:
                coeffs[b] = Coeffs::makeLowShelf(sampleRate_, freq, q, A);
                break;
            case BandType::HighShelf:
                coeffs[b] = Coeffs::makeHighShelf(sampleRate_, freq, q, A);
                break;
            case BandType::LowPass:
                coeffs[b] = Coeffs::makeLowPass(sampleRate_, freq, q);
                break;
            case BandType::HighPass:
                coeffs[b] = Coeffs::makeHighPass(sampleRate_, freq, q);
                break;
            case BandType::Bell:
            default:
                coeffs[b] = Coeffs::makePeakFilter(sampleRate_, freq, q, A);
                break;
        }
    }

    const float logMin = std::log10(freqMin_);
    const float logMax = std::log10(freqMax_);

    for (int i = 0; i < w; ++i)
    {
        const float t    = static_cast<float>(i) / static_cast<float>(juce::jmax(1, w - 1));
        const double testFreq = std::pow(10.0, static_cast<double>(logMin + t * (logMax - logMin)));

        float totalDb = 0.0f;
        for (int b = 0; b < numBands; ++b)
        {
            if (coeffs[b] != nullptr)
            {
                const double mag = coeffs[b]->getMagnitudeForFrequency(testFreq, sampleRate_);
                if (mag > 0.0)
                    totalDb += juce::Decibels::gainToDecibels(static_cast<float>(mag));
            }
        }
        responseMag_[i] = totalDb;
    }

    curveOverlay_.repaint();
}

//==============================================================================
// Painting
//
// Split in two: the panel itself here, the curve and dots in paintCurve(), which
// the overlay child calls. The spectrum analyser is a child between them, so it
// covers this background and is covered by that foreground.

void FinalEQComponent::paint(juce::Graphics& g)
{
    if (displayArea_.isEmpty())
        return;

    const auto da = displayArea_.toFloat();

    g.setColour(juce::Colour(0xff0b0b1e));
    g.fillRoundedRectangle(da, 5.0f);
}

void FinalEQComponent::paintCurve(juce::Graphics& g)
{
    if (displayArea_.isEmpty())
        return;

    const auto da = displayArea_.toFloat();

    // Knob colour constants (matching SpaceDustLookAndFeel; arc/glow follow meter red zone)
    const juce::Colour knobBodyLight (0xff2a2a48);
    const juce::Colour knobBodyDark  (0xff1a1a30);
    juce::Colour knobArcCol = juce::Colour(0xff00d4ff);
    juce::Colour knobGlowCol = juce::Colour(0xff00b4ff);
    if (auto* laf = dynamic_cast<SpaceDustLookAndFeel*>(&getLookAndFeel()))
    {
        knobArcCol = laf->getMeterResponsiveKnobArcColour();
        knobGlowCol = laf->getMeterResponsiveKnobGlowColour();
    }

    // --- Border (over the spectrum, so the panel keeps a clean edge) ---
    g.setColour(juce::Colour(0xff2a2a55));
    g.drawRoundedRectangle(da, 5.0f, 1.0f);

    // --- 0 dB centre line only (thin, subtle) ---
    {
        const float zeroY = gainToY(0.0f);
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.drawHorizontalLine(static_cast<int>(zeroY), da.getX() + 4.0f, da.getRight() - 4.0f);
    }

    // --- Frequency response curve ---
    const int w = static_cast<int>(responseMag_.size());
    if (w > 1)
    {
        const float x0    = static_cast<float>(displayArea_.getX());
        const float zeroY = gainToY(0.0f);

        // Filled area between curve and 0 dB line
        juce::Path filled;
        filled.startNewSubPath(x0, zeroY);
        for (int i = 0; i < w; ++i)
            filled.lineTo(x0 + static_cast<float>(i), gainToY(responseMag_[i]));
        filled.lineTo(x0 + static_cast<float>(w - 1), zeroY);
        filled.closeSubPath();

        g.setColour(knobGlowCol.withAlpha(0.12f));
        g.fillPath(filled);

        // Stroke
        juce::Path stroke;
        stroke.startNewSubPath(x0, gainToY(responseMag_[0]));
        for (int i = 1; i < w; ++i)
            stroke.lineTo(x0 + static_cast<float>(i), gainToY(responseMag_[i]));

        // Blooms with the rest of the UI, by the shared meter-driven law.
        if (auto* sdLnf = dynamic_cast<SpaceDustLookAndFeel*>(&getLookAndFeel()))
            sdLnf->glowPath(g, stroke, knobArcCol, 1.8f);

        g.setColour(knobArcCol.withAlpha(0.9f));
        g.strokePath(stroke, juce::PathStrokeType(1.8f,
                                                   juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }

    // --- Band dots (styled like mini knobs) ---
    for (int b = 0; b < numBands; ++b)
    {
        const auto dot = getBandDotPos(b);
        const bool isSelected = (b == selectedBand_);
        const float r = isSelected ? dotRadius_ + 1.5f : dotRadius_;

        // Outer glow halo (matches knob glow). The selected band wears a stronger
        // one -- it is the band the knobs beside the display are editing.
        {
            const float glowR = r + (isSelected ? 6.0f : 4.0f);
            juce::ColourGradient glow(knobGlowCol.withAlpha(isSelected ? 0.45f : 0.25f), dot.x, dot.y,
                                      knobGlowCol.withAlpha(0.0f),  dot.x, dot.y - glowR, true);
            g.setGradientFill(glow);
            g.fillEllipse(dot.x - glowR, dot.y - glowR, glowR * 2.0f, glowR * 2.0f);
        }

        // Body gradient (matches knob body)
        {
            juce::ColourGradient body(knobBodyLight, dot.x, dot.y - r * 0.35f,
                                      knobBodyDark,  dot.x, dot.y + r * 0.8f, false);
            g.setGradientFill(body);
            g.fillEllipse(dot.x - r, dot.y - r, r * 2.0f, r * 2.0f);
        }

        // Rim (matches knob arc colour)
        g.setColour(isSelected ? knobArcCol : knobArcCol.withAlpha(0.65f));
        g.drawEllipse(dot.x - r, dot.y - r, r * 2.0f, r * 2.0f, isSelected ? 2.0f : 1.5f);

        // Band number label
        g.setColour(knobArcCol.withAlpha(isSelected ? 1.0f : 0.85f));
        g.setFont(juce::Font(8.0f, juce::Font::bold));
        g.drawText(juce::String(b + 1),
                   static_cast<int>(dot.x - 5.0f), static_cast<int>(dot.y - 5.0f),
                   10, 10, juce::Justification::centred, false);
    }
}

//==============================================================================
// Mouse handling

void FinalEQComponent::mouseDown(const juce::MouseEvent& e)
{
    const auto pos = e.position;
    draggedBand_ = getBandNearPos(pos);

    if (draggedBand_ >= 0)
    {
        dragStartFreq_ = cachedFreq_[draggedBand_];
        dragStartGain_ = cachedGain_[draggedBand_];
        dragStartPos_  = pos;

        // Clicking a dot is also how you choose which band the Node dropdown and
        // the Quality / Frequency / Gain knobs are pointed at.
        setSelectedBand(draggedBand_);
        if (onBandSelected)
            onBandSelected(draggedBand_);

        // Open a balanced host-automation gesture for the params this drag will move.
        // mouseDrag streams setValueNotifyingHost (performEdit) calls; without a
        // surrounding begin/endChangeGesture the host receives naked, unbalanced edits.
        // FL Studio's "Last Tweaked" tracking relies on balanced gestures to know which
        // parameter the user touched — naked edits corrupt it, so automation lanes
        // created for any later-tweaked parameter latch onto the wrong target.
        if (auto* fp = apvts_.getParameter(freqId(draggedBand_))) fp->beginChangeGesture();
        if (auto* gp = apvts_.getParameter(gainId(draggedBand_))) gp->beginChangeGesture();
    }
}

void FinalEQComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (draggedBand_ < 0)
        return;

    setParam(freqId(draggedBand_), juce::jlimit(freqMin_, freqMax_, xToFreq(e.position.x)));

    // Vertical drag is gain, and a cut has none -- for those the dot slides along
    // the 0 dB line instead of silently writing a gain nothing applies.
    if (SpaceDustFinalEQ::typeUsesGain(getBandType(draggedBand_)))
        setParam(gainId(draggedBand_), juce::jlimit(-gainRange_, gainRange_, yToGain(e.position.y)));
}

void FinalEQComponent::mouseUp(const juce::MouseEvent&)
{
    // Close the gestures opened in mouseDown (balanced begin/end per drag).
    if (draggedBand_ >= 0)
    {
        if (auto* fp = apvts_.getParameter(freqId(draggedBand_))) fp->endChangeGesture();
        if (auto* gp = apvts_.getParameter(gainId(draggedBand_))) gp->endChangeGesture();
    }
    draggedBand_ = -1;
}

void FinalEQComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    const int b = getBandNearPos(e.position);

    if (b < 0)
        return;

    // Double-clicking a control to put it back to its default is the convention
    // the knobs already follow, so the dots follow it too.
    setSelectedBand(b);
    if (onBandSelected)
        onBandSelected(b);

    resetBand(b);
}

void FinalEQComponent::resetBand(int band)
{
    band = juce::jlimit(0, numBands - 1, band);

    for (const auto& id : { freqId(band), gainId(band), qId(band) })
    {
        if (auto* p = apvts_.getParameter(id))
        {
            // Balanced gesture per edit, as everywhere else this component writes
            // parameters, so hosts see one clean move rather than a naked write.
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->getDefaultValue());
            p->endChangeGesture();
        }
    }
}

void FinalEQComponent::mouseWheelMove(const juce::MouseEvent& e,
                                       const juce::MouseWheelDetails& wheel)
{
    const int b = getBandNearPos(e.position);
    if (b < 0)
        return;

    // Q for a bell is its width, for a shelf its slope, for a cut its corner
    // resonance -- every shape uses it, so the wheel always does something.
    const float delta = wheel.deltaY * 0.5f;  // sensitivity
    const float newQ  = juce::jlimit(0.1f, 10.0f, cachedQ_[b] + delta);

    // Balanced gesture around the single edit (see mouseDown for why this matters).
    if (auto* qp = apvts_.getParameter(qId(b)))
    {
        qp->beginChangeGesture();
        setParam(qId(b), newQ);
        qp->endChangeGesture();
    }
}

//==============================================================================
// Parameter listener

void FinalEQComponent::parameterChanged(const juce::String& /*parameterID*/, float /*newValue*/)
{
    // CRITICAL: APVTS delivers parameterChanged on the AUDIO / automation thread (e.g.
    // FL Studio streaming parameter automation), NOT the message thread. The old code
    // called recomputeResponse() directly here, which calls repaint() — and
    // Component::repaint() / juce::RectangleList are message-thread-ONLY. Touching them
    // from the audio thread corrupts the Component's internal repaint RectangleList; a
    // later RectangleList::add() then writes past the array → heap corruption (0xC0000374
    // in FL; pinned to juce::RectangleList<int>::add via a page-heap dump while a user
    // automated the EQ heavily). Every other parameterChanged in this codebase already
    // marshals UI work to the message thread — this one was the outlier.
    //
    // Coalesce to the message thread. AsyncUpdater merges a burst of automation into a
    // single handleAsyncUpdate() call, so heavy EQ automation no longer floods anything.
    triggerAsyncUpdate();
}

//==============================================================================
void FinalEQComponent::handleAsyncUpdate()
{
    // Message thread. Re-read every band's live value from the APVTS (we coalesced, so
    // we don't track which single param changed), then recompute + repaint here where
    // touching the Component is safe.
    for (int b = 0; b < numBands; ++b)
    {
        if (auto* p = apvts_.getRawParameterValue(freqId(b))) cachedFreq_[b] = p->load();
        if (auto* p = apvts_.getRawParameterValue(gainId(b))) cachedGain_[b] = p->load();
        if (auto* p = apvts_.getRawParameterValue(qId(b)))    cachedQ_[b]    = p->load();
        if (auto* p = apvts_.getRawParameterValue(typeId(b))) cachedType_[b] = static_cast<int>(p->load());
    }
    recomputeResponse();   // ends with a repaint — safe on the message thread
}

//==============================================================================
// Helper: set a parameter value via normalised host notification

void FinalEQComponent::setParam(const juce::String& paramId, float value)
{
    if (auto* p = apvts_.getParameter(paramId))
        p->setValueNotifyingHost(p->getNormalisableRange().convertTo0to1(value));
}
