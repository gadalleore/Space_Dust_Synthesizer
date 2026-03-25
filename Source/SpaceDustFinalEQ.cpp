#include "SpaceDustFinalEQ.h"

//==============================================================================
void SpaceDustFinalEQ::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;

    juce::dsp::ProcessSpec monoSpec = spec;
    monoSpec.numChannels = 1;

    for (int band = 0; band < numBands; ++band)
        for (int ch = 0; ch < maxChannels; ++ch)
            filters_[band][ch].prepare(monoSpec);

    updateCoefficients();
    reset();
}

void SpaceDustFinalEQ::reset()
{
    for (int band = 0; band < numBands; ++band)
        for (int ch = 0; ch < maxChannels; ++ch)
            filters_[band][ch].reset();
}

void SpaceDustFinalEQ::updateCoefficients()
{
    if (sampleRate_ <= 0.0)
        return;

    for (int i = 0; i < numBands; ++i)
    {
        const auto& b    = params_.bands[i];
        const float freq = juce::jlimit(20.0f, 20000.0f, b.freqHz);
        const float gain = juce::jlimit(-15.0f, 15.0f, b.gainDb);
        const float q    = juce::jlimit(0.1f, 10.0f, b.Q);
        const float A    = std::pow(10.0f, gain / 20.0f);

        juce::ReferenceCountedObjectPtr<Coeffs> newCoeffs;

        switch (b.type)
        {
            case BandType::LowShelf:
                newCoeffs = Coeffs::makeLowShelf(sampleRate_, freq, q, A);
                break;
            case BandType::HighShelf:
                newCoeffs = Coeffs::makeHighShelf(sampleRate_, freq, q, A);
                break;
            case BandType::Peak:
            default:
                newCoeffs = Coeffs::makePeakFilter(sampleRate_, freq, q, A);
                break;
        }

        for (int ch = 0; ch < maxChannels; ++ch)
            filters_[i][ch].coefficients = newCoeffs;
    }
}

void SpaceDustFinalEQ::setParameters(const Parameters& p)
{
    params_ = p;
    updateCoefficients();
}

void SpaceDustFinalEQ::process(juce::AudioBuffer<float>& buffer)
{
    if (!params_.enabled)
        return;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), maxChannels);

    if (numSamples <= 0 || numChannels <= 0)
        return;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* data = buffer.getWritePointer(ch);
        for (int band = 0; band < numBands; ++band)
        {
            for (int s = 0; s < numSamples; ++s)
                data[s] = filters_[band][ch].processSample(data[s]);
        }
    }
}
