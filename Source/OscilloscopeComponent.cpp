#include "OscilloscopeComponent.h"
#include "SpaceDustLookAndFeel.h"

#include <cmath>

//==============================================================================
// Builds the L/R trace across the full time window. Split out of paint() because
// update() builds it too -- the ghost history has to advance with the audio, not
// with however often the component happens to be repainted.
juce::Path OscilloscopeComponent::buildTrace() const
{
    juce::Path path;

    const int numSamples = historyBuffer.getNumSamples();
    const int numCh      = historyBuffer.getNumChannels();

    if (numSamples < 2 || numCh < 1 || getWidth() <= 0 || getHeight() <= 0)
        return path;

    const float w     = static_cast<float>(getWidth());
    const float h     = static_cast<float>(getHeight());
    const float cy    = h * 0.5f;
    const float halfH = (h - 20.0f) * 0.5f;

    // The whole window is valid: it is filled wholesale from the processor's
    // continuous history every update, not accumulated block by block here.
    const int available = numSamples;

    if (available < 2)
        return path;

    // One point per pixel column at most: 4096 samples into a few hundred px is far
    // more detail than can be shown, and stroking every one of them is wasted work.
    const int stride = juce::jmax(1, available / juce::jmax(1, static_cast<int>(w)));

    //==========================================================================
    // -- Lane separation, and what happens past full scale (Giuseppe, 2026-08-03) --
    // Each channel keeps to its own half of the display. laneHalf sets how far the
    // two resting lines sit from centre and yScale how far the wave swings, so the
    // lane a channel can occupy runs from yBase - yScale to yBase + yScale and the
    // traces stay readable as two.
    //
    // Past full scale the trace is simply NOT DRAWN. The subpath breaks and picks
    // up again where the signal comes back in range, leaving a gap in the line.
    //
    // It clamped at first, which flat-topped the wave against the lane edge. That
    // was worse: a flat line is a drawn line, and it puts a stretch of waveform on
    // screen that the audio never contained. A gap says "over" without inventing
    // signal to say it with.
    //
    // Where the break FALLS matters too (Giuseppe, 2026-08-04). Ending the subpath
    // on the last in-range column left every break at a different height, because
    // with stride decimation that column sits anywhere between the lane edge and a
    // whole stride short of it -- so a redlining saw frayed into ragged stumps. The
    // segment that leaves the lane is now cut at the point it actually crosses +/-1
    // and the line is carried to there, so every break lands flush on the same
    // edge: the trace hits a wall. Same on the way back in.
    //
    // The two walls are butted up against each other. The resting lines stay exactly
    // where they were; it is the SWING that grew to meet in the middle, equally up
    // and down, until L's floor and R's ceiling sit one stroke width apart -- edges
    // just touching, no strip of empty space left between the lanes.
    const float laneHalf = halfH * 0.5f;                          // resting lines, unmoved
    const float yScale   = laneHalf - kTraceThickness * 0.5f;     // swing, out to the wall

    for (int ch = 0; ch < juce::jmin(2, numCh); ++ch)
    {
        const float yBase = (ch == 0) ? (cy - laneHalf) : (cy + laneHalf);

        bool  started  = false;   // a subpath is open
        bool  havePrev = false;   // the previous column held a usable sample
        float prevSample = 0.0f;
        float prevX      = 0.0f;

        // Where along prev..cur the segment passes through the lane edge, as a
        // fraction. One end is in range and the other is not, so they always
        // differ; the guard is only there to keep a denormal gap from producing
        // an infinity, and the clamp to keep the cut inside the segment.
        const auto crossingT = [] (float from, float to, float level)
        {
            const float span = to - from;

            if (std::abs(span) < 1.0e-12f)
                return 0.0f;

            return juce::jlimit(0.0f, 1.0f, (level - from) / span);
        };

        for (int n = 0; n < available; n += stride)
        {
            // Oldest sample first, so the wave travels left to right the way a
            // scope's does. readSpectrumSamples already hands them over in order.
            const float sample = historyBuffer.getSample(ch, n);

            const float x = juce::jmap(static_cast<float>(n), 0.0f,
                                       static_cast<float>(available - 1), 10.0f, w - 10.0f);

            // A NaN or infinite sample can't be interpolated towards, so it breaks
            // the line outright and takes the crossing history with it.
            if (! std::isfinite(sample))
            {
                started = havePrev = false;
                continue;
            }

            if (std::abs(sample) <= 1.0f)
            {
                const float y = yBase - sample * yScale;

                if (! started)
                {
                    // havePrev here means the column before was out of the lane, so
                    // pick the line up on the edge it comes back through rather than
                    // in mid-air a stride inside.
                    if (havePrev)
                    {
                        const float level = (prevSample > 1.0f) ? 1.0f : -1.0f;
                        const float t     = crossingT(prevSample, sample, level);

                        path.startNewSubPath(prevX + (x - prevX) * t, yBase - level * yScale);
                        path.lineTo(x, y);
                    }
                    else
                    {
                        path.startNewSubPath(x, y);
                    }

                    started = true;
                }
                else
                {
                    path.lineTo(x, y);
                }
            }
            else
            {
                // Leaving the lane: carry the line as far as the edge, then break it.
                if (started)
                {
                    const float level = (sample > 1.0f) ? 1.0f : -1.0f;
                    const float t     = crossingT(prevSample, sample, level);

                    path.lineTo(prevX + (x - prevX) * t, yBase - level * yScale);
                }

                started = false;
            }

            prevSample = sample;
            prevX      = x;
            havePrev   = true;
        }
    }

    return path;
}

