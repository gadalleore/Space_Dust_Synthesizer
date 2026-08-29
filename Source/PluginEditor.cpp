#include "PluginEditor.h"
#include "BinaryData.h"

#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>

//==============================================================================
// -- Safe String Helper (Same as PluginProcessor) --
// CRITICAL: Prevents juce_String.cpp:327 assertion from invalid UTF-8 strings
namespace
{
    juce::String safeString(const char* raw)
    {
        if (raw == nullptr || !juce::CharPointer_UTF8::isValidString(raw, -1))
            return "(safe fallback)";
        return juce::String(raw);
    }

    //==========================================================================
    // -- The shaping and unison parameter ids --
    //
    // At FILE scope because two places need exactly this list and they must
    // never drift apart: the constructor builds each knob's attachment from it,
    // and wrapAssignableKnobs() makes the same knobs assignable from it.
    //
    // Retyping them in the second place would have been thirty-six more chances
    // at a typo -- and a wrong id there is not an error, it is a knob that
    // silently never lights up in assign mode. One list, used twice, cannot be
    // wrong in one place and right in the other.
    //
    // Verified against createParameterLayout: the shaping five are BendPlus,
    // BendMinus, BendPlusMinus, Spectrum, Sync on each of osc1 / osc2 / subOsc;
    // the unison four are UnisonVoices / Detune / Width / Phase on each of
    // osc1 / osc2 / subOsc / noise. UnisonVoices is an AudioParameterInt, so it
    // is deliberately NOT a legal destination and wrapKnob passes over it.
    constexpr int kNumShapingIds = 5;
    constexpr int kNumUnisonIds  = 4;

    const char* const kOsc1ShapingIds[kNumShapingIds] =
        { "osc1BendPlus", "osc1BendMinus", "osc1BendPlusMinus", "osc1Spectrum", "osc1Sync" };
    const char* const kOsc2ShapingIds[kNumShapingIds] =
        { "osc2BendPlus", "osc2BendMinus", "osc2BendPlusMinus", "osc2Spectrum", "osc2Sync" };
    const char* const kSubOscShapingIds[kNumShapingIds] =
        { "subOscBendPlus", "subOscBendMinus", "subOscBendPlusMinus", "subOscSpectrum", "subOscSync" };

    const char* const kOsc1UnisonIds[kNumUnisonIds] =
        { "osc1UnisonVoices", "osc1UnisonDetune", "osc1UnisonWidth", "osc1UnisonPhase" };
    const char* const kOsc2UnisonIds[kNumUnisonIds] =
        { "osc2UnisonVoices", "osc2UnisonDetune", "osc2UnisonWidth", "osc2UnisonPhase" };
    const char* const kSubOscUnisonIds[kNumUnisonIds] =
        { "subOscUnisonVoices", "subOscUnisonDetune", "subOscUnisonWidth", "subOscUnisonPhase" };
    const char* const kNoiseUnisonIds[kNumUnisonIds] =
        { "noiseUnisonVoices", "noiseUnisonDetune", "noiseUnisonWidth", "noiseUnisonPhase" };

    // The starfield moved to SpaceDustLookAndFeel, because the Waveforms window
    // needs the same sky behind it and could not reach a function private to this
    // file. Kept as a forwarder rather than changing the call sites: the five
    // pages below each paint their own background and all say drawStarfield.
    void drawStarfield(juce::Graphics& g, int w, int h, float meterLevel = 0.0f)
    {
        SpaceDustLookAndFeel::drawStarfield(g, w, h, meterLevel);
    }

    //==========================================================================
    // -- Ambient glow: OFF (Giuseppe, 2026-08-01) --
    // One switch for the level-reactive bloom that used to sit behind everything:
    // the halos around each group box, the arcade edge glow along the top and
    // bottom of the plate, and the glow strips behind the tab bars.
    //
    // Switched off rather than deleted, because it is a look that may well be
    // wanted back, and the drawing code is worth more than the lines it costs.
    // Flip this to true and all of it returns exactly as it was.
    //
    // NOT included: the starfield (that is background texture, not glow) and the
    // toggle buttons' lit state (that is how a button shows it is on). Both are
    // still drawn. The knobs' own bloom went earlier, with the knob bodies.
    constexpr bool kGlowEnabled = false;

    // Draw glow halos directly (keeps bleed fix from LookAndFeel, overlap may add slightly)
    void drawGlows(juce::Graphics& g, int baseAlpha, juce::Colour glowCol,
                  std::initializer_list<const juce::Component*> groupList)
    {
        if constexpr (! kGlowEnabled)
        {
            juce::ignoreUnused(g, baseAlpha, glowCol, groupList);
            return;
        }

        const float cornerSize = 6.0f, glowExtent = 18.0f;
        const int numBands = 8;
        for (const juce::Component* comp : groupList)
        {
            if (comp == nullptr || !comp->isVisible()) continue;
            auto r = comp->getBoundsInParent().toFloat().expanded(1.0f);
            juce::Path p;
            p.setUsingNonZeroWinding(false);
            for (int i = 0; i < numBands; ++i)
            {
                float oEx = i * (glowExtent / numBands), iEx = (i + 1) * (glowExtent / numBands);
                float oCs = juce::jmax(4.0f, cornerSize + oEx * 0.4f), iCs = juce::jmax(4.0f, cornerSize + iEx * 0.4f);
                auto oR = r.expanded(oEx), iR = r.expanded(iEx);
                p.clear();
                p.addRoundedRectangle(oR.getX(), oR.getY(), oR.getWidth(), oR.getHeight(), oCs);
                p.addRoundedRectangle(iR.getX(), iR.getY(), iR.getWidth(), iR.getHeight(), iCs);
                float t = (float)i / (float)numBands;
                juce::uint8 alpha = static_cast<juce::uint8>(juce::jlimit(0, 255, static_cast<int>(baseAlpha * (1.0f - t * t))));
                g.setColour(glowCol.withAlpha(alpha));
                g.fillPath(p);
            }
        }
    }

    /** Group halo / drawGlows hue: cyan vs red when meter is in clipping hold (matches edge glow). */
    inline juce::Colour meterLinkedGroupGlowHue(bool clipping)
    {
        return clipping ? juce::Colour(SpaceDustLookAndFeel::kClipRed) : juce::Colour(0xff00b4ff);
    }

    /** Title outer-glow layers (slightly brighter cyan than group halos). */
    inline juce::Colour meterLinkedTitleGlowHue(bool clipping)
    {
        return clipping ? juce::Colour(SpaceDustLookAndFeel::kClipRed) : juce::Colour(0xff00d4ff);
    }

    //==========================================================================
    // -- Bloom for a keyed image (title, 63C logo) --
    // Shared so the two bloom identically. They were asked for as the same effect
    // (Giuseppe, 2026-08-09: "have the 63C logo glow the same way the header has been
    // glowing"), and one function is the only way to keep that true as it is tuned.

    /** The bloom at SILENCE. Both marks keep a small halo at rest rather than going
        flat, which is the one place the face deliberately departs from "no glow at
        silence". Raising this lifts the whole curve with it. */
    constexpr float kBloomFloor = 0.30f;

    /** Maps the meter's glow amount onto the bloom range, floor included. */
    inline float bloomAmount(float glow) noexcept
    {
        return kBloomFloor + (1.0f - kBloomFloor) * glow;
    }

    /** Draws `image` as light around itself, in `colour`, at strength `bloom`.
        Works ONLY on an image keyed to transparent: fillAlphaChannelWithCurrentBrush
        uses the alpha as a mask, so an opaque image would bloom its rectangle.

        The copies are the SAME SIZE and OFFSET, never enlarged. Enlarging is right for
        a stroke and wrong for a shape with separate parts: it moves the parts away from
        each other, so nothing lands on top of itself and the result reads as a second
        copy behind the first rather than as light.

        Both the reach and the brightness scale with `bloom`, so they multiply and the
        response is steep. Radii are fractions of the height, so a small mark and a
        large one bloom in proportion rather than by a fixed pixel count.

        `reachScale` and `alphaScale` are per-caller trims on top of that proportion.
        They exist because proportion alone is not always what the eye wants: the 63C
        logo is small, so a proportional halo on it is correspondingly small, and it
        was asked to bloom harder than its size would give (Giuseppe, 2026-08-09).
        The title passes 1.0 for both and is therefore untouched. */
    inline void bloomKeyedImage(juce::Graphics& g, const juce::Image& image,
                                juce::Rectangle<int> area, juce::Colour colour, float bloom,
                                float reachScale = 1.0f, float alphaScale = 1.0f)
    {
        if (! image.isValid() || area.isEmpty() || bloom <= 0.0f)
            return;

        // More copies when the halo is asked to reach further. Eight points spaced
        // around a wide ring start to read as separate blobs rather than as one glow;
        // the count rises with the reach so the ring stays smooth. Derived from
        // reachScale and NOT from the radius, so a caller passing 1.0 gets exactly
        // eight and the title's approved look does not shift.
        const int dirs = juce::jlimit(8, 16, juce::roundToInt(8.0f * reachScale));

        const float radius[2] { area.getHeight() * 0.16f * bloom * reachScale,    // outer
                                area.getHeight() * 0.08f * bloom * reachScale };  // inner
        const float alpha[2]  { 0.065f * bloom * alphaScale, 0.10f * bloom * alphaScale };

        for (int ring = 0; ring < 2; ++ring)
        {
            for (int d = 0; d < dirs; ++d)
            {
                const float theta = juce::MathConstants<float>::twoPi
                                  * (float) d / (float) dirs;

                const auto off = area.translated(
                    juce::roundToInt(std::cos(theta) * radius[ring]),
                    juce::roundToInt(std::sin(theta) * radius[ring]));

                g.setColour(colour.withAlpha(alpha[ring]));
                g.drawImageWithin(image, off.getX(), off.getY(),
                                  off.getWidth(), off.getHeight(),
                                  juce::RectanglePlacement::centred, true);
            }
        }
    }
}

//==============================================================================
// -- TabGlowOverlayComponent: Draws parabolic glow ON TOP of tab bar so it shines through --
class TabGlowOverlayComponent : public juce::Component
{
public:
    TabGlowOverlayComponent(SpaceDustAudioProcessorEditor& ed) : editor(ed)
    {
        setInterceptsMouseClicks(false, false);  // Clicks pass through to tabs
    }
    void paint(juce::Graphics& g) override
    {
        const int w = getWidth();
        const int oh = getHeight();
        if (w <= 0 || oh <= 0) return;

        // Ambient glow is off; this strip draws nothing. See kGlowEnabled.
        if constexpr (! kGlowEnabled) { juce::ignoreUnused(g); return; }

        float avgLevel = editor.getGlowMeterLevel();  // single per-frame averaged L/R snapshot
        const bool isRed = (editor.clippingHoldTicks > 0);
        // 50% more subtle on tabs: scale down alpha
        juce::uint8 peakAlpha = static_cast<juce::uint8>(juce::jlimit(0, 255, static_cast<int>((6 + 60 * avgLevel) * 0.5f)));

        const juce::Colour edgeCol = isRed ? juce::Colour(SpaceDustLookAndFeel::kClipRed) : juce::Colour(0xff00d4ff);
        const juce::Colour midCol  = isRed ? juce::Colour(SpaceDustLookAndFeel::kClipRed).darker(0.6f) : juce::Colour(0xff0066aa);
        const juce::Colour fadeCol = juce::Colours::transparentBlack;

        // Parabolic depth for top glow (same as main editor)
        auto parabolicDepth = [](float xNorm, float layerHeight) -> float {
            float t = 1.0f - 4.0f * (xNorm - 0.5f) * (xNorm - 0.5f);
            t = juce::jmax(0.0f, t);
            return layerHeight * (0.25f + 0.75f * t);
        };

        const float maxGlowDepth = static_cast<float>(oh) * 2.5f;
        for (float alphaScale : { 0.35f, 0.55f, 0.85f })
        {
            juce::Path path;
            path.startNewSubPath(0.0f, 0.0f);
            path.lineTo(static_cast<float>(w), 0.0f);
            for (int x = w; x >= 0; x -= 2)
            {
                float xNorm = static_cast<float>(x) / static_cast<float>(w);
                float depth = juce::jmin(static_cast<float>(oh), parabolicDepth(xNorm, maxGlowDepth));
                path.lineTo(static_cast<float>(x), depth);
            }
            path.closeSubPath();

            juce::ColourGradient grad(edgeCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale)), (float)w * 0.5f, 0.0f,
                                      fadeCol, (float)w * 0.5f, static_cast<float>(oh), false);
            grad.addColour(0.15f, edgeCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale * 0.85f)));
            grad.addColour(0.35f, midCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale * 0.5f)));
            grad.addColour(0.55f, midCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale * 0.22f)));
            grad.addColour(0.78f, fadeCol);
            g.setGradientFill(grad);
            g.fillPath(path);
        }
    }
private:
    SpaceDustAudioProcessorEditor& editor;
};

//==============================================================================
// -- BottomTabGlowOverlayComponent: Draws bottom parabolic glow over tab content --
class BottomTabGlowOverlayComponent : public juce::Component
{
public:
    BottomTabGlowOverlayComponent(SpaceDustAudioProcessorEditor& ed) : editor(ed)
    {
        setInterceptsMouseClicks(false, false);  // Clicks pass through
    }
    void paint(juce::Graphics& g) override
    {
        const int w = getWidth();
        const int oh = getHeight();
        if (w <= 0 || oh <= 0) return;

        // Ambient glow is off; this strip draws nothing. See kGlowEnabled.
        if constexpr (! kGlowEnabled) { juce::ignoreUnused(g); return; }

        float avgLevel = editor.getGlowMeterLevel();  // single per-frame averaged L/R snapshot
        const bool isRed = (editor.clippingHoldTicks > 0);
        // 50% more subtle on tabs: scale down alpha
        juce::uint8 peakAlpha = static_cast<juce::uint8>(juce::jlimit(0, 255, static_cast<int>((6 + 60 * avgLevel) * 0.5f)));

        const juce::Colour edgeCol = isRed ? juce::Colour(SpaceDustLookAndFeel::kClipRed) : juce::Colour(0xff00d4ff);
        const juce::Colour midCol  = isRed ? juce::Colour(SpaceDustLookAndFeel::kClipRed).darker(0.6f) : juce::Colour(0xff0066aa);
        const juce::Colour fadeCol = juce::Colours::transparentBlack;

        auto parabolicDepth = [](float xNorm, float layerHeight) -> float {
            float t = 1.0f - 4.0f * (xNorm - 0.5f) * (xNorm - 0.5f);
            t = juce::jmax(0.0f, t);
            return layerHeight * (0.25f + 0.75f * t);
        };

        const float maxGlowDepth = static_cast<float>(oh) * 2.5f;
        for (float alphaScale : { 0.35f, 0.55f, 0.85f })
        {
            juce::Path path;
            path.startNewSubPath(0.0f, static_cast<float>(oh));
            path.lineTo(static_cast<float>(w), static_cast<float>(oh));
            for (int x = w; x >= 0; x -= 2)
            {
                float xNorm = static_cast<float>(x) / static_cast<float>(w);
                float depth = juce::jmin(static_cast<float>(oh), parabolicDepth(xNorm, maxGlowDepth));
                path.lineTo(static_cast<float>(x), static_cast<float>(oh) - depth);
            }
            path.closeSubPath();

            juce::ColourGradient grad(fadeCol, (float)w * 0.5f, 0.0f,
                                      edgeCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale)), (float)w * 0.5f, static_cast<float>(oh), false);
            grad.addColour(0.22f, fadeCol);
            grad.addColour(0.45f, midCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale * 0.22f)));
            grad.addColour(0.65f, midCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale * 0.5f)));
            grad.addColour(0.85f, edgeCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale * 0.85f)));
            g.setGradientFill(grad);
            g.fillPath(path);
        }
    }
private:
    SpaceDustAudioProcessorEditor& editor;
};

//==============================================================================
// -- StereoLevelMeterComponent Implementation --
StereoLevelMeterComponent::StereoLevelMeterComponent(SpaceDustAudioProcessor& processor)
    : audioProcessor(processor)
{
    setAccessible(false);
    startTimerHz(kFps);
}

// Ballistics, run per frame at kFps. Ported from Sol Voice Tuner's EdgeMeters:
// instant attack, a short hold, then a fall -- and a separate peak-hold mark that
// sinks more slowly and dissolves as it goes.
void StereoLevelMeterComponent::timerCallback()
{
    const float in[2] { audioProcessor.getLeftPeakLevel(),
                        audioProcessor.getRightPeakLevel() };
    bool changed = false;

    for (int ch = 0; ch < 2; ++ch)
    {
        const float wasLevel = level[ch];
        const float wasMark  = mark[ch];
        const float wasFade  = markFade[ch];

        if (in[ch] >= level[ch])
        {
            level[ch] = in[ch];
            hold[ch]  = kHoldFrames;
        }
        else if (hold[ch] > 0)
        {
            --hold[ch];
        }
        else
        {
            level[ch] *= kRelease;

            // Below the scale's floor there is nothing left to draw, so snap to
            // zero rather than repainting an invisible bar forever.
            if (dbToHeight(linearToDb(level[ch])) <= 0.0f)
                level[ch] = 0.0f;
        }

        // The tick takes every new high instantly and never sits below the bar;
        // left alone it sinks at its own rate and dissolves as it goes.
        if (level[ch] >= mark[ch])
        {
            mark[ch]     = level[ch];
            markHold[ch] = kMarkHoldFrames;
            markFade[ch] = 1.0f;
        }
        else if (markHold[ch] > 0)
        {
            --markHold[ch];
        }
        else
        {
            mark[ch] *= kMarkRelease;

            const float markNorm = dbToHeight(linearToDb(mark[ch]));

            // Lit the whole way down, dissolving only across the last kMarkFadeSpan
            // of the scale. Tying alpha to the height left rather than to a frame
            // count is what keeps the two in step: the tick can no longer run out of
            // opacity while it still has most of the bar to travel.
            markFade[ch] = juce::jlimit(0.0f, 1.0f, markNorm / kMarkFadeSpan);

            // Only once it has actually reached the floor -- by then it is both at
            // zero height and at zero alpha, so nothing visible is being cut off.
            if (markNorm <= 0.0f)
            {
                mark[ch]     = 0.0f;
                markFade[ch] = 0.0f;
            }
        }

        changed = changed
               || level[ch]    != wasLevel
               || mark[ch]     != wasMark
               || markFade[ch] != wasFade;
    }

    // Only when something actually moved: at silence every value is pinned at zero
    // and the meter stops asking to be redrawn entirely.
    if (changed)
        repaint();
}

void StereoLevelMeterComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    const int totalWidth = (barWidth * 2) + barGap;
    int startX = (bounds.getWidth() - totalWidth) / 2;  // Center the meters

    // Vertical room for the halo. The bars used to run flush to the top and bottom
    // edges, so the glow below them had nowhere to go and was clipped square -- which
    // is why some corners bloomed and others were sharp (Giuseppe, 2026-08-01).
    const int barTop = padY;
    const int barH   = juce::jmax(1, bounds.getHeight() - padY * 2);

    const juce::Rectangle<int> meters[2] {
        juce::Rectangle<int>(startX, barTop, barWidth, barH),
        juce::Rectangle<int>(startX + barWidth + barGap, barTop, barWidth, barH)
    };

    // Bloom amount, taken from the LookAndFeel this component inherits rather than
    // pushed in: the plate carries SpaceDustLookAndFeel, so every child can just ask.
    float glow = 0.0f;
    if (auto* sdLnf = dynamic_cast<SpaceDustLookAndFeel*>(&getLookAndFeel()))
        glow = sdLnf->getGlowAmount();

    // No background and no border (Giuseppe, 2026-08-01): the bars are the whole
    // meter, and everything else is transparent so the plate shows through. No L/R
    // lettering either -- two vertical bars side by side read as left and right
    // without being told.
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto  meterRect = meters[ch];
        const auto  colF      = meterRect.toFloat();

        const float db         = linearToDb(level[ch]);
        const float heightNorm = dbToHeight(db);
        const int   fillHeight = static_cast<int>(meterRect.getHeight() * heightNorm);

        const float markNorm = dbToHeight(linearToDb(mark[ch]));

        // Silent channel with no mark left: draw nothing at all, and drop the trail
        // history so the next sound does not smear from wherever it stopped.
        if (fillHeight <= 0 && markNorm <= 0.0f)
        {
            trails[ch].clearQuick();
            markTrails[ch].clearQuick();
            continue;
        }

        if (fillHeight > 0)
        {
            auto fillRect = meterRect.withHeight(fillHeight).withY(meterRect.getBottom() - fillHeight);

            // The RGB smear goes down FIRST, so the bar lands on top of its own
            // tail rather than being veiled by it.
            paintTrail(g, colF, (float) fillRect.getY(), trails[ch], kTrailHeadHeight);

            // Split the fill into its red overshoot and the cyan below it FIRST, so
            // the bloom can follow the same split.
            //
            // The first cut of this glowed the whole bar in one colour chosen by
            // whether the level was clipping, which is why a clipping meter grew a
            // red wash across a bar that was still mostly cyan (Giuseppe, 2026-08-01:
            // "a random red bar in the middle"). Each segment now blooms in its own
            // colour, so the glow always matches the ink it comes from.
            const bool inRed = (db > -1.0f);

            const int redHeight = inRed
                ? juce::jmin(fillHeight, static_cast<int>(meterRect.getHeight() * 0.15f))
                : 0;

            const auto redRect  = fillRect.withHeight(redHeight).withY(fillRect.getY());
            const auto cyanRect = fillRect.withHeight(fillHeight - redHeight)
                                          .withY(fillRect.getY() + redHeight);

            // Bloom: light thrown OUTWARD off the bar, not a tint inside it.
            //
            // Each pass is drawn as a HALO -- the expanded rect with the bar's own
            // footprint cut out of it -- so no ink lands where the solid bar will go.
            // Filling plain expanded rects (the first attempt) stacked four alpha
            // layers over the bar itself, which read as an inward wash rather than a
            // glow (Giuseppe, 2026-08-01: "should be outwardly glowing, not just
            // inwardly"). Reach is wider now too, so it clearly leaves the bar.
            // Glow for one segment, thrown clear of the WHOLE bar.
            //
            // The cut-out is `fillRect` (red plus cyan together), never the segment's
            // own rectangle. Cutting each halo against only its own segment put a hard
            // edge along the red/cyan boundary, because each one stopped dead where the
            // other began -- so the light ringed the parts instead of ringing the bar.
            // Excluding the whole bar instead means the two segments' halos meet
            // seamlessly and the glow runs continuously all the way around.
            auto bloom = [&](juce::Rectangle<int> seg, juce::Colour c)
            {
                if (glow <= 0.01f || seg.getHeight() <= 0)
                    return;

                for (int pass = 2; pass >= 0; --pass)
                {
                    // Sideways reach is deliberately SMALL and vertical reach larger.
                    // The bars are only barGap apart, and the first cut of this threw
                    // light up to 19px sideways -- so the two halos completely overlapped
                    // in the gap, stacked alpha, and the pair read as one wide uneven
                    // blob instead of two bars. Kept under half the gap so neighbouring
                    // halos can never meet.
                    const float widenX = 1.5f + (float) pass * 1.5f;
                    const float widenY = 3.0f + (float) pass * 3.0f;
                    const float a      = glow * 0.22f / (1.0f + (float) pass * 1.5f);

                    juce::Graphics::ScopedSaveState saved(g);
                    g.excludeClipRegion(fillRect);   // never paint over the bar itself

                    g.setColour(c.withAlpha(a));
                    g.fillRect(seg.toFloat().expanded(widenX, widenY));
                }
            };

            const juce::Colour cyanHue = juce::Colour(0xff00ffff);
            const juce::Colour redHue  = juce::Colour(SpaceDustLookAndFeel::kClipRed);

            bloom(cyanRect, cyanHue);
            bloom(redRect,  redHue);

            if (cyanRect.getHeight() > 0)
            {
                g.setColour(cyanHue);
                g.fillRect(cyanRect);
            }

            if (redHeight > 0)
            {
                g.setColour(redHue);
                g.fillRect(redRect);
            }
        }
        else
        {
            trails[ch].clearQuick();
        }

        // The peak-hold tick, trailing above the bar on its slower fall.
        if (markNorm > 0.0f)
        {
            const float markY = colF.getBottom() - markNorm * colF.getHeight();
            const float drawY = juce::jmin(markY, colF.getBottom() - kMarkThickness);

            // It moves, so it smears too -- the whole tick, not just an edge of it.
            paintTrail(g, colF, drawY, markTrails[ch], kMarkThickness);

            // Red when the peak it is holding was in the red (Giuseppe, 2026-08-01).
            // (Bloom applied just below, once the colour is known.)
            // Tested against the MARK's own level, not the live one, because the tick
            // represents that captured peak: it stays red the whole way down from a
            // clip and only cools once it has sunk back below the line.
            const bool markInRed = linearToDb(mark[ch]) > -1.0f;

            const juce::Colour markHue = markInRed ? juce::Colour(SpaceDustLookAndFeel::kClipRed)
                                                   : juce::Colour(0xff00ffff);
            const float markAlpha = juce::jlimit(0.0f, 1.0f, markFade[ch]);

            // The tick blooms too, faded by its own dissolve so a dying mark does not
            // glow harder than the bar that outlived it.
            //
            // A HALO like the bar's, not a filled expanded rect. Filling washed the
            // tick's own colour across the bar underneath it -- a cyan mark sitting on
            // the red overshoot painted a pale fringe over the red, which is the light
            // smear at the top of the meter in Giuseppe's screenshot (2026-08-01).
            if (glow > 0.01f)
            {
                const auto markCore = colF.withY(drawY).withHeight(kMarkThickness);
                const auto markCut  = markCore.toNearestInt();

                for (int pass = 2; pass >= 0; --pass)
                {
                    const float widenX = 1.5f + (float) pass * 1.5f;
                    const float widenY = 2.0f + (float) pass * 2.0f;

                    juce::Graphics::ScopedSaveState saved(g);
                    g.excludeClipRegion(markCut);   // ring the tick, do not wash over it

                    g.setColour(markHue.withAlpha(glow * markAlpha * 0.22f
                                                  / (1.0f + (float) pass * 1.5f)));
                    g.fillRect(markCore.expanded(widenX, widenY));
                }
            }

            g.setColour(markHue.withAlpha(markAlpha));
            g.fillRect(colF.withY(drawY).withHeight(kMarkThickness));
        }
    }
}

void StereoLevelMeterComponent::paintTrail(juce::Graphics& g, juce::Rectangle<float> col,
                                           float top, juce::Array<float>& history,
                                           float headHeight)
{
    if (history.isEmpty() || std::abs(top - history.getLast()) > kTrailMinStep)
    {
        history.add(top);

        while (history.size() > kTrailLength)
            history.remove(0);
    }
    else if (history.size() > 1)
    {
        // Standing still: collapse the streak back into the bar fast -- two frames'
        // worth per frame, so it is gone in a blink.
        history.remove(0);

        if (history.size() > 1)
            history.remove(0);
    }

    if (history.isEmpty())
        return;

    // The streak spans the whole distance travelled, not one frame's worth.
    const float displacement = history.getFirst() - top;

    if (std::abs(displacement) < kTrailMinSmear)
        return;

    // Only the head smears. Streaking the whole bar would stamp a column of stipple
    // down the plate every time the level moved.
    juce::Path head;
    head.addRectangle(col.withTop(top)
                         .withHeight(juce::jmin(headHeight, col.getBottom() - top)));

    SpaceDustDither::streakRgb(g, head, { 0.0f, displacement }, kTrailSteps, kTrailAlpha, *ditherTiles);
}

void StereoLevelMeterComponent::resized()
{
    // Component is already sized by parent, just trigger repaint
    repaint();
}

float StereoLevelMeterComponent::linearToDb(float linear)
{
    if (linear <= 0.0f) return -100.0f;  // -Inf (represented as -100 dB)
    return 20.0f * std::log10(linear);
}

float StereoLevelMeterComponent::dbToHeight(float db)
{
    // Map dB to normalized height: -60 dB = 0.0 (bottom), 0 dB = 1.0 (top)
    // Use logarithmic scaling for musical level display
    const float minDb = -60.0f;
    const float maxDb = 0.0f;
    
    if (db <= minDb) return 0.0f;
    if (db >= maxDb) return 1.0f;
    
    // Logarithmic mapping for better visual representation
    float normalized = (db - minDb) / (maxDb - minDb);
    return normalized;
}

//==============================================================================
// -- MainPageComponent Implementation --
MainPageComponent::MainPageComponent(SpaceDustAudioProcessorEditor& editor)
    : parentEditor(editor)
{
    setAccessible(false);
    
    // Add all main page components as children
    addAndMakeVisible(parentEditor.oscillatorsGroup);
    addAndMakeVisible(parentEditor.osc1WaveformCombo);
    addAndMakeVisible(parentEditor.osc1WaveformLabel);
    addAndMakeVisible(parentEditor.osc1WaveformEditButton);
    addAndMakeVisible(parentEditor.osc2WaveformEditButton);
    addAndMakeVisible(parentEditor.noiseWaveformEditButton);
    addAndMakeVisible(parentEditor.osc1CoarseTuneSlider);
    addAndMakeVisible(parentEditor.osc1CoarseTuneLabel);
    addAndMakeVisible(parentEditor.osc1DetuneSlider);
    addAndMakeVisible(parentEditor.osc1DetuneLabel);
    addAndMakeVisible(parentEditor.osc1LevelSlider);
    addAndMakeVisible(parentEditor.osc1LevelLabel);
    addAndMakeVisible(parentEditor.osc1PanSlider);
    addAndMakeVisible(parentEditor.osc1PanLabel);
    addAndMakeVisible(parentEditor.osc2WaveformCombo);
    addAndMakeVisible(parentEditor.osc2WaveformLabel);
    addAndMakeVisible(parentEditor.osc2CoarseTuneSlider);
    addAndMakeVisible(parentEditor.osc2CoarseTuneLabel);
    addAndMakeVisible(parentEditor.osc2DetuneSlider);
    addAndMakeVisible(parentEditor.osc2DetuneLabel);
    addAndMakeVisible(parentEditor.osc2LevelSlider);
    addAndMakeVisible(parentEditor.osc2LevelLabel);
    addAndMakeVisible(parentEditor.osc2PanSlider);
    addAndMakeVisible(parentEditor.osc2PanLabel);
    addAndMakeVisible(parentEditor.noiseColorCombo);
    addAndMakeVisible(parentEditor.noiseColorLabel);
    addAndMakeVisible(parentEditor.noiseLevelSlider);
    addAndMakeVisible(parentEditor.noiseLevelLabel);
    addAndMakeVisible(parentEditor.lowShelfAmountSlider);
    addAndMakeVisible(parentEditor.lowShelfAmountLabel);
    addAndMakeVisible(parentEditor.highShelfAmountSlider);
    addAndMakeVisible(parentEditor.highShelfAmountLabel);
    
    addAndMakeVisible(parentEditor.filterGroup);
    addAndMakeVisible(parentEditor.filterModeCombo);
    addAndMakeVisible(parentEditor.filterModeLabel);
    addAndMakeVisible(parentEditor.filterCutoffSlider);
    addAndMakeVisible(parentEditor.filterCutoffLabel);
    addAndMakeVisible(parentEditor.filterResonanceSlider);
    addAndMakeVisible(parentEditor.filterResonanceLabel);
    addAndMakeVisible(parentEditor.warmSaturationMasterButton);
    addAndMakeVisible(parentEditor.filterKeyTrackButton);
    addChildComponent(parentEditor.filterNoteLockButton);     // shown by resized() when Key Tracking is on
    addChildComponent(parentEditor.filterHarmonicLockButton); // shown by resized() when Note Lock is on

    addAndMakeVisible(parentEditor.filterEnvGroup);
    addAndMakeVisible(parentEditor.filterEnvAttackSlider);
    addAndMakeVisible(parentEditor.filterEnvAttackLabel);
    addAndMakeVisible(parentEditor.filterEnvDecaySlider);
    addAndMakeVisible(parentEditor.filterEnvDecayLabel);
    addAndMakeVisible(parentEditor.filterEnvReleaseSlider);
    addAndMakeVisible(parentEditor.filterEnvReleaseLabel);
    addAndMakeVisible(parentEditor.filterEnvAmountSlider);
    addAndMakeVisible(parentEditor.filterEnvAmountLabel);
    
    addAndMakeVisible(parentEditor.envelopeGroup);
    addAndMakeVisible(parentEditor.envAttackSlider);
    addAndMakeVisible(parentEditor.envAttackLabel);
    addAndMakeVisible(parentEditor.envDecaySlider);
    addAndMakeVisible(parentEditor.envDecayLabel);
    addAndMakeVisible(parentEditor.envSustainSlider);
    addAndMakeVisible(parentEditor.envSustainLabel);
    addAndMakeVisible(parentEditor.envReleaseSlider);
    addAndMakeVisible(parentEditor.envReleaseLabel);
    addAndMakeVisible(parentEditor.pitchEnvAmountSlider);
    addAndMakeVisible(parentEditor.pitchEnvAmountLabel);
    addAndMakeVisible(parentEditor.pitchEnvTimeSlider);
    addAndMakeVisible(parentEditor.pitchEnvTimeLabel);
    addAndMakeVisible(parentEditor.pitchEnvPitchSlider);
    addAndMakeVisible(parentEditor.pitchEnvPitchLabel);
    addAndMakeVisible(parentEditor.subOscToggleButton);
    addAndMakeVisible(parentEditor.subOscWaveformCombo);
    addAndMakeVisible(parentEditor.subOscWaveformEditButton);
    addAndMakeVisible(parentEditor.subOscLevelSlider);
    addAndMakeVisible(parentEditor.subOscCoarseSlider);
    addAndMakeVisible(parentEditor.subOscWaveformLabel);
    addAndMakeVisible(parentEditor.subOscLevelLabel);
    addAndMakeVisible(parentEditor.subOscCoarseLabel);
    
    parentEditor.audioProcessor.getValueTreeState().addParameterListener("subOscOn", this);
    // Key Tracking gates whether the Note Lock toggle exists, and Note Lock in turn
    // gates the Harmonic Series toggle (resized() reads both).
    parentEditor.audioProcessor.getValueTreeState().addParameterListener("filterKeyTrack", this);
    parentEditor.audioProcessor.getValueTreeState().addParameterListener("filterNoteLock", this);
    updateSubOscVisibility();
    
    // Pan labels: click to reset to center, with tooltip
    parentEditor.osc1PanLabel.addMouseListener(this, false);
    parentEditor.osc2PanLabel.addMouseListener(this, false);
    parentEditor.osc1PanLabel.setTooltip("Click to reset to center pan");
    parentEditor.osc2PanLabel.setTooltip("Click to reset to center pan");
    
    // Master section components are now handled by main editor, not MainPageComponent
}

MainPageComponent::~MainPageComponent()
{
    parentEditor.audioProcessor.getValueTreeState().removeParameterListener("subOscOn", this);
    parentEditor.audioProcessor.getValueTreeState().removeParameterListener("filterKeyTrack", this);
    parentEditor.audioProcessor.getValueTreeState().removeParameterListener("filterNoteLock", this);
    cancelPendingUpdate();   // listener gone; drop any queued visibility refresh
    parentEditor.osc1PanLabel.removeMouseListener(this);
    parentEditor.osc2PanLabel.removeMouseListener(this);
}

void MainPageComponent::mouseUp(const juce::MouseEvent& event)
{
    if (event.eventComponent == &parentEditor.osc1PanLabel)
        parentEditor.osc1PanSlider.setValue(0.0, juce::sendNotificationSync);
    else if (event.eventComponent == &parentEditor.osc2PanLabel)
        parentEditor.osc2PanSlider.setValue(0.0, juce::sendNotificationSync);
}

void MainPageComponent::parameterChanged(const juce::String&, float)
{
    // APVTS delivers this on the AUDIO thread under host automation. Both params we
    // listen for ("subOscOn", "filterKeyTrack") end in a relayout, and that calls
    // setVisible()/resized() -> grabKeyboardFocus() -> macOS HIToolbox, which aborts
    // with SIGILL if not on the message thread (dispatch_assert_queue). Marshal it;
    // AsyncUpdater coalesces automation bursts into a single relayout and self-cancels
    // on destruction. (Matches EffectsPageComponent / FinalEQComponent.)
    triggerAsyncUpdate();
}

void MainPageComponent::handleAsyncUpdate()
{
    // Message thread (AsyncUpdater guarantee) â€” safe to touch the UI.
    // This also covers "filterKeyTrack": updateSubOscVisibility ends in resized(),
    // which is where the Note Lock toggle's visibility is decided.
    updateSubOscVisibility();
}

void MainPageComponent::updateSubOscVisibility()
{
    bool on = parentEditor.safeGetParam("subOscOn") > 0.5f;
    parentEditor.subOscWaveformCombo.setVisible(on);
    parentEditor.subOscWaveformEditButton.setVisible(on);
    parentEditor.subOscLevelSlider.setVisible(on);
    parentEditor.subOscCoarseSlider.setVisible(on);
    parentEditor.subOscWaveformLabel.setVisible(on);
    parentEditor.subOscLevelLabel.setVisible(on);
    parentEditor.subOscCoarseLabel.setVisible(on);
    resized();  // Re-layout so Amp Envelope box shrinks/expands (like Filter in Effects tab)
}

void MainPageComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0a0a1f));
    float avgLevel = parentEditor.getGlowMeterLevel();  // single per-frame averaged L/R snapshot
    drawStarfield(g, getWidth(), getHeight(), avgLevel);
    const int baseAlpha = 8 + static_cast<int>(44.0f * avgLevel);
    drawGlows(g, baseAlpha, meterLinkedGroupGlowHue(parentEditor.clippingHoldTicks > 0),
        { &parentEditor.oscillatorsGroup, &parentEditor.filterGroup, &parentEditor.filterEnvGroup, &parentEditor.envelopeGroup });
}

void MainPageComponent::resized()
{
    // ============================================================================
    // MAIN TAB LAYOUT: MATCHING HAND-DRAWN SKETCH
    // ============================================================================
    // Left side (~2/3): Oscillators (top, full height) + Filter (bottom, below Oscillators)
    // Right side (~1/3): Two tall narrow vertical strips - Amp Envelope (left) + Master (right)
    
    // ============================================================================
    // CONSTANTS - Tight spacing for compact layout
    // ============================================================================
    const int outerMargin = 8;            // Compact outer margin
    const int leftRightGap = 6;           // Horizontal gap between left and right sides
    const int topBottomGap = 8;          // Vertical gap between Oscillators and Filter
    const int groupPadding = 10;         // Padding inside group boxes
    const int groupTitleHeight = 32;     // Compact height for group title area
    const int knobDiameter = 56;
    const int labelHeight = 18;           // Label height (slightly larger to prevent clipping)
    const int labelGap = 4;                // Gap between label and control (compact)
    const int comboHeight = 26;           // Combo box height
    const int comboWidth = 110;           // Combo box width
    const int verticalSpacing = 4;       // Compact vertical spacing between controls
    // Increased horizontal spacing to give knobs more breathing room and match Modulation tab's balanced look
    const int horizontalSpacing = 50;      // Horizontal spacing between knobs (increased significantly: 18 + 32 = 50px for better visual balance)
    const int oscillatorTextBoxHeight = 12; // Text box height offset for oscillators (consistent for all)
    const int oscLabelSpacing = oscillatorTextBoxHeight + (labelGap / 2); // Gap below knob text box (no separate title label below)
    const int oscKnobLabelGap = 6;        // Match filter: gap between title label and knob
    const int topPaddingReduction = 0;   // Title now inside box - keep content below it
    
    // ============================================================================
    // CALCULATE DIMENSIONS
    // ============================================================================
    // Tab width has been expanded by 10%, so getWidth() is now 10% larger
    // Calculate old availableWidth to preserve the gap (keep absolute gap value the same)
    int oldTabWidth = static_cast<int>(getWidth() / 1.1);  // Reverse the 10% expansion
    int oldAvailableWidth = oldTabWidth - (2 * outerMargin);
    int availableWidth = getWidth() - (2 * outerMargin);  // New available width (10% larger)
    int availableHeight = getHeight() - outerMargin - 8;
    
    // Left side: Oscillators + Filter
    // Right side: Only Amp Envelope (Master section is now handled by main editor, always visible)
    // First calculate heights (needed before calculating widths)
    // Room for the Noise section's labels, plus the two shaping rows (Wave Mode,
    // Intensity, Sync) that now sit under each oscillator. Each shaping row is a
    // label, a combo-or-knob and a value box -- about 84 px -- and the two of them
    // pushed the Noise section past the bottom of the box at the old 70.
    const int oscHeightExtra = 70;  // Ensure Noise section labels (Level, Low/High Shelf) fit inside box
    int oscHeight = static_cast<int>((static_cast<int>(availableHeight * 0.50) + oscHeightExtra) * 0.9408f);  // ~6% smaller total (4% + 2%) to give Filter more room
    int filterHeight = availableHeight - oscHeight - topBottomGap; // Filter gets remaining
    
    // Calculate target gap to match LFO 2 gap on Modulation tab
    // Keep the absolute gap value the same (based on old availableWidth)
    // This ensures the gap between Amp Envelope's right edge and tab's right edge stays the same
    int targetGapToRight = static_cast<int>(oldAvailableWidth * 0.05);  // Keep same absolute gap value
    
    // Right side: Only Amp Envelope (reduced by 50% from current width, keeping right edge gap the same)
    // Calculate Amp Envelope width first (needed to position it)
    int rightSideWidth = availableWidth;  // Will be adjusted based on positioning
    int gapToRightEdge = static_cast<int>(rightSideWidth * 0.5 * 0.1);  // 10% of original gap
    int currentAmpEnvWidth = static_cast<int>((rightSideWidth - gapToRightEdge) * 0.6);  // Current width (reduced by 40%)
    int narrowStripWidth = static_cast<int>(currentAmpEnvWidth * 0.5);  // Reduced by 50% from current width
    
    // Calculate where Amp Envelope should be positioned to match the gap
    // Amp Envelope right edge should be at: getWidth() - outerMargin - targetGapToRight
    // This keeps the gap the same absolute value, moving Amp Envelope to the right
    // So Amp Envelope X position should be: getWidth() - outerMargin - targetGapToRight - narrowStripWidth
    int ampEnvRightEdge = getWidth() - outerMargin - targetGapToRight;
    int rightX = ampEnvRightEdge - narrowStripWidth;
    
    // Gap is targetGapToRight, which will be matched in ModulationPageComponent using the same calculation
    
    // Now calculate leftSideWidth to fill the space up to rightX
    int leftX = outerMargin;
    int leftSideWidth = rightX - leftX - leftRightGap;  // Increased to fill space before Amp Envelope
    // Cosmetic: center the whole box cluster (Oscillators/Filter + Amp Envelope)
    // horizontally within the tab, splitting the leftover space evenly. Shifting
    // both anchors by the same offset preserves every box size and gap.
    {
        int clusterWidth  = (rightX + narrowStripWidth) - leftX;
        int clusterOffset = juce::jmax(0, (getWidth() - clusterWidth) / 2 - leftX);
        leftX  += clusterOffset;
        rightX += clusterOffset;
    }
    // Cosmetic: set the gap between the two boxes equal to the Effects tab's
    // box-to-box distance (EffectsPageComponent::resized() colGap == 8) so the
    // horizontal spacing between boxes is consistent across tabs. The boxes are
    // only RELOCATED (leftSideWidth / narrowStripWidth are unchanged), so box
    // sizes and their internal contents stay exactly as before. The native gap
    // here is leftRightGap, so shift each box out by half the difference; the
    // symmetric shift keeps the cluster centred.
    {
        const int effectsColGap = 8;   // == EffectsPageComponent::resized() colGap
        const int boxScoot = juce::jlimit(0, juce::jmax(0, leftX - outerMargin),
                                          (effectsColGap - leftRightGap) / 2);
        leftX  -= boxScoot;
        rightX += boxScoot;
    }
    int topY = outerMargin;
    int oscY = topY;
    int filterY = oscY + oscHeight + topBottomGap;
    
    // ============================================================================
    // LEFT SIDE - TOP: OSCILLATORS SECTION (Wide box, full height of top area)
    // ============================================================================
    // Calculate Filter box width to determine its right-side buffer (will be set later)
    // Filter box will use leftSideWidth, so its right-side buffer = rightX - leftX - leftSideWidth = leftRightGap
    // Make Oscillators box have the same right-side buffer as Filter box
    // Since Filter uses leftSideWidth, Oscillators should also use leftSideWidth to match
    int oscWidth = leftSideWidth;  // Match Filter box width to ensure same right-side buffer
    auto oscArea = juce::Rectangle<int>(leftX, oscY, oscWidth, oscHeight);
    parentEditor.oscillatorsGroup.setBounds(oscArea);
    
    auto oscContent = oscArea.reduced(groupPadding, groupTitleHeight + groupPadding);
    // Cosmetic: center the control block horizontally within the box. The block
    // runs from the combo column (left) to the Level knob (right); shifting the
    // content origin moves every control together since they all derive from
    // oscContent.getX().
    {
        const int controlBlockWidth = comboWidth + horizontalSpacing
                                     + 2 * (knobDiameter + horizontalSpacing) + knobDiameter;
        oscContent.translate(juce::jmax(0, (oscContent.getWidth() - controlBlockWidth) / 2), 0);
    }
    // Reduce top padding by ~50%: move content up by topPaddingReduction (from 65px to ~33px top padding)
    int oscStartY = oscContent.getY() - topPaddingReduction;
    
    // Consistent spacing constants for Oscillators section
    const int oscRowSpacing = 28; // Vertical spacing between oscillator rows (compact)

    // Cosmetic lift for the knob groups. The knob title labels (Coarse / Detune /
    // Level, Low Shelf/Cut / High Shelf/Cut / Level) must sit flush with the row
    // label (Waveform 1 / Waveform 2 / Noize Type), which starts
    // (labelHeight + labelGap) above the combo box. Both use the same font and
    // label height, so an equal top Y reads as one line.
    // Row flow below still uses the unlifted knob bottom, so no other element,
    // and no box height derived from it, moves.
    const int oscKnobLift = (comboHeight - knobDiameter) / 2 + labelHeight + labelGap;

    // The button that opens the Waveforms window sits in the gap that already
    // exists between the combo column and the first knob column (horizontalSpacing
    // is 50 px), so nothing else moves and no width has to be recalculated.
    // 6 + 40 leaves four pixels before the knob column starts.
    const int waveEditButtonWidth = 40;
    const int waveEditButtonSize = 22;
    const int waveEditButtonGap = 6;

    // Osc1 - Waveform combo + 3 knobs horizontally
    int osc1Y = oscStartY;
    parentEditor.osc1WaveformLabel.setBounds(oscContent.getX(), osc1Y, comboWidth, labelHeight);
    osc1Y += labelHeight + labelGap;
    parentEditor.osc1WaveformCombo.setBounds(oscContent.getX(), osc1Y, comboWidth, comboHeight);
    parentEditor.osc1WaveformEditButton.setBounds(
        oscContent.getX() + comboWidth + waveEditButtonGap,
        osc1Y + (comboHeight - waveEditButtonSize) / 2,
        waveEditButtonWidth, waveEditButtonSize);


    // Pan slider in green box area (below waveform combo, left of knobs)
    const int panSliderHeight = 20;
    int osc1PanY = osc1Y + comboHeight + 4;
    parentEditor.osc1PanSlider.setBounds(oscContent.getX(), osc1PanY, comboWidth, panSliderHeight);
    parentEditor.osc1PanLabel.setBounds(oscContent.getX(), osc1PanY + panSliderHeight + 2, comboWidth, labelHeight);
    
    // Knobs shifted down so title labels (Coarse, Detune, Level) sit above â€” matches Filter Cutoff/Resonance
    int osc1KnobRowY = osc1Y + (comboHeight - knobDiameter) / 2 + labelHeight + oscKnobLabelGap; // row-flow anchor
    int osc1KnobY = osc1KnobRowY - oscKnobLift;
    int osc1KnobX = oscContent.getX() + comboWidth + horizontalSpacing;
    
    parentEditor.osc1CoarseTuneLabel.setBounds(osc1KnobX, osc1KnobY - labelHeight - oscKnobLabelGap, knobDiameter, labelHeight);
    parentEditor.osc1CoarseTuneSlider.setBounds(osc1KnobX, osc1KnobY, knobDiameter, knobDiameter);
    
    osc1KnobX += knobDiameter + horizontalSpacing;
    parentEditor.osc1DetuneLabel.setBounds(osc1KnobX, osc1KnobY - labelHeight - oscKnobLabelGap, knobDiameter, labelHeight);
    parentEditor.osc1DetuneSlider.setBounds(osc1KnobX, osc1KnobY, knobDiameter, knobDiameter);
    
    osc1KnobX += knobDiameter + horizontalSpacing;
    parentEditor.osc1LevelLabel.setBounds(osc1KnobX, osc1KnobY - labelHeight - oscKnobLabelGap, knobDiameter, labelHeight);
    parentEditor.osc1LevelSlider.setBounds(osc1KnobX, osc1KnobY, knobDiameter, knobDiameter);
    
    // Bottom of row: knob + value text box (no title label below)
    int osc1Bottom = osc1KnobRowY + knobDiameter + oscLabelSpacing;


    // Osc2 - Same layout, below Osc1 with proper spacing to prevent overlaps
    int osc2Y = osc1Bottom + oscRowSpacing;
    parentEditor.osc2WaveformLabel.setBounds(oscContent.getX(), osc2Y, comboWidth, labelHeight);
    osc2Y += labelHeight + labelGap;
    parentEditor.osc2WaveformCombo.setBounds(oscContent.getX(), osc2Y, comboWidth, comboHeight);
    parentEditor.osc2WaveformEditButton.setBounds(
        oscContent.getX() + comboWidth + waveEditButtonGap,
        osc2Y + (comboHeight - waveEditButtonSize) / 2,
        waveEditButtonWidth, waveEditButtonSize);


    // Pan slider in green box area (below waveform combo, left of knobs)
    int osc2PanY = osc2Y + comboHeight + 4;
    parentEditor.osc2PanSlider.setBounds(oscContent.getX(), osc2PanY, comboWidth, panSliderHeight);
    parentEditor.osc2PanLabel.setBounds(oscContent.getX(), osc2PanY + panSliderHeight + 2, comboWidth, labelHeight);
    
    int osc2KnobRowY = osc2Y + (comboHeight - knobDiameter) / 2 + labelHeight + oscKnobLabelGap; // row-flow anchor
    int osc2KnobY = osc2KnobRowY - oscKnobLift;
    int osc2KnobX = oscContent.getX() + comboWidth + horizontalSpacing;
    
    parentEditor.osc2CoarseTuneLabel.setBounds(osc2KnobX, osc2KnobY - labelHeight - oscKnobLabelGap, knobDiameter, labelHeight);
    parentEditor.osc2CoarseTuneSlider.setBounds(osc2KnobX, osc2KnobY, knobDiameter, knobDiameter);
    
    osc2KnobX += knobDiameter + horizontalSpacing;
    parentEditor.osc2DetuneLabel.setBounds(osc2KnobX, osc2KnobY - labelHeight - oscKnobLabelGap, knobDiameter, labelHeight);
    parentEditor.osc2DetuneSlider.setBounds(osc2KnobX, osc2KnobY, knobDiameter, knobDiameter);
    
    osc2KnobX += knobDiameter + horizontalSpacing;
    parentEditor.osc2LevelLabel.setBounds(osc2KnobX, osc2KnobY - labelHeight - oscKnobLabelGap, knobDiameter, labelHeight);
    parentEditor.osc2LevelSlider.setBounds(osc2KnobX, osc2KnobY, knobDiameter, knobDiameter);
    
    int osc2Bottom = osc2KnobRowY + knobDiameter + oscLabelSpacing;

    // Noise - Below Osc2 with proper padding
    // Match the Osc1->Osc2 row gap (oscRowSpacing) so the Waveform2->Noise distance is identical
    const int noisePadding = oscRowSpacing; // Padding between osc2 and noise section
    int noiseY = osc2Bottom + noisePadding;
    parentEditor.noiseColorLabel.setBounds(oscContent.getX(), noiseY, comboWidth, labelHeight);
    noiseY += labelHeight + labelGap;
    parentEditor.noiseColorCombo.setBounds(oscContent.getX(), noiseY, comboWidth, comboHeight);
    parentEditor.noiseWaveformEditButton.setBounds(
        oscContent.getX() + comboWidth + waveEditButtonGap,
        noiseY + (comboHeight - waveEditButtonSize) / 2,
        waveEditButtonWidth, waveEditButtonSize);


    int noiseKnobRowY = noiseY + (comboHeight - knobDiameter) / 2 + labelHeight + oscKnobLabelGap; // row-flow anchor
    int noiseKnobY = noiseKnobRowY - oscKnobLift;
    int noiseKnobX = oscContent.getX() + comboWidth + horizontalSpacing;  // First knob column

    // Noise EQ knobs occupy the first two columns; Level sits in the third column
    // so it aligns with the Osc1/Osc2 Level knobs (rightmost of the three).
    int eqKnob1X = noiseKnobX;                                        // Low Shelf/Cut (first column)
    int eqKnob2X = eqKnob1X + knobDiameter + horizontalSpacing;       // High Shelf/Cut (second column)
    int noiseLevelX = eqKnob2X + knobDiameter + horizontalSpacing;    // Level (third column)
    const int eqLabelWidth = 90;  // "Low Shelf/Cut" / "High Shelf/Cut" â€” wider than knob
    int lowShelfLabelX = eqKnob1X + (knobDiameter - eqLabelWidth) / 2;
    int highShelfLabelX = eqKnob2X + (knobDiameter - eqLabelWidth) / 2;
    parentEditor.lowShelfAmountLabel.setBounds(lowShelfLabelX, noiseKnobY - labelHeight - oscKnobLabelGap, eqLabelWidth, labelHeight);
    parentEditor.lowShelfAmountSlider.setBounds(eqKnob1X, noiseKnobY, knobDiameter, knobDiameter);
    parentEditor.highShelfAmountLabel.setBounds(highShelfLabelX, noiseKnobY - labelHeight - oscKnobLabelGap, eqLabelWidth, labelHeight);
    parentEditor.highShelfAmountSlider.setBounds(eqKnob2X, noiseKnobY, knobDiameter, knobDiameter);

    parentEditor.noiseLevelLabel.setBounds(noiseLevelX, noiseKnobY - labelHeight - oscKnobLabelGap, knobDiameter, labelHeight);
    parentEditor.noiseLevelSlider.setBounds(noiseLevelX, noiseKnobY, knobDiameter, knobDiameter);
    parentEditor.noiseLevelSlider.setVisible(true);
    parentEditor.noiseLevelLabel.setVisible(true);
    parentEditor.lowShelfAmountSlider.setVisible(true);
    parentEditor.lowShelfAmountLabel.setVisible(true);
    parentEditor.highShelfAmountSlider.setVisible(true);
    parentEditor.highShelfAmountLabel.setVisible(true);
    
    // ============================================================================
    // RIGHT SIDE: AMP ENVELOPE (Tall, narrow vertical column)
    // Master section is now handled by main editor, always visible on right side
    // Box shrinks/expands based on Sub Oscillator toggle (like Filter in Effects tab)
    // Labelâ†’knobâ†’value: same rhythm as Oscillators (oscKnobLabelGap + oscLabelSpacing).
    // Between rows/sections: ampVerticalSpacing (tighter than default verticalSpacing) to save height.
    // ============================================================================
    // Use temp full height to compute content positions, then set final height
    auto ampEnvAreaTemp = juce::Rectangle<int>(rightX, topY, narrowStripWidth, availableHeight);
    auto ampEnvContent = ampEnvAreaTemp.reduced(groupPadding, groupTitleHeight + groupPadding);
    int ampEnvKnobX = ampEnvContent.getCentreX() - knobDiameter / 2; // Centered
    // Reduce top padding by ~50%: move content up by topPaddingReduction
    int ampEnvKnobY = ampEnvContent.getY() - topPaddingReduction;
    const int ampVerticalSpacing = juce::jmax(2, verticalSpacing / 2); // compact gap between ADSR / pitch / sub blocks only
    
    auto layoutAmpEnvKnob = [&](juce::Slider& knob, juce::Label& label)
    {
        label.setBounds(ampEnvKnobX, ampEnvKnobY, knobDiameter, labelHeight);
        ampEnvKnobY += labelHeight + oscKnobLabelGap; // match Oscillators: label bottom â†’ knob top
        knob.setBounds(ampEnvKnobX, ampEnvKnobY, knobDiameter, knobDiameter);
        ampEnvKnobY += knobDiameter + oscLabelSpacing + ampVerticalSpacing; // knob â†’ value area (osc) + tight row gap
    };
    layoutAmpEnvKnob(parentEditor.envAttackSlider, parentEditor.envAttackLabel);
    layoutAmpEnvKnob(parentEditor.envDecaySlider, parentEditor.envDecayLabel);
    layoutAmpEnvKnob(parentEditor.envSustainSlider, parentEditor.envSustainLabel);
    layoutAmpEnvKnob(parentEditor.envReleaseSlider, parentEditor.envReleaseLabel);
    
    // Pitch envelope: 3 knobs in a row (Amount, Time, Pitch) â€” labels above knobs
    const int pitchEnvKnobSize = 56;
    // 56Ã—56 bounds match other Main-tab rotaries (LAF splits dial + textbox inside the rect).
    // Place Sub Osc using LookAndFeel-measured textBoxBounds (hardcoded gaps drift vs actual layout).
    int pitchEnvTotalWidth = 3 * pitchEnvKnobSize + 2 * 10;  // 3 knobs + 2 gaps
    int pitchEnvStartX = ampEnvContent.getCentreX() - pitchEnvTotalWidth / 2;
    int pitchEnvGap = 10;
    int pitchEnvKnob2X = pitchEnvStartX + pitchEnvKnobSize + pitchEnvGap;
    int pitchEnvKnob3X = pitchEnvKnob2X + pitchEnvKnobSize + pitchEnvGap;
    int pitchEnvRowTop = ampEnvKnobY;
    parentEditor.pitchEnvAmountLabel.setBounds(pitchEnvStartX, pitchEnvRowTop, pitchEnvKnobSize, labelHeight);
    parentEditor.pitchEnvTimeLabel.setBounds(pitchEnvKnob2X, pitchEnvRowTop, pitchEnvKnobSize, labelHeight);
    parentEditor.pitchEnvPitchLabel.setBounds(pitchEnvKnob3X, pitchEnvRowTop, pitchEnvKnobSize, labelHeight);
    int pitchEnvKnobY = pitchEnvRowTop + labelHeight + oscKnobLabelGap;
    parentEditor.pitchEnvAmountSlider.setBounds(pitchEnvStartX, pitchEnvKnobY, pitchEnvKnobSize, pitchEnvKnobSize);
    parentEditor.pitchEnvTimeSlider.setBounds(pitchEnvKnob2X, pitchEnvKnobY, pitchEnvKnobSize, pitchEnvKnobSize);
    parentEditor.pitchEnvPitchSlider.setBounds(pitchEnvKnob3X, pitchEnvKnobY, pitchEnvKnobSize, pitchEnvKnobSize);
    // Anchor to full slider component bottom (rotary + value). getSliderLayout().textBoxBounds can disagree
    // with the Slider's actual bounds when a custom LAF draws the dial but uses default layout math.
    const int pitchValueBottom = juce::jmax(
        parentEditor.pitchEnvAmountSlider.getBottom(),
        parentEditor.pitchEnvTimeSlider.getBottom(),
        parentEditor.pitchEnvPitchSlider.getBottom());
    
    // Sub oscillator (below pitch envelope, expandable when toggle is on)
    // Section gap after pitch value text = topBottomGap (same as Oscillators â†” Filter)
    int subOscY = pitchValueBottom + topBottomGap;
    int subOscToggleWidth = 120;  // Wide enough for "Sub Oscillator"
    int subOscToggleHeight = 22;
    parentEditor.subOscToggleButton.setBounds(ampEnvContent.getCentreX() - subOscToggleWidth / 2, subOscY, subOscToggleWidth, subOscToggleHeight);
    
    int subOscKnobsY = subOscY + subOscToggleHeight + topBottomGap;
    int subOscItemWidth = pitchEnvKnobSize;   // Match pitch envelope knob size (60)
    int subOscItemGap = pitchEnvGap;          // Match pitch envelope gap (10)
    int subOscKnobsTotalWidth = 2 * subOscItemWidth + subOscItemGap;  // Level + Coarse only
    int subOscKnobsStartX = ampEnvContent.getCentreX() - subOscKnobsTotalWidth / 2;
    // Row 1: Level and Coarse â€” labels above knobs (match Main tab)
    parentEditor.subOscLevelLabel.setBounds(subOscKnobsStartX, subOscKnobsY, subOscItemWidth, labelHeight);
    parentEditor.subOscCoarseLabel.setBounds(subOscKnobsStartX + subOscItemWidth + subOscItemGap, subOscKnobsY, subOscItemWidth, labelHeight);
    subOscKnobsY += labelHeight + oscKnobLabelGap;
    parentEditor.subOscLevelSlider.setBounds(subOscKnobsStartX, subOscKnobsY, subOscItemWidth, pitchEnvKnobSize);
    parentEditor.subOscCoarseSlider.setBounds(subOscKnobsStartX + subOscItemWidth + subOscItemGap, subOscKnobsY, subOscItemWidth, pitchEnvKnobSize);
    const int subOscValueBottom = juce::jmax(
        parentEditor.subOscLevelSlider.getBottom(),
        parentEditor.subOscCoarseSlider.getBottom());
    // Row 2: Wave â€” topBottomGap after Level/Coarse value text, then label + combo
    // The combo and its Edit button are centred as one unit, so the pair sits
    // under the Level/Coarse knobs rather than the combo alone sitting off to one
    // side of them. The label stays over the combo, which is what it names.
    int subOscWaveLabelTop = subOscValueBottom + topBottomGap;
    int subOscWaveWidth = 80;  // Wide enough for "Triangle" and for a User slot name
    const int subOscEditWidth = 40;
    const int subOscEditHeight = 22;
    const int subOscEditGap = 6;
    int subOscWaveTotalWidth = subOscWaveWidth + subOscEditGap + subOscEditWidth;
    int subOscWaveX = ampEnvContent.getCentreX() - subOscWaveTotalWidth / 2;
    parentEditor.subOscWaveformLabel.setBounds(subOscWaveX, subOscWaveLabelTop, subOscWaveWidth, labelHeight);
    int subOscWaveComboY = subOscWaveLabelTop + labelHeight + 1;
    parentEditor.subOscWaveformCombo.setBounds(subOscWaveX, subOscWaveComboY, subOscWaveWidth, comboHeight);
    parentEditor.subOscWaveformEditButton.setBounds(
        subOscWaveX + subOscWaveWidth + subOscEditGap,
        subOscWaveComboY + (comboHeight - subOscEditHeight) / 2,
        subOscEditWidth, subOscEditHeight);
    
    // Amp Envelope box height: shrink when Sub Osc off, expand when on (like Filter in Effects tab)
    // Bottom inset below last control matches Modulation tab LFO1 box: after the Filter toggle row,
    // ModulationPageComponent uses lfo1FinalH = lfo1CurrentY - modulationContent.getY() + lfoBoxPadV
    // where lfo1CurrentY already includes modRowSpacing below the row â€” same as lfoBoxPadV + modRowSpacing
    // from the bottom of the Filter button to the bottom of the LFO1 group (32 + 4).
    const int modLfoBoxPadV = 32;       // ModulationPageComponent lfoBoxPadV
    const int modRowSpacing = 4;      // ModulationPageComponent modRowSpacing (gap after last row)
    const int ampEnvBottomInset = modLfoBoxPadV + modRowSpacing;
    bool subOscOn = parentEditor.safeGetParam("subOscOn") > 0.5f;
    int ampEnvBottom = subOscOn ? parentEditor.subOscWaveformCombo.getBottom() : (subOscY + subOscToggleHeight);
    int ampEnvHeight = ampEnvBottom - topY + ampEnvBottomInset;
    parentEditor.envelopeGroup.setBounds(rightX, topY, narrowStripWidth, ampEnvHeight);
    
    // Master section is now handled by main editor's resized(), not MainPageComponent
    
    // ============================================================================
    // LEFT SIDE - BOTTOM: FILTER SECTION (Below Oscillators, same width or slightly narrower)
    // ============================================================================
    // Filter box spans width of Oscillators box, positioned below it
    int filterWidth = leftSideWidth;
    auto filterArea = juce::Rectangle<int>(leftX, filterY, filterWidth, filterHeight);
    parentEditor.filterGroup.setBounds(filterArea);
    parentEditor.filterGroup.setVisible(true);
    
    auto filterContent = filterArea.reduced(groupPadding, groupTitleHeight + groupPadding);
    // Cosmetic: center the control block horizontally (matches the Oscillators
    // box). Same block width: combo column (left) to Amount knob (right).
    {
        const int controlBlockWidth = comboWidth + horizontalSpacing
                                     + 2 * (knobDiameter + horizontalSpacing) + knobDiameter;
        filterContent.translate(juce::jmax(0, (filterContent.getWidth() - controlBlockWidth) / 2), 0);
    }

    // Consistent spacing for Filter section
    // Reduce top padding by ~50%: move content up by topPaddingReduction from reduced() result (base padding reduction)
    // Reduce filterTopPadding by 50% as well to maintain proportional spacing (30px -> 15px)
    const int filterTopPadding = 10;  // Compact padding from top of filter content area
    const int filterLabelGap = 6;     // Gap between label and knob
    const int filterRowGap = 12;      // Compact vertical gap between filter rows
    
    // Apply base padding reduction (move up), then add reduced filterTopPadding
    int filterControlY = filterContent.getY() - topPaddingReduction + filterTopPadding;
    
    // Filter Mode: Label above combo
    parentEditor.filterModeLabel.setBounds(filterContent.getX(), filterControlY, comboWidth, labelHeight);
    int filterModeComboY = filterControlY + labelHeight + filterLabelGap;
    parentEditor.filterModeCombo.setBounds(filterContent.getX(), filterModeComboY, comboWidth, comboHeight);
    
    // Filter knobs: Labels above knobs, aligned with Mode combo row
    int filterKnobStartY = filterControlY; // Align labels with Mode label
    int filterKnobX = filterContent.getX() + comboWidth + horizontalSpacing;
    
    // Cutoff knob: Label above
    parentEditor.filterCutoffLabel.setBounds(filterKnobX, filterKnobStartY, knobDiameter, labelHeight);
    int filterCutoffKnobY = filterKnobStartY + labelHeight + filterLabelGap;
    parentEditor.filterCutoffSlider.setBounds(filterKnobX, filterCutoffKnobY, knobDiameter, knobDiameter);
    
    filterKnobX += knobDiameter + horizontalSpacing;
    // Resonance knob: Label above (wider than knob to fit "Resonance" text, centered over knob)
    const int resonanceLabelWidth = 100;  // "Resonance" needs more space than "Cutoff" (was clipping final "e")
    int resonanceLabelX = filterKnobX + (knobDiameter - resonanceLabelWidth) / 2;  // Center label over knob
    parentEditor.filterResonanceLabel.setBounds(resonanceLabelX, filterKnobStartY, resonanceLabelWidth, labelHeight);
    int filterResonanceKnobY = filterKnobStartY + labelHeight + filterLabelGap;
    parentEditor.filterResonanceSlider.setBounds(filterKnobX, filterResonanceKnobY, knobDiameter, knobDiameter);

    // Row below the Cutoff/Resonance knobs. Warm Saturation sits at its left end and
    // Note Lock at its right, so both share this Y and read as one row. Hoisted above
    // the Key Tracking block because Note Lock is positioned there but has to line up
    // with a button laid out further down.
    const int belowFilterKnobsRowY = filterResonanceKnobY + knobDiameter + 12;

    // Key Tracking toggle: centred horizontally over the Filter-Envelope "Amount"
    // knob column (one column right of Resonance), sitting above it. Height matches
    // the Sub Oscillator button (22). Vertically the button centre lines up with the
    // Resonance knob's CIRCLE centre â€” the rotary sits above its value box, so the
    // circle centre is higher than the bounds centre by half the text-box height.
    {
        const int keyTrackW = 86;
        const int keyTrackH = 22;
        const int resTextBoxH = 20;  // matches filterResonanceSlider TextBoxBelow height
        int amountColX = filterKnobX + knobDiameter + horizontalSpacing;  // Amount env-knob column
        int keyTrackX = amountColX + (knobDiameter - keyTrackW) / 2;       // centre button over that column
        int keyTrackY = filterResonanceKnobY + (knobDiameter - resTextBoxH) / 2 - keyTrackH / 2;
        parentEditor.filterKeyTrackButton.setBounds(keyTrackX, keyTrackY, keyTrackW, keyTrackH);

        // Note Lock sits below Key Tracking in the same column, on the Warm Saturation
        // row so the two toggles line up across the box. It only exists while Key
        // Tracking is on -- quantising the cutoff to steps from the played note is only
        // a meaningful idea once the cutoff tracks the note at all. Visibility is set
        // here rather than only in the parameter listener because resized() runs on
        // every relayout and would otherwise reveal a button that should be hidden.
        parentEditor.filterNoteLockButton.setBounds(keyTrackX, belowFilterKnobsRowY,
                                                    keyTrackW, keyTrackH);
        const bool keyTrackOn = parentEditor.safeGetParam("filterKeyTrack") > 0.5f;
        parentEditor.filterNoteLockButton.setVisible(keyTrackOn);

        // Harmonic Series sits immediately left of Note Lock on the same row, centred
        // over the Resonance column the way Note Lock is centred over the Amount one.
        // It is a mode FOR Note Lock, so it only exists while Note Lock is on --
        // nesting the visibility the same way Note Lock nests inside Key Tracking.
        int harmonicX = filterKnobX + (knobDiameter - keyTrackW) / 2;   // filterKnobX == Resonance column
        parentEditor.filterHarmonicLockButton.setBounds(harmonicX, belowFilterKnobsRowY,
                                                        keyTrackW, keyTrackH);
        parentEditor.filterHarmonicLockButton.setVisible(
            keyTrackOn && parentEditor.safeGetParam("filterNoteLock") > 0.5f);
    }

    // Warm Saturation toggle: below resonance row, before Filter Envelope.
    // Shares belowFilterKnobsRowY with Note Lock at the other end of the row.
    filterKnobX = filterContent.getX();
    parentEditor.warmSaturationMasterButton.setBounds(filterKnobX, belowFilterKnobsRowY, 130, 22);
    
    // ============================================================================
    // FILTER ENVELOPE: Position inside Filter box (below Cutoff/Resonance)
    // ============================================================================
    // Based on sketch, Filter Envelope controls are inside Filter box
    // Calculate bottom of filter knobs (including label spacing) for proper spacing to Filter Envelope
    const int filterLabelSpacing = oscillatorTextBoxHeight + (labelGap / 2); // Gap below knob text box (no title label below)
    int filterKnobsBottom = filterCutoffKnobY + knobDiameter + filterLabelSpacing + labelHeight;
    int filterEnvY = filterKnobsBottom + filterRowGap;
    // Align the 4 filter-envelope knobs to the same column grid as the
    // Cutoff/Resonance knobs above (and the Oscillator knob columns): a fixed
    // knobDiameter + horizontalSpacing step. Decay/Release/Amount sit directly
    // under the Cutoff/Resonance/(osc Level) columns; Attack is one step left.
    const int filterEnvColStep = knobDiameter + horizontalSpacing;
    int filterEnvCol1X = filterContent.getX() + comboWidth + horizontalSpacing;  // Cutoff column
    int filterEnvKnobX = filterEnvCol1X - filterEnvColStep;                       // Attack (one column left)
    // Title labels above knobs (match Cutoff/Resonance)
    int filterEnvKnobY = filterEnvY + labelHeight + filterLabelGap;

    parentEditor.filterEnvAttackLabel.setBounds(filterEnvKnobX, filterEnvY, knobDiameter, labelHeight);
    parentEditor.filterEnvAttackSlider.setBounds(filterEnvKnobX, filterEnvKnobY, knobDiameter, knobDiameter);
    filterEnvKnobX += filterEnvColStep;

    parentEditor.filterEnvDecayLabel.setBounds(filterEnvKnobX, filterEnvY, knobDiameter, labelHeight);
    parentEditor.filterEnvDecaySlider.setBounds(filterEnvKnobX, filterEnvKnobY, knobDiameter, knobDiameter);
    filterEnvKnobX += filterEnvColStep;

    parentEditor.filterEnvReleaseLabel.setBounds(filterEnvKnobX, filterEnvY, knobDiameter, labelHeight);
    parentEditor.filterEnvReleaseSlider.setBounds(filterEnvKnobX, filterEnvKnobY, knobDiameter, knobDiameter);
    filterEnvKnobX += filterEnvColStep;

    parentEditor.filterEnvAmountLabel.setBounds(filterEnvKnobX, filterEnvY, knobDiameter, labelHeight);
    parentEditor.filterEnvAmountSlider.setBounds(filterEnvKnobX, filterEnvKnobY, knobDiameter, knobDiameter);

    // ------------------------------------------------------------------
    // Cosmetic: shrink the Filter box so the gap below its last element
    // matches the gap below the last element in the Oscillators box.
    // (Both boxes share the same spacing constants, so the element-bottom
    // definition is identical and the two gaps line up exactly.)
    // ------------------------------------------------------------------
    int oscElementsBottom    = noiseKnobRowY + knobDiameter + oscLabelSpacing;
    int oscBottomGap         = (oscY + oscHeight) - oscElementsBottom;
    int filterElementsBottom = filterEnvKnobY + knobDiameter + filterLabelSpacing;
    int matchedFilterHeight  = (filterElementsBottom - filterY) + oscBottomGap;
    parentEditor.filterGroup.setBounds(leftX, filterY, filterWidth, matchedFilterHeight);

    // Publish the Filter box bottom edge in the editor's coordinate space so the
    // always-visible Master section can line its own bottom up with it. This page
    // sits inside the tabbed component, so add the tab component's Y and this
    // page's own Y (the tab-bar offset) to the filter bottom in page coords.
    parentEditor.filterBoxBottomY = parentEditor.tabbedComponent.getY()
                                  + getY()
                                  + filterY + matchedFilterHeight;

    // Cosmetic: when the Sub Oscillator is expanded, line the Amp Envelope box
    // bottom up with the Filter box bottom (it otherwise stops a touch short).
    // Collapsing behaviour when Sub Osc is off is left untouched - we only ever
    // extend, never shrink below the content-driven height computed above.
    if (subOscOn) {
        int filterBoxBottom      = filterY + matchedFilterHeight;
        int alignedAmpEnvHeight  = filterBoxBottom - topY;
        if (alignedAmpEnvHeight > ampEnvHeight) {
            parentEditor.envelopeGroup.setBounds(rightX, topY, narrowStripWidth, alignedAmpEnvHeight);
        }
    }

    // Hide the separate Filter Envelope group box (it's now inside Filter)
    parentEditor.filterEnvGroup.setBounds(0, 0, 0, 0);
    parentEditor.filterEnvGroup.setVisible(false);
    
    // Ensure all Filter and Filter Envelope controls are visible
    parentEditor.filterModeCombo.setVisible(true);
    parentEditor.filterModeLabel.setVisible(true);
    parentEditor.filterCutoffSlider.setVisible(true);
    parentEditor.filterCutoffLabel.setVisible(true);
    parentEditor.filterResonanceSlider.setVisible(true);
    parentEditor.filterResonanceLabel.setVisible(true);
    parentEditor.warmSaturationMasterButton.setVisible(true);
    parentEditor.filterEnvAttackSlider.setVisible(true);
    parentEditor.filterEnvAttackLabel.setVisible(true);
    parentEditor.filterEnvDecaySlider.setVisible(true);
    parentEditor.filterEnvDecayLabel.setVisible(true);
    parentEditor.filterEnvSustainSlider.setVisible(false);
    parentEditor.filterEnvSustainLabel.setVisible(false);
    parentEditor.filterEnvReleaseSlider.setVisible(true);
    parentEditor.filterEnvReleaseLabel.setVisible(true);
    parentEditor.filterEnvAmountSlider.setVisible(true);
    parentEditor.filterEnvAmountLabel.setVisible(true);
}

//==============================================================================
// -- ModulationPageComponent Implementation --
ModulationPageComponent::ModulationPageComponent(SpaceDustAudioProcessorEditor& editor)
    : parentEditor(editor)
{
    setAccessible(false);
    for (const auto& id : relayoutTriggerParams())
        parentEditor.audioProcessor.getValueTreeState().addParameterListener(id, this);

    // Add all modulation page components as children (no outer Modulation box or title)
    addAndMakeVisible(parentEditor.lfo1Group);
    addAndMakeVisible(parentEditor.lfo1EnabledButton);
    addAndMakeVisible(parentEditor.lfo1WaveformCombo);
    addAndMakeVisible(parentEditor.lfo1WaveformLabel);
    addAndMakeVisible(parentEditor.lfo1TargetCombo);
    addAndMakeVisible(parentEditor.lfo1TargetLabel);
    addAndMakeVisible(parentEditor.lfo1SyncButton);
    addAndMakeVisible(parentEditor.lfo1SyncLabel);
    addAndMakeVisible(parentEditor.lfo1TripletButton);
    addAndMakeVisible(parentEditor.lfo1TripletStraightButton);
    addAndMakeVisible(parentEditor.lfo1FreeRateSlider);
    addAndMakeVisible(parentEditor.lfo1SyncRateCombo);
    addAndMakeVisible(parentEditor.lfo1RateLabel);
    addAndMakeVisible(parentEditor.lfo1RateValueLabel);
    addAndMakeVisible(parentEditor.lfo1DepthSlider);
    addAndMakeVisible(parentEditor.lfo1DepthLabel);
    addAndMakeVisible(parentEditor.lfo1PhaseSlider);
    addAndMakeVisible(parentEditor.lfo1PhaseLabel);
    addAndMakeVisible(parentEditor.lfo1RetriggerButton);
    
    addAndMakeVisible(parentEditor.lfo2Group);
    addAndMakeVisible(parentEditor.lfo2EnabledButton);
    addAndMakeVisible(parentEditor.lfo2WaveformCombo);
    addAndMakeVisible(parentEditor.lfo2WaveformLabel);
    addAndMakeVisible(parentEditor.lfo2TargetCombo);
    addAndMakeVisible(parentEditor.lfo2TargetLabel);
    addAndMakeVisible(parentEditor.lfo2SyncButton);
    addAndMakeVisible(parentEditor.lfo2SyncLabel);
    addAndMakeVisible(parentEditor.lfo2TripletButton);
    addAndMakeVisible(parentEditor.lfo2TripletStraightButton);
    addAndMakeVisible(parentEditor.lfo2FreeRateSlider);
    addAndMakeVisible(parentEditor.lfo2SyncRateCombo);
    addAndMakeVisible(parentEditor.lfo2RateLabel);
    addAndMakeVisible(parentEditor.lfo2RateValueLabel);
    addAndMakeVisible(parentEditor.lfo2DepthSlider);
    addAndMakeVisible(parentEditor.lfo2DepthLabel);
    addAndMakeVisible(parentEditor.lfo2PhaseSlider);
    addAndMakeVisible(parentEditor.lfo2PhaseLabel);
    addAndMakeVisible(parentEditor.lfo2RetriggerButton);
    
    // The Assign button for each LFO, top-right of its panel. Built in the
    // editor's constructor ahead of this page, so they exist by now.
    for (auto* assignButton : parentEditor.lfoAssignButtons)
        addAndMakeVisible(assignButton);

    addAndMakeVisible(parentEditor.modFilterShowButton);
    addAndMakeVisible(parentEditor.modFilterShowButton2);
    addAndMakeVisible(parentEditor.modFilter1Group);
    addAndMakeVisible(parentEditor.modFilter1LinkButton);
    addAndMakeVisible(parentEditor.modFilter1ModeCombo);
    addAndMakeVisible(parentEditor.modFilter1CutoffSlider);
    addAndMakeVisible(parentEditor.modFilter1ResonanceSlider);
    addAndMakeVisible(parentEditor.warmSaturationMod1Button);
    addAndMakeVisible(parentEditor.modFilter1KeyTrackButton);
    addChildComponent(parentEditor.modFilter1NoteLockButton);     // shown by resized() when Key Tracking is on
    addChildComponent(parentEditor.modFilter1HarmonicLockButton); // shown by resized() when Note Lock is on
    addAndMakeVisible(parentEditor.modFilter1ModeLabel);
    addAndMakeVisible(parentEditor.modFilter1CutoffLabel);
    addAndMakeVisible(parentEditor.modFilter1ResonanceLabel);
    addAndMakeVisible(parentEditor.modFilter2Group);
    addAndMakeVisible(parentEditor.modFilter2LinkButton);
    addAndMakeVisible(parentEditor.modFilter2ModeCombo);
    addAndMakeVisible(parentEditor.modFilter2CutoffSlider);
    addAndMakeVisible(parentEditor.modFilter2ResonanceSlider);
    addAndMakeVisible(parentEditor.warmSaturationMod2Button);
    addAndMakeVisible(parentEditor.modFilter2KeyTrackButton);
    addChildComponent(parentEditor.modFilter2NoteLockButton);     // shown by resized() when Key Tracking is on
    addChildComponent(parentEditor.modFilter2HarmonicLockButton); // shown by resized() when Note Lock is on
    addAndMakeVisible(parentEditor.modFilter2ModeLabel);
    addAndMakeVisible(parentEditor.modFilter2CutoffLabel);
    addAndMakeVisible(parentEditor.modFilter2ResonanceLabel);

    // MPE controls
    addAndMakeVisible(parentEditor.mpeGroup);
    addAndMakeVisible(parentEditor.mpeModeCombo);
    addAndMakeVisible(parentEditor.mpeModeLabel);
    addAndMakeVisible(parentEditor.mpePitchBendRangeSlider);
    addAndMakeVisible(parentEditor.mpePitchBendRangeLabel);
    addAndMakeVisible(parentEditor.mpePressureDepthSlider);
    addAndMakeVisible(parentEditor.mpePressureDepthLabel);
    addAndMakeVisible(parentEditor.mpeTimbreDepthSlider);
    addAndMakeVisible(parentEditor.mpeTimbreDepthLabel);
}

ModulationPageComponent::~ModulationPageComponent()
{
    for (const auto& id : relayoutTriggerParams())
        parentEditor.audioProcessor.getValueTreeState().removeParameterListener(id, this);
}

juce::StringArray ModulationPageComponent::relayoutTriggerParams()
{
    // Params whose value changes what this page shows, so resized() has to re-run:
    //   *Show      -- whether each mod filter's controls exist at all
    //   *KeyTrack  -- whether that filter's Note Lock toggle is offered
    //   *NoteLock  -- whether that filter's Harmonic Series toggle is offered
    //   *Link      -- which filter's toggles the shown ones reflect
    // Registration and removal share this list so the two can never drift apart.
    //   voiceMode  -- LFO key tracking only exists in Mono/Legato
    //   lfo*Sync   -- and only with the LFO free-running
    //   lfo*KeyTrack / lfo*NoteLock -- each gates the next toggle in the row
    return { "modFilter1Show",         "modFilter2Show",
             "filterKeyTrack",         "filterNoteLock",
             "modFilter1KeyTrack",     "modFilter2KeyTrack",
             "modFilter1NoteLock",     "modFilter2NoteLock",
             "modFilter1LinkToMaster", "modFilter2LinkToMaster",
             "voiceMode",
             "lfo1Sync",               "lfo2Sync" };
}

void ModulationPageComponent::parameterChanged(const juce::String& parameterID, float newValue)
{
    juce::ignoreUnused(parameterID, newValue);

    // Every param we subscribe to resolves to the same response: relayout. APVTS
    // delivers this on the audio thread under host automation, so marshal it.
    juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<ModulationPageComponent>(this)]
    {
        if (safeThis != nullptr)
        {
            safeThis->resized();
            safeThis->repaint();
        }
    });
}

void ModulationPageComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0a0a1f));
    float avgLevel = parentEditor.getGlowMeterLevel();  // single per-frame averaged L/R snapshot
    drawStarfield(g, getWidth(), getHeight(), avgLevel);
    const int baseAlpha = 10 + static_cast<int>(48.0f * avgLevel);
    drawGlows(g, baseAlpha, meterLinkedGroupGlowHue(parentEditor.clippingHoldTicks > 0),
        { &parentEditor.modulationGroup, &parentEditor.lfo1Group, &parentEditor.lfo2Group,
          &parentEditor.modFilter1Group, &parentEditor.modFilter2Group, &parentEditor.mpeGroup });
}

void ModulationPageComponent::resized()
{
    // Modulation page layout: MPE strip at top, then LFO1 and LFO2 below
    const int outerMargin = 8;
    const int mpeStripH = 110; // MPE controls strip at top (fits rotary + text box like LFO knobs)
    const int mpeGap = 6;

    // Cosmetic: center the box cluster (MPE strip + LFO1 + LFO2) horizontally.
    // All three share the same left edge (outerMargin) and right edge, so one
    // offset centers the whole column while preserving box sizes and gaps.
    const int modContentRightEdge = getWidth() - outerMargin - 12;
    const int modGapToRight       = static_cast<int>(((getWidth() - 2 * outerMargin - 12) / 1.1) * 0.05);
    const int modClusterRightEdge = modContentRightEdge - modGapToRight;
    const int modClusterOffset    = juce::jmax(0, (getWidth() - (modClusterRightEdge - outerMargin)) / 2 - outerMargin);

    //==============================================================================
    // -- MPE Section (horizontal strip at top of Modulation tab) --
    {
        // Match LFO 2's right edge: same trim that LFO layout below applies via lfoGapToRight.
        const int mpeBaseWidth = getWidth() - 2 * outerMargin - 12;
        const int mpeRightTrim = static_cast<int>((mpeBaseWidth / 1.1) * 0.05);
        int mpeX = outerMargin + modClusterOffset;
        int mpeY = 8;
        int mpeW = mpeBaseWidth - mpeRightTrim;
        parentEditor.mpeGroup.setText("MPE");
        parentEditor.mpeGroup.setBounds(mpeX, mpeY, mpeW, mpeStripH);
        parentEditor.mpeGroup.setVisible(true);

        // Inner content area (clear the group title)
        auto mc = parentEditor.mpeGroup.getBounds().reduced(10, 24);
        int cy = mc.getY();
        int slotW = mc.getWidth() / 4;

        // Slot 1: MPE Mode combo
        const int comboW = 100;
        const int comboH = 22;
        const int lblH = 14;
        const int knobSz = 38;                 // Rotary diameter (matches LFO modRateKnobSize)
        const int knobTextH = 18;              // TextBoxBelow height (matches LFO modTextBoxH)
        const int knobGap = 2;                 // Gap between rotary and text box
        const int knobTotalH = knobSz + knobGap + knobTextH; // Full slider area incl. text box
        int sx = mc.getX();
        int cx = sx + slotW / 2;
        parentEditor.mpeModeLabel.setBounds(cx - comboW / 2, cy, comboW, lblH);
        parentEditor.mpeModeCombo.setBounds(cx - comboW / 2, cy + lblH + 2, comboW, comboH);
        parentEditor.mpeModeLabel.setVisible(true);
        parentEditor.mpeModeCombo.setVisible(true);

        // Label width is wider than the knob so multi-word labels fit
        const int knobLabelW = 90;

        // Slot 2: Bend Range knob
        sx += slotW;
        cx = sx + slotW / 2;
        parentEditor.mpePitchBendRangeLabel.setBounds(cx - knobLabelW / 2, cy, knobLabelW, lblH);
        parentEditor.mpePitchBendRangeSlider.setBounds(cx - knobSz / 2, cy + lblH + 2, knobSz, knobTotalH);
        parentEditor.mpePitchBendRangeLabel.setVisible(true);
        parentEditor.mpePitchBendRangeSlider.setVisible(true);

        // Slot 3: Pressure Depth knob
        sx += slotW;
        cx = sx + slotW / 2;
        parentEditor.mpePressureDepthLabel.setBounds(cx - knobLabelW / 2, cy, knobLabelW, lblH);
        parentEditor.mpePressureDepthSlider.setBounds(cx - knobSz / 2, cy + lblH + 2, knobSz, knobTotalH);
        parentEditor.mpePressureDepthLabel.setVisible(true);
        parentEditor.mpePressureDepthSlider.setVisible(true);

        // Slot 4: Timbre Depth knob
        sx += slotW;
        cx = sx + slotW / 2;
        parentEditor.mpeTimbreDepthLabel.setBounds(cx - knobLabelW / 2, cy, knobLabelW, lblH);
        parentEditor.mpeTimbreDepthSlider.setBounds(cx - knobSz / 2, cy + lblH + 2, knobSz, knobTotalH);
        parentEditor.mpeTimbreDepthLabel.setVisible(true);
        parentEditor.mpeTimbreDepthSlider.setVisible(true);
    }

    // LFO content area starts below the MPE strip
    auto modulationContent = juce::Rectangle<int>(
        outerMargin + modClusterOffset,
        8 + mpeStripH + mpeGap,
        getWidth() - 2 * outerMargin - 12,
        getHeight() - 24 - mpeStripH - mpeGap
    );
    
    // Larger gap between LFO columns so boxes do not intersect
    const int columnGap = 22;  // Increased by 10% from reduced value
    
    const int modRateKnobSize = 38;  // Rate, Depth, Phase (and mod-filter knobs): one consistent rotary size
    const int modRateLabelWidth = 70;  // Width for "1/8 bar", "1/128 bar" etc. - prevents cutoff
    const int modLabelHeight = 14;
    const int modComboHeight = 22;
    const int modComboWidth = 100;
    const int modButtonWidth = 70;
    const int modButtonHeight = 22;
    const int modFilterButtonW = 75;   // Filter toggle (narrower)
    const int modWarmSatButtonW = 128; // Warm Saturation (wider so full text fits)
    const int modLabelGap = 2;
    const int modRowSpacing = 4;
    const int modTextBoxH = 18;       // TextBoxBelow on Depth/Phase (matches slider text box height)
    const int modValueTextH = 14;     // Hz / sync value under Rate knob
    const int gapKnobToValue = 2;     // Space between rotary and value text (Rate + TextBoxBelow)
    const int gapValueToNextLabel = modRowSpacing; // Same gap after Hz or after Depth/Phase value to next label
    // Extra padding inside each LFO box (title now inside box - need space below it)
    const int lfoBoxPadH = 8;
    const int lfoBoxPadV = 32;  // Match groupTitleHeight so content clears in-box title
    const int lfoContentTop = 0;
    
    // Check if filter sections are shown (each LFO has its own toggle)
    bool modFilter1Show = parentEditor.audioProcessor.getValueTreeState().getParameter("modFilter1Show") != nullptr
        && parentEditor.safeGetParam("modFilter1Show") > 0.5f;
    bool modFilter2Show = parentEditor.audioProcessor.getValueTreeState().getParameter("modFilter2Show") != nullptr
        && parentEditor.safeGetParam("modFilter2Show") > 0.5f;
    // LFO box heights will be set after layout to shrink-to-fit
    int lfo1AreaHeight = modulationContent.getHeight();
    int lfo2AreaHeight = modulationContent.getHeight();
    
    // Calculate LFO boxes to match Amp Envelope gap exactly
    // Tab width has been expanded by 10%, so modulationContent is now 10% larger
    // Calculate old modulationContent width to preserve the gap (keep absolute gap value the same)
    int oldModulationContentWidth = static_cast<int>(modulationContent.getWidth() / 1.1);  // Reverse the 10% expansion
    // Keep the absolute gap value the same (based on old modulationContent width)
    // This ensures the gap between LFO 2's right edge and tab's right edge stays the same
    int lfoGapToRight = static_cast<int>(oldModulationContentWidth * 0.05);  // Keep same absolute gap value
    
    // Calculate LFO2 width to achieve this gap
    // LFO2 right edge should be at: modulationContent.getRight() - lfoGapToRight
    // This keeps the gap the same absolute value, expanding LFO boxes to fill the space
    int lfo2RightEdge = modulationContent.getRight() - lfoGapToRight;
    // LFO2 starts after LFO1 with columnGap between them
    // We need to calculate: if LFO1 has width W, then LFO2 starts at modulationContent.getX() + W + columnGap
    // And LFO2 width = lfo2RightEdge - (modulationContent.getX() + W + columnGap)
    // But we want LFO1 and LFO2 to have the same width, so:
    // Let W = LFO width (same for both)
    // LFO1: modulationContent.getX() to modulationContent.getX() + W
    // LFO2: modulationContent.getX() + W + columnGap to modulationContent.getX() + W + columnGap + W
    // LFO2 right edge = modulationContent.getX() + W + columnGap + W = modulationContent.getX() + 2*W + columnGap
    // So: modulationContent.getX() + 2*W + columnGap = lfo2RightEdge
    // Therefore: W = (lfo2RightEdge - modulationContent.getX() - columnGap) / 2
    int lfoWidth = (lfo2RightEdge - modulationContent.getX() - columnGap) / 2;
    int lfo2Width = lfoWidth;
    
    // LFO1 Column (Left) - Same width as LFO2, height based on its own Filter toggle
    int lfo1Width = lfoWidth;  // Same width as LFO2 (calculated above)
    int lfo1X = modulationContent.getX();
    auto lfo1Area = juce::Rectangle<int>(
        lfo1X,
        modulationContent.getY(),
        lfo1Width,
        lfo1AreaHeight
    );
    parentEditor.lfo1Group.setBounds(lfo1Area);
    
    auto lfo1Content = lfo1Area.reduced(lfoBoxPadH, lfoBoxPadV);
    int lfo1CurrentY = lfo1Content.getY() + lfoContentTop;
    int controlWidth = modComboWidth;
    int lfo1CentreX = lfo1Content.getX() + lfo1Content.getWidth() / 2;
    const int modOnBtnW = 62;
    const int modOnBtnH = 28;
    
    // LFO1 On button (upper-left like Effects tab, larger for visibility)
    parentEditor.lfo1EnabledButton.setBounds(lfo1Content.getX(), lfo1CurrentY, modOnBtnW, modOnBtnH);

    // Assign, on the same row at the far right. That row held only the On button,
    // so nothing below it moves to make room.
    const int assignBtnW = 74;
    if (parentEditor.lfoAssignButtons.size() > 0)
        parentEditor.lfoAssignButtons[0]->setBounds(lfo1Content.getRight() - assignBtnW,
                                                    lfo1CurrentY, assignBtnW, modOnBtnH);

    lfo1CurrentY += modOnBtnH + modRowSpacing;
    
    // LFO1 Destination (Target) - above Waveform (more important)
    parentEditor.lfo1TargetLabel.setBounds(lfo1CentreX - controlWidth / 2, lfo1CurrentY, controlWidth, modLabelHeight);
    lfo1CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo1TargetCombo.setBounds(lfo1CentreX - controlWidth / 2, lfo1CurrentY, controlWidth, modComboHeight);
    lfo1CurrentY += modComboHeight + modRowSpacing;
    
    // LFO1 Waveform
    parentEditor.lfo1WaveformLabel.setBounds(lfo1CentreX - controlWidth / 2, lfo1CurrentY, controlWidth, modLabelHeight);
    lfo1CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo1WaveformCombo.setBounds(lfo1CentreX - controlWidth / 2, lfo1CurrentY, controlWidth, modComboHeight);
    lfo1CurrentY += modComboHeight + modRowSpacing;
    
    // LFO1 Sync
    parentEditor.lfo1SyncLabel.setBounds(lfo1CentreX - modButtonWidth / 2, lfo1CurrentY, modButtonWidth, modLabelHeight);
    lfo1CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo1SyncButton.setBounds(lfo1CentreX - modButtonWidth / 2, lfo1CurrentY, modButtonWidth, modButtonHeight);
    lfo1CurrentY += modButtonHeight + modRowSpacing;
    
    // LFO1 Rate (label + knob + Hz/sync value; spacing matches Depth/Phase rows)
    const int modRateSliderTotalH = modRateKnobSize + gapKnobToValue + modValueTextH;
    parentEditor.lfo1RateLabel.setBounds(lfo1CentreX - modRateKnobSize / 2, lfo1CurrentY, modRateKnobSize, modLabelHeight);
    parentEditor.lfo1RateLabel.setVisible(true);
    lfo1CurrentY += modLabelHeight + modLabelGap;
    const juce::Rectangle<int> lfo1RateKnobBounds (lfo1CentreX - modRateKnobSize / 2, lfo1CurrentY,
                                                   modRateKnobSize, modRateKnobSize);
    parentEditor.lfo1FreeRateSlider.setBounds(lfo1RateKnobBounds);
    parentEditor.lfo1RateValueLabel.setBounds(lfo1CentreX - modRateLabelWidth / 2, lfo1CurrentY + modRateKnobSize + gapKnobToValue, modRateLabelWidth, modValueTextH);
    parentEditor.lfo1RateValueLabel.setVisible(true);   // Shows Hz or sync division
    parentEditor.lfo1RateValueLabel.setAlpha(1.0f);
    parentEditor.lfo1SyncRateCombo.setBounds(lfo1CentreX - controlWidth / 2, lfo1CurrentY, controlWidth, modComboHeight);
    // Triplet button: positioned to the right of Rate knob, vertically centered
    const int tripletButtonSize = 24;
    const int tripletButtonWidth = 50;
    const int tripletButtonGap = 4;
    parentEditor.lfo1TripletButton.setBounds(lfo1CentreX + modRateKnobSize / 2 + tripletButtonGap, lfo1CurrentY + (modRateKnobSize - tripletButtonSize) / 2, tripletButtonWidth, tripletButtonSize);
    // Triplet/Straight toggle button: positioned to the left of Rate knob, vertically centered
    parentEditor.lfo1TripletStraightButton.setBounds(lfo1CentreX - modRateKnobSize / 2 - tripletButtonGap - tripletButtonSize, lfo1CurrentY + (modRateKnobSize - tripletButtonSize) / 2, tripletButtonSize, tripletButtonSize);
    lfo1CurrentY += modRateSliderTotalH + gapValueToNextLabel;

    // LFO1 Key Tracking / Note Lock / Harmonics, arranged around the Rate knob the
    // same way the filter arranges its three. Only exists with Sync off and in
    // Mono/Legato; Note Lock nests inside Key Tracking, Harmonics inside Note Lock.
    
    // LFO1 Depth
    const int modRotaryTextBoxTotalH = modRateKnobSize + gapKnobToValue + modTextBoxH;
    parentEditor.lfo1DepthLabel.setBounds(lfo1CentreX - modRateKnobSize / 2, lfo1CurrentY, modRateKnobSize, modLabelHeight);
    lfo1CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo1DepthSlider.setBounds(lfo1CentreX - modRateKnobSize / 2, lfo1CurrentY, modRateKnobSize, modRotaryTextBoxTotalH);
    lfo1CurrentY += modRotaryTextBoxTotalH + gapValueToNextLabel;
    
    // LFO1 Phase
    parentEditor.lfo1PhaseLabel.setBounds(lfo1CentreX - modRateKnobSize / 2, lfo1CurrentY, modRateKnobSize, modLabelHeight);
    lfo1CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo1PhaseSlider.setBounds(lfo1CentreX - modRateKnobSize / 2, lfo1CurrentY, modRateKnobSize, modRotaryTextBoxTotalH);
    lfo1CurrentY += modRotaryTextBoxTotalH + modRowSpacing;
    
    // LFO1 Retrigger button (below Phase knob)
    parentEditor.lfo1RetriggerButton.setBounds(lfo1CentreX - modButtonWidth / 2, lfo1CurrentY, modButtonWidth, modButtonHeight);
    lfo1CurrentY += modButtonHeight + modRowSpacing;
    
    // LFO1 Filter button; when on, Warm Saturation next to it (only visible when filter is on)
    const int modFilterRowGap = 6;
    if (modFilter1Show)
    {
        int modRowW = modFilterButtonW + modFilterRowGap + modWarmSatButtonW;
        int modRowLeft = lfo1CentreX - modRowW / 2;
        parentEditor.modFilterShowButton.setBounds(modRowLeft, lfo1CurrentY, modFilterButtonW, modButtonHeight);
        parentEditor.warmSaturationMod1Button.setBounds(modRowLeft + modFilterButtonW + modFilterRowGap, lfo1CurrentY, modWarmSatButtonW, modButtonHeight);
        parentEditor.warmSaturationMod1Button.setVisible(true);
    }
    else
    {
        parentEditor.modFilterShowButton.setBounds(lfo1CentreX - modFilterButtonW / 2, lfo1CurrentY, modFilterButtonW, modButtonHeight);
        parentEditor.warmSaturationMod1Button.setVisible(false);
    }
    lfo1CurrentY += modButtonHeight + modRowSpacing;
    
    // LFO1 Filter controls (when filter shown): Cutoff, Resonance, Mode dropdown, Link to master
    // Rotary must use modRotaryTextBoxTotalH like Depth/Phase â€” TextBoxBelow in a square bounds shrinks the knob
    if (modFilter1Show)
    {
        const int filterKnobSize = modRateKnobSize;  // Rotary width matches Rate/Depth/Phase
        const int filterKnobGap = 28;                // Wider gap so the full "Resonance" label fits
        const int filterLabelW = 62;                 // Label wider than the knob to show "Resonance" uncut
        const int filterComboW = 90;
        const int filterComboH = 20;
        int filterPairLeft = lfo1CentreX - (2 * filterKnobSize + filterKnobGap) / 2;  // Center pair under Filter button
        int resX = filterPairLeft + filterKnobSize + filterKnobGap;
        const int cutoffCentre = filterPairLeft + filterKnobSize / 2;
        const int resCentre = resX + filterKnobSize / 2;
        // Labels are centred on each knob but wider than it, so long words ("Resonance") aren't clipped.
        parentEditor.modFilter1CutoffLabel.setBounds(cutoffCentre - filterLabelW / 2, lfo1CurrentY, filterLabelW, modLabelHeight);
        parentEditor.modFilter1ResonanceLabel.setBounds(resCentre - filterLabelW / 2, lfo1CurrentY, filterLabelW, modLabelHeight);
        lfo1CurrentY += modLabelHeight + modLabelGap;
        parentEditor.modFilter1CutoffSlider.setBounds(filterPairLeft, lfo1CurrentY, filterKnobSize, modRotaryTextBoxTotalH);
        parentEditor.modFilter1ResonanceSlider.setBounds(resX, lfo1CurrentY, filterKnobSize, modRotaryTextBoxTotalH);
        // Key Tracking toggle: in the open space to the right of the Resonance knob, centred on the
        // rotary. Height matches the Filter / Warm Saturation buttons (modButtonHeight).
        {
            const int ktH = modButtonHeight;
            int ktX = resCentre + filterKnobSize / 2 + 8;
            int ktY = lfo1CurrentY + (filterKnobSize - ktH) / 2;
            int ktW = juce::jmin(84, lfo1Content.getRight() - ktX);
            parentEditor.modFilter1KeyTrackButton.setBounds(ktX, ktY, ktW, ktH);
            // Note Lock directly below, same column. Only while Key Tracking is on --
            // and when this filter is linked to the master, the master's Key Tracking
            // is what the shared toggle reflects, so read the same param the button does.
            const bool linked1 = parentEditor.safeGetParam("modFilter1LinkToMaster") > 0.5f;
            const int noteLockY = ktY + ktH + 12;
            parentEditor.modFilter1NoteLockButton.setBounds(ktX, noteLockY, ktW, ktH);
            const bool kt1 = parentEditor.safeGetParam(linked1 ? "filterKeyTrack" : "modFilter1KeyTrack") > 0.5f;
            parentEditor.modFilter1NoteLockButton.setVisible(kt1);

            // Harmonic Series mirrors Note Lock across the knob pair: same row, same
            // width, but in the empty column to the LEFT of Cutoff. Clamped to the box
            // so a narrow LFO column shrinks it instead of letting it escape the panel.
            int hsX = juce::jmax(lfo1Content.getX(), filterPairLeft - 8 - ktW);
            int hsW = juce::jmin(ktW, filterPairLeft - 8 - hsX);
            parentEditor.modFilter1HarmonicLockButton.setBounds(hsX, noteLockY, hsW, ktH);
            parentEditor.modFilter1HarmonicLockButton.setVisible(
                kt1 && parentEditor.safeGetParam(linked1 ? "filterNoteLock" : "modFilter1NoteLock") > 0.5f);
        }
        lfo1CurrentY += modRotaryTextBoxTotalH + gapValueToNextLabel;
        parentEditor.modFilter1ModeLabel.setBounds(lfo1CentreX - filterComboW / 2, lfo1CurrentY, filterComboW, 12);
        lfo1CurrentY += 14;
        parentEditor.modFilter1ModeCombo.setBounds(lfo1CentreX - filterComboW / 2, lfo1CurrentY, filterComboW, filterComboH);
        lfo1CurrentY += filterComboH + modRowSpacing;
        parentEditor.modFilter1LinkButton.setBounds(lfo1CentreX - 55, lfo1CurrentY, 110, modButtonHeight);
        lfo1CurrentY += modButtonHeight + modRowSpacing;
        parentEditor.modFilter1CutoffSlider.setVisible(true);
        parentEditor.modFilter1CutoffLabel.setVisible(true);
        parentEditor.modFilter1ResonanceSlider.setVisible(true);
        parentEditor.modFilter1ResonanceLabel.setVisible(true);
        parentEditor.modFilter1ModeCombo.setVisible(true);
        parentEditor.modFilter1ModeLabel.setVisible(true);
        parentEditor.modFilter1LinkButton.setVisible(true);
        parentEditor.modFilter1KeyTrackButton.setVisible(true);
    }
    else
    {
        parentEditor.modFilter1CutoffSlider.setVisible(false);
        parentEditor.modFilter1CutoffLabel.setVisible(false);
        parentEditor.modFilter1ResonanceSlider.setVisible(false);
        parentEditor.modFilter1ResonanceLabel.setVisible(false);
        parentEditor.modFilter1ModeCombo.setVisible(false);
        parentEditor.modFilter1ModeLabel.setVisible(false);
        parentEditor.modFilter1LinkButton.setVisible(false);
        parentEditor.modFilter1KeyTrackButton.setVisible(false);
        parentEditor.modFilter1NoteLockButton.setVisible(false);
        parentEditor.modFilter1HarmonicLockButton.setVisible(false);
    }
    
    // Shrink LFO1 box to fit its actual content
    {
        int lfo1FinalH = lfo1CurrentY - modulationContent.getY() + lfoBoxPadV;
        parentEditor.lfo1Group.setBounds(lfo1X, modulationContent.getY(), lfo1Width, lfo1FinalH);
    }

    // LFO2 Column (Right)
    int lfo2X = lfo1X + lfo1Width + columnGap;
    auto lfo2Area = juce::Rectangle<int>(
        lfo2X,
        modulationContent.getY(),
        lfo2Width,
        lfo2AreaHeight
    );
    parentEditor.lfo2Group.setBounds(lfo2Area);
    
    auto lfo2Content = lfo2Area.reduced(lfoBoxPadH, lfoBoxPadV);
    int lfo2CurrentY = lfo2Content.getY() + lfoContentTop;
    int lfo2CentreX = lfo2Content.getX() + lfo2Content.getWidth() / 2;
    
    // LFO2 On button (upper-left like Effects tab, larger for visibility)
    parentEditor.lfo2EnabledButton.setBounds(lfo2Content.getX(), lfo2CurrentY, modOnBtnW, modOnBtnH);

    // Assign, mirroring LFO 1's.
    if (parentEditor.lfoAssignButtons.size() > 1)
        parentEditor.lfoAssignButtons[1]->setBounds(lfo2Content.getRight() - assignBtnW,
                                                    lfo2CurrentY, assignBtnW, modOnBtnH);

    lfo2CurrentY += modOnBtnH + modRowSpacing;
    
    // LFO2 Destination (Target) - above Waveform (more important)
    parentEditor.lfo2TargetLabel.setBounds(lfo2CentreX - controlWidth / 2, lfo2CurrentY, controlWidth, modLabelHeight);
    lfo2CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo2TargetCombo.setBounds(lfo2CentreX - controlWidth / 2, lfo2CurrentY, controlWidth, modComboHeight);
    lfo2CurrentY += modComboHeight + modRowSpacing;
    
    // LFO2 Waveform
    parentEditor.lfo2WaveformLabel.setBounds(lfo2CentreX - controlWidth / 2, lfo2CurrentY, controlWidth, modLabelHeight);
    lfo2CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo2WaveformCombo.setBounds(lfo2CentreX - controlWidth / 2, lfo2CurrentY, controlWidth, modComboHeight);
    lfo2CurrentY += modComboHeight + modRowSpacing;
    
    // LFO2 Sync
    parentEditor.lfo2SyncLabel.setBounds(lfo2CentreX - modButtonWidth / 2, lfo2CurrentY, modButtonWidth, modLabelHeight);
    lfo2CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo2SyncButton.setBounds(lfo2CentreX - modButtonWidth / 2, lfo2CurrentY, modButtonWidth, modButtonHeight);
    lfo2CurrentY += modButtonHeight + modRowSpacing;
    
    // LFO2 Rate (label + knob + Hz/sync value; spacing matches Depth/Phase rows)
    parentEditor.lfo2RateLabel.setBounds(lfo2CentreX - modRateKnobSize / 2, lfo2CurrentY, modRateKnobSize, modLabelHeight);
    parentEditor.lfo2RateLabel.setVisible(true);
    lfo2CurrentY += modLabelHeight + modLabelGap;
    const juce::Rectangle<int> lfo2RateKnobBounds (lfo2CentreX - modRateKnobSize / 2, lfo2CurrentY,
                                                   modRateKnobSize, modRateKnobSize);
    parentEditor.lfo2FreeRateSlider.setBounds(lfo2RateKnobBounds);
    parentEditor.lfo2RateValueLabel.setBounds(lfo2CentreX - modRateLabelWidth / 2, lfo2CurrentY + modRateKnobSize + gapKnobToValue, modRateLabelWidth, modValueTextH);
    parentEditor.lfo2RateValueLabel.setVisible(true);   // Shows Hz or sync division
    parentEditor.lfo2RateValueLabel.setAlpha(1.0f);
    parentEditor.lfo2SyncRateCombo.setBounds(lfo2CentreX - controlWidth / 2, lfo2CurrentY, controlWidth, modComboHeight);
    // Triplet button: positioned to the right of Rate knob, vertically centered
    parentEditor.lfo2TripletButton.setBounds(lfo2CentreX + modRateKnobSize / 2 + tripletButtonGap, lfo2CurrentY + (modRateKnobSize - tripletButtonSize) / 2, tripletButtonWidth, tripletButtonSize);
    // Triplet/Straight toggle button: positioned to the left of Rate knob, vertically centered
    parentEditor.lfo2TripletStraightButton.setBounds(lfo2CentreX - modRateKnobSize / 2 - tripletButtonGap - tripletButtonSize, lfo2CurrentY + (modRateKnobSize - tripletButtonSize) / 2, tripletButtonSize, tripletButtonSize);
    lfo2CurrentY += modRateSliderTotalH + gapValueToNextLabel;

    // LFO2 key-tracking row -- see the LFO1 block above.
    
    // LFO2 Depth
    parentEditor.lfo2DepthLabel.setBounds(lfo2CentreX - modRateKnobSize / 2, lfo2CurrentY, modRateKnobSize, modLabelHeight);
    lfo2CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo2DepthSlider.setBounds(lfo2CentreX - modRateKnobSize / 2, lfo2CurrentY, modRateKnobSize, modRotaryTextBoxTotalH);
    lfo2CurrentY += modRotaryTextBoxTotalH + gapValueToNextLabel;
    
    // LFO2 Phase
    parentEditor.lfo2PhaseLabel.setBounds(lfo2CentreX - modRateKnobSize / 2, lfo2CurrentY, modRateKnobSize, modLabelHeight);
    lfo2CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo2PhaseSlider.setBounds(lfo2CentreX - modRateKnobSize / 2, lfo2CurrentY, modRateKnobSize, modRotaryTextBoxTotalH);
    lfo2CurrentY += modRotaryTextBoxTotalH + modRowSpacing;
    
    // LFO2 Retrigger button (below Phase knob)
    parentEditor.lfo2RetriggerButton.setBounds(lfo2CentreX - modButtonWidth / 2, lfo2CurrentY, modButtonWidth, modButtonHeight);
    lfo2CurrentY += modButtonHeight + modRowSpacing;
    
    // LFO2 Filter button; when on, Warm Saturation next to it (only visible when filter is on)
    if (modFilter2Show)
    {
        int modRowW = modFilterButtonW + modFilterRowGap + modWarmSatButtonW;
        int modRowLeft = lfo2CentreX - modRowW / 2;
        parentEditor.modFilterShowButton2.setBounds(modRowLeft, lfo2CurrentY, modFilterButtonW, modButtonHeight);
        parentEditor.warmSaturationMod2Button.setBounds(modRowLeft + modFilterButtonW + modFilterRowGap, lfo2CurrentY, modWarmSatButtonW, modButtonHeight);
        parentEditor.warmSaturationMod2Button.setVisible(true);
    }
    else
    {
        parentEditor.modFilterShowButton2.setBounds(lfo2CentreX - modFilterButtonW / 2, lfo2CurrentY, modFilterButtonW, modButtonHeight);
        parentEditor.warmSaturationMod2Button.setVisible(false);
    }
    lfo2CurrentY += modButtonHeight + modRowSpacing;

    // LFO2 Filter controls (when filter shown): Cutoff, Resonance, Mode dropdown, Link to master
    // Rotary must use modRotaryTextBoxTotalH like Depth/Phase â€” TextBoxBelow in a square bounds shrinks the knob
    if (modFilter2Show)
    {
        const int filterKnobSize = modRateKnobSize;  // Rotary width matches Rate/Depth/Phase
        const int filterKnobGap = 28;                // Wider gap so the full "Resonance" label fits
        const int filterLabelW = 62;                 // Label wider than the knob to show "Resonance" uncut
        const int filterComboW = 90;
        const int filterComboH = 20;
        int filterPairLeft = lfo2CentreX - (2 * filterKnobSize + filterKnobGap) / 2;  // Center pair under Filter button
        int resX = filterPairLeft + filterKnobSize + filterKnobGap;
        const int cutoffCentre = filterPairLeft + filterKnobSize / 2;
        const int resCentre = resX + filterKnobSize / 2;
        // Labels are centred on each knob but wider than it, so long words ("Resonance") aren't clipped.
        parentEditor.modFilter2CutoffLabel.setBounds(cutoffCentre - filterLabelW / 2, lfo2CurrentY, filterLabelW, modLabelHeight);
        parentEditor.modFilter2ResonanceLabel.setBounds(resCentre - filterLabelW / 2, lfo2CurrentY, filterLabelW, modLabelHeight);
        lfo2CurrentY += modLabelHeight + modLabelGap;
        parentEditor.modFilter2CutoffSlider.setBounds(filterPairLeft, lfo2CurrentY, filterKnobSize, modRotaryTextBoxTotalH);
        parentEditor.modFilter2ResonanceSlider.setBounds(resX, lfo2CurrentY, filterKnobSize, modRotaryTextBoxTotalH);
        // Key Tracking toggle: in the open space to the right of the Resonance knob, centred on the
        // rotary. Height matches the Filter / Warm Saturation buttons (modButtonHeight).
        {
            const int ktH = modButtonHeight;
            int ktX = resCentre + filterKnobSize / 2 + 8;
            int ktY = lfo2CurrentY + (filterKnobSize - ktH) / 2;
            int ktW = juce::jmin(84, lfo2Content.getRight() - ktX);
            parentEditor.modFilter2KeyTrackButton.setBounds(ktX, ktY, ktW, ktH);
            // Note Lock directly below, Harmonic Series mirrored to the left of the knob
            // pair (see the LFO 1 block for the linked-filter and clamping notes).
            const bool linked2 = parentEditor.safeGetParam("modFilter2LinkToMaster") > 0.5f;
            const int noteLockY = ktY + ktH + 12;
            parentEditor.modFilter2NoteLockButton.setBounds(ktX, noteLockY, ktW, ktH);
            const bool kt2 = parentEditor.safeGetParam(linked2 ? "filterKeyTrack" : "modFilter2KeyTrack") > 0.5f;
            parentEditor.modFilter2NoteLockButton.setVisible(kt2);

            int hsX = juce::jmax(lfo2Content.getX(), filterPairLeft - 8 - ktW);
            int hsW = juce::jmin(ktW, filterPairLeft - 8 - hsX);
            parentEditor.modFilter2HarmonicLockButton.setBounds(hsX, noteLockY, hsW, ktH);
            parentEditor.modFilter2HarmonicLockButton.setVisible(
                kt2 && parentEditor.safeGetParam(linked2 ? "filterNoteLock" : "modFilter2NoteLock") > 0.5f);
        }
        lfo2CurrentY += modRotaryTextBoxTotalH + gapValueToNextLabel;
        parentEditor.modFilter2ModeLabel.setBounds(lfo2CentreX - filterComboW / 2, lfo2CurrentY, filterComboW, 12);
        lfo2CurrentY += 14;
        parentEditor.modFilter2ModeCombo.setBounds(lfo2CentreX - filterComboW / 2, lfo2CurrentY, filterComboW, filterComboH);
        lfo2CurrentY += filterComboH + modRowSpacing;
        parentEditor.modFilter2LinkButton.setBounds(lfo2CentreX - 55, lfo2CurrentY, 110, modButtonHeight);
        lfo2CurrentY += modButtonHeight + modRowSpacing;
        parentEditor.modFilter2CutoffSlider.setVisible(true);
        parentEditor.modFilter2CutoffLabel.setVisible(true);
        parentEditor.modFilter2ResonanceSlider.setVisible(true);
        parentEditor.modFilter2ResonanceLabel.setVisible(true);
        parentEditor.modFilter2ModeCombo.setVisible(true);
        parentEditor.modFilter2ModeLabel.setVisible(true);
        parentEditor.modFilter2LinkButton.setVisible(true);
        parentEditor.modFilter2KeyTrackButton.setVisible(true);
    }
    else
    {
        parentEditor.modFilter2CutoffSlider.setVisible(false);
        parentEditor.modFilter2CutoffLabel.setVisible(false);
        parentEditor.modFilter2ResonanceSlider.setVisible(false);
        parentEditor.modFilter2ResonanceLabel.setVisible(false);
        parentEditor.modFilter2ModeCombo.setVisible(false);
        parentEditor.modFilter2ModeLabel.setVisible(false);
        parentEditor.modFilter2LinkButton.setVisible(false);
        parentEditor.modFilter2KeyTrackButton.setVisible(false);
        parentEditor.modFilter2NoteLockButton.setVisible(false);
        parentEditor.modFilter2HarmonicLockButton.setVisible(false);
    }
    
    // Shrink LFO2 box to fit its actual content
    {
        int lfo2FinalH = lfo2CurrentY - modulationContent.getY() + lfoBoxPadV;
        parentEditor.lfo2Group.setBounds(lfo2X, modulationContent.getY(), lfo2Width, lfo2FinalH);
    }

    parentEditor.modFilter1Group.setVisible(false);
    parentEditor.modFilter2Group.setVisible(false);
    parentEditor.modFilterShowLabel.setVisible(false);

}

//==============================================================================
// -- EffectsPageComponent Implementation --
EffectsPageComponent::EffectsPageComponent(SpaceDustAudioProcessorEditor& editor)
    : parentEditor(editor)
{
    setAccessible(false);
    addAndMakeVisible(parentEditor.delayGroup);
    addAndMakeVisible(parentEditor.reverbGroup);
    addAndMakeVisible(parentEditor.delayEnabledButton);
    addAndMakeVisible(parentEditor.delayEnabledLabel);
    addAndMakeVisible(parentEditor.delaySyncButton);
    addAndMakeVisible(parentEditor.delaySyncLabel);
    addAndMakeVisible(parentEditor.delayFreeRateSlider);
    addAndMakeVisible(parentEditor.delaySyncRateCombo);
    addAndMakeVisible(parentEditor.delayRateLabel);
    addAndMakeVisible(parentEditor.delayRateValueLabel);
    addAndMakeVisible(parentEditor.delayDecaySlider);
    addAndMakeVisible(parentEditor.delayDecayLabel);
    addAndMakeVisible(parentEditor.delayDryWetSlider);
    addAndMakeVisible(parentEditor.delayDryWetLabel);
    addAndMakeVisible(parentEditor.delayPingPongButton);
    addAndMakeVisible(parentEditor.delayPingPongLabel);
    addAndMakeVisible(parentEditor.delayFilterShowButton);
    addAndMakeVisible(parentEditor.delayFilterHPCutoffSlider);
    addAndMakeVisible(parentEditor.delayFilterHPResonanceSlider);
    addAndMakeVisible(parentEditor.delayFilterLPCutoffSlider);
    addAndMakeVisible(parentEditor.delayFilterLPResonanceSlider);
    addAndMakeVisible(parentEditor.delayFilterWarmSaturationButton);
    addAndMakeVisible(parentEditor.delayFilterHPCutoffLabel);
    addAndMakeVisible(parentEditor.delayFilterHPResonanceLabel);
    addAndMakeVisible(parentEditor.delayFilterLPCutoffLabel);
    addAndMakeVisible(parentEditor.delayFilterLPResonanceLabel);
    addAndMakeVisible(parentEditor.reverbEnabledButton);
    addAndMakeVisible(parentEditor.reverbEnabledLabel);
    addAndMakeVisible(parentEditor.reverbTypeCombo);
    addAndMakeVisible(parentEditor.reverbTypeLabel);
    addAndMakeVisible(parentEditor.reverbWetMixSlider);
    addAndMakeVisible(parentEditor.reverbWetMixLabel);
    addAndMakeVisible(parentEditor.reverbDecayTimeSlider);
    addAndMakeVisible(parentEditor.reverbDecayTimeLabel);
    addAndMakeVisible(parentEditor.reverbFilterShowButton);
    addAndMakeVisible(parentEditor.reverbFilterWarmSaturationButton);
    addAndMakeVisible(parentEditor.reverbFilterHPCutoffSlider);
    addAndMakeVisible(parentEditor.reverbFilterHPResonanceSlider);
    addAndMakeVisible(parentEditor.reverbFilterLPCutoffSlider);
    addAndMakeVisible(parentEditor.reverbFilterLPResonanceSlider);
    addAndMakeVisible(parentEditor.reverbFilterHPCutoffLabel);
    addAndMakeVisible(parentEditor.reverbFilterHPResonanceLabel);
    addAndMakeVisible(parentEditor.reverbFilterLPCutoffLabel);
    addAndMakeVisible(parentEditor.reverbFilterLPResonanceLabel);
    addAndMakeVisible(parentEditor.grainDelayGroup);
    addAndMakeVisible(parentEditor.grainDelayEnabledButton);
    addAndMakeVisible(parentEditor.grainDelayEnabledLabel);
    addAndMakeVisible(parentEditor.grainDelayTimeSlider);
    addAndMakeVisible(parentEditor.grainDelayTimeLabel);
    addAndMakeVisible(parentEditor.grainDelaySizeSlider);
    addAndMakeVisible(parentEditor.grainDelaySizeLabel);
    addAndMakeVisible(parentEditor.grainDelayPitchSlider);
    addAndMakeVisible(parentEditor.grainDelayPitchLabel);
    addAndMakeVisible(parentEditor.grainDelayMixSlider);
    addAndMakeVisible(parentEditor.grainDelayMixLabel);
    addAndMakeVisible(parentEditor.grainDelayDecaySlider);
    addAndMakeVisible(parentEditor.grainDelayDecayLabel);
    addAndMakeVisible(parentEditor.grainDelayDensitySlider);
    addAndMakeVisible(parentEditor.grainDelayDensityLabel);
    addAndMakeVisible(parentEditor.grainDelayJitterSlider);
    addAndMakeVisible(parentEditor.grainDelayJitterLabel);
    addAndMakeVisible(parentEditor.grainDelayPingPongButton);
    addAndMakeVisible(parentEditor.grainDelayPingPongLabel);
    addAndMakeVisible(parentEditor.grainDelayFilterShowButton);
    addAndMakeVisible(parentEditor.grainDelayFilterHPCutoffSlider);
    addAndMakeVisible(parentEditor.grainDelayFilterHPResonanceSlider);
    addAndMakeVisible(parentEditor.grainDelayFilterLPCutoffSlider);
    addAndMakeVisible(parentEditor.grainDelayFilterLPResonanceSlider);
    addAndMakeVisible(parentEditor.grainDelayFilterWarmSaturationButton);
    addAndMakeVisible(parentEditor.grainDelayFilterHPCutoffLabel);
    addAndMakeVisible(parentEditor.grainDelayFilterHPResonanceLabel);
    addAndMakeVisible(parentEditor.grainDelayFilterLPCutoffLabel);
    addAndMakeVisible(parentEditor.grainDelayFilterLPResonanceLabel);
    addAndMakeVisible(parentEditor.phaserGroup);
    addAndMakeVisible(parentEditor.phaserEnabledButton);
    addAndMakeVisible(parentEditor.phaserEnabledLabel);
    addAndMakeVisible(parentEditor.phaserRateSlider);
    addAndMakeVisible(parentEditor.phaserRateLabel);
    addAndMakeVisible(parentEditor.phaserDepthSlider);
    addAndMakeVisible(parentEditor.phaserDepthLabel);
    addAndMakeVisible(parentEditor.phaserFeedbackSlider);
    addAndMakeVisible(parentEditor.phaserFeedbackLabel);
    addAndMakeVisible(parentEditor.phaserScriptModeButton);
    addAndMakeVisible(parentEditor.phaserScriptModeLabel);
    addAndMakeVisible(parentEditor.phaserMixSlider);
    addAndMakeVisible(parentEditor.phaserMixLabel);
    addAndMakeVisible(parentEditor.phaserCentreSlider);
    addAndMakeVisible(parentEditor.phaserCentreLabel);
    addAndMakeVisible(parentEditor.phaserStagesCombo);
    addAndMakeVisible(parentEditor.phaserStagesLabel);
    addAndMakeVisible(parentEditor.phaserStereoOffsetSlider);
    addAndMakeVisible(parentEditor.phaserStereoOffsetLabel);
    addAndMakeVisible(parentEditor.phaserVintageModeButton);
    addAndMakeVisible(parentEditor.phaserVintageModeLabel);
    addAndMakeVisible(parentEditor.flangerGroup);
    addAndMakeVisible(parentEditor.flangerEnabledButton);
    addAndMakeVisible(parentEditor.flangerEnabledLabel);
    addAndMakeVisible(parentEditor.flangerRateSlider);
    addAndMakeVisible(parentEditor.flangerRateLabel);
    addAndMakeVisible(parentEditor.flangerDepthSlider);
    addAndMakeVisible(parentEditor.flangerDepthLabel);
    addAndMakeVisible(parentEditor.flangerFeedbackSlider);
    addAndMakeVisible(parentEditor.flangerFeedbackLabel);
    addAndMakeVisible(parentEditor.flangerWidthSlider);
    addAndMakeVisible(parentEditor.flangerWidthLabel);
    addAndMakeVisible(parentEditor.flangerMixSlider);
    addAndMakeVisible(parentEditor.flangerMixLabel);
    addAndMakeVisible(parentEditor.tranceGateGroup);
    addAndMakeVisible(parentEditor.tranceGateEnabledButton);
    addAndMakeVisible(parentEditor.tranceGateEnabledLabel);
    addAndMakeVisible(parentEditor.tranceGatePreEffectButton);
    addAndMakeVisible(parentEditor.tranceGatePreEffectLabel);
    addAndMakeVisible(parentEditor.tranceGateStepsCombo);
    addAndMakeVisible(parentEditor.tranceGateStepsLabel);
    addAndMakeVisible(parentEditor.tranceGateSyncButton);
    addAndMakeVisible(parentEditor.tranceGateSyncLabel);
    addAndMakeVisible(parentEditor.tranceGateRateSlider);
    addAndMakeVisible(parentEditor.tranceGateRateLabel);
    addAndMakeVisible(parentEditor.tranceGateAttackSlider);
    addAndMakeVisible(parentEditor.tranceGateReleaseSlider);
    addAndMakeVisible(parentEditor.tranceGateMixSlider);
    addAndMakeVisible(parentEditor.tranceGateAttackLabel);
    addAndMakeVisible(parentEditor.tranceGateReleaseLabel);
    addAndMakeVisible(parentEditor.tranceGateMixLabel);
    addAndMakeVisible(parentEditor.tranceGateStep1Button);
    addAndMakeVisible(parentEditor.tranceGateStep2Button);
    addAndMakeVisible(parentEditor.tranceGateStep3Button);
    addAndMakeVisible(parentEditor.tranceGateStep4Button);
    addAndMakeVisible(parentEditor.tranceGateStep5Button);
    addAndMakeVisible(parentEditor.tranceGateStep6Button);
    addAndMakeVisible(parentEditor.tranceGateStep7Button);
    addAndMakeVisible(parentEditor.tranceGateStep8Button);
    addAndMakeVisible(parentEditor.tranceGateStep9Button);
    addAndMakeVisible(parentEditor.tranceGateStep10Button);
    addAndMakeVisible(parentEditor.tranceGateStep11Button);
    addAndMakeVisible(parentEditor.tranceGateStep12Button);
    addAndMakeVisible(parentEditor.tranceGateStep13Button);
    addAndMakeVisible(parentEditor.tranceGateStep14Button);
    addAndMakeVisible(parentEditor.tranceGateStep15Button);
    addAndMakeVisible(parentEditor.tranceGateStep16Button);
    // Re-layout step buttons when Steps dropdown changes
    parentEditor.tranceGateStepsCombo.onChange = [this]() { resized(); };
    auto& apvts = parentEditor.audioProcessor.getValueTreeState();
    apvts.addParameterListener(juce::ParameterID{"delayFilterShow", 1}.getParamID(), this);
    apvts.addParameterListener(juce::ParameterID{"reverbFilterShow", 1}.getParamID(), this);
    apvts.addParameterListener(juce::ParameterID{"grainDelayFilterShow", 1}.getParamID(), this);
    updateDelayFilterVisibility();
    updateReverbFilterVisibility();
    updateGrainDelayFilterVisibility();
}

EffectsPageComponent::~EffectsPageComponent()
{
    parentEditor.tranceGateStepsCombo.onChange = nullptr;
    auto& apvts = parentEditor.audioProcessor.getValueTreeState();
    apvts.removeParameterListener(juce::ParameterID{"delayFilterShow", 1}.getParamID(), this);
    apvts.removeParameterListener(juce::ParameterID{"reverbFilterShow", 1}.getParamID(), this);
    apvts.removeParameterListener(juce::ParameterID{"grainDelayFilterShow", 1}.getParamID(), this);
    // Listeners are gone, so no new triggers can arrive; drop any update already queued.
    cancelPendingUpdate();
}

void EffectsPageComponent::parameterChanged(const juce::String&, float)
{
    // CAUTION: APVTS calls this synchronously on whatever thread changes the
    // parameter. Under host automation that is the AUDIO thread (the value is
    // applied inside processBlock). The visibility updates below call
    // Component::setVisible()/resized() -> grabKeyboardFocus() -> macOS HIToolbox,
    // which asserts it is on the main thread (dispatch_assert_queue) and aborts
    // with SIGILL when it is not. So NEVER touch the UI here directly: marshal to
    // the message thread. triggerAsyncUpdate() is thread-safe, coalesces a flood
    // of automation changes into a single relayout, and is auto-cancelled if this
    // component is destroyed before it fires. We refresh all three filter sections
    // (each re-reads its own *FilterShow param) so we don't need to track which one
    // changed across the thread hop.
    triggerAsyncUpdate();
}

void EffectsPageComponent::handleAsyncUpdate()
{
    // Always runs on the message thread (AsyncUpdater guarantee) â€” safe for UI.
    updateDelayFilterVisibility();
    updateReverbFilterVisibility();
    updateGrainDelayFilterVisibility();
}

void EffectsPageComponent::updateDelayFilterVisibility()
{
    bool show = parentEditor.safeGetParam("delayFilterShow") > 0.5f;
    parentEditor.delayFilterHPCutoffSlider.setVisible(show);
    parentEditor.delayFilterHPResonanceSlider.setVisible(show);
    parentEditor.delayFilterLPCutoffSlider.setVisible(show);
    parentEditor.delayFilterLPResonanceSlider.setVisible(show);
    parentEditor.delayFilterWarmSaturationButton.setVisible(show);
    parentEditor.delayFilterHPCutoffLabel.setVisible(show);
    parentEditor.delayFilterHPResonanceLabel.setVisible(show);
    parentEditor.delayFilterLPCutoffLabel.setVisible(show);
    parentEditor.delayFilterLPResonanceLabel.setVisible(show);
    resized();  // Re-layout to position filter controls when toggled
}

void EffectsPageComponent::updateGrainDelayFilterVisibility()
{
    bool show = parentEditor.safeGetParam("grainDelayFilterShow") > 0.5f;
    parentEditor.grainDelayFilterHPCutoffSlider.setVisible(show);
    parentEditor.grainDelayFilterHPResonanceSlider.setVisible(show);
    parentEditor.grainDelayFilterLPCutoffSlider.setVisible(show);
    parentEditor.grainDelayFilterLPResonanceSlider.setVisible(show);
    parentEditor.grainDelayFilterWarmSaturationButton.setVisible(show);
    parentEditor.grainDelayFilterHPCutoffLabel.setVisible(show);
    parentEditor.grainDelayFilterHPResonanceLabel.setVisible(show);
    parentEditor.grainDelayFilterLPCutoffLabel.setVisible(show);
    parentEditor.grainDelayFilterLPResonanceLabel.setVisible(show);
    resized();
}

void EffectsPageComponent::updateReverbFilterVisibility()
{
    bool show = parentEditor.safeGetParam("reverbFilterShow") > 0.5f;
    parentEditor.reverbFilterWarmSaturationButton.setVisible(show);
    parentEditor.reverbFilterHPCutoffSlider.setVisible(show);
    parentEditor.reverbFilterHPResonanceSlider.setVisible(show);
    parentEditor.reverbFilterLPCutoffSlider.setVisible(show);
    parentEditor.reverbFilterLPResonanceSlider.setVisible(show);
    parentEditor.reverbFilterHPCutoffLabel.setVisible(show);
    parentEditor.reverbFilterHPResonanceLabel.setVisible(show);
    parentEditor.reverbFilterLPCutoffLabel.setVisible(show);
    parentEditor.reverbFilterLPResonanceLabel.setVisible(show);
    resized();
}

void EffectsPageComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0a0a1f));
    float avgLevel = parentEditor.getGlowMeterLevel();  // single per-frame averaged L/R snapshot
    drawStarfield(g, getWidth(), getHeight(), avgLevel);
    const int baseAlpha = 10 + static_cast<int>(48.0f * avgLevel);
    drawGlows(g, baseAlpha, meterLinkedGroupGlowHue(parentEditor.clippingHoldTicks > 0),
        { &parentEditor.delayGroup, &parentEditor.reverbGroup, &parentEditor.grainDelayGroup,
          &parentEditor.phaserGroup, &parentEditor.flangerGroup, &parentEditor.tranceGateGroup, &parentEditor.delayFilterGroup });
}

void EffectsPageComponent::resized()
{
    auto r = getLocalBounds();
    const int pad = 8;        // Tighter padding
    const int colGap = 8;     // Gap between effect columns
    const int gap = 4;        // Compact spacing between elements
    const int labelGap = 2;
    const int knobSize = 56;  // Uniform knob size for Reverb, Grain Delay, Phaser, Trance Gate
    const int delayKnobSize = 56;  // Same as all other knobs
    const int filterLabelW = 62;  // Matches knob+fGap slot so "HP Cutoff" / "LP Cutoff" fit
    const int btnW = 52;
    const int btnH = 22;
    const int onBtnW = 62;
    const int onBtnH = 28;
    const int labelH = 14;    // Slightly taller to prevent label clipping
    const int groupTitleH = 32;  // Title inside box - keep content below it
    
    // Three columns: Delay+Grain Delay (left) | Reverb+Trance Gate (center) | Phaser+Flanger (right) [Bit Crusher on Saturation Color tab]
    const int colW = (r.getWidth() - 2 * pad - 2 * colGap) / 3;
    int delayColX = pad;
    int reverbColX = pad + colW + colGap;
    int grainColX = pad + 2 * (colW + colGap);
    
    // ---- Delay section (far left) - On button upper-left below label, Time+Decay+Mix in one row ----
    int cx = delayColX + colW / 2;
    int y = groupTitleH;

    auto placeLabel = [&](juce::Label& lbl, int w) { lbl.setBounds(cx - w/2, y, w, labelH); y += labelH + labelGap; };
    
    // On button: upper-left corner, right below the group label (larger for visibility)
    parentEditor.delayEnabledButton.setBounds(delayColX + pad, y, onBtnW, onBtnH);
    y += onBtnH + gap;
    
    placeLabel(parentEditor.delaySyncLabel, btnW);
    parentEditor.delaySyncButton.setBounds(cx - btnW/2, y, btnW, btnH);
    y += btnH + gap;
    
    // Time | Decay | Mix - all three in one row (smaller knobs)
    const int dKg = 6;
    const int dRowW = 3 * delayKnobSize + 2 * dKg;
    int dRowLeft = cx - dRowW / 2;
    parentEditor.delayRateLabel.setBounds(dRowLeft, y, delayKnobSize, labelH);
    parentEditor.delayDecayLabel.setBounds(dRowLeft + delayKnobSize + dKg, y, delayKnobSize, labelH);
    parentEditor.delayDryWetLabel.setBounds(dRowLeft + 2 * (delayKnobSize + dKg), y, delayKnobSize, labelH);
    y += labelH + labelGap;
    // Time knob uses NoTextBox, so give it the same height as the rotary area
    // of TextBoxBelow knobs (delayKnobSize - 18) to match visual size
    const int delayTimeKnobH = delayKnobSize - 18;
    parentEditor.delayFreeRateSlider.setBounds(dRowLeft, y, delayKnobSize, delayTimeKnobH);
    parentEditor.delayDecaySlider.setBounds(dRowLeft + delayKnobSize + dKg, y, delayKnobSize, delayKnobSize);
    parentEditor.delayDryWetSlider.setBounds(dRowLeft + 2 * (delayKnobSize + dKg), y, delayKnobSize, delayKnobSize);
    parentEditor.delayRateValueLabel.setBounds(dRowLeft, y + delayTimeKnobH + 2, delayKnobSize, 12);
    y += delayKnobSize + 12 + gap;
    
    parentEditor.delayPingPongLabel.setVisible(false);
    parentEditor.delayPingPongButton.setBounds(cx - 50, y, 100, btnH);
    y += btnH + gap;
    
    // Filter toggle; when on, Warm Saturation next to it (only visible when filter is on)
    bool filterShow = parentEditor.safeGetParam("delayFilterShow") > 0.5f;
    const int warmSatW = 120;
    const int filterRowGap = 6;
    if (filterShow)
    {
        int rowW = btnW + filterRowGap + warmSatW;
        int rowLeft = cx - rowW / 2;
        parentEditor.delayFilterShowButton.setBounds(rowLeft, y, btnW, btnH);
        parentEditor.delayFilterWarmSaturationButton.setBounds(rowLeft + btnW + filterRowGap, y, warmSatW, btnH);
        parentEditor.delayFilterWarmSaturationButton.setVisible(true);
    }
    else
    {
        parentEditor.delayFilterShowButton.setBounds(cx - btnW/2, y, btnW, btnH);
        parentEditor.delayFilterWarmSaturationButton.setVisible(false);
    }
    y += btnH + gap;
    
    // ---- Filter section: HP Cutoff | HP Res | LP Cutoff | LP Res (same knob size as other effects) ----
    if (filterShow)
    {
        int fGap = 6;
        int filterW = 4 * knobSize + 3 * fGap;
        int filterLeft = cx - filterW / 2;
        
        parentEditor.delayFilterHPCutoffLabel.setBounds(filterLeft, y, filterLabelW, labelH);
        parentEditor.delayFilterHPResonanceLabel.setBounds(filterLeft + knobSize + fGap, y, filterLabelW, labelH);
        parentEditor.delayFilterLPCutoffLabel.setBounds(filterLeft + 2*(knobSize + fGap), y, filterLabelW, labelH);
        parentEditor.delayFilterLPResonanceLabel.setBounds(filterLeft + 3*(knobSize + fGap), y, filterLabelW, labelH);
        y += labelH + labelGap;
        
        parentEditor.delayFilterHPCutoffSlider.setBounds(filterLeft, y, knobSize, knobSize);
        parentEditor.delayFilterHPResonanceSlider.setBounds(filterLeft + knobSize + fGap, y, knobSize, knobSize);
        parentEditor.delayFilterLPCutoffSlider.setBounds(filterLeft + 2*(knobSize + fGap), y, knobSize, knobSize);
        parentEditor.delayFilterLPResonanceSlider.setBounds(filterLeft + 3*(knobSize + fGap), y, knobSize, knobSize);
        y += knobSize + gap;
    }
    
    const int delayContentHeight = y + pad;
    parentEditor.delayGroup.setBounds(delayColX, pad, colW, delayContentHeight);

    // ---- Grain Delay section (below Delay in left column) ----
    const int sectionGap = 6;  // Compact gap between effect sections
    int grainStartY = pad + delayContentHeight + sectionGap;
    int gCx = delayColX + colW / 2;
    int gY = grainStartY + groupTitleH;
    const int gKnobSize = knobSize;
    const int gBtnW = 52;
    const int gBtnH = 22;
    const int gOnBtnW = 62;
    const int gOnBtnH = 28;
    const int gLabelH = labelH;
    const int gLabelGap = labelGap;
    const int gGap = gap;

    // On button: upper-left below label (larger for visibility)
    parentEditor.grainDelayEnabledButton.setBounds(delayColX + pad, gY, gOnBtnW, gOnBtnH);
    gY += gOnBtnH + gGap;

    // Row 1: Time | Size | Decay
    const int gKg = 6;
    const int gRowW = 3 * gKnobSize + 2 * gKg;
    int gRowLeft = gCx - gRowW / 2;
    parentEditor.grainDelayTimeLabel.setBounds(gRowLeft, gY, gKnobSize, gLabelH);
    parentEditor.grainDelaySizeLabel.setBounds(gRowLeft + gKnobSize + gKg, gY, gKnobSize, gLabelH);
    parentEditor.grainDelayDecayLabel.setBounds(gRowLeft + 2 * (gKnobSize + gKg), gY, gKnobSize, gLabelH);
    gY += gLabelH + gLabelGap;
    parentEditor.grainDelayTimeSlider.setBounds(gRowLeft, gY, gKnobSize, gKnobSize);
    parentEditor.grainDelaySizeSlider.setBounds(gRowLeft + gKnobSize + gKg, gY, gKnobSize, gKnobSize);
    parentEditor.grainDelayDecaySlider.setBounds(gRowLeft + 2 * (gKnobSize + gKg), gY, gKnobSize, gKnobSize);
    gY += gKnobSize + gGap;

    // Row 2: Pitch | Density | Jitter
    parentEditor.grainDelayPitchLabel.setBounds(gRowLeft, gY, gKnobSize, gLabelH);
    parentEditor.grainDelayDensityLabel.setBounds(gRowLeft + gKnobSize + gKg, gY, gKnobSize, gLabelH);
    parentEditor.grainDelayJitterLabel.setBounds(gRowLeft + 2 * (gKnobSize + gKg), gY, gKnobSize, gLabelH);
    gY += gLabelH + gLabelGap;
    parentEditor.grainDelayPitchSlider.setBounds(gRowLeft, gY, gKnobSize, gKnobSize);
    parentEditor.grainDelayDensitySlider.setBounds(gRowLeft + gKnobSize + gKg, gY, gKnobSize, gKnobSize);
    parentEditor.grainDelayJitterSlider.setBounds(gRowLeft + 2 * (gKnobSize + gKg), gY, gKnobSize, gKnobSize);
    gY += gKnobSize + gGap;

    // Ping-Pong toggle (label hidden - button text is sufficient)
    parentEditor.grainDelayPingPongLabel.setVisible(false);
    parentEditor.grainDelayPingPongButton.setBounds(gCx - 50, gY, 100, gBtnH);
    gY += gBtnH + gGap;

    // Filter toggle; when on, Warm Saturation next to it (only visible when filter is on)
    bool grainFilterShow = parentEditor.safeGetParam("grainDelayFilterShow") > 0.5f;
    const int gWarmSatW = 120;
    const int gFilterRowGap = 6;
    if (grainFilterShow)
    {
        int gFilterRowW = gBtnW + gFilterRowGap + gWarmSatW;
        int gFilterRowLeft = gCx - gFilterRowW / 2;
        parentEditor.grainDelayFilterShowButton.setBounds(gFilterRowLeft, gY, gBtnW, gBtnH);
        parentEditor.grainDelayFilterWarmSaturationButton.setBounds(gFilterRowLeft + gBtnW + gFilterRowGap, gY, gWarmSatW, gBtnH);
        parentEditor.grainDelayFilterWarmSaturationButton.setVisible(true);
    }
    else
    {
        parentEditor.grainDelayFilterShowButton.setBounds(gCx - gBtnW/2, gY, gBtnW, gBtnH);
        parentEditor.grainDelayFilterWarmSaturationButton.setVisible(false);
    }
    gY += gBtnH + gGap;
    
    if (grainFilterShow)
    {
        int gFGap = 6;
        int gFilterW = 4 * gKnobSize + 3 * gFGap;
        int gFilterLeft = gCx - gFilterW / 2;
        parentEditor.grainDelayFilterHPCutoffLabel.setBounds(gFilterLeft, gY, filterLabelW, gLabelH);
        parentEditor.grainDelayFilterHPResonanceLabel.setBounds(gFilterLeft + gKnobSize + gFGap, gY, filterLabelW, gLabelH);
        parentEditor.grainDelayFilterLPCutoffLabel.setBounds(gFilterLeft + 2*(gKnobSize + gFGap), gY, filterLabelW, gLabelH);
        parentEditor.grainDelayFilterLPResonanceLabel.setBounds(gFilterLeft + 3*(gKnobSize + gFGap), gY, filterLabelW, gLabelH);
        gY += gLabelH + gLabelGap;
        parentEditor.grainDelayFilterHPCutoffSlider.setBounds(gFilterLeft, gY, gKnobSize, gKnobSize);
        parentEditor.grainDelayFilterHPResonanceSlider.setBounds(gFilterLeft + gKnobSize + gFGap, gY, gKnobSize, gKnobSize);
        parentEditor.grainDelayFilterLPCutoffSlider.setBounds(gFilterLeft + 2*(gKnobSize + gFGap), gY, gKnobSize, gKnobSize);
        parentEditor.grainDelayFilterLPResonanceSlider.setBounds(gFilterLeft + 3*(gKnobSize + gFGap), gY, gKnobSize, gKnobSize);
        gY += gKnobSize + gGap;
    }

    // Mix knob - always the lowest knob
    parentEditor.grainDelayMixLabel.setBounds(gCx - gKnobSize/2, gY, gKnobSize, gLabelH);
    gY += gLabelH + gLabelGap;
    parentEditor.grainDelayMixSlider.setBounds(gCx - gKnobSize/2, gY, gKnobSize, gKnobSize);
    gY += gKnobSize + pad;

    const int grainNaturalContentHeight = gY - grainStartY;

    // ---- Reverb section (center column) - On button upper-left, Mix lowest ----
    int rCx = reverbColX + colW / 2;
    int rY = groupTitleH;
    const int rKnobSize = knobSize;
    const int rBtnW = 48;
    const int rBtnH = 20;
    const int rOnBtnW = 62;
    const int rOnBtnH = 28;
    
    // On button: upper-left below label (larger for visibility)
    parentEditor.reverbEnabledButton.setBounds(reverbColX + pad, rY, rOnBtnW, rOnBtnH);
    rY += rOnBtnH + gap;
    
    parentEditor.reverbTypeLabel.setBounds(rCx - 60, rY, 120, labelH);
    rY += labelH + labelGap;
    parentEditor.reverbTypeCombo.setBounds(rCx - 60, rY, 120, 20);
    rY += 24 + gap;
    
    // Decay (Mix moves to bottom)
    parentEditor.reverbDecayTimeLabel.setBounds(rCx - rKnobSize/2, rY, rKnobSize, labelH);
    rY += labelH + labelGap;
    parentEditor.reverbDecayTimeSlider.setBounds(rCx - rKnobSize/2, rY, rKnobSize, rKnobSize);
    rY += rKnobSize + gap;
    
    // Filter toggle; when on, Warm Saturation next to it (only visible when filter is on)
    bool reverbFilterShow = parentEditor.safeGetParam("reverbFilterShow") > 0.5f;
    const int rWarmSatW = 120;
    const int rFilterRowGap = 6;
    if (reverbFilterShow)
    {
        int rRowW = rBtnW + rFilterRowGap + rWarmSatW;
        int rRowLeft = rCx - rRowW / 2;
        parentEditor.reverbFilterShowButton.setBounds(rRowLeft, rY, rBtnW, rBtnH);
        parentEditor.reverbFilterWarmSaturationButton.setBounds(rRowLeft + rBtnW + rFilterRowGap, rY, rWarmSatW, rBtnH);
        parentEditor.reverbFilterWarmSaturationButton.setVisible(true);
    }
    else
    {
        parentEditor.reverbFilterShowButton.setBounds(rCx - rBtnW/2, rY, rBtnW, rBtnH);
        parentEditor.reverbFilterWarmSaturationButton.setVisible(false);
    }
    rY += rBtnH + gap;

    if (reverbFilterShow)
    {
        int rFGap = 6;
        int rFilterW = 4 * rKnobSize + 3 * rFGap;
        int rFilterLeft = rCx - rFilterW / 2;
        parentEditor.reverbFilterHPCutoffLabel.setBounds(rFilterLeft, rY, filterLabelW, labelH);
        parentEditor.reverbFilterHPResonanceLabel.setBounds(rFilterLeft + rKnobSize + rFGap, rY, filterLabelW, labelH);
        parentEditor.reverbFilterLPCutoffLabel.setBounds(rFilterLeft + 2*(rKnobSize + rFGap), rY, filterLabelW, labelH);
        parentEditor.reverbFilterLPResonanceLabel.setBounds(rFilterLeft + 3*(rKnobSize + rFGap), rY, filterLabelW, labelH);
        rY += labelH + labelGap;
        parentEditor.reverbFilterHPCutoffSlider.setBounds(rFilterLeft, rY, rKnobSize, rKnobSize);
        parentEditor.reverbFilterHPResonanceSlider.setBounds(rFilterLeft + rKnobSize + rFGap, rY, rKnobSize, rKnobSize);
        parentEditor.reverbFilterLPCutoffSlider.setBounds(rFilterLeft + 2*(rKnobSize + rFGap), rY, rKnobSize, rKnobSize);
        parentEditor.reverbFilterLPResonanceSlider.setBounds(rFilterLeft + 3*(rKnobSize + rFGap), rY, rKnobSize, rKnobSize);
        rY += rKnobSize + gap;
    }

    // Mix always at bottom (same pattern as Grain Delay)
    parentEditor.reverbWetMixLabel.setBounds(rCx - rKnobSize/2, rY, rKnobSize, labelH);
    rY += labelH + labelGap;
    parentEditor.reverbWetMixSlider.setBounds(rCx - rKnobSize/2, rY, rKnobSize, rKnobSize);
    rY += rKnobSize + pad;

    const int reverbContentHeight = rY;
    parentEditor.reverbGroup.setBounds(reverbColX, pad, colW, reverbContentHeight);

    // ---- Trance Gate section (below Reverb in center column) ----
    const int gateSectionGap = sectionGap;  // Same gap as other columns
    int gateStartY = pad + reverbContentHeight + gateSectionGap;
    int tCx = reverbColX + colW / 2;
    int tY = gateStartY + groupTitleH;
    const int tKnobSize = knobSize;
    const int tBtnW = 48;
    const int tBtnH = 20;
    const int tOnBtnW = 62;
    const int tOnBtnH = 28;
    const int tLabelH = labelH;
    const int tGap = gap;

    // On button: upper-left below label (larger for visibility)
    parentEditor.tranceGateEnabledButton.setBounds(reverbColX + pad, tY, tOnBtnW, tOnBtnH);
    tY += tOnBtnH + tGap;

    parentEditor.tranceGatePreEffectButton.setBounds(tCx - 60, tY, 120, tBtnH);
    tY += tBtnH + tGap;

    parentEditor.tranceGateStepsCombo.setBounds(tCx - 60, tY, 120, 20);
    tY += 24 + tGap;

    parentEditor.tranceGateSyncButton.setBounds(tCx - tBtnW/2, tY, tBtnW, tBtnH);
    tY += tBtnH + tGap;

    // Rate | Attack | Release - all three in one row
    const int tKg = 6;
    const int tRowW = 3 * tKnobSize + 2 * tKg;
    int tRowLeft = tCx - tRowW / 2;
    parentEditor.tranceGateRateLabel.setBounds(tRowLeft, tY, tKnobSize, tLabelH);
    parentEditor.tranceGateAttackLabel.setBounds(tRowLeft + tKnobSize + tKg, tY, tKnobSize, tLabelH);
    parentEditor.tranceGateReleaseLabel.setBounds(tRowLeft + 2 * (tKnobSize + tKg), tY, tKnobSize, tLabelH);
    tY += tLabelH + labelGap;
    parentEditor.tranceGateRateSlider.setBounds(tRowLeft, tY, tKnobSize, tKnobSize);
    parentEditor.tranceGateAttackSlider.setBounds(tRowLeft + tKnobSize + tKg, tY, tKnobSize, tKnobSize);
    parentEditor.tranceGateReleaseSlider.setBounds(tRowLeft + 2 * (tKnobSize + tKg), tY, tKnobSize, tKnobSize);
    tY += tKnobSize + tGap;

    // Step buttons - show 4, 8, or 16 (two rows of 8) based on Steps dropdown
    const int stepBtnSize = 24;
    const int stepGap = 4;
    const int selectedId = parentEditor.tranceGateStepsCombo.getSelectedId();
    const int totalSteps = (selectedId == 1) ? 4 : (selectedId == 3) ? 16 : 8;
    const int rowSize = 8;  // max buttons per row
    const int row1Count = juce::jmin(totalSteps, rowSize);

    const int row1TotalW = row1Count * stepBtnSize + (row1Count - 1) * stepGap;
    int stepLeft = tCx - row1TotalW / 2;

    juce::ToggleButton* stepBtns[16] = {
        &parentEditor.tranceGateStep1Button, &parentEditor.tranceGateStep2Button,
        &parentEditor.tranceGateStep3Button, &parentEditor.tranceGateStep4Button,
        &parentEditor.tranceGateStep5Button, &parentEditor.tranceGateStep6Button,
        &parentEditor.tranceGateStep7Button, &parentEditor.tranceGateStep8Button,
        &parentEditor.tranceGateStep9Button, &parentEditor.tranceGateStep10Button,
        &parentEditor.tranceGateStep11Button, &parentEditor.tranceGateStep12Button,
        &parentEditor.tranceGateStep13Button, &parentEditor.tranceGateStep14Button,
        &parentEditor.tranceGateStep15Button, &parentEditor.tranceGateStep16Button
    };

    // Row 1: steps 1-8 (or 1-4)
    for (int s = 0; s < rowSize; ++s)
    {
        if (s < row1Count)
        {
            stepBtns[s]->setVisible(true);
            stepBtns[s]->setBounds(stepLeft + s * (stepBtnSize + stepGap), tY, stepBtnSize, stepBtnSize);
        }
        else
        {
            stepBtns[s]->setVisible(false);
        }
    }
    tY += stepBtnSize + stepGap;

    // Row 2: steps 9-16 (only when 16 steps selected)
    if (totalSteps == 16)
    {
        for (int s = 0; s < rowSize; ++s)
        {
            stepBtns[rowSize + s]->setVisible(true);
            stepBtns[rowSize + s]->setBounds(stepLeft + s * (stepBtnSize + stepGap), tY, stepBtnSize, stepBtnSize);
        }
        tY += stepBtnSize + tGap;
    }
    else
    {
        // Hide steps 9-16 when not in 16-step mode
        for (int s = 0; s < rowSize; ++s)
            stepBtns[rowSize + s]->setVisible(false);
        tY += tGap - stepGap;  // restore normal gap (we already added stepGap above)
    }

    // Mix knob - always the lowest knob
    parentEditor.tranceGateMixLabel.setBounds(tCx - tKnobSize/2, tY, tKnobSize, tLabelH);
    tY += tLabelH + labelGap;
    parentEditor.tranceGateMixSlider.setBounds(tCx - tKnobSize/2, tY, tKnobSize, tKnobSize);
    tY += tKnobSize + pad;
    const int tranceNaturalContentHeight = tY - gateStartY;

    // ---- Phaser section (right column, top) ----
    int pCx = grainColX + colW / 2;
    int pY = groupTitleH;
    const int pKnobSize = knobSize;
    const int pBtnW = 48;
    const int pBtnH = 20;
    const int pOnBtnW = 62;
    const int pOnBtnH = 28;
    const int pLabelH = labelH;
    const int pLabelGap = labelGap;
    const int pGap = gap;

    // On button: upper-left below label (larger for visibility)
    parentEditor.phaserEnabledButton.setBounds(grainColX + pad, pY, pOnBtnW, pOnBtnH);
    pY += pOnBtnH + pGap;

    // Rate | Depth | Feedback - all three in one row
    const int pKg = 6;
    const int pRowW = 3 * pKnobSize + 2 * pKg;
    int pRowLeft = pCx - pRowW / 2;
    parentEditor.phaserRateLabel.setBounds(pRowLeft, pY, pKnobSize, pLabelH);
    parentEditor.phaserDepthLabel.setBounds(pRowLeft + pKnobSize + pKg, pY, pKnobSize, pLabelH);
    parentEditor.phaserFeedbackLabel.setBounds(pRowLeft + 2 * (pKnobSize + pKg), pY, pKnobSize, pLabelH);
    pY += pLabelH + pLabelGap;
    parentEditor.phaserRateSlider.setBounds(pRowLeft, pY, pKnobSize, pKnobSize);
    parentEditor.phaserDepthSlider.setBounds(pRowLeft + pKnobSize + pKg, pY, pKnobSize, pKnobSize);
    parentEditor.phaserFeedbackSlider.setBounds(pRowLeft + 2 * (pKnobSize + pKg), pY, pKnobSize, pKnobSize);
    pY += pKnobSize + pGap;

    // Script and Vintage toggles - side by side
    const int pToggleGap = 6;
    const int pToggleW = 55;
    const int pToggleRowW = 2 * pToggleW + pToggleGap;
    int pToggleLeft = pCx - pToggleRowW / 2;
    parentEditor.phaserScriptModeLabel.setBounds(pToggleLeft, pY, pToggleW, pLabelH);
    parentEditor.phaserVintageModeLabel.setBounds(pToggleLeft + pToggleW + pToggleGap, pY, pToggleW, pLabelH);
    pY += pLabelH + pLabelGap;
    parentEditor.phaserScriptModeButton.setBounds(pToggleLeft, pY, pToggleW, pBtnH);
    parentEditor.phaserVintageModeButton.setBounds(pToggleLeft + pToggleW + pToggleGap, pY, pToggleW, pBtnH);
    pY += pBtnH + pGap;

    // Width | Center - side by side to save vertical space
    const int pPairGap = 8;
    const int pPairW = 2 * pKnobSize + pPairGap;
    int pPairLeft = pCx - pPairW / 2;
    parentEditor.phaserStereoOffsetLabel.setBounds(pPairLeft, pY, pKnobSize, pLabelH);
    parentEditor.phaserCentreLabel.setBounds(pPairLeft + pKnobSize + pPairGap, pY, pKnobSize, pLabelH);
    pY += pLabelH + pLabelGap;
    parentEditor.phaserStereoOffsetSlider.setBounds(pPairLeft, pY, pKnobSize, pKnobSize);
    parentEditor.phaserCentreSlider.setBounds(pPairLeft + pKnobSize + pPairGap, pY, pKnobSize, pKnobSize);
    pY += pKnobSize + pGap;

    parentEditor.phaserStagesLabel.setBounds(pCx - 60, pY, 120, pLabelH);
    pY += pLabelH + pLabelGap;
    parentEditor.phaserStagesCombo.setBounds(pCx - 60, pY, 120, 20);
    pY += 24 + pGap;

    // Mix knob - always the lowest knob
    parentEditor.phaserMixLabel.setBounds(pCx - pKnobSize/2, pY, pKnobSize, pLabelH);
    pY += pLabelH + pLabelGap;
    parentEditor.phaserMixSlider.setBounds(pCx - pKnobSize/2, pY, pKnobSize, pKnobSize);
    pY += pKnobSize + pad;

    const int phaserContentHeight = pY;
    parentEditor.phaserGroup.setBounds(grainColX, pad, colW, phaserContentHeight);

    // ---- Flanger section (below Phaser in right column) ----
    int flangerStartY = pad + phaserContentHeight + sectionGap;
    int fCx = grainColX + colW / 2;
    int fY = flangerStartY + groupTitleH;
    const int fKnobSize = knobSize;
    const int fOnBtnW = 62;
    const int fOnBtnH = 28;
    const int fLabelH = labelH;
    const int fLabelGap = labelGap;
    const int fGap = gap;

    parentEditor.flangerEnabledButton.setBounds(grainColX + pad, fY, fOnBtnW, fOnBtnH);
    fY += fOnBtnH + fGap;

    const int fKg = 6;
    const int fRowW = 3 * fKnobSize + 2 * fKg;
    int fRowLeft = fCx - fRowW / 2;
    parentEditor.flangerRateLabel.setBounds(fRowLeft, fY, fKnobSize, fLabelH);
    parentEditor.flangerDepthLabel.setBounds(fRowLeft + fKnobSize + fKg, fY, fKnobSize, fLabelH);
    parentEditor.flangerFeedbackLabel.setBounds(fRowLeft + 2 * (fKnobSize + fKg), fY, fKnobSize, fLabelH);
    fY += fLabelH + fLabelGap;
    parentEditor.flangerRateSlider.setBounds(fRowLeft, fY, fKnobSize, fKnobSize);
    parentEditor.flangerDepthSlider.setBounds(fRowLeft + fKnobSize + fKg, fY, fKnobSize, fKnobSize);
    parentEditor.flangerFeedbackSlider.setBounds(fRowLeft + 2 * (fKnobSize + fKg), fY, fKnobSize, fKnobSize);
    fY += fKnobSize + fGap;

    // Width | Mix - side by side
    const int fPairW = 2 * fKnobSize + fKg;
    int fPairLeft = fCx - fPairW / 2;
    parentEditor.flangerWidthLabel.setBounds(fPairLeft, fY, fKnobSize, fLabelH);
    parentEditor.flangerMixLabel.setBounds(fPairLeft + fKnobSize + fKg, fY, fKnobSize, fLabelH);
    fY += fLabelH + fLabelGap;
    parentEditor.flangerWidthSlider.setBounds(fPairLeft, fY, fKnobSize, fKnobSize);
    parentEditor.flangerMixSlider.setBounds(fPairLeft + fKnobSize + fKg, fY, fKnobSize, fKnobSize);
    fY += fKnobSize + pad;
    const int flangerNaturalContentHeight = fY - flangerStartY;

    // Lower-row alignment uses Delay/Reverb heights *without* their filter rows, so toggling those filters
    // only resizes Delay/Reverb and moves Grain/Trance â€” Grain & Flanger group heights stay the same.
    static constexpr int kEffectsTranceGateReferenceAlignHeight = 352;
    const int delayFilterRowExtra = filterShow ? (labelH + labelGap + knobSize + gap) : 0;
    const int reverbFilterRowExtra = reverbFilterShow ? (labelH + labelGap + knobSize + gap) : 0;
    const int delayContentHeightForAlign = delayContentHeight - delayFilterRowExtra;
    const int grainStartYBase = pad + delayContentHeightForAlign + sectionGap;
    const int reverbContentHeightForAlign = reverbContentHeight - reverbFilterRowExtra;
    const int gateStartYBase = pad + reverbContentHeightForAlign + gateSectionGap;
    const int targetLowerBoxBottomY = gateStartYBase + kEffectsTranceGateReferenceAlignHeight;

    const int grainGroupHeight = juce::jmax(grainNaturalContentHeight, targetLowerBoxBottomY - grainStartYBase);
    const int tranceGateGroupHeight = juce::jmax(tranceNaturalContentHeight, kEffectsTranceGateReferenceAlignHeight);
    const int flangerGroupHeight = juce::jmax(flangerNaturalContentHeight, targetLowerBoxBottomY - flangerStartY);

    parentEditor.grainDelayGroup.setBounds(delayColX, grainStartY, colW, grainGroupHeight);
    parentEditor.tranceGateGroup.setBounds(reverbColX, gateStartY, colW, tranceGateGroupHeight);
    parentEditor.flangerGroup.setBounds(grainColX, flangerStartY, colW, flangerGroupHeight);
}

//==============================================================================
// -- SaturationColorPageComponent Implementation --
SaturationColorPageComponent::SaturationColorPageComponent(SpaceDustAudioProcessorEditor& editor)
    : parentEditor(editor)
{
    setAccessible(false);
    addAndMakeVisible(parentEditor.bitCrusherGroup);
    addAndMakeVisible(parentEditor.bitCrusherEnabledButton);
    addAndMakeVisible(parentEditor.bitCrusherEnabledLabel);
    addAndMakeVisible(parentEditor.bitCrusherPostEffectButton);
    addAndMakeVisible(parentEditor.bitCrusherPostEffectLabel);
    addAndMakeVisible(parentEditor.bitCrusherAmountSlider);
    addAndMakeVisible(parentEditor.bitCrusherAmountLabel);
    addAndMakeVisible(parentEditor.bitCrusherRateSlider);
    addAndMakeVisible(parentEditor.bitCrusherRateLabel);
    addAndMakeVisible(parentEditor.bitCrusherMixSlider);
    addAndMakeVisible(parentEditor.bitCrusherMixLabel);
    addAndMakeVisible(parentEditor.softClipperGroup);
    addAndMakeVisible(parentEditor.softClipperEnabledButton);
    addAndMakeVisible(parentEditor.softClipperEnabledLabel);
    addAndMakeVisible(parentEditor.softClipperModeCombo);
    addAndMakeVisible(parentEditor.softClipperModeLabel);
    addAndMakeVisible(parentEditor.softClipperDriveSlider);
    addAndMakeVisible(parentEditor.softClipperDriveLabel);
    addAndMakeVisible(parentEditor.softClipperKneeSlider);
    addAndMakeVisible(parentEditor.softClipperKneeLabel);
    addAndMakeVisible(parentEditor.softClipperOversampleCombo);
    addAndMakeVisible(parentEditor.softClipperOversampleLabel);
    addAndMakeVisible(parentEditor.softClipperMixSlider);
    addAndMakeVisible(parentEditor.softClipperMixLabel);
    addAndMakeVisible(parentEditor.compressorGroup);
    addAndMakeVisible(parentEditor.compressorEnabledButton);
    addAndMakeVisible(parentEditor.compressorEnabledLabel);
    addAndMakeVisible(parentEditor.compressorTypeCombo);
    addAndMakeVisible(parentEditor.compressorTypeLabel);
    addAndMakeVisible(parentEditor.compressorThresholdSlider);
    addAndMakeVisible(parentEditor.compressorThresholdLabel);
    addAndMakeVisible(parentEditor.compressorRatioSlider);
    addAndMakeVisible(parentEditor.compressorRatioLabel);
    addAndMakeVisible(parentEditor.compressorAttackSlider);
    addAndMakeVisible(parentEditor.compressorAttackLabel);
    addAndMakeVisible(parentEditor.compressorReleaseSlider);
    addAndMakeVisible(parentEditor.compressorReleaseLabel);
    addAndMakeVisible(parentEditor.compressorMakeupSlider);
    addAndMakeVisible(parentEditor.compressorMakeupLabel);
    addAndMakeVisible(parentEditor.compressorMixSlider);
    addAndMakeVisible(parentEditor.compressorMixLabel);
    addAndMakeVisible(parentEditor.compressorAutoReleaseButton);
    addAndMakeVisible(parentEditor.compressorAutoReleaseLabel);
    addAndMakeVisible(parentEditor.compressorSoftClipButton);
    addAndMakeVisible(parentEditor.compressorSoftClipLabel);
    addAndMakeVisible(parentEditor.lofiGroup);
    addAndMakeVisible(parentEditor.lofiEnabledButton);
    addAndMakeVisible(parentEditor.lofiEnabledLabel);
    addAndMakeVisible(parentEditor.lofiAmountSlider);
    addAndMakeVisible(parentEditor.lofiAmountLabel);
    addAndMakeVisible(parentEditor.analogDriftSlider);
    addAndMakeVisible(parentEditor.analogDriftLabel);
    // Final EQ â€“ spanning center+right columns
    addAndMakeVisible(parentEditor.finalEQGroup);
    addAndMakeVisible(parentEditor.finalEQEnabledButton);
    addAndMakeVisible(parentEditor.finalEQEnabledLabel);
    if (parentEditor.finalEQComponent)
        addAndMakeVisible(parentEditor.finalEQComponent.get());
    addAndMakeVisible(parentEditor.finalEQNodeCombo);
    addAndMakeVisible(parentEditor.finalEQNodeLabel);
    addAndMakeVisible(parentEditor.finalEQResetButton);
    addAndMakeVisible(parentEditor.finalEQTypeCombo);
    addAndMakeVisible(parentEditor.finalEQTypeLabel);
    addAndMakeVisible(parentEditor.finalEQQSlider);
    addAndMakeVisible(parentEditor.finalEQQLabel);
    addAndMakeVisible(parentEditor.finalEQFreqSlider);
    addAndMakeVisible(parentEditor.finalEQFreqLabel);
    addAndMakeVisible(parentEditor.finalEQGainSlider);
    addAndMakeVisible(parentEditor.finalEQGainLabel);
    addAndMakeVisible(parentEditor.transientGroup);
    addAndMakeVisible(parentEditor.transientEnabledButton);
    addAndMakeVisible(parentEditor.transientEnabledLabel);
    addAndMakeVisible(parentEditor.transientTypeCombo);
    addAndMakeVisible(parentEditor.transientTypeEditButton);
    addAndMakeVisible(parentEditor.transientTypeLabel);
    addAndMakeVisible(parentEditor.transientMixSlider);
    addAndMakeVisible(parentEditor.transientMixLabel);
    addAndMakeVisible(parentEditor.transientPostEffectButton);
    addAndMakeVisible(parentEditor.transientPostEffectLabel);
    addAndMakeVisible(parentEditor.transientKaDonkSlider);
    addAndMakeVisible(parentEditor.transientKaDonkLabel);
    addAndMakeVisible(parentEditor.transientCoarseSlider);
    addAndMakeVisible(parentEditor.transientCoarseLabel);
    addAndMakeVisible(parentEditor.transientLengthSlider);
    addAndMakeVisible(parentEditor.transientLengthLabel);
}

void SaturationColorPageComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0a0a1f));
    float avgLevel = parentEditor.getGlowMeterLevel();  // single per-frame averaged L/R snapshot
    drawStarfield(g, getWidth(), getHeight(), avgLevel);
    const int baseAlpha = 10 + static_cast<int>(48.0f * avgLevel);
    drawGlows(g, baseAlpha, meterLinkedGroupGlowHue(parentEditor.clippingHoldTicks > 0), { &parentEditor.bitCrusherGroup, &parentEditor.compressorGroup, &parentEditor.softClipperGroup, &parentEditor.lofiGroup, &parentEditor.transientGroup, &parentEditor.finalEQGroup });
}

void SaturationColorPageComponent::resized()
{
    auto r = getLocalBounds();
    const int pad = 8;
    const int colGap = 8;
    const int groupTitleH = 32;
    const int knobSize = 56;
    const int labelH = 14;
    const int labelGap = 2;
    const int gap = 4;
    const int bOnBtnW = 62;
    const int bOnBtnH = 28;

    // Three columns: Bit Crusher (left) | Compressor (center) | Soft Clipper (right)
    const int colW = (r.getWidth() - 2 * pad - 2 * colGap) / 3;
    const int leftColX = pad;
    const int centerColX = pad + colW + colGap;
    const int rightColX = pad + 2 * (colW + colGap);

    // --- Bit Crusher (left column) ---
    int by = pad + groupTitleH;
    int bCx = leftColX + colW / 2;
    parentEditor.bitCrusherEnabledButton.setBounds(leftColX + pad, by, bOnBtnW, bOnBtnH);
    by += bOnBtnH + gap;
    parentEditor.bitCrusherPostEffectButton.setBounds(bCx - 60, by, 120, 20);
    by += 20 + gap;
    const int bKg = 6;
    const int bTripleW = 3 * knobSize + 2 * bKg;
    int bTripleLeft = leftColX + (colW - bTripleW) / 2;
    parentEditor.bitCrusherAmountLabel.setBounds(bTripleLeft, by, knobSize, labelH);
    parentEditor.bitCrusherRateLabel.setBounds(bTripleLeft + knobSize + bKg, by, knobSize, labelH);
    parentEditor.bitCrusherMixLabel.setBounds(bTripleLeft + 2 * (knobSize + bKg), by, knobSize, labelH);
    by += labelH + labelGap;
    parentEditor.bitCrusherAmountSlider.setBounds(bTripleLeft, by, knobSize, knobSize);
    parentEditor.bitCrusherRateSlider.setBounds(bTripleLeft + knobSize + bKg, by, knobSize, knobSize);
    parentEditor.bitCrusherMixSlider.setBounds(bTripleLeft + 2 * (knobSize + bKg), by, knobSize, knobSize);
    by += knobSize + 24 + pad;
    parentEditor.bitCrusherGroup.setBounds(leftColX, pad, colW, by - pad);

    // --- Compressor (center column) ---
    int cy = pad + groupTitleH;
    int cCx = centerColX + colW / 2;
    const int cKg = 6;
    const int cComboW = 100;
    parentEditor.compressorEnabledButton.setBounds(centerColX + pad, cy, bOnBtnW, bOnBtnH);
    cy += bOnBtnH + gap;
    parentEditor.compressorTypeCombo.setBounds(cCx - cComboW / 2, cy, cComboW, 22);
    cy += 24 + gap;
    parentEditor.compressorTypeLabel.setBounds(cCx - cComboW / 2, cy, cComboW, labelH);
    cy += labelH + labelGap;
    // Row 1: Threshold | Ratio
    const int cPairW = 2 * knobSize + cKg;
    int cPairLeft = centerColX + (colW - cPairW) / 2;
    parentEditor.compressorThresholdLabel.setBounds(cPairLeft, cy, knobSize, labelH);
    parentEditor.compressorRatioLabel.setBounds(cPairLeft + knobSize + cKg, cy, knobSize, labelH);
    cy += labelH + labelGap;
    parentEditor.compressorThresholdSlider.setBounds(cPairLeft, cy, knobSize, knobSize);
    parentEditor.compressorRatioSlider.setBounds(cPairLeft + knobSize + cKg, cy, knobSize, knobSize);
    cy += knobSize + gap;
    // Row 2: Attack | Release
    parentEditor.compressorAttackLabel.setBounds(cPairLeft, cy, knobSize, labelH);
    parentEditor.compressorReleaseLabel.setBounds(cPairLeft + knobSize + cKg, cy, knobSize, labelH);
    cy += labelH + labelGap;
    parentEditor.compressorAttackSlider.setBounds(cPairLeft, cy, knobSize, knobSize);
    parentEditor.compressorReleaseSlider.setBounds(cPairLeft + knobSize + cKg, cy, knobSize, knobSize);
    cy += knobSize + gap;
    // Auto Release | Soft Clip toggles
    const int cToggleW = 80;
    const int cToggleH = 20;
    const int cToggleGap = 6;
    const int cToggleRowW = 2 * cToggleW + cToggleGap;
    int cToggleLeft = cCx - cToggleRowW / 2;
    parentEditor.compressorAutoReleaseButton.setBounds(cToggleLeft, cy, cToggleW, cToggleH);
    parentEditor.compressorSoftClipButton.setBounds(cToggleLeft + cToggleW + cToggleGap, cy, cToggleW, cToggleH);
    cy += cToggleH + gap;
    // Row 3: Makeup | Mix
    parentEditor.compressorMakeupLabel.setBounds(cPairLeft, cy, knobSize, labelH);
    parentEditor.compressorMixLabel.setBounds(cPairLeft + knobSize + cKg, cy, knobSize, labelH);
    cy += labelH + labelGap;
    parentEditor.compressorMakeupSlider.setBounds(cPairLeft, cy, knobSize, knobSize);
    parentEditor.compressorMixSlider.setBounds(cPairLeft + knobSize + cKg, cy, knobSize, knobSize);
    cy += knobSize + pad;
    parentEditor.compressorGroup.setBounds(centerColX, pad, colW, cy - pad);

    // --- Soft Clipper (right column) ---
    int sy = pad + groupTitleH;
    int sCx = rightColX + colW / 2;
    const int sComboW = 100;
    const int sOsComboW = 56;
    parentEditor.softClipperEnabledButton.setBounds(rightColX + pad, sy, bOnBtnW, bOnBtnH);
    sy += bOnBtnH + gap;
    parentEditor.softClipperModeCombo.setBounds(sCx - sComboW / 2, sy, sComboW, 22);
    sy += 24 + gap;
    parentEditor.softClipperModeLabel.setBounds(sCx - sComboW / 2, sy, sComboW, labelH);
    sy += labelH + labelGap;
    const int sKg = 6;
    const int sPairW = 2 * knobSize + sKg;
    int sPairLeft = rightColX + (colW - sPairW) / 2;
    parentEditor.softClipperDriveLabel.setBounds(sPairLeft, sy, knobSize, labelH);
    parentEditor.softClipperKneeLabel.setBounds(sPairLeft + knobSize + sKg, sy, knobSize, labelH);
    sy += labelH + labelGap;
    parentEditor.softClipperDriveSlider.setBounds(sPairLeft, sy, knobSize, knobSize);
    parentEditor.softClipperKneeSlider.setBounds(sPairLeft + knobSize + sKg, sy, knobSize, knobSize);
    sy += knobSize + gap;
    parentEditor.softClipperOversampleCombo.setBounds(sCx - sOsComboW / 2, sy, sOsComboW, 22);
    sy += 24 + gap;
    const int sOsLabelW = 90;  // Wider to fit "Oversampling"
    parentEditor.softClipperOversampleLabel.setBounds(sCx - sOsLabelW / 2, sy, sOsLabelW, labelH);
    sy += labelH + labelGap;
    parentEditor.softClipperMixSlider.setBounds(sCx - knobSize / 2, sy, knobSize, knobSize);
    parentEditor.softClipperMixLabel.setBounds(sCx - knobSize / 2, sy + knobSize + 2, knobSize, labelH);
    sy += knobSize + labelH + 24 + pad;
    const int softClipperH = juce::jmax(sy - pad, cy - pad);  // Match Compressor height
    parentEditor.softClipperGroup.setBounds(rightColX, pad, colW, softClipperH);

    // --- Lo-Fi + Analog Drift (under Bit Crusher, same width; knobs match tab knobSize) ---
    const int lofiY = by + pad;
    const int lKg = 6;
    const int lofiRowW = 2 * knobSize + lKg;
    int lfy = lofiY + groupTitleH;
    int lRowLeft = leftColX + (colW - lofiRowW) / 2;
    parentEditor.lofiEnabledButton.setBounds(leftColX + pad, lfy, bOnBtnW, bOnBtnH);
    lfy += bOnBtnH + gap;
    parentEditor.lofiAmountLabel.setBounds(lRowLeft, lfy, knobSize, labelH);
    parentEditor.analogDriftLabel.setBounds(lRowLeft + knobSize + lKg, lfy, knobSize, labelH);
    lfy += labelH + labelGap;
    parentEditor.lofiAmountSlider.setBounds(lRowLeft, lfy, knobSize, knobSize);
    parentEditor.analogDriftSlider.setBounds(lRowLeft + knobSize + lKg, lfy, knobSize, knobSize);
    lfy += knobSize + pad + 12;
    // The Lo-Fi bottom is where the Transient starts, and the Final EQ starts below
    // the lower of the two groups beside it. Extend Lo-Fi to that same line, so the
    // Transient and the Final EQ share a top edge -- they already share a bottom
    // edge (see below), so this also makes the two boxes the same height.
    lfy = juce::jmax(lfy, juce::jmax(cy, sy));
    parentEditor.lofiGroup.setBounds(leftColX, lofiY, colW, lfy - lofiY);

    // --- Transient (under Lo-Fi, same column width) ---
    const int transY = lfy + pad;
    int tfy = transY + groupTitleH;
    int tCx = leftColX + colW / 2;
    const int tComboW = 120;
    const int tKg = 6;

    // On button
    parentEditor.transientEnabledButton.setBounds(leftColX + pad, tfy, bOnBtnW, bOnBtnH);
    tfy += bOnBtnH + gap;

    // Type combo, with the Edit button that opens the Waveforms window beside it.
    // The two are centred as one unit; the "Sound" label stays under the combo,
    // which is what it names.
    const int tEditW = 40;
    const int tEditH = 22;
    const int tEditGap = 6;
    const int tComboLeft = tCx - (tComboW + tEditGap + tEditW) / 2;
    parentEditor.transientTypeCombo.setBounds(tComboLeft, tfy, tComboW, 22);
    parentEditor.transientTypeEditButton.setBounds(tComboLeft + tComboW + tEditGap, tfy, tEditW, tEditH);
    tfy += 24 + gap;
    parentEditor.transientTypeLabel.setBounds(tComboLeft, tfy, tComboW, labelH);
    tfy += labelH + labelGap;

    // Post Effect toggle
    parentEditor.transientPostEffectButton.setBounds(tCx - 60, tfy, 120, 20);
    tfy += 20 + gap;

    // Row 1: Length | Ka-Donk | Coarse
    // The two knob-row tops are kept: the Final EQ beside this box lines its own
    // knobs up on them, so the knobs across the bottom half of the tab sit on one
    // grid instead of two that nearly agree.
    const int tRow1Y = tfy;
    const int tTripleW = 3 * knobSize + 2 * tKg;
    int tTripleLeft = leftColX + (colW - tTripleW) / 2;
    parentEditor.transientLengthLabel.setBounds(tTripleLeft, tfy, knobSize, labelH);
    parentEditor.transientKaDonkLabel.setBounds(tTripleLeft + knobSize + tKg, tfy, knobSize, labelH);
    parentEditor.transientCoarseLabel.setBounds(tTripleLeft + 2 * (knobSize + tKg), tfy, knobSize, labelH);
    tfy += labelH + labelGap;
    parentEditor.transientLengthSlider.setBounds(tTripleLeft, tfy, knobSize, knobSize);
    parentEditor.transientKaDonkSlider.setBounds(tTripleLeft + knobSize + tKg, tfy, knobSize, knobSize);
    parentEditor.transientCoarseSlider.setBounds(tTripleLeft + 2 * (knobSize + tKg), tfy, knobSize, knobSize);
    tfy += knobSize + gap;

    // Row 2: Mix on its own, centred and lowest -- the same place the modulation
    // FX groups put theirs. It also lengthens this group, and the Final EQ beside
    // it grows with it because both share this bottom edge.
    const int tRow2Y = tfy;
    parentEditor.transientMixLabel.setBounds(tCx - knobSize / 2, tfy, knobSize, labelH);
    tfy += labelH + labelGap;
    parentEditor.transientMixSlider.setBounds(tCx - knobSize / 2, tfy, knobSize, knobSize);
    tfy += knobSize + 24 + pad;

    parentEditor.transientGroup.setBounds(leftColX, transY, colW, tfy - transY);

    // --- Final EQ (center+right columns, below compressor / soft clipper) ---
    // Top and bottom both taken from the Transient group, so the two boxes line up
    // on each edge and are the same height. Lo-Fi above is stretched down to clear
    // the compressor / soft clipper, so this top is always below them too.
    {
        const int eqTopY  = transY;
        const int eqBotY  = tfy;   // same bottom as Transient
        const int eqWidth = (rightColX + colW) - centerColX;
        int efy = eqTopY + groupTitleH;

        parentEditor.finalEQEnabledButton.setBounds(centerColX + pad, efy, bOnBtnW, bOnBtnH);
        efy += bOnBtnH + gap;

        // Band controls, all acting on the node the Node dropdown names:
        //   Quality / Frequency / Gain stack down the right-hand side, beside the
        //   curve, and Node | Reset | Type sit in a short row under it. Both take
        //   their room out of the curve display, not out of the group, which stays
        //   flush with the Transient.
        const int eqComboH   = 22;
        const int eqNodeW    = 84;
        const int eqResetW   = 56;
        const int eqTypeW    = 110;
        const int eqCtrlGap  = 12;

        // -- Right-hand knob column: three label+knob pairs on the Transient's knob
        // grid. The lower two land exactly on its Length/Ka-Donk/Coarse and Mix rows
        // and Quality carries the same pitch on above them, so the knobs across the
        // whole bottom half of the tab read as one row structure.
        const int eqKnobColW  = knobSize + 2 * gap;
        const int eqKnobColX  = centerColX + eqWidth - pad - eqKnobColW;
        const int eqKnobH     = labelH + labelGap + knobSize;
        const int eqKnobPitch = juce::jmax(eqKnobH + gap, tRow2Y - tRow1Y);   // the Transient's own
        const int eqKnobTopY  = juce::jmax(eqTopY + groupTitleH, tRow2Y - 2 * eqKnobPitch);
        const int eqKnobBotY  = eqKnobTopY + 2 * eqKnobPitch + eqKnobH;

        juce::Slider* eqKnobs[3]  = { &parentEditor.finalEQQSlider,
                                      &parentEditor.finalEQFreqSlider,
                                      &parentEditor.finalEQGainSlider };
        juce::Label*  eqKnobLbl[3] = { &parentEditor.finalEQQLabel,
                                       &parentEditor.finalEQFreqLabel,
                                       &parentEditor.finalEQGainLabel };
        int eky = eqKnobTopY;
        for (int k = 0; k < 3; ++k)
        {
            // The label spans the column, not the knob: "Frequency" does not fit in
            // 56 px, and the labels are centred and transparent.
            eqKnobLbl[k]->setBounds(eqKnobColX, eky, eqKnobColW, labelH);
            eqKnobs[k]->setBounds(eqKnobColX + (eqKnobColW - knobSize) / 2,
                                  eky + labelH + labelGap, knobSize, knobSize);
            eky += eqKnobPitch;
        }

        // -- Display: everything left of the knob column, above the dropdown row.
        // The dropdown row ends level with the knob column rather than at the very
        // bottom of the group, so the block of controls sits together and the group
        // keeps the same clear strip along its bottom edge the Transient has.
        const int eqDisplayX = centerColX + pad;
        const int eqDisplayW = juce::jmax(80, eqKnobColX - gap - eqDisplayX);
        const int eqCtrlRowH = labelH + labelGap + eqComboH;
        const int eqCtrlY    = eqKnobBotY - eqCtrlRowH;

        // -- Dropdown row, centred under the display it belongs to.
        const int eqCtrlRowW = eqNodeW + eqResetW + eqTypeW + 2 * eqCtrlGap;
        int ex = eqDisplayX + (eqDisplayW - eqCtrlRowW) / 2;

        parentEditor.finalEQNodeLabel.setBounds(ex, eqCtrlY, eqNodeW, labelH);
        parentEditor.finalEQNodeCombo.setBounds(ex, eqCtrlY + labelH + labelGap, eqNodeW, eqComboH);
        ex += eqNodeW + eqCtrlGap;

        // No label of its own -- it reads as part of the Node control beside it.
        parentEditor.finalEQResetButton.setBounds(ex, eqCtrlY + labelH + labelGap, eqResetW, eqComboH);
        ex += eqResetW + eqCtrlGap;

        parentEditor.finalEQTypeLabel.setBounds(ex, eqCtrlY, eqTypeW, labelH);
        parentEditor.finalEQTypeCombo.setBounds(ex, eqCtrlY + labelH + labelGap, eqTypeW, eqComboH);

        const int eqDisplayH = juce::jmax(40, eqCtrlY - gap - efy);
        if (parentEditor.finalEQComponent)
            parentEditor.finalEQComponent->setBounds(eqDisplayX, efy, eqDisplayW, eqDisplayH);

        parentEditor.finalEQGroup.setBounds(centerColX, eqTopY, eqWidth, eqBotY - eqTopY);
    }
}

//==============================================================================
// -- SpectralPageComponent Implementation --
//==============================================================================
// -- SpectralPageComponent::GlowOverlayComponent (draws glow on top for cleaner look) --
SpectralPageComponent::GlowOverlayComponent::GlowOverlayComponent(SpectralPageComponent& page)
    : pageRef(page)
{
    setInterceptsMouseClicks(false, false);  // Let clicks pass through to controls
    setAccessible(false);
}

void SpectralPageComponent::GlowOverlayComponent::paint(juce::Graphics& g)
{
    float avgLevel = pageRef.parentEditor.getGlowMeterLevel();  // single per-frame averaged L/R snapshot
    const int baseAlpha = 8 + static_cast<int>(44.0f * avgLevel);
    drawGlows(g, baseAlpha, meterLinkedGroupGlowHue(pageRef.parentEditor.clippingHoldTicks > 0),
        { &pageRef.goniometerGroup, &pageRef.oscilloscopeGroup, &pageRef.spectrumGroup });
}

//==============================================================================
SpectralPageComponent::SpectralPageComponent(SpaceDustAudioProcessorEditor& editor)
    : parentEditor(editor)
{
    setAccessible(false);
    // Mark spectral viewports for glow (synthwave-style, set in editor for all groups)
    goniometerGroup.getProperties().set("viewportGlow", true);
    oscilloscopeGroup.getProperties().set("viewportGlow", true);
    spectrumGroup.getProperties().set("viewportGlow", true);
    addAndMakeVisible(goniometerGroup);
    addAndMakeVisible(oscilloscopeGroup);
    addAndMakeVisible(spectrumGroup);
    oscilloscope = std::make_unique<OscilloscopeComponent>();
    spectrumAnalyser = std::make_unique<SpectrumAnalyserComponent>();
    // Self-drive the spectrum at 60fps, reading a continuous (gap-free) window from
    // the processor's FIFO. Lambda captures the processor, which outlives the editor.
    spectrumAnalyser->fillSamplesCallback = [this](float* dest, int numSamples)
    {
        parentEditor.audioProcessor.readSpectrumSamples(dest, numSamples);
    };
    spectrumAnalyser->start();

    // The scope reads its own gap-free STEREO history. It used to be fed per-block
    // goniometer snapshots, which the UI only sampled 20 times a second while blocks
    // arrived ~86 times a second -- so its long window was stitched from non-adjacent
    // audio and broke up visibly. See OscilloscopeComponent.h.
    oscilloscope->fillSamplesCallback = [this](float* destL, float* destR, int numSamples)
    {
        parentEditor.audioProcessor.readScopeSamples(destL, destR, numSamples);
    };
    oscilloscopeGroup.addAndMakeVisible(*oscilloscope);
    spectrumGroup.addAndMakeVisible(*spectrumAnalyser);
    // Glow overlay on top so Oscilloscope/Spectrum sit behind it - cleaner look
    glowOverlay = std::make_unique<GlowOverlayComponent>(*this);
    addAndMakeVisible(*glowOverlay);
}

void SpectralPageComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0a0a1f));
    float avgLevel = parentEditor.getGlowMeterLevel();  // single per-frame averaged L/R snapshot
    drawStarfield(g, getWidth(), getHeight(), avgLevel);
    if (!lissajousDrawArea.isEmpty())
        drawLissajous(g, lissajousDrawArea, parentEditor.audioProcessor.getGoniometerBuffer(),
                      parentEditor.audioProcessor.getGoniometerValidSamples());
    // Glow drawn by GlowOverlayComponent (on top of Oscilloscope/Spectrum) for cleaner look
}

void SpectralPageComponent::resized()
{
    auto bounds = getLocalBounds();
    if (bounds.isEmpty())
        return;
    const int marginH = 8;        // Left/right margin - match Main tab's outerMargin
    const int marginTop = 8;     // Match Main tab's outerMargin - same distance from top as labels
    const int marginBottom = 20;
    const int pad = 12;
    const int headerH = 28;

    // Row height: Spectrum sets the height; Lissajous+Oscilloscope match it for a clean look
    const float verticalShrink = 0.975f;
    const float spectrumShrink = 0.95f;

    // Content area - top matches Main tab (8px), sides and bottom use standard margin
    auto content = bounds.withTrimmedTop(marginTop).withTrimmedBottom(marginBottom).reduced(marginH, 0);
    const int totalContentH = juce::jmax(100, content.getHeight() - headerH * 2 - pad);
    const int L = totalContentH / 2;

    // Natural row height (Spectrum height) - used to derive the square Lissajous size
    const int rowH = juce::jmax(60, static_cast<int>((L + headerH) * verticalShrink * spectrumShrink));

    const int labelSpace = 32;  // Match groupTitleHeight (title inside box)

    // The Lissajous box reads as a SQUARE: its width fits a square draw area below the
    // label at the natural row height; we then reuse that width as the height for ALL
    // three boxes so the box is square and Oscilloscope/Spectrum share its (shorter)
    // height. This shrinks the rows slightly vertically vs. the natural rowH.
    const int innerH = rowH - 8 - labelSpace;        // draw height at natural rowH
    const int gonioW = juce::jmax(80, innerH + 16);  // square box side (width == height)
    const int squareRowH = gonioW;                   // shared height for all three boxes

    // Top row: Lissajous (square) | Oscilloscope (stretches to fill) - same height as Spectrum
    auto topRow = content.withHeight(squareRowH);
    auto gonioGroupBounds = topRow.withWidth(gonioW);
    goniometerGroup.setBounds(gonioGroupBounds);
    oscilloscopeGroup.setBounds(topRow.withX(gonioGroupBounds.getRight()).withWidth(content.getWidth() - gonioGroupBounds.getWidth()));

    // Bottom row: Spectrum - same height as top row
    auto bottomRow = content.withY(topRow.getBottom() + pad).withHeight(squareRowH);
    spectrumGroup.setBounds(bottomRow);

    // Lissajous: below label, centered square
    auto gonioInner = gonioGroupBounds.reduced(8);
    gonioInner.removeFromTop(labelSpace);
    const int gonioDim = juce::jmax(80, juce::jmin(gonioInner.getWidth(), gonioInner.getHeight()));
    int gx = gonioGroupBounds.getX() + (gonioGroupBounds.getWidth() - gonioDim) / 2;
    int gy = gonioGroupBounds.getY() + labelSpace + (gonioInner.getHeight() - gonioDim) / 2;

    const auto newLissajousArea = juce::Rectangle<int>(gx, gy, gonioDim, gonioDim);

    // Stored ghosts hold absolute coordinates from the old area, so they would smear
    // in the wrong place; drop them and let the trail rebuild over the next few frames.
    if (newLissajousArea != lissajousDrawArea)
        lissajousHistory.clear();

    lissajousDrawArea = newLissajousArea;

    // Oscilloscope: center the component vertically in its box (symmetric top/bottom
    // margins) so the trace sits in the MIDDLE of the box rather than low. The trace is
    // drawn at the component's vertical centre, so equal margins => trace at box centre.
    oscilloscope->setBounds(oscilloscopeGroup.getLocalBounds().reduced(8)
                                .withTrimmedTop(labelSpace - 8)
                                .withTrimmedBottom(labelSpace - 8));
    spectrumAnalyser->setBounds(spectrumGroup.getLocalBounds().withTrimmedTop(labelSpace).reduced(8));

    if (glowOverlay != nullptr)
        glowOverlay->setBounds(bounds);
}

void SpectralPageComponent::drawLissajous(juce::Graphics& g, juce::Rectangle<int> area, const juce::AudioBuffer<float>& buffer, int validSamples)
{
    const int cw = area.getWidth();
    const int ch = area.getHeight();
    if (cw <= 0 || ch <= 0)
        return;

    const float maxGain = 3.981f;     // +12 dB

    bool showClipping = parentEditor.clippingHoldTicks > 0;
    const juce::Colour pathColour = showClipping ? juce::Colour(SpaceDustLookAndFeel::kClipRed) : juce::Colour(0xff48bde8);

    int dim = juce::jmin(cw, ch);
    int margin = juce::jmin(4, dim / 12);
    float halfDim = juce::jmax(8.0f, (dim - 2 * margin) * 0.5f);
    float cx = area.getX() + cw * 0.5f;
    float cy = area.getY() + ch * 0.5f;
    float left   = cx - halfDim;
    float right  = cx + halfDim;
    float bottom = cy + halfDim;
    float top    = cy - halfDim;

    g.saveState();
    g.reduceClipRegion(area);

    const int numS = (validSamples > 0) ? juce::jmin(validSamples, buffer.getNumSamples())
                                        : buffer.getNumSamples();
    if (buffer.getNumChannels() >= 2 && numS > 0)
    {
        juce::Path p;
        for (int i = 0; i < numS; ++i)
        {
            float L = buffer.getSample(0, i);
            float R = buffer.getSample(1, i);
            float S = L - R;
            float M = L + R;
            float xCoord = juce::jmap(S, -maxGain, maxGain, left, right);
            float yCoord = juce::jmap(M, -maxGain, maxGain, bottom, top);  // M pos = top
            xCoord = juce::jlimit(left, right, xCoord);
            yCoord = juce::jlimit(top, bottom, yCoord);
            juce::Point<float> pt(xCoord, yCoord);
            if (i == 0)
                p.startNewSubPath(pt);
            else if (std::isfinite(pt.x) && std::isfinite(pt.y))
                p.lineTo(pt);
        }
        if (!p.isEmpty())
        {
            // Older figures first, so the live one lands on top of its own ghosts.
            SpaceDustDither::ghostTrail(g, lissajousHistory, 2.5f * 1.4f,
                                        kLissajousSpread, kLissajousAlpha, *ditherTiles);

            // Bloom, scaled by the meter -- the same law as every other element.
            if (auto* sdLnf = dynamic_cast<SpaceDustLookAndFeel*>(&getLookAndFeel()))
                sdLnf->glowTrace(g, p, pathColour, 2.5f);

            g.setColour(pathColour);
            g.strokePath(p, juce::PathStrokeType(2.5f));

            // Advance the trail. This page repaints off the editor's timer, in step
            // with the goniometer snapshot it draws, so one frame here is one frame
            // of audio -- the same relationship the scope's history has.
            lissajousHistory.push_back(p);

            while (static_cast<int>(lissajousHistory.size()) > kLissajousHistory)
                lissajousHistory.erase(lissajousHistory.begin());
        }
    }

    g.restoreState();
}

//==============================================================================
// -- Standalone playable keyboard with Z/X octave shift --
// Standalone-only helper: a MidiKeyboardComponent that adds Z = octave down,
// X = octave up to the default QWERTY note mapping. resetAnyKeysInUse() is called
// before each shift so a key held across an octave change can't strand a note-on
// (the note-off would otherwise be computed at the new octave and never match).
//
// It also paints itself, because JUCE's default keyboard is a plain white-and-black
// piano and the panel above it is a dark sky with cyan light on it -- the keys were
// the one part of the Standalone that did not look like the instrument (Giuseppe,
// 2026-08-12). The keybed is dark, the keys are lit from their front edge, and every
// glow here goes through the SAME SpaceDustLookAndFeel helpers the knobs and group
// boxes use, so the keyboard blooms with the rest of the UI off one output level
// and turns red with it on clipping. Nothing about the geometry or the playing
// behaviour changes: only drawKeyboardBackground / drawWhiteNote / drawBlackNote /
// drawUpDownButton are overridden.
namespace
{
    class StandaloneKeyboard : public juce::MidiKeyboardComponent
    {
    public:
        StandaloneKeyboard(juce::MidiKeyboardState& s, Orientation o)
            : juce::MidiKeyboardComponent(s, o), kbState(s)
        {
            // Switch off everything the base class paints that is NOT a key.
            //
            // MidiKeyboardComponent::drawKeyboardBackground is final, so the keybed
            // cannot be replaced by overriding it; paint() below draws ours first and
            // then lets the base run. Clearing these three colours is what stops the
            // base painting its own white fill, top shadow and bottom rule straight
            // over the top of it. The keys themselves are still ours -- drawWhiteNote
            // and drawBlackNote are not final.
            setColour(whiteNoteColourId,        juce::Colours::transparentBlack);
            setColour(shadowColourId,           juce::Colours::transparentBlack);
            setColour(keySeparatorLineColourId, juce::Colours::transparentBlack);

            // colourChanged() has just made us non-opaque, because it judges that by
            // the white-note colour we cleared above. paintKeybed() fills every pixel,
            // so declare it back: an opaque component spares JUCE an alpha pass over
            // the whole strip on every frame the glow moves.
            setOpaque(true);
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            const auto c = key.getTextCharacter();
            if (c == 'z' || c == 'Z') { shiftOctave(-1); return true; }
            if (c == 'x' || c == 'X') { shiftOctave(+1); return true; }
            return juce::MidiKeyboardComponent::keyPressed(key);
        }

        // -- Don't restart a held QWERTY note when focus bounces within our window --
        // Clicking any non-text control (a knob, the background, a tab) makes that control
        // grab keyboard focus; the editor's globalFocusChanged() then hands focus straight
        // back to us. The default MidiKeyboardComponent::focusLost() reacts to that brief
        // loss by note-OFF-ing every held key and clearing keysPressed, so the re-grab's
        // keyStateChanged() sees the key still physically down and re-triggers it - the note
        // audibly restarts on every click. Suppress that reset while focus is merely moving
        // to another control in our own window (and not into a text field, which keeps focus
        // and where releasing a held key should still stop the note). A genuine window-leave
        // leaves getCurrentlyFocusedComponent() == nullptr (or another top-level), so the base
        // reset still runs and notes can't hang when the user switches apps/windows.
        void focusLost(juce::Component::FocusChangeType cause) override
        {
            if (auto* newFocus = juce::Component::getCurrentlyFocusedComponent())
            {
                const bool staysInOurWindow = newFocus->getTopLevelComponent() == getTopLevelComponent();
                const bool intoTextField    = dynamic_cast<juce::TextEditor*>(newFocus) != nullptr;
                if (staysInOurWindow && ! intoTextField)
                    return;  // editor will re-grab focus for us; keep held notes intact
            }
            juce::MidiKeyboardComponent::focusLost(cause);
        }

        //======================================================================
        // -- The keyboard, dressed as part of the panel --

        void paint(juce::Graphics& g) override
        {
            paintKeybed(g, getLocalBounds().toFloat());

            // The base draws the keys through drawWhiteNote / drawBlackNote below.
            // Its own background pass is inert -- see the colours cleared in the ctor.
            juce::MidiKeyboardComponent::paint(g);

            paintHeldKeyBloom(g);
        }

        /** The light a played key throws over the keys around it (Giuseppe,
            2026-08-12: "make the keys glow the same way everything else does when
            I play").

            It is glowAround() -- the same helper the toggles and combo boxes bloom
            with -- so a played key breathes with the output level and goes red on
            clipping exactly when they do. What is different here is only WHEN it is
            drawn, and that is forced by the base class's paint order.

            glowAround() wants to be drawn before the control's own fill, so the fill
            covers the blurred core and only the outward spill survives. Inside
            drawWhiteNote() that does not work: the base draws the naturals left to
            right, so the neighbour drawn next paints over the right-hand half of the
            halo and a held key ends up glowing on one side only. So the bloom runs
            here, after every key is down, and each held key's own face is put back on
            top of its halo -- glowAround()'s contract kept, just resequenced.

            The three passes are the paint order the base itself uses, for the same
            reason: a natural's rectangle runs UNDER the accidentals beside it, so
            re-facing one erases them, and they have to go back before the accidentals
            get their own light. */
        void paintHeldKeyBloom(juce::Graphics& g)
        {
            auto* lf = getSpaceDust();

            if (lf == nullptr || lf->getGlowAmount() <= 0.01f)
                return;

            const auto lit  = litColour(lf);
            const int  mask = getMidiChannelsToDisplay();

            const auto isHeld = [&](int note)
            {
                return kbState.isNoteOnForChannels(mask, note);
            };

            const auto bloomAndReface = [&](int note, bool isBlack)
            {
                const auto area = getRectangleForKey(note);

                if (area.isEmpty())
                    return;

                lf->glowAround(g, keyBody(area), isBlack ? 2.5f : 3.0f, lit);

                if (isBlack) drawBlackNote(note, g, area, true, false, {});
                else         drawWhiteNote(note, g, area, true, false, {}, {});
            };

            // 1. Naturals, with their light over whatever is beside them.
            for (int note = getRangeStart(); note <= getRangeEnd(); ++note)
                if (isHeld(note) && ! juce::MidiMessage::isMidiNoteBlack(note))
                    bloomAndReface(note, false);

            // 2. The accidentals a re-faced natural has just painted over, put back in
            //    the state they were already drawn in. A held one is skipped: pass 3
            //    draws it, lit and blooming.
            for (int note = getRangeStart(); note <= getRangeEnd(); ++note)
            {
                if (! isHeld(note) || juce::MidiMessage::isMidiNoteBlack(note))
                    continue;

                for (const int side : { -1, 1 })
                {
                    const int neighbour = note + side;

                    if (neighbour >= getRangeStart() && neighbour <= getRangeEnd()
                        && juce::MidiMessage::isMidiNoteBlack(neighbour) && ! isHeld(neighbour))
                    {
                        const auto area = getRectangleForKey(neighbour);

                        if (! area.isEmpty())
                            drawBlackNote(neighbour, g, area, false, false, {});
                    }
                }
            }

            // 3. Accidentals last, so their light lands on top of the naturals' --
            //    the order the base draws them in, and the order they sit in.
            for (int note = getRangeStart(); note <= getRangeEnd(); ++note)
                if (isHeld(note) && juce::MidiMessage::isMidiNoteBlack(note))
                    bloomAndReface(note, true);
        }

        /** The keybed the keys sit in.
            The base class fills this with the white-note colour; here it is the dark
            trough under the panel, closed at the top by a lit rail. That rail is the
            seam between the layout above and the keys below, and it is the one line
            in the strip that blooms with the output -- the same edge-of-a-lit-panel
            idea the group boxes' outlines carry. */
        void paintKeybed(juce::Graphics& g, juce::Rectangle<float> area)
        {
            auto* lf = getSpaceDust();

            juce::ColourGradient trough(juce::Colour(kTroughTop),    area.getX(), area.getY(),
                                        juce::Colour(kTroughBottom), area.getX(), area.getBottom(),
                                        false);
            g.setGradientFill(trough);
            g.fillRect(area);

            const auto edge = litColour(lf);
            const auto rail = area.withHeight(1.0f);

            if (lf != nullptr)
                lf->glowRect(g, rail, edge);

            g.setColour(edge.withAlpha(0.55f));
            g.fillRect(rail);
        }

        /** A natural key: a dark slab, lighter towards the player, with a lit front
            lip. The lip is what reads as "this is a key" on a dark keybed, so every
            white note has one; a C has a brighter one, which is what makes the
            octaves countable at a glance now that the keys are no longer white.

            lineColour and textColour are ignored: the separator is the trough showing
            through the gap between slabs, and the note names are drawn in the panel's
            own label cyan rather than in a colour set on the component. */
        void drawWhiteNote(int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area,
                           bool isDown, bool isOver, juce::Colour, juce::Colour) override
        {
            auto* lf = getSpaceDust();
            const auto lit = litColour(lf);
            const float bloom = bloomAmount(lf);

            // Half a pixel off each side leaves the trough visible between slabs, so
            // the keys are separated by the background rather than by a drawn line.
            const auto body = keyBody(area);
            const auto keyShape = bottomRounded(body, 3.0f);
            const bool isC = (midiNoteNumber % 12) == 0;

            if (isDown)
            {
                // The held key's own face brightens with the output, the way the knob
                // arcs fill with it -- a key you are holding through a decayed note is
                // still plainly held, but it is at its brightest while the note sounds.
                const auto face = lit.withMultipliedBrightness(0.6f + 0.4f * bloom);

                g.setGradientFill({ face.withMultipliedBrightness(0.45f), body.getX(), body.getY(),
                                    face,                                body.getX(), body.getBottom(),
                                    false });
            }
            else
            {
                g.setGradientFill({ juce::Colour(kWhiteTop),    body.getX(), body.getY(),
                                    juce::Colour(kWhiteBottom), body.getX(), body.getBottom(),
                                    false });
            }

            g.fillPath(keyShape);

            if (isOver && ! isDown)
            {
                g.setColour(lit.withAlpha(0.12f));
                g.fillPath(keyShape);
            }

            // The front lip: a quiet cyan on a resting key, brighter on a C, and none
            // at all on a held one (the whole face is lit there). It lifts with the
            // output as well, so the whole bed breathes rather than only the key being
            // played. The alphas carry further than they look: the face under them is
            // the title's white now, so a thin cyan wash is all a C needs to count.
            const float lipAlpha = isDown ? 0.0f
                                          : (isC ? 0.70f : 0.35f) * (1.0f + 0.6f * bloom);

            if (lipAlpha > 0.0f)
            {
                g.setColour(lit.withAlpha(lipAlpha));
                g.fillPath(bottomRounded(body.withTop(body.getBottom() - 2.0f), 1.0f));
            }

            // A hairline down the left side keeps neighbouring slabs apart when the
            // gradient brings them close in tone.
            g.setColour(juce::Colour(kSeamDark).withAlpha(isDown ? 0.35f : 0.8f));
            g.fillRect(body.withWidth(1.0f));

            if (const auto text = getWhiteNoteText(midiNoteNumber); text.isNotEmpty())
            {
                g.setFont(lf != nullptr ? lf->getBodyFont(9.0f, true)
                                        : juce::Font(juce::FontOptions(9.0f)));

                // Both faces are light now -- the title's white at rest, lit cyan
                // when held -- so the name is dark on either, as on a real keybed.
                g.setColour(juce::Colour(kTroughBottom).withAlpha(isDown ? 0.85f : 0.55f));

                g.drawText(text, body.withTrimmedBottom(4.0f).toNearestInt(),
                           juce::Justification::centredBottom, false);
            }
        }

        /** An accidental: near-black, so it still reads as the shorter, darker key on
            a dark keybed, held apart from its neighbours by a cyan rim rather than by
            being a different black. Lit, it is the same cyan a natural goes. */
        void drawBlackNote(int, juce::Graphics& g, juce::Rectangle<float> area,
                           bool isDown, bool isOver, juce::Colour) override
        {
            auto* lf = getSpaceDust();
            const auto lit = litColour(lf);
            const float bloom = bloomAmount(lf);

            const auto body = keyBody(area);
            const auto keyShape = bottomRounded(body, 2.5f);

            if (isDown)
            {
                // Brightens with the output, exactly as a natural does.
                const auto face = lit.withMultipliedBrightness(0.6f + 0.4f * bloom);

                g.setGradientFill({ face.withMultipliedBrightness(0.35f), body.getX(), body.getY(),
                                    face,                                body.getX(), body.getBottom(),
                                    false });
            }
            else
            {
                g.setGradientFill({ juce::Colour(kBlackTop),    body.getX(), body.getY(),
                                    juce::Colour(kBlackBottom), body.getX(), body.getBottom(),
                                    false });
            }

            g.fillPath(keyShape);

            if (isOver && ! isDown)
            {
                g.setColour(lit.withAlpha(0.15f));
                g.fillPath(keyShape);
            }

            if (! isDown)
            {
                g.setColour(lit.withAlpha(0.22f));
                g.strokePath(keyShape, juce::PathStrokeType(1.0f));
            }
        }

        /** The octave-scroll buttons at the two ends of the strip. Drawn in the same
            trough colours as the keybed with a cyan arrow, so the ends of the
            keyboard do not fall back to JUCE's grey. */
        void drawUpDownButton(juce::Graphics& g, int w, int h,
                              bool isMouseOver, bool isButtonPressed, bool movesOctavesUp) override
        {
            auto* lf = getSpaceDust();
            const auto lit = litColour(lf);
            const auto bounds = juce::Rectangle<float>(0.0f, 0.0f, (float) w, (float) h);

            g.setGradientFill({ juce::Colour(kTroughTop),    bounds.getX(), bounds.getY(),
                                juce::Colour(kTroughBottom), bounds.getX(), bounds.getBottom(),
                                false });
            g.fillRect(bounds);

            juce::Path arrow;
            arrow.addTriangle(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.5f);

            if (! movesOctavesUp)
                arrow.applyTransform(juce::AffineTransform::rotation(juce::MathConstants<float>::pi,
                                                                    0.5f, 0.5f));

            arrow.applyTransform(arrow.getTransformToScaleToFit(bounds.reduced(3.0f, (float) h * 0.4f),
                                                                true));

            g.setColour(lit.withAlpha(isButtonPressed ? 1.0f : (isMouseOver ? 0.75f : 0.4f)));
            g.fillPath(arrow);
        }

    private:
        //======================================================================
        // -- Keybed palette --
        // The panel's own colours: the trough is the background the group boxes sit
        // on, and every lit thing here is the knob-arc cyan reached through the
        // LookAndFeel, so clipping turns the keys red with everything else.
        static constexpr juce::uint32 kTroughTop    = 0xff0b0d1a;
        static constexpr juce::uint32 kTroughBottom = 0xff05060e;
        // The naturals are the hue of the SPACE DUST title above them (Giuseppe,
        // 2026-08-12): the title's white, cooled towards the panel's light cyan at
        // the back of the key and full-white at the front lip the player sees most.
        static constexpr juce::uint32 kWhiteTop     = 0xffb9dcee;
        static constexpr juce::uint32 kWhiteBottom  = 0xfff2fbff;
        static constexpr juce::uint32 kBlackTop     = 0xff05060e;
        static constexpr juce::uint32 kBlackBottom  = 0xff121729;
        static constexpr juce::uint32 kSeamDark     = 0xff05060e;

        SpaceDustLookAndFeel* getSpaceDust()
        {
            return dynamic_cast<SpaceDustLookAndFeel*>(&getLookAndFeel());
        }

        /** The colour everything lit in the strip is drawn in. Taken from the
            LookAndFeel so it goes red on clipping exactly when the knobs do; the
            literal is only reached if this is ever dropped into a bare component. */
        static juce::Colour litColour(SpaceDustLookAndFeel* lf)
        {
            return lf != nullptr ? lf->getMeterResponsiveKnobArcColour()
                                 : juce::Colour(0xff00d4ff);
        }

        /** How brightly the strip is lit right now, 0..1.

            getGlowAmount() is the law every glowing thing in the plugin obeys, but it
            tops out at kGlowTrim rather than at 1 -- it is an ALPHA, and the trim is
            the master pull-back on all of them. Dividing by the trim turns it back
            into a plain "how loud is it" for the places here that scale a colour
            rather than an alpha, so the keys follow the same curve as the knob arcs
            without needing the trim's value baked in. */
        static float bloomAmount(SpaceDustLookAndFeel* lf)
        {
            if (lf == nullptr)
                return 0.0f;

            return juce::jlimit(0.0f, 1.0f,
                                lf->getGlowAmount() / SpaceDustLookAndFeel::kGlowTrim);
        }

        /** The part of a key's slot that is actually painted. Half a pixel off each
            side leaves the trough showing between slabs, and a pixel off the top keeps
            the keys clear of the lit rail. One definition, because the bloom pass has
            to hand glowAround() the very same footprint the key was drawn in. */
        static juce::Rectangle<float> keyBody(juce::Rectangle<float> keySlot)
        {
            return keySlot.reduced(0.5f, 0.0f).withTrimmedTop(1.0f);
        }

        /** A key: square where it meets the keybed, rounded at the front edge. */
        static juce::Path bottomRounded(juce::Rectangle<float> r, float corner)
        {
            juce::Path p;
            p.addRoundedRectangle(r.getX(), r.getY(), r.getWidth(), r.getHeight(),
                                  corner, corner,
                                  false, false, true, true);
            return p;
        }

        juce::MidiKeyboardState& kbState;
        int baseOctave = 4;  // matches setKeyPressBaseOctave(4) in the editor ctor

        void shiftOctave(int delta)
        {
            // Clamp so the QWERTY range (offset 0..16) stays within MIDI 0..127.
            const int newOctave = juce::jlimit(0, 9, baseOctave + delta);
            if (newOctave == baseOctave)
                return;

            kbState.allNotesOff(getMidiChannel());  // release anything held so it can't hang
            baseOctave = newOctave;
            setKeyPressBaseOctave(baseOctave);
            // Scroll the visible keys so the new playable range is on-screen,
            // with about one octave of context below it.
            setLowestVisibleKey(juce::jlimit(0, 108, 12 * baseOctave - 12));
        }
    };

    // Standalone: stop knobs / buttons / combos from grabbing keyboard focus when
    // clicked, so the QWERTY computer-keyboard keeps playing notes while the user
    // twists controls. Text fields are intentionally left alone so typing (preset
    // names, search) still works. Applied recursively to every descendant.
    //==========================================================================
    // -- Cache every label's rendering --
    // The whole-face repaint that animates the glow re-renders every descendant, and
    // b16fc8d measured where that goes: "full text layout (drawLabel -> drawText ->
    // GlyphArrangement) for every label plus every rotary, toggle and group outline",
    // ~130% CPU. That commit stopped the repaint happening at SILENCE; while notes are
    // sounding the level moves every tick and it still runs at 20Hz.
    //
    // Labels are the half of that cost which is pure waste: their text does not change
    // when the glow level does, so they lay out the same glyphs 20 times a second to
    // produce identical pixels. Buffered, they blit a cached image instead.
    //
    // Safe under mainView's scale transform: JUCE's StandardCachedComponentImage sizes
    // the cache by g.getInternalContext().getPhysicalPixelScaleFactor(), so it is
    // rendered at true on-screen resolution and re-rendered when a drag-resize changes
    // that factor. Text stays sharp at every zoom.
    //
    // Labels ONLY. Sliders are deliberately excluded: the knob arcs fill by output level
    // (setOutputMeterLevel), so they genuinely have new pixels every frame and caching
    // them would add an image copy to work that has to happen anyway.
    void cacheLabelRendering(juce::Component& parent)
    {
        for (int i = 0; i < parent.getNumChildComponents(); ++i)
        {
            if (auto* child = parent.getChildComponent(i))
            {
                if (dynamic_cast<juce::Label*>(child) != nullptr)
                    child->setBufferedToImage(true);

                cacheLabelRendering(*child);
            }
        }
    }

    void preventControlsStealingKeyboardFocus(juce::Component& parent)
    {
        for (int i = 0; i < parent.getNumChildComponents(); ++i)
        {
            if (auto* child = parent.getChildComponent(i))
            {
                if (dynamic_cast<juce::Slider*>  (child) != nullptr
                 || dynamic_cast<juce::Button*>  (child) != nullptr
                 || dynamic_cast<juce::ComboBox*>(child) != nullptr)
                {
                    child->setMouseClickGrabsKeyboardFocus(false);
                }
                preventControlsStealingKeyboardFocus(*child);
            }
        }
    }
}

//==============================================================================
// -- Constructor --


SpaceDustAudioProcessorEditor::SpaceDustAudioProcessorEditor(SpaceDustAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p),
      tabbedComponent(juce::TabbedButtonBar::TabsAtTop),
      oscillatorsGroup("Oscillators", "Oscillators"),
      filterGroup("Filter", "Filter"),
      filterEnvGroup("Filter Envelope", "Filter Envelope"),
      voiceGroup("Voice", "Voice"),
      envelopeGroup("Amp Envelope", "Amp Envelope"),
      masterGroup("Master", "Master"),
      modulationGroup("", ""),  // Empty title, using separate label for "Modulation" title
      lfo1Group("LFO 1", "LFO 1"),
      lfo2Group("LFO 2", "LFO 2"),
      modFilter1Group("Filter 1", "Filter 1"),
      modFilter2Group("Filter 2", "Filter 2"),
      delayGroup("Delay", "Delay"),
      reverbGroup("Reverb", "Reverb"),
      grainDelayGroup("Grain Delay", "Grain Delay"),
      phaserGroup("Phaser", "Phaser"),
      flangerGroup("Flanger", "Flanger"),
      bitCrusherGroup("Bit Crusher", "Bit Crusher"),
      softClipperGroup("Soft Clipper", "Soft Clipper"),
      compressorGroup("Compressor", "Compressor"),
      transientGroup("Transient", "Transient"),
      lofiGroup("Lo-Fi", "Lo-Fi"),
      tranceGateGroup("Trance Gate", "Trance Gate"),
      delayFilterGroup("Filter", "Filter")
{
    //==============================================================================
    // -- DEBUG: Editor Constructor Start --
    DBG("Space Dust: Editor ctor START - initializer list completed");
    
    #if JUCE_DEBUG
    try
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeString("SpaceDust_DebugLog.txt"));
        juce::FileOutputStream out(logFile);
        if (out.openedOk())
        {
            out.setPosition(out.getPosition());  // Append
            out.writeText("Space Dust: Editor ctor START\n", false, false, nullptr);
            out.flush();
        }
    }
    catch (...) {}
    #endif
    
    //==============================================================================
    // -- CRITICAL: LookAndFeel Setup (FIRST, before any component operations) --
    // CRITICAL: LookAndFeel must be set BEFORE any component operations
    // This prevents LookAndFeel access issues and juce_LookAndFeel.cpp:82 assertions
    DBG("Space Dust: Editor ctor - Creating LookAndFeel...");
    try
    {
        // LookAndFeel is already created (member variable), just set it
        DBG("Space Dust: Editor ctor - Setting L&F...");
        setLookAndFeel(&customLookAndFeel);
        #if JUCE_DEBUG
        try
        {
            juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile("SpaceDust_DebugLog.txt");
            juce::FileOutputStream out(logFile);
            if (out.openedOk())
            {
                out.setPosition(out.getPosition());
                out.writeText("Space Dust: LookAndFeel set successfully\n", false, false, nullptr);
                out.flush();
            }
        }
        catch (...) {}
        #endif
        DBG("Space Dust: Editor ctor - LookAndFeel set successfully");
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception setting LookAndFeel: " + juce::String(e.what()));
        // Continue anyway - components will use default LookAndFeel
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception setting LookAndFeel");
        // Continue anyway - components will use default LookAndFeel
    }
    
    //==============================================================================
    // -- Disable Accessibility (Performance Optimization) --
    // Disable accessibility for all components to improve performance and stability
    // This is safe for audio plugins that don't require screen reader support
    #if JUCE_DEBUG
    try
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeString("SpaceDust_DebugLog.txt"));
        juce::FileOutputStream out(logFile);
        if (out.openedOk())
        {
            out.setPosition(out.getPosition());
            out.writeText("Space Dust: About to call setAccessible(false)\n", false, false, nullptr);
            out.flush();
        }
    }
    catch (...) {}
    #endif
    
    setAccessible(false);

    // Viewport glow on all GroupComponents (synthwave aesthetic, matches synth color scheme)
    oscillatorsGroup.getProperties().set("viewportGlow", true);
    filterGroup.getProperties().set("viewportGlow", true);
    filterEnvGroup.getProperties().set("viewportGlow", true);
    envelopeGroup.getProperties().set("viewportGlow", true);
    masterGroup.getProperties().set("viewportGlow", true);
    modulationGroup.getProperties().set("viewportGlow", true);
    lfo1Group.getProperties().set("viewportGlow", true);
    lfo2Group.getProperties().set("viewportGlow", true);
    modFilter1Group.getProperties().set("viewportGlow", true);
    modFilter2Group.getProperties().set("viewportGlow", true);
    mpeGroup.getProperties().set("viewportGlow", true);
    delayGroup.getProperties().set("viewportGlow", true);
    reverbGroup.getProperties().set("viewportGlow", true);
    grainDelayGroup.getProperties().set("viewportGlow", true);
    phaserGroup.getProperties().set("viewportGlow", true);
    flangerGroup.getProperties().set("viewportGlow", true);
    bitCrusherGroup.getProperties().set("viewportGlow", true);
    softClipperGroup.getProperties().set("viewportGlow", true);
    compressorGroup.getProperties().set("viewportGlow", true);
    lofiGroup.getProperties().set("viewportGlow", true);
    transientGroup.getProperties().set("viewportGlow", true);
    tranceGateGroup.getProperties().set("viewportGlow", true);
    delayFilterGroup.getProperties().set("viewportGlow", true);

    // TooltipWindow: required for setTooltip() to display (e.g. Pan labels)
    tooltipWindow = std::make_unique<juce::TooltipWindow>(this, 500);
    
    #if JUCE_DEBUG
    try
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeString("SpaceDust_DebugLog.txt"));
        juce::FileOutputStream out(logFile);
        if (out.openedOk())
        {
            out.setPosition(out.getPosition());
            out.writeText("Space Dust: setAccessible(false) completed\n", false, false, nullptr);
            out.flush();
        }
    }
    catch (...) {}
    #endif
    
    //==============================================================================
    // -- Preset Manager Setup --
    presetManager = std::make_unique<PresetManager>(audioProcessor.getValueTreeState());
    presetManager->setCurrentPresetName(audioProcessor.currentPresetName);

    presetCombo.setTextWhenNothingSelected(audioProcessor.currentPresetName);
    presetCombo.setTooltip("Select a preset");
    presetCombo.onChange = [this]()
    {
        auto presets = presetManager->getAvailablePresets();
        int idx = presetCombo.getSelectedItemIndex();
        if (idx >= 0 && idx < presets.size())
        {
            presetManager->loadPreset(presets[idx]);
            audioProcessor.currentPresetName = presetManager->getCurrentPresetName();
            audioProcessor.updateVoicesWithParameters();
        }
    };
    addAndMakeVisible(presetCombo);

    savePresetButton.setTooltip("Save current settings as a preset");
    savePresetButton.onClick = [this]() { showSavePresetDialog(); };
    addAndMakeVisible(savePresetButton);

    initPresetButton.setTooltip("Reset all parameters to defaults and clear every imported sample");
    initPresetButton.onClick = [this]()
    {
        presetManager->loadInitPreset();

        // After the parameters, not before: they have just gone back to their
        // defaults, so every waveform dropdown is on a built-in shape and no
        // selection is left pointing at a slot this is about to empty. Clearing
        // the slots calls back into rebuildWaveformMenus, which then finds the
        // menus and the parameters already agreeing.
        audioProcessor.getUserWaveLibrary().clearAllSlots();
        hideCheezeGuyTab();

        audioProcessor.currentPresetName = "Init";
        audioProcessor.updateVoicesWithParameters();
        presetCombo.setSelectedId(0, juce::dontSendNotification);
        presetCombo.setTextWhenNothingSelected("Init");
    };
    addAndMakeVisible(initPresetButton);

    folderPresetButton.setTooltip("Select folder for presets");
    folderPresetButton.onClick = [this]()
    {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Select Preset Folder",
            presetManager->getPresetFolder(),
            "");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this, chooser](const juce::FileChooser& fc)
            {
                auto result = fc.getResult();
                if (result.exists() && result.isDirectory())
                {
                    presetManager->setPresetFolder(result);
                    refreshPresetList();
                }
            });
    };
    addAndMakeVisible(folderPresetButton);

    refreshPresetList();

    //==============================================================================
    // -- Oscillators Section Setup --
    #if JUCE_DEBUG
    try
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeString("SpaceDust_DebugLog.txt"));
        juce::FileOutputStream out(logFile);
        if (out.openedOk())
        {
            out.setPosition(out.getPosition());
            out.writeText("Space Dust: Setting up Oscillators section\n", false, false, nullptr);
            out.flush();
        }
    }
    catch (...) {}
    #endif

    DBG("Space Dust: Setting up Oscillators section");
    
    // Note: Components will be added to page components, not directly to editor
    
    // Oscillator 1
    #if JUCE_DEBUG
    try
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeString("SpaceDust_DebugLog.txt"));
        juce::FileOutputStream out(logFile);
        if (out.openedOk())
        {
            out.setPosition(out.getPosition());
            out.writeText("Space Dust: Setting up osc1WaveformCombo\n", false, false, nullptr);
            out.flush();
        }
    }
    catch (...) {}
    #endif
    
    // Item ids run 1..N in list order and the parameter's choices run 0..N-1 in
    // the same order, so a row's id is its choice index plus one. The Waveforms
    // panel leans on that -- see rowForSelection() -- so the built-ins are added
    // straight from OscShape rather than written out by hand, where a typo or an
    // omission would put the two lists out of step.
    for (int i = 0; i < OscShape::numShapes; ++i)
        osc1WaveformCombo.addItem(safeString(OscShape::names[i]), i + 1);

    // The imported waveforms are added by rebuildWaveformMenus() once every combo
    // exists, and only the slots that actually hold something get an entry.
    osc1WaveformCombo.setSelectedId(2);  // Default to Triangle
    
    #if JUCE_DEBUG
    try
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeString("SpaceDust_DebugLog.txt"));
        juce::FileOutputStream out(logFile);
        if (out.openedOk())
        {
            out.setPosition(out.getPosition());
            out.writeText("Space Dust: Creating osc1WaveformAttachment\n", false, false, nullptr);
            out.flush();
        }
    }
    catch (...) {}
    #endif
    
    // CRITICAL: Verify parameter exists before creating attachment
    // This prevents crashes if parameter ID doesn't match
    #if JUCE_DEBUG
    try
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeString("SpaceDust_DebugLog.txt"));
        juce::FileOutputStream out(logFile);
        if (out.openedOk())
        {
            out.setPosition(out.getPosition());
            out.writeText("Space Dust: Checking if osc1Waveform parameter exists\n", false, false, nullptr);
            out.flush();
        }
    }
    catch (...) {}
    #endif
    
    if (auto* osc1WaveformParam = audioProcessor.getValueTreeState().getParameter("osc1Waveform"))
        osc1WaveformAttachment = std::make_unique<WaveformChoiceAttachment>(
            *osc1WaveformParam, osc1WaveformCombo);
    osc1WaveformLabel.setText(safeString("Waveform 1"), juce::dontSendNotification);
    osc1WaveformLabel.setJustificationType(juce::Justification::centred);
    osc1WaveformLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    osc1WaveformLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    osc1CoarseTuneSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    osc1CoarseTuneSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    osc1CoarseTuneSlider.setTextValueSuffix(" st");
    osc1CoarseTuneAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "osc1CoarseTune", osc1CoarseTuneSlider);
    osc1CoarseTuneLabel.setText(safeString("Coarse"), juce::dontSendNotification);
    osc1CoarseTuneLabel.setJustificationType(juce::Justification::centred);
    osc1CoarseTuneLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    osc1CoarseTuneLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    osc1DetuneSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    osc1DetuneSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    osc1DetuneSlider.setTextValueSuffix(" ct");
    osc1DetuneAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "osc1Detune", osc1DetuneSlider);
    osc1DetuneLabel.setText(safeString("Detune"), juce::dontSendNotification);
    osc1DetuneLabel.setJustificationType(juce::Justification::centred);
    osc1DetuneLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    osc1DetuneLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    osc1LevelSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    osc1LevelSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    osc1LevelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "osc1Level", osc1LevelSlider);
    osc1LevelLabel.setText(safeString("Level"), juce::dontSendNotification);
    osc1LevelLabel.setJustificationType(juce::Justification::centred);
    osc1LevelLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    osc1LevelLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    osc1PanSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    osc1PanSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    osc1PanAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "osc1Pan", osc1PanSlider);
    osc1PanLabel.setText(safeString("Pan"), juce::dontSendNotification);
    osc1PanLabel.setJustificationType(juce::Justification::centred);
    osc1PanLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    osc1PanLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    osc1PanLabel.setInterceptsMouseClicks(true, true);  // Clickable to reset pan to center
    
    // Oscillator 2
    for (int i = 0; i < OscShape::numShapes; ++i)
        osc2WaveformCombo.addItem(safeString(OscShape::names[i]), i + 1);
    osc2WaveformCombo.setSelectedId(2);  // Default to Triangle
    if (auto* osc2WaveformParam = audioProcessor.getValueTreeState().getParameter("osc2Waveform"))
        osc2WaveformAttachment = std::make_unique<WaveformChoiceAttachment>(
            *osc2WaveformParam, osc2WaveformCombo);
    osc2WaveformLabel.setText(safeString("Waveform 2"), juce::dontSendNotification);
    osc2WaveformLabel.setJustificationType(juce::Justification::centred);
    osc2WaveformLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    osc2WaveformLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    osc2CoarseTuneSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    osc2CoarseTuneSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    osc2CoarseTuneSlider.setTextValueSuffix(" st");
    osc2CoarseTuneAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "osc2CoarseTune", osc2CoarseTuneSlider);
    osc2CoarseTuneLabel.setText(safeString("Coarse"), juce::dontSendNotification);
    osc2CoarseTuneLabel.setJustificationType(juce::Justification::centred);
    osc2CoarseTuneLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    osc2CoarseTuneLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    osc2DetuneSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    osc2DetuneSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    osc2DetuneSlider.setTextValueSuffix(" ct");
    osc2DetuneAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "osc2Detune", osc2DetuneSlider);
    osc2DetuneLabel.setText(safeString("Detune"), juce::dontSendNotification);
    osc2DetuneLabel.setJustificationType(juce::Justification::centred);
    osc2DetuneLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    osc2DetuneLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    osc2LevelSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    osc2LevelSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    osc2LevelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "osc2Level", osc2LevelSlider);
    osc2LevelLabel.setText(safeString("Level"), juce::dontSendNotification);
    osc2LevelLabel.setJustificationType(juce::Justification::centred);
    osc2LevelLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    osc2LevelLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    osc2PanSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    osc2PanSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    osc2PanAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "osc2Pan", osc2PanSlider);
    osc2PanLabel.setText(safeString("Pan"), juce::dontSendNotification);
    osc2PanLabel.setJustificationType(juce::Justification::centred);
    osc2PanLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    osc2PanLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    osc2PanLabel.setInterceptsMouseClicks(true, true);  // Clickable to reset pan to center
    
    // Noise
    noiseColorCombo.addItem(safeString("White"), 1);
    noiseColorCombo.addItem(safeString("Pink"), 2);
    noiseColorCombo.setLookAndFeel(&customLookAndFeel);
    // Attach to the "noiseType" parameter (White=0, Pink=1) so it can be automated
    // and saved like any other control. Items must be added before the attachment.
    if (auto* noiseTypeParam = audioProcessor.getValueTreeState().getParameter("noiseType"))
        noiseColorAttachment = std::make_unique<WaveformChoiceAttachment>(
            *noiseTypeParam, noiseColorCombo);
    noiseColorLabel.setText(safeString("Noize Type"), juce::dontSendNotification);
    noiseColorLabel.setJustificationType(juce::Justification::centred);
    noiseColorLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    noiseColorLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    //==========================================================================
    // -- The three buttons that open the Waveforms window --
    // One beside each dropdown that can select an imported sample. They open the
    // same window; the only difference is which slot it lands on, taken from what
    // that dropdown is currently set to.
    {
        using Kind = WaveformEditorComponent::BuiltInKind;

        struct EditButtonTarget
        {
            SpaceDustToggleStyleButton* button;
            juce::ComboBox* combo;
            int userBase;
            Kind kind;
            UserWave::Group group;
            const char* tip;
        };

        // The sub oscillator's and the Transient's buttons are set up here too,
        // even though their dropdowns are built further down and in other
        // sections of the panel: what makes these five one thing is that they all
        // open the same window, and that is easier to see when they are written
        // out together. Each one opens it on its OWN eight slots -- the group
        // column -- so a sample loaded for one of them changes that waveform and
        // no other.
        const EditButtonTarget targets[] =
        {
            { &osc1WaveformEditButton,   &osc1WaveformCombo,   UserWave::oscUserBase,       Kind::Shapes,
              UserWave::Group::Osc1,      "Import a sample as an Oscillator 1 waveform" },
            { &osc2WaveformEditButton,   &osc2WaveformCombo,   UserWave::oscUserBase,       Kind::Shapes,
              UserWave::Group::Osc2,      "Import a sample as an Oscillator 2 waveform" },
            { &noiseWaveformEditButton,  &noiseColorCombo,     UserWave::noiseUserBase,     Kind::Noise,
              UserWave::Group::Noise,     "Import a sample as a Noize source" },
            { &subOscWaveformEditButton, &subOscWaveformCombo, UserWave::oscUserBase,       Kind::Shapes,
              UserWave::Group::Sub,       "Import a sample as the Sub Oscillator waveform" },
            { &transientTypeEditButton,  &transientTypeCombo,  UserWave::transientUserBase, Kind::Drums,
              UserWave::Group::Transient, "Import a sample to play as the Transient hit" },
        };

        for (const auto& target : targets)
        {
            // No colours set here. The button paints itself through the same
            // routine that draws the toggles beside it, so its navy, its border
            // and its bloom all come from that one place.
            target.button->setButtonText(safeString("Edit"));
            target.button->setTooltip(safeString(target.tip));

            auto* combo = target.combo;
            auto* button = target.button;
            const int userBase = target.userBase;
            const Kind kind = target.kind;
            const auto group = target.group;
            target.button->onClick = [this, button, combo, userBase, kind, group]
            {
                openWaveformWindow(button, combo, userBase, kind, group);
            };
        }
    }

    //==========================================================================
    // -- The five shaping knobs per oscillator --
    // Bend +, Bend -, Bend +/-, Spectrum and Sync. Any of them may be turned up
    // together; see PhaseShaper.h. Built here and lent to the Waveforms panel.
    {
        const char* const shapingLabels[numShapingKnobs] =
            { "Bend +", "Bend -", "Bend +/-", "Spectrum", "Sync" };

        // From the one list at the top of this file, which wrapAssignableKnobs()
        // also reads -- so the attachments and the assign-mode wrappers cannot
        // name different parameters.
        static_assert(kNumShapingIds == numShapingKnobs, "shaping id table is the wrong length");

        auto* const* const osc1Ids   = kOsc1ShapingIds;
        auto* const* const osc2Ids   = kOsc2ShapingIds;
        auto* const* const subOscIds = kSubOscShapingIds;

        const char* const shapingTips[numShapingKnobs] =
        {
            "Leans the cycle forward, so the waveform grows a hard edge.",
            "Leans the cycle back, so the waveform softens. Turned up with Bend + it cancels it.",
            "Pulls both ends of the cycle away from its middle. Hollow.",
            "Fades the waveform towards a plain sine -- the upper harmonics going.",
            "Restarts the waveform several times per note: the hard sync tear."
        };

        // Three, not two. The sub oscillator is a real oscillator reading a real
        // cycle, so it gets the same five on its own parameters -- which is what
        // makes its Waveforms panel the same panel as the other two.
        for (int osc = 0; osc < 3; ++osc)
        {
            auto* sliders = osc == 0 ? osc1ShapingSliders : osc == 1 ? osc2ShapingSliders : subOscShapingSliders;
            auto* labels = osc == 0 ? osc1ShapingLabels : osc == 1 ? osc2ShapingLabels : subOscShapingLabels;
            auto* attachments = osc == 0 ? osc1ShapingAttachments : osc == 1 ? osc2ShapingAttachments : subOscShapingAttachments;
            auto* const* ids = osc == 0 ? osc1Ids : osc == 1 ? osc2Ids : subOscIds;
            auto& controls = osc == 0 ? osc1ShapingControls : osc == 1 ? osc2ShapingControls : subOscShapingControls;

            for (int i = 0; i < numShapingKnobs; ++i)
            {
                auto& s = sliders[i];
                s.setSliderStyle(juce::Slider::RotaryVerticalDrag);
                s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 46, 16);
                s.setLookAndFeel(&customLookAndFeel);
                s.setTooltip(safeString(shapingTips[i]));

                attachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                    audioProcessor.getValueTreeState(), ids[i], s);

                // AFTER the attachment. The attachment sets the slider's range
                // from the parameter, and setting the range resets how many
                // decimals it shows -- so doing this first left every knob
                // reading 0.00000 instead of 0.00.
                s.setNumDecimalPlacesToDisplay(2);

                auto& l = labels[i];
                l.setText(safeString(shapingLabels[i]), juce::dontSendNotification);
                l.setJustificationType(juce::Justification::centred);
                l.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
                l.setFont(customLookAndFeel.getBodyFont(11.0f, true));

                controls.knobs[i] = &s;
                controls.labels[i] = &l;
            }
        }

        //======================================================================
        // -- Voices, Detune and Width --
        const char* const unisonLabels[numUnisonKnobs] = { "Voices", "Detune", "Width", "Random Phase" };

        // Same one list at the top of this file, for the same reason.
        static_assert(kNumUnisonIds == numUnisonKnobs, "unison id table is the wrong length");

        auto* const* const osc1UnisonIds   = kOsc1UnisonIds;
        auto* const* const osc2UnisonIds   = kOsc2UnisonIds;
        auto* const* const subOscUnisonIds = kSubOscUnisonIds;
        auto* const* const noiseUnisonIds  = kNoiseUnisonIds;

        const char* const unisonTips[numUnisonKnobs] =
        {
            "How many copies of this oscillator play at once. One is the plain oscillator.",
            "How far apart the copies are tuned. At zero they sit on top of each other.",
            "How far the copies are spread across the stereo field. At zero they are all centred.",
            "How far apart the copies START in the cycle. At zero they all start together, which makes the first instant of a note louder than the rest of it."
        };

        // The noise source gets its OWN three tips. Detune is the reason: on
        // built-in White and Pink there is no pitch to pull apart, and a tooltip
        // that says there is would be the knob lying about itself.
        const char* const noiseUnisonTips[numUnisonKnobs] =
        {
            "How many independent noise streams play at once. Turn Width up to hear them.",
            "Only for an imported sample, which has a pitch. White and Pink have none, so this does nothing to them.",
            "How far the streams are spread across the stereo field. This is what makes the noise wide.",
            "Only for an imported sample, which has a cycle to start in. White and Pink have no phase, so this does nothing to them."
        };

        // A table rather than a fourth arm on four ternary chains. Four sources
        // now share this loop and the chains had stopped being readable.
        struct UnisonGroupSetup
        {
            juce::Slider* sliders;
            juce::Label* labels;
            std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>* attachments;
            const char* const* ids;
            WaveformEditorComponent::ShapingControls* controls;
            const char* const* tips;
        };

        const UnisonGroupSetup unisonGroups[] =
        {
            { osc1UnisonSliders,   osc1UnisonLabels,   osc1UnisonAttachments,
              osc1UnisonIds,   &osc1ShapingControls,   unisonTips },
            { osc2UnisonSliders,   osc2UnisonLabels,   osc2UnisonAttachments,
              osc2UnisonIds,   &osc2ShapingControls,   unisonTips },
            { subOscUnisonSliders, subOscUnisonLabels, subOscUnisonAttachments,
              subOscUnisonIds, &subOscShapingControls, unisonTips },
            { noiseUnisonSliders,  noiseUnisonLabels,  noiseUnisonAttachments,
              noiseUnisonIds,  &noiseShapingControls,  noiseUnisonTips },
        };

        for (const auto& group : unisonGroups)
        {
            for (int i = 0; i < numUnisonKnobs; ++i)
            {
                auto& s = group.sliders[i];
                s.setSliderStyle(juce::Slider::RotaryVerticalDrag);
                s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 46, 16);
                s.setLookAndFeel(&customLookAndFeel);
                s.setTooltip(safeString(group.tips[i]));

                group.attachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                    audioProcessor.getValueTreeState(), group.ids[i], s);

                // AFTER the attachment, which sets the range and resets this.
                // Voices is a count, so it shows none: "3", not "3.00".
                s.setNumDecimalPlacesToDisplay(i == 0 ? 0 : 2);

                auto& l = group.labels[i];
                l.setText(safeString(unisonLabels[i]), juce::dontSendNotification);
                l.setJustificationType(juce::Justification::centred);
                l.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
                l.setFont(customLookAndFeel.getBodyFont(11.0f, true));

                group.controls->unisonKnobs[i] = &s;
                group.controls->unisonLabels[i] = &l;
            }
        }
    }



    noiseLevelSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    noiseLevelSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    noiseLevelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "noiseLevel", noiseLevelSlider);
    noiseLevelLabel.setText(safeString("Level"), juce::dontSendNotification);
    noiseLevelLabel.setJustificationType(juce::Justification::centred);
    noiseLevelLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    noiseLevelLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // Noise EQ: Low Shelf/Cut
    lowShelfAmountSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    lowShelfAmountSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    lowShelfAmountSlider.setTextValueSuffix(" %");
    lowShelfAmountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "lowShelfAmount", lowShelfAmountSlider);
    lowShelfAmountLabel.setText(safeString("Low Shelf/Cut"), juce::dontSendNotification);
    lowShelfAmountLabel.setJustificationType(juce::Justification::centred);
    lowShelfAmountLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    lowShelfAmountLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // Noise EQ: High Shelf/Cut
    highShelfAmountSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    highShelfAmountSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    highShelfAmountSlider.setTextValueSuffix(" %");
    highShelfAmountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "highShelfAmount", highShelfAmountSlider);
    highShelfAmountLabel.setText(safeString("High Shelf/Cut"), juce::dontSendNotification);
    highShelfAmountLabel.setJustificationType(juce::Justification::centred);
    highShelfAmountLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    highShelfAmountLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    //==============================================================================
    // -- Filter Section Setup --
    
    // Item IDs are 1-based; the attachment maps them onto the parameter's choice
    // indices in order, so Notch/Peak must stay appended after High Pass.
    // From NonlinearSVF, so the menu and the filter cannot fall out of step. Item
    // ids run 1..N against choice indices 0..N-1, as everywhere else here.
    for (int i = 0; i < NonlinearSVF::numModes; ++i)
        filterModeCombo.addItem(safeString(NonlinearSVF::modeNames()[i]), i + 1);
    filterModeCombo.setSelectedId(1);
    filterModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "filterMode", filterModeCombo);
    filterModeLabel.setText(safeString("Mode"), juce::dontSendNotification);
    filterModeLabel.setJustificationType(juce::Justification::centred);
    filterModeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    filterModeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    filterCutoffSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    filterCutoffSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
    filterCutoffSlider.setTextValueSuffix(" Hz");
    filterCutoffSlider.activeGrid = [this] { return activeNoteLockGrid(0); };
    filterCutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "filterCutoff", filterCutoffSlider);
    filterCutoffLabel.setText(safeString("Cutoff"), juce::dontSendNotification);
    filterCutoffLabel.setJustificationType(juce::Justification::centred);
    filterCutoffLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    filterCutoffLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    filterResonanceSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    filterResonanceSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
    filterResonanceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "filterResonance", filterResonanceSlider);
    filterResonanceLabel.setText(safeString("Resonance"), juce::dontSendNotification);
    filterResonanceLabel.setJustificationType(juce::Justification::centred);
    filterResonanceLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    filterResonanceLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    warmSaturationMasterButton.setButtonText(safeString("Warm Saturation"));
    warmSaturationMasterAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "warmSaturationMaster", warmSaturationMasterButton);

    filterKeyTrackButton.setButtonText(safeString("Key Tracking"));
    filterKeyTrackButton.setTooltip(safeString("Filter cutoff follows the played key (neutral at middle C)"));
    filterKeyTrackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "filterKeyTrack", filterKeyTrackButton);

    // Shared by all three of each toggle (master + both mod filters), which are set up
    // far apart in this constructor -- one string each so the wording cannot drift.
    const juce::String noteLockTip (safeString(
        "Note Lock: the Cutoff knob clicks into semitone steps measured from the note you play "
        "-- a half step above it, a whole step, and so on all the way up and down. "
        "The filter lands on the note's harmonics instead of somewhere between them."));

    const juce::String harmonicTip (safeString(
        "Harmonic Series: locks the Cutoff to the played note's real overtones "
        "-- 2x its frequency, 3x, 4x and so on up, and 1/2, 1/3 and so on down -- "
        "instead of to even half steps. These are the partials the oscillators actually "
        "produce, so a resonant peak parked on one rings. They are not the same as the "
        "semitone grid: the 7th overtone sits 31 cents below the note you would expect."));

    filterNoteLockButton.setButtonText(safeString("Note Lock"));
    filterNoteLockButton.setTooltip(noteLockTip);
    filterNoteLockAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "filterNoteLock", filterNoteLockButton);
    // Pull the cutoff onto the grid the moment it is switched on, so engaging Note
    // Lock locks the sound you are already hearing. The attachment has written the
    // param by the time onClick fires, so activeNoteLockGrid() already sees the new state.
    filterNoteLockButton.onClick = [this] { snapCutoffToNoteLock(0); };

    // Button text is "Harmonics", not "Harmonic Series": the toggles are 84-86px wide
    // and the LookAndFeel draws their text on one unwrapped line at 12pt bold, so the
    // longer name clips. The tooltip carries the full name.
    filterHarmonicLockButton.setButtonText(safeString("Harmonics"));
    filterHarmonicLockButton.setTooltip(harmonicTip);
    filterHarmonicLockAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "filterHarmonicLock", filterHarmonicLockButton);
    // Switching grid re-snaps too, so the cutoff jumps to the nearest partial rather
    // than sitting between two of them until the knob is next touched.
    filterHarmonicLockButton.onClick = [this] { snapCutoffToNoteLock(0); };

    // Filter Envelope
    filterEnvAttackSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    filterEnvAttackSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    filterEnvAttackSlider.setTextValueSuffix(" s");
    filterEnvAttackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "filterEnvAttack", filterEnvAttackSlider);
    filterEnvAttackLabel.setText(safeString("Attack"), juce::dontSendNotification);
    filterEnvAttackLabel.setJustificationType(juce::Justification::centred);
    filterEnvAttackLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    filterEnvAttackLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    filterEnvDecaySlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    filterEnvDecaySlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    filterEnvDecaySlider.setTextValueSuffix(" s");
    filterEnvDecayAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "filterEnvDecay", filterEnvDecaySlider);
    filterEnvDecayLabel.setText(safeString("Decay"), juce::dontSendNotification);
    filterEnvDecayLabel.setJustificationType(juce::Justification::centred);
    filterEnvDecayLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    filterEnvDecayLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    filterEnvSustainSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    filterEnvSustainSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    filterEnvSustainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "filterEnvSustain", filterEnvSustainSlider);
    filterEnvSustainLabel.setText(safeString("Sustain"), juce::dontSendNotification);
    filterEnvSustainLabel.setJustificationType(juce::Justification::centred);
    filterEnvSustainLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    filterEnvSustainLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    filterEnvReleaseSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    filterEnvReleaseSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    filterEnvReleaseSlider.setTextValueSuffix(" s");
    filterEnvReleaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "filterEnvRelease", filterEnvReleaseSlider);
    filterEnvReleaseLabel.setText(safeString("Release"), juce::dontSendNotification);
    filterEnvReleaseLabel.setJustificationType(juce::Justification::centred);
    filterEnvReleaseLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    filterEnvReleaseLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    filterEnvAmountSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    filterEnvAmountSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    filterEnvAmountSlider.setTextValueSuffix(" %");
    filterEnvAmountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "filterEnvAmount", filterEnvAmountSlider);
    filterEnvAmountLabel.setText(safeString("Amount"), juce::dontSendNotification);
    filterEnvAmountLabel.setJustificationType(juce::Justification::centred);
    filterEnvAmountLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    filterEnvAmountLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    //==============================================================================
    // -- Envelope Section Setup --
    
    envAttackSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    envAttackSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    envAttackSlider.setTextValueSuffix(" s");
    envAttackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "envAttack", envAttackSlider);
    envAttackLabel.setText(safeString("Attack"), juce::dontSendNotification);
    envAttackLabel.setJustificationType(juce::Justification::centred);
    envAttackLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    envAttackLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    envDecaySlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    envDecaySlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    envDecaySlider.setTextValueSuffix(" s");
    envDecayAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "envDecay", envDecaySlider);
    envDecayLabel.setText(safeString("Decay"), juce::dontSendNotification);
    envDecayLabel.setJustificationType(juce::Justification::centred);
    envDecayLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    envDecayLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    envSustainSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    envSustainSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    envSustainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "envSustain", envSustainSlider);
    envSustainLabel.setText(safeString("Sustain"), juce::dontSendNotification);
    envSustainLabel.setJustificationType(juce::Justification::centred);
    envSustainLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    envSustainLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    envReleaseSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    envReleaseSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    envReleaseSlider.setTextValueSuffix(" s");
    envReleaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "envRelease", envReleaseSlider);
    envReleaseLabel.setText(safeString("Release"), juce::dontSendNotification);
    envReleaseLabel.setJustificationType(juce::Justification::centred);
    envReleaseLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    envReleaseLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // Pitch envelope (Amount: -100% to 100%, 12 o'clock = 0)
    pitchEnvAmountSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchEnvAmountSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    pitchEnvAmountSlider.setNumDecimalPlacesToDisplay(0);
    pitchEnvAmountSlider.setTextValueSuffix(" %");
    pitchEnvAmountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "pitchEnvAmount", pitchEnvAmountSlider);
    pitchEnvAmountLabel.setText(safeString("Amount"), juce::dontSendNotification);
    pitchEnvAmountLabel.setJustificationType(juce::Justification::centred);
    pitchEnvAmountLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    pitchEnvAmountLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // Pitch envelope (Time: 0-5 s)
    pitchEnvTimeSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchEnvTimeSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    pitchEnvTimeSlider.setTextValueSuffix(" s");
    pitchEnvTimeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "pitchEnvTime", pitchEnvTimeSlider);
    pitchEnvTimeLabel.setText(safeString("Time"), juce::dontSendNotification);
    pitchEnvTimeLabel.setJustificationType(juce::Justification::centred);
    pitchEnvTimeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    pitchEnvTimeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // Pitch envelope (Pitch: 0-24 st)
    pitchEnvPitchSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchEnvPitchSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    pitchEnvPitchSlider.setNumDecimalPlacesToDisplay(1);
    pitchEnvPitchSlider.setTextValueSuffix(" st");
    pitchEnvPitchAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "pitchEnvPitch", pitchEnvPitchSlider);
    pitchEnvPitchLabel.setText(safeString("Pitch"), juce::dontSendNotification);
    pitchEnvPitchLabel.setJustificationType(juce::Justification::centred);
    pitchEnvPitchLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    pitchEnvPitchLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    // Explicit LookAndFeel for labels that don't inherit correctly (fixes font inconsistency)
    osc1CoarseTuneLabel.setLookAndFeel(&customLookAndFeel);
    osc2CoarseTuneLabel.setLookAndFeel(&customLookAndFeel);
    filterCutoffLabel.setLookAndFeel(&customLookAndFeel);
    filterResonanceLabel.setLookAndFeel(&customLookAndFeel);
    envSustainLabel.setLookAndFeel(&customLookAndFeel);
    envReleaseLabel.setLookAndFeel(&customLookAndFeel);
    filterEnvReleaseLabel.setLookAndFeel(&customLookAndFeel);
    pitchEnvPitchLabel.setLookAndFeel(&customLookAndFeel);
    pitchBendLabel.setLookAndFeel(&customLookAndFeel);

    // Sub oscillator (expandable when toggle is on)
    subOscToggleButton.setButtonText(safeString("Sub Oscillator"));
    subOscToggleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "subOscOn", subOscToggleButton);
    // Items come from rebuildWaveformMenus below, like the other waveform menus:
    // it is the one place that knows which User slots hold anything.
    for (int i = 0; i < OscShape::numShapes; ++i)
        subOscWaveformCombo.addItem(safeString(OscShape::names[i]), i + 1);
    subOscWaveformCombo.setLookAndFeel(&customLookAndFeel);
    if (auto* subOscWaveformParam = audioProcessor.getValueTreeState().getParameter("subOscWaveform"))
        subOscWaveformAttachment = std::make_unique<WaveformChoiceAttachment>(
            *subOscWaveformParam, subOscWaveformCombo);
    subOscLevelSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    subOscLevelSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    subOscLevelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "subOscLevel", subOscLevelSlider);
    subOscLevelLabel.setText(safeString("Level"), juce::dontSendNotification);
    subOscLevelLabel.setJustificationType(juce::Justification::centred);
    subOscLevelLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    subOscCoarseSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    subOscCoarseSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    subOscCoarseSlider.setNumDecimalPlacesToDisplay(0);
    subOscCoarseSlider.setTextValueSuffix(" st");
    subOscCoarseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "subOscCoarse", subOscCoarseSlider);
    subOscWaveformLabel.setText(safeString("Wave"), juce::dontSendNotification);
    subOscWaveformLabel.setJustificationType(juce::Justification::centred);
    subOscWaveformLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    subOscCoarseLabel.setText(safeString("Coarse"), juce::dontSendNotification);
    subOscCoarseLabel.setJustificationType(juce::Justification::centred);
    subOscCoarseLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    subOscCoarseLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    //==============================================================================
    // -- Master Section Setup --
    
    masterVolumeSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    masterVolumeSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
    masterVolumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "masterVolume", masterVolumeSlider);
    masterVolumeLabel.setText(safeString("Volume"), juce::dontSendNotification);
    masterVolumeLabel.setJustificationType(juce::Justification::centred);
    masterVolumeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    masterVolumeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    // Easter egg: 7 clicks on master knob in 5 seconds launches Cheeze Guy
    masterVolumeSlider.onClicked = [this]()
    {
        auto now = (juce::int64)juce::Time::getMillisecondCounter();

        if (masterKnobClickCount < 7)
            masterKnobClickTimes[masterKnobClickCount++] = now;
        else
        {
            for (int i = 0; i < 6; i++)
                masterKnobClickTimes[i] = masterKnobClickTimes[i + 1];
            masterKnobClickTimes[6] = now;
        }

        if (masterKnobClickCount >= 7 &&
            (masterKnobClickTimes[6] - masterKnobClickTimes[0]) <= 5000)
        {
            masterKnobClickCount = 0;

            // Add game tab if not already added
            if (!cheezeGuyTabAdded)
            {
                cheezeGuyGame = std::make_unique<CheezeGuyGameComponent>();
                tabbedComponent.addTab(safeString("Cheeze Guy"),
                    juce::Colour(0xff0a0a1f), cheezeGuyGame.get(), false);
                cheezeGuyTabAdded = true;
                audioProcessor.cheezeGuyActivated = true;
            }
            else
            {
                cheezeGuyGame->resetGame();
            }

            // Switch to the game tab
            tabbedComponent.setCurrentTabIndex(tabbedComponent.getNumTabs() - 1);

            // Grab keyboard focus for arrow key controls. Guard with a SafePointer:
            // callAfterDelay can't be cancelled, so if the editor dies within 100ms
            // this would fire on a freed `this` (heap-use-after-free).
            juce::Component::SafePointer<SpaceDustAudioProcessorEditor> safeThis(this);
            juce::Timer::callAfterDelay(100, [this, safeThis]()
            {
                if (safeThis == nullptr)
                    return;
                if (cheezeGuyGame != nullptr)
                    cheezeGuyGame->grabKeyboardFocus();
            });
        }
    };

    // Pitch bend amount (1-24 semitones)
    pitchBendAmountSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchBendAmountSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 18);
    pitchBendAmountSlider.setNumDecimalPlacesToDisplay(0);
    pitchBendAmountSlider.setTextValueSuffix(" st");
    pitchBendAmountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "pitchBendAmount", pitchBendAmountSlider);
    pitchBendAmountLabel.setText(safeString("Bend Range"), juce::dontSendNotification);
    pitchBendAmountLabel.setJustificationType(juce::Justification::centred);
    pitchBendAmountLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    pitchBendAmountLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    velocityAmountSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    velocityAmountSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 18);
    velocityAmountSlider.setTextValueSuffix(" %");
    velocityAmountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "velocityAmount", velocityAmountSlider);
    velocityAmountLabel.setText(safeString("Velocity"), juce::dontSendNotification);
    velocityAmountLabel.setJustificationType(juce::Justification::centred);
    velocityAmountLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    velocityAmountLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    const juce::String velocityTip = safeString(
        "How much how hard you play sets the note. At 0 every note is the same, "
        "whatever the keyboard sends. Turn it up and softer notes come out quieter "
        "and darker; a note at full velocity sounds the same at any setting.");
    velocityAmountSlider.setTooltip(velocityTip);
    velocityAmountLabel.setTooltip(velocityTip);
    
    // Pitch bend vertical fader (bipolar -1 to 1)
    pitchBendSlider.setSliderStyle(juce::Slider::LinearVertical);
    pitchBendSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 18);
    pitchBendAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "pitchBend", pitchBendSlider);
    pitchBendSlider.addListener(this);  // Snap back to center on mouse release
    pitchBendLabel.setText(safeString("Pitch"), juce::dontSendNotification);
    pitchBendLabel.setJustificationType(juce::Justification::centred);
    pitchBendLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    pitchBendLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // Force LookAndFeel on labels that may not inherit (ensures consistent font rendering)
    osc1CoarseTuneLabel.setLookAndFeel(&customLookAndFeel);
    osc2CoarseTuneLabel.setLookAndFeel(&customLookAndFeel);
    filterCutoffLabel.setLookAndFeel(&customLookAndFeel);
    filterResonanceLabel.setLookAndFeel(&customLookAndFeel);
    envSustainLabel.setLookAndFeel(&customLookAndFeel);
    envReleaseLabel.setLookAndFeel(&customLookAndFeel);
    filterEnvReleaseLabel.setLookAndFeel(&customLookAndFeel);
    pitchEnvPitchLabel.setLookAndFeel(&customLookAndFeel);
    pitchBendLabel.setLookAndFeel(&customLookAndFeel);
    subOscCoarseLabel.setLookAndFeel(&customLookAndFeel);
    lfo1PhaseLabel.setLookAndFeel(&customLookAndFeel);
    lfo2PhaseLabel.setLookAndFeel(&customLookAndFeel);
    lfo1TargetLabel.setLookAndFeel(&customLookAndFeel);
    lfo2TargetLabel.setLookAndFeel(&customLookAndFeel);
    grainDelayDensityLabel.setLookAndFeel(&customLookAndFeel);
    phaserStagesLabel.setLookAndFeel(&customLookAndFeel);
    compressorThresholdLabel.setLookAndFeel(&customLookAndFeel);
    modFilter1ResonanceLabel.setLookAndFeel(&customLookAndFeel);
    modFilter2ResonanceLabel.setLookAndFeel(&customLookAndFeel);
    compressorReleaseLabel.setLookAndFeel(&customLookAndFeel);
    
    // Voice Mode (moved to Master section)
    voiceModeCombo.addItem(safeString("Poly"), 1);
    voiceModeCombo.addItem(safeString("Mono"), 2);
    voiceModeCombo.addItem(safeString("Legato"), 3);
    voiceModeCombo.setSelectedId(1);
    voiceModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "voiceMode", voiceModeCombo);
    voiceModeLabel.setText(safeString("Mode"), juce::dontSendNotification);
    voiceModeLabel.setJustificationType(juce::Justification::centred);
    voiceModeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    voiceModeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    glideTimeSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    glideTimeSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
    glideTimeSlider.setTextValueSuffix(" s");
    glideTimeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "glideTime", glideTimeSlider);
    glideTimeLabel.setText(safeString("Glide"), juce::dontSendNotification);
    glideTimeLabel.setJustificationType(juce::Justification::centred);
    glideTimeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    glideTimeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // Legato Glide toggle (Fingered Glide)
    legatoGlideButton.setButtonText(safeString("Legato Glide"));
    legatoGlideAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "legatoGlide", legatoGlideButton);
    // Label is hidden - button text shows "Legato Glide"
    legatoGlideLabel.setText(safeString(""), juce::dontSendNotification);
    legatoGlideLabel.setJustificationType(juce::Justification::centred);
    legatoGlideLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    legatoGlideLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    //==============================================================================
    // -- MPE Controls (shown on Modulation tab) --

    // Tooltip applied to every MPE child â€” JUCE's TooltipWindow only consults the
    // component directly under the cursor, so the group-level tooltip alone is
    // unreachable while hovering knobs/labels/combo.
    const juce::String mpeHostTip ("NOTE: Only useable with an MPE compatible DAW or an MPE emulator");
    mpeGroup.setTooltip(mpeHostTip);
    mpeModeLabel.setTooltip(mpeHostTip);
    mpeModeCombo.setTooltip(mpeHostTip);
    mpePitchBendRangeLabel.setTooltip(mpeHostTip);
    mpePitchBendRangeSlider.setTooltip(mpeHostTip);
    mpePressureDepthLabel.setTooltip(mpeHostTip);
    mpePressureDepthSlider.setTooltip(mpeHostTip);
    mpeTimbreDepthLabel.setTooltip(mpeHostTip);
    mpeTimbreDepthSlider.setTooltip(mpeHostTip);

    mpeModeCombo.addItem(safeString("Legacy"), 1);
    mpeModeCombo.addItem(safeString("Lower Zone"), 2);
    mpeModeCombo.setSelectedId(1);
    mpeModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "mpeMode", mpeModeCombo);
    mpeModeLabel.setText(safeString("MPE Mode"), juce::dontSendNotification);
    mpeModeLabel.setJustificationType(juce::Justification::centred);
    mpeModeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    mpeModeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    mpePitchBendRangeSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    mpePitchBendRangeSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 18);
    mpePitchBendRangeSlider.setTextValueSuffix(" st");
    mpePitchBendRangeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "mpePitchBendRange", mpePitchBendRangeSlider);
    mpePitchBendRangeLabel.setText(safeString("Bend Range"), juce::dontSendNotification);
    mpePitchBendRangeLabel.setJustificationType(juce::Justification::centred);
    mpePitchBendRangeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    mpePitchBendRangeLabel.setFont(customLookAndFeel.getBodyFont(11.0f, true));

    mpePressureDepthSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    mpePressureDepthSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 18);
    mpePressureDepthSlider.setTextValueSuffix(" %");
    mpePressureDepthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "mpePressureDepth", mpePressureDepthSlider);
    mpePressureDepthLabel.setText(safeString("Pressure"), juce::dontSendNotification);
    mpePressureDepthLabel.setJustificationType(juce::Justification::centred);
    mpePressureDepthLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    mpePressureDepthLabel.setFont(customLookAndFeel.getBodyFont(11.0f, true));

    mpeTimbreDepthSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    mpeTimbreDepthSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 18);
    mpeTimbreDepthSlider.setTextValueSuffix(" %");
    mpeTimbreDepthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "mpeTimbreDepth", mpeTimbreDepthSlider);
    mpeTimbreDepthLabel.setText(safeString("Timbre"), juce::dontSendNotification);
    mpeTimbreDepthLabel.setJustificationType(juce::Justification::centred);
    mpeTimbreDepthLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    mpeTimbreDepthLabel.setFont(customLookAndFeel.getBodyFont(11.0f, true));

    // Stereo Level Meters: Create at bottom of Master box
    stereoLevelMeter = std::make_unique<StereoLevelMeterComponent>(audioProcessor);
    stereoLevelMeter->setAccessible(false);

    //==============================================================================
    // -- Modulation Section Setup --
    
    // Modulation title label (large, centered, cosmic style)
    modulationTitleLabel.setText(safeString("Modulation"), juce::dontSendNotification);
    modulationTitleLabel.setJustificationType(juce::Justification::centred);
    modulationTitleLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light cyan
    modulationTitleLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // LFO1 Sub-group
    // LFO1 Waveform
    lfo1WaveformCombo.addItem(safeString("Sine"), 1);
    lfo1WaveformCombo.addItem(safeString("Triangle"), 2);
    lfo1WaveformCombo.addItem(safeString("Saw Up"), 3);
    lfo1WaveformCombo.addItem(safeString("Saw Down"), 4);
    lfo1WaveformCombo.addItem(safeString("Square"), 5);
    lfo1WaveformCombo.addItem(safeString("S&H"), 6);
    lfo1WaveformCombo.setSelectedId(1);
    lfo1WaveformAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "lfo1Waveform", lfo1WaveformCombo);
    lfo1WaveformLabel.setText(safeString("Waveform"), juce::dontSendNotification);
    lfo1WaveformLabel.setJustificationType(juce::Justification::centred);
    lfo1WaveformLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo1WaveformLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // LFO1 On toggle
    lfo1EnabledButton.setButtonText(safeString("On"));
    lfo1EnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "lfo1Enabled", lfo1EnabledButton);
    
    // LFO1 Destination (Target)
    lfo1TargetCombo.addItem(safeString("Pitch"), 1);
    lfo1TargetCombo.addItem(safeString("Filter"), 2);
    lfo1TargetCombo.addItem(safeString("Master Vol"), 3);
    lfo1TargetCombo.addItem(safeString("Osc1 Vol"), 4);
    lfo1TargetCombo.addItem(safeString("Osc2 Vol"), 5);
    lfo1TargetCombo.addItem(safeString("Noise Vol"), 6);
    lfo1TargetCombo.setSelectedId(2);  // Default to Filter
    lfo1TargetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "lfo1Target", lfo1TargetCombo);
    lfo1TargetLabel.setText(safeString("Destination"), juce::dontSendNotification);
    lfo1TargetLabel.setJustificationType(juce::Justification::centred);
    lfo1TargetLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo1TargetLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // LFO1 Sync button (glows when checked)
    lfo1SyncButton.setButtonText(safeString("Sync"));
    lfo1SyncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "lfo1Sync", lfo1SyncButton);
    lfo1SyncLabel.setText(safeString("Sync"), juce::dontSendNotification);
    lfo1SyncLabel.setJustificationType(juce::Justification::centred);
    lfo1SyncLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo1SyncLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // LFO1 Triplet button
    lfo1TripletButton.setButtonText(safeString("Triplet"));
    lfo1TripletAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "lfo1TripletEnabled", lfo1TripletButton);
    lfo1TripletButton.setVisible(false);
    
    // LFO1 All toggle button
    lfo1TripletStraightButton.setButtonText(safeString("All"));
    lfo1TripletStraightAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "lfo1TripletStraightToggle", lfo1TripletStraightButton);
    lfo1TripletStraightButton.setVisible(false);
    
    // LFO1 Rate: NoTextBox - only lfo1RateValueLabel shows Hz/sync; no raw value display
    lfo1FreeRateSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    lfo1FreeRateSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    lfo1FreeRateSlider.setRange(0.0, 12.0, 0.01);  // Maps to 0.01-200 Hz logarithmically (free mode)
    lfo1FreeRateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "lfo1Rate", lfo1FreeRateSlider);
    lfo1RateLabel.setText(safeString("Rate"), juce::dontSendNotification);
    lfo1RateLabel.setJustificationType(juce::Justification::centred);
    lfo1RateLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo1RateLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    lfo1RateValueLabel.setText(safeString("1.00 Hz"), juce::dontSendNotification);
    lfo1RateValueLabel.setJustificationType(juce::Justification::centred);
    lfo1RateValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo1RateValueLabel.setFont(customLookAndFeel.getBodyFont(12.0f, false));
    
    // LFO1 Rate: Sync rate combo (shown when sync on)
    lfo1SyncRateCombo.addItem(safeString("1/32"), 1);
    lfo1SyncRateCombo.addItem(safeString("1/16"), 2);
    lfo1SyncRateCombo.addItem(safeString("1/8"), 3);
    lfo1SyncRateCombo.addItem(safeString("1/4"), 4);
    lfo1SyncRateCombo.addItem(safeString("1/2"), 5);
    lfo1SyncRateCombo.addItem(safeString("1/1"), 6);
    lfo1SyncRateCombo.addItem(safeString("2/1"), 7);
    lfo1SyncRateCombo.addItem(safeString("4/1"), 8);
    lfo1SyncRateCombo.addItem(safeString("8/1"), 9);
    lfo1SyncRateCombo.addItem(safeString("16/1"), 10);
    lfo1SyncRateCombo.addItem(safeString("32/1"), 11);
    lfo1SyncRateCombo.addItem(safeString("64/1"), 12);
    lfo1SyncRateCombo.addItem(safeString("128/1"), 13);
    lfo1SyncRateListener = std::make_unique<SyncRateComboListener>(&lfo1FreeRateSlider);
    lfo1SyncRateCombo.addListener(lfo1SyncRateListener.get());
    // Sync combo from parameter (restores saved value when editor reopens)
    lfo1SyncRateCombo.setSelectedId(juce::jlimit(1, 13, static_cast<int>(std::round(lfo1FreeRateSlider.getValue())) + 1),
                                    juce::dontSendNotification);
    // Always show the knob, hide the combo box
    lfo1FreeRateSlider.setVisible(true);
    lfo1SyncRateCombo.setVisible(false);
    lfo1RateValueLabel.setVisible(true);  // Shows Hz or sync division
    
    lfo1DepthSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    lfo1DepthSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    lfo1DepthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "lfo1Depth", lfo1DepthSlider);
    lfo1DepthLabel.setText(safeString("Depth"), juce::dontSendNotification);
    lfo1DepthLabel.setJustificationType(juce::Justification::centred);
    lfo1DepthLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo1DepthLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    lfo1PhaseSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    lfo1PhaseSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    lfo1PhaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "lfo1Phase", lfo1PhaseSlider);
    lfo1PhaseLabel.setText(safeString("Phase"), juce::dontSendNotification);
    lfo1PhaseLabel.setJustificationType(juce::Justification::centred);
    lfo1PhaseLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo1PhaseLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // LFO1 Retrigger button
    lfo1RetriggerButton.setButtonText(safeString("Retrigger"));
    lfo1RetriggerButton.setToggleState(true, juce::dontSendNotification);
    lfo1RetriggerAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "lfo1Retrigger", lfo1RetriggerButton);
    
    // LFO2 Sub-group
    lfo2WaveformCombo.addItem(safeString("Sine"), 1);
    lfo2WaveformCombo.addItem(safeString("Triangle"), 2);
    lfo2WaveformCombo.addItem(safeString("Saw Up"), 3);
    lfo2WaveformCombo.addItem(safeString("Saw Down"), 4);
    lfo2WaveformCombo.addItem(safeString("Square"), 5);
    lfo2WaveformCombo.addItem(safeString("S&H"), 6);
    lfo2WaveformCombo.setSelectedId(1);
    lfo2WaveformAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "lfo2Waveform", lfo2WaveformCombo);
    lfo2WaveformLabel.setText(safeString("Waveform"), juce::dontSendNotification);
    lfo2WaveformLabel.setJustificationType(juce::Justification::centred);
    lfo2WaveformLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo2WaveformLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // LFO2 On toggle
    lfo2EnabledButton.setButtonText(safeString("On"));
    lfo2EnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "lfo2Enabled", lfo2EnabledButton);
    
    // LFO2 Destination (Target)
    lfo2TargetCombo.addItem(safeString("Pitch"), 1);
    lfo2TargetCombo.addItem(safeString("Filter"), 2);
    lfo2TargetCombo.addItem(safeString("Master Vol"), 3);
    lfo2TargetCombo.addItem(safeString("Osc1 Vol"), 4);
    lfo2TargetCombo.addItem(safeString("Osc2 Vol"), 5);
    lfo2TargetCombo.addItem(safeString("Noise Vol"), 6);
    lfo2TargetCombo.setSelectedId(1);  // Default to Pitch
    lfo2TargetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "lfo2Target", lfo2TargetCombo);
    lfo2TargetLabel.setText(safeString("Destination"), juce::dontSendNotification);
    lfo2TargetLabel.setJustificationType(juce::Justification::centred);
    lfo2TargetLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo2TargetLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    lfo2SyncButton.setButtonText(safeString("Sync"));
    lfo2SyncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "lfo2Sync", lfo2SyncButton);
    lfo2SyncLabel.setText(safeString("Sync"), juce::dontSendNotification);
    lfo2SyncLabel.setJustificationType(juce::Justification::centred);
    lfo2SyncLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo2SyncLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // LFO2 Triplet button
    lfo2TripletButton.setButtonText(safeString("Triplet"));
    lfo2TripletAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "lfo2TripletEnabled", lfo2TripletButton);
    lfo2TripletButton.setVisible(false);
    
    // LFO2 All toggle button
    lfo2TripletStraightButton.setButtonText(safeString("All"));
    lfo2TripletStraightAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "lfo2TripletStraightToggle", lfo2TripletStraightButton);
    lfo2TripletStraightButton.setVisible(false);
    
    lfo2FreeRateSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    lfo2FreeRateSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);  // Only Hz/sync label below
    lfo2FreeRateSlider.setRange(0.0, 12.0, 0.01);
    lfo2FreeRateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "lfo2Rate", lfo2FreeRateSlider);
    lfo2RateLabel.setText(safeString("Rate"), juce::dontSendNotification);
    lfo2RateLabel.setJustificationType(juce::Justification::centred);
    lfo2RateLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo2RateLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    lfo2RateValueLabel.setText(safeString("1.00 Hz"), juce::dontSendNotification);
    lfo2RateValueLabel.setJustificationType(juce::Justification::centred);
    lfo2RateValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo2RateValueLabel.setFont(customLookAndFeel.getBodyFont(12.0f, false));
    
    lfo2SyncRateCombo.addItem(safeString("1/32"), 1);
    lfo2SyncRateCombo.addItem(safeString("1/16"), 2);
    lfo2SyncRateCombo.addItem(safeString("1/8"), 3);
    lfo2SyncRateCombo.addItem(safeString("1/4"), 4);
    lfo2SyncRateCombo.addItem(safeString("1/2"), 5);
    lfo2SyncRateCombo.addItem(safeString("1/1"), 6);
    lfo2SyncRateCombo.addItem(safeString("2/1"), 7);
    lfo2SyncRateCombo.addItem(safeString("4/1"), 8);
    lfo2SyncRateCombo.addItem(safeString("8/1"), 9);
    lfo2SyncRateCombo.addItem(safeString("16/1"), 10);
    lfo2SyncRateCombo.addItem(safeString("32/1"), 11);
    lfo2SyncRateCombo.addItem(safeString("64/1"), 12);
    lfo2SyncRateCombo.addItem(safeString("128/1"), 13);
    lfo2SyncRateListener = std::make_unique<SyncRateComboListener>(&lfo2FreeRateSlider);
    lfo2SyncRateCombo.addListener(lfo2SyncRateListener.get());
    // Sync combo from parameter (restores saved value when editor reopens)
    lfo2SyncRateCombo.setSelectedId(juce::jlimit(1, 13, static_cast<int>(std::round(lfo2FreeRateSlider.getValue())) + 1),
                                    juce::dontSendNotification);
    // Always show the knob, hide the combo box
    lfo2FreeRateSlider.setVisible(true);
    lfo2SyncRateCombo.setVisible(false);
    lfo2RateValueLabel.setVisible(true);  // Shows Hz or sync division
    
    lfo2DepthSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    lfo2DepthSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    lfo2DepthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "lfo2Depth", lfo2DepthSlider);
    lfo2DepthLabel.setText(safeString("Depth"), juce::dontSendNotification);
    lfo2DepthLabel.setJustificationType(juce::Justification::centred);
    lfo2DepthLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo2DepthLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    lfo2PhaseSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    lfo2PhaseSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    lfo2PhaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "lfo2Phase", lfo2PhaseSlider);
    lfo2PhaseLabel.setText(safeString("Phase"), juce::dontSendNotification);
    lfo2PhaseLabel.setJustificationType(juce::Justification::centred);
    lfo2PhaseLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lfo2PhaseLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    // Mod Filter toggles (each LFO has its own - filter controls only appear when Filter is toggled)
    modFilterShowButton.setButtonText(safeString("Filter"));
    modFilterShowAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "modFilter1Show", modFilterShowButton);
    modFilterShowButton2.setButtonText(safeString("Filter"));
    modFilterShowAttachment2 = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "modFilter2Show", modFilterShowButton2);
    modFilterShowLabel.setText(safeString("Show Filters"), juce::dontSendNotification);
    modFilterShowLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    modFilterShowLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    modFilter1LinkButton.setButtonText(safeString("Link to Master"));
    modFilter1LinkAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "modFilter1LinkToMaster", modFilter1LinkButton);
    for (int i = 0; i < NonlinearSVF::numModes; ++i)
        modFilter1ModeCombo.addItem(safeString(NonlinearSVF::modeNames()[i]), i + 1);
    modFilter1ModeCombo.setSelectedId(1);
    modFilter1CutoffSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    modFilter1CutoffSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    modFilter1CutoffSlider.setTextValueSuffix(" Hz");
    modFilter1CutoffSlider.activeGrid = [this] { return activeNoteLockGrid(1); };
    modFilter1ResonanceSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    modFilter1ResonanceSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    warmSaturationMod1Button.setButtonText(safeString("Warm Saturation"));
    modFilter1KeyTrackButton.setButtonText(safeString("Key Tracking"));
    modFilter1KeyTrackButton.setTooltip(safeString("Filter cutoff follows the played key (neutral at middle C)"));
    modFilter1NoteLockButton.setButtonText(safeString("Note Lock"));
    modFilter1NoteLockButton.setTooltip(noteLockTip);
    modFilter1NoteLockButton.onClick = [this] { snapCutoffToNoteLock(1); };
    modFilter1HarmonicLockButton.setButtonText(safeString("Harmonics"));
    modFilter1HarmonicLockButton.setTooltip(harmonicTip);
    modFilter1HarmonicLockButton.onClick = [this] { snapCutoffToNoteLock(1); };
    // Cutoff/Resonance/Mode/WarmSat/KeyTrack/NoteLock attachments are created in rebuildLinkedFilterAttachments()
    // (called below and on every link toggle) so they can point at master or own params.
    modFilter1ModeLabel.setText(safeString("Mode"), juce::dontSendNotification);
    modFilter1CutoffLabel.setText(safeString("Cutoff"), juce::dontSendNotification);
    modFilter1ResonanceLabel.setText(safeString("Resonance"), juce::dontSendNotification);
    modFilter1ModeLabel.setJustificationType(juce::Justification::centred);
    modFilter1ModeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    modFilter1ModeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    modFilter1CutoffLabel.setJustificationType(juce::Justification::centred);
    modFilter1CutoffLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    modFilter1CutoffLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    modFilter1ResonanceLabel.setJustificationType(juce::Justification::centred);
    modFilter1ResonanceLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    modFilter1ResonanceLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    modFilter2LinkButton.setButtonText(safeString("Link to Master"));
    modFilter2LinkAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "modFilter2LinkToMaster", modFilter2LinkButton);
    for (int i = 0; i < NonlinearSVF::numModes; ++i)
        modFilter2ModeCombo.addItem(safeString(NonlinearSVF::modeNames()[i]), i + 1);
    modFilter2ModeCombo.setSelectedId(1);
    modFilter2CutoffSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    modFilter2CutoffSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    modFilter2CutoffSlider.setTextValueSuffix(" Hz");
    modFilter2CutoffSlider.activeGrid = [this] { return activeNoteLockGrid(2); };
    modFilter2ResonanceSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    modFilter2ResonanceSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    warmSaturationMod2Button.setButtonText(safeString("Warm Saturation"));
    modFilter2KeyTrackButton.setButtonText(safeString("Key Tracking"));
    modFilter2KeyTrackButton.setTooltip(safeString("Filter cutoff follows the played key (neutral at middle C)"));
    modFilter2NoteLockButton.setButtonText(safeString("Note Lock"));
    modFilter2NoteLockButton.setTooltip(noteLockTip);
    modFilter2NoteLockButton.onClick = [this] { snapCutoffToNoteLock(2); };
    modFilter2HarmonicLockButton.setButtonText(safeString("Harmonics"));
    modFilter2HarmonicLockButton.setTooltip(harmonicTip);
    modFilter2HarmonicLockButton.onClick = [this] { snapCutoffToNoteLock(2); };
    modFilter2ModeLabel.setText(safeString("Mode"), juce::dontSendNotification);
    modFilter2CutoffLabel.setText(safeString("Cutoff"), juce::dontSendNotification);
    modFilter2ResonanceLabel.setText(safeString("Resonance"), juce::dontSendNotification);
    modFilter2ModeLabel.setJustificationType(juce::Justification::centred);
    modFilter2ModeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    modFilter2ModeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    modFilter2CutoffLabel.setJustificationType(juce::Justification::centred);
    modFilter2CutoffLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    modFilter2CutoffLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    modFilter2ResonanceLabel.setJustificationType(juce::Justification::centred);
    modFilter2ResonanceLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    modFilter2ResonanceLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    // Point the mod-filter knobs at the master params (if linked) or their own params (if not).
    // Re-run on every link toggle via syncLinkedFilterParams(). No onChange/setValueNotifyingHost
    // sync is needed any more: a linked filter literally shares the master's parameter, so one
    // automation lane drives both knobs and moving either edits the same param.
    rebuildLinkedFilterAttachments();
    // While linked, mod-tab Warm Saturation toggle must push to warmSaturationMaster.
    // buttonStateChanged() handles the push; without these listeners the mod toggle
    // updates only warmSaturationMod*, and the next master-side mirror reverts the visual.
    warmSaturationMod1Button.addListener(this);
    warmSaturationMod2Button.addListener(this);
    
    // LFO2 Retrigger button
    lfo2RetriggerButton.setButtonText(safeString("Retrigger"));
    lfo2RetriggerButton.setToggleState(true, juce::dontSendNotification);
    lfo2RetriggerAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "lfo2Retrigger", lfo2RetriggerButton);
    
    //==============================================================================
    // -- Delay Effect Setup (Effects tab) --
    delayEnabledButton.setButtonText(safeString("On"));
    delayEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "delayEnabled", delayEnabledButton);
    delayEnabledLabel.setText(safeString("On"), juce::dontSendNotification);
    delayEnabledLabel.setJustificationType(juce::Justification::centred);
    delayEnabledLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    delayEnabledLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    delaySyncButton.setButtonText(safeString("Sync"));
    delaySyncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "delaySync", delaySyncButton);
    delaySyncLabel.setText(safeString("Sync"), juce::dontSendNotification);
    delaySyncLabel.setJustificationType(juce::Justification::centred);
    delaySyncLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    delaySyncLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    delayTripletButton.setButtonText(safeString("Triplet"));
    delayTripletAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "delayTripletEnabled", delayTripletButton);
    delayTripletButton.setVisible(false);
    
    delayTripletStraightButton.setButtonText(safeString("All"));
    delayTripletStraightAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "delayTripletStraightToggle", delayTripletStraightButton);
    delayTripletStraightButton.setVisible(false);
    
    delayFreeRateSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    delayFreeRateSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    delayFreeRateSlider.setColour(juce::Slider::textBoxTextColourId, juce::Colours::transparentBlack);
    delayFreeRateSlider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    delayFreeRateSlider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    delayFreeRateSlider.setRange(0.0, 12.0, 0.01);
    delayFreeRateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "delayRate", delayFreeRateSlider);
    delayRateLabel.setText(safeString("Time"), juce::dontSendNotification);
    delayRateLabel.setJustificationType(juce::Justification::centred);
    delayRateLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    delayRateLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    delayRateValueLabel.setText(safeString("1/4"), juce::dontSendNotification);
    delayRateValueLabel.setJustificationType(juce::Justification::centred);
    delayRateValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    delayRateValueLabel.setFont(customLookAndFeel.getBodyFont(12.0f, false));
    
    delaySyncRateCombo.addItem(safeString("1/32"), 1);
    delaySyncRateCombo.addItem(safeString("1/16"), 2);
    delaySyncRateCombo.addItem(safeString("1/8"), 3);
    delaySyncRateCombo.addItem(safeString("1/4"), 4);
    delaySyncRateCombo.addItem(safeString("1/2"), 5);
    delaySyncRateCombo.addItem(safeString("1/1"), 6);
    delaySyncRateCombo.addItem(safeString("2/1"), 7);
    delaySyncRateCombo.addItem(safeString("4/1"), 8);
    delaySyncRateCombo.addItem(safeString("8/1"), 9);
    delaySyncRateCombo.addItem(safeString("16/1"), 10);
    delaySyncRateCombo.addItem(safeString("32/1"), 11);
    delaySyncRateCombo.addItem(safeString("64/1"), 12);
    delaySyncRateCombo.addItem(safeString("128/1"), 13);
    delaySyncRateListener = std::make_unique<SyncRateComboListener>(&delayFreeRateSlider);
    delaySyncRateCombo.addListener(delaySyncRateListener.get());
    delaySyncRateCombo.setSelectedId(juce::jlimit(1, 13, static_cast<int>(std::round(delayFreeRateSlider.getValue())) + 1),
                                      juce::dontSendNotification);
    delayFreeRateSlider.setVisible(true);
    delaySyncRateCombo.setVisible(false);
    delayRateValueLabel.setVisible(true);
    
    delayDecaySlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    delayDecaySlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    delayDecayAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "delayDecay", delayDecaySlider);
    delayDecayLabel.setText(safeString("Decay"), juce::dontSendNotification);
    delayDecayLabel.setJustificationType(juce::Justification::centred);
    delayDecayLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    delayDecayLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    delayDryWetSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    delayDryWetSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    delayDryWetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "delayDryWet", delayDryWetSlider);
    delayDryWetLabel.setText(safeString("Mix"), juce::dontSendNotification);
    delayDryWetLabel.setJustificationType(juce::Justification::centred);
    delayDryWetLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    delayDryWetLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    delayPingPongButton.setButtonText(safeString("Ping-Pong"));
    delayPingPongAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "delayPingPong", delayPingPongButton);
    delayPingPongLabel.setText(safeString("Ping-Pong"), juce::dontSendNotification);
    delayPingPongLabel.setJustificationType(juce::Justification::centred);
    delayPingPongLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    delayPingPongLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    delayFilterShowButton.setButtonText(safeString("Filter"));
    delayFilterShowAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "delayFilterShow", delayFilterShowButton);
    
    delayFilterHPCutoffSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    delayFilterHPCutoffSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    delayFilterHPCutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "delayFilterHPCutoff", delayFilterHPCutoffSlider);
    delayFilterHPCutoffLabel.setText(safeString("HP Cutoff"), juce::dontSendNotification);
    delayFilterHPCutoffLabel.setJustificationType(juce::Justification::centred);
    delayFilterHPCutoffLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    delayFilterHPCutoffLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    delayFilterHPResonanceSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    delayFilterHPResonanceSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    delayFilterHPResonanceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "delayFilterHPResonance", delayFilterHPResonanceSlider);
    delayFilterHPResonanceLabel.setText(safeString("HP Res"), juce::dontSendNotification);
    delayFilterHPResonanceLabel.setJustificationType(juce::Justification::centred);
    delayFilterHPResonanceLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    delayFilterHPResonanceLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    delayFilterLPCutoffSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    delayFilterLPCutoffSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    delayFilterLPCutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "delayFilterLPCutoff", delayFilterLPCutoffSlider);
    delayFilterLPCutoffLabel.setText(safeString("LP Cutoff"), juce::dontSendNotification);
    delayFilterLPCutoffLabel.setJustificationType(juce::Justification::centred);
    delayFilterLPCutoffLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    delayFilterLPCutoffLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    delayFilterLPResonanceSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    delayFilterLPResonanceSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    delayFilterLPResonanceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "delayFilterLPResonance", delayFilterLPResonanceSlider);
    delayFilterLPResonanceLabel.setText(safeString("LP Res"), juce::dontSendNotification);
    delayFilterLPResonanceLabel.setJustificationType(juce::Justification::centred);
    delayFilterLPResonanceLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    delayFilterLPResonanceLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    delayFilterWarmSaturationButton.setButtonText(safeString("Warm Saturation"));
    delayFilterWarmSaturationAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "delayFilterWarmSaturation", delayFilterWarmSaturationButton);
    
    //==============================================================================
    // -- Reverb Effect Setup (Effects tab) --
    reverbEnabledButton.setButtonText(safeString("On"));
    reverbEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "reverbEnabled", reverbEnabledButton);
    reverbEnabledLabel.setText(safeString("On"), juce::dontSendNotification);
    reverbEnabledLabel.setJustificationType(juce::Justification::centred);
    reverbEnabledLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    reverbEnabledLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    reverbTypeCombo.addItem("Schroeder", 1);
    reverbTypeCombo.addItem("Void Verb", 2);
    reverbTypeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "reverbType", reverbTypeCombo);
    reverbTypeLabel.setText("Type", juce::dontSendNotification);
    reverbTypeLabel.setJustificationType(juce::Justification::centred);
    reverbTypeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    reverbTypeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    reverbWetMixSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    reverbWetMixSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    reverbWetMixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "reverbWetMix", reverbWetMixSlider);
    reverbWetMixLabel.setText("Mix", juce::dontSendNotification);
    reverbWetMixLabel.setJustificationType(juce::Justification::centred);
    reverbWetMixLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    reverbWetMixLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    reverbDecayTimeSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    reverbDecayTimeSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    reverbDecayTimeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "reverbDecayTime", reverbDecayTimeSlider);
    reverbDecayTimeLabel.setText("Decay", juce::dontSendNotification);
    reverbDecayTimeLabel.setJustificationType(juce::Justification::centred);
    reverbDecayTimeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    reverbDecayTimeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    reverbFilterShowButton.setButtonText(safeString("Filter"));
    reverbFilterShowAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "reverbFilterShow", reverbFilterShowButton);
    
    reverbFilterWarmSaturationButton.setButtonText(safeString("Warm Saturation"));
    reverbFilterWarmSaturationAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "reverbFilterWarmSaturation", reverbFilterWarmSaturationButton);
    
    reverbFilterHPCutoffSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    reverbFilterHPCutoffSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    reverbFilterHPCutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "reverbFilterHPCutoff", reverbFilterHPCutoffSlider);
    reverbFilterHPCutoffLabel.setText("HP Cutoff", juce::dontSendNotification);
    reverbFilterHPCutoffLabel.setJustificationType(juce::Justification::centred);
    reverbFilterHPCutoffLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    reverbFilterHPCutoffLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    reverbFilterHPResonanceSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    reverbFilterHPResonanceSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    reverbFilterHPResonanceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "reverbFilterHPResonance", reverbFilterHPResonanceSlider);
    reverbFilterHPResonanceLabel.setText("HP Res", juce::dontSendNotification);
    reverbFilterHPResonanceLabel.setJustificationType(juce::Justification::centred);
    reverbFilterHPResonanceLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    reverbFilterHPResonanceLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    reverbFilterLPCutoffSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    reverbFilterLPCutoffSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    reverbFilterLPCutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "reverbFilterLPCutoff", reverbFilterLPCutoffSlider);
    reverbFilterLPCutoffLabel.setText("LP Cutoff", juce::dontSendNotification);
    reverbFilterLPCutoffLabel.setJustificationType(juce::Justification::centred);
    reverbFilterLPCutoffLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    reverbFilterLPCutoffLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
    reverbFilterLPResonanceSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    reverbFilterLPResonanceSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    reverbFilterLPResonanceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "reverbFilterLPResonance", reverbFilterLPResonanceSlider);
    reverbFilterLPResonanceLabel.setText("LP Res", juce::dontSendNotification);
    reverbFilterLPResonanceLabel.setJustificationType(juce::Justification::centred);
    reverbFilterLPResonanceLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    reverbFilterLPResonanceLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    //==============================================================================
    // -- Grain Delay Effect Setup (Effects tab) --
    grainDelayEnabledButton.setButtonText(safeString("On"));
    grainDelayEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayEnabled", grainDelayEnabledButton);
    grainDelayEnabledLabel.setText(safeString("On"), juce::dontSendNotification);
    grainDelayEnabledLabel.setJustificationType(juce::Justification::centred);
    grainDelayEnabledLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    grainDelayEnabledLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    grainDelayTimeSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    grainDelayTimeSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 18);
    grainDelayTimeSlider.setTextValueSuffix(" ms");
    grainDelayTimeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayTime", grainDelayTimeSlider);
    grainDelayTimeLabel.setText(safeString("Time"), juce::dontSendNotification);
    grainDelayTimeLabel.setJustificationType(juce::Justification::centred);
    grainDelayTimeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    grainDelayTimeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    grainDelaySizeSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    grainDelaySizeSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 18);
    grainDelaySizeSlider.setTextValueSuffix(" ms");
    grainDelaySizeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "grainDelaySize", grainDelaySizeSlider);
    grainDelaySizeLabel.setText(safeString("Size"), juce::dontSendNotification);
    grainDelaySizeLabel.setJustificationType(juce::Justification::centred);
    grainDelaySizeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    grainDelaySizeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    grainDelayPitchSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    grainDelayPitchSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 18);
    grainDelayPitchSlider.setTextValueSuffix(" st");
    grainDelayPitchSlider.setNumDecimalPlacesToDisplay(1);
    grainDelayPitchAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayPitch", grainDelayPitchSlider);
    grainDelayPitchLabel.setText(safeString("Pitch"), juce::dontSendNotification);
    grainDelayPitchLabel.setJustificationType(juce::Justification::centred);
    grainDelayPitchLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    grainDelayPitchLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    grainDelayMixSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    grainDelayMixSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 18);
    grainDelayMixSlider.setTextValueSuffix(" %");
    grainDelayMixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayMix", grainDelayMixSlider);
    grainDelayMixLabel.setText(safeString("Mix"), juce::dontSendNotification);
    grainDelayMixLabel.setJustificationType(juce::Justification::centred);
    grainDelayMixLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    grainDelayMixLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    grainDelayDecaySlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    grainDelayDecaySlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 18);
    grainDelayDecaySlider.setTextValueSuffix(" %");
    grainDelayDecayAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayDecay", grainDelayDecaySlider);
    grainDelayDecayLabel.setText(safeString("Decay"), juce::dontSendNotification);
    grainDelayDecayLabel.setJustificationType(juce::Justification::centred);
    grainDelayDecayLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    grainDelayDecayLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    grainDelayDensitySlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    grainDelayDensitySlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 18);
    grainDelayDensitySlider.setTextValueSuffix("");
    grainDelayDensityAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayDensity", grainDelayDensitySlider);
    grainDelayDensityLabel.setText(safeString("Density"), juce::dontSendNotification);
    grainDelayDensityLabel.setJustificationType(juce::Justification::centred);
    grainDelayDensityLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    grainDelayDensityLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    grainDelayJitterSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    grainDelayJitterSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 18);
    grainDelayJitterSlider.setTextValueSuffix(" %");
    grainDelayJitterAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayJitter", grainDelayJitterSlider);
    grainDelayJitterLabel.setText(safeString("Jitter"), juce::dontSendNotification);
    grainDelayJitterLabel.setJustificationType(juce::Justification::centred);
    grainDelayJitterLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    grainDelayJitterLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    grainDelayPingPongButton.setButtonText(safeString("Ping-Pong"));
    grainDelayPingPongAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayPingPong", grainDelayPingPongButton);
    grainDelayPingPongLabel.setText(safeString("Ping-Pong"), juce::dontSendNotification);
    grainDelayPingPongLabel.setJustificationType(juce::Justification::centred);
    grainDelayPingPongLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    grainDelayPingPongLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    grainDelayFilterShowButton.setButtonText(safeString("Filter"));
    grainDelayFilterShowAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayFilterShow", grainDelayFilterShowButton);
    grainDelayFilterHPCutoffSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    grainDelayFilterHPCutoffSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    grainDelayFilterHPCutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayFilterHPCutoff", grainDelayFilterHPCutoffSlider);
    grainDelayFilterHPCutoffLabel.setText(safeString("HP Cutoff"), juce::dontSendNotification);
    grainDelayFilterHPCutoffLabel.setJustificationType(juce::Justification::centred);
    grainDelayFilterHPCutoffLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    grainDelayFilterHPCutoffLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    grainDelayFilterHPResonanceSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    grainDelayFilterHPResonanceSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    grainDelayFilterHPResonanceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayFilterHPResonance", grainDelayFilterHPResonanceSlider);
    grainDelayFilterHPResonanceLabel.setText(safeString("HP Res"), juce::dontSendNotification);
    grainDelayFilterHPResonanceLabel.setJustificationType(juce::Justification::centred);
    grainDelayFilterHPResonanceLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    grainDelayFilterHPResonanceLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    grainDelayFilterLPCutoffSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    grainDelayFilterLPCutoffSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    grainDelayFilterLPCutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayFilterLPCutoff", grainDelayFilterLPCutoffSlider);
    grainDelayFilterLPCutoffLabel.setText(safeString("LP Cutoff"), juce::dontSendNotification);
    grainDelayFilterLPCutoffLabel.setJustificationType(juce::Justification::centred);
    grainDelayFilterLPCutoffLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    grainDelayFilterLPCutoffLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    grainDelayFilterLPResonanceSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    grainDelayFilterLPResonanceSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    grainDelayFilterLPResonanceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayFilterLPResonance", grainDelayFilterLPResonanceSlider);
    grainDelayFilterLPResonanceLabel.setText(safeString("LP Res"), juce::dontSendNotification);
    grainDelayFilterLPResonanceLabel.setJustificationType(juce::Justification::centred);
    grainDelayFilterLPResonanceLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    grainDelayFilterLPResonanceLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    grainDelayFilterWarmSaturationButton.setButtonText(safeString("Warm Saturation"));
    grainDelayFilterWarmSaturationAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "grainDelayFilterWarmSaturation", grainDelayFilterWarmSaturationButton);

    // Phaser Effect (Effects tab)
    phaserEnabledButton.setButtonText(safeString("On"));
    phaserEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "phaserEnabled", phaserEnabledButton);
    phaserEnabledLabel.setText(safeString("Phaser"), juce::dontSendNotification);
    phaserEnabledLabel.setJustificationType(juce::Justification::centred);
    phaserEnabledLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    phaserEnabledLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    phaserRateSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    phaserRateSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    phaserRateSlider.setTextValueSuffix(" Hz");
    phaserRateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "phaserRate", phaserRateSlider);
    phaserRateLabel.setText(safeString("Rate"), juce::dontSendNotification);
    phaserRateLabel.setJustificationType(juce::Justification::centred);
    phaserRateLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    phaserRateLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    phaserDepthSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    phaserDepthSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    phaserDepthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "phaserDepth", phaserDepthSlider);
    phaserDepthLabel.setText(safeString("Depth"), juce::dontSendNotification);
    phaserDepthLabel.setJustificationType(juce::Justification::centred);
    phaserDepthLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    phaserDepthLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    phaserFeedbackSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    phaserFeedbackSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    phaserFeedbackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "phaserFeedback", phaserFeedbackSlider);
    phaserFeedbackLabel.setText(safeString("Feedback"), juce::dontSendNotification);
    phaserFeedbackLabel.setJustificationType(juce::Justification::centred);
    phaserFeedbackLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    phaserFeedbackLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    phaserScriptModeButton.setButtonText(safeString("Script"));
    phaserScriptModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "phaserScriptMode", phaserScriptModeButton);
    phaserScriptModeLabel.setText(safeString("Mode"), juce::dontSendNotification);
    phaserScriptModeLabel.setJustificationType(juce::Justification::centred);
    phaserScriptModeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    phaserScriptModeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    phaserMixSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    phaserMixSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    phaserMixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "phaserMix", phaserMixSlider);
    phaserMixLabel.setText(safeString("Mix"), juce::dontSendNotification);
    phaserMixLabel.setJustificationType(juce::Justification::centred);
    phaserMixLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    phaserMixLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    phaserCentreSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    phaserCentreSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    phaserCentreSlider.setTextValueSuffix(" Hz");
    phaserCentreAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "phaserCentre", phaserCentreSlider);
    phaserCentreLabel.setText(safeString("Center"), juce::dontSendNotification);
    phaserCentreLabel.setJustificationType(juce::Justification::centred);
    phaserCentreLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    phaserCentreLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    phaserStagesCombo.addItem(safeString("4 (Phase 90)"), 1);
    phaserStagesCombo.addItem(safeString("6 (Deeper)"), 2);
    phaserStagesCombo.setSelectedId(1);
    phaserStagesAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "phaserStages", phaserStagesCombo);
    phaserStagesLabel.setText(safeString("Stages"), juce::dontSendNotification);
    phaserStagesLabel.setJustificationType(juce::Justification::centred);
    phaserStagesLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    phaserStagesLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    phaserStereoOffsetSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    phaserStereoOffsetSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    phaserStereoOffsetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "phaserStereoOffset", phaserStereoOffsetSlider);
    phaserStereoOffsetLabel.setText(safeString("Width"), juce::dontSendNotification);
    phaserStereoOffsetLabel.setJustificationType(juce::Justification::centred);
    phaserStereoOffsetLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    phaserStereoOffsetLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    phaserVintageModeButton.setButtonText(safeString("Vintage"));
    phaserVintageModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "phaserVintageMode", phaserVintageModeButton);
    phaserVintageModeLabel.setText(safeString("LFO"), juce::dontSendNotification);
    phaserVintageModeLabel.setJustificationType(juce::Justification::centred);
    phaserVintageModeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    phaserVintageModeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    // Flanger Effect (Effects tab)
    flangerEnabledButton.setButtonText(safeString("On"));
    flangerEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "flangerEnabled", flangerEnabledButton);
    flangerEnabledLabel.setText(safeString("Flanger"), juce::dontSendNotification);
    flangerEnabledLabel.setJustificationType(juce::Justification::centred);
    flangerEnabledLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    flangerEnabledLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    flangerRateSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    flangerRateSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    flangerRateSlider.setTextValueSuffix(" Hz");
    flangerRateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "flangerRate", flangerRateSlider);
    flangerRateLabel.setText(safeString("Rate"), juce::dontSendNotification);
    flangerRateLabel.setJustificationType(juce::Justification::centred);
    flangerRateLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    flangerRateLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    flangerDepthSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    flangerDepthSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    flangerDepthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "flangerDepth", flangerDepthSlider);
    flangerDepthLabel.setText(safeString("Depth"), juce::dontSendNotification);
    flangerDepthLabel.setJustificationType(juce::Justification::centred);
    flangerDepthLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    flangerDepthLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    flangerFeedbackSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    flangerFeedbackSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    flangerFeedbackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "flangerFeedback", flangerFeedbackSlider);
    flangerFeedbackLabel.setText(safeString("Feedback"), juce::dontSendNotification);
    flangerFeedbackLabel.setJustificationType(juce::Justification::centred);
    flangerFeedbackLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    flangerFeedbackLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    flangerWidthSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    flangerWidthSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    flangerWidthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "flangerWidth", flangerWidthSlider);
    flangerWidthLabel.setText(safeString("Width"), juce::dontSendNotification);
    flangerWidthLabel.setJustificationType(juce::Justification::centred);
    flangerWidthLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    flangerWidthLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    flangerMixSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    flangerMixSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    flangerMixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "flangerMix", flangerMixSlider);
    flangerMixLabel.setText(safeString("Mix"), juce::dontSendNotification);
    flangerMixLabel.setJustificationType(juce::Justification::centred);
    flangerMixLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    flangerMixLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    // Bit Crusher Effect (Effects tab)
    bitCrusherEnabledButton.setButtonText(safeString("On"));
    bitCrusherEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "bitCrusherEnabled", bitCrusherEnabledButton);
    bitCrusherEnabledLabel.setText(safeString("Bit Crusher"), juce::dontSendNotification);
    bitCrusherEnabledLabel.setJustificationType(juce::Justification::centred);
    bitCrusherEnabledLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    bitCrusherEnabledLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    bitCrusherPostEffectButton.setButtonText(safeString("Post Effect"));
    bitCrusherPostEffectAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "bitCrusherPostEffect", bitCrusherPostEffectButton);
    bitCrusherPostEffectLabel.setText(safeString("Before / After"), juce::dontSendNotification);
    bitCrusherPostEffectLabel.setJustificationType(juce::Justification::centred);
    bitCrusherPostEffectLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    bitCrusherPostEffectLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    bitCrusherAmountSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    bitCrusherAmountSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    bitCrusherAmountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "bitCrusherAmount", bitCrusherAmountSlider);
    bitCrusherAmountLabel.setText(safeString("Amount"), juce::dontSendNotification);
    bitCrusherAmountLabel.setJustificationType(juce::Justification::centred);
    bitCrusherAmountLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    bitCrusherAmountLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    bitCrusherRateSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    bitCrusherRateSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    bitCrusherRateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "bitCrusherRate", bitCrusherRateSlider);
    bitCrusherRateLabel.setText(safeString("Rate"), juce::dontSendNotification);
    bitCrusherRateLabel.setJustificationType(juce::Justification::centred);
    bitCrusherRateLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    bitCrusherRateLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    bitCrusherMixSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    bitCrusherMixSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    bitCrusherMixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "bitCrusherMix", bitCrusherMixSlider);
    bitCrusherMixLabel.setText(safeString("Mix"), juce::dontSendNotification);
    bitCrusherMixLabel.setJustificationType(juce::Justification::centred);
    bitCrusherMixLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    bitCrusherMixLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    // Soft Clipper (Saturation Color tab)
    softClipperEnabledButton.setButtonText(safeString("On"));
    softClipperEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "softClipperEnabled", softClipperEnabledButton);
    softClipperEnabledLabel.setText(safeString("Soft Clipper"), juce::dontSendNotification);
    softClipperEnabledLabel.setJustificationType(juce::Justification::centred);
    softClipperEnabledLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    softClipperEnabledLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    softClipperModeCombo.addItem(safeString("Smooth"), 1);
    softClipperModeCombo.addItem(safeString("Crisp"), 2);
    softClipperModeCombo.addItem(safeString("Tube"), 3);
    softClipperModeCombo.addItem(safeString("Tape"), 4);
    softClipperModeCombo.addItem(safeString("Guitar"), 5);
    softClipperModeCombo.setSelectedId(1);
    softClipperModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "softClipperMode", softClipperModeCombo);
    softClipperModeLabel.setText(safeString("Mode"), juce::dontSendNotification);
    softClipperModeLabel.setJustificationType(juce::Justification::centred);
    softClipperModeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    softClipperModeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    softClipperDriveSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    softClipperDriveSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    softClipperDriveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "softClipperDrive", softClipperDriveSlider);
    softClipperDriveLabel.setText(safeString("Drive"), juce::dontSendNotification);
    softClipperDriveLabel.setJustificationType(juce::Justification::centred);
    softClipperDriveLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    softClipperDriveLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    softClipperKneeSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    softClipperKneeSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    softClipperKneeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "softClipperKnee", softClipperKneeSlider);
    softClipperKneeLabel.setText(safeString("Knee"), juce::dontSendNotification);
    softClipperKneeLabel.setJustificationType(juce::Justification::centred);
    softClipperKneeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    softClipperKneeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    softClipperOversampleCombo.addItem(safeString("2x"), 1);
    softClipperOversampleCombo.addItem(safeString("4x"), 2);
    softClipperOversampleCombo.addItem(safeString("8x"), 3);
    softClipperOversampleCombo.addItem(safeString("16x"), 4);
    softClipperOversampleCombo.setSelectedId(2);
    softClipperOversampleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "softClipperOversample", softClipperOversampleCombo);
    softClipperOversampleLabel.setText(safeString("Oversampling"), juce::dontSendNotification);
    softClipperOversampleLabel.setJustificationType(juce::Justification::centred);
    softClipperOversampleLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    softClipperOversampleLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    softClipperMixSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    softClipperMixSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    softClipperMixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "softClipperMix", softClipperMixSlider);
    softClipperMixLabel.setText(safeString("Mix"), juce::dontSendNotification);
    softClipperMixLabel.setJustificationType(juce::Justification::centred);
    softClipperMixLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    softClipperMixLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    // Compressor (Saturation Color tab)
    compressorEnabledButton.setButtonText(safeString("On"));
    compressorEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "compressorEnabled", compressorEnabledButton);
    compressorEnabledLabel.setText(safeString("Compressor"), juce::dontSendNotification);
    compressorEnabledLabel.setJustificationType(juce::Justification::centred);
    compressorEnabledLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    compressorEnabledLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    compressorTypeCombo.addItem(safeString("Compressor 1"), 1);
    compressorTypeCombo.addItem(safeString("Compressor 2"), 2);
    compressorTypeCombo.addItem(safeString("Compressor 3"), 3);
    compressorTypeCombo.setSelectedId(1);
    compressorTypeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "compressorType", compressorTypeCombo);
    compressorTypeLabel.setText(safeString("Type"), juce::dontSendNotification);
    compressorTypeLabel.setJustificationType(juce::Justification::centred);
    compressorTypeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    compressorTypeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    compressorThresholdSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    compressorThresholdSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    compressorThresholdSlider.setTextValueSuffix(safeString(" dB"));
    compressorThresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "compressorThreshold", compressorThresholdSlider);
    compressorThresholdLabel.setText(safeString("Threshold"), juce::dontSendNotification);
    compressorThresholdLabel.setJustificationType(juce::Justification::centred);
    compressorThresholdLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    compressorThresholdLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    compressorRatioSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    compressorRatioSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    compressorRatioSlider.setTextValueSuffix(safeString(":1"));
    compressorRatioAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "compressorRatio", compressorRatioSlider);
    compressorRatioLabel.setText(safeString("Ratio"), juce::dontSendNotification);
    compressorRatioLabel.setJustificationType(juce::Justification::centred);
    compressorRatioLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    compressorRatioLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    compressorAttackSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    compressorAttackSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    compressorAttackSlider.setTextValueSuffix(safeString(" ms"));
    compressorAttackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "compressorAttack", compressorAttackSlider);
    compressorAttackLabel.setText(safeString("Attack"), juce::dontSendNotification);
    compressorAttackLabel.setJustificationType(juce::Justification::centred);
    compressorAttackLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    compressorAttackLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    compressorReleaseSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    compressorReleaseSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    compressorReleaseSlider.setTextValueSuffix(safeString(" ms"));
    compressorReleaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "compressorRelease", compressorReleaseSlider);
    compressorReleaseLabel.setText(safeString("Release"), juce::dontSendNotification);
    compressorReleaseLabel.setJustificationType(juce::Justification::centred);
    compressorReleaseLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    compressorReleaseLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    compressorMakeupSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    compressorMakeupSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    compressorMakeupSlider.setTextValueSuffix(safeString(" dB"));
    compressorMakeupAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "compressorMakeup", compressorMakeupSlider);
    compressorMakeupLabel.setText(safeString("Makeup"), juce::dontSendNotification);
    compressorMakeupLabel.setJustificationType(juce::Justification::centred);
    compressorMakeupLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    compressorMakeupLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    compressorMixSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    compressorMixSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    compressorMixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "compressorMix", compressorMixSlider);
    compressorMixLabel.setText(safeString("Mix"), juce::dontSendNotification);
    compressorMixLabel.setJustificationType(juce::Justification::centred);
    compressorMixLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    compressorMixLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    compressorAutoReleaseButton.setButtonText(safeString("Auto Rel"));
    compressorAutoReleaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "compressorAutoRelease", compressorAutoReleaseButton);
    compressorAutoReleaseLabel.setText(safeString("Auto"), juce::dontSendNotification);
    compressorAutoReleaseLabel.setJustificationType(juce::Justification::centred);
    compressorAutoReleaseLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    compressorAutoReleaseLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    compressorSoftClipButton.setButtonText(safeString("Soft Clip"));
    compressorSoftClipAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "compressorSoftClip", compressorSoftClipButton);
    compressorSoftClipLabel.setText(safeString("Clip"), juce::dontSendNotification);
    compressorSoftClipLabel.setJustificationType(juce::Justification::centred);
    compressorSoftClipLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    compressorSoftClipLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    // Transient (Saturation Color tab)
    transientEnabledButton.setButtonText(safeString("On"));
    transientEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "transientEnabled", transientEnabledButton);
    transientEnabledLabel.setText(safeString("Transient"), juce::dontSendNotification);
    transientEnabledLabel.setJustificationType(juce::Justification::centred);
    transientEnabledLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    transientEnabledLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    // The ten drums, and then whichever User slots hold a sample -- filled in by
    // rebuildWaveformMenus, as for every other menu that can select an import.
    transientTypeCombo.addItem(safeString("808 Kick"), 1);
    transientTypeCombo.addItem(safeString("808 Snare"), 2);
    transientTypeCombo.addItem(safeString("808 Hat"), 3);
    transientTypeCombo.addItem(safeString("808 Open Hat"), 4);
    transientTypeCombo.addItem(safeString("808 Clap"), 5);
    transientTypeCombo.addItem(safeString("808 Tom"), 6);
    transientTypeCombo.addItem(safeString("808 Rim"), 7);
    transientTypeCombo.addItem(safeString("808 Cowbell"), 8);
    transientTypeCombo.addItem(safeString("909 Kick"), 9);
    transientTypeCombo.addItem(safeString("909 Snare"), 10);
    if (auto* transientTypeParam = audioProcessor.getValueTreeState().getParameter("transientType"))
        transientTypeAttachment = std::make_unique<WaveformChoiceAttachment>(
            *transientTypeParam, transientTypeCombo);
    transientTypeLabel.setText(safeString("Sound"), juce::dontSendNotification);
    transientTypeLabel.setJustificationType(juce::Justification::centred);
    transientTypeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    transientTypeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    transientMixSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    transientMixSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    transientMixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "transientMix", transientMixSlider);
    transientMixLabel.setText(safeString("Mix"), juce::dontSendNotification);
    transientMixLabel.setJustificationType(juce::Justification::centred);
    transientMixLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    transientMixLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    transientPostEffectButton.setButtonText(safeString("Post Effect"));
    transientPostEffectAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "transientPostEffect", transientPostEffectButton);
    transientPostEffectLabel.setText(safeString("Pre / Post"), juce::dontSendNotification);
    transientPostEffectLabel.setJustificationType(juce::Justification::centred);
    transientPostEffectLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    transientPostEffectLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    transientKaDonkSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    transientKaDonkSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    transientKaDonkAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "transientKaDonk", transientKaDonkSlider);
    transientKaDonkLabel.setText(safeString("Ka-Donk"), juce::dontSendNotification);
    transientKaDonkLabel.setJustificationType(juce::Justification::centred);
    transientKaDonkLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    transientKaDonkLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    transientCoarseSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    transientCoarseSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    transientCoarseSlider.setTextValueSuffix(safeString(" st"));
    transientCoarseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "transientCoarse", transientCoarseSlider);
    transientCoarseLabel.setText(safeString("Coarse"), juce::dontSendNotification);
    transientCoarseLabel.setJustificationType(juce::Justification::centred);
    transientCoarseLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    transientCoarseLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    transientLengthSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    transientLengthSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    transientLengthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "transientLength", transientLengthSlider);
    transientLengthLabel.setText(safeString("Length"), juce::dontSendNotification);
    transientLengthLabel.setJustificationType(juce::Justification::centred);
    transientLengthLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    transientLengthLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));

    // Lo-Fi (Saturation Color tab)
    lofiEnabledButton.setButtonText(safeString("On"));
    lofiEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "lofiEnabled", lofiEnabledButton);
    lofiEnabledLabel.setText(safeString("Lo-Fi"), juce::dontSendNotification);
    lofiEnabledLabel.setJustificationType(juce::Justification::centred);
    lofiEnabledLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lofiEnabledLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    lofiAmountSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    lofiAmountSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    lofiAmountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "lofiAmount", lofiAmountSlider);
    lofiAmountLabel.setText(safeString("Lofi"), juce::dontSendNotification);
    lofiAmountLabel.setJustificationType(juce::Justification::centred);
    lofiAmountLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    lofiAmountLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    analogDriftSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    analogDriftSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    analogDriftAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "analogDrift", analogDriftSlider);
    analogDriftLabel.setText(safeString("Drift"), juce::dontSendNotification);
    analogDriftLabel.setJustificationType(juce::Justification::centred);
    analogDriftLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    analogDriftLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    {
        const juce::String driftTip = safeString(
            "Only active while Lo-Fi is on. Analog-style imprecision: small random pitch offset per oscillator "
            "and filter cutoff per note, plus a very slow wander while you play, like component tolerances "
            "and drift in hardware synths. Higher values add more warmth and movement; keep low for subtlety.");
        analogDriftSlider.setTooltip(driftTip);
        analogDriftLabel.setTooltip(driftTip);
    }

    // -- Final EQ --
    finalEQGroup.setText(safeString("Final Equalizer"));
    finalEQGroup.getProperties().set("viewportGlow", true);
    finalEQEnabledButton.setButtonText(safeString("On"));
    finalEQEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "finalEQEnabled", finalEQEnabledButton);
    finalEQEnabledLabel.setText(safeString("Final Equalizer"), juce::dontSendNotification);
    finalEQEnabledLabel.setJustificationType(juce::Justification::centred);
    finalEQEnabledLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    finalEQEnabledLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    finalEQComponent = std::make_unique<FinalEQComponent>(
        audioProcessor.getValueTreeState(),
        audioProcessor.getSampleRate() > 0.0 ? audioProcessor.getSampleRate() : 44100.0);
    finalEQEnabledLabel.setVisible(false);
    // Same audio the Spectral tab analyses, so the bars behind the EQ curve are the
    // same picture of the same signal. The component drives its own FFT at 60 fps.
    finalEQComponent->setSampleSource([this](float* dest, int numSamples)
    {
        audioProcessor.readSpectrumSamples(dest, numSamples);
    });
    // Clicking a dot in the display moves the controls below it onto that band.
    finalEQComponent->onBandSelected = [this](int band) { setFinalEQEditedBand(band); };

    for (int b = 1; b <= FinalEQComponent::numBands; ++b)
        finalEQNodeCombo.addItem("Node " + juce::String(b), b);   // built here, so already valid UTF-8
    finalEQNodeCombo.setSelectedId(1, juce::dontSendNotification);
    finalEQNodeCombo.onChange = [this]
    {
        setFinalEQEditedBand(finalEQNodeCombo.getSelectedId() - 1);
    };
    finalEQNodeLabel.setText(safeString("Node"), juce::dontSendNotification);

    finalEQResetButton.setButtonText(safeString("Reset"));
    finalEQResetButton.setTooltip(safeString(
        "Puts the chosen node back where it started: its default frequency, no gain, "
        "and its default Quality. The node's Type is left as you set it. "
        "Double-clicking the node in the display does the same thing."));
    finalEQResetButton.onClick = [this]
    {
        if (finalEQComponent != nullptr)
            finalEQComponent->resetBand(finalEQEditedBand_);
    };

    {
        const auto typeNames = SpaceDustFinalEQ::typeChoices();
        for (int i = 0; i < typeNames.size(); ++i)
            finalEQTypeCombo.addItem(typeNames[i], i + 1);
    }
    // The attachment maps by item index, so the order above must stay exactly the
    // order of the parameter's choices. Fires on user picks AND on the attachment's
    // own updates, which is what keeps the Gain knob's greying honest.
    finalEQTypeCombo.onChange = [this]
    {
        const bool usesGain = SpaceDustFinalEQ::typeUsesGain(
            SpaceDustFinalEQ::typeFromChoiceIndex(finalEQTypeCombo.getSelectedItemIndex()));
        finalEQGainSlider.setEnabled(usesGain);
        finalEQGainLabel.setEnabled(usesGain);
    };
    finalEQTypeLabel.setText(safeString("Type"), juce::dontSendNotification);

    finalEQQSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    finalEQQSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    finalEQQLabel.setText(safeString("Quality"), juce::dontSendNotification);

    finalEQFreqSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    finalEQFreqSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    finalEQFreqSlider.setTextValueSuffix(safeString(" Hz"));
    finalEQFreqLabel.setText(safeString("Frequency"), juce::dontSendNotification);

    finalEQGainSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    finalEQGainSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    finalEQGainSlider.setTextValueSuffix(safeString(" dB"));
    finalEQGainLabel.setText(safeString("Gain"), juce::dontSendNotification);

    for (auto* l : { &finalEQNodeLabel, &finalEQTypeLabel, &finalEQQLabel,
                     &finalEQFreqLabel, &finalEQGainLabel })
    {
        l->setJustificationType(juce::Justification::centred);
        l->setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
        l->setFont(customLookAndFeel.getBodyFont(12.0f, true));
    }

    // Builds the four attachments for the first time; nothing above may rely on
    // them existing before this call.
    setFinalEQEditedBand(0);

    // Trance Gate Effect (Effects tab)
    tranceGateEnabledButton.setButtonText(safeString("On"));
    tranceGateEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateEnabled", tranceGateEnabledButton);
    tranceGateEnabledLabel.setText(safeString("On"), juce::dontSendNotification);
    tranceGateEnabledLabel.setJustificationType(juce::Justification::centred);
    tranceGateEnabledLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    tranceGateEnabledLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    tranceGatePreEffectButton.setButtonText(safeString("Post Effect"));
    tranceGatePreEffectAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGatePostEffect", tranceGatePreEffectButton);
    tranceGatePreEffectLabel.setText(safeString("Before / After"), juce::dontSendNotification);
    tranceGatePreEffectLabel.setJustificationType(juce::Justification::centred);
    tranceGatePreEffectLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    tranceGatePreEffectLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    tranceGateStepsCombo.addItem(safeString("4 Steps"), 1);
    tranceGateStepsCombo.addItem(safeString("8 Steps"), 2);
    tranceGateStepsCombo.addItem(safeString("16 Steps"), 3);
    tranceGateStepsCombo.setSelectedId(2);
    tranceGateStepsAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateSteps", tranceGateStepsCombo);
    tranceGateStepsLabel.setText(safeString("Steps"), juce::dontSendNotification);
    tranceGateStepsLabel.setJustificationType(juce::Justification::centred);
    tranceGateStepsLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    tranceGateStepsLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    tranceGateSyncButton.setButtonText(safeString("Sync"));
    tranceGateSyncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateSync", tranceGateSyncButton);
    tranceGateSyncLabel.setText(safeString("Sync"), juce::dontSendNotification);
    tranceGateSyncLabel.setJustificationType(juce::Justification::centred);
    tranceGateSyncLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    tranceGateSyncLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    tranceGateRateSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    tranceGateRateSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    tranceGateRateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateRate", tranceGateRateSlider);
    tranceGateRateLabel.setText(safeString("Rate"), juce::dontSendNotification);
    tranceGateRateLabel.setJustificationType(juce::Justification::centred);
    tranceGateRateLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    tranceGateRateLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    tranceGateAttackSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    tranceGateAttackSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    tranceGateAttackSlider.setTextValueSuffix(" ms");
    tranceGateAttackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateAttack", tranceGateAttackSlider);
    tranceGateAttackLabel.setText(safeString("Attack"), juce::dontSendNotification);
    tranceGateAttackLabel.setJustificationType(juce::Justification::centred);
    tranceGateAttackLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    tranceGateAttackLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    tranceGateReleaseSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    tranceGateReleaseSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    tranceGateReleaseSlider.setTextValueSuffix(" ms");
    tranceGateReleaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateRelease", tranceGateReleaseSlider);
    tranceGateReleaseLabel.setText(safeString("Release"), juce::dontSendNotification);
    tranceGateReleaseLabel.setJustificationType(juce::Justification::centred);
    tranceGateReleaseLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    tranceGateReleaseLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    tranceGateMixSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    tranceGateMixSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    tranceGateMixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateMix", tranceGateMixSlider);
    tranceGateMixLabel.setText(safeString("Mix"), juce::dontSendNotification);
    tranceGateMixLabel.setJustificationType(juce::Justification::centred);
    tranceGateMixLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    tranceGateMixLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    tranceGateStep1Button.setButtonText(safeString("1"));
    tranceGateStep1Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep1", tranceGateStep1Button);
    tranceGateStep2Button.setButtonText(safeString("2"));
    tranceGateStep2Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep2", tranceGateStep2Button);
    tranceGateStep3Button.setButtonText(safeString("3"));
    tranceGateStep3Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep3", tranceGateStep3Button);
    tranceGateStep4Button.setButtonText(safeString("4"));
    tranceGateStep4Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep4", tranceGateStep4Button);
    tranceGateStep5Button.setButtonText(safeString("5"));
    tranceGateStep5Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep5", tranceGateStep5Button);
    tranceGateStep6Button.setButtonText(safeString("6"));
    tranceGateStep6Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep6", tranceGateStep6Button);
    tranceGateStep7Button.setButtonText(safeString("7"));
    tranceGateStep7Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep7", tranceGateStep7Button);
    tranceGateStep8Button.setButtonText(safeString("8"));
    tranceGateStep8Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep8", tranceGateStep8Button);
    tranceGateStep9Button.setButtonText(safeString("9"));
    tranceGateStep9Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep9", tranceGateStep9Button);
    tranceGateStep10Button.setButtonText(safeString("10"));
    tranceGateStep10Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep10", tranceGateStep10Button);
    tranceGateStep11Button.setButtonText(safeString("11"));
    tranceGateStep11Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep11", tranceGateStep11Button);
    tranceGateStep12Button.setButtonText(safeString("12"));
    tranceGateStep12Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep12", tranceGateStep12Button);
    tranceGateStep13Button.setButtonText(safeString("13"));
    tranceGateStep13Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep13", tranceGateStep13Button);
    tranceGateStep14Button.setButtonText(safeString("14"));
    tranceGateStep14Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep14", tranceGateStep14Button);
    tranceGateStep15Button.setButtonText(safeString("15"));
    tranceGateStep15Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep15", tranceGateStep15Button);
    tranceGateStep16Button.setButtonText(safeString("16"));
    tranceGateStep16Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "tranceGateStep16", tranceGateStep16Button);

    // Hide redundant labels to save UI space (toggles are self-evident)
    delayEnabledLabel.setVisible(false);
    reverbEnabledLabel.setVisible(false);
    grainDelayEnabledLabel.setVisible(false);
    phaserEnabledLabel.setVisible(false);
    phaserVintageModeLabel.setVisible(false);  // Remove label above Vintage toggle
    flangerEnabledLabel.setVisible(false);
    bitCrusherEnabledLabel.setVisible(false);
    bitCrusherPostEffectLabel.setVisible(false);
    softClipperEnabledLabel.setVisible(false);
    compressorEnabledLabel.setVisible(false);
    compressorAutoReleaseLabel.setVisible(false);
    compressorSoftClipLabel.setVisible(false);
    lofiEnabledLabel.setVisible(false);
    transientEnabledLabel.setVisible(false);
    transientPostEffectLabel.setVisible(false);
    tranceGateEnabledLabel.setVisible(false);
    tranceGatePreEffectLabel.setVisible(false);
    tranceGateStepsLabel.setVisible(false);
    tranceGateSyncLabel.setVisible(false);
    
    reverbFilterWarmSaturationButton.setVisible(false);
    reverbFilterHPCutoffSlider.setVisible(false);
    reverbFilterHPResonanceSlider.setVisible(false);
    reverbFilterLPCutoffSlider.setVisible(false);
    reverbFilterLPResonanceSlider.setVisible(false);
    reverbFilterHPCutoffLabel.setVisible(false);
    reverbFilterHPResonanceLabel.setVisible(false);
    reverbFilterLPCutoffLabel.setVisible(false);
    reverbFilterLPResonanceLabel.setVisible(false);
    
    // delayFilterGroup not used (redundant box removed)
    delayFilterHPCutoffSlider.setVisible(false);
    delayFilterHPResonanceSlider.setVisible(false);
    delayFilterLPCutoffSlider.setVisible(false);
    delayFilterLPResonanceSlider.setVisible(false);
    delayFilterWarmSaturationButton.setVisible(false);
    delayFilterHPCutoffLabel.setVisible(false);
    delayFilterHPResonanceLabel.setVisible(false);
    delayFilterLPCutoffLabel.setVisible(false);
    delayFilterLPResonanceLabel.setVisible(false);

    // Wire On toggles to glow effect/LFO groups when enabled
    auto syncGroupGlow = [this](juce::ToggleButton& btn, juce::GroupComponent& grp) {
        grp.getProperties().set("isActive", btn.getToggleState());
        grp.repaint();
    };
    delayEnabledButton.addListener(this);
    phaserEnabledButton.addListener(this);
    flangerEnabledButton.addListener(this);
    bitCrusherEnabledButton.addListener(this);
    softClipperEnabledButton.addListener(this);
    compressorEnabledButton.addListener(this);
    lofiEnabledButton.addListener(this);
    transientEnabledButton.addListener(this);
    reverbEnabledButton.addListener(this);
    tranceGateEnabledButton.addListener(this);
    grainDelayEnabledButton.addListener(this);
    lfo1EnabledButton.addListener(this);
    lfo2EnabledButton.addListener(this);
    syncGroupGlow(delayEnabledButton, delayGroup);
    syncGroupGlow(phaserEnabledButton, phaserGroup);
    syncGroupGlow(flangerEnabledButton, flangerGroup);
    syncGroupGlow(bitCrusherEnabledButton, bitCrusherGroup);
    syncGroupGlow(softClipperEnabledButton, softClipperGroup);
    syncGroupGlow(compressorEnabledButton, compressorGroup);
    syncGroupGlow(lofiEnabledButton, lofiGroup);
    syncGroupGlow(transientEnabledButton, transientGroup);
    syncGroupGlow(reverbEnabledButton, reverbGroup);
    syncGroupGlow(tranceGateEnabledButton, tranceGateGroup);
    syncGroupGlow(grainDelayEnabledButton, grainDelayGroup);
    syncGroupGlow(lfo1EnabledButton, lfo1Group);
    syncGroupGlow(lfo2EnabledButton, lfo2Group);
    finalEQEnabledButton.addListener(this);
    syncGroupGlow(finalEQEnabledButton, finalEQGroup);

    //==============================================================================
    // -- Assign buttons, one per LFO panel --
    // Built BEFORE the pages, because ModulationPageComponent's constructor adds
    // them as its children and its resized() puts them in the LFO boxes.
    //
    // Two, not spacedust::numLfos: LFOs 3 and 4 have their buffers and their
    // colours but no parameters and no panel yet, so there is nowhere to put a
    // third button. When they get a panel, this loop grows with it.
    for (int lfo = 0; lfo < numLfoPanels; ++lfo)
    {
        auto* button = lfoAssignButtons.add(new juce::TextButton(safeString("Assign")));

        // The LFO's own colour, so which LFO a lit knob belongs to is the same
        // colour as the button that lit it.
        button->setColour(juce::TextButton::buttonColourId,
                          spacedust::AssignModeState::colourFor(lfo).withAlpha(0.35f));
        button->setColour(juce::TextButton::buttonOnColourId,
                          spacedust::AssignModeState::colourFor(lfo).withAlpha(0.85f));

        // A second press on the LFO already being assigned leaves the mode, so
        // the button that turns it on turns it off again.
        button->onClick = [this, lfo]
        {
            assignMode.setActiveLfo(assignMode.activeLfo() == lfo ? -1 : lfo);
        };

        button->setTooltip(safeString(
            "Point this LFO at a knob: press, then drag any knob on another page."));
    }

    // Lives at the right-hand end of the tab strip, and is only there while the
    // mode is on -- leaving the mode is then where the eye already is when
    // switching tabs.
    exitLfoModeButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff4aa3ff));
    exitLfoModeButton.onClick = [this] { assignMode.setActiveLfo(-1); };
    addChildComponent(exitLfoModeButton);

    assignMode.addChangeListener(this);

    grainDelayFilterHPCutoffSlider.setVisible(false);
    grainDelayFilterHPResonanceSlider.setVisible(false);
    grainDelayFilterLPCutoffSlider.setVisible(false);
    grainDelayFilterLPResonanceSlider.setVisible(false);
    grainDelayFilterWarmSaturationButton.setVisible(false);
    grainDelayFilterHPCutoffLabel.setVisible(false);
    grainDelayFilterHPResonanceLabel.setVisible(false);
    grainDelayFilterLPCutoffLabel.setVisible(false);
    grainDelayFilterLPResonanceLabel.setVisible(false);
    
    //==============================================================================
    // -- Create Tabbed Pages --
    // Create page components and add them to the tabbed component
    DBG("Space Dust: Creating tabbed pages");
    try
    {
        mainPage = std::make_unique<MainPageComponent>(*this);
        modulationPage = std::make_unique<ModulationPageComponent>(*this);
        effectsPage = std::make_unique<EffectsPageComponent>(*this);
        saturationColorPage = std::make_unique<SaturationColorPageComponent>(*this);
        spectralPage = std::make_unique<SpectralPageComponent>(*this);
        
        tabbedComponent.addTab(safeString("Main"), juce::Colour(0xff0a0a1f), mainPage.get(), false);
        tabbedComponent.addTab(safeString("Modulation"), juce::Colour(0xff0a0a1f), modulationPage.get(), false);
        tabbedComponent.addTab(safeString("Effects"), juce::Colour(0xff0a0a1f), effectsPage.get(), false);
        tabbedComponent.addTab(safeString("Saturation Color"), juce::Colour(0xff0a0a1f), saturationColorPage.get(), false);
        tabbedComponent.addTab(safeString("Spectral"), juce::Colour(0xff0a0a1f), spectralPage.get(), false);

        // Restore Cheeze Guy tab if easter egg was previously activated
        if (audioProcessor.cheezeGuyActivated)
        {
            cheezeGuyGame = std::make_unique<CheezeGuyGameComponent>();
            tabbedComponent.addTab(safeString("Cheeze Guy"),
                juce::Colour(0xff0a0a1f), cheezeGuyGame.get(), false);
            cheezeGuyTabAdded = true;
        }

        // Ensure ALL labels in all tabs use our LookAndFeel (fixes random font inconsistencies)
        mainPage->setLookAndFeel(&customLookAndFeel);
        modulationPage->setLookAndFeel(&customLookAndFeel);
        effectsPage->setLookAndFeel(&customLookAndFeel);
        saturationColorPage->setLookAndFeel(&customLookAndFeel);
        spectralPage->setLookAndFeel(&customLookAndFeel);

        addAndMakeVisible(tabbedComponent);
        tabbedComponent.setOpaque(false);
        tabbedComponent.getTabbedButtonBar().setOpaque(false);

        // Restore the tab the user last had open (survives editor close/reopen).
        if (tabbedComponent.getNumTabs() > 0)
            tabbedComponent.setCurrentTabIndex(
                juce::jlimit(0, tabbedComponent.getNumTabs() - 1,
                             audioProcessor.lastActiveTabIndex));
        tabGlowOverlay = std::make_unique<TabGlowOverlayComponent>(*this);
        addAndMakeVisible(tabGlowOverlay.get());
        bottomTabGlowOverlay = std::make_unique<BottomTabGlowOverlayComponent>(*this);
        addAndMakeVisible(bottomTabGlowOverlay.get());
        DBG("Space Dust: Tabbed pages created and added");
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception creating tabbed pages: " + juce::String(e.what()));
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception creating tabbed pages");
    }
    
    //==============================================================================
    // -- Add Master Section Components (Always Visible) --
    // Master section is now always visible on both tabs, outside the TabbedComponent
    addAndMakeVisible(masterGroup);
    addAndMakeVisible(masterVolumeSlider);
    addAndMakeVisible(masterVolumeLabel);
    addAndMakeVisible(pitchBendAmountSlider);
    addAndMakeVisible(pitchBendAmountLabel);
    addAndMakeVisible(velocityAmountSlider);
    addAndMakeVisible(velocityAmountLabel);
    addAndMakeVisible(pitchBendSlider);
    addAndMakeVisible(pitchBendLabel);
    addAndMakeVisible(voiceModeCombo);
    addAndMakeVisible(voiceModeLabel);
    addAndMakeVisible(glideTimeSlider);
    addAndMakeVisible(glideTimeLabel);
    addAndMakeVisible(legatoGlideButton);
    addAndMakeVisible(legatoGlideLabel);
    if (stereoLevelMeter != nullptr)
        addAndMakeVisible(stereoLevelMeter.get());

    //==============================================================================
    // -- Standalone-only playable keyboard --
    // In the Standalone app there's no DAW to send MIDI, so add an on-screen keyboard
    // (also playable via the QWERTY computer keys when it has focus). Skipped entirely
    // in the VST3 build, so the plugin UI/layout is unchanged.
    if (audioProcessor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
    {
        standaloneKeyboard = std::make_unique<StandaloneKeyboard>(
            audioProcessor.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard);
        standaloneKeyboard->setLowestVisibleKey(36);          // start at C2
        standaloneKeyboard->setKeyPressBaseOctave(4);         // QWERTY row plays around C4
        // Z = octave down, X = octave up (handled by StandaloneKeyboard::keyPressed)
        standaloneKeyboard->setWantsKeyboardFocus(true);
        addAndMakeVisible(standaloneKeyboard.get());

        // Keep the keyboard playable while clicking knobs: when focus moves to a
        // non-text control inside our editor, globalFocusChanged() hands it back.
        juce::Desktop::getInstance().addFocusChangeListener(this);
    }

    //==============================================================================
    // -- Assign mode: lay a wrapper over every assignable knob --
    // HERE, and not earlier, for two reasons. Every page exists by now, so each
    // slider has the parent its wrapper must join; and this is still ahead of
    // the re-parent into mainView below, so a wrapper over a knob that is a
    // direct child of the editor travels into mainView with that knob instead
    // of being left behind on top of everything.
    wrapAssignableKnobs();
    logModCoverage();

    //==============================================================================
    // -- Drag-resize: move the whole UI into the scalable mainView container --
    // Every control added above is currently a direct child of the editor. Re-parent
    // them all into mainView (preserving visibility) so one transform on mainView scales
    // the entire UI uniformly. Done after all addAndMakeVisible() calls.
    addChildComponent(mainView);
    mainView.setInterceptsMouseClicks(false, true);   // transparent container; children stay clickable
    {
        auto existingChildren = getChildren();         // snapshot (we mutate parentage below)
        for (auto* child : existingChildren)
        {
            if (child == &mainView)
                continue;
            const bool wasVisible = child->isVisible();
            mainView.addChildComponent(child);         // re-parents into mainView
            child->setVisible(wasVisible);
        }
    }
    mainView.setVisible(true);

    //==============================================================================
    // -- The face goes straight into the host's rectangle --
    // mainView holds every control and is a direct child of this editor. It is added
    // FIRST so it sits at the back: the resizable corner added by setResizable() below
    // must stay on top of it to remain grabbable.
    addAndMakeVisible(mainView);

    // paintPlate() fills every pixel of this component (fillAll, then the face on top),
    // so declaring it spares JUCE an alpha pass over the whole window. This used to be
    // carried by the plate, which was opaque; removing the plate silently dropped it and
    // idle CPU went 1.9% -> 10.2% until this line was put back.
    setOpaque(true);

    designHeight_ = 857 + (standaloneKeyboard != nullptr ? standaloneKeyboardHeight : 0);

    //==============================================================================
    // -- Make the editor resizable NOW (synchronously, in the ctor) --
    // Restored verbatim from before the floating window (git 74f776e removed it on the
    // grounds that a fixed-size stub gave the host nothing to cache; the stub is gone,
    // so the original hazard is back and so is the fix).
    //
    // The host queries IPlugView::canResize() -> editor->isResizable() at attach time,
    // which is BEFORE the deferred timer below fires. If resizability isn't set yet the
    // host caches "fixed size" (confirmed on macOS after a clean .pkg install + rescan;
    // Windows only won the timing race). setResizable() is safe here: it just sets the
    // flag and adds the corner child - it does NOT trigger resized()/paint(), so the
    // Ableton crash that forced us to defer setSize() does not apply. (setSize() itself
    // stays deferred below.) This MUST come after the plate is added above so the
    // resizable corner stays on top of it and remains grabbable.
    setResizable(true, true);   // dragging the corner scales the whole UI (locked aspect)
    if (auto* constrainer = getConstrainer())
    {
        constrainer->setFixedAspectRatio((double) kDesignWidth / (double) designHeight_);
        constrainer->setSizeLimits(juce::roundToInt(kDesignWidth  * 0.45f),
                                   juce::roundToInt(designHeight_ * 0.45f),
                                   juce::roundToInt(kDesignWidth  * 1.60f),
                                   juce::roundToInt(designHeight_ * 1.60f));
    }

    //==============================================================================
    // -- Keyboard policy --
    // A plugin must never hold the keyboard, or the DAW stops seeing the spacebar and
    // the transport dies under the user's hands while they work the UI. The Standalone
    // is the opposite case: there is no host transport to protect, and the QWERTY
    // keyboard has to be able to hold focus to play notes at all.
    //
    // This is the job FloatingShell::setKeyboardFocusPolicy used to do. Back in the
    // host's own window the second half of that policy is no longer ours to enforce --
    // the host owns the top-level window and answers WM_MOUSEACTIVATE itself -- so what
    // is left is stopping our controls from grabbing focus when clicked, which is
    // exactly what preventControlsStealingKeyboardFocus does. Text editors are left
    // alone by it, so the preset-name field still types.
    if (audioProcessor.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
        preventControlsStealingKeyboardFocus(*this);

    // Everything the editor owns is parented by now, so this reaches the whole tree.
    // (Anything added later -- the CheezeGuy tab -- simply goes unbuffered, which costs
    // nothing beyond missing the optimisation.)
    cacheLabelRendering(*this);

    //==============================================================================
    // -- Set Window Size (DEFERRED VIA TIMER CALLBACK) --
    // CRITICAL: In Ableton Live, setSize() immediately triggers resized()/paint()
    // which may access LookAndFeel or components before the constructor completes.
    // Solution: Defer setSize() until after constructor returns, using a timer callback.
    // This ensures the constructor completes before any callbacks fire.
    #if JUCE_DEBUG
    try
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeString("SpaceDust_DebugLog.txt"));
        juce::FileOutputStream out(logFile);
        if (out.openedOk())
        {
            out.setPosition(out.getPosition());
            out.writeText("Space Dust: All components created, deferring setSize() via timer\n", false, false, nullptr);
            out.flush();
        }
    }
    catch (...) {}
    #endif
    
    // Defer setSize() until after constructor completes using a one-shot timer
    // This prevents resized()/paint() from firing during construction
    DBG("Space Dust: Editor ctor - Scheduling setSize() via timer");
    // callAfterDelay CANNOT be cancelled: if the editor is destroyed within these
    // 10ms (host opens/closes the view quickly) the lambda fires on a freed `this`.
    // A SafePointer goes null on destruction; check it WITHOUT dereferencing `this`
    // first, or we read freed memory (heap-use-after-free, confirmed via ASan).
    // (The old isBeingDestroyed flag check was itself the use-after-free.)
    juce::Component::SafePointer<SpaceDustAudioProcessorEditor> safeThis(this);
    juce::Timer::callAfterDelay(10, [this, safeThis]() {
        if (safeThis == nullptr)
            return;  // editor already gone â€” do not touch `this`
        DBG("Space Dust: Timer callback - About to set window size");
        #if JUCE_DEBUG
        try
        {
            juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(safeString("SpaceDust_DebugLog.txt"));
            juce::FileOutputStream out(logFile);
            if (out.openedOk())
            {
                out.setPosition(out.getPosition());
                out.writeText("Space Dust: Timer callback - checking isBeingDestroyed\n", false, false, nullptr);
                out.flush();
            }
        }
        catch (...) {}
        #endif
        
        if (isBeingDestroyed.load())
        {
            DBG("Space Dust: Timer callback - isBeingDestroyed=true, skipping setSize");
            #if JUCE_DEBUG
            try
            {
                juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile(safeString("SpaceDust_DebugLog.txt"));
                juce::FileOutputStream out(logFile);
                if (out.openedOk())
                {
                    out.setPosition(out.getPosition());
                    out.writeText("Space Dust: Timer callback - isBeingDestroyed=true, skipping\n", false, false, nullptr);
                    out.flush();
                }
            }
            catch (...) {}
            #endif
            return;
        }
        
        DBG("Space Dust: Timer callback - isBeingDestroyed=false, proceeding with setSize");
        #if JUCE_DEBUG
        try
        {
            juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(safeString("SpaceDust_DebugLog.txt"));
            juce::FileOutputStream out(logFile);
            if (out.openedOk())
            {
                out.setPosition(out.getPosition());
                out.writeText("Space Dust: Timer callback - About to call setSize(1120, 800)\n", false, false, nullptr);
                out.flush();
            }
        }
        catch (...) {}
        #endif
        
        // Calculate the correct window height for tabbed interface
        // Effects tab needs extra height when Delay/Grain Delay filter is toggled on (controls must stay visible)
        // Standalone build reserves an extra strip at the bottom for the playable keyboard.
        const int calculatedHeight = 857                                       // Original height
                                   + (standaloneKeyboard != nullptr ? standaloneKeyboardHeight : 0);

        DBG("Space Dust: Timer callback - Calling setSize(1120, " + juce::String(calculatedHeight) + ")");
        try
        {
            // CRITICAL: setSize() triggers resized() which will layout components
            // resized() must NOT call setSize() again to prevent infinite recursion
            designHeight_ = calculatedHeight;

            // A pair of stepper arrows on every dropdown in the plugin.
            //
            // Here, and by walking the tree, rather than named one by one beside
            // each combo: there are twenty-six of them across five pages, and a
            // list written by hand is a list that goes stale the first time a
            // dropdown is added. Everything exists by now -- the pages build their
            // controls in their constructors, which ran before this timer.
            // Every page, by name, and then the editor itself for the preset box.
            //
            // Walking the editor alone found seven dropdowns of twenty-six: a
            // TabbedComponent only parents the page that is SHOWING, so four
            // pages' worth of controls are not in the tree at all until their tab
            // is picked -- and a walk done once at startup would never see them.
            // The editor owns the five pages outright, so they can be asked
            // directly whether they are on screen or not.
            for (juce::Component* page : { (juce::Component*) mainPage.get(),
                                           (juce::Component*) modulationPage.get(),
                                           (juce::Component*) effectsPage.get(),
                                           (juce::Component*) saturationColorPage.get(),
                                           (juce::Component*) spectralPage.get() })
                if (page != nullptr)
                    attachComboSteppers(*page);

            attachComboSteppers(*this);

            // Auto-fit the INITIAL window size to the display (the user can then drag-resize).
            // Largest scale <= 0.95 that fits the primary display's user area; on a normal
            // 1080p+ screen the fit factors exceed 0.95 so it opens at the design 0.95.
            float initScale = 0.95f;
            if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            {
                const auto area    = display->userArea;   // excludes the taskbar
                const float margin = 0.92f;                // breathing room for title bar / DAW chrome
                const float fitW   = (float) area.getWidth()  * margin / (float) kDesignWidth;
                const float fitH   = (float) area.getHeight() * margin / (float) designHeight_;
                initScale = juce::jlimit(0.40f, 0.95f, juce::jmin(initScale, fitW, fitH));
            }
            // The EDITOR's logical size is the on-screen size; layoutPlate() scales
            // mainView and the painted background by getWidth()/kDesignWidth.
            setSize(juce::roundToInt(kDesignWidth  * initScale),
                    juce::roundToInt(designHeight_ * initScale));

            DBG("Space Dust: Timer callback - setSize() completed");
            #if JUCE_DEBUG
            try
            {
                juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile(safeString("SpaceDust_DebugLog.txt"));
                juce::FileOutputStream out(logFile);
                if (out.openedOk())
                {
                    out.setPosition(out.getPosition());
                    out.writeText("Space Dust: Timer callback - setSize() completed\n", false, false, nullptr);
                    out.flush();
                }
            }
            catch (...) {}
            #endif
        }
        catch (const std::exception& e)
        {
            DBG("Space Dust: Exception in setSize(): " + juce::String(e.what()));
            #if JUCE_DEBUG
            try
            {
                juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile(safeString("SpaceDust_DebugLog.txt"));
                juce::FileOutputStream out(logFile);
                if (out.openedOk())
                {
                    out.setPosition(out.getPosition());
                    out.writeText("Space Dust: Exception in setSize(): " + juce::String(e.what()) + "\n", false, false, nullptr);
                    out.flush();
                }
            }
            catch (...) {}
            #endif
        }
        catch (...)
        {
            DBG("Space Dust: Unknown exception in setSize()");
            #if JUCE_DEBUG
            try
            {
                juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile(safeString("SpaceDust_DebugLog.txt"));
                juce::FileOutputStream out(logFile);
                if (out.openedOk())
                {
                    out.setPosition(out.getPosition());
                    out.writeText("Space Dust: Unknown exception in setSize()\n", false, false, nullptr);
                    out.flush();
                }
            }
            catch (...) {}
            #endif
        }
        
        // (setResizable + aspect-lock constrainer now happen synchronously in the ctor,
        // before the host queries canResize() - see the re-parent section above.)

        // Standalone: give the on-screen keyboard focus so the QWERTY computer keys work
        // immediately, without the user having to click a note first. (Guarded SafePointer
        // re-grab in case the window isn't on-screen yet at this point.)
        if (standaloneKeyboard != nullptr)
        {
            standaloneKeyboard->grabKeyboardFocus();
            // Knobs/buttons must not steal that focus when clicked, so the QWERTY keys
            // keep playing while the user twists controls. (Text fields keep focus-grab.)
            preventControlsStealingKeyboardFocus(*this);
            juce::Component::SafePointer<SpaceDustAudioProcessorEditor> kbSafe(this);
            juce::Timer::callAfterDelay(250, [kbSafe]() {
                if (kbSafe != nullptr && kbSafe->standaloneKeyboard != nullptr)
                    kbSafe->standaloneKeyboard->grabKeyboardFocus();
            });
        }

        DBG("Space Dust: Timer callback - Window size set successfully");
        #if JUCE_DEBUG
        try
        {
            juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(safeString("SpaceDust_DebugLog.txt"));
            juce::FileOutputStream out(logFile);
            if (out.openedOk())
            {
                out.setPosition(out.getPosition());
                out.writeText("Space Dust: Window size set successfully in timer callback\n", false, false, nullptr);
                out.flush();
            }
        }
        catch (...) {}
        #endif
    });
    
    //==============================================================================
    // -- Register APVTS Listeners for Bidirectional Filter Sync --
    {
        // Only the link toggles need a listener: when one flips, we repoint that filter's knob
        // attachments at the master or its own params. Linked knobs share the master parameter
        // directly, so master-filter changes propagate to them with no extra listening.
        auto& vts = audioProcessor.getValueTreeState();
        vts.addParameterListener("modFilter1LinkToMaster", this);
        vts.addParameterListener("modFilter2LinkToMaster", this);
    }

    //==============================================================================
    // -- Waveform Menus --
    // Last, because every dropdown this fills and every attachment it reads the
    // selection from has to exist first -- and they are created in five different
    // sections of this constructor. The menus then follow whatever has been
    // imported and keep following it: the library calls back on every change.
    rebuildWaveformMenus();

    audioProcessor.getUserWaveLibrary().onChange = [this]
    {
        rebuildWaveformMenus();
    };

    //==============================================================================
    // -- Start Timer for LFO Rate Display Updates --
    startTimer(50);  // Update every 50ms for smooth rate display
    
    //==============================================================================
    // -- DEBUG: Editor Constructor End --
    DBG("Space Dust: Editor ctor END");
    
    try
    {
        DBG("Space Dust: Editor ctor - All components created");
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception in editor ctor: " + juce::String(e.what()));
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception in editor ctor");
    }
}

//==============================================================================
// -- Destructor --

//==============================================================================
// Standalone: keep the on-screen keyboard playable while clicking knobs. Whenever
// focus moves to a non-text control inside our editor, hand it straight back to the
// keyboard so the QWERTY computer keys keep working. (Knob drags use mouse capture,
// not keyboard focus, so the knob you grabbed still drags normally - true
// play-and-twist at the same time.) Registered only in the standalone build.
void SpaceDustAudioProcessorEditor::globalFocusChanged(juce::Component* focusedComponent)
{
    if (standaloneKeyboard == nullptr || focusedComponent == nullptr)
        return;
    if (focusedComponent == standaloneKeyboard.get())
        return;
    // Focus left our window (popup / other app) - don't fight it.
    if (focusedComponent != this && ! isParentOf(focusedComponent))
        return;
    if (dynamic_cast<juce::TextEditor*>(focusedComponent) != nullptr)  // user is typing - leave it
        return;
    // The CheezeGuy easter-egg game needs the arrow keys, so let it keep focus.
    if (cheezeGuyGame != nullptr
        && (focusedComponent == cheezeGuyGame.get() || cheezeGuyGame->isParentOf(focusedComponent)))
        return;
    standaloneKeyboard->grabKeyboardFocus();
}

//==============================================================================
void SpaceDustAudioProcessorEditor::rebuildWaveformMenus()
{
    auto& library = audioProcessor.getUserWaveLibrary();

    // The built-in entries, in the order the parameter declares them. Their ids
    // are their parameter index plus one, exactly like the User slots. Taken from
    // the menu's own current contents rather than written out again here: the
    // built-ins were added when the box was created and never change, so reading
    // them back is one fewer list that could fall out of step with the parameter.
    struct MenuToBuild
    {
        juce::ComboBox* combo;
        WaveformChoiceAttachment* attachment;
        int userBase;
        UserWave::Group group;
    };

    const MenuToBuild menus[] =
    {
        { &osc1WaveformCombo,   osc1WaveformAttachment.get(),   UserWave::oscUserBase,       UserWave::Group::Osc1 },
        { &osc2WaveformCombo,   osc2WaveformAttachment.get(),   UserWave::oscUserBase,       UserWave::Group::Osc2 },
        { &subOscWaveformCombo, subOscWaveformAttachment.get(), UserWave::oscUserBase,       UserWave::Group::Sub },
        { &noiseColorCombo,     noiseColorAttachment.get(),     UserWave::noiseUserBase,     UserWave::Group::Noise },
        { &transientTypeCombo,  transientTypeAttachment.get(),  UserWave::transientUserBase, UserWave::Group::Transient },
    };

    for (const auto& menu : menus)
    {
        if (menu.combo == nullptr || menu.attachment == nullptr)
            continue;

        // Read the selection from the PARAMETER, not from the box. The box is
        // about to be emptied, and its own selection may point at a slot that has
        // since been cleared and so has no entry left to report.
        const int selectedIndex = menu.attachment->currentIndex();

        // Keep the built-in names before emptying the box. They were put there
        // when the box was created and never change, so carrying them over is
        // one fewer copy of the list to fall out of step with the parameter.
        juce::StringArray builtIns;

        for (int i = 0; i < menu.userBase; ++i)
        {
            const int index = menu.combo->indexOfItemId(i + 1);
            juce::String text = index >= 0 ? menu.combo->getItemText(index) : juce::String();

            // Cannot happen: the box is filled with its built-ins when it is
            // created, and every rebuild puts them straight back. Named anyway,
            // because a ComboBox refuses an item with no name at all.
            jassert(text.isNotEmpty());

            if (text.isEmpty())
                text = "Item " + juce::String(i + 1);

            builtIns.add(text);
        }

        menu.combo->clear(juce::dontSendNotification);

        for (int i = 0; i < menu.userBase; ++i)
            menu.combo->addItem(builtIns[i], i + 1);

        for (int slot = 0; slot < UserWave::numSlots; ++slot)
        {
            const int id = menu.userBase + slot + 1;

            if (library.bank().slot(menu.group, slot).isPlayable())
                menu.combo->addItem(library.choiceNameForSlot(menu.group, slot), id);
        }

        //======================================================================
        // Where the parameter points, and whether the menu has an entry for it.
        //
        // It may not: a song can be opened whose imported waveforms did not
        // travel with it -- a Full Sample slot whose copy is gone from the
        // Wavetables folder, or a preset written on another machine.
        //
        // That case used to add an entry reading "User n (missing)", on the
        // grounds that it kept the state visible and recoverable. That reasoning
        // does not survive contact with what is actually heard: a choice
        // pointing at an empty slot resolves to a null slot, and
        // SynthVoice::generateWaveform falls through to `default: sin(angle)`.
        // So the oscillator was ALREADY playing a plain sine while the menu
        // announced a waveform that was missing -- the state was neither
        // preserved nor recoverable, only mislabelled.
        //
        // Stepping back to the nearest entry that does exist at least makes the
        // menu agree with the sound. Item id 1 is always a built-in and always
        // present, so the search below cannot fall off the end.
        int idToSelect = selectedIndex + 1;
        const bool haveEntry = menu.combo->indexOfItemId(idToSelect) >= 0;

        if (! haveEntry)
        {
            for (int id = idToSelect - 1; id >= 1; --id)
            {
                if (menu.combo->indexOfItemId(id) >= 0)
                {
                    idToSelect = id;
                    break;
                }
            }
        }

        // sendNotificationSync only when the selection had to MOVE, so the
        // parameter follows the menu. When it did not, dontSendNotification:
        // writing back what the parameter already says would be a redundant
        // gesture in the host's undo history.
        menu.combo->setSelectedId(idToSelect, haveEntry ? juce::dontSendNotification
                                                        : juce::sendNotificationSync);
    }

    if (waveformWindow != nullptr)
        waveformWindow->refreshContent();
}

void SpaceDustAudioProcessorEditor::attachComboSteppers(juce::Component& root)
{
    // Gathered first, attached second. A stepper adds itself to its combo's
    // parent, so attaching during the walk would grow the very child list being
    // walked.
    std::vector<juce::ComboBox*> combos;

    const std::function<void(juce::Component&)> gather = [&](juce::Component& parent)
    {
        for (auto* child : parent.getChildren())
        {
            if (auto* combo = dynamic_cast<juce::ComboBox*>(child))
            {
                // Not into a combo: its own children are the text label and the
                // popup's parts, and neither wants arrows of its own.
                combos.push_back(combo);
                continue;
            }

            gather(*child);
        }
    };

    gather(root);

    comboSteppers.reserve(comboSteppers.size() + combos.size());

    for (auto* combo : combos)
    {
        // Once each. The showing page is reached BOTH by its own walk and by the
        // walk of the editor that contains it, and a second stepper on one box
        // would sit exactly on the first and step it twice per click.
        if (std::find(steppedCombos.begin(), steppedCombos.end(), combo) != steppedCombos.end())
            continue;

        auto stepper = std::make_unique<ComboStepper>();
        stepper->attachTo(*combo);
        comboSteppers.push_back(std::move(stepper));
        steppedCombos.push_back(combo);
    }

}

void SpaceDustAudioProcessorEditor::openWaveformWindow(juce::Component* anchorButton,
                                                      juce::ComboBox* combo, int userBase,
                                                      WaveformEditorComponent::BuiltInKind kind,
                                                      UserWave::Group group)
{
    auto& library = audioProcessor.getUserWaveLibrary();

    if (waveformWindow == nullptr)
    {
        waveformWindow = std::make_unique<WaveformEditorPanel>(library, customLookAndFeel);

        // Parented to mainView, which lays the whole plugin out in design
        // coordinates and carries its single scale transform -- so the panel
        // scales with the window and may float over the tab bar.
        mainView.addChildComponent(*waveformWindow);

        // The synth the panel resamples. Safe to hand it `this`: the panel is
        // owned by this editor and is destroyed with it.
        waveformWindow->setResampleHost(this);
    }

    // Keep the panel clear of the standalone's keyboard strip. mainView is taller
    // than the controls by exactly that strip, and a panel clamped to mainView
    // could otherwise have its foot -- the shaping knobs -- pushed among the keys.
    waveformWindow->setKeepAboveBottom(designHeight_
                                       - (standaloneKeyboard != nullptr ? standaloneKeyboardHeight : 0));

    // The panel drives this dropdown directly, so its list and this menu are the
    // same list and cannot drift apart. -1 means "do not move the selection": the
    // dropdown is on a built-in shape, and opening a panel must never change the
    // sound by itself.
    const int slot = combo->getSelectedId() - 1 - userBase;

    // The three oscillators have shaping and unison. The noise source has unison
    // ONLY -- its ShapingControls carries the three unison knobs and leaves the
    // five shaping ones null, and the panel draws the Unison box on its own when
    // it finds them missing. Bend and Sync move a position in a cycle, and
    // built-in noise has no cycle to have a position in.
    //
    // The Transient has neither, so it gets null and no strip at all.
    const WaveformEditorComponent::ShapingControls* shaping = nullptr;

    if (group == UserWave::Group::Osc1)
        shaping = &osc1ShapingControls;
    else if (group == UserWave::Group::Osc2)
        shaping = &osc2ShapingControls;
    else if (group == UserWave::Group::Sub)
        shaping = &subOscShapingControls;
    else if (group == UserWave::Group::Noise)
        shaping = &noiseShapingControls;

    waveformWindow->showFor(anchorButton, combo, userBase, kind, group,
                            (slot >= 0 && slot < UserWave::numSlots) ? slot : -1,
                            shaping);
}

//==============================================================================
bool SpaceDustAudioProcessorEditor::startCapture(juce::String& errorMessage)
{
    if (audioProcessor.startResampleRecording())
        return true;

    errorMessage = "The synth cannot record just now. Try again in a moment.";
    return false;
}

bool SpaceDustAudioProcessorEditor::captureIsRunning() const
{
    return audioProcessor.isResampleRecording();
}

float SpaceDustAudioProcessorEditor::captureProgress() const
{
    return audioProcessor.getResampleProgress();
}

void SpaceDustAudioProcessorEditor::abandonCapture()
{
    audioProcessor.cancelResampleRecording();
}

float SpaceDustAudioProcessorEditor::playbackPhase(UserWave::Group group) const
{
    return audioProcessor.getUserWavePhase(group);
}

void SpaceDustAudioProcessorEditor::setPlaybackPhaseWanted(bool wanted)
{
    audioProcessor.setUserWavePhaseWanted(wanted);
}

bool SpaceDustAudioProcessorEditor::collectCapture(WaveformEditorComponent::Capture& capture,
                                                   juce::String& errorMessage)
{
    if (!audioProcessor.takeResampleRecording(capture.mono, capture.right, capture.sampleRate, capture.cutShort))
    {
        errorMessage = "Middle C made no sound. Check the levels and the waveform.";
        return false;
    }

    // Measured HERE, before the library normalises it. It is the one thing that
    // normalisation destroys and Resample + Init has to put back.
    float peak = 0.0f;
    for (float value : capture.mono)
        peak = juce::jmax(peak, std::abs(value));

    capture.peak = peak;

    // Named after the patch it came out of, because that is what it is. A patch
    // with no name of its own gives the plain word.
    const auto preset = audioProcessor.currentPresetName.trim();
    capture.name = (preset.isEmpty() || preset == "Init") ? juce::String("Resample") : preset;

    return true;
}

void SpaceDustAudioProcessorEditor::setParameterValue(const juce::String& parameterID, float value)
{
    auto* param = audioProcessor.getValueTreeState().getParameter(parameterID);

    if (param == nullptr)
    {
        jassertfalse;   // a renamed or removed parameter, caught at the first press
        return;
    }

    const auto& range = param->getNormalisableRange();

    // Balanced gesture, as everywhere else in this editor: a burst of naked
    // setValueNotifyingHost calls corrupts FL Studio's "Last Tweaked" tracking,
    // which breaks automation created afterwards (see PresetManager::loadInitPreset).
    param->beginChangeGesture();
    param->setValueNotifyingHost(range.convertTo0to1(juce::jlimit(range.start, range.end, value)));
    param->endChangeGesture();
}

namespace
{
    /** How much of one source reaches the output when its own level is full.

        A voice pans with a constant-power law, so a source in the middle arrives
        at 1/sqrt(2) on each side -- and the mono sum the capture keeps is that
        same 1/sqrt(2). The noise source carries a further 0.75 of its own. Both
        are read straight off SynthVoice's mixing step; if that changes, this does.

        Resample + Init divides the captured peak by this before it sets the
        master volume, so the sound comes back at the level it was taken at rather
        than 3 dB under it. */
    float sourcePlaybackGain(UserWave::Group group)
    {
        constexpr float centreGain = 0.70710678f;   // SynthVoice's centerGain

        switch (group)
        {
            case UserWave::Group::Noise:     return centreGain * 0.75f;

            // Summed into the master chain rather than panned by a voice, so it
            // arrives whole.
            case UserWave::Group::Transient: return 1.0f;

            case UserWave::Group::Osc1:
            case UserWave::Group::Osc2:
            case UserWave::Group::Sub:
            default:                         return centreGain;
        }
    }
}

void SpaceDustAudioProcessorEditor::initialiseAroundWaveform(UserWave::Group group,
                                                             int choiceIndex, float peak)
{
    // Everything back to its default first. Every effect in this synth is off by
    // default, so this single call IS "no effects chain" -- and it will still be,
    // for effects added long after this was written.
    presetManager->loadInitPreset();

    // Two things Initialize Preset does are deliberately NOT done here. The
    // imported waveforms are not cleared -- a blank patch should not carry the
    // last session's samples, but the whole point of this button is the waveform
    // made a moment ago. And the Cheeze Guy tab is left where it is: it is not
    // part of a patch, so stripping a patch back has no business taking it away.

    // -- The one source that is to be heard, and the waveform it plays --
    setParameterValue("osc1Level",  group == UserWave::Group::Osc1 ? 1.0f : 0.0f);
    setParameterValue("osc2Level",  group == UserWave::Group::Osc2 ? 1.0f : 0.0f);
    setParameterValue("noiseLevel", group == UserWave::Group::Noise ? 1.0f : 0.0f);
    setParameterValue("subOscOn",   group == UserWave::Group::Sub ? 1.0f : 0.0f);

    if (group == UserWave::Group::Sub)
        setParameterValue("subOscLevel", 1.0f);

    setParameterValue("transientEnabled", group == UserWave::Group::Transient ? 1.0f : 0.0f);

    if (group == UserWave::Group::Transient)
        setParameterValue("transientMix", 1.0f);

    switch (group)
    {
        case UserWave::Group::Osc1:      setParameterValue("osc1Waveform",   (float) choiceIndex); break;
        case UserWave::Group::Osc2:      setParameterValue("osc2Waveform",   (float) choiceIndex); break;
        case UserWave::Group::Sub:       setParameterValue("subOscWaveform", (float) choiceIndex); break;
        case UserWave::Group::Noise:     setParameterValue("noiseType",      (float) choiceIndex); break;
        case UserWave::Group::Transient: setParameterValue("transientType",  (float) choiceIndex); break;
    }

    // -- Nothing left to shape it --
    // The filter cannot be taken out of the path, so it is put where it does the
    // least: fully open, no resonance, and no envelope pointed at it.
    setParameterValue("filterCutoff", 20000.0f);
    setParameterValue("filterResonance", 0.0f);
    setParameterValue("filterEnvAmount", 0.0f);

    // The amplitude envelope is already inside the sample -- its attack, its decay
    // and its tail were all recorded -- so a second envelope on top would shape the
    // same sound twice. This is as close to a plain gate as the ranges allow: ten
    // milliseconds in and ten out, short enough not to be heard as a shape and long
    // enough not to click.
    setParameterValue("envAttack", 0.01f);
    setParameterValue("envDecay", 0.01f);
    setParameterValue("envSustain", 1.0f);
    setParameterValue("envRelease", 0.01f);

    // And the level it was recorded at -- but never above 0 dBFS.
    //
    // The library normalises every waveform it stores, so without this the sound
    // would come back at whatever its loudest moment happened to be rather than
    // where the player left it. The cap is what stops that being a licence to
    // clip: a patch already running hot -- and one built by this very button is,
    // because it sets the master to the level it found -- would be handed a master
    // volume that puts its peaks over full scale. Everything downstream then
    // squares them off, and resampling THAT gives a flatter waveform again, so
    // each pass through the button is more squashed than the last
    // (Giuseppe, 2026-08-13).
    //
    // So the loudest point of the resampled patch sits exactly at full scale, and
    // a patch that was quieter than that keeps its own level.
    const float gain = sourcePlaybackGain(group);
    const float wanted = juce::jmin(peak, 1.0f);
    setParameterValue("masterVolume", gain > 0.0f ? wanted / gain : wanted);

    // The patch is no longer the preset it was built from, and says so.
    audioProcessor.currentPresetName = "Init";
    presetCombo.setSelectedId(0, juce::dontSendNotification);
    presetCombo.setTextWhenNothingSelected("Init");

    audioProcessor.updateVoicesWithParameters();
}

//==============================================================================
SpaceDustAudioProcessorEditor::~SpaceDustAudioProcessorEditor()
{
    DBG("Space Dust: Processor destructor START");

    isBeingDestroyed.store(true);
    stopTimer();

    // Stop receiving focus-change callbacks before teardown (standalone only;
    // harmless if it was never registered).
    juce::Desktop::getInstance().removeFocusChangeListener(this);

    // Assign mode: stop listening before anything is torn down. The wrappers
    // deregister themselves in their own destructors -- see the note beside
    // modKnobs, which is declared last so it dies first.
    assignMode.removeChangeListener(this);

    // Easter egg cleanup
    cheezeGuyGame.reset();

    // The library outlives this editor -- it belongs to the processor -- and its
    // change callback captures this. Clear it before anything else is torn down,
    // or a later import would call into a half-destroyed editor.
    audioProcessor.getUserWaveLibrary().onChange = nullptr;
    waveformWindow.reset();

    // Remove APVTS filter sync listeners
    {
        auto& vts = audioProcessor.getValueTreeState();
        vts.removeParameterListener("modFilter1LinkToMaster", this);
        vts.removeParameterListener("modFilter2LinkToMaster", this);
    }

    // Remove all listeners first
    delayEnabledButton.removeListener(this);
    phaserEnabledButton.removeListener(this);
    flangerEnabledButton.removeListener(this);
    bitCrusherEnabledButton.removeListener(this);
    softClipperEnabledButton.removeListener(this);
    compressorEnabledButton.removeListener(this);
    lofiEnabledButton.removeListener(this);
    transientEnabledButton.removeListener(this);
    reverbEnabledButton.removeListener(this);
    tranceGateEnabledButton.removeListener(this);
    grainDelayEnabledButton.removeListener(this);
    lfo1EnabledButton.removeListener(this);
    lfo2EnabledButton.removeListener(this);
    finalEQEnabledButton.removeListener(this);
    pitchBendSlider.removeListener(this);
    modFilter1CutoffSlider.removeListener(this);
    modFilter1ResonanceSlider.removeListener(this);
    modFilter2CutoffSlider.removeListener(this);
    modFilter2ResonanceSlider.removeListener(this);
    warmSaturationMod1Button.removeListener(this);
    warmSaturationMod2Button.removeListener(this);
    if (lfo1SyncRateListener)
        lfo1SyncRateCombo.removeListener(lfo1SyncRateListener.get());
    if (lfo2SyncRateListener)
        lfo2SyncRateCombo.removeListener(lfo2SyncRateListener.get());
    if (delaySyncRateListener)
        delaySyncRateCombo.removeListener(delaySyncRateListener.get());
    
    // Clear LookAndFeel from labels we explicitly assigned (before customLookAndFeel is invalidated)
    osc1CoarseTuneLabel.setLookAndFeel(nullptr);
    osc2CoarseTuneLabel.setLookAndFeel(nullptr);
    filterCutoffLabel.setLookAndFeel(nullptr);
    filterResonanceLabel.setLookAndFeel(nullptr);
    envSustainLabel.setLookAndFeel(nullptr);
    envReleaseLabel.setLookAndFeel(nullptr);
    filterEnvReleaseLabel.setLookAndFeel(nullptr);
    pitchEnvPitchLabel.setLookAndFeel(nullptr);
    pitchBendLabel.setLookAndFeel(nullptr);
    subOscCoarseLabel.setLookAndFeel(nullptr);
    lfo1PhaseLabel.setLookAndFeel(nullptr);
    lfo2PhaseLabel.setLookAndFeel(nullptr);
    lfo1TargetLabel.setLookAndFeel(nullptr);
    lfo2TargetLabel.setLookAndFeel(nullptr);
    grainDelayDensityLabel.setLookAndFeel(nullptr);
    phaserStagesLabel.setLookAndFeel(nullptr);
    compressorThresholdLabel.setLookAndFeel(nullptr);
    modFilter1ResonanceLabel.setLookAndFeel(nullptr);
    modFilter2ResonanceLabel.setLookAndFeel(nullptr);
    compressorReleaseLabel.setLookAndFeel(nullptr);

    // Clear LookAndFeel on page components and editor before destruction
    if (mainPage) mainPage->setLookAndFeel(nullptr);
    if (modulationPage) modulationPage->setLookAndFeel(nullptr);
    if (effectsPage) effectsPage->setLookAndFeel(nullptr);
    if (saturationColorPage) saturationColorPage->setLookAndFeel(nullptr);
    if (spectralPage) spectralPage->setLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
    
    // Clear page components before tabbed component
    modulationPage.reset();
    mainPage.reset();
    effectsPage.reset();
    saturationColorPage.reset();
    spectralPage.reset();
    
    DBG("Space Dust: Processor destructor END");
}

//==============================================================================
// -- Safe Parameter Access (post-2026 Ableton hardening) --

float SpaceDustAudioProcessorEditor::safeGetParam(const juce::String& paramID, float fallback) const noexcept
{
    if (auto* v = audioProcessor.getValueTreeState().getRawParameterValue(paramID))
        return v->load();
    // Parameter missing (bad state, version mismatch, or restore in progress).
    // Return fallback instead of crashing the message thread.
    return fallback;
}

//==============================================================================
// -- Preset Management Helpers --

void SpaceDustAudioProcessorEditor::refreshPresetList()
{
    presetCombo.clear(juce::dontSendNotification);
    auto presets = presetManager->getAvailablePresets();
    for (int i = 0; i < presets.size(); ++i)
        presetCombo.addItem(presets[i].getFileNameWithoutExtension(), i + 1);

    // Try to select current preset by name
    auto currentName = presetManager->getCurrentPresetName();
    for (int i = 0; i < presets.size(); ++i)
    {
        if (presets[i].getFileNameWithoutExtension() == currentName)
        {
            presetCombo.setSelectedId(i + 1, juce::dontSendNotification);
            return;
        }
    }
    presetCombo.setTextWhenNothingSelected(currentName);
}

void SpaceDustAudioProcessorEditor::showSavePresetDialog()
{
    auto currentName = presetManager->getCurrentPresetName();
    auto* alertWindow = new juce::AlertWindow("Save Preset",
        "Enter a name for this preset:",
        juce::AlertWindow::NoIcon, this);
    alertWindow->addTextEditor("presetName", currentName, "Preset Name:");
    alertWindow->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alertWindow->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    alertWindow->enterModalState(true, juce::ModalCallbackFunction::create(
        [this, alertWindow](int result)
        {
            if (result == 1)
            {
                auto name = alertWindow->getTextEditorContents("presetName").trim();
                if (name.isNotEmpty())
                {
                    presetManager->savePreset(name);
                    audioProcessor.currentPresetName = presetManager->getCurrentPresetName();
                    refreshPresetList();
                }
            }
            delete alertWindow;
        }), false);
}

//==============================================================================
// -- APVTS Listener: master / mod filter "Link to Master" (host-safe sync) --

void SpaceDustAudioProcessorEditor::parameterChanged(const juce::String& parameterID, float newValue)
{
    auto safeThis = juce::Component::SafePointer<SpaceDustAudioProcessorEditor>(this);
    juce::MessageManager::callAsync([safeThis, parameterID, newValue]()
    {
        if (safeThis == nullptr || safeThis->isBeingDestroyed.load()) return;
        safeThis->syncLinkedFilterParams(parameterID, newValue);
    });
}

void SpaceDustAudioProcessorEditor::rebuildLinkedFilterAttachments()
{
    auto& vts = audioProcessor.getValueTreeState();
    const bool link1 = safeGetParam("modFilter1LinkToMaster") > 0.5f;
    const bool link2 = safeGetParam("modFilter2LinkToMaster") > 0.5f;

    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;

    auto setSlider = [&](std::unique_ptr<SliderAtt>& att, juce::Slider& s, const juce::String& id)
    { att.reset(); att = std::make_unique<SliderAtt>(vts, id, s); };
    auto setCombo = [&](std::unique_ptr<ComboAtt>& att, juce::ComboBox& c, const juce::String& id)
    { att.reset(); att = std::make_unique<ComboAtt>(vts, id, c); };
    auto setButton = [&](std::unique_ptr<ButtonAtt>& att, juce::Button& b, const juce::String& id)
    { att.reset(); att = std::make_unique<ButtonAtt>(vts, id, b); };

    // Linked  -> attach to the MASTER filter params, so the linked filter is literally the same
    //            automatable parameter as the master (one automation lane moves both knobs;
    //            moving either knob edits the same param).
    // Unlinked -> attach to the filter's OWN params, for fully independent automation.
    // Recreating an attachment only reads the target param to set the widget; it never writes a
    // param, so this is safe to call any time and never re-enters the host's automation engine.
    setCombo (modFilter1ModeAttachment,      modFilter1ModeCombo,       link1 ? "filterMode"           : "modFilter1Mode");
    setSlider(modFilter1CutoffAttachment,    modFilter1CutoffSlider,    link1 ? "filterCutoff"         : "modFilter1Cutoff");
    setSlider(modFilter1ResonanceAttachment, modFilter1ResonanceSlider, link1 ? "filterResonance"      : "modFilter1Resonance");
    setButton(warmSaturationMod1Attachment,  warmSaturationMod1Button,  link1 ? "warmSaturationMaster" : "warmSaturationMod1");
    setButton(modFilter1KeyTrackAttachment,  modFilter1KeyTrackButton,  link1 ? "filterKeyTrack"       : "modFilter1KeyTrack");
    setButton(modFilter1NoteLockAttachment,  modFilter1NoteLockButton,  link1 ? "filterNoteLock"       : "modFilter1NoteLock");
    setButton(modFilter1HarmonicLockAttachment, modFilter1HarmonicLockButton, link1 ? "filterHarmonicLock" : "modFilter1HarmonicLock");

    setCombo (modFilter2ModeAttachment,      modFilter2ModeCombo,       link2 ? "filterMode"           : "modFilter2Mode");
    setSlider(modFilter2CutoffAttachment,    modFilter2CutoffSlider,    link2 ? "filterCutoff"         : "modFilter2Cutoff");
    setSlider(modFilter2ResonanceAttachment, modFilter2ResonanceSlider, link2 ? "filterResonance"      : "modFilter2Resonance");
    setButton(warmSaturationMod2Attachment,  warmSaturationMod2Button,  link2 ? "warmSaturationMaster" : "warmSaturationMod2");
    setButton(modFilter2KeyTrackAttachment,  modFilter2KeyTrackButton,  link2 ? "filterKeyTrack"       : "modFilter2KeyTrack");
    setButton(modFilter2NoteLockAttachment,  modFilter2NoteLockButton,  link2 ? "filterNoteLock"       : "modFilter2NoteLock");
    setButton(modFilter2HarmonicLockAttachment, modFilter2HarmonicLockButton, link2 ? "filterHarmonicLock" : "modFilter2HarmonicLock");

    // The wrapper follows the attachment. Assigning an LFO to a LINKED mod
    // filter's cutoff must reach the master cutoff -- the same parameter the
    // knob itself is now editing -- or the knob would move under a modulation
    // that was written to a parameter nothing is reading.
    // Null on the first call: this runs from the constructor before the knobs
    // are wrapped, and the wrap then picks up whatever is current.
    if (modFilter1CutoffModKnob != nullptr)
        modFilter1CutoffModKnob->setDestination(link1 ? "filterCutoff" : "modFilter1Cutoff");
    if (modFilter1ResonanceModKnob != nullptr)
        modFilter1ResonanceModKnob->setDestination(link1 ? "filterResonance" : "modFilter1Resonance");
    if (modFilter2CutoffModKnob != nullptr)
        modFilter2CutoffModKnob->setDestination(link2 ? "filterCutoff" : "modFilter2Cutoff");
    if (modFilter2ResonanceModKnob != nullptr)
        modFilter2ResonanceModKnob->setDestination(link2 ? "filterResonance" : "modFilter2Resonance");
}

//==============================================================================
// -- Assign mode --
//
// One ModulatableKnob is laid over each assignable knob. It is transparent to
// the mouse until assign mode is on, so every knob behaves exactly as it always
// did; while the mode is on it takes the drag instead and writes a routing.

ModulatableKnob* SpaceDustAudioProcessorEditor::wrapKnob(juce::Slider& slider,
                                                         const juce::String& parameterId)
{
    auto* param = audioProcessor.getValueTreeState().getParameter(parameterId);
    auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param);

    // A parameter that does not exist is a TYPO in the list below, and it is the
    // one failure this whole feature is prone to: there are about 150 of these
    // calls, and a mistyped id used to be skipped in silence and ship as a knob
    // that simply never lights up -- no error, no crash, nothing in any log.
    // So it stops the build's first run instead.
    if (ranged == nullptr)
    {
        DBG("Space Dust: wrapKnob - NO SUCH PARAMETER: " + parameterId);
        jassertfalse;   // <- read the id above; it is misspelled in wrapAssignableKnobs()
        return nullptr;
    }

    // A parameter that exists but may not be modulated is the normal case, not a
    // mistake: Unison Voices is a count, the toggles are bools, and no LFO
    // control may drive another. Those are passed over without a word, which is
    // what lets the list below name a knob without checking what kind it is.
    if (! spacedust::DestinationTable::isLegalDestination(*ranged))
        return nullptr;

    auto* wrapper = modKnobs.add(new ModulatableKnob(
        slider,
        audioProcessor.modMatrix,
        assignMode,
        [this] { audioProcessor.rebuildCompiledRoutings(); }));

    wrapper->setDestination(parameterId.toStdString());
    return wrapper;
}

void SpaceDustAudioProcessorEditor::wrapAssignableKnobs()
{
    // wrapKnob passes over anything that is not a legal destination, so this
    // list may name a knob without knowing what kind of parameter drives it.
    // What it may NOT do is misspell one -- see the assert in wrapKnob.

    // -- Oscillators, sub and noise --
    wrapKnob(osc1CoarseTuneSlider, "osc1CoarseTune");
    wrapKnob(osc1DetuneSlider,     "osc1Detune");
    wrapKnob(osc1LevelSlider,      "osc1Level");
    wrapKnob(osc1PanSlider,        "osc1Pan");
    wrapKnob(osc2CoarseTuneSlider, "osc2CoarseTune");
    wrapKnob(osc2DetuneSlider,     "osc2Detune");
    wrapKnob(osc2LevelSlider,      "osc2Level");
    wrapKnob(osc2PanSlider,        "osc2Pan");
    wrapKnob(subOscLevelSlider,    "subOscLevel");
    wrapKnob(subOscCoarseSlider,   "subOscCoarse");
    wrapKnob(noiseLevelSlider,     "noiseLevel");
    wrapKnob(lowShelfAmountSlider,  "lowShelfAmount");
    wrapKnob(highShelfAmountSlider, "highShelfAmount");

    // -- Shaping and unison, from the one id table at the top of this file --
    // The same table the attachments were built from, so a knob cannot be
    // attached to one parameter and assigned to another.
    for (int i = 0; i < numShapingKnobs; ++i)
    {
        wrapKnob(osc1ShapingSliders[i],   kOsc1ShapingIds[i]);
        wrapKnob(osc2ShapingSliders[i],   kOsc2ShapingIds[i]);
        wrapKnob(subOscShapingSliders[i], kSubOscShapingIds[i]);
    }

    for (int i = 0; i < numUnisonKnobs; ++i)
    {
        wrapKnob(osc1UnisonSliders[i],   kOsc1UnisonIds[i]);
        wrapKnob(osc2UnisonSliders[i],   kOsc2UnisonIds[i]);
        wrapKnob(subOscUnisonSliders[i], kSubOscUnisonIds[i]);
        wrapKnob(noiseUnisonSliders[i],  kNoiseUnisonIds[i]);
    }

    // -- Master filter and its envelope --
    wrapKnob(filterCutoffSlider,     "filterCutoff");
    wrapKnob(filterResonanceSlider,  "filterResonance");
    wrapKnob(filterEnvAttackSlider,  "filterEnvAttack");
    wrapKnob(filterEnvDecaySlider,   "filterEnvDecay");
    wrapKnob(filterEnvSustainSlider, "filterEnvSustain");
    wrapKnob(filterEnvReleaseSlider, "filterEnvRelease");
    wrapKnob(filterEnvAmountSlider,  "filterEnvAmount");

    // -- Amp and pitch envelopes --
    wrapKnob(envAttackSlider,      "envAttack");
    wrapKnob(envDecaySlider,       "envDecay");
    wrapKnob(envSustainSlider,     "envSustain");
    wrapKnob(envReleaseSlider,     "envRelease");
    wrapKnob(pitchEnvAmountSlider, "pitchEnvAmount");
    wrapKnob(pitchEnvTimeSlider,   "pitchEnvTime");
    wrapKnob(pitchEnvPitchSlider,  "pitchEnvPitch");

    // -- Master section --
    // pitchBendSlider is deliberately absent: the host drives that one, and
    // isLegalDestination refuses it.
    wrapKnob(masterVolumeSlider,    "masterVolume");
    wrapKnob(pitchBendAmountSlider, "pitchBendAmount");
    wrapKnob(velocityAmountSlider,  "velocityAmount");
    wrapKnob(glideTimeSlider,       "glideTime");

    // -- MPE --
    wrapKnob(mpePitchBendRangeSlider, "mpePitchBendRange");
    wrapKnob(mpePressureDepthSlider,  "mpePressureDepth");
    wrapKnob(mpeTimbreDepthSlider,    "mpeTimbreDepth");

    // -- The two modulation-page filters --
    // Their destination is not fixed: it follows Link to Master, exactly as
    // their attachments do. rebuildLinkedFilterAttachments re-points them.
    const bool link1 = safeGetParam("modFilter1LinkToMaster") > 0.5f;
    const bool link2 = safeGetParam("modFilter2LinkToMaster") > 0.5f;

    modFilter1CutoffModKnob    = wrapKnob(modFilter1CutoffSlider,    link1 ? "filterCutoff"    : "modFilter1Cutoff");
    modFilter1ResonanceModKnob = wrapKnob(modFilter1ResonanceSlider, link1 ? "filterResonance" : "modFilter1Resonance");
    modFilter2CutoffModKnob    = wrapKnob(modFilter2CutoffSlider,    link2 ? "filterCutoff"    : "modFilter2Cutoff");
    modFilter2ResonanceModKnob = wrapKnob(modFilter2ResonanceSlider, link2 ? "filterResonance" : "modFilter2Resonance");

    // -- Delay --
    wrapKnob(delayFreeRateSlider,           "delayRate");
    wrapKnob(delayDecaySlider,              "delayDecay");
    wrapKnob(delayDryWetSlider,             "delayDryWet");
    wrapKnob(delayFilterHPCutoffSlider,     "delayFilterHPCutoff");
    wrapKnob(delayFilterHPResonanceSlider,  "delayFilterHPResonance");
    wrapKnob(delayFilterLPCutoffSlider,     "delayFilterLPCutoff");
    wrapKnob(delayFilterLPResonanceSlider,  "delayFilterLPResonance");

    // -- Reverb --
    wrapKnob(reverbWetMixSlider,             "reverbWetMix");
    wrapKnob(reverbDecayTimeSlider,          "reverbDecayTime");
    wrapKnob(reverbFilterHPCutoffSlider,     "reverbFilterHPCutoff");
    wrapKnob(reverbFilterHPResonanceSlider,  "reverbFilterHPResonance");
    wrapKnob(reverbFilterLPCutoffSlider,     "reverbFilterLPCutoff");
    wrapKnob(reverbFilterLPResonanceSlider,  "reverbFilterLPResonance");

    // -- Grain delay --
    wrapKnob(grainDelayTimeSlider,               "grainDelayTime");
    wrapKnob(grainDelaySizeSlider,               "grainDelaySize");
    wrapKnob(grainDelayPitchSlider,              "grainDelayPitch");
    wrapKnob(grainDelayMixSlider,                "grainDelayMix");
    wrapKnob(grainDelayDecaySlider,              "grainDelayDecay");
    wrapKnob(grainDelayDensitySlider,            "grainDelayDensity");
    wrapKnob(grainDelayJitterSlider,             "grainDelayJitter");
    wrapKnob(grainDelayFilterHPCutoffSlider,     "grainDelayFilterHPCutoff");
    wrapKnob(grainDelayFilterHPResonanceSlider,  "grainDelayFilterHPResonance");
    wrapKnob(grainDelayFilterLPCutoffSlider,     "grainDelayFilterLPCutoff");
    wrapKnob(grainDelayFilterLPResonanceSlider,  "grainDelayFilterLPResonance");

    // -- Phaser --
    wrapKnob(phaserRateSlider,          "phaserRate");
    wrapKnob(phaserDepthSlider,         "phaserDepth");
    wrapKnob(phaserFeedbackSlider,      "phaserFeedback");
    wrapKnob(phaserMixSlider,           "phaserMix");
    wrapKnob(phaserCentreSlider,        "phaserCentre");
    wrapKnob(phaserStereoOffsetSlider,  "phaserStereoOffset");

    // -- Flanger --
    wrapKnob(flangerRateSlider,     "flangerRate");
    wrapKnob(flangerDepthSlider,    "flangerDepth");
    wrapKnob(flangerFeedbackSlider, "flangerFeedback");
    wrapKnob(flangerWidthSlider,    "flangerWidth");
    wrapKnob(flangerMixSlider,      "flangerMix");

    // -- Bit crusher, soft clipper, lo-fi --
    wrapKnob(bitCrusherAmountSlider, "bitCrusherAmount");
    wrapKnob(bitCrusherRateSlider,   "bitCrusherRate");
    wrapKnob(bitCrusherMixSlider,    "bitCrusherMix");
    wrapKnob(softClipperDriveSlider, "softClipperDrive");
    wrapKnob(softClipperKneeSlider,  "softClipperKnee");
    wrapKnob(softClipperMixSlider,   "softClipperMix");
    wrapKnob(lofiAmountSlider,       "lofiAmount");
    wrapKnob(analogDriftSlider,      "analogDrift");

    // -- Compressor --
    wrapKnob(compressorThresholdSlider, "compressorThreshold");
    wrapKnob(compressorRatioSlider,     "compressorRatio");
    wrapKnob(compressorAttackSlider,    "compressorAttack");
    wrapKnob(compressorReleaseSlider,   "compressorRelease");
    wrapKnob(compressorMakeupSlider,    "compressorMakeup");
    wrapKnob(compressorMixSlider,       "compressorMix");

    // -- Transient --
    wrapKnob(transientMixSlider,     "transientMix");
    wrapKnob(transientKaDonkSlider,  "transientKaDonk");
    wrapKnob(transientCoarseSlider,  "transientCoarse");
    wrapKnob(transientLengthSlider,  "transientLength");

    // -- Trance gate --
    wrapKnob(tranceGateRateSlider,    "tranceGateRate");
    wrapKnob(tranceGateAttackSlider,  "tranceGateAttack");
    wrapKnob(tranceGateReleaseSlider, "tranceGateRelease");
    wrapKnob(tranceGateMixSlider,     "tranceGateMix");

    // -- Final EQ --
    // Three knobs edit whichever band the Node dropdown has chosen, so their
    // destination follows that choice. setFinalEQEditedBand re-points them, the
    // same call that rebuilds their attachments. The other four bands' twelve
    // parameters have no knob of their own at any one moment, which is why they
    // are the bulk of the unwrapped list logged below.
    {
        const juce::String n(finalEQEditedBand_ + 1);
        finalEQQModKnob    = wrapKnob(finalEQQSlider,    "finalEQB" + n + "Q");
        finalEQFreqModKnob = wrapKnob(finalEQFreqSlider, "finalEQB" + n + "Freq");
        finalEQGainModKnob = wrapKnob(finalEQGainSlider, "finalEQB" + n + "Gain");
    }

    // Every wrapper joins the parent its knob already has, and follows that knob
    // from then on -- including out of one parent and into another, because
    // attachToKnobParent runs again on every re-parent. That is what the
    // shaping and unison knobs need: they have NO parent at this point and only
    // get one when the Waveforms panel borrows them, so a one-shot attach here
    // would leave twenty-seven wrappers built and unreachable for good.
    //
    // One sweep at the end rather than inside wrapKnob, so the list above does
    // not have to care whether a page exists yet.
    for (auto* wrapper : modKnobs)
        wrapper->attachToKnobParent();
}

void SpaceDustAudioProcessorEditor::logModCoverage()
{
    // WHY THIS EXISTS
    //
    // wrapKnob passes over an illegal destination in silence, and the list above
    // is about 150 lines long. "Did I get them all?" is not a question care can
    // answer at that count -- so the answer is printed instead.
    //
    // TWO numbers, deliberately, because they are not the same and confusing
    // them is how this check lied once already. A wrapper that has been
    // CONSTRUCTED is an object in modKnobs; that is all. A wrapper that is
    // REACHABLE has a parent, so it is laid out, painted and hit-tested and the
    // player can actually point at that knob. An earlier version counted only
    // the first and reported full coverage while twenty-seven knobs -- every
    // Bend, Spectrum, Sync, Unison Detune, Width and Random Phase -- had no
    // parent at all and could not be assigned to. A reassuring number in place
    // of a missing knob is worse than no check.
    //
    // The reachable figure legitimately MOVES at runtime, and is expected to be
    // lower here than later on: the shaping and unison knobs have no parent
    // until the Waveforms panel borrows them, so they join the moment it is
    // first opened. This line is the count at editor-open. Anything in the
    // "no wrapper at all" list, by contrast, is a real gap and should have a
    // reason.
    std::unordered_set<std::string> constructed;
    std::unordered_set<std::string> reachable;

    for (auto* wrapper : modKnobs)
    {
        constructed.insert(wrapper->getDestination());

        if (wrapper->getParentComponent() != nullptr)
            reachable.insert(wrapper->getDestination());
    }

    const int total = audioProcessor.modDestinations.size();

    juce::StringArray noWrapper;      // nothing was ever built for these
    juce::StringArray notYetParented; // built, but off-screen for now

    for (int slot = 0; slot < total; ++slot)
    {
        const auto& id = audioProcessor.modDestinations.idAt(slot);

        if (constructed.find(id) == constructed.end())
            noWrapper.add(juce::String(id));
        else if (reachable.find(id) == reachable.end())
            notYetParented.add(juce::String(id));
    }

    #if JUCE_DEBUG
    for (const auto& id : noWrapper)
        DBG("Space Dust: mod coverage - NO WRAPPER AT ALL for destination: " + id);

    for (const auto& id : notYetParented)
        DBG("Space Dust: mod coverage - wrapper built but NOT PARENTED YET: " + id);
    #endif

    const auto summary =
        juce::String("Space Dust: mod coverage - constructed ")
        + juce::String((int) constructed.size()) + " of " + juce::String(total)
        + ", reachable now " + juce::String((int) reachable.size()) + " of " + juce::String(total)
        + "; no wrapper at all (" + juce::String(noWrapper.size()) + "): "
        + (noWrapper.isEmpty() ? juce::String("none") : noWrapper.joinIntoString(", "))
        + "; built but not parented yet (" + juce::String(notYetParented.size())
        + ", these join when the Waveforms panel is first opened): "
        + (notYetParented.isEmpty() ? juce::String("none") : notYetParented.joinIntoString(", "));

    DBG(summary);

    // Also to the debug log file the rest of this editor writes, so the list can
    // be read after the fact rather than only in a debugger.
    #if JUCE_DEBUG
    try
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeString("SpaceDust_DebugLog.txt"));
        juce::FileOutputStream out(logFile);
        if (out.openedOk())
        {
            out.setPosition(out.getPosition());
            out.writeText(summary + "\n", false, false, nullptr);
            out.flush();
        }
    }
    catch (...) {}
    #endif
}

void SpaceDustAudioProcessorEditor::refreshModIndicators()
{
    if (modKnobs.isEmpty())
        return;

    float phases[spacedust::numLfos] {};

    for (int i = 0; i < spacedust::numLfos; ++i)
        phases[i] = (float) juce::jlimit(0.0, 1.0, audioProcessor.lfoCurrentPhase[i]);

    // setLfoPhases repaints only the bar, and only for a knob something actually
    // reaches, so this costs nothing on a patch with no routings.
    for (auto* wrapper : modKnobs)
        wrapper->setLfoPhases(phases);
}

void SpaceDustAudioProcessorEditor::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source != &assignMode || isBeingDestroyed.load())
        return;

    const bool assigning = assignMode.activeLfo() >= 0;

    // NOTHING is hidden on any page while the mode is on.
    //
    // The rule this feature actually has is "an LFO may not modulate an LFO
    // control", and it is enforced where it belongs: isLegalDestination refuses
    // every id beginning with "lfo", so wrapKnob never builds a wrapper for one
    // and there is nothing on the Modulation page to hide.
    //
    // An earlier version suppressed every wrapper while that page was showing.
    // That is a cheap approximation of the rule and it is wrong: it also swept
    // up the two mod filters' cutoff and resonance and the three MPE depths,
    // which are ordinary float destinations, and left those seven with no way
    // to be given a first routing at all -- the indicator bar only takes a
    // click once a routing exists, so there was no other path in.

    for (int lfo = 0; lfo < lfoAssignButtons.size(); ++lfo)
        lfoAssignButtons[lfo]->setToggleState(assignMode.activeLfo() == lfo,
                                              juce::dontSendNotification);

    exitLfoModeButton.setVisible(assigning);
    exitLfoModeButton.setColour(juce::TextButton::buttonColourId,
                                spacedust::AssignModeState::colourFor(assignMode.activeLfo()));

    layoutPlate();
}

bool SpaceDustAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && assignMode.activeLfo() >= 0)
    {
        assignMode.setActiveLfo(-1);
        return true;
    }

    // Everything else falls through, so the Standalone's QWERTY keyboard keeps
    // every key it already had.
    return false;
}

//==============================================================================
// -- Note Lock --

std::optional<NoteLock::Grid> SpaceDustAudioProcessorEditor::activeNoteLockGrid(int filterIndex) const
{
    // Note Lock is only offered while Key Tracking is on -- that is what makes the
    // cutoff a ratio to the played note, and so what makes one grid mean "n steps
    // above the root" for every key. Checking it here as well as in the layout means
    // a preset that saved Note Lock on with Key Tracking off (or host automation
    // turning Key Tracking off under a hidden toggle) leaves the knob free rather
    // than silently quantised by a control you cannot see. Harmonic Series is nested
    // the same way inside Note Lock.
    juce::String keyTrackID = "filterKeyTrack";
    juce::String noteLockID = "filterNoteLock";
    juce::String harmonicID = "filterHarmonicLock";

    if (filterIndex == 1 && safeGetParam("modFilter1LinkToMaster") <= 0.5f)
    {
        keyTrackID = "modFilter1KeyTrack";
        noteLockID = "modFilter1NoteLock";
        harmonicID = "modFilter1HarmonicLock";
    }
    else if (filterIndex == 2 && safeGetParam("modFilter2LinkToMaster") <= 0.5f)
    {
        keyTrackID = "modFilter2KeyTrack";
        noteLockID = "modFilter2NoteLock";
        harmonicID = "modFilter2HarmonicLock";
    }
    // A linked mod filter falls through to the master IDs above, which is exactly how
    // its knobs and its Key Tracking toggle already behave.

    if (safeGetParam(keyTrackID) <= 0.5f || safeGetParam(noteLockID) <= 0.5f)
        return std::nullopt;

    return safeGetParam(harmonicID) > 0.5f ? NoteLock::Grid::Harmonics
                                           : NoteLock::Grid::Semitones;
}

void SpaceDustAudioProcessorEditor::snapCutoffToNoteLock(int filterIndex)
{
    const auto grid = activeNoteLockGrid(filterIndex);
    if (!grid)
        return;

    // A linked mod filter shares the master's cutoff parameter, so snap that one --
    // writing to the filter's own (currently detached) param would change nothing.
    juce::String cutoffID = "filterCutoff";
    if (filterIndex == 1 && safeGetParam("modFilter1LinkToMaster") <= 0.5f)
        cutoffID = "modFilter1Cutoff";
    else if (filterIndex == 2 && safeGetParam("modFilter2LinkToMaster") <= 0.5f)
        cutoffID = "modFilter2Cutoff";

    auto* param = dynamic_cast<juce::AudioParameterFloat*>(
        audioProcessor.getValueTreeState().getParameter(cutoffID));
    if (param == nullptr)
        return;

    const auto& range = param->getNormalisableRange();
    const float current = param->get();
    const auto snapped = static_cast<float>(
        NoteLock::snapHz(current, range.start, range.end, *grid));

    if (std::abs(snapped - current) < 0.001f)
        return;   // already on a detent -- do not emit a pointless automation gesture

    // Wrapped in a balanced gesture: a naked setValueNotifyingHost corrupts FL
    // Studio's "Last Tweaked" tracking, which breaks later-created automation
    // (same reasoning as PresetManager::loadInitPreset).
    param->beginChangeGesture();
    param->setValueNotifyingHost(range.convertTo0to1(snapped));
    param->endChangeGesture();
}

void SpaceDustAudioProcessorEditor::syncLinkedFilterParams(const juce::String& parameterID, float /*newValue*/)
{
    // The only thing to handle now is a "Link to Master" toggle flipping: repoint that filter's
    // knobs at the master params (linked) or its own params (unlinked). Master-filter edits reach
    // a linked knob automatically because the knob shares the master parameter. No param is ever
    // written from here, so nothing can re-enter Live's automation pass.
    if (parameterID == "modFilter1LinkToMaster" || parameterID == "modFilter2LinkToMaster")
        rebuildLinkedFilterAttachments();
}

//==============================================================================
// -- Slider Listener (linked mod filter -> master, pitch bend snap-back) --

void SpaceDustAudioProcessorEditor::sliderDragStarted(juce::Slider* /*slider*/)
{
    // No-op: linked mod-filter knobs share the master parameter via their attachment, so there
    // is no drag-driven mod->master push to arm here.
}

void SpaceDustAudioProcessorEditor::sliderValueChanged(juce::Slider* /*slider*/)
{
    // No-op: mod-filter <-> master coupling is handled by attachment retargeting
    // (see rebuildLinkedFilterAttachments), not by pushing values between parameters.
}

void SpaceDustAudioProcessorEditor::sliderDragEnded(juce::Slider* slider)
{
    if (slider == &pitchBendSlider)
    {
        // Trigger processor-based ramp: smooth linear return to 0 over 0.05s (no stepped sound)
        double val = pitchBendSlider.getValue();
        if (std::abs(val) > 0.001f)
        {
            audioProcessor.pitchBendSnapStartValue.store(static_cast<float>(val));
            audioProcessor.pitchBendRampReset.store(true);
            audioProcessor.pitchBendSnapActive.store(true);
            audioProcessor.pitchBendRampComplete.store(false);
            pitchBendSnapActive = true;
            startTimer(8);  // Poll for ramp complete + update display
        }
        else
        {
            pitchBendSlider.setValue(0.0, juce::sendNotificationSync);
        }
    }
}

//==============================================================================
// -- Easter egg: put the tab away --

void SpaceDustAudioProcessorEditor::hideCheezeGuyTab()
{
    if (! cheezeGuyTabAdded)
        return;

    const int index = tabbedComponent.getTabNames().indexOf("Cheeze Guy");

    if (index >= 0)
    {
        // Step off it first. The tab was added with "do not delete the component",
        // so removing it only unparents the game -- which is what lets us destroy
        // it here, in that order and not the other way round.
        if (tabbedComponent.getCurrentTabIndex() == index)
            tabbedComponent.setCurrentTabIndex(0);

        tabbedComponent.removeTab(index);
    }

    cheezeGuyGame.reset();
    cheezeGuyTabAdded = false;
    // Cleared in the processor too, or the tab would come back the next time the
    // editor is opened -- that flag is what survives closing the window.
    audioProcessor.cheezeGuyActivated = false;

    // The game held the keyboard focus for its arrow keys; hand it back.
    if (standaloneKeyboard != nullptr)
        standaloneKeyboard->grabKeyboardFocus();
}

//==============================================================================
// -- Final EQ: point one set of controls at one band --
//
// Five bands share a Type dropdown and three knobs, so the controls are moved from
// band to band rather than duplicated five times over. Moving them means rebuilding
// their parameter attachments, since an attachment is bound to one parameter ID for
// its lifetime. Whichever way the band is chosen -- the Node dropdown, or a click on
// a dot in the display -- it arrives here.

void SpaceDustAudioProcessorEditor::setFinalEQEditedBand(int band)
{
    band = juce::jlimit(0, FinalEQComponent::numBands - 1, band);
    finalEQEditedBand_ = band;

    auto& vts = audioProcessor.getValueTreeState();
    const juce::String n(band + 1);

    // Release the old attachments BEFORE making the new ones: two attachments live
    // on one control would both answer its changes and write to different bands.
    finalEQTypeAttachment.reset();
    finalEQQAttachment.reset();
    finalEQFreqAttachment.reset();
    finalEQGainAttachment.reset();

    finalEQTypeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        vts, "finalEQB" + n + "Type", finalEQTypeCombo);
    finalEQQAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        vts, "finalEQB" + n + "Q", finalEQQSlider);
    finalEQFreqAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        vts, "finalEQB" + n + "Freq", finalEQFreqSlider);
    finalEQGainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        vts, "finalEQB" + n + "Gain", finalEQGainSlider);

    // The assign-mode wrappers follow the attachments onto the new band, or a
    // routing written on the Gain knob would land on whichever band happened to
    // be chosen when the editor opened.
    if (finalEQQModKnob != nullptr)
        finalEQQModKnob->setDestination(("finalEQB" + n + "Q").toStdString());
    if (finalEQFreqModKnob != nullptr)
        finalEQFreqModKnob->setDestination(("finalEQB" + n + "Freq").toStdString());
    if (finalEQGainModKnob != nullptr)
        finalEQGainModKnob->setDestination(("finalEQB" + n + "Gain").toStdString());

    // Silent, because the selection can arrive from the dropdown itself.
    if (finalEQNodeCombo.getSelectedId() != band + 1)
        finalEQNodeCombo.setSelectedId(band + 1, juce::dontSendNotification);

    if (finalEQComponent != nullptr)
        finalEQComponent->setSelectedBand(band);
}

//==============================================================================
// -- Button Listener (On toggle -> group glow sync) --

void SpaceDustAudioProcessorEditor::buttonClicked(juce::Button* button)
{
    // No-op: the mod-tab Warm Saturation toggles are attached directly to "warmSaturationMaster"
    // when linked (and to their own param when not), so a click edits the correct parameter with
    // no manual mod->master push. Kept as an empty override in case future buttons need it.
    juce::ignoreUnused(button);
}

void SpaceDustAudioProcessorEditor::buttonStateChanged(juce::Button* button)
{
    if (isBeingDestroyed.load() || button == nullptr)
        return;

    auto sync = [](juce::ToggleButton& btn, juce::GroupComponent& grp) {
        grp.getProperties().set("isActive", btn.getToggleState());
        grp.repaint();
        if (auto* parent = grp.getParentComponent())
            parent->repaint();  // Repaint page so outer halos update
    };
    if (button == &delayEnabledButton)
        sync(delayEnabledButton, delayGroup);
    else if (button == &phaserEnabledButton)
        sync(phaserEnabledButton, phaserGroup);
    else if (button == &flangerEnabledButton)
        sync(flangerEnabledButton, flangerGroup);
    else if (button == &bitCrusherEnabledButton)
        sync(bitCrusherEnabledButton, bitCrusherGroup);
    else if (button == &softClipperEnabledButton)
        sync(softClipperEnabledButton, softClipperGroup);
    else if (button == &compressorEnabledButton)
        sync(compressorEnabledButton, compressorGroup);
    else if (button == &lofiEnabledButton)
        sync(lofiEnabledButton, lofiGroup);
    else if (button == &transientEnabledButton)
        sync(transientEnabledButton, transientGroup);
    else if (button == &reverbEnabledButton)
        sync(reverbEnabledButton, reverbGroup);
    else if (button == &tranceGateEnabledButton)
        sync(tranceGateEnabledButton, tranceGateGroup);
    else if (button == &grainDelayEnabledButton)
        sync(grainDelayEnabledButton, grainDelayGroup);
    else if (button == &lfo1EnabledButton)
        sync(lfo1EnabledButton, lfo1Group);
    else if (button == &lfo2EnabledButton)
        sync(lfo2EnabledButton, lfo2Group);
    else if (button == &finalEQEnabledButton)
        sync(finalEQEnabledButton, finalEQGroup);
}

//==============================================================================
// -- Timer Callback (for LFO rate display updates) --

// Hard-coded LFO sync mode label arrays
// Straight mode: 9 steps (rateIndex 0-8)
static const juce::String straightLabels[9] = {
    "2 bar", "1 bar", "1/2 bar", "1/4 bar", "1/8 bar", "1/16 bar", "1/32 bar", "1/64 bar", "1/128 bar"
};

// Triplet mode: 9 steps (rateIndex 0-8)
static const juce::String tripletLabels[9] = {
    "1.5 bar", "1 bar", "2/3 bar", "1/3 bar", "1/6 bar", "1/12 bar", "1/24 bar", "1/48 bar", "1/96 bar"
};

// All mode: 18 steps (mappedIndex 0-17)
static const juce::String allLabels[18] = {
    "2 bar", "1.5 bar", "1 bar", "2/3 bar", "1/2 bar", "1/3 bar", "1/4 bar", "1/6 bar",
    "1/8 bar", "1/12 bar", "1/16 bar", "1/24 bar", "1/32 bar", "1/48 bar", "1/64 bar",
    "1/98 bar", "1/128 bar", "1/128 bar"
};

void SpaceDustAudioProcessorEditor::timerCallback()
{
    if (isBeingDestroyed.load())
        return;

    // Remember the active tab so closing/reopening the editor returns here, not Main.
    if (tabbedComponent.getNumTabs() > 0)
        audioProcessor.lastActiveTabIndex = tabbedComponent.getCurrentTabIndex();

    // Assign mode: where each LFO is in its cycle, for the marker on the
    // indicator bars. Ahead of the pitch-bend branch below, which returns early.
    refreshModIndicators();

    // Pitch bend snap-back: sync display with processor ramp, then set to 0 when complete
    if (pitchBendSnapActive)
    {
        if (audioProcessor.pitchBendRampComplete.load())
        {
            pitchBendSnapActive = false;
            pitchBendSlider.setValue(0.0, juce::sendNotificationSync);
            audioProcessor.pitchBendRampComplete.store(false);
            startTimer(50);  // Restore LFO rate display interval
        }
        else
        {
            // Update slider display to match processor's ramped value
            float ramped = audioProcessor.pitchBendRampCurrentValue.load();
            pitchBendSlider.setValue(ramped, juce::dontSendNotification);
        }
        return;  // Skip LFO display updates this tick
    }
    
    // Update LFO1 rate display
    bool lfo1Sync = safeGetParam("lfo1Sync") > 0.5f;
    double lfo1Rate = lfo1FreeRateSlider.getValue();
    bool lfo1Triplet = safeGetParam("lfo1TripletEnabled") > 0.5f;
    bool lfo1All = safeGetParam("lfo1TripletStraightToggle") > 0.5f;
    
    lfo1FreeRateSlider.setVisible(true);
    lfo1SyncRateCombo.setVisible(false);
    
    if (lfo1Sync)
    {
        // Sync mode: linear mapping (matches processor - avoids fold-back)
        float rateClamped = juce::jlimit(0.0f, 12.0f, static_cast<float>(lfo1Rate));
        int musicalIndex = static_cast<int>(std::round(rateClamped * 8.0f / 12.0f));
        musicalIndex = juce::jlimit(0, 8, musicalIndex);
        
        juce::String syncText;
        if (lfo1Triplet && lfo1All)
        {
            int mappedIndex = static_cast<int>(std::round(rateClamped * 17.0f / 12.0f));
            mappedIndex = juce::jlimit(0, 17, mappedIndex);
            syncText = allLabels[mappedIndex];
        }
        else if (lfo1Triplet && !lfo1All)
        {
            syncText = tripletLabels[musicalIndex];
        }
        else
        {
            syncText = straightLabels[musicalIndex];
        }
        
        if (!isBeingDestroyed.load())
        {
            lfo1RateValueLabel.setText(syncText, juce::dontSendNotification);
            lfo1TripletButton.setVisible(true);
            lfo1TripletStraightButton.setVisible(lfo1Triplet);
        }
    }
    else
    {
        // Free mode: 0.01-200 Hz logarithmic. Shares the processor's mapping so the
        // readout cannot drift from the rate actually being rendered.
        const juce::String rateText = juce::String::formatted("%.2f Hz",
            SpaceDustAudioProcessor::lfoKnobToHz(static_cast<double>(lfo1Rate)));
        if (!isBeingDestroyed.load())
        {
            lfo1RateValueLabel.setText(rateText, juce::dontSendNotification);
            lfo1TripletButton.setVisible(false);
            lfo1TripletStraightButton.setVisible(false);
        }
    }
    
    // Update LFO2 rate display
    bool lfo2Sync = safeGetParam("lfo2Sync") > 0.5f;
    double lfo2Rate = lfo2FreeRateSlider.getValue();
    bool lfo2Triplet = safeGetParam("lfo2TripletEnabled") > 0.5f;
    bool lfo2All = safeGetParam("lfo2TripletStraightToggle") > 0.5f;
    
    lfo2FreeRateSlider.setVisible(true);
    lfo2SyncRateCombo.setVisible(false);
    
    if (lfo2Sync)
    {
        // Sync mode: linear mapping (matches processor - avoids fold-back)
        float rateClamped = juce::jlimit(0.0f, 12.0f, static_cast<float>(lfo2Rate));
        int musicalIndex = static_cast<int>(std::round(rateClamped * 8.0f / 12.0f));
        musicalIndex = juce::jlimit(0, 8, musicalIndex);
        
        juce::String syncText;
        if (lfo2Triplet && lfo2All)
        {
            int mappedIndex = static_cast<int>(std::round(rateClamped * 17.0f / 12.0f));
            mappedIndex = juce::jlimit(0, 17, mappedIndex);
            syncText = allLabels[mappedIndex];
        }
        else if (lfo2Triplet && !lfo2All)
        {
            syncText = tripletLabels[musicalIndex];
        }
        else
        {
            syncText = straightLabels[musicalIndex];
        }
        
        if (!isBeingDestroyed.load())
        {
            lfo2RateValueLabel.setText(syncText, juce::dontSendNotification);
            lfo2TripletButton.setVisible(true);
            lfo2TripletStraightButton.setVisible(lfo2Triplet);
        }
    }
    else
    {
        // Free mode -- see the LFO1 branch above.
        const juce::String rateText = juce::String::formatted("%.2f Hz",
            SpaceDustAudioProcessor::lfoKnobToHz(static_cast<double>(lfo2Rate)));
        if (!isBeingDestroyed.load())
        {
            lfo2RateValueLabel.setText(rateText, juce::dontSendNotification);
            lfo2TripletButton.setVisible(false);
            lfo2TripletStraightButton.setVisible(false);
        }
    }
    
    // Update Delay rate display (inverted: knob 0 = long, knob 12 = short)
    // Use same parameter value as processor for consistency
    bool delaySync = safeGetParam("delaySync") > 0.5f;
    float delayRateParam = safeGetParam("delayRate");
    double delayRateClamped = juce::jlimit(0.0, 12.0, static_cast<double>(delayRateParam));
    double delayRateInverted = 12.0 - delayRateClamped;
    
    if (delaySync)
    {
        // Unified list: straight, dotted (1/8., 1/4.), and triplets baked in.
        // Sample directly into 0..17 to match the processor; the previous
        // 0..12 -> 0..17 round mapping skipped 1/16, 1/8., 2, and 5.
        double normalized = juce::jlimit(0.0, 1.0, delayRateInverted / 12.0);
        double curved = std::pow(normalized, 2.5);
        int musicalIndex = static_cast<int>(std::round(curved * 17.0));
        musicalIndex = juce::jlimit(0, 17, musicalIndex);
        static const juce::String delayLabels[18] = {
            "1/32", "1/24", "1/16", "1/12", "1/8", "1/8.", "1/4", "1/4.",
            "1/2", "3/4", "1", "3/2", "2", "3", "4", "5", "8", "8"
        };
        if (!isBeingDestroyed.load())
        {
            delayRateValueLabel.setText(delayLabels[musicalIndex], juce::dontSendNotification);
            delayRateValueLabel.setVisible(true);
        }
    }
    else
    {
        // Free mode: same as processor
        float normalizedRate = juce::jlimit(0.0f, 1.0f, static_cast<float>(delayRateInverted) / 12.0f);
        float logMin = std::log(20.0f);
        float logMax = std::log(2000.0f);
        float logMs = logMin + normalizedRate * (logMax - logMin);
        float delayMs = std::exp(logMs);
        delayMs = juce::jlimit(20.0f, 2000.0f, delayMs);
        juce::String msText = juce::String::formatted("%.0f ms", delayMs);
        if (!isBeingDestroyed.load())
        {
            delayRateValueLabel.setText(msText, juce::dontSendNotification);
            delayRateValueLabel.setVisible(true);
        }
    }
    
    // Snapshot the averaged L/R output level ONCE per frame so every glow site reads the
    // identical value. (Reading the live atomics at each paint site let the audio thread
    // update them mid paint-pass -> objects glowed at slightly different rates.)
    {
        float leftPeak  = audioProcessor.getLeftPeakLevel();
        float rightPeak = audioProcessor.getRightPeakLevel();
        glowMeterLevel_ = juce::jmin(1.0f, 0.5f * (leftPeak + rightPeak));

        // The stereo meter is NOT pushed from here any more. It reads the same peak
        // atomics itself on its own 60Hz timer, because its peak-hold fall and RGB
        // motion smear are per-frame effects that stutter when fed at this timer's
        // 20Hz. Reading the atomics twice is free -- they are plain loads, not
        // consumed like Sol's getAndClearMeterPeak.
    }
    // Update clipping hold state (runs every tick regardless of active tab)
    {
        const float clipThreshold = 0.891f;  // -1 dB, matches meter red zone
        bool peakInRed = audioProcessor.getLeftPeakLevel() >= clipThreshold
                      || audioProcessor.getRightPeakLevel() >= clipThreshold;
        if (peakInRed)
            clippingHoldTicks = clippingHoldDuration;
        else if (clippingHoldTicks > 0)
            --clippingHoldTicks;
    }

    customLookAndFeel.setOutputMeterClipping(clippingHoldTicks > 0);

    // Feeds the knob arcs, which fill by output level. Same per-frame snapshot every
    // other glow site reads, so knobs, halos and starfield all move off one value.
    customLookAndFeel.setOutputMeterLevel(glowMeterLevel_);

    // Redraw glow halos / starfield / edge glow so they follow output level - but ONLY when
    // the level or clipping state actually changed. The whole-editor repaint re-lays-out every
    // label/knob, so doing it unconditionally every tick pegged the CPU even at silence (the
    // Standalone appeared to hang on launch). The painted look is a pure function of these two
    // values, so skipping unchanged frames is visually identical and lets CPU fall to idle.
    const bool nowClipping = (clippingHoldTicks > 0);
    if (std::abs(glowMeterLevel_ - lastPaintedGlowLevel_) > 0.001f || nowClipping != lastPaintedClipping_)
    {
        lastPaintedGlowLevel_ = glowMeterLevel_;
        lastPaintedClipping_  = nowClipping;
        repaint();

        // The Waveforms window is a window of its own, so this editor's repaint
        // does not reach it -- but it shares this LookAndFeel, and its waveforms
        // and buttons bloom off the same level. Without this line they would
        // hold whatever glow they had when they were last drawn for some other
        // reason. Inside the guard, so it costs nothing at silence, and only
        // while the window is actually open.
        if (waveformWindow != nullptr && waveformWindow->isVisible())
            waveformWindow->repaintContent();
    }


    // Update Spectral tab (Lissajous drawn in SpectralPage::paint, Oscilloscope, Spectrum)
    constexpr int spectralTabIndex = 4;
    if (spectralPage != nullptr && tabbedComponent.getCurrentTabIndex() == spectralTabIndex && !isBeingDestroyed.load())
    {
        const auto& buf = audioProcessor.getGoniometerBuffer();
        const int validSamples = audioProcessor.getGoniometerValidSamples();
        spectralPage->repaint();
        bool showClipping = clippingHoldTicks > 0;
        if (auto* osc = spectralPage->getOscilloscope())
        {
            osc->setClipping(showClipping);
            osc->update(buf, validSamples);
            osc->repaint();
        }
        if (auto* spec = spectralPage->getSpectrumAnalyser())
        {
            // FFT + repaint are self-driven at 60fps inside the component;
            // here we just keep its clipping colour and sample rate current.
            spec->setClipping(showClipping);
            spec->setSampleRate(audioProcessor.getSampleRate());
        }
    }

    // The Final EQ's spectrum is the same self-driven analyser, so it needs the
    // same two things kept current -- and only while its tab is the one on show.
    constexpr int saturationTabIndex = 3;
    if (finalEQComponent != nullptr && tabbedComponent.getCurrentTabIndex() == saturationTabIndex
        && !isBeingDestroyed.load())
    {
        finalEQComponent->setSampleRate(audioProcessor.getSampleRate());
        finalEQComponent->setClipping(clippingHoldTicks > 0);
    }

    // Update Legato Glide button visibility based on voice mode.
    // Show in Mono (1) and Legato (2): in both modes the toggle gates whether
    // glide applies only on legato (overlapping) notes vs. on every note change.
    // Envelope retrigger behaviour stays mode-specific (Mono always retriggers,
    // Legato preserves on overlap).
    if (!isBeingDestroyed.load())
    {
        int voiceModeIndex = 0;
        if (auto* voiceModeParam = audioProcessor.getValueTreeState().getParameter("voiceMode"))
        {
            if (auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(voiceModeParam))
            {
                voiceModeIndex = choiceParam->getIndex();
            }
        }
        const bool isMonoOrLegato = (voiceModeIndex == 1 || voiceModeIndex == 2);
        if (legatoGlideButton.isVisible() != isMonoOrLegato)
        {
            // Mode changed (UI, automation, or preset): the Master box height now
            // depends on this, so relayout to collapse (Poly) / expand (Mono/Legato).
            //
            // layoutPlate(), NOT resized(). The editor's resized() is the stub's now and
            // does nothing, so calling it here silently stopped the Master box resizing
            // when the voice mode changed -- the border simply never moved. The face is
            // repainted too because the painted background (the logo sits off the Master
            // box's bottom edge) depends on the layout this just changed.
            legatoGlideButton.setVisible(isMonoOrLegato);
            layoutPlate();
            repaint();
        }
    }
}

//==============================================================================
// -- Paint Method --

// This is the whole Space Dust face â€” starfield, edge glow, logo, version. It used to
// be split out when the UI lived in a floating window. `plateWidth` is simply the
// editor's own width now; the parameter is kept so the internal call sites read the
// same. Nothing about what gets drawn has changed.
void SpaceDustAudioProcessorEditor::paintPlate(juce::Graphics& g, int plateWidth)
{
    if (isBeingDestroyed.load())
        return;

    // Scale the painted background + decorations by the same factor as mainView so they
    // align with the (scaled) controls and fill the resized window. Everything below is
    // drawn in DESIGN coordinates (kDesignWidth x designHeight_); the transform maps it to
    // the on-screen size. fillAll still fills the whole clip region regardless of transform.
    const float paintScale = (plateWidth > 0) ? plateWidth / (float) kDesignWidth : 1.0f;

    g.fillAll(juce::Colour(0xff0a0a1f));

    g.addTransform(juce::AffineTransform::scale(paintScale));
    const int w = kDesignWidth;
    const int h = designHeight_;
    {
        drawStarfield(g, w, h, getGlowMeterLevel());  // single per-frame averaged L/R snapshot
    }

    //==============================================================================
    // -- Arcade Edge Glow (top & bottom) - parabolic, subtle, smooth color gradient --
    // Cyan -> deep blue -> transparent. Red variant when metering in red zone.
    // Tabs are translucent so glow shows through at their bottom edge.
    //
    // Off as of 2026-08-01 -- see kGlowEnabled at the top of this file. The whole
    // block is compiled out by the `if constexpr`, so it costs nothing while off and
    // comes back untouched when the flag is flipped.
    if constexpr (kGlowEnabled)
    {
        float avgLevel = getGlowMeterLevel();  // single per-frame averaged L/R snapshot

        const float maxGlowDepth = 90.0f;
        const bool isRed = (clippingHoldTicks > 0);
        // More subtle: reduced base alpha (was 15+140, now 6+60)
        juce::uint8 peakAlpha = static_cast<juce::uint8>(juce::jlimit(0, 255, static_cast<int>(6 + 60 * avgLevel)));

        // Color gradient: bright at edge, deeper shade inward, then transparent
        const juce::Colour edgeCol = isRed ? juce::Colour(SpaceDustLookAndFeel::kClipRed) : juce::Colour(0xff00d4ff);  // Darker red when clipping
        const juce::Colour midCol  = isRed ? juce::Colour(SpaceDustLookAndFeel::kClipRed).darker(0.6f) : juce::Colour(0xff0066aa);  // Deeper red
        const juce::Colour fadeCol = juce::Colours::transparentBlack;

        // Parabolic depth: center extends further (U/n-shape)
        auto parabolicDepth = [](float xNorm, float layerHeight) -> float {
            float t = 1.0f - 4.0f * (xNorm - 0.5f) * (xNorm - 0.5f);
            t = juce::jmax(0.0f, t);
            return layerHeight * (0.25f + 0.75f * t);
        };

        auto drawTopGlow = [&](float layerHeight, float alphaScale)
        {
            juce::Path path;
            path.startNewSubPath(0.0f, 0.0f);
            path.lineTo(static_cast<float>(w), 0.0f);
            for (int x = w; x >= 0; x -= 2)
            {
                float xNorm = static_cast<float>(x) / static_cast<float>(w);
                float depth = parabolicDepth(xNorm, layerHeight);
                path.lineTo(static_cast<float>(x), depth);
            }
            path.closeSubPath();

            // Smooth gradient: edge (bright) -> 30% -> 60% -> 90% (transparent). Color gradient cyan->blue or red shades
            juce::ColourGradient grad(edgeCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale)), (float)w * 0.5f, 0.0f,
                                      fadeCol, (float)w * 0.5f, layerHeight, false);
            grad.addColour(0.15f, edgeCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale * 0.85f)));
            grad.addColour(0.35f, midCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale * 0.5f)));
            grad.addColour(0.55f, midCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale * 0.22f)));
            grad.addColour(0.78f, fadeCol);
            g.setGradientFill(grad);
            g.fillPath(path);
        };
        auto drawBottomGlow = [&](float layerHeight, float alphaScale)
        {
            // In the Standalone build a playable keyboard occupies the bottom strip; anchor
            // the bottom edge-glow to the bottom of the CONTENT area (above the keys) so it
            // stays visible instead of being hidden behind the keyboard. kbH == 0 in the
            // VST3 (no keyboard), so the glow sits at the window bottom there, unchanged.
            const int kbH = (standaloneKeyboard != nullptr) ? standaloneKeyboardHeight : 0;
            float yBase = static_cast<float>(h - kbH);
            juce::Path path;
            path.startNewSubPath(0.0f, yBase);
            path.lineTo(static_cast<float>(w), yBase);
            for (int x = w; x >= 0; x -= 2)
            {
                float xNorm = static_cast<float>(x) / static_cast<float>(w);
                float depth = parabolicDepth(xNorm, layerHeight);
                path.lineTo(static_cast<float>(x), yBase - depth);
            }
            path.closeSubPath();

            juce::ColourGradient grad(fadeCol, (float)w * 0.5f, yBase - layerHeight,
                                      edgeCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale)), (float)w * 0.5f, yBase, false);
            grad.addColour(0.22f, fadeCol);
            grad.addColour(0.45f, midCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale * 0.22f)));
            grad.addColour(0.65f, midCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale * 0.5f)));
            grad.addColour(0.85f, edgeCol.withAlpha(static_cast<juce::uint8>(peakAlpha * alphaScale * 0.85f)));
            g.setGradientFill(grad);
            g.fillPath(path);
        };

        // Draw back-to-front: outer halo first, then bright core. Subtler layer alphas.
        drawTopGlow(maxGlowDepth * 1.4f, 0.35f);
        drawBottomGlow(maxGlowDepth * 1.4f, 0.35f);
        drawTopGlow(maxGlowDepth, 0.55f);
        drawBottomGlow(maxGlowDepth, 0.55f);
        drawTopGlow(maxGlowDepth * 0.55f, 0.85f);
        drawBottomGlow(maxGlowDepth * 0.55f, 0.85f);
    }

    //==============================================================================
    // -- Title: Space Dust (nebula logo artwork) --
    {
        auto titleArea = juce::Rectangle<int>(0, 2, w, 44);

        // Lazily fetch the embedded logo (black already keyed to transparent).
        if (! titleImage.isValid())
            titleImage = juce::ImageCache::getFromMemory(BinaryData::SpaceDustTitle_png,
                                                         BinaryData::SpaceDustTitle_pngSize);

        if (titleImage.isValid())
        {
            // Draw the logo centred in the header strip, preserving aspect.
            //
            // Fitted inside a slightly SHRUNK copy of the strip rather than the strip
            // itself, which is how the title is sized: drawImageWithin scales to fit
            // whatever rectangle it is given, so 0.9 here is 90% of full size. Shrunk
            // about the centre so it stays put rather than climbing towards the top.
            // The artwork is wider than the strip's proportions, so the HEIGHT is what
            // binds -- but both are scaled, so this still holds if the art ever changes
            // to something width-limited.
            constexpr float kTitleScale = 0.9f;

            const auto drawArea = titleArea.withSizeKeepingCentre(
                juce::roundToInt(titleArea.getWidth()  * kTitleScale),
                juce::roundToInt(titleArea.getHeight() * kTitleScale));

            //--------------------------------------------------------------
            // -- Title bloom, driven by the meter --
            // The title blooms with the output level, the same as the knob arcs, the
            // group halos and the starfield: getGlowAmount() is the one glow law, so
            // the title rises and falls with the rest of the face.
            //
            // NOT gated on the level, unlike every other glow site. The title keeps a
            // small halo at rest so it never goes flat (kBloomFloor). This costs
            // nothing while idle: the plate repaints only when the level moves, so at
            // silence the resting bloom is painted once and then stays on screen.
            //
            // See bloomKeyedImage for the method and for why the copies are offset
            // rather than enlarged. The 63C logo below blooms through the same call.
            bloomKeyedImage(g, titleImage, drawArea,
                            meterLinkedTitleGlowHue(clippingHoldTicks > 0),
                            bloomAmount(customLookAndFeel.getGlowAmount()));

            g.setColour(juce::Colours::white);
            g.drawImageWithin(titleImage, drawArea.getX(), drawArea.getY(),
                              drawArea.getWidth(), drawArea.getHeight(),
                              juce::RectanglePlacement::centred, false);
        }
        else
        {
            // Fallback: rendered text title (Glitch Goblin font).
            juce::Font titleFont = customLookAndFeel.getTitleFont(40.0f);
            const juce::Colour glowCol = meterLinkedTitleGlowHue(clippingHoldTicks > 0);
            for (int i = 3; i >= 1; --i)
            {
                g.setColour(glowCol.withAlpha(static_cast<juce::uint8>(18 / i)));
                g.setFont(titleFont);
                g.drawText(safeString("Space Dust"), titleArea.expanded(i * 2, i), juce::Justification::centred, true);
            }
            g.setColour(juce::Colour(0x44000000));
            g.setFont(titleFont);
            g.drawText(safeString("Space Dust"), titleArea.translated(1, 2), juce::Justification::centred, true);
            g.setColour(juce::Colour(0xffffffff));
            g.setFont(titleFont);
            g.drawText(safeString("Space Dust"), titleArea, juce::Justification::centred, true);
        }
    }

    //==============================================================================
    // -- Master Section Glow --
    if (masterGroup.isVisible())
    {
        float avgLevel = getGlowMeterLevel();  // single per-frame averaged L/R snapshot
        const int baseAlpha = 8 + static_cast<int>(44.0f * avgLevel);
        drawGlows(g, baseAlpha, meterLinkedGroupGlowHue(clippingHoldTicks > 0), { &masterGroup });
    }

    //==============================================================================
    // -- 63C company logo (bottom-right watermark) + version (top-right) --
    {
        if (! logoImage.isValid())
            logoImage = juce::ImageCache::getFromMemory(BinaryData::Logo63C_png,
                                                        BinaryData::Logo63C_pngSize);

        // Geometry shared by the bottom-right logo and the top-right version text.
        // The Master box bottom in Legato (glide toggle visible) is filterBoxBottomY;
        // the logo lives in the band beneath it, sized to a fraction of the tallest
        // height that still clears every box on all tabs. In the Standalone the playable
        // keyboard occupies the bottom strip, so the band stops at the keyboard top
        // (not the window bottom) â€” otherwise the keys cover the logo.
        const int kbStripH = (standaloneKeyboard != nullptr) ? standaloneKeyboardHeight : 0;
        const int contentBottom = h - kbStripH;
        const int masterBottom = (filterBoxBottomY > 0 ? filterBoxBottomY : contentBottom - 60);
        const int band = contentBottom - masterBottom;
        const int cleanH = juce::jlimit(24, 64, band - 9);
        const int logoH = juce::roundToInt(cleanH * 0.5985f);  // cumulative downscales
        const int logoW = juce::roundToInt(logoH * (logoImage.getWidth()
                                                    / (float) logoImage.getHeight()));
        // Tuck into the bottom-right corner with an equal margin on every side:
        // gap to the window's right edge == gap to the window's bottom == gap
        // up to the Master box bottom.
        const int gap = (band - logoH) / 2;

        if (logoImage.isValid())
        {
            const int logoX = (w - gap) - logoW;
            const int logoY = masterBottom + gap;

            // Blooms with the meter, through the same call the title uses, so the two
            // marks light up together and by the same law.
            //
            // The trims are the departure. The radii inside are fractions of the height,
            // and the logo is about 25px against the title's 40px, so a proportional
            // halo on it came out small and the glow read as faint (Giuseppe,
            // 2026-08-09). The reach is doubled and the light lifted, which is a
            // deliberate break from proportion for this mark only -- the logo now
            // blooms harder than its size alone would give.
            //
            // Logo63C.png is a white ghost on transparent, so the alpha mask has a
            // shape to work with. An opaque logo would bloom its bounding box.
            constexpr float kLogoReach = 2.0f;
            constexpr float kLogoLight = 1.15f;

            bloomKeyedImage(g, logoImage,
                            juce::Rectangle<int>(logoX, logoY, logoW, logoH),
                            meterLinkedTitleGlowHue(clippingHoldTicks > 0),
                            bloomAmount(customLookAndFeel.getGlowAmount()),
                            kLogoReach, kLogoLight);

            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.drawImageWithin(logoImage, logoX, logoY, logoW, logoH,
                              juce::RectanglePlacement::centred, false);
        }

        // -- Version number (top-right) --
        // Mirrors the 63C watermark into the top-right corner: the same distance from
        // the top as the logo sits from the (content) bottom, and the same distance
        // from the right edge â€” both equal to `gap`. Uses the synth's standard 12pt
        // body font and the same light-blue label colour as the knob labels.
        {
            const juce::String versionText = "v" JucePlugin_VersionString;
            const int textH = 16;
            const int textW = 120;
            const int textX = (w - gap) - textW;  // right edge sits `gap` from the right
            const int textY = gap;                // top edge sits `gap` from the top
            g.setFont(customLookAndFeel.getBodyFont(12.0f, true));
            g.setColour(juce::Colours::white);  // white, pairs with the 63C watermark
            g.drawText(versionText, textX, textY, textW, textH,
                       juce::Justification::topRight, false);
        }
    }
}

//==============================================================================
// -- paint / resized --
// Thin wrappers over the two functions that do the real work. They kept the "Plate"
// names when the UI lived in a floating window and then in a plate component inside the
// editor; both of those are gone (the shake was the only thing the plate existed to
// move), so the editor paints and lays out the face directly again, as it did before
// git 74f776e. The names are left alone because a dozen call sites inside this class
// call layoutPlate() to force a relayout, and they read correctly as-is.

void SpaceDustAudioProcessorEditor::paint(juce::Graphics& g)
{
    if (isBeingDestroyed.load())
        return;

    paintPlate(g, getWidth());
}

void SpaceDustAudioProcessorEditor::resized()
{
    layoutPlate();
}

void SpaceDustAudioProcessorEditor::layoutPlate()
{
    //==============================================================================
    // -- Safety Check: Don't resize if being destroyed --
    if (isBeingDestroyed.load())
        return;
    
    //==============================================================================
    // -- DEBUG: First Resized Call --
    // Note: LookAndFeel is declared first in header, so it's always valid during resized
    static bool firstResized = true;
    if (firstResized)
    {
        DBG("Space Dust: First resized() called");
        #if JUCE_DEBUG
        try
        {
            juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(safeString("SpaceDust_DebugLog.txt"));
            juce::FileOutputStream out(logFile);
            if (out.openedOk())
            {
                out.setPosition(out.getPosition());
                out.writeText("Space Dust: First resized() called - width=" + juce::String(getWidth()) + ", height=" + juce::String(getHeight()) + "\n", false, false, nullptr);
                out.flush();
            }
        }
        catch (...) {}
        #endif
        firstResized = false;
    }
    
    //==============================================================================
    // -- CRITICAL: Wrap entire resized() in try/catch to catch crashes --
    try
    {
        DBG("Space Dust: resized() - Starting layout (width=" + juce::String(getWidth()) + ", height=" + juce::String(getHeight()) + ")");
        
        // Write to log file for debugging
        #if JUCE_DEBUG
        try
        {
            juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(safeString("SpaceDust_DebugLog.txt"));
            juce::FileOutputStream out(logFile);
            if (out.openedOk())
            {
                out.setPosition(out.getPosition());
                out.writeText("Space Dust: resized() called - width=" + juce::String(getWidth()) + ", height=" + juce::String(getHeight()) + "\n", false, false, nullptr);
                out.flush();
            }
        }
        catch (...) {}
        #endif
    
    //==============================================================================
    // -- Drag-resize: size + scale the container holding the whole UI --
    // mainView is laid out at the fixed design size; one uniform transform scales it to the
    // editor's current (resizable) size. All layout below positions controls in design space.
    {
        const float viewScale = (getWidth() > 0) ? getWidth() / (float) kDesignWidth : 1.0f;
        mainView.setBounds(0, 0, kDesignWidth, designHeight_);
        mainView.setTransform(juce::AffineTransform::scale(viewScale));
    }

    //==============================================================================
    // -- Preset Controls Layout (Top Header Bar) --
    const int titleHeight = 48;  // Compact: title + tab bar
    {
        const int presetY = 10;
        const int presetH = 26;
        const int gap = 4;
        int px = 8;

        // The preset box gives up its left edge to the stepper arrows rather than
        // taking room from the buttons beside it: it starts where the arrows end,
        // and is narrower by exactly what they need. px advances by the original
        // 142 either way, so Save, Initialize and Folder do not move a pixel.
        //
        // Without this the box sat hard against x = 8, the arrows wanted to be at
        // -6, and they clamped to the window edge and sat on top of it.
        const int stepperRoom = ComboStepper::stripWidth + ComboStepper::gapToBox + 2;

        presetCombo.setBounds(px + stepperRoom, presetY, 142 - stepperRoom, presetH);
        px += 142 + gap;

        savePresetButton.setBounds(px, presetY, 80, presetH);
        px += 80 + gap;

        initPresetButton.setBounds(px, presetY, 105, presetH);
        px += 105 + gap;

        folderPresetButton.setBounds(px, presetY, 90, presetH);
    }

    //==============================================================================
    // -- Tabbed Component Layout --
    // Position tabbed component below title area, leaving space for Master section on right
    const int masterWidth = 220;  // Width of Master section
    const int masterGap = 80;     // Original gap (used for tabbedWidth calculation only)
    const int actualMasterGap = 40;  // Reduced by 50% - actual gap between tab and Master
    
    // TabbedComponent: left side, below title, leaving space for Master on right
    // Standalone reserves a strip at the very bottom for the playable keyboard; the rest
    // of the layout uses the height above it. In the plugin (no keyboard) this is 0 and
    // the layout is identical to before.
    const int kbStripH = (standaloneKeyboard != nullptr) ? standaloneKeyboardHeight : 0;
    const int layoutHeight = designHeight_ - kbStripH;   // design space (mainView is scaled, not this)

    // Expand tab width by 10%
    int tabbedWidth = static_cast<int>((kDesignWidth - masterWidth - masterGap) * 0.9 * 1.1);
    tabbedComponent.setBounds(0, titleHeight, tabbedWidth, layoutHeight - titleHeight);
    // Exit LFO Mode, at the right-hand end of the tab strip and only while the
    // mode is on. The strip's own bounds are relative to the tabbed component,
    // so they are moved into this layout's coordinates before use -- the button
    // is a sibling of the tabbed component, not a child of it. It takes no room
    // from the tabs: the five tab buttons end well short of the right edge.
    if (exitLfoModeButton.isVisible())
    {
        auto strip = tabbedComponent.getTabbedButtonBar().getBounds()
                     + tabbedComponent.getPosition();

        exitLfoModeButton.setBounds(strip.removeFromRight(120).reduced(4, 4));
        exitLfoModeButton.toFront(false);
    }

    // Tab glow overlay: sits on top of tab bar so parabolic glow shines through (drawn above tabs)
    const int tabBarHeight = 36;
    const int bottomGlowHeight = 90;
    if (tabGlowOverlay != nullptr)
        tabGlowOverlay->setBounds(0, titleHeight, tabbedWidth, tabBarHeight);
    if (bottomTabGlowOverlay != nullptr)
        bottomTabGlowOverlay->setBounds(0, layoutHeight - bottomGlowHeight, tabbedWidth, bottomGlowHeight);
    if (standaloneKeyboard != nullptr)
        standaloneKeyboard->setBounds(0, layoutHeight, kDesignWidth, kbStripH);
    
    //==============================================================================
    // -- Master Section Layout (Always Visible, Right Side) --
    // Master section spans from below title to bottom of window
    const int groupPadding = 10;         // Padding inside group boxes
    const int groupTitleHeight = 32;     // Compact height for group title area
    const int knobDiameter = 56;
    const int labelHeight = 18;           // Label height
    const int labelGap = 5;                // Gap between label and control
    const int comboHeight = 26;           // Combo box height
    const int comboWidth = 110;           // Combo box width

    int masterX = tabbedWidth + actualMasterGap;
    // Top: align the "Master" title with the tab labels. The tab bar starts at
    // titleHeight; both the group title text and the tab text sit ~15px below
    // their container top, so matching the box top to the tab bar top lines them up.
    int masterY = titleHeight;

    // Voice mode drives the box height: Poly (0) collapses the box; Mono (1) and
    // Legato (2) expand it to reveal the Legato Glide toggle at the bottom.
    int voiceModeIndex = 0;
    if (auto* voiceModeParam = audioProcessor.getValueTreeState().getParameter("voiceMode"))
        if (auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(voiceModeParam))
            voiceModeIndex = choiceParam->getIndex();
    const bool showLegato = (voiceModeIndex != 0);

    // --- Consistent vertical rhythm -------------------------------------------
    // Every section is laid out top-down with the SAME fixed gap between sections,
    // so the spacing reads evenly. The box height is the sum of its sections (it
    // is NOT stretched to the window), which is what collapses it in Poly and
    // expands it downward to reveal the Legato Glide toggle in Mono/Legato. The
    // six always-visible sections keep the same Y positions in every mode; only
    // the box border grows to make room for the toggle at the bottom.
    const int pitchFaderWidth = 24;
    const int pitchFaderHeight = 80;
    const int meterBarHeight = 170;            // fixed stereo-meter height
    const int legatoBtnH = 22;
    const int sectionGap = 20;                 // uniform gap between every section (tightened ~30% from 28)

    // Natural content height of each section: label + gap + control. Rotary value
    // readouts are drawn INSIDE the knob bounds, so no extra tail is reserved.
    const int knobGroupH  = labelHeight + labelGap + knobDiameter; // Volume / Bend / Glide
    const int comboGroupH = labelHeight + labelGap + comboHeight;  // Voice Mode
    const int faderGroupH = labelHeight + labelGap + pitchFaderHeight; // Pitch Bend
    // (the stereo meter has no label, its height is meterBarHeight)

    int masterContentH = knobGroupH                  // Volume
                       + sectionGap + meterBarHeight  // Stereo meter
                       + sectionGap + knobGroupH      // Bend Range
                       + sectionGap + faderGroupH     // Pitch Bend fader
                       + sectionGap + comboGroupH     // Voice Mode
                       + sectionGap + knobGroupH;     // Glide
    if (showLegato)
        masterContentH += sectionGap + legatoBtnH;    // Legato Glide toggle

    // Box height depends on whether the Legato Glide toggle is showing:
    //  - Mono/Legato (toggle visible): anchor the box BOTTOM to the Filter box
    //    bottom so the two line up. The Filter box is content-sized on the Main
    //    page (see MainPageComponent::resized(), matchedFilterHeight), so we read
    //    its real bottom edge and translate it into this editor's coordinates
    //    rather than assuming a fixed window margin.
    //  - Poly (toggle hidden): size to content, but with DOUBLE the normal bottom
    //    padding (2 * groupPadding) between the last element (Glide) and the
    //    border, since the elements stay top-anchored.
    const int masterContentHeight = groupTitleHeight + groupPadding + masterContentH + groupPadding;
    int masterHeight;
    if (showLegato)
    {
        // Filter box bottom in editor space, published by MainPageComponent::resized().
        // Fall back to the content height if the Main page hasn't been laid out yet.
        const int filterBoxBottom = (filterBoxBottomY > 0) ? filterBoxBottomY
                                                           : (masterY + masterContentHeight);
        // Never shorter than the content needs (keeps the toggle from clipping).
        masterHeight = juce::jmax(masterContentHeight, filterBoxBottom - masterY);
    }
    else
    {
        masterHeight = masterContentHeight + groupPadding;  // double bottom padding
    }
    masterGroup.setBounds(masterX, masterY, masterWidth, masterHeight);

    // Content rect: title + padding off the top, only padding off the bottom.
    // (reduced(x, gth+gp) would trim gth+gp off BOTH ends, eating ~32px at the
    // bottom and letting the last section drift toward the border.)
    auto masterContent = masterGroup.getBounds().reduced(groupPadding, 0);
    masterContent.removeFromTop(groupTitleHeight + groupPadding);
    masterContent.removeFromBottom(groupPadding);

    const int masterKnobX  = masterContent.getCentreX() - knobDiameter / 2;
    const int pitchCentreX = masterContent.getCentreX();
    int masterCurrentY = masterContent.getY();

    // Volume (label + rotary + value)
    masterVolumeLabel.setBounds(masterKnobX, masterCurrentY, knobDiameter, labelHeight);
    masterVolumeSlider.setBounds(masterKnobX, masterCurrentY + labelHeight + labelGap, knobDiameter, knobDiameter);
    masterVolumeSlider.setVisible(true);
    masterVolumeLabel.setVisible(true);
    masterCurrentY += knobGroupH + sectionGap;

    // Stereo level meter (fixed height). Width comes from the component itself --
    // it includes room for the glow, and hardcoding a second copy of the geometry
    // here is exactly what clipped the halo before.
    const int totalMeterWidth = StereoLevelMeterComponent::requiredWidth();
    int meterX = masterContent.getCentreX() - totalMeterWidth / 2;
    if (stereoLevelMeter != nullptr)
    {
        stereoLevelMeter->setBounds(meterX, masterCurrentY, totalMeterWidth, meterBarHeight);
        stereoLevelMeter->setVisible(true);
    }
    masterCurrentY += meterBarHeight + sectionGap;

    // Bend Range and Velocity, two to a row rather than a new line for Velocity.
    //
    // Everything else in Master is one control per line, but the box is height
    // matched to the Filter box beside it, and a sixth line would push Glide and
    // the Legato toggle past its bottom. Two 56 px knobs fit across 220 px with
    // room to spare, so the row splits instead.
    {
        // The pair is centred as a GROUP, with the same 10 px gap the Amp Envelope
        // leaves between Amount and Time, rather than each knob centred in its own
        // half of the box. Halving the box put these 94 px apart against that
        // row's 66, so the two did not read as the same kind of row.
        const int pairGap    = 10;                    // == pitchEnvGap on the Main page
        const int pairWidth  = 2 * knobDiameter + pairGap;
        const int pairLeft   = masterContent.getCentreX() - pairWidth / 2;
        const int bendCentre = pairLeft + knobDiameter / 2;
        const int velCentre  = pairLeft + knobDiameter + pairGap + knobDiameter / 2;
        const int labelW     = knobDiameter + pairGap;
        const int knobY      = masterCurrentY + labelHeight + labelGap;

        pitchBendAmountLabel.setBounds (bendCentre - labelW / 2, masterCurrentY, labelW, labelHeight);
        pitchBendAmountSlider.setBounds (bendCentre - knobDiameter / 2, knobY, knobDiameter, knobDiameter);

        velocityAmountLabel.setBounds (velCentre - labelW / 2, masterCurrentY, labelW, labelHeight);
        velocityAmountSlider.setBounds (velCentre - knobDiameter / 2, knobY, knobDiameter, knobDiameter);

        pitchBendAmountSlider.setVisible(true);
        pitchBendAmountLabel.setVisible(true);
        velocityAmountSlider.setVisible(true);
        velocityAmountLabel.setVisible(true);
    }
    masterCurrentY += knobGroupH + sectionGap;

    // Pitch Bend (label + vertical fader)
    pitchBendLabel.setBounds(pitchCentreX - 18, masterCurrentY, 36, labelHeight);
    pitchBendSlider.setBounds(pitchCentreX - pitchFaderWidth / 2, masterCurrentY + labelHeight + labelGap, pitchFaderWidth, pitchFaderHeight);
    pitchBendSlider.setVisible(true);
    pitchBendLabel.setVisible(true);
    masterCurrentY += faderGroupH + sectionGap;

    // Voice Mode (label + combo)
    int voiceModeX = masterContent.getCentreX() - comboWidth / 2;
    voiceModeLabel.setBounds(voiceModeX, masterCurrentY, comboWidth, labelHeight);
    voiceModeCombo.setBounds(voiceModeX, masterCurrentY + labelHeight + labelGap, comboWidth, comboHeight);
    voiceModeCombo.setVisible(true);
    voiceModeLabel.setVisible(true);
    masterCurrentY += comboGroupH + sectionGap;

    // Glide (label + rotary + value)
    glideTimeLabel.setBounds(masterKnobX, masterCurrentY, knobDiameter, labelHeight);
    glideTimeSlider.setBounds(masterKnobX, masterCurrentY + labelHeight + labelGap, knobDiameter, knobDiameter);
    glideTimeSlider.setVisible(true);
    glideTimeLabel.setVisible(true);
    masterCurrentY += knobGroupH + sectionGap;

    // Legato Glide toggle: only in Mono/Legato. Its presence (reflected in the
    // box height computed above) is what collapses Poly and expands the others.
    legatoGlideButton.setBounds(masterKnobX + (knobDiameter - 100) / 2, masterCurrentY, 100, legatoBtnH);
    legatoGlideButton.setVisible(showLegato);
    legatoGlideLabel.setVisible(false);
    
    DBG("Space Dust: resized() - Layout complete");
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception in resized(): " + juce::String(e.what()));
        #if JUCE_DEBUG
        try
        {
            juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(safeString("SpaceDust_DebugLog.txt"));
            juce::FileOutputStream out(logFile);
            if (out.openedOk())
            {
                out.setPosition(out.getPosition());
                out.writeText("Space Dust: Exception in resized(): " + juce::String(e.what()) + "\n", false, false, nullptr);
                out.flush();
            }
        }
        catch (...) {}
        #endif
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception in resized()");
        #if JUCE_DEBUG
        try
        {
            juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(safeString("SpaceDust_DebugLog.txt"));
            juce::FileOutputStream out(logFile);
            if (out.openedOk())
            {
                out.setPosition(out.getPosition());
                out.writeText("Space Dust: Unknown exception in resized()\n", false, false, nullptr);
                out.flush();
            }
        }
        catch (...) {}
        #endif
    }
}

