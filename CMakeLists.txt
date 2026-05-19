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
    FORMATS VST3 Standalone
    PRODUCT_NAME "Space Dust"
    DESCRIPTION "Cosmic particles of sound"
    VERSION 1.0.0
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER "Shades"
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
    Source/SpaceDustFinalEQ.cpp
    Source/SpaceDustFinalEQ.h
    Source/FinalEQComponent.cpp
    Source/FinalEQComponent.h
    Source/SexiconReverb.cpp
    Source/SexiconReverb.h
    Source/SpaceDustLookAndFeel.cpp
    Source/SpaceDustLookAndFeel.h
    Source/PresetManager.cpp
    Source/PresetManager.h
    Source/CheezeGuyGame.h
)

# Disable VST2 replacement compatibility for VST3-only plugin
target_compile_definitions(SpaceDust
    PRIVATE
        JUCE_VST3_CAN_REPLACE_VST2=0
)

#==============================================================================
# Visual Leak Detector (VLD) — optional, OFF by default
# Configure with: cmake -B build -DENABLE_VLD=ON -DCMAKE_BUILD_TYPE=Debug
# Reports memory leaks with call stacks when the host process exits.
# WARNING: Debug-only. Never enable for Release builds shipped to users.
# After build, copy "<VLD_DIR>/bin/Win64/vld_x64.dll" + "dbghelp.dll" into
# the same folder as the host executable, or into Windows PATH.
#==============================================================================
option(ENABLE_VLD "Link Visual Leak Detector (Debug builds only)" OFF)
if(ENABLE_VLD)
    set(VLD_DIR "C:/Program Files (x86)/Visual Leak Detector" CACHE PATH "VLD install root")
    if(NOT EXISTS "${VLD_DIR}/include/vld.h")
        message(FATAL_ERROR "ENABLE_VLD=ON but vld.h not found at ${VLD_DIR}/include/. Install VLD from https://github.com/KindDragon/vld/releases or set -DVLD_DIR=...")
    endif()
    target_include_directories(SpaceDust PRIVATE "${VLD_DIR}/include")
    target_link_libraries(SpaceDust PRIVATE "${VLD_DIR}/lib/Win64/vld.lib")
    target_compile_definitions(SpaceDust PRIVATE VLD_ENABLED=1)
    message(STATUS "VLD enabled. Headers: ${VLD_DIR}/include   Lib: ${VLD_DIR}/lib/Win64/vld.lib")
    message(STATUS "Reminder: copy vld_x64.dll + dbghelp.dll from ${VLD_DIR}/bin/Win64/ next to the host exe.")
endif()

#==============================================================================
# AddressSanitizer (ASan) — optional, OFF by default. MSVC ASan since VS 16.9.
# Configure with: cmake -B build-asan -DENABLE_ASAN=ON
# Detects heap/stack overflows, use-after-free, double-free at runtime.
# WARNING: Significant runtime overhead; never ship.
#==============================================================================
option(ENABLE_ASAN "Build with AddressSanitizer (MSVC /fsanitize=address)" OFF)
if(ENABLE_ASAN)
    if(MSVC)
        target_compile_options(SpaceDust PRIVATE /fsanitize=address /Zi)
        # ASan in MSVC requires disabling /RTC* and /INCREMENTAL; CMake handles RTC.
        target_link_options(SpaceDust PRIVATE /INCREMENTAL:NO)
        target_compile_definitions(SpaceDust PRIVATE SPACEDUST_ASAN=1)
        message(STATUS "AddressSanitizer ENABLED for SpaceDust target.")
    else()
        message(WARNING "ENABLE_ASAN currently only wired up for MSVC; ignored.")
    endif()
endif()

#==============================================================================
# qa-test custom target — runs the full QA suite via run-full-qa.ps1.
# Usage: cmake --build build --target qa-test
#==============================================================================
add_custom_target(qa-test
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/run-full-qa.ps1"
            -BuildDir "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running Space Dust full QA suite (pluginval + installer + diagnostics)"
    VERBATIM
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

