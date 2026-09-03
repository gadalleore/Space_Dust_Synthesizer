#include "PitchCurveEditor.h"

#include <algorithm>
#include <cmath>

//==============================================================================
// -- PitchCurveBox --

PitchCurveBox::PitchCurveBox (const spacedust::PitchCurve& curveToShow)
    : curve (curveToShow)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void PitchCurveBox::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced (2.0f);

    g.setColour (juce::Colour (0xff14142c));
    g.fillRoundedRectangle (area, 3.0f);

    // The middle line is no bend, so a flat curve reads as "nothing happens".
    g.setColour (juce::Colour (0xff2a2a4a));
    g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());

    juce::Path path;

    for (int x = 0; x <= (int) area.getWidth(); ++x)
    {
        const float t01 = (float) x / juce::jmax (1.0f, area.getWidth());
        const float semitones = curve.valueAt (t01);
        const float y = area.getCentreY()
                      - (semitones / maxSemitones) * area.getHeight() * 0.5f;

        if (x == 0) path.startNewSubPath (area.getX() + (float) x, y);
        else        path.lineTo         (area.getX() + (float) x, y);
    }

    g.setColour (juce::Colour (0xffa0d8ff));
    g.strokePath (path, juce::PathStrokeType (1.5f));

    g.setColour (juce::Colour (0xff00d4ff));
    g.drawRoundedRectangle (area.reduced (0.5f), 3.0f, 1.0f);
}

void PitchCurveBox::mouseUp (const juce::MouseEvent& event)
{
    if (getLocalBounds().contains (event.getPosition()) && onClick)
        onClick();
}

//==============================================================================
// -- PitchCurvePlot --

PitchCurvePlot::PitchCurvePlot (spacedust::PitchCurve& curveToEdit)
    : curve (curveToEdit)
{
    refresh();
}

float PitchCurvePlot::xToT01 (float x) const noexcept
{
    return juce::jlimit (0.0f, 1.0f, x / juce::jmax (1.0f, (float) getWidth()));
}

float PitchCurvePlot::yToSemitones (float y) const noexcept
{
    const float top    = verticalInset;
    const float bottom = juce::jmax (top + 1.0f, (float) getHeight() - verticalInset);
    const float t      = juce::jlimit (0.0f, 1.0f, (y - top) / (bottom - top));

    return juce::jmap (t, 0.0f, 1.0f, maxSemitones, minSemitones);
}

float PitchCurvePlot::t01ToX (float t01) const noexcept
{
    return t01 * (float) getWidth();
}

float PitchCurvePlot::semitonesToY (float semitones) const noexcept
{
    const float top    = verticalInset;
    const float bottom = juce::jmax (top + 1.0f, (float) getHeight() - verticalInset);

    return juce::jmap (semitones, maxSemitones, minSemitones, top, bottom);
}

float PitchCurvePlot::snapSemitones (float semitones, const juce::ModifierKeys& mods) noexcept
{
    // Shift is the OVERRIDE, not the trigger. A pitch envelope is nearly always
    // wanted in whole semitones -- an octave drop, a fifth, a two-semitone
    // scoop -- and the times it is not (a slow drift, a deliberately detuned
    // fall) are the exception you ask for. Snapping by default also means the
    // grid drawn behind the plot is telling the truth about where a node will
    // land, which a grid that only decorates would not be.
    if (mods.isShiftDown())
        return semitones;

    return std::round (semitones);
}

juce::Point<float> PitchCurvePlot::bendHandleFor (int index) const
{
    if (index < 0 || index + 1 >= (int) working.size())
        return { -1000.0f, -1000.0f };

    const float midT = (working[(size_t) index].t01
                        + working[(size_t) index + 1].t01) * 0.5f;

    // Read the height through the CURVE, not through the two end points, so the
    // handle sits on the bent line rather than on the straight one it used to
    // be. That is what lets it stay under the finger while a bend is dragged.
    return { t01ToX (midT), semitonesToY (curve.valueAt (midT)) };
}

int PitchCurvePlot::bendHandleAt (juce::Point<float> position) const
{
    int   best         = -1;
    float bestDistance = bendHitRadius;

    for (int i = 0; i + 1 < (int) working.size(); ++i)
    {
        const float d = bendHandleFor (i).getDistanceFrom (position);

        if (d <= bestDistance)
        {
            bestDistance = d;
            best = i;
        }
    }

    return best;
}

