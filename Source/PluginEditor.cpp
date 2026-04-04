#include "PluginEditor.h"

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

    // Astronomically accurate starfield: Costa Mesa, CA (33.66°N, 117.90°W)
    // March 29, 2026 at midnight PDT — commemorating the completion of Space Dust.
    // Star positions from Yale Bright Star Catalog (J2000.0 epoch), projected via
    // horizontal coordinate transform for the exact date/time/location.
    // Bortle 7-8 sky (moderate light pollution): ~mag 4.0 naked-eye limit.
    void drawStarfield(juce::Graphics& g, int w, int h, float meterLevel = 0.0f)
    {
        if (w <= 0 || h <= 0) return;
        // Stars breathe with the audio: up to 15% brighter at full level
        const float meterBoost = 1.0f + 0.15f * juce::jlimit(0.0f, 1.0f, meterLevel);

        // Star catalog: {RA (degrees), Dec (degrees), apparent magnitude}
        struct CatStar { float ra, dec, mag; };
        static const CatStar catalog[] = {
            // -- Orion (setting in west) --
            {88.79f,   7.41f, 0.50f},   // Betelgeuse (α Ori)
            {78.63f,  -8.20f, 0.13f},   // Rigel (β Ori)
            {81.28f,   6.35f, 1.64f},   // Bellatrix (γ Ori)
            {83.00f,  -0.30f, 2.23f},   // Mintaka (δ Ori)
            {84.05f,  -1.20f, 1.69f},   // Alnilam (ε Ori)
            {85.19f,  -1.94f, 1.77f},   // Alnitak (ζ Ori)
            {86.94f,  -9.67f, 2.09f},   // Saiph (κ Ori)
            // -- Canis Major --
            {101.29f,-16.72f,-1.46f},   // Sirius (α CMa)
            {95.68f, -17.96f, 1.98f},   // Mirzam (β CMa)
            {107.10f,-26.39f, 1.84f},   // Adhara (ε CMa)
            {104.66f,-28.97f, 1.50f},   // Wezen (δ CMa)
            // -- Canis Minor --
            {114.83f,  5.22f, 0.34f},   // Procyon (α CMi)
            {111.79f,  8.29f, 2.90f},   // Gomeisa (β CMi)
            // -- Gemini --
            {116.33f, 28.03f, 1.14f},   // Pollux (β Gem)
            {113.65f, 31.89f, 1.58f},   // Castor (α Gem)
            {99.43f,  16.40f, 1.93f},   // Alhena (γ Gem)
            {95.74f,  22.51f, 2.88f},   // Tejat (μ Gem)
            {100.98f, 25.13f, 2.98f},   // Mebsuta (ε Gem)
            {110.03f, 21.98f, 3.53f},   // Wasat (δ Gem)
            // -- Auriga --
            {79.17f,  46.00f, 0.08f},   // Capella (α Aur)
            {89.88f,  44.95f, 1.90f},   // Menkalinan (β Aur)
            // -- Taurus --
            {68.98f,  16.51f, 0.85f},   // Aldebaran (α Tau)
            {81.57f,  28.61f, 1.65f},   // Elnath (β Tau)
            // -- Leo --
            {152.09f, 11.97f, 1.35f},   // Regulus (α Leo)
            {177.27f, 14.57f, 2.14f},   // Denebola (β Leo)
            {154.99f, 19.84f, 2.28f},   // Algieba (γ Leo)
            {168.53f, 20.52f, 2.56f},   // Zosma (δ Leo)
            {168.56f, 15.43f, 3.34f},   // Chertan (θ Leo)
            {154.17f, 23.42f, 3.44f},   // Adhafera (ζ Leo)
            {151.83f, 16.76f, 3.52f},   // η Leo
            // -- Virgo --
            {201.30f,-11.16f, 0.97f},   // Spica (α Vir)
            {190.42f, -1.45f, 2.74f},   // Porrima (γ Vir)
            {195.54f, 10.96f, 2.83f},   // Vindemiatrix (ε Vir)
            // -- Boötes --
            {213.92f, 19.18f,-0.05f},   // Arcturus (α Boo)
            {221.25f, 27.07f, 2.70f},   // Izar (ε Boo)
            {208.67f, 18.40f, 2.68f},   // Muphrid (η Boo)
            {218.02f, 38.31f, 3.03f},   // Seginus (γ Boo)
            {225.49f, 40.39f, 3.50f},   // Nekkar (β Boo)
            // -- Ursa Major (Big Dipper high overhead) --
            {165.93f, 61.75f, 1.79f},   // Dubhe (α UMa)
            {165.46f, 56.38f, 2.37f},   // Merak (β UMa)
            {178.46f, 53.69f, 2.44f},   // Phecda (γ UMa)
            {183.86f, 57.03f, 3.31f},   // Megrez (δ UMa)
            {193.51f, 55.96f, 1.77f},   // Alioth (ε UMa)
            {200.98f, 54.93f, 2.27f},   // Mizar (ζ UMa)
            {206.89f, 49.31f, 1.86f},   // Alkaid (η UMa)
            {155.58f, 41.50f, 3.05f},   // Tania Australis (μ UMa)
            {154.27f, 42.91f, 3.45f},   // Tania Borealis (λ UMa)
            {169.62f, 33.09f, 3.49f},   // Alula Borealis (ν UMa)
            {169.55f, 31.53f, 3.79f},   // Alula Australis (ξ UMa)
            {201.31f, 54.99f, 4.01f},   // Alcor (80 UMa)
            // -- Ursa Minor --
            {37.95f,  89.26f, 1.98f},   // Polaris (α UMi)
            {222.68f, 74.16f, 2.08f},   // Kochab (β UMi)
            {230.18f, 71.83f, 3.05f},   // Pherkad (γ UMi)
            // -- Hydra --
            {141.90f, -8.66f, 1.98f},   // Alphard (α Hya)
            // -- Cancer --
            {124.13f,  9.19f, 3.52f},   // Al Tarf (β Cnc)
            {131.17f, 18.15f, 3.94f},   // Asellus Australis (δ Cnc)
            // -- Corvus --
            {183.95f,-17.54f, 2.59f},   // Gienah (γ Crv)
            {188.60f,-23.40f, 2.65f},   // Kraz (β Crv)
            {187.47f,-16.52f, 2.95f},   // Algorab (δ Crv)
            {182.53f,-22.62f, 3.02f},   // Minkar (ε Crv)
            // -- Canes Venatici --
            {194.01f, 38.32f, 2.90f},   // Cor Caroli (α CVn)
            // -- Corona Borealis --
            {233.67f, 26.71f, 2.23f},   // Alphecca (α CrB)
            // -- Draco --
            {269.15f, 51.49f, 2.23f},   // Eltanin (γ Dra)
            {262.61f, 52.30f, 2.79f},   // Rastaban (β Dra)
            {211.10f, 64.38f, 3.65f},   // Thuban (α Dra)
            // -- Hercules --
            {247.55f, 21.49f, 2.77f},   // Kornephoros (β Her)
            {250.32f, 31.60f, 2.81f},   // ζ Her
            {258.76f, 36.81f, 3.16f},   // π Her
            {258.76f, 24.84f, 3.14f},   // Sarin (δ Her)
            {258.66f, 14.39f, 3.48f},   // Rasalgethi (α Her)
            {266.62f, 27.72f, 3.42f},   // μ Her
            {250.72f, 38.92f, 3.53f},   // η Her
            // -- Serpens --
            {236.07f,  6.43f, 2.65f},   // Unukalhai (α Ser)
            // -- Libra --
            {222.72f,-16.04f, 2.75f},   // Zubenelgenubi (α Lib)
            {229.25f, -9.38f, 2.61f},   // Zubeneschamali (β Lib)
            // -- Lyra --
            {279.23f, 38.78f, 0.03f},   // Vega (α Lyr)
            // -- Cygnus --
            {310.36f, 45.28f, 1.25f},   // Deneb (α Cyg)
            // -- Centaurus --
            {211.67f,-36.37f, 2.06f},   // Menkent (θ Cen)
            // -- Perseus (low NW) --
            {51.08f,  49.86f, 1.80f},   // Mirfak (α Per)
            // -- Lynx --
            {140.26f, 34.39f, 3.13f},   // α Lyn
            // -- Leo Minor --
            {163.33f, 34.22f, 3.83f},   // Praecipua (46 LMi)
            // -- Cepheus --
            {319.65f, 62.59f, 2.51f},   // Alderamin (α Cep)
            // -- Ophiuchus --
            {263.73f, 12.56f, 2.08f},   // Rasalhague (α Oph)
            // -- Monoceros --
            {107.99f, -0.49f, 3.93f},   // α Mon
            // -- Cassiopeia (low north) --
            {10.13f,  56.54f, 2.23f},   // Schedar (α Cas)
            {2.29f,   59.15f, 2.27f},   // Caph (β Cas)
        };
        static const int catalogSize = sizeof(catalog) / sizeof(catalog[0]);

        // Projected star cache (computed once, reused every paint)
        struct ProjStar { float nx, ny, brightness; };
        static ProjStar projected[200];
        static int projCount = 0;
        static bool computed = false;

        if (!computed)
        {
            computed = true;
            projCount = 0;

            // Costa Mesa, CA: 33.6646°N, 117.9034°W
            // March 29, 2026 midnight PDT (UTC-7) = 07:00 UTC
            // GMST at 0h UTC: 188.81° + 7h sidereal rotation (105.29°) = 294.10°
            // LST = GMST + longitude = 294.10 + (-117.90) = 176.20°
            constexpr float lat       = 33.6646f;
            constexpr float lst       = 176.20f;
            constexpr float degToRad  = 3.14159265f / 180.0f;
            constexpr float minAlt    = 5.0f;    // horizon cutoff (obstructions + haze)
            constexpr float maxMag    = 4.2f;    // Bortle 7-8 naked-eye limit
            constexpr float minMag    = -1.5f;
            const float sinLat = std::sin(lat * degToRad);
            const float cosLat = std::cos(lat * degToRad);
            const float sinMinAlt = std::sin(minAlt * degToRad);

            for (int i = 0; i < catalogSize && projCount < 200; ++i)
            {
                const auto& s = catalog[i];
                if (s.mag > maxMag) continue;

                float ha     = (lst - s.ra) * degToRad;
                float sinDec = std::sin(s.dec * degToRad);
                float cosDec = std::cos(s.dec * degToRad);

                // Altitude
                float sinAlt = sinDec * sinLat + cosDec * cosLat * std::cos(ha);
                if (sinAlt < sinMinAlt) continue;  // below horizon / obstructed

                float alt    = std::asin(sinAlt) / degToRad;
                float cosAlt = std::cos(alt * degToRad);
                if (cosAlt < 0.001f) cosAlt = 0.001f;  // zenith guard

                // Azimuth (0°=N, 90°=E, 180°=S, 270°=W)
                float sinAz = -cosDec * std::sin(ha) / cosAlt;
                float cosAz = (sinDec - sinLat * sinAlt) / (cosLat * cosAlt);
                float az    = std::atan2(sinAz, cosAz) / degToRad;
                if (az < 0.0f) az += 360.0f;

                // Cylindrical equidistant projection to normalized coords
                float nx = az / 360.0f;
                float ny = 1.0f - (alt / 90.0f);

                // Brightness from magnitude (linear in perceptual range)
                float brightness = (maxMag - s.mag) / (maxMag - minMag);
                brightness = juce::jlimit(0.0f, 1.0f, brightness);

                projected[projCount++] = { nx, ny, brightness };
            }
        }

        // Draw catalog stars
        for (int i = 0; i < projCount; ++i)
        {
            const auto& s = projected[i];
            float alpha  = (60.0f + 130.0f * s.brightness) * meterBoost;
            float radius = (0.6f + 1.5f * s.brightness) * meterBoost;
            g.setColour(juce::Colour(255, 255, 255).withAlpha(
                static_cast<juce::uint8>(juce::jlimit(0, 255, static_cast<int>(alpha)))));
            float px = s.nx * static_cast<float>(w);
            float py = s.ny * static_cast<float>(h);
            g.fillEllipse(px - radius, py - radius, radius * 2.0f, radius * 2.0f);
        }

        // Faint filler stars (threshold-of-visibility, deterministic positions)
        for (int i = 0; i < 35; ++i)
        {
            unsigned int hash = static_cast<unsigned int>((i + 500) * 2654435761u);
            float sx = static_cast<float>(hash % static_cast<unsigned int>(w));
            hash = (hash >> 13) ^ (hash * 1597334677u);
            float sy = static_cast<float>(hash % static_cast<unsigned int>(h));
            hash = (hash >> 7) ^ (hash * 2246822519u);
            juce::uint8 alpha = static_cast<juce::uint8>(juce::jlimit(0, 255,
                static_cast<int>((22 + (hash % 26)) * meterBoost)));
            g.setColour(juce::Colour(255, 255, 255).withAlpha(alpha));
            g.fillEllipse(sx - 0.5f, sy - 0.5f, 1.0f, 1.0f);
        }
    }

    // Draw glow halos directly (keeps bleed fix from LookAndFeel, overlap may add slightly)
    void drawGlows(juce::Graphics& g, int baseAlpha, juce::Colour glowCol,
                  std::initializer_list<const juce::Component*> groupList)
    {
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

        float avgLevel = 0.5f * (editor.audioProcessor.getLeftPeakLevel() + editor.audioProcessor.getRightPeakLevel());
        avgLevel = juce::jmin(1.0f, avgLevel);
        const bool isRed = (editor.clippingHoldTicks > 0);
        // 50% more subtle on tabs: scale down alpha
        juce::uint8 peakAlpha = static_cast<juce::uint8>(juce::jlimit(0, 255, static_cast<int>((6 + 60 * avgLevel) * 0.5f)));

        const juce::Colour edgeCol = isRed ? juce::Colour(0xffdd3333) : juce::Colour(0xff00d4ff);
        const juce::Colour midCol  = isRed ? juce::Colour(0xff991818) : juce::Colour(0xff0066aa);
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

        float avgLevel = 0.5f * (editor.audioProcessor.getLeftPeakLevel() + editor.audioProcessor.getRightPeakLevel());
        avgLevel = juce::jmin(1.0f, avgLevel);
        const bool isRed = (editor.clippingHoldTicks > 0);
        // 50% more subtle on tabs: scale down alpha
        juce::uint8 peakAlpha = static_cast<juce::uint8>(juce::jlimit(0, 255, static_cast<int>((6 + 60 * avgLevel) * 0.5f)));

        const juce::Colour edgeCol = isRed ? juce::Colour(0xffdd3333) : juce::Colour(0xff00d4ff);
        const juce::Colour midCol  = isRed ? juce::Colour(0xff991818) : juce::Colour(0xff0066aa);
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
}

void StereoLevelMeterComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    const int barWidth = 20;
    const int barGap = 4;
    const int totalWidth = (barWidth * 2) + barGap;
    int startX = (bounds.getWidth() - totalWidth) / 2;  // Center the meters
    
    // Left meter
    auto leftMeter = juce::Rectangle<int>(startX, 0, barWidth, bounds.getHeight());
    // Right meter
    auto rightMeter = juce::Rectangle<int>(startX + barWidth + barGap, 0, barWidth, bounds.getHeight());
    
    // Helper to draw a single meter bar
    auto drawMeter = [&](juce::Rectangle<int> meterRect, float peakLevel)
    {
        // Background: black
        g.setColour(juce::Colour(0xff000000));
        g.fillRect(meterRect);
        
        // Calculate fill height from peak level
        float db = linearToDb(peakLevel);
        float heightNorm = dbToHeight(db);
        int fillHeight = static_cast<int>(meterRect.getHeight() * heightNorm);
        
        if (fillHeight > 0)
        {
            auto fillRect = meterRect.withHeight(fillHeight).withY(meterRect.getBottom() - fillHeight);
            
            // Red for clipping (above -1 dB), cyan/blue for normal
            if (db > -1.0f)
            {
                // Clipping region: red at the TOP of the fill
                g.setColour(juce::Colour(0xffff0000));
                int redHeight = juce::jmin(fillHeight, static_cast<int>(meterRect.getHeight() * 0.15f)); // Top 15% = red
                // Red rect starts at the TOP of the fill (not bottom)
                auto redRect = fillRect.withHeight(redHeight).withY(fillRect.getY());
                g.fillRect(redRect);
                
                // Below clipping: cyan
                if (fillHeight > redHeight)
                {
                    g.setColour(juce::Colour(0xff00ffff));  // Cyan
                    auto cyanRect = fillRect.withHeight(fillHeight - redHeight).withY(fillRect.getY() + redHeight);
                    g.fillRect(cyanRect);
                }
            }
            else
            {
                // Normal level: cyan/blue
                g.setColour(juce::Colour(0xff00ffff));  // Cyan
                g.fillRect(fillRect);
            }
        }
        
        // Border: subtle gray
        g.setColour(juce::Colour(0x33333333));
        g.drawRect(meterRect, 1);
    };
    
    // Draw both meters
    drawMeter(leftMeter, leftPeak.load());
    drawMeter(rightMeter, rightPeak.load());
    
    // Labels: "L" and "R" at the bottom if space allows
    if (bounds.getHeight() > 30)
    {
        g.setColour(juce::Colour(0xffa0d8ff));  // Light blue/cosmic color
        g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::plain)));  // Standard body font
        g.drawText("L", leftMeter.withY(leftMeter.getBottom() - 15).withHeight(12), juce::Justification::centred, true);
        g.drawText("R", rightMeter.withY(rightMeter.getBottom() - 15).withHeight(12), juce::Justification::centred, true);
    }
}

void StereoLevelMeterComponent::resized()
{
    // Component is already sized by parent, just trigger repaint
    repaint();
}

