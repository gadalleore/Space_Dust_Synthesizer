#include "OscilloscopeComponent.h"

//==============================================================================
void OscilloscopeComponent::paint(juce::Graphics& g)
{
    drawBackground(g);

    const int numSamples = internalBuffer.getNumSamples();
    const int numCh = internalBuffer.getNumChannels();
    if (numSamples < 2 || numCh < 1)
        return;

    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());
    const float cy = h * 0.5f;
    const float halfH = (h - 20.0f) * 0.5f;

    // Draw L channel (top half) and R channel (bottom half), or mono
    for (int ch = 0; ch < juce::jmin(2, numCh); ++ch)
    {
        float yBase = (ch == 0) ? (cy - halfH * 0.5f) : (cy + halfH * 0.5f);
        float yScale = halfH * 0.4f;

        g.setColour(traceColour);
        juce::Path path;
        bool started = false;
        for (int i = 0; i < numSamples; ++i)
        {
            float sample = internalBuffer.getSample(ch, i);
            float x = juce::jmap(static_cast<float>(i), 0.0f, static_cast<float>(numSamples - 1), 10.0f, w - 10.0f);
            float y = yBase - sample * yScale;
            if (!started)
            {
                path.startNewSubPath(x, y);
                started = true;
            }
            else
                path.lineTo(x, y);
        }
        if (started)
            g.strokePath(path, juce::PathStrokeType(2.5f));
    }
}

//==============================================================================
void OscilloscopeComponent::resized()
{
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
    const int numS = (validSamples > 0) ? juce::jmin(validSamples, buffer.getNumSamples())
                                        : buffer.getNumSamples();
    if (numCh < 1 || numS <= 0)
        return;

    if (internalBuffer.getNumChannels() != numCh || internalBuffer.getNumSamples() != numS)
        internalBuffer.setSize(numCh, numS, false, true, true);
    internalBuffer.clear();
    for (int ch = 0; ch < numCh; ++ch)
        internalBuffer.copyFrom(ch, 0, buffer, ch, 0, numS);
}
