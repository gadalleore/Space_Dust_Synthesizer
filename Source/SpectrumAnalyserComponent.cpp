#include "SpectrumAnalyserComponent.h"

//==============================================================================
SpectrumAnalyserComponent::SpectrumAnalyserComponent()
{
    forwardFFT = std::make_unique<juce::dsp::FFT>(fftOrder);
    window = std::make_unique<juce::dsp::WindowingFunction<float>>(fftSize, juce::dsp::WindowingFunction<float>::hann);
    fftData.resize(fftSize * 2);
    fifo.resize(fftSize);
    displayMagnitudes.resize(fftSize / 2 + 1);
    std::fill(fftData.begin(), fftData.end(), 0.0f);
    std::fill(fifo.begin(), fifo.end(), 0.0f);
    std::fill(displayMagnitudes.begin(), displayMagnitudes.end(), -60.0f);
}

//==============================================================================
void SpectrumAnalyserComponent::paint(juce::Graphics& g)
{
    drawBackground(g);

    const int numMagnitudes = fftSize / 2 + 1;
    const float minDb = -60.0f;
    const float maxDb = 0.0f;
    // Snappier response: faster attack, faster decay
    const float decay = 0.91f;
    const float attack = 0.42f;

    if (nextFFTBlockReady)
    {
        for (int i = 1; i < numMagnitudes; ++i)
        {
            float magnitude = fftData[i];
            float magnitudeDb = magnitude > 0.0f
                ? juce::jlimit(minDb, maxDb, 20.0f * std::log10(magnitude / static_cast<float>(numMagnitudes) + 1e-6f))
                : minDb;
            float prev = displayMagnitudes[static_cast<size_t>(i)];
            displayMagnitudes[static_cast<size_t>(i)] = prev * (1.0f - attack) + magnitudeDb * attack;
        }
        nextFFTBlockReady = false;
    }
    else
    {
        for (int i = 1; i < numMagnitudes; ++i)
        {
            float p = displayMagnitudes[static_cast<size_t>(i)];
            displayMagnitudes[static_cast<size_t>(i)] = p * decay + minDb * (1.0f - decay);
        }
    }

    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());
    const float pad = 8.0f;
    const float drawW = w - 2.0f * pad;
    const float drawH = h - 2.0f * pad;

    // One bar per FFT bin (skip DC), uniform width, solid fill
    const int numBars = numMagnitudes - 1;  // bins 1..256 = 256 bars
    const float barWidth = drawW / static_cast<float>(numBars);

    for (int b = 0; b < numBars; ++b)
    {
        int bin = b + 1;  // skip DC (bin 0)
        float magnitudeDb = displayMagnitudes[static_cast<size_t>(bin)];
        float barHeight = juce::jmap(magnitudeDb, minDb, maxDb, 0.0f, drawH);
        float x = pad + static_cast<float>(b) * barWidth;
        float y = pad + drawH - barHeight;
        juce::Rectangle<float> bar(x, y, barWidth, barHeight);
        g.setColour(fillColour);
        g.fillRect(bar);
    }
}

//==============================================================================
void SpectrumAnalyserComponent::resized()
{
}

//==============================================================================
void SpectrumAnalyserComponent::drawBackground(juce::Graphics& g)
{
    g.fillAll(bgColour);
}

//==============================================================================
void SpectrumAnalyserComponent::pushNextBlock(const float* src, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        fifo[fifoIndex] = src[i];
        if (++fifoIndex >= fftSize)
        {
            fifoIndex = 0;
            for (int j = 0; j < fftSize; ++j)
                fftData[j] = fifo[j];
            window->multiplyWithWindowingTable(fftData.data(), fftSize);
            for (int j = fftSize; j < fftSize * 2; ++j)
                fftData[j] = 0.0f;
            forwardFFT->performFrequencyOnlyForwardTransform(fftData.data(), true);
            nextFFTBlockReady = true;
            break;
        }
    }
}

//==============================================================================
void SpectrumAnalyserComponent::update(const juce::AudioBuffer<float>& buffer)
{
    if (buffer.getNumChannels() < 1 || buffer.getNumSamples() <= 0)
        return;
    const float* src = buffer.getReadPointer(0);
    pushNextBlock(src, buffer.getNumSamples());
}