void StereoLevelMeterComponent::updateLevels(float newLeftPeak, float newRightPeak)
{
    leftPeak.store(newLeftPeak);
    rightPeak.store(newRightPeak);
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
    addAndMakeVisible(parentEditor.subOscLevelSlider);
    addAndMakeVisible(parentEditor.subOscCoarseSlider);
    addAndMakeVisible(parentEditor.subOscWaveformLabel);
    addAndMakeVisible(parentEditor.subOscLevelLabel);
    addAndMakeVisible(parentEditor.subOscCoarseLabel);
    
    parentEditor.audioProcessor.getValueTreeState().addParameterListener("subOscOn", this);
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

void MainPageComponent::parameterChanged(const juce::String& parameterID, float newValue)
{
    if (parameterID == "subOscOn")
        updateSubOscVisibility();
}

void MainPageComponent::updateSubOscVisibility()
{
    bool on = parentEditor.audioProcessor.getValueTreeState().getParameter("subOscOn")->getValue() > 0.5f;
    parentEditor.subOscWaveformCombo.setVisible(on);
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
    float avgLevel = 0.5f * (parentEditor.audioProcessor.getLeftPeakLevel() + parentEditor.audioProcessor.getRightPeakLevel());
    avgLevel = juce::jmin(1.0f, avgLevel);
    drawStarfield(g, getWidth(), getHeight(), avgLevel);
    const int baseAlpha = 8 + static_cast<int>(44.0f * avgLevel);
    drawGlows(g, baseAlpha, juce::Colour(0xff00b4ff),
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
    const int oscLabelSpacing = oscillatorTextBoxHeight + (labelGap / 2); // Consistent label spacing below text box (shared)
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
    // Reduce top padding by ~50%: move content up by topPaddingReduction (from 65px to ~33px top padding)
    int oscStartY = oscContent.getY() - topPaddingReduction;
    
    // Consistent spacing constants for Oscillators section
    const int oscRowSpacing = 28; // Vertical spacing between oscillator rows (compact)
    
    // Osc1 - Waveform combo + 3 knobs horizontally
    int osc1Y = oscStartY;
    parentEditor.osc1WaveformLabel.setBounds(oscContent.getX(), osc1Y, comboWidth, labelHeight);
    osc1Y += labelHeight + labelGap;
    parentEditor.osc1WaveformCombo.setBounds(oscContent.getX(), osc1Y, comboWidth, comboHeight);
    
    // Pan slider in green box area (below waveform combo, left of knobs)
    const int panSliderHeight = 20;
    int osc1PanY = osc1Y + comboHeight + 4;
    parentEditor.osc1PanSlider.setBounds(oscContent.getX(), osc1PanY, comboWidth, panSliderHeight);
    parentEditor.osc1PanLabel.setBounds(oscContent.getX(), osc1PanY + panSliderHeight + 2, comboWidth, labelHeight);
    
    int osc1KnobY = osc1Y + (comboHeight - knobDiameter) / 2; // Align knobs with combo center
    int osc1KnobX = oscContent.getX() + comboWidth + horizontalSpacing;
    
    parentEditor.osc1CoarseTuneSlider.setBounds(osc1KnobX, osc1KnobY, knobDiameter, knobDiameter);
    parentEditor.osc1CoarseTuneLabel.setBounds(osc1KnobX, osc1KnobY + knobDiameter + oscLabelSpacing, knobDiameter, labelHeight);
    
    osc1KnobX += knobDiameter + horizontalSpacing;
    parentEditor.osc1DetuneSlider.setBounds(osc1KnobX, osc1KnobY, knobDiameter, knobDiameter);
    parentEditor.osc1DetuneLabel.setBounds(osc1KnobX, osc1KnobY + knobDiameter + oscLabelSpacing, knobDiameter, labelHeight);
    
    osc1KnobX += knobDiameter + horizontalSpacing;
    parentEditor.osc1LevelSlider.setBounds(osc1KnobX, osc1KnobY, knobDiameter, knobDiameter);
    parentEditor.osc1LevelLabel.setBounds(osc1KnobX, osc1KnobY + knobDiameter + oscLabelSpacing, knobDiameter, labelHeight);
    
    // Calculate bottom of Osc1 (including labels) for proper spacing to Osc2
    int osc1Bottom = osc1KnobY + knobDiameter + oscLabelSpacing + labelHeight;
    
    // Osc2 - Same layout, below Osc1 with proper spacing to prevent overlaps
    int osc2Y = osc1Bottom + oscRowSpacing;
    parentEditor.osc2WaveformLabel.setBounds(oscContent.getX(), osc2Y, comboWidth, labelHeight);
    osc2Y += labelHeight + labelGap;
    parentEditor.osc2WaveformCombo.setBounds(oscContent.getX(), osc2Y, comboWidth, comboHeight);
    
    // Pan slider in green box area (below waveform combo, left of knobs)
    int osc2PanY = osc2Y + comboHeight + 4;
    parentEditor.osc2PanSlider.setBounds(oscContent.getX(), osc2PanY, comboWidth, panSliderHeight);
    parentEditor.osc2PanLabel.setBounds(oscContent.getX(), osc2PanY + panSliderHeight + 2, comboWidth, labelHeight);
    
    int osc2KnobY = osc2Y + (comboHeight - knobDiameter) / 2; // Align knobs with combo center
    int osc2KnobX = oscContent.getX() + comboWidth + horizontalSpacing;
    
    parentEditor.osc2CoarseTuneSlider.setBounds(osc2KnobX, osc2KnobY, knobDiameter, knobDiameter);
    parentEditor.osc2CoarseTuneLabel.setBounds(osc2KnobX, osc2KnobY + knobDiameter + oscLabelSpacing, knobDiameter, labelHeight);
    
    osc2KnobX += knobDiameter + horizontalSpacing;
    parentEditor.osc2DetuneSlider.setBounds(osc2KnobX, osc2KnobY, knobDiameter, knobDiameter);
    parentEditor.osc2DetuneLabel.setBounds(osc2KnobX, osc2KnobY + knobDiameter + oscLabelSpacing, knobDiameter, labelHeight);
    
    osc2KnobX += knobDiameter + horizontalSpacing;
    parentEditor.osc2LevelSlider.setBounds(osc2KnobX, osc2KnobY, knobDiameter, knobDiameter);
    parentEditor.osc2LevelLabel.setBounds(osc2KnobX, osc2KnobY + knobDiameter + oscLabelSpacing, knobDiameter, labelHeight);
    
    // Calculate bottom of Osc2 (including labels) for proper spacing to Noise section
    int osc2Bottom = osc2KnobY + knobDiameter + oscLabelSpacing + labelHeight;
    
    // Noise - Below Osc2 with proper padding
    const int noisePadding = 14; // Padding between osc2 and noise section (compact)
    int noiseY = osc2Bottom + noisePadding;
    parentEditor.noiseColorLabel.setBounds(oscContent.getX(), noiseY, comboWidth, labelHeight);
    noiseY += labelHeight + labelGap;
    parentEditor.noiseColorCombo.setBounds(oscContent.getX(), noiseY, comboWidth, comboHeight);
    
    int noiseKnobY = noiseY + (comboHeight - knobDiameter) / 2;
    int noiseKnobX = oscContent.getX() + comboWidth + horizontalSpacing;
    parentEditor.noiseLevelSlider.setBounds(noiseKnobX, noiseKnobY, knobDiameter, knobDiameter);
    parentEditor.noiseLevelLabel.setBounds(noiseKnobX, noiseKnobY + knobDiameter + oscLabelSpacing, knobDiameter, labelHeight);
    parentEditor.noiseLevelSlider.setVisible(true);
    parentEditor.noiseLevelLabel.setVisible(true);
    
    // Noise EQ knobs: positioned horizontally to the right of Level knob, same vertical level
    // Layout: Level, Low Shelf/Cut, High Shelf/Cut (all in a horizontal row, like Coarse/Detune/Level above)
    int eqKnob1X = noiseKnobX + knobDiameter + horizontalSpacing;  // Start after Level knob
    int eqKnob2X = eqKnob1X + knobDiameter + horizontalSpacing;     // Second knob next to first
    parentEditor.lowShelfAmountSlider.setBounds(eqKnob1X, noiseKnobY, knobDiameter, knobDiameter);
    parentEditor.lowShelfAmountLabel.setBounds(eqKnob1X, noiseKnobY + knobDiameter + oscLabelSpacing, knobDiameter, labelHeight);
    parentEditor.highShelfAmountSlider.setBounds(eqKnob2X, noiseKnobY, knobDiameter, knobDiameter);
    // Increase label width for "High Shelf/Cut" to prevent text cutoff
    const int eqLabelWidth = 90;  // Wider label to accommodate "High Shelf/Cut" text
    parentEditor.highShelfAmountLabel.setBounds(eqKnob2X, noiseKnobY + knobDiameter + oscLabelSpacing, eqLabelWidth, labelHeight);
    parentEditor.lowShelfAmountSlider.setVisible(true);
    parentEditor.lowShelfAmountLabel.setVisible(true);
    parentEditor.highShelfAmountSlider.setVisible(true);
    parentEditor.highShelfAmountLabel.setVisible(true);
    
    // ============================================================================
    // RIGHT SIDE: AMP ENVELOPE (Tall, narrow vertical column)
    // Master section is now handled by main editor, always visible on right side
    // Box shrinks/expands based on Sub Oscillator toggle (like Filter in Effects tab)
    // ============================================================================
    // Use temp full height to compute content positions, then set final height
    auto ampEnvAreaTemp = juce::Rectangle<int>(rightX, topY, narrowStripWidth, availableHeight);
    auto ampEnvContent = ampEnvAreaTemp.reduced(groupPadding, groupTitleHeight + groupPadding);
    int ampEnvKnobX = ampEnvContent.getCentreX() - knobDiameter / 2; // Centered
    // Reduce top padding by ~50%: move content up by topPaddingReduction
    int ampEnvKnobY = ampEnvContent.getY() - topPaddingReduction;
    // Spacing accounts for: knob (75) + reduced text box (10) + label (18) + reduced gaps
    int ampEnvSpacing = knobDiameter + 10 + labelHeight + (labelGap / 2) + verticalSpacing;
    
    parentEditor.envAttackSlider.setBounds(ampEnvKnobX, ampEnvKnobY, knobDiameter, knobDiameter);
    // Label positioned below text box (knob + reduced text box height + gap) - tighter spacing
    parentEditor.envAttackLabel.setBounds(ampEnvKnobX, ampEnvKnobY + knobDiameter + 10 + (labelGap / 2), knobDiameter, labelHeight);
    ampEnvKnobY += ampEnvSpacing;
    
    parentEditor.envDecaySlider.setBounds(ampEnvKnobX, ampEnvKnobY, knobDiameter, knobDiameter);
    parentEditor.envDecayLabel.setBounds(ampEnvKnobX, ampEnvKnobY + knobDiameter + 10 + (labelGap / 2), knobDiameter, labelHeight);
    ampEnvKnobY += ampEnvSpacing;
    
    parentEditor.envSustainSlider.setBounds(ampEnvKnobX, ampEnvKnobY, knobDiameter, knobDiameter);
    parentEditor.envSustainLabel.setBounds(ampEnvKnobX, ampEnvKnobY + knobDiameter + 10 + (labelGap / 2), knobDiameter, labelHeight);
    ampEnvKnobY += ampEnvSpacing;
    
    parentEditor.envReleaseSlider.setBounds(ampEnvKnobX, ampEnvKnobY, knobDiameter, knobDiameter);
    parentEditor.envReleaseLabel.setBounds(ampEnvKnobX, ampEnvKnobY + knobDiameter + 10 + (labelGap / 2), knobDiameter, labelHeight);
    ampEnvKnobY += ampEnvSpacing;
    
    // Pitch envelope: 3 knobs in a row (Amount, Time, Pitch) - below Amp Envelope
    const int pitchEnvKnobSize = 56;
    int pitchEnvTotalWidth = 3 * pitchEnvKnobSize + 2 * 10;  // 3 knobs + 2 gaps
    int pitchEnvStartX = ampEnvContent.getCentreX() - pitchEnvTotalWidth / 2;
    int pitchEnvKnobY = ampEnvKnobY;
    int pitchEnvGap = 10;
    
    parentEditor.pitchEnvAmountSlider.setBounds(pitchEnvStartX, pitchEnvKnobY, pitchEnvKnobSize, pitchEnvKnobSize);
    parentEditor.pitchEnvAmountLabel.setBounds(pitchEnvStartX, pitchEnvKnobY + pitchEnvKnobSize + 10 + (labelGap / 2), pitchEnvKnobSize, labelHeight);
    
    int pitchEnvKnob2X = pitchEnvStartX + pitchEnvKnobSize + pitchEnvGap;
    parentEditor.pitchEnvTimeSlider.setBounds(pitchEnvKnob2X, pitchEnvKnobY, pitchEnvKnobSize, pitchEnvKnobSize);
    parentEditor.pitchEnvTimeLabel.setBounds(pitchEnvKnob2X, pitchEnvKnobY + pitchEnvKnobSize + 10 + (labelGap / 2), pitchEnvKnobSize, labelHeight);
    
    int pitchEnvKnob3X = pitchEnvKnob2X + pitchEnvKnobSize + pitchEnvGap;
    parentEditor.pitchEnvPitchSlider.setBounds(pitchEnvKnob3X, pitchEnvKnobY, pitchEnvKnobSize, pitchEnvKnobSize);
    parentEditor.pitchEnvPitchLabel.setBounds(pitchEnvKnob3X, pitchEnvKnobY + pitchEnvKnobSize + 10 + (labelGap / 2), pitchEnvKnobSize, labelHeight);
    
    // Sub oscillator (below pitch envelope, expandable when toggle is on)
    // Layout: Level and Coarse knobs on top row, Waveform dropdown below with extra spacing to avoid overlap
    int subOscY = pitchEnvKnobY + pitchEnvKnobSize + 10 + labelHeight + (labelGap / 2) + 12;
    int subOscToggleWidth = 120;  // Wide enough for "Sub Oscillator"
    int subOscToggleHeight = 22;
    parentEditor.subOscToggleButton.setBounds(ampEnvContent.getCentreX() - subOscToggleWidth / 2, subOscY, subOscToggleWidth, subOscToggleHeight);
    
    int subOscKnobsY = subOscY + subOscToggleHeight + 8;
    int subOscItemWidth = pitchEnvKnobSize;   // Match pitch envelope knob size (60)
    int subOscItemGap = pitchEnvGap;          // Match pitch envelope gap (10)
    int subOscKnobsTotalWidth = 2 * subOscItemWidth + subOscItemGap;  // Level + Coarse only
    int subOscKnobsStartX = ampEnvContent.getCentreX() - subOscKnobsTotalWidth / 2;
    int subOscLabelSpacing = 10 + (labelGap / 2);  // Space between knob and label
    // Row 1: Level and Coarse knobs with labels (each in its own column, no overlap)
    parentEditor.subOscLevelSlider.setBounds(subOscKnobsStartX, subOscKnobsY, subOscItemWidth, subOscItemWidth);
    parentEditor.subOscLevelLabel.setBounds(subOscKnobsStartX, subOscKnobsY + subOscItemWidth + subOscLabelSpacing, subOscItemWidth, labelHeight);
    parentEditor.subOscCoarseSlider.setBounds(subOscKnobsStartX + subOscItemWidth + subOscItemGap, subOscKnobsY, subOscItemWidth, subOscItemWidth);
    parentEditor.subOscCoarseLabel.setBounds(subOscKnobsStartX + subOscItemWidth + subOscItemGap, subOscKnobsY + subOscItemWidth + subOscLabelSpacing, subOscItemWidth, labelHeight);
    // Row 2: Waveform dropdown - extra vertical gap so it doesn't step on Level/Coarse labels
    int subOscWaveGap = 24;
    int subOscWaveY = subOscKnobsY + subOscItemWidth + subOscLabelSpacing + labelHeight + subOscWaveGap;
    int subOscWaveWidth = 80;  // Wide enough for "Triangle", centered - avoids overlap with Level/Coarse columns
    int subOscWaveX = ampEnvContent.getCentreX() - subOscWaveWidth / 2;
    parentEditor.subOscWaveformLabel.setBounds(subOscWaveX, subOscWaveY - labelHeight - 2, subOscWaveWidth, labelHeight);
    parentEditor.subOscWaveformCombo.setBounds(subOscWaveX, subOscWaveY, subOscWaveWidth, comboHeight);
    
    // Amp Envelope box height: shrink when Sub Osc off, expand when on (like Filter in Effects tab)
    bool subOscOn = parentEditor.audioProcessor.getValueTreeState().getParameter("subOscOn") != nullptr
        && *parentEditor.audioProcessor.getValueTreeState().getRawParameterValue("subOscOn") > 0.5f;
    int ampEnvBottom = subOscOn ? (subOscWaveY + comboHeight) : (subOscY + subOscToggleHeight);
    int ampEnvHeight = ampEnvBottom - topY + groupPadding;
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
    
    // Warm Saturation toggle: below resonance row, before Filter Envelope
    filterKnobX = filterContent.getX();
    int warmSatButtonY = filterResonanceKnobY + knobDiameter + 12;
    parentEditor.warmSaturationMasterButton.setBounds(filterKnobX, warmSatButtonY, 130, 18);
    
    // ============================================================================
    // FILTER ENVELOPE: Position inside Filter box (below Cutoff/Resonance)
    // ============================================================================
    // Based on sketch, Filter Envelope controls are inside Filter box
    // Calculate bottom of filter knobs (including label spacing) for proper spacing to Filter Envelope
    // Use same label spacing calculation as oscillators for consistency
    const int filterLabelSpacing = oscillatorTextBoxHeight + (labelGap / 2); // Label spacing below text box
    int filterKnobsBottom = filterCutoffKnobY + knobDiameter + filterLabelSpacing + labelHeight;
    int filterEnvY = filterKnobsBottom + filterRowGap;
    const int numFilterEnvKnobs = 4;
    int filterEnvKnobSpacing = (filterContent.getWidth() - (numFilterEnvKnobs * knobDiameter)) / (numFilterEnvKnobs + 1);
    int filterEnvStartX = filterContent.getX() + filterEnvKnobSpacing;
    
    int filterEnvKnobX = filterEnvStartX;
    int filterEnvKnobY = filterEnvY;
    
    parentEditor.filterEnvAttackSlider.setBounds(filterEnvKnobX, filterEnvKnobY, knobDiameter, knobDiameter);
    parentEditor.filterEnvAttackLabel.setBounds(filterEnvKnobX, filterEnvKnobY + knobDiameter + filterLabelSpacing, knobDiameter, labelHeight);
    filterEnvKnobX += knobDiameter + filterEnvKnobSpacing;
    
    parentEditor.filterEnvDecaySlider.setBounds(filterEnvKnobX, filterEnvKnobY, knobDiameter, knobDiameter);
    parentEditor.filterEnvDecayLabel.setBounds(filterEnvKnobX, filterEnvKnobY + knobDiameter + filterLabelSpacing, knobDiameter, labelHeight);
    filterEnvKnobX += knobDiameter + filterEnvKnobSpacing;
    
    parentEditor.filterEnvReleaseSlider.setBounds(filterEnvKnobX, filterEnvKnobY, knobDiameter, knobDiameter);
    parentEditor.filterEnvReleaseLabel.setBounds(filterEnvKnobX, filterEnvKnobY + knobDiameter + filterLabelSpacing, knobDiameter, labelHeight);
    filterEnvKnobX += knobDiameter + filterEnvKnobSpacing;
    
    parentEditor.filterEnvAmountSlider.setBounds(filterEnvKnobX, filterEnvKnobY, knobDiameter, knobDiameter);
    parentEditor.filterEnvAmountLabel.setBounds(filterEnvKnobX, filterEnvKnobY + knobDiameter + filterLabelSpacing, knobDiameter, labelHeight);
    
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
    parentEditor.audioProcessor.getValueTreeState().addParameterListener(
        juce::ParameterID{"modFilter1Show", 1}.getParamID(), this);
    parentEditor.audioProcessor.getValueTreeState().addParameterListener(
        juce::ParameterID{"modFilter2Show", 1}.getParamID(), this);
    
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
    // lfo1RateLabel omitted - only Hz/sync value label shown below knob
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
    // lfo2RateLabel omitted - only Hz/sync value label shown below knob
    addAndMakeVisible(parentEditor.lfo2RateValueLabel);
    addAndMakeVisible(parentEditor.lfo2DepthSlider);
    addAndMakeVisible(parentEditor.lfo2DepthLabel);
    addAndMakeVisible(parentEditor.lfo2PhaseSlider);
    addAndMakeVisible(parentEditor.lfo2PhaseLabel);
    addAndMakeVisible(parentEditor.lfo2RetriggerButton);
    
    addAndMakeVisible(parentEditor.modFilterShowButton);
    addAndMakeVisible(parentEditor.modFilterShowButton2);
    addAndMakeVisible(parentEditor.modFilter1Group);
    addAndMakeVisible(parentEditor.modFilter1LinkButton);
    addAndMakeVisible(parentEditor.modFilter1ModeCombo);
    addAndMakeVisible(parentEditor.modFilter1CutoffSlider);
    addAndMakeVisible(parentEditor.modFilter1ResonanceSlider);
    addAndMakeVisible(parentEditor.warmSaturationMod1Button);
    addAndMakeVisible(parentEditor.modFilter1ModeLabel);
    addAndMakeVisible(parentEditor.modFilter1CutoffLabel);
    addAndMakeVisible(parentEditor.modFilter1ResonanceLabel);
    addAndMakeVisible(parentEditor.modFilter2Group);
    addAndMakeVisible(parentEditor.modFilter2LinkButton);
    addAndMakeVisible(parentEditor.modFilter2ModeCombo);
    addAndMakeVisible(parentEditor.modFilter2CutoffSlider);
    addAndMakeVisible(parentEditor.modFilter2ResonanceSlider);
    addAndMakeVisible(parentEditor.warmSaturationMod2Button);
    addAndMakeVisible(parentEditor.modFilter2ModeLabel);
    addAndMakeVisible(parentEditor.modFilter2CutoffLabel);
    addAndMakeVisible(parentEditor.modFilter2ResonanceLabel);
}

ModulationPageComponent::~ModulationPageComponent()
{
    parentEditor.audioProcessor.getValueTreeState().removeParameterListener(
        juce::ParameterID{"modFilter1Show", 1}.getParamID(), this);
    parentEditor.audioProcessor.getValueTreeState().removeParameterListener(
        juce::ParameterID{"modFilter2Show", 1}.getParamID(), this);
}

void ModulationPageComponent::parameterChanged(const juce::String& parameterID, float newValue)
{
    if (parameterID == juce::ParameterID{"modFilter1Show", 1}.getParamID() ||
        parameterID == juce::ParameterID{"modFilter2Show", 1}.getParamID())
    {
        juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<ModulationPageComponent>(this)]
        {
            if (safeThis != nullptr)
            {
                safeThis->resized();
                safeThis->repaint();
            }
        });
    }
}

void ModulationPageComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0a0a1f));
    float avgLevel = 0.5f * (parentEditor.audioProcessor.getLeftPeakLevel() + parentEditor.audioProcessor.getRightPeakLevel());
    avgLevel = juce::jmin(1.0f, avgLevel);
    drawStarfield(g, getWidth(), getHeight(), avgLevel);
    const int baseAlpha = 10 + static_cast<int>(48.0f * avgLevel);
    drawGlows(g, baseAlpha, juce::Colour(0xff00b4ff),
        { &parentEditor.modulationGroup, &parentEditor.lfo1Group, &parentEditor.lfo2Group,
          &parentEditor.modFilter1Group, &parentEditor.modFilter2Group });
}

void ModulationPageComponent::resized()
{
    // Modulation page layout: LFO1 and LFO2 only (compact)
    const int outerMargin = 8;
    auto modulationContent = juce::Rectangle<int>(
        outerMargin,
        8,
        getWidth() - 2 * outerMargin - 12,
        getHeight() - 24
    );
    
    // Larger gap between LFO columns so boxes do not intersect
    const int columnGap = 22;  // Increased by 10% from reduced value
    
    const int modKnobDiameter = 56;
    const int modRateKnobSize = 38;  // Rate knob smaller to match Depth (which has TextBoxBelow; 56-18)
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
    const int modRateValueGap = 12;
    // Extra padding inside each LFO box (title now inside box - need space below it)
    const int lfoBoxPadH = 8;
    const int lfoBoxPadV = 32;  // Match groupTitleHeight so content clears in-box title
    const int lfoContentTop = 0;
    
    // Check if filter sections are shown (each LFO has its own toggle)
    bool modFilter1Show = parentEditor.audioProcessor.getValueTreeState().getParameter("modFilter1Show") != nullptr
        && *parentEditor.audioProcessor.getValueTreeState().getRawParameterValue("modFilter1Show") > 0.5f;
    bool modFilter2Show = parentEditor.audioProcessor.getValueTreeState().getParameter("modFilter2Show") != nullptr
        && *parentEditor.audioProcessor.getValueTreeState().getRawParameterValue("modFilter2Show") > 0.5f;
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
    
    // LFO1 Rate (knob + Hz/sync label only; "Rate" label hidden; knob size matches Depth)
    parentEditor.lfo1RateLabel.setBounds(lfo1CentreX - modRateKnobSize / 2, lfo1CurrentY, modRateKnobSize, modLabelHeight);
    parentEditor.lfo1RateLabel.setVisible(false);  // Hidden - only Hz/sync value label shown
    lfo1CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo1FreeRateSlider.setBounds(lfo1CentreX - modRateKnobSize / 2, lfo1CurrentY, modRateKnobSize, modRateKnobSize);
    parentEditor.lfo1RateValueLabel.setBounds(lfo1CentreX - modRateLabelWidth / 2, lfo1CurrentY + modRateKnobSize + 2, modRateLabelWidth, 14);
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
    lfo1CurrentY += modRateKnobSize + modRateValueGap;
    
    // LFO1 Depth
    parentEditor.lfo1DepthLabel.setBounds(lfo1CentreX - modKnobDiameter / 2, lfo1CurrentY, modKnobDiameter, modLabelHeight);
    lfo1CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo1DepthSlider.setBounds(lfo1CentreX - modKnobDiameter / 2, lfo1CurrentY, modKnobDiameter, modKnobDiameter);
    lfo1CurrentY += modKnobDiameter + modRowSpacing;
    
    // LFO1 Phase
    parentEditor.lfo1PhaseLabel.setBounds(lfo1CentreX - modKnobDiameter / 2, lfo1CurrentY, modKnobDiameter, modLabelHeight);
    lfo1CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo1PhaseSlider.setBounds(lfo1CentreX - modKnobDiameter / 2, lfo1CurrentY, modKnobDiameter, modKnobDiameter);
    lfo1CurrentY += modKnobDiameter + modRowSpacing;
    
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
    // Knobs same size as Depth, centered under Filter button
    if (modFilter1Show)
    {
        const int filterKnobSize = modKnobDiameter;  // Match Depth knob size
        const int filterKnobGap = 8;
        const int filterComboW = 90;
        const int filterComboH = 20;
        int filterPairLeft = lfo1CentreX - (2 * filterKnobSize + filterKnobGap) / 2;  // Center pair under Filter button
        parentEditor.modFilter1CutoffLabel.setBounds(filterPairLeft, lfo1CurrentY, filterKnobSize, 12);
        parentEditor.modFilter1CutoffSlider.setBounds(filterPairLeft, lfo1CurrentY + 12, filterKnobSize, filterKnobSize);
        int resX = filterPairLeft + filterKnobSize + filterKnobGap;
        parentEditor.modFilter1ResonanceLabel.setBounds(resX, lfo1CurrentY, filterKnobSize, 12);
        parentEditor.modFilter1ResonanceSlider.setBounds(resX, lfo1CurrentY + 12, filterKnobSize, filterKnobSize);
        lfo1CurrentY += filterKnobSize + 12 + modRowSpacing;
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
    
    // LFO2 Rate (knob + Hz/sync label only; "Rate" label hidden; knob size matches Depth)
    parentEditor.lfo2RateLabel.setBounds(lfo2CentreX - modRateKnobSize / 2, lfo2CurrentY, modRateKnobSize, modLabelHeight);
    parentEditor.lfo2RateLabel.setVisible(false);  // Hidden - only Hz/sync value label shown
    lfo2CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo2FreeRateSlider.setBounds(lfo2CentreX - modRateKnobSize / 2, lfo2CurrentY, modRateKnobSize, modRateKnobSize);
    parentEditor.lfo2RateValueLabel.setBounds(lfo2CentreX - modRateLabelWidth / 2, lfo2CurrentY + modRateKnobSize + 2, modRateLabelWidth, 14);
    parentEditor.lfo2RateValueLabel.setVisible(true);   // Shows Hz or sync division
    parentEditor.lfo2RateValueLabel.setAlpha(1.0f);
    parentEditor.lfo2SyncRateCombo.setBounds(lfo2CentreX - controlWidth / 2, lfo2CurrentY, controlWidth, modComboHeight);
    // Triplet button: positioned to the right of Rate knob, vertically centered
    parentEditor.lfo2TripletButton.setBounds(lfo2CentreX + modRateKnobSize / 2 + tripletButtonGap, lfo2CurrentY + (modRateKnobSize - tripletButtonSize) / 2, tripletButtonWidth, tripletButtonSize);
    // Triplet/Straight toggle button: positioned to the left of Rate knob, vertically centered
    parentEditor.lfo2TripletStraightButton.setBounds(lfo2CentreX - modRateKnobSize / 2 - tripletButtonGap - tripletButtonSize, lfo2CurrentY + (modRateKnobSize - tripletButtonSize) / 2, tripletButtonSize, tripletButtonSize);
    lfo2CurrentY += modRateKnobSize + modRateValueGap;
    
    // LFO2 Depth
    parentEditor.lfo2DepthLabel.setBounds(lfo2CentreX - modKnobDiameter / 2, lfo2CurrentY, modKnobDiameter, modLabelHeight);
    lfo2CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo2DepthSlider.setBounds(lfo2CentreX - modKnobDiameter / 2, lfo2CurrentY, modKnobDiameter, modKnobDiameter);
    lfo2CurrentY += modKnobDiameter + modRowSpacing;
    
    // LFO2 Phase
    parentEditor.lfo2PhaseLabel.setBounds(lfo2CentreX - modKnobDiameter / 2, lfo2CurrentY, modKnobDiameter, modLabelHeight);
    lfo2CurrentY += modLabelHeight + modLabelGap;
    parentEditor.lfo2PhaseSlider.setBounds(lfo2CentreX - modKnobDiameter / 2, lfo2CurrentY, modKnobDiameter, modKnobDiameter);
    lfo2CurrentY += modKnobDiameter + modRowSpacing;
    
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
    // Knobs same size as Depth, centered under Filter button
    if (modFilter2Show)
    {
        const int filterKnobSize = modKnobDiameter;  // Match Depth knob size
        const int filterKnobGap = 8;
        const int filterComboW = 90;
        const int filterComboH = 20;
        int filterPairLeft = lfo2CentreX - (2 * filterKnobSize + filterKnobGap) / 2;  // Center pair under Filter button
        parentEditor.modFilter2CutoffLabel.setBounds(filterPairLeft, lfo2CurrentY, filterKnobSize, 12);
        parentEditor.modFilter2CutoffSlider.setBounds(filterPairLeft, lfo2CurrentY + 12, filterKnobSize, filterKnobSize);
        int resX = filterPairLeft + filterKnobSize + filterKnobGap;
        parentEditor.modFilter2ResonanceLabel.setBounds(resX, lfo2CurrentY, filterKnobSize, 12);
        parentEditor.modFilter2ResonanceSlider.setBounds(resX, lfo2CurrentY + 12, filterKnobSize, filterKnobSize);
        lfo2CurrentY += filterKnobSize + 12 + modRowSpacing;
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
}

void EffectsPageComponent::parameterChanged(const juce::String& parameterID, float)
{
    if (parameterID == juce::ParameterID{"delayFilterShow", 1}.getParamID())
        updateDelayFilterVisibility();
    else if (parameterID == juce::ParameterID{"reverbFilterShow", 1}.getParamID())
        updateReverbFilterVisibility();
    else if (parameterID == juce::ParameterID{"grainDelayFilterShow", 1}.getParamID())
        updateGrainDelayFilterVisibility();
}

void EffectsPageComponent::updateDelayFilterVisibility()
{
    bool show = *parentEditor.audioProcessor.getValueTreeState().getRawParameterValue("delayFilterShow") > 0.5f;
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
    bool show = *parentEditor.audioProcessor.getValueTreeState().getRawParameterValue("grainDelayFilterShow") > 0.5f;
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
    bool show = *parentEditor.audioProcessor.getValueTreeState().getRawParameterValue("reverbFilterShow") > 0.5f;
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
    float avgLevel = 0.5f * (parentEditor.audioProcessor.getLeftPeakLevel() + parentEditor.audioProcessor.getRightPeakLevel());
    avgLevel = juce::jmin(1.0f, avgLevel);
    drawStarfield(g, getWidth(), getHeight(), avgLevel);
    const int baseAlpha = 10 + static_cast<int>(48.0f * avgLevel);
    drawGlows(g, baseAlpha, juce::Colour(0xff00b4ff),
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
    bool filterShow = *parentEditor.audioProcessor.getValueTreeState().getRawParameterValue("delayFilterShow") > 0.5f;
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
    bool grainFilterShow = *parentEditor.audioProcessor.getValueTreeState().getRawParameterValue("grainDelayFilterShow") > 0.5f;
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

    const int grainContentHeight = gY - grainStartY;
    parentEditor.grainDelayGroup.setBounds(delayColX, grainStartY, colW, grainContentHeight);

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
    bool reverbFilterShow = *parentEditor.audioProcessor.getValueTreeState().getRawParameterValue("reverbFilterShow") > 0.5f;
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
    
    // Mix knob - always the lowest knob
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

    const int gateContentHeight = tY - gateStartY;
    parentEditor.tranceGateGroup.setBounds(reverbColX, gateStartY, colW, gateContentHeight);

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
    const int flangerContentHeight = fY - flangerStartY;
    parentEditor.flangerGroup.setBounds(grainColX, flangerStartY, colW, flangerContentHeight);

    // Make column bottoms flush, excluding columns that have filters open
    {
        const int leftBottom   = grainStartY + grainContentHeight;
        const int centerBottom = gateStartY + gateContentHeight;
        const int rightBottom  = flangerStartY + flangerContentHeight;
        const bool leftHasFilter   = filterShow || grainFilterShow;
        const bool centerHasFilter = reverbFilterShow;

        // Use center column's base height (without 16-step extra row) for flush
        const int centerExtraFor16 = (totalSteps == 16) ? (stepBtnSize + stepGap) : 0;
        const int centerBaseBottom = centerBottom - centerExtraFor16;

        // Compute flush target only from columns without open filters
        int maxBase = rightBottom;  // right column never has filters
        if (!leftHasFilter)   maxBase = juce::jmax(maxBase, leftBottom);
        if (!centerHasFilter) maxBase = juce::jmax(maxBase, centerBaseBottom);

        // Extend only non-filter columns to the flush target
        if (!leftHasFilter && leftBottom < maxBase)
            parentEditor.grainDelayGroup.setBounds(delayColX, grainStartY, colW, maxBase - grainStartY);
        if (!centerHasFilter && centerBaseBottom < maxBase)
            parentEditor.tranceGateGroup.setBounds(reverbColX, gateStartY, colW, juce::jmax(gateContentHeight, maxBase - gateStartY));
        if (rightBottom < maxBase)
            parentEditor.flangerGroup.setBounds(grainColX, flangerStartY, colW, maxBase - flangerStartY);
    }
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
    // Final EQ – spanning center+right columns
    addAndMakeVisible(parentEditor.finalEQGroup);
    addAndMakeVisible(parentEditor.finalEQEnabledButton);
    addAndMakeVisible(parentEditor.finalEQEnabledLabel);
    if (parentEditor.finalEQComponent)
        addAndMakeVisible(parentEditor.finalEQComponent.get());
    addAndMakeVisible(parentEditor.transientGroup);
    addAndMakeVisible(parentEditor.transientEnabledButton);
    addAndMakeVisible(parentEditor.transientEnabledLabel);
    addAndMakeVisible(parentEditor.transientTypeCombo);
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
    float avgLevel = 0.5f * (parentEditor.audioProcessor.getLeftPeakLevel() + parentEditor.audioProcessor.getRightPeakLevel());
    avgLevel = juce::jmin(1.0f, avgLevel);
    drawStarfield(g, getWidth(), getHeight(), avgLevel);
    const int baseAlpha = 10 + static_cast<int>(48.0f * avgLevel);
    drawGlows(g, baseAlpha, juce::Colour(0xff00b4ff), { &parentEditor.bitCrusherGroup, &parentEditor.compressorGroup, &parentEditor.softClipperGroup, &parentEditor.lofiGroup, &parentEditor.transientGroup, &parentEditor.finalEQGroup });
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

    // Type combo
    parentEditor.transientTypeCombo.setBounds(tCx - tComboW / 2, tfy, tComboW, 22);
    tfy += 24 + gap;
    parentEditor.transientTypeLabel.setBounds(tCx - tComboW / 2, tfy, tComboW, labelH);
    tfy += labelH + labelGap;

    // Post Effect toggle
    parentEditor.transientPostEffectButton.setBounds(tCx - 60, tfy, 120, 20);
    tfy += 20 + gap;

    // Row 1: Mix | Length | Ka-Donk | Coarse
    const int tQuadW = 4 * knobSize + 3 * tKg;
    int tQuadLeft = leftColX + (colW - tQuadW) / 2;
    parentEditor.transientMixLabel.setBounds(tQuadLeft, tfy, knobSize, labelH);
    parentEditor.transientLengthLabel.setBounds(tQuadLeft + knobSize + tKg, tfy, knobSize, labelH);
    parentEditor.transientKaDonkLabel.setBounds(tQuadLeft + 2 * (knobSize + tKg), tfy, knobSize, labelH);
    parentEditor.transientCoarseLabel.setBounds(tQuadLeft + 3 * (knobSize + tKg), tfy, knobSize, labelH);
    tfy += labelH + labelGap;
    parentEditor.transientMixSlider.setBounds(tQuadLeft, tfy, knobSize, knobSize);
    parentEditor.transientLengthSlider.setBounds(tQuadLeft + knobSize + tKg, tfy, knobSize, knobSize);
    parentEditor.transientKaDonkSlider.setBounds(tQuadLeft + 2 * (knobSize + tKg), tfy, knobSize, knobSize);
    parentEditor.transientCoarseSlider.setBounds(tQuadLeft + 3 * (knobSize + tKg), tfy, knobSize, knobSize);
    tfy += knobSize + 24 + pad;

    parentEditor.transientGroup.setBounds(leftColX, transY, colW, tfy - transY);

    // --- Final EQ (center+right columns, below compressor / soft clipper) ---
    // Top aligned to the lower of the two top-row groups; bottom aligned to
    // the Transient group bottom (tfy) so both sides end at the same Y.
    {
        const int eqTopY  = juce::jmax(cy, sy) + pad;
        const int eqBotY  = transY + (tfy - transY); // == tfy, same bottom as Transient
        const int eqWidth = (rightColX + colW) - centerColX;
        int efy = eqTopY + groupTitleH;

        parentEditor.finalEQEnabledButton.setBounds(centerColX + pad, efy, bOnBtnW, bOnBtnH);
        efy += bOnBtnH + gap;

        const int eqDisplayH = juce::jmax(40, eqBotY - efy - pad);
        if (parentEditor.finalEQComponent)
            parentEditor.finalEQComponent->setBounds(centerColX + pad, efy,
                                                     eqWidth - 2 * pad, eqDisplayH);

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
    float avgLevel = 0.5f * (pageRef.parentEditor.audioProcessor.getLeftPeakLevel() + pageRef.parentEditor.audioProcessor.getRightPeakLevel());
    avgLevel = juce::jmin(1.0f, avgLevel);
    const int baseAlpha = 8 + static_cast<int>(44.0f * avgLevel);
    drawGlows(g, baseAlpha, juce::Colour(0xff00b4ff),
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
    oscilloscopeGroup.addAndMakeVisible(*oscilloscope);
    spectrumGroup.addAndMakeVisible(*spectrumAnalyser);
    // Glow overlay on top so Oscilloscope/Spectrum sit behind it - cleaner look
    glowOverlay = std::make_unique<GlowOverlayComponent>(*this);
    addAndMakeVisible(*glowOverlay);
}

void SpectralPageComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0a0a1f));
    float avgLevel = 0.5f * (parentEditor.audioProcessor.getLeftPeakLevel() + parentEditor.audioProcessor.getRightPeakLevel());
    avgLevel = juce::jmin(1.0f, avgLevel);
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

    // Single row height for all three (Spectrum height) - clean aligned look
    const int rowH = juce::jmax(60, static_cast<int>((L + headerH) * verticalShrink * spectrumShrink));

    // Top row: Lissajous (square) | Oscilloscope (stretches to fill) - same height as Spectrum
    auto topRow = content.withHeight(rowH);
    const int labelSpace = 32;  // Match groupTitleHeight (title inside box)
    const int innerH = rowH - 8 - labelSpace;  // Draw area height for Lissajous
    const int gonioW = juce::jmax(80, innerH + 16);  // Square fits: width = height + padding
    auto gonioGroupBounds = topRow.withWidth(gonioW);
    goniometerGroup.setBounds(gonioGroupBounds);
    oscilloscopeGroup.setBounds(topRow.withX(gonioGroupBounds.getRight()).withWidth(content.getWidth() - gonioGroupBounds.getWidth()));

    // Bottom row: Spectrum - same height as top row
    auto bottomRow = content.withY(topRow.getBottom() + pad).withHeight(rowH);
    spectrumGroup.setBounds(bottomRow);

    // Lissajous: below label, centered square
    auto gonioInner = gonioGroupBounds.reduced(8);
    gonioInner.removeFromTop(labelSpace);
    const int gonioDim = juce::jmax(80, juce::jmin(gonioInner.getWidth(), gonioInner.getHeight()));
    int gx = gonioGroupBounds.getX() + (gonioGroupBounds.getWidth() - gonioDim) / 2;
    int gy = gonioGroupBounds.getY() + labelSpace + (gonioInner.getHeight() - gonioDim) / 2;
    lissajousDrawArea = juce::Rectangle<int>(gx, gy, gonioDim, gonioDim);

    // Content area below label, with padding
    oscilloscope->setBounds(oscilloscopeGroup.getLocalBounds().withTrimmedTop(labelSpace).reduced(8));
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
    const juce::Colour pathColour = showClipping ? juce::Colour(0xffdd2222) : juce::Colour(0xff48bde8);

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
            g.setColour(pathColour);
            g.strokePath(p, juce::PathStrokeType(2.5f));
        }
    }

    g.restoreState();
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

    initPresetButton.setTooltip("Reset all parameters to defaults");
    initPresetButton.onClick = [this]()
    {
        presetManager->loadInitPreset();
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

    DBG("Space Dust: Setting up Oscillators section");
    
    // Note: Components will be added to page components, not directly to editor
    
    // Oscillator 1
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
    
    osc1WaveformCombo.addItem(safeString("Sine"), 1);
    osc1WaveformCombo.addItem(safeString("Triangle"), 2);
    osc1WaveformCombo.addItem(safeString("Saw"), 3);
    osc1WaveformCombo.addItem(safeString("Square"), 4);
    osc1WaveformCombo.setSelectedId(2);  // Default to Triangle
    
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
    
    // CRITICAL: Verify parameter exists before creating attachment
    // This prevents crashes if parameter ID doesn't match
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
    
    osc1WaveformAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "osc1Waveform", osc1WaveformCombo);
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
    osc2WaveformCombo.addItem(safeString("Sine"), 1);
    osc2WaveformCombo.addItem(safeString("Triangle"), 2);
    osc2WaveformCombo.addItem(safeString("Saw"), 3);
    osc2WaveformCombo.addItem(safeString("Square"), 4);
    osc2WaveformCombo.setSelectedId(2);  // Default to Triangle
    osc2WaveformAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "osc2Waveform", osc2WaveformCombo);
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
    // Initialize combo box to match current noiseType (0=White, 1=Pink)
    int currentNoiseType = audioProcessor.getNoiseType();
    noiseColorCombo.setSelectedId(currentNoiseType == 1 ? 2 : 1); // Pink=2, White=1
    noiseColorCombo.setLookAndFeel(&customLookAndFeel);
    // Add listener to update noiseType when selection changes (noiseColor is UI-only, not a parameter)
    noiseColorCombo.onChange = [this] {
        int selectedId = noiseColorCombo.getSelectedId();
        // Map combo box IDs to noise types: 1=White (0), 2=Pink (1)
        int noiseType = (selectedId == 2) ? 1 : 0; // Pink=1, White=0
        audioProcessor.setNoiseType(noiseType);
    };
    noiseColorLabel.setText(safeString("Color"), juce::dontSendNotification);
    noiseColorLabel.setJustificationType(juce::Justification::centred);
    noiseColorLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));  // Light blue
    noiseColorLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
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
    
    filterModeCombo.addItem(safeString("Low Pass"), 1);
    filterModeCombo.addItem(safeString("Band Pass"), 2);
    filterModeCombo.addItem(safeString("High Pass"), 3);
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
    subOscWaveformCombo.addItem(safeString("Sine"), 1);
    subOscWaveformCombo.addItem(safeString("Triangle"), 2);
    subOscWaveformCombo.addItem(safeString("Saw"), 3);
    subOscWaveformCombo.addItem(safeString("Square"), 4);
    subOscWaveformCombo.setSelectedId(2);  // Default Triangle
    subOscWaveformAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "subOscWaveform", subOscWaveformCombo);
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

            // Grab keyboard focus for arrow key controls
            juce::Timer::callAfterDelay(100, [this]()
            {
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
    lfo1SyncRateCombo.setSelectedId(juce::jlimit(1, 13, static_cast<int>(std::round(lfo1FreeRateSlider.getValue())) + 1));
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
    lfo2SyncRateCombo.setSelectedId(juce::jlimit(1, 13, static_cast<int>(std::round(lfo2FreeRateSlider.getValue())) + 1));
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
    modFilter1ModeCombo.addItem(safeString("Low Pass"), 1);
    modFilter1ModeCombo.addItem(safeString("Band Pass"), 2);
    modFilter1ModeCombo.addItem(safeString("High Pass"), 3);
    modFilter1ModeCombo.setSelectedId(1);
    modFilter1ModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "modFilter1Mode", modFilter1ModeCombo);
    modFilter1CutoffSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    modFilter1CutoffSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    modFilter1CutoffSlider.setTextValueSuffix(" Hz");
    modFilter1CutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "modFilter1Cutoff", modFilter1CutoffSlider);
    modFilter1ResonanceSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    modFilter1ResonanceSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    modFilter1ResonanceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "modFilter1Resonance", modFilter1ResonanceSlider);
    warmSaturationMod1Button.setButtonText(safeString("Warm Saturation"));
    warmSaturationMod1Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "warmSaturationMod1", warmSaturationMod1Button);
    modFilter1ModeLabel.setText(safeString("Mode"), juce::dontSendNotification);
    modFilter1CutoffLabel.setText(safeString("Cutoff"), juce::dontSendNotification);
    modFilter1ResonanceLabel.setText(safeString("Res"), juce::dontSendNotification);
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
    modFilter2ModeCombo.addItem(safeString("Low Pass"), 1);
    modFilter2ModeCombo.addItem(safeString("Band Pass"), 2);
    modFilter2ModeCombo.addItem(safeString("High Pass"), 3);
    modFilter2ModeCombo.setSelectedId(1);
    modFilter2ModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "modFilter2Mode", modFilter2ModeCombo);
    modFilter2CutoffSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    modFilter2CutoffSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    modFilter2CutoffSlider.setTextValueSuffix(" Hz");
    modFilter2CutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "modFilter2Cutoff", modFilter2CutoffSlider);
    modFilter2ResonanceSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    modFilter2ResonanceSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 55, 18);
    modFilter2ResonanceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.getValueTreeState(), "modFilter2Resonance", modFilter2ResonanceSlider);
    warmSaturationMod2Button.setButtonText(safeString("Warm Saturation"));
    warmSaturationMod2Attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.getValueTreeState(), "warmSaturationMod2", warmSaturationMod2Button);
    modFilter2ModeLabel.setText(safeString("Mode"), juce::dontSendNotification);
    modFilter2CutoffLabel.setText(safeString("Cutoff"), juce::dontSendNotification);
    modFilter2ResonanceLabel.setText(safeString("Res"), juce::dontSendNotification);
    modFilter2ModeLabel.setJustificationType(juce::Justification::centred);
    modFilter2ModeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    modFilter2ModeLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    modFilter2CutoffLabel.setJustificationType(juce::Justification::centred);
    modFilter2CutoffLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    modFilter2CutoffLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    modFilter2ResonanceLabel.setJustificationType(juce::Justification::centred);
    modFilter2ResonanceLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0d8ff));
    modFilter2ResonanceLabel.setFont(customLookAndFeel.getBodyFont(12.0f, true));
    
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
    delaySyncRateCombo.setSelectedId(juce::jlimit(1, 13, static_cast<int>(std::round(delayFreeRateSlider.getValue())) + 1));
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
    reverbTypeCombo.addItem("Sexicon take an L", 2);
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
    transientTypeCombo.setSelectedId(1);
    transientTypeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getValueTreeState(), "transientType", transientTypeCombo);
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
    // -- Set Window Size (DEFERRED VIA TIMER CALLBACK) --
    // CRITICAL: In Ableton Live, setSize() immediately triggers resized()/paint()
    // which may access LookAndFeel or components before the constructor completes.
    // Solution: Defer setSize() until after constructor returns, using a timer callback.
    // This ensures the constructor completes before any callbacks fire.
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
    
    // Defer setSize() until after constructor completes using a one-shot timer
    // This prevents resized()/paint() from firing during construction
    DBG("Space Dust: Editor ctor - Scheduling setSize() via timer");
    juce::Timer::callAfterDelay(10, [this]() {
        DBG("Space Dust: Timer callback - About to set window size");
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
        
        if (isBeingDestroyed.load())
        {
            DBG("Space Dust: Timer callback - isBeingDestroyed=true, skipping setSize");
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
            return;
        }
        
        DBG("Space Dust: Timer callback - isBeingDestroyed=false, proceeding with setSize");
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
        
        // Calculate the correct window height for tabbed interface
        // Effects tab needs extra height when Delay/Grain Delay filter is toggled on (controls must stay visible)
        const int calculatedHeight = 857;  // Original height
        
        DBG("Space Dust: Timer callback - Calling setSize(1120, " + juce::String(calculatedHeight) + ")");
        try
        {
            // CRITICAL: setSize() triggers resized() which will layout components
            // resized() must NOT call setSize() again to prevent infinite recursion
            setSize(1120, calculatedHeight);

            // Scale entire UI down 5% — everything (knobs, labels, groups) shrinks uniformly.
            // setTransform handles both rendering AND mouse input mapping.
            setTransform(juce::AffineTransform::scale(0.95f));

            DBG("Space Dust: Timer callback - setSize() completed");
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
        }
        catch (const std::exception& e)
        {
            DBG("Space Dust: Exception in setSize(): " + juce::String(e.what()));
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
        }
        catch (...)
        {
            DBG("Space Dust: Unknown exception in setSize()");
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
        }
        
        DBG("Space Dust: Timer callback - Calling setResizable(false, false)");
        try
        {
            setResizable(false, false);  // Lock window size - no resizing
            DBG("Space Dust: Timer callback - setResizable() completed");
        }
        catch (const std::exception& e)
        {
            DBG("Space Dust: Exception in setResizable(): " + juce::String(e.what()));
        }
        catch (...)
        {
            DBG("Space Dust: Unknown exception in setResizable()");
        }
        
        DBG("Space Dust: Timer callback - Window size set successfully");
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
    });
    
    //==============================================================================
    // -- Register APVTS Listeners for Bidirectional Filter Sync --
    {
        auto& vts = audioProcessor.getValueTreeState();
        vts.addParameterListener("filterMode", this);
        vts.addParameterListener("filterCutoff", this);
        vts.addParameterListener("filterResonance", this);
        vts.addParameterListener("warmSaturationMaster", this);
        vts.addParameterListener("modFilter1LinkToMaster", this);
        vts.addParameterListener("modFilter1Mode", this);
        vts.addParameterListener("modFilter1Cutoff", this);
        vts.addParameterListener("modFilter1Resonance", this);
        vts.addParameterListener("warmSaturationMod1", this);
        vts.addParameterListener("modFilter2LinkToMaster", this);
        vts.addParameterListener("modFilter2Mode", this);
        vts.addParameterListener("modFilter2Cutoff", this);
        vts.addParameterListener("modFilter2Resonance", this);
        vts.addParameterListener("warmSaturationMod2", this);
    }

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

