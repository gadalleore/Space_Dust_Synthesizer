// =====================================================================
//  Resample capture test
//  ---------------------------------------------------------------------
//  Exercises the REAL ResampleCapture -- this links Source/ResampleCapture.cpp
//  rather than reimplementing the state machine, because a reimplemented
//  harness only ever proves the harness.
//
//  What is checked:
//    * the handshake: armed once, started once, finished once, taken once
//    * a recording ends when the TAIL goes quiet, and not before
//    * silence during the held note does NOT end it -- a slow attack survives
//    * the silence at the end is trimmed off what is handed back
//    * a gap between delay repeats does NOT end it -- the echoes are kept
//    * a sound that never dies away is stopped by the length of the buffer,
//      and says so, because that tail really is cut
//    * a recording of nothing hands back nothing, so the window can say so
//
//  Build & run:
//      cmake --build build --config Release --target resample-test
//      ./build/resample_test/Release/resample_test.exe
// =====================================================================
#include "../../Source/ResampleCapture.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int failures = 0;

    void check(bool ok, const std::string& what)
    {
        if (!ok)
        {
            std::printf("  FAIL  %s\n", what.c_str());
            ++failures;
        }
    }

    constexpr double kRate = 48000.0;
    constexpr int kBlock = 512;

    // A tone well clear of the silence floor, and silence.
    std::vector<float> tone(int length, float amplitude = 0.5f)
    {
        std::vector<float> v((std::size_t) length);
        for (int i = 0; i < length; ++i)
            v[(std::size_t) i] = amplitude * (float) std::sin(6.2831853 * 220.0 * i / kRate);
        return v;
    }

    // Let the note go, the way processBlock does: run the hold out a block at a
    // time until the recorder says this is the block to release in. Returns how
    // many samples of hold went by, so a test can check it against holdSeconds.
    int release(ResampleCapture& capture, int blockSize = kBlock)
    {
        int offset = 0;
        int elapsed = 0;

        // Far more blocks than the hold can need, so a recorder that never says
        // "release" fails the test rather than hanging it.
        for (int i = 0; i < 10000; ++i)
        {
            if (capture.releaseThisBlock(blockSize, offset))
                return elapsed + offset;

            elapsed += blockSize;
        }

        return -1;
    }

    // Push `seconds` of one buffer's worth of material through, a block at a time,
    // and report how many samples went in before the recorder stopped listening.
    int pump(ResampleCapture& capture, const std::vector<float>& material, double seconds)
    {
        const int total = (int) (seconds * kRate);
        int sent = 0;

        for (int i = 0; i < total && capture.isRecording(); i += kBlock)
        {
            const int n = std::min(kBlock, total - i);
            std::vector<float> block((std::size_t) n);

            for (int j = 0; j < n; ++j)
                block[(std::size_t) j] = material[(std::size_t) ((i + j) % material.size())];

            capture.write(block.data(), block.data(), n);
            sent += n;
        }

        return sent;
    }

    /** Like pump, but with a DIFFERENT signal on each side -- which is the whole
        point of a stereo recording and the only way to prove the two are kept
        apart rather than summed. */
    int pumpStereo(ResampleCapture& capture, float left, float right, double seconds)
    {
        const int total = (int) (seconds * kRate);
        int sent = 0;

        for (int i = 0; i < total && capture.isRecording(); i += kBlock)
        {
            const int n = std::min(kBlock, total - i);
            std::vector<float> l((std::size_t) n, left);
            std::vector<float> r((std::size_t) n, right);

            capture.write(l.data(), r.data(), n);
            sent += n;
        }

        return sent;
    }
}