void PitchCurvePlot::paintBendHandle (juce::Graphics& g, int index) const
{
    const auto centre = bendHandleFor (index);

    if (centre.x < 0.0f)
        return;

    const float r = bendHandleRadius;

    // A filled disc first, so the icon reads against the curve line running
    // underneath it rather than fighting with it.
    g.setColour (juce::Colour (0xff0f0f26));
    g.fillEllipse (centre.x - r, centre.y - r, r * 2.0f, r * 2.0f);

    g.setColour (juce::Colour (0xff00d4ff));
    g.drawEllipse (centre.x - r, centre.y - r, r * 2.0f, r * 2.0f, 1.2f);

    // The glyph: a small arc bowing upwards, which is what the gesture does.
    juce::Path arc;
    arc.startNewSubPath (centre.x - r * 0.55f, centre.y + r * 0.35f);
    arc.quadraticTo     (centre.x,             centre.y - r * 0.75f,
                         centre.x + r * 0.55f, centre.y + r * 0.35f);

    g.strokePath (arc, juce::PathStrokeType (1.4f));
}

float PitchCurvePlot::snapTime (float t01, const juce::ModifierKeys& mods) noexcept
{
    if (mods.isShiftDown())
        return t01;

    return std::round (t01 * (float) timeSnapDivisions) / (float) timeSnapDivisions;
}

int PitchCurvePlot::findNearest (juce::Point<float> position) const noexcept
{
    int best = -1;
    float bestDistance = hitRadius;

    for (int i = 0; i < (int) working.size(); ++i)
    {
        const juce::Point<float> p (t01ToX (working[(size_t) i].t01),
                                    semitonesToY (working[(size_t) i].semitones));
        const float d = p.getDistanceFrom (position);

        if (d <= bestDistance)
        {
            bestDistance = d;
            best = i;
        }
    }

    return best;
}

void PitchCurvePlot::commit()
{
    std::sort (working.begin(), working.end(),
              [] (const WorkingPoint& a, const WorkingPoint& b) { return a.t01 < b.t01; });

    std::vector<spacedust::PitchCurve::Point> toPublish;
    toPublish.reserve (working.size());

    for (const auto& w : working)
        toPublish.push_back ({ w.t01, w.semitones, w.bend, w.skew });

    // ONE atomic publish for the whole shape -- see PitchCurve::setPoints.
    // clear() + addPoint() in a loop would let a playing voice sample an
    // empty, flat curve for one buffer swap in between the two calls.
    curve.setPoints (toPublish);

    repaint();

    if (onCurveChanged)
        onCurveChanged();
}

void PitchCurvePlot::refresh()
{
    working.clear();

    const int n = curve.pointCount();
    for (int i = 0; i < n; ++i)
    {
        const auto p = curve.pointAt (i);
        working.push_back ({ p.t01, p.semitones, p.bend, p.skew, nextId++ });
    }

    draggingId     = -1;
    bendingSegment = -1;
    hoveredIndex   = -1;
    hoveredSegment = -1;
    repaint();
}

void PitchCurvePlot::setTimeGrid (bool syncedToTempo, double beats)
{
    if (gridSynced == syncedToTempo && gridBeats == beats)
        return;

    gridSynced = syncedToTempo;
    gridBeats  = beats;
    repaint();
}

void PitchCurvePlot::paintTimeGrid (juce::Graphics& g, juce::Rectangle<float> area) const
{
    const float width = area.getWidth();

    if (width <= 1.0f)
        return;

    // Weight 1, faintest: the SNAP positions. Every place a node can land gets
    // a line, for the same reason every semitone gets a hairline -- a grid that
    // does not mark where a node will land is decoration, not a grid.
    const float snapSpacing = width / (float) timeSnapDivisions;

    if (snapSpacing >= minGridSpacing)
    {
        g.setColour (juce::Colour (0xff17172f));

        for (int i = 1; i < timeSnapDivisions; ++i)
            g.drawVerticalLine ((int) (snapSpacing * (float) i), 0.0f, area.getHeight());
    }

    // Free running: there is no musical unit, because the shape plays over
    // however many seconds the Time knob says. Quarters of the span are still
    // worth marking, and nothing here pretends they are beats.
    if (! gridSynced)
    {
        g.setColour (juce::Colour (0xff26264a));

        for (int i = 1; i < 4; ++i)
            g.drawVerticalLine ((int) (width * (float) i / 4.0f), 0.0f, area.getHeight());

        return;
    }

    // Weight 2, brighter: BEATS. Weight 3, brightest: BARS. Each is drawn only
    // while it is far enough apart to still read as separate lines -- at eight
    // bars the beats would be on top of each other, so the beats go and the
    // bars stay.
    const double beats       = juce::jmax (0.0, gridBeats);
    const float  beatSpacing = beats > 0.0 ? width / (float) beats : 0.0f;
    const float  barSpacing  = beatSpacing * 4.0f;

    if (beatSpacing >= minGridSpacing)
    {
        g.setColour (juce::Colour (0xff26264a));

        for (int b = 1; b < (int) std::ceil (beats); ++b)
            g.drawVerticalLine ((int) (beatSpacing * (float) b), 0.0f, area.getHeight());
    }

    if (barSpacing >= minGridSpacing && beats >= 4.0)
    {
        g.setColour (juce::Colour (0xff3a3a68));

        for (int bar = 1; bar < (int) std::ceil (beats / 4.0); ++bar)
            g.drawVerticalLine ((int) (barSpacing * (float) bar), 0.0f, area.getHeight());
    }
}

