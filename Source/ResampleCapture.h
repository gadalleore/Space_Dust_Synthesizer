#pragma once

#include <atomic>
#include <vector>

/**
    Resample -- the synth plays itself a note, and keeps what comes out.

    THE PLAYER DOES NOT PLAY THE NOTE

    Pressing Resample is enough. The plugin puts a middle C in front of its own
    synthesiser, at the top of the next audio block, and records the output of the
    whole chain until the sound has died away. Nothing has to be held down, and
    nothing has to be timed by hand -- which is the only way a button pressed with
    the pointer could ever have caught a note held with the other hand.

    Middle C, and no other note, because middle C is the note a waveform slot is
    played back at unchanged. What goes in at middle C comes back out at middle C,
    at the pitch and the speed it was recorded, with no analysis in between that
    could disagree -- see UserWaveSlot::allowRetune.

    WHERE A RECORDING ENDS

    Not at a fixed length. The note is held for holdSeconds and then released, and
    the recording runs on through whatever follows it -- the release of the
    envelope, the delay repeats, the reverb tail -- until the output has stayed
    under the silence floor for tailSilenceSeconds. A short pluck therefore gives a
    short waveform and a long pad gives a long one, and neither is padded out or
    cut off. The buffer's own length is the only limit, and it is as long as a
    waveform slot can hold.

    The silence rule applies only AFTER the note is released. During the hold it
    would end the recording before a slow attack had got started, and it would cut
    a gated or tremolo patch off at its first gap.
*/
namespace Resample
{
    /** Below this a sample counts as nothing.

        -66 dBFS, which is deliberately BELOW the bottom of the level meter: that
        reads zero at -60 dB, so a tail is still being kept for a while after the
        panel says there is nothing left. Well above a denormal or a stuck DC
        offset, and well above the noise a dithered output floor sits at. */
    inline constexpr float silenceThreshold = 0.0005f;

    /** How long middle C is held down before it is released.

        Long enough for an attack, a decay and some sustain to be heard, short
        enough that pressing the button does not feel like a wait. */
    inline constexpr double holdSeconds = 2.0;

    /** How long the tail must stay under the floor before the recording stops.

        Two seconds, which is far longer than it takes to be sure a reverb has
        finished -- because a reverb is not the only thing that can be still
        running. A delay is SILENT BETWEEN ITS REPEATS, and a bar of it at 120 bpm
        is two seconds of nothing with more sound to come. A shorter window ends
        the recording inside that gap and throws the rest of the echoes away.

        Waiting this long costs nothing but the wait. The silence at the end is
        trimmed off before the sample is stored, so a sound that really did finish
        early is not padded out with two seconds of nothing. */
    inline constexpr double tailSilenceSeconds = 2.0;

    /** How long a tail the progress bar is drawn against.

        A recording ends when the sound does, so how long it has left cannot be
        known while it is running -- a pluck is over in a moment and a pad rings
        for ten seconds. The bar is therefore drawn against an ordinary tail
        rather than a real one, and is held just short of full until the recording
        actually finishes. It never goes backwards and never lies about being
        done, which is as much as a bar over an unknown length can promise. */
    inline constexpr double nominalTailSeconds = 4.0;

    /** The note that is played, and how hard.

        Velocity does not scale this synth's amplitude, so 100 rather than 127 is
        not a quieter recording -- it is simply an ordinary press. */
    inline constexpr int middleC = 60;
    inline constexpr int velocity = 100;

    /** How much of the very end is faded away to nothing.

        A recording is trimmed at the last sample that was ABOVE the silence
        floor, and that sample can fall anywhere in a cycle -- so the sample would
        otherwise end on a step rather than at zero. That step is a click every
        time a one-shot finishes and every time a loop comes round.

        Twenty milliseconds, over material that is by definition below -66 dBFS,
        so what it fades away is nothing anybody can hear. */
    inline constexpr double endFadeSeconds = 0.02;

