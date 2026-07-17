cmake_minimum_required(VERSION 3.22)

# macOS distribution defaults: build a universal (Apple Silicon + Intel) binary
# and target an older deployment baseline so it runs on both architectures and
# older OS versions. Only applied on a Mac host, and only when the caller hasn't
# already set these on the command line. No effect on Windows/Linux builds.
if(CMAKE_HOST_APPLE)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS build architectures")
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
    endif()
endif()

project(SpaceDust VERSION 1.0.15)

# --- Windows: statically link the MSVC C Run-Time (CRT) ---
# Bakes VCRUNTIME140/MSVCP140 into the binaries so the plugin loads on a clean
# Windows machine WITHOUT the "Visual C++ 2015-2022 Redistributable" installed.
# Must be set BEFORE add_subdirectory(JUCE) below so JUCE's static libraries use
# the same (/MT) runtime and we don't hit a CRT-mismatch link error. Relies on
# policy CMP0091 (NEW by default since CMake 3.15; we require 3.22).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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
# ASan builds: skip JUCE's VST3 auto-manifest step. It generates moduleinfo.json by
# *executing* the freshly-built plugin, which aborts under ASan ("interceptors loaded
# too late") since the helper runs without DYLD_INSERT_LIBRARIES. Ableton 10 predates
# moduleinfo and scans the plugin the old way, so skipping it is harmless for testing.
set(SPACEDUST_VST3_AUTO_MANIFEST TRUE)
if(ENABLE_ASAN)
    set(SPACEDUST_VST3_AUTO_MANIFEST FALSE)
endif()

juce_add_plugin(SpaceDust
    COMPANY_NAME "63C"
    PLUGIN_NAME "Space Dust"
    PLUGIN_MANUFACTURER_CODE YcPl
    PLUGIN_CODE SpDs
    FORMATS VST3 AU Standalone
    VST3_AUTO_MANIFEST ${SPACEDUST_VST3_AUTO_MANIFEST}
    BUNDLE_ID "com.63c.SpaceDust"
    PRODUCT_NAME "Space Dust"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.png"
    DESCRIPTION "Cosmic particles of sound"
    VERSION 1.0.15
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER "63C"
)

# Embed fonts as binary data (Glitch Goblin for title)
juce_add_binary_data(SpaceDustBinaryData
    SOURCES
        Resources/GlitchGoblin.ttf
        Resources/SpaceDustTitle.png
        Resources/Logo63C.png
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
    Source/VoidVerbReverb.cpp
    Source/VoidVerbReverb.h
    Source/SpaceDustLookAndFeel.cpp
    Source/SpaceDustLookAndFeel.h
    Source/PresetManager.cpp
    Source/PresetManager.h
    Source/CheezeGuyGame.h
    Source/MemorySafetyLogger.cpp
    Source/MemorySafetyLogger.h
)