void PitchCurvePlot::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();

    g.setColour (juce::Colour (0xff0f0f26));
    g.fillRect (area);

    // Time first, pitch over it: the horizontal lines are the ones a node
    // actually snaps to, so they are the ones that must stay readable where the
    // two cross.
    paintTimeGrid (g, area);

    // -- The semitone grid --
    //
    // Three weights, because they answer three different questions. A hairline
    // at every semitone is what a snapped node lands on, so it has to be there
    // to be landed on. A brighter line every twelve is the octave, which is
    // what the ear actually counts in. The number beside that one says WHICH
    // octave, so a shape drawn near the top does not have to be counted up
    // from zero.
    //
    // Forty-eight semitones over a plot this size puts the hairlines about
    // four pixels apart, which is close enough that they read as a texture
    // rather than as separate lines. That is the intended reading: the texture
    // says "this axis is quantised", and the octave lines say by how much.
    // The hairline colour is deliberately near the background for the same
    // reason -- it must not compete with the curve drawn over it.
    // The ENDS are not drawn: +24 lands on y=0 and -24 on y=height, and only
    // the first of those is inside the component -- the second is one pixel
    // past the bottom and is clipped away. That asymmetry showed as a line
    // above the "+24" label with nothing to match it under "-24". The border
    // box drawn at the end of this method is the boundary those two were
    // trying to be, so they are left out rather than nudged inwards
    // (Giuseppe, 2026-09-01).
    for (int s = -23; s <= 23; ++s)
    {
        if (s == 0)
            continue;   // the zero line is drawn last, over the top of the rest

        g.setColour (s % 12 == 0 ? juce::Colour (0xff3a3a5f)
                                 : juce::Colour (0xff1c1c38));
        g.drawHorizontalLine ((int) semitonesToY ((float) s), 0.0f, area.getWidth());
    }

    // Zero line: no bend, same meaning as the box's centre line.
    g.setColour (juce::Colour (0xff4a4a7a));
    g.drawHorizontalLine ((int) semitonesToY (0.0f), 0.0f, area.getWidth());

    // Octave numbers, down the left-hand edge. Clamped inside the plot so the
    // outermost two are not cut in half by the top and bottom edges.
    g.setColour (juce::Colour (0xff5a5a8a));
    g.setFont (10.0f);

    for (int s = -24; s <= 24; s += 12)
    {
        const int y = juce::jlimit (0, juce::jmax (0, getHeight() - octaveLabelHeight),
                                    (int) semitonesToY ((float) s) - octaveLabelHeight / 2);

        g.drawText (s > 0 ? ("+" + juce::String (s)) : juce::String (s),
                    3, y, octaveLabelWidth, octaveLabelHeight,
                    juce::Justification::centredLeft, false);
    }

    // The curve itself. Reads through PitchCurve::valueAt(), which is safe to
    // call from this (message) thread -- see PitchCurve.h's class comment --
    // so this draws exactly what the audio thread hears, not a separate copy
    // of the same math.
    juce::Path path;

    for (int x = 0; x <= (int) area.getWidth(); ++x)
    {
        const float t01 = (float) x / juce::jmax (1.0f, area.getWidth());
        const float y = semitonesToY (curve.valueAt (t01));

        if (x == 0) path.startNewSubPath ((float) x, y);
        else        path.lineTo         ((float) x, y);
    }

    g.setColour (juce::Colour (0xffa0d8ff));
    g.strokePath (path, juce::PathStrokeType (2.0f));

    // The bend arc, under the points so a point is never hidden by it. Drawn
    // for the segment under the mouse, and for one being dragged even after the
    // pointer has run off the arc it grabbed.
    if (bendingSegment >= 0)
        paintBendHandle (g, bendingSegment);
    else if (hoveredSegment >= 0)
        paintBendHandle (g, hoveredSegment);

    // Point handles: the one being dragged is brighter and larger, and the one
    // merely under the mouse wears a ring. The ring is what says WHICH point the
    // tooltip is talking about when two of them sit close together.
    for (int i = 0; i < (int) working.size(); ++i)
    {
        const auto& w = working[(size_t) i];

        const juce::Point<float> p (t01ToX (w.t01), semitonesToY (w.semitones));
        const bool isDragging = (w.id == draggingId);
        const bool isHovered  = (i == hoveredIndex) && ! isDragging;
        const float r = isDragging ? 6.0f : 4.5f;

        if (isHovered)
        {
            g.setColour (juce::Colour (0x8000d4ff));
            g.drawEllipse (p.x - r - 3.0f, p.y - r - 3.0f,
                           (r + 3.0f) * 2.0f, (r + 3.0f) * 2.0f, 1.5f);
        }

        g.setColour (isDragging ? juce::Colour (0xffffffff) : juce::Colour (0xff00d4ff));
        g.fillEllipse (p.x - r, p.y - r, r * 2.0f, r * 2.0f);
    }

    // The frame, drawn LAST so nothing crosses it -- a curve that reaches the
    // full +/-24 would otherwise sit on top of its own boundary. Dimmer than
    // the panel's own border on purpose: this is the plot inside the window,
    // not a second window, and two edges of equal weight would read as one box
    // drawn twice.
    g.setColour (juce::Colour (0xff00d4ff).withAlpha (0.45f));
    g.drawRect (area, 1.0f);
}

