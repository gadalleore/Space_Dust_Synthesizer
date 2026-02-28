#include "SpaceDustParametricEQ.h"

//==============================================================================
void SpaceDustParametricEQ::prepare(const juce::dsp::ProcessSpec& spec)
{
    spec_ = spec;
    for (auto& f : filters_)
    {
        f.state = Coeffs::makePeakFilter(spec.sampleRate, 1000.0f, 0.707f, 1.0f);
        f.prepare(spec);
    }
    reset();
}

void SpaceDustParametricEQ::reset()
{
    for (auto& f : filters_)
        f.reset();
}

void SpaceDustParametricEQ::updateCoefficients()
{
    const double sr = spec_.sampleRate;

    // Low shelf
    float lf = juce::jlimit(20.0f, 20000.0f, params_.lowShelf.freqHz);
    float lg = juce::jlimit(-12.0f, 12.0f, params_.lowShelf.gainDb);
    float lq = juce::jlimit(0.1f, 10.0f, params_.lowShelf.Q);
    float lA = std::pow(10.0f, lg / 20.0f);
    filters_[0].state = Coeffs::makeLowShelf(sr, lf, lq, lA);

    // Peaking
    float pf = juce::jlimit(20.0f, 20000.0f, params_.peak.freqHz);
    float pg = juce::jlimit(-12.0f, 12.0f, params_.peak.gainDb);
    float pq = juce::jlimit(0.1f, 10.0f, params_.peak.Q);
    float pA = std::pow(10.0f, pg / 20.0f);
    filters_[1].state = Coeffs::makePeakFilter(sr, pf, pq, pA);

    // High shelf
    float hf = juce::jlimit(20.0f, 20000.0f, params_.highShelf.freqHz);
    float hg = juce::jlimit(-12.0f, 12.0f, params_.highShelf.gainDb);
    float hq = juce::jlimit(0.1f, 10.0f, params_.highShelf.Q);
    float hA = std::pow(10.0f, hg / 20.0f);
    filters_[2].state = Coeffs::makeHighShelf(sr, hf, hq, hA);
}

void SpaceDustParametricEQ::setParameters(const Parameters& p)
{
    params_ = p;
    updateCoefficients();
}

void SpaceDustParametricEQ::process(juce::AudioBuffer<float>& buffer)
{
    if (!params_.enabled || buffer.getNumSamples() <= 0)
        return;

    setParameters(params_);

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    for (auto& f : filters_)
        f.process(context);
}