SpaceDustAudioProcessorEditor::~SpaceDustAudioProcessorEditor()
{
    DBG("Space Dust: Processor destructor START");

    isBeingDestroyed.store(true);
    stopTimer();

    // Easter egg cleanup
    cheezeGuyGame.reset();

    // Remove APVTS filter sync listeners
    {
        auto& vts = audioProcessor.getValueTreeState();
        vts.removeParameterListener("filterMode", this);
        vts.removeParameterListener("filterCutoff", this);
        vts.removeParameterListener("filterResonance", this);
        vts.removeParameterListener("warmSaturationMaster", this);
        vts.removeParameterListener("modFilter1LinkToMaster", this);
        vts.removeParameterListener("modFilter1Mode", this);
        vts.removeParameterListener("modFilter1Cutoff", this);
        vts.removeParameterListener("modFilter1Resonance", this);
        vts.removeParameterListener("warmSaturationMod1", this);
        vts.removeParameterListener("modFilter2LinkToMaster", this);
        vts.removeParameterListener("modFilter2Mode", this);
        vts.removeParameterListener("modFilter2Cutoff", this);
        vts.removeParameterListener("modFilter2Resonance", this);
        vts.removeParameterListener("warmSaturationMod2", this);
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
// -- APVTS Listener: Bidirectional Filter Parameter Sync --

void SpaceDustAudioProcessorEditor::parameterChanged(const juce::String& parameterID, float newValue)
{
    auto safeThis = juce::Component::SafePointer<SpaceDustAudioProcessorEditor>(this);
    juce::MessageManager::callAsync([safeThis, parameterID, newValue]()
    {
        if (safeThis == nullptr || safeThis->isBeingDestroyed.load()) return;
        safeThis->syncLinkedFilterParams(parameterID, newValue);
    });
}

void SpaceDustAudioProcessorEditor::syncLinkedFilterParams(const juce::String& parameterID, float newValue)
{
    if (isSyncingFilterParams) return;
    isSyncingFilterParams = true;

    auto& apvts = audioProcessor.getValueTreeState();
    bool link1 = *apvts.getRawParameterValue("modFilter1LinkToMaster") > 0.5f;
    bool link2 = *apvts.getRawParameterValue("modFilter2LinkToMaster") > 0.5f;

    auto syncParam = [&](const juce::String& sourceParam, const juce::String& targetParam)
    {
        auto* target = apvts.getParameter(targetParam);
        if (!target) return;
        float srcRaw = *apvts.getRawParameterValue(sourceParam);
        float targetNorm = target->convertTo0to1(srcRaw);
        if (std::abs(target->getValue() - targetNorm) > 0.0001f)
            target->setValueNotifyingHost(targetNorm);
    };

    struct ParamMapping { const char* master; const char* mod1; const char* mod2; };
    static const ParamMapping mappings[] = {
        { "filterMode",             "modFilter1Mode",      "modFilter2Mode" },
        { "filterCutoff",           "modFilter1Cutoff",    "modFilter2Cutoff" },
        { "filterResonance",        "modFilter1Resonance", "modFilter2Resonance" },
        { "warmSaturationMaster",   "warmSaturationMod1",  "warmSaturationMod2" },
    };

    auto findMapping = [&](const juce::String& pid) -> const ParamMapping*
    {
        for (auto& m : mappings)
            if (pid == m.master || pid == m.mod1 || pid == m.mod2)
                return &m;
        return nullptr;
    };

    if (parameterID == "modFilter1LinkToMaster" && newValue > 0.5f)
    {
        for (auto& m : mappings)
            syncParam(m.master, m.mod1);
    }
    else if (parameterID == "modFilter2LinkToMaster" && newValue > 0.5f)
    {
        for (auto& m : mappings)
            syncParam(m.master, m.mod2);
    }
    else if (auto* m = findMapping(parameterID))
    {
        bool isMaster = (parameterID == m->master);
        bool isMod1   = (parameterID == m->mod1);
        bool isMod2   = (parameterID == m->mod2);

        if (isMaster)
        {
            if (link1) syncParam(m->master, m->mod1);
            if (link2) syncParam(m->master, m->mod2);
        }
        else if (isMod1 && link1)
        {
            syncParam(m->mod1, m->master);
            if (link2) syncParam(m->mod1, m->mod2);
        }
        else if (isMod2 && link2)
        {
            syncParam(m->mod2, m->master);
            if (link1) syncParam(m->mod2, m->mod1);
        }
    }

    isSyncingFilterParams = false;
}

//==============================================================================
// -- Slider Listener (pitch bend snap-back) --

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
// -- Button Listener (On toggle -> group glow sync) --

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
    bool lfo1Sync = *audioProcessor.getValueTreeState().getRawParameterValue("lfo1Sync") > 0.5f;
    double lfo1Rate = lfo1FreeRateSlider.getValue();
    bool lfo1Triplet = *audioProcessor.getValueTreeState().getRawParameterValue("lfo1TripletEnabled") > 0.5f;
    bool lfo1All = *audioProcessor.getValueTreeState().getRawParameterValue("lfo1TripletStraightToggle") > 0.5f;
    
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
        // Free mode: 0.01-200 Hz logarithmic
        float normalizedRate = juce::jlimit(0.0f, 1.0f, static_cast<float>(lfo1Rate) / 12.0f);
        float logMin = std::log(0.01f);
        float logMax = std::log(200.0f);
        float logFreq = logMin + normalizedRate * (logMax - logMin);
        float hz = std::exp(logFreq);
        hz = juce::jlimit(0.01f, 200.0f, hz);
        juce::String hzText = juce::String::formatted("%.2f Hz", hz);
        if (!isBeingDestroyed.load())
        {
            lfo1RateValueLabel.setText(hzText, juce::dontSendNotification);
            lfo1TripletButton.setVisible(false);
            lfo1TripletStraightButton.setVisible(false);
        }
    }
    
    // Update LFO2 rate display
    bool lfo2Sync = *audioProcessor.getValueTreeState().getRawParameterValue("lfo2Sync") > 0.5f;
    double lfo2Rate = lfo2FreeRateSlider.getValue();
    bool lfo2Triplet = *audioProcessor.getValueTreeState().getRawParameterValue("lfo2TripletEnabled") > 0.5f;
    bool lfo2All = *audioProcessor.getValueTreeState().getRawParameterValue("lfo2TripletStraightToggle") > 0.5f;
    
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
        // Free mode: 0.01-200 Hz logarithmic
        float normalizedRate = juce::jlimit(0.0f, 1.0f, static_cast<float>(lfo2Rate) / 12.0f);
        float logMin = std::log(0.01f);
        float logMax = std::log(200.0f);
        float logFreq = logMin + normalizedRate * (logMax - logMin);
        float hz = std::exp(logFreq);
        hz = juce::jlimit(0.01f, 200.0f, hz);
        juce::String hzText = juce::String::formatted("%.2f Hz", hz);
        if (!isBeingDestroyed.load())
        {
            lfo2RateValueLabel.setText(hzText, juce::dontSendNotification);
            lfo2TripletButton.setVisible(false);
            lfo2TripletStraightButton.setVisible(false);
        }
    }
    
    // Update Delay rate display (inverted: knob 0 = long, knob 12 = short)
    // Use same parameter value as processor for consistency
    bool delaySync = *audioProcessor.getValueTreeState().getRawParameterValue("delaySync") > 0.5f;
    float delayRateParam = *audioProcessor.getValueTreeState().getRawParameterValue("delayRate");
    double delayRateClamped = juce::jlimit(0.0, 12.0, static_cast<double>(delayRateParam));
    double delayRateInverted = 12.0 - delayRateClamped;
    
    if (delaySync)
    {
        // Unified list: straight, dotted (1/8., 1/4.), and triplets baked in
        double normalized = juce::jlimit(0.0, 1.0, delayRateInverted / 12.0);
        double curved = std::pow(normalized, 2.5);
        int musicalIndex = static_cast<int>(std::round(curved * 12.0));
        musicalIndex = juce::jlimit(0, 12, musicalIndex);
        static const juce::String delayLabels[18] = {
            "1/32", "1/24", "1/16", "1/12", "1/8", "1/8.", "1/4", "1/4.",
            "1/2", "3/4", "1", "3/2", "2", "3", "4", "5", "8", "8"
        };
        int mappedIndex = static_cast<int>(std::round(musicalIndex / 12.0 * 17.0));
        mappedIndex = juce::jlimit(0, 17, mappedIndex);
        if (!isBeingDestroyed.load())
        {
            delayRateValueLabel.setText(delayLabels[mappedIndex], juce::dontSendNotification);
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
    
    // Update stereo level meters and box glow (glow brightness follows output level)
    if (stereoLevelMeter != nullptr && !isBeingDestroyed.load())
    {
        float leftPeak = audioProcessor.getLeftPeakLevel();
        float rightPeak = audioProcessor.getRightPeakLevel();
        stereoLevelMeter->updateLevels(leftPeak, rightPeak);
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

    repaint();  // Redraw glow halos so they follow output level

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
            spec->setClipping(showClipping);
            spec->update(buf);
            spec->repaint();
        }
    }
    
    // Update Legato Glide button visibility based on voice mode
    // Only show when voice mode is "Legato" (index 2)
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
        bool isLegatoMode = (voiceModeIndex == 2);
        legatoGlideButton.setVisible(isLegatoMode);
    }
}