void PitchCurvePlot::mouseDown (const juce::MouseEvent& event)
{
    const int hit = findNearest (event.position);

    // A point wins over a bend handle wherever the two are close enough to
    // compete: the point is the thing you meant if you are on top of one.
    const int handle = (hit >= 0) ? -1 : bendHandleAt (event.position);

    if (event.mods.isRightButtonDown())
    {
        if (hit >= 0)
        {
            working.erase (working.begin() + hit);
            commit();
        }
        else if (handle >= 0)
        {
            // Right-clicking the arc straightens that segment, which is the
            // only way back to a straight line short of dragging by eye. Both
            // axes go, not just the bow: a segment with no bow but a leftover
            // lean is straight on screen and would silently bend the moment it
            // was bowed again.
            working[(size_t) handle].bend = 0.0f;
            working[(size_t) handle].skew = 0.0f;
            commit();
        }

        return;
    }

    if (hit >= 0)
    {
        // Grab the existing point -- do not add a new one on top of it.
        draggingId = working[(size_t) hit].id;
        return;
    }

    if (handle >= 0)
    {
        // Start bending, and do NOT add a point: the arc sits on the curve, so
        // without this every attempt to bend would drop a point on the line.
        bendingSegment = handle;
        bendStartValue = working[(size_t) handle].bend;
        skewStartValue = working[(size_t) handle].skew;
        bendStartMouse = event.position;
        return;
    }

    if ((int) working.size() >= spacedust::PitchCurve::maxPoints)
        return;   // At the cap -- see PitchCurve::maxPoints. Move or remove one first.

    const float t01      = snapTime (xToT01 (event.position.x), event.mods);
    const float semitones = snapSemitones (yToSemitones (event.position.y), event.mods);

    // A new point leaves a STRAIGHT line behind it -- bending is a separate,
    // deliberate gesture on the arc, never something a click inherits.
    working.push_back ({ t01, semitones, 0.0f, 0.0f, nextId });
    draggingId = nextId;
    ++nextId;

    commit();
}

void PitchCurvePlot::mouseDrag (const juce::MouseEvent& event)
{
    if (bendingSegment >= 0)
    {
        if (bendingSegment + 1 < (int) working.size())
        {
            const auto& p0 = working[(size_t) bendingSegment];
            const auto& p1 = working[(size_t) bendingSegment + 1];

            const juce::Point<float> a (t01ToX (p0.t01), semitonesToY (p0.semitones));
            const juce::Point<float> b (t01ToX (p1.t01), semitonesToY (p1.semitones));

            const float dx  = b.x - a.x;
            const float dy  = b.y - a.y;
            const float len = std::sqrt (dx * dx + dy * dy);

            if (len >= 1.0f)
            {
                // The drag is measured against the LINE, not against the
                // screen: away from it bows, along it leans. That is what makes
                // the handle feel like it belongs to the line rather than being
                // a knob that happens to sit on one, and it means a steep
                // segment behaves like a shallow one.
                //
                // Both axes are measured from where the drag STARTED rather
                // than from the previous move, so a slow drag and a quick one
                // over the same distance end in the same place.
                const float alongX = dx / len, alongY = dy / len;

                // Rotate "along" by a quarter turn to get the side of the line.
                // In screen space y grows downward, so this is the UP side --
                // which is the side positive bend already means.
                const float upX = alongY, upY = -alongX;

                const auto  delta = event.position - bendStartMouse;
                const float away  = delta.x * upX    + delta.y * upY;
                const float along = delta.x * alongX + delta.y * alongY;

                // A long segment should not need the same little flick as a
                // short one to lean fully -- see minSkewDragRange.
                const float skewRange = juce::jmax (minSkewDragRange, len * 0.5f);

                auto& seg = working[(size_t) bendingSegment];
                seg.bend = juce::jlimit (-1.0f, 1.0f, bendStartValue + away  / bendDragRange);
                seg.skew = juce::jlimit (-1.0f, 1.0f, skewStartValue + along / skewRange);

                commit();
            }
        }

        return;
    }

    if (draggingId < 0)
        return;

    const float t01       = snapTime (xToT01 (event.position.x), event.mods);
    const float semitones  = snapSemitones (yToSemitones (event.position.y), event.mods);

    for (auto& w : working)
    {
        if (w.id == draggingId)
        {
            w.t01       = t01;
            w.semitones = semitones;
            break;
        }
    }

    commit();
}

