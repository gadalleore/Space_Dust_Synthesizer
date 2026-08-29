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
    const float t = juce::jlimit (0.0f, 1.0f, y / juce::jmax (1.0f, (float) getHeight()));
    return juce::jmap (t, 0.0f, 1.0f, maxSemitones, minSemitones);
}

float PitchCurvePlot::t01ToX (float t01) const noexcept
{
    return t01 * (float) getWidth();
}

float PitchCurvePlot::semitonesToY (float semitones) const noexcept
{
    return juce::jmap (semitones, maxSemitones, minSemitones, 0.0f, (float) getHeight());
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
        toPublish.push_back ({ w.t01, w.semitones });

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
        working.push_back ({ p.t01, p.semitones, nextId++ });
    }

    draggingId = -1;
    repaint();
}

void PitchCurvePlot::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();

    g.setColour (juce::Colour (0xff0f0f26));
    g.fillRect (area);

    // Zero line: no bend, same meaning as the box's centre line.
    const float zeroY = semitonesToY (0.0f);
    g.setColour (juce::Colour (0xff4a4a7a));
    g.drawHorizontalLine ((int) zeroY, 0.0f, area.getWidth());

    // +/-12 semitone guides, for a sense of scale -- an octave either way.
    g.setColour (juce::Colour (0xff2a2a4a));
    g.drawHorizontalLine ((int) semitonesToY (12.0f),  0.0f, area.getWidth());
    g.drawHorizontalLine ((int) semitonesToY (-12.0f), 0.0f, area.getWidth());

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

    // Point handles, the one being dragged drawn brighter and larger.
    for (const auto& w : working)
    {
        const juce::Point<float> p (t01ToX (w.t01), semitonesToY (w.semitones));
        const bool isDragging = (w.id == draggingId);
        const float r = isDragging ? 6.0f : 4.5f;

        g.setColour (isDragging ? juce::Colour (0xffffffff) : juce::Colour (0xff00d4ff));
        g.fillEllipse (p.x - r, p.y - r, r * 2.0f, r * 2.0f);
    }
}

void PitchCurvePlot::mouseDown (const juce::MouseEvent& event)
{
    const int hit = findNearest (event.position);

    if (event.mods.isRightButtonDown())
    {
        if (hit >= 0)
        {
            working.erase (working.begin() + hit);
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

    if ((int) working.size() >= spacedust::PitchCurve::maxPoints)
        return;   // At the cap -- see PitchCurve::maxPoints. Move or remove one first.

    const float t01      = xToT01 (event.position.x);
    const float semitones = yToSemitones (event.position.y);

    working.push_back ({ t01, semitones, nextId });
    draggingId = nextId;
    ++nextId;

    commit();
}

void PitchCurvePlot::mouseDrag (const juce::MouseEvent& event)
{
    if (draggingId < 0)
        return;

    const float t01       = xToT01 (event.position.x);
    const float semitones  = yToSemitones (event.position.y);

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

void PitchCurvePlot::mouseUp (const juce::MouseEvent&)
{
    draggingId = -1;
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

    constexpr int timeRowHeight = 74;
    auto timeRow = area.removeFromBottom (timeRowHeight);

    constexpr int timeKnobSize = 56;
    auto timeCol = timeRow.withSizeKeepingCentre (timeKnobSize, timeRowHeight);
    timeLabel.setBounds (timeCol.removeFromTop (16));
    timeSlider.setBounds (timeCol.withSizeKeepingCentre (timeKnobSize, timeKnobSize));

    plot.setBounds (area.withTrimmedBottom (8));
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

    constexpr int width  = 360;
    constexpr int height = 300;
    setSize (width, height);

    if (auto* parent = getParentComponent())
    {
        if (! hasBeenMoved && anchorBox != nullptr)
        {
            const auto anchor = parent->getLocalArea (anchorBox, anchorBox->getLocalBounds());
            setTopLeftPosition (anchor.getX(), anchor.getBottom() + 4);
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
