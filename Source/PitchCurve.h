#pragma once

#include <atomic>
#include <vector>

namespace spacedust
{
    /** A pitch shape drawn by hand, in semitones over time.

        Replaces the three Pitch Env knobs, which could only make one straight
        fall. Free of JUCE for the same reason ModMatrix is.

        Time is 0..1 across whatever the Time knob is set to. Value is
        semitones, and the editor limits it to -24..+24.

        Outside the drawn range the nearest end value is HELD. That is what makes
        a curve drawn from some pitch back to zero behave exactly as the old ramp
        did, so a migrated patch sounds the same.

        THREAD SAFETY (task 12 -- the editor draws into this while notes hold)

        clear(), addPoint() and setPoints() are message-thread only: the state
        loader and the pitch curve editor are the only callers. valueAt() and
        isFlat() are read from BOTH threads -- every voice calls them once per
        sample through the single raw pointer every voice shares (see
        SynthVoice::setPitchCurve), and the editor/thumbnail box calls the very
        same two methods to draw, exactly like the modmatrix-test tool does.

        Editing this object while a voice is reading it from another thread is
        not a hypothetical here, so the shape a mutator builds is never touched
        in place. Each edit is copied whole into one of THREE fixed buffers and
        handed over with a single atomic store -- the same handover shape as
        SpaceDustAudioProcessor::CompiledSet uses for the modulation matrix (see
        the long comment on numCompiledBuffers in PluginProcessor.h), scaled
        down for a much lighter reader: every read here is one independent,
        self-contained call (an audio-thread valueAt() for one sample, or a UI
        paint()), never a whole block's worth of calls sharing one cached
        index. So there is no block-scoped latch step to remember to call --
        valueAt()/isFlat() take their own atomic snapshot of which buffer is
        live on every single call, and publish() always advances to the NEXT
        buffer in a fixed 0-1-2-0-1-2... rotation, never the one just
        published. That means a buffer is only reused after the two buffers
        after it have each been published at least once -- for a reader to see
        torn data, it would have to load an index, then stall for the entire
        duration of two more complete publishes (each a bounded copy of at
        most maxPoints points) before resuming and reading the array it
        loaded. On real hardware that is not a window; it is left open on
        purpose rather than closed with a spin, for the identical reason
        CompiledSet's own residual window is -- see that comment. */
    class PitchCurve
    {
    public:
        struct Point
        {
            float t01 = 0.0f;
            float semitones = 0.0f;

            /** How the line LEAVING this point bows on its way to the next.

                -1..+1, zero being straight, and positive always meaning the
                middle of the segment sits ABOVE the straight line between its
                ends -- whether that segment rises or falls. Keeping "positive
                is up" true in both directions costs a sign flip when the shape
                is published and saves the player from a control that reverses
                itself halfway down a shape (see publish()).

                The bow is SYMMETRIC about the middle of the segment: both
                halves mirror each other, and the line meets its end points at
                the same angle it leaves them. +/-1 is the monotonic limit --
                at the extreme the middle sits a quarter of the segment's own
                height off the straight line, and any further would fold the
                line back on itself. See interpolate().

                The LAST point's bend is meaningless: there is no segment after
                it. It is kept, saved and restored anyway, so that dragging a
                point past the end of the shape and back does not silently lose
                the bend it had. */
            float bend = 0.0f;

            /** Where along the segment the bow PEAKS.

                -1..+1, zero putting it exactly halfway. Positive leans it
                towards the later of the two points, negative towards the
                earlier. This is the axis you get by pulling the handle ALONG
                the line rather than away from it, so one handle carries both
                how far the line bows and where it bows most.

                Costs the bow some depth as it leans -- see publish(). That is
                the price of the line never folding back on itself. */
            float skew = 0.0f;
        };

        /** Hard cap on point count.

            valueAt() is a linear scan over every point, run once per sample by
            every playing voice. 32 points keeps that scan trivial (worst case
            32 float compares) no matter how many voices or how much unison is
            stacked, while comfortably outreaching what a hand-drawn shape
            needs -- the editor adds one point per CLICK, never one per pixel
            of a drag, so reaching 32 takes 32 deliberate clicks. Enforced here
            as well as in the editor, so a hand-edited or corrupt save file
            cannot blow past it either. */
        static constexpr int maxPoints = 32;

        void clear();

        /** Points are kept sorted by time, so drawing right to left works.
            Silently ignored once pointCount() has reached maxPoints -- see
            maxPoints. */
        void addPoint (float t01, float semitones,
                       float bend = 0.0f, float skew = 0.0f);

        /** Replace every point at once, as a single atomic publish.

            The editor calls this instead of clear() + addPoint() in a loop:
            that sequence publishes an empty, flat curve for one buffer swap in
            between the two calls, which a playing voice could sample as a
            flash back to zero bend. setPoints() builds the whole new shape off
            to one side and hands it over in one step, so a reader always sees
            either the old shape or the new one, never a moment with neither.

            Sorted by t01, clamped to [0,1] x [-24,24], and truncated to
            maxPoints if the caller passes more than that -- normal editor
            gestures never do, since the editor itself enforces the cap before
            it ever builds this list. */
        void setPoints (const std::vector<Point>& newPoints);

        /** Message thread. Current point count, e.g. for saving to XML. */
        int pointCount() const noexcept { return (int) points.size(); }

        /** Message thread. */
        Point pointAt (int index) const { return points[(size_t) index]; }

        /** Straight between points, held past either end.

            Safe from the audio thread (per-sample, no lock, no allocation) and
            from the message thread (the editor and its thumbnail box draw
            with this too) -- see the class comment. */
        float valueAt (float t01) const noexcept;

        /** No points, or every point at zero. A flat curve is skipped entirely
            in the voice, so a patch that draws nothing costs nothing. Same
            thread rules as valueAt(). */
        bool isFlat() const noexcept;

    private:
        struct Snapshot
        {
            Point points[maxPoints];

            /** Each segment's bend, with its sign already resolved against the
                direction that segment runs. See publish() for why that is
                decided here, and interpolate() for the expression using it.

                bendK[i] belongs to the segment from points[i] to points[i+1].
                Zero is a straight line, and zero is what an unbent shape
                stores, so nothing has to branch on "is this bent" -- the
                straight case falls out of the same expression. */
            float bendK[maxPoints] {};

            /** Each segment's lean, straight through from Point::skew. Unlike
                the bend it needs no sign resolving: leaning "towards the later
                point" means the same thing whether the segment rises or
                falls, because it is a lean along TIME. */
            float skewK[maxPoints] {};

            int   count = 0;
            bool  flat  = true;
        };

        /** Interpolation, shared so valueAt() has one definition. */
        static float interpolate (const Point* pts, const float* bendK,
                                  const float* skewK, int count, float t01) noexcept;

        /** Build a Snapshot from `points` and hand it over. Message thread. */
        void publish();

        // -- Message-thread source of truth --
        std::vector<Point> points;

        // -- The handover. Three fixed buffers, cycled in a straight rotation
        //    -- see the class comment for why that is enough. --
        static constexpr int numBuffers = 3;
        Snapshot buffers[numBuffers];
        std::atomic<int> liveIndex { 0 };
    };
}