void PitchCurvePlot::mouseUp (const juce::MouseEvent& event)
{
    draggingId     = -1;
    bendingSegment = -1;

    // The point may have moved out from under the mouse, or a right-click may
    // have just removed the one the index pointed at -- either way the stored
    // index is about to be wrong. Re-ask rather than keep it.
    const int nowOver = findNearest (event.position);

    if (nowOver != hoveredIndex)
    {
        hoveredIndex = nowOver;
        repaint();
    }
}

void PitchCurvePlot::mouseDoubleClick (const juce::MouseEvent& event)
{
    const int hit = findNearest (event.position);

    if (hit < 0)
    {
        // On the arc instead: straighten that segment, the same as right-click.
        const int handle = bendHandleAt (event.position);

        if (handle >= 0 && (working[(size_t) handle].bend != 0.0f
                            || working[(size_t) handle].skew != 0.0f))
        {
            working[(size_t) handle].bend = 0.0f;
            working[(size_t) handle].skew = 0.0f;
            commit();
        }

        return;
    }

    working.erase (working.begin() + hit);

    // The point the hover ring and the tooltip were pointing at has just gone,
    // and the index would otherwise name whichever point slid into its place.
    hoveredIndex = -1;
    draggingId   = -1;

    commit();
}

void PitchCurvePlot::mouseMove (const juce::MouseEvent& event)
{
    const int nowOver   = findNearest (event.position);
    const int nowOverSeg = (nowOver >= 0) ? -1 : bendHandleAt (event.position);

    if (nowOver == hoveredIndex && nowOverSeg == hoveredSegment)
        return;

    hoveredIndex   = nowOver;
    hoveredSegment = nowOverSeg;
    repaint();
}

void PitchCurvePlot::mouseExit (const juce::MouseEvent&)
{
    if (hoveredIndex < 0 && hoveredSegment < 0)
        return;

    hoveredIndex   = -1;
    hoveredSegment = -1;
    repaint();
}

juce::String PitchCurvePlot::describeSemitones (float semitones)
{
    const bool whole = (semitones == std::floor (semitones));
    const juce::String number (semitones, whole ? 0 : 2);

    return (semitones > 0.0f ? "+" : "") + number + " st";
}

juce::String PitchCurvePlot::describeTime (float t01) const
{
    if (gridSynced && gridBeats > 0.0)
    {
        // Beats are counted from one, the way a musician counts them.
        const double beat = (double) t01 * gridBeats + 1.0;

        return "beat " + juce::String (beat, 2).trimCharactersAtEnd ("0")
                                               .trimCharactersAtEnd (".");
    }

    return juce::String (juce::roundToInt (t01 * 100.0f)) + "% in";
}