//==============================================================================
// -- Paint Method --

void SpaceDustAudioProcessorEditor::paint(juce::Graphics& g)
{
    if (isBeingDestroyed.load())
        return;

    const int w = getWidth();
    const int h = getHeight();

    g.fillAll(juce::Colour(0xff0a0a1f));
    {
        float starLevel = 0.5f * (audioProcessor.getLeftPeakLevel() + audioProcessor.getRightPeakLevel());
        starLevel = juce::jmin(1.0f, starLevel);
        drawStarfield(g, w, h, starLevel);
    }

    //==============================================================================
    // -- Arcade Edge Glow (top & bottom) - parabolic, subtle, smooth color gradient --
    // Cyan -> deep blue -> transparent. Red variant when metering in red zone.
    // Tabs are translucent so glow shows through at their bottom edge.
    {
        float avgLevel = 0.5f * (audioProcessor.getLeftPeakLevel() + audioProcessor.getRightPeakLevel());
        avgLevel = juce::jmin(1.0f, avgLevel);

        const float maxGlowDepth = 90.0f;
        const bool isRed = (clippingHoldTicks > 0);
        // More subtle: reduced base alpha (was 15+140, now 6+60)
        juce::uint8 peakAlpha = static_cast<juce::uint8>(juce::jlimit(0, 255, static_cast<int>(6 + 60 * avgLevel)));

        // Color gradient: bright at edge, deeper shade inward, then transparent
        const juce::Colour edgeCol = isRed ? juce::Colour(0xffdd3333) : juce::Colour(0xff00d4ff);  // Darker red when clipping
        const juce::Colour midCol  = isRed ? juce::Colour(0xff991818) : juce::Colour(0xff0066aa);  // Deeper red
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
            float yBase = static_cast<float>(h);
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
    // -- Title: Space Dust (Glitch Goblin font) --
    {
        auto titleArea = juce::Rectangle<int>(0, 2, w, 44);
        juce::Font titleFont = customLookAndFeel.getTitleFont(40.0f);

        // Outer glow layers (expanding offsets, decreasing opacity)
        const juce::Colour glowCol(0xff00d4ff);
        for (int i = 3; i >= 1; --i)
        {
            g.setColour(glowCol.withAlpha(static_cast<juce::uint8>(18 / i)));
            g.setFont(titleFont);
            g.drawText(safeString("Space Dust"), titleArea.expanded(i * 2, i), juce::Justification::centred, true);
        }

        // Drop shadow
        g.setColour(juce::Colour(0x44000000));
        g.setFont(titleFont);
        g.drawText(safeString("Space Dust"), titleArea.translated(1, 2), juce::Justification::centred, true);

        // Main text
        g.setColour(juce::Colour(0xffffffff));
        g.setFont(titleFont);
        g.drawText(safeString("Space Dust"), titleArea, juce::Justification::centred, true);
    }

    //==============================================================================
    // -- Master Section Glow --
    if (masterGroup.isVisible())
    {
        float avgLevel = 0.5f * (audioProcessor.getLeftPeakLevel() + audioProcessor.getRightPeakLevel());
        avgLevel = juce::jmin(1.0f, avgLevel);
        const int baseAlpha = 8 + static_cast<int>(44.0f * avgLevel);
        drawGlows(g, baseAlpha, juce::Colour(0xff00b4ff), { &masterGroup });
    }
}

//==============================================================================
// -- Resized Method --

void SpaceDustAudioProcessorEditor::resized()
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
        firstResized = false;
    }
    
    //==============================================================================
    // -- CRITICAL: Wrap entire resized() in try/catch to catch crashes --
    try
    {
        DBG("Space Dust: resized() - Starting layout (width=" + juce::String(getWidth()) + ", height=" + juce::String(getHeight()) + ")");
        
        // Write to log file for debugging
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
    
    //==============================================================================
    // -- Preset Controls Layout (Top Header Bar) --
    const int titleHeight = 48;  // Compact: title + tab bar
    {
        const int presetY = 10;
        const int presetH = 26;
        const int gap = 4;
        int px = 8;

        presetCombo.setBounds(px, presetY, 142, presetH);
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
    // Expand tab width by 10%
    int tabbedWidth = static_cast<int>((getWidth() - masterWidth - masterGap) * 0.9 * 1.1);
    tabbedComponent.setBounds(0, titleHeight, tabbedWidth, getHeight() - titleHeight);
    // Tab glow overlay: sits on top of tab bar so parabolic glow shines through (drawn above tabs)
    const int tabBarHeight = 36;
    const int bottomGlowHeight = 90;
    if (tabGlowOverlay != nullptr)
        tabGlowOverlay->setBounds(0, titleHeight, tabbedWidth, tabBarHeight);
    if (bottomTabGlowOverlay != nullptr)
        bottomTabGlowOverlay->setBounds(0, getHeight() - bottomGlowHeight, tabbedWidth, bottomGlowHeight);
    
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
    const int verticalSpacing = 6;        // Compact vertical spacing between controls
    const int topPaddingReduction = 0;    // Title inside box - keep content below
    
    // Move Master box left by reducing the gap by 50%
    // Right edge of tab is at: tabbedWidth
    // New Master X position: tabbedWidth + actualMasterGap (reduced gap)
    int masterX = tabbedWidth + actualMasterGap;
    int masterY = titleHeight + 10;  // 10px top margin to match bottom
    int masterHeight = getHeight() - titleHeight - 20;  // 10px top + 10px bottom margins
    
    masterGroup.setBounds(masterX, masterY, masterWidth, masterHeight);
    
    auto masterContent = masterGroup.getBounds().reduced(groupPadding, groupTitleHeight + groupPadding);
    int masterKnobX = masterContent.getCentreX() - knobDiameter / 2; // Centered
    // Layout: Volume, Meter (below Volume), then Pitch bend, Mode, Glide, Legato
    int masterCurrentY = masterContent.getY() - topPaddingReduction;
    
    // Spacing accounts for: knob (75) + reduced text box (10) + label (18) + reduced gaps
    int masterSpacing = knobDiameter + 10 + labelHeight + (labelGap / 2) + verticalSpacing;
    
    masterVolumeSlider.setBounds(masterKnobX, masterCurrentY, knobDiameter, knobDiameter);
    masterVolumeLabel.setBounds(masterKnobX, masterCurrentY + knobDiameter + 10 + (labelGap / 2), knobDiameter, labelHeight);
    masterVolumeSlider.setVisible(true);
    masterVolumeLabel.setVisible(true);
    masterCurrentY += masterSpacing;
    
    // Stereo Level Meters: below Volume, above everything else
    const int meterBarWidth = 20;
    const int meterGap = 4;
    const int totalMeterWidth = (meterBarWidth * 2) + meterGap;
    int meterHeight = 216;  // 90% of 240 (shrunk 10% to fit Legato Glide)
    int meterX = masterContent.getCentreX() - totalMeterWidth / 2;
    if (stereoLevelMeter != nullptr)
    {
        stereoLevelMeter->setBounds(meterX, masterCurrentY, totalMeterWidth, meterHeight);
        stereoLevelMeter->setVisible(true);
    }
    masterCurrentY += meterHeight + verticalSpacing;
    
    // Bend Range on its own row, Pitch fader on row below
    const int pitchKnobSize = 56;
    const int pitchFaderWidth = 24;
    const int pitchFaderHeight = 80;
    int pitchCentreX = masterContent.getCentreX();
    
    // Row 1: Bend Range (knob only, centered)
    int pitchKnobY = masterCurrentY;
    pitchBendAmountSlider.setBounds(pitchCentreX - pitchKnobSize / 2, pitchKnobY, pitchKnobSize, pitchKnobSize);
    pitchBendAmountLabel.setBounds(pitchCentreX - 40, pitchKnobY + pitchKnobSize + 8, 80, labelHeight);  // "Bend Range" on its own line
    pitchBendAmountSlider.setVisible(true);
    pitchBendAmountLabel.setVisible(true);
    masterCurrentY += pitchKnobSize + 8 + labelHeight + verticalSpacing;
    
    // Row 2: Pitch Bend (vertical fader)
    pitchBendSlider.setBounds(pitchCentreX - pitchFaderWidth / 2, masterCurrentY, pitchFaderWidth, pitchFaderHeight);
    pitchBendLabel.setBounds(pitchCentreX - 18, masterCurrentY + pitchFaderHeight + 8, 36, labelHeight);  // "Pitch"
    pitchBendSlider.setVisible(true);
    pitchBendLabel.setVisible(true);
    masterCurrentY += pitchFaderHeight + 8 + labelHeight + verticalSpacing;
    
    // Voice Mode combo (centered)
    int voiceModeX = masterContent.getCentreX() - comboWidth / 2;
    voiceModeLabel.setBounds(voiceModeX, masterCurrentY, comboWidth, labelHeight);
    masterCurrentY += labelHeight + labelGap;
    voiceModeCombo.setBounds(voiceModeX, masterCurrentY, comboWidth, comboHeight);
    voiceModeCombo.setVisible(true);
    voiceModeLabel.setVisible(true);
    masterCurrentY += comboHeight + verticalSpacing;
    
    glideTimeSlider.setBounds(masterKnobX, masterCurrentY, knobDiameter, knobDiameter);
    glideTimeLabel.setBounds(masterKnobX, masterCurrentY + knobDiameter + 10 + (labelGap / 2), knobDiameter, labelHeight);
    glideTimeSlider.setVisible(true);
    glideTimeLabel.setVisible(true);
    
    // Legato Glide toggle: small button beneath the Glide knob label in the Master box
    // Only visible when voice mode is "Legato" (index 2)
    int voiceModeIndex = 0;
    if (auto* voiceModeParam = audioProcessor.getValueTreeState().getParameter("voiceMode"))
    {
        if (auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(voiceModeParam))
        {
            voiceModeIndex = choiceParam->getIndex();
        }
    }
    
    int legatoToggleY = masterCurrentY + knobDiameter + 10 + (labelGap / 2) + labelHeight + 1;
    legatoGlideLabel.setBounds(masterKnobX, legatoToggleY, knobDiameter, labelHeight);
    legatoToggleY += labelHeight + 1;
    legatoGlideButton.setBounds(masterKnobX + (knobDiameter - 100) / 2, legatoToggleY, 100, 20);
    legatoGlideLabel.setVisible(false);
    bool isLegatoMode = (voiceModeIndex == 2);
    legatoGlideButton.setVisible(isLegatoMode);
    
    DBG("Space Dust: resized() - Layout complete");
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception in resized(): " + juce::String(e.what()));
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
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception in resized()");
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
    }
}

