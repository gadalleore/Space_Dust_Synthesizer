#include "SexiconReverb.h"
#include <cmath>
#include <algorithm>

//==============================================================================
int SexiconReverb::scaleDelay(int samplesAt29761) const
{
    return static_cast<int>(std::round(samplesAt29761 * sampleRate_ / kOriginalFs));
}

//==============================================================================
float SexiconReverb::processAllpass(AllpassState& state, int delaySamples, float input, float diffusion)
{
    if (state.buffer.empty() || delaySamples < 1) return input;

    const int size = static_cast<int>(state.buffer.size());
    const int readIdx = (state.writeIndex - delaySamples + size) % size;
    const float delayed = state.buffer[readIdx];
    const float out = diffusion * (input - delayed) + delayed;
    state.buffer[state.writeIndex] = out;
    state.writeIndex = (state.writeIndex + 1) % size;
    return out;
}

//==============================================================================
void SexiconReverb::prepare(double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate;
    reset();
}

void SexiconReverb::reset()
{
    const int maxLen = scaleDelay(4500) + 256;
    for (auto& ap : {&inputAp1_, &inputAp2_, &inputAp3_, &inputAp4_})
    {
        ap->buffer.assign(maxLen, 0.0f);
        ap->writeIndex = 0;
    }
    const int tankLens[8] = {672, 4453, 1800, 3720, 908, 4217, 2656, 3163};
    for (int i = 0; i < 8; ++i)
    {
        tankLengths_[i] = std::max(scaleDelay(tankLens[i]), 64);
        tank_[i].assign(static_cast<size_t>(tankLengths_[i]), 0.0f);
        tankWriteIndex_[i] = 0;
    }
    dampState1_ = dampState2_ = bandwidthState_ = 0.0f;
    dcState_ = dcPrevIn_ = 0.0f;
}

void SexiconReverb::setDecay(float decayTimeSeconds)
{
    const float t = juce::jmax(0.0f, decayTimeSeconds);
    if (t <= 0.0f)
    {
        decay_ = 0.0f;
        return;
    }
    if (t < 0.8f)
    {
        decay_ = (t / 0.8f) * 0.55f;
        return;
    }
    const float norm = juce::jlimit(0.0f, 1.0f, (t - 0.8f) / 639.2f);
    decay_ = 0.55f + 0.35f * norm;  // 0.55–0.9 range - 4x longer max decay
}

void SexiconReverb::setSampleRate(double sampleRate)
{
    if (std::abs(sampleRate_ - sampleRate) > 1.0)
    {
        sampleRate_ = sampleRate;
        reset();
    }
}

//==============================================================================
void SexiconReverb::processSample(float input, float& outputL, float& outputR)
{
    const int d1 = scaleDelay(142);
    const int d2 = scaleDelay(107);
    const int d3 = scaleDelay(379);
    const int d4 = scaleDelay(277);

    // DC blocker - prevents low-freq buildup that causes perpetual buzz
    dcState_ = 0.995f * dcState_ + input - dcPrevIn_;
    dcPrevIn_ = input;
    float x = dcState_;
    x = processAllpass(inputAp1_, d1, x, inputDiffusion1_);
    x = processAllpass(inputAp2_, d2, x, inputDiffusion1_);
    x = processAllpass(inputAp3_, d3, x, inputDiffusion2_);
    x = processAllpass(inputAp4_, d4, x, inputDiffusion2_);

    bandwidthState_ = bandwidth_ * bandwidthState_ + (1.0f - bandwidth_) * x;
    x = bandwidthState_;

    const int L0 = tankLengths_[0], L1 = tankLengths_[1], L2 = tankLengths_[2], L3 = tankLengths_[3];
    const int L4 = tankLengths_[4], L5 = tankLengths_[5], L6 = tankLengths_[6], L7 = tankLengths_[7];

    auto read = [this](int line, int delay) -> float
    {
        const int idx = (tankWriteIndex_[line] - delay + tankLengths_[line]) % tankLengths_[line];
        return tank_[line][idx];
    };

    // No modulation - was causing periodic buzzing; fixed delays still give lush diffusion
    const int L0Mod = L0, L4Mod = L4;

    float v0 = x + decay_ * read(7, L7) + decayDiffusion1_ * read(0, L0Mod);
    float v1 = read(0, L0Mod) - decayDiffusion1_ * v0;
    dampState1_ = damping_ * dampState1_ + (1.0f - damping_) * v1;
    float v2 = decay_ * read(1, L1) - decayDiffusion2_ * read(2, L2);
    float v3 = read(2, L2) + decayDiffusion2_ * v2;

    float v4 = x + decay_ * read(3, L3) + decayDiffusion1_ * read(4, L4Mod);
    float v5 = read(4, L4Mod) - decayDiffusion1_ * v4;
    dampState2_ = damping_ * dampState2_ + (1.0f - damping_) * v5;
    float v6 = decay_ * read(5, L5) - decayDiffusion2_ * read(6, L6);
    float v7 = read(6, L6) + decayDiffusion2_ * v6;

    // Soft saturation - prevents runaway without hard-clip buzz
    auto softLimit = [](float x) { return 2.0f * std::tanh(x * 0.5f); };
    v0 = softLimit(v0);
    dampState1_ = softLimit(dampState1_);
    v2 = softLimit(v2);
    v3 = softLimit(v3);
    v4 = softLimit(v4);
    dampState2_ = softLimit(dampState2_);
    v6 = softLimit(v6);
    v7 = softLimit(v7);

    // Tank leak - 4x longer: 0.99998 = minimal drain for very long tails
    constexpr float kTankLeak = 0.99998f;
    v0 *= kTankLeak;
    dampState1_ *= kTankLeak;
    v2 *= kTankLeak;
    v3 *= kTankLeak;
    v4 *= kTankLeak;
    dampState2_ *= kTankLeak;
    v6 *= kTankLeak;
    v7 *= kTankLeak;

    tank_[0][tankWriteIndex_[0]] = v0;
    tank_[1][tankWriteIndex_[1]] = dampState1_;
    tank_[2][tankWriteIndex_[2]] = v2;
    tank_[3][tankWriteIndex_[3]] = v3;
    tank_[4][tankWriteIndex_[4]] = v4;
    tank_[5][tankWriteIndex_[5]] = dampState2_;
    tank_[6][tankWriteIndex_[6]] = v6;
    tank_[7][tankWriteIndex_[7]] = v7;

    for (int i = 0; i < 8; ++i)
        tankWriteIndex_[i] = (tankWriteIndex_[i] + 1) % tankLengths_[i];

    const int t266 = scaleDelay(266), t2974 = scaleDelay(2974), t1913 = scaleDelay(1913);
    const int t1996 = scaleDelay(1996), t1990 = scaleDelay(1990), t187 = scaleDelay(187);
    const int t1066 = scaleDelay(1066), t353 = scaleDelay(353), t3627 = scaleDelay(3627);
    const int t1228 = scaleDelay(1228), t2673 = scaleDelay(2673), t2111 = scaleDelay(2111);
    const int t335 = scaleDelay(335), t121 = scaleDelay(121);

    const float outGain = 4.0f;   // 4x louder again (was 1.0)
    outputL = outGain * (read(5, t266) + read(5, t2974) - read(6, t1913) + read(7, t1996)
                         - read(1, t1990) - read(2, t187) - read(3, t1066));
    outputR = outGain * (read(1, t353) + read(1, t3627) - read(2, t1228) + read(3, t2673)
                         - read(5, t2111) - read(6, t335) - read(7, t121));
}
