#include "Goniometer.h"

//==============================================================================
void Goniometer::paint(juce::Graphics& g)
{
    const int cw = getWidth();
    const int ch = getHeight();
    if (cw <= 0 || ch <= 0)
        return;

    drawBackground(g);
    p.clear();

    const int internalBufferSize = internalBuffer.getNumSamples();
    if (internalBufferSize == 0 || internalBuffer.getNumChannels() < 2)
        return;

    int dim = juce::jmin(cw, ch);
    int margin = juce::jmin(16, dim / 6);
    float halfDim = juce::jmax(8.0f, (dim - 2 * margin) * 0.5f);
    float cx = cw * 0.5f;
    float cy = ch * 0.5f;
    float left   = cx - halfDim;
    float right  = cx + halfDim;
    float bottom = cy + halfDim;
    float top    = cy - halfDim;

    // Mid/Side -> XY (Side on X, Mid on Y)
    const float coefficient = juce::Decibels::decibelsToGain(0.0f + juce::Decibels::gainToDecibels(scale));
    const float maxGain = juce::Decibels::decibelsToGain(GONIO_MAX_DECIBELS);
    const float minGain = juce::Decibels::decibelsToGain(GONIO_NEGATIVE_INFINITY);

    for (int i = 0; i < internalBufferSize; ++i)
    {
        float leftRaw  = internalBuffer.getSample(0, i);
        float rightRaw = internalBuffer.getSample(1, i);
        float S = (leftRaw - rightRaw) * coefficient;   // Side
        float M = (leftRaw + rightRaw) * coefficient;   // Mid

        float xCoord = juce::jmap(S, minGain, maxGain, left, right);
        float yCoord = juce::jmap(M, minGain, maxGain, bottom, top);  // M pos = top
        xCoord = juce::jlimit(left, right, xCoord);
        yCoord = juce::jlimit(top, bottom, yCoord);

        juce::Point<float> pt{ xCoord, yCoord };
        if (i == 0)
            p.startNewSubPath(pt);
        else if (std::isfinite(pt.x) && std::isfinite(pt.y))
            p.lineTo(pt);
    }

    if (!p.isEmpty())
    {
        p.applyTransform(juce::AffineTransform::verticalFlip(ch * 0.5f));
        g.setColour(pathColourOutside);
        g.strokePath(p, juce::PathStrokeType(2.5f));
    }
}

//==============================================================================
void Goniometer::resized()
{
    const int cw = juce::jmax(1, getWidth());
    const int ch = juce::jmax(1, getHeight());
    int dim = juce::jmin(cw, ch);
    int margin = juce::jmin(16, dim / 6);
    w = h = juce::jmax(16, dim - 2 * margin);
    center = juce::Point<int>(cw / 2, ch / 2);
}

//==============================================================================
void Goniometer::drawBackground(juce::Graphics& g)
{
    const int cw = getWidth();
    const int ch = getHeight();
    if (cw <= 0 || ch <= 0)
        return;
    int dim = juce::jmin(cw, ch);
    int margin = juce::jmin(16, dim / 6);
    int drawW = juce::jmax(16, dim - 2 * margin);
    int drawH = drawW;
    float cx = cw * 0.5f;
    float cy = ch * 0.5f;
    float halfW = drawW * 0.5f;
    float halfH = drawH * 0.5f;

    g.setColour(ellipseFillColour);
    g.fillEllipse(cx - halfW, cy - halfH, static_cast<float>(drawW), static_cast<float>(drawH));
    g.setColour(edgeColour);
    g.drawEllipse(cx - halfW, cy - halfH, static_cast<float>(drawW), static_cast<float>(drawH), 2.0f);

    // Radial lines and labels (L, R, M, +S, -S) - scale to fit
    const float radius = halfW * 0.85f;
    for (int i = 0; i < 8; ++i)
    {
        float angle = static_cast<float>(i) * juce::MathConstants<float>::pi / 4.0f + juce::MathConstants<float>::pi / 2.0f;
        juce::Point<float> endPoint(cx + radius * std::cos(angle), cy - radius * std::sin(angle));

        g.setColour(juce::Colours::grey.withAlpha(0.6f));
        g.drawLine(cx, cy, endPoint.x, endPoint.y, 1.0f);

        if (endPoint.y <= cy)
        {
            int labelIdx = (i == 0) ? 0 : i - 3;
            if (labelIdx >= 0 && labelIdx < 5)
            {
                int extraX = 0, extraY = 0;
                if (i == 6) { extraY = -15; }
                else if (i == 5) { extraX = -12; extraY = -15; }
                else if (i == 7) { extraX = 12;  extraY = -15; }
                else if (i == 4) { extraX = -10; extraY = -7; }
                else if (i == 0) { extraX = 10;  extraY = -7; }

                g.setColour(edgeColour.withAlpha(0.9f));
                g.drawText(chars[static_cast<size_t>(labelIdx)],
                           static_cast<int>(endPoint.x - 10 + extraX),
                           static_cast<int>(endPoint.y + extraY),
                           20, 10, juce::Justification::centredTop);
            }
        }
    }
}

//==============================================================================
void Goniometer::update(const juce::AudioBuffer<float>& buffer)
{
    const int numCh = juce::jmin(2, buffer.getNumChannels());
    const int numS  = buffer.getNumSamples();
    if (numCh < 2 || numS <= 0)
        return;

    if (internalBuffer.getNumChannels() != numCh || internalBuffer.getNumSamples() < numS)
        internalBuffer.setSize(numCh, numS, false, true, true);

    internalBuffer.clear();
    for (int ch = 0; ch < numCh; ++ch)
        internalBuffer.copyFrom(ch, 0, buffer, ch, 0, numS);

    if (numS < 256)
        internalBuffer.applyGain(juce::Decibels::decibelsToGain(-3.0f));
}

//==============================================================================
void Goniometer::updateCoeff(float new_db)
{
    scale = new_db;
}