    /** The length of data with the silence at its end taken off.

        Only the end. The start of a recording is the moment the note began, and
        whatever quiet follows it is the attack taking its time -- part of the
        sound, not something to be tidied away. */
    int lengthWithoutTrailingSilence (const float* data, int numSamples,
                                      float threshold) noexcept;

    /** Fade the last fadeSamples of data down to exactly zero. */
    void fadeOutEnd (float* data, int numSamples, int fadeSamples) noexcept;
}

//==============================================================================
/**
    The recording itself, and the handshake that starts and ends it.

    THREADS

    Four states, one atomic. The message thread arms and later takes; the audio
    thread does everything in between, and the state is what hands the buffer from
    one to the other:

        message thread   arm()               idle    -> armed
        audio thread     startsThisBlock()   armed   -> recording   (once)
        audio thread     write()             recording -> finished  (when done)
        message thread   take()              finished -> idle

    Nothing else touches the buffer, and no state moves backwards, so there is no
    moment at which both threads hold it. cancel() is the one exception, for a
    recording that can never finish because the audio device has stopped.
*/
class ResampleCapture
{
public:
    ResampleCapture() = default;

    /** Size the buffer. Message thread only -- this allocates. Called from
        prepareToPlay, which the host promises not to run against processBlock. */
    void prepare (double sampleRate, double maxSeconds);

    //==========================================================================
    // -- Message thread --

    /** Ask for a recording. False when one is already under way, or when there is
        no buffer to record into because audio was never prepared. */
    bool arm();

    /** Whether a recording is waiting to start or running. */
    bool isBusy() const noexcept;

    /** How far the recording has got, 0 to 1, for the bar the player watches.
        See nominalTailSeconds for what that is measured against. */
    float progress() const noexcept;

    /** Take the finished recording, with the silence at its end removed. False
        while one is still running and after it has already been taken.

        stoppedByLength says the buffer ran out while the sound was still going --
        a reverb or a delay longer than a waveform slot can hold. The tail is
        genuinely cut in that case, and the player is the only one who can do
        anything about it, so they are told. */
    bool take (std::vector<float>& mono, double& sampleRate, bool& stoppedByLength);

    /** Give up on a recording that cannot finish -- no audio device, or a host
        that has stopped calling processBlock. */
    void cancel() noexcept;

    //==========================================================================
    // -- Audio thread --

    /** Whether THIS block is the one that starts a recording. Answers true
        exactly once per arming, so the caller can put the note into the same
        block whose output it begins keeping. */
    bool startsThisBlock() noexcept;

    /** Whether middle C must be let go during this block, and at which sample.

        The hold is counted down in here rather than by the caller so that the
        rule has one home and can be tested without a synth around it -- the hold
        is a whole number of seconds and so lands exactly on a block boundary at
        every buffer size that divides it, and a version of this that answered
        "not yet" on that block held the note down for good.

        Answers true once per recording, and puts it into its tail phase, after
        which silence is what ends it. Answers true even for a recording that has
        been given up on: the note still has to be let go. */
    bool releaseThisBlock (int numSamples, int& offset) noexcept;

    bool isRecording() const noexcept;

    /** Keep this block, if a recording is running. Summed to mono here rather
        than at the far end, because a waveform slot is mono and holding both
        channels would double the memory for something thrown away. */
    void write (const float* left, const float* right, int numSamples) noexcept;

private:
    enum class State
    {
        idle,
        armed,
        recording,
        finished
    };

    std::atomic<State> state { State::idle };

    std::vector<float> buffer;

    /** How much of the buffer holds sound. Written by the audio thread while it
        is recording and read by the message thread only after the state has moved
        to finished, which is the release/acquire pair that publishes it. */
    int used = 0;

    /** Audio thread only. */
    int holdRemaining = 0;
    int quietRun = 0;
    bool inTail = false;

    /** Whether the buffer filling is what ended it. Written by the audio thread
        before the state moves to finished, and read after -- the same
        release/acquire pair that publishes `used`. */
    bool filledUp = false;

    double rate = 0.0;
};