juce::String PitchCurvePlot::getTooltip()
{
    if (hoveredSegment >= 0 && hoveredSegment < (int) working.size())
    {
        const float bend = working[(size_t) hoveredSegment].bend;
        const float skew = working[(size_t) hoveredSegment].skew;

        juce::String state ("This line is straight.");

        if (bend != 0.0f)
        {
            state = "This line is bent by "
                  + juce::String (juce::roundToInt (bend * 100.0f)) + "%";

            state += (skew == 0.0f)
                   ? juce::String (".")
                   : ", leaning " + juce::String (juce::roundToInt (std::abs (skew) * 100.0f))
                       + "% " + (skew > 0.0f ? "right." : "left.");
        }

        return state
             + "\nDrag away from the line to bend it."
               "\nDrag along the line to lean the bend towards either point."
               "\nDouble-click or right-click to straighten it.";
    }

    if (hoveredIndex >= 0 && hoveredIndex < (int) working.size())
    {
        const auto& w = working[(size_t) hoveredIndex];

        return describeSemitones (w.semitones) + " at " + describeTime (w.t01)
             + "\nDrag to move it. Double-click or right-click to remove it."
               "\nIt snaps to whole semitones and to 1/32 of the shape."
               "\nHold Shift while dragging to put it anywhere.";
    }

    return "Click to add a point."
           "\nPoints snap to whole semitones and to 1/32 of the shape."
           "\nHold Shift while clicking or dragging to put one anywhere."
           "\nDouble-click or right-click a point to remove it.";
}

//==============================================================================
// -- PitchCurveEditorPanel --

PitchCurveEditorPanel::PitchCurveEditorPanel (spacedust::PitchCurve& curveToEdit,
                                             juce::AudioProcessorValueTreeState& stateToUse,
                                             juce::AudioProcessor& processorToNotify,
                                             SpaceDustLookAndFeel& lookAndFeelToUse)
    : processorForHostNotify (processorToNotify),
      lookAndFeel (lookAndFeelToUse),
      plot (curveToEdit)
{
    setOpaque (true);

    addAndMakeVisible (plot);

    plot.onCurveChanged = [this]
    {
        // Tells the host this project has unsaved changes -- PITCHCURVE is
        // not an APVTS parameter, so nothing else would ever do this for a
        // drawn edit. See juce::AudioProcessor::updateHostDisplay.
        processorForHostNotify.updateHostDisplay (
            juce::AudioProcessor::ChangeDetails{}.withNonParameterStateChanged (true));

        // Let the owner (the thumbnail box on the main tab) redraw while this
        // panel is open and the shape is changing under the mouse.
        if (onCurveChanged)
            onCurveChanged();
    };

    timeSlider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    timeSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 55, 18);
    timeSlider.setTextValueSuffix (" s");
    timeSlider.setLookAndFeel (&lookAndFeel);
    timeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        stateToUse, "pitchCurveTime", timeSlider);
    addAndMakeVisible (timeSlider);

    timeLabel.setText ("Time", juce::dontSendNotification);
    timeLabel.setJustificationType (juce::Justification::centred);
    timeLabel.setColour (juce::Label::textColourId, juce::Colour (0xffa0d8ff));
    timeLabel.setFont (lookAndFeel.getBodyFont (12.0f, true));
    timeLabel.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (timeLabel);

    // A knob, not a dropdown, and it needs no list of its own: pitchCurveDivision
    // is a Choice parameter, so its normalisable range is already 0..8 in steps
    // of one, and JUCE's SliderParameterAttachment gives the knob a
    // textFromValueFunction built on the parameter's own getText(). The readout
    // therefore says "1/4" and "8/1" rather than 3 and 8, and the names can only
    // ever come from the parameter -- there is no second list here to fall out
    // of step with the divisionBeats[] table in processBlock.
    divisionSlider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    divisionSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 55, 18);
    divisionSlider.setLookAndFeel (&lookAndFeel);
    divisionAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        stateToUse, "pitchCurveDivision", divisionSlider);

    // The grid under the curve counts whatever this knob says, so it has to
    // follow every move of it -- including moves that come from the host rather
    // than from the mouse, which is why this is onValueChange and not a drag
    // callback.
    divisionSlider.onValueChange = [this] { updateSyncControls(); };
    addChildComponent (divisionSlider);

    divisionLabel.setText ("Division", juce::dontSendNotification);
    divisionLabel.setJustificationType (juce::Justification::centred);
    divisionLabel.setColour (juce::Label::textColourId, juce::Colour (0xffa0d8ff));
    divisionLabel.setFont (lookAndFeel.getBodyFont (12.0f, true));
    divisionLabel.setLookAndFeel (&lookAndFeel);
    addChildComponent (divisionLabel);

    syncButton.setColour (juce::ToggleButton::textColourId, juce::Colour (0xffa0d8ff));
    syncButton.setLookAndFeel (&lookAndFeel);
    syncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        stateToUse, "pitchCurveSync", syncButton);

    // onStateChange rather than onClick: the parameter can also move from the
    // HOST -- automation, a preset load -- and the swap has to follow it then
    // too, not only when the button itself is pressed.
    syncButton.onStateChange = [this] { updateSyncControls(); };
    addAndMakeVisible (syncButton);

    loopButton.setColour (juce::ToggleButton::textColourId, juce::Colour (0xffa0d8ff));
    loopButton.setLookAndFeel (&lookAndFeel);
    loopAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        stateToUse, "pitchCurveLoop", loopButton);
    addAndMakeVisible (loopButton);

    updateSyncControls();

    closeButton.setLookAndFeel (&lookAndFeel);
    closeButton.onClick = [this] { hidePanel(); };
    addAndMakeVisible (closeButton);

    setVisible (false);
}

