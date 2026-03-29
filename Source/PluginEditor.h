#pragma once

#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "SpaceDustLookAndFeel.h"
#include "OscilloscopeComponent.h"
#include "SpectrumAnalyserComponent.h"
#include "FinalEQComponent.h"
#include "PresetManager.h"

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
                          public juce::AudioProcessorValueTreeState::Listener
{
public:
    MainPageComponent(SpaceDustAudioProcessorEditor& editor);
    ~MainPageComponent() override;
    
    void paint(juce::Graphics& g) override;
    void resized() override;
    void parameterChanged(const juce::String& parameterID, float newValue) override;
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
    SpaceDustAudioProcessorEditor& parentEditor;
};

//==============================================================================
// -- Effects Page Component (Delay, Chorus, Reverb, etc.) --
class EffectsPageComponent : public juce::Component,
                             public juce::AudioProcessorValueTreeState::Listener
{
public:
    EffectsPageComponent(SpaceDustAudioProcessorEditor& editor);
    ~EffectsPageComponent() override;
    
    void paint(juce::Graphics& g) override;
    void resized() override;
    void parameterChanged(const juce::String& parameterID, float newValue) override;
    
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
};

//==============================================================================
// -- Stereo Level Meter Component --
/**
    Real-time stereo level meters for L/R channels.
    Displays two vertical bars showing peak levels from -Inf to 0 dB.
    Updates smoothly via timer, with cyan/blue fill and red clipping indicator.
*/
class StereoLevelMeterComponent : public juce::Component
{
public:
    StereoLevelMeterComponent(SpaceDustAudioProcessor& processor);
    ~StereoLevelMeterComponent() override = default;
    
    void paint(juce::Graphics& g) override;
    void resized() override;
    
    // Called from editor timer to refresh display
    void updateLevels(float leftPeak, float rightPeak);
    
private:
    SpaceDustAudioProcessor& audioProcessor;
    
    // Current peak levels (0.0 = silence, 1.0 = 0 dB, > 1.0 = clipping)
    std::atomic<float> leftPeak{0.0f};
    std::atomic<float> rightPeak{0.0f};
    
    // Meter dimensions
    static constexpr int meterWidth = 20;  // Width of each bar
    static constexpr int meterGap = 4;     // Gap between L and R bars
    
    // Helper: Convert linear peak to dB (returns -Inf for 0, 0 for 1.0)
    float linearToDb(float linear);
    
    // Helper: Convert dB to normalized height (0.0 = -Inf at bottom, 1.0 = 0 dB at top)
    float dbToHeight(float db);
};

//==============================================================================
class SpaceDustAudioProcessorEditor : public juce::AudioProcessorEditor,
                                      public juce::Timer,
                                      public juce::Slider::Listener,
                                      public juce::Button::Listener,
                                      public juce::AudioProcessorValueTreeState::Listener
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
    void sliderValueChanged(juce::Slider* slider) override {}
    void sliderDragEnded(juce::Slider* slider) override;
    void buttonClicked(juce::Button* button) override {}
    void buttonStateChanged(juce::Button* button) override;
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    //==============================================================================
    void paint(juce::Graphics&) override;
    void resized() override;

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

    // Bidirectional filter parameter sync when "Link to Master" is enabled
    bool isSyncingFilterParams = false;
    void syncLinkedFilterParams(const juce::String& parameterID, float newValue);

    // Pitch bend snap-back: poll processor ramp and sync display
    bool pitchBendSnapActive{false};

    // Clipping hold for spectral tab (keeps red state for a duration after clipping)
    int clippingHoldTicks = 0;
    static constexpr int clippingHoldDuration = 10;  // ~500ms at 50ms timer

    // TooltipWindow required for setTooltip() to display (e.g. Pan labels)
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

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
    
    // Oscillator tuning controls (simple, intuitive system)
    juce::Slider osc1CoarseTuneSlider;
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
    juce::Slider filterCutoffSlider;
    juce::Slider filterResonanceSlider;
    juce::ToggleButton warmSaturationMasterButton;
    
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
    
    juce::Slider masterVolumeSlider;
    juce::Label masterVolumeLabel;
    
    // Pitch bend and master pitch (in Master section)
    juce::Slider pitchBendAmountSlider;
    juce::Label pitchBendAmountLabel;
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
    juce::Slider modFilter1CutoffSlider;
    juce::Slider modFilter1ResonanceSlider;
    juce::ToggleButton warmSaturationMod1Button;
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
    juce::Slider modFilter2CutoffSlider;
    juce::Slider modFilter2ResonanceSlider;
    juce::ToggleButton warmSaturationMod2Button;
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

    // Final EQ (Saturation Color tab, end of chain)
    juce::GroupComponent finalEQGroup;
    juce::ToggleButton   finalEQEnabledButton;
    juce::Label          finalEQEnabledLabel;
    std::unique_ptr<FinalEQComponent> finalEQComponent;

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
    
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> osc1WaveformAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> osc2WaveformAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc1CoarseTuneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc1DetuneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc2CoarseTuneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc2DetuneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc1LevelAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc2LevelAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc1PanAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> osc2PanAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> noiseLevelAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> noiseColorAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lowShelfAmountAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> highShelfAmountAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> filterModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> filterCutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> filterResonanceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> warmSaturationMasterAttachment;
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
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> subOscWaveformAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> subOscLevelAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> subOscCoarseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> voiceModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> glideTimeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterVolumeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchBendAmountAttachment;
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
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> modFilter2LinkAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modFilter2ModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> modFilter2CutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> modFilter2ResonanceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> warmSaturationMod2Attachment;
    
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
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> transientTypeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> transientMixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> transientPostEffectAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> transientKaDonkAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> transientCoarseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> transientLengthAttachment;

    // Lo-Fi Attachments (Saturation Color tab)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lofiEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lofiAmountAttachment;

    // Final EQ Attachment (Saturation Color tab)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> finalEQEnabledAttachment;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustAudioProcessorEditor)
};