int main()
{
    std::printf("Resample capture tests (rate %.0f Hz, hold %.1f s, tail silence %.2f s)\n",
                kRate, Resample::holdSeconds, Resample::tailSilenceSeconds);

    //--------------------------------------------------------------------------
    std::printf("\nThe handshake:\n");
    {
        ResampleCapture capture;
        check(!capture.arm(), "an unprepared recorder armed");

        capture.prepare(kRate, 15.0);
        check(!capture.isBusy(), "a fresh recorder claimed to be busy");
        check(!capture.startsThisBlock(), "a recorder that was never armed started");

        check(capture.arm(), "arming failed");
        check(capture.isBusy(), "an armed recorder did not report itself busy");
        check(!capture.arm(), "a second arming was accepted while one was running");

        check(capture.startsThisBlock(), "the armed recording did not start");
        check(!capture.startsThisBlock(), "the recording started twice");
        check(capture.isRecording(), "the started recording is not running");

        std::vector<float> taken;
        std::vector<float> takenRight;
        double rate = 0.0;
        bool cut = false;
        check(!capture.take(taken, takenRight, rate, cut), "a running recording could be taken");
    }

    //--------------------------------------------------------------------------
    std::printf("\nA held note, then a tail that dies away:\n");
    {
        ResampleCapture capture;
        capture.prepare(kRate, 15.0);
        capture.arm();
        capture.startsThisBlock();

        const auto sound = tone(4800);
        const std::vector<float> quiet((std::size_t) 4800, 0.0f);

        // Two seconds of held note, all of it sounding.
        const int held = pump(capture, sound, Resample::holdSeconds);
        check(capture.isRecording(), "the recording stopped while the note was held");

        // Then the release: one second still sounding, then silence.
        release(capture);
        pump(capture, sound, 1.0);
        check(capture.isRecording(), "the recording stopped while the tail still sounded");

        const int inSilence = pump(capture, quiet, 5.0);
        check(!capture.isRecording(), "the recording did not stop when the tail went quiet");

        std::printf("  held %d, then stopped after %d samples of silence (floor is %d)\n",
                    held, inSilence, (int) (Resample::tailSilenceSeconds * kRate));
        check(inSilence <= (int) (Resample::tailSilenceSeconds * kRate) + kBlock,
              "the recording ran on well past the silence it stopped for");

        std::vector<float> taken;
        std::vector<float> takenRight;
        double rate = 0.0;
        bool cut = false;
        check(capture.take(taken, takenRight, rate, cut), "the finished recording could not be taken");
        check(rate == kRate, "the rate did not survive the recording");

        // Three seconds of sound went in -- two held, one of tail -- and the
        // silence after them must be trimmed off, all but the short piece the
        // end is faded away over.
        const int fade = (int) (Resample::endFadeSeconds * kRate);
        const int expected = (int) (Resample::holdSeconds * kRate) + (int) kRate + fade;
        std::printf("  3 s of sound then %d samples of silence went in, %d taken back"
                    " (expected about %d, fade included)\n",
                    inSilence, (int) taken.size(), expected);
        check((int) taken.size() <= expected + kBlock, "the silence at the end was kept");
        check((int) taken.size() >= expected - kBlock, "part of the sound was trimmed away");

        // The whole point of that fade: a recording ENDS at zero. Anything else is
        // a step, and a step is a click every time a one-shot finishes or a loop
        // comes round.
        check(! taken.empty() && taken.back() == 0.0f,
              "the recording does not end at zero");

        check(!capture.take(taken, takenRight, rate, cut), "the same recording could be taken twice");
        check(!capture.isBusy(), "the recorder stayed busy after being taken");
    }

    //--------------------------------------------------------------------------
    // The note is let go by counting the hold down a block at a time. The hold is
    // a whole number of seconds, so at every buffer size that divides it -- 32, 64,
    // 128 and 256 all divide 2 s at 48 kHz -- it runs out exactly ON a boundary.
    // A version of this that only looked for the hold ending INSIDE a block
    // stepped over those, never sent a note-off, and left middle C held down for
    // good: the note blasted away and no waveform was ever made.
    std::printf("\nThe note is let go at every buffer size, boundary or not:\n");
    {
        const int expected = (int) (Resample::holdSeconds * kRate);

        for (int blockSize : { 32, 64, 128, 256, 441, 512, 1024, 2048 })
        {
            ResampleCapture capture;
            capture.prepare(kRate, 15.0);
            capture.arm();
            capture.startsThisBlock();

            const int at = release(capture, blockSize);
            const bool onBoundary = (expected % blockSize) == 0;

            std::printf("  block %5d %s -> let go at sample %d\n", blockSize,
                        onBoundary ? "(lands on a boundary)" : "                     ", at);

            check(at >= 0, "the note was never let go at this buffer size");
            check(at > expected - blockSize && at <= expected,
                  "the note was let go at the wrong moment");

            int again = 0;
            check(!capture.releaseThisBlock(blockSize, again),
                  "the note was let go twice");
        }
    }

    //--------------------------------------------------------------------------
    // A delay is silent between its repeats. The tail window has to be longer
    // than that gap, or the recording ends inside it and every echo after the
    // first is thrown away.
    std::printf("\nA gap between delay repeats does not end the recording:\n");
    {
        ResampleCapture capture;
        capture.prepare(kRate, 15.0);
        capture.arm();
        capture.startsThisBlock();
        release(capture);

        const auto echo = tone(4800);
        const std::vector<float> quiet((std::size_t) 4800, 0.0f);

        // A bar of delay at 120 bpm: two seconds between repeats, just under the
        // window. Three echoes, and the recording must survive all of them.
        const double gap = 1.9;

        for (int repeat = 0; repeat < 3; ++repeat)
        {
            pump(capture, echo, 0.2);
            pump(capture, quiet, gap);
            check(capture.isRecording(), "the recording died in the gap between repeats");
        }

        pump(capture, quiet, Resample::tailSilenceSeconds + 1.0);
        check(!capture.isRecording(), "the recording did not stop after the last repeat");

        std::vector<float> taken;
        std::vector<float> takenRight;
        double rate = 0.0;
        bool cut = false;
        check(capture.take(taken, takenRight, rate, cut), "nothing came back from the delayed recording");
        check(!cut, "a recording that finished on its own claimed it was cut short");

        // Three echoes and the two gaps between them, with the last gap trimmed.
        const int expected = (int) ((0.2 * 3 + gap * 2) * kRate);
        std::printf("  three repeats %0.1f s apart -> %d samples kept (expected about %d)\n",
                    gap, (int) taken.size(), expected);
        check((int) taken.size() > (int) (expected * 0.9),
              "the echoes after the first gap were lost");
    }

    //--------------------------------------------------------------------------
    std::printf("\nSilence during the held note does not end it:\n");
    {
        ResampleCapture capture;
        capture.prepare(kRate, 15.0);
        capture.arm();
        capture.startsThisBlock();

        // A slow attack: two seconds of nothing at all while the note is held.
        const std::vector<float> quiet((std::size_t) 4800, 0.0f);
        pump(capture, quiet, 2.0);
        check(capture.isRecording(), "a slow attack ended the recording before it started");

        const auto sound = tone(4800);
        pump(capture, sound, 1.0);
        release(capture);
        pump(capture, quiet, 5.0);
        check(!capture.isRecording(), "the recording did not stop once the tail was quiet");

        std::vector<float> taken;
        std::vector<float> takenRight;
        double rate = 0.0;
        bool cut = false;
        check(capture.take(taken, takenRight, rate, cut), "nothing came back from the slow-attack recording");
        std::printf("  kept %d samples (the 2 s of attack silence and the 1 s of sound)\n",
                    (int) taken.size());
        check((int) taken.size() > (int) (2.5 * kRate),
              "the quiet start of the sound was thrown away");
    }

    //--------------------------------------------------------------------------
    std::printf("\nA sound that never dies away is stopped by the buffer:\n");
    {
        ResampleCapture capture;
        capture.prepare(kRate, 2.0);          // a short buffer, to keep the test quick
        capture.arm();
        capture.startsThisBlock();
        release(capture);

        const auto sound = tone(4800);
        const int sent = pump(capture, sound, 10.0);
        check(!capture.isRecording(), "an endless sound was not stopped by the buffer");
        std::printf("  buffer holds %d samples, recorder listened for %d before stopping\n",
                    (int) (2.0 * kRate), sent);
        check(sent <= (int) (2.0 * kRate) + kBlock, "the recorder ran on past a full buffer");

        std::vector<float> taken;
        std::vector<float> takenRight;
        double rate = 0.0;
        bool cut = false;
        check(capture.take(taken, takenRight, rate, cut), "the full buffer could not be taken");
        check((int) taken.size() <= (int) (2.0 * kRate), "more came back than the buffer holds");
        check(cut, "a tail cut off by the buffer did not say so");
    }

    //--------------------------------------------------------------------------
    std::printf("\nA recording of nothing hands back nothing:\n");
    {
        ResampleCapture capture;
        capture.prepare(kRate, 15.0);
        capture.arm();
        capture.startsThisBlock();
        release(capture);

        const std::vector<float> quiet((std::size_t) 4800, 0.0f);
        pump(capture, quiet, 5.0);

        std::vector<float> taken;
        std::vector<float> takenRight;
        double rate = 0.0;
        bool cut = false;
        check(!capture.take(taken, takenRight, rate, cut), "a silent recording claimed to hold sound");
        check(taken.empty(), "a silent recording handed back samples");
        std::printf("  handled\n");
    }

    //--------------------------------------------------------------------------
    std::printf("\nGiving up on a recording:\n");
    {
        ResampleCapture capture;
        capture.prepare(kRate, 15.0);
        capture.arm();
        check(capture.isBusy(), "the armed recording is not busy");

        capture.cancel();
        check(!capture.isBusy(), "cancelling left the recorder busy");
        check(!capture.startsThisBlock(), "a cancelled recording still started");
        check(capture.arm(), "the recorder could not be armed again after cancelling");
        std::printf("  handled\n");
    }

    //--------------------------------------------------------------------------
    std::printf("\nTrimming the silence off the end:\n");
    {
        auto material = tone(1000);
        material.resize(3000, 0.0f);
        const int trimmed = Resample::lengthWithoutTrailingSilence(
            material.data(), (int) material.size(), Resample::silenceThreshold);
        std::printf("  3000 samples in, %d after trimming\n", trimmed);
        check(trimmed <= 1000, "the silence at the end was not trimmed");
        check(trimmed > 900, "the sound itself was trimmed");

        const std::vector<float> quiet(1000, 0.0f);
        check(Resample::lengthWithoutTrailingSilence(quiet.data(), 1000,
                                                     Resample::silenceThreshold) == 0,
              "silence alone did not trim to nothing");
        check(Resample::lengthWithoutTrailingSilence(nullptr, 100,
                                                     Resample::silenceThreshold) == 0,
              "a null buffer produced a length");
    }

    //--------------------------------------------------------------------------
    std::printf("\nFading the end down to zero:\n");
    {
        std::vector<float> flat(1000, 0.5f);
        Resample::fadeOutEnd(flat.data(), (int) flat.size(), 100);

        check(flat.back() == 0.0f, "the last sample is not zero");
        check(flat[899] == 0.5f, "the fade reached back past its own length");
        check(flat[949] > 0.0f && flat[949] < 0.5f, "the fade is not sloping");

        // Degenerate shapes must be harmless, not a crash. A fade asked to be
        // longer than the buffer covers the whole of it -- which still STARTS at
        // full and only ends at zero, because that is what a fade is.
        Resample::fadeOutEnd(nullptr, 100, 10);
        Resample::fadeOutEnd(flat.data(), (int) flat.size(), 0);
        Resample::fadeOutEnd(flat.data(), (int) flat.size(), 100000);
        check(flat.back() == 0.0f, "an over-long fade did not end at zero");
        check(flat.front() > 0.4f, "an over-long fade swallowed the start of the buffer");
        std::printf("  handled\n");
    }

    //--------------------------------------------------------------------------
    std::printf("\nBoth channels are kept, and kept apart:\n");
    {
        // The recording used to be summed to mono here, which threw away the
        // reverb and the chorus -- most of the reason to resample a pad at all.
        // This writes two DIFFERENT channels and checks they come back different.
        ResampleCapture capture;
        capture.prepare(kRate, 15.0);
        capture.arm();
        capture.startsThisBlock();

        // The same shape the test above uses: hold for the whole hold, then let
        // go, then silence until the tail detector ends it. Releasing early does
        // nothing -- the hold is counted in whole seconds -- and a recording that
        // never releases never finishes and can never be taken.
        pumpStereo(capture, 0.5f, -0.25f, Resample::holdSeconds);
        check(capture.isRecording(), "the stereo recording stopped while the note was held");

        release(capture);
        pumpStereo(capture, 0.5f, -0.25f, 1.0);

        const std::vector<float> quiet((std::size_t) 4800, 0.0f);
        pump(capture, quiet, 5.0);
        check(!capture.isRecording(), "the stereo recording did not stop when it went quiet");

        std::vector<float> takenL, takenR;
        double rate = 0.0;
        bool cut = false;

        if (capture.take(takenL, takenR, rate, cut))
        {
            check(takenL.size() == takenR.size(),
                  "the two channels came back different lengths");
            check(!takenL.empty(), "nothing was recorded");

            if (!takenL.empty())
            {
                // Compared away from the very end, where the fade-out pulls both
                // sides towards zero and would make any two channels look alike.
                const std::size_t at = takenL.size() / 4;

                check(takenL[at] > 0.4f, "the left channel is not what was written");
                check(takenR[at] < -0.2f, "the right channel is not what was written");
                check(std::abs(takenL[at] - takenR[at]) > 0.5f,
                      "the two channels were summed together");

                std::printf("  left %.3f, right %.3f -- kept apart\n",
                            takenL[at], takenR[at]);
            }
        }
        else
        {
            check(false, "the stereo recording could not be taken");
        }
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