PitchCurveEditorPanel::~PitchCurveEditorPanel()
{
    // A panel destroyed while open must leave no listener behind on a parent
    // that outlives it -- see WaveformEditorPanel's identical guard.
    if (auto* parent = getParentComponent(); parent != nullptr && watchingOutsideClicks)
        parent->removeMouseListener (&outsideClicks);

    timeSlider.setLookAndFeel (nullptr);
    timeLabel.setLookAndFeel (nullptr);
    divisionSlider.setLookAndFeel (nullptr);
    divisionLabel.setLookAndFeel (nullptr);
    syncButton.setLookAndFeel (nullptr);
    loopButton.setLookAndFeel (nullptr);
    closeButton.setLookAndFeel (nullptr);
}

void PitchCurveEditorPanel::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();

    g.setColour (juce::Colour (0xff0a0a1f));
    g.fillRoundedRectangle (area, 6.0f);

    g.setColour (juce::Colour (0xff00d4ff));
    g.drawRoundedRectangle (area.reduced (0.5f), 6.0f, 1.5f);

    g.setColour (juce::Colour (0xffa0d8ff));
    g.setFont (lookAndFeel.getBodyFont (13.0f, true));
    g.drawText ("Pitch Curve",
               getLocalBounds().removeFromTop (titleHeight).reduced (frameInset, 0),
               juce::Justification::centredLeft, false);
}

void PitchCurveEditorPanel::resized()
{
    auto area = getLocalBounds();
    auto titleRow = area.removeFromTop (titleHeight);

    closeButton.setBounds (titleRow.removeFromRight (titleHeight + frameInset)
                                   .reduced (4, 3));

    area = area.reduced (frameInset, 0).withTrimmedBottom (frameInset);

    // The controls read top to bottom as a sentence: the two switches decide
    // WHAT the knob under them means, so they sit above it rather than beside
    // it, and the knob is the one thing the eye lands on last.
    constexpr int controlsRowHeight = 104;
    auto controlsRow = area.removeFromBottom (controlsRowHeight);

    constexpr int toggleHeight = 26;
    constexpr int toggleWidth  = 84;
    constexpr int toggleGap    = 12;

    auto togglePair = controlsRow.removeFromTop (toggleHeight)
                                 .withSizeKeepingCentre (toggleWidth * 2 + toggleGap,
                                                         toggleHeight);
    syncButton.setBounds (togglePair.removeFromLeft (toggleWidth));
    togglePair.removeFromLeft (toggleGap);
    loopButton.setBounds (togglePair.removeFromLeft (toggleWidth));

    controlsRow.removeFromTop (6);

    // Time and Division are given the SAME rectangle, because only one of them
    // is ever visible -- see updateSyncControls(). Two knobs in one place, not
    // two places with one empty.
    constexpr int knobSize  = 56;
    constexpr int knobColumn = 96;
    auto knobCol = controlsRow.withSizeKeepingCentre (knobColumn, controlsRow.getHeight());
    const auto labelRow = knobCol.removeFromTop (16);
    timeLabel.setBounds (labelRow);
    divisionLabel.setBounds (labelRow);

    const auto knobBounds = knobCol.withSizeKeepingCentre (knobSize, knobSize);
    timeSlider.setBounds (knobBounds);
    divisionSlider.setBounds (knobBounds);

    plot.setBounds (area.withTrimmedBottom (8));
}

void PitchCurveEditorPanel::updateSyncControls()
{
    const bool synced = syncButton.getToggleState();

    timeSlider.setVisible (! synced);
    timeLabel.setVisible (! synced);
    divisionSlider.setVisible (synced);
    divisionLabel.setVisible (synced);

    // The plot's vertical grid counts the same division the shape is played to.
    // Clamped rather than trusted: the knob's value comes from a parameter a
    // host can automate, and an out-of-range index here would read off the end
    // of the beats table.
    const int division = juce::jlimit (0, spacedust::numPitchCurveDivisions - 1,
                                       (int) std::round (divisionSlider.getValue()));

    plot.setTimeGrid (synced, spacedust::pitchCurveDivisionBeats[division]);
}

