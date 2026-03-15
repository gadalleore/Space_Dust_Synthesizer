cmake_minimum_required(VERSION 3.22)

project(SpaceDust VERSION 1.0.0)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add JUCE as a subdirectory
# Set JUCE path: use -DJUCE_DIR=... when running cmake, or set JUCE_DIR env var, or edit below
if(DEFINED ENV{JUCE_DIR})
    set(JUCE_DIR "$ENV{JUCE_DIR}" CACHE PATH "Path to JUCE")
elseif(NOT JUCE_DIR)
    set(JUCE_DIR "C:/path/to/your/JUCE" CACHE PATH "Path to JUCE - edit this or set JUCE_DIR")
endif()
if(EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    message(FATAL_ERROR "JUCE not found at ${JUCE_DIR}. Please ensure JUCE is installed at the specified location.")
endif()

# Create the plugin
juce_add_plugin(SpaceDust
    COMPANY_NAME "Shades"
    PLUGIN_NAME "Space Dust"
    PLUGIN_MANUFACTURER_CODE YcPl
    PLUGIN_CODE SpDs
    FORMATS VST3
    PRODUCT_NAME "Space Dust"
    DESCRIPTION "Cosmic particles of sound"
    VERSION 1.0.0
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER "Shades"
)

# Automatically copy plugin to system VST3 folder after build
set_target_properties(SpaceDust PROPERTIES
    JUCE_COPY_PLUGIN_AFTER_BUILD TRUE
)

# Embed fonts as binary data (Glitch Goblin for title)
juce_add_binary_data(SpaceDustBinaryData
    SOURCES
        Resources/GlitchGoblin.ttf
)

# Add source files
target_sources(SpaceDust PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/Goniometer.cpp
    Source/Goniometer.h
    Source/OscilloscopeComponent.cpp
    Source/OscilloscopeComponent.h
    Source/SpectrumAnalyserComponent.cpp
    Source/SpectrumAnalyserComponent.h
    Source/SynthVoice.cpp
    Source/SynthVoice.h
    Source/SynthSound.cpp
    Source/SynthSound.h
    Source/SpaceDustSynthesiser.cpp
    Source/SpaceDustSynthesiser.h
    Source/SpaceDustReverb.cpp
    Source/SpaceDustReverb.h
    Source/SpaceDustGrainDelay.cpp
    Source/SpaceDustGrainDelay.h
    Source/SpaceDustPhaser.cpp
    Source/SpaceDustPhaser.h
    Source/SpaceDustTranceGate.cpp
    Source/SpaceDustTranceGate.h
    Source/SpaceDustFlanger.cpp
    Source/SpaceDustFlanger.h
    Source/SpaceDustBitCrusher.cpp
    Source/SpaceDustBitCrusher.h
    Source/SpaceDustSoftClipper.cpp
    Source/SpaceDustSoftClipper.h
    Source/SpaceDustCompressor.cpp
    Source/SpaceDustCompressor.h
    Source/SpaceDustLofi.cpp
    Source/SpaceDustLofi.h
    Source/SpaceDustTransient.cpp
    Source/SpaceDustTransient.h
    Source/SpaceDustParametricEQ.cpp
    Source/SpaceDustParametricEQ.h
    Source/SexiconReverb.cpp
    Source/SexiconReverb.h
    Source/SpaceDustLookAndFeel.cpp
    Source/SpaceDustLookAndFeel.h
)

# Disable VST2 replacement compatibility for VST3-only plugin
target_compile_definitions(SpaceDust
    PRIVATE
        JUCE_VST3_CAN_REPLACE_VST2=0
)

# Add JUCE modules
target_link_libraries(SpaceDust
    PRIVATE
        SpaceDustBinaryData
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

