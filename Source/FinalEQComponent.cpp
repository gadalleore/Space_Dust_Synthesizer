#include "FinalEQComponent.h"

//==============================================================================
// Static helpers

juce::String FinalEQComponent::freqId(int band) { return "finalEQB" + juce::String(band + 1) + "Freq"; }
juce::String FinalEQComponent::gainId(int band) { return "finalEQB" + juce::String(band + 1) + "Gain"; }
juce::String FinalEQComponent::qId  (int band) { return "finalEQB" + juce::String(band + 1) + "Q";    }

FinalEQComponent::BandType FinalEQComponent::getBandType(int band)
{
    if (band == 0) return BandType::LowShelf;
    if (band == 4) return BandType::HighShelf;
    return BandType::Peak;
}

//==============================================================================
FinalEQComponent::FinalEQComponent(juce::AudioProcessorValueTreeState& apvts, double sampleRate)
    : apvts_(apvts), sampleRate_(sampleRate > 0.0 ? sampleRate : 44100.0)
{
    // Seed cached values from APVTS
    const float defaultFreqs[5] = { 80.0f, 250.0f, 1000.0f, 4000.0f, 10000.0f };
    const float defaultQs[5]    = { 0.707f, 1.0f, 1.0f, 1.0f, 0.707f };

    for (int b = 0; b < numBands; ++b)
    {
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

        apvts_.addParameterListener(freqId(b), this);
        apvts_.addParameterListener(gainId(b), this);
        apvts_.addParameterListener(qId(b),    this);
    }

    setOpaque(false);
}

FinalEQComponent::~FinalEQComponent()
{
    for (int b = 0; b < numBands; ++b)
    {
        apvts_.removeParameterListener(freqId(b), this);
        apvts_.removeParameterListener(gainId(b), this);
        apvts_.removeParameterListener(qId(b),    this);
    }
}

//==============================================================================
void FinalEQComponent::setSampleRate(double sr)
{
    sampleRate_ = (sr > 0.0) ? sr : 44100.0;
    recomputeResponse();
}

//==============================================================================
void FinalEQComponent::resized()
{
    // Leave a small inset so dots near the edges are fully visible
    displayArea_ = getLocalBounds().reduced(static_cast<int>(dotRadius_) + 2);
    const int w = displayArea_.getWidth();
    if (w > 0)
        responseMag_.assign(w, 0.0f);
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
    return { freqToX(cachedFreq_[band]), gainToY(cachedGain_[band]) };
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

    // Build one set of coefficients per band (message thread – allocation is fine)
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
            case BandType::Peak:
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

    repaint();
}

//==============================================================================
// Painting

void FinalEQComponent::paint(juce::Graphics& g)
{
    if (displayArea_.isEmpty())
        return;

    const auto da = displayArea_.toFloat();

    // Knob colour constants (matching SpaceDustLookAndFeel)
    const juce::Colour knobBodyLight (0xff2a2a48);
    const juce::Colour knobBodyDark  (0xff1a1a30);
    const juce::Colour knobArcCyan   (0xff00d4ff);
    const juce::Colour knobGlowCyan  (0xff00b4ff);

    // --- Background ---
    g.setColour(juce::Colour(0xff0b0b1e));
    g.fillRoundedRectangle(da, 5.0f);

    // --- Border ---
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

        g.setColour(knobGlowCyan.withAlpha(0.12f));
        g.fillPath(filled);

        // Stroke
        juce::Path stroke;
        stroke.startNewSubPath(x0, gainToY(responseMag_[0]));
        for (int i = 1; i < w; ++i)
            stroke.lineTo(x0 + static_cast<float>(i), gainToY(responseMag_[i]));

        g.setColour(knobArcCyan.withAlpha(0.9f));
        g.strokePath(stroke, juce::PathStrokeType(1.8f,
                                                   juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }

    // --- Band dots (styled like mini knobs) ---
    for (int b = 0; b < numBands; ++b)
    {
        const auto dot = getBandDotPos(b);

        // Outer glow halo (matches knob glow)
        {
            const float glowR = dotRadius_ + 4.0f;
            juce::ColourGradient glow(knobGlowCyan.withAlpha(0.25f), dot.x, dot.y,
                                      knobGlowCyan.withAlpha(0.0f),  dot.x, dot.y - glowR, true);
            g.setGradientFill(glow);
            g.fillEllipse(dot.x - glowR, dot.y - glowR, glowR * 2.0f, glowR * 2.0f);
        }

        // Body gradient (matches knob body)
        {
            juce::ColourGradient body(knobBodyLight, dot.x, dot.y - dotRadius_ * 0.35f,
                                      knobBodyDark,  dot.x, dot.y + dotRadius_ * 0.8f, false);
            g.setGradientFill(body);
            g.fillEllipse(dot.x - dotRadius_, dot.y - dotRadius_,
                          dotRadius_ * 2.0f, dotRadius_ * 2.0f);
        }

        // Cyan rim (matches knob arc colour)
        g.setColour(knobArcCyan);
        g.drawEllipse(dot.x - dotRadius_, dot.y - dotRadius_,
                      dotRadius_ * 2.0f, dotRadius_ * 2.0f, 1.5f);

        // Band number label
        g.setColour(knobArcCyan.withAlpha(0.85f));
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
    }
}

void FinalEQComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (draggedBand_ < 0)
        return;

    const float newFreq = xToFreq(e.position.x);
    const float newGain = yToGain(e.position.y);

    setParam(freqId(draggedBand_), juce::jlimit(freqMin_, freqMax_, newFreq));
    setParam(gainId(draggedBand_), juce::jlimit(-gainRange_, gainRange_, newGain));
}

void FinalEQComponent::mouseUp(const juce::MouseEvent&)
{
    draggedBand_ = -1;
}

void FinalEQComponent::mouseWheelMove(const juce::MouseEvent& e,
                                       const juce::MouseWheelDetails& wheel)
{
    const int b = getBandNearPos(e.position);
    if (b < 0)
        return;

    // Q only adjustable for peak bands; shelf slope via Q still works visually
    const float delta = wheel.deltaY * 0.5f;  // sensitivity
    const float newQ  = juce::jlimit(0.1f, 10.0f, cachedQ_[b] + delta);
    setParam(qId(b), newQ);
}

//==============================================================================
// Parameter listener

void FinalEQComponent::parameterChanged(const juce::String& parameterID, float newValue)
{
    // Runs on the message thread
    for (int b = 0; b < numBands; ++b)
    {
        if (parameterID == freqId(b)) { cachedFreq_[b] = newValue; recomputeResponse(); return; }
        if (parameterID == gainId(b)) { cachedGain_[b] = newValue; recomputeResponse(); return; }
        if (parameterID == qId(b))   { cachedQ_[b]    = newValue; recomputeResponse(); return; }
    }
}

//==============================================================================
// Helper: set a parameter value via normalised host notification

void FinalEQComponent::setParam(const juce::String& paramId, float value)
{
    if (auto* p = apvts_.getParameter(paramId))
        p->setValueNotifyingHost(p->getNormalisableRange().convertTo0to1(value));
}
