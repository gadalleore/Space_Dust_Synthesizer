#pragma once

#include <atomic>
#include <optional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>   // juce::MidiKeyboardComponent (Standalone keyboard)
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "NoteLockGrid.h"
#include "SpaceDustLookAndFeel.h"
#include "OscilloscopeComponent.h"
#include "SpectrumAnalyserComponent.h"
#include "FinalEQComponent.h"
#include "PresetManager.h"
#include "CheezeGuyGame.h"
#include "WaveformChoiceAttachment.h"
#include "ComboStepper.h"
#include "WaveformEditorComponent.h"
#include "SpaceDustDither.h"

// Glow overlays are defined in PluginEditor.cpp; forward-declare them here so the
// unique_ptr members below resolve under ordinary name lookup. (A `friend class`
// declaration alone is not visible to ordinary lookup â€” MSVC tolerates it, Clang
// does not, which previously broke the macOS/Xcode build.)
class TabGlowOverlayComponent;
class BottomTabGlowOverlayComponent;

//==============================================================================
/**
    SpaceDust Audio Processor Editor
    
    A beautiful cosmic-themed GUI for the Space Dust synthesizer.
    Features organized sections with GroupBoxes for clear visual hierarchy.
    
    Design Philosophy:
    - Dark cosmic background (0xff0a0a1f) for immersive experience
    - Large glowing "Space Dust" title at top center
    - Three organized GroupBoxes with generous spacing:
      * Oscillators (left half): waveforms, detune, tuning, mix
      * Filter (right middle): mode, cutoff, resonance
      * Envelope (bottom left): ADSR sliders
      * Master (bottom right): master volume
    - Rotary sliders for musical continuous parameters
    - Clear labels and aligned layout, no overlaps
    - Resizable window (1200x650 preferred, min constraints)
    
    Real-time Safety: All parameter updates via AudioProcessorValueTreeState attachments.
*/
//==============================================================================
// -- Safe ComboBox Listener for Sync Rate Combos --
class SyncRateComboListener : public juce::ComboBox::Listener
{
public:
    SyncRateComboListener(juce::Slider* slider) : targetSlider(slider) {}
    ~SyncRateComboListener() override = default;
    
    void comboBoxChanged(juce::ComboBox* comboBoxThatHasChanged) override
    {
        if (targetSlider != nullptr && comboBoxThatHasChanged != nullptr)
        {
            int selectedId = comboBoxThatHasChanged->getSelectedId();
            if (selectedId > 0)
            {
                double rateValue = static_cast<double>(selectedId - 1);  // 0-12
                targetSlider->setValue(rateValue, juce::sendNotificationSync);
            }
        }
    }
    
private:
    juce::Slider* targetSlider;
};

// Forward declaration
class SpaceDustAudioProcessorEditor;

