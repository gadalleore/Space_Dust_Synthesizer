#include "ResampleCapture.h"

#include <algorithm>
#include <cmath>

//==============================================================================
int Resample::lengthWithoutTrailingSilence (const float* data, int numSamples,
                                            float threshold) noexcept
{
    if (data == nullptr || numSamples <= 0)
        return 0;

    for (int i = numSamples - 1; i >= 0; --i)
        if (std::abs (data[i]) > threshold)
            return i + 1;

    return 0;
}

void Resample::fadeOutEnd (float* data, int numSamples, int fadeSamples) noexcept
{
    if (data == nullptr || numSamples <= 0 || fadeSamples <= 0)
        return;

    if (fadeSamples > numSamples)
        fadeSamples = numSamples;

    const int from = numSamples - fadeSamples;

    for (int i = 0; i < fadeSamples; ++i)
    {
        // (fade - 1 - i) / fade, so the last sample is multiplied by zero rather
        // than by one over the length of the fade. The point of this is that the
        // recording ENDS at zero; ending one sample short of it would be the same
        // step it exists to remove.
        const float gain = (float) (fadeSamples - 1 - i) / (float) fadeSamples;
        data[from + i] *= gain;
    }
}

//==============================================================================
void ResampleCapture::prepare (double sampleRate, double maxSeconds)
{
    rate = sampleRate;

    const auto wanted = (sampleRate > 0.0 && maxSeconds > 0.0)
                          ? (std::size_t) (sampleRate * maxSeconds) : (std::size_t) 0;

    // A recording running across a sample-rate change cannot be finished at the
    // rate it was started at, so it is dropped rather than half re-made.
    state.store (State::idle, std::memory_order_release);
    used = 0;
    holdRemaining = 0;
    quietRun = 0;
    inTail = false;

    buffer.assign (wanted, 0.0f);
    bufferRight.assign (wanted, 0.0f);
}

//==============================================================================
bool ResampleCapture::arm()
{
    if (buffer.empty() || rate <= 0.0)
        return false;

    // Only from idle. A second press while one is running must not throw away the
    // recording that is already half made.
    State expected = State::idle;
    return state.compare_exchange_strong (expected, State::armed,
                                          std::memory_order_acq_rel);
}

bool ResampleCapture::isBusy() const noexcept
{
    const auto current = state.load (std::memory_order_acquire);
    return current == State::armed || current == State::recording;
}

float ResampleCapture::progress() const noexcept
{
    const auto current = state.load (std::memory_order_acquire);

    if (current == State::finished)
        return 1.0f;

    if (current != State::recording || rate <= 0.0)
        return 0.0f;

    const double expected = (Resample::holdSeconds + Resample::nominalTailSeconds) * rate;

    if (expected <= 0.0)
        return 0.0f;

    // Read while the audio thread is writing it, which is why it is only ever
    // used to draw with: one frame of a bar drawn from a sample count that is a
    // block out of date is not a thing anybody can see.
    //
    // Held below full: a bar that sits at 100% while the sound is still ringing
    // reads as a hang. It is only ever full when the recording really has ended.
    const double done = (double) used / expected;
    return (float) (done < 0.98 ? done : 0.98);
}

void ResampleCapture::cancel() noexcept
{
    state.store (State::idle, std::memory_order_release);
}

bool ResampleCapture::take (std::vector<float>& leftOut, std::vector<float>& rightOut,
                            double& sampleRate, bool& stoppedByLength)
{
    if (state.load (std::memory_order_acquire) != State::finished)
        return false;

    // Where the sound ends is measured on the LEFT channel alone only because the
    // tail detector that wrote it already used the sum -- see write(). Anything
    // still sounding on either side kept the recording alive, so the last audible
    // sample is the same instant for both.
    int length = Resample::lengthWithoutTrailingSilence (buffer.data(), used,
                                                         Resample::silenceThreshold);

    if (length > 0)
    {
        const int fade = (int) (Resample::endFadeSeconds * rate);

        // Keep a little of what comes AFTER the last audible sample. It is the
        // sound still going, below the floor -- so the fade below starts from
        // where the sound really was rather than from where it was cut.
        length = (int) std::min ((std::size_t) used, (std::size_t) (length + fade));

        // Both sides faded by the same amount over the same samples, or the image
        // would swing across the last few milliseconds of every recording.
        Resample::fadeOutEnd (buffer.data(), length, fade);
        Resample::fadeOutEnd (bufferRight.data(), length, fade);
    }

    leftOut.assign (buffer.begin(), buffer.begin() + length);
    rightOut.assign (bufferRight.begin(), bufferRight.begin() + length);
    sampleRate = rate;
    stoppedByLength = filledUp;

    state.store (State::idle, std::memory_order_release);
    return length > 0;
}

//==============================================================================
bool ResampleCapture::startsThisBlock() noexcept
{
    State expected = State::armed;

    if (! state.compare_exchange_strong (expected, State::recording,
                                         std::memory_order_acq_rel))
        return false;

    used = 0;
    holdRemaining = (int) (Resample::holdSeconds * rate);
    quietRun = 0;
    inTail = false;
    filledUp = false;
    return true;
}

bool ResampleCapture::releaseThisBlock (int numSamples, int& offset) noexcept
{
    if (holdRemaining <= 0 || numSamples <= 0)
        return false;

    // More hold left than this block can use up: none of it is the release block.
    if (holdRemaining > numSamples)
    {
        holdRemaining -= numSamples;
        return false;
    }

    // The hold ends inside this block -- including exactly at its end, which is
    // the case that used to be stepped over. A release at the last sample rather
    // than at the first sample of the next block is one sample early, out of
    // ninety-six thousand.
    offset = std::min (holdRemaining, numSamples - 1);
    holdRemaining = 0;
    inTail = true;
    quietRun = 0;
    return true;
}

bool ResampleCapture::isRecording() const noexcept
{
    return state.load (std::memory_order_acquire) == State::recording;
}

void ResampleCapture::write (const float* left, const float* right, int numSamples) noexcept
{
    if (state.load (std::memory_order_acquire) != State::recording)
        return;

    if (numSamples <= 0 || left == nullptr)
        return;

    if (right == nullptr)
        right = left;

    const int capacity = (int) buffer.size();
    const int room = capacity - used;
    const int count = std::min (numSamples, room);
    const int quietEnough = (int) (Resample::tailSilenceSeconds * rate);

    for (int i = 0; i < count; ++i)
    {
        buffer[(std::size_t) (used + i)] = left[i];
        bufferRight[(std::size_t) (used + i)] = right[i];

        // The mono sum still decides when the tail has gone quiet. Silence is a
        // property of the sound, not of one side of it -- a ping-pong delay whose
        // last repeat sits hard right is not silence, and testing the left alone
        // would have cut it off.
        const float value = 0.5f * (left[i] + right[i]);

        // Only once the note has been let go. Before that a slow attack, or the
        // closed step of a gate, would end the recording before it had begun.
        if (inTail)
        {
            if (std::abs (value) > Resample::silenceThreshold)
            {
                quietRun = 0;
            }
            else if (++quietRun >= quietEnough)
            {
                used += i + 1;
                state.store (State::finished, std::memory_order_release);
                return;
            }
        }
    }

    used += count;

    // Out of room. The cap is as long as a waveform slot can hold, so there is
    // nowhere for the rest of the tail to go even if it is still sounding -- and
    // it usually is, which is what filledUp goes on to tell the player.
    if (used >= capacity)
    {
        filledUp = true;
        state.store (State::finished, std::memory_order_release);
    }
}
