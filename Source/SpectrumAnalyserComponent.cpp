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
    // Low floor keeps bars tall/legible; generous headroom above 0 dB means loud
    // peaks render their natural pointed shape instead of clamping into a plateau.
    const float minDb = -90.0f;
    const float maxDb = 9.0f;
    // Modest lift only - a big +18 dB boost slammed everything into the ceiling.
    const float displayGainDb = 9.0f;
    // Heavy temporal averaging so a held note reads as a steady bar. Low coeffs
    // average out the frame-to-frame leakage wobble; we still rise a touch faster
    // than we fall so transients stay lively.
    const float attackCoeff  = 0.22f;
    const float releaseCoeff = 0.12f;

    if (nextFFTBlockReady)
    {
        for (int i = 1; i < numMagnitudes; ++i)
        {
            float magnitude = fftData[i];
            float magnitudeDb = magnitude > 0.0f
                ? juce::jlimit(minDb, maxDb, 20.0f * std::log10(magnitude / static_cast<float>(numMagnitudes) + 1e-6f) + displayGainDb)
                : minDb;
            float prev  = displayMagnitudes[static_cast<size_t>(i)];
            float coeff = (magnitudeDb > prev) ? attackCoeff : releaseCoeff;
            displayMagnitudes[static_cast<size_t>(i)] = prev + (magnitudeDb - prev) * coeff;
        }
        nextFFTBlockReady = false;
    }
    // else: hold the last smoothed values (no per-paint decay -> no jitter)

    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());
    const float pad = 8.0f;
    const float drawW = w - 2.0f * pad;
    const float drawH = h - 2.0f * pad;

    // Log-frequency display (SPAN-style): the x-axis runs from minFreq..maxFreq
    // geometrically, so every octave occupies equal width and harmonics fan out
    // evenly instead of crushing into the left edge. We keep the thin vertical
    // "lines that make it up" look by drawing many narrow columns across the width.
    const float topBin    = static_cast<float>(numMagnitudes - 1);
    const float hzPerBin  = sampleRate / static_cast<float>(fftSize);
    const float binsPerHz = 1.0f / hzPerBin;
    const float nyquist   = sampleRate * 0.5f;
    const float hiFreq    = juce::jmin(maxFreq, nyquist);
    const float logRatio  = std::log(hiFreq / minFreq);

    // Aim for ~3px-wide columns so the bars stay crisp and line-like at any size.
    const int numColumns = juce::jlimit(64, 512, static_cast<int>(drawW / 3.0f));
    const float colWidth = drawW / static_cast<float>(numColumns);

    // Scale drawn heights so peaks sit below the top instead of running off it.
    const float barHeightScale = 0.80f;

    // Sample the (already-smoothed) magnitude spectrum at a fractional bin,
    // taking the peak across a bin span so high-frequency harmonics stay sharp
    // while the low end interpolates into SPAN's smooth, wide skirt.
    auto sampleDb = [&](float binLo, float binHi) -> float
    {
        int lo = juce::jmax(1, static_cast<int>(std::floor(binLo)));
        int hi = juce::jmin(numMagnitudes - 1, static_cast<int>(std::ceil(binHi)));

        if (binHi - binLo < 1.0f)
        {
            // Sub-bin resolution at the low end: linearly interpolate.
            float center = 0.5f * (binLo + binHi);
            int   b0     = juce::jlimit(1, numMagnitudes - 2, static_cast<int>(std::floor(center)));
            float frac   = juce::jlimit(0.0f, 1.0f, center - static_cast<float>(b0));
            float a = displayMagnitudes[static_cast<size_t>(b0)];
            float b = displayMagnitudes[static_cast<size_t>(b0 + 1)];
            return a + (b - a) * frac;
        }

        float peak = minDb;
        for (int bin = lo; bin <= hi; ++bin)
            peak = juce::jmax(peak, displayMagnitudes[static_cast<size_t>(bin)]);
        return peak;
    };

    for (int c = 0; c < numColumns; ++c)
    {
        float t0 = static_cast<float>(c)       / static_cast<float>(numColumns);
        float t1 = static_cast<float>(c + 1)   / static_cast<float>(numColumns);
        float f0 = minFreq * std::exp(logRatio * t0);
        float f1 = minFreq * std::exp(logRatio * t1);

        float binLo = juce::jmin(topBin, f0 * binsPerHz);
        float binHi = juce::jmin(topBin, f1 * binsPerHz);

        float magnitudeDb = sampleDb(binLo, binHi);
        float barHeight = juce::jmap(magnitudeDb, minDb, maxDb, 0.0f, drawH) * barHeightScale;
        if (barHeight < 0.5f)
            continue;

        float x = pad + static_cast<float>(c) * colWidth;
        float y = pad + drawH - barHeight;
        // Leave a hairline gap between columns so the individual lines read.
        float drawWidth = juce::jmax(1.0f, colWidth - 1.0f);

        g.setColour(fillColour);
        g.fillRect(x, y, drawWidth, barHeight);
        // Brighter cap on top for that crisp, futuristic edge.
        g.setColour(lineColour);
        g.fillRect(x, y, drawWidth, juce::jmin(1.5f, barHeight));
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
void SpectrumAnalyserComponent::computeSpectrum(const float* samples)
{
    // `samples` already holds a contiguous, gap-free window of the most-recent
    // fftSize output samples, so the magnitude spectrum of a steady note is
    // identical frame to frame -> a stable display.
    for (int j = 0; j < fftSize; ++j)
        fftData[static_cast<size_t>(j)] = samples[j];
    window->multiplyWithWindowingTable(fftData.data(), fftSize);
    for (int j = fftSize; j < fftSize * 2; ++j)
        fftData[static_cast<size_t>(j)] = 0.0f;
    forwardFFT->performFrequencyOnlyForwardTransform(fftData.data(), true);
    nextFFTBlockReady = true;
}

//==============================================================================
void SpectrumAnalyserComponent::timerCallback()
{
    // Self-driven at 60 fps, but do no work unless we're the visible tab.
    if (!isShowing() || ! fillSamplesCallback)
        return;

    fillSamplesCallback(fifo.data(), fftSize);  // contiguous most-recent fftSize samples
    computeSpectrum(fifo.data());
    repaint();
}