//==============================================================================
void OscilloscopeComponent::paint(juce::Graphics& g)
{
    drawBackground(g);

    // Older sweeps first: the RGB dither marking where the trace has just been.
    SpaceDustDither::ghostTrail(g, traceHistory, kTraceThickness * 1.4f, kTrailSpread, kTrailAlpha, *ditherTiles);

    const auto trace = buildTrace();

    if (! trace.isEmpty())
    {
        // Bloom, scaled by the meter. Asked from the inherited LookAndFeel rather
        // than pushed in, so this needs no wiring from the editor. glowTrace, not
        // glowPath: see the note there on why a trace needs the tighter spread.
        if (auto* sdLnf = dynamic_cast<SpaceDustLookAndFeel*>(&getLookAndFeel()))
            sdLnf->glowTrace(g, trace, traceColour, kTraceThickness);

        g.setColour(traceColour);
        g.strokePath(trace, juce::PathStrokeType(kTraceThickness));
    }
}

//==============================================================================
void OscilloscopeComponent::resized()
{
    // Every stored path was built against the old size, so they would ghost in the
    // wrong places. Cheaper and more honest to drop them and let the trail rebuild.
    traceHistory.clear();
}

//==============================================================================
void OscilloscopeComponent::drawBackground(juce::Graphics& g)
{
    g.fillAll(bgColour);
}

//==============================================================================
void OscilloscopeComponent::update(const juce::AudioBuffer<float>& buffer, int validSamples)
{
    juce::ignoreUnused(buffer, validSamples);

    // Stereo, gap-free: both channels come from the processor's continuous scope
    // history, so the long window is real audio rather than stitched blocks.
    // split the scope used to show, and it is the right trade -- a gap-free mono
    // waveform is a truer picture of what is coming out than two stereo traces
    // stitched from non-adjacent blocks.
    if (fillSamplesCallback == nullptr)
        return;

    if (historyBuffer.getNumChannels() != 2 || historyBuffer.getNumSamples() != kWindowSamples)
    {
        historyBuffer.setSize(2, kWindowSamples, false, true, true);
        historyBuffer.clear();
        traceHistory.clear();
    }

    fillSamplesCallback(historyBuffer.getWritePointer(0),
                        historyBuffer.getWritePointer(1), kWindowSamples);

    // Advance the ghost trail one frame, in step with the audio.
    traceHistory.push_back(buildTrace());

    while (static_cast<int>(traceHistory.size()) > kHistoryLength)
        traceHistory.erase(traceHistory.begin());
}