# Disable VST2 replacement compatibility for VST3-only plugin
target_compile_definitions(SpaceDust
    PRIVATE
        JUCE_VST3_CAN_REPLACE_VST2=0
        # MIDI-CC-as-parameters: KEEP ENABLED (=1). This emulation is the ONLY way a
        # JUCE VST3 receives controller data — pitch-bend, channel pressure and CC74 —
        # which MPE rides on. Setting it to 0 silently broke MPE (getMidiControllerAssignment
        # returns kResultFalse, so the host delivers no controllers; this JUCE has no
        # Note-Expression path). The downside of the default is it exposes 16*130 = 2080
        # phantom "MIDI CC" parameters that bloat Live's automation list (and were blamed
        # for a heap-corruption crash whose REAL cause was a separate block-size buffer
        # overflow, fixed by grow-guards). So we keep emulation ON and use our local JUCE
        # patch (JUCE_VST3_LIMIT_MIDI_CC_PARAMS) to expose ONLY the controllers MPE/pedals
        # need (~96 instead of 2080). See juce_audio_plugin_client_VST3.cpp.
        JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=1
        JUCE_VST3_LIMIT_MIDI_CC_PARAMS=1
        # Force VST3 Audio Module "Class Flags" to 0.
        # JUCE defaults JucePlugin_Vst3ComponentFlags to kSimpleModeSupported (=2)
        # for synths. FL Studio interprets that flag as a second usable mode and
        # registers the plugin TWICE, so it shows up as "Space Dust_2". Other hosts
        # (Ableton, etc.) ignore it. Setting it to 0 (matching e.g. Serum) makes FL
        # list the plugin once. JUCE guards this macro with #ifndef, so this override
        # wins. See juce_VST3ModuleInfo.h.
        JucePlugin_Vst3ComponentFlags=0
        # Activates our local JUCE patch (juce_Windowing_windows.cpp) that forwards
        # unhandled keystrokes to the host's ROOT window instead of the direct parent.
        # Fixes FL Studio "Typing keyboard to piano" dying whenever the plugin holds
        # keyboard focus: FL's direct parent of the editor is a wrapper child window
        # that doesn't route those keys, so they were dropped. Known JUCE<->FL issue
        # (forum.juce.com/t/return-keyboard-focus-in-fl-studio/50711). Windows-only;
        # macro is undefined (=> no-op) for other plugins sharing this JUCE copy.
        # See patches/apply-juce-keyboard-focus-patch.ps1.
        JUCE_FORWARD_KEYS_TO_ROOT_WINDOW=1
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
        # JUCE's VST3 wrapper object is built by a separate target WITHOUT ASan, so its
        # STL container-overflow annotations (annotate_vector/annotate_string) are off.
        # Disable them here too or the link fails: LNK2038 mismatch '1' vs '0'.
        # Heap-buffer-overflow / use-after-free / double-free detection is unaffected.
        target_compile_definitions(SpaceDust PRIVATE SPACEDUST_ASAN=1
                                   _DISABLE_STRING_ANNOTATION
                                   _DISABLE_VECTOR_ANNOTATION)
        message(STATUS "AddressSanitizer ENABLED for SpaceDust target (MSVC).")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # clang/AppleClang/gcc: ASan needs the flags on BOTH compile and link.
        # -fno-omit-frame-pointer + -g keep backtraces precise to the faulting line.
        target_compile_options(SpaceDust PRIVATE
            -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(SpaceDust PRIVATE -fsanitize=address)
        target_compile_definitions(SpaceDust PRIVATE SPACEDUST_ASAN=1)
        message(STATUS "AddressSanitizer ENABLED for SpaceDust target (Clang/GCC).")

        # The plugin FORMAT wrapper targets (VST3, AU) link the instrumented shared
        # code but SpaceDust's -fsanitize=address is PRIVATE and does not propagate.
        # The final .vst3/.component binary must itself be compiled+linked with ASan
        # so the runtime is embedded — this lets us load the plugin into Ableton and
        # catch the host-driven OOB write that the standalone/harness cannot reach.
        # (Standalone is handled in its own block below.)
        foreach(_fmt SpaceDust_VST3 SpaceDust_AU)
            if(TARGET ${_fmt})
                target_compile_options(${_fmt} PRIVATE
                    -fsanitize=address -fno-omit-frame-pointer -g)
                target_link_options(${_fmt} PRIVATE -fsanitize=address)
                message(STATUS "AddressSanitizer ENABLED for ${_fmt} (Clang/GCC).")
            endif()
        endforeach()
    else()
        message(WARNING "ENABLE_ASAN: unrecognized compiler; ignored.")
    endif()
endif()

#==============================================================================
# ThreadSanitizer (TSan) — finds DATA RACES that ASan cannot see (races on
# already-allocated memory, e.g. the audio thread vs the message thread).
# Mutually exclusive with ASan. Applied to the shared code + the stress harness
# only (not the format wrappers — TSan + a real editor is too noisy).
# Configure with: cmake -B build-tsan -DENABLE_TSAN=ON
#==============================================================================
option(ENABLE_TSAN "Build with ThreadSanitizer (clang -fsanitize=thread)" OFF)
if(ENABLE_TSAN)
    if(ENABLE_ASAN)
        message(FATAL_ERROR "ENABLE_TSAN and ENABLE_ASAN are mutually exclusive.")
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(SpaceDust PRIVATE
            -fsanitize=thread -fno-omit-frame-pointer -g)
        target_link_options(SpaceDust PRIVATE -fsanitize=thread)
        target_compile_definitions(SpaceDust PRIVATE SPACEDUST_TSAN=1)
        message(STATUS "ThreadSanitizer ENABLED for SpaceDust target (Clang/GCC).")
    else()
        message(WARNING "ENABLE_TSAN: unrecognized compiler; ignored.")
    endif()
endif()

#==============================================================================
# Memory Safety Logger — always-on dev logger for RT-safe voice / grain /
# pointer / buffer / delay-line / filter tracking.
# Configure with: cmake -B build-safety -DENABLE_MEMORY_SAFETY_LOGGING=ON
# When OFF (default):
#   * SPACEDUST_ENABLE_SAFETY_LOGGING=0 → every SAFETY_LOG_* macro is a no-op.
#   * JUCE_LOG_ASSERTIONS=0             → silences juce::Logger assertion lines.
#   * Release builds also have JUCE_DEBUG=0 (set by NDEBUG), so DBG(...) is empty.
# Result: clean Release builds emit zero log code. See disable-all-logging-for-release.ps1.
#==============================================================================
option(ENABLE_MEMORY_SAFETY_LOGGING "Build with Space Dust memory-safety logger" OFF)
if(ENABLE_MEMORY_SAFETY_LOGGING)
    target_compile_definitions(SpaceDust PRIVATE SPACEDUST_ENABLE_SAFETY_LOGGING=1)
    if(WIN32)
        target_link_libraries(SpaceDust PRIVATE DbgHelp)
    endif()
    message(STATUS "MemorySafetyLogger ENABLED. Logs: %APPDATA%/63C/Space Dust/Logs/Safety/")
else()
    target_compile_definitions(SpaceDust PRIVATE
        SPACEDUST_ENABLE_SAFETY_LOGGING=0
        JUCE_LOG_ASSERTIONS=0)
    message(STATUS "MemorySafetyLogger DISABLED (logging compiled out).")
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

#==============================================================================
# Standalone: auto-detect MIDI keyboards on launch.
# JUCE's stock standalone app forces autoOpenMidiDevices=false on desktop, so a
# connected MIDI keyboard does nothing until manually enabled in Audio/MIDI
# Settings. We supply our own app (Source/StandaloneApp.cpp) that opens MIDI
# inputs automatically, enabled via JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1.
#==============================================================================
if(TARGET SpaceDust_Standalone)
    target_sources(SpaceDust_Standalone PRIVATE Source/StandaloneApp.cpp)
    target_compile_definitions(SpaceDust_Standalone PRIVATE JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
    message(STATUS "Standalone: custom app with auto MIDI-input detection ENABLED.")

    # When ASan is on, the SpaceDust shared code is instrumented but its
    # -fsanitize=address is PRIVATE and does not propagate to this executable.
    # The final Standalone binary must still be compiled+linked with ASan so the
    # runtime is present and the wrapper/UI code is instrumented too — that lets
    # us click through the REAL preset browser + UI under ASan to catch an OOB
    # write the headless harness can't reach.
    if(ENABLE_ASAN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(SpaceDust_Standalone PRIVATE
            -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(SpaceDust_Standalone PRIVATE -fsanitize=address)
        message(STATUS "AddressSanitizer ENABLED for SpaceDust_Standalone (Clang/GCC).")
    endif()
endif()

#==============================================================================
# Parameter-flood stress harness (ASan builds only). A console app that floods
# every parameter from one thread while processBlock runs on another, to catch
# host-automation-flood heap corruption that pluginval/the editor can't trigger.
# Build:  cmake -B build-asan -DENABLE_ASAN=ON ; cmake --build build-asan --target SpaceDustStress
#==============================================================================
if(ENABLE_ASAN OR ENABLE_TSAN)
    juce_add_console_app(SpaceDustStress PRODUCT_NAME "SpaceDustStress")
    target_sources(SpaceDustStress PRIVATE tools/stress/stress_main.cpp)
    # Source/ for PluginProcessor.h (preset sweep downcasts to SpaceDustAudioProcessor),
    # and the factory preset folder so the sweep can load every .sdpreset.
    target_include_directories(SpaceDustStress PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_compile_definitions(SpaceDustStress PRIVATE
        SPACEDUST_PRESET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/installer/Files/Presets")
    if(ENABLE_TSAN)
        target_compile_options(SpaceDustStress PRIVATE
            -fsanitize=thread -fno-omit-frame-pointer -g)
        target_link_options(SpaceDustStress PRIVATE -fsanitize=thread)
    elseif(MSVC)
        target_compile_options(SpaceDustStress PRIVATE /fsanitize=address /Zi)
        target_link_options(SpaceDustStress PRIVATE /INCREMENTAL:NO)
        # Match SpaceDust's container-annotation setting so the ASan objects link.
        target_compile_definitions(SpaceDustStress PRIVATE
                                   _DISABLE_STRING_ANNOTATION
                                   _DISABLE_VECTOR_ANNOTATION)
    else()
        target_compile_options(SpaceDustStress PRIVATE
            -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(SpaceDustStress PRIVATE -fsanitize=address)
    endif()
    # Link the plugin's shared code (provides createPluginFilter + all DSP) + JUCE.
    target_link_libraries(SpaceDustStress PRIVATE
        SpaceDust
        SpaceDustBinaryData
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra)
    message(STATUS "SpaceDustStress harness ENABLED (ASan).")
endif()

#==============================================================================
# Transient Pre/Post filter routing test (plain Release, no sanitizer). A tiny
# console app that drives the processor offline to confirm the "Post Effect"
# toggle changes whether the Main filter colours the transient.
# Build:  cmake -B build-ttest -DENABLE_TRANSIENT_TEST=ON ; cmake --build build-ttest --target SpaceDustTransientTest
#==============================================================================
option(ENABLE_TRANSIENT_TEST "Build the offline transient Pre/Post routing test" OFF)
if(ENABLE_TRANSIENT_TEST)
    juce_add_console_app(SpaceDustTransientTest PRODUCT_NAME "SpaceDustTransientTest")
    target_sources(SpaceDustTransientTest PRIVATE tools/transienttest/transient_test_main.cpp)
    target_include_directories(SpaceDustTransientTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_link_libraries(SpaceDustTransientTest PRIVATE
        SpaceDust
        SpaceDustBinaryData
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra)
    message(STATUS "SpaceDustTransientTest ENABLED.")
endif()

#==============================================================================
# VST3-wrapper crash host (ASan builds only). Unlike SpaceDustStress (which calls
# the processor directly) and the Standalone, this HOSTS the real .vst3 through
# juce::VST3PluginFormat — exercising the VST3 wrapper + host state path that
# Ableton uses. The host is itself ASan-instrumented so the runtime loads BEFORE
# it dlopens the (also ASan) plugin: no "interceptors loaded too late" abort.
# It deliberately does NOT link the SpaceDust shared lib (the code lives in the
# dlopened .vst3) to avoid duplicate symbols.
# Build:  cmake --build build-asan --target SpaceDustVST3Host
#==============================================================================
if(ENABLE_ASAN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    juce_add_console_app(SpaceDustVST3Host PRODUCT_NAME "SpaceDustVST3Host")
    target_sources(SpaceDustVST3Host PRIVATE tools/vst3host/vst3host_main.cpp)
    target_compile_definitions(SpaceDustVST3Host PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_MODAL_LOOPS_PERMITTED=0
        SPACEDUST_PRESET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/installer/Files/Presets"
        SPACEDUST_VST3_PATH="${CMAKE_CURRENT_BINARY_DIR}/SpaceDust_artefacts/RelWithDebInfo/VST3/Space Dust.vst3")
    target_compile_options(SpaceDustVST3Host PRIVATE
        -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(SpaceDustVST3Host PRIVATE -fsanitize=address)
    target_link_libraries(SpaceDustVST3Host PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra)
    message(STATUS "SpaceDustVST3Host ENABLED (ASan, hosts the real VST3).")
endif()