//==============================================================================
// -- Main Page Component (Oscillators, Filter, Envelopes, Master) --
class MainPageComponent : public juce::Component,
                          public juce::AudioProcessorValueTreeState::Listener,
                          public juce::AsyncUpdater
{
public:
    MainPageComponent(SpaceDustAudioProcessorEditor& editor);
    ~MainPageComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void parameterChanged(const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;
    void mouseUp(const juce::MouseEvent& event) override;

private:
    void updateSubOscVisibility();
    SpaceDustAudioProcessorEditor& parentEditor;
};

//==============================================================================
// -- Modulation Page Component (LFO Section) --
class ModulationPageComponent : public juce::Component,
                               public juce::AudioProcessorValueTreeState::Listener
{
public:
    ModulationPageComponent(SpaceDustAudioProcessorEditor& editor);
    ~ModulationPageComponent() override;
    
    void paint(juce::Graphics& g) override;
    void resized() override;
    void parameterChanged(const juce::String& parameterID, float newValue) override;

private:
    /** Params this page subscribes to; every one of them means "re-run resized()". */
    static juce::StringArray relayoutTriggerParams();

    SpaceDustAudioProcessorEditor& parentEditor;
};

//==============================================================================
// -- Effects Page Component (Delay, Chorus, Reverb, etc.) --
class EffectsPageComponent : public juce::Component,
                             public juce::AudioProcessorValueTreeState::Listener,
                             public juce::AsyncUpdater
{
public:
    EffectsPageComponent(SpaceDustAudioProcessorEditor& editor);
    ~EffectsPageComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void parameterChanged(const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;

private:
    void updateDelayFilterVisibility();
    void updateReverbFilterVisibility();
    void updateGrainDelayFilterVisibility();
    SpaceDustAudioProcessorEditor& parentEditor;
};

//==============================================================================
// -- Saturation Color Page Component --
class SaturationColorPageComponent : public juce::Component
{
public:
    SaturationColorPageComponent(SpaceDustAudioProcessorEditor& editor);
    ~SaturationColorPageComponent() override = default;
    
    void paint(juce::Graphics& g) override;
    void resized() override;
    
private:
    SpaceDustAudioProcessorEditor& parentEditor;
};

//==============================================================================
// -- Spectral Page Component (Lissajous drawn in-place, Oscilloscope, Spectrum) --
class SpectralPageComponent : public juce::Component
{
public:
    SpectralPageComponent(SpaceDustAudioProcessorEditor& editor);
    ~SpectralPageComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    OscilloscopeComponent* getOscilloscope() { return oscilloscope.get(); }
    SpectrumAnalyserComponent* getSpectrumAnalyser() { return spectrumAnalyser.get(); }

private:
    void drawLissajous(juce::Graphics& g, juce::Rectangle<int> area, const juce::AudioBuffer<float>& buffer, int validSamples = -1);

    // Glow overlay - draws on top of Oscilloscope/Spectrum for cleaner look
    class GlowOverlayComponent : public juce::Component
    {
    public:
        GlowOverlayComponent(SpectralPageComponent& page);
        void paint(juce::Graphics& g) override;
    private:
        SpectralPageComponent& pageRef;
    };
    std::unique_ptr<GlowOverlayComponent> glowOverlay;

    SpaceDustAudioProcessorEditor& parentEditor;
    juce::GroupComponent goniometerGroup { "Lissajous", "Lissajous" };
    juce::GroupComponent oscilloscopeGroup { "Oscilloscope", "Oscilloscope" };
    juce::GroupComponent spectrumGroup { "Spectrum", "Spectrum" };
    juce::Rectangle<int> lissajousDrawArea;
    std::unique_ptr<OscilloscopeComponent> oscilloscope;
    std::unique_ptr<SpectrumAnalyserComponent> spectrumAnalyser;

    //==========================================================================
    // -- Lissajous motion dither (Giuseppe, 2026-08-02) --
    // The Lissajous is drawn straight into this page rather than by a component of
    // its own, and it was the one scope that never picked up the treatment the
    // others have: no ghost trail, no bloom, just a flat stroke. Same technique as
    // OscilloscopeComponent -- the last few figures ghosted behind the live one,
    // each in a different colour channel -- since the whole figure is somewhere new
    // every frame and so has no single moving head to streak.
    static constexpr int   kLissajousHistory = 4;
    static constexpr float kLissajousSpread  = 3.5f;
    static constexpr float kLissajousAlpha   = 0.55f;

    std::vector<juce::Path> lissajousHistory;
    SpaceDustDither::TilesPtr ditherTiles;
};

//==============================================================================
// -- Stereo Level Meter Component --
/**
    Real-time stereo level meters for L/R channels.
    Displays two vertical bars showing peak levels from -Inf to 0 dB.
    Updates smoothly via timer, with cyan/blue fill and red clipping indicator.
*/
class StereoLevelMeterComponent : public juce::Component,
                                  private juce::Timer
{
public:
    StereoLevelMeterComponent(SpaceDustAudioProcessor& processor);
    ~StereoLevelMeterComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    //==========================================================================
    // -- Geometry, owned HERE and nowhere else --
    // The layout in layoutPlate() used to hardcode its own copy of the bar width
    // and gap, and the moment paint's gap changed the two disagreed: the component
    // was allotted 44px while paint wanted 52, so startX went NEGATIVE and the bars
    // were drawn partly outside their own component. That clipped the outer edges
    // flat and left no room at all for a halo (Giuseppe, 2026-08-01: the glow "still
    // not fully surrounding the metering"). Ask for requiredWidth() and these can
    // never drift apart again.
    static constexpr int barWidth = 20;
    static constexpr int barGap   = 12;   // wide enough that two halos cannot meet

    /** Padding INSIDE the component, so the glow has somewhere to go on every side.
        Without it a halo is clipped by the component's own bounds. */
    static constexpr int padX = 10;
    static constexpr int padY = 10;

    static constexpr int requiredWidth()  { return barWidth * 2 + barGap + padX * 2; }
    static constexpr int minimumHeight()  { return padY * 2 + 20; }

private:
    void timerCallback() override;

    SpaceDustAudioProcessor& audioProcessor;

    //==========================================================================
    // -- Ballistics (ported from Sol Voice Tuner's EdgeMeters) --
    // Self-driven at 60Hz rather than pushed by the editor's 20Hz timer. Sol's
    // release constants are PER FRAME at 60Hz, and the peak-hold fall and the
    // motion smear both need the frame rate to read properly -- at 20Hz the mark
    // descends in visible steps and the smear stutters. Reading the processor's
    // peak atomics directly costs nothing (they are plain loads, not consumed),
    // and it keeps the meter's timing independent of the UI repaint throttle.
    static constexpr int kFps = 60;

    // Bar: instant attack, short hold, then fall.
    static constexpr float kRelease    = 0.89f;   // per frame
    static constexpr int   kHoldFrames = 4;       // ~65ms at 60Hz

    // Peak-hold ticks: slower than the bar by enough that the mark reads as its
    // own object. It stays fully lit for the whole descent and dissolves only over
    // the last stretch, so it lands and vanishes together.
    //
    // The dissolve used to run on its own clock (0.93 per frame, killed at 0.06
    // alpha), which put the tick out after ~0.65s -- and at ~25 dB/s that is barely
    // a quarter of the way down. From a full-scale peak the mark was snapped to zero
    // while still at 74% height, so it left the scale in one step instead of falling
    // (Giuseppe, 2026-08-09: "jumps down instead of slowly lowering to zero").
    // Alpha is taken from the distance still to fall now, so it cannot outrun the
    // fall no matter how high the peak it started from.
    static constexpr float kMarkRelease    = 0.955f;  // ~25 dB/s at 60Hz
    static constexpr float kMarkFadeSpan   = 0.15f;   // fraction of scale it fades across
    static constexpr int   kMarkHoldFrames = 12;      // ~0.2s at 60Hz
    static constexpr float kMarkThickness  = 2.0f;

    // Motion trail (SpaceDustDither::streakRgb).
    static constexpr int   kTrailLength     = 8;     // frames of travel retained
    static constexpr int   kTrailSteps      = 9;     // stamps along the streak
    static constexpr float kTrailMinStep    = 0.9f;  // px before a ghost is kept
    static constexpr float kTrailMinSmear   = 2.5f;  // px of travel before drawing
    static constexpr float kTrailAlpha      = 0.75f;
    static constexpr float kTrailHeadHeight = 6.0f;  // px of bar top that smears

    float level[2]     { 0.0f, 0.0f };
    int   hold[2]      { 0, 0 };
    float mark[2]      { 0.0f, 0.0f };
    float markFade[2]  { 0.0f, 0.0f };
    int   markHold[2]  { 0, 0 };

    juce::Array<float> trails[2];
    juce::Array<float> markTrails[2];   // the falling peak ticks smear too
    SpaceDustDither::TilesPtr ditherTiles;

    /** Records where a moving edge is now and streaks the head back over where it
        has just been. A held level travels nowhere and so draws nothing.
        `headHeight` is how much of the shape smears -- the bar streaks only its top
        few px, the peak tick streaks its whole (2px) self. */
    void paintTrail(juce::Graphics& g, juce::Rectangle<float> col,
                    float top, juce::Array<float>& history, float headHeight);

    // Meter dimensions
    static constexpr int meterWidth = 20;  // Width of each bar
    static constexpr int meterGap = 4;     // Gap between L and R bars

    // Helper: Convert linear peak to dB (returns -Inf for 0, 0 for 1.0)
    float linearToDb(float linear);

    // Helper: Convert dB to normalized height (0.0 = -Inf at bottom, 1.0 = 0 dB at top)
    float dbToHeight(float db);
};

//==============================================================================
// -- Note Lock grid --
// The grid itself is pure arithmetic with no JUCE or editor dependency, so it
// lives in Source/NoteLockGrid.h where tools/notelocktest can exercise it
// directly. Only the KNOB snaps to it: juce::Slider::snapValue is called on drag
// and wheel but never by setValue, so preset recall and host automation pass
// through untouched and nothing retroactively rewrites a saved patch.

//==============================================================================
/** A cutoff knob that clicks into the Note Lock grid while dragged or scrolled.

    Behaves as a stock juce::Slider whenever activeGrid is unset or returns nullopt,
    so the knob is untouched with the feature off. */
class NoteLockSlider : public juce::Slider
{
public:
    /** Set by the editor. Returns the grid THIS knob should click into, or nullopt
        when Note Lock is off for it. One callback rather than an on/off flag plus a
        separate mode, so the two can never disagree -- and a callback rather than
        stored state because a mod filter follows the master's setting while
        "Link to Master" is engaged. */
    std::function<std::optional<NoteLock::Grid>()> activeGrid;

    /** Optional mapping between this slider's own units and the Hz the grid works in.

        Unset means the slider already IS in Hz -- the filter cutoff knobs. The LFO
        Rate knob is a 0-12 abstract control that means a RATIO to the played note, so
        it supplies a pair of converters and snaps on the same grid regardless. Both
        must be set, and toGridHz must be increasing, or neither is used. */
    std::function<double(double)> toGridHz;
    std::function<double(double)> fromGridHz;

    double snapValue(double attemptedValue, DragMode dragMode) override
    {
        if (activeGrid != nullptr)
        {
            if (const auto grid = activeGrid())
            {
                const bool mapped = (toGridHz != nullptr && fromGridHz != nullptr);
                const double hz = mapped ? toGridHz(attemptedValue) : attemptedValue;
                const double lo = mapped ? toGridHz(getMinimum())    : getMinimum();
                const double hi = mapped ? toGridHz(getMaximum())    : getMaximum();

                const double snapped = NoteLock::snapHz(hz, lo, hi, *grid);
                return mapped ? fromGridHz(snapped) : snapped;
            }
        }

        return juce::Slider::snapValue(attemptedValue, dragMode);
    }
};

//==============================================================================
// -- Easter Egg Slider: detects rapid clicks on the master knob --
class EasterEggSlider : public juce::Slider
{
public:
    std::function<void()> onClicked;

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (onClicked) onClicked();
        juce::Slider::mouseDown(e);
    }
};

//==============================================================================
class SpaceDustAudioProcessorEditor : public juce::AudioProcessorEditor,
                                      public juce::Timer,
                                      public juce::Slider::Listener,
                                      public juce::Button::Listener,
                                      public juce::AudioProcessorValueTreeState::Listener,
                                      public juce::FocusChangeListener,
                                      // What the Waveforms window needs to resample the synth:
                                      // start a recording, watch it, take it, and strip the
                                      // patch back around what it made. See ResampleHost.
                                      private WaveformEditorComponent::ResampleHost
{
    // Allow page components to access private members for layout
    friend class MainPageComponent;
    friend class ModulationPageComponent;
    friend class EffectsPageComponent;
    friend class SaturationColorPageComponent;
    friend class SpectralPageComponent;
    friend class TabGlowOverlayComponent;
    friend class BottomTabGlowOverlayComponent;
    
public:
    SpaceDustAudioProcessorEditor(SpaceDustAudioProcessor&);
    ~SpaceDustAudioProcessorEditor() override;
    
    void timerCallback() override;
    void globalFocusChanged(juce::Component* focusedComponent) override;
    void sliderDragStarted(juce::Slider* slider) override;
    void sliderValueChanged(juce::Slider* slider) override;
    void sliderDragEnded(juce::Slider* slider) override;
    void buttonClicked(juce::Button* button) override;
    void buttonStateChanged(juce::Button* button) override;
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    //==============================================================================
    void paint(juce::Graphics&) override;
    void resized() override;

    //==============================================================================
    // -- The real paint and layout --
    // paint()/resized() above are one-line wrappers over these. They kept the "Plate"
    // names from when the UI lived in a floating window with a plate component inside
    // it; both are gone, but a dozen sites inside this class call layoutPlate() to
    // force a relayout, so the names stay. `plateWidth` is just getWidth().
    void paintPlate(juce::Graphics& g, int plateWidth);
    void layoutPlate();

    // ========================================================================
    // SAFE PARAMETER ACCESS (2026 session crash hardening)
    // ========================================================================
    // Replaces dangerous direct *getRawParameterValue(...) calls throughout the
    // editor and its sub-components. Prevents null dereference crashes when a
    // parameter ID is missing during Ableton session restore, rapid automation,
    // or partial state load.
    // ========================================================================
    // Safe accessor for raw parameter values (returns the actual/denormalized value,
    // not the 0-1 normalized representation). Prevents null derefs during session
    // restore or when parameter IDs are missing.
    float safeGetParam(const juce::String& paramID, float fallback = 0.0f) const noexcept;

private:
    SpaceDustAudioProcessor& audioProcessor;
    
    //==============================================================================
    // -- Custom LookAndFeel for Premium Typography --
    // CRITICAL: LookAndFeel MUST be declared FIRST (before ANY Component/slider/button/group)
    // This ensures proper destruction order: components destroyed before LookAndFeel
    // Prevents juce_LookAndFeel.cpp:82 weak refcount assertion in Ableton Live
    SpaceDustLookAndFeel customLookAndFeel;
    
    // Flag to prevent timerCallback from accessing components during destruction
    std::atomic<bool> isBeingDestroyed{false};

    // Master / mod filter "Link to Master": when a Mod-tab filter is linked, its knobs are
    // repointed at the MASTER filter parameters so the linked filter is literally the same
    // automatable parameter as the master (one automation lane drives both knobs). When
    // unlinked, the knobs point back at the filter's own params for independent automation.
    // This deliberately avoids any param-to-param syncing (setValueNotifyingHost), which
    // re-enters Live's automation engine and crashes it.
    bool isSyncingFilterParams = false;
    void syncLinkedFilterParams(const juce::String& parameterID, float newValue);
    void rebuildLinkedFilterAttachments();

    // Note Lock. isNoteLockActive answers "is this filter's cutoff knob currently
    // quantised", following Link to Master the same way Key Tracking does: a linked
    // mod filter shares the master's lock param, an unlinked one has its own.
    // snapCutoffToNoteLock pulls a cutoff onto the grid immediately when the toggle
    // is switched on, so engaging it locks what you are already hearing rather than
    // waiting for the next knob move.
    // 0 = master, 1 = mod 1, 2 = mod 2.
    std::optional<NoteLock::Grid> activeNoteLockGrid(int filterIndex) const;
    void snapCutoffToNoteLock(int filterIndex);

    // Pitch bend snap-back: poll processor ramp and sync display
    bool pitchBendSnapActive{false};

    // Clipping hold: how long the red state persists after the output leaves the red zone.
    //
    // Was 10 ticks (~500ms), which is what made the UI sit red long after the sound had
    // stopped clipping -- and because the hold RE-ARMS on every red frame, a passage that
    // touched the threshold repeatedly kept restarting the full 500ms. Now one tick, so
    // the red follows the meter down almost immediately (Giuseppe, 2026-08-01: "as soon as
    // the synth goes out of the red, the color should go back to blue").
    //
    // Not zero: the level is sampled by a 20Hz timer, so a hold of one tick is what
    // guarantees a clip that happens between two samples is still shown for a frame rather
    // than missed entirely. If brief clips start being easy to miss, this is the number to
    // raise -- each tick is 50ms.
    //
    // NOTE: the peak level itself has no decay (PluginProcessor stores raw per-block
    // getMagnitude()), so nothing else here is holding the red on. If red still lingers
    // for seconds after this, the output genuinely is at or above -1 dBFS that long.
    int clippingHoldTicks = 0;
    static constexpr int clippingHoldDuration = 1;   // ~50ms at the 50ms timer

    // -- Unified glow meter level --
    // Single per-frame snapshot of the averaged L/R output level driving ALL glow in the
    // UI (group halos, edge glow, starfield, tab overlays). Computed ONCE per timer tick in
    // timerCallback so every object in a given frame glows from the identical value. Reading
    // the live atomics independently at each paint site let the audio thread update them
    // mid paint-pass, so different objects sampled slightly different levels -> they appeared
    // to glow at different rates. All glow sites now read this snapshot instead.
    float glowMeterLevel_ = 0.0f;

    // -- Idle-repaint suppression --
    // The whole-editor repaint in timerCallback (redrawing the level-driven glow/starfield)
    // is expensive: it re-lays-out every label/knob each tick. Since the ENTIRE painted look
    // is a pure function of glowMeterLevel_ + clippingHoldTicks (no time-based animation), we
    // only repaint when one of those actually changes. At silence the output level sits at 0
    // and stops changing, so repaints stop and CPU drops to idle (was pegged ~130% before â€”
    // the Standalone, with nothing to throttle its UI, appeared to hang on launch). Playing
    // notes moves the level every tick, so the glow animates exactly as before.
    float lastPaintedGlowLevel_ = -1.0f;   // force a paint on the very first tick
    bool  lastPaintedClipping_  = false;

public:
    /** Averaged (L+R)/2 output level [0,1] for glow, snapshotted once per timer tick.
        All glow drawing must use this so every object glows consistently. */
    float getGlowMeterLevel() const noexcept { return glowMeterLevel_; }

private:

    // TooltipWindow required for setTooltip() to display (e.g. Pan labels)
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    // STANDALONE-ONLY playable keyboard strip at the bottom of the window (mouse + QWERTY
    // computer keys). Created only when wrapperType == wrapperType_Standalone; null in the
    // VST3, where resized() leaves the layout untouched. Bound to processor.keyboardState
    // (which outlives the editor). Declared after customLookAndFeel so it is destroyed
    // before the LookAndFeel it inherits from the editor.
    std::unique_ptr<juce::MidiKeyboardComponent> standaloneKeyboard;
    static constexpr int standaloneKeyboardHeight = 78;

    //==============================================================================
    // -- Easter Egg: Cheeze Guy Game --
    juce::int64 masterKnobClickTimes[7] = {};
    int masterKnobClickCount = 0;
    std::unique_ptr<CheezeGuyGameComponent> cheezeGuyGame;

    /** The Waveforms window. Built on first use and then kept, hidden, so that
        closing and reopening it does not lose the selected slot or move it back
        to the middle of the screen. */
    /** One per dropdown, built by attachComboSteppers. Each lays itself over its
        combo box and follows it, so no layout here has to make room. */
    std::vector<std::unique_ptr<ComboStepper>> comboSteppers;

    /** Which boxes already carry one, so a box reached by two walks -- its own
        page and the editor that contains it -- is not given two. */
    std::vector<juce::ComboBox*> steppedCombos;

    std::unique_ptr<WaveformEditorPanel> waveformWindow;

    /** Open the Waveforms window, pointed at the dropdown that asked for it.

        The window drives that dropdown directly rather than keeping a list of its
        own, so choosing a waveform in the window and choosing it in the menu are
        the same act, and the two can never disagree about what is selected.
        userBase says where that dropdown's User entries start, which differs
        between the oscillators, the noise source and the Transient, kind says
        what the entries before them are so the window can draw them, and group
        says which of the five sets of import slots the window is to show --
        each dropdown has its own. */
    /** Give every dropdown in the plugin a pair of stepper arrows.

        Walks the tree rather than naming twenty-six combos by hand: a list
        written out here would go stale the first time a dropdown was added, and
        a dropdown without arrows among twenty-five with them reads as a bug. */
    void attachComboSteppers(juce::Component& root);

    void openWaveformWindow(juce::Component* anchorButton, juce::ComboBox* combo,
                            int userBase, WaveformEditorComponent::BuiltInKind kind,
                            UserWave::Group group);

    //==========================================================================
    // -- WaveformEditorComponent::ResampleHost --
    // The Resample buttons, answered here because this is where the processor and
    // the parameters are. The synth plays itself a middle C and records the whole
    // chain; see ResampleCapture for how that starts and ends.

    bool startCapture(juce::String& errorMessage) override;
    bool captureIsRunning() const override;
    float captureProgress() const override;
    void abandonCapture() override;
    float playbackPhase(UserWave::Group group) const override;
    void setPlaybackPhaseWanted(bool wanted) override;

    /** Take the finished recording, measure how loud it was before the library
        normalises it, and name it after the patch it came out of. */
    bool collectCapture(WaveformEditorComponent::Capture& capture,
                        juce::String& errorMessage) override;

    /** Strip the patch back to nothing but one waveform -- Resample + Init.

        Every parameter goes to its default, which turns every effect off, and
        then the handful that are not silent-by-default are set: the one source
        that is to be heard, the filter wide open, the amplitude envelope out of
        the way, and the master volume put back to where the sound was recorded
        (the library normalises what it stores, and this undoes exactly that).
        The imported waveforms are NOT cleared -- unlike Initialize Preset, the
        point of this is the waveform that was just made. */
    void initialiseAroundWaveform(UserWave::Group group, int choiceIndex, float peak) override;

    /** Set one parameter to a value in its own units, as a complete gesture.
        Wrapped like every other write in this editor -- a naked
        setValueNotifyingHost corrupts FL Studio's "Last Tweaked" tracking. */
    void setParameterValue(const juce::String& parameterID, float value);

    /** Rebuild the five waveform dropdowns from the imported waveforms.

        The menus list the built-in shapes and then only the User slots that hold
        something, so the player never scrolls past entries they cannot choose.
        The PARAMETER behind each menu is untouched and still offers all eight
        slots, because a host's automation is written against that fixed list --
        see WaveformChoiceAttachment for how the two are kept apart. */
    void rebuildWaveformMenus();

    // -- Drag-resize scaling --
    // The whole UI is authored at a fixed design size (kDesignWidth x designHeight_)
    // and rendered through a single uniform scale. mainView is a transparent container
    // that holds every control and is scaled by k = getWidth()/kDesignWidth; paintPlate()
    // scales its background/decorations by the same k via g.addTransform. The editor is
    // resizable with a locked aspect ratio, so dragging its corner grows/shrinks
    // everything together without moving any item relative to another.
    static constexpr int kDesignWidth = 1120;
    int designHeight_ = 857;          // set in the ctor timer (+ keyboard strip in standalone)
    juce::Component mainView;         // scalable container parenting the entire UI
    bool cheezeGuyTabAdded = false;

    /** Takes the Cheeze Guy tab back off the bar and forgets it was ever earned.
        Initialize Preset calls it: the easter egg is not part of a patch, so a
        fresh start starts fresh. Safe to call when the tab is not there. */
    void hideCheezeGuyTab();

    //==============================================================================
    // -- Preset Management --
    std::unique_ptr<PresetManager> presetManager;
    juce::ComboBox presetCombo;
    juce::TextButton savePresetButton { "Save Preset" };
    juce::TextButton initPresetButton { "Initialize Preset" };
    juce::TextButton folderPresetButton { "Preset Folder" };
    void refreshPresetList();
    void showSavePresetDialog();

    //==============================================================================
    // Space Dust title artwork (nebula logo, black keyed to transparent). Drawn in
    // the top header strip in place of the rendered "Space Dust" text. Falls back
    // to the text title if the image fails to load.
    juce::Image titleImage;

    //==============================================================================
    // 63C company logo (white ghost on transparent). Drawn as a small watermark
    // in the bottom-right corner of the window, below the Master box.
    juce::Image logoImage;

    //==============================================================================
    // Filter box bottom edge in EDITOR coordinates, published by
    // MainPageComponent::resized() (where filterGroup is final and parented).
    // The always-visible Master section reads this to line its own bottom up with
    // the Filter box in Mono/Legato mode. 0 = not laid out yet.
    int filterBoxBottomY = 0;

    //==============================================================================
    // -- Tabbed Component for Main/Modulation Pages --
    juce::TabbedComponent tabbedComponent;
    std::unique_ptr<TabGlowOverlayComponent> tabGlowOverlay;
    std::unique_ptr<BottomTabGlowOverlayComponent> bottomTabGlowOverlay;
    std::unique_ptr<MainPageComponent> mainPage;
    std::unique_ptr<ModulationPageComponent> modulationPage;
    std::unique_ptr<EffectsPageComponent> effectsPage;
    std::unique_ptr<SaturationColorPageComponent> saturationColorPage;
    std::unique_ptr<SpectralPageComponent> spectralPage;

    //==============================================================================
    // -- GUI Components: Oscillators Section --
    
    juce::GroupComponent oscillatorsGroup;
    
    // Waveform selectors
    juce::ComboBox osc1WaveformCombo;
    juce::ComboBox osc2WaveformCombo;

    // Opens the Waveforms window, one beside each dropdown that can select an
    // imported sample. All five open the same window on the same eight slots;
    // they differ only in which list is shown around them and which slot it
    // lands on. The sub oscillator's and the Transient's live in other sections
    // of the panel, but they are the same button doing the same thing.
    // Drawn like the toggles they sit among -- see SpaceDustToggleStyleButton.
    SpaceDustToggleStyleButton osc1WaveformEditButton;
    SpaceDustToggleStyleButton osc2WaveformEditButton;
    SpaceDustToggleStyleButton noiseWaveformEditButton;
    SpaceDustToggleStyleButton subOscWaveformEditButton;
    SpaceDustToggleStyleButton transientTypeEditButton;
    
    // Oscillator tuning controls (simple, intuitive system)
    juce::Slider osc1CoarseTuneSlider;
    //==========================================================================
    // -- Bend, Spectrum and Sync --
    //
    // Five knobs per oscillator, shown in the Waveforms panel rather than on the
    // main page: they shape whatever waveform that oscillator plays, so they
    // belong beside the list where a waveform is chosen.
    //
    // Owned here, not by the panel, because they are attached to parameters and
    // the panel knows nothing about a processor. The panel borrows the right five
    // when it is opened -- see WaveformEditorComponent::ShapingControls.
    static constexpr int numShapingKnobs = 5;

    juce::Slider osc1ShapingSliders[numShapingKnobs];
    juce::Slider osc2ShapingSliders[numShapingKnobs];
    juce::Label osc1ShapingLabels[numShapingKnobs];
    juce::Label osc2ShapingLabels[numShapingKnobs];
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        osc1ShapingAttachments[numShapingKnobs];
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        osc2ShapingAttachments[numShapingKnobs];

    /** Voices, Detune and Width per oscillator, lent to the Waveforms panel the
        same way the shaping knobs are. */
    static constexpr int numUnisonKnobs = 3;

    juce::Slider osc1UnisonSliders[numUnisonKnobs];
    juce::Slider osc2UnisonSliders[numUnisonKnobs];
    juce::Label osc1UnisonLabels[numUnisonKnobs];
    juce::Label osc2UnisonLabels[numUnisonKnobs];
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        osc1UnisonAttachments[numUnisonKnobs];
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        osc2UnisonAttachments[numUnisonKnobs];

    WaveformEditorComponent::ShapingControls osc1ShapingControls;
    WaveformEditorComponent::ShapingControls osc2ShapingControls;

    juce::Slider osc1DetuneSlider;
    juce::Slider osc2CoarseTuneSlider;
    juce::Slider osc2DetuneSlider;
    
    // Independent oscillator and noise level controls
    juce::Slider osc1LevelSlider;
    juce::Slider osc2LevelSlider;
    juce::Slider osc1PanSlider;
    juce::Slider osc2PanSlider;
    juce::Slider noiseLevelSlider;
    
    // Noise color selector (White/Pink)
    juce::ComboBox noiseColorCombo;
    
    // Noise EQ controls (Low Shelf/Cut and High Shelf/Cut)
    juce::Slider lowShelfAmountSlider;
    juce::Slider highShelfAmountSlider;
    
    // Labels
    juce::Label osc1WaveformLabel;
    juce::Label osc2WaveformLabel;
    juce::Label osc1CoarseTuneLabel;
    juce::Label osc1DetuneLabel;
    juce::Label osc2CoarseTuneLabel;
    juce::Label osc2DetuneLabel;
    juce::Label osc1LevelLabel;
    juce::Label osc2LevelLabel;
    juce::Label osc1PanLabel;
    juce::Label osc2PanLabel;
    juce::Label noiseLevelLabel;
    juce::Label noiseColorLabel;
    juce::Label lowShelfAmountLabel;
    juce::Label highShelfAmountLabel;
    juce::Label osc1RowLabel;
    juce::Label osc2RowLabel;
    juce::Label noiseRowLabel;
    
    //==============================================================================
    // -- GUI Components: Filter Section --
    
    juce::GroupComponent filterGroup;
    
    juce::ComboBox filterModeCombo;
    NoteLockSlider filterCutoffSlider;
    juce::Slider filterResonanceSlider;
    juce::ToggleButton warmSaturationMasterButton;
    juce::ToggleButton filterKeyTrackButton;
    juce::ToggleButton filterNoteLockButton;    // Shown only while Key Tracking is on
    juce::ToggleButton filterHarmonicLockButton; // Shown only while Note Lock is on

    // Filter Envelope controls
    juce::GroupComponent filterEnvGroup;
    juce::Slider filterEnvAttackSlider;
    juce::Slider filterEnvDecaySlider;
    juce::Slider filterEnvSustainSlider;
    juce::Slider filterEnvReleaseSlider;
    juce::Slider filterEnvAmountSlider;
    
    juce::Label filterModeLabel;
    juce::Label filterCutoffLabel;
    juce::Label filterResonanceLabel;
    juce::Label filterEnvAttackLabel;
    juce::Label filterEnvDecayLabel;
    juce::Label filterEnvSustainLabel;
    juce::Label filterEnvReleaseLabel;
    juce::Label filterEnvAmountLabel;
    
    //==============================================================================
    // -- GUI Components: Voice Section --
    
    juce::GroupComponent voiceGroup;
    
    juce::ComboBox voiceModeCombo;
    juce::Slider glideTimeSlider;
    
    juce::Label voiceModeLabel;
    juce::Label glideTimeLabel;
    
    //==============================================================================
    // -- GUI Components: MPE Section (Modulation tab) --

    // GroupComponent with tooltip support (juce::GroupComponent has no setTooltip by default)
    struct TooltipGroupComponent : public juce::GroupComponent, public juce::SettableTooltipClient {};
    TooltipGroupComponent mpeGroup;
    juce::ComboBox mpeModeCombo;
    juce::Label mpeModeLabel;
    juce::Slider mpePitchBendRangeSlider;
    juce::Label mpePitchBendRangeLabel;
    juce::Slider mpePressureDepthSlider;
    juce::Label mpePressureDepthLabel;
    juce::Slider mpeTimbreDepthSlider;
    juce::Label mpeTimbreDepthLabel;

    //==============================================================================
    // -- GUI Components: Envelope Section --
    
    juce::GroupComponent envelopeGroup;
    
    juce::Slider envAttackSlider;
    juce::Slider envDecaySlider;
    juce::Slider envSustainSlider;
    juce::Slider envReleaseSlider;
    
    juce::Label envAttackLabel;
    juce::Label envDecayLabel;
    juce::Label envSustainLabel;
    juce::Label envReleaseLabel;
    
    // Pitch envelope (below Amp Envelope)
    juce::Slider pitchEnvAmountSlider;
    juce::Slider pitchEnvTimeSlider;
    juce::Slider pitchEnvPitchSlider;
    juce::Label pitchEnvAmountLabel;
    juce::Label pitchEnvTimeLabel;
    juce::Label pitchEnvPitchLabel;
    
    // Sub oscillator (expandable when toggle is on)
    juce::ToggleButton subOscToggleButton;
    juce::ComboBox subOscWaveformCombo;
    juce::Slider subOscLevelSlider;
    juce::Slider subOscCoarseSlider;
    juce::Label subOscWaveformLabel;
    juce::Label subOscLevelLabel;
    juce::Label subOscCoarseLabel;
    
    //==============================================================================
    // -- GUI Components: Master Section --
    
    juce::GroupComponent masterGroup;
    
    EasterEggSlider masterVolumeSlider;
    juce::Label masterVolumeLabel;
    
    // Pitch bend and master pitch (in Master section)
    juce::Slider pitchBendAmountSlider;
    juce::Label pitchBendAmountLabel;
    // Sits beside Bend Range: how much a note's velocity sets its level and
    // opens its filter.
    juce::Slider velocityAmountSlider;
    juce::Label velocityAmountLabel;
    juce::Slider pitchBendSlider;
    juce::Label pitchBendLabel;
    juce::ToggleButton legatoGlideButton;   // New: Legato Glide / Fingered Glide toggle
    juce::Label legatoGlideLabel;
    
    // Stereo level meters (L/R channels)
    std::unique_ptr<StereoLevelMeterComponent> stereoLevelMeter;
    
    //==============================================================================
    // -- GUI Components: Modulation Section --
    
    juce::GroupComponent modulationGroup;
    juce::Label modulationTitleLabel;  // Large centered title "Modulation"
    
    // LFO1 Sub-group and Controls
    juce::GroupComponent lfo1Group;
    juce::ToggleButton lfo1EnabledButton;
    juce::ComboBox lfo1WaveformCombo;
    juce::ComboBox lfo1TargetCombo;  // Destination: Pitch or Filter
    juce::ToggleButton lfo1SyncButton;
    juce::ToggleButton lfo1TripletButton;  // Triplet timing toggle (only visible when sync is on)
    juce::ToggleButton lfo1TripletStraightButton;  // Triplet/Straight toggle (only visible when triplet is enabled)
    juce::Slider lfo1FreeRateSlider;  // Free rate slider (0.01-200 Hz)
    juce::ComboBox lfo1SyncRateCombo;  // Sync rate combo (1/32 to 8)
    juce::Slider lfo1DepthSlider;
    juce::Slider lfo1PhaseSlider;
    juce::ToggleButton lfo1RetriggerButton;
    
    juce::Label lfo1WaveformLabel;
    juce::Label lfo1TargetLabel;
    juce::Label lfo1SyncLabel;
    juce::Label lfo1RateLabel;
    juce::Label lfo1RateValueLabel;  // Value display for free rate (Hz) or sync beats
    juce::Label lfo1DepthLabel;
    juce::Label lfo1PhaseLabel;
    
    // LFO2 Sub-group and Controls
    juce::GroupComponent lfo2Group;
    juce::ToggleButton lfo2EnabledButton;
    juce::ComboBox lfo2WaveformCombo;
    juce::ComboBox lfo2TargetCombo;  // Destination: Pitch or Filter
    juce::ToggleButton lfo2SyncButton;
    juce::ToggleButton lfo2TripletButton;  // Triplet timing toggle (only visible when sync is on)
    juce::ToggleButton lfo2TripletStraightButton;  // Triplet/Straight toggle (only visible when triplet is enabled)
    juce::Slider lfo2FreeRateSlider;  // Free rate slider (0.01-200 Hz)
    juce::ComboBox lfo2SyncRateCombo;  // Sync rate combo (1/32 to 8)
    juce::Slider lfo2DepthSlider;
    juce::Slider lfo2PhaseSlider;
    juce::ToggleButton lfo2RetriggerButton;
    
    juce::Label lfo2WaveformLabel;
    juce::Label lfo2TargetLabel;
    juce::Label lfo2SyncLabel;
    juce::Label lfo2RateLabel;
    juce::Label lfo2RateValueLabel;  // Value display for free rate (Hz) or sync beats
    juce::Label lfo2DepthLabel;
    juce::Label lfo2PhaseLabel;
    
    // Mod tab filters (inside LFO boxes, below Retrigger, each has its own toggle)
    juce::ToggleButton modFilterShowButton;   // In LFO 1 box -> modFilter1Show
    juce::ToggleButton modFilterShowButton2;  // In LFO 2 box -> modFilter2Show
    juce::Label modFilterShowLabel;
    juce::GroupComponent modFilter1Group;
    juce::ToggleButton modFilter1LinkButton;
    juce::ComboBox modFilter1ModeCombo;
    NoteLockSlider modFilter1CutoffSlider;
    juce::Slider modFilter1ResonanceSlider;
    juce::ToggleButton warmSaturationMod1Button;
    juce::ToggleButton modFilter1KeyTrackButton;
    juce::ToggleButton modFilter1NoteLockButton;
    juce::ToggleButton modFilter1HarmonicLockButton;
    juce::Label modFilter1ModeLabel;
    juce::Label modFilter1CutoffLabel;
    juce::Label modFilter1ResonanceLabel;
    juce::GroupComponent modFilter2Group;
    juce::ToggleButton modFilter2LinkButton;
    
    // Delay Effect (Effects tab)
    juce::GroupComponent delayGroup;
    juce::GroupComponent reverbGroup;
    juce::GroupComponent grainDelayGroup;
    juce::GroupComponent phaserGroup;
    juce::ToggleButton reverbEnabledButton;
    juce::Label reverbEnabledLabel;
    juce::ComboBox reverbTypeCombo;
    juce::Label reverbTypeLabel;
    juce::Slider reverbWetMixSlider;
    juce::Label reverbWetMixLabel;
    juce::Slider reverbDecayTimeSlider;
    juce::Label reverbDecayTimeLabel;
    juce::ToggleButton reverbFilterShowButton;
    juce::ToggleButton reverbFilterWarmSaturationButton;
    juce::Slider reverbFilterHPCutoffSlider;
    juce::Slider reverbFilterHPResonanceSlider;
    juce::Slider reverbFilterLPCutoffSlider;
    juce::Slider reverbFilterLPResonanceSlider;
    juce::Label reverbFilterHPCutoffLabel;
    juce::Label reverbFilterHPResonanceLabel;
    juce::Label reverbFilterLPCutoffLabel;
    juce::Label reverbFilterLPResonanceLabel;
    juce::ToggleButton delayEnabledButton;
    juce::Label delayEnabledLabel;
    juce::ToggleButton delaySyncButton;
    juce::ToggleButton delayTripletButton;
    juce::ToggleButton delayTripletStraightButton;
    juce::Slider delayFreeRateSlider;
    juce::ComboBox delaySyncRateCombo;
    juce::Slider delayDecaySlider;
    juce::Slider delayDryWetSlider;
    juce::ToggleButton delayPingPongButton;
    juce::ToggleButton delayFilterShowButton;
    juce::GroupComponent delayFilterGroup;
    juce::Slider delayFilterHPCutoffSlider;
    juce::Slider delayFilterHPResonanceSlider;
    juce::Slider delayFilterLPCutoffSlider;
    juce::Slider delayFilterLPResonanceSlider;
    juce::ToggleButton delayFilterWarmSaturationButton;
    juce::Label delayFilterHPCutoffLabel;
    juce::Label delayFilterHPResonanceLabel;
    juce::Label delayFilterLPCutoffLabel;
    juce::Label delayFilterLPResonanceLabel;
    juce::Label delaySyncLabel;
    juce::Label delayRateLabel;
    juce::Label delayRateValueLabel;
    juce::Label delayDecayLabel;
    juce::Label delayDryWetLabel;
    juce::Label delayPingPongLabel;
    juce::ComboBox modFilter2ModeCombo;
    NoteLockSlider modFilter2CutoffSlider;
    juce::Slider modFilter2ResonanceSlider;
    juce::ToggleButton warmSaturationMod2Button;
    juce::ToggleButton modFilter2KeyTrackButton;
    juce::ToggleButton modFilter2NoteLockButton;
    juce::ToggleButton modFilter2HarmonicLockButton;
    juce::Label modFilter2ModeLabel;
    juce::Label modFilter2CutoffLabel;
    juce::Label modFilter2ResonanceLabel;

    // Grain Delay Effect (Effects tab)
    juce::ToggleButton grainDelayEnabledButton;
    juce::Label grainDelayEnabledLabel;
    juce::Slider grainDelayTimeSlider;
    juce::Label grainDelayTimeLabel;
    juce::Slider grainDelaySizeSlider;
    juce::Label grainDelaySizeLabel;
    juce::Slider grainDelayPitchSlider;
    juce::Label grainDelayPitchLabel;
    juce::Slider grainDelayMixSlider;
    juce::Label grainDelayMixLabel;
    juce::Slider grainDelayDecaySlider;
    juce::Label grainDelayDecayLabel;
    juce::Slider grainDelayDensitySlider;
    juce::Label grainDelayDensityLabel;
    juce::Slider grainDelayJitterSlider;
    juce::Label grainDelayJitterLabel;
    juce::ToggleButton grainDelayPingPongButton;
    juce::Label grainDelayPingPongLabel;
    juce::ToggleButton grainDelayFilterShowButton;
    juce::Slider grainDelayFilterHPCutoffSlider;
    juce::Slider grainDelayFilterHPResonanceSlider;
    juce::Slider grainDelayFilterLPCutoffSlider;
    juce::Slider grainDelayFilterLPResonanceSlider;
    juce::ToggleButton grainDelayFilterWarmSaturationButton;
    juce::Label grainDelayFilterHPCutoffLabel;
    juce::Label grainDelayFilterHPResonanceLabel;
    juce::Label grainDelayFilterLPCutoffLabel;
    juce::Label grainDelayFilterLPResonanceLabel;

    // Phaser Effect (Effects tab)
    juce::ToggleButton phaserEnabledButton;
    juce::Label phaserEnabledLabel;
    juce::Slider phaserRateSlider;
    juce::Label phaserRateLabel;
    juce::Slider phaserDepthSlider;
    juce::Label phaserDepthLabel;
    juce::Slider phaserFeedbackSlider;
    juce::Label phaserFeedbackLabel;
    juce::ToggleButton phaserScriptModeButton;
    juce::Label phaserScriptModeLabel;
    juce::Slider phaserMixSlider;
    juce::Label phaserMixLabel;
    juce::Slider phaserCentreSlider;
    juce::Label phaserCentreLabel;
    juce::ComboBox phaserStagesCombo;
    juce::Label phaserStagesLabel;
    juce::Slider phaserStereoOffsetSlider;
    juce::Label phaserStereoOffsetLabel;
    juce::ToggleButton phaserVintageModeButton;
    juce::Label phaserVintageModeLabel;

    // Flanger Effect (Effects tab)
    juce::GroupComponent flangerGroup;
    juce::ToggleButton flangerEnabledButton;
    juce::Label flangerEnabledLabel;
    juce::Slider flangerRateSlider;
    juce::Label flangerRateLabel;
    juce::Slider flangerDepthSlider;
    juce::Label flangerDepthLabel;
    juce::Slider flangerFeedbackSlider;
    juce::Label flangerFeedbackLabel;
    juce::Slider flangerWidthSlider;
    juce::Label flangerWidthLabel;
    juce::Slider flangerMixSlider;
    juce::Label flangerMixLabel;

    // Bit Crusher Effect (Effects tab / Saturation Color tab)
    juce::GroupComponent bitCrusherGroup;
    juce::ToggleButton bitCrusherEnabledButton;
    juce::Label bitCrusherEnabledLabel;
    juce::ToggleButton bitCrusherPostEffectButton;
    juce::Label bitCrusherPostEffectLabel;
    juce::Slider bitCrusherAmountSlider;
    juce::Label bitCrusherAmountLabel;
    juce::Slider bitCrusherRateSlider;
    juce::Label bitCrusherRateLabel;
    juce::Slider bitCrusherMixSlider;
    juce::Label bitCrusherMixLabel;

    // Soft Clipper Effect (Saturation Color tab)
    juce::GroupComponent softClipperGroup;
    juce::ToggleButton softClipperEnabledButton;
    juce::Label softClipperEnabledLabel;
    juce::ComboBox softClipperModeCombo;
    juce::Label softClipperModeLabel;
    juce::Slider softClipperDriveSlider;
    juce::Label softClipperDriveLabel;
    juce::Slider softClipperKneeSlider;
    juce::Label softClipperKneeLabel;
    juce::ComboBox softClipperOversampleCombo;
    juce::Label softClipperOversampleLabel;
    juce::Slider softClipperMixSlider;
    juce::Label softClipperMixLabel;

    // Compressor Effect (Saturation Color tab)
    juce::GroupComponent compressorGroup;
    juce::ToggleButton compressorEnabledButton;
    juce::Label compressorEnabledLabel;
    juce::ComboBox compressorTypeCombo;
    juce::Label compressorTypeLabel;
    juce::Slider compressorThresholdSlider;
    juce::Label compressorThresholdLabel;
    juce::Slider compressorRatioSlider;
    juce::Label compressorRatioLabel;
    juce::Slider compressorAttackSlider;
    juce::Label compressorAttackLabel;
    juce::Slider compressorReleaseSlider;
    juce::Label compressorReleaseLabel;
    juce::Slider compressorMakeupSlider;
    juce::Label compressorMakeupLabel;
    juce::Slider compressorMixSlider;
    juce::Label compressorMixLabel;
    juce::ToggleButton compressorAutoReleaseButton;
    juce::Label compressorAutoReleaseLabel;
    juce::ToggleButton compressorSoftClipButton;
    juce::Label compressorSoftClipLabel;

    // Transient Effect (Saturation Color tab)
    juce::GroupComponent transientGroup;
    juce::ToggleButton transientEnabledButton;
    juce::Label transientEnabledLabel;
    juce::ComboBox transientTypeCombo;
    juce::Label transientTypeLabel;
    juce::Slider transientMixSlider;
    juce::Label transientMixLabel;
    juce::ToggleButton transientPostEffectButton;
    juce::Label transientPostEffectLabel;
    juce::Slider transientKaDonkSlider;
    juce::Label transientKaDonkLabel;
    juce::Slider transientCoarseSlider;
    juce::Label transientCoarseLabel;
    juce::Slider transientLengthSlider;
    juce::Label transientLengthLabel;

    // Lo-Fi Effect (Saturation Color tab)
    juce::GroupComponent lofiGroup;
    juce::ToggleButton lofiEnabledButton;
    juce::Label lofiEnabledLabel;
    juce::Slider lofiAmountSlider;
    juce::Label lofiAmountLabel;
    juce::Slider analogDriftSlider;
    juce::Label analogDriftLabel;

    // Final EQ (Saturation Color tab, end of chain)
    juce::GroupComponent finalEQGroup;
    juce::ToggleButton   finalEQEnabledButton;
    juce::Label          finalEQEnabledLabel;
    std::unique_ptr<FinalEQComponent> finalEQComponent;

    // One set of controls edits one band at a time; the Node dropdown picks which,
    // and clicking a dot in the display moves it. See setFinalEQEditedBand().
    juce::ComboBox finalEQNodeCombo;
    juce::Label    finalEQNodeLabel;
    SpaceDustToggleStyleButton finalEQResetButton;   // puts the chosen node back
    juce::ComboBox finalEQTypeCombo;
    juce::Label    finalEQTypeLabel;
    juce::Slider   finalEQQSlider;
    juce::Label    finalEQQLabel;
    juce::Slider   finalEQFreqSlider;
    juce::Label    finalEQFreqLabel;
    juce::Slider   finalEQGainSlider;
    juce::Label    finalEQGainLabel;

    /** Points the Type / Quality / Frequency / Gain controls at `band` (0-based),
        rebuilding their parameter attachments, and syncs the Node dropdown and the
        display's highlight to match. */
    void setFinalEQEditedBand(int band);

    // Trance Gate Effect (Effects tab)
    juce::GroupComponent tranceGateGroup;
    juce::ToggleButton tranceGateEnabledButton;
    juce::Label tranceGateEnabledLabel;
    juce::ToggleButton tranceGatePreEffectButton;
    juce::Label tranceGatePreEffectLabel;
    juce::ComboBox tranceGateStepsCombo;
    juce::Label tranceGateStepsLabel;
    juce::ToggleButton tranceGateSyncButton;
    juce::Label tranceGateSyncLabel;
    juce::Slider tranceGateRateSlider;
    juce::Label tranceGateRateLabel;
    juce::Slider tranceGateAttackSlider;
    juce::Slider tranceGateReleaseSlider;
    juce::Slider tranceGateMixSlider;
    juce::Label tranceGateAttackLabel;
    juce::Label tranceGateReleaseLabel;
    juce::Label tranceGateMixLabel;
    juce::ToggleButton tranceGateStep1Button;
    juce::ToggleButton tranceGateStep2Button;
    juce::ToggleButton tranceGateStep3Button;
    juce::ToggleButton tranceGateStep4Button;
    juce::ToggleButton tranceGateStep5Button;
    juce::ToggleButton tranceGateStep6Button;
    juce::ToggleButton tranceGateStep7Button;
    juce::ToggleButton tranceGateStep8Button;
    juce::ToggleButton tranceGateStep9Button;
    juce::ToggleButton tranceGateStep10Button;
    juce::ToggleButton tranceGateStep11Button;
    juce::ToggleButton tranceGateStep12Button;
    juce::ToggleButton tranceGateStep13Button;
    juce::ToggleButton tranceGateStep14Button;
    juce::ToggleButton tranceGateStep15Button;
    juce::ToggleButton tranceGateStep16Button;

    //==============================================================================
    // -- Parameter Attachments (Declared LAST for proper destruction order) --
    // CRITICAL: Attachments must be declared AFTER components to ensure they are
    // destroyed BEFORE components (C++ destroys members in reverse declaration order).
    // This prevents Ableton Live crashes: attachments hold listeners that reference
    // components, so attachments must be destroyed first to detach before components die.
    //
    // Destruction order (reverse of declaration):
    // 1. Attachments (destroyed first - declared last)
    // 2. Components (destroyed second - declared first)
    // 3. Base class destructor
    //
    // These connect GUI controls to AudioProcessorValueTreeState for automatic
    // bidirectional parameter updates (thread-safe, real-time compatible)
    
    // Bound by item id rather than by position, so the menu can leave empty User
    // slots out while the parameter keeps all eight. See WaveformChoiceAttachment.
    std::unique_ptr<WaveformChoiceAttachment> osc1WaveformAttachment;
    std::unique_ptr<WaveformChoiceAttachment> osc2WaveformAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc1CoarseTuneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc1DetuneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc2CoarseTuneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc2DetuneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc1LevelAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc2LevelAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc1PanAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc2PanAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> noiseLevelAttachment;
    std::unique_ptr<WaveformChoiceAttachment> noiseColorAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lowShelfAmountAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> highShelfAmountAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> filterModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> filterCutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> filterResonanceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> warmSaturationMasterAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> filterKeyTrackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> filterNoteLockAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> filterHarmonicLockAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> filterEnvAttackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> filterEnvDecayAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> filterEnvSustainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> filterEnvReleaseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> filterEnvAmountAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> envAttackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> envDecayAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> envSustainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> envReleaseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchEnvAmountAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchEnvTimeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchEnvPitchAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> subOscToggleAttachment;
    std::unique_ptr<WaveformChoiceAttachment> subOscWaveformAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> subOscLevelAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> subOscCoarseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> voiceModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> glideTimeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterVolumeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchBendAmountAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> velocityAmountAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchBendAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> legatoGlideAttachment;
    
    // LFO Attachments (Modulation Section)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lfo1EnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> lfo1WaveformAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> lfo1TargetAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lfo1SyncAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lfo1TripletAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lfo1TripletStraightAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lfo1RetriggerAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lfo1FreeRateAttachment;  // Attached to lfo1Rate parameter
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lfo1DepthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lfo1PhaseAttachment;
    
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lfo2EnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> lfo2WaveformAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> lfo2TargetAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lfo2SyncAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lfo2TripletAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lfo2TripletStraightAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lfo2RetriggerAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lfo2FreeRateAttachment;  // Attached to lfo2Rate parameter
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lfo2DepthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lfo2PhaseAttachment;
    
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> modFilterShowAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> modFilterShowAttachment2;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> modFilter1LinkAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modFilter1ModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> modFilter1CutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> modFilter1ResonanceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> warmSaturationMod1Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> modFilter1KeyTrackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> modFilter1NoteLockAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> modFilter1HarmonicLockAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> modFilter2LinkAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modFilter2ModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> modFilter2CutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> modFilter2ResonanceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> warmSaturationMod2Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> modFilter2KeyTrackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> modFilter2NoteLockAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> modFilter2HarmonicLockAttachment;

    // Listeners for sync rate combos (must be destroyed before components)
    std::unique_ptr<SyncRateComboListener> lfo1SyncRateListener;
    std::unique_ptr<SyncRateComboListener> lfo2SyncRateListener;
    std::unique_ptr<SyncRateComboListener> delaySyncRateListener;
    
    // Delay Attachments (Effects Section)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> delayEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> delaySyncAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> delayTripletAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> delayTripletStraightAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayFreeRateAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayDecayAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayDryWetAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> delayPingPongAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> delayFilterShowAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayFilterHPCutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayFilterHPResonanceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayFilterLPCutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayFilterLPResonanceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> delayFilterWarmSaturationAttachment;

    // Reverb Attachments (Effects Section)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> reverbEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> reverbTypeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> reverbWetMixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> reverbDecayTimeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> reverbFilterShowAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> reverbFilterWarmSaturationAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> reverbFilterHPCutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> reverbFilterHPResonanceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> reverbFilterLPCutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> reverbFilterLPResonanceAttachment;

    // Grain Delay Attachments (Effects Section)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> grainDelayEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> grainDelayTimeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> grainDelaySizeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> grainDelayPitchAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> grainDelayMixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> grainDelayDecayAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> grainDelayDensityAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> grainDelayJitterAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> grainDelayPingPongAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> grainDelayFilterShowAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> grainDelayFilterHPCutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> grainDelayFilterHPResonanceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> grainDelayFilterLPCutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> grainDelayFilterLPResonanceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> grainDelayFilterWarmSaturationAttachment;

    // Phaser Attachments (Effects Section)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> phaserEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> phaserRateAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> phaserDepthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> phaserFeedbackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> phaserScriptModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> phaserMixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> phaserCentreAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> phaserStagesAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> phaserStereoOffsetAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> phaserVintageModeAttachment;

    // Flanger attachments
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> flangerEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> flangerRateAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> flangerDepthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> flangerFeedbackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> flangerWidthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> flangerMixAttachment;

    // Bit Crusher attachments
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bitCrusherEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bitCrusherPostEffectAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> bitCrusherAmountAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> bitCrusherRateAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> bitCrusherMixAttachment;

    // Soft Clipper attachments
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> softClipperEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> softClipperModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> softClipperDriveAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> softClipperKneeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> softClipperOversampleAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> softClipperMixAttachment;

    // Compressor Attachments (Saturation Color tab)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> compressorEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> compressorTypeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> compressorThresholdAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> compressorRatioAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> compressorAttackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> compressorReleaseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> compressorMakeupAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> compressorMixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> compressorAutoReleaseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> compressorSoftClipAttachment;

    // Transient Attachments (Saturation Color tab)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> transientEnabledAttachment;
    std::unique_ptr<WaveformChoiceAttachment> transientTypeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> transientMixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> transientPostEffectAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> transientKaDonkAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> transientCoarseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> transientLengthAttachment;

    // Lo-Fi Attachments (Saturation Color tab)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lofiEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lofiAmountAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> analogDriftAttachment;

    // Final EQ Attachments (Saturation Color tab)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> finalEQEnabledAttachment;
    // Rebuilt every time the edited band changes -- they follow the Node dropdown
    // from one band's parameters to the next, so they are not fixed to any one ID.
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> finalEQTypeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   finalEQQAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   finalEQFreqAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   finalEQGainAttachment;
    int finalEQEditedBand_ = 0;

    // Trance Gate attachments
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGatePreEffectAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> tranceGateStepsAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateSyncAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tranceGateRateAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tranceGateAttackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tranceGateReleaseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tranceGateMixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep1Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep2Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep3Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep4Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep5Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep6Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep7Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep8Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep9Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep10Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep11Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep12Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep13Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep14Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep15Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tranceGateStep16Attachment;

    // MPE Attachments (Modulation tab)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> mpeModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mpePitchBendRangeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mpePressureDepthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mpeTimbreDepthAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustAudioProcessorEditor)
};
