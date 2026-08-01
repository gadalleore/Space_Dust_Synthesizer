#include "OscilloscopeComponent.h"
#include "SpaceDustLookAndFeel.h"

//==============================================================================
// Builds the L/R trace across the full time window. Split out of paint() because
// update() builds it too -- the ghost history has to advance with the audio, not
// with however often the component happens to be repainted.
juce::Path OscilloscopeComponent::buildTrace() const
{
    juce::Path path;

    const int numSamples = historyBuffer.getNumSamples();
    const int numCh      = historyBuffer.getNumChannels();

    if (numSamples < 2 || numCh < 1 || getWidth() <= 0 || getHeight() <= 0)
        return path;

    const float w     = static_cast<float>(getWidth());
    const float h     = static_cast<float>(getHeight());
    const float cy    = h * 0.5f;
    const float halfH = (h - 20.0f) * 0.5f;

    // The whole window is valid: it is filled wholesale from the processor's
    // continuous history every update, not accumulated block by block here.
    const int available = numSamples;

    if (available < 2)
        return path;

    // One point per pixel column at most: 4096 samples into a few hundred px is far
    // more detail than can be shown, and stroking every one of them is wasted work.
    const int stride = juce::jmax(1, available / juce::jmax(1, static_cast<int>(w)));

    for (int ch = 0; ch < juce::jmin(2, numCh); ++ch)
    {
        const float yBase  = (ch == 0) ? (cy - halfH * 0.5f) : (cy + halfH * 0.5f);
        const float yScale = halfH * 0.4f;   // as it always was; 0.8 read as "zoomed in"

        bool started = false;

        for (int n = 0; n < available; n += stride)
        {
            // Oldest sample first, so the wave travels left to right the way a
            // scope's does. readSpectrumSamples already hands them over in order.
            const float sample = historyBuffer.getSample(ch, n);
            const float x = juce::jmap(static_cast<float>(n), 0.0f,
                                       static_cast<float>(available - 1), 10.0f, w - 10.0f);
            const float y = yBase - sample * yScale;

            if (! started)
            {
                path.startNewSubPath(x, y);
                started = true;
            }
            else
            {
                path.lineTo(x, y);
            }
        }
    }

    return path;
}

//==============================================================================
void OscilloscopeComponent::paint(juce::Graphics& g)
{
    drawBackground(g);

    // Older sweeps first: the RGB dither marking where the trace has just been.
    SpaceDustDither::ghostTrail(g, traceHistory, 2.5f * 1.4f, kTrailSpread, kTrailAlpha, *ditherTiles);

    const auto trace = buildTrace();

    if (! trace.isEmpty())
    {
        // Bloom, scaled by the meter. Asked from the inherited LookAndFeel rather
        // than pushed in, so this needs no wiring from the editor.
        if (auto* sdLnf = dynamic_cast<SpaceDustLookAndFeel*>(&getLookAndFeel()))
        {
            if (const float glow = sdLnf->getGlowAmount(); glow > 0.01f)
            {
                for (int pass = 0; pass < 2; ++pass)
                {
                    g.setColour(traceColour.withAlpha(glow * (pass == 0 ? 0.14f : 0.24f)));
                    g.strokePath(trace, juce::PathStrokeType(2.5f * (pass == 0 ? 4.0f : 2.2f)));
                }
            }
        }

        g.setColour(traceColour);
        g.strokePath(trace, juce::PathStrokeType(2.5f));
    }
}

//==============================================================================
void OscilloscopeComponent::resized()
{
    // Every stored path was built against the old size, so they would ghost in the
    // wrong places. Cheaper and more honest to drop them and let the trail rebuild.
    traceHistory.clear();
}

//==============================================================================
void OscilloscopeComponent::drawBackground(juce::Graphics& g)
{
    g.fillAll(bgColour);
}

//==============================================================================
void OscilloscopeComponent::update(const juce::AudioBuffer<float>& buffer, int validSamples)
{
    juce::ignoreUnused(buffer, validSamples);

    // Stereo, gap-free: both channels come from the processor's continuous scope
    // history, so the long window is real audio rather than stitched blocks.
    // split the scope used to show, and it is the right trade -- a gap-free mono
    // waveform is a truer picture of what is coming out than two stereo traces
    // stitched from non-adjacent blocks.
    if (fillSamplesCallback == nullptr)
        return;

    if (historyBuffer.getNumChannels() != 2 || historyBuffer.getNumSamples() != kWindowSamples)
    {
        historyBuffer.setSize(2, kWindowSamples, false, true, true);
        historyBuffer.clear();
        traceHistory.clear();
    }

    fillSamplesCallback(historyBuffer.getWritePointer(0),
                        historyBuffer.getWritePointer(1), kWindowSamples);

    // Advance the ghost trail one frame, in step with the audio.
    traceHistory.push_back(buildTrace());

    while (static_cast<int>(traceHistory.size()) > kHistoryLength)
        traceHistory.erase(traceHistory.begin());
}