juce::Rectangle<int> PitchCurveEditorPanel::titleBarArea() const
{
    // The close button sits at the right-hand end of the title row and is a
    // button, not a handle -- see resized(), which gives it the same width.
    return getLocalBounds().removeFromTop (titleHeight)
                           .withTrimmedRight (titleHeight + frameInset);
}

void PitchCurveEditorPanel::clampInsideParent()
{
    auto* parent = getParentComponent();

    if (parent == nullptr)
        return;

    const int usableHeight = keepAboveBottom > 0
                           ? juce::jmin (parent->getHeight(), keepAboveBottom)
                           : parent->getHeight();

    const int maxX = juce::jmax (0, parent->getWidth() - getWidth());
    const int maxY = juce::jmax (0, usableHeight - getHeight());

    setTopLeftPosition (juce::jlimit (0, maxX, getX()),
                        juce::jlimit (0, maxY, getY()));
}

void PitchCurveEditorPanel::mouseDown (const juce::MouseEvent& event)
{
    if (titleBarArea().contains (event.getPosition()))
    {
        draggingPanel = true;
        dragger.startDraggingComponent (this, event);
    }
}

void PitchCurveEditorPanel::mouseDrag (const juce::MouseEvent& event)
{
    if (! draggingPanel)
        return;

    dragger.dragComponent (this, event, nullptr);
    clampInsideParent();
    hasBeenMoved = true;
}

void PitchCurveEditorPanel::mouseUp (const juce::MouseEvent&)
{
    draggingPanel = false;
}

void PitchCurveEditorPanel::mouseMove (const juce::MouseEvent& event)
{
    setMouseCursor (titleBarArea().contains (event.getPosition())
                        ? juce::MouseCursor::DraggingHandCursor
                        : juce::MouseCursor::NormalCursor);
}

bool PitchCurveEditorPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        hidePanel();
        return true;
    }

    return false;
}

void PitchCurveEditorPanel::OutsideClickWatcher::mouseDown (const juce::MouseEvent& event)
{
    if (! owner.isVisible())
        return;

    if (event.eventComponent == &owner || owner.isParentOf (event.eventComponent))
        return;

    owner.hidePanel();
}

void PitchCurveEditorPanel::showFor (juce::Component* anchorBox)
{
    plot.refresh();
    updateSyncControls();

    // Bigger than it was, for two reasons that both came in with this build.
    // Wider: the octave numbers now sit down the left-hand edge and would
    // otherwise crowd a curve drawn near t=0. Taller: 48 semitones over the old
    // 186-pixel plot put the hairlines under four pixels apart, which is below
    // where a grid stops reading as a grid.
    // Taller than it was again: the switches moved from beside the knob to above
    // it, which costs 30 pixels the plot must not pay for. 48 semitones over the
    // plot this leaves puts the hairlines about five pixels apart, which is
    // where a grid still reads as a grid.
    constexpr int width  = 380;
    constexpr int height = 380;
    setSize (width, height);

    if (auto* parent = getParentComponent())
    {
        if (! hasBeenMoved && anchorBox != nullptr)
        {
            const auto anchor = parent->getLocalArea (anchorBox, anchorBox->getLocalBounds());

            const int usableBottom = keepAboveBottom > 0
                                   ? juce::jmin (parent->getHeight(), keepAboveBottom)
                                   : parent->getHeight();

            // Below the box when there is room, above it when there is not, and
            // never ON it. The box is the control that OPENS this panel, so a
            // panel lying over its own button turns the next click meant for
            // that button into a click in the plot -- which draws a point
            // instead of doing nothing, and leaves the player wondering where
            // the point came from. The old 300-pixel panel already overlapped
            // the box once the keyboard strip pushed it up; at 380 it covers it
            // outright (Giuseppe, 2026-09-01).
            const int below = anchor.getBottom() + 4;
            const int above = anchor.getY() - getHeight() - 4;

            const int y = (below + getHeight() <= usableBottom) ? below
                        : (above >= 0                          ? above
                                                               : below);

            setTopLeftPosition (anchor.getX(), y);
        }

        clampInsideParent();
    }

    setVisible (true);
    toFront (true);

    if (auto* parent = getParentComponent(); parent != nullptr && ! watchingOutsideClicks)
    {
        parent->addMouseListener (&outsideClicks, true);
        watchingOutsideClicks = true;
    }

    setWantsKeyboardFocus (true);
    grabKeyboardFocus();
}

void PitchCurveEditorPanel::hidePanel()
{
    if (! isVisible())
        return;

    if (auto* parent = getParentComponent(); parent != nullptr && watchingOutsideClicks)
    {
        parent->removeMouseListener (&outsideClicks);
        watchingOutsideClicks = false;
    }

    setVisible (false);
}
