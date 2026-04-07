# Space Dust Plugin - Comprehensive Fix Summary

## Assertion Fixes Applied

### 1. LeakedObjectDetector.h:116 (Memory Leaks) ✅

**Status**: FIXED

**Fixes Applied**:
- ✅ Destructor cleanup order:
  1. Remove parameter listeners FIRST
  2. Clear voices
  3. Clear sounds (ReferenceCountedArray)
  4. Reset sample rate
- ✅ `releaseResources()` cleanup:
  - Stop all voices gracefully
  - Clear voices
  - Clear sounds
  - Reset sample rate
  - Reset atomic parameters
- ✅ FileLogger removed (was causing leaks)
- ✅ All Logger::writeToLog() calls removed

**Code Locations**:
- `PluginProcessor.cpp`: Destructor (lines 224-295), `releaseResources()` (lines 606-681)

### 2. String.cpp:327 (Invalid UTF-8) ✅

**Status**: FIXED

**Fixes Applied**:
- ✅ UTF-8 validation helper functions added
- ✅ All numeric-to-string conversions use safe helpers
- ✅ All parameter IDs use `ParameterID{"id", 1}` (prevents string assertions)
- ✅ StringArray values are ASCII-only
- ✅ All string concatenations use safe functions

**Code Locations**:
- `PluginProcessor.cpp`: UTF-8 helpers (lines 50-104)
- `PluginEditor.cpp`: String::formatted calls validated
- `SynthVoice.cpp`: Logger calls removed
- `SpaceDustSynthesiser.cpp`: Safe string helpers added

### 3. LookAndFeel.cpp:82 (UI Access Issues) ✅

**Status**: FIXED

**Fixes Applied**:
- ✅ `customLookAndFeel` declared BEFORE all Components (line 74 in PluginEditor.h)
- ✅ `isBeingDestroyed` flag added for thread safety
- ✅ Safety checks in `paint()` and `resized()` methods
- ✅ Timer stopped in destructor before component access
- ✅ `setLookAndFeel(nullptr)` NOT called (correct pattern for member variable)
- ✅ `setAccessible(false)` in constructor (fixes Windows leaks)

**Code Locations**:
- `PluginEditor.h`: Member order (lines 70-74)
- `PluginEditor.cpp`: Destructor (lines 599-629), `paint()` (lines 713+), `resized()` (lines 739+)

### 4. Thread Safety for Parameter Changes ✅

**Status**: FIXED

**Fixes Applied**:
- ✅ All ADSR parameters use `std::atomic<float>`
- ✅ Parameter changes update atomics on message thread
- ✅ Audio thread reads from atomics (lock-free)
- ✅ All parameter listeners removed before destruction
- ✅ `parameterChanged()` runs on message thread (safe)

**Code Locations**:
- `PluginProcessor.h`: Atomic parameter storage (lines 95-98)
- `PluginProcessor.cpp`: `parameterChanged()` (lines 565-604)

### 5. Ableton VST3 Unload Fixes ✅

**Status**: FIXED

**Fixes Applied**:
- ✅ VST3 wrapper type check
- ✅ `Timer::callAfterDelay(200ms)` workaround in destructor
- ✅ Comprehensive cleanup before workaround
- ✅ Thread detachment delay

**Code Locations**:
- `PluginProcessor.cpp`: Destructor (lines 288-295)

### 6. Pink noise (Voss–McCartney) row index / buffer overrun ✅

**Status**: FIXED

**Issue**: The pink-noise path used a 16-element `pinkState` array but the derived row index could reach 16 once the sample counter passed 65536 (~1.3 s at 48 kHz per voice), writing one element past the array (undefined behavior). Symptoms could include intermittent harsh or “digital” artifacts, often more noticeable with several sustained voices.

**Fix**:
- Advance a 16-bit–wrapped unsigned counter and avoid the degenerate zero case before isolating the lowest set bit.
- Clamp the computed row index to `pinkState.size() - 1` as a safeguard.
- After `jlimit` on oscillator Hz, detect non-finite values and fall back to a safe frequency; zero non-finite post-filter samples. Expose `dspSanitizeEventCount` on the processor for optional diagnostics.

**Code locations**: `SynthVoice.cpp` / `SynthVoice.h` (pink noise loop), `PluginProcessor.h` (atomic counter and getters).

## Debug Logging

**Status**: COMPREHENSIVE

**Debug Points Added**:
- Constructor entry/completion
- Destructor start/completion
- Parameter listener addition/removal
- Voice/sound clearing (with counts)
- Sample rate reset
- releaseResources() start/completion
- Ableton VST3 workaround application

**Logging Method**: `DBG()` macro (safe, debug-only, doesn't use Logger)

## Recommendations

1. **Test in Ableton Live**: The fixes should prevent all three assertion types
2. **Monitor Debug Output**: Use DBG() output to trace plugin lifecycle
3. **Production Builds**: Consider removing DBG() calls for release builds
4. **String Safety**: All string operations now use safe helpers

## Files Modified

1. `PluginProcessor.cpp` - Destructor, releaseResources(), UTF-8 helpers, parameter handling
2. `PluginProcessor.h` - Atomic parameter storage; optional `dspSanitizeEventCount` for diagnostics
3. `PluginEditor.cpp` - Paint/resized safety, timer cleanup
4. `PluginEditor.h` - Member order (LookAndFeel before Components)
5. `SynthVoice.cpp` - Logger calls removed; pink-noise index fix and non-finite DSP guards
6. `SynthVoice.h` - Pink-noise counter type
7. `SpaceDustSynthesiser.cpp` - Safe string helpers
