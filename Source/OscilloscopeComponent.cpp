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

    // Until the ring has filled once, only the part actually written holds audio;
    // drawing the rest would paint a flat line through silence that was never there.
    const int available = ringPrimed ? numSamples : writePos;

    if (available < 2)
        return path;

    // One point per pixel column at most: 4096 samples into a few hundred px is far
    // more detail than can be shown, and stroking every one of them is wasted work.
    const int stride = juce::jmax(1, available / juce::jmax(1, static_cast<int>(w)));

    for (int ch = 0; ch < juce::jmin(2, numCh); ++ch)
    {
        const float yBase  = (ch == 0) ? (cy - halfH * 0.5f) : (cy + halfH * 0.5f);
        const float yScale = halfH * 0.4f;

        bool started = false;

        for (int n = 0; n < available; n += stride)
        {
            // Oldest sample first, so the wave travels left to right the way a
            // scope's does. The ring's write head is the newest sample.
            const int idx = ringPrimed ? ((writePos + n) % numSamples) : n;

            const float sample = historyBuffer.getSample(ch, idx);
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
    SpaceDustDither::ghostTrail(g, traceHistory, 2.5f * 1.4f, kTrailSpread, kTrailAlpha);

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
    const int numCh = juce::jmin(2, buffer.getNumChannels());
    const int numS  = (validSamples > 0) ? juce::jmin(validSamples, buffer.getNumSamples())
                                         : buffer.getNumSamples();
    if (numCh < 1 || numS <= 0)
        return;

    // Allocate the ring once, or whenever the channel count changes.
    if (historyBuffer.getNumChannels() != numCh || historyBuffer.getNumSamples() != kWindowSamples)
    {
        historyBuffer.setSize(numCh, kWindowSamples, false, true, true);
        historyBuffer.clear();
        writePos   = 0;
        ringPrimed = false;
        traceHistory.clear();
    }

    // Append this block into the ring, wrapping as needed. A block longer than the
    // whole window keeps only its most recent tail.
    const int src0  = juce::jmax(0, numS - kWindowSamples);
    const int toCopy = numS - src0;

    for (int i = 0; i < toCopy; ++i)
    {
        for (int ch = 0; ch < numCh; ++ch)
            historyBuffer.setSample(ch, writePos, buffer.getSample(ch, src0 + i));

        if (++writePos >= kWindowSamples)
        {
            writePos   = 0;
            ringPrimed = true;
        }
    }

    // Advance the ghost trail one frame, in step with the audio.
    traceHistory.push_back(buildTrace());

    while (static_cast<int>(traceHistory.size()) > kHistoryLength)
        traceHistory.erase(traceHistory.begin());

    // Kept in sync for anything still reading it.
    if (internalBuffer.getNumChannels() != numCh || internalBuffer.getNumSamples() != numS)
        internalBuffer.setSize(numCh, numS, false, true, true);
    internalBuffer.clear();
    for (int ch = 0; ch < numCh; ++ch)
        internalBuffer.copyFrom(ch, 0, buffer, ch, 0, numS);
}
