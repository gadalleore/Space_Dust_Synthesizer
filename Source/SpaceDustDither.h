/*
    SpaceDustDither.h
    -----------------
    The RGB motion smear from Sol Voice Tuner's SolDither, ported for Space Dust
    (2026-08-01). Only the streak is here -- Sol's file also carries a monochrome
    gradient stipple, film grain and a lens-dirt generator, none of which Space
    Dust uses.

    What it draws: three single-colour stipple stamps of a shape, offset from each
    other along the direction of travel, giving the chromatic separation old music
    visualisers had. Not a hue sweep -- the channels genuinely pull apart.

    Two things about it that are load-bearing, both learned in Sol:

      - The dither is ORDERED (an 8x8 Bayer matrix), not random noise. Random
        noise crawls between frames and reads as video static; an ordered matrix
        holds still, so a moving shape smears while the texture itself stays put.

      - Each channel gets a DIFFERENT SLICE of the matrix, so the three interleave
        instead of stacking back into white.

    Sol's version sits on a white plate. Space Dust's is near-black, so the stipple
    tints here are the brighter of the two options -- on a dark ground the smear has
    to be lighter than what it sits on to register at all.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace SpaceDustDither
{
    /** Classic 8x8 Bayer threshold matrix, normalised to 0..1. */
    inline float bayer(int x, int y) noexcept
    {
        static constexpr int m[8][8] = {
            {  0, 32,  8, 40,  2, 34, 10, 42 },
            { 48, 16, 56, 24, 50, 18, 58, 26 },
            { 12, 44,  4, 36, 14, 46,  6, 38 },
            { 60, 28, 52, 20, 62, 30, 54, 22 },
            {  3, 35, 11, 43,  1, 33,  9, 41 },
            { 51, 19, 59, 27, 49, 17, 57, 25 },
            { 15, 47,  7, 39, 13, 45,  5, 37 },
            { 63, 31, 55, 23, 61, 29, 53, 21 }
        };

        return (float) m[y & 7][x & 7] / 64.0f;
    }

    inline constexpr int kTileSize = 64;

    enum class Chan { r, g, b };

    /** The three stipple tiles, built once and shared.

        NOT function-local statics, which is how this was first written and which is
        a genuine hazard in a plugin: a static juce::Image lives until the DLL is
        UNLOADED, and by then JUCE's own shutdown has already run, so destroying it
        touches a library that is no longer there. That is a classic cause of a host
        hanging or crashing on quit, and Space Dust hung FL Studio on exit until this
        was changed (2026-08-01).

        Held instead through juce::SharedResourcePointer: the tiles are heap-allocated
        on first use and destroyed when the last holder goes away -- editor teardown,
        while JUCE is still alive. Every user keeps one as a member. */
    struct Tiles
    {
        Tiles()
        {
            auto build = [](juce::Colour tint, float lo, float hi)
            {
                juce::Image img(juce::Image::ARGB, kTileSize, kTileSize, true);
                juce::Image::BitmapData data(img, juce::Image::BitmapData::writeOnly);

                for (int y = 0; y < kTileSize; ++y)
                    for (int x = 0; x < kTileSize; ++x)
                    {
                        const float b  = bayer(x, y);
                        const bool  on = b >= lo && b < hi;

                        data.setPixelColour(x, y, on ? tint : juce::Colours::transparentBlack);
                    }

                return img;
            };

            red   = build(juce::Colour(0xffff4d4d), 0.00f, 0.34f);
            green = build(juce::Colour(0xff4dff7a), 0.34f, 0.67f);
            blue  = build(juce::Colour(0xff4d8bff), 0.67f, 1.00f);
        }

        const juce::Image& get(Chan c) const
        {
            switch (c)
            {
                case Chan::r: return red;
                case Chan::g: return green;
                case Chan::b: default: return blue;
            }
        }

        juce::Image red, green, blue;
    };

    /** What every user of this namespace should hold as a member. */
    using TilesPtr = juce::SharedResourcePointer<Tiles>;

    /** Tiles `tile` across `area` at `alpha`. Caller sets any clip first. */
    inline void tileOver(juce::Graphics& g, const juce::Image& tile,
                         juce::Rectangle<float> area, float alpha)
    {
        if (area.isEmpty() || alpha <= 0.0f)
            return;

        juce::Graphics::ScopedSaveState saved(g);
        g.setOpacity(juce::jlimit(0.0f, 1.0f, alpha));

        const int x0 = (int) std::floor(area.getX());
        const int y0 = (int) std::floor(area.getY());
        const int x1 = (int) std::ceil (area.getRight());
        const int y1 = (int) std::ceil (area.getBottom());

        for (int y = y0; y < y1; y += kTileSize)
            for (int x = x0; x < x1; x += kTileSize)
                g.drawImageAt(tile, x, y);
    }

    /** Stamps `shape`, displaced by `offset`, in one colour channel's stipple. */
    inline void fillPathChannel(juce::Graphics& g, const juce::Path& shape,
                                juce::Point<float> offset, Chan c, float alpha,
                                const Tiles& tiles)
    {
        if (shape.isEmpty() || alpha <= 0.0f)
            return;

        auto ghost = shape;

        if (! offset.isOrigin())
            ghost.applyTransform(juce::AffineTransform::translation(offset.x, offset.y));

        juce::Graphics::ScopedSaveState saved(g);
        g.reduceClipRegion(ghost);
        tileOver(g, tiles.get(c), ghost.getBounds(), alpha);
    }

    /** Ghost trail for a TRACE that redraws itself wholesale every frame -- the
        scopes, where there is no single moving "head" to streak because the entire
        line is somewhere new each time.

        Instead of stamping one shape back along its travel, this stamps the last
        few frames OF THE TRACE ITSELF, each nudged further out and each in a
        different channel, so successive sweeps leave interleaved red/green/blue
        rather than one flat grey ghost. Sol does the same on its Lissajous.

        `history` runs oldest-first. Pass unstroked paths; stroking happens here so
        the ghosts can be drawn a little heavier than the live trace. */
    inline void ghostTrail(juce::Graphics& g, const std::vector<juce::Path>& history,
                           float thickness, float spreadMax, float alpha,
                           const Tiles& tiles)
    {
        const int n = (int) history.size();

        if (n <= 0)
            return;

        static constexpr Chan order[] = { Chan::r, Chan::g, Chan::b };

        for (int h = 0; h < n; ++h)
        {
            const auto& trace = history[(size_t) h];

            if (trace.isEmpty())
                continue;

            juce::Path stroked;
            juce::PathStrokeType(thickness).createStrokedPath(stroked, trace);

            // Oldest is faintest and furthest out.
            const float recency = (float) (h + 1) / (float) n;
            const float spread  = (1.0f - recency) * spreadMax;

            fillPathChannel(g, stroked, { spread, -spread }, order[h % 3],
                            recency * alpha, tiles);
        }
    }

    /** Draws an RGB streak: `steps` stamps of `shape` spread evenly back along
        `displacement`, cycling red -> green -> blue and fading with distance.

        Separating three channels by a fraction of one frame's movement is
        invisible -- the separation has to span the whole travel. Interleaving the
        channels along the streak is what gives the smeared prism look rather than
        three tidy ghosts. */
    inline void streakRgb(juce::Graphics& g, const juce::Path& shape,
                          juce::Point<float> displacement, int steps, float alpha,
                          const Tiles& tiles)
    {
        if (shape.isEmpty() || steps <= 0 || alpha <= 0.0f)
            return;

        static constexpr Chan order[] = { Chan::r, Chan::g, Chan::b };

        // Furthest-back first, so nearer and brighter stamps land on top.
        for (int i = steps; i >= 1; --i)
        {
            const float t = (float) i / (float) steps;

            const juce::Point<float> offset { displacement.x * t, displacement.y * t };

            // Linear falloff with distance: far enough back to read as a tail,
            // without the aggressive curve that made it vanish entirely.
            fillPathChannel(g, shape, offset, order[i % 3], alpha * (1.0f - t * 0.75f), tiles);
        }
    }
}
